// GX FIFOs: write-gather pipe -> PI FIFO in memory -> command processor.
//
// As on the console, everything the CPU writes to the gather pipe (0xCC008000) lands in the
// CPU FIFO in main memory at the PI write pointer. The command processor reads its own FIFO
// (CP base/end/read/write pointers) when reading is enabled; while the two are linked (CP_CR
// GP link, the normal immediate mode) its write pointer follows the PI's. Display lists are
// recorded by pointing the PI FIFO at a buffer with the link off, and executed later through
// CALL_DL. The GPU is infinitely fast here: commands run as soon as they are complete.
#include "gx_fifo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../runtime.h"

namespace rt {
namespace {

GxStats s_stats;
GxSink s_sink = nullptr;
void (*s_syncHook)() = nullptr;  // the renderer catches up (the CPU looks at the GP's progress)
bool s_log = false;

// --- PI (CPU side) FIFO ---
u32 s_piBase = 0, s_piEnd = 0, s_piWp = 0;  // physical; wp bit 29 = wrap flag
// --- CP (GPU side) FIFO ---
u16 s_cpReg[0x40] = {};  // raw 16-bit registers 0x00..0x7E
u32 s_cpBase = 0, s_cpEnd = 0, s_cpWp = 0, s_cpRp = 0, s_cpHi = 0, s_cpLo = 0, s_cpBp = 0;
u16 s_cpCr = 0;
std::vector<u8> s_cmd;  // bytes of an incomplete command read from the CP FIFO

u32 s_vcdLo = 0, s_vcdHi = 0;
u32 s_vatA[8] = {}, s_vatB[8] = {}, s_vatC[8] = {};

u32 compSize(u32 fmt) {
    switch (fmt) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    case 4: return 4;
    default: return 0;
    }
}
u32 colSize(u32 fmt) {
    switch (fmt) {
    case 0: case 3: return 2;
    case 1: case 4: return 3;
    case 2: case 5: return 4;
    default: return 0;
    }
}
u32 idxSize(u32 type) { return type == 2 ? 1 : type == 3 ? 2 : 0; }

u32 vertexSize(u32 fmt) {
    const u32 lo = s_vcdLo, hi = s_vcdHi, A = s_vatA[fmt], B = s_vatB[fmt], C = s_vatC[fmt];
    u32 size = 0;
    for (int i = 0; i < 9; i++)
        if (lo & (1u << i)) size += 1;  // PNMTXIDX, TEX0..7MTXIDX
    const u32 pos = (lo >> 9) & 3;
    if (pos == 1) size += compSize((A >> 1) & 7) * ((A & 1) ? 3 : 2);
    else size += idxSize(pos);
    const u32 nrm = (lo >> 11) & 3;
    const bool nbt = (A >> 9) & 1;
    if (nrm == 1) size += compSize((A >> 10) & 7) * (nbt ? 9 : 3);
    else if (nrm) size += idxSize(nrm) * ((nbt && (A >> 31)) ? 3 : 1);
    const u32 c0 = (lo >> 13) & 3, c1 = (lo >> 15) & 3;
    if (c0 == 1) size += colSize((A >> 14) & 7);
    else size += idxSize(c0);
    if (c1 == 1) size += colSize((A >> 18) & 7);
    else size += idxSize(c1);
    struct Tex {
        u32 cnt, fmt;
    } tex[8] = {
        {(A >> 21) & 1, (A >> 22) & 7}, {B & 1, (B >> 1) & 7},         {(B >> 9) & 1, (B >> 10) & 7},
        {(B >> 18) & 1, (B >> 19) & 7}, {(B >> 27) & 1, (B >> 28) & 7}, {(C >> 5) & 1, (C >> 6) & 7},
        {(C >> 14) & 1, (C >> 15) & 7}, {(C >> 23) & 1, (C >> 24) & 7},
    };
    for (int i = 0; i < 8; i++) {
        const u32 t = (hi >> (2 * i)) & 3;
        if (t == 1) size += compSize(tex[i].fmt) * (tex[i].cnt ? 2 : 1);
        else size += idxSize(t);
    }
    return size;
}

inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
inline u32 be16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }

