// Game-side hooks (hooks.txt) for the port's options: frame rate, skips, cheats (settings.h).
#include "funcs.h"
#include "runtime.h"
#include "discord.h"
#include "gyro.h"
#include "gfx/xr.h"
#include "settings.h"
#include "hw/gx_fifo.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>

namespace rt {

// GameFrameDelta (0x7FD46244): the seconds the frame advances the game by, stored at 0x7FDE8288
// by the per-frame update (0x7FC02CF0) of every main loop. The game computes
// 1 / (59.94 - lost) with `lost` the frames per second it measured missing (0x7FE0C6A4,
// from its retrace callback 0x7FD46A10, capped at 29.97): 1/59.94 s when it keeps up. At a
// higher retrace rate (settings fps) it would keep stepping 1/59.94 s per frame and run fast,
// so there the delta is the real time since the previous frame.
static void frameDelta(Ctx& c) {
    f_7FD46244_orig(c);
    const double hz = viRate();
    if (hz < 60.5) return;  // 59.94 Hz: the game's own measure
    // Real time rather than retraces: those can fire late when the host is busy, which would
    // run the game a few percent slow. Capped like the game's own measure (1/29.97 s), e.g.
    // after a load; no real lower bound, or short frames (two updates in one, as loading
    // screens' 0x7FC03BC4 can do) would add time and run the game fast.
    static u64 lastTb = 0;
    const u64 tb = tbNow();
    const double dt = lastTb ? double(tb - lastTb) / double(kTbClock) : 1.0 / hz;
    lastTb = tb;
    c.f[1] = c.ps1[1] = rsingle(std::clamp(dt, 0.00025, 1.0 / 29.97));
}

// The player's craft (0x7FDEFE14 in missions), or 0 when that pointer holds something else:
// a craft's vtable (+0x80) has CraftDestroy (0x7FC3E528).
static u32 s_craftLast = 0;  // playerCraft's cache (a state load clears it)
static bool s_craftLastIsCraft = false;
static u32 playerCraft() {
    u32& last = s_craftLast;
    bool& lastIsCraft = s_craftLastIsCraft;
    const u32 p = rd32(0x7FDEFE14);
    if (p != last) {
        last = p;
        lastIsCraft = false;
        const u32 vt = p ? rd32(p + 0x80) : 0;
        if (vt >= 0x7F000000 && vt < 0x81800000)
            for (u32 o = 0; o < 0x400 && !lastIsCraft; o += 4) lastIsCraft = rd32(vt + o) == 0x7FC3E528;
    }
    return lastIsCraft ? p : 0;
}

// R2_GYRO_LOG: twice a second the player's craft's turn rate - the angle of its rotation (3x3 at
// +0x84) since the last line, whichever way the matrix is stored (trace of R1 R0^T) - against what
// the gyro sent (gyro.cpp logs that): to measure the turn rate a stick deflection gives
static void gyroProbe(u32 craft, bool flying) {
    static const bool log = envFlag("R2_GYRO_LOG");
    if (!log) return;
    static float last[9];
    static u64 lastTb = 0;
    const u64 tb = tbNow();
    if (!craft || !flying) {
        lastTb = 0;
        return;
    }
    if (lastTb && tb - lastTb < kTbClock / 2) return;
    float m[9];
    for (int i = 0; i < 9; i++) {
        const u32 w = rd32(craft + 0x84 + u32(i) * 4);
        memcpy(&m[i], &w, 4);
    }
    if (lastTb) {
        double tr = 0;
        for (int i = 0; i < 9; i++) tr += double(m[i]) * double(last[i]);
        const double ang = std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0)) * 57.29577951;
        const double dt = double(tb - lastTb) / double(kTbClock);
        // (rows: right, up, forward - heading atan2(m6, m8), pitch asin(m7); y is height)
        logf("gyro", "craft turns %.1f deg/s; m %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f", ang / dt, m[0], m[1],
             m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
    }
    memcpy(last, m, sizeof m);
    lastTb = tb;
}

// Skippable cutscenes (settings): the game has no skip for the crawl and most cutscenes, so a
// press of B in a letterboxed cinematic (the VI shows 304 lines there, 450 in flight) runs the
// game flat out until play resumes: retraces at kSkipHz, kSkipStep game seconds per frame
// (8x a 60 fps frame: what cinematics were tested at), nothing drawn (gx_bridge.cpp), the
// audio clock (AI DMA -> MusyX: sequencer, voices, streams) as fast and silent, disc reads
// faster so streams keep up. It ends once the player's craft flies (state +0x370 = 0) in a
// frame that is not letterboxed: between the crawl and the mission intro the game shows full
// frames for a moment. Outside missions, after 3 s of full frames. And after kSkipMaxSeconds in
// any case (issue #38): a skipped scene that leads into a letterboxed screen waiting for a button
// never shows full frames, and the skip ran on at 600 Hz behind "Skipping..." until the player
// closed the game. Skips take 0.7-4.5 s (a crawl and a mission intro ~3 s; #38's log).
constexpr double kSkipStep = 8.0 / 60.0;
constexpr int kSkipHz = 600;
constexpr double kSkipMaxSeconds = 10.0;
static bool s_skip = false;
bool cinematicSkip() { return s_skip; }

// The game has a skip of its own for scenes seen before (this session): SceneSkipAllowed
// (0x7FD56FD8; scene id r3) returns A pressed if SceneSeen (0x7FD56E5C) says so, scenes 13 and 23
// always, and in game mode 5 (the attract demo) as its exit. The scene player (0x7FD544B8,
// 0x7FD5540C, 0x7FD554DC) then goes on to the next scene, not to play: on a mission's second crawl
// the next cutscene's music started about 2 s after the press while the crawl still scrolled,
// the crawl faded a second later, and the cutscene opened on several seconds of stars before its
// voices - it looked like the crawl went on under the next scene's sound. With skippable cutscenes
// on, a skip the game allows starts ours too (as B does), straight to play.
static bool s_gameSkipped = false;
void hle_SceneSkipAllowed(Ctx& c) {
    f_7FD56FD8_orig(c);
    if ((c.r[3] & 0xFF) && rd8(0x800962EF) != 5) s_gameSkipped = true;  // r13-19121: game mode
}

// Skip intro logos (settings): the boot screen (mode 5, screen 22 at 0x7FDE8254/0x7FDE822C, see
// the top-level loop 0x7FC00030) shows the legal notice, checks the memory card, then plays the
// 3D LucasArts intro and the logos: 16x and silent, still drawn (the card dialogs may need an
// answer), its logic (card checks, loads) as it is.
static bool bootScreen() { return rd32(0x7FDE8254) == 5 && rd32(0x7FDE822C) == 22; }

static void keepHealth();
static void keepAmmo();
static void craftDiffProbe();
static void resultsTest(Ctx& c);
static void unlocksFrame();

