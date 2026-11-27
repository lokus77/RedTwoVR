// GameCube hardware registers (0xCC000000) as the game's SDK code drives them.
//
// The whole SDK runs recompiled; devices are modelled at the register level, written from
// the public descriptions of the hardware (YAGCD and the SDK's own register usage).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
#include <set>
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>

#include "../card_image.h"
#include "../mods.h"
#include "../achievements.h"
#include "../runtime.h"
#include "../settings.h"
#include "gx_fifo.h"
#include "musyx_dsp.h"

namespace rt {

extern void (*g_eventHandlers[EV_COUNT])();
extern volatile u32 g_irqLine;

u64 g_viLateMaxTb = 0, g_viLateSumTb = 0;  // how late retraces fire (R2_GX_LOG reports, resets)

namespace {

void siPollAll();
void cardFlushAll();

bool s_logIo = false;
std::set<u32> s_seen;            // outside the register block (EFB etc.)
std::vector<bool> s_seenIo(0x20000);  // 0xCC00xxxx registers: (offset, read/write) seen yet
void noteAccess(const char* dev, u32 a, bool write, u32 v) {
    if (s_logIo) {
        logf("io", "%s %s %08X = %08X", dev, write ? "W" : "R", a, v);
        return;
    }
    if ((a >> 16) == 0xCC00) {  // the hot path: one bit per register and direction
        const u32 k = ((a & 0xFFFE) << 1) | (write ? 1u : 0u);
        if (s_seenIo[k]) return;
        s_seenIo[k] = true;
    } else if (!s_seen.insert((a & ~1u) | (write ? 1u : 0u)).second) {
        return;
    }
    logf("io", "first %s of %s reg %08X (%08X)", write ? "write" : "read", dev, a, v);
}

// =======================================================================================
// PI: interrupt controller, CPU FIFO pointers
// =======================================================================================
u32 pi_intsr = 0, pi_intmr = 0;

void piUpdate() {
    if (pi_intsr & pi_intmr) {
        g_irqLine |= 1;
        g_irqPending = 1;
    } else {
        g_irqLine &= ~1u;
    }
}

u32 piRead(u32 off) {
    switch (off) {
    case 0x00: return pi_intsr | 0x00010000;  // bit 16: reset switch released
    case 0x04: return pi_intmr;
    case 0x0C: case 0x10: case 0x14: return gxPiRead(off);
    case 0x2C: return 0x246500B1;  // Flipper revision
    default: return 0;
    }
}
void piWrite(u32 off, u32 v) {
    switch (off) {
    case 0x00: pi_intsr &= ~(v & (PI_ERROR | PI_RSW)); piUpdate(); break;
    case 0x04: pi_intmr = v; piUpdate(); break;
    case 0x0C: case 0x10: case 0x14: case 0x18: gxPiWrite(off, v); break;
    case 0x24:
        logf("pi", "reset code write %08X (the game asked for a reset)", v);
        break;
    default: break;
    }
}

// =======================================================================================
// VI: video interface (timing only; the picture comes from EFB copies)
// =======================================================================================
u16 vi[0x40] = {};
u64 s_viFrameStartTb = 0;
u64 s_viDueTb = 0;  // when the timer was asked to raise the current retrace
u32 s_viFrames = 0;
// Retrace period: 59.94 Hz (NTSC) unless the player picked a higher frame rate (settings fps):
// the game paces its frames on the retrace, and its frame delta is then corrected
// (hle_GameFrameDelta), so gameplay keeps its speed.
u64 kViPeriod = kTbClock * 1001 / 60000;
double s_viHz = 60000.0 / 1001.0;

}  // namespace

// Skipping a cinematic (hle_game.cpp) runs the game many times faster: the audio clock keeps
// pace (onAi) silently, and disc reads are as much faster so streams keep up.
static double s_audioSpeed = 1.0;
static bool s_audioMute = false;
static double s_dvdBoost = 1.0;
void setAudioSpeed(double speed, bool mute) {
    s_audioSpeed = speed < 1.0 ? 1.0 : speed;
    s_audioMute = mute;
}
void setDvdBoost(double boost) { s_dvdBoost = boost < 1.0 ? 1.0 : boost; }

void viSetRate(int fps) { viSetRateHz(fps == 60 ? 60000.0 / 1001.0 : double(fps)); }
void viSetRateHz(double hz) {
    if (hz < 1.0) hz = 60000.0 / 1001.0;
    s_viHz = hz;
    kViPeriod = u64(double(kTbClock) / s_viHz + 0.5);
    logf("vi", "retrace rate %.2f Hz", s_viHz);
}
double viRate() { return s_viHz; }
u32 viRetraceCount() { return s_viFrames; }

// R2_AI_LOG=1: audio events (B boundary + block latched, A AI interrupt acknowledged,
// M MusyX mixed a block into an AI buffer, D DSP interrupt acknowledged); a block that plays
// without a new mix since it last played prints the recent ones.
namespace {
struct AiEvent {
    u64 tb;
    char kind;
    u32 v;
};
AiEvent s_aiEvents[24];
u32 s_aiEventCount = 0;
std::unordered_map<u32, u32> s_aiMixes, s_aiMixesPlayed;
}  // namespace
bool aiTraceOn() {
    static const bool on = envFlag("R2_AI_LOG");
    return on;
}
void aiTraceEvent(char kind, u32 v) {
    if (!aiTraceOn()) return;
    if (kind == 'M' || kind == 'B') v &= 0x03FFFFE0;
    s_aiEvents[s_aiEventCount++ % std::size(s_aiEvents)] = {tbNow(), kind, v};
    if (kind == 'M') s_aiMixes[v]++;
    if (kind != 'B') return;
    const u32 mixes = s_aiMixes[v];
    const auto it = s_aiMixesPlayed.find(v);
    // (a skipped cinematic runs the audio clock fast and silent: blocks repeat there by design)
    if (it != s_aiMixesPlayed.end() && it->second == mixes && s_audioSpeed == 1.0 && !s_audioMute) {
        std::string trail;
        const u64 now = tbNow();
        for (u32 i = 0; i < std::size(s_aiEvents) && i < s_aiEventCount; i++) {
            const AiEvent& e = s_aiEvents[(s_aiEventCount - 1 - i) % std::size(s_aiEvents)];
            char b[48];
            if (e.v) snprintf(b, sizeof b, " %c:%04X@%.1f", e.kind, e.v & 0xFFFF, double(now - e.tb) * 1000.0 / kTbClock);
            else snprintf(b, sizeof b, " %c@%.1f", e.kind, double(now - e.tb) * 1000.0 / kTbClock);
            trail += b;
        }
        logf("ai", "block %08X plays without a new mix; recent (ms ago):%s", v, trail.c_str());
    }
    s_aiMixesPlayed[v] = mixes;
}

namespace {

void viUpdateIrq() {
    bool any = false;
    for (int n = 0; n < 4; n++) {
        const u16 hi = vi[(0x30 + 4 * n) / 2];
        if ((hi & 0x8000) && (hi & 0x1000)) any = true;
    }
    piSetIrq(PI_VI, any);
}

void onVi() {
    const u64 now = tbNow(), due = s_viDueTb;
    if (due && now > due) {
        g_viLateSumTb += now - due;
        if (now - due > g_viLateMaxTb) g_viLateMaxTb = now - due;
    }
    s_viFrameStartTb = now;
    s_viFrames++;
    for (int n = 0; n < 2; n++) {
        u16& hi = vi[(0x30 + 4 * n) / 2];
        if (hi & 0x1000) hi |= 0x8000;
    }
    viUpdateIrq();
    siPollAll();
    cardFlushAll();
    mods::codesFrame();  // Gecko / Action Replay codes and patches, like Dolphin once per field
    ra::frame();         // RetroAchievements: server replies, then this frame's checks (Dolphin: once per field too)
    viFrameHook();
    // The timer fires a little late every time (and the guest may be busy for a moment): counting
    // the next period from the actual retrace ran 60 fps at ~59 and 144 at ~139. Schedule early by
    // the average lateness instead, at most 1/8 of a period: retraces keep (nearly) their spacing.
    // (Keeping a strict grid fired them back to back after a late one, which the game's frame
    // pacing never sees on hardware: it hung switching screens.)
    // Only timer jitter counts: a stall (the guest busy, a late safepoint) is not repeated.
    static double lateEma = 0.0;
    const u64 late = due && now > due ? now - due : 0;
    if (late < kViPeriod / 4) lateEma = lateEma * 0.9 + double(late) * 0.1;
    const u64 early = std::min(u64(lateEma), kViPeriod / 8);
    s_viDueTb = s_viFrameStartTb + kViPeriod - early;
    schedule(EV_VI, s_viDueTb);
}

u16 viRead(u32 off) {
    if (off == 0x2C) {  // VCOUNT
        const u64 ph = tbNow() - s_viFrameStartTb;
        return u16(1 + (ph * 263 / kViPeriod) % 263);
    }
    if (off == 0x2E) return 1;
    if (off == 0x6E) return 0;  // VISEL: no component cable
    return vi[(off & 0x7F) / 2];
}
// Frame buffers the VI has been told to scan out (TFBL 0x1C, BFBL 0x24). The renderer
// presents only display copies into one of these: the game also clears EFB bands with
// display copies into a scratch buffer the VI never shows.
std::vector<u32> s_viFbs;
u32 s_viTop[2] = {};  // the two most recent distinct top-field frame buffers (front and back)

void viNoteFb(u32 hiOff) {
    const u32 v = (u32(vi[hiOff / 2]) << 16) | vi[hiOff / 2 + 1];
    const u32 addr = (v & 0x10000000) ? (v & 0x00FFFFFF) << 5 : v & 0x00FFFFFF;  // POFF: address >> 5
    if (!addr) return;
    if (hiOff == 0x1C && addr != s_viTop[0]) {
        s_viTop[1] = s_viTop[0];
        s_viTop[0] = addr;
    }
    if (std::find(s_viFbs.begin(), s_viFbs.end(), addr) != s_viFbs.end()) return;
    if (s_viFbs.size() >= 32) s_viFbs.erase(s_viFbs.begin());
    s_viFbs.push_back(addr);
    logf("vi", "frame buffer %08X", addr);
}


void viWrite(u32 off, u16 v) {
    vi[(off & 0x7F) / 2] = v;
    if (off >= 0x30 && off < 0x40) viUpdateIrq();
    // The 32-bit frame buffer registers arrive as two 16-bit writes (high, then low): take
    // the address once the low half is in, so no half-updated mix is recorded.
    if (off == 0x1E) viNoteFb(0x1C);
    else if (off == 0x26) viNoteFb(0x24);
}

// =======================================================================================
// DI: DVD interface
// =======================================================================================
u32 di[0x10] = {};
enum { DISR = 0, DICVR, DICMD0, DICMD1, DICMD2, DIMAR, DILEN, DICR, DIIMM, DICFG };
u32 s_diPendingErr = 0;

void diUpdateIrq() {
    const u32 sr = di[DISR];
    const bool on = ((sr & 0x04) && (sr & 0x02)) || ((sr & 0x10) && (sr & 0x08)) || ((sr & 0x40) && (sr & 0x20)) ||
                    ((di[DICVR] & 0x04) && (di[DICVR] & 0x02));
    piSetIrq(PI_DI, on);
}

struct DiOp {
    bool active = false;
    u32 cmd0 = 0, off = 0, len = 0, mar = 0;
} s_diOp;
u64 s_diDueTb = 0;  // when the emulated transfer ended (host state: a late read's wait, for the log)

u64 diDiscOffset() { return (s_diOp.cmd0 & 0xC0) == 0x40 ? 0 : u64(s_diOp.off) << 2; }

void onDi() {
    if (!s_diOp.active) return;
    const u32 cmd = s_diOp.cmd0 >> 24;
    if (cmd == 0xA8) {
        u8* dst = physPtr(s_diOp.mar, s_diOp.len);
        if (!dst) fatal("DVD read to bad address %08X len %X", s_diOp.mar, s_diOp.len);
        const u64 off = diDiscOffset();
        // The bytes come from the reader thread (disc_reader.cpp), which started at diStart.
        // Not in yet (a slow drive): the drive finishes later, as a real one with a long seek
        // would, and the game runs on meanwhile instead of freezing.
        switch (discTake(off, dst, s_diOp.len)) {
        case DiscData::Pending:
            if (!s_diDueTb) s_diDueTb = tbNow();
            schedule(EV_DI, tbNow() + kTbClock / 1000);
            return;
        case DiscData::Failed: fatal("DVD read past the disc: %llX + %X", (unsigned long long)off, s_diOp.len);
        case DiscData::Ready: break;
        }
        if (s_diDueTb) {
            const u64 now = tbNow();  // (a save state loaded meanwhile moves the time base back)
            const double ms = now > s_diDueTb ? double(now - s_diDueTb) * 1000.0 / double(kTbClock) : 0.0;
            s_diDueTb = 0;
            if (ms >= 20.0) discLate(off, s_diOp.len, ms);
        }
        watchHost(s_diOp.mar, s_diOp.len, "DVD read");
        di[DIMAR] = s_diOp.mar + s_diOp.len;
    }
    s_diOp.active = false;
    di[DILEN] = 0;
    di[DICR] &= ~1u;
    di[DISR] |= 0x10;  // TCINT
    diUpdateIrq();
}

void diStart() {
    const u32 cmd0 = di[DICMD0];
    const u32 cmd = cmd0 >> 24;
    s_diOp = {true, cmd0, di[DICMD1], di[DILEN], di[DIMAR]};
    u64 latencyTb = kTbClock / 2000;  // 0.5 ms command overhead
    switch (cmd) {
    case 0xA8:
        if ((cmd0 & 0xC0) == 0x40) s_diOp.len = 0x20;  // read disc ID
        {   // transfer time: R2_DVD_SPEED=<MB/s>, else the drive's ~3 MB/s or 200 (fast loading)
            static const double forced = [] {
                const char* v = getenv("R2_DVD_SPEED");
                return v ? atof(v) : 0.0;
            }();
            const double mbps = forced > 0 ? forced : settings::get().fastDisc ? 200.0 : 3.0;
            latencyTb += u64(double(s_diOp.len) * double(kTbClock) / (mbps * 1024.0 * 1024.0));
            latencyTb = u64(double(latencyTb) / s_dvdBoost);
        }
        if (static const bool dvdLog = envFlag("R2_DVD_LOG"); dvdLog) {
            const u64 o = u64(s_diOp.off) << 2;
            logf("dvd", "read %08llX len %X -> %08X  %s", (unsigned long long)o, s_diOp.len, s_diOp.mar, discFileAt(o).c_str());
        }
        s_diDueTb = 0;
        discNoteRead(diDiscOffset());  // which level the game loads (the log, Discord Rich Presence)
        discPrefetch(diDiscOffset(), s_diOp.len);
        break;
    case 0xAB: break;  // seek
    case 0x12: {       // inquiry
        u8* dst = physPtr(di[DIMAR], 0x20);
        if (dst) {
            memset(dst, 0, 0x20);
            dst[2] = 0x00; dst[3] = 0x02;  // revision
            dst[4] = 0x20; dst[5] = 0x01; dst[6] = 0x06; dst[7] = 0x08;  // date
            dst[8] = 0x61;
        }
        break;
    }
    case 0xE0: di[DIIMM] = s_diPendingErr; break;  // request error
    case 0xE1: case 0xE2: case 0xE4: di[DIIMM] = 0; break;  // audio streaming: not used
    case 0xE3: break;  // stop motor
    default: logf("dvd", "unknown command %08X %08X %08X", cmd0, di[DICMD1], di[DICMD2]); break;
    }
    schedule(EV_DI, tbNow() + latencyTb);
}

u32 diRead(u32 off) { return di[(off & 0x3F) / 4]; }
void diWrite(u32 off, u32 v) {
    const u32 r = (off & 0x3F) / 4;
    switch (r) {
    case DISR: {
        const u32 ack = v & 0x54;
        di[DISR] = (di[DISR] & ~0x2Au) | (v & 0x2A);
        di[DISR] &= ~ack;
        diUpdateIrq();
        break;
    }
    case DICVR: {
        di[DICVR] = (di[DICVR] & ~0x02u) | (v & 0x02);
        if (v & 0x04) di[DICVR] &= ~0x04u;
        diUpdateIrq();
        break;
    }
    case DICR:
        di[DICR] = v;
        if (v & 1) diStart();
        break;
    default: di[r] = v; break;
    }
}

// =======================================================================================
// SI: serial interface (controllers)
// =======================================================================================
u32 si[0x40] = {};  // 0x00..0xFF (32-bit regs incl. the 128-byte I/O buffer at 0x80)
constexpr u32 SIPOLL = 0x30 / 4, SICOMCSR = 0x34 / 4, SISR = 0x38 / 4;
PadState s_pad[4];

void siUpdateIrq() {
    const u32 cs = si[SICOMCSR];
    const bool on = ((cs & 0x80000000u) && (cs & 0x40000000u)) || ((cs & 0x10000000u) && (cs & 0x08000000u));
    piSetIrq(PI_SI, on);
}

void siPollResponse(int ch, u32& hi, u32& lo) {
    const PadState& p = s_pad[ch];
    const u16 buttons = ch == 0 ? u16(p.buttons | mouseMenuPadButtons()) : p.buttons;  // (the mouse in the game's menus)
    hi = (u32(buttons) << 16) | (u32(p.stickX) << 8) | p.stickY;
    lo = (u32(p.substickX) << 24) | (u32(p.substickY) << 16) | (u32(p.triggerL) << 8) | p.triggerR;
}

void siPollAll() {
    const u32 poll = si[SIPOLL];
    for (int ch = 0; ch < 4; ch++) {
        if (!(poll & (0x80u >> ch))) continue;
        u32 hi, lo;
        if (!s_pad[ch].connected) {
            si[SISR] |= 0x08u << (8 * (3 - ch));  // NOREP
            continue;
        }
        siPollResponse(ch, hi, lo);
        si[ch * 3 + 1] = hi;
        si[ch * 3 + 2] = lo;
        si[SISR] |= 0x20u << (8 * (3 - ch));  // RDST
    }
    if (si[SISR] & 0x20202020u) {
        si[SICOMCSR] |= 0x10000000u;  // RDSTINT
        siUpdateIrq();
    }
}

void siTransfer() {
    const u32 cs = si[SICOMCSR];
    const int ch = (cs >> 1) & 3;
    u32 outLen = (cs >> 16) & 0x7F, inLen = (cs >> 8) & 0x7F;
    if (!outLen) outLen = 128;
    if (!inLen) inLen = 128;
    u8 buf[128];
    for (int i = 0; i < 32; i++) {
        const u32 w = si[0x20 + i];
        buf[i * 4] = u8(w >> 24); buf[i * 4 + 1] = u8(w >> 16); buf[i * 4 + 2] = u8(w >> 8); buf[i * 4 + 3] = u8(w);
    }
    u8 resp[128] = {};
    bool ok = s_pad[ch].connected;
    if (ok) {
        const PadState& p = s_pad[ch];
        switch (buf[0]) {
        case 0x00: case 0xFF:  // reset / identify: standard controller
            resp[0] = 0x09; resp[1] = 0x00; resp[2] = 0x00;
            break;
        case 0x40: {
            u32 hi, lo;
            siPollResponse(ch, hi, lo);
            for (int i = 0; i < 4; i++) { resp[i] = u8(hi >> (24 - 8 * i)); resp[4 + i] = u8(lo >> (24 - 8 * i)); }
            break;
        }
        case 0x41: case 0x42:  // origin / recalibrate
            resp[0] = 0x00; resp[1] = 0x80;
            resp[2] = 0x80; resp[3] = 0x80; resp[4] = 0x80; resp[5] = 0x80;
            resp[6] = p.triggerL; resp[7] = p.triggerR; resp[8] = 0; resp[9] = 0;
            break;
        default:
            logf("si", "unknown SI command %02X on port %d", buf[0], ch);
            ok = false;
            break;
        }
    }
    for (u32 i = 0; i < 32; i++)
        si[0x20 + i] = (u32(resp[i * 4]) << 24) | (u32(resp[i * 4 + 1]) << 16) | (u32(resp[i * 4 + 2]) << 8) | resp[i * 4 + 3];
    si[SICOMCSR] &= ~1u;
    si[SICOMCSR] |= 0x80000000u;  // TCINT
    if (!ok) {
        si[SICOMCSR] |= 0x20000000u;           // COMERR
        si[SISR] |= 0x08u << (8 * (3 - ch));   // NOREP
    }
    (void)inLen;
    siUpdateIrq();
}

u32 siRead(u32 off) {
    const u32 r = (off & 0xFF) / 4;
    const u32 v = si[r];
    if (r < 12 && (r % 3) == 1) si[SISR] &= ~(0x20u << (8 * (3 - r / 3)));  // reading INBUFH clears RDST
    return v;
}
void siWrite(u32 off, u32 v) {
    const u32 r = (off & 0xFF) / 4;
    switch (r) {
    case SICOMCSR: {
        u32 cs = si[SICOMCSR];
        if (v & 0x80000000u) cs &= ~0x80000000u;  // ack TCINT
        if (v & 0x10000000u) cs &= ~0x10000000u;  // ack RDSTINT
        const u32 keep = 0x80000000u | 0x10000000u | 0x20000000u;
        cs = (cs & keep) | (v & ~keep);
        si[SICOMCSR] = cs;
        if (v & 1) siTransfer();
        siUpdateIrq();
        break;
    }
    case SISR:
        if (v & 0x80000000u) {  // WR: the output buffers take effect; bits 0-1 drive the rumble motor
            for (int n = 0; n < 4; n++) input::rumble(n, si[n * 3] & 3);
        }
        si[SISR] &= ~(v & 0x0F0F0F0Fu);  // clear error bits written as 1
        break;
    case SIPOLL:
        si[SIPOLL] = v;
        break;
    default: si[r] = v; break;
    }
}

// =======================================================================================
// EXI: memory card in slot A, IPL ROM / RTC / SRAM
// =======================================================================================
struct ExiChan {
    u32 csr = 0, mar = 0, len = 0, cr = 0, data = 0;
} exi[3];
u8 s_sram[64];
struct IplDev {
    bool haveCmd = false;
    u32 cmd = 0;
    u32 pos = 0;
} s_ipl;

void exiUpdateIrq() {
    bool on = false;
    for (auto& c : exi)
        if (((c.csr & 0x002) && (c.csr & 0x001)) || ((c.csr & 0x008) && (c.csr & 0x004)) || ((c.csr & 0x800) && (c.csr & 0x400)))
            on = true;
    piSetIrq(PI_EXI, on);
}

// OSSramEx (SRAM 0x14..0x3F): flashID[2][12] at 0x14, flashIDCheckSum[2] at 0x3A. CARD
// mounts an unlocked card only if each checksum is the inverted byte sum of its flash ID.
void sramSetFlashId(int slot, const u8* id) {
    u8 sum = 0;
    for (int i = 0; i < 12; i++) {
        s_sram[0x14 + 12 * slot + i] = id[i];
        sum = u8(sum + id[i]);
    }
    s_sram[0x3A + slot] = u8(~sum);
}

void sramInit() {
    memset(s_sram, 0, sizeof s_sram);
    const u8 zero[12] = {};
    sramSetFlashId(0, zero);
    sramSetFlashId(1, zero);
    s_sram[0x13] = 0x2C;  // flags: stereo
    s_sram[0x12] = 0;     // English
    u16 sum = 0, inv = 0;
    for (int i = 0; i < 4; i++) {
        const u16 w = u16(s_sram[0x0C + 2 * i] << 8 | s_sram[0x0D + 2 * i]);
        sum = u16(sum + w);
        inv = u16(inv + u16(~w));
    }
    s_sram[0] = u8(sum >> 8); s_sram[1] = u8(sum);
    s_sram[2] = u8(inv >> 8); s_sram[3] = u8(inv);
}

u8 iplReadByte() {
    const u32 a = (s_ipl.cmd & 0x7FFFFFFF) >> 6;
    u8 v = 0;
    if ((s_ipl.cmd & 0x7FFFFF00) == 0x20000000) {  // RTC
        const u32 secs = rtcSeconds();  // local time since 2000-01-01 (cpu.cpp clockInit)
        v = u8(secs >> (24 - 8 * (s_ipl.pos & 3)));
    } else if ((s_ipl.cmd & 0x7FFFFF00) == 0x20000100) {  // SRAM
        v = s_sram[s_ipl.pos & 63];
    } else {
        (void)a;  // IPL ROM: not available (fonts come from the game)
    }
    s_ipl.pos++;
    return v;
}
void iplWriteByte(u8 b) {
    if ((s_ipl.cmd & 0x7FFFFF00) == 0x20000100) s_sram[s_ipl.pos & 63] = b;
    s_ipl.pos++;
}

// --- memory card (EXI channel 0/1, device 0) ---------------------------------------------
// A Memory Card 59 (4 Mbit, 8 KiB sectors) behind the flash command set the SDK's CARD
// library speaks. The image is a raw dump of the card (the same layout as Dolphin's .raw).
// It reports itself unlocked, so the SDK skips the DSP-assisted unlock handshake.
struct MemCard {
    bool present = false;
    std::vector<u8> data;
    std::string path;
    u32 id = 0x00000004;  // size 4 Mbit; latency index 0 (4 bytes); sector size index 0 (8 KiB)
    u8 status = 0x41;     // ready | unlocked
    bool irqEnabled = false;
    // the transfer since the last chip select
    u32 pos = 0;
    u8 cmd = 0;
    u8 args[5] = {};
    u32 addr = 0;
    u32 dirtyLo = ~0u, dirtyHi = 0;
    FILE* file = nullptr;  // the image, open for the whole session
    bool detached = false;  // a save was imported into the file: the game's writes stay in memory
} s_card[2];
constexpr u32 kCardLatency = 4;  // dummy bytes between a read command and its data

u32 cardAddr(const u8* a) { return (u32(a[1] & 0x7F) << 17) | (u32(a[2]) << 9) | (u32(a[3] & 3) << 7) | (a[4] & 0x7F); }

void cardMarkDirty(MemCard& m, u32 lo, u32 hi) {
    m.dirtyLo = std::min(m.dirtyLo, lo);
    m.dirtyHi = std::max(m.dirtyHi, hi);
}

// Changes reach the image file once a frame (cardFlushAll from the retrace) through a handle
// kept open: opening the file for every 128-byte page program cost several milliseconds each
// on Windows (file-open scanning), so saving stalled the game for seconds.
void cardFlush(MemCard& m) {
    if (m.dirtyLo >= m.dirtyHi || m.path.empty() || m.detached) return;
    if (!m.file) m.file = fopen(m.path.c_str(), "r+b");
    if (m.file && fseek(m.file, long(m.dirtyLo), SEEK_SET) == 0 &&
        fwrite(m.data.data() + m.dirtyLo, 1, m.dirtyHi - m.dirtyLo, m.file) == m.dirtyHi - m.dirtyLo &&
        fflush(m.file) == 0) {
        m.dirtyLo = ~0u;
        m.dirtyHi = 0;
    } else {
        static bool warned = false;
        if (!warned) logf("card", "cannot write %s", m.path.c_str());
        warned = true;
    }
}
void cardFlushAll() {
    for (auto& m : s_card)
        if (m.present) cardFlush(m);
}

void cardInit(int slot, const std::string& path) {
    MemCard& m = s_card[slot];
    m = MemCard{};
    m.path = path;
    // An image of another standard size (Dolphin's and Nintendont's cards are usually 251 or 2043
    // blocks) is that card: the card ID reports its size, as CARDMount checks it against the
    // header's (a 2 MiB card read as 4 Mbit was "corrupted data, needs to be formatted").
    FILE* f = fopen(path.c_str(), "rb");
    if (f && fseek(f, 0, SEEK_END) == 0) {
        const long bytes = ftell(f);
        for (u32 mbit = 4; mbit <= 128; mbit *= 2)
            if (bytes == long(mbit) * 1024 * 1024 / 8) m.id = mbit;  // (latency and sector size bits 0)
        if (bytes != long(m.id & 0xFC) * 1024 * 1024 / 8)
            logf("card", "slot %c: %s has %ld bytes, no memory card size: read as a 4 Mbit (59-block) card", 'A' + slot,
                 path.c_str(), bytes);
        fseek(f, 0, SEEK_SET);
    }
    const u32 size = (m.id & 0xFC) * 1024 * 1024 / 8;
    m.data.assign(size, 0xFF);  // erased flash
    // A new card, or one that is still all erased flash, is formatted here (as the save importer
    // does: CARDFormat's layout, flash ID 0, which cardSyncSram recovers), so the game's first
    // start asks no "format the card?"; with CardCheckSlot's create flag (hle_game.cpp) it makes
    // its save by itself. From ZeDctr's Red Two Plus fork. A card with anything on it stays as is.
    const auto formatNew = [&] { card::format(m.data, u64(std::max<time_t>(0, time(nullptr) - 946684800)) * kTbClock); };
    if (f) {
        fread(m.data.data(), 1, size, f);
        fclose(f);
        if (!card::isFormatted(m.data) && std::all_of(m.data.begin(), m.data.end(), [](u8 b) { return b == 0xFF; })) {
            formatNew();
            if (FILE* f2 = fopen(path.c_str(), "wb")) {
                fwrite(m.data.data(), 1, size, f2);
                fclose(f2);
            }
            logf("card", "slot %c: %s was blank: formatted (%u blocks)", 'A' + slot, path.c_str(), size / 0x2000 - 5);
        } else {
            logf("card", "slot %c: %s (%u Mbit, %u blocks)", 'A' + slot, path.c_str(), m.id & 0xFC, size / 0x2000 - 5);
        }
    } else if (FILE* f2 = fopen(path.c_str(), "wb")) {
        formatNew();
        fwrite(m.data.data(), 1, size, f2);
        fclose(f2);
        logf("card", "slot %c: new card %s (formatted, %u blocks)", 'A' + slot, path.c_str(), size / 0x2000 - 5);
    } else {
        logf("card", "slot %c: cannot create %s; no card", 'A' + slot, path.c_str());
        return;
    }
    m.present = true;
    m.file = fopen(path.c_str(), "r+b");
    static const bool atExit = [] { return std::atexit(cardFlushAll) == 0; }();
    (void)atExit;
}

}  // namespace (reopened below)

// --- card images for the save importer (runtime.h) ----------------------------------------
std::string cardImagePath(int slot) {
    if (slot != 0) return {};
    const char* env = getenv("R2_CARD_A");
    if (env && *env) return env;
#ifndef _WIN32
    // Linux (an AppImage starts in any folder): saves/ of the working folder if it has the card,
    // else the user data folder's (~/.local/share/Red Two/saves/)
    if (FILE* f = fopen("saves/GSWE64_A.raw", "rb")) {
        fclose(f);
        return "saves/GSWE64_A.raw";
    }
    if (const std::string u = mods::userDir(); !u.empty()) return u + "saves/GSWE64_A.raw";
#else
    // Windows: the portable user folder's (else saves\ of the working folder, as before)
    if (mods::portable()) return mods::userDir() + "saves\\GSWE64_A.raw";
#endif
    return "saves/GSWE64_A.raw";
}

bool cardCurrentImage(int slot, std::vector<u8>& out) {
    MemCard& m = s_card[slot & 1];
    if (!m.present) return false;
    cardFlush(m);
    out = m.data;
    return true;
}

// The importer wrote a new image to the file: the running game keeps the card it mounted
// (its save is already in RAM and would be written back over the import), so from now on the
// card lives in memory only. The player restarts to play with the imported save.
void cardDetach(int slot) {
    MemCard& m = s_card[slot & 1];
    m.detached = true;
    if (m.file) {
        fclose(m.file);
        m.file = nullptr;
    }
}

void cardFlushNow() { cardFlushAll(); }

namespace {

// A formatted card's header serial is the console's flash ID scrambled by a generator seeded
// with the format time (CARDFormat; checked by VerifyID). Recover the flash ID from it so a
// card formatted elsewhere (another console, an emulator) mounts here.
void cardSyncSram(int slot) {
    const MemCard& m = s_card[slot];
    if (!m.present || m.data.size() < 0x200) return;
    const u8* h = m.data.data();
    const u16 deviceId = u16(h[0x20] << 8 | h[0x21]);
    if (deviceId != 0) return;  // not formatted
    u64 t = 0;
    for (int i = 0; i < 8; i++) t = (t << 8) | h[12 + i];
    s64 rnd = s64(t);
    u8 flash[12];
    for (int i = 0; i < 12; i++) {
        rnd = s64(u64(rnd) * 1103515245u + 12345u) >> 16;
        flash[i] = u8(h[i] - u8(rnd));
        rnd = (s64(u64(rnd) * 1103515245u + 12345u) >> 16) & 0x7FFF;
    }
    sramSetFlashId(slot, flash);
}

void cardRaiseIrq(int ch) {
    if (!s_card[ch].irqEnabled) return;
    exi[ch].csr |= 0x002;  // EXIINT: the erase/program finished
    exiUpdateIrq();
}

void cardSelect(int ch) {
    MemCard& m = s_card[ch];
    m.pos = 0;
    m.cmd = 0;
}

// Chip select released: erase and program commands take effect now.
void cardDeselect(int ch) {
    MemCard& m = s_card[ch];
    if (!m.present || m.pos == 0) return;
    static const bool log = envFlag("R2_CARD_LOG");
    if (log)
        logf("card", "cmd %02X args %02X %02X %02X %02X addr %06X: %u bytes, status %02X", m.cmd, m.args[1], m.args[2], m.args[3],
             m.args[4], cardAddr(m.args), m.pos, m.status);
    const u32 size = u32(m.data.size());
    switch (m.cmd) {
    case 0xF1: {  // erase sector
        const u32 a = ((u32(m.args[1] & 0x7F) << 17) | (u32(m.args[2]) << 9)) & ~0x1FFFu & (size - 1);
        memset(&m.data[a], 0xFF, 0x2000);
        cardMarkDirty(m, a, a + 0x2000);
        cardRaiseIrq(ch);
        break;
    }
    case 0xF2:  // page program (bytes were stored as they arrived)
        cardRaiseIrq(ch);
        break;
    case 0xF4:  // erase card
        std::fill(m.data.begin(), m.data.end(), u8(0xFF));
        cardMarkDirty(m, 0, size);
        cardRaiseIrq(ch);
        break;
    default: break;
    }
    m.pos = 0;
}

u8 cardByte(int ch, u8 in) {
    MemCard& m = s_card[ch];
    const u32 p = m.pos++;
    const u32 size = u32(m.data.size());
    if (p == 0) {
        m.cmd = in;
        if (in == 0x89) m.status &= ~0x18u;  // clear status: program/erase error bits
        static bool seen[256];
        if (!seen[in] && in != 0x00 && in != 0x52 && in != 0x83 && in != 0x89 && in != 0x81 && in != 0xF1 &&
            in != 0xF2 && in != 0xF4) {
            seen[in] = true;
            logf("card", "unhandled command %02X", in);
        }
        return 0xFF;
    }
    switch (m.cmd) {
    case 0x00:  // ID (EXIGetID: 2 command bytes, then the 32-bit id)
        return p >= 2 && p < 6 ? u8(m.id >> (8 * (5 - p))) : 0;
    case 0x83:  // read status
        return p >= 2 ? m.status : 0;
    case 0x81:  // interrupt enable
        if (p == 1) m.irqEnabled = in & 1;
        return 0;
    case 0x52:  // read array: address, latency bytes, data
        if (p <= 4) {
            m.args[p] = in;
            if (p == 4) m.addr = cardAddr(m.args);
            return 0;
        }
        if (p < 5 + kCardLatency) return 0;
        return m.data[(m.addr++) & (size - 1)];
    case 0xF2:  // page program: address, then up to 128 bytes within the page
        if (p <= 4) {
            m.args[p] = in;
            if (p == 4) m.addr = cardAddr(m.args);
            return 0;
        } else {
            const u32 a = m.addr & (size - 1);
            m.data[a] = in;
            cardMarkDirty(m, a, a + 1);
            m.addr = (m.addr & ~0x7Fu) | ((m.addr + 1) & 0x7F);
            return 0;
        }
    case 0xF1:  // erase sector: 2 address bytes
    case 0xF4:  // erase card
        if (p < 5) m.args[p] = in;
        return 0;
    default:
        return 0;
    }
}

int exiDevice(int ch) {
    const u32 cs = (exi[ch].csr >> 7) & 7;
    if (!cs) return -1;
    if (ch == 0 && (cs & 2)) return 1;              // IPL/RTC/SRAM
    if (ch < 2 && (cs & 1) && s_card[ch].present) return 2;  // memory card
    return 0;                                       // absent
}

void exiTransfer(int ch) {
    ExiChan& c = exi[ch];
    const int dev = exiDevice(ch);
    const bool dma = c.cr & 2;
    const u32 rw = (c.cr >> 2) & 3;  // 0 read, 1 write, 2 read/write
    if (dev == 1) {
        if (dma) {
            u8* p = physPtr(c.mar, c.len);
            if (p) {
                for (u32 i = 0; i < c.len; i++) {
                    if (rw == 0) p[i] = iplReadByte();
                    else iplWriteByte(p[i]);
                }
            }
        } else {
            const u32 n = ((c.cr >> 4) & 3) + 1;
            if (!s_ipl.haveCmd && rw == 1) {
                s_ipl.cmd = c.data;
                s_ipl.haveCmd = true;
                s_ipl.pos = 0;
            } else if (rw == 1) {
                for (u32 i = 0; i < n; i++) iplWriteByte(u8(c.data >> (24 - 8 * i)));
            } else {
                u32 v = 0;
                for (u32 i = 0; i < n; i++) v |= u32(iplReadByte()) << (24 - 8 * i);
                c.data = v;
            }
        }
    } else if (dev == 2) {  // memory card: one byte stream per chip select
        if (dma) {
            if (u8* p = physPtr(c.mar, c.len)) {
                for (u32 i = 0; i < c.len; i++) {
                    if (rw == 0) p[i] = cardByte(ch, 0);
                    else cardByte(ch, p[i]);
                }
            }
        } else {
            const u32 n = ((c.cr >> 4) & 3) + 1;
            u32 v = 0;
            for (u32 i = 0; i < n; i++) {
                const u8 out = cardByte(ch, rw == 0 ? 0 : u8(c.data >> (24 - 8 * i)));
                v |= u32(out) << (24 - 8 * i);
            }
            if (rw != 1) c.data = v;
        }
    } else if (!dma && rw != 1) {
        c.data = dev < 0 ? 0 : 0xFFFFFFFF;
    }
    c.cr &= ~1u;
    c.csr |= 0x008;  // TCINT
    exiUpdateIrq();
}

u32 exiRead(u32 off) {
    const int ch = int(off / 0x14);
    if (ch > 2) return 0;
    ExiChan& c = exi[ch];
    switch (off % 0x14) {
    case 0x00: {
        u32 v = c.csr;
        v &= ~0x1000u;
        if (ch < 2 && s_card[ch].present) v |= 0x1000u;  // EXT: a memory card is inserted
        return v;
    }
    case 0x04: return c.mar;
    case 0x08: return c.len;
    case 0x0C: return c.cr;
    default: return c.data;
    }
}
void exiWrite(u32 off, u32 v) {
    const int ch = int(off / 0x14);
    if (ch > 2) return;
    ExiChan& c = exi[ch];
    switch (off % 0x14) {
    case 0x00: {
        const u32 oldCs = (c.csr >> 7) & 7;
        const u32 ack = v & (0x002 | 0x008 | 0x800);
        c.csr = (c.csr & (0x002 | 0x008 | 0x800)) | (v & ~(0x002u | 0x008u | 0x800u));
        c.csr &= ~ack;
        const u32 newCs = (c.csr >> 7) & 7;
        if (ch == 0 && (newCs & 2) && !(oldCs & 2)) s_ipl.haveCmd = false;  // IPL selected: new command
        if (ch < 2 && (newCs & 1) && !(oldCs & 1)) cardSelect(ch);
        if (ch < 2 && (oldCs & 1) && !(newCs & 1)) cardDeselect(ch);
        exiUpdateIrq();
        break;
    }
    case 0x04: c.mar = v & 0x03FFFFE0; break;
    case 0x08: c.len = v & 0x03FFFFE0; break;
    case 0x0C:
        c.cr = v;
        if (v & 1) exiTransfer(ch);
        break;
    default: c.data = v; break;
    }
}

// =======================================================================================
// AI + DSP + ARAM
// =======================================================================================
u32 ai[4] = {};
u16 dsp[0x20] = {};  // 16-bit registers 0x00..0x3E
std::vector<u8> s_aram(16 * 1024 * 1024);
u32 s_mailToDsp = 0;
bool s_mailToDspFull = false;
std::vector<u32> s_mailsFromDsp;  // queued DSP->CPU mails
u32 s_mailOut = 0;
bool s_mailOutValid = false;
enum class DspState { Reset, Rom, BootParams, Ucode } s_dspState = DspState::Reset;
u32 s_bootParams[16] = {};
int s_bootParamIdx = -1;
struct {
    bool on = false;
    u32 addr = 0, blocks = 0;
} s_aiDma;

constexpr u16 CSR_RES = 1 << 0, CSR_PIINT = 1 << 1, CSR_HALT = 1 << 2, CSR_AIDINT = 1 << 3, CSR_AIDMSK = 1 << 4,
              CSR_ARINT = 1 << 5, CSR_ARMSK = 1 << 6, CSR_DSPINT = 1 << 7, CSR_DSPMSK = 1 << 8, CSR_DMABUSY = 1 << 9,
              CSR_RESETINIT = 1 << 11;
u16& dspCsr() { return dsp[0x0A / 2]; }

void dspUpdateIrq() {
    const u16 c = dspCsr();
    const bool on = ((c & CSR_AIDINT) && (c & CSR_AIDMSK)) || ((c & CSR_ARINT) && (c & CSR_ARMSK)) ||
                    ((c & CSR_DSPINT) && (c & CSR_DSPMSK));
    piSetIrq(PI_DSP, on);
}

void dspPostMail(u32 m, bool irq) {
    if (!s_mailOutValid) {
        s_mailOut = m;
        s_mailOutValid = true;
    } else {
        s_mailsFromDsp.push_back(m);
    }
    if (irq) {
        dspCsr() |= CSR_DSPINT;
        dspUpdateIrq();
    }
}

void dspMailFromCpu(u32 m) {
    if (static const bool dspLog = envFlag("R2_DSP_LOG"); dspLog) logf("dsp", "CPU->DSP mail %08X (state %d)", m, int(s_dspState));
    switch (s_dspState) {
    case DspState::Rom:
    case DspState::BootParams:
        // ROM bootloader: pairs of (0x80F3xxxx command, value)
        if (s_bootParamIdx < 0) {
            if ((m >> 16) == 0x80F3) {
                s_bootParamIdx = int(m & 0xF) + 4 * int((m >> 12) & 3);
                s_bootParams[15] = m;
            }
        } else {
            const u32 cmd = s_bootParams[15];
            s_bootParamIdx = -1;
            if (cmd == 0x80F3A001) s_bootParams[0] = m;       // IRAM mmem address
            else if (cmd == 0x80F3C002) s_bootParams[1] = m;  // IRAM DSP address
            else if (cmd == 0x80F3A002) s_bootParams[2] = m;  // IRAM length
            else if (cmd == 0x80F3B002) s_bootParams[3] = m;  // DRAM length
            else if (cmd == 0x80F3D001) {                     // start vector: ucode runs
                s_bootParams[4] = m;
                s_dspState = DspState::Ucode;
                logf("dsp", "ucode boot: iram mmem=%08X len=%X dsp=%04X, start %04X", s_bootParams[0], s_bootParams[2],
                     s_bootParams[1], m);
                dspPostMail(0xDCD10000, true);  // task framework: DSP_INIT
            }
        }
        break;
    case DspState::Ucode: {  // the MusyX ucode (musyx_dsp.cpp)
        u32 reply = 0;
        if (musyxDspMail(m, reply)) dspPostMail(reply, true);
        break;
    }
    default:
        break;
    }
}

void dspReset() {
    s_dspState = DspState::Reset;
    s_mailsFromDsp.clear();
    s_mailOutValid = false;
    s_mailToDspFull = false;
    s_bootParamIdx = -1;
}

u64 s_aramDoneTb = 0;  // when the ARAM DMA in progress completes

void onAram() {
    s_aramDoneTb = 0;
    dspCsr() &= ~CSR_DMABUSY;
    dspCsr() |= CSR_ARINT;
    dspUpdateIrq();
}

void aramDma() {
    const u32 mm = ((u32(dsp[0x20 / 2]) << 16) | dsp[0x22 / 2]) & 0x03FFFFE0;
    const u32 ar = ((u32(dsp[0x24 / 2]) << 16) | dsp[0x26 / 2]) & 0x03FFFFE0;
    const u32 cntH = dsp[0x28 / 2];
    const u32 cnt = ((cntH & 0x7FFF) << 16 | dsp[0x2A / 2]) & 0x03FFFFE0;
    const bool toMram = cntH & 0x8000;
    u8* m = physPtr(mm, cnt);
    if (!m) fatal("ARAM DMA with bad main memory address %08X len %X", mm, cnt);
    // Past the 16 MiB of internal ARAM is the expansion port, empty on a GameCube: reads give
    // zeros, writes go nowhere (Dolphin's HSP device "none"). The SDK's ARAM size check at boot
    // (__ARChecksize, 0x800402C8) writes test data at 0x1000000 and 0x1400000 and reads it back:
    // when those wrapped to 0 / 0x400000 here it took the ARAM for 8 MiB (__AR_Size 0x80096824),
    // MusyX put its stream buffers at 8 MiB on top of sound effects the game had loaded there,
    // and those effects played streamed music and crackled (issue #57: the AT-ATs' steps).
    // The check also sets the memory map (AR_INFO 0xCC005012, low bits) to 4 for a while, in which
    // a write below 4 MiB lands 4 MiB higher too; Dolphin's DSP.cpp Do_ARAM_DMA does the same.
    constexpr u32 kAramSize = 16 * 1024 * 1024;
    const bool map4 = (dsp[0x12 / 2] & 0xF) == 4;
    for (u32 i = 0; i < cnt; i++) {
        const u32 x = ar + i;
        if (x >= kAramSize) {
            if (toMram) m[i] = 0;
            continue;
        }
        u8& a = s_aram[x];
        if (toMram) {
            m[i] = a;
        } else {
            a = m[i];
            if (map4 && x < 0x400000) s_aram[x + 0x400000] = m[i];
        }
    }
    if (toMram) watchHost(mm, cnt, "ARAM DMA");
    if (static const bool aramLog = envFlag("R2_ARAM_LOG"); aramLog) logf("aram", "%s mram %08X aram %08X len %X", toMram ? "A->M" : "M->A", mm, ar, cnt);
    // The game's pager busy-waits on this flag for every 4 KiB page it brings in (the intro
    // pages a lot): model a fast ARAM (2 GiB/s, ~3 us a page) and let a status read complete a
    // due transfer at once (dspRead) instead of after the timer thread's wake-up and a safepoint.
    dspCsr() |= CSR_DMABUSY;
    s_aramDoneTb = tbNow() + 50 + u64(cnt) * kTbClock / (2048ull * 1024 * 1024);
    schedule(EV_ARAM, s_aramDoneTb);
}

u64 s_aiDue = 0;           // when the AI DMA block playing now ends

// MusyX's AI interrupt state (hw_dolphin.c; small data, r13 = 0x8009ADA0), identified in
// salCallback 0x800269C0 (AIInitDMA, OSGetTick -> salLastTick, then these flags):
// salLogicIsWaiting r13-18372 (the interrupt came before the DSP finished: the mix waits for
// the DSP's resume) and salLogicActive r13-18368 (its callback is mixing the block two ahead
// and running the synth for the next frame, interrupts enabled). An AI interrupt taken while
// either is set loses that frame's mix.
// salAIBufferBase r13-18380 (four 0x280-byte blocks) precedes them.
constexpr u32 kSalAIBufferBase = 0x000965D4;  // salLogicIsWaiting +8, salLogicActive +12
u32 rdPhys32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
// the DMA plays MusyX's buffers (else nothing waits: boot, other AI users)
bool musyxPlaying() {
    const u8* p = physPtr(kSalAIBufferBase, 4);
    const u32 base = p ? rdPhys32(p) & 0x03FFFFE0 : 0;
    return base && s_aiDma.addr >= base && s_aiDma.addr < base + 4 * 0x280;
}
bool aiCatchUp() {
    static const bool on = [] {
        const char* v = getenv("R2_AI_CATCHUP");
        return !v || atoi(v) != 0;
    }();
    return on;
}
bool musyxFrameBusy() {
    const u8* p = physPtr(kSalAIBufferBase + 8, 8);
    return p && (rdPhys32(p) == 1 || rdPhys32(p + 4) == 1);
}

// For the perf lines (issue #22: sound running short in cutscenes on some PCs, the logs could not
// say where): blocks the audio clock skipped when it fell too far behind (the guest thread held
// up), and how late block boundaries came (waiting for the game or for MusyX).
std::atomic<u64> s_aiLostBlocks{0}, s_aiLateSumUs{0}, s_aiLateMaxUs{0}, s_aiBoundaries{0};

void onAi() {
    if (!s_aiDma.on) return;
    // The game hands the DMA its next buffer from the AI interrupt, and MusyX mixes a block two
    // ahead from the same handler. We take interrupts at the guest thread's safepoints: after a
    // boundary a few ms late (the guest held up on the host) the catch-up boundaries came back
    // to back. Before the handler ran, the block just played was latched again (5 ms repeated);
    // while MusyX's callback still ran, its next interrupt skipped a mix (a stale block two
    // later): clicks either way. Hardware's boundaries are 5 ms apart and the game always keeps
    // up, so a boundary now waits until the handler gave the DMA its next buffer (AIInitDMA:
    // the address registers moved on; the AIDINT acknowledgement comes before that) and MusyX
    // is done with that frame, looking again every 0.05 ms (0.5 ms before catch-up), at most
    // kAiWaitMax: the catch-up runs as fast as the game handles it and the host's audio cushion
    // covers the delay. Not while a cinematic is skipped: silent, and the audio clock must keep
    // the game's pace. Catch-up (ZeDctr's Red Two Plus fork; R2_AI_CATCHUP=0 for the old way):
    // the guest thread's safepoints also start due blocks at once (threads.cpp, aiCatchUpStep)
    // instead of waiting for the timer thread, a block waits for MusyX up to 100 ms (was 20) and
    // the clock keeps counting from when blocks were due through stalls of up to 16 blocks
    // (80 ms; was 4): choppy music with V-Sync waits (a frame's 16.7 ms held up the timer's
    // boundaries) and short host stalls lost blocks instead of catching up.
    const u64 now = tbNow();
    const u64 kAiWaitMax = aiCatchUp() ? kTbClock / 10 : kTbClock / 50;
    if (s_audioSpeed == 1.0 && (dspCsr() & CSR_AIDMSK) && s_aiDue && now - s_aiDue < kAiWaitMax &&
        musyxPlaying()) {
        const u32 next = ((u32(dsp[0x30 / 2]) << 16) | dsp[0x32 / 2]) & 0x03FFFFE0;
        if ((dspCsr() & CSR_AIDINT) || next == s_aiDma.addr || musyxFrameBusy()) {
            schedule(EV_AI, now + (aiCatchUp() ? kTbClock / 20000 : kTbClock / 2000));
            return;
        }
    }
    static const bool aiLog = envFlag("R2_AI_LOG");  // R2_AI_LOG=1: late or repeated blocks
    if (aiLog) {
        const u32 next = ((u32(dsp[0x30 / 2]) << 16) | dsp[0x32 / 2]) & 0x03FFFFE0;
        const bool pending = dspCsr() & CSR_AIDINT;
        const double late = s_aiDue ? double(now - s_aiDue) * 1000.0 / kTbClock : 0.0;
        if ((pending || next == s_aiDma.addr || late > 2.0) && s_audioSpeed == 1.0)
            logf("ai", "boundary %.2f ms late: interrupt %s, next %08X%s", late, pending ? "NOT handled" : "handled",
                 next, next == s_aiDma.addr ? " = the block just played" : "");
    }
    if (s_aiDue && s_audioSpeed == 1.0) {
        const u64 lateUs = now > s_aiDue ? (now - s_aiDue) * 1000000 / kTbClock : 0;
        s_aiLateSumUs += lateUs;
        if (lateUs > s_aiLateMaxUs.load(std::memory_order_relaxed)) s_aiLateMaxUs = lateUs;
        s_aiBoundaries++;
    }
    // the block just finished: latch the next one from the registers and interrupt
    s_aiDma.addr = ((u32(dsp[0x30 / 2]) << 16) | dsp[0x32 / 2]) & 0x03FFFFE0;
    s_aiDma.blocks = dsp[0x36 / 2] & 0x7FFF;
    aiTraceEvent('B', s_aiDma.addr);
    dspCsr() |= CSR_AIDINT;
    dspUpdateIrq();
    const u64 samples = u64(s_aiDma.blocks ? s_aiDma.blocks : 1) * 32 / 4;
    const u32 rate = (ai[0] & 0x40) ? 32000 : 48000;
    // the latched block plays now: hand it to the host
    if (!s_audioMute)
        if (const u8* p = physPtr(s_aiDma.addr, u32(samples) * 4)) audio::push(p, u32(samples), rate);
    // Next block from when this one was due, not from now: a late interrupt (the guest was
    // busy) must not slow the audio clock down. Resynchronise only after a long stall.
    // Skipping shortens the blocks: MusyX (synthesis, sequencer, streams) keeps pace.
    u64& due = s_aiDue;
    const u64 period = u64(double(samples) * double(kTbClock) / (double(rate) * s_audioSpeed));
    const bool keep = due && now - due < (aiCatchUp() ? 16 : 4) * period;
    if (due && !keep && s_audioSpeed == 1.0 && period) s_aiLostBlocks += (now - due) / period;
    due = keep ? due + period : now + period;
    schedule(EV_AI, due);
}

u16 dspRead(u32 off) {
    if (off == 0x0A && s_aramDoneTb && tbNow() >= s_aramDoneTb) {  // CSR: a polled ARAM DMA is done
        unschedule(EV_ARAM);
        onAram();
    }
    switch (off) {
    // bit 15 of the high half is the mailbox's "full" flag, not part of the mail
    case 0x00: return u16(((s_mailToDsp >> 16) & 0x7FFF) | (s_mailToDspFull ? 0x8000 : 0));
    case 0x02: return u16(s_mailToDsp);
    case 0x04: return u16(((s_mailOut >> 16) & 0x7FFF) | (s_mailOutValid ? 0x8000 : 0));
    case 0x06: {
        const u16 v = u16(s_mailOut);
        if (s_mailOutValid) {
            s_mailOutValid = false;
            if (!s_mailsFromDsp.empty()) {
                s_mailOut = s_mailsFromDsp.front();
                s_mailsFromDsp.erase(s_mailsFromDsp.begin());
                s_mailOutValid = true;
            }
        }
        return v;
    }
    case 0x16: return u16(dsp[0x16 / 2] | 1);  // ARAM mode: initialized
    case 0x3A: return u16(s_aiDma.on ? s_aiDma.blocks : 0);
    default: return dsp[(off & 0x3F) / 2];
    }
}

void dspWrite(u32 off, u16 v) {
    switch (off) {
    case 0x00:
        s_mailToDsp = (s_mailToDsp & 0xFFFF) | (u32(v & 0x7FFF) << 16) | (u32(v & 0x8000) << 16);
        return;
    case 0x02:
        s_mailToDsp = (s_mailToDsp & 0xFFFF0000u) | v;
        s_mailToDspFull = false;  // the DSP takes it at once
        dspMailFromCpu(s_mailToDsp);
        return;
    case 0x0A: {
        u16& c = dspCsr();
        const u16 old = c;
        const u16 ack = v & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT);
        c = u16((c & (CSR_AIDINT | CSR_ARINT | CSR_DSPINT | CSR_DMABUSY)) |
                (v & ~(CSR_AIDINT | CSR_ARINT | CSR_DSPINT | CSR_DMABUSY | CSR_RES)));
        c &= u16(~ack);
        if (ack & CSR_AIDINT) aiTraceEvent('A', 0);
        if (ack & CSR_DSPINT) aiTraceEvent('D', 0);
        if (v & CSR_RES) dspReset();
        // released from halt after a reset: the ROM boots and says hello
        if ((old & CSR_HALT) && !(c & CSR_HALT) && s_dspState == DspState::Reset) {
            s_dspState = DspState::Rom;
            dspPostMail(0x8071FEED, false);
        }
        if (v & CSR_PIINT) {
            if (static const bool dspLog = envFlag("R2_DSP_LOG"); dspLog) logf("dsp", "CPU interrupts DSP");
        }
        dspUpdateIrq();
        return;
    }
    case 0x2A:
        dsp[0x2A / 2] = v;
        aramDma();
        return;
    case 0x36: {
        const bool was = s_aiDma.on;
        dsp[0x36 / 2] = v;
        s_aiDma.on = v & 0x8000;
        if (s_aiDma.on && !was) {
            s_aiDma.addr = ((u32(dsp[0x30 / 2]) << 16) | dsp[0x32 / 2]) & 0x03FFFFE0;
            s_aiDma.blocks = v & 0x7FFF;
            onAi();
        } else if (!s_aiDma.on) {
            unschedule(EV_AI);
        }
        return;
    }
    default:
        dsp[(off & 0x3F) / 2] = v;
        return;
    }
}

// Streaming sample counter (AISCNT) runs while PSTAT is set; the SDK measures it at init.
u64 s_aiCountBaseTb = 0;
u32 s_aiCountBase = 0;
u32 aiRate() { return (ai[0] & 0x02) ? 48000 : 32000; }
u32 aiCounter() {
    if (!(ai[0] & 1)) return s_aiCountBase;
    return s_aiCountBase + u32((tbNow() - s_aiCountBaseTb) * aiRate() / kTbClock);
}
void aiScheduleIrq() {
    if (!(ai[0] & 1) || !(ai[0] & 0x10)) return unschedule(EV_EXI2);  // AIINTVLD
    const u32 now = aiCounter(), target = ai[3];
    const u32 delta = target > now ? target - now : 1;
    schedule(EV_EXI2, tbNow() + u64(delta) * kTbClock / aiRate());
}
void onAiCount() {
    ai[0] |= 0x08;  // AIINT
    piSetIrq(PI_AI, ai[0] & 0x04);
}

u32 aiRead(u32 off) {
    if (off == 0x08) return aiCounter();
    return ai[(off & 0xF) / 4];
}
void aiWrite(u32 off, u32 v) {
    const u32 r = (off & 0xF) / 4;
    if (r == 0) {
        const u32 cur = aiCounter();
        const u32 ack = v & 0x08;  // AIINT
        ai[0] = (ai[0] & 0x08) | (v & ~0x08u);
        ai[0] &= ~ack;
        s_aiCountBase = (v & 0x20) ? 0 : cur;  // SCRESET
        s_aiCountBaseTb = tbNow();
        ai[0] &= ~0x20u;
        piSetIrq(PI_AI, (ai[0] & 0x08) && (ai[0] & 0x04));
        aiScheduleIrq();
        return;
    }
    ai[r] = v;
    if (r == 3) aiScheduleIrq();
}

// =======================================================================================
// CP / PE / MI
// =======================================================================================
u16 pe[0x10] = {};
u16 mi[0x80] = {};

u16 cpRead(u32 off) { return gxCpRead(off); }
void cpWrite(u32 off, u16 v) { gxCpWrite(off, v); }

u16 peRead(u32 off) { return pe[(off & 0x1F) / 2]; }
void peWrite(u32 off, u16 v) {
    if (static const bool gxLog = envFlag("R2_GX_LOG"); off == 0x0A && gxLog) logf("gx", "PE_CTRL write %04X", v);
    if (off == 0x0A) {
        u16& c = pe[0x0A / 2];
        const u16 ack = v & 0x0C;
        c = u16((c & 0x0C) | (v & 0x03));
        c &= u16(~ack);
        piSetIrq(PI_PE_TOKEN, (c & 0x04) && (c & 0x01));
        piSetIrq(PI_PE_FINISH, (c & 0x08) && (c & 0x02));
        return;
    }
    pe[(off & 0x1F) / 2] = v;
}

}  // namespace