// Length of the command at p, or 0 if more bytes are needed.
size_t cmdLength(const u8* p, size_t avail) {
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        if (avail < 3) return 0;
        const u32 vs = vertexSize(op & 7);
        if (!vs && be16(p + 1)) {
            static int warned = 0;
            if (warned++ < 8)
                logf("gx", "draw %02X with %u vertices of size 0: VCD %08X/%08X VAT %08X %08X %08X", op, be16(p + 1), s_vcdLo,
                     s_vcdHi, s_vatA[op & 7], s_vatB[op & 7], s_vatC[op & 7]);
        }
        return 3 + size_t(be16(p + 1)) * vs;
    }
    switch (op) {
    case 0x00: case 0x48: return 1;
    case 0x08: return 6;
    case 0x10:
        if (avail < 5) return 0;
        return 5 + 4 * size_t((be32(p + 1) >> 16) + 1);
    case 0x20: case 0x28: case 0x30: case 0x38: return 5;
    case 0x40: return 9;
    case 0x61: return 5;
    default: return 1;  // unknown: skip a byte and resynchronize
    }
}

void runDisplayList(u32 addr, u32 size, int depth);

struct Recent {  // last commands, for desync reports
    u32 len = 0;
    bool dl = false;
    u8 bytes[12] = {};
};
Recent s_recent[48];
u32 s_recentPos = 0;

int s_cmdLog = 0;     // R2_FIFO_LOG=2: every command (first 16 bytes) once the GP FIFO has moved
int s_gpFifoSets = 0;  // GP FIFO base writes (GXSetGPFifo)

void execute(const u8* p, size_t len, int depth) {
    const u8 op = p[0];
    bool known = true;
    if (s_cmdLog >= 2 && s_gpFifoSets >= 2 && op != 0x00) {
        char hexb[64] = {};
        for (size_t k = 0; k < len && k < 16; k++) snprintf(hexb + 3 * k, 4, "%02X ", p[k]);
        logf("cmd", "%s%s(len %zu)", depth ? "DL " : "", hexb, len);
    }
    if (op >= 0x80 && op < 0xC0) {
        s_stats.draws++;
        s_stats.vertices += be16(p + 1);
    } else {
        switch (op) {
        case 0x00: case 0x48: break;
        case 0x08: {
            s_stats.cp++;
            const u8 reg = p[1];
            const u32 v = be32(p + 2);
            if (reg == 0x50) s_vcdLo = v;
            else if (reg == 0x60) s_vcdHi = v;
            else if ((reg & 0xF8) == 0x70) s_vatA[reg & 7] = v;
            else if ((reg & 0xF8) == 0x80) s_vatB[reg & 7] = v;
            else if ((reg & 0xF8) == 0x90) s_vatC[reg & 7] = v;
            break;
        }
        case 0x10: s_stats.xf++; break;
        case 0x20: case 0x28: case 0x30: case 0x38: s_stats.xf++; break;
        case 0x40: {
            s_stats.dl++;
            const u32 a = be32(p + 1), n = be32(p + 5);
            if (s_cmdLog >= 2 && s_gpFifoSets >= 2) logf("gx", "call display list %08X size %X", a, n);
            if (depth == 0) runDisplayList(a, n, depth + 1);
            break;
        }
        case 0x61: {
            s_stats.bp++;
            const u32 v = be32(p + 1);
            const u8 reg = u8(v >> 24);
            if (s_log && (reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52))
                logf("gx", "BP %02X = %06X%s", reg, v & 0xFFFFFF, depth ? " (display list)" : "");
            if (reg == 0x45 && (v & 2)) peFinish();
            else if (reg == 0x47) peToken(u16(v), true);
            else if (reg == 0x48) peToken(u16(v), false);
            break;
        }
        default:
            known = false;
            if (s_stats.unknown++ < 16) {
                logf("gx", "unknown FIFO opcode %02X (%s); last commands, oldest first:", op, depth ? "display list" : "FIFO");
                for (u32 i = 0; i < 48; i++) {
                    const Recent& r = s_recent[(s_recentPos + i) % 48];
                    if (!r.len) continue;
                    char hexb[64] = {};
                    for (u32 k = 0; k < r.len && k < 12; k++) snprintf(hexb + 3 * k, 4, "%02X ", r.bytes[k]);
                    logf("gx", "  %s len %5u: %s", r.dl ? "DL  " : "FIFO", r.len, hexb);
                }
            }
            break;
        }
    }
    {
        Recent& r = s_recent[s_recentPos++ % 48];
        r.len = u32(len);
        r.dl = depth != 0;
        memcpy(r.bytes, p, len < 12 ? len : 12);
    }
    // The renderer gets every command, display list contents inline (not the CALL_DL).
    if (s_sink && op != 0x40 && known) s_sink(p, u32(len));
}