void hle_GameFrameDelta(Ctx& c) {
    frameDelta(c);
    const settings::Settings& s = settings::get();
    static u64 lastTb = 0;  // real time since the previous frame
    const u64 tb = tbNow();
    const double real = lastTb ? std::max(double(tb - lastTb) / double(kTbClock), 0.0001) : 1.0 / 60.0;
    lastTb = tb;

    const bool cinematic = viXfbLines() < 400;
    const bool b = (padState(0).buttons & 0x0200) != 0;  // PAD_BUTTON_B
    static bool bWas = false;
    const bool bPressed = b && !bWas;
    bWas = b;
    const bool gameSkipped = s_gameSkipped;
    s_gameSkipped = false;
    if (!s_skip && s.skipCutscenes && cinematic && (bPressed || gameSkipped)) {
        s_skip = true;
        logf("skip", "skipping to the next gameplay%s", bPressed ? "" : " (the game's own skip of a scene seen before)");
        viSetRate(kSkipHz);
        presentPaced(false);  // (V-Sync would hold the skip to the display's refresh)
        setDvdBoost(50.0);
    }
    static double fullFor = 0;  // real seconds of frames that are not letterboxed while skipping
    fullFor = s_skip && !cinematic ? fullFor + real : 0;
    static double skipFor = 0;  // real seconds since the skip started
    skipFor = s_skip ? skipFor + real : 0;
    const u32 craft = playerCraft();
    static int flyingFrames = 0;  // (the craft passes through state 0 while it is set up)
    flyingFrames = !cinematic && craft && rd32(craft + 0x370) == 0 ? flyingFrames + 1 : 0;
    const bool skipTooLong = s_skip && skipFor > kSkipMaxSeconds;
    if (s_skip && (!s.skipCutscenes || flyingFrames >= 10 || fullFor > 3.0 || skipTooLong)) {
        s_skip = false;
        if (skipTooLong)
            logf("skip", "stopped after %.0f s: still %s, no flying craft (a screen waiting for a button?); "
                         "the game goes on at normal speed",
                 skipFor, cinematic ? "letterboxed" : "in full frames");
        else
            logf("skip", "back to play after %.1f s", skipFor);
        viSetRateHz(frameRateHz());
        presentPaced(true);
        setDvdBoost(1.0);
    }
    if (s_skip) {
        c.f[1] = c.ps1[1] = rsingle(kSkipStep);
        setAudioSpeed(std::min(kSkipStep / real, 64.0), true);
    } else if (s.skipLogos && bootScreen()) {
        c.f[1] = c.ps1[1] = rsingle(c.f[1] * 16.0);
        setAudioSpeed(16.0, true);
    } else {
        setAudioSpeed(1.0, false);
    }
    // VR haptics (both Touch controllers): firing - faint for a laser shot, hard for a secondary
    // weapon (torpedoes, missiles, bombs) - from the mission's stats (0x7FDF04F0, see
    // hle_MissionResults: +0x00 laser shots fired, +0x04 the others, a death: hle_CraftDestroy);
    // damage from the craft's health (+0x6F4, read before the invincibility
    // cheat fills it back), stronger the bigger the hit.
    {
        const bool play = rd32(0x7FDE8668) == 0 && !s_skip && !cinematic;
        static u32 lasers = 0, others = 0, hpCraft = 0;
        static float hp = 0.f, hpFull = 0.f;
        constexpr u32 kStats = 0x7FDF04F0;
        const u32 l = rd32(kStats), o = rd32(kStats + 4);
        if (play && flyingFrames > 1) {
            if (o > others && o - others < 16) xr::haptic(1.0f, 0.4f);
            else if (l > lasers && l - lasers < 16) xr::haptic(0.12f, 0.03f);
        }
        lasers = l;
        others = o;
        const bool flying = craft && rd32(craft + 0x370) == 0;
        const float h = flying ? float(s2d(rd32(craft + 0x6F4))) : 0.f;
        if (!flying || craft != hpCraft || !(h > 0.f && h <= 100000.f)) {
            hpCraft = flying ? craft : 0;
            hp = hpFull = flying && h > 0.f && h <= 100000.f ? h : 0.f;
        } else {
            if (play && hpFull > 0.f && h < hp - 0.01f) {
                const float hit = (hp - h) / hpFull;  // (a fraction of the craft's full health)
                xr::haptic(std::clamp(0.4f + hit * 3.f, 0.4f, 0.9f), std::clamp(0.1f + hit * 1.5f, 0.1f, 0.35f));
            }
            hp = h;
            hpFull = std::max(hpFull, h);
        }
    }
    keepHealth();
    keepAmmo();
    // R2_MISSION_WIN=<seconds>[:<n>[:<r>]] (tests: a mission's end without flying it, e.g. Strike at
    // the Core's finale, issues #33 / #50): once the player's craft has flown that long, what the
    // mission script's end-mission action (0x2F, ScriptRun's table entry 0x7FC96128) does with a
    // record {2F, 01, r, n}: game object 0x7FDE8000 +0x271 = (r == 0), +0x274 = n + 1, +0x21A = 1.
    // r = 0 is a failure (+0x274: the reason, "FrntEnd_missionFailed_level10_<n+1>"; Strike at the
    // Core's 2F 01 00 00 = "The Millennium Falcon was destroyed"), r = 1 a win.
    if (static const char* win = getenv("R2_MISSION_WIN"); win && *win) {
        static double flown = 0;
        static bool done = false;
        flown = craft && !cinematic && rd32(craft + 0x370) == 0 ? flown + real : flown;
        if (!done && flown >= atof(win)) {
            done = true;
            u32 n = 0, r = 0;
            if (const char* c1 = strchr(win, ':')) {
                n = u32(atoi(c1 + 1));
                if (const char* c2 = strchr(c1 + 1, ':')) r = u32(atoi(c2 + 1));
            }
            constexpr u32 kGame = 0x7FDE8000;
            if (rd8(kGame + 0x21A) == 0) {
                wr8(kGame + 0x271, r == 0 ? 1 : 0);
                wr32(kGame + 0x274, n + 1);
                wr8(kGame + 0x21A, 1);
                logf("test", "R2_MISSION_WIN: end-mission action {2F 01 %02X %02X} after %.0f s of flight", r, n, flown);
            }
        }
    }
    craftDiffProbe();
    resultsTest(c);
    unlocksFrame();
    // widescreen: the flight HUD goes to the screen edges (not the menus: their layout is 4:3).
    // Nor the pause screen, drawn while the craft exists: its objectives' check marks and dots
    // sit left of the centered text and were moved out to the left (by the 4:3 margin, 240 px
    // at 1080p). GameState 0x7FDE8668 (setter 0x7FC03CD8): 0 in play, 2 paused (0x7FCD95D4,
    // back to 0 at 0x7FCD96B8), 3 in front-end loops; found diffing dumps flying / paused.
    const bool inPlay = rd32(0x7FDE8668) == 0;
    // gyro aiming steers only the craft in flight: menus, the pause screen and cutscenes read the
    // main stick too
    gyro::setFlight(flyingFrames > 0 && inPlay && !s_skip);
    xr::setFlight(flyingFrames > 0 && inPlay && !s_skip);  // (VR: stereo 3D only in flight)
    gyroProbe(craft, flyingFrames > 0);
    gfx::setHudAnchoring(s.hudEdges && s.aspect != settings::Aspect::R4_3 && playerCraft() && !cinematic && inPlay);
    // menu screens only (no player craft; not the boot screen's intro and logos, whose 2D draws
    // also look like a run of panels, nor letterboxed cinematics): the background's panels
    // repeat into the sides
    gfx::setMenuTiling(s.tileMenus && !playerCraft() && !bootScreen() && !cinematic);
    // Discord Rich Presence (discord.cpp names the mission from the level the game loaded): the
    // front end (the boot screen, GameState 3 in its loops, game mode 5 - byte r13-19121 =
    // 0x800962EF, tested by the attract demo's exit 0x7FD79648), paused (GameState 2 with the
    // craft: checked first, the pause screen may be letterboxed), a letterboxed cinematic (the
    // crawl, briefings, cutscenes; also while the port skips one), flying (the player's craft
    // exists), else none (loading, the results).
    {
        using discord::Phase;
        const u32 gameState = rd32(0x7FDE8668);
        Phase phase = Phase::None;
        if (bootScreen() || rd8(0x800962EF) == 5) phase = Phase::FrontEnd;
        else if (craft && gameState == 2 && !s_skip) phase = Phase::Paused;
        else if (cinematic || s_skip) phase = Phase::Cutscene;
        else if (gameState == 3) phase = Phase::FrontEnd;
        else if (craft) phase = Phase::Flying;
        discord::gameFrame(phase);
    }
    // R2_DT_LOG: every 10 s, frames and the game time they advanced against the wall clock
    static const bool dtLog = envFlag("R2_DT_LOG");
    if (!dtLog) return;
    static u64 logTb = 0, frames = 0;
    static double simulated = 0;
    if (!logTb) logTb = tb;
    frames++;
    simulated += c.f[1];
    if (tb - logTb >= 10 * kTbClock) {
        logf("dt", "%llu frames, %.2f s game time in %.2f s", (unsigned long long)frames, simulated,
             double(tb - logTb) / kTbClock);
        logTb = tb;
        frames = 0;
        simulated = 0;
    }
}

// Widescreen at the source. Aurora widens perspective projections on a wider target, but the
// game culls with its own 4:3 view volume: the world camera (0x7FCDA190) passes 4/3 from its
// small-data constant to the camera setup 0x7FC14CEC, which keeps the aspect (camera +0x12C),
// derives the horizontal field of view (+0x130) and builds the projection from them
// (0x7FD43668 -> MTXPerspective). Each camera function has its own copy of the constant; the
// ones below take the display aspect, so the game renders and culls for it. Aurora leaves
// projections that are already wide alone (Aurora patch 0009).
static std::atomic<float> s_displayAspect{4.f / 3.f};
// Aspects written into the constants before (the window was resized, or the setting changed):
// camera objects copied the default when they were created and keep passing the old value.
static std::atomic<float> s_oldAspects[8];
static std::atomic<u32> s_oldAspectCount{0};
constexpr u32 k43 = 0x3FAAAAAB;  // 4/3
static u32 s_aspectWritten = k43;  // the value setGameAspect last put into the constants

static bool oldGameAspect(double aspect) {
    const u32 n = std::min<u32>(s_oldAspectCount.load(std::memory_order_relaxed), 8);
    for (u32 i = 0; i < n; i++)
        if (std::abs(aspect - double(s_oldAspects[i].load(std::memory_order_relaxed))) < 0.0005) return true;
    return false;
}

void setGameAspect(double aspect) {
    const float previous = s_displayAspect.exchange(float(aspect), std::memory_order_relaxed);
    if (previous != float(aspect) && std::abs(previous - 4.0 / 3.0) > 0.0005 && !oldGameAspect(previous)) {
        const u32 i = s_oldAspectCount.fetch_add(1, std::memory_order_relaxed);
        s_oldAspects[i % 8].store(previous, std::memory_order_relaxed);
    }
    // (The flight camera culls with the camera-object default. HUD cameras built from it, such
    // as the radar globe's, aim off-center; Aurora keeps that offset in the 4:3 HUD layout.
    // 0x8009935C, the HUD's own MTXPerspective calls, stays 4:3.)
    static const u32 kCameraAspect[] = {
        0x80096C64,  // 0x7FC150B0: camera object defaults (+0x274)
        0x80099444,  // 0x7FCD3C38: camera object (+0x58)
        0x800994F4,  // 0x7FCD771C
        0x80099548,  // 0x7FCDA190: the world camera
    };
    u32& written = s_aspectWritten;
    const float f = float(aspect);
    u32 bits;
    memcpy(&bits, &f, 4);
    if (bits == written) return;
    for (u32 a : kCameraAspect) {
        const u32 cur = rd32(a);
        if (cur == k43 || cur == written) wr32(a, bits);
        else logf("wide", "camera aspect at %08X is %08X, left alone", a, cur);
    }
    written = bits;
    logf("wide", "game camera aspect %.4f", aspect);
}