// the perf lines: how late the audio clock's block boundaries came, and blocks it skipped
std::string aiClockText() {
    const u64 n = s_aiBoundaries.exchange(0), sum = s_aiLateSumUs.exchange(0), mx = s_aiLateMaxUs.exchange(0),
              lost = s_aiLostBlocks.exchange(0);
    char t[128];
    snprintf(t, sizeof t, "blocks late %.1f ms on average (at most %.0f), %llu skipped", n ? sum / 1000.0 / double(n) : 0.0,
             mx / 1000.0, (unsigned long long)lost);
    return t;
}

// threads.cpp safepoint: a block due now (catch-up, see onAi) starts at once when the game's
// handler and MusyX are ready for it; else the timer's EV_AI keeps waiting as before
bool aiCatchUpPending() {
    return aiCatchUp() && s_aiDma.on && s_audioSpeed == 1.0 && (dspCsr() & CSR_AIDMSK) && s_aiDue && tbNow() >= s_aiDue &&
           musyxPlaying();
}
bool aiCatchUpStep() {
    if (!aiCatchUpPending()) return false;
    const u32 next = ((u32(dsp[0x30 / 2]) << 16) | dsp[0x32 / 2]) & 0x03FFFFE0;
    if ((dspCsr() & CSR_AIDINT) || next == s_aiDma.addr || musyxFrameBusy()) return false;
    unschedule(EV_AI);
    onAi();
    return true;
}