// Commands in a display list run with the current CP state.
void runDisplayList(u32 addr, u32 size, int depth) {
    const u8* base = physPtr(addr, size);
    if (!base) {
        logf("gx", "display list outside memory: %08X size %X", addr, size);
        return;
    }
    size_t pos = 0;
    while (pos < size) {
        const size_t len = cmdLength(base + pos, size - pos);
        if (!len || pos + len > size) break;
        execute(base + pos, len, depth);
        pos += len;
    }
}

// CP_CR bits
constexpr u16 CR_READ = 1 << 0, CR_BP = 1 << 1, CR_OVFINT = 1 << 2, CR_UNFINT = 1 << 3, CR_LINK = 1 << 4,
              CR_BPINT = 1 << 5;
bool s_bpHit = false;  // CP_SR breakpoint: the GP stopped at the breakpoint

// Bytes written but not yet read by the GP (it only lags behind a breakpoint).
u32 cpDistance() {
    const u32 size = s_cpEnd + 32 - s_cpBase;
    if (!s_cpEnd || s_cpEnd <= s_cpBase) return 0;
    return s_cpWp >= s_cpRp ? s_cpWp - s_cpRp : s_cpWp + size - s_cpRp;
}
// The watermark flags are live, as the hardware evaluates them: the SDK suspends the writing
// thread on overflow and resumes it on underflow.
bool cpOverflow() { return cpDistance() > s_cpHi; }
bool cpUnderflow() { return cpDistance() < s_cpLo; }

void cpUpdateIrq() {
    const bool on = (s_bpHit && (s_cpCr & CR_BPINT)) || ((s_cpCr & CR_OVFINT) && cpOverflow()) ||
                    ((s_cpCr & CR_UNFINT) && cpUnderflow());
    piSetIrq(PI_CP, on);
}

// Feed the bytes between the CP read and write pointers to the command splitter. With the
// breakpoint enabled the GP stops when its read pointer reaches it (the game keeps one trailing
// its own write pointer and moves it from the breakpoint interrupt).
void cpRun() {
    if (!(s_cpCr & CR_READ)) return;  // GP FIFO reading disabled
    if (!s_cpEnd || s_cpEnd <= s_cpBase) return;
    int guard = 0;
    while (s_cpRp != s_cpWp && guard++ < 1000000) {
        if ((s_cpCr & CR_BP) && s_cpRp == s_cpBp) {
            if (!s_bpHit) {
                s_bpHit = true;
                cpUpdateIrq();
            }
            break;
        }
        // contiguous run up to the write pointer or the end of the buffer
        u32 stop = s_cpWp > s_cpRp ? s_cpWp : s_cpEnd + 32;
        if ((s_cpCr & CR_BP) && s_cpBp > s_cpRp && s_cpBp < stop) stop = s_cpBp;
        const u32 n = stop - s_cpRp;
        const u8* src = physPtr(s_cpRp, n);
        if (!src) {
            logf("gx", "CP FIFO outside memory: rp %08X wp %08X", s_cpRp, s_cpWp);
            s_cpRp = s_cpWp;
            break;
        }
        s_cmd.insert(s_cmd.end(), src, src + n);
        s_cpRp += n;
        if (s_cpRp >= s_cpEnd + 32) s_cpRp = s_cpBase;
    }
    size_t pos = 0;
    while (pos < s_cmd.size()) {
        const size_t len = cmdLength(&s_cmd[pos], s_cmd.size() - pos);
        if (!len || pos + len > s_cmd.size()) break;
        execute(&s_cmd[pos], len, 0);
        pos += len;
    }
    s_cmd.erase(s_cmd.begin(), s_cmd.begin() + ptrdiff_t(pos));
    cpUpdateIrq();
}

