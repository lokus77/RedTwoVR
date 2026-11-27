// Guest threads and exceptions on host fibers.
//
// The game's own OS (Dolphin SDK) schedules threads by saving and loading OSContext
// structures: OSSaveContext returns 0, and later OSLoadContext of that context makes it
// "return" 1 again; exceptions save the interrupted state into OSCurrentContext and a
// handler later OSLoadContext's it. Recompiled code keeps its state on the host stack, so:
//   * every guest context belongs to a host fiber (a fresh context gets a new fiber),
//   * OSSaveContext records the caller's jump buffer (RT_SAVECTX_CALL in generated code),
//   * an exception is delivered at a safepoint inside a host frame that records its own
//     jump buffer; loading that context jumps back there and the code continues,
//   * loading a context owned by another fiber switches to that fiber.
// Only one fiber runs at a time, so the guest sees a single CPU, as on the console.
//
// The fibers are our own rather than Windows': stacks at fixed addresses (memory.cpp) and
// switches through rt_setjmp/rt_longjmp, so a save state can copy them and put them back
// (also in a later session). The program's main thread is the root: it starts the boot
// fiber and otherwise only takes and restores snapshots, on its own stack.
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <unordered_map>
#include <vector>

#include "runtime.h"
#include "savestate.h"
#include "table.h"

// A fresh fiber starts here: rbx = the fiber, r12 = its entry function (initJb below);
// AArch64: x19 and x20.
#if defined(__aarch64__)
extern "C" __attribute__((naked)) void r2FiberTrampoline() {
    asm volatile(
        "mov x0, x19\n\t"
        "mov x29, #0\n\t"  // (the end of the frame chain for unwinders)
        "blr x20\n\t"
        "brk #0\n\t");
}
#elif defined(_WIN32)
extern "C" __attribute__((naked)) void r2FiberTrampoline() {
    asm volatile(
        "movq %rbx, %rcx\n\t"
        "subq $32, %rsp\n\t"
        "callq *%r12\n\t"
        "ud2\n\t");
}
#else
// (System V: the argument in rdi, no shadow space; rsp is 16-byte aligned here)
extern "C" __attribute__((naked)) void r2FiberTrampoline() {
    asm volatile(
        "movq %rbx, %rdi\n\t"
        "callq *%r12\n\t"
        "ud2\n\t");
}
#endif