bool viIsFramebuffer(u32 addr) { return std::find(s_viFbs.begin(), s_viFbs.end(), addr) != s_viFbs.end(); }

u8* aramBase() { return s_aram.data(); }

// Lines in the frame buffer the VI scans out: active lines per field (VTR ACV), times two
// when interlaced (DCR NIN clear).
u32 viXfbLines() {
    const u32 acv = (vi[0x00 / 2] >> 4) & 0x3FF;
    const bool nonInterlaced = vi[0x02 / 2] & 4;
    return nonInterlaced ? acv : acv * 2;
}

bool viLocateCopy(u32 dest, u32 strideBytes, u32& line, u32& lines) {
    lines = viXfbLines();
    if (!strideBytes || !lines) return false;
    for (u32 base : s_viTop) {
        if (!base || dest < base) continue;
        const u32 off = dest - base;
        if (off % strideBytes == 0 && off / strideBytes < lines) {
            line = off / strideBytes;
            return true;
        }
    }
    return false;
}

// --- events raised by the GX FIFO parser -----------------------------------------------
void peToken(u16 token, bool irq) {
    pe[0x0E / 2] = token;
    if (irq) {
        pe[0x0A / 2] |= 0x04;
        piSetIrq(PI_PE_TOKEN, pe[0x0A / 2] & 0x01);
    }
}
void peFinish() {
    pe[0x0A / 2] |= 0x08;
    piSetIrq(PI_PE_FINISH, pe[0x0A / 2] & 0x02);
}