u32 regPair(u32 lo, u32 hi) { return (u32(s_cpReg[hi / 2] & 0x3FF) << 16) | s_cpReg[lo / 2]; }

}  // namespace

GxStats& gxStats() { return s_stats; }
void gxSetSink(GxSink sink) { s_sink = sink; }
void gxSetSyncHook(void (*hook)()) { s_syncHook = hook; }
void gxInit() {
    s_log = envFlag("R2_GX_LOG");
    if (const char* v = getenv("R2_FIFO_LOG")) s_cmdLog = atoi(v);
}

// --- gather pipe ------------------------------------------------------------------------
// The CPU's write-gather buffer: memory sees 32-byte bursts (the SDK pads with NOPs to flush).
namespace {
u8 s_gp[64];
u32 s_gpLen = 0;

void burst() {
    const u32 wp = s_piWp & 0x03FFFFE0;
    u8* dst = physPtr(wp, 32);
    if (!dst || !s_piEnd) {
        static int n = 0;
        if (n++ < 8) logf("gx", "gather pipe burst lost: no CPU FIFO (base %08X end %08X wp %08X)", s_piBase, s_piEnd, s_piWp);
        return;
    }
    memcpy(dst, s_gp, 32);
    u32 nwp = wp + 32;
    u32 wrap = s_piWp & 0x20000000u;
    if (nwp >= s_piEnd + 32) {  // past the end of the buffer: wrap
        nwp = s_piBase;
        wrap ^= 0x20000000u;
    }
    s_piWp = nwp | wrap;
    if (s_cpCr & CR_LINK) {  // linked: each burst also advances the GP's write pointer
        s_cpWp += 32;
        if (s_cpWp >= s_cpEnd + 32) s_cpWp = s_cpBase;
        cpRun();
        cpUpdateIrq();  // watermarks (cpRun returns early while reading is off)
    }
}
}  // namespace

void gxFifoWrite(u32 v, int size) {
    s_stats.bytes += u32(size);
    u8* d = s_gp + s_gpLen;  // big-endian into the pipe (s_gp has room past 32 bytes)
    if (size == 4) {
        const u32 b = __builtin_bswap32(v);
        memcpy(d, &b, 4);
    } else if (size == 2) {
        const u16 b = __builtin_bswap16(u16(v));
        memcpy(d, &b, 2);
    } else {
        *d = u8(v);
    }
    s_gpLen += u32(size);
    if (s_gpLen >= 32) {
        burst();
        s_gpLen -= 32;
        if (s_gpLen) memcpy(s_gp, s_gp + 32, s_gpLen);
    }
}

void gxFlushGatherPipe() {
    if (!s_gpLen) return;
    memset(s_gp + s_gpLen, 0, 32 - s_gpLen);  // NOPs
    s_gpLen = 0;
    burst();
}