// CameraSetup (0x7FC14CEC; view r3, matrix r4, aspect f1, vertical fov f2 in degrees) derives a
// camera's culling volume - a pyramid from the camera to the far plane's corners (+0x138..+0x168,
// side planes after them; horizontal fov +0x130) - from the aspect and fov its camera object
// holds (+0x274/+0x270), and keeps them (+0x12C, +0x134) for the projection 0x7FC153C0 builds
// next. Not every camera takes the patched constants above: the LucasArts intro's has its own
// 1.3333, letterboxed cinematics and the hangar use 2.105 (4/3 * 480/304) and 4/3. Aurora shows
// such a projection opened up to fill the screen (patch 0009, widescreen_scale): with k = display
// aspect / (4/3) and v = 480 / the XFB's lines in a letterboxed frame (1 otherwise), r = k / v
// opens it r times wider if r > 1, 1/r times taller if r < 1.
//
// So the culling volume is computed for that opened view, and outside free flight (intro,
// menus, the hangar, cinematics) kSceneMargin times wider and taller still for objects (the
// frustum planes): objects whose bounds sit just outside the edge while part of them shows (a
// trooper's feet below the screen) no longer drop out. The margin stays small and the
// horizontal fov (+0x130, which e.g. the terrain grid walk 0x7FC269C4 sizes its area by) stays
// the screen's: a 3x margin overflowed the game's per-frame buffers (heap chunk lists freed
// with bad pointers in the logos and the menus). In missions the flight camera (already at the
// display aspect: setGameAspect) and the rest take kTerrainMargin, the horizontal fov included
// (the terrain, below).
// The projection: a full frame opened sideways gets the game's aspect widened r times with the
// game's fov (4:3 -> the display's aspect; a cinematic camera's 2.105 drawn inside a full
// 450-line frame, like the take-off, -> 2.105 r), which Aurora shows as it is (the same
// picture; anything else the game tests against the projection sees the whole screen);
// otherwise (taller, letterboxed) the game's own aspect and fov, which Aurora opens. The
// camera's own copy of the projection (+0xA4) follows: the game places the engine glows with
// it. R2_GAME_CULLING=1 keeps the game's own volumes, R2_CULL_MARGIN=<f> sets the margin (1 =
// exactly the screen), R2_CAM_LOG=1 logs the camera setups.
constexpr double kSceneMargin = 1.2;
constexpr double kTerrainMargin = 1.2;

// CameraHFovAtan (0x7FC8DE3C; only caller CameraSetup, 0x7FC14D30): atan of f1 clamped to
// [-1, 1]. CameraSetup takes the horizontal fov (+0x130) as 2 * this(aspect * tan(fovY / 2)), so
// it never passes 90 degrees, and the far-plane corners and side planes derived from it neither:
// plenty at 4:3 (the cockpit's 55-degree view is 69.5 wide), but the cockpit at 2.13:1 shows 96
// degrees (106 with the margin) and the Death Star's terrain (TerrainDraw 0x7FCBCCC4 lays out
// the tiles inside the hull of the camera and the far corners seen from above) left the sides
// out in a sawtooth, at 32:9 all but a wedge in the middle. While hle_CameraSetup sets up a widened volume in a mission the full atan
// (0x8005348C, the SDK's) is taken, up to 70 degrees (140 wide): the terrain span array (4x its
// rows, hle_HeapAlloc) holds a footprint twice the far corners' distance across, which stays
// under ~250 of its 284 rows there. The heightmap terrain (Hoth) sizes its own table the same
// way and overflowed it in the cockpit at 21:9: hle_TerrainRows.
static bool s_wideVolume = false;
constexpr double kMaxHalfTan = 2.7474774;  // tan(70 degrees)

void hle_CameraHFovAtan(Ctx& c) {
    static const bool keepCap = envFlag("R2_HFOV_CAP");  // tests: the game's 90-degree cap
    if (s_wideVolume && !keepCap && std::abs(c.f[1]) > 1.0) {
        c.f[1] = std::clamp(c.f[1], -kMaxHalfTan, kMaxHalfTan);
        f_8005348C(c);
        return;
    }
    f_7FC8DE3C_orig(c);
}

// MTXPerspective (SDK 0x8002FEA4; fovY f1, aspect f2, near f3, far f4). The HUD's cameras (the
// radar globe: 0x7FCD22C4, 0x7FCD4F60, 0x7FCD50F4) build their projections here, not through
// CameraSetup, from the aspect their objects copied when the mission started: after a window
// resize they kept the old window's (2.37 at 1280x1024: the globe drew squeezed). An aspect
// setGameAspect wrote before is a widened 4:3 one, like in hle_CameraSetup: today's instead.
void hle_MTXPerspective(Ctx& c) {
    const double aspect = c.f[2];
    const double display = s_displayAspect.load(std::memory_order_relaxed);
    if (std::abs(aspect - display) >= 0.0005 && oldGameAspect(aspect)) c.f[2] = c.ps1[2] = rsingle(display);
    f_8002FEA4_orig(c);
}

// VR in flight: the camera's matrix (CameraSetup's r4, 3x4) turned with the player's head, so
// that the game culls, lays the terrain out and draws where the head looks (the eyes' views are
// relative to it: xr::cameraRotation). R2_XR_CAM_MODE=1 takes the matrix as camera -> world
// (0: world -> camera, the view). A matrix the game left as this wrote it (not rebuilt this
// frame) is turned from the game's own again, not twice.
static void xrHeadCamera(u32 matrix) {
    float r[9];
    if (!xr::cameraRotation(r)) return;
    static const bool camToWorld = [] {
        const char* e = getenv("R2_XR_CAM_MODE");
        return e && atoi(e) == 1;
    }();
    struct Saved {
        float game[12], wrote[12];
    };
    static std::unordered_map<u32, Saved> saved;
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = float(s2d(rd32(matrix + 4 * i)));
    Saved& sv = saved[matrix];
    if (std::equal(m, m + 12, sv.wrote)) std::copy(sv.game, sv.game + 12, m);
    std::copy(m, m + 12, sv.game);
    static int logged = 0;
    if (logged < 3) {
        logged++;
        logf("xr", "camera matrix %08X (%s): [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]", matrix,
             camToWorld ? "camera -> world" : "world -> camera", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
             m[9], m[10], m[11]);
    }
    float out[12];
    if (!camToWorld) {
        // view' = R^T view: rows i = sum_k r[k][i] * row k (the translations too)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                out[i * 4 + j] = r[0 * 3 + i] * m[0 * 4 + j] + r[1 * 3 + i] * m[1 * 4 + j] + r[2 * 3 + i] * m[2 * 4 + j];
    } else {
        // camera' = camera R (the axes), the position kept
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++)
                out[i * 4 + j] = m[i * 4 + 0] * r[0 * 3 + j] + m[i * 4 + 1] * r[1 * 3 + j] + m[i * 4 + 2] * r[2 * 3 + j];
            out[i * 4 + 3] = m[i * 4 + 3];
        }
    }
    for (int i = 0; i < 12; i++) wr32(matrix + 4 * i, d2s(out[i]));
    for (int i = 0; i < 12; i++) sv.wrote[i] = float(s2d(rd32(matrix + 4 * i)));
    gfx::resendStereoViews();
}

void hle_CameraSetup(Ctx& c) {
    double aspect = c.f[1];
    const double fov = c.f[2];
    const double display = s_displayAspect.load(std::memory_order_relaxed);
    // A camera object made before the window was resized (or the aspect setting changed) still
    // holds the aspect it copied then: it is a widened 4:3 camera like the rest, so it takes
    // today's. (It kept the old one: the flight camera's culling was opened from it and the
    // radar globe drew squeezed.)
    if (std::abs(aspect - display) >= 0.0005 && oldGameAspect(aspect)) {
        aspect = display;
        c.f[1] = c.ps1[1] = rsingle(display);
    }
    // R2_CAM_LOG=1: each camera setup (view, caller, aspect, fov) when it changes
    static const bool camLog = envFlag("R2_CAM_LOG");
    if (camLog) {
        struct Seen {
            u32 view, lr;
            float aspect, fov;
        };
        static std::vector<Seen> seen;
        const Seen s{c.r[3], c.lr, float(aspect), float(fov)};
        Seen* e = nullptr;
        for (Seen& x : seen)
            if (x.view == s.view && x.lr == s.lr) e = &x;
        if (!e || e->aspect != s.aspect || e->fov != s.fov) {
            if (e) *e = s;
            else if (seen.size() < 256) seen.push_back(s);
            logf("cam", "setup view %08X from %08X: aspect %.4f fov %.2f (display %.4f, xfb %u lines)", s.view, s.lr,
                 aspect, fov, display, viXfbLines());
        }
    }
    static const bool gameCulling = envFlag("R2_GAME_CULLING");
    // VR stereo 3D in flight: the headset sees far more than the game's view (a wider fov, and
    // the head turns), so the mission's volume opens kXrMargin times (R2_XR_CULL_MARGIN), even
    // in a 4:3 window
    // (setting xr_view_range; R2_XR_CULL_MARGIN for tests)
    static const char* xrMarginEnv = getenv("R2_XR_CULL_MARGIN");
    const double xrMargin = std::clamp(xrMarginEnv ? atof(xrMarginEnv) : double(settings::get().xrViewRange), 1.0, 12.0);
    const bool xrFlight = xr::stereo() && xr::active() && playerCraft() != 0;
    if (gameCulling || (display < 4.0 / 3.0 + 0.01 && !xrFlight) || !(aspect > 0.5 && aspect < 8.0) || !(fov > 1.0 && fov < 170.0)) {
        f_7FC14CEC_orig(c);
        return;
    }
    const u32 view = c.r[3];
    const u32 matrix = c.r[4];
    const u32 craft = playerCraft();
    // (tests only, R2_XR_HEAD_CAMERA=1: the player could not see their craft, and it made them dizzy)
    static const bool headCamera = envFlag("R2_XR_HEAD_CAMERA");
    if (headCamera && xrFlight && craft) xrHeadCamera(matrix);
    // already at the display's aspect (setGameAspect: the flight and HUD cameras): not opened
    const bool widened = std::abs(aspect - display) < 0.002;
    const u32 lines = viXfbLines();
    const double k = widened ? 1.0 : display * 0.75;
    const double v = !widened && lines && lines < 408 ? 480.0 / lines : 1.0, r = k / v;
    static const double sceneMargin = [] {
        const char* e = getenv("R2_CULL_MARGIN");
        return e ? std::clamp(atof(e), 1.0, 20.0) : kSceneMargin;
    }();
    static const double terrainMargin = [] {
        const char* e = getenv("R2_TERRAIN_MARGIN");
        return e ? std::clamp(atof(e), 1.0, 3.0) : kTerrainMargin;
    }();
    // In a letterboxed frame every camera culls for at least the letterboxed picture: the hangar
    // (ship select) renders at 2.105 but sets the camera up once more per frame at 4:3, and that
    // volume decides what is there (the bay's sides popped in as the camera panned).
    const double base = v > 1.0 ? std::max(aspect, 4.0 / 3.0 * v) : aspect;
    const double margin = craft ? (xrFlight ? std::max(xrMargin, terrainMargin) : terrainMargin) : sceneMargin;
    // in missions the horizontal fov may pass 90 degrees (hle_CameraHFovAtan)
    struct WideVolume {
        explicit WideVolume(bool on) { s_wideVolume = on; }
        ~WideVolume() { s_wideVolume = false; }
    } wideVolume(craft != 0);
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    auto setup = [&](double m) {  // the volume opened for the screen, m times more
        const double wider = std::max(1.0, r) * m, taller = std::max(1.0, 1.0 / r) * m;
        c.r[3] = view;  // (the call keeps the non-volatile registers; arguments again)
        c.r[4] = matrix;
        c.f[1] = c.ps1[1] = rsingle(base * wider / taller);
        c.f[2] = c.ps1[2] = rsingle(std::min(2.0 * std::atan(std::tan(fov * 0.5 * kRad) * taller) / kRad, 170.0));
        f_7FC14CEC_orig(c);
    };
    if (widened && margin == 1.0) {
        f_7FC14CEC_orig(c);
        return;
    }
    // Outside missions the horizontal fov stays the screen's (see above); in missions it takes
    // the margin too: the terrain (the Death Star's surface) is laid out from the volume, whole
    // tiles inside it, and the tiles at the sides dropped out in a sawtooth just inside the edges
    // (and past 90 degrees everywhere outside that: hle_CameraHFovAtan).
    u32 hfov = 0;
    if (margin != 1.0 && !craft) {
        setup(1.0);
        hfov = rd32(view + 0x130);
    }
    setup(margin);
    if (hfov) wr32(view + 0x130, hfov);
    // The projection. A full frame opened sideways: the game's view widened r times (4:3 ->
    // the display's aspect; a cinematic camera's 2.105 inside a full frame -> 2.105 r), which
    // Aurora is told to show as it is. Otherwise (letterboxed, or a display taller than the
    // camera) the game's own, which Aurora opens.
    const bool sideways = v == 1.0 && r >= 1.0;
    const double projAspect = sideways ? aspect * r : aspect;
    if (sideways) gfx::noteWidenedAspect(float(projAspect));
    wr32(view + 0x12C, d2s(projAspect));
    wr32(view + 0x134, d2s(fov));
    if (xrFlight && craft) xr::setGameView(float(fov), float(projAspect));  // (the HUD's plane)
    // The camera keeps its own copy of the projection (+0xA4, and +0xE4 derived from it; built
    // by 0x7FC14B8C from the aspect, fov and near/far): the game projects points with it, e.g.
    // to place the engine glows. Built for the culling volume above, it put them closer to the
    // center than the ships' engines; rebuilt here for the projection that is shown.
    c.r[3] = view;
    c.r[4] = view + 0xA4;
    c.r[5] = view + 0xE4;
    f_7FC14B8C(c);
}

