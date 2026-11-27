**English** | [Español](README_ES.md)

# Red Two VR

PC VR version of the **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube) recompilation.

Based on version 1.4 of Red Two: https://github.com/gminustwo/red_two/releases/tag/v1.4

**No game data is included**: you need your own disc image of the USA release (`GSWE64`,
revision 0), both for the downloads and for your own build.

**Download**: the Windows zip is on the [Releases](../../releases) page. Unzip it anywhere and run
`red_two.exe`; if it can't find the disc, the launcher asks for it (a `GSWE64.rvz` next to
`red_two.exe` is found by itself). The zip is portable (everything of yours stays in its `user`
folder) and brings the Visual C++ runtime with it.

## Video

[![Watch the video](https://img.youtube.com/vi/utCDfpDxA8A/0.jpg)](https://www.youtube.com/watch?v=utCDfpDxA8A)

## Virtual reality

- A headset with OpenXR running before you start: Virtual Desktop, Quest Link or SteamVR.
- The launcher's **VR** tab > **Play in VR** starts in the headset; `red_two.exe --no-xr` starts
  on the monitor.
- Flight in stereo 3D with head tracking and the HUD at a fixed distance; menus, the hangar and
  cutscenes on a virtual screen.
- **Resolution** (VR tab): what is rendered per eye, from Low (1728) to Godlike (3200), or the
  headset's own; the eyes reach the headset at full resolution.
- **Refresh rate** of the headset: 72, 80, 90 or 120 Hz, with the game at its own frame rate.
- **Haptics** on both Touch controllers: lasers faint, torpedoes hard, damage by the size of the
  hit, and a long rumble when your craft blows up.

### Touch controls

| Control | In flight |
|---|---|
| A | fire lasers |
| B | fire torpedoes (hold 2 s: skip a cutscene) |
| X | cockpit view |
| Y | cockpit view with the targeting computer (press to turn it on and off); in menus, audio description |
| Right grip + right stick up / down | move the camera closer |
| Left grip + left stick | give orders to your squadron (the d-pad) |
| Triggers | throttle and brake |

## Spanish

Red Two VR can be played in Spanish (texts, menus and radio voices) with the pieces extracted from
your own disc of the Spanish release (`GSWS64`), in a `red_two_es` folder next to the USA disc:

```
red_two.exe --extract-spanish <your GSWS64 disc image> red_two_es
```

It writes `frontendSP.bin`, `resDir_rows.bin`, `staticGame_sp.bin`, `text_Spanish.bin`,
`voiceDir.bin` and `r2voice.vdy` (about 132 MB). Then, in the launcher: GAME > Game language >
Español. GAME > Language puts Red Two's own menus in Spanish.

## Building (Windows)

Requirements: Visual Studio 2022 Build Tools (C++), CMake 3.25+, Ninja, clang-cl (LLVM).

```powershell
# 1. Build: recompiles the game straight from your disc image (RVZ, WIA, ISO or GCM)
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Disc "<your Rogue Leader.rvz>"

# 2. Run
build\windows\red_two.exe --disc "<your Rogue Leader.rvz>"
```

Without `--disc` the game takes the disc chosen in the launcher before (F1 > GAME > Game disc),
else looks for `orig\GSWE64.iso`, `orig\GSWE64.rvz`, `GSWE64.rvz`, `GSWE64.iso` and any GSWE64
image next to the program or in the user folder, else the launcher asks for it.
`tools\rvz.py` (Python + libzstd) can still extract a disc or convert it to ISO.

The code generated from your disc ends up in `build/` and is yours alone: never share it.

`tools\make_release_win.ps1` packs a build into `build\RedTwo-windows-x64.zip` (the program, its
symbols, DLLs, a readme) for your own use on another PC.

### Playing

- **Launcher**: Red Two opens on its settings (the same pages as the in-game menu) before the
  game starts. Start game (the first row) or the controller's START launches it. Turn off
  "Show launcher at start" there to go straight into the game from then on; the in-game menu
  (F1 or Back) has the same switch and "Back to launcher". `--launcher` / `--no-launcher` override
  the setting for one start. A reset from the menu goes straight back into the game.
- Gamepads work through SDL (with rumble), also while the window is in the background. On an
  Xbox-style pad the face buttons follow the GameCube positions: A = A (fire), X (left) = B,
  B (right) = X, Y = Y; LT/RT = L/R (throttle and brake), LB and RB = Z (hold with the stick to roll),
  right stick = C-stick, Menu = Start (a PlayStation
  controller: cross = A, circle = B, square = X, triangle = Y). Keyboard: WASD or arrows steer, Space = A (fire),
  Left Shift = B, E = X, Q = Y, R = Z, 1/2 = L/R, Enter = Start, IJKL = C-stick, TFGH = D-pad.
- The game's button prompts show your controller's buttons (the one each GameCube button is
  bound to, e.g. "PRESS (Y)" and SELECT CRAFT's "(B) > pilot"): the PlayStation symbols on a
  PS3 / PS4 / PS5 controller, the Switch letters on a Switch Pro Controller or Joy-Cons, the Xbox
  buttons on any other, and SELECT CRAFT's C-stick becomes the right stick. F1 > Controls >
  Button prompts picks Automatic, Xbox, PlayStation, Switch or the original GameCube ones.
- **Sticks** (F1 > Controls): the control stick and the C-stick each have a sensitivity (25-200%)
  and a round deadzone (0-50%; the output grows from zero at its edge), and the stick's whole
  travel past the deadzone is the game's range. **Response** sets how the craft's turning follows
  the control stick: the game's own GameCube curve (default), Smooth (gentle near the center,
  building up evenly to full speed) or Linear. A meter for each stick shows where it is and what the
  game gets (the turn and pitch speed for the control stick). **Invert Y axis** makes the stick climb when
  pushed up and dive when pulled down, in flight only (the menus keep up = up).
- **Triggers**: L and R are analog, as on a GameCube controller: throttle and brake build up over
  the whole pull, and the click at the end gives full boost or full brake. **Move trigger clicks to
  LB / RB** (Controls) puts the clicks on the bumpers instead, so the triggers only throttle and
  brake (roll, Z, then moves to the right stick click). Bars on the Controls page show both
  triggers and what the game gets.
- **Gyro aiming** (Controls): turn and tilt the controller to steer in flight, on top of the stick,
  with the Steam Deck, a DualSense, DualShock 4 or Switch Pro Controller (Xbox controllers have no
  gyro): always on, while a button is held, toggled, or off while held - the Deck's left stick
  touch or a finger on the touchpad keeps it on while it rests there. Steam Input hides the gyro:
  turn it off for Red Two (Properties > Controller).
- F11 or Alt+Enter toggles fullscreen; `--fullscreen` starts that way.
- **Graphics** (F1, DISPLAY): internal resolution up to 4K, 4x MSAA anti-aliasing (from the next
  start; WebGPU offers no other sample count) and anisotropic filtering up to 16x. **Widescreen**
  (16:9, 16:10, 21:9 or the window's shape) widens the 3D view, puts the flight HUD at the screen
  edges and fills the screen in cutscenes (**Cinematic bars** keeps them in their 2.1:1 cinema
  frame with black bars instead); wider windows get bars at the sides. **Frame rate**: 60,
  90, 120 or 144 frames a second, the game at its normal speed. **V-Sync** (off by default) shows
  each frame in step with the display's refresh: no tearing, a little more input lag. With it on, a
  frame rate at or above the display's refresh runs at the refresh (144 on a 120 Hz display: 120),
  and cutscene skips still run at full speed.
- **Cutscenes** (F1, GAMEPLAY): with Skippable cutscenes on, hold B for 2 seconds in the opening
  crawl or a cutscene to skip it (a ring in the corner fills while you hold); holding B also skips
  the intro at start. Skip intro logos reaches the title in about 10 seconds by itself.
- **Special Features**: the documentary and the teaser play at their own speed at any frame rate;
  left / right jumps 5 seconds, and A, B or START pauses them (CONTINUE / BACK). While the audio
  commentary plays in a mission, L3 (keyboard C; F1 > Controls > Hotkeys) pauses it.
- **The mouse** works in the game's own menus: point at an item to select it, click for A, right
  click for B, and the wheel moves up and down (also on the pause screen and in the art gallery).
- **Loading**: Fast loading (Gameplay, on) reads the disc at SSD speed instead of the GameCube
  drive's 3 MB/s. The disc image is read ahead of the game on a thread of its own, so it can sit on
  a hard disk or a network drive without the game stuttering while it streams music and voices.
- **Where your files are**: on Windows a `user` folder next to `red_two.exe` (the zip has one)
  holds settings, the memory card, save states, logs, texture packs, codes and the shader cache,
  so the folder can live anywhere (another drive, a NAS). An install that has no `user` folder
  but an older `%APPDATA%\Red Two` keeps using that one; create `user` to make it portable (its
  first start copies the settings, controls and memory card over).
- **Cheats** (F1 > Cheats): invincibility, infinite lives, infinite bombs, perfect mission
  stats (the results screen says 0:01, every enemy destroyed, 100% accuracy, no friendlies or
  lives lost and 100% targeting computer efficiency, and the medal is awarded for those), and
  all craft (every craft a mission allows waits in its hangar). **Passcodes**: the game's own
  unlocks without typing codes - all missions, all tech upgrades, ace mode, art gallery, music
  hall, documentary, audio commentary, credits, black and white - kept in the game's save as
  if the codes had been entered.
- **Reset game**, **Back to launcher** and **Quit to desktop** are on the menu's first tab, GAME.
- **Save states**: Back + RB saves, Back + LB loads, Back + D-pad right / left picks the slot
  (8 slots; keyboard F5 / F8, F7 / F6). F1 > Controls has the slot, save / load buttons and the
  hotkeys, each any combination of buttons (or keys) held together: pick the row, press A, hold
  the buttons, let go. On a controller Back alone (released with nothing else pressed) still
  opens the menu. States live in the user folder's `states` and also load after a restart, but
  only into the same build of Red Two (a rebuild makes them "OLD"). The memory card is not part
  of a state: loading one does not undo the game's own saves.
- Saves go to the user folder's `saves/GSWE64_A.raw` (slot A; a new card comes formatted and the
  game makes its save at the first start without asking). A
  `.raw` card from Dolphin or Nintendont can be copied there as it is, whatever its size (59 to
  2043 blocks). `R2_CARD_A=<path>` uses another card image.
- **Mods** (F1, MODS tab): texture packs in Dolphin's format go in the user folder's
  `textures\` (a `GSWE64` folder inside, exactly as in Dolphin's
  `Load\Textures`; `.dds` and `.png`, mip files and wildcards work), or stay wherever you unpacked
  them: MODS -> Texture pack folder opens a folder browser (d-pad / stick, A opens, X goes up, B
  back) - pick the pack's folder (the one holding `GSWE64`, or `GSWE64` itself), e.g. in Downloads
  or on an SD card. The Texture pack folder row shows how many textures loaded from it, and says
  why when there are none (an archive still to unpack, images not named `tex1_...`). The IMPORT
  FROM DOLPHIN rows are only about copying things from Dolphin. Back + D-pad up (F9) turns the
  texture packs off and on again at once, to compare (rebindable in Controls). Gecko / Action
  Replay codes and
  patches go in the user folder's `codes.ini` (Dolphin's GameSettings format) and can be switched
  in the menu. The game is recompiled ahead of time, so codes that change its code (Gecko C2
  "insert ASM", writes into code) cannot work; the menu marks them as not supported.
- **Importing from Dolphin**: MODS -> IMPORT FROM DOLPHIN, or
  `red_two.exe --import-dolphin[=saves,textures,codes] --console`. Red Two finds Dolphin's user
  folder (AppData, Documents, a portable install, the registry, a custom Load folder in
  `Dolphin.ini`) and brings over this game's newest save (from GCI folders or `.raw` cards; the
  previous card is kept in `saves\backup`, and the game restarts), its texture packs (copied) and
  its codes (with their on/off states). `R2_DOLPHIN_DIR=<User folder>` points it elsewhere.
- **Shaders** (F1, DISPLAY -> PERFORMANCE): "Prepare shaders at start" (on) compiles every shader
  the game has used on this device before it starts; off, the game starts at once and objects may
  pop in while their shaders compile. "Ubershaders" (on) draws not-yet-compiled objects with a
  generic shader instead of skipping them.
- **Sound** (F1, AUDIO): the game mixes for Dolby Pro Logic II. **Speakers**: Automatic follows
  the system's speaker setup; Stereo sends the encoded mix for a receiver's Pro Logic II decoder;
  5.1 and 7.1 give the rear its own speakers (7.1 splits it into side and back pairs). Keep the
  game's own sound setting (Options > Sound Settings) on STEREO. **Audio buffer** (normal, large,
  huge): a larger one if the sound is choppy. `R2_NO_AUDIO=1` runs without sound.
- Every session writes a log to the user folder's `logs`,
  also when another program starts Red Two (RecompLauncher, a terminal): that program gets a copy
  of it as well. `--console` shows it in a console window on Windows.

## Credits

Red Two is gminustwo's [recompilation](https://github.com/gminustwo/red_two). Some features come
from ZeDctr's [Red Two Plus](https://github.com/ZeDctr/red_two_plus) fork:
hold-to-skip cutscenes, the documentary's speed fix, seeking and pause menu, Switch button
prompts, the memory card's first save, pausing the audio commentary, the mouse in the game's menus,
cinematic bars and faster audio catch-up. Rendering is [Aurora](https://github.com/encounter/aurora)'s.

## Legal

Red Two is released under the [MIT License](LICENSE). It covers this repository's own code
only, not the game's code that a build (and the prebuilt downloads) contain in recompiled form.

This repository contains no code or assets from the game. Star Wars and Rogue Leader are
trademarks of Lucasfilm Ltd.; GameCube is a trademark of Nintendo. This project is not
affiliated with or endorsed by them.
