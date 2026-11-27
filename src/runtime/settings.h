// Player settings: settings.ini in the user folder (mods::userDir), edited in the in-game menu (F1 or the
// controller's Back/Select button, see gfx/menu.cpp). Automated runs (R2_BACKGROUND,
// R2_HEADLESS, R2_INPUT) neither read nor write the file; R2_SET="key=value;..." overrides
// any setting for them (and for the player's own runs).
#pragma once
#include <string>

namespace rt::settings {

enum class Resolution : int { Window = 0, X1, X2, X3, X4K, Count };  // internal (EFB) resolution
// settings.ini keeps the numbers of the old list: 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 (was 32:9:
// now 21:9), 5 match the window
enum class Aspect : int { R4_3 = 0, R16_9, R16_10, R21_9, Window, Count };
// gyro aiming (gyro.cpp): when the controller's gyro steers. Held / Paused / Toggle use gyroButton.
enum class GyroMode : int { Off = 0, Always, Held, Toggle, Paused, Count };

// The widest aspect rendered (21:9); a wider window gets bars at the sides. Wider views make the
// game's cameras cull for more of the world and it keeps more in its virtual memory, which ARAM
// backs (1536 4 KiB slots): the Battle of Hoth intro at 32:9 needed 387+ slots more in its first
// second and stopped with "FATAL VM ERROR: Out of ARAM memory"; at 21:9 its gameplay uses ~1014.
inline constexpr float kMaxAspect = 64.f / 27.f;

// The game's button prompts (gfx/button_icons.cpp): Auto shows PlayStation symbols while port 1's
// controller is a PlayStation one and Xbox buttons otherwise; Xbox / PlayStation always those;
// GameCube the game's own. settings.ini: button_prompts=auto|xbox|playstation|gamecube (the
// older xbox_buttons=1/0 reads as auto / gamecube).
enum class ButtonPrompts : int { Auto = 0, Xbox, PlayStation, GameCube, Count };

// How the craft's turn and pitch rate follow the control stick (input.cpp mainStickToGame):
// Smooth grows with the deflection to the power 1.5, Linear in proportion, GameCube is the game's
// own curve (half rate up to 80% of the stick, then three times as steep). settings.ini:
// stick_curve=smooth|linear|gamecube
enum class StickCurve : int { Smooth = 0, Linear, GameCube, Count };

// Sound output (audio.cpp): Auto follows the default device's channels (the system's speaker
// setup); 5.1 / 7.1 play the game's Pro Logic II surround on speakers of their own, stereo plays
// it matrix-encoded (a receiver's Pro Logic II decoder unfolds it). settings.ini:
// speakers=auto|stereo|5.1|7.1
enum class Speakers : int { Auto = 0, Stereo, Surround51, Surround71, Count };

struct Settings {
    // start: the launcher (the settings menu before the game boots, gfx/menu.cpp) or straight
    // into the game. On until the player turns it off; --launcher / R2_LAUNCHER=1 show it anyway.
    bool launcher = true;
    // At start, ask GitHub whether a newer release is out (update.cpp); the menu offers it.
    bool checkUpdates = false;
    // Discord Rich Presence (discord.cpp): the player's Discord profile shows the mission and what
    // they are doing in it, through the Discord app on this computer. Off: nothing goes to Discord.
    bool discordPresence = false;  // (no switch in this build's menu: off unless set in settings.ini)
    // The menu's language (the launcher and the in-game menu): 0 English, 1 Spanish (gfx/menu_es.inc)
    int language = 0;
    // The game's language (its texts, menus and voices): 0 English, 1 Spanish - from the Spanish disc
    // (GSWS64, found beside the USA one or at spanishDiscPath; disc_language.cpp). From the next start.
    int gameLanguage = 0;
    std::string spanishDiscPath;
    // VR stereo 3D (--xr, gfx/xr.cpp; also set in the headset: hold the right grip and move
    // the right thumbstick): game units per metre of head movement (more: stronger depth, a
    // smaller-looking world), and how far ahead of the game's camera the eyes sit, in metres.
    float xrWorldScale = 1.f;
    float xrCameraForward = 0.f;
    // The 3D screen (not flying: the hangar, cinematics): how far apart the eyes are, times
    // the real distance between them (1: true to the scene; more: stronger).
    float xrScreenDepth = 2.f;
    // In flight: the HUD (and the radar's window) on a plane this far ahead of the game's camera,
    // in game units: fixed to the game's view, the reticle over the aim point.
    float xrHudDistance = 10.f;
    // The headset's refresh rate asked of the OpenXR runtime (XR_FB_display_refresh_rate; Quest
    // through Virtual Desktop, Quest Link, SteamVR may offer it): 0 = the runtime's own, else 72,
    // 80, 90 or 120 Hz. The game still runs at its own rate (fps); the runtime fills the headset's
    // frames in between from the last one, turned with the head.
    int xrRefreshRate = 0;
    // VR: what the game renders at while in the headset (each eye in flight, the virtual screen
    // otherwise), in lines per eye: 0 = the Display page's internal resolution, -1 = what the
    // headset's OpenXR runtime recommends, else 1728..3200 (Virtual Desktop's Quest 3 / Pico 4
    // levels, low to godlike)
    int xrResolution = 0;
    // VR: start in the headset (OpenXR) as --xr does; --no-xr starts on the monitor anyway
    bool vr = false;
    // Stereo 3D in flight (off: the game on a flat virtual screen all the time); from the next start
    bool xrStereo = true;
    // In flight: how far round the game's camera the game draws the world (times its own view):
    // the headset sees more than the game's 40 degrees. More: no ground missing under the craft
    // when looking down or to the sides; less: fewer far buildings appearing late.
    int xrViewRange = 5;
    // The flat virtual screen (menus, hangar, cinematics; and everything with stereo 3D off):
    // its width in metres, 2.5 m away
    float xrScreenWidth = 3.2f;
    // The game disc image chosen in the launcher (its disc page): used at every start unless
    // --disc gives another; empty or unusable: found next to the program, else asked for.
    std::string discPath;
    // display
    bool fullscreen = false;
    Resolution resolution = Resolution::Window;
    Aspect aspect = Aspect::R4_3;
    bool hudEdges = true;  // widescreen: the flight HUD at the screen edges
    bool tileMenus = true;  // widescreen: menu backgrounds continue into the sides
    // The game's own dither: a screen-space texture blended over every frame, sized for 640x480;
    // above that it shows as a coarse grid (the Hoth sky). Off like Dolphin's "Disable Dithering" code.
    bool gameDither = false;
    int fps = 60;  // 60, 90, 120 or 144: the emulated retrace rate
    // V-Sync: frames are shown in step with the display's refresh (WebGPU's FIFO present mode).
    // Off: the newest frame is shown at the next refresh without waiting (mailbox; no tearing in
    // a window or borderless fullscreen), as before. The game paces itself on its emulated retrace
    // either way; with V-Sync on a frame rate above the display's refresh waits for it.
    bool vsync = false;
    bool showFps = false;
    // Multisample anti-aliasing: 1 (off) or 4 samples (WebGPU allows no other count); Aurora sets
    // it up when it starts, so a change applies from the next start
    int msaa = 1;
    // Anisotropic filtering forced on mipmapped, filtered textures: 1 (off: the game's own), 2, 4,
    // 8 or 16 (Aurora patch 0030; applies at once)
    int anisotropy = 1;
    // While a shader compiles, a generic one (Aurora's ubershader) draws the object instead of it
    // being skipped (Dolphin's hybrid ubershaders). Without it a shader never seen before on this
    // PC skips its objects for a few frames: flicker all through a first playthrough (issue #7).
    // On by default (since 1.2): its pipelines take ~20 s to compile once (8 workers here), then
    // load from the cache in under a second. Off on a Steam Deck's first start (slower to compile
    // there) and in automated runs (their shader cache stays as it was).
    bool ubershaders = true;
    // Every shader in the cache compiled before the game starts ("PREPARING SHADERS",
    // gx_bridge.cpp precompileShaders). Off: the game starts at once and they compile while it
    // runs (objects whose shader is not ready yet are skipped for a few frames: pop-in).
    bool precompileShaders = true;
    // audio
    int volume = 100;            // master volume, 0..100 (output gain (volume/100)^2)
    bool backgroundAudio = true;  // keep playing while the window is in the background
    Speakers speakers = Speakers::Auto;
    // the sound's cushion (audio.cpp): 0 normal (50 ms), 1 large (100 ms), 2 huge (160 ms); more
    // rides out a slower PC's late frames without crackling, a little later behind the picture
    int audioBuffer = 0;
    // game
    bool skipLogos = false;      // boot straight past the legal screen, intro and logos
    bool skipCutscenes = false;  // B (or Start) skips the opening crawl and in-game cutscenes
    // Disc reads at 200 MB/s instead of the drive's 3 MB/s (Dolphin's "Speed up Disc Transfer
    // Rate"): the hangar + Battle of Hoth load read ~50 MiB, ~16 s of it waiting on the drive.
    // Automated runs default to the drive's speed (their input scripts are timed for it).
    bool fastDisc = true;
    // cheats
    bool invincible = false;
    bool infiniteLives = false;
    bool infiniteAmmo = false;  // secondary weapons: torpedoes, bombs, missiles
    bool perfectStats = false;  // mission results: 0:01, every enemy, 100%, nothing lost (hle_game.cpp)
    bool allCraft = false;      // every craft a mission allows in its hangar (passcodes and medals; hle_game.cpp)
    // RetroAchievements (achievements.cpp): softcore only. The account's name and its login token
    // (from logging in with the password in the menu, or Dolphin's RetroAchievements.ini); the
    // password itself is never stored.
    bool achievements = false;
    std::string raUser, raToken;
    bool raNotifications = true;  // pop-ups for unlocks, leaderboards, the game's progress
    bool raEncore = false;        // achievements already unlocked can trigger (and pop up) again
    // mods
    bool customTextures = true;  // texture packs in the textures folder (Dolphin's format; mods.cpp)
    bool prefetchTextures = true;  // the packs loaded into memory up front (Dolphin's prefetch)
    // A folder anywhere (picked in the MODS page's folder browser) whose texture packs load too,
    // besides Red Two's own textures folder: e.g. a pack unpacked in ~/Downloads. Empty: none.
    std::string texturesFolder;
    // controls
    ButtonPrompts buttonPrompts = ButtonPrompts::Auto;  // the game's on-screen button prompts (gfx/button_icons.cpp)
    // Controller sticks (input.cpp shapeStick): a round deadzone in % of the stick's travel that
    // the output rescales from (both sticks), and the control stick's sensitivity in % (output =
    // input x sensitivity, clamped: above 100 full deflection comes sooner, below it never does)
    int stickDeadzone = 15;      // 0..50 (the control stick; the C-stick has its own)
    int cStickDeadzone = 15;     // 0..50
    int cStickSensitivity = 100;  // 25..200
    // The triggers' clicks (full boost / full brake) on LB / RB, the triggers analog only, Z from
    // RB to the right stick click (input.cpp placeClicks: the controllers' bindings)
    bool bumperClicks = false;
    int stickSensitivity = 100;  // 25..200
    StickCurve stickCurve = StickCurve::Smooth;
    // In flight, the control stick's up / down reversed (forward climbs, back dives); the menus
    // keep it as it is.
    bool stickInvertY = false;
    // In flight, Y (the targeting computer) toggles: a press turns it on, the next off. Off: it
    // shows while Y is held, as on the GameCube.
    bool targetingToggle = true;
    // Gyro aiming (gyro.cpp): turning and tilting the controller (the Steam Deck itself, a
    // DualSense, a Switch Pro Controller) steers the craft in flight, on top of the stick. The
    // button is an SDL gamepad button name (misc3 is the Deck's left stick touch; "touch" a finger
    // on a touchpad); on a controller without it the gyro uses the first of misc3, touch and
    // leftstick it has. Sensitivities are percent (100: the craft turns as far as the controller).
    GyroMode gyroMode = GyroMode::Off;
    std::string gyroButton = "misc3";
    int gyroSpeedX = 100, gyroSpeedY = 100;
    bool gyroInvertY = false;
    // save states (savestate.cpp): the slot the hotkeys use, and the hotkeys (hotkeys.cpp), each a
    // combination held together: controller buttons by SDL name, keys by SDL key name, " + "
    // between them; empty = none
    int stateSlot = 1;
    std::string hotkeyPad[5] = {"back + rightshoulder", "back + leftshoulder", "back + dpright", "back + dpleft", "back + dpup"};
    std::string hotkeyKeys[5] = {"F5", "F8", "F7", "F6", "F9"};
};
// hotkey actions (the index into hotkeyPad / hotkeyKeys)
// (HkToggleTextures: texture packs hidden / shown at once, mods.cpp toggleTexturesShown)
enum HotkeyAction : int { HkSaveState = 0, HkLoadState, HkNextSlot, HkPrevSlot, HkToggleTextures, HkCount };
const char* hotkeyDefault(int action, bool pad);

Settings& get();
void load();  // once at startup, before anything reads the settings
void save();  // after a change in the menu (no-op for automated runs)
std::string summary();  // every setting on one line (the session log)

const char* resolutionName(Resolution r);
const char* aspectName(Aspect a);
const char* buttonPromptsName(ButtonPrompts p);  // as settings.ini writes it
const char* gyroModeName(GyroMode m);
const char* speakersName(Speakers s);  // as settings.ini writes it
const char* stickCurveName(StickCurve c);  // as settings.ini writes it
inline constexpr int kGyroSpeedMin = 10, kGyroSpeedMax = 400;
float resolutionScale(Resolution r);  // Aurora frame buffer scale; 0 = the window's own size
float aspectValue(Aspect a);          // width / height (4:3 = the game's own); -1 = the window's
inline constexpr int kFpsChoices[] = {60, 90, 120, 144};

}  // namespace rt::settings