// The terrain's span array. TerrainSetup (0x7FD89210) allocates it at a mission's start with
// one 4-byte (left, right) pair per grid row, as many rows as the view distance spans for the
// game's own 4:3 view; every frame TerrainSpans (0x7FCBAA0C) rasterizes the view footprint onto
// the terrain grid and writes one pair per row it covers, with no bound. Widened views (the
// display's aspect, the terrain margin above) cover more rows: the pairs ran past the array
// into the next heap block's header (the tile records, 0x80095FA4), and the game crashed in
// HeapFree when the mission ended (or later). That one allocation (this return address) gets
// four times the room, enough for 32:9 with the margin.
// The heightmap terrain's visible-tile list (HeightTerrainLists 0x7FD8AA18, this return address)
// gets four times its 0xC000 bytes too: hle_HeightTerrainTiles.
constexpr u32 kTileListRet = 0x7FD8AA58u, kTileListBytes = 0xC000, kTileListScale = 4;
static u32 s_tileListRecords = kTileListBytes / 12;
static u32 s_heightRowsExtra = 0;  // rows hle_TerrainRows added for HeightTerrainSetup's block

// The enlarged tables live outside the game's heap. Taken from it (MEM1, 22 MiB, mission data
// filling most of it), the extra room moved every block allocated after them, and the heap ran
// short where it never does on hardware: the user's Revenge on Yavin (bonus row) stopped ~5
// minutes in with "*** OUT OF MEMORY (size 530352, free 1019424)! ***" from the loader thread
// (0x7FD5B1BC, a level resource read in 256 KiB pieces), which then read the disc into the null
// pointer it got (fault at 0x0002F880). Their state, 22 s into that mission: 962 KB free in 23
// pieces, the largest 485 KB, and the 4x visible-tile list alone took 147 KB more than the game
// planned. So each table takes its original size from the game's heap, as on hardware (the heap's
// layout and history stay the game's own), and the game gets an enlarged block of the port's
// (kPortArena, past the end of MEM1, saved in states) whose header copies the original's;
// HeapFree of it frees the original. One slot per table: a mission sets each up once.
struct PortTable {
    const char* name;
    u32 lr;                // HeapAlloc's return address in the table's setup
    u32 offset, room;      // the slot in the port arena (data at kPortArena + offset)
    u32 block = 0, size = 0;  // the game's own (original-size) block, the enlarged size
};
static PortTable s_portTables[] = {
    {"terrain span array", 0x7FD893E8u, 0x00100, 0x0FF00},   // TerrainSetup (the Death Star)
    {"heightmap terrain rows", 0x7FD8A3C8u, 0x10100, 0x2FF00},  // HeightTerrainSetup (~95 KB in Yavin)
    {"heightmap visible-tile list", kTileListRet, 0x40100, kTileListBytes * kTileListScale},  // HeightTerrainLists
};
static_assert(0x40100 + kTileListBytes * kTileListScale <= kPortArenaSize);

void hle_HeapAlloc(Ctx& c) {
    const u32 lr = c.lr;
    u32 original = 0, enlarged = 0;
    if (lr == 0x7FD893E8u) [[unlikely]] {
        original = c.r[3], enlarged = c.r[3] * 4;
        logf("wide", "terrain span array: room for %u rows instead of %u", enlarged / 4, original / 4);
    } else if (lr == kTileListRet && c.r[3] == kTileListBytes) [[unlikely]] {
        original = c.r[3], enlarged = c.r[3] * kTileListScale;
        s_tileListRecords = enlarged / 12;
        logf("wide", "heightmap terrain: room for %u visible tiles instead of %u", s_tileListRecords, original / 12);
    } else if (lr == 0x7FD8A3C8u && s_heightRowsExtra && c.r[3] > s_heightRowsExtra * 6) [[unlikely]] {
        // rows * 6 + n * 86 bytes: the table (4 bytes a row), a 2-byte array by rows, records
        enlarged = c.r[3], original = c.r[3] - s_heightRowsExtra * 6;
        s_heightRowsExtra = 0;
    }
    PortTable* t = nullptr;
    for (auto& e : s_portTables)
        if (enlarged && e.lr == lr) t = &e;
    if (!t || enlarged > t->room) [[likely]] {
        if (t) logf("wide", "%s: %u bytes do not fit the port's %u: taken from the game's heap", t->name, enlarged, t->room);
        if (enlarged) c.r[3] = enlarged;
        f_800070C4_orig(c);
        return;
    }
    const u32 flags = c.r[4];
    c.r[3] = original;
    f_800070C4_orig(c);  // the game's own block, as on hardware
    const u32 block = c.r[3];
    if (!block) return;  // out of memory: the game's own handling
    if (t->block) logf("wide", "%s: the previous one (game block %08X) was never freed", t->name, t->block);
    const u32 p = kPortArena + t->offset;
    for (u32 o = 0; o < 0x20; o += 4) wr32(p - 0x20 + o, rd32(block - 0x20 + o));  // its header
    if (flags & 0x100) memset(g_base + p, 0, enlarged);  // HeapAlloc's clear flag
    t->block = block, t->size = enlarged;
    c.r[3] = p;
    logf("wide", "%s: %u bytes at %08X (the game's %u-byte block at %08X stays as on hardware)", t->name, enlarged, p,
         original, block);
}

// The game's heap extension. Vengeance on Kothlis (the user's session, 16:10, ~5 minutes into
// the mission) stopped with "*** OUT OF MEMORY (size 4096, free 3136)! ***" twice, then a
// store through the null pointer it got (fatal at address 0, guest lr 0x7FC0ED1C): the MEM1
// heap had 772 KB free in 9 pieces in the user's save state two minutes before, 3 KB then.
// HeapAllocFrom (0x80006CD0: the game's heap record r3 - MEM1 0x80068088, VM 0x800680A4 -,
// size r4 rounded to 32, report r5), HeapAlloc's only way into OSAllocFromHeap (0x80028AC4),
// prints that line when the allocation fails with report set (OSCheckHeap's free count), calls
// 0x800074F8(0, 3) and returns 0, which HeapAlloc's callers do not check. Callers that can live
// with a failure pass flag 0x40000 (report 0): they still get 0, as on hardware. The others now
// get a block of the extension: 8 MiB right after MEM1 (kMem1Ext, runtime.h), as if the
// console had 32 MiB, but only once the game's own heap has no piece that fits - until then
// its layout and history are the game's own, as on hardware. Each block has the SDK's 32-byte
// cell header (prev and next 0, the cell's size), behind which HeapAlloc writes its fields as
// for any block (size, flags, heap record: the MEM1 one); HeapFree gives them back here
// instead of to OSFreeToHeap (they are in none of the SDK's lists). First fit by address, like
// OSAlloc. Logged as "heap" lines with HeapAlloc's caller; kept in save states (version 4).
constexpr u32 kHeapExtBase = 0x80000000u + kMem1Size, kHeapExtEnd = 0x80000000u + kMem1Mapped;
constexpr u32 kMem1HeapRecord = 0x80068088u;
static std::map<u32, u32> s_extFree, s_extUsed;  // cell address -> cell size (header included)
static u32 s_extInUse = 0, s_extPeak = 0;
struct ExtCell {
    u32 cell, size;
};  // (save states)