// The GP's state for stall reports (profiler.cpp): a game thread waiting for its frames to be
// drawn (frames in flight, 0x7FE0C670: one per submitted frame until its draw done's interrupt)
std::string gpuStateText() {
    char b[160];
    snprintf(b, sizeof b, "; PE_CTRL %04X, PI cause %05X mask %05X, frames in flight %u", pe[0x0A / 2], pi_intsr, pi_intmr,
             peekGuest32(0x7FE0C670));
    return gxFifoText() + b;
}

// --- PI helpers (runtime.h) --------------------------------------------------------------
void piSetIrq(u32 bits, bool on) {
    if (on) pi_intsr |= bits;
    else pi_intsr &= ~bits;
    piUpdate();
}
bool piPending() { return (pi_intsr & pi_intmr) != 0; }

PadState& padState(int port) { return s_pad[port & 3]; }

void hwInit() {
    s_logIo = envFlag("R2_IO_LOG");
    gxInit();
    sramInit();
    // Memory card in slot A: R2_CARD_A=<path> (default saves/GSWE64_A.raw), R2_NO_CARD=1 for none.
    if (!envFlag("R2_NO_CARD")) {
        const std::string path = cardImagePath(0);
        std::error_code ec;
        const auto dir = std::filesystem::path(path).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
        cardInit(0, path);
        cardSyncSram(0);
    }
    di[DICVR] = 0;  // cover closed
    for (auto& p : s_pad) p = PadState{};
    s_pad[0].connected = true;
    g_eventHandlers[EV_VI] = onVi;
    g_eventHandlers[EV_DI] = onDi;
    g_eventHandlers[EV_AI] = onAi;
    g_eventHandlers[EV_ARAM] = onAram;
    g_eventHandlers[EV_SI] = siPollAll;
    g_eventHandlers[EV_EXI2] = onAiCount;  // (slot reused for the AI sample counter interrupt)
    s_viFrameStartTb = tbNow();
    s_viDueTb = s_viFrameStartTb + kViPeriod;
    schedule(EV_VI, s_viDueTb);
}

