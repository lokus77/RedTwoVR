// Renders the game's GX command stream with Aurora.
//
// Aurora decodes raw BP/CP/XF registers and draws itself, but it takes memory references
// (vertex arrays, textures, palettes, EFB copy destinations) as host pointers through its own
// GX_AURORA sub-commands, as a source port's GX calls would send them. The game's stream has
// the hardware form instead: physical addresses in CP 0xA0-0xAF, BP TX_SETIMAGE3, the TLUT
// load registers and the copy registers. This bridge forwards the stream and translates those:
//   * vertex arrays: sized from the indices the draws actually use (Aurora uploads whole arrays)
//   * textures: one LOAD_TEXOBJ per changed texture map before a draw, from the raw registers
//     (Aurora caches textures by content hash, so memory the game rewrites shows up)
//   * palettes: TMEM TLUT loads are remembered and bound to Aurora TLUT slots at draw time
//   * EFB copies: source rect, destination size/format/address; display copies end the frame
// Display lists are inlined by the CP model (hw/gx_fifo.cpp), so CALL_DL never arrives here.
#include "gx_bridge.h"
#include "gfx/xr.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx/GXStruct.h>
#include <aurora/texture.hpp>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <xxhash.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>

#include "../../../extern/aurora/lib/gx/fifo.hpp"

#ifdef _WIN32
// user32 SetWindowPos without pulling <windows.h> into this file (its macros clash with GX names).
extern "C" __declspec(dllimport) int __stdcall SetWindowPos(void*, void*, int, int, int, int, unsigned);
extern "C" __declspec(dllimport) long long __stdcall GetWindowLongPtrW(void*, int);
extern "C" __declspec(dllimport) long long __stdcall SetWindowLongPtrW(void*, int, long long);
extern "C" __declspec(dllimport) int __stdcall GetSystemMetrics(int);
extern "C" __declspec(dllimport) int __stdcall ShowWindow(void*, int);
#endif
#include "../achievements.h"
#include "../discord.h"
#include "../hw/gx_fifo.h"
#include "../hw/musyx_dsp.h"
#include "../hotkeys.h"
#include "../mods.h"
#include "tex_dump.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../settings.h"

namespace rt::gfx {
namespace fifo = aurora::gx::fifo;

namespace {

bool s_enabled = false;
bool s_inFrame = false;
u64 s_lastFrameTb = 0;
u64 s_frames = 0;
u64 s_vblanks = 0;    // retraces seen (R2_GX_SKIP windows)
u32 s_frameDraws = 0;  // draws so far in this presented frame (R2_GX_SKIP i<from>-<to>)
bool s_topCopied = false;  // this frame already has a display copy at the XFB's first line

// --- raw state shadowed from the stream ---
u32 s_bpMask = 0xFFFFFF;
u32 s_bp[256] = {};
u32 s_vcdLo = 0, s_vcdHi = 0, s_vatA[8] = {}, s_vatB[8] = {}, s_vatC[8] = {};
u32 s_cpMtxA = 0;         // CP 0x30: position matrix index (draws without per-vertex indices)
float s_xfMtx[256] = {};  // XF 0x000-0x0FF: position/texture matrices (where sprites are, drawExtentX)
// Everything the stream set, for a save state: after a load it is replayed (bridgeAfterLoad)
std::bitset<256> s_bpValid;   // BP registers written so far
u32 s_cp[256] = {};           // CP registers as written
std::bitset<256> s_cpValid;
constexpr u32 kXfShadow = 0x1058;  // XF memory (matrices, lights) and registers
u32 s_xfMem[kXfShadow] = {};
u16 s_xfBlock[kXfShadow] = {};     // XF loads by start address: their length (Aurora takes blocks)

struct Array {
    u32 base = 0;       // physical
    u32 stride = 0;
    u32 sentSize = 0;   // size Aurora has; 0 = not sent since the base changed
};
Array s_arrays[16];

struct TexMap {
    bool dirty = true;
    u64 frame = ~0ull;  // frame the map was last sent
};
TexMap s_tex[8];

struct TlutLoad {
    u32 addr = 0, entries = 0;
};
std::unordered_map<u32, TlutLoad> s_tlutLoads;  // TMEM offset -> last load
struct TlutSlot {
    u32 tmem = ~0u, fmt = 0, addr = 0, entries = 0, id = 0;
};
TlutSlot s_tlutSlots[16];
u32 s_tlutNext = 0;
std::unordered_map<u64, u32> s_tlutIds;  // content key -> Aurora TLUT object id
u32 s_nextTlutId = 1;

inline u32 be16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }
inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

void aur(u16 sub) {
    fifo::write_u8(GX_AURORA);
    fifo::write_u16(sub);
}

// ---------------------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------------------
u64 s_lastPollTb = 0;

void pollEvents() {
    s_lastPollTb = tbNow();
    const AuroraEvent* e = aurora_update();
    for (; e && e->type != AURORA_NONE; ++e) {
        if (e->type == AURORA_EXIT) {
            logf("gfx", "window closed");
            quitToDesktop();
        }
        if (e->type == AURORA_SDL_EVENT) {
            menuEvent(&e->sdl);
            touchEvent(&e->sdl);
            audio::deviceEvent(&e->sdl);
        }
        // F11 or Alt+Enter: toggle fullscreen
        if (e->type == AURORA_SDL_EVENT && e->sdl.type == SDL_EVENT_KEY_DOWN && !e->sdl.key.repeat &&
            (e->sdl.key.key == SDLK_F11 || (e->sdl.key.key == SDLK_RETURN && (e->sdl.key.mod & SDL_KMOD_ALT)))) {
            if (SDL_Window* w = aurora_get_window()) {
                const bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(w, !full);
                settings::get().fullscreen = !full;
                settings::save();
            }
        }
    }
}

// Every pipeline in the cache - each one the game has drawn with on this PC - is built before
// the game starts (the player's choice: nothing seen before pops in while its shader compiles;
// Aurora skips a draw until its pipeline exists). Aurora queues them all when it initializes
// (rebuild_pipeline_cache, in the order the game first used them); with Dawn's blob cache that
// takes ~2 s, after a graphics driver update each one compiles again (~25 ms on Aurora's one
// worker: ~55 s for 2,100; patch 0020's 8 workers: ~10 s). The texture packs' prefetch (mods.cpp,
// setting prefetch_textures) runs meanwhile and is waited for too. The setting
// precompile_shaders off (Display page) or R2_NO_PRECOMPILE=1 starts the game at once: the
// queue is still built, on the workers while the game runs, and a draw whose pipeline is not
// built yet is skipped (or drawn by the ubershader, if that is on) until it is.
void precompileShaders() {
    const AuroraStats* st = aurora_get_stats();
    if (!st) return;
    if (envFlag("R2_NO_PRECOMPILE") || !settings::get().precompileShaders) {
        logf("gfx", "shaders not precompiled (setting): %u from the cache compile while the game runs",
             st->createdPipelines + st->queuedPipelines);
        return;
    }
    const u64 t0 = tbNow();
    u32 total = 0;
    bool waited = false;
    for (;;) {
        const u32 queued = st->queuedPipelines;
        const auto tex = aurora::texture::prefetch_status();
        if (queued == 0 && tex.done >= tex.total) break;
        waited = true;
        const u32 done = st->createdPipelines;
        total = std::max(total, done + queued);
        diagUiFrame();  // (a freeze here is reported in the log like one in the game)
        pollEvents();
        if (aurora_begin_frame()) {
            drawShaderProgress(done, total, tex.done, tex.total, tex.bytes);
            aurora_end_frame();
        }
        SDL_Delay(16);
    }
    if (!waited) return;
    const auto tex = aurora::texture::prefetch_status();
    logf("gfx", "before the game started (%.1f s): %u pipelines built, %llu textures prefetched (%.1f GiB%s)",
         double(tbNow() - t0) / double(kTbClock), st->createdPipelines, (unsigned long long)tex.total,
         double(tex.bytes) / double(1ull << 30), tex.budgetFull ? ", memory budget reached" : "");
}

// The launcher: the settings menu (menu.cpp, in a launcher mode) before the game boots, so
// everything - display, texture packs, shader preparation, controls, achievements - can be set
// before anything of the game runs. On by default (settings::launcher, the first page's "Show
// launcher at start"); R2_LAUNCHER=1/0 overrides (a restart sets it: a reset goes straight into
// the game, "Back to launcher" here), then automated runs skip it, then --launcher /
// --no-launcher, then the setting. Aurora is up meanwhile: its workers build the cached
// pipelines and prefetch textures while the player looks at the settings.
bool launcherWanted(int argc, char** argv) {
    if (const char* e = getenv("R2_LAUNCHER"); e && *e) return *e == '1';
    if (envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT") || envFlag("R2_MENU")) return false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--launcher")) return true;
        if (!strcmp(argv[i], "--no-launcher")) return false;
    }
    return settings::get().launcher;
}

void runLauncher() {
    logf("gfx", "launcher open");
    const u64 t0 = tbNow();
    discord::setLauncher(true);  // Rich Presence: "In the launcher"
    launcherOpen();
    for (;;) {
        diagUiFrame();  // (a freeze here is reported in the log like one in the game)
        // test: R2_LAUNCHER_FREEZE_TEST=<s> freezes the launcher once, 12 s, that long after it opened
        if (static double freezeAt = getenv("R2_LAUNCHER_FREEZE_TEST") ? atof(getenv("R2_LAUNCHER_FREEZE_TEST")) : -1.0;
            freezeAt >= 0 && double(tbNow() - t0) / double(kTbClock) >= freezeAt) {
            freezeAt = -1.0;
            logf("gfx", "launcher freeze test: 12 s");
            SDL_Delay(12000);
        }
        pollEvents();  // (closing the window quits)
        ra::idle();    // a login from the ACHIEVEMENTS page
        bool start = false;
        if (aurora_begin_frame()) {
            start = launcherFrame();
            aurora_end_frame();
        }
        if (start) break;
        SDL_Delay(8);
    }
    discord::setLauncher(false);
    logf("gfx", "launcher: game started after %.1f s; settings: %s", double(tbNow() - t0) / double(kTbClock),
         settings::summary().c_str());
}

void testQuad();
bool s_testClear = false;  // R2_GFX_TEST: clear each frame to magenta (is presentation working?)
bool s_xfb = true;         // R2_NO_XFB: present the EFB instead of the emulated XFB
bool s_copyLog = false;  // R2_COPY_LOG: every display copy and where it lands

// Aurora renders on its own thread (GX_PROCESSING_THREAD); gpSync waits until it has caught
// up with every command sent. The game sees the GPU's progress only through draw done, draw
// sync tokens and the CP's status and pointers: syncing there keeps memory the game reuses
// afterwards (vertex arrays, textures) from being read late. R2_GX_THREAD=0 renders inline.
bool s_gxThread = false;
void gpSync() {
    if (s_gxThread && s_inFrame) fifo::drain();
}

// VR stereo: each frame's eye views (gfx/xr.cpp), before its first draw
bool s_stereoSent = false;
void sendStereoViews() {
    if (!xr::stereo()) return;
    float views[2][16];
    const bool on = xr::frameViews(views);
    if (!on && !s_stereoSent) return;
    s_stereoSent = on;
    aur(GX_AURORA_SET_STEREO_VIEWS);
    fifo::write_u8(on ? 1 : 0);
    if (on)
        for (auto& eye : views)
            for (float v : eye) fifo::write_f32(v);
}

void ensureFrame();

}  // namespace

void resendStereoViews() {
    if (s_inFrame) sendStereoViews();
}