static u32 heapExtAlloc(u32 size) {
    if (s_extFree.empty() && s_extUsed.empty()) s_extFree[kHeapExtBase] = kMem1Ext;  // first use
    if (size > kMem1Ext) return 0;
    const u32 need = (size + 0x20 + 0x1F) & ~0x1Fu;
    for (auto it = s_extFree.begin(); it != s_extFree.end(); ++it) {
        if (it->second < need) continue;
        const u32 cell = it->first, left = it->second - need;
        u32 take = need;
        s_extFree.erase(it);
        if (left >= 0x40) s_extFree[cell + need] = left;  // (OSAlloc's smallest cell)
        else take += left;
        s_extUsed[cell] = take;
        wr32(cell, 0), wr32(cell + 4, 0), wr32(cell + 8, take);
        s_extInUse += take;
        return cell + 0x20;
    }
    return 0;
}

static void heapExtFree(u32 p) {
    const auto used = s_extUsed.find(p - 0x20);
    if (used == s_extUsed.end()) {
        logf("heap", "HeapFree of %08X in the heap extension: not a block of it (ignored)", p);
        return;
    }
    u32 cell = used->first, size = used->second;
    s_extUsed.erase(used);
    s_extInUse -= size;
    if (size >= 0x40000)  // (level data, at a mission's end)
        logf("heap", "heap extension: %u KiB at %08X freed (%u KiB in use)", size >> 10, p, s_extInUse >> 10);
    auto next = s_extFree.lower_bound(cell);
    if (next != s_extFree.end() && cell + size == next->first) {
        size += next->second;
        next = s_extFree.erase(next);
    }
    if (next != s_extFree.begin()) {
        if (auto prev = std::prev(next); prev->first + prev->second == cell) {
            prev->second += size;
            cell = 0;
        }
    }
    if (cell) s_extFree[cell] = size;
    if (!s_extInUse) logf("heap", "heap extension empty again (the game's heap has room)");
}

void hle_HeapAllocFrom(Ctx& c) {
    const u32 heap = c.r[3], size = c.r[4], report = c.r[5];
    if (heap != kMem1HeapRecord || !report) {
        f_80006CD0_orig(c);
        return;
    }
    // R2_HEAP_EXT_TEST=<n> (tests): every n-th of these goes to the extension with the heap not
    // full - blocks there read from the disc, drawn from, run (code modules) and freed
    static const u32 testEvery = [] {
        const char* e = getenv("R2_HEAP_EXT_TEST");
        return e ? u32(strtoul(e, nullptr, 10)) : 0u;
    }();
    static u32 testCount = 0;
    if (!testEvery || ++testCount % testEvery) {
        c.r[5] = 0;  // no report yet: the extension may take it
        f_80006CD0_orig(c);
        if (c.r[3]) return;
    }
    // HeapAlloc's frame is r1 (stwu -0x30): its caller's return address was saved at +0x34
    const u32 caller = rd32(c.r[1] + 0x34);
    const u32 before = s_extInUse;
    if (const u32 p = heapExtAlloc(size)) {
        // the first block since it was empty, then every new 256 KiB of peak use
        if (!before || s_extInUse >= s_extPeak + 0x40000) {
            logf("heap", "MEM1 heap full: %u bytes at %08X from the heap extension for %08X (%u KiB of %u in use)", size, p,
                 caller, s_extInUse >> 10, kMem1Ext >> 10);
            s_extPeak = std::max(s_extPeak, s_extInUse);
        }
        c.r[3] = p;
        return;
    }
    logf("heap", "MEM1 heap full and the heap extension too (%u bytes for %08X, %u KiB in use)", size, caller,
         s_extInUse >> 10);
    c.r[3] = heap, c.r[4] = size, c.r[5] = report;
    f_80006CD0_orig(c);  // the game's own report, and 0
}

// HeapFree (0x80007270, ptr r3): a port table's block frees the game's own; a block of the heap
// extension goes back to it.
void hle_HeapFree(Ctx& c) {
    const u32 p = c.r[3];
    if (p >= kHeapExtBase && p < kHeapExtEnd) [[unlikely]] {
        heapExtFree(p);
        return;
    }
    if (p >= kPortArena && p < kPortArena + kPortArenaSize) [[unlikely]] {
        PortTable* t = nullptr;
        for (auto& e : s_portTables)
            if (p == kPortArena + e.offset) t = &e;
        if (!t || !t->block) {
            logf("wide", "HeapFree of %08X in the port arena: not a block of it (ignored)", p);
            return;
        }
        logf("wide", "%s freed (the game's block %08X)", t->name, t->block);
        c.r[3] = t->block;
        t->block = 0, t->size = 0;
    }
    f_80007270_orig(c);
}

// The heightmap terrain (Hoth and the other ground missions; state 0x7FDF5E60) has the same
// kind of span table, sized by TerrainRows (0x7FC92B7C: the footprint of a view 41 degrees to
// each side, at the mission's view distance) - but HeightTerrainSetup (0x7FD8A240) allocates it
// in one block with arrays laid out after it by that row count (+0x248 = table + rows * 4, then
// 86-byte records), so its row count itself gets four times the rows. The widened cockpit view
// (21:9: 112 degrees with the margin, past the game's 90-degree cap: hle_CameraHFovAtan) spans
// more rows than the game's count: HeightTerrainSpans (0x7FCBD92C, no bound) wrote past the
// table into those arrays, and the walk over the heightmap (0x7FCBE5F0) then met an inverted
// span (left > right): a row ends only when the column equals its end, so it slid down from
// MEM1 through the whole VM, every page it touched taking one of the pager's 1536 ARAM slots:
// "FATAL VM ERROR: Out of ARAM memory" seconds into a fight (or a call through a pointer the
// overflow had overwritten: 0x4D657461, "Meta"). The Death Star's TerrainSetup (0x7FD893BC)
// keeps its count (its table is a block of its own: hle_HeapAlloc).
void hle_TerrainRows(Ctx& c) {
    const u32 lr = c.lr;
    f_7FC92B7C_orig(c);
    if (lr == 0x7FD8A308u) {
        logf("wide", "heightmap terrain: room for %u rows instead of %u", c.r[3] * 4, c.r[3]);
        s_heightRowsExtra = c.r[3] * 3;  // its block comes from the port (hle_HeapAlloc)
        c.r[3] *= 4;
    }
}

// HeightTerrainSpans (0x7FCBD92C): uses rows 0..[+0x23C] of the span table ([+0x240] rows).
// Logs the most used so far (every 8 more) and, once, a footprint the table cannot hold.
static u32 s_heightRowsMax = 0;
void hle_HeightTerrainSpans(Ctx& c) {
    f_7FCBD92C_orig(c);
    constexpr u32 kState = 0x7FDF5E60;
    const u32 used = rd32(kState + 0x23C) + 1, rows = rd32(kState + 0x240);
    if (used >= s_heightRowsMax + 8 || (used > rows && s_heightRowsMax <= rows)) {
        logf("wide", "heightmap terrain: %u of %u span rows used%s", used, rows,
             used > rows ? " - PAST THE TABLE (memory after it overwritten)" : "");
        s_heightRowsMax = used;
    }
}

// HeightTerrainTiles (0x7FCC0A00): every tile inside the spans goes into the +0xC0 list (12-byte
// records, u16 indices from 1, chained per material through the buckets at +0xBC: u16 head, tail,
// 6 bytes each, 384 of them, cleared each frame), with no capacity check, and is then drawn
// through the jump table 0x7FDCB5A8 by its record's level byte. The game's 0xC000 bytes hold
// 4096; the widened cockpit at 21:9 saw more, the list ran on into the next heap block, and a
// record read back as garbage called 0x4D657461 or 0x30000000 (0x7FCC09E0). hle_HeapAlloc gives
// it four times the room (16384 records; the indices stay u16). The largest bucket tail after the
// call is the number of records used: the most so far is logged (every 512 more), and once a
// count the list cannot hold.
static u32 s_tilesMax = 0;
void hle_HeightTerrainTiles(Ctx& c) {
    f_7FCC0A00_orig(c);
    constexpr u32 kState = 0x7FDF5E60;
    const u32 buckets = rd32(kState + 0xBC);
    if (!buckets) return;
    u32 used = 0;
    for (u32 i = 0; i < 0x900 / 6; i++) used = std::max(used, rd16(buckets + i * 6 + 2));
    if (used >= s_tilesMax + 512 || (used > s_tileListRecords && s_tilesMax <= s_tileListRecords)) {
        logf("wide", "heightmap terrain: %u of %u visible tile records used%s", used, s_tileListRecords,
             used > s_tileListRecords ? " - PAST THE LIST (memory after it overwritten)" : "");
        s_tilesMax = used;
    }
}

