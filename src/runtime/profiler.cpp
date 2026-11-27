// Sampling profiler (R2_PROFILE=<seconds>): samples the instruction pointer of the guest's
// host thread (all guest threads are fibers on it) about once a millisecond and, every
// <seconds>, logs the hottest host functions - generated f_XXXXXXXX and runtime code alike -
// resolved from the PDB through DbgHelp.
//
// Always on, for the session log (main.cpp): every 30 s a "perf" line (frame rate, slow frames,
// draws, how busy the game thread was), and stall reports - when no frame is presented for
// 250 ms, a watchdog samples the game thread's call stack every 5 ms until frames come again
// (or every 10 s while they do not) and logs where it was.
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hw/musyx_dsp.h"
#include "runtime.h"

namespace rt {

namespace {

// The level the game loaded last (main.cpp discNoteRead: lNN of Rogue2/lvlPackNN.dat; "l22" the
// front end), for stall and perf lines. (They showed the byte 0x800962EC before: 22 all through
// play, the mission's number only from its results on.)
std::string levelName() {
    const int l = discLevel();
    char b[16];
    if (l >= 0) snprintf(b, sizeof b, "l%02d", l);
    else snprintf(b, sizeof b, "-");
    return b;
}

#ifdef _WIN32
struct Sym {
    std::string name;
    u64 start = 0;
};

// DbgHelp is single-threaded: the profiler and the stall watchdog take this around lookups.
std::mutex s_symMu;

void symInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    });
}

// Resolves an address to its function; caches whole functions by start address. s_symMu held.
const Sym& resolve(u64 rip) {
    static std::unordered_map<u64, Sym> byRip;
    auto it = byRip.find(rip);
    if (it != byRip.end()) return it->second;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    Sym s;
    if (SymFromAddr(GetCurrentProcess(), rip, &disp, si)) {
        s.name = si->Name;
        s.start = rip - disp;
    } else {
        // no symbols (a system or driver DLL): its module and offset
        char b[MAX_PATH + 32];
        HMODULE mod = nullptr;
        char path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &mod) &&
            GetModuleFileNameA(mod, path, sizeof path)) {
            const char* base = strrchr(path, '\\');
            snprintf(b, sizeof b, "%s+%llx", base ? base + 1 : path, (unsigned long long)(rip - u64(mod)));
        } else {
            snprintf(b, sizeof b, "?%llx", (unsigned long long)rip);
        }
        s.name = b;
    }
    return byRip.emplace(rip, std::move(s)).first->second;
}

// Library leaf functions are attributed to their caller too (the return address on the stack
// at the sample), e.g. "memmove <- rt::aramDma".
bool isLeafHelper(const std::string& n) {
    return n == "memmove" || n == "memcpy" || n == "memset" || n == "memcmp" || n.rfind("XXH", 0) == 0 ||
           n.rfind("NtWait", 0) == 0 || n.rfind("ZwWait", 0) == 0 || n.rfind("WaitFor", 0) == 0 ||
           n.rfind("SleepConditionVariable", 0) == 0 || n.rfind("RtlSleepConditionVariable", 0) == 0 ||
           n.find("SRWLock") != std::string::npos || n.rfind("_NLG", 0) == 0 || n.rfind("std::_Atomic", 0) == 0 ||
           n == "strchr" || n == "RtlRaiseStatus" || n.rfind("NtAlert", 0) == 0 ||
           // system calls (device I/O, registry, files): who made them
           n.rfind("Nt", 0) == 0 || n.rfind("Zw", 0) == 0 || n == "DeviceIoControl";
}

std::unordered_map<u64, std::unordered_map<u64, u32>> s_callers;  // rip -> return address -> hits
struct Chain {
    u64 f[6];
};
std::unordered_map<u64, std::vector<Chain>> s_chains;  // rip -> caller chains (helpers only)