namespace {

void ensureFrame() {
    if (s_inFrame) return;
    pollEvents();
    s_inFrame = aurora_begin_frame();
    if (s_inFrame) sendStereoViews();
    if (s_inFrame && s_testClear) {
        aur(GX_AURORA_CLEAR);
        fifo::write_u8(0x0F);  // whole target, color, alpha, depth
        for (int i = 0; i < 4; i++) fifo::write_u32(0);
        fifo::write_u8(255); fifo::write_u8(0); fifo::write_u8(255); fifo::write_u8(255);
        fifo::write_u32(0xFFFFFF);
        testQuad();
    }
}

// A green quad drawn purely with raw hardware registers, as the game's own GX code would.
void testQuad() {
    auto bp = [](u32 v) { fifo::write_u8(0x61); fifo::write_u32(v); };
    auto cp = [](u8 r, u32 v) { fifo::write_u8(0x08); fifo::write_u8(r); fifo::write_u32(v); };
    auto xf = [](u16 a, std::initializer_list<u32> vals) {
        fifo::write_u8(0x10);
        fifo::write_u32((u32(vals.size() - 1) << 16) | a);
        for (u32 v : vals) fifo::write_u32(v);
    };
    auto f = [](float x) { u32 u; memcpy(&u, &x, 4); return u; };
    cp(0x50, 0x2200);      // POS, CLR0 direct
    cp(0x60, 0);
    cp(0x70, 0x40016009);  // POS XYZ f32, CLR0 RGBA8
    xf(0x1008, {1});       // one color
    xf(0x1009, {1});
    xf(0x100E, {1});       // color0: vertex color, no lighting
    xf(0x1010, {1});
    xf(0x103F, {0});
    bp(0x00000010);        // genmode: 1 color, 0 texgens, 1 stage, no culling
    bp(0xC008FFFA);        // TEV color = RASC
    bp(0xC108FFD0);        // TEV alpha = RASA
    bp(0x2800003F);        // stage 0: no texture, color0
    bp(0x41000018);        // color+alpha update, no blending
    bp(0x40000000);        // no Z
    bp(0xF33F0000);        // alpha test always
    xf(0x0000, {f(1), 0, 0, 0, 0, f(1), 0, 0, 0, 0, f(1), 0});
    cp(0x30, 0);
    xf(0x1018, {0});
    xf(0x101A, {f(320), f(-240), f(16777215), f(660), f(580), f(16777215)});
    xf(0x1020, {f(2.f / 640), f(-1), f(-2.f / 480), f(1), 0, f(-0.5f), 1});
    bp(0x20154154);
    bp(0x213D3333);
    bp(0x590D5B56);  // scissor offset 340 in Aurora's (negated) convention
    fifo::write_u8(0x80);  // quads, format 0
    fifo::write_u16(4);
    const float vtx[4][2] = {{100, 100}, {540, 100}, {540, 380}, {100, 380}};
    for (auto& v : vtx) {
        fifo::write_u32(f(v[0]));
        fifo::write_u32(f(v[1]));
        fifo::write_u32(0);
        fifo::write_u32(0x00FF00FF);
    }
    fifo::finish_draw();
}

// R2_FRAME_LOG: every 10 s, presented frame intervals against the retrace period (how many
// overran it) and the time Aurora's end of frame (submit, present) took.
void frameLog(u64 endStart, u64 endDone) {
    // R2_FRAME_LOG=1: every 10 s; =<n> (2 or more): every n seconds
    static const int secs = getenv("R2_FRAME_LOG") ? std::max(1, atoi(getenv("R2_FRAME_LOG"))) : 0;
    if (!secs) return;
    static u64 last = 0, windowStart = 0, frames = 0, over = 0, maxInterval = 0, endSum = 0, endMax = 0, drawsSum = 0,
               drawsMax = 0;
    // Aurora's per-frame buffers, largest frame (its stats describe the last frame it finished)
    static u32 vertMax = 0, idxMax = 0, uniMax = 0, storMax = 0;
    const u64 period = u64(double(kTbClock) / viRate());
    if (last) {
        const u64 interval = endDone - last;
        if (interval > period + period / 2) over++;
        maxInterval = std::max(maxInterval, interval);
    }
    last = endDone;
    if (!windowStart) windowStart = endDone;
    frames++;
    endSum += endDone - endStart;
    endMax = std::max(endMax, endDone - endStart);
    drawsSum += s_frameDraws;
    drawsMax = std::max<u64>(drawsMax, s_frameDraws);
    if (const AuroraStats* st = aurora_get_stats()) {
        vertMax = std::max(vertMax, st->lastVertSize);
        idxMax = std::max(idxMax, st->lastIndexSize);
        uniMax = std::max(uniMax, st->lastUniformSize);
        storMax = std::max(storMax, st->lastStorageSize);
    }
    if (endDone - windowStart >= u64(secs == 1 ? 10 : secs) * kTbClock) {
        auto ms = [](u64 tb) { return double(tb) * 1000.0 / double(kTbClock); };
        logf("frame", "%llu frames (%.1f fps), %llu over 1.5 periods, longest %.2f ms; end of frame avg %.2f max %.2f ms; "
             "%llu draws/frame (max %llu); largest frame: verts %u KiB, indices %u KiB, uniforms %u KiB, storage %u KiB",
             (unsigned long long)frames, double(frames) * double(kTbClock) / double(endDone - windowStart),
             (unsigned long long)over, ms(maxInterval), ms(endSum) / double(frames), ms(endMax),
             (unsigned long long)(drawsSum / frames), (unsigned long long)drawsMax, vertMax >> 10, idxMax >> 10,
             uniMax >> 10, storMax >> 10);
        windowStart = endDone;
        frames = over = maxInterval = endSum = endMax = drawsSum = drawsMax = 0;
        vertMax = idxMax = uniMax = storMax = 0;
    }
}

void tileFrameEnd();

void endFrame() {
    if (!s_inFrame) return;
    menuDraw();
    touchDraw();
    const u64 endStart = tbNow();
    mods::textureFrame(false);  // (Aurora's replacement counters reset at the end of a frame)
    aurora_end_frame();
    mods::textureFrame(true);
    const u64 endDone = tbNow();
    frameLog(endStart, endDone);
    diagFramePresented(s_frameDraws, endDone - endStart);  // session log: perf lines, stall reports (profiler.cpp)
    // test: R2_VSYNC_TOGGLE_TEST=<s> flips V-Sync once that many seconds in, as the menu does
    static const u64 firstFrameTb = endDone;
    if (static double flipAt = getenv("R2_VSYNC_TOGGLE_TEST") ? atof(getenv("R2_VSYNC_TOGGLE_TEST")) : -1.0;
        flipAt >= 0 && double(endDone - firstFrameTb) / double(kTbClock) >= flipAt) {
        flipAt = -1.0;
        settings::get().vsync = !settings::get().vsync;
        logf("gfx", "V-Sync toggle test: %s", settings::get().vsync ? "on" : "off");
        applyDisplaySettings();
    }
    tileFrameEnd();
    s_inFrame = false;
    s_frames++;
    s_frameDraws = 0;
    s_topCopied = false;
    s_lastFrameTb = tbNow();
}

// ---------------------------------------------------------------------------------------
// vertex arrays
// ---------------------------------------------------------------------------------------
u64 s_arraySends = 0, s_arrayBytes = 0;  // statistics (stats())

void sendArray(u32 n, u32 need) {
    Array& a = s_arrays[n];
    if (!a.base) return;
    if (a.sentSize && need <= a.sentSize) return;
    u32 size = a.sentSize ? a.sentSize : 256;
    while (size < need) size *= 2;
    const u32 maxSize = kMem1Mapped - (a.base & 0x01FFFFFF);
    if (size > maxSize) size = maxSize;
    u8* host = physPtr(a.base, size);
    if (!host) return;
    aur(u16(GX_AURORA_LOAD_ARRAYBASE | n));
    fifo::write_u64(reinterpret_cast<u64>(host));
    fifo::write_u32(size);
    fifo::write_u8(0);  // big-endian data
    a.sentSize = size;
    s_arraySends++;
    s_arrayBytes += size;
}

u32 compSize(u32 fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : fmt == 4 ? 4 : 0; }
u32 colSize(u32 fmt) {
    static const u32 k[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return k[fmt & 7];
}

// Walk the vertices of a draw: grow each indexed array to cover the largest index used.
void prepareArrays(const u8* p) {
    const u32 fmt = p[0] & 7;
    const u32 count = be16(p + 1);
    const u32 lo = s_vcdLo, hi = s_vcdHi, A = s_vatA[fmt], B = s_vatB[fmt], C = s_vatC[fmt];
    struct Ref {
        u32 array, offset, bytes, count;  // count: indices per vertex for this attribute
    } refs[12];
    u32 nrefs = 0, off = 0;
    for (int i = 0; i < 9; i++)
        if (lo & (1u << i)) off += 1;
    auto attr = [&](u32 type, u32 array, u32 directBytes, u32 idxPerVtx) {
        if (type == 1) off += directBytes;
        else if (type >= 2) {
            const u32 b = type == 2 ? 1 : 2;
            refs[nrefs++] = {array, off, b, idxPerVtx};
            off += b * idxPerVtx;
        }
    };
    attr((lo >> 9) & 3, 0, compSize((A >> 1) & 7) * ((A & 1) ? 3 : 2), 1);
    const bool nbt = (A >> 9) & 1;
    attr((lo >> 11) & 3, 1, compSize((A >> 10) & 7) * (nbt ? 9 : 3), (nbt && (A >> 31)) ? 3 : 1);
    attr((lo >> 13) & 3, 2, colSize((A >> 14) & 7), 1);
    attr((lo >> 15) & 3, 3, colSize((A >> 18) & 7), 1);
    const u32 texCnt[8] = {(A >> 21) & 1, B & 1, (B >> 9) & 1, (B >> 18) & 1, (B >> 27) & 1, (C >> 5) & 1, (C >> 14) & 1, (C >> 23) & 1};
    const u32 texFmt[8] = {(A >> 22) & 7, (B >> 1) & 7, (B >> 10) & 7, (B >> 19) & 7, (B >> 28) & 7, (C >> 6) & 7, (C >> 15) & 7, (C >> 24) & 7};
    for (u32 t = 0; t < 8; t++) attr((hi >> (2 * t)) & 3, 4 + t, compSize(texFmt[t]) * (texCnt[t] ? 2 : 1), 1);
    // 8-bit indices (the common case) cannot pass 255: size those arrays for 256 entries at
    // once instead of scanning every draw's indices. 16-bit ones are scanned unless the array
    // already went up big enough for any index.
    u32 n = 0;
    for (u32 r = 0; r < nrefs; r++) {
        const Array& a = s_arrays[refs[r].array];
        const u32 stride = a.stride ? a.stride : 16;
        if (a.sentSize && a.sentSize >= (refs[r].bytes == 1 ? 256u : 65536u) * stride + 16) continue;
        refs[n++] = refs[r];
    }
    nrefs = n;
    if (!nrefs) return;
    // one tight loop per index (attribute outer, vertices inner: no per-vertex branching)
    const u32 vsize = off;
    for (u32 r = 0; r < nrefs; r++) {
        u32 maxIdx = 0;
        for (u32 k = 0; k < refs[r].count; k++) {
            const u8* q = p + 3 + refs[r].offset + k * refs[r].bytes;
            if (refs[r].bytes == 1) {
                for (u32 i = 0; i < count; i++, q += vsize) maxIdx = std::max<u32>(maxIdx, q[0]);
            } else {
                for (u32 i = 0; i < count; i++, q += vsize) maxIdx = std::max<u32>(maxIdx, u32(q[0]) << 8 | q[1]);
            }
        }
        const Array& a = s_arrays[refs[r].array];
        const u32 stride = a.stride ? a.stride : 16;
        sendArray(refs[r].array, (maxIdx + 1) * stride + 16);
    }
}

// ---------------------------------------------------------------------------------------
// textures and palettes
// ---------------------------------------------------------------------------------------
u32 texRegBase(u32 map) { return map < 4 ? 0x80 + map : 0xA0 + (map - 4); }
bool isPalette(u32 fmt) { return fmt == 8 || fmt == 9 || fmt == 10; }

u64 hashBytes(const u8* p, u32 n) {
    u64 h = 1469598103934665603ull;  // FNV-1a (palettes are at most 512 bytes)
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Bind the palette the texture names (TX_SETTLUT: TMEM offset + format) to an Aurora slot.
u32 bindTlut(u32 tlutReg) {
    const u32 tmem = tlutReg & 0x3FF, fmt = (tlutReg >> 10) & 3;
    auto it = s_tlutLoads.find(tmem);
    if (it == s_tlutLoads.end()) return 0;
    const TlutLoad& ld = it->second;
    const u32 bytes = ld.entries * 2;
    const u8* src = physPtr(ld.addr, bytes);
    if (!src) return 0;
    const u64 key = hashBytes(src, bytes) ^ (u64(fmt) << 60) ^ (u64(ld.entries) << 40);
    u32& id = s_tlutIds[key];
    if (!id) id = s_nextTlutId++;
    u32 slot = 0;
    for (; slot < 16; slot++)
        if (s_tlutSlots[slot].tmem == tmem && s_tlutSlots[slot].fmt == fmt) break;
    if (slot == 16) slot = (s_tlutNext++) % 16;
    TlutSlot& s = s_tlutSlots[slot];
    if (s.tmem != tmem || s.fmt != fmt || s.id != id || s.addr != ld.addr || s.entries != ld.entries) {
        s = {tmem, fmt, ld.addr, ld.entries, id};
        aur(GX_AURORA_LOAD_TLUT);
        fifo::write_u8(u8(slot));
        fifo::write_u64(reinterpret_cast<u64>(src));
        fifo::write_u32(fmt);
        fifo::write_u16(u16(ld.entries));
        fifo::write_u32(id);
        fifo::write_u32(0);
    }
    return slot;
}

// Texture identity for Aurora's cache: one object id per (address, size, format, mips), and a
// version that changes when the texture's bytes do (hashed at most once per frame).
struct TexInfo {
    u32 id = 0, version = 0;
    u64 hash = 0, frame = ~0ull;
    u64 hashTb = 0;  // when the contents were last hashed
};
std::unordered_map<u64, TexInfo> s_texInfo;
u32 s_nextTexId = 1;
// bits a texel takes in memory: texture formats (GX_TF_*) and EFB copy formats (copyFormat)
u32 texelBits(u32 fmt) {
    switch (fmt) {
    case 0x00: case 0x08: case 0x0E: case 0x20: case 0x30: return 4;  // I4 C4 CMPR, R4 Z4
    case 0x01: case 0x02: case 0x09: case 0x11: case 0x22: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x39:
    case 0x3A: return 8;  // I8 IA4 C8, Z8 RA4 A8 R8 G8 B8 Z8M Z8L
    case 0x06: case 0x16: case 0x26: return 32;  // RGBA8, Z24X8 YUVA8
    default: return 16;
    }
}

// EFB copy destinations. Their contents live on the GPU (Aurora's copy textures, keyed by the
// destination's host pointer), not in guest memory, and Aurora samples the copy for any texture
// at that address - also after the game put other textures there: the Millennium Falcon's
// cockpit (The Asteroid Field) drew scraps of a rendered frame, or nothing, after the space slug
// cinematic. What the memory held at the copy is hashed; a texture read from there once it
// changed, with another size or texel depth than the copy, is what the game loaded there: the
// copy is dropped and the address is an ordinary texture again (Dolphin's texture cache drops
// its EFB copies the same way). Memory changes alone are not enough: guest memory does not
// hold what the hardware's copies would have written there (display copies never reach it),
// and leaving SELECT GAME the game draws a 640x452 texture at the second XFB's address, where
// it made a 640x450 copy at boot before filling memory with junk (that junk showed, full
// screen). Display copy destinations therefore always keep their copy.
struct CopyDest {
    u32 bytes = 0;         // the largest copy made there, in memory
    u32 w = 0, h = 0, bits = 0;  // the last copy: texels and bits a texel
    u64 hash = 0;          // of those bytes in guest memory, at the last copy
    u64 checked = ~0ull;   // frame of the last comparison
};
std::unordered_map<u32, CopyDest> s_copyDests;
std::unordered_set<u32> s_dispDests;  // display copy (XFB) destinations: their memory says nothing

u32 textureBytes(u32 fmt, u32 w, u32 h, u32 levels) {
    u32 bw = 4, bh = 4, bytes = 32;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;  // I4 C4 CMPR
    case 1: case 2: case 9: bw = 8; bh = 4; break;   // I8 IA4 C8
    case 6: bytes = 64; break;                        // RGBA8
    default: break;                                   // 4x4 blocks of 32 bytes
    }
    u32 total = 0;
    for (u32 l = 0; l < levels; l++) {
        total += ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

void textureIdentity(u32 addr, u32 image0, bool mips, u32 levels, const u8* host, u32& id, u32& version) {
    id = version = 0;
    if (s_copyDests.count(addr)) return;  // render target: let Aurora resolve it every time
    const u32 fmt = (image0 >> 20) & 0xF, w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1;
    const u32 bytes = textureBytes(fmt, w, h, levels);
    if (!physPtr(addr, bytes)) return;
    const u64 key = (u64(addr) << 32) ^ (u64(image0) << 1) ^ (mips ? 1 : 0) ^ (u64(levels) << 56);
    TexInfo& t = s_texInfo[key];
    if (!t.id) t.id = s_nextTexId++;
    // Rehashed once a frame, but no more than 60 times a second: at 144 fps hashing every frame
    // was ~9% of the time, and the game changes texture memory at its own pace (movies 30 fps).
    if (t.frame != s_frames && (!t.hashTb || tbNow() - t.hashTb >= kTbClock / 60)) {
        t.hashTb = tbNow();
        const u64 hsh = XXH3_64bits(host, bytes);
        if (hsh != t.hash) {
            t.hash = hsh;
            t.version++;
        }
        t.frame = s_frames;
    }
    id = t.id;
    version = t.version;
}

void prepareTextures() {
    for (u32 m = 0; m < 8; m++) {
        TexMap& t = s_tex[m];
        const u32 base = texRegBase(m);
        const u32 image3 = s_bp[base + 0x14];
        const u32 tlutReg = s_bp[base + 0x18];
        const u32 image0 = s_bp[base + 0x08];
        const u32 fmt = (image0 >> 20) & 0xF;
        // re-sent when the registers change, once a frame (contents may change behind
        // unchanged registers: movies, render targets), and for palettes
        if (!t.dirty && t.frame == s_frames && !isPalette(fmt)) continue;
        t.dirty = false;
        t.frame = s_frames;
        const u32 addr = (image3 & 0x1FFFFF) << 5;
        if (!addr) continue;
        const u32 w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1;
        u8* host = physPtr(addr, 32);
        if (!host) continue;
        if (const auto it = s_copyDests.find(addr); it != s_copyDests.end() && it->second.checked != s_frames) {
            it->second.checked = s_frames;
            const u8* mem = physPtr(addr, it->second.bytes);
            const u64 hsh = mem ? XXH3_64bits(mem, it->second.bytes) : it->second.hash;
            if (hsh != it->second.hash) {  // see CopyDest
                CopyDest& c = it->second;
                const bool same = (w == c.w && h == c.h && texelBits(fmt) == c.bits) || s_dispDests.count(addr);
                static u32 logs = 0;
                if (logs < 50 && ++logs)
                    logf("gfx", "texture at %08X (%ux%u, %u bits): memory changed since an EFB copy (%ux%u, %u bits) went there: %s",
                         addr, w, h, texelBits(fmt), c.w, c.h, c.bits, same ? "still the copy" : "copy dropped");
                if (same) {
                    c.hash = hsh;
                } else {
                    s_copyDests.erase(it);
                    aur(GX_AURORA_DESTROY_COPY_TEX);
                    fifo::write_u64(reinterpret_cast<u64>(host));
                }
            }
        }
        const u32 mode0 = s_bp[base + 0x00], mode1 = s_bp[base + 0x04];
        const u32 minFilter = (mode0 >> 5) & 7;
        const bool mips = (minFilter == 1 || minFilter == 2 || minFilter == 5 || minFilter == 6) && ((mode1 >> 8) & 0xFF) > 0;
        const u32 tlut = isPalette(fmt) ? bindTlut(tlutReg) : 0;
        static const bool logTex = envFlag("R2_TEX_LOG");
        if (logTex) {
            // with Dolphin's replacement name (tex1_<w>x<h>[_m]_<XXH64 of level 0>[_<palette>]_<fmt>)
            const u32 bytes0 = textureBytes(fmt, w, h, 1);
            const u8* all = physPtr(addr, bytes0);
            const u64 h64 = all ? XXH64(all, bytes0, 0) : 0;
            logf("tex", "map %u: %08X %ux%u fmt %u tlut %u mips %d mode0 %06X mode1 %06X: tex1_%ux%u%s_%016llx%s_%u", m, addr,
                 w, h, fmt, tlut, int(mips), mode0, mode1, w, h, mips ? "_m" : "", (unsigned long long)h64,
                 isPalette(fmt) ? "_<tlut>" : "", fmt);
        }
        {  // prompts baked into textures (button_icons.cpp; it only looks at a few sizes)
            const u32 bytes0 = textureBytes(fmt, w, h, 1);
            buttonIconsNoteTexture(fmt, w, h, physPtr(addr, bytes0), bytes0);
        }
        if (texdump::directory() && !s_copyDests.count(addr)) {  // R2_TEX_DUMP (tex_dump.cpp)
            const u8* all = physPtr(addr, textureBytes(fmt, w, h, 1));
            const u8* pal = nullptr;
            u32 entries = 0, palFmt = 0;
            if (isPalette(fmt)) {
                const auto it = s_tlutLoads.find(tlutReg & 0x3FF);
                if (it != s_tlutLoads.end()) {
                    entries = it->second.entries;
                    pal = physPtr(it->second.addr, entries * 2);
                    palFmt = (tlutReg >> 10) & 3;
                }
            }
            texdump::dump(fmt, w, h, mips, all, palFmt, pal, entries);
        }
        aur(GX_AURORA_LOAD_TEXOBJ);
        fifo::write_u8(u8(m));
        fifo::write_u64(reinterpret_cast<u64>(host));
        fifo::write_u32(w);
        fifo::write_u32(h);
        fifo::write_u32(fmt);
        fifo::write_u32(tlut);
        fifo::write_u8(mips ? 1 : 0);
        u32 id, version;
        const u32 levels = mips ? ((mode1 >> 8) & 0xFF) / 16 + 1 : 1;
        textureIdentity(addr, image0, mips, levels, host, id, version);
        fifo::write_u32(id);
        fifo::write_u32(version);
    }
}

// ---------------------------------------------------------------------------------------
// EFB copies
// ---------------------------------------------------------------------------------------
// Inverse of GXSetTexCopyDst's encoding: format bits 3-6, intensity (bits 15-16 = 3), and a
// depth copy when the PE pixel format is Z24.
u32 copyFormat(u32 cp) {
    const u32 f = ((cp >> 4) & 7) | (((cp >> 3) & 1) << 3);
    const bool intensity = ((cp >> 15) & 3) == 3;
    const bool depth = (s_bp[0x43] & 7) == 3;
    if (depth) {
        switch (f) {
        case 0: return 0x30;            // GX_CTF_Z4
        case 1: case 8: return 0x11;    // GX_TF_Z8
        case 6: return 0x16;            // GX_TF_Z24X8
        case 9: return 0x39;            // GX_CTF_Z8M
        case 10: return 0x3A;           // GX_CTF_Z8L
        case 11: case 3: return 0x13;   // GX_TF_Z16
        case 12: return 0x3C;           // GX_CTF_Z16L
        default: return 0x16;
        }
    }
    if (intensity) {
        switch (f) {
        case 0: return 0;     // GX_TF_I4
        case 1: return 1;     // GX_TF_I8
        case 2: return 2;     // GX_TF_IA4
        case 3: return 3;     // GX_TF_IA8
        case 6: return 0x26;  // GX_CTF_YUVA8
        default: return 1;
        }
    }
    switch (f) {
    case 0: return 0x20;           // GX_CTF_R4
    case 1: case 8: return 0x28;   // GX_CTF_R8
    case 2: return 0x22;           // GX_CTF_RA4
    case 3: return 0x23;           // GX_CTF_RA8
    case 4: return 4;              // GX_TF_RGB565
    case 5: return 5;              // GX_TF_RGB5A3
    case 6: return 6;              // GX_TF_RGBA8
    case 7: return 0x27;           // GX_CTF_A8
    case 9: return 0x29;           // GX_CTF_G8
    case 10: return 0x2A;          // GX_CTF_B8
    case 11: return 0x2B;          // GX_CTF_RG8
    case 12: return 0x2C;          // GX_CTF_GB8
    default: return 6;
    }
}

// bytes an EFB copy of w x h texels takes in memory, by its texture format (copyFormat)
u32 copyBytes(u32 fmt, u32 w, u32 h) {
    const u32 bits = texelBits(fmt);
    const u32 bw = bits <= 8 ? 8 : 4, bh = bits == 4 ? 8 : 4, bytes = bits == 32 ? 64 : 32;
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

void prepareCopy(u32 cp) {
    const u32 tl = s_bp[0x49], wh = s_bp[0x4A];
    const u32 left = tl & 0x3FF, top = (tl >> 10) & 0x3FF;
    const u32 w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    if (cp & (1u << 14)) {  // to the XFB
        s_dispDests.insert((s_bp[0x4B] & 0xFFFFFF) << 5);  // (see CopyDest)
        aur(GX_AURORA_LOAD_DISP_COPY_SRC);
        fifo::write_u32(left);
        fifo::write_u32(top);
        fifo::write_u32(w);
        fifo::write_u32(h);
        return;
    }
    const bool half = (cp >> 9) & 1;
    const u32 dw = half ? (w + 1) / 2 : w, dh = half ? (h + 1) / 2 : h;
    const u32 dest = (s_bp[0x4B] & 0xFFFFFF) << 5;
    u8* host = physPtr(dest, 32);
    {
        CopyDest& d = s_copyDests[dest];
        const u32 fmt = copyFormat(cp);
        d.w = dw;
        d.h = dh;
        d.bits = texelBits(fmt);
        d.bytes = std::max(d.bytes, copyBytes(fmt, dw, dh));
        const u8* mem = physPtr(dest, d.bytes);
        if (!mem) mem = physPtr(dest, d.bytes = copyBytes(fmt, dw, dh));
        d.hash = mem ? XXH3_64bits(mem, d.bytes) : 0;
        d.checked = s_frames;
    }
    aur(GX_AURORA_LOAD_COPY_SRC);
    fifo::write_u32(left);
    fifo::write_u32(top);
    fifo::write_u32(w);
    fifo::write_u32(h);
    aur(GX_AURORA_LOAD_COPY_DST);
    fifo::write_u32(dw);
    fifo::write_u32(dh);
    fifo::write_u32(copyFormat(cp));
    fifo::write_u8(half ? 1 : 0);
    aur(GX_AURORA_LOAD_COPY_DEST);
    fifo::write_u64(reinterpret_cast<u64>(host));
}

// ---------------------------------------------------------------------------------------
// the stream
// ---------------------------------------------------------------------------------------
// R2_GX_DUMP=<frame>[,<count>]: write every command of those presented frames to
// build/gx_frame_<frame>.txt
FILE* s_dump = nullptr;
u64 s_dumpFrame = ~0ull;
u64 s_dumpCount = 1;  // R2_GX_DUMP=<first>[,<count>]
u64 s_dumpAtTb = 0;   // R2_GX_DUMP=t<seconds>[;t<seconds>...][,<count>]: frames after those times
std::vector<u64> s_dumpTimes;
u64 s_dumpLast = ~0ull;
// indexed XF loads seen in the dump: their memory is printed again when the dump closes, to
// compare what the GP read with what the CPU had written by the end of the frame
struct DumpedLoad {
    u32 addr, words;
};
std::vector<DumpedLoad> s_dumpLoads;

void dumpCommand(const u8* p, u32 len) {
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        const u32 n = be16(p + 1), vs = n ? (len - 3) / n : 0;
        fprintf(s_dump, "DRAW prim %02X fmt %u count %u vsize %u vcd %08X/%08X vat %08X %08X %08X:", op & 0xF8, op & 7, n,
                vs, s_vcdLo, s_vcdHi, s_vatA[op & 7], s_vatB[op & 7], s_vatC[op & 7]);
        for (u32 i = 3; i < len && i < 3 + 4 * vs && i < 67; i++) fprintf(s_dump, " %02X", p[i]);
        fputc('\n', s_dump);
        // Indexed positions: the array and the raw data behind the first indices (offline decoding).
        const u32 posType = (s_vcdLo >> 9) & 3;
        if (posType >= 2 && n && vs) {
            const u32 off = (s_vcdLo & 1) + u32(__builtin_popcount((s_vcdLo >> 1) & 0xFF));
            const Array& a = s_arrays[0];
            fprintf(s_dump, "  POS array %08X stride %u:", a.base, a.stride);
            for (u32 v = 0; v < n && v < 6; v++) {
                const u8* vp = p + 3 + v * vs + off;
                const u32 idx = posType == 2 ? vp[0] : be16(vp);
                fprintf(s_dump, " [%u]", idx);
                const u32 bytes = a.stride && a.stride < 12 ? a.stride : 12;
                if (const u8* d = physPtr(a.base + idx * a.stride, bytes))
                    for (u32 b = 0; b < bytes; b++) fprintf(s_dump, "%02X", d[b]);
            }
            fputc('\n', s_dump);
        }
    } else if (op == 0x61) {
        fprintf(s_dump, "BP %02X = %06X\n", p[1], be32(p + 1) & 0xFFFFFF);
    } else if (op == 0x08) {
        fprintf(s_dump, "CP %02X = %08X\n", p[1], be32(p + 2));
    } else if (op == 0x10) {
        const u32 h = be32(p + 1), cnt = (h >> 16) + 1, addr = h & 0xFFFF;
        fprintf(s_dump, "XF %04X x%u:", addr, cnt);
        for (u32 i = 0; i < cnt && i < 16; i++) {
            const u32 w = be32(p + 5 + 4 * i);
            float f;
            memcpy(&f, &w, 4);
            fprintf(s_dump, " %08X(%g)", w, f);
        }
        fputc('\n', s_dump);
    } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        const u32 idx = be16(p + 1), cnt = (be16(p + 3) >> 12) + 1;
        const Array& a = s_arrays[12 + (op - 0x20) / 8];
        fprintf(s_dump, "LOAD_INDX %c idx %u len %u dst %03X from %08X:", 'A' + (op - 0x20) / 8, idx, cnt, be16(p + 3) & 0xFFF,
                a.base + idx * a.stride);
        if (const u8* d = physPtr(a.base + idx * a.stride, cnt * 4))
            for (u32 i = 0; i < cnt; i++) fprintf(s_dump, " %08X", be32(d + 4 * i));
        fputc('\n', s_dump);
        s_dumpLoads.push_back({a.base + idx * a.stride, cnt});
    } else {
        fprintf(s_dump, "OP %02X len %u\n", op, len);
    }
}

// --- scissor box offset ------------------------------------------------------------------
// GXSetScissorBoxOffset moves rendering in the EFB (EFB = screen - offset). Aurora only moves
// its scissor, so while an offset is active the bridge re-sends the projection with the same
// shift folded into its translation terms (the viewport itself must stay inside the target).
u32 s_xfVp[6] = {};    // XF 0x101A..0x101F: viewport scale x/y/z, origin x/y/z
u32 s_xfProj[7] = {};  // XF 0x1020..0x1026: projection p0..p5, type
bool s_projKnown = false;
s32 s_boxOffX = 0, s_boxOffY = 0;

float asFloat(u32 b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
u32 asBits(float f) {
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

bool s_boxBiasSent = false;

// A BP write that is complete on its own (no pending mask from the game's stream).
void bpFull(u32 v) {
    fifo::write_u8(0x61);
    fifo::write_u32(0xFEFFFFFFu);
    fifo::write_u8(0x61);
    fifo::write_u32(v);
}

void sendScissor() {
    const u32 s0 = s_bp[0x20], s1 = s_bp[0x21];
    auto adj = [](u32 r, s32 off) { return u32(std::clamp(s32(r) - off, 0, 2047)); };
    const u32 tp = adj(s0 & 0x7FF, s_boxOffY), lf = adj((s0 >> 12) & 0x7FF, s_boxOffX);
    const u32 bm = adj(s1 & 0x7FF, s_boxOffY), rt = adj((s1 >> 12) & 0x7FF, s_boxOffX);
    bpFull(0x20000000u | (lf << 12) | tp);
    bpFull(0x21000000u | (rt << 12) | bm);
}

// shiftX: orthographic draws moved by that much (NDC); mirror: orthographic draws reflected
// about x = axis (NDC): x' = 2 axis - x.
void sendProjection(float shiftX = 0.f, bool mirror = false, float axis = 0.f) {
    if (!s_projKnown) return;
    u32 pr[7];
    memcpy(pr, s_xfProj, sizeof pr);
    if (shiftX != 0.f && pr[6] != 0) pr[1] = asBits(asFloat(pr[1]) + shiftX);  // orthographic: NDC
    if (mirror && pr[6] != 0) {
        pr[0] = asBits(-asFloat(pr[0]));
        pr[1] = asBits(2.f * axis - asFloat(pr[1]));
    }
    if (s_boxOffX || s_boxOffY) {
        // screen = ndc * scale + origin: moving the screen by -off moves ndc by -off / scale.
        // Perspective: ndc = clip / w with w = -z, so the shift enters p1/p3 negated;
        // orthographic: w = 1 and it adds directly.
        const float sx = asFloat(s_xfVp[0]), sy = asFloat(s_xfVp[1]);
        const float dx = sx != 0.f ? -float(s_boxOffX) / sx : 0.f;
        const float dy = sy != 0.f ? -float(s_boxOffY) / sy : 0.f;
        const bool ortho = pr[6] != 0;
        pr[1] = asBits(asFloat(pr[1]) + (ortho ? dx : -dx));
        pr[3] = asBits(asFloat(pr[3]) + (ortho ? dy : -dy));
        if (s_copyLog)
            logf("gfx", "box offset %d,%d: %s projection p1 %g -> %g, p3 %g -> %g (viewport scale %g %g)", s_boxOffX,
                 s_boxOffY, ortho ? "ortho" : "persp", asFloat(s_xfProj[1]), asFloat(pr[1]), asFloat(s_xfProj[3]),
                 asFloat(pr[3]), sx, sy);
    }
    fifo::write_u8(0x10);
    fifo::write_u32((6u << 16) | 0x1020);
    for (u32 w : pr) fifo::write_u32(w);
}

// --- screen-space 2D ---------------------------------------------------------------------
// The targeting computer (cockpit view, Y) draws its orange screen as a 2D quad in 640x480
// screen units at corners the game projected from 3D with the camera's own projection, which
// widescreen already made wide: those are positions on the widened screen, and Aurora's 4:3
// layout for 2D squeezed the quad a second time (the screen came out narrower than its frame by
// the widening, and the targets it shows did not line up). hle_TargetingPass brackets that
// drawing with a BP write to an unused register (never forwarded); orthographic draws in
// screen units (not the identity projection: the centered reticle diamond keeps its shape)
// inside it go to Aurora as screen space (GX_AURORA_SET_SCREEN_SPACE_2D).
constexpr u8 kScreenSpaceMarker = 0x7E;
constexpr u32 kScreenSpaceMagic = 0x5C5200;
// hle_ReticleDraw's bracket (the flight reticle): every orthographic draw, the identity
// projection included - the reticle's own (issue #10)
constexpr u32 kScreenSpaceAllMagic = 0x5C5220;
bool s_screenSpaceWindow = false, s_screenSpaceAllWindow = false, s_screenSpaceSent = false;

void screenSpace2D() {
    const bool ortho = s_projKnown && s_xfProj[6] != 0;
    const bool want = ortho && ((s_screenSpaceWindow && asFloat(s_xfProj[0]) != 1.f) || s_screenSpaceAllWindow);
    if (want == s_screenSpaceSent) return;
    s_screenSpaceSent = want;
    static int logs = 0;
    if (s_copyLog && logs < 20) {
        logs++;
        logf("gfx", "screen-space 2D %s (projection p0 %g)", want ? "on" : "off", asFloat(s_xfProj[0]));
    }
    aur(GX_AURORA_SET_SCREEN_SPACE_2D);
    fifo::write_u8(want ? 1 : 0);
}

// --- movies filling the screen -------------------------------------------------------------
// Cutscene movies (the Death Star's explosion at the end of mission 1, the capital ship's in the
// Factor 5 intro) are quads in the 3D scene that cover the game's own view with some margin. A
// wider screen (or, for a letterboxed cinematic, a narrower one) showed the scene past their
// edges. hle_CutsceneVideoDraw brackets their drawing with BP writes to the unused register the
// targeting computer uses (another magic value); draws inside go to Aurora with
// GX_AURORA_SET_FILL_SCREEN: edges that stop inside the screen move out to it. The NDC shift a
// scissor box offset put into the projection (two-pass frames: sendProjection) goes along, so
// the frame's edges count, not the pass's.
constexpr u32 kFillMagic = 0x5C5210;
bool s_fillWindow = false, s_fillSent = false;
float s_fillShiftX = 0.f, s_fillShiftY = 0.f;

void fillScreen() {
    float dx = 0.f, dy = 0.f;
    if (s_fillWindow && (s_boxOffX || s_boxOffY)) {
        const float sx = asFloat(s_xfVp[0]), sy = asFloat(s_xfVp[1]);
        dx = sx != 0.f ? -float(s_boxOffX) / sx : 0.f;
        dy = sy != 0.f ? -float(s_boxOffY) / sy : 0.f;
    }
    if (s_fillWindow == s_fillSent && dx == s_fillShiftX && dy == s_fillShiftY) return;
    s_fillSent = s_fillWindow;
    s_fillShiftX = dx;
    s_fillShiftY = dy;
    aur(GX_AURORA_SET_FILL_SCREEN);
    fifo::write_u8(s_fillWindow ? 1 : 0);
    fifo::write_f32(dx);
    fifo::write_f32(dy);
}

// Debugging: R2_GX_SKIP=<first>-<last>:<filter>[;...] drops the draws matching <filter> while
// the retrace count is in [first, last] (s<first>-<last>: seconds since the start instead; the
// retrace count runs ahead while a cinematic is skipped, and with the frame rate). Filters: v<VCD low hex> (e.g. v1E00),
// p<primitive hex> (e.g. pB8), t<map 0 image address hex> (BP 0x94 value, e.g. t47F9B),
// b<BP 0x41 blend mode hex>, z<BP 0x40 z mode hex>, i<first>-<last> (draw index within the
// frame, decimal), or * (everything). One rule per window: screenshots taken
// meanwhile show what each group of draws is.
struct SkipRule {
    u64 first, last;
    bool seconds;
    char kind;
    u32 value, value2;
};
std::vector<SkipRule> parseSkipRules() {
    std::vector<SkipRule> rules;
    const char* s = getenv("R2_GX_SKIP");
    while (s && *s) {
        char* end;
        SkipRule r{};
        r.seconds = *s == 's';
        if (r.seconds) s++;
        r.first = strtoull(s, &end, 10);
        r.last = *end == '-' ? strtoull(end + 1, &end, 10) : r.first;
        if (*end != ':') break;
        r.kind = end[1];
        if (r.kind == '*') {
            end += 2;
        } else if (r.kind == 'i') {
            r.value = u32(strtoul(end + 2, &end, 10));
            r.value2 = *end == '-' ? u32(strtoul(end + 1, &end, 10)) : r.value;
        } else {
            r.value = u32(strtoul(end + 2, &end, 16));
        }
        rules.push_back(r);
        logf("gfx", "skip rule: vblanks %llu-%llu drop %c%X", (unsigned long long)r.first, (unsigned long long)r.last,
             r.kind, r.value);
        s = *end == ';' ? end + 1 : end;
    }
    return rules;
}
bool skipDraw(u8 op) {
    static const std::vector<SkipRule> rules = parseSkipRules();
    const u32 draw = s_frameDraws++;
    static u64 announced = ~0ull;
    for (size_t i = 0; i < rules.size(); i++) {
        const SkipRule& r = rules[i];
        static const u64 startTb = tbNow();
        const u64 now = r.seconds ? (tbNow() - startTb) / kTbClock : s_vblanks;
        if (now < r.first || now > r.last) continue;
        if (announced != i) {
            announced = i;
            logf("gfx", "skip rule %zu active (vblank %llu, frame %llu)", i, (unsigned long long)s_vblanks, (unsigned long long)s_frames);
        }
        if (r.kind == '*' || (r.kind == 'v' && s_vcdLo == r.value) || (r.kind == 'p' && (op & 0xF8) == r.value) ||
            (r.kind == 't' && (s_bp[0x94] & 0xFFFFFF) == r.value) || (r.kind == 'i' && draw >= r.value && draw <= r.value2) ||
            (r.kind == 'b' && (s_bp[0x41] & 0xFFFF) == r.value) || (r.kind == 'z' && (s_bp[0x40] & 0x1F) == r.value))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// widescreen menu backgrounds
// ---------------------------------------------------------------------------------------
// The menu screens (title, SELECT GAME, Options...) lay a mosaic of panel tiles over the 4:3
// screen: dozens of orthographic CMPR quads, each with its own texture, the textures allocated
// one after another and drawn in that order. In widescreen those quads keep their shape inside
// the 4:3 area (bars at the sides). Tiling fills the sides: each tile is drawn again one 4:3
// width to the left and to the right (the projection's x translation +-2 in NDC; Aurora then
// squeezes it like the original), so the right side continues with the background's left
// edge and the left side with its right edge.
// A run of at least kTileRun such draws with ascending texture addresses marks those textures
// as background tiles; draws using them are tiled from then on (the run's first frame is not).
// Off in missions (setMenuTiling): HUD pieces must never be repeated.
// The mosaic was not made to repeat, so where a copy meets the original the panels break off
// in a hard vertical line. Once the run is drawn (seamBlend, before the next draw that is not
// a tile), a band just outside each seam starts as the mirror image of the original's edge,
// which continues it seamlessly, and fades into the copy (a custom Aurora draw sampling a
// snapshot of the EFB).
constexpr u32 kTileRun = 8;
bool s_menuTiling = false;
struct TileTex {
    u64 lastFrame;
};
std::unordered_map<u64, TileTex> s_tileTex;  // texture (address | format/size << 32)
std::vector<u64> s_tileRun;                   // current run of ascending candidate textures
u32 s_tileRunLast = 0;
bool s_tileRunOk = false;    // the current run reached kTileRun: its later tiles join at once
bool s_seamPending = false;  // tiles were copied since the last seam blend

// how many copies each side needs to reach the screen's edge (0: not wider than 4:3)
int tileCopies() {
    const settings::Settings& st = settings::get();
    float a = settings::aspectValue(st.aspect);
    if (a <= 0.f) {
        u32 w = 0, h = 0;
        AuroraGetRenderSize(&w, &h);
        a = h ? float(w) / float(h) : 4.f / 3.f;
    }
    const float k = a / (4.f / 3.f);
    if (k < 1.01f) return 0;
    return int((k - 1.f) / 2.f) + 1;  // copy n covers up to 2n + 1 widths
}

// a candidate: orthographic, textured from map 0 with a CMPR texture of tile size
bool tileCandidate(u64& key) {
    if (!s_projKnown || s_xfProj[6] == 0) return false;
    if ((s_vcdHi & 3) == 0 || (s_bp[0x00] & 0xF) == 0) return false;  // no texture coordinates / texgens
    const u32 img = s_bp[0x88];
    if (((img >> 20) & 0xF) != 0xE) return false;  // CMPR
    const u32 w = (img & 0x3FF) + 1, h = ((img >> 10) & 0x3FF) + 1;
    if (w < 16 || h < 16 || w > 512 || h > 512) return false;
    const u32 addr = (s_bp[0x94] & 0xFFFFFF) << 5;
    key = u64(addr) | (u64(img & 0xFFFFFF) << 32);
    return true;
}

// the draw at p (already sent once): its copies at the sides
void tileDraw(const u8* p, u32 len) {
    if (!s_menuTiling) return;
    u64 key;
    if (!tileCandidate(key)) return;
    const u32 addr = u32(key);
    // run detection: ascending addresses, a few KiB apart (the same texture again continues it)
    if (s_tileRun.empty() || addr != s_tileRunLast) {
        const bool continues = !s_tileRun.empty() && addr > s_tileRunLast && addr - s_tileRunLast <= 0x10000;
        if (!continues) {
            s_tileRun.clear();
            s_tileRunOk = false;
        }
        if (s_tileRunOk) {
            s_tileTex[key] = {s_frames};
        } else {
            s_tileRun.push_back(key);
            if (s_tileRun.size() >= kTileRun) {
                for (u64 k : s_tileRun) s_tileTex[k] = {s_frames};
                s_tileRunOk = true;
            }
        }
    }
    s_tileRunLast = addr;
    auto it = s_tileTex.find(key);
    if (it == s_tileTex.end()) return;
    it->second.lastFrame = s_frames;
    const int n = tileCopies();
    if (!n) return;
    for (int i = 1; i <= n; i++)
        for (const float side : {2.f, -2.f}) {
            sendProjection(side * float(i));
            fifo::write_data(p, len);
            fifo::finish_draw();
        }
    sendProjection();
    s_seamPending = true;
}

// is the draw about to be sent one of the known background tiles?
bool isTileDraw() {
    u64 key;
    return s_menuTiling && tileCandidate(key) && s_tileTex.count(key);
}

// --- seam blend ---------------------------------------------------------------------------
// Runs on Aurora's render worker inside the EFB pass: two bands, one outside each seam, as
// wide as kSeamBand of the target's height. The seams are where the 4:3 picture ends: the
// logical frame (640x480) maps to the target's full height, so they sit 2/3 of the height
// either side of the center. Each pixel mixes the mirror image across the seam (continues the
// original) into what is there (the copy) along a smoothstep. Shaders run per pixel even with
// MSAA; the snapshot is resolved.
constexpr float kSeamBand = 0.08f;
aurora::gfx::DrawTypeId s_seamType = aurora::gfx::InvalidDrawType;

struct SeamPayload {
    WGPUTextureView snapshot;  // a reference of its own, released by the draw
    float band;
};

const char* const kSeamShader = R"(
@group(0) @binding(0) var snap: texture_2d<f32>;
struct Band { band: f32 };
@group(0) @binding(1) var<uniform> u: Band;

struct VOut { @builtin(position) pos: vec4<f32> };

// the mirror axes: the seams moved in by two logical pixels (the picture's outermost texels
// carry the tiles' edge filtering, a bright line the mirror would double)
fn seams(dim: vec2<f32>) -> vec2<f32> {
    let half = dim.y * (2.0 / 3.0) - dim.y / 240.0;
    return vec2<f32>(dim.x * 0.5 - half, dim.x * 0.5 + half);
}

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VOut {
    let dim = vec2<f32>(textureDimensions(snap));
    let s = seams(dim);
    let band = dim.y * u.band;
    let right = vi >= 6u;
    let x0 = select(s.x, s.y, right);
    let x1 = select(s.x - band, s.y + band, right);
    let c = vi % 6u;
    let fu = select(0.0, 1.0, c == 1u || c == 4u || c == 5u);
    let fv = select(0.0, 1.0, c == 2u || c == 3u || c == 5u);
    let px = mix(x0, x1, fu);
    var o: VOut;
    o.pos = vec4<f32>(px / dim.x * 2.0 - 1.0, 1.0 - fv * 2.0, 0.0, 1.0);
    return o;
}

@fragment
fn fs_main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
    let dimi = vec2<i32>(textureDimensions(snap));
    let dim = vec2<f32>(dimi);
    let s = seams(dim);
    let seam = select(s.x, s.y, p.x > dim.x * 0.5);
    let t = smoothstep(0.0, 1.0, abs(p.x - seam) / (dim.y * u.band));
    let y = clamp(i32(p.y), 0, dimi.y - 1);
    let mx = clamp(i32(2.0 * seam - p.x), 0, dimi.x - 1);
    let a = textureLoad(snap, vec2<i32>(mx, y), 0);
    let b = textureLoad(snap, vec2<i32>(clamp(i32(p.x), 0, dimi.x - 1), y), 0);
    return mix(a, b, t);
}
)";

void seamDraw(const aurora::gfx::DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* payload,
              size_t size, void*) {
    if (size != sizeof(SeamPayload)) return;
    SeamPayload sp;
    memcpy(&sp, payload, sizeof sp);
    const wgpu::TextureView snapshot = wgpu::TextureView::Acquire(sp.snapshot);
    const auto& layout = ctx.layout;
    if (!layout.colorAttachmentCount || !snapshot) return;
    // one pipeline per render target layout (render worker thread only)
    struct Pipe {
        wgpu::RenderPipeline pipeline;
        wgpu::BindGroupLayout bgl;
        wgpu::Buffer uniform;
        float band = -1.f;
    };
    static std::unordered_map<u64, Pipe> pipes;
    Pipe& pp = pipes[layout.key];
    if (!pp.pipeline) {
        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = kSeamShader;
        const wgpu::ShaderModuleDescriptor md{.nextInChain = &wgsl, .label = "menu seams"};
        const wgpu::ShaderModule module = ctx.device.CreateShaderModule(&md);
        const wgpu::BindGroupLayoutEntry entries[2] = {
            {.binding = 0,
             .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
             .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                         .viewDimension = wgpu::TextureViewDimension::e2D}},
            {.binding = 1,
             .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
             .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = 16}},
        };
        const wgpu::BindGroupLayoutDescriptor bd{.label = "menu seams", .entryCount = 2, .entries = entries};
        pp.bgl = ctx.device.CreateBindGroupLayout(&bd);
        const wgpu::PipelineLayoutDescriptor pld{.label = "menu seams", .bindGroupLayoutCount = 1,
                                                 .bindGroupLayouts = &pp.bgl};
        const wgpu::PipelineLayout pl = ctx.device.CreatePipelineLayout(&pld);
        std::array<wgpu::ColorTargetState, aurora::gfx::MaxColorAttachments> targets{};
        for (u32 i = 0; i < layout.colorAttachmentCount; i++)
            targets[i] = {.format = layout.colorAttachments[i].format,
                          .writeMask = i == aurora::gfx::SceneColorAttachmentIndex ? wgpu::ColorWriteMask::All
                                                                                   : wgpu::ColorWriteMask::None};
        const wgpu::FragmentState fs{
            .module = module, .entryPoint = "fs_main", .targetCount = layout.colorAttachmentCount, .targets = targets.data()};
        const wgpu::DepthStencilState ds{.format = layout.depthStencilFormat,
                                         .depthWriteEnabled = false,
                                         .depthCompare = wgpu::CompareFunction::Always};
        const wgpu::RenderPipelineDescriptor rd{
            .label = "menu seams",
            .layout = pl,
            .vertex = {.module = module, .entryPoint = "vs_main"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
            .depthStencil = layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &ds : nullptr,
            .multisample = {.count = layout.sampleCount},
            .fragment = &fs,
        };
        pp.pipeline = ctx.device.CreateRenderPipeline(&rd);
        const wgpu::BufferDescriptor ubd{.label = "menu seams",
                                         .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                         .size = 16};
        pp.uniform = ctx.device.CreateBuffer(&ubd);
    }
    if (pp.band != sp.band) {
        const float u[4] = {sp.band, 0.f, 0.f, 0.f};
        ctx.queue.WriteBuffer(pp.uniform, 0, u, sizeof u);
        pp.band = sp.band;
    }
    const wgpu::BindGroupEntry bge[2] = {{.binding = 0, .textureView = snapshot},
                                         {.binding = 1, .buffer = pp.uniform, .size = 16}};
    const wgpu::BindGroupDescriptor bgd{.label = "menu seams", .layout = pp.bgl, .entryCount = 2, .entries = bge};
    const wgpu::BindGroup bg = ctx.device.CreateBindGroup(&bgd);
    const u32 w = layout.colorAttachments[0].width, h = layout.colorAttachments[0].height;
    pass.SetPipeline(pp.pipeline);
    pass.SetBindGroup(0, bg);
    pass.SetViewport(0.f, 0.f, float(w), float(h), 0.f, 1.f);
    pass.SetScissorRect(0, 0, w, h);
    pass.Draw(12);
}

// After a run of tiles (and their copies): blend the seams on what is in the EFB now.
void seamBlend() {
    s_seamPending = false;
    if (aurora::gfx::is_offscreen()) return;
    if (s_seamType == aurora::gfx::InvalidDrawType)
        s_seamType = aurora::gfx::register_draw_type({.label = "menu seams", .draw = seamDraw});
    static const float band = [] {
        const char* e = getenv("R2_SEAM_BAND");  // tuning: fraction of the height (0 = off)
        return e ? float(atof(e)) : kSeamBand;
    }();
    if (band <= 0.f) return;
    aurora::gfx::ResolvedTargets out;
    if (!aurora::gfx::resolve_pass({.color = true}, out) || !out.color) return;
    SeamPayload sp{out.color.Get(), band};
    wgpuTextureViewAddRef(sp.snapshot);
    if (!aurora::gfx::push_custom_draw(s_seamType, &sp, sizeof sp)) wgpuTextureViewRelease(sp.snapshot);
}

// --- SELECT GAME's X-wing -----------------------------------------------------------------
// SELECT GAME shows an X-wing from behind (or below) as three orthographic CMPR sprites: the
// upper and the lower wing (560x160, rotated by their position matrices) and then the fuselage
// (224x256), right of the menu with both wings reaching left. In widescreen the picture goes
// on right of the fuselage, where the other two wings belong: each wing is drawn again,
// mirrored about the fuselage's center line (projection x scale negated, culling swapped for
// the flipped winding), so it moves and turns with the original. The fuselage comes after
// the wings, so the axis is the wing's center plus the offset to the fuselage measured before
// (the sprites move together).
bool s_wingSeen = false;  // this frame
float s_wingCenter = 0.f;
bool s_wingOffsetKnown = false;
float s_wingOffset = 0.f;  // fuselage center - wing center (NDC)

// The draw's horizontal extent in (4:3) NDC: its positions through the current position
// matrix and projection. Plain formats only: no per-vertex matrix index.
bool drawExtentX(const u8* p, u32 len, float& minX, float& maxX) {
    const u32 fmt = p[0] & 7, count = be16(p + 1);
    if (!count || !s_projKnown || (s_vcdLo & 1)) return false;
    const u32 vs = (len - 3) / count;
    const u32 A = s_vatA[fmt];
    const u32 posType = (s_vcdLo >> 9) & 3, compType = (A >> 1) & 7, comps = (A & 1) ? 3 : 2;
    const u32 cs = compSize(compType);
    if (!posType || !cs) return false;
    const float scale = 1.f / float(1u << ((A >> 4) & 0x1F));
    const u32 off = u32(__builtin_popcount((s_vcdLo >> 1) & 0xFF));  // texture matrix indices
    const float* m = &s_xfMtx[(s_cpMtxA & 0x3F) * 4];
    const float p0 = asFloat(s_xfProj[0]), p1 = asFloat(s_xfProj[1]);
    minX = 1e30f;
    maxX = -1e30f;
    for (u32 i = 0; i < count; i++) {
        const u8* d = p + 3 + i * vs + off;
        if (posType >= 2) {
            const u32 idx = posType == 2 ? d[0] : be16(d);
            d = physPtr(s_arrays[0].base + idx * s_arrays[0].stride, comps * cs);
            if (!d) return false;
        }
        float v[3] = {0.f, 0.f, 0.f};
        for (u32 c = 0; c < comps; c++) {
            const u8* q = d + c * cs;
            switch (compType) {
            case 0: v[c] = float(q[0]) * scale; break;
            case 1: v[c] = float(s8(q[0])) * scale; break;
            case 2: v[c] = float(be16(q)) * scale; break;
            case 3: v[c] = float(s16(be16(q))) * scale; break;
            default: v[c] = asFloat(be32(q)); break;
            }
        }
        const float ex = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
        float x;
        if (s_xfProj[6]) {
            x = p0 * ex + p1;
        } else {
            const float ez = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
            if (ez >= 0.f) return false;
            x = (p0 * ex + p1 * ez) / -ez;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
    }
    return true;
}

// the draw at p (already sent once): a wing gets its mirror image
void xwingDraw(const u8* p, u32 len) {
    if (!s_menuTiling || !s_projKnown || s_xfProj[6] == 0) return;
    const u32 img = s_bp[0x88];
    if (((img >> 20) & 0xF) != 0xE) return;  // CMPR
    const u32 w = (img & 0x3FF) + 1, h = ((img >> 10) & 0x3FF) + 1;
    const bool wing = w == 560 && h == 160, body = w == 224 && h == 256;
    if (!wing && !body) return;
    float x0, x1;
    if (!drawExtentX(p, len, x0, x1)) return;
    const float c = (x0 + x1) * 0.5f;
    if (body) {
        if (s_wingSeen) {
            s_wingOffset = c - s_wingCenter;
            s_wingOffsetKnown = true;
        }
        return;
    }
    s_wingSeen = true;
    s_wingCenter = c;
    if (!s_wingOffsetKnown || !tileCopies()) return;
    const u32 gen = s_bp[0x00] & 0xFFFFFF, cull = (gen >> 14) & 3;
    const bool swap = cull == 1 || cull == 2;
    if (swap) bpFull((gen & ~(3u << 14)) | ((3 - cull) << 14));
    sendProjection(0.f, true, c + s_wingOffset);
    fifo::write_data(p, len);
    fifo::finish_draw();
    sendProjection();
    if (swap) bpFull(gen);
}

// --- full-frame cards in widened views ------------------------------------------------------
// The crawl's "A long time ago..." card is one perspective quad (640x384 CMPR: blue text on
// black, clamped) placed to fill the cinematic's own 2.105:1 view edge to edge. Widescreen opens
// that view, and on displays wider than 2.105 (21:9, 32:9) the starfield drawn before it showed
// past its sides; on displays below about 3:2 (windows) the view opens taller and it showed above
// and below. Such a quad (four vertices covering the game's view, direct f32 positions, no indexed
// attributes) gets four extensions drawn after it: from each edge outward, every vertex carrying
// that edge's attributes (texture coordinates: the clamped edge column or row, the card's
// border). They do not overlap the card, so a fading card blends the same everywhere; where the
// view does not reach past the card they lie off-screen.
void cardFill(const u8* p, u32 len) {
    if (p[0] != 0x80 || be16(p + 1) != 4 || !s_projKnown || s_xfProj[6] != 0) return;  // one perspective quad
    const u32 lo = s_vcdLo, hi = s_vcdHi, fmt = p[0] & 7, A = s_vatA[fmt];
    if (lo & 0x1FF) return;                    // per-vertex matrix indices
    if (((lo >> 9) & 3) != 1 || ((A >> 1) & 7) != 4 || !(A & 1)) return;  // direct f32 xyz positions
    for (u32 sh = 11; sh <= 15; sh += 2)
        if (((lo >> sh) & 3) > 1) return;     // indexed normals / colors
    for (u32 t = 0; t < 8; t++)
        if (((hi >> (2 * t)) & 3) > 1) return;  // indexed texture coordinates
    const u32 vs = (len - 3) / 4;
    if (vs < 12) return;
    const float* m = &s_xfMtx[(s_cpMtxA & 0x3F) * 4];
    const float p0 = asFloat(s_xfProj[0]), p1 = asFloat(s_xfProj[1]), p2 = asFloat(s_xfProj[2]),
                p3 = asFloat(s_xfProj[3]);
    float pos[4][3], nx[4], ny[4];
    for (u32 i = 0; i < 4; i++) {
        const u8* d = p + 3 + i * vs;  // positions come first (no matrix indices)
        for (u32 c = 0; c < 3; c++) pos[i][c] = asFloat(be32(d + 4 * c));
        const float* v = pos[i];
        const float ex = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
        const float ey = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
        const float ez = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
        if (ez >= 0.f) return;
        nx[i] = (p0 * ex + p1 * ez) / -ez;
        ny[i] = (p2 * ey + p3 * ez) / -ez;
    }
    const float minX = std::min({nx[0], nx[1], nx[2], nx[3]}), maxX = std::max({nx[0], nx[1], nx[2], nx[3]});
    const float minY = std::min({ny[0], ny[1], ny[2], ny[3]}), maxY = std::max({ny[0], ny[1], ny[2], ny[3]});
    if (minX > -0.98f || maxX < 0.98f || minY > -0.98f || maxY < 0.98f) return;  // not the whole view
    // a screen-aligned rectangle: two vertices on each side edge, two on the top and bottom
    const float tolX = 0.02f * (maxX - minX), tolY = 0.02f * (maxY - minY);
    bool left[4], top[4];
    for (u32 i = 0; i < 4; i++) {
        left[i] = nx[i] - minX < tolX;
        if (!left[i] && maxX - nx[i] >= tolX) return;
        top[i] = maxY - ny[i] < tolY;
        if (!top[i] && ny[i] - minY >= tolY) return;
    }
    u32 corners = 0;  // one vertex at each
    for (u32 i = 0; i < 4; i++) corners |= 1u << (left[i] * 2 + top[i]);
    if (corners != 15) return;
    // the vertex on the other side edge in the same row (h), on the other of top and bottom in
    // the same column (v)
    u32 partner[2][4] = {};
    for (u32 i = 0; i < 4; i++)
        for (u32 j = 0; j < 4; j++) {
            if (left[j] != left[i] && top[j] == top[i]) partner[0][i] = j;
            if (top[j] != top[i] && left[j] == left[i]) partner[1][i] = j;
        }
    constexpr float kOut = 4.f;  // how far out (card sizes): past a 32:9 screen's sides
    static std::vector<u8> buf;
    buf.assign(p, p + len);
    for (int side = 0; side < 4; side++) {  // left, right, top, bottom extensions
        const bool* edge = side < 2 ? left : top;
        const u32* other = partner[side / 2];
        for (u32 i = 0; i < 4; i++) {
            const bool onSide = edge[i] == (side % 2 == 0);
            const u32 src = onSide ? i : other[i];  // that edge's vertex in this row / column
            u8* d = buf.data() + 3 + i * vs;
            memcpy(d, p + 3 + src * vs, vs);
            if (onSide) {  // moved outward, away from the opposite edge
                const u32 o = other[i];
                for (u32 c = 0; c < 3; c++) {
                    const u32 w = asBits(pos[i][c] + (pos[i][c] - pos[o][c]) * kOut);
                    d[4 * c] = u8(w >> 24); d[4 * c + 1] = u8(w >> 16); d[4 * c + 2] = u8(w >> 8); d[4 * c + 3] = u8(w);
                }
            }
        }
        fifo::write_data(buf.data(), len);
        fifo::finish_draw();
    }
}

void tileFrameEnd() {
    s_tileRun.clear();
    s_tileRunLast = 0;
    s_tileRunOk = false;
    s_seamPending = false;
    s_wingSeen = false;
    // a screen's tiles stop being drawn: forget them (the memory may hold other textures later)
    if (s_frames % 64 == 0)
        for (auto it = s_tileTex.begin(); it != s_tileTex.end();)
            it = s_frames - it->second.lastFrame > 120 ? s_tileTex.erase(it) : std::next(it);
}
}  // namespace

// the display's refresh rate, exact where SDL knows it (119.88 Hz), 0 if unknown
double displayRefresh() {
    SDL_Window* w = aurora_get_window();
    const SDL_DisplayMode* m = w ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)) : nullptr;
    if (!m) return 0.0;
    if (m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0)
        return double(m->refresh_rate_numerator) / double(m->refresh_rate_denominator);
    return m->refresh_rate > 0 ? double(m->refresh_rate) : 0.0;
}

int displayRefreshHz() { return int(displayRefresh() + 0.5); }

// The emulated retrace rate. With V-Sync on, a frame waits for the display's refresh: at 144 on
// a 120 Hz display the game thread waited for Aurora's presents, so the game ran 111-120 frames a
// second and its sound clock fell up to 28% short. So with V-Sync the retrace follows the
// display: its refresh when the setting is at or above it, else a whole fraction of it within
// 1 Hz of the setting (60 on a 120 Hz display: exactly 60.00, no frame shown three times now and
// then), else the setting. The game paces itself by real time, so any rate keeps its speed.
double retraceHz() {
    const settings::Settings& s = settings::get();
    const double set = s.fps == 60 ? 60000.0 / 1001.0 : double(s.fps);
    const double r = s.vsync ? displayRefresh() : 0.0;
    if (r < 30.0) return set;
    if (set >= r - 1.0) return r;
    for (int k = 2; k <= 4; k++)
        if (std::abs(r / k - set) < 1.0) return r / k;
    return set;
}

void setMenuTiling(bool on) {
    if (on == s_menuTiling) return;
    s_menuTiling = on;
    s_tileTex.clear();
    s_tileRun.clear();
    s_tileRunOk = false;
    s_seamPending = false;
    s_wingOffsetKnown = false;
}

namespace {

void onCommand(const u8* p, u32 len) {
    ensureFrame();
    if (s_dumpAtTb && tbNow() >= s_dumpAtTb && !s_dump) [[unlikely]] {
        s_dumpAtTb = 0;
        if (!s_dumpTimes.empty()) {
            s_dumpAtTb = s_dumpTimes.front();
            s_dumpTimes.erase(s_dumpTimes.begin());
        }
        s_dumpFrame = s_frames + 1;
    }
    if (s_frames >= s_dumpFrame && s_frames < s_dumpFrame + s_dumpCount) [[unlikely]] {
        if (!s_dump) {
            char name[64];
            snprintf(name, sizeof name, "build/gx_frame_%llu.txt", (unsigned long long)s_frames);
            s_dump = fopen(name, "w");
        }
        if (s_dump && s_frames != s_dumpLast) {
            // the frame's starting state: every BP register and the vertex arrays
            fprintf(s_dump, "=== presented frame %llu\nBP state:", (unsigned long long)s_frames);
            for (int r = 0; r < 256; r++) fprintf(s_dump, "%s%02X=%06X", r % 16 ? " " : "\n  ", r, s_bp[r] & 0xFFFFFF);
            fprintf(s_dump, "\nCP arrays:");
            for (int r = 0; r < 16; r++) fprintf(s_dump, " %d:%08X/%u", r, s_arrays[r].base, s_arrays[r].stride);
            fputc('\n', s_dump);
            s_dumpLast = s_frames;
        }
        if (s_dump) dumpCommand(p, len);
    } else if (s_dump) {
        for (const auto& l : s_dumpLoads) {
            fprintf(s_dump, "LATER %08X:", l.addr);
            if (const u8* d = physPtr(l.addr, l.words * 4))
                for (u32 i = 0; i < l.words; i++) fprintf(s_dump, " %08X", be32(d + 4 * i));
            fputc('\n', s_dump);
        }
        s_dumpLoads.clear();
        fclose(s_dump);
        s_dump = nullptr;
        s_dumpFrame = ~0ull;
        logf("gfx", "frames dumped to build/gx_frame_*.txt");
    }
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        // skipping a cinematic (hle_game.cpp): the game runs flat out and nothing is drawn
        if (cinematicSkip()) return;
        if (skipDraw(op)) [[unlikely]]
            return;
        if (s_seamPending && !isTileDraw()) seamBlend();  // the background is complete
        screenSpace2D();
        fillScreen();
        prepareArrays(p);
        prepareTextures();
        // Aurora merges consecutive draws with unchanged state. That broke on this game's
        // empty/short strips until Aurora patch 0005; R2_NO_MERGE=1 makes every draw stand alone
        // again (a TLUT load into slot 1023, none, is always handled and dirties the textures).
        static const bool allowMerge = !envFlag("R2_NO_MERGE");
        if (!allowMerge && s_bpMask == 0xFFFFFF) {
            fifo::write_u8(0x61);
            fifo::write_u32(0x650003FFu);
        }
        fifo::write_data(p, len);
        fifo::finish_draw();
        tileDraw(p, len);
        xwingDraw(p, len);
        cardFill(p, len);
        return;
    }
    switch (op) {
    case 0x08: {  // CP register
        const u8 reg = p[1];
        const u32 v = be32(p + 2);
        s_cp[reg] = v;
        s_cpValid.set(reg);
        if (reg >= 0xA0 && reg <= 0xAF) {  // array base: sent as a host pointer when used
            Array& a = s_arrays[reg - 0xA0];
            if (a.base != (v & 0x03FFFFFF)) {
                a.base = v & 0x03FFFFFF;
                a.sentSize = 0;
            }
            return;
        }
        if (reg >= 0xB0 && reg <= 0xBF) s_arrays[reg - 0xB0].stride = v & 0xFF;
        else if (reg == 0x50) s_vcdLo = v;
        else if (reg == 0x60) s_vcdHi = v;
        else if ((reg & 0xF8) == 0x70) s_vatA[reg & 7] = v;
        else if ((reg & 0xF8) == 0x80) s_vatB[reg & 7] = v;
        else if ((reg & 0xF8) == 0x90) s_vatC[reg & 7] = v;
        else if (reg == 0x30) s_cpMtxA = v;
        fifo::write_data(p, len);
        return;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {  // indexed XF load from arrays 12-15
        const u32 n = 12 + (op - 0x20) / 8;
        const u32 idx = be16(p + 1), cnt = (be16(p + 3) >> 12) + 1;
        const u32 stride = s_arrays[n].stride ? s_arrays[n].stride : 4;
        const u32 dst = be16(p + 3) & 0xFFF;
        if (const u8* d = physPtr(s_arrays[n].base + idx * stride, cnt * 4)) {
            for (u32 i = 0; i < cnt && dst + i < kXfShadow; i++) {
                const u32 w = be32(d + 4 * i);
                s_xfMem[dst + i] = w;
                if (n == 12 && dst + i < 256) memcpy(&s_xfMtx[dst + i], &w, 4);  // position matrices
            }
            if (dst < kXfShadow) s_xfBlock[dst] = std::max(s_xfBlock[dst], u16(cnt));
        }
        sendArray(n, idx * stride + cnt * 4);
        fifo::write_data(p, len);
        return;
    }
    case 0x61: {  // BP register
        const u32 raw = be32(p + 1);
        const u8 reg = u8(raw >> 24);
        if (reg == kScreenSpaceMarker && (raw & 0xFFFFFE) == kScreenSpaceMagic) {
            s_screenSpaceWindow = raw & 1;  // hle_TargetingPass's bracket: not for the GPU (and
            return;                         // a pending BP mask stays for the game's next write)
        }
        if (reg == kScreenSpaceMarker && (raw & 0xFFFFFE) == kScreenSpaceAllMagic) {
            s_screenSpaceAllWindow = raw & 1;  // hle_ReticleDraw's bracket
            return;
        }
        if (reg == kScreenSpaceMarker && (raw & 0xFFFFFE) == kFillMagic) {
            s_fillWindow = raw & 1;  // hle_CutsceneVideoDraw's bracket
            return;
        }
        if (reg == 0xFE) {
            s_bpMask = raw & 0xFFFFFF;
            fifo::write_data(p, len);
            return;
        }
        const u32 v = (s_bp[reg] & ~s_bpMask) | (raw & s_bpMask & 0xFFFFFF);
        s_bpMask = 0xFFFFFF;
        s_bp[reg] = v;
        s_bpValid.set(reg);
        if ((reg >= 0x80 && reg <= 0x9B) || (reg >= 0xA0 && reg <= 0xBB)) {
            const u32 idx = reg & 0x1F;
            if (idx < 0x1C) s_tex[(idx & 3) + (reg >= 0xA0 ? 4 : 0)].dirty = true;
        } else if (reg == 0x59) {
            // Scissor box offset. Hardware: field = (offset + 340) / 2, and both the scissor
            // box and the primitives move by -offset in the EFB. Aurora only moves its scissor,
            // by 2 * a 9-bit signed field (its GXSetScissorBoxOffset writes (offset - 342) / 2),
            // which cannot hold this game's offsets (the bottom-half pass uses 238). So Aurora
            // always gets the fixed bias (-340) and the bridge applies the offset itself: to the
            // scissor box and, through the projection, to the geometry.
            const s32 offX = s32(2 * (v & 0x3FF)) - 340, offY = s32(2 * ((v >> 10) & 0x3FF)) - 340;
            if (offX != s_boxOffX || offY != s_boxOffY || !s_boxBiasSent) {
                s_boxOffX = offX;
                s_boxOffY = offY;
                s_boxBiasSent = true;
                const u32 f = (0u - 170u) & 0x3FF;
                bpFull(0x59000000u | (f << 10) | f);
                sendScissor();
                sendProjection();
            }
            return;
        } else if (reg == 0x20 || reg == 0x21) {  // scissor box: moved by the box offset
            sendScissor();
            return;
        } else if (reg == 0x65) {  // TLUT load into TMEM: remember it, bind at draw time
            s_tlutLoads[v & 0x3FF] = {(s_bp[0x64] & 0x1FFFFF) << 5, ((v >> 10) & 0x7FF) * 16};
            return;
        } else if (reg == 0x52) {
            if (s_seamPending) seamBlend();  // before the copy takes the background
            const bool disp = v & (1u << 14);
            // A display copy makes a picture only if it lands in the frame buffer the VI scans
            // out (front or back). The game builds each 640x450 XFB from two 640x226 copies
            // (top and bottom halves rendered in separate EFB passes), and also uses display
            // copies into a scratch buffer just to clear EFB bands: those go to Aurora as
            // texture copies, which keeps the clear without touching the picture.
            const u32 dest = (s_bp[0x4B] & 0xFFFFFF) << 5, stride = (s_bp[0x4D] & 0x3FF) * 32;
            const u32 h = ((s_bp[0x4A] >> 10) & 0x3FF) + 1;
            u32 line = 0, lines = 0;
            const bool shown = disp && viLocateCopy(dest, stride, line, lines);
            // Reaching the bottom completes the frame. So does a second copy to the top (a
            // frame that never reached the bottom was a whole picture after all), which bounds
            // how many game frames can pile into one of Aurora's. Battle of Hoth's intro copies
            // its first 304-line frame while the VI still scans 450 lines (the new VI setup
            // lands at the next retrace), the second into an XFB the VI does not show yet, and
            // only the third completes: ~54,000 draws in one frame (Aurora patch 0018).
            const bool completes = shown && (line + h >= lines || (line == 0 && s_topCopied));
            if (shown && line == 0) s_topCopied = true;
            if (disp && s_copyLog)
                logf("gfx", "display copy src %u,%u %ux%u stride %u -> %08X: %s line %u of %u (%u draws in the frame)", s_bp[0x49] & 0x3FF,
                     (s_bp[0x49] >> 10) & 0x3FF, (s_bp[0x4A] & 0x3FF) + 1, h, stride, dest, shown ? "XFB" : "not in XFB",
                     line, lines, s_frameDraws);
            static const bool presentAtTexCopy = getenv("R2_NO_XFB") && atoi(getenv("R2_NO_XFB")) == 2;
            if (!disp && presentAtTexCopy) {  // debugging: show the EFB as a texture copy sees it
                endFrame();
                ensureFrame();
            }
            if (disp && !shown) {
                const u32 tex = v & ~(1u << 14);
                prepareCopy(tex);
                fifo::write_u8(0x61);
                fifo::write_u32(0x52000000u | tex);
                fifo::publish();
                return;
            }
            if (completes && !s_xfb) {  // present the EFB as it is before the copy clears it
                endFrame();
                ensureFrame();
            }
            if (shown) {
                aur(GX_AURORA_LOAD_DISP_COPY_DST);
                fifo::write_u32(line);
                fifo::write_u32(lines);
            }
            prepareCopy(v);
            fifo::write_data(p, len);
            fifo::publish();
            if (completes && s_xfb) endFrame();  // the XFB is complete
            return;
        }
        // Debugging: R2_GX_OVERRIDE=nocull,zalways,aalways,nofog relaxes per-draw tests to find
        // what rejects missing geometry.
        static const char* ovr = getenv("R2_GX_OVERRIDE");
        if (ovr && (reg == 0x00 || reg == 0x40 || reg == 0xF3 || reg == 0xF1)) {
            u32 w = v;
            if (reg == 0x00 && strstr(ovr, "nocull") && ((w >> 14) & 3) != 3) w &= ~(3u << 14);  // not CULL_ALL
            if (reg == 0x40 && strstr(ovr, "zalways")) w |= 7u << 1;
            if (reg == 0xF3 && strstr(ovr, "aalways")) w = (w & ~(0xFFu << 16)) | (7u << 16) | (7u << 19);
            if (reg == 0xF1 && strstr(ovr, "nofog")) w &= ~(7u << 21);
            bpFull((u32(reg) << 24) | w);
            return;
        }
        fifo::write_data(p, len);
        // Draw done / draw sync tokens: the game may reuse the memory behind earlier draws once
        // it sees them (Aurora's worker reads arrays and textures when it gets to a draw)
        if ((reg == 0x45 && (v & 2)) || reg == 0x47 || reg == 0x48) gpSync();
        return;
    }
    case 0x10: {  // XF register load: shadow the viewport and projection (scissor box offset)
        const u32 hdr = be32(p + 1);
        const u32 addr = hdr & 0xFFFF, cnt = (len - 5) / 4;
        bool touched = false;
        if (addr < kXfShadow) s_xfBlock[addr] = std::max(s_xfBlock[addr], u16(cnt));  // (the longest: it covers most)
        for (u32 i = 0; i < cnt; i++) {
            const u32 a = addr + i, w = be32(p + 5 + 4 * i);
            if (a < kXfShadow) s_xfMem[a] = w;
            if (a < 0x100) {
                memcpy(&s_xfMtx[a], &w, 4);
            } else if (a >= 0x101A && a <= 0x101F) {
                s_xfVp[a - 0x101A] = w;
                touched = true;
            } else if (a >= 0x1020 && a <= 0x1026) {
                s_xfProj[a - 0x1020] = w;
                s_projKnown = true;
                touched = true;
            }
        }
        fifo::write_data(p, len);
        if (touched && (s_boxOffX || s_boxOffY)) sendProjection();
        return;
    }
    case 0x40:  // CALL_DL: inlined by the CP model
        return;
    default:
        fifo::write_data(p, len);
        return;
    }
}

#ifdef _WIN32
void sendToBack(void* hwnd) {
    constexpr unsigned SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10;
    SetWindowPos(hwnd, reinterpret_cast<void*>(1) /* HWND_BOTTOM */, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    // Out of the way entirely: left of every monitor and without a taskbar button (a tool
    // window), so a test run cannot be clicked, closed or minimized by someone working at the
    // machine. DWM still renders it, and PrintWindow still captures it. R2_ONSCREEN=1 keeps it
    // visible.
    if (envFlag("R2_ONSCREEN")) return;
    constexpr int GWL_EXSTYLE = -20, SM_XVIRTUALSCREEN = 76, SW_HIDE = 0, SW_SHOWNOACTIVATE = 4;
    constexpr long long WS_EX_TOOLWINDOW = 0x80, WS_EX_APPWINDOW = 0x40000;
    ShowWindow(hwnd, SW_HIDE);  // (the taskbar only drops the button of a hidden window)
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
    SetWindowPos(hwnd, reinterpret_cast<void*>(1), GetSystemMetrics(SM_XVIRTUALSCREEN) - 8000, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
#else
void sendToBack(void*) {  // (Windows only: test windows out of the way)
#endif
}

void logCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const char* names[] = {"debug", "info", "warn", "error", "fatal"};
    static const bool debug = envFlag("R2_AURORA_DEBUG");
    if (level == LOG_DEBUG && !debug) return;
    logf("aurora", "%s %s: %.*s", names[level], module, int(len), message);
    if (level == LOG_FATAL) fatal("Aurora: %.*s", int(len), message);
}

}  // namespace

bool init(int argc, char** argv, std::string& disc, const std::string& discNote) {
    AuroraConfig cfg{};
    cfg.appName = "Red Two";
    // Pipeline cache and other Aurora files: %APPDATA%\Red Two for the player. Automated runs
    // (R2_BACKGROUND, R2_HEADLESS or R2_INPUT) keep their own in build/test_cache, so debugging
    // overrides never leave pipelines in the player's cache (Aurora warms the whole cache at
    // startup: one bad entry crashed every start). R2_CACHE_DIR picks any directory.
    static std::string cacheDir;
    if (const char* dir = getenv("R2_CACHE_DIR")) cacheDir = dir;
    else if (envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT")) cacheDir = "build/test_cache/";
    else if (mods::portable()) cacheDir = mods::userDir();  // (else Aurora's own: %APPDATA%\Red Two, the same)
    if (!cacheDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        cfg.cachePath = cacheDir.c_str();
        cfg.userPath = cacheDir.c_str();  // controller/keyboard bindings (input.json) too
    }
    cfg.desiredBackend = BACKEND_AUTO;
    cfg.msaa = settings::get().msaa >= 4 ? 4 : 1;  // (WebGPU: 1 or 4 samples)
    cfg.vsync = settings::get().vsync;  // (the game paces itself on its emulated retrace either way)
    s_testClear = envFlag("R2_GFX_TEST");
    if (const char* d = getenv("R2_GX_DUMP")) {
        char* end = nullptr;
        if (*d == 't') {
            end = const_cast<char*>(d);
            while (*end == 't') {
                s_dumpTimes.push_back(u64(strtod(end + 1, &end) * double(kTbClock)));
                if (*end == ';' || *end == '+') end++;
            }
            s_dumpAtTb = s_dumpTimes.front();
            s_dumpTimes.erase(s_dumpTimes.begin());
        } else {
            s_dumpFrame = strtoull(d, &end, 10);
        }
        if (end && *end == ',') s_dumpCount = strtoull(end + 1, nullptr, 10);
    }
    s_xfb = !envFlag("R2_NO_XFB");
    s_copyLog = envFlag("R2_COPY_LOG");
    cfg.emulateXfb = s_xfb;
    cfg.windowWidth = 1280;
    cfg.windowHeight = 960;
    // (Linux desktops match the window to red-two.desktop - the AppImage's - by this id)
    SDL_SetHint(SDL_HINT_APP_ID, "red-two");
    // smaller screens (the Steam Deck's 1280x800 desktop): a 4:3 window that fits its usable area
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.h > 0 && usable.h < 1000) {
            const int h = std::max(480, usable.h - 60), w = h * 4 / 3;
            cfg.windowWidth = u32(std::min(w, std::max(640, usable.w)));
            cfg.windowHeight = u32(h);
        }
    }
    if (const char* ws = getenv("R2_WINDOW")) {  // R2_WINDOW=<w>x<h> (screenshots)
        unsigned w = 0, h = 0;
        if (sscanf(ws, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) {
            cfg.windowWidth = w;
            cfg.windowHeight = h;
        }
    }
    cfg.windowPosX = -1;
    cfg.windowPosY = -1;
    cfg.logCallback = logCallback;
    cfg.imGuiInitCallback = [](const AuroraWindowSize* size) { menuImGuiInit(size); };
    // Controllers keep working when the window is not focused (SDL drops their events
    // otherwise, and a window started from another program often opens unfocused). Not for
    // automated runs: someone's controller must not steer a scripted test (R2_BG_PADS=0/1
    // overrides).
    const char* bgPads = getenv("R2_BG_PADS");
    cfg.allowJoystickBackgroundEvents = bgPads ? *bgPads == '1'
                                               : !envFlag("R2_BACKGROUND") && !envFlag("R2_HEADLESS") && !getenv("R2_INPUT");
    cfg.logLevel = envFlag("R2_AURORA_DEBUG") ? LOG_DEBUG : LOG_INFO;
    {
        const char* t = getenv("R2_GX_THREAD");
        s_gxThread = !t || *t != '0';
    }
    cfg.gxProcessingMode = s_gxThread ? GX_PROCESSING_THREAD : GX_PROCESSING_INLINE;
    cfg.mem1Size = 0;
    cfg.mem2Size = 0;
    if (envFlag("R2_HEADLESS")) cfg.surface.type = AURORA_SURFACE_HEADLESS;
    cfg.startFullscreen = settings::get().fullscreen;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--fullscreen")) cfg.startFullscreen = settings::get().fullscreen = true;
    // R2_BACKGROUND: automated runs - the window opens behind everything without taking focus
    // (it still renders, and PrintWindow can capture it).
    const bool background = envFlag("R2_BACKGROUND");
    if (background) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
    }
    // Controllers are updated on the input thread (input.cpp): SDL's device scans take seconds
    // on some machines, and the event pump would run them on the game's thread.
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
#ifdef R2_HAS_ICON
    // the icon built into the exe (res/red_two.rc) for the window and taskbar too
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
    // The ubershader mode before Aurora loads the pipeline cache: with them off, the uber
    // pipelines are neither derived from the cached shaders nor built from their stored rows
    // (patch 0028) - Aurora's own default is on, and applyDisplaySettings below comes too late.
    AuroraSetUberShaders(uberShaderMode());
    // OpenXR (--xr / R2_XR=1): the headset's adapter, on D3D12 (gfx/xr.cpp)
    const bool xrWanted = xr::preinit(argc, argv);
    if (xrWanted) cfg.desiredBackend = BACKEND_D3D12;
    const AuroraInfo info = aurora_initialize(argc, argv, &cfg);
    if (xrWanted) xr::start();
    if (background) {
        if (SDL_Window* w = aurora_get_window()) {
            if (void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                sendToBack(hwnd);
        }
    }
    applyDisplaySettings();  // (also the ubershader mode)
    mods::texturesInit();  // texture packs (Dolphin's format) into Aurora's replacement registry
    buttonIconsUpdate();   // Xbox button prompts (button_icons.cpp)
    static const char* backends[] = {"auto", "D3D11", "D3D12", "Metal", "Vulkan", "OpenGL", "OpenGL ES", "WebGPU", "null"};
    logf("gfx", "Aurora up: %s backend, window %ux%u, display %d Hz, V-Sync %s (frame rate %d)", backends[info.backend],
         info.windowSize.width, info.windowSize.height, displayRefreshHz(), settings::get().vsync ? "on" : "off",
         settings::get().fps);
    input::init(true);  // (before the launcher: the controllers drive it)
    // the launcher; also when there is no usable disc (it asks for one: setting off or not)
    launcherSetDisc(disc, discNote);
    if (disc.empty() || launcherWanted(argc, argv)) {
        if (disc.empty()) logf("disc", "none found: the launcher asks for it%s%s", discNote.empty() ? "" : ": ", discNote.c_str());
        runLauncher();
        disc = launcherDisc();
    }
    precompileShaders();
    s_enabled = true;
    gxSetSink(onCommand);
    gxSetSyncHook(gpSync);
    return true;
}

// R2_RESIZE="<seconds>:<w>x<h>[,...]": resize the window at those times (tests of what a
// player's resizing does, e.g. with the aspect matching the window)
static void scriptedResize() {
    struct Resize {
        u64 tb;
        int w, h;
    };
    static std::vector<Resize> list = [] {
        std::vector<Resize> v;
        if (const char* e = getenv("R2_RESIZE")) {
            for (const char* p = e; *p;) {
                double s = 0;
                int w = 0, h = 0;
                if (sscanf(p, "%lf:%dx%d", &s, &w, &h) == 3 && w >= 160 && h >= 120)
                    v.push_back({u64(s * double(kTbClock)), w, h});
                const char* comma = strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
        }
        return v;
    }();
    if (list.empty()) return;
    static const u64 start = tbNow();
    if (tbNow() - start < list.front().tb) return;
    if (SDL_Window* w = aurora_get_window()) {
        SDL_SetWindowSize(w, list.front().w, list.front().h);
        logf("gfx", "R2_RESIZE: window %dx%d", list.front().w, list.front().h);
    }
    list.erase(list.begin());
}

void vblank() {
    s_vblanks++;
    if (!s_enabled) return;
    scriptedResize();
    hotkeys::tick();  // (before the game's input: a hotkey's buttons are muted for it)
    input::poll();
    // Keep the window responsive (and closable) while the game is not presenting: loading
    // screens, or a frame that never ends. Polling only reads SDL events, so it is safe mid-frame.
    if (tbNow() - s_lastPollTb > kTbClock / 10) pollEvents();
}

u64 frames() { return s_frames; }

std::string stats() {
    const AuroraStats* st = aurora_get_stats();
    if (!st) return {};
    char b[320];
    snprintf(b, sizeof b,
             "aurora: %u draws (%u merged), pipelines %u queued %u created, %u skipped %u uber draws, %u KiB verts, "
             "%u KiB tex upload; arrays sent: %llu (%llu MiB)",
             st->drawCallCount, st->mergedDrawCallCount, st->queuedPipelines, st->createdPipelines, st->skippedDraws,
             st->uberDraws, st->lastVertSize / 1024,
             st->lastTextureUploadSize / 1024, (unsigned long long)s_arraySends, (unsigned long long)(s_arrayBytes >> 20));
    s_arraySends = s_arrayBytes = 0;
    std::string out = b;
    if (mods::loadedTextureCount()) out += "; " + mods::textureStats();
    return out;
}

}  // namespace rt::gfx

// --- save states ----------------------------------------------------------------------------
// The register state the stream built (BP, CP, XF as loaded, palette loads) is part of a state.
// After a load it goes to Aurora again through onCommand, as the game's own writes would
// (actions left out: EFB copies, palette loads, draw done and tokens), so Aurora's state, which
// belongs to the session's own timeline, matches the restored game. Textures and arrays are
// sent again at their next use; one frame may mix the two timelines.
namespace rt::gfx {
namespace {
struct TlutRec {
    u32 tmem, addr, entries;
};
bool replayedBp(u32 reg) {
    switch (reg) {
    case 0x45: case 0x47: case 0x48:  // draw done, tokens
    case 0x52:                        // EFB copy
    case 0x64: case 0x65: case 0x66:  // palette load, texture cache invalidation
    case 0x67:                        // performance counters
    case kScreenSpaceMarker:
    case 0xFE:                        // mask
        return false;
    default:
        return true;
    }
}
}  // namespace

void gpuDrain() { gpSync(); }

void bridgeSaveState(state::Writer& w) {
    w.section("GFX ");
    w.pod(s_bp);
    w.pod(s_bpValid);
    w.pod(s_bpMask);
    w.pod(s_cp);
    w.pod(s_cpValid);
    w.pod(s_xfMem);
    w.pod(s_xfBlock);
    std::vector<TlutRec> tluts;
    for (const auto& [tmem, ld] : s_tlutLoads) tluts.push_back({tmem, ld.addr, ld.entries});
    w.vec(tluts);
    w.pod(s_screenSpaceWindow);
    w.pod(s_fillWindow);
}

void bridgeLoadState(state::Reader& r) {
    r.section("GFX ");
    r.pod(s_bp);
    r.pod(s_bpValid);
    r.pod(s_bpMask);
    r.pod(s_cp);
    r.pod(s_cpValid);
    r.pod(s_xfMem);
    r.pod(s_xfBlock);
    std::vector<TlutRec> tluts;
    r.vec(tluts);
    s_tlutLoads.clear();
    for (const TlutRec& t : tluts) s_tlutLoads[t.tmem] = {t.addr, t.entries};
    r.pod(s_screenSpaceWindow);
    r.pod(s_fillWindow);
}

void bridgeAfterLoad() {
    if (!s_enabled) return;
    const u32 mask = s_bpMask;
    std::vector<u8> cmd;
    auto put32 = [&](u32 v) {
        for (int s = 24; s >= 0; s -= 8) cmd.push_back(u8(v >> s));
    };
    for (u32 reg = 0; reg < 256; reg++) {  // CP (array bases included: the bridge keeps those)
        if (!s_cpValid[reg]) continue;
        cmd = {0x08, u8(reg)};
        put32(s_cp[reg]);
        onCommand(cmd.data(), u32(cmd.size()));
    }
    for (u32 a = 0; a < kXfShadow; a++) {  // XF, in the blocks the game loaded
        const u32 n = std::min<u32>(s_xfBlock[a], kXfShadow - a);
        if (!n) continue;
        cmd = {0x10};
        put32(((n - 1) << 16) | a);
        for (u32 i = 0; i < n; i++) put32(s_xfMem[a + i]);
        onCommand(cmd.data(), u32(cmd.size()));
    }
    for (u32 reg = 0; reg < 256; reg++) {  // BP
        if (!s_bpValid[reg] || !replayedBp(reg)) continue;
        cmd = {0x61};
        put32((reg << 24) | (s_bp[reg] & 0xFFFFFF));
        onCommand(cmd.data(), u32(cmd.size()));
    }
    sendScissor();
    sendProjection();
    if (mask != 0xFFFFFF) {  // the game's pending BP mask (its next write takes it)
        s_bpMask = mask;
        fifo::write_u8(0x61);
        fifo::write_u32(0xFE000000u | mask);
    }
    for (Array& a : s_arrays) a.sentSize = 0;
    for (TexMap& t : s_tex) t.dirty = true;
    for (TlutSlot& t : s_tlutSlots) t = TlutSlot{};
    for (auto& [key, t] : s_texInfo) {
        t.frame = ~0ull;
        t.hashTb = 0;
    }
    setMenuTiling(false);
    logf("state", "renderer state replayed");
}

}  // namespace rt::gfx

namespace rt {
double frameRateHz() { return gfx::retraceHz(); }
// A cinematic skip runs the game flat out: frames must not wait for the display meanwhile.
void presentPaced(bool on) { aurora_enable_vsync(on && settings::get().vsync); }
}  // namespace rt