// AreaTriggerUpdate (0x7FC23008; object r3, position r4) runs after an object with a trigger list
// moves (the player's craft after each step, 0x7FC36168). The list (+0x34) holds object indices
// (low half, from 1, into [[0x7FDECF10] + 0x54]; bit 31 = inside; 0xFFFFFFFF ends it). An area
// trigger is a level record read in place: half 0x001F, byte +2 the shape (6 box, 7 cylinder of
// radius +0x1C), id +4, min corner +0x10 and size +0x28 (x, y, z); +0x3C an event bit and
// +0x40/+0x44 a counter threshold that must hold first, +0x48 nonzero = height ignored, +0x4C the
// actions on entering (event bit, value, counter, amount, script offset from the record), +0x60
// those on leaving. Prisons of the Maw lays two gates across the approach to the prison's force
// field, x -2000..3000, y 4300..7300, 100 deep: 0x82 at z 4900 (counter 21: two voice lines) and
// 0x7C at z -100 (counter 15: the force field lines, then "You should switch to ion cannons").
// The user flew over 0x7C at y ~7500 and never heard them; asked for "a wall the player flies
// through", both get the game's own ignore-height flag (that mission's 0x7D has it): full height,
// the width and depth as they were.
static void mawGateWalls(u32 list) {
    struct Gate { u32 id, minZ, counter; };
    static constexpr Gate kGates[] = {{0x7C, 0xC2C80000u /* -100 */, 0x0004000Fu},
                                      {0x82, 0x45992000u /* 4900 */, 0x00040015u}};
    u32 table = 0;
    for (u32 i = 0; i < 4096; i++) {
        const u32 w = rd32(list + i * 4);
        if (w == 0xFFFFFFFFu) break;
        const u32 index = w & 0xFFFF;
        if (!index) continue;
        if (!table) table = rd32(rd32(0x7FDECF10) + 0x54);  // read as the game does, for a list with entries
        const u32 rec = rd32(table + (index - 1) * 4);
        if (rd32(rec) != 0x001F0600u || rd32(rec + 0x48) != 0) continue;
        const u32 id = rd32(rec + 4) >> 16;
        for (const Gate& g : kGates) {
            if (id != g.id || rd32(rec + 0x18) != g.minZ || rd32(rec + 0x54) != g.counter) continue;
            if (rd32(rec + 0x10) != 0xC4FA0000u || rd32(rec + 0x14) != 0x45866000u ||  // -2000, 4300
                rd32(rec + 0x28) != 0x459C4000u || rd32(rec + 0x2C) != 0x453B8000u ||  // 5000, 3000
                rd32(rec + 0x30) != 0x42C80000u)                                        // 100
                continue;
            wr32(rec + 0x48, 1);
            logf("game", "Prisons of the Maw: gate %X (counter %u) reaches from the ground to the sky (record %08X)",
                 id, g.counter & 0xFFFF, rec);
        }
    }
}

void hle_AreaTriggerUpdate(Ctx& c) {
    if (const u32 list = rd32(c.r[3] + 0x34)) mawGateWalls(list);
    f_7FC23008_orig(c);
}

// Prisons of the Maw's force field: two hexagons (OBJmawShieldCore, vtable 0x7FDD1CD0; a list
// from 0x80096054 = r13-0x4D4C, next +0x104), each six triangular panels: centre +0xA8,
// generator i, generator i+1 (count +0xE0, their level records +0xE4, position at record +0x10).
// MawShieldCoreUpdate (0x7FD02AAC) flags generator i at +0x1F8 + i while its OBJmawShieldGen's
// +0x104 is above 0 (ion cannons set it: disabled) and the hexagon at +0xFC (down) once fewer
// than 3 of its generators run. MawShieldCoreDraw (0x7FD02BE8) draws a panel only if neither of
// its generators is flagged, but MawShieldCollide (0x7FD02EC0: the ray test for craft and shots)
// tests every panel of a hexagon that is not down: a panel next to a disabled generator vanishes
// and stays solid (the user flew into the gap and crashed). The game's own code does that on
// hardware too; as the user asked, the panel test (MawShieldPanelHit 0x7FD02CC0, called only from
// there: ray r3/r4, centre r5, generator positions r6/r7) now misses the panels the draw leaves
// out.
void hle_MawShieldPanelHit(Ctx& c) {
    const u32 core = c.r[5] - 0xA8;
    const u32 n = rd32(core + 0xE0);
    if (rd32(core + 0x80) == 0x7FDD1CD0u && n >= 3 && n <= 6) {
        u32 open = 0, panel = n;  // open: panels with a disabled generator (bit i: generators i, i+1)
        for (u32 i = 0; i < n; i++) {
            const u32 next = (i + 1) % n;
            if (rd8(core + 0x1F8 + i) || rd8(core + 0x1F8 + next)) open |= 1u << i;
            if (rd32(core + 0xE4 + i * 4) + 0x10 == c.r[6] && rd32(core + 0xE4 + next * 4) + 0x10 == c.r[7])
                panel = i;
        }
        static u32 s_core[4], s_open[4];
        for (u32 k = 0; k < 4; k++) {
            if (s_core[k] && s_core[k] != core) continue;
            if (s_core[k] != core || s_open[k] != open) {
                s_core[k] = core;
                s_open[k] = open;
                logf("game", "Prisons of the Maw: force field hexagon %08X: panels open (generator disabled, no collision): mask %02X",
                     core, open);
            }
            break;
        }
        if (panel < n && (open >> panel & 1)) {
            c.r[3] = 0;  // no hit
            return;
        }
    }
    f_7FD02CC0_orig(c);
}

// Perfect mission stats (settings). The mission's running stats live at 0x7FDF04F0 (StatsReset
// 0x7FC464B0 at a mission's start): +0x00/+0x04 shots fired (lasers / other), +0x0C hits, +0x10
// enemies that appeared, +0x14 enemies the player destroyed, +0x18 friendlies, +0x1C friendlies
// lost (StatsObjectDestroyed 0x7FC46840), +0x20 lives lost, +0x24 mission time (seconds; weapons
// and respawns read it, so it is left alone during play), +0x28/+0x2C targeting computer uses
// and seconds. At the end MissionResults (0x7FD76514) turns them into the record the results
// screen shows and MedalRank (0x7FD765F4) compares with the mission's requirements: +0x00 time
// (minutes, float), +0x04 enemies destroyed, +0x06 accuracy %, +0x08 friendlies lost (u16 each),
// +0x0C lives lost (u32), +0x10 targeting computer efficiency % (float). With the cheat the
// record says 0:01, every enemy that appeared destroyed, 100%, 0, 0 and 100%. (Mission 15's
// +0x08 counts something where more is better: left as it is.)
// DitherStrength (0x7FD345CC): the alpha of the TEV color with which the frame setup (0x7FD34A78)
// blends a screen-space dither texture over the picture (25 at boot, 45 later; 0 = none). Its
// texture matrix (0x7FD2AB48) repeats every 4 pixels of a 640x480 screen, so at 4K a texel
// covers ~4.5 pixels: a zigzag grid over smooth areas such as Hoth's sky. Off by default, like
// Dolphin's "Disable Dithering" code (it zeroes this byte and the pattern's mode word). The
// pattern is not zero-mean: it also brightened the picture (~10/255 in Hoth's sky).
void hle_DitherStrength(Ctx& c) {
    f_7FD345CC_orig(c);
    if (!settings::get().gameDither) c.r[3] = 0;
}

void hle_MissionResults(Ctx& c) {
    const u32 rec = c.r[4];
    f_7FD76514_orig(c);
    if (!settings::get().perfectStats) return;
    constexpr u32 kStats = 0x7FDF04F0;
    const u32 enemies = std::max(rd32(kStats + 0x10), rd32(kStats + 0x14));
    wr32(rec + 0x00, fbits(1.25f / 60.f));  // 1.25 s: shown as 0:01, compared in whole seconds
    wr16(rec + 0x04, std::min<u32>(enemies, 0xFFFF));
    wr16(rec + 0x06, 100);
    if (rd8(0x800962EC) != 15) wr16(rec + 0x08, 0);  // the mission (r13-0x4AB4)
    wr32(rec + 0x0C, 0);
    wr32(rec + 0x10, fbits(100.f));
    logf("cheat", "perfect mission stats: 0:01, %u enemies destroyed, 100%% accuracy, nothing lost, 100%% targeting computer",
         enemies);
}

// R2_RESULTS_TEST=<seconds>: once, that long after the start, MissionResults into a scratch
// record below the guest stack, logged before and after the cheat (missions are long to finish
// in a test). Called from GameFrameDelta, an ordinary guest call.
static void resultsTest(Ctx& c) {
    static const double at = getenv("R2_RESULTS_TEST") ? atof(getenv("R2_RESULTS_TEST")) : -1.0;
    static bool done = false;
    if (at < 0 || done) return;
    static const u64 t0 = tbNow();
    if (double(tbNow() - t0) / double(kTbClock) < at) return;
    done = true;
    for (int pass = 0; pass < 2; pass++) {
        settings::Settings& s = settings::get();
        const bool keep = s.perfectStats;
        s.perfectStats = pass == 1;
        Ctx t = c;
        t.r[1] = c.r[1] - 0x200;
        const u32 rec = t.r[1] + 0x100;
        t.r[4] = rec;
        t.lr = 0;
        hle_MissionResults(t);
        s.perfectStats = keep;
        logf("cheat", "results test (%s): time %.3f min, enemies %u, accuracy %u%%, friendlies lost %u, lives lost %u, targeting computer %.1f%%",
             pass ? "cheat" : "game's", s2d(rd32(rec)), rd16(rec + 4), rd16(rec + 6), rd16(rec + 8), rd32(rec + 12),
             s2d(rd32(rec + 16)));
    }
}

// The targeting computer (cockpit view, Y). TargetingRect (0x7FCD5D08) projects the corners of
// its screen with the camera's projection (+0xA4) to 640x480 screen units, and TargetingPass
// (0x7FCD7D28, each frame pass) draws the orange screen there as a 2D quad and copies the EFB
// under it. In widescreen those
// are positions on the widened screen; Aurora laid the quad out like any 4:3 2D element and
// squeezed it a second time. The drawing is bracketed for the bridge (gfx/gx_bridge.cpp,
// screenSpace2D) by BP writes to an unused register, through the gather pipe so they stay in
// order with the game's commands.
static void gatherBp(u32 v) {
    wr8(0xCC008000, 0x61);
    wr32(0xCC008000, v);
}

void hle_TargetingPass(Ctx& c) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("wide", "targeting computer screen drawn in screen space");
    }
    gatherBp(0x7E5C5201);
    f_7FCD7D28_orig(c);
    gatherBp(0x7E5C5200);
}

// The flight reticle (issue #10: "the crosshair is not a circle when using widescreen").
// ReticleDraw (0x7FC6BFA8) draws its rings and side lines as 2D quads under an identity
// orthographic projection, at the aim point it projected with the camera, which widescreen
// already made wide - so the game's numbers are already right for the wide screen, and Aurora's
// 4:3 layout for 2D squeezed the rings toward the middle a second time (about 0.75 as wide at
// 16:9). Bracketed like the targeting computer, with another value: the bridge sends these as
// screen space, identity projection included.
void hle_ReticleDraw(Ctx& c) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("wide", "flight reticle drawn in screen space");
    }
    gatherBp(0x7E5C5221);
    f_7FC6BFA8_orig(c);
    gatherBp(0x7E5C5220);
}

