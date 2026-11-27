// MusyX DSP emulated at the command-list level (HLE).
//
// Rogue Leader's DSP ucode is byte-identical to the one MusyX uses for versions up to 2.0.0
// (checked against the MusyX decomp's dsp_import.c). It is an AX derivative: every 5 ms the
// CPU builds a command list (salBuildCommandList) and mails 0xBABE0000|firstSize, then its
// address; the DSP runs the list and answers 0xDCD10001 (the task manager's "resume",
// MusyX's dspResumeCallback). The list mixes 160 samples (32 kHz) per studio through nine
// buses (main/aux A/aux B x L/R/S) from voice parameter blocks (_PB) in main memory, hands aux
// buffers to the CPU's effects, and writes the final stereo frame into the AI DMA buffer.
//
// Command and structure layouts follow the MusyX decomp (MIT, ../MetroidPrimePort/extern/
// musyx: hw_dspctrl.c, voice.h); the DSP-side behaviour is reimplemented here from the AX
// model: ARAM sample fetch (ADPCM/PCM8/PCM16 with loops), 4-tap polyphase resampling, a
// per-sample volume envelope and mix ramps, per-subframe parameter updates and depop values.
#include "musyx_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../runtime.h"

namespace rt {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kN = 160;   // samples per frame (5 ms at 32 kHz)
constexpr int kSub = 32;  // samples per subframe (5 subframes)

// PB word indices (voice.h, MusyX <= 2.0.0: 0xBC bytes)
enum : int {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_SELECT = 4, PB_COEF_SELECT = 5, PB_MIXER_CTRL = 6, PB_STATE = 7,
    PB_LOOP_TYPE = 8,
    PB_VL = 9, PB_DL = 10, PB_VR = 11, PB_DR = 12, PB_VAL = 13, PB_DAL = 14, PB_VAR = 15, PB_DAR = 16,
    PB_VBL = 17, PB_DBL = 18, PB_VBR = 19, PB_DBR = 20, PB_VBS = 21, PB_DBS = 22, PB_VS = 23, PB_DS = 24,
    PB_VAS = 25, PB_DAS = 26,
    PB_UPD_NUM = 34, PB_UPD_HI = 39, PB_UPD_LO = 40,
    PB_DPOP_L = 41, PB_DPOP_AL = 42, PB_DPOP_BL = 43, PB_DPOP_R = 44, PB_DPOP_AR = 45, PB_DPOP_BR = 46,
    PB_DPOP_S = 47, PB_DPOP_AS = 48, PB_DPOP_BS = 49,
    PB_VE_VOL = 50, PB_VE_DELTA = 51,
    PB_LOOP_FLAG = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60,
    PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_GAIN = 79, PB_PS = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92, PB_STREAM_LOOPS = 93,
    PB_WORDS = 94,
};

// buses: main L/R/S, aux A L/R/S, aux B L/R/S
enum { L = 0, R = 1, S = 2 };
s32 s_main[3][kN], s_auxA[3][kN], s_auxB[3][kN];
s32 s_out[3][kN];  // final mix of the master studios
s16 s_poly[128][4];
bool s_log = false;
u64 s_lists = 0, s_voices = 0;

// --- surround, kept apart for discrete 5.1 / 7.1 output (host only: the game sees its stereo) ---
// Rogue Leader's MusyX studio is a DPL2 one from the start (its STEREO sound setting is Pro Logic II;
// MONO has no surround), so (hw_volconv.c CalcBusDPL2):
// a voice's front volumes go to main L/R, its rear volumes to aux B L/R, and MIX_AUXB_LR encodes
// the rear into the front - rear left in phase into L, rear right inverted into R, one frame
// late - for the receiver's decoder. The mono surround bus S (Dolby Surround, and reverb returns)
// goes in with opposite signs (SET_OPPOSITE_LR, also a frame late). Both are recorded here on
// their way, so the output can take them back out of the front: FL = Lt + S - rear L,
// FR = Rt - S + rear R, and the rear and S get speakers of their own.
// For 7.1 each voice's rear share is split between side and back speakers by how far back the
// voice sits (its rear against its front volume: equal at the side, front 0 behind).
struct Rear {
    s32 rear[2][kN];  // the rear L/R as encoded into the front
    float side[2][kN], back[2][kN];  // the same split for 7.1 (equal power)
    void clear() { std::memset(this, 0, sizeof *this); }
    void add(const Rear& o, float g = 1.f) {
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < kN; i++) {
                rear[c][i] += s32(o.rear[c][i] * g);
                side[c][i] += o.side[c][i] * g;
                back[c][i] += o.back[c][i] * g;
            }
    }
};
float s_auxBSide[2][kN], s_auxBBack[2][kN];  // this studio's voices on aux B L/R, split (7.1)
Rear s_studioRear;                          // what this studio's MIX_AUXB_LR put into its main
bool s_studioHasRear = false;
// aux B L/R uploads (MIX_AUXB_LR): downloaded again a frame later, with the 7.1 split
struct RearUpload {
    u32 addr = 0;
    s32 data[2][kN];
    float side[2][kN], back[2][kN];
};
RearUpload s_rearUploads[8];
int s_rearUploadNext = 0;
// studios' main buffers this list uploaded with rear in them (UPLOAD_LRS), for the final mix
struct StudioRear {
    u32 addr = 0;
    u64 list = 0;
    Rear rear;
};
StudioRear s_studioRears[16];
Rear s_outRear;      // the final mix's encoded rear
s32 s_outS[kN];      // and its folded surround (SET_OPPOSITE_LR)
bool s_outHasSurround = false;
bool s_listDpl2 = false, s_dpl2 = false, s_loggedMode = false;
// output blocks (OUTPUT): the stereo written and its discrete channels, found again by the AI
// DMA's data when the block plays
struct DiscreteBlock {
    u32 addr = 0;
    u8 stereo[kN * 4];
    s16 ch[kN][8];
};
DiscreteBlock s_blocks[8];
int s_blockNext = 0;

