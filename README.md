# Red Two

Version PCVR del recomp **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube).

Basado en la versión 1.3 de Rengar https://github.com/gminustwo/red_two/releases/tag/v1.3


**No se incluyen datos de juego**: necesitas tu propia imagen de disco de la versión estadounidense (`GSWE64`, revisión 0), tanto para las descargas como para tu propia compilación.

Esta versión admite jugar en Castellano, si utilizamos en una carpeta \red_two_es\ los ficheros extraídos de la rom spain de gamecube del juego.
Se requieren: 
frontendSP.bin
resDir_rows.bin
staticGame_sp.bin
text_Spanish.bin
voiceDir.bin
r2voice.vdy

Ocupan 132Mb.

-Añadida opción de control eje Y invertido


## Building (Windows)

Requirements: Visual Studio 2022 Build Tools (C++), CMake 3.25+, Ninja, clang-cl (LLVM).

```powershell
# 1. Build: recompiles the game straight from your disc image (RVZ, WIA, ISO or GCM)
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Disc "<your Rogue Leader.rvz>"

# 2. Run
build\windows\red_two.exe --disc "<your Rogue Leader.rvz>"
```

Without `--disc` the game takes the disc chosen in the launcher before (F1 > DISPLAY > Game disc),
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
  B (right) = X, Y = Y; LT/RT = L/R, RB = Z, right stick = C-stick, Menu = Start (a PlayStation
  controller: cross = A, circle = B, square = X, triangle = Y). Keyboard: WASD or arrows steer, Space = A (fire),
  Left Shift = B, E = X, Q = Y, R = Z, 1/2 = L/R, Enter = Start, IJKL = C-stick, TFGH = D-pad.
- The game's button prompts show your controller's buttons (the one each GameCube button is
  bound to, e.g. "PRESS (Y)" and SELECT CRAFT's "(B) > pilot"): the PlayStation symbols on a
  PS3 / PS4 / PS5 controller, the Xbox buttons on any other. F1 > Controls > Button prompts
  picks Automatic, Xbox, PlayStation or the original GameCube ones. The C-stick icon stays.
- **Sticks** (F1 > Controls): the control stick and the C-stick each have a sensitivity (25-200%)
  and a round deadzone (0-50%; the output grows from zero at its edge), and the stick's whole
  travel past the deadzone is the game's range. **Response** sets how the craft's turning follows
  the control stick: Smooth (default: gentle near the center, building up evenly to full speed),
  Linear, or the game's own GameCube curve. A meter for each stick shows where it is and what the
  game gets (the turn and pitch speed for the control stick).
- **Triggers**: L and R are analog, as on a GameCube controller: throttle and brake build up over
  the whole pull, and the click at the end gives full boost or full brake. **Trigger clicks on LB /
  RB** (Controls) moves the clicks to the bumpers, so the triggers only throttle and brake (Z then
  moves to the right stick click). Bars on the Controls page show both triggers and what the game
  gets.
- **Gyro aiming** (Controls): turn and tilt the controller to steer in flight, on top of the stick,
  with the Steam Deck, a DualSense, DualShock 4 or Switch Pro Controller (Xbox controllers have no
  gyro): always on, while a button is held, toggled, or off while held - the Deck's left stick
  touch or a finger on the touchpad keeps it on while it rests there. Steam Input hides the gyro:
  turn it off for Red Two (Properties > Controller).
- F11 or Alt+Enter toggles fullscreen; `--fullscreen` starts that way.
- **Graphics** (F1, DISPLAY): internal resolution up to 4K, 4x MSAA anti-aliasing (from the next
  start; WebGPU offers no other sample count) and anisotropic filtering up to 16x. **Widescreen**
  (16:9, 16:10, 21:9 or the window's shape) widens the 3D view, puts the flight HUD at the screen
  edges and fills the screen in cutscenes; wider windows get bars at the sides. **Frame rate**: 60,
  90, 120 or 144 frames a second, the game at its normal speed. **V-Sync** (off by default) shows
  each frame in step with the display's refresh: no tearing, a little more input lag. With it on, a
  frame rate at or above the display's refresh runs at the refresh (144 on a 120 Hz display: 120),
  and cutscene skips still run at full speed.
- **Loading**: Fast loading (Gameplay, on) reads the disc at SSD speed instead of the GameCube
  drive's 3 MB/s. The disc image is read ahead of the game on a thread of its own, so it can sit on
  a hard disk or a network drive without the game stuttering while it streams music and voices.
- **Where your files are**: on Windows a `user` folder next to `red_two.exe` (the zip has one)
  holds settings, the memory card, save states, logs, texture packs, codes and the shader cache,
  so the folder can live anywhere (another drive, a NAS). An install that has no `user` folder
  but an older `%APPDATA%\Red Two` keeps using that one; create `user` to make it portable (its
  first start copies the settings, controls and memory card over). Linux: `~/.local/share/Red Two/`;
  macOS: `~/Library/Application Support/Red Two/`.
- **Cheats** (F1 > Cheats): invincibility, infinite lives, infinite bombs, perfect mission
  stats (the results screen says 0:01, every enemy destroyed, 100% accuracy, no friendlies or
  lives lost and 100% targeting computer efficiency, and the medal is awarded for those), and
  all craft (every craft a mission allows waits in its hangar). **Passcodes**: the game's own
  unlocks without typing codes - all missions, all tech upgrades, ace mode, art gallery, music
  hall, documentary, audio commentary, credits, black and white - kept in the game's save as
  if the codes had been entered.
- **Reset game**, **Back to launcher** and **Quit to desktop** are at the top of the menu's first page.
- **Save states**: Back + RB saves, Back + LB loads, Back + D-pad right / left picks the slot
  (8 slots; keyboard F5 / F8, F7 / F6). F1 > Controls has the slot, save / load buttons and the
  hotkeys, each any combination of buttons (or keys) held together: pick the row, press A, hold
  the buttons, let go. On a controller Back alone (released with nothing else pressed) still
  opens the menu. States live in the user folder's `states` and also load after a restart, but
  only into the same build of Red Two (a rebuild makes them "OLD"). The memory card is not part
  of a state: loading one does not undo the game's own saves.
- Saves go to the user folder's `saves/GSWE64_A.raw` (slot A; the game formats a new card). A
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
  pop in while their shaders compile. The downloads bring a starter list of the game's shaders, so
  a new install's first start compiles them before the game begins (about 20 seconds on a desktop,
  once; later starts take about a second). "Ubershaders" (on; off on a Steam Deck's first start)
  draws not-yet-compiled objects with a generic shader instead of skipping them.
- **Sound** (F1, AUDIO): the game mixes for Dolby Pro Logic II. **Speakers**: Automatic follows
  the system's speaker setup; Stereo sends the encoded mix for a receiver's Pro Logic II decoder;
  5.1 and 7.1 give the rear its own speakers (7.1 splits it into side and back pairs). Keep the
  game's own sound setting (Options > Sound Settings) on STEREO. `R2_NO_AUDIO=1` runs without
  sound.
- Every session writes a log to the user folder's `logs` 
  also when another program starts Red Two (RecompLauncher, a terminal): that program gets a copy
  of it as well. `--console` shows it in a console window on Windows.


## Legal

Red Two is released under the [MIT License](LICENSE). It covers this repository's own code
only, not the game's code that a build (and the prebuilt downloads) contain in recompiled form.

This repository contains no code or assets from the game. Star Wars and Rogue Leader are
trademarks of Lucasfilm Ltd.; GameCube is a trademark of Nintendo. This project is not
affiliated with or endorsed by them.