// Cutscene movies: CutsceneVideoDraw (0x7FD68C54) draws a movie object's quad (the Death Star's
// explosion at the end of mission 1, the capital ship's in the Factor 5 intro), placed in the 3D
// scene to cover the game's own view with a margin. Widescreen showed the scene past its edges
// (7% at each side of the intro's at 2.12:1); the bridge (gfx/gx_bridge.cpp, fillScreen)
// stretches draws inside this bracket out to the screen's edges.
void hle_CutsceneVideoDraw(Ctx& c) {
    gatherBp(0x7E5C5211);
    f_7FD68C54_orig(c);
    gatherBp(0x7E5C5210);
}

// Infinite lives (settings): LoseLife (0x7FD7648C) decrements the lives byte at 0x800962EE
// (3 ships at a mission start; at 1 it ends the game instead: 0x800962EF = 0x21, mode 6) and
// returns 1. With the cheat it reports the life lost without taking it.
void hle_LoseLife(Ctx& c) {
    if (settings::get().infiniteLives && rd8(0x800962EF) != 0x21) {
        c.r[3] = 1;
        return;
    }
    f_7FD7648C_orig(c);
}

// --- Passcodes and unlocks --------------------------------------------------------------------
// The game's progress block (0x7FE1C4A0, bss in its VM; saved to the memory card: +0 = 1 asks for
// a save, +2 the current pilot, pilot records of 288 bytes after it) keeps two flag words that
// the Options > Passcodes screen sets (0x7FD6D4A0: the 8 characters, zero-padded to 1 KiB, go
// through SHA-1 1000 times - each digest written back into the buffer after the code - and the
// result is looked up in a table of 71 digests at 0x7FDE2350; verified with THATSME!, ?INSIDER,
// BLAHBLAH, EXHIBIT!, COMPOSER, LIONHEAD, U!?!VWZC + GIVEITUP, !??QWTTJ + CLASSIC, ...):
//   +0x658 unlocks (SetUnlockFlag 0x7FD588C0: progress r3, mask r4, on r5): 0x1 credits, 0x2 the
//     documentary ("making of"), 0x8 audio commentary, 0x10 art gallery, 0x20 music hall,
//     0x40 ace mode (Options > Special Features shows each, 0x7FD70188), 0x100 / 0x200
//     commentary / ace mode switched on there; 0x400..0x40000 and 0x200000 the craft codes
//     (read only by CraftMask, below). Indices 1-7 of the table set bits 0x1..0x40.
//   +0x654 cheats (SetCheatFlag 0x7FD58914): 0x1 every main mission open (0x7FD7F9C0 ORs
//     0x7FF into the pilot's mission masks), 0x10000..0x100000 the five bonus missions (each
//     once the main mission before it is open), 0x2 every tech upgrade at its best (UpgradeLevel
//     0x7FD57FF0 returns 3), 0x8 infinite lives (LoseLife), 0x8000 black and white (LIONHEAD,
//     a toggle; the renderer's frame setup 0x7FD34978), 0x10..0x4000 single upgrade levels,
//     0x200000 a developer test mode (test textures: left out).
// Both setters mark the block for saving unless the mask only holds bits the game does not
// keep (cheats 0x8 / 0x8000, unlock 0x200000). The menu's rows set and clear the same bits
// (applied from the per-frame update, an ordinary guest call: the block's VM page can fault in
// there), so they end up in the game's save as if the codes had been entered.
namespace {
constexpr u32 kProgress = 0x7FE1C4A0, kCheatFlags = kProgress + 0x654, kUnlockFlags = kProgress + 0x658;
constexpr u32 kCraftCodeBits = 0x0007FC00u | 0x00200000u;  // the craft passcodes' unlock bits
struct Unlock {
    const char* name;
    const char* help;
    int word;  // 0 cheats (+0x654), 1 unlocks (+0x658)
    u32 mask;
    bool saved;
};
const Unlock kUnlocks[unlocks::Count] = {
    {"All missions", "Every mission is open, the five bonus missions too (the game's \"all missions\" passcodes).", 0,
     0x001F0001u, true},
    {"All tech upgrades", "Every craft has every tech upgrade at its best: advanced lasers, homing torpedoes, shields, "
     "targeting computer and the rest.", 0, 0x2u, true},
    {"Ace mode", "Options > Special Features gets Ace Mode: the game at its hardest.", 1, 0x40u, true},
    {"Art gallery", "Options > Special Features gets the Art Gallery.", 1, 0x10u, true},
    {"Music hall", "Options > Special Features gets the Music Hall.", 1, 0x20u, true},
    {"Documentary", "Options > Special Features gets the documentary (the making of the game).", 1, 0x2u, true},
    {"Audio commentary", "Options > Special Features gets the developers' audio commentary (switch it on there).", 1,
     0x8u, true},
    {"Credits", "Options > Special Features gets the end credits.", 1, 0x1u, true},
    {"Black and white", "The whole game in black and white, like an old film (the LIONHEAD passcode). The game does not "
     "save it.", 0, 0x8000u, false},
};
std::atomic<u32> s_flagsSeen[2];       // the words as last read by the game's frame update
std::atomic<bool> s_flagsKnown{false};
std::atomic<u32> s_setBits[2], s_clearBits[2];  // asked for by the menu, not applied yet
}  // namespace

namespace unlocks {
const char* name(int i) { return i >= 0 && i < Count ? kUnlocks[i].name : ""; }
const char* help(int i) { return i >= 0 && i < Count ? kUnlocks[i].help : ""; }
int state(int i) {
    if (i < 0 || i >= Count || !s_flagsKnown.load(std::memory_order_relaxed)) return -1;
    const Unlock& u = kUnlocks[i];
    return (s_flagsSeen[u.word].load(std::memory_order_relaxed) & u.mask) == u.mask;
}
void set(int i, bool on) {
    if (i < 0 || i >= Count) return;
    const Unlock& u = kUnlocks[i];
    (on ? s_setBits : s_clearBits)[u.word].fetch_or(u.mask);
    (on ? s_clearBits : s_setBits)[u.word].fetch_and(~u.mask);
    // shown at once (the game's frame update confirms it)
    s_flagsSeen[u.word].store(on ? s_flagsSeen[u.word].load() | u.mask : s_flagsSeen[u.word].load() & ~u.mask);
}
}  // namespace unlocks

// From GameFrameDelta (every mode loop's frame update): the menu's changes go in, the words come out.
static void unlocksFrame() {
    // R2_UNLOCKS=<seconds>:all or <seconds>:<item numbers> (unlocks::Item, comma-separated):
    // switched on that long after the start (tests of the menu's rows; the game loads its save
    // from the card at boot, over anything set before)
    static const char* env = getenv("R2_UNLOCKS");
    static const u64 envStart = tbNow();
    if (env && double(tbNow() - envStart) / double(kTbClock) >= atof(env)) {
        const char* list = strchr(env, ':') ? strchr(env, ':') + 1 : env;
        const std::string l = std::string(",") + list + ",";
        for (int i = 0; i < unlocks::Count; i++)
            if (!strcmp(list, "all") || l.find("," + std::to_string(i) + ",") != std::string::npos) unlocks::set(i, true);
        env = nullptr;
    }
    const u32 addr[2] = {kCheatFlags, kUnlockFlags};
    for (int w = 0; w < 2; w++) {
        const u32 on = s_setBits[w].exchange(0), off = s_clearBits[w].exchange(0);
        if (on | off) {
            const u32 v = rd32(addr[w]), nv = (v | on) & ~off;
            if (nv != v) {
                wr32(addr[w], nv);
                bool saved = false;
                for (const Unlock& u : kUnlocks)
                    if (u.word == w && ((v ^ nv) & u.mask) && u.saved) saved = true;
                if (saved) wr8(kProgress, 1);  // as the game's setters do: the front end saves it
                for (const Unlock& u : kUnlocks)
                    if (u.word == w && ((v ^ nv) & u.mask))
                        logf("unlock", "%s %s%s", u.name, (nv & u.mask) == u.mask ? "on" : "off",
                             u.saved ? " (kept in the game's save)" : "");
            }
        }
        s_flagsSeen[w].store(rd32(addr[w]), std::memory_order_relaxed);
    }
    s_flagsKnown.store(true, std::memory_order_relaxed);
}

// All craft (settings): CraftMask (0x7FCB9544; mission r3, game mode r4) builds the hangar's
// craft for a mission from the craft passcodes' unlock bits (+0x658: 0x400 .. 0x40000, 0x200000;
// copied to a mask at r13-19980) and the pilot's medals (AllMissionsMedal 0x7FCB9458: every
// main (r3 = 0) or every (r3 = 1) mission at medal level r4 or better; MissionMedal 0x7FCB94E4:
// mission r3 at level r4 - both only called by the craft code, 0x7FCB9458..0x7FCB9FB4). With the
// cheat both answer yes and the craft bits are set for the call only: every craft the game lets
// a mission have, nothing written to the save. Its per-mission rules (the snowspeeder only on
// Hoth's surface, ...) stay the game's.
void hle_CraftMask(Ctx& c) {
    const u32 mission = c.r[3];
    const bool all = settings::get().allCraft;
    const u32 v = all ? rd32(kUnlockFlags) : 0, add = all ? kCraftCodeBits & ~v : 0;
    if (add) wr32(kUnlockFlags, v | add);
    f_7FCB9544_orig(c);
    if (add) wr32(kUnlockFlags, rd32(kUnlockFlags) & ~add);
    static u32 lastMission = ~0u, lastMask = ~0u;
    if (mission != lastMission || c.r[3] != lastMask) {
        lastMission = mission;
        lastMask = c.r[3];
        logf("craft", "mission %u: craft %04X%s", mission, c.r[3], all ? " (all craft)" : "");
    }
}
void hle_AllMissionsMedal(Ctx& c) {
    if (settings::get().allCraft) {
        c.r[3] = 1;
        return;
    }
    f_7FCB9458_orig(c);
}
void hle_MissionMedal(Ctx& c) {
    if (settings::get().allCraft) {
        c.r[3] = 1;
        return;
    }
    f_7FCB94E4_orig(c);
}