u8* aram() { return aramBase(); }
constexpr u32 kAramMask = 0x00FFFFFF;

u8* mem(u32 a, u32 len) { return physPtr(a & 0x01FFFFFF, len); }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
s32 be32s(const u8* p) { return s32(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]); }
void put32(u8* p, s32 v) {
    p[0] = u8(u32(v) >> 24); p[1] = u8(u32(v) >> 16); p[2] = u8(u32(v) >> 8); p[3] = u8(v);
}
s16 clamp16(s64 v) { return s16(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

void initPolyphase() {
    // 4 taps around the interpolation point (between taps 1 and 2), 128 phases: a windowed
    // sinc normalised to unity gain. The DSP's own tables live in its coefficient ROM.
    for (int p = 0; p < 128; p++) {
        const double f = p / 128.0;
        double w[4], sum = 0;
        for (int k = 0; k < 4; k++) {
            const double x = (k - 1) - f;
            const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double win = std::fabs(x) < 2 ? std::cos(kPi * x / 4.0) : 0.0;
            w[k] = sinc * win;
            sum += w[k];
        }
        int acc = 0;
        for (int k = 0; k < 3; k++) {
            s_poly[p][k] = s16(std::lround(w[k] / sum * 32767.0));
            acc += s_poly[p][k];
        }
        s_poly[p][3] = s16(32767 - acc);
    }
}

// --- voices ---------------------------------------------------------------------------
struct Voice {
    u16 w[PB_WORDS];
    u32 cur() const { return u32(w[PB_CUR_HI]) << 16 | w[PB_CUR_LO]; }
    void setCur(u32 a) { w[PB_CUR_HI] = u16(a >> 16); w[PB_CUR_LO] = u16(a); }
    u32 end() const { return u32(w[PB_END_HI]) << 16 | w[PB_END_LO]; }
    u32 loop() const { return u32(w[PB_LOOP_HI]) << 16 | w[PB_LOOP_LO]; }
};

// One sample from ARAM through the accelerator, advancing (and looping) the address.
s16 fetch(Voice& v) {
    const u8* ar = aram();
    u32 a = v.cur();
    s32 val = 0;
    switch (v.w[PB_FORMAT]) {
    case 0x00: {  // ADPCM: nibble addresses, 8-byte frames (header byte + 14 nibbles)
        if ((a & 15) == 0) {
            v.w[PB_PS] = ar[(a >> 1) & kAramMask];
            a += 2;
        }
        const u8 byte = ar[(a >> 1) & kAramMask];
        int nib = (a & 1) ? (byte & 15) : (byte >> 4);
        if (nib >= 8) nib -= 16;
        const u16 ps = v.w[PB_PS];
        const int scale = 1 << (ps & 15), ci = (ps >> 4) & 7;
        const s32 c1 = s16(v.w[PB_COEFS + ci * 2]), c2 = s16(v.w[PB_COEFS + ci * 2 + 1]);
        const s32 yn1 = s16(v.w[PB_YN1]), yn2 = s16(v.w[PB_YN2]);
        s64 x = (s64(nib * scale) << 11) + s64(c1) * yn1 + s64(c2) * yn2;
        val = clamp16((x + 1024) >> 11);
        v.w[PB_YN2] = v.w[PB_YN1];
        v.w[PB_YN1] = u16(val);
        break;
    }
    case 0x0A: {  // PCM16: sample addresses
        const u8* p = ar + ((a * 2) & kAramMask);
        val = clamp16((s64(s16(be16(p))) * v.w[PB_GAIN]) >> 11);
        break;
    }
    case 0x19:  // PCM8: byte addresses
        val = clamp16(s64(s8(ar[a & kAramMask])) * v.w[PB_GAIN]);
        break;
    default:
        val = 0;
        break;
    }
    if (a == v.end()) {
        a = v.loop();
        if (v.w[PB_FORMAT] == 0) {
            if (v.w[PB_LOOP_FLAG]) {
                v.w[PB_PS] = v.w[PB_LOOP_PS];
                if (v.w[PB_LOOP_TYPE] == 0) {  // a sample loop restarts its decoder history
                    v.w[PB_YN1] = v.w[PB_LOOP_YN1];
                    v.w[PB_YN2] = v.w[PB_LOOP_YN2];
                }
            } else {  // one-shot: on into ARAM's zero buffer
                v.w[PB_PS] = 0;
                v.w[PB_YN1] = v.w[PB_YN2] = 0;
            }
        }
        if (v.w[PB_LOOP_FLAG]) v.w[PB_STREAM_LOOPS]++;
    } else {
        a++;
    }
    v.setCur(a);
    return s16(val);
}

// 32 resampled samples.
void resample(Voice& v, s32* out) {
    const u16 sel = v.w[PB_SRC_SELECT];
    u32 ratio = u32(v.w[PB_RATIO_HI]) << 16 | v.w[PB_RATIO_LO];
    if (sel == 2) ratio = 0x10000;
    u32 frac = v.w[PB_FRAC];
    s16 h[4];
    for (int k = 0; k < 4; k++) h[k] = s16(v.w[PB_LAST + k]);
    for (int i = 0; i < kSub; i++) {
        frac += ratio;
        while (frac >= 0x10000) {
            h[0] = h[1];
            h[1] = h[2];
            h[2] = h[3];
            h[3] = fetch(v);
            frac -= 0x10000;
        }
        if (sel == 0) {
            const s16* c = s_poly[(frac & 0xFFFF) >> 9];
            out[i] = clamp16((s64(h[0]) * c[0] + s64(h[1]) * c[1] + s64(h[2]) * c[2] + s64(h[3]) * c[3]) >> 15);
        } else if (sel == 1) {
            out[i] = h[1] + s32((s64(h[2] - h[1]) * s64(frac & 0xFFFF)) >> 16);
        } else {
            out[i] = h[3];
        }
    }
    v.w[PB_FRAC] = u16(frac);
    for (int k = 0; k < 4; k++) v.w[PB_LAST + k] = u16(h[k]);
}

struct BusMix {
    int vol, delta, dpop;
    s32* dst;
};

// R2_VOICE_LOG=1 (issue #57): once a second every voice that played - its PB, format, loop flag
// and type, the addresses it read (ADPCM: nibbles), its loop and end addresses, its loudest raw
// sample and volumes, and how often it was past its end address after a subframe.
struct VoiceSeen {
    u32 pb = 0, first = 0, lo = 0, hi = 0, loop = 0, end = 0, pastEnd = 0, frames = 0;
    u16 fmt = 0, lf = 0, lt = 0, vol = 0, vl = 0, vr = 0, vbl = 0, vbr = 0;
    s32 peak = 0;
};
VoiceSeen s_seen[96];
int s_seenCount = 0;
u64 s_voiceLogLists = 0;
u64 s_listNo = 0;  // lists run so far (R2_AUDIO_LOG: voices starting)
struct VoiceRun {
    u32 pb;
    u64 list;
};
VoiceRun s_running[128];
// R2_AUDIO_LOG=1: a voice that was not playing in the previous list starts (latency tests)
void noteVoiceStart(u32 pb, const u16* w) {
    static const bool on = envFlag("R2_AUDIO_LOG");
    if (!on) return;
    VoiceRun* slot = nullptr;
    for (VoiceRun& r : s_running)
        if (r.pb == pb) slot = &r;
    if (!slot)
        for (VoiceRun& r : s_running)
            if (!r.pb || r.list + 2 < s_listNo) slot = &r;
    if (!slot) return;
    const bool fresh = slot->pb != pb || slot->list + 1 < s_listNo;
    slot->pb = pb;
    slot->list = s_listNo;
    if (fresh && w[PB_LOOP_FLAG] == 0)
        logf("audio", "voice %08X starts (one-shot, end %07X, volume L %04X R %04X)", pb,
             u32(w[PB_END_HI]) << 16 | w[PB_END_LO], w[PB_VL], w[PB_VR]);
}
bool voiceLogOn() {
    static const bool on = envFlag("R2_VOICE_LOG");
    return on;
}
VoiceSeen& seen(u32 pb, const Voice& v) {
    for (int i = 0; i < s_seenCount; i++)
        if (s_seen[i].pb == pb) return s_seen[i];
    VoiceSeen& s = s_seen[s_seenCount < 96 ? s_seenCount++ : 95];
    s = VoiceSeen{};
    s.pb = pb;
    s.first = s.lo = s.hi = v.cur();
    return s;
}
void voiceLogFlush() {
    if (++s_voiceLogLists % 200 || !s_seenCount) return;
    for (int i = 0; i < s_seenCount; i++) {
        const VoiceSeen& s = s_seen[i];
        // other voices whose reads fall in this one's
        char ov[64] = "";
        for (int j = 0; j < s_seenCount; j++)
            if (j != i && s_seen[j].lo <= s.hi && s_seen[j].hi >= s.lo && std::strlen(ov) < 50)
                snprintf(ov + std::strlen(ov), sizeof ov - std::strlen(ov), " %08X", s_seen[j].pb);
        logf("voice", "%08X fmt %02X loop %u/%u first %07X read %07X-%07X loop %07X end %07X past end %u/%u peak %5d vol %04X "
             "L %04X R %04X BL %04X BR %04X%s%s",
             s.pb, s.fmt, s.lf, s.lt, s.first, s.lo, s.hi, s.loop, s.end, s.pastEnd, s.frames, s.peak, s.vol, s.vl, s.vr,
             s.vbl, s.vbr, *ov ? " overlaps" : "", ov);
    }
    s_seenCount = 0;
}

void processVoice(u32 pbAddr) {
    u8* p = mem(pbAddr, PB_WORDS * 2);
    if (!p) return;
    Voice v;
    for (int i = 0; i < PB_WORDS; i++) v.w[i] = be16(p + 2 * i);
    s_voices++;

    // which buses this voice feeds (mixerCtrl, MusyX <= 2.0.0; see HandleDepopVoice)
    const u16 mc = v.w[PB_MIXER_CTRL];
    const bool useS = mc & 4, useA = mc & 1, useB = mc & 0x12;
    BusMix bus[9] = {
        {PB_VL, PB_DL, PB_DPOP_L, s_main[L]},     {PB_VR, PB_DR, PB_DPOP_R, s_main[R]},
        {PB_VS, PB_DS, PB_DPOP_S, s_main[S]},     {PB_VAL, PB_DAL, PB_DPOP_AL, s_auxA[L]},
        {PB_VAR, PB_DAR, PB_DPOP_AR, s_auxA[R]},  {PB_VAS, PB_DAS, PB_DPOP_AS, s_auxA[S]},
        {PB_VBL, PB_DBL, PB_DPOP_BL, s_auxB[L]},  {PB_VBR, PB_DBR, PB_DPOP_BR, s_auxB[R]},
        {PB_VBS, PB_DBS, PB_DPOP_BS, s_auxB[S]},
    };
    const bool enabled[9] = {true, true, useS, useA, useA, useA && (mc & 0x14), useB, useB, useB && useS};

    u32 upd = u32(v.w[PB_UPD_HI]) << 16 | v.w[PB_UPD_LO];
    s32 last = 0;
    for (int sf = 0; sf < 5; sf++) {
        // parameter updates take effect at the start of their subframe
        for (u16 n = v.w[PB_UPD_NUM + sf]; n; n--) {
            const u8* e = mem(upd, 4);
            upd += 4;
            if (!e) break;
            const u16 off = be16(e), val = be16(e + 2);
            if (off < PB_WORDS) v.w[off] = val;
        }
        if (v.w[PB_STATE] != 1) continue;
        if (sf == 0) noteVoiceStart(pbAddr, v.w);
        s32 smp[kSub];
        VoiceSeen* vs = voiceLogOn() ? &seen(pbAddr, v) : nullptr;
        resample(v, smp);
        if (vs) {
            const u32 c = v.cur();
            vs->lo = std::min(vs->lo, c);
            vs->hi = std::max(vs->hi, c);
            vs->fmt = v.w[PB_FORMAT], vs->lf = v.w[PB_LOOP_FLAG], vs->lt = v.w[PB_LOOP_TYPE];
            vs->loop = v.loop(), vs->end = v.end();
            vs->pastEnd += c > v.end();
            vs->frames++;
            for (int i = 0; i < kSub; i++) vs->peak = std::max(vs->peak, std::abs(smp[i]));
            vs->vol = v.w[PB_VE_VOL], vs->vl = v.w[PB_VL], vs->vr = v.w[PB_VR], vs->vbl = v.w[PB_VBL], vs->vbr = v.w[PB_VBR];
        }
        s32 vol = v.w[PB_VE_VOL];
        const s32 vdelta = s16(v.w[PB_VE_DELTA]);
        for (int i = 0; i < kSub; i++) {
            smp[i] = s32((s64(smp[i]) * vol) >> 15);
            vol += vdelta;
            if (vol < 0) vol = 0;
            if (vol > 0xFFFF) vol = 0xFFFF;
        }
        v.w[PB_VE_VOL] = u16(vol);
        last = smp[kSub - 1];
        const int base = sf * kSub;
        // (7.1) how far back the voice sits, from its front and aux B (rear, in a DPL2 studio)
        // volumes: CalcBusDPL2 gives both 0.71 at the side, front 0 behind
        const float front = float(v.w[PB_VL]) + float(v.w[PB_VR]), rear = float(v.w[PB_VBL]) + float(v.w[PB_VBR]);
        const float wBack = front + rear > 0.f ? std::clamp((rear - front) / (rear + front), 0.f, 1.f) : 0.f;
        const float gBack = std::sqrt(wBack), gSide = std::sqrt(1.f - wBack);
        for (int b = 0; b < 9; b++) {
            s32 mv = v.w[bus[b].vol];
            const s32 md = s16(v.w[bus[b].delta]);
            if (!enabled[b] || (mv == 0 && md == 0)) continue;
            s32* d = bus[b].dst + base;
            const int split = b == 6 ? 0 : b == 7 ? 1 : -1;  // aux B L/R
            for (int i = 0; i < kSub; i++) {
                const s32 x = s32((s64(smp[i]) * mv) >> 15);
                d[i] += x;
                if (split >= 0) {
                    s_auxBSide[split][base + i] += float(x) * gSide;
                    s_auxBBack[split][base + i] += float(x) * gBack;
                }
                mv += md;
                if (mv < 0) mv = 0;
                if (mv > 0xFFFF) mv = 0xFFFF;
            }
            v.w[bus[b].vol] = u16(mv);
        }
    }
    // depop: what each bus last received from this voice (faded out by the CPU on stop)
    for (int b = 0; b < 9; b++)
        v.w[bus[b].dpop] = u16(enabled[b] ? clamp16((s64(last) * v.w[bus[b].vol]) >> 15) : 0);
    for (int i = 0; i < PB_WORDS; i++) {
        p[2 * i] = u8(v.w[i] >> 8);
        p[2 * i + 1] = u8(v.w[i]);
    }
    watchHost(pbAddr, PB_WORDS * 2, "DSP PB write-back");
}

// --- buffers in main memory: L[160], R[160], S[160] of big-endian s32 -------------------
void readLRS(u32 addr, s32 (*dst)[kN], s32 vol, bool add) {
    const u8* p = mem(addr, 3 * kN * 4);
    if (!p) return;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < kN; i++) {
            const s32 x = be32s(p + (c * kN + i) * 4);
            const s32 y = vol == 0x8000 ? x : s32((s64(x) * vol) >> 15);
            dst[c][i] = add ? dst[c][i] + y : y;
        }
}
void writeLRS(u32 addr, s32 (*src)[kN]) {
    u8* p = mem(addr, 3 * kN * 4);
    if (!p) return;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < kN; i++) put32(p + (c * kN + i) * 4, src[c][i]);
    watchHost(addr, 3 * kN * 4, "DSP buffer upload");
}

