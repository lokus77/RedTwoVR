#include "settings.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mods.h"
#include "runtime.h"

namespace rt::settings {
namespace {

Settings s_settings;
std::string s_path;  // empty: do not persist (automated runs)
const char* const kHotkeyNames[HkCount] = {"save_state", "load_state", "next_slot", "previous_slot", "toggle_textures", "commentary"};

bool parseBool(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

#ifndef _WIN32
std::string dmiField(const char* name) {
    std::string s;
    if (FILE* f = fopen((std::string("/sys/devices/virtual/dmi/id/") + name).c_str(), "r")) {
        char buf[64];
        if (fgets(buf, sizeof buf, f)) s = buf;
        fclose(f);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
#endif

// A Steam Deck. Steam sets SteamDeck=1 for what it starts (Game Mode, or a Steam shortcut in
// Desktop Mode), but not for a start from the desktop (Dolphin, a terminal): the user's first
// start was one, so it got the desktop defaults (4:3 in a window). The DMI names say it either
// way: Valve's Jupiter (LCD) and Galileo (OLED).
bool steamDeck() {
    if (const char* deck = getenv("SteamDeck"); deck && !strcmp(deck, "1")) return true;
#ifdef _WIN32
    return false;
#else
    if (dmiField("sys_vendor") != "Valve") return false;
    const std::string product = dmiField("product_name");
    return product == "Jupiter" || product == "Galileo";
#endif
}

void set(const std::string& key, const std::string& value) {
    Settings& s = s_settings;
    const int n = atoi(value.c_str());
    if (key == "launcher") s.launcher = parseBool(value);
    else if (key == "disc_path") s.discPath = value;
    else if (key == "fullscreen") s.fullscreen = parseBool(value);
    else if (key == "resolution") { if (n >= 0 && n < int(Resolution::Count)) s.resolution = Resolution(n); }
    else if (key == "msaa") s.msaa = n >= 4 ? 4 : 1;
    else if (key == "anisotropy") s.anisotropy = n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
    else if (key == "aspect") {
        if (n >= 0 && n <= 3) s.aspect = Aspect(n);
        else if (n == 4) s.aspect = Aspect::R21_9;  // 32:9 (retired: kMaxAspect)
        else if (n == 5) s.aspect = Aspect::Window;
    }
    else if (key == "fps") { for (int f : kFpsChoices) if (f == n) s.fps = n; }
    else if (key == "show_fps") s.showFps = parseBool(value);
    else if (key == "ubershaders") s.ubershaders = parseBool(value);
    else if (key == "precompile_shaders") s.precompileShaders = parseBool(value);
    else if (key == "hud_edges") s.hudEdges = parseBool(value);
    else if (key == "tile_menus") s.tileMenus = parseBool(value);
    else if (key == "cinematic_bars") s.cinemaBars = parseBool(value);
    else if (key == "game_dither") s.gameDither = parseBool(value);
    else if (key == "vsync") s.vsync = parseBool(value);
    else if (key == "volume") s.volume = n < 0 ? 0 : n > 100 ? 100 : n;
    else if (key == "background_audio") s.backgroundAudio = parseBool(value);
    else if (key == "check_updates") s.checkUpdates = parseBool(value);
    else if (key == "discord_presence") s.discordPresence = parseBool(value);
    else if (key == "xr_world_scale") s.xrWorldScale = std::clamp(float(atof(value.c_str())), 0.05f, 1000.f);
    else if (key == "xr_hud_distance") s.xrHudDistance = std::clamp(float(atof(value.c_str())), 0.5f, 1000.f);
    else if (key == "language") s.language = value == "es" ? 1 : 0;
    else if (key == "game_language") s.gameLanguage = value == "es" ? 1 : 0;
    else if (key == "spanish_disc_path") s.spanishDiscPath = value;
    else if (key == "xr_resolution") {
        // (a scale, as the first test build wrote it: 4.5 = 2160 lines)
        const double v = atof(value.c_str());
        s.xrResolution = v > 0.0 && v < 10.0 ? int(v * 480.0 + 0.5) : std::clamp(n, -1, 4096);
    }
    else if (key == "xr_refresh_rate") s.xrRefreshRate = std::clamp(n, 0, 240);
    else if (key == "xr_stereo") s.xrStereo = parseBool(value);
    else if (key == "vr") s.vr = parseBool(value);
    else if (key == "xr_view_range") s.xrViewRange = std::clamp(n, 1, 12);
    else if (key == "xr_screen_width") s.xrScreenWidth = std::clamp(float(atof(value.c_str())), 0.5f, 20.f);
    else if (key == "xr_screen_depth") s.xrScreenDepth = std::clamp(float(atof(value.c_str())), 0.f, 5000.f);
    else if (key == "xr_camera_forward") s.xrCameraForward = std::clamp(float(atof(value.c_str())), -200.f, 200.f);
    else if (key == "audio_buffer") s.audioBuffer = std::clamp(n, 0, 2);
    else if (key == "speakers") {
        for (int i = 0; i < int(Speakers::Count); i++)
            if (value == speakersName(Speakers(i))) s.speakers = Speakers(i);
    }
    else if (key == "skip_logos") s.skipLogos = parseBool(value);
    else if (key == "fast_disc") s.fastDisc = parseBool(value);
    else if (key == "skip_cutscenes") s.skipCutscenes = parseBool(value);
    else if (key == "invincible") s.invincible = parseBool(value);
    else if (key == "infinite_lives") s.infiniteLives = parseBool(value);
    else if (key == "infinite_ammo") s.infiniteAmmo = parseBool(value);
    else if (key == "perfect_stats") s.perfectStats = parseBool(value);
    else if (key == "all_craft") s.allCraft = parseBool(value);
    else if (key == "achievements") s.achievements = parseBool(value);
    else if (key == "ra_user") s.raUser = value;
    else if (key == "ra_token") s.raToken = value;
    else if (key == "ra_notifications") s.raNotifications = parseBool(value);
    else if (key == "ra_encore") s.raEncore = parseBool(value);
    else if (key == "custom_textures") s.customTextures = parseBool(value);
    else if (key == "prefetch_textures") s.prefetchTextures = parseBool(value);
    else if (key == "textures_folder") s.texturesFolder = value;
    else if (key == "button_prompts") {
        for (int i = 0; i < int(ButtonPrompts::Count); i++)
            if (value == buttonPromptsName(ButtonPrompts(i))) s.buttonPrompts = ButtonPrompts(i);
    } else if (key == "stick_deadzone") s.stickDeadzone = std::clamp(n, 0, 50);
    else if (key == "stick_sensitivity") s.stickSensitivity = std::clamp(n, 25, 200);
    else if (key == "cstick_deadzone") s.cStickDeadzone = std::clamp(n, 0, 50);
    else if (key == "cstick_sensitivity") s.cStickSensitivity = std::clamp(n, 25, 200);
    else if (key == "stick_curve") {
        for (int i = 0; i < int(StickCurve::Count); i++)
            if (value == stickCurveName(StickCurve(i))) s.stickCurve = StickCurve(i);
    }
    else if (key == "xbox_buttons")  // (before button_prompts: on = Xbox or, now, PlayStation)
        s.buttonPrompts = parseBool(value) ? ButtonPrompts::Auto : ButtonPrompts::GameCube;
    else if (key == "gyro") { if (n >= 0 && n < int(GyroMode::Count)) s.gyroMode = GyroMode(n); }
    else if (key == "gyro_button") s.gyroButton = value;
    else if (key == "bumper_clicks") s.bumperClicks = parseBool(value);
    else if (key == "invert_y") s.invertY = parseBool(value);
    else if (key == "gyro_speed_x") s.gyroSpeedX = std::clamp(n, kGyroSpeedMin, kGyroSpeedMax);
    else if (key == "gyro_speed_y") s.gyroSpeedY = std::clamp(n, kGyroSpeedMin, kGyroSpeedMax);
    else if (key == "gyro_invert_y") s.gyroInvertY = parseBool(value);
    else if (key == "stick_invert_y") s.invertY = parseBool(value);  // (Red Two VR 0.1-0.2's name: now invert_y)
    else if (key == "targeting_toggle") s.targetingToggle = parseBool(value);
    else if (key == "state_slot") s.stateSlot = n < 1 ? 1 : n > 8 ? 8 : n;
    else {
        for (int a = 0; a < HkCount; a++) {
            const std::string base = std::string("hotkey_") + kHotkeyNames[a];
            if (key == base + "_pad") return void(s.hotkeyPad[a] = value);
            if (key == base + "_keys") return void(s.hotkeyKeys[a] = value);
        }
        logf("config", "unknown setting '%s'", key.c_str());
    }
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

void parseLine(const std::string& line) {
    const std::string l = trim(line);
    if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '[') return;
    const size_t eq = l.find('=');
    if (eq == std::string::npos) return;
    set(trim(l.substr(0, eq)), trim(l.substr(eq + 1)));
}

}  // namespace

Settings& get() { return s_settings; }

void load() {
    const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    if (automated) {
        s_settings.fastDisc = false;     // input scripts are timed for the drive's speed
        s_settings.ubershaders = false;  // (their tests and shader cache were made without)
    }
    if (const std::string note = mods::userDirNote(); !note.empty()) logf("config", "%s", note.c_str());
    if (!automated) {
        if (const std::string dir = mods::userDir(); !dir.empty()) s_path = dir + "settings.ini";
    }
    if (!s_path.empty()) {
        if (FILE* f = fopen(s_path.c_str(), "r")) {
            char buf[4096];  // (textures_folder holds a path)
            while (fgets(buf, sizeof buf, f)) parseLine(buf);
            fclose(f);
            logf("config", "settings from %s", s_path.c_str());
        } else if (steamDeck()) {
            // the first start on a Steam Deck: fullscreen, the picture as wide as its 16:10
            // screen, and texture packs read from the SSD when used rather than all prefetched:
            // the Deck's 16 GB are shared with the GPU, which keeps up to 4 GB of replacement
            // textures itself, and prefetching the user's 6 GB pack made the process hold ~7 GiB
            // more (on the PC) - most of the memory Steam and the system leave. DDS packs load on
            // Aurora's workers in time anyway (on the PC one load pending in 526 Hoth samples).
            s_settings.fullscreen = true;
            s_settings.aspect = Aspect::Window;
            s_settings.prefetchTextures = false;
            s_settings.ubershaders = false;  // (a faster start: they take long to compile there)
            logf("config", "first start on a Steam Deck: fullscreen, aspect of the screen, no texture prefetch, no ubershaders");
        } else {
#ifdef __ANDROID__
            // the first start on Android: the picture as wide as the screen (wider phones get
            // 21:9 and bars), rendered at 960 lines rather than the screen's own (a phone's GPU)
            s_settings.fullscreen = true;
            s_settings.aspect = Aspect::Window;
            s_settings.resolution = Resolution::X2;
            s_settings.ubershaders = false;
            logf("config", "first start on Android: aspect of the screen, 960p");
#endif
        }
    }
    // R2_SET="fps=120;aspect=1": overrides, e.g. for test runs
    if (const char* o = getenv("R2_SET")) {
        std::string all = o;
        size_t start = 0;
        while (start <= all.size()) {
            const size_t end = all.find(';', start);
            parseLine(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    logf("config", "settings: %s", summary().c_str());
    // R2_SAVE_SETTINGS=1: settings.ini written at once (a release's default one, from a fresh user folder)
    if (const char* w = getenv("R2_SAVE_SETTINGS"); w && atoi(w)) save();
}

// every setting on one line, for the session log
std::string summary() {
    const Settings& s = s_settings;
    char b[4096];
    snprintf(b, sizeof b,
             "disc_path=%s launcher=%d check_updates=%d discord_presence=%d fullscreen=%d resolution=%s msaa=%d anisotropy=%d aspect=%s fps=%d vsync=%d ubershaders=%d precompile_shaders=%d hud_edges=%d tile_menus=%d cinematic_bars=%d game_dither=%d "
             "show_fps=%d volume=%d background_audio=%d speakers=%s skip_logos=%d skip_cutscenes=%d fast_disc=%d invincible=%d "
             "infinite_lives=%d infinite_ammo=%d perfect_stats=%d all_craft=%d custom_textures=%d prefetch_textures=%d "
             "textures_folder=%s button_prompts=%s stick_deadzone=%d stick_sensitivity=%d stick_curve=%s cstick_deadzone=%d cstick_sensitivity=%d bumper_clicks=%d invert_y=%d gyro=%s gyro_button=%s gyro_speed=%d/%d gyro_invert_y=%d state_slot=%d achievements=%d ra_user=%s ra_notifications=%d ra_encore=%d",
             s.discPath.empty() ? "-" : s.discPath.c_str(), s.launcher, s.checkUpdates, s.discordPresence, s.fullscreen, resolutionName(s.resolution), s.msaa, s.anisotropy, aspectName(s.aspect), s.fps, s.vsync, s.ubershaders, s.precompileShaders, s.hudEdges,
             s.tileMenus, s.cinemaBars, s.gameDither, s.showFps, s.volume, s.backgroundAudio, speakersName(s.speakers), s.skipLogos, s.skipCutscenes,
             s.fastDisc, s.invincible, s.infiniteLives, s.infiniteAmmo, s.perfectStats, s.allCraft, s.customTextures,
             s.prefetchTextures, s.texturesFolder.empty() ? "-" : s.texturesFolder.c_str(), buttonPromptsName(s.buttonPrompts), s.stickDeadzone, s.stickSensitivity, stickCurveName(s.stickCurve), s.cStickDeadzone, s.cStickSensitivity, s.bumperClicks, s.invertY, gyroModeName(s.gyroMode),
             s.gyroButton.c_str(), s.gyroSpeedX, s.gyroSpeedY, s.gyroInvertY, s.stateSlot, s.achievements, s.raUser.empty() ? "-" : s.raUser.c_str(),
             s.raNotifications, s.raEncore);
    return b;
}

void save() {
    logf("config", "settings changed: %s", summary().c_str());
    if (s_path.empty()) return;
    const Settings& s = s_settings;
    FILE* f = fopen(s_path.c_str(), "w");
    if (!f) {
        logf("config", "cannot write %s", s_path.c_str());
        return;
    }
    fprintf(f, "# Red Two settings (also editable in game: F1 or the controller's Back/Select button)\n");
    fprintf(f, "# your Rogue Leader disc image (chosen in the launcher); empty: one next to Red Two, else it asks\n");
    fprintf(f, "disc_path=%s\n", s.discPath.c_str());
    fprintf(f, "# the launcher (these settings before the game starts); 0: straight into the game\n");
    fprintf(f, "launcher=%d\n", s.launcher);
    fprintf(f, "fullscreen=%d\n", s.fullscreen);
    fprintf(f, "# 0 window size, 1 480p, 2 960p, 3 1440p, 4 2160p (4K)\n");
    fprintf(f, "resolution=%d\n", int(s.resolution));
    fprintf(f, "# multisample anti-aliasing: 1 off, 4 (4x); from the next start\n");
    fprintf(f, "msaa=%d\n", s.msaa);
    fprintf(f, "# anisotropic filtering forced on: 1 off (the game's own), 2, 4, 8, 16\n");
    fprintf(f, "anisotropy=%d\n", s.anisotropy);
    fprintf(f, "# 0 4:3, 1 16:9, 2 16:10, 3 21:9, 5 match the window (at most 21:9)\n");
    fprintf(f, "aspect=%d\n", s.aspect == Aspect::Window ? 5 : int(s.aspect));
    fprintf(f, "fps=%d\n", s.fps);
    fprintf(f, "# frames shown in step with the display's refresh (no tearing, a little more input lag)\n");
    fprintf(f, "vsync=%d\n", s.vsync);
    fprintf(f, "show_fps=%d\n", s.showFps);
    fprintf(f, "# a generic shader draws objects whose own shader is still compiling (no pop-in)\n");
    fprintf(f, "ubershaders=%d\n", s.ubershaders);
    fprintf(f, "# every shader this PC has seen compiled before the game starts (0: start at once, shaders compile in play)\n");
    fprintf(f, "precompile_shaders=%d\n", s.precompileShaders);
    fprintf(f, "hud_edges=%d\n", s.hudEdges);
    fprintf(f, "tile_menus=%d\n", s.tileMenus);
    fprintf(f, "cinematic_bars=%d\n", s.cinemaBars);
    fprintf(f, "# the game's own dither pattern (made for 640x480: a grid at higher resolutions)\n");
    fprintf(f, "game_dither=%d\n", s.gameDither);
    fprintf(f, "# master volume 0..100\n");
    fprintf(f, "volume=%d\n", s.volume);
    fprintf(f, "background_audio=%d\n", s.backgroundAudio);
    fprintf(f, "# ask GitHub for a newer release at start (the menu offers to install it)\n");
    fprintf(f, "check_updates=%d\n", s.checkUpdates);
    fprintf(f, "# Discord Rich Presence: your Discord profile shows the mission (through the Discord app on this PC)\n");
    fprintf(f, "discord_presence=%d\n", s.discordPresence);
    fprintf(f, "# VR (--xr) stereo 3D: game units per metre (more: stronger depth) and the eyes' distance ahead of the\n");
    fprintf(f, "# game's camera in metres; in the headset: hold the right grip and move the right thumbstick\n");
    fprintf(f, "xr_world_scale=%g\n", s.xrWorldScale);
    fprintf(f, "xr_camera_forward=%g\n", s.xrCameraForward);
    fprintf(f, "# VR: depth of the 3D screen (hangar, cinematics); in the headset: right grip + right thumbstick left/right\n");
    fprintf(f, "xr_screen_depth=%g\n", s.xrScreenDepth);
    fprintf(f, "# VR: in flight, the HUD's distance ahead of the camera (game units; more: farther)\n");
    fprintf(f, "xr_hud_distance=%g\n", s.xrHudDistance);
    fprintf(f, "# VR: start in the headset (OpenXR), as with --xr (--no-xr: on the monitor anyway)\n");
    fprintf(f, "vr=%d\n", s.vr);
    fprintf(f, "# VR: the headset's refresh rate (0: the runtime's own; 72, 80, 90, 120 Hz); the game keeps its own fps\n");
    fprintf(f, "xr_refresh_rate=%d\n", s.xrRefreshRate);
    fprintf(f, "# VR: the game's resolution in the headset, lines per eye (0: the internal resolution, -1: the headset's own; 1728..3200)\n");
    fprintf(f, "xr_resolution=%d\n", s.xrResolution);
    fprintf(f, "# the menu's language: en (English) or es (Spanish)\n");
    fprintf(f, "language=%s\n", s.language == 1 ? "es" : "en");
    fprintf(f, "# the game's texts, menus and voices: en, or es (needs the Spanish disc, GSWS64, beside the USA one or at spanish_disc_path)\n");
    fprintf(f, "game_language=%s\n", s.gameLanguage == 1 ? "es" : "en");
    fprintf(f, "spanish_disc_path=%s\n", s.spanishDiscPath.c_str());
    fprintf(f, "# VR: stereo 3D in flight (0: a flat virtual screen all the time)\n");
    fprintf(f, "xr_stereo=%d\n", s.xrStereo);
    fprintf(f, "# VR: in flight, how far round the game's view the world is drawn (times; more: no ground missing)\n");
    fprintf(f, "xr_view_range=%d\n", s.xrViewRange);
    fprintf(f, "# VR: the flat virtual screen's width in metres (2.5 m away)\n");
    fprintf(f, "xr_screen_width=%g\n", s.xrScreenWidth);
    fprintf(f, "# auto (the system's speaker setup), stereo (Pro Logic II matrix-encoded), 5.1 or 7.1\n");
    fprintf(f, "speakers=%s\n", speakersName(s.speakers));
    fprintf(f, "# the sound's buffer: 0 normal (50 ms), 1 large (100 ms), 2 huge (160 ms; choppy sound on a slower PC)\n");
    fprintf(f, "audio_buffer=%d\n", s.audioBuffer);
    fprintf(f, "skip_logos=%d\n", s.skipLogos);
    fprintf(f, "skip_cutscenes=%d\n", s.skipCutscenes);
    fprintf(f, "# disc reads at SSD speed rather than the GameCube drive's 3 MB/s (shorter loading)\n");
    fprintf(f, "fast_disc=%d\n", s.fastDisc);
    fprintf(f, "invincible=%d\n", s.invincible);
    fprintf(f, "infinite_lives=%d\n", s.infiniteLives);
    fprintf(f, "infinite_ammo=%d\n", s.infiniteAmmo);
    fprintf(f, "perfect_stats=%d\n", s.perfectStats);
    fprintf(f, "# every craft a mission allows in its hangar (the craft passcodes and medal rewards)\n");
    fprintf(f, "all_craft=%d\n", s.allCraft);
    fprintf(f, "# texture packs in the textures folder next to this file (Dolphin's format)\n");
    fprintf(f, "custom_textures=%d\n", s.customTextures);
    fprintf(f, "# texture packs loaded into memory before the game starts (up to half the RAM)\n");
    fprintf(f, "prefetch_textures=%d\n", s.prefetchTextures);
    fprintf(f, "# another folder whose texture packs load too (the folder holding GSWE64, or GSWE64 itself); empty: none\n");
    fprintf(f, "textures_folder=%s\n", s.texturesFolder.c_str());
    fprintf(f, "# on-screen button prompts: auto (PlayStation symbols for a PlayStation controller, else Xbox), xbox, playstation, gamecube\n");
    fprintf(f, "button_prompts=%s\n", buttonPromptsName(s.buttonPrompts));
    fprintf(f, "# controller sticks: a round deadzone in %% of the travel (0-50, both sticks), the control stick's sensitivity in %% (25-200)\n");
    fprintf(f, "stick_deadzone=%d\n", s.stickDeadzone);
    fprintf(f, "stick_sensitivity=%d\n", s.stickSensitivity);
    fprintf(f, "# the C-stick (the controller's right stick): deadzone 0..50, sensitivity 25..200\n");
    fprintf(f, "cstick_deadzone=%d\n", s.cStickDeadzone);
    fprintf(f, "cstick_sensitivity=%d\n", s.cStickSensitivity);
    fprintf(f, "# how turning follows the control stick: smooth, linear or gamecube (the game's own curve)\n");
    fprintf(f, "stick_curve=%s\n", stickCurveName(s.stickCurve));
    fprintf(f, "# in flight Y (the targeting computer) toggles on / off (0: shown while held)\n");
    fprintf(f, "targeting_toggle=%d\n", s.targetingToggle);
    fprintf(f, "# gyro aiming in flight: 0 off, 1 always, 2 while the button is held, 3 the button toggles it, 4 off while held\n");
    fprintf(f, "gyro=%d\n", int(s.gyroMode));
    fprintf(f, "# the triggers' clicks (full boost / brake) on LB / RB, the triggers analog only, Z on the right stick click\n");
    fprintf(f, "bumper_clicks=%d\n", s.bumperClicks);
    fprintf(f, "invert_y=%d\n", s.invertY);
    fprintf(f, "# SDL gamepad button name: misc3 / misc4 the Steam Deck's left / right stick touch, paddle1-4 R4 L4 R5 L5\n");
    fprintf(f, "gyro_button=%s\n", s.gyroButton.c_str());
    fprintf(f, "# percent (%d-%d): turning (left / right) and pitch (up / down)\n", kGyroSpeedMin, kGyroSpeedMax);
    fprintf(f, "gyro_speed_x=%d\n", s.gyroSpeedX);
    fprintf(f, "gyro_speed_y=%d\n", s.gyroSpeedY);
    fprintf(f, "gyro_invert_y=%d\n", s.gyroInvertY);
    fprintf(f, "# RetroAchievements (softcore): the account and its login token (not the password)\n");
    fprintf(f, "achievements=%d\n", s.achievements);
    fprintf(f, "ra_user=%s\n", s.raUser.c_str());
    fprintf(f, "ra_token=%s\n", s.raToken.c_str());
    fprintf(f, "ra_notifications=%d\n", s.raNotifications);
    fprintf(f, "ra_encore=%d\n", s.raEncore);
    fprintf(f, "# save states: the slot (1-8) and the hotkeys, buttons or keys held together (SDL names, \" + \" between)\n");
    fprintf(f, "state_slot=%d\n", s.stateSlot);
    for (int a = 0; a < HkCount; a++) {
        fprintf(f, "hotkey_%s_pad=%s\n", kHotkeyNames[a], s.hotkeyPad[a].c_str());
        fprintf(f, "hotkey_%s_keys=%s\n", kHotkeyNames[a], s.hotkeyKeys[a].c_str());
    }
    fclose(f);
}

const char* hotkeyDefault(int action, bool pad) {
    static const Settings d;
    if (action < 0 || action >= HkCount) return "";
    return pad ? d.hotkeyPad[action].c_str() : d.hotkeyKeys[action].c_str();
}

const char* resolutionName(Resolution r) {
    switch (r) {
    case Resolution::Window: return "Window size";
    case Resolution::X1: return "480p (native)";
    case Resolution::X2: return "960p (2x)";
    case Resolution::X3: return "1440p (3x)";
    case Resolution::X4K: return "2160p (4K)";
    default: return "?";
    }
}

const char* aspectName(Aspect a) {
    switch (a) {
    case Aspect::R4_3: return "4:3 (original)";
    case Aspect::R16_9: return "16:9 widescreen";
    case Aspect::R16_10: return "16:10";
    case Aspect::R21_9: return "21:9 ultrawide";
    case Aspect::Window: return "Match window";
    default: return "?";
    }
}

const char* buttonPromptsName(ButtonPrompts p) {
    switch (p) {
    case ButtonPrompts::Xbox: return "xbox";
    case ButtonPrompts::PlayStation: return "playstation";
    case ButtonPrompts::Switch: return "switch";
    case ButtonPrompts::GameCube: return "gamecube";
    default: return "auto";
    }
}

const char* stickCurveName(StickCurve c) {
    switch (c) {
    case StickCurve::Linear: return "linear";
    case StickCurve::GameCube: return "gamecube";
    default: return "smooth";
    }
}

const char* speakersName(Speakers s) {
    switch (s) {
    case Speakers::Stereo: return "stereo";
    case Speakers::Surround51: return "5.1";
    case Speakers::Surround71: return "7.1";
    default: return "auto";
    }
}

const char* gyroModeName(GyroMode m) {
    switch (m) {
    case GyroMode::Off: return "Off";
    case GyroMode::Always: return "Always on";
    case GyroMode::Held: return "While held";
    case GyroMode::Toggle: return "Button toggles";
    case GyroMode::Paused: return "Off while held";
    default: return "?";
    }
}

float resolutionScale(Resolution r) {
    switch (r) {
    case Resolution::X1: return 1.f;
    case Resolution::X2: return 2.f;
    case Resolution::X3: return 3.f;
    case Resolution::X4K: return 4.5f;  // 480 * 4.5 = 2160 lines
    default: return 0.f;
    }
}

float aspectValue(Aspect a) {
    switch (a) {
    case Aspect::R16_9: return 16.f / 9.f;
    case Aspect::R16_10: return 16.f / 10.f;
    case Aspect::R21_9: return 64.f / 27.f;  // 2560x1080 / 3440x1440 class panels
    case Aspect::Window: return -1.f;
    default: return 4.f / 3.f;  // the game's own aspect, pillarboxed in a wider window
    }
}

}  // namespace rt::settings