void report(const std::unordered_map<u64, u32>& hits, u64 total, int secs) {
    std::lock_guard lock(s_symMu);
    std::unordered_map<std::string, u32> byName;
    for (auto& [rip, n] : hits) {
        const std::string& name = resolve(rip).name;
        if (isLeafHelper(name)) {
            if (auto it = s_chains.find(rip); it != s_chains.end()) {
                for (const Chain& ch : it->second) {
                    std::string label = name;
                    int shown = 0;
                    for (u64 f : ch.f) {
                        if (!f) break;
                        const std::string& fn = resolve(f).name;
                        if (isLeafHelper(fn)) continue;
                        label += " <- " + fn;
                        if (++shown == 4) break;
                    }
                    byName[label]++;
                }
                continue;
            }
        }
        byName[name] += n;
    }
    s_callers.clear();
    s_chains.clear();
    // where the time goes, roughly: guest code, guest memory/FP helpers, graphics, waiting
    {
        u32 cat[6] = {};  // game, memory, graphics (bridge), wait, other, graphics (Aurora)
        for (auto& [name, n] : byName) {
            const auto has = [&name](const char* p) { return name.find(p) != std::string::npos; };
            int c = 4;
            if (name.rfind("NtWait", 0) == 0 || name.rfind("ZwWait", 0) == 0 || name.rfind("WaitFor", 0) == 0 || has("idleWait")) c = 3;
            else if (name.rfind("rt::f_", 0) == 0) c = 0;
            else if (has("aurora::") || has("dawn") || has("wgpu")) c = 5;
            else if (has("gfx::") || has("gxFifo") || has("rt::gx") || name.rfind("XXH", 0) == 0 || has("cmdLength") || has("cpRun"))
                c = 2;
            else if (name.rfind("rt::rd", 0) == 0 || name.rfind("rt::wr", 0) == 0 || name.rfind("rt::psq", 0) == 0 || has("noteAccess") ||
                     has("tlb") || has("Tlb") || has("vm"))
                c = 1;
            cat[c] += n;
        }
        const double t = double(total ? total : 1) / 100.0;
        logf("prof", "game code %.1f%%, memory/psq %.1f%%, graphics bridge %.1f%% + Aurora %.1f%%, waiting %.1f%%, other %.1f%%",
             cat[0] / t, cat[1] / t, cat[2] / t, cat[5] / t, cat[3] / t, cat[4] / t);
    }
    std::vector<std::pair<std::string, u32>> v(byName.begin(), byName.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    logf("prof", "--- %llu samples over %d s (top functions, self time) ---", (unsigned long long)total, secs);
    for (size_t i = 0; i < v.size() && i < 40; i++)
        logf("prof", "%5.1f%%  %s", 100.0 * v[i].second / double(total ? total : 1), v[i].first.c_str());
}

// Up to n return addresses from a stopped thread's context. A bad frame (code without unwind
// data, a stack that does not look like one) ends the walk instead of crashing.
__declspec(noinline) void unwind(const CONTEXT& start, u64* chain, int n) {
    CONTEXT uc = start;
    __try {
        for (int d = 0; d < n; d++) {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(uc.Rip, &imageBase, nullptr);
            if (!fe) {  // leaf: the return address is on top of the stack
                uc.Rip = *reinterpret_cast<const DWORD64*>(uc.Rsp);
                uc.Rsp += 8;
            } else {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, uc.Rip, fe, &uc, &handlerData, &establisher, nullptr);
            }
            if (!uc.Rip) break;
            chain[d] = uc.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The game thread's current function and up to n - 1 callers (nothing allocated while it is
// stopped: it may hold the heap's lock).
bool sampleStack(HANDLE thread, u64* chain, int n) {
    if (SuspendThread(thread) == DWORD(-1)) return false;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    const bool ok = GetThreadContext(thread, &ctx);
    if (ok) {
        chain[0] = ctx.Rip;
        unwind(ctx, chain + 1, n - 1);
    }
    ResumeThread(thread);
    return ok;
}

HANDLE gameThreadHandle() {
    static HANDLE h = [] {
        HANDLE t = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t, 0, FALSE, DUPLICATE_SAME_ACCESS);
        return t;
    }();
    return h;
}

// --- stall reports ------------------------------------------------------------------------
std::atomic<u64> s_lastPresent{0};  // time base of the last presented frame (0: none yet)
constexpr int kStallDepth = 14;
struct StallSample {
    u64 f[kStallDepth];
};

void stallReport(const std::vector<StallSample>& samples, double ms, bool ongoing) {
    std::lock_guard lock(s_symMu);
    symInit();
    std::unordered_map<std::string, u32> byChain;
    for (const StallSample& s : samples) {
        std::string label;
        int shown = 0;
        for (int i = 0; i < kStallDepth && s.f[i]; i++) {
            const std::string& name = resolve(s.f[i]).name;
            if (i > 0 && isLeafHelper(name)) continue;
            if (!label.empty()) label += " <- ";
            label += name;
            if (++shown == 8) break;
        }
        byChain[label]++;
    }
    std::vector<std::pair<std::string, u32>> v(byChain.begin(), byChain.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    logf("stall", "%s %.0f ms without a new frame (level %s); the game thread in %zu samples:",
         ongoing ? "still waiting:" : "frames again after", ms, levelName().c_str(), samples.size());
    for (size_t i = 0; i < v.size() && i < 4; i++)
        logf("stall", "  %3.0f%%  %s", 100.0 * v[i].second / double(samples.size()), v[i].first.c_str());
    if (ongoing) logf("stall", "  gpu: %s", gpuStateText().c_str());
}

#else
// Linux: no host stack samples (the stall is reported with its length and the level)
std::atomic<u64> s_lastPresent{0};
#endif

#ifdef _WIN32
void stallWatchdog(HANDLE thread) {
    constexpr u64 kStall = kTbClock / 4;        // 250 ms without a frame
    constexpr u64 kInterim = 10 * kTbClock;     // a longer one is reported every 10 s meanwhile,
    constexpr u64 kFirst = 2 * kTbClock;        // the first time after 2 s (a game closed during a
                                                // freeze of a few seconds left no trace: issue #38)
    for (;;) {
        Sleep(20);
        const u64 last = s_lastPresent.load(std::memory_order_relaxed);
        if (!last || tbNow() - last < kStall) continue;
        std::vector<StallSample> samples;
        u64 reported = last;
        while (s_lastPresent.load(std::memory_order_relaxed) == last) {
            StallSample s{};
            if (sampleStack(thread, s.f, kStallDepth)) samples.push_back(s);
            Sleep(5);
            const u64 now = tbNow();
            if (now - reported >= (reported == last ? kFirst : kInterim) && s_lastPresent.load(std::memory_order_relaxed) == last) {
                stallReport(samples, double(now - last) * 1000.0 / double(kTbClock), true);
                samples.clear();
                reported = now;
            }
        }
        const u64 next = s_lastPresent.load(std::memory_order_relaxed);
        if (!samples.empty()) stallReport(samples, double(next - last) * 1000.0 / double(kTbClock), false);
    }
}
#else
void stallWatchdog() {
    constexpr u64 kStall = kTbClock / 4, kInterim = 10 * kTbClock, kFirst = 2 * kTbClock;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const u64 last = s_lastPresent.load(std::memory_order_relaxed);
        if (!last || tbNow() - last < kStall) continue;
        u64 reported = last;
        while (s_lastPresent.load(std::memory_order_relaxed) == last) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const u64 now = tbNow();
            if (now - reported >= (reported == last ? kFirst : kInterim) && s_lastPresent.load(std::memory_order_relaxed) == last) {
                logf("stall", "still waiting: %.0f ms without a new frame (level %s)", double(now - last) * 1000.0 / double(kTbClock),
                     levelName().c_str());
                logf("stall", "  gpu: %s", gpuStateText().c_str());
                reported = now;
            }
        }
        const u64 next = s_lastPresent.load(std::memory_order_relaxed);
        logf("stall", "frames again after %.0f ms without a new frame (level %s)", double(next - last) * 1000.0 / double(kTbClock),
             levelName().c_str());
    }
}
#endif

// --- the 30-second perf line ---------------------------------------------------------------
struct PerfWindow {
    u64 start = 0, last = 0, frames = 0, slow = 0, longest = 0, draws = 0, idleAtStart = 0, present = 0;
};
PerfWindow s_perf;

}  // namespace