// Invincibility (settings): CraftDestroy (0x7FC3E528, a method every craft class shares) starts
// a craft's explosion - whatever destroyed it, shots or a crash. The player's craft
// (0x7FDEFE14) is spared.
void hle_CraftDestroy(Ctx& c) {
    if (settings::get().invincible && c.r[3] == rd32(0x7FDEFE14)) return;
    // (VR: the controllers rumble for 2 s as the player's craft blows up)
    if (c.r[3] == rd32(0x7FDEFE14)) xr::haptic(1.0f, 2.0f);
    f_7FC3E528_orig(c);
}

// Infinite bombs: WeaponInit (0x7FC396D4) sets up each weapon of a craft - count (+0) and
// maximum (+4) from the craft's data, the weapon slot at +0x520. Weapon objects sit at a
// different place in each craft class (inside it or behind a pointer), so they are recorded
// here as the game initializes them. The player's secondary weapons are given back what they
// fire right away (WeaponUpdate, below) and topped up every frame (missions can also restore a
// count from a checkpoint); unlimited weapons have a maximum of 0 and are left alone.
//
// Death Star Attack fails ("You have no proton torpedoes left to fire at the exhaust port")
// from its mission script once the torpedo count runs out; the count has to stay up in every
// state the craft flies in, the trench run included, not only in free flight (+0x370 = 0).
struct WeaponRec {
    u32 craft, weapon, slot, max;
};
static std::vector<WeaponRec> s_weapons;

void hle_WeaponInit(Ctx& c) {
    const u32 weapon = c.r[3], craft = c.r[4], slot = c.r[5];
    f_7FC396D4_orig(c);
    const u32 max = rd32(weapon + 4);
    for (WeaponRec& w : s_weapons)
        if (w.weapon == weapon) {  // re-initialized (respawn), or memory reused by another craft
            w = {craft, weapon, slot, max};
            return;
        }
    if (s_weapons.size() >= 1024) s_weapons.erase(s_weapons.begin(), s_weapons.begin() + 512);
    s_weapons.push_back({craft, weapon, slot, max});
}

// fill one of the player craft's recorded weapons (a mission may have set a new maximum)
static void refill(WeaponRec& w) {
    if (w.max == 0 || rd32(w.weapon + 0x520) != w.slot) return;  // unlimited, or no longer this weapon
    const u32 max = rd32(w.weapon + 4);
    if (max == 0 || max > 999) return;
    w.max = max;
    const s32 count = s32(rd32(w.weapon));
    if (count >= 0 && count < s32(max)) wr32(w.weapon, max);  // (-1: unlimited already)
}

static void keepAmmo() {
    if (!settings::get().infiniteAmmo) return;
    const u32 craft = playerCraft();
    if (!craft) return;
    for (WeaponRec& w : s_weapons)
        if (w.craft == craft) refill(w);
}

// WeaponUpdate (0x7FC39F24, weapon r3, craft r4): a weapon's update, which fires it on the
// trigger and takes one from its count. The player's shot is given back before anything (HUD,
// mission script) reads the count.
void hle_WeaponUpdate(Ctx& c) {
    const u32 weapon = c.r[3], craft = c.r[4];
    f_7FC39F24_orig(c);
    if (!settings::get().infiniteAmmo || !craft || craft != playerCraft()) return;
    for (WeaponRec& w : s_weapons)
        if (w.weapon == weapon && w.craft == craft) {
            refill(w);
            break;
        }
}

// ...and its health (craft +0x6F4, a float: 180 for the X-wing) is kept full, so the HUD ring
// stays green. Only for an object whose vtable (+0x80) has CraftDestroy: 0x7FDEFE14 points
// at other things outside missions.
// The full value is what the craft has when it starts flying (state +0x370 = 0: the game fills it
// at a spawn and a respawn); while the craft is being set up the field holds other things, and
// keeping one of those (more than full) made the HUD's health ring vanish.
static u32 s_healthCraft = 0;  // keepHealth's craft and its full health
static float s_healthFull = 0.f;
static bool s_healthFlying = false;
static void keepHealth() {
    u32& craft = s_healthCraft;
    float& full = s_healthFull;
    bool& flyingBefore = s_healthFlying;
    const u32 p = settings::get().invincible ? playerCraft() : 0;
    if (p != craft) {
        craft = p;
        full = 0.f;
        flyingBefore = false;
    }
    if (!craft) return;
    const bool flying = rd32(craft + 0x370) == 0;
    const float h = float(s2d(rd32(craft + 0x6F4)));
    if (!flying) {
        flyingBefore = false;
        return;
    }
    if (!flyingBefore || full <= 0.f) {
        flyingBefore = true;
        full = h > 0.f && h <= 100000.f ? h : 0.f;
        return;
    }
    if (h < full) wr32(craft + 0x6F4, d2s(full));
}

// Debugging: R2_CRAFT_DIFF=<len hex>[,<deref offset hex>...]: every frame, compare the player
// craft object's first <len> bytes (or those of objects it points to at the given offsets)
// with the previous frame and log bytes/halfwords/words that drop by exactly 1 from a small
// value (ammo counters); fields that keep changing are muted after 8 reports.
static void craftDiffProbe() {
    static const char* spec = getenv("R2_CRAFT_DIFF");
    if (!spec) return;
    static u32 len = 0;
    static std::vector<s32> derefs;
    if (!len) {
        char* end;
        len = u32(strtoul(spec, &end, 16)) & ~3u;
        while (*end == ',') derefs.push_back(s32(strtol(end + 1, &end, 16)));
        if (derefs.empty()) derefs.push_back(-1);
    }
    const u32 craft = playerCraft();
    static std::vector<std::vector<u8>> prev;
    static std::vector<u32> prevBase;
    static std::unordered_map<u64, int> reports;
    prev.resize(derefs.size());
    prevBase.resize(derefs.size());
    for (size_t d = 0; d < derefs.size(); d++) {
        u32 base = craft;
        if (craft && derefs[d] >= 0) {
            base = rd32(craft + u32(derefs[d]));
            if ((base >> 24) != 0x80 && (base >> 24) != 0x81 && (base >> 24) != 0x7F) base = 0;
        }
        std::vector<u8> cur;
        if (base) {
            cur.resize(len);
            for (u32 o = 0; o < len; o += 4) {
                const u32 w = rd32(base + o);
                cur[o] = u8(w >> 24), cur[o + 1] = u8(w >> 16), cur[o + 2] = u8(w >> 8), cur[o + 3] = u8(w);
            }
        }
        if (base && base == prevBase[d] && prev[d].size() == len) {
            for (u32 o = 0; o < len; o++) {
                for (u32 size : {1u, 2u, 4u}) {
                    if (o % size) continue;
                    u32 a = 0, b = 0;
                    for (u32 i = 0; i < size; i++) a = a << 8 | prev[d][o + i], b = b << 8 | cur[o + i];
                    if (a >= 1 && a <= 255 && b == a - 1) {
                        const u64 key = (u64(d) << 40) | (u64(o) << 8) | size;
                        if (reports[key]++ < 8)
                            logf("diff", "craft %08X (%s+%X) +%03X size %u: %u -> %u", base, derefs[d] < 0 ? "obj" : "ptr",
                                 derefs[d] < 0 ? 0 : derefs[d], o, size, a, b);
                    }
                }
            }
        }
        prev[d] = std::move(cur);
        prevBase[d] = base;
    }
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "savestate.h"
namespace rt {
void hleSaveState(state::Writer& w) {
    w.section("HLE ");
    w.vec(s_weapons);
    for (const auto& t : s_portTables) w.pod(t.block), w.pod(t.size);  // (their data: memory.cpp)
    w.pod(s_tileListRecords), w.pod(s_heightRowsExtra);
    // the heap extension's blocks (their data: memory.cpp, with MEM1)
    std::vector<ExtCell> cells;
    for (const auto* m : {&s_extFree, &s_extUsed}) {
        cells.clear();
        for (const auto& [cell, size] : *m) cells.push_back({cell, size});
        w.vec(cells);
    }
    w.pod(s_extInUse), w.pod(s_extPeak);
}

void hleLoadState(state::Reader& r) {
    r.section("HLE ");
    r.vec(s_weapons);
    for (auto& t : s_portTables) r.pod(t.block), r.pod(t.size);
    r.pod(s_tileListRecords), r.pod(s_heightRowsExtra);
    std::vector<ExtCell> cells;
    for (auto* m : {&s_extFree, &s_extUsed}) {
        r.vec(cells);
        m->clear();
        for (const auto& e : cells) (*m)[e.cell] = e.size;
    }
    r.pod(s_extInUse), r.pod(s_extPeak);
}

// After a load (on the guest's fiber): caches of guest objects start over, a skip in progress
// ends, and the camera aspect constants hold what the saved session wrote (a stale aspect for
// the camera objects of the state if the window has another shape now).
void hleAfterLoad() {
    s_craftLast = 0;
    s_craftLastIsCraft = false;
    s_healthCraft = 0;
    s_healthFull = 0.f;
    s_healthFlying = false;
    if (s_skip) {
        s_skip = false;
        viSetRateHz(frameRateHz());
        presentPaced(true);
        setDvdBoost(1.0);
        setAudioSpeed(1.0, false);
    }
    const u32 bits = rd32(0x80099548);  // the world camera's (see setGameAspect)
    s_aspectWritten = bits;
    float f;
    memcpy(&f, &bits, 4);
    const float display = s_displayAspect.load(std::memory_order_relaxed);
    if (bits != k43 && f > 0.5f && f < 8.f && std::abs(f - display) >= 0.0005f && !oldGameAspect(f)) {
        const u32 i = s_oldAspectCount.fetch_add(1, std::memory_order_relaxed);
        s_oldAspects[i % 8].store(f, std::memory_order_relaxed);
    }
}
}  // namespace rt