void runList(u32 addr) {
    s_lists++;
    s_listDpl2 = false;
    for (int guard = 0; guard < 100000; guard++) {
        const u8* p = mem(addr, 16);
        if (!p) return;
        const u16 cmd = be16(p);
        auto arg = [&](int i) { return be16(p + 2 + 2 * i); };
        auto arg32 = [&](int i) { return u32(arg(i)) << 16 | arg(i + 1); };
        if (s_log && s_lists < 4) logf("dsp", "cmd %u at %08X", cmd, addr);
        switch (cmd) {
        case 0: {  // SETUP: the studio's buses start from its depop ramps
            const u8* spb = mem(arg32(0), 0x36);
            s32* dst[9] = {s_main[L], s_main[R], s_main[S], s_auxA[L], s_auxA[R], s_auxA[S],
                           s_auxB[L], s_auxB[R], s_auxB[S]};
            for (int b = 0; b < 9; b++) {
                s32 start = spb ? be32s(spb + b * 6) : 0;
                const s32 delta = spb ? s16(be16(spb + b * 6 + 4)) : 0;
                for (int i = 0; i < kN; i++, start += delta) dst[b][i] = start;
            }
            std::memset(s_auxBSide, 0, sizeof s_auxBSide);
            std::memset(s_auxBBack, 0, sizeof s_auxBBack);
            s_studioRear.clear();
            s_studioHasRear = false;
            addr += 2 * 3;
            break;
        }
        case 1: {  // DL_AND_VOL_MIX: another studio's output into main / aux A / aux B
            const u32 src = arg32(0);
            const s32 vol = arg(2), volA = arg(3), volB = arg(4);
            s32 tmp[3][kN];
            readLRS(src, tmp, 0x8000, false);
            for (int c = 0; c < 3; c++)
                for (int i = 0; i < kN; i++) {
                    s_main[c][i] += s32((s64(tmp[c][i]) * vol) >> 15);
                    s_auxA[c][i] += s32((s64(tmp[c][i]) * volA) >> 15);
                    s_auxB[c][i] += s32((s64(tmp[c][i]) * volB) >> 15);
                }
            for (int c = 0; c < 2; c++)  // (7.1: into a DPL2 studio's rear, at the side)
                for (int i = 0; i < kN; i++) s_auxBSide[c][i] += float((s64(tmp[c][i]) * volB) >> 15);
            // that studio's own rear, as last frame's main buffer carried it (the final mix's
            // buffers come from this list; an input's from the previous one)
            for (const StudioRear& sr : s_studioRears)
                if (sr.addr == src && sr.list + 1 == s_lists && vol) {
                    s_studioRear.add(sr.rear, float(vol) / 32768.f);
                    s_studioHasRear = true;
                }
            addr += 2 * 6;
            break;
        }
        case 2: {  // PB_ADDR + PROCESS: the voice list
            u32 pb = arg32(0);
            addr += 2 * 3;
            const u8* q = mem(addr, 2);
            if (q && be16(q) == 3) addr += 2;
            for (int n = 0; pb && n < 256; n++) {
                const u8* h = mem(pb, 4);
                if (!h) break;
                const u32 next = u32(be16(h)) << 16 | be16(h + 2);
                processVoice(pb);
                pb = next;
            }
            break;
        }
        case 3:  // PROCESS without PB_ADDR: nothing to do
            addr += 2;
            break;
        case 4:  // MIX_AUXA: aux A out to the CPU's effect, its processed return into main
        case 5: {  // MIX_AUXB
            s32(*aux)[kN] = cmd == 4 ? s_auxA : s_auxB;
            writeLRS(arg32(0), aux);
            readLRS(arg32(2), s_main, 0x8000, true);
            addr += 2 * 5;
            break;
        }
        case 16: {  // MIX_AUXB_LR (DPL2 studios): the rear on aux B L/R, encoded into main L/R
            // Up: aux B L/R (two channels). Down: what went up the frame before (the game leaves
            // it alone), rear left into L, rear right inverted into R (as Dolphin's AX HLE, which
            // matches the ucode; this was added like an effect return before: in phase, so a
            // Pro Logic decoder put the rear in the front - issue #15).
            const u32 up = arg32(0), down = arg32(2);
            if (u8* p = mem(up, 2 * kN * 4)) {
                for (int c = 0; c < 2; c++)
                    for (int i = 0; i < kN; i++) put32(p + (c * kN + i) * 4, s_auxB[c][i]);
                watchHost(up, 2 * kN * 4, "DSP aux B L/R upload");
                RearUpload* slot = &s_rearUploads[s_rearUploadNext];
                for (RearUpload& u : s_rearUploads)
                    if (u.addr == up) slot = &u;
                if (slot == &s_rearUploads[s_rearUploadNext]) s_rearUploadNext = (s_rearUploadNext + 1) % 8;
                slot->addr = up;
                std::memcpy(slot->data, s_auxB, sizeof slot->data);
                std::memcpy(slot->side, s_auxBSide, sizeof slot->side);
                std::memcpy(slot->back, s_auxBBack, sizeof slot->back);
            }
            if (const u8* p = mem(down, 2 * kN * 4)) {
                const RearUpload* known = nullptr;
                for (const RearUpload& u : s_rearUploads)
                    if (u.addr == down) known = &u;
                for (int c = 0; c < 2; c++)
                    for (int i = 0; i < kN; i++) {
                        const s32 x = be32s(p + (c * kN + i) * 4);
                        s_main[c][i] += c == L ? x : -x;
                        s_studioRear.rear[c][i] += x;
                        // the 7.1 split went up with it (else: all of it at the side)
                        if (known && known->data[c][i] == x) {
                            s_studioRear.side[c][i] += known->side[c][i];
                            s_studioRear.back[c][i] += known->back[c][i];
                        } else {
                            s_studioRear.side[c][i] += float(x);
                        }
                    }
                s_studioHasRear = true;
            }
            s_listDpl2 = true;
            addr += 2 * 5;
            break;
        }
        case 6: {  // UPLOAD_LRS: the studio's main buses to main memory
            const u32 up = arg32(0);
            writeLRS(up, s_main);
            if (s_studioHasRear) {  // (its rear goes along, for the final mix / other studios)
                StudioRear* slot = nullptr;
                for (StudioRear& sr : s_studioRears)
                    if (sr.addr == up || (!slot && sr.list + 1 < s_lists)) slot = &sr;
                if (!slot) slot = &s_studioRears[s_lists % 16];
                slot->addr = up;
                slot->list = s_lists;
                slot->rear = s_studioRear;
            }
            addr += 2 * 3;
            break;
        }
        case 17: {  // SET_OPPOSITE_LR: the final mix starts with last frame's surround folded in
            // (OUTPUT uploads S there): -S into L, +S into R (Dolphin's AX HLE)
            const u8* p = mem(arg32(0), kN * 4);
            s_outHasSurround = false;
            for (int i = 0; i < kN; i++) {
                const s32 x = p ? be32s(p + i * 4) : 0;
                s_out[L][i] = -x;
                s_out[R][i] = x;
                s_out[S][i] = 0;
                s_outS[i] = x;
                s_outHasSurround |= x != 0;
            }
            s_outRear.clear();
            addr += 2 * 3;
            break;
        }
        case 9: {  // MIX_AUXB_NOWRITE: a master studio's uploaded output into the final mix
            const u32 src = arg32(0);
            readLRS(src, s_out, 0x8000, true);
            for (const StudioRear& sr : s_studioRears)
                if (sr.addr == src && sr.list == s_lists) s_outRear.add(sr.rear);
            addr += 2 * 3;
            break;
        }
        case 14: {  // OUTPUT: S up for the next frame's fold, 16-bit stereo to the AI buffer
            if (u8* surr = mem(arg32(0), kN * 4))
                for (int i = 0; i < kN; i++) put32(surr + i * 4, s_out[S][i]);
            const u32 lr = arg32(2);
            if (u8* d = mem(lr, kN * 4)) {
                DiscreteBlock& blk = s_blocks[s_blockNext];
                s_blockNext = (s_blockNext + 1) % 8;
                blk.addr = lr;
                for (int i = 0; i < kN; i++) {
                    const s16 l = clamp16(s_out[L][i]), r = clamp16(s_out[R][i]);
                    d[i * 4 + 0] = u8(u16(l) >> 8); d[i * 4 + 1] = u8(l);
                    d[i * 4 + 2] = u8(u16(r) >> 8); d[i * 4 + 3] = u8(r);
                    // discrete: the front without the surround and rear folded into it
                    const s32 sv = s_outS[i];
                    const s32 rl = s_outRear.rear[L][i], rr = s_outRear.rear[R][i];
                    s16* ch = blk.ch[i];
                    ch[0] = clamp16(s64(s_out[L][i]) + sv - rl);
                    ch[1] = clamp16(s64(s_out[R][i]) - sv + rr);
                    // 5.1's surround pair: all of the rear, the mono surround on both (-3 dB)
                    const s32 s3 = s32((s64(sv) * 23170) >> 15), s6 = sv / 2;
                    ch[2] = clamp16(s64(rl) + s3);
                    ch[3] = clamp16(s64(rr) + s3);
                    // 7.1: back and side pairs (the mono surround on all four, -6 dB)
                    ch[4] = clamp16(std::lround(s_outRear.back[L][i]) + s6);
                    ch[5] = clamp16(std::lround(s_outRear.back[R][i]) + s6);
                    ch[6] = clamp16(std::lround(s_outRear.side[L][i]) + s6);
                    ch[7] = clamp16(std::lround(s_outRear.side[R][i]) + s6);
                }
                std::memcpy(blk.stereo, d, sizeof blk.stereo);
                // R2_SURROUND_LOG=1: levels of the parts every quarter second
                static const bool slog = envFlag("R2_SURROUND_LOG");
                if (slog) {
                    static double e[8];
                    static int frames = 0;
                    for (int i = 0; i < kN; i++) {
                        const double v[8] = {double(s_out[L][i]), double(s_out[R][i]), double(s_outS[i]),
                                             double(s_outRear.rear[L][i]), double(s_outRear.rear[R][i]),
                                             double(blk.ch[i][0]), double(blk.ch[i][1]),
                                             double(s_outRear.back[L][i]) + double(s_outRear.back[R][i])};
                        for (int k = 0; k < 8; k++) e[k] += v[k] * v[k];
                    }
                    if (++frames == 50) {
                        auto dbv = [](double x) { return x > 0 ? 10.0 * std::log10(x / (50.0 * kN) / (32768.0 * 32768.0)) : -120.0; };
                        logf("dsp", "surround: Lt %.1f Rt %.1f S %.1f rearL %.1f rearR %.1f FL %.1f FR %.1f back %.1f dB",
                             dbv(e[0]), dbv(e[1]), dbv(e[2]), dbv(e[3]), dbv(e[4]), dbv(e[5]), dbv(e[6]), dbv(e[7]));
                        frames = 0;
                        std::memset(e, 0, sizeof e);
                    }
                }
            }
            if (!s_loggedMode && s_outHasSurround) {
                s_loggedMode = true;
                logf("dsp", "MusyX: surround channel in use");
            }
            watchHost(lr, kN * 4, "DSP output");
            aiTraceEvent('M', lr);
            watchHost(arg32(0), kN * 4, "DSP surround");
            addr += 2 * 5;
            break;
        }
        case 15:  // END
            if (s_listDpl2 != s_dpl2) {
                s_dpl2 = s_listDpl2;
                logf("dsp", "MusyX: Pro Logic II mixing %s (rear channels encoded into the stereo)", s_dpl2 ? "on" : "off");
            }
            return;
        case 13:  // MORE: the list continues at another address
            addr = arg32(0);
            break;
        case 18:  // compressor (MusyX >= 2.0.1)
            addr += 2 * 5;
            break;
        default: {
            static int warned = 0;
            if (warned++ < 8) logf("dsp", "MusyX: unknown command %u at %08X", cmd, addr);
            return;
        }
        }
    }
}