// (read on the stall watchdog's thread while the game thread may be changing it: a hint, not exact)
std::string gxFifoText() {
    char b[256];
    snprintf(b, sizeof b,
             "GP FIFO %08X-%08X read %08X write %08X (%u bytes unread), breakpoint %08X %s%s, CP_CR %04X; "
             "CPU FIFO write %08X, %u bytes in the gather pipe",
             s_cpBase, s_cpEnd, s_cpRp, s_cpWp, cpDistance(), s_cpBp, (s_cpCr & CR_BP) ? "on" : "off",
             s_bpHit ? " (the GP stopped there)" : "", s_cpCr, s_piWp, s_gpLen);
    return b;
}

void gxResetGatherPipe() {
    static int n = 0;
    if (s_gpLen && n++ < 16) {
        char b[100] = {};
        for (u32 i = 0; i < s_gpLen && i < 32; i++) snprintf(b + 3 * i, 4, "%02X ", s_gp[i]);
        logf("gx", "gather pipe reset with %u bytes pending: %s", s_gpLen, b);
    }
    s_gpLen = 0;
}

// --- PI FIFO registers (0xCC00300C..0x14) ---------------------------------------------------
u32 gxPiRead(u32 off) {
    switch (off) {
    case 0x0C: return s_piBase;
    case 0x10: return s_piEnd;
    case 0x14: return s_piWp;
    default: return 0;
    }
}
bool fifoLog() {
    static const bool on = envFlag("R2_FIFO_LOG");
    return on;
}

void gxPiWrite(u32 off, u32 v) {
    if (fifoLog()) logf("fifo", "PI %02X = %08X (gp pending %u)", off, v, s_gpLen);
    switch (off) {
    case 0x0C: s_piBase = v & 0x03FFFFE0; break;
    // Battle of Hoth records display lists with base 0, end 0x04000000 (past all memory: no wrap)
    // and the write pointer at the list; masked to 26 bits that end read as 0, "no FIFO", and
    // every burst of the list was dropped. (Dolphin keeps all bits of these registers.)
    case 0x10: s_piEnd = v & 0x07FFFFE0; break;
    case 0x14: s_piWp = v & 0x23FFFFE0; break;
    case 0x18:  // FIFO reset: the write pointer goes back to the base
        if (v & 1) s_piWp = s_piBase;
        break;
    }
}