u64 g_idleTb = 0;  // events.cpp idleWait: time the game thread spent waiting for the next event

void diagStart() {
#ifdef _WIN32
    std::thread(stallWatchdog, gameThreadHandle()).detach();
#else
    std::thread(stallWatchdog).detach();
#endif
}

// Before the game: the launcher and the shader preparation screen run their own frame loops on
// the same thread. Each turn counts as a frame for the stall reports (not for the perf lines),
// so a freeze there (issue #41: the window stopped responding in the launcher, nothing logged)
// is reported with that thread's stacks every 10 s.
void diagUiFrame() { s_lastPresent.store(tbNow(), std::memory_order_relaxed); }

void diagFramePresented(u32 draws, u64 presentTb) {
    const u64 now = tbNow();
    s_lastPresent.store(now, std::memory_order_relaxed);
    PerfWindow& w = s_perf;
    if (!w.start) {
        w.start = w.last = now;
        w.idleAtStart = g_idleTb;
        return;
    }
    const u64 interval = now - w.last, period = u64(double(kTbClock) / viRate());
    w.last = now;
    w.frames++;
    if (interval > period + period / 2) w.slow++;
    w.longest = std::max(w.longest, interval);
    w.draws += draws;
    w.present += presentTb;
    if (now - w.start < 30 * kTbClock) return;
    const double secs = double(now - w.start) / double(kTbClock);
    const double busy = 100.0 * (1.0 - double(g_idleTb - w.idleAtStart) / double(now - w.start));
    logf("perf", "%.0f s: %.1f fps at %.0f Hz, %llu frames over %.1f ms, longest %.0f ms, %llu draws/frame, game thread "
                 "busy %.0f%% (presenting %.1f ms/frame), level %s, %s",
         secs, double(w.frames) / secs, viRate(), (unsigned long long)w.slow, 1500.0 / viRate(),
         double(w.longest) * 1000.0 / double(kTbClock), (unsigned long long)(w.draws / std::max<u64>(w.frames, 1)),
         std::clamp(busy, 0.0, 100.0), double(w.present) * 1000.0 / double(kTbClock) / double(std::max<u64>(w.frames, 1)),
         levelName().c_str(), audio::perfText().c_str());
    w = PerfWindow{};
    w.start = w.last = now;
    w.idleAtStart = g_idleTb;
}

