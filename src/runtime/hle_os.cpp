// OS-level hooks: the game's debug output and halts.
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime.h"

namespace rt {

namespace {
// printf-style formatting of a guest call: integers from r4 (or r3 with fmtReg=3) onward,
// then the stack's parameter area; doubles from f1 onward (PowerPC EABI varargs).
std::string guestFormat(Ctx& c, u32 fmtAddr, u32 firstReg) {
    std::string out;
    u32 gpr = firstReg, fpr = 1, stackArg = c.r[1] + 8;
    auto nextInt = [&]() -> u32 {
        if (gpr <= 10) return c.r[gpr++];
        const u32 v = rd32(stackArg);
        stackArg += 4;
        return v;
    };
    auto nextDouble = [&]() -> double {
        if (fpr <= 8) return c.f[fpr++];
        stackArg = (stackArg + 7) & ~7u;
        const double v = bdbl(rd64(stackArg));
        stackArg += 8;
        return v;
    };
    for (u32 p = fmtAddr; out.size() < 4096; p++) {
        const char ch = char(rd8(p));
        if (!ch) break;
        if (ch != '%') {
            out += ch;
            continue;
        }
        std::string spec = "%";
        char t;
        for (;;) {
            t = char(rd8(++p));
            if (!t) return out;
            if (t == '*') {
                spec += std::to_string(s32(nextInt()));
                continue;
            }
            if (strchr("-+ #0123456789.", t)) {
                spec += t;
                continue;
            }
            if (t == 'h' || t == 'l' || t == 'L' || t == 'z' || t == 'j' || t == 't') continue;  // ints are 32-bit
            break;
        }
        char buf[512];
        switch (t) {
        case '%': out += '%'; continue;
        case 'd': case 'i': spec += 'd'; snprintf(buf, sizeof buf, spec.c_str(), s32(nextInt())); break;
        case 'u': case 'x': case 'X': case 'o': spec += t; snprintf(buf, sizeof buf, spec.c_str(), nextInt()); break;
        case 'c': spec += 'c'; snprintf(buf, sizeof buf, spec.c_str(), int(nextInt() & 0xFF)); break;
        case 'p': snprintf(buf, sizeof buf, "0x%08X", nextInt()); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            spec += t;
            snprintf(buf, sizeof buf, spec.c_str(), nextDouble());
            break;
        case 's': {
            const u32 s = nextInt();
            std::string str;
            if (!s) str = "(null)";
            else
                for (u32 q = s; str.size() < 1024; q++) {
                    const char k = char(rd8(q));
                    if (!k) break;
                    str += k;
                }
            spec += 's';
            snprintf(buf, sizeof buf, spec.c_str(), str.c_str());
            break;
        }
        default: snprintf(buf, sizeof buf, "%%%c", t); break;
        }
        out += buf;
    }
    return out;
}

void report(const std::string& text) {
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    if (!t.empty()) logf("game", "%s", t.c_str());
}
}  // namespace

void hle_OSReport(Ctx& c) { report(guestFormat(c, c.r[3], 4)); }

void hle_OSPanic(Ctx& c) {
    // OSPanic(file, line, fmt, ...)
    std::string file;
    for (u32 q = c.r[3]; q && file.size() < 256; q++) {
        const char k = char(rd8(q));
        if (!k) break;
        file += k;
    }
    fatal("the game panicked at %s:%u: %s", file.c_str(), c.r[4], guestFormat(c, c.r[5], 6).c_str());
}

void hle_PPCHalt(Ctx& c) { fatal("the game halted (PPCHalt, called from %08X)", c.lr); }

// The SDK's __AI_SRC_INIT (AIInit, AISetDSPSampleRate, the stream rate setter): times one tick of
// the streaming sample counter at 48 kHz right after one at 32 kHz with OSGetTime and retries
// until it lands in one of two windows (a check of the audio chip's sample rate converter). The
// guest's clock is the host's, so the time between two register writes is the host's too: under
// arm64 translation (Android's x86_64 emulators) each try took ~7,000 ticks against the windows'
// 1,155 / 1,580, and the boot never got past AIInit. Nothing comes out of it but the AI control
// register it leaves: sample counter reset, streaming stopped, 32 kHz.
void hle_AISrcInit(Ctx&) {
    constexpr u32 kAiCr = 0xCC006C00;
    static bool logged = false;
    if (!logged) logged = true, logf("audio", "AI sample rate converter check skipped (__AI_SRC_INIT)");
    wr32(kAiCr, (rd32(kAiCr) & ~0x20u) | 0x20u);  // SCRESET
    wr32(kAiCr, rd32(kAiCr) & ~0x03u);           // 32 kHz, PSTAT off
}

}  // namespace rt