// --- CP registers (0xCC000000) ------------------------------------------------------------
u16 gxCpRead(u32 off) {
    off &= 0x7E;
    if (s_syncHook) s_syncHook();
    switch (off) {
    case 0x00: {  // status: overflow, underflow, read idle, command idle, breakpoint
        // A GP halted at a breakpoint with commands still behind it is not idle: before moving
        // the GP FIFO the game spins on GXGetGPStatus until read+command idle, while its
        // interrupt callback releases the queued frames one breakpoint at a time. Reporting
        // idle at the breakpoint made it switch FIFOs with two frames (and their draw-dones)
        // still unread, so its frames-in-flight counter never drained.
        const bool readIdle = s_cpRp == s_cpWp;
        const bool cmdIdle = readIdle || !(s_cpCr & CR_READ);
        return u16((cpOverflow() ? 1 : 0) | (cpUnderflow() ? 2 : 0) | (readIdle ? 4 : 0) | (cmdIdle ? 8 : 0) |
                   (s_bpHit ? 0x10 : 0));
    }
    case 0x02: return s_cpCr;
    case 0x30: case 0x32: {  // read-write distance: what is still unread (behind a breakpoint)
        const u32 size = s_cpEnd + 32 - s_cpBase;
        const u32 d = s_cpWp >= s_cpRp ? s_cpWp - s_cpRp : s_cpWp + size - s_cpRp;
        return off == 0x30 ? u16(d) : u16(d >> 16);
    }
    case 0x34: return u16(s_cpWp);
    case 0x36: return u16(s_cpWp >> 16);
    case 0x38: return u16(s_cpRp);
    case 0x3A: return u16(s_cpRp >> 16);
    default: return s_cpReg[off / 2];
    }
}
void gxCpWrite(u32 off, u16 v) {
    off &= 0x7E;
    if (fifoLog() && off >= 0x02) {
        logf("fifo", "CP %02X = %04X (rp %08X wp %08X pi wp %08X)", off, v, s_cpRp, s_cpWp, s_piWp);
        if (s_cmdLog >= 3 && (off == 0x02 || off == 0x20 || off == 0x3C)) logf("fifo", "  from %s", traceRing(10).c_str());
    }
    s_cpReg[off / 2] = v;
    switch (off) {
    case 0x02: {
        const u16 old = s_cpCr;
        s_cpCr = v;
        if (s_log && (old ^ v)) logf("gx", "CP_CR %04X (read %d, link %d)", v, v & 1, (v >> 4) & 1);
        if (!(v & CR_BP)) s_bpHit = false;  // disabling the breakpoint releases the GP
        cpUpdateIrq();
        cpRun();
        break;
    }
    case 0x04:  // CP_CLEAR: the watermark flags are live; re-evaluate
        cpUpdateIrq();
        break;
    case 0x20: case 0x22: s_cpBase = regPair(0x20, 0x22) & 0x03FFFFE0; if (off == 0x20) s_gpFifoSets++; break;
    case 0x24: case 0x26: s_cpEnd = regPair(0x24, 0x26) & 0x03FFFFE0; break;
    case 0x28: case 0x2A: s_cpHi = regPair(0x28, 0x2A); break;
    case 0x2C: case 0x2E: s_cpLo = regPair(0x2C, 0x2E); break;
    case 0x30: case 0x32: break;  // read-write distance: derived from the pointers
    case 0x34: case 0x36:
        s_cpWp = regPair(0x34, 0x36) & 0x03FFFFE0;
        if (off == 0x36) cpRun();
        break;
    case 0x38: case 0x3A:
        s_cpRp = regPair(0x38, 0x3A) & 0x03FFFFE0;
        s_cmd.clear();
        break;
    case 0x3C: case 0x3E:
        s_cpBp = regPair(0x3C, 0x3E) & 0x03FFFFE0;
        if (s_bpHit && s_cpRp != s_cpBp) {  // moved: the GP may go on
            s_bpHit = false;
            cpUpdateIrq();
        }
        if (off == 0x3E) cpRun();
        break;
    }
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "../savestate.h"
namespace rt {
void gxFifoSaveState(state::Writer& w) {
    w.section("GXFF");
    w.pod(s_piBase);
    w.pod(s_piEnd);
    w.pod(s_piWp);
    w.pod(s_cpReg);
    w.pod(s_cpBase);
    w.pod(s_cpEnd);
    w.pod(s_cpWp);
    w.pod(s_cpRp);
    w.pod(s_cpHi);
    w.pod(s_cpLo);
    w.pod(s_cpBp);
    w.pod(s_cpCr);
    w.vec(s_cmd);
    w.pod(s_vcdLo);
    w.pod(s_vcdHi);
    w.pod(s_vatA);
    w.pod(s_vatB);
    w.pod(s_vatC);
    w.pod(s_bpHit);
    w.pod(s_gp);
    w.pod(s_gpLen);
    w.pod(s_gpFifoSets);
}

void gxFifoLoadState(state::Reader& r) {
    r.section("GXFF");
    r.pod(s_piBase);
    r.pod(s_piEnd);
    r.pod(s_piWp);
    r.pod(s_cpReg);
    r.pod(s_cpBase);
    r.pod(s_cpEnd);
    r.pod(s_cpWp);
    r.pod(s_cpRp);
    r.pod(s_cpHi);
    r.pod(s_cpLo);
    r.pod(s_cpBp);
    r.pod(s_cpCr);
    r.vec(s_cmd);
    r.pod(s_vcdLo);
    r.pod(s_vcdHi);
    r.pod(s_vatA);
    r.pod(s_vatB);
    r.pod(s_vatC);
    r.pod(s_bpHit);
    r.pod(s_gp);
    r.pod(s_gpLen);
    r.pod(s_gpFifoSets);
    cpUpdateIrq();
}
}  // namespace rt