void siVBlankPoll() { siPollAll(); }

// --- MMIO router ------------------------------------------------------------------------
static u32 read16dev(u16 (*rd)(u32), u32 off, int size) {
    if (size == 4) return (u32(rd(off)) << 16) | rd(off + 2);
    const u16 v = rd(off & ~1u);
    return size == 2 ? v : ((off & 1) ? v & 0xFF : v >> 8);
}
static void write16dev(void (*wr)(u32, u16), u16 (*rd)(u32), u32 off, u32 v, int size) {
    if (size == 4) {
        wr(off, u16(v >> 16));
        wr(off + 2, u16(v));
    } else if (size == 2) {
        wr(off, u16(v));
    } else {
        u16 cur = rd(off & ~1u);
        cur = (off & 1) ? u16((cur & 0xFF00) | (v & 0xFF)) : u16((cur & 0x00FF) | ((v & 0xFF) << 8));
        wr(off & ~1u, cur);
    }
}
static u32 read32dev(u32 (*rd)(u32), u32 off, int size) {
    const u32 v = rd(off & ~3u);
    if (size == 4) return v;
    if (size == 2) return (off & 2) ? v & 0xFFFF : v >> 16;
    return (v >> (24 - 8 * (off & 3))) & 0xFF;
}
static void write32dev(void (*wr)(u32, u32), u32 (*rd)(u32), u32 off, u32 v, int size) {
    if (size == 4) return wr(off, v);
    u32 cur = rd(off & ~3u);
    if (size == 2) cur = (off & 2) ? (cur & 0xFFFF0000u) | (v & 0xFFFF) : (cur & 0xFFFF) | (v << 16);
    else {
        const u32 sh = 24 - 8 * (off & 3);
        cur = (cur & ~(0xFFu << sh)) | ((v & 0xFF) << sh);
    }
    wr(off & ~3u, cur);
}