void profilerStart() {
    const char* env = getenv("R2_PROFILE");
    if (!env || !*env) return;
#ifndef _WIN32
    logf("prof", "R2_PROFILE: the sampling profiler is Windows-only");
    return;
#else
    const int secs = atoi(env) > 0 ? atoi(env) : 5;
    HANDLE target = gameThreadHandle();
    std::thread([target, secs] {
        {
            std::lock_guard lock(s_symMu);
            symInit();
        }
        std::unordered_map<u64, u32> hits;
        u64 total = 0;
        ULONGLONG next = GetTickCount64() + ULONGLONG(secs) * 1000;
        for (;;) {
            Sleep(1);
            // Caller chain while the thread is stopped: RtlVirtualUnwind needs no allocation.
            u64 chain[7] = {};
            const bool ok = sampleStack(target, chain, 7);
            if (ok) {  // resolve only after resuming (sampleStack did): symbol lookup allocates
                hits[chain[0]]++;
                // the first caller outside the helper (memmove's own frames are skipped in report)
                s_callers[chain[0]][chain[1]]++;
                s_chains[chain[0]].push_back({chain[1], chain[2], chain[3], chain[4], chain[5], chain[6]});
                total++;
            }
            if (GetTickCount64() >= next) {
                report(hits, total, secs);
                hits.clear();
                total = 0;
                next += ULONGLONG(secs) * 1000;
            }
        }
    }).detach();
    logf("prof", "sampling every 1 ms, report every %d s", secs);
#endif
}

}  // namespace rt