u32 s_pendingFirstSize = 0;
bool s_expectList = false;

}  // namespace

bool musyxDspMail(u32 mail, u32& reply) {
    static bool init = false;
    if (!init) {
        init = true;
        initPolyphase();
        s_log = envFlag("R2_DSP_LOG");
    }
    if (s_expectList) {
        s_expectList = false;
        runList(mail);
        s_listNo++;
        if (voiceLogOn()) voiceLogFlush();
        reply = 0xDCD10001;  // done with this frame: the task manager calls MusyX's resume callback
        return true;
    }
    if ((mail >> 16) == 0xBABE) {
        s_pendingFirstSize = mail & 0xFFFF;
        s_expectList = true;
        return false;
    }
    if (s_log) logf("dsp", "MusyX: mail %08X ignored", mail);
    return false;
}

const s16* musyxDiscreteBlock(const u8* stereo, u32 frames) {
    if (frames != kN) return nullptr;
    for (const DiscreteBlock& b : s_blocks)
        if (b.addr && std::memcmp(b.stereo, stereo, sizeof b.stereo) == 0) return &b.ch[0][0];
    return nullptr;
}

std::string musyxDspStats() {
    char b[96];
    snprintf(b, sizeof b, "%llu lists, %llu voice frames", (unsigned long long)s_lists, (unsigned long long)s_voices);
    s_lists = s_voices = 0;
    return b;
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "../savestate.h"
namespace rt {
void musyxSaveState(state::Writer& w) {
    w.section("MUSY");
    w.pod(s_pendingFirstSize);
    w.pod(s_expectList);
}
void musyxLoadState(state::Reader& r) {
    r.section("MUSY");
    r.pod(s_pendingFirstSize);
    r.pod(s_expectList);
}
}  // namespace rt