static u16 miRead(u32 off) { return mi[(off & 0xFF) / 2]; }
static void miWrite(u32 off, u16 v) { mi[(off & 0xFF) / 2] = v; }

u32 ioRead(u32 a, int size) {
    if (a < 0xCC000000u) {
        noteAccess("EFB", a, false, 0);
        return 0;
    }
    const u32 off = a & 0xFFFF;
    u32 v = 0;
    const char* dev = "?";
    switch (off >> 12) {
    case 0x0: v = read16dev(cpRead, off & 0xFFF, size); dev = "CP"; break;
    case 0x1: v = read16dev(peRead, off & 0xFFF, size); dev = "PE"; break;
    case 0x2: v = read16dev(viRead, off & 0xFFF, size); dev = "VI"; break;
    case 0x3: v = read32dev(piRead, off & 0xFFF, size); dev = "PI"; break;
    case 0x4: v = read16dev(miRead, off & 0xFFF, size); dev = "MI"; break;
    case 0x5: v = read16dev(dspRead, off & 0xFFF, size); dev = "DSP"; break;
    case 0x6:
        if (off < 0x6400) { v = read32dev(diRead, off & 0x3FF, size); dev = "DI"; }
        else if (off < 0x6800) { v = read32dev(siRead, off & 0x3FF, size); dev = "SI"; }
        else if (off < 0x6C00) { v = read32dev(exiRead, off & 0x3FF, size); dev = "EXI"; }
        else { v = read32dev(aiRead, off & 0x3FF, size); dev = "AI"; }
        break;
    default: dev = "unmapped"; break;
    }
    noteAccess(dev, a, false, v);
    return v;
}