namespace rt {

extern volatile u32 g_irqLine;

namespace {

// OSContext layout
constexpr u32 kGpr = 0x000, kCr = 0x080, kLr = 0x084, kCtr = 0x088, kXer = 0x08C, kFpscr = 0x194, kSrr0 = 0x198,
              kSrr1 = 0x19C, kState = 0x1A2, kGqr = 0x1A4;
constexpr u16 kStateExc = 2;
constexpr u32 kMsrEE = 0x8000;

struct IrqFrame {
    JmpBuf jb;
    u32 ctxAddr;
    u32 pc;
    Ctx snapshot;
};

struct HostFiber {
    JmpBuf jb;           // where it continues when switched to
    u8* lo = nullptr;    // its stack [lo, hi), committed
    u8* hi = nullptr;
    bool used = false;
    u32 bootEntry = 0;   // the boot fiber: the DOL's entry point (no OSContext)
    Ctx ctx{};
    u32 resumeCtx = 0;   // set by whoever switches to this fiber
    int id = 0;
};

enum class Kind : u32 { Fresh, Running, Saved, Exc };
struct Rec {
    HostFiber* fiber = nullptr;
    Kind kind = Kind::Fresh;
    JmpBuf* jb = nullptr;       // Saved: caller's jump buffer (on that fiber's stack)
    IrqFrame* frame = nullptr;  // Exc: delivering frame
};

// Static storage: recompiled frames hold &fiber->ctx, frames and stacks are at fixed addresses.
constexpr int kMaxFrames = 64;
HostFiber s_fibers[kMaxFibers];
IrqFrame s_framePool[kMaxFrames];
bool s_frameUsed[kMaxFrames];

struct Root {
    JmpBuf jb;
    void *base = nullptr, *limit = nullptr, *dealloc = nullptr;  // the main thread's stack (TEB)
} s_root;
void (*s_rootFn)() = nullptr;  // what the root does when a fiber hands it control

std::unordered_map<u32, Rec> s_recs;
bool s_log = false;
IrqFrame* s_resumeFrame = nullptr;
HostFiber* s_cur = nullptr;
int s_nextId = 0;
u32 s_addrInitContextOrig = 0, s_addrSaveContext = 0;

u32 symAddr(const char* name) {
    for (const SymName* s = g_symNames; s->name; s++)
        if (!strcmp(s->name, name)) return s->addr;
    fatal("symbol %s missing from config/GSWE64/symbols.txt", name);
}

IrqFrame* frameAlloc() {
    for (int i = 0; i < kMaxFrames; i++)
        if (!s_frameUsed[i]) {
            s_frameUsed[i] = true;
            return &s_framePool[i];
        }
    fatal("more than %d exceptions in flight", kMaxFrames);
}
void frameFree(IrqFrame* f) { s_frameUsed[f - s_framePool] = false; }

// --- stacks and switching -------------------------------------------------------------------
// The TEB's stack bounds follow the fiber running (stack probes, exception dispatch).
void setTebStack(void* base, void* limit, void* dealloc) {
#ifdef _WIN32
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    tib->StackBase = base;
    tib->StackLimit = limit;
    *reinterpret_cast<void**>(reinterpret_cast<u8*>(tib) + 0x1478) = dealloc;  // TEB64 DeallocationStack
#else
    (void)base, (void)limit, (void)dealloc;  // (Linux keeps no stack bounds per thread)
#endif
}

// Continues `to` where it was; returns when someone switches back to the caller (whose place
// goes to *fromJb).
void switchStack(JmpBuf* fromJb, HostFiber* to) {
    if (rt_setjmp(fromJb) == 0) {
        setTebStack(to->hi, to->lo, to->lo);
        rt_longjmp(&to->jb, 1);
    }
}
void switchToRoot(JmpBuf* fromJb) {
    if (rt_setjmp(fromJb) == 0) {
        setTebStack(s_root.base, s_root.limit, s_root.dealloc);
        rt_longjmp(&s_root.jb, 1);
    }
}

void allocStack(HostFiber* f) {
    if (f->lo) return;
    const int i = int(f - s_fibers);
    u8* lo = fiberStackRegion() + u64(i) * kFiberStackSize;
#ifdef _WIN32
    if (!VirtualAlloc(lo, kFiberStackSize, MEM_COMMIT, PAGE_READWRITE)) fatal("cannot commit the stack of fiber %d", i);
#else
    if (mprotect(lo, kFiberStackSize, PROT_READ | PROT_WRITE) != 0) fatal("cannot commit the stack of fiber %d", i);
#endif
    f->lo = lo;
    f->hi = lo + kFiberStackSize;
}

void fiberEntry(HostFiber* f);
void initJb(HostFiber* f) {
    memset(&f->jb, 0, sizeof f->jb);
    f->jb.arg() = u64(f);
    f->jb.sp() = (u64(f->hi) - 64) & ~15ull;
    f->jb.entry() = u64(&fiberEntry);
    f->jb.pc() = u64(&r2FiberTrampoline);
}

void loadAll(Ctx& c, u32 ctx) {
    for (int i = 0; i < 32; i++) c.r[i] = rd32(ctx + kGpr + 4 * i);
    c.mtcrf(0xFF, rd32(ctx + kCr));
    c.lr = rd32(ctx + kLr);
    c.ctr = rd32(ctx + kCtr);
    c.setxer(rd32(ctx + kXer));
    for (int i = 1; i < 8; i++) c.gqr[i] = rd32(ctx + kGqr + 4 * i);
    c.msr = rd32(ctx + kSrr1);
}

[[noreturn]] void resumeLocal(u32 ctx);

void fiberEntry(HostFiber* f) {
    Ctx& c = f->ctx;
    if (f->bootEntry) {
        dispatch(c, f->bootEntry);
        logf("boot", "the game returned from its entry point");
        quit(0);
    }
    const u32 ctx = f->resumeCtx;
    loadAll(c, ctx);
    c.fpscr = rd32(ctx + kFpscr);
    c.fiber = f;
    s_recs[ctx] = Rec{f, Kind::Running, nullptr, nullptr};
    const u32 pc = rd32(ctx + kSrr0);
    logf("thread", "fiber %d starts context %08X at %s (sp=%08X)", f->id, ctx, describeAddr(pc).c_str(), c.r[1]);
    dispatch(c, pc);
    // The entry returned: continue at LR (OSExitThread), as blr would.
    dispatch(c, c.lr);
    fatal("thread context %08X ran past its exit (lr=%08X)", ctx, c.lr);
}

HostFiber* newFiber() {
    for (u32 i = 1; i < kMaxFibers; i++) {
        HostFiber* f = &s_fibers[i];
        if (f->used) continue;
        allocStack(f);
        f->used = true;
        f->bootEntry = 0;
        f->ctx = Ctx{};
        f->ctx.fiber = f;
        f->resumeCtx = 0;
        f->id = ++s_nextId;
        initJb(f);
        return f;
    }
    fatal("more than %u guest threads", kMaxFibers);
}

// Switch to fiber f and ask it to resume context ctx. Returns only by jumping elsewhere.
[[noreturn]] void switchTo(HostFiber* f, u32 ctx) {
    HostFiber* me = s_cur;
    f->resumeCtx = ctx;
    s_cur = f;
    switchStack(&me->jb, f);
    // Someone switched back to us: resume the context they asked for.
    s_cur = me;
    resumeLocal(me->resumeCtx);
}

[[noreturn]] void resumeLocal(u32 ctx) {
    auto it = s_recs.find(ctx);
    if (it == s_recs.end() || it->second.fiber != s_cur)
        fatal("resume of context %08X on fiber %d, which does not own it", ctx, s_cur->id);
    Rec& r = it->second;
    switch (r.kind) {
    case Kind::Exc:
        s_resumeFrame = r.frame;
        rt_longjmp(&r.frame->jb, 1);
    case Kind::Saved:
        if (!r.jb) fatal("context %08X was saved without a resume point", ctx);
        s_cur->ctx.resumeCtxAddr = ctx;
        rt_longjmp(r.jb, 1);
    default:
        fatal("context %08X cannot be resumed (state %d)", ctx, int(r.kind));
    }
}

}  // namespace

Ctx& curCtx() { return s_cur->ctx; }

// The running guest thread's calls (fatal errors): LR, then the return addresses its stack's
// back chain holds (each frame's caller saved its LR at back chain + 4).
std::string guestBacktrace() {
    if (!s_cur) return {};
    const Ctx& c = s_cur->ctx;
    std::string out = "guest calls (fiber " + std::to_string(s_cur->id) + ", newest first): " + describeAddr(c.lr);
    u32 sp = c.r[1];
    for (int i = 0; i < 24 && sp; i++) {
        const u32 next = peekGuest32(sp);
        if (!next || next <= sp || next - sp > 0x100000) break;
        out += " < " + describeAddr(peekGuest32(next + 4));
        sp = next;
    }
    return out + "\n";
}

void threadsInit(Ctx*& boot) {
#ifdef _WIN32
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    s_root.base = tib->StackBase;
    s_root.limit = tib->StackLimit;
    s_root.dealloc = *reinterpret_cast<void**>(reinterpret_cast<u8*>(tib) + 0x1478);
#endif
    HostFiber* f = &s_fibers[0];
    allocStack(f);
    f->used = true;
    f->ctx.fiber = f;
    s_cur = f;
    boot = &f->ctx;
    s_log = envFlag("R2_IRQ_LOG");
    s_addrInitContextOrig = symAddr("OSInitContext");
    s_addrSaveContext = symAddr("OSSaveContext");
}

// The main thread from here on: the boot fiber runs the game; whenever a fiber hands control
// back (threadsYieldToRoot) the root runs what it asked for and continues the current fiber,
// which after a state load is the one the state was saved on.
void threadsRun(u32 entry) {
    HostFiber* f = &s_fibers[0];
    f->bootEntry = entry;
    initJb(f);
    for (;;) {
        switchStack(&s_root.jb, s_cur);
        if (void (*fn)() = s_rootFn) {
            s_rootFn = nullptr;
            fn();
        }
    }
}

void threadsYieldToRoot(void (*fn)()) {
    s_rootFn = fn;
    switchToRoot(&s_cur->jb);
}

// --- HLE: the OS context primitives -----------------------------------------------------
void hle_OSSaveContext(Ctx& c) {
    const u32 ctx = c.r[3];
    GuestFn orig = lookupOrig(s_addrSaveContext);
    orig(c);  // writes the guest context exactly as the original does; r3 = 0
    s_recs[ctx] = Rec{s_cur, Kind::Saved, static_cast<JmpBuf*>(c.pendingJmp), nullptr};
    if (s_log) logf("ctx", "save %08X on fiber %d (jb %p, lr %08X)", ctx, s_cur->id, c.pendingJmp, c.lr);
    c.pendingJmp = nullptr;
}

void ctxResumed(Ctx& c) {
    // OSLoadContext of a context saved by OSSaveContext: registers come back from it
    // (r3 = 1, stored by OSSaveContext), FP state stays (lazy FPU switching).
    const u32 ctx = c.resumeCtxAddr;
    c.r[0] = rd32(ctx + kGpr);
    c.r[1] = rd32(ctx + kGpr + 4);
    c.r[2] = rd32(ctx + kGpr + 8);
    for (int i = 13; i < 32; i++) c.r[i] = rd32(ctx + kGpr + 4 * i);
    c.r[3] = rd32(ctx + kGpr + 12);
    c.r[4] = rd32(ctx + kGpr + 16);
    c.mtcrf(0xFF, rd32(ctx + kCr));
    c.lr = rd32(ctx + kLr);
    c.ctr = rd32(ctx + kCtr);
    c.setxer(rd32(ctx + kXer));
    for (int i = 1; i < 8; i++) c.gqr[i] = rd32(ctx + kGqr + 4 * i);
    c.msr = rd32(ctx + kSrr1);
    s_recs[ctx].kind = Kind::Running;
    if ((c.msr & kMsrEE) && g_irqLine) g_irqPending = 1;
}

void hle_OSLoadContext(Ctx& c) {
    const u32 ctx = c.r[3];
    const u16 st = u16(rd16(ctx + kState));
    if (st & kStateExc) wr16(ctx + kState, st & ~kStateExc);
    auto it = s_recs.find(ctx);
    if (s_log)
        logf("ctx", "load %08X from fiber %d: %s", ctx, s_cur->id,
             it == s_recs.end() ? "unknown" : it->second.kind == Kind::Fresh ? "fresh" : it->second.kind == Kind::Saved ? "saved" : it->second.kind == Kind::Exc ? "exception" : "running");
    if (it == s_recs.end() || it->second.kind == Kind::Fresh) {
        HostFiber* f = newFiber();
        switchTo(f, ctx);
    }
    if (it->second.fiber == s_cur) resumeLocal(ctx);
    switchTo(it->second.fiber, ctx);
}

void hle_OSInitContext(Ctx& c) {
    const u32 ctx = c.r[3];
    lookupOrig(s_addrInitContextOrig)(c);
    s_recs[ctx] = Rec{nullptr, Kind::Fresh, nullptr, nullptr};
}

// --- exceptions -----------------------------------------------------------------------
void deliverException(Ctx& c, u32 exc, u32 pc) {
    const u32 ctx = rd32(0x800000D4);  // OSCurrentContext
    if (!ctx) fatal("exception %u with no current context", exc);
    for (int i = 0; i < 32; i++) wr32(ctx + kGpr + 4 * i, c.r[i]);
    wr32(ctx + kCr, c.getcr());
    wr32(ctx + kLr, c.lr);
    wr32(ctx + kCtr, c.ctr);
    wr32(ctx + kXer, c.getxer());
    wr32(ctx + kSrr0, pc);
    wr32(ctx + kSrr1, c.msr);
    for (int i = 1; i < 8; i++) wr32(ctx + kGqr + 4 * i, c.gqr[i]);
    wr16(ctx + kState, rd16(ctx + kState) | kStateExc);

    if (s_log) logf("irq", "exception %u at %08X, context %08X, handler %08X, INTSR&MASK pending=%d", exc, pc, ctx, rd32(0x80003000 + exc * 4), int(piPending()));
    IrqFrame* fr = frameAlloc();
    fr->ctxAddr = ctx;
    fr->pc = pc;
    fr->snapshot = c;
    s_recs[ctx] = Rec{s_cur, Kind::Exc, nullptr, fr};
    if (rt_setjmp(&fr->jb) == 0) {
        const u32 handler = rd32(0x80003000 + exc * 4);
        if (!handler) fatal("exception %u at %08X with no handler installed", exc, pc);
        c.srr0 = pc;
        c.srr1 = c.msr;
        c.msr &= ~kMsrEE;
        c.r[3] = exc;
        c.r[4] = ctx;
        c.lr = 0;
        dispatch(c, handler);
        logf("irq", "exception %u handler %08X returned instead of loading a context", exc, handler);
    }
    // Back from the exception (OSLoadContext of ctx, or the handler returned).
    HostFiber* me = s_cur;
    if (s_resumeFrame) {
        fr = s_resumeFrame;
        s_resumeFrame = nullptr;
    }
    if (s_log) logf("irq", "return from exception to %08X on fiber %d", fr->pc, me->id);
    Ctx& cc = me->ctx;
    void* pend = cc.pendingJmp;
    cc = fr->snapshot;
    cc.pendingJmp = pend;
    auto it = s_recs.find(fr->ctxAddr);
    if (it != s_recs.end() && it->second.frame == fr) it->second = Rec{me, Kind::Running, nullptr, nullptr};
    frameFree(fr);
}

u64 g_safepoints = 0, g_exceptions = 0;  // R2_GX_LOG statistics

void safepoint(Ctx& c, u32 pc) {
    g_safepoints++;
    // a save state request: the snapshot is taken here, with only recompiled frames (and the
    // runtime's own exception / page fault frames) below
    if (state::g_pending) [[unlikely]] state::service(c);
    g_irqPending = 0;
    runDueEvents();
    if (!(c.msr & kMsrEE)) return;
    if (piPending()) {
        deliverException(c, 4, pc);
    } else if (decPending()) {
        decAck();
        deliverException(c, 8, pc);
    }
    // audio blocks that are due start here at once, each with its interrupt, instead of waiting
    // for the timer thread (hw.cpp onAi: catch-up after a V-Sync wait or a host stall; ZeDctr's
    // Red Two Plus fork)
    for (int n = 0; n < 24 && (c.msr & kMsrEE) && aiCatchUpPending(); n++) {
        if (piPending()) {
            deliverException(c, 4, pc);
            continue;
        }
        if (!aiCatchUpStep()) break;
        if (piPending()) deliverException(c, 4, pc);
    }
    // more to take once the handler is done (it runs with interrupts off)
    if (g_irqLine && (c.msr & kMsrEE)) g_irqPending = 1;
}

// --- save states ----------------------------------------------------------------------------
// Runs on the root (no fiber is running): each fiber's place (jump buffer) and the part of its
// stack in use, the guest contexts' records and the exception frames in flight.
void threadsSaveState(state::Writer& w) {
    w.section("THRD");
    w.pod(s_nextId);
    w.pod(s32(s_cur - s_fibers));
    for (const HostFiber& f : s_fibers) {
        w.pod(f.used);
        if (!f.used) continue;
        w.pod(f.jb);
        w.pod(f.bootEntry);
        w.pod(f.ctx);
        w.pod(f.resumeCtx);
        w.pod(f.id);
        const u8* sp = reinterpret_cast<const u8*>(f.jb.sp());
        if (sp < f.lo || sp > f.hi) fatal("fiber %d is not on its stack (sp %p)", f.id, sp);
        w.pod(u64(f.hi - sp));
        w.bytes(sp, size_t(f.hi - sp));
    }
    w.pod(s_frameUsed);
    for (int i = 0; i < kMaxFrames; i++)
        if (s_frameUsed[i]) w.pod(s_framePool[i]);
    w.pod(u64(s_recs.size()));
    for (const auto& [ctx, r] : s_recs) {
        w.pod(ctx);
        w.pod(s32(r.fiber ? r.fiber - s_fibers : -1));
        w.pod(r.kind);
        w.pod(u64(r.jb));
        w.pod(s32(r.frame ? r.frame - s_framePool : -1));
    }
}

void threadsLoadState(state::Reader& r) {
    r.section("THRD");
    r.pod(s_nextId);
    const s32 cur = r.get<s32>();
    for (HostFiber& f : s_fibers) {
        r.pod(f.used);
        if (!f.used) continue;
        allocStack(&f);
        r.pod(f.jb);
        r.pod(f.bootEntry);
        r.pod(f.ctx);
        r.pod(f.resumeCtx);
        r.pod(f.id);
        const u64 n = r.get<u64>();
        if (n > kFiberStackSize) fatal("save state: fiber stack of %llu bytes", (unsigned long long)n);
        r.bytes(f.hi - n, size_t(n));
    }
    r.pod(s_frameUsed);
    for (int i = 0; i < kMaxFrames; i++)
        if (s_frameUsed[i]) r.pod(s_framePool[i]);
    s_recs.clear();
    const u64 n = r.get<u64>();
    for (u64 i = 0; i < n && r.ok; i++) {
        const u32 ctx = r.get<u32>();
        const s32 fiber = r.get<s32>();
        Rec rec;
        rec.fiber = fiber >= 0 && fiber < s32(kMaxFibers) ? &s_fibers[fiber] : nullptr;
        r.pod(rec.kind);
        rec.jb = reinterpret_cast<JmpBuf*>(r.get<u64>());
        const s32 frame = r.get<s32>();
        rec.frame = frame >= 0 && frame < kMaxFrames ? &s_framePool[frame] : nullptr;
        s_recs[ctx] = rec;
    }
    s_resumeFrame = nullptr;
    if (cur < 0 || cur >= s32(kMaxFibers) || !s_fibers[cur].used) fatal("save state: no current fiber");
    s_cur = &s_fibers[cur];
}

}  // namespace rt