void ioWrite(u32 a, u32 v, int size) {
    if ((a & 0xFFFFFFF8u) == 0xCC008000u) [[likely]] {
        gxFifoWrite(v, size);
        return;
    }
    if (a < 0xCC000000u) {
        noteAccess("EFB", a, true, v);
        return;
    }
    const u32 off = a & 0xFFFF;
    const char* dev = "?";
    switch (off >> 12) {
    case 0x0: write16dev(cpWrite, cpRead, off & 0xFFF, v, size); dev = "CP"; break;
    case 0x1: write16dev(peWrite, peRead, off & 0xFFF, v, size); dev = "PE"; break;
    case 0x2: write16dev(viWrite, viRead, off & 0xFFF, v, size); dev = "VI"; break;
    case 0x3: write32dev(piWrite, piRead, off & 0xFFF, v, size); dev = "PI"; break;
    case 0x4: write16dev(miWrite, miRead, off & 0xFFF, v, size); dev = "MI"; break;
    case 0x5: write16dev(dspWrite, dspRead, off & 0xFFF, v, size); dev = "DSP"; break;
    case 0x6:
        if (off < 0x6400) { write32dev(diWrite, diRead, off & 0x3FF, v, size); dev = "DI"; }
        else if (off < 0x6800) { write32dev(siWrite, siRead, off & 0x3FF, v, size); dev = "SI"; }
        else if (off < 0x6C00) { write32dev(exiWrite, exiRead, off & 0x3FF, v, size); dev = "EXI"; }
        else { write32dev(aiWrite, aiRead, off & 0x3FF, v, size); dev = "AI"; }
        break;
    default: dev = "unmapped"; break;
    }
    noteAccess(dev, a, true, v);
}


}  // namespace rt

// --- save states ----------------------------------------------------------------------------
// Register files, transfers in flight and their timers (moved to now), ARAM. Not saved: the
// controllers (live input), the retrace rate (a setting) and the memory card's contents (its
// file keeps the game's saves; only the transfer state of the EXI channel is restored).
#include "../savestate.h"
namespace rt {
void hwSaveState(state::Writer& w) {
    w.section("HW  ");
    w.pod(pi_intsr);
    w.pod(pi_intmr);
    w.pod(vi);
    w.pod(s_viFrameStartTb);
    w.pod(s_viDueTb);
    w.pod(s_viFrames);
    w.vec(s_viFbs);
    w.pod(s_viTop);
    w.pod(di);
    w.pod(s_diPendingErr);
    w.pod(s_diOp);
    w.pod(si);
    w.pod(exi);
    w.pod(s_sram);
    w.pod(s_ipl);
    for (const MemCard& m : s_card) {
        w.pod(m.status);
        w.pod(m.irqEnabled);
        w.pod(m.pos);
        w.pod(m.cmd);
        w.pod(m.args);
        w.pod(m.addr);
    }
    w.pod(ai);
    w.pod(dsp);
    w.pod(s_mailToDsp);
    w.pod(s_mailToDspFull);
    w.vec(s_mailsFromDsp);
    w.pod(s_mailOut);
    w.pod(s_mailOutValid);
    w.pod(s_dspState);
    w.pod(s_bootParams);
    w.pod(s_bootParamIdx);
    w.pod(s_aiDma);
    w.pod(s_aramDoneTb);
    w.pod(s_aiCountBaseTb);
    w.pod(s_aiCountBase);
    w.pod(s_aiDue);
    w.pod(pe);
    w.pod(mi);
    w.vec(s_aram);
}

void hwLoadState(state::Reader& r) {
    r.section("HW  ");
    r.pod(pi_intsr);
    r.pod(pi_intmr);
    r.pod(vi);
    r.pod(s_viFrameStartTb);
    r.pod(s_viDueTb);
    r.pod(s_viFrames);
    r.vec(s_viFbs);
    r.pod(s_viTop);
    r.pod(di);
    r.pod(s_diPendingErr);
    r.pod(s_diOp);
    r.pod(si);
    r.pod(exi);
    r.pod(s_sram);
    r.pod(s_ipl);
    for (MemCard& m : s_card) {
        r.pod(m.status);
        r.pod(m.irqEnabled);
        r.pod(m.pos);
        r.pod(m.cmd);
        r.pod(m.args);
        r.pod(m.addr);
    }
    r.pod(ai);
    r.pod(dsp);
    r.pod(s_mailToDsp);
    r.pod(s_mailToDspFull);
    r.vec(s_mailsFromDsp);
    r.pod(s_mailOut);
    r.pod(s_mailOutValid);
    r.pod(s_dspState);
    r.pod(s_bootParams);
    r.pod(s_bootParamIdx);
    r.pod(s_aiDma);
    r.pod(s_aramDoneTb);
    r.pod(s_aiCountBaseTb);
    r.pod(s_aiCountBase);
    r.pod(s_aiDue);
    r.pod(pe);
    r.pod(mi);
    if (r.get<u64>() == s_aram.size()) r.bytes(s_aram.data(), s_aram.size());  // (aramBase() stays put)
    else r.ok = false;
    s_viFrameStartTb = r.time(s_viFrameStartTb);
    s_viDueTb = r.time(s_viDueTb);
    s_aramDoneTb = r.time(s_aramDoneTb);
    s_aiCountBaseTb = r.time(s_aiCountBaseTb);
    s_aiDue = r.time(s_aiDue);
    piUpdate();
}
}  // namespace rt
