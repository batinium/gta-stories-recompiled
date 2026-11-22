# GTA: Liberty City Stories (second game target)

The same runtime and most of the same PC / Android features also run **GTA: Liberty City Stories**, recompiled from
its PS2 executable the way Vice City Stories is. LCS is newer and less tested than VCS, but it is playable: from boot
through the opening, the first mission, free roam on all three islands, driving, bikes, boats, helicopters, wanted
levels, death and arrest, saving and loading, and all 76 mission-launcher entries start and run (Windows and macOS,
hidden test runs; nobody has played the whole story through yet). On a phone it reached gameplay; everything added
since is built but not checked there.

## For players (short version)

- **You need**: your own copy of the USA disc image, **SLUS-21423** (`SYSTEM.CNF` VER 1.01; executable `SLUS_214.23`,
  sha1 `9b021b4c4e42087bb9b374709f26cf67744b75ab`). Other regions / versions are refused. The build reads the
  executable from your `.iso`; the game streams everything else from it. Nothing from the disc is in the repository;
  the generated code, the game binary and the game library built from it are yours only (never commit, upload or
  share them: [UPSTREAM_AGENTS.md](UPSTREAM_AGENTS.md)).
- **Windows**: start `GTAStoriesLauncher.exe`, choose (or drop) the LCS `.iso`, press *Build the game* (first time about 10
  minutes; the compiler downloads itself), then *Play*. Draw distance, resolution and the other options: *Game
  settings* on Home before you play, or F2 in the game. The terminal route is under "Building".
- **macOS / Linux**: `python3 tools/build.py --game lcs --recompile "<iso>"`, then `build/lcs/lcs`.
- **Android**: the one app ("GTA Stories Recompiled") plays either game, LCS selected first; you build LCS's game library
  from your disc (section "Android"). Everywhere LCS is the default game; `tools/build.py` without `--game` still builds
  VCS (upstream's behaviour), so always say `--game lcs`.
- **Works as on VCS**: keyboard / mouse / gamepad controls (mouse camera on foot and in vehicles, free aim, scopes,
  drive-by aiming, radio wheel, the pause menu by pointer), widescreen and HUD options, frame rates from 30 up to 240
  with the game's timing kept right, draw distance, traffic and pedestrian density, native sound and movies, saves
  (the game's own safehouses, plus Save game here, quick save F9 and quick load F10), the trainer (F5: player,
  weapons, vehicles, teleport, world, camera, missions, save), the free camera (F6), Hide HUD, and the mod loader.
- **Different or missing**: Drive-by fire rate is greyed (LCS has nothing to pace); button prompts are text names, not
  button pictures; blue police cars are not done (the PS2's police lights already flash red and blue); the pale,
  white-ish midday sky is the PS2's own look, not a bug.
- **Your data** (memory card, saves, mods) is LCS's own folder, apart from VCS's (both under GTAStoriesRecomp, never upstream's VCSRecomp):
  `%LOCALAPPDATA%\GTAStoriesRecomp\LCS` (Windows), `~/Library/Application Support/GTAStoriesRecomp/LCS` (macOS),
  `~/.local/share/GTAStoriesRecomp/LCS` (Linux), `files/userdata-lcs` (Android).

## For developers

- Where it lives: `game/lcs/` (function map `functions.csv` + `extra_functions.csv`, recompiler config `lcs.toml`,
  `overrides.toml`, `vu1_uploads.txt`), `src/game/lcs_*.cpp` (one module per feature, each address commented with its
  evidence), `src/game/lcs_overrides.cpp` (`applyLcs`: what is installed for the LCS executable). VCS's own files carry
  small `[lcs]` seams where LCS shares their code. Design details per platform: [ANDROID.md](ANDROID.md) ("Games:
  Vice City Stories and Liberty City Stories"); addresses: [game-internals.md](game-internals.md).
- Sections below: Building, The desktop launcher, Testing, Android, Features (the table), Overlay theme, Switches,
  Mission smoke test, Draw distance, High frame rates, RSL functions, Known issues, Mods, Police cars and pressure.

## Building

LCS's generated code never goes into the repository's `generated/` (that is VCS's): its default is `generated-lcs/`
(git-ignored, never committed), or a folder you choose. LCS builds in a folder of its own, `build/lcs/` (its CMake
tree, the `lcs` binary - `lcs.exe` on Windows -, its `vcs.ini`), so the two games never rebuild over each other. Run
everything from the repository root.

| | Windows | macOS | Linux |
|---|---|---|---|
| Compiler | nothing to install: llvm-mingw, CMake and Ninja are downloaded by the first build (~190 MB, once) | Apple's Command Line Tools (`xcode-select --install`) | clang 19+, lld and the packages `python3 tools/build.py --check-deps` lists ([LINUX.md](LINUX.md)) |
| First build | `python tools/build.py --game lcs --recompile "<iso>"` | `python3 tools/build.py --game lcs --recompile "<iso>"` | same as macOS |
| Run | `build\lcs\lcs.exe` | `build/lcs/lcs` | `build/lcs/lcs` |
| Without a terminal | `GTAStoriesLauncher.exe` (next section) | `GTAStoriesLauncher.app` (`build.py --launcher`; its LCS side is unchecked on macOS) | `GTAStoriesLauncher` (unchecked for LCS) |

Checked for LCS: Windows (launcher and terminal), macOS (terminal). Linux uses the same pipeline but LCS has not been
built there.

```
# 1. Once, and after every update that changes game/lcs/ or the recompiler: your disc -> generated-lcs/, then build
python tools/build.py --game lcs --recompile "<iso>"            # add --quick for a shorter build (= --opt none)

# 2. Later rebuilds (the disc is remembered in build/lcs/vcs.ini; the build says when step 1 is due again)
python tools/build.py --game lcs --quick
python tools/build.py --game lcs --recompile-if-needed          # recompiles only when the generated code is stale

# 3. Run
build/lcs/lcs                                                   # Windows: build\lcs\lcs.exe
```

- `--recompile` extracts `SLUS_214.23` from the disc, builds the recompiler, writes `generated-lcs/ee`, compiles LCS's
  VU1 microcode into `generated-lcs/vu1` (`game/lcs/vu1_uploads.txt`) and stamps it (`generated-lcs/recompile_stamp.json`:
  a hash of `game/lcs` and the recompiler). The recompile took 22 s, a launcher-driven Quick build from nothing
  9 min 40 s, on the Windows test PC.
- **Another folder for the generated code**: `--generated <dir>` on every build (the folder or its `ee/`; build.py
  reconfigures CMake when it changes), or the recompile on its own: `tools/recompile.py --game lcs [--out <dir>]
  <your LCS iso>`; `--check` says whether that code is current.
- **Stale code**: every LCS build compares the stamp with the source and prints `WARNING: the generated code in ...
  is stale` (before the build and again on its last line) when `game/lcs/functions.csv`, the config or the recompiler
  changed - the function map changes as bugs in it are found (2026-11-18: the function containing 0x19BDC0-0x19BDD8
  had been cut short, which stopped the game thread after a death or a wreck - "frozen, black screen"). Generated code
  made before the stamp existed is reported as stale too ("no stamp"): recompile once.
- `--quick` (= `--opt none`) keeps rebuilds short; the default and `--opt lto` are ThinLTO (the fastest game, a long
  link, kept in `build/lcs`'s CMake cache). `--opt pgo`, `--launcher` and `--iop-tests` are VCS's. `build.py` without
  `--game` builds VCS exactly as before.
- `VCS_BUILD_DIR=<folder>` builds LCS in that folder instead of `build/lcs` (the `lcs` binary at its top). A build
  folder chosen with `tools/build_dir.py` puts LCS's in its `lcs/` subfolder.
- **The disc**: the first argument, or `iso=<path>` in the `vcs.ini` next to the `lcs` binary (build.py writes the disc
  it was given there), or the first `*.iso` next to it or in the working folder. For the tools: `--recompile <iso>`,
  `LCS_ISO`, that `iso=`, or a `*Liberty City Stories*.iso` in the repository root (`VCS_ISO` stays VCS's). The settings
  are that `vcs.ini` too, so LCS's settings are apart from VCS's `build/vcs.ini`.
- **The data folder** (memory card `mc0`, the executable cache, card backups, `mods/`) is LCS's own (see "For players"),
  printed at start (`[lcs] data folder: ...`). `VCS_DATA_DIR=<folder>` chooses one outright (Android does this).
- Windows: the system may reserve TCP ports 49683-49982 and 50000-50159 (`netsh int ipv4 show excludedportrange
  protocol=tcp`); pick another `PS2X_CONTROL_PORT` for test runs.

## The desktop launcher (Windows, 2026-11-18)

`GTAStoriesLauncher.exe` (docs/LAUNCHER.md, "Two games") builds and plays LCS next to VCS, without a terminal:

1. Choose (or drop) the LCS `.iso`: the disc check knows both games (`src/disc/known_games.h`), and an LCS disc
   switches the launcher to **Liberty City Stories** (the title bar's switch; LCS is the default at the first start,
   and only a Vice City Stories disc without an LCS one makes it start there). The disc is remembered in `build/lcs/vcs.ini`.
2. *Build the game*: `tools/build.py --game lcs --recompile-if-needed <iso> --progress json` - the recompile into
   `generated-lcs/` when its stamp is stale, then `build/lcs/lcs.exe` (Standard = ThinLTO, or Quick; no Fastest/PGO for
   LCS). `tools/doctor.py --json --game lcs` says what's next (its disc, `generated-lcs/`'s stamp, `build/lcs`'s
   `build_info.json`).
3. *Play*: `build/lcs/lcs.exe <iso>` in `build/lcs`, which takes its own data folder (`%LOCALAPPDATA%\GTAStoriesRecomp\LCS`).
   *Saves* lists LCS's slots (`mc0/BASLUS-21423GTA30001`, `backup.py --game lcs list`), each with *Play* (the boot
   loads that slot: `VCS_LOAD_SLOT`), and backs up / restores LCS's card on its own.
4. *Game settings* (Home, Settings, Help; 2026-11-18): LCS's own settings before it starts, grouped as on Android's
   side panel - Display (internal resolution 1x-8x and Match window, aspect, HUD, frame rate cap, timing, vsync, MSAA,
   texture filtering, **Colour filter resolution**), World (**draw distance 100-1000%**, fog and far plane, model
   streaming, traffic, pedestrians), Audio, Controls, Game. The sheet takes them from the game itself
   (`lcs.exe --dump-settings-json`: the registry with LCS's ranges and locks; VCS-only rows such as Drive-by fire rate
   are left out) and writes `build/lcs/vcs.ini` (the file next to `lcs.exe`, not the data folder); a change applies at
   the next start, F2 in the game changes them live. docs/LAUNCHER.md "Game settings". Checked 2026-11-18: draw
   distance 650% and Match window set in the sheet, then a hidden `lcs.exe` answered `setting enhance.draw_distance`
   -> `ok 650`, `setting display.scale` -> `ok 0`.

The first time the player has control (after the opening cutscene: `src/game/player_control.cpp` - CPad 0
DisablePlayerControls `0x408C60 + 0x82` clear, the active CCam's mode `TheCamera 0x43BBF0 + 0x220 + ActiveCam (u8
+0x10C) x 0x2B0 + 0x40` = 4 on foot, or the player in a vehicle outside modes 14-17; for 2 s), it shows "Press F2 for
settings, F5 for the trainer, F6 for the free camera" (the keys as bound; `hints.keys=1` in vcs.ini afterwards).
Never in hidden runs; `VCS_KEYS_HINT=1` forces it, `=0` turns it off (switches table).

With LCS shown the launcher takes LCS's own colours, sampled from its PS2 pause menu and HUD (amber `#F9B035` tabs,
crimson `#AC0000` titles, steel blue `#74AEDA` entries, navy `#01011F`; `launcher/ui/theme.h` kLcs*, docs/LAUNCHER.md).
Home's backdrop is then Liberty City at night instead of the Vice City sunset: the app icon's scene as a panorama (a
suspension bridge across the window, an amber moon, a lit skyline in three layers, their reflections; windows, the
moon's reflection, traffic and the towers' lights move), the same city by day in the light palette; drawn from shapes
in `launcher/screens/backdrop.cpp` (`liberty`), no game art. A game switch crossfades the two scenes.
Checked end to end on Windows (2026-11-18, hidden): disc -> recompile + Quick build in 9 min 40 s -> Play.
Switching back to Vice City Stories keeps all of it; each game has its own disc, build state and saves.
`GTAStoriesLauncher.exe --build --game lcs` / `--play --game lcs` / `--check --game lcs` do the same from a terminal.


## Testing (hidden runs; Windows, Linux, macOS)

`tools/lcs_test.py` (standard library only) starts hidden `lcs` instances, each with its own control port and data
folder (card, `lcs.log`, `lcs.pid`), and drives them; `tools/run.py` / `tools/rig.py` stay VCS's (rig's `player`,
`scm`, `scripts` commands read VCS addresses: wrong on LCS).

```
python tools/lcs_test.py start   --port 49001 --data <scratch>\d1 [--card <card copy>] [--slot 8] [--seconds 3600]
python tools/lcs_test.py newgame --port 49001     # menus -> New Game -> skip the opening cutscene -> gameplay (~40 s)
python tools/lcs_test.py ingame  --port 49001     # after --card/--slot: wait for the loaded game
python tools/lcs_test.py state | info | perf | shot <png> | goto X Y Z | landmark N | save [slot] | load [slot] --port 49001
python tools/lcs_test.py trainer <verb ...> | raw <control command> | quit --port 49001
python tools/lcs_test.py smoke   --port 49001 --data <scratch>\d1   # start, new game, state, perf, screenshot, quit
```

- `--exe` (default `VCS_EXE`, else the lcs build of `VCS_BUILD_DIR` / `build/lcs`), `--iso` (default `LCS_ISO`, else a
  `*Liberty City Stories*.iso` in the repository root), `--env KEY=VALUE` for the game. The game outlives the tool;
  a reaper kills it after `--seconds`. Never use the player's own data folder for `--data`.
- Library: `import lcs_test; g = lcs_test.Lcs.start(port=..., data=..., card=..., slot=...)`, then `g.new_game()` /
  `g.wait_ingame()`, `g.state()` (frame, player, position, health, ped state, vehicle, wanted level, mission flag,
  camera mode), `g.goto()`, `g.trainer("spawn 194")`, `g.hold("W", 2)`, `g.shot()`, `g.perf()`, `g.quit()`.
- Card copies (a folder holding `mc0/`): `--card <folder> --slot N` boots straight into that save (`VCS_LOAD_SLOT`);
  `lcs_test.py savecard --data <data> --to <folder>` keeps a run's card as one. `lcs_test.py vic1` (after `newgame`)
  passes the first mission by teleports (~2 min) to make a free-roam card: then `save 8`, `quit`, `savecard`.
- Defaults of a run: intro movies skipped (`VCS_SKIP_MOVIES=1`; with them, and no key pressed, a boot load takes ~100 s),
  sound off (a hidden run's default; `--env PS2X_AUDIO=1` turns it on), the player's frame-rate settings (60 fps,
  frame-rate independent; `--env VCS_FPS=30 --env VCS_TIMING=0` pins the PS2's).
- Frame counts: CTimer's counter (0x3D9B6C) is not monotonic when read from outside above 30 fps (lcs_framerate.cpp
  swaps its low 6 bits for a 30 fps count while the HUD draws); `frames()` / `state()` use the rig's own count.
- Teleports straight into a building's upper floor (the safehouse) fall through: interiors' collision is loaded by the
  game's own entry; ground-floor shops (Ammu-Nation 1070.8, -397.0, 15.5) work.
- Memory soak (Windows): `python tools/lcs_memsoak.py NAME PORT tele|mix MINUTES --card <card> [VCS_DRAW_DISTANCE=400 ...]`
  loops every landmark (three islands; `mix` also drives a spawned car at each), private bytes / working set every 30 s
  into a CSV, with the least-squares trend of the second half. `tools/lcs_vmmap.py PID` (committed memory by type and
  region size; `ws PID` the working set by type). `PS2X_HW_UPSTATS=1` prints the GS caches' sizes every 5 s
  (`[upload] caches:`). Result 2026-11-18 (quick build, hidden): the process levels off, see "Memory" under Known issues.


## Android

One runtime APK runs either game: the game is the **game library**'s, not the APK's (ANDROID.md, "Games: Vice City
Stories and Liberty City Stories"; the launcher's game switch: ANDROID_LAUNCHER.md "The game switch").

```
python3 tools/recompile.py --game lcs "<your LCS iso>"                 # generated-lcs/ (as above; --out <gen> elsewhere)
python3 tools/android.py build                                         # the APK: the same for both games
python3 tools/android.py build-game --game lcs                         # build/android/game-lcs/libvcs_game.so
                                                                       # (--generated <gen>/ee for another folder)
python3 tools/android.py push-game --game lcs                          # (development) replaces the app's library
```

- The library's stamp names its game (`game=lcs`); the launcher shows "Liberty City Stories" and Play needs the
  picked disc and the library to be the same game.
- LCS's data is `files/userdata-lcs` (memory card, saves, mods; VCS's `files/userdata` is untouched); `vcs.ini` is
  shared. For adb testing, `Android/data/<app>/files/disc-lcs.iso` is LCS's disc copy (`disc.iso` stays VCS's).
- `--generated` defaults to `generated-lcs/ee` for LCS. `build --personal --game lcs --generated <gen>/ee` builds the
  older separate LCS-only app (`build/android-lcs`; never share it); moving its data into the one app: ANDROID.md.
- **Status**: LCS reached gameplay on a phone (2026-11-17) and the maintainer reported it "works well" with the
  separate LCS app. Everything since is built (NDK, ARM64) but not checked on a phone: performance with the renderer
  and the later speed-ups (PERFORMANCE.md 33 / 34), sound, saves, the trainer, the side panel's LCS rows, the pedals /
  LOCK in a car, button prompts on touch, the launcher's per-game Saves list. Boot movies: FFmpeg is in the APK since 2026-11-18 (docs/ANDROID.md "Movies (FFmpeg)"; unchecked on the phone),
  one game library at a time, the launcher's Mods screen writes VCS's `files/userdata/mods` (an LCS `active.txt`
  copied into `files/userdata-lcs/mods` by hand is read), and the overlay colours are VCS's. The gap list and the
  reasoning per feature: ANDROID.md ("What's left for LCS on Android").
- **Phone speed (2026-11-18 droidperf2, A/B with `python tools/android.py env NAME=value`)**: on by default and
  switchable - the native inflate (teleports / loads: `LCS_INFLATE=0`), the audio keeper (`PS2X_AUDIO_KEEPER=0`), the
  governor's slow-frame back-off (`LCS_DRAW_SLOW_MS=0`, or another threshold), zero-weight bones left out of the
  skinning (`VCS_SKIN_SKIP=0`). Still off on Android, to try: `LCS_RSL=1` (the replay now compares sampled bytes
  between whole compares, `LCS_RSL_SAMPLE`; on the PC in the Android configuration it cut the render worker, e.g.
  Atlantic Quays 19.6 -> 13.9 ms, but adds game-thread render time - a wash or better depending on which thread
  limits). The heavy frames after a teleport at 300% traffic are the car spawner's path searches
  (game-internals.md 12e).

## Features: LCS compared with VCS

**works** = does what it does on VCS (checked in runs: hidden runs on the Mac and Windows, as noted); **partial** =
works with the limits given; **VCS only** = not on LCS (rows the game lacks are greyed or hidden). Where the last
column says "same", Windows, macOS and Android behave alike (Android's own gaps are listed in the "Android" section).

| Area | Feature | LCS | Notes | Windows / macOS / Android |
|---|---|---|---|---|
| Branding | Icons | works | GTA Stories Recompiled's own icon (Liberty City's bridge and an amber moon at night, our drawing, no game art) on the launcher, the Android app (adaptive, themed) and the AppImage; lcs.exe the same scene by day (`src/lcs.ico`, picked by `src/vcs.rc` when `VCS_GAME` is lcs); vcs.exe keeps upstream's `src/vcs.ico`. All drawn by `launcher/make_icon.py` (docs/LAUNCHER.md) | same |
| Controls | Keyboard / mouse bindings (F2 > Keyboard), walking speed | works | `vcs_input.cpp` feeds the pad for any game; LCS's driving state from `lcs_input.cpp` | same |
|  | Gamepad (buttons, sticks, deadzones, vibration) | works | game independent; `Vibration` scales all rumble. LCS's road rumble (a shake every frame a vehicle moves, ~19% of the large motor) is scaled by `Driving vibration` (default 40%, 0..200): `lcs_input.cpp` hooks CPad::StartShake 0x16ECC8, see game-internals.md 12 | same |
|  | Mouse wheel on foot (next / previous weapon) | works | Keyboard > Mouse wheel (default on): a notch is L2 / R2 for one frame, as the PS2's weapon cycling (`vcs_input.cpp`); the weapon slot (ped +0x6B8) went 0,1,3,5,.. down and back up in a run; not while scoped (zoom), aiming / locked on, or with the wheel bound to a control. Controls are locked for the first seconds of a new game. Checked in a hidden run on Windows | no wheel on Android |
|  | Raw mouse input (Keyboard > Raw mouse input, default on) | works | game independent: SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE 0 (on) / 1 (off: Windows' pointer speed and acceleration apply); SDL's relative mode itself reads raw counts | desktop only |
|  | Driving vibration (Gamepad > Vibration) | works | road rumble scaled by `pad.driving_vibration` (default 40%, 0..200); crashes untouched. A guess for the default: not felt on a real pad | Android: strength unchecked on a phone (the maintainer found it constant and strong before the setting) |
|  | Pressure-sensitive triggers (`pad.pressure`, default on) and pedals (`pad.pedals`, default off) | partial | LT / RT report their travel as L2 / R2 pressure and LCS enters press mode, but LCS's vehicles are not throttle-proportional (same distance at 30% and 100%), so there is no driving effect. Section "Police cars and pressure" | same |
|  | Mouse camera on foot | works | `lcs_camera.cpp`: turns LCS's follow camera itself (sensitivity, invert) | same |
|  | Mouse camera behind a vehicle (car camera, its sensitivity and return) | works | `lcs_carcam.cpp`; water-cannon hold and boats not checked | same |
|  | Rhino turret by the mouse | works | `lcs_carcam.cpp`: once the mouse has turned the camera behind the tank the turret turns toward where it looks (2.5 rad/s at most; the stick's own ~1 rad/s still adds) and the camera stays put (no return) until the right stick takes it back; the shell lands where the camera looks (run t1, 2026-11-18). `LCS_TANK_AIM=0`: the stick only | same |
|  | Scopes / first-person views by the mouse, wheel zoom | works | `lcs_camera.cpp`, checked in a run (2026-11-18, weapons from the trainer): sniper (28) and laser rifle (29) mode 7, rocket launcher (30) mode 8, camera (36) mode 46: the right button enters, the mouse turns them 1:1 (200 counts = 0.5 rad at 70 deg), the wheel zooms 7 / 46 (70 -> 56 -> 44.8, back out 56; the rocket launcher has no zoom, as on the PS2); 4:3, 16:9 and 21:9: the scope circle round, the bars to the picture's edges. A gun on a vehicle (ATTACH_CHAR_TO_CAR 0x0469 -> mode 45): the mouse turns it, signed, within the attachment's limit (42 / 43 / 44 give 180 deg: a full turn, the camera's update keeps the pitch within 60 deg up, 30 down); it fires with the left button, Ctrl and the gamepad's Circle (runs 2026-11-18: missions 42 / 43 / 44 put Toni on a vehicle's gun at their start, ammo fell under each; the gamepad's left stick turns it, as on the PS2). With Gamepad > Free aim on, as on VCS (`lcs_aim.cpp` mounted): the right stick turns it (2 rad/s at full tilt, on a curve) and the gamepad's R1 fires (Circle still does); the mouse turns it as before (runs 2026-11-18 m42 / m43 / m44: R1 1 s 5-10 rounds, right stick 0.5 s ~1.03 rad, half tilt 0.19, mouse 300 counts 0.75 rad with and without). `LCS_MOUNTED_PAD=0`: the game's way (R1 nothing, the right stick barely). The trainer's `mountgun MODEL WEAPON` streams the gun's model in first, as the missions do | Android: no wheel, so no zoom by touch |
|  | Free aim (right mouse button; Gamepad > Free aim) | works | `lcs_aim.cpp`: LCS has VCS's free aim (R1 + L3) and it is driven the same way. LCS has no script-forced free aim (only ProcessPlayerWeapon 0x34AF20 ever sets ped +0xCBC; no SET_PLAYER_FORCE_FREE_AIMING / drive-by-mode path as VCS's Jive Drive / shooting range): the mission aims are the drive-bys and the guns on vehicles (scopes row), both by the mouse. A one-frame camera snap may show on release | same |
|  | Drive-by aiming with the mouse or the right stick (look left / right with an SMG, fire) | works | `lcs_driveby.cpp`: the mouse / right stick (invert and camera speed as set) turn the look-left / right camera (95 deg either way, 15 down to 25 up), the view follows one to one and the shots go through the middle of the screen, where the game's own aiming circle is drawn (a little over the vehicle's roof); the turned camera goes through the game's wall test (AvoidTheGeometry); on a bike the straight-ahead drive-by takes the camera's heading (60 deg either way); cars and bikes checked in runs. **Toni's gun arm follows the aim** (the drive-by clips hold it square to the side): the game's own arm IK each frame toward the circle, forearm and hand straightened along it, within 2 deg of the aim (bone matrices, PCJ-600 and Stinger, both sides, 80 deg either way, up / down), up to 140 deg either way of ahead (further behind him the arm stays there). `LCS_DRIVEBY_ARM=0`: the clip's arm | same |
|  | Radio wheel (mouse wheel in a vehicle) | works | 11 positions (10 stations + off), L1 steps as on VCS | Android: radio by pad L1 only |
|  | Pause menu: Escape / Enter, left click = Cross, right click = Triangle, wheel = up / down, the pointer | works | `lcs_frontend.cpp`: the pointer picks the tab under it (the D-pad steps there) and the page's entry under it; a click activates it (from the tab bar too). Map: the cursor follows the pointer, drag pans (in the game's limits), the wheel zooms about the pointer, a click is Cross (legend). Stats / Brief: the wheel scrolls the text. Load list (8 slots + cancel) and the yes / no boxes by hover and click. Checked with an injected cursor (hidden runs), the window mapping included | Android: touch gives hover / click / drag; map zoom and Stats / Brief scroll are wheel-only |
|  | Button prompts by device (Enhancements > Prompts) | partial | `lcs_prompts.cpp`: LCS's help text names the control in text for the device (keys, Xbox or PS names); no button pictures (its GTA III font has no glyph table to extend); the PS pad's face buttons stay the font's PS2 symbols; the name is fixed when the help box expands | Android: touch layout unchecked |
|  | Free camera (F6) | works | `lcs_freecam.cpp` (F6; the trainer's Camera page: speed, teleport player here). `LCS_FREECAM=0` leaves the camera unhooked | Android: the side panel's Free camera row (unchecked) |
| Display | Widescreen Hor+ / Vert+ (any aspect, 4:3 to 21:9 and portrait) | works | `lcs_display.cpp`: view window, culling, LCS's own widescreen byte | same |
|  | Sky colour at midday | works | near-white / cyan at noon is LCS's own timecycle and sprites (traced end to end, same with `LCS_RENDER=0`); a bluer sky would be a look change, not done | same |
|  | Fog without dark specks | works | `lcs_sky.cpp`: the sky's fill below the horizon in the fog colour (the PS2's is dark grey) and, in fog only (timecycle far clip under 400 m), no horizon band: the dark specks in the haze and the dark horizon line over the sea in fog are gone; clear-weather horizon unchanged. `LCS_SKY_FILL=0`: the PS2's | same (unchecked) |
|  | HUD: Keep 4:3 / Screen edges / Stretch | works | `lcs_hud.cpp`: native HUD layer on; menus and loading screens kept 4:3; radar and text placed by who draws them; sniper scope bars to the screen edges | same |
|  | Cutscene black bars | works |  | same |
|  | Native renderer | works | `lcs_render.cpp`: VCS's world / lit-model HLE (LCS's VU1 code is VCS's but for skinning), block-keyed textures, compiled VU1, the flip without the CSR fence. Mac: cutscene 10-17 -> cap, gameplay ~20 -> cap. Car paint: the same pixels as the VU1 path (Windows A/B in one frame, `lcsrender lit 0`, four cars at noon and 18:00) | Mac: cutscene 10-17 -> cap, gameplay ~20 -> cap (2026-11-17). Android: speed not measured on a phone |
|  | Frame rate cap 30 - 240, independent timing | works | `lcs_framerate.cpp`: game and cutscene clocks follow real time; CTimer::Update on the host with a 0.2 step floor, so the ceiling is 240 as on VCS (`LCS_HOST_TIMER=0`: the game's Update, ceiling 100) | Android: LSFG frame generation caps the game at refresh / multiplier by design |
|  | Performance | works | a cap of 120 gave ~40 fps (Windows as on the phone): one VU0 program (0x0C00, the sphere clip on the plane sets at +12) ran interpreted, ~45% of the game thread; native now (`vcs_vu0_native.cpp`). And host-paced frames no longer drop to refresh / 2 or / 3 when the render pipeline is slow (`ps2HostTick`). Game thread 11-24 -> 5-10 ms (PERFORMANCE.md 33). Then the render worker / GS stage in Staunton / driving: VIF1 UNPACK fast paths, the GS queue in batches, texture pixels interned by the chain walker ("chain images"), 9-15 -> 5-7 ms; Staunton at a 120 cap 87 -> 112 fps on the loaded PC, unlimited 102 -> 140 (PERFORMANCE.md 34) | Windows numbers (quick / ThinLTO builds, shared PC). Mac: the VU0 fix helped there too. Android: not measured |
|  | Behaviour above 30 fps (the game as at 30) | works | measured at 1/30 and 1/144 time steps in one run (section "High frame rates"): rolling resistance, bike brake wobble, water cannon (`lcs_physics_timing.cpp`); particles and effects (`lcs_particle_timing.cpp`, `lcs_effects_timing.cpp`); radio tune-in (`lcs_music_timing.cpp`); scrolling LED signs and the water's texture drift (`lcs_world_timing.cpp`); the HUD's flashing (wanted stars, timers) on a 30 Hz counter; objects rotated / slid by mission scripts and the per-frame ramps found so far (`lcs_script_timing.cpp`). Bike lean and turning, ped animations: on the step already. Running 5.28 m a game second at 1/30, 1/144, 1/240. Not ported (rotor blades, the push from a body under a wheel, per-event mission adds): Known issues | same |
|  | Show frame rate, performance overlay | works | `lcs_frame.cpp` counts LCS's frames | same |
|  | Resolution, anti-aliasing, texture filtering, vsync, window mode | works | game independent | same |
|  | Colour filter resolution (Graphics > Image quality; default Internal resolution) | LCS only | LCS's colour filter (the trails pass: the frame downsampled into a 512x448 buffer, blended back at FIX 32 and added back tinted at FIX 128, game-internals.md 6p) runs in a buffer as large as the frame target and reads the frame at full resolution (`gsHwSetPostFullResolution`): same tint, the copy as sharp as the picture. Original (PS2) keeps the 512-wide buffer, which made the picture soft and washed above 1x (sharpness of a static wall at 2x: 5.9 -> 7.7, mean colour unchanged; agent crisp) | Android: the side panel's Graphics rows have it; not checked on a phone |
|  | Reduce Post Processing Effects | works | the renderer half (motion-blur history) and, as on VCS, particles and effects back to once a frame (`lcs_particle_timing.cpp` / `lcs_effects_timing.cpp` read the setting) | same |
| Sound | Music, effects, speech, radio, cutscene audio | works | `lcs_sound.cpp`: native driver for LCS's CDSTREAM.IRX, no emulated IOP; checked against the IOP's replies and by recordings | Android: unheard on a phone |
|  | Music / Sound effects volume | works | both ways (setting -> game, the pause menu's Audio page -> setting); LCS's effects default is its own 96 | same |
|  | User tracks (the player's own music as a radio station) | LCS only | Audio > User tracks: `.mp3 .ogg .flac .wav` in `<data>/music` replace one station (default LCFR) or every station; section "Mods" below | Android: not checked on a phone |
|  | Effect sounds replaced from audio files | LCS only | mod directive `sound` (section "Mods") | same; no Android UI yet |
|  | Boot movies, Skip intro movies | works | `lcs_movie.cpp` through VCS's FFmpeg player; Android: FFmpeg built from source into `libmain.so` (2026-11-18, ANDROID.md "Movies (FFmpeg)") | Android: unchecked on the phone |
| Saves | The game's own saves (safehouse, 8 slots, card backups) | works | GTA III-style card code; slots `"N: <mission>"` in `BASLUS-21423GTA30001` | same |
|  | Save game here (trainer / side panel) | works | opens the game's own Save screen; refused during missions, cutscenes, in a vehicle, while moving, inside a safehouse (its own save point is there); also refused with a wanted level ("Lose your wanted level first"), as on VCS | same |
|  | Save pictures: `<data>/save_shots/slot<N>.png` for every save (safehouse, the game's own Save screen, Save game here, F9), none of the menu or HUD notices; a deleted save loses its picture | works | `game/save_shots.cpp` (shared with VCS, desktop too): a free-play shot every 20 s into `latest.png`, one more as a save of ours starts (`fresh.png`, taken before the Save screen opens); `lcs_save.cpp` copies the matching one to `slot<N>.png` when `SaveSlot` finishes and removes it in the `DeleteSlot` hook (0x2D1DB8). The desktop launcher's Saves sheet and Android's Saves list show them. `VCS_SAVE_SHOTS=0` turns them off | same |
|  | Quick save F9 / quick load F10 (slot 8) | works | LCS only (VCS has no quick load) | same |
|  | `VCS_LOAD_SLOT=<1-8>` boot slot; launcher Saves list | works | launcher: Android (unchecked on a phone) and the desktop launcher (Saves > Saved games > Play); import of saves VCS only | same |
| Launcher | Desktop launcher: disc detection, recompile + build, Play, per-game saves and backups | works | Windows (2026-11-18); `GTAStoriesLauncher.exe`, docs/LAUNCHER.md "Two games" | Windows only so far (macOS / Linux builds of the launcher not compiled with LCS) |
|  | A quick save / Save game here loads as a saved game | works | LCS's load keeps only the SCM's 541 listed globals and restarts the scripts; the main script takes the "loaded" path only with global 6 = 1, which only the save points' subroutine (SCM 0x177A3) sets: `lcs_save.cpp` sets it and the spawn (globals 7..9) before its saves, or a free-roam save loads as a new game (the opening mission again), and the rest of what that subroutine stores for missions (ammo per weapon type 0x7C..0x9C, current weapon, wanted level, armour, money 0x9E..0xA1, the four pickup values 0x122..0x125, time of day), so the saved globals match a save point's. Player back where he saved. The load's init script (mission index 5) also sets "inside a safehouse" (global 1116) on every load, which only a safehouse save is (global 211 = the safehouse, -1 elsewhere): after loading a save made outside, the safehouse doors stayed shut for good and the phone-call scripts' condition failed. `lcs_save.cpp` clears global 1116 after a load when no safehouse was saved in (and refuses its saves inside a safehouse: use its save point). Checked: quick save -> quick load keeps every story global; the Portland door and save icon work after it (game-internals.md 11j). Globals 0x1F9 / 0x1FA are one-time help-text flags: nothing to do | same |
| Trainer | Menu (F5 / Back + RB): Player, Weapons (34), Vehicles (79), Teleport (24 districts), World, Camera, Missions, Save | works | `lcs_trainer.cpp` on LCS's own script VM. The wanted-level command works in free roam (the first mission clears it every frame); setting more than the story's current limit raises the limit | Android: side panel rows unchecked |
|  | Overlay colours (F5, F2, fps) | works | LCS's own palette from its pause menu (section "Overlay theme"); Android's panels still use VCS's colours | same |
|  | Camera page (free camera, Hide HUD) and the mission launcher (70 story missions + 6 side jobs) | works | Hide HUD skips CHud::Draw (`lcs_hud.cpp`). Missions start without the story's globals; mission 111 needs $20,000 on the player (the launcher's notice says so; it is the game's design). All 76 start and run (section "Mission smoke test"). Only "Put me back after loading" is hidden (LCS's saves restore the position) | same |
| World | Traffic density | works | `lcs_population.cpp`: random cars on a Portland street 5 at 100% -> 26-28 at 300% (the game's own generator stops at 30). Above 100% the cars are made near the player out of view (35-55 m behind / beside him, the game's 20 m spacing to other vehicles narrowed to 13.5 / 7 m), kept off screen to 109 / 130 m (200 / 300%) and made a few tries a frame (none while the world streams in); their drivers no longer take the street's peds. Cars driving within 50 / 100 m of the player at 300%: Portland 3.6 / 4.7 -> 7.2 / 17.4, Chinatown 7.6 / 11.3 -> 9.4 / 16.8, Staunton 7.7 / 10.2 -> 13.0 / 20.4; driving 4.6 / 7.4 -> 6.4 / 15.0 (the 70-car vehicle pool is the limit there). None seen appearing in view nearer than 80 m, none vanishing; < 0.1 ms of game thread a frame (PC) | same |
|  | Pedestrian density | works | 24 -> 41-53 peds at 300% | same |
|  | Draw distance (100-1000%) | works | `lcs_world.cpp` + `lcs_drawdist.cpp`: the LOD scale, the object scan (300 m x the setting), the level's props (100 m x the setting, LCS's pop-in distance), the far plane; the level's props past its 300-entry list (a second list, up to 1,000 in all, as the frame's DMA buffer allows); backs off by itself when the game has to throw drawn objects out of memory or the render lists fill; cars in view are kept (to the full 1000%). Section "Draw distance" below | same |
|  | Fog and far plane (Original / With draw distance / With draw distance + haze) | LCS only | the timecycle's far clip and fog start times the draw distance (at most 6 km); + haze (default): the far plane moves out but the haze starts where the PS2's does and is complete at ~1.5 x the PS2's view distance, so the far city fades into the timecycle's fog colour instead of showing dark; Original = the PS2's haze and far plane at any draw distance | same |
|  | Model streaming (PS2 pace / Fast) | LCS only | Fast: CStreaming's load step runs again while models wait (up to 24 a frame, 3 ms); PS2 pace = one a frame. (Until 2026-11-18 world2 the loop stopped after one step: a runtime detail, game-internals.md 12e) | same |
|  | In the background: Mute / Pause | works | Pause not checked in a run | same |
|  | Swimming (Enhancements > Gameplay, default off; forced on by `STORIES_DUALWORLD`) | LCS only | `lcs_swim.cpp`: Toni floats at the surface instead of drowning and swims with the on-foot controls; with the mod "Swim animations (from your VCS disc)" (`tools/stories_pack.py swim <VCS iso>`, or the launcher's Mods sheet) he treads water / swims breaststroke / crawl with Vic's own animations, else a stand-in (the game's idle / walk / run, slowed; no in-the-air pose); walks out up a beach, climbs onto a ledge within 2.5 m (a low quay, a deck) with jump, else a small hop; boards a boat with enter-vehicle; no aiming or shooting in the water; slower turning with drift. Section "Swimming" | same (Android unchecked; the Android launcher can't make the pack yet) |
|  | Vice City Stories' vehicles (trainer list, border) | LCS only (experimental) | `tools/stories_pack.py build <VCS iso> --game lcs` packs VCS's 111 vehicles from the player's own disc into mods/stories_vcs_vehicles; `lcs_crossover.cpp` lists them in F5 > Vehicles > Spawn vehicle under a Vice City Stories header and binds one on demand into a pool slot (6 cars, 2 bikes, 2 boats at once), also when it crosses the both-cities border. docs/STORIES.md "Cross-game vehicles" | Windows checked; same code on macOS / Android (unchecked) |
| Settings menu | Drive-by fire rate (greyed: "Vice City Stories only for now") | VCS only | the only locked setting (`lcs_settings.cpp`): LCS's people in cars don't fire once a frame, nothing to pace. Button prompts are ported (`lcs_prompts.cpp`, below) | same |
|  | Internal resolution, MSAA, texture filtering and game timing change while playing (no restart) | works | 2026-11-18, D3D11 and Vulkan (Windows, hidden runs: 1x -> 3x -> MSAA 4x -> aniso 16x -> Match window -> timing both ways, 20 back-and-forth cycles with GPU memory flat). Graphics API and graphics card stay next-start (one device for everything). `VCS_LIVE_GRAPHICS=0` / `VCS_LIVE_TIMING=0`: next start | Android: the same code (side panel rows lose "next start"); frame generation also switches on live with a Lossless.dll added after the start; not checked on a phone |
| Mods | Mod loader (`<data>/mods/active.txt`) | partial | `vcs_mods.cpp` with LCS's addresses (section "Mods" below): whole disc files replaced by name (`replace`: GXT text checked), streaming entries redirected (`stream`: a model swap checked), `weapon.dat` values (`weapon`: checked), `handling.cfg` values (`handling`: top speed and acceleration measured), PC `.dff` / `.txd` models (`model`: a weapon and a car made from scratch drawn in game; converted once, cached). Not on LCS: the Android launcher's Mods screen (writes VCS's folder) | Android: the launcher's Mods screen writes VCS's folder |

## Overlay theme

The overlay UI we draw (F5 trainer panel + notices, F2 settings menu, the fps readout) takes its colours from one table,
`src/ui/theme.cpp` (`ui::theme::palette()`, chosen by `game::isLcs()`). VCS keeps its pink / violet / cyan and ImGui's
default dark settings style; LCS uses colours sampled from its own pause menu (colours only, no art or fonts):
crimson `#AC0000` (title bar, selected tab / rows), amber `#F9B035` (stripes, buttons, fps, check marks), steel blue
`#74AEDA` (text, breadcrumb), navy `#01011F` (panels). Source list: the launcher agent's lcs_palette.md.


## Switches (environment variables)

For comparisons and testing; the defaults are the ones to play with. "Off" = set to `0`. Every `LCS_*` / `VCS_*`
variable `src/game/lcs_*.cpp` reads, plus the shared ones LCS's work added or uses, by area. (The runtime's own
`PS2X_*` switches are defined in `third_party/PS2Recomp/ps2xRuntime`.)

**Rendering and the GS / VU path**

| Variable | Effect on LCS |
|---|---|
| `LCS_SWIM=0/1` | the Swimming setting for one run (0: Toni drowns as in the original; 1: he swims). `LCS_SWIM_HOOK=0`: not hooked at all (even with `STORIES_DUALWORLD`); `LCS_SWIM_LOG=1`: once a second where he swims, the water level, speed, state, the animation playing; ledge probes, hops, climbs and exits; `LCS_SWIM_ANIMS=0`: the swim animations mod not used this run (the stand-in); `LCS_SWIM_TEST_MOVE=1/2/4/5`: tests - that move state while swimming (the free camera leaves the pad alone) |
| `LCS_POST_RES=0` | the colour filter (trails pass) in its PS2-size 512x448 buffer, as before 2026-11-18 (`=1` the frame's resolution, the default; the setting Colour filter resolution) |
| `VCS_LIVE_GRAPHICS=0` | Internal resolution, Anti-aliasing (MSAA) and Texture filtering apply at the next start again, as before 2026-11-18 (default: live, from the next frame; `gsHwSetInternalScale` / `Msaa` / `Anisotropy`) |
| `VCS_LIVE_TIMING=0` | Game timing applies at the next start again (default: live; each switch continues from CTimer's last count, `lcs_framerate.cpp noteTimingMode`) |
| `VCS_LSFG_FEATURES=0` | Android: frame generation's Vulkan features asked for only with a Lossless.dll at the start (default: whenever the GPU has them, so a DLL added while playing works without a restart) |
| `LCS_RENDER=0` | none of LCS's renderer paths (block keys, lit models native, CSR without fence) |
| `LCS_HLE_LIT=0` | lit models (peds, vehicles, props) on the VU1 instead of the native HLE |
| control `lcsrender lit 0` / `1` | tests: the same switch while the game runs (A/B of one frame under the free camera) |
| `LCS_SKY_FILL=0` | the PS2's dark grey sky fill below the horizon and its horizon band in fog (the dark specks in fog) |
| `STORIES_DUALWORLD=front\|back` | Stories: both cities (experimental, docs/STORIES.md; `tools/stories.py` sets it): Vice City drawn on the horizon from a second, hidden VCS process |
| `LCS_CSR_FENCE=1` | the flip's GS CSR read waits for the render worker again (any value turns it on) |
| `VCS_VU0_SPHERE12=0` | VU0 0x0C00 (sphere vs three plane sets at +12) on the interpreter again (~45% of the game thread) |
| `PS2X_VU0_VERIFY=1` | check the VU0 natives against the interpreter |
| `PS2X_TRACE_VU0=1` | VU0 microprogram calls and host time per start address (an interpreted one shows as ms per call) |
| `PS2X_VU0_CODE_LOG=1` | debug: which bytes of VU0 micro memory changed between program calls, every VIF0 MPG |
| `PS2X_HW_BLOCK_KEYS=0` / `1` | force the runtime's block-level texture keys off / on |
| `PS2X_CHAIN_IMAGES=0` / `verify` | texture pixel REFs copied through the chain, the worker and the GS queue again (PERFORMANCE.md 34) / interned, each reuse compared with RAM (`PS2X_HW_UPSTATS=1` reports) |
| `PS2X_VIF_UNPACK_FAST=0` / `1` / `verify`, `PS2X_VIF_UNPACK_SIMD=0` | VIF1 UNPACKs: the general loop / the older unmasked fast path / the typed and blend paths checked against the general loop; the typed path without the SSE2 blend |
| `PS2X_GS_BATCH=1` | the GS stage's records published one at a time (seq_cst, as before 2026-11-18) |
| `LCS_RSL=0` / `1` | model / building packets through VIF1 as before / replayed even without the write watch (Android: off by default) |
| `LCS_RSL_SKIP=0`, `LCS_RSL_MODELS=0`, `LCS_RSL_CHAIN_IMAGES=0`, `LCS_RSL_RASTERS=1`, `LCS_RSL_HASH=1` | the replay's parts: unchanged packets copied anyway / no packet marked / texture uploads copied through the chain as VCS's / textures named by raster (VCS step 3d) / recordings checked by hash instead of their bytes (no write watch) |
| `LCS_RSL_SAMPLE=<n>` | no write watch (Android, `PS2X_WRITE_WATCH=0`): a recording is compared with its block byte for byte every n-th hit (default 16), between those only its first and last 128 bytes and a qword every 1/8 (packets in the frame's DMA buffer always whole); `0` / `1`: every byte every hit, as before. The memcmp was ~8% of the render worker with `LCS_RSL=1`. A model streamed in at the same address with the same length and samples would draw its old mesh for up to n-1 frames |
| `LCS_INFLATE=0` / `verify` | the game's zlib inflate_fast (FUN_0022BE20) recompiled again instead of native (`lcs_inflate.cpp`) / native on a scratch copy and the guest's on RAM, compared (`[lcs-inflate]` every 5 s) |
| `VCS_SKIN_SKIP=0` | native skinning computes all four bones for every vertex again (default: a zero-weight bone is left out, bit-identical - `VCS_SKIN_VERIFY=1` compares with the scalar reference and counts the vertices; shared with VCS) |
| `PS2X_AUDIO_KEEPER=0` | no audio keeper: the native sound driver is clocked by the EE thread alone (a host stall of the game thread over the queue's 60 ms then empties it). `PS2X_AUDIO_STATS=1` also counts the stalls the keeper covered |
| `PS2X_CHAIN_ASYNC=0` / `verify`, `PS2X_CHAIN_ASYNC_LOG=1` | the VIF1 chain walked on the game thread at each kick (as before 2026-11-18; with `LCS_RSL` on - the PC's default - it always is) / walked at the kick as well and compared with the asynchronous walk / walk times, where the game waited (PERFORMANCE.md 37) |
| `LCS_RSL_NATIVE=0` | LCS's state flush FUN_002A02F0 recompiled again instead of native (`lcs_rsl.cpp` -> `vcs_rsl_native.cpp`'s `installOther`; `VCS_RSL_NATIVE_VERIFY=1` compares the two, `VCS_RSL_NATIVE_LOG=1` counts, `VCS_RSL_PLUGIN_LOG=1` names plug-in callbacks that still run the original) |
| `LCS_RSL_ASYNC=0` | the RSL replay's observed chain walked on the game thread at the kick again (since 2026-11-18 it runs on the asynchronous walk's thread too: `ps2chain::setAsyncWithObserver`, the frame kick waits for the last walk before the draw list changes hands) |
| `LCS_DMA_SYNC=0` | the model heap's frees and moves (`lcs_dma_sync.cpp`) no longer wait for the asynchronous chain walk (only to show what they guard: `PS2X_CHAIN_ASYNC=verify` then reports differences on teleports) |
| `VCS_RSL_WORLD_VERIFY=1`, `VCS_RSL_WORLD_LOG=1` | every replay checked against the packet path / the replay's counts every 10 s (also for LCS) |
| `PS2X_CHAIN_BOUNDARIES=1` | the chain copy parses VIF command boundaries and folds the write watch even with no observer |
| `PS2X_CHAIN_IMAGE_MEMO=0` | chain images: each reuse checked again (without write watch: hashed) even when the same kick already found it current, and looked up in the map alone (PERFORMANCE.md 35) |
| `PS2X_HLE_MESH_HASH=0` | native draws' mesh keys by the old serial FNV chain instead of six lanes (PERFORMANCE.md 35; shared with VCS) |
| `PS2X_HLE_CODE_MEMO=0` | the native handlers' VU1 code checks hash the code again at every code change instead of answering a byte-identical image from memory (PERFORMANCE.md 35; shared with VCS) |
| `PS2X_HW_FLAT_MAPS=0` | the GS stage's mesh LRU and two upload maps as unordered_maps again (PERFORMANCE.md 35) |
| `PS2X_GS_HOLD=<n>` | GS records per run the GS stage keeps the state lock across (default 32); `0` / `1`: taken per record, as before (PERFORMANCE.md 35) |
| `PS2X_HOST_TICK_BACKLOG=1` / `2` | frames on their way allowed at a host tick: 1 = before 2026-11-18 (refresh / n quantisation with a slow render pipeline), 2 = always; default: 2 while a frame takes over two host frames to build |
| `PS2X_TICK_LOG=1` | every 5 s: host ticks released / dropped, how often and how long the game waited for one |
| `VCS_HUD_NATIVE=0` | the HUD squeezed into the game's frame buffer instead of its own layer |
| `VCS_HUD_RW=0` | none of `lcs_hud.cpp` |
| `VCS_MENU_NATIVE=0`, `VCS_LOADING_NATIVE=0` | menus / loading screens not marked as menus |
| `VCS_SCOPE_FILL=0` | the sniper scope's side bars stay where the game draws them |
| `VCS_WIDE_CULL=0`, `VCS_WIDESCREEN=0` / `1` / `game` | LCS's own culling; force the widescreen preference (testing) |

**Frame rate and timing**

| Variable | Effect on LCS |
|---|---|
| `VCS_FRAMERATE_HOOKS=0` | the game's own limiter (30 fps) |
| `LCS_HOST_TIMER=0`, `LCS_MIN_STEP=<x>` | the game's own CTimer::Update (0.5 step floor, at most 100 fps); another floor for the host Update (experiments) |
| `VCS_EVEN_PACE=0` / `1` | Display > Even frame pacing off / on (default on): a cap the frames don't fit is paced at a whole fraction of the refresh they do (Smoothness below). `VCS_EVEN_PACE_REFRESH=<Hz>`: the levels for that refresh (hidden test windows report 60) |
| `VCS_PRIORITY=1` / `0` / `-1` | the process's CPU priority: above normal (default for the played game), normal, below normal (default for Stories' background city; dualworld swaps it with the front) |
| `VCS_PACE_LOG=<file.csv>` | smoothness log: every present (P), game frames each host frame shows (T), each frame's CTimer step in ms (S) and end (F), `kind,t_us,value` on the steady clock |
| `VCS_TIMESTEP=<N>` | exactly 1/N s a frame, unpaced (timing tests, video capture); in such a run the control command `lcstimestep <N>` changes N (the same scene at two steps: `tools/lcs_timing_audit.py`) |
| `VCS_PHYSICS_TIMESTEP=0` | rolling resistance, the bike's brake wobble and the water cannon per frame, as the PS2 (cars brake twice as hard at 60) |
| `VCS_PARTICLE_TIMING=0`, `VCS_EFFECTS_TIMING=0` | particles' fades / emitters, and lightning, clouds, shake, explosions ... once a frame (`VCS_PARTICLE_LOG=1`, `VCS_EFFECTS_LOG=1`: what took effect) |
| `VCS_MUSIC_TIMESTEP=0` | the radio's off-air progress counted per frame as the game does |
| `LCS_WORLD_TIMING=0`, `LCS_SCROLLBAR_TIMING=0`, `LCS_WATER_TIMING=0` | the scrolling LED signs and / or the water's texture drift once a frame (`LCS_WORLD_TIMING_LOG=1`: their rates every 10 s) |
| `LCS_SCRIPT_TIMING=0` | mission scripts' object rotate / slide steps and the listed per-frame ramps once a frame; `LCS_SCRIPT_TIMING_LOG=1`: every 20 game s, the script adds that ran on most frames (the survey) |
| `LCS_HUD_BLINK_LOG=1` | how often the HUD's flash bits flip, per real and per game second |

**World, draw distance, density**

| Variable | Effect on LCS |
|---|---|
| `LCS_DRAWDIST=0` | none of `lcs_drawdist.cpp`: the draw distance is the LOD scale alone (as before 2026-11-18): the PS2's 300 m scan, 100 m props, far plane and one-model-a-frame streaming |
| `LCS_DRAW_GUARD=0` | no back-off: the draw distance as set, whatever memory and the render lists say (testing; the 2,000-entry list guard stays) |
| `LCS_DRAW_SLOW_MS=<ms>` | the governor also backs off while frames are slow: the game thread's busy time (game + render + IOP, `ps2perf::lastFrame`) averaged over ~8 frames above <ms> (default 40), or the whole frame above 1.25 x that (worker / GS / GPU bound); `0`: frame time not watched (before 2026-11-18). `lcsdraw` reports `slow=` (frames backed off for it) and `busyavg=` |
| `LCS_PATH_STATS=1` | diagnostic: FUN_00147950 (the path search, VC's CPathFind::DoPathSearch shape) calls by caller every 5 s (`[lcs-path]`; the host time only covers calls that ran to their end without an EE checkpoint) |
| `LCS_FAR_FOG=0` / `1` / `2`, `LCS_STREAM_FAST=0` / `1` | force Fog and far plane (Original / With draw distance / + haze), Model streaming (PS2 pace / Fast) |
| `LCS_HAZE_K=0.15` | + haze: where the haze is complete, timecycle far x (1 + (far scale - 1) x K) (0..1; tuning) |
| `LCS_LEVEL_LIST=0`, `LCS_LEVEL_MAX=N` | no second level-prop list (the PS2's 300 only, far ones dropped first) / both lists' total (300..1000, default 1000) |
| `VCS_WORLD_DETAIL_HOOKS=0` | density and draw distance left alone |
| `VCS_WORLD_KEEP=0` | no pool reserve, cars in view not kept, no wider off-screen distance |
| `VCS_LCS_TRAFFIC_RING=0` | Traffic density without the wider off-screen distance (the PS2's 60 m at every setting: 5 cars at 300% where it is 28 otherwise) |
| `VCS_LCS_TRAFFIC_NEAR=0`, `VCS_LCS_TRAFFIC_PACE=0` | Traffic above 100% as before the near spawning (world2's: no near spots / spacing / driver share, 60 m x the setting off screen) / world2's pace (100 tries every 1 s x 100 / (setting - 100)) |
| `VCS_LCS_KEEP300=m`, `VCS_LCS_NEAR_DRIVING=f`, `VCS_LCS_SPAWN_BUDGET_US=us` | off-screen keep distance at 300% (default 130) / share of near spots while driving (0.25) / spawner time a frame for the extra tries (250) |
| `VCS_LCS_POP_TRACE=1` | spawner statistics (`lcspopstages` on the control channel) |

**Input, camera, aiming**

| Variable | Effect on LCS |
|---|---|
| `LCS_CARCAM=0` | the mouse moves the right stick behind a vehicle instead of the camera |
| `LCS_FREECAM=0` | the free camera is not installed (the trainer's Camera page does nothing) |
| `VCS_FREEAIM=0` | no mouse free aim |
| `LCS_SHOT_LOG=1` | one `[lcs-shot]` line per player shot (stderr / logcat tag vcs; at most 40 a second): which of FireInstantHit's four line tests made it (`target` = toward the target, the arm IK's gun-pointed bit +0x33C set; `hand-bone` = a target but that bit clear, so along the right hand bone's X axis; `special`; `no-target` = along his matrix forward), the shot's heading / pitch, his matrix heading, m_fRotationCur / Dest, the hand bone's heading / pitch, the gun's offset from him, the target, the IK flags, +0x190 bits 39 / 41, the camera mode and Front heading (degrees, 0 = +x). `free-aim` lines: the line after `lcs_aim`'s camera redirect. Diagnostics for report B4 (shots / muzzle flash going another way than Toni faces), see Known issues |
| `LCS_SHOT_TO_TARGET=1` | A/B only: a `hand-bone` shot (target, IK not pointed) is sent at the target instead (no spread) |
| `LCS_MOUNTED_PAD=0` | a gun on a vehicle (mode 45) left to the game with Gamepad > Free aim on: the right stick does not turn it, R1 does not fire it |
| `LCS_DRIVEBY_AIM=0`, `LCS_DRIVEBY_BIKE_FORWARD=0` | the game's own drive-by; a bike's straight-ahead drive-by left alone |
| `LCS_DRIVEBY_ARM=0` | Toni's arm keeps the drive-by clip's pose (square to the side) instead of following the aim |
| `LCS_DRIVEBY_CROSSHAIR=0`, `LCS_DRIVEBY_LIFT=<m>` | no aiming circle in the drive-by look; how far over the game's look point the view looks (0.7 m; 0 = through it, the game's framing) |
| `LCS_TANK_AIM=0` | the Rhino's turret by the right stick only (the mouse turns the camera, not the turret) |
| `LCS_MENU_MOUSE=0` | no pointer in the pause menu (buttons and wheel only; the wheel is the D-pad everywhere) |
| `VCS_PROMPTS_HOOKS=0` | no button-prompt text (the game's PS2 symbols) |
| `VCS_RUMBLE_LOG=1` | log every CPad::StartShake (caller, time, frequency) and every change of the motor levels the game sets |

**Sound, movies, saves, mods**

| Variable | Effect on LCS |
|---|---|
| `VCS_NATIVE_SOUND=0` | sound through the emulated IOP instead of the native driver |
| `PS2X_IOP=1` | run the emulated IOP (LCS's IRX modules) as the reference |
| `VCS_GAME_VOLUME=0` | leave LCS's music / effects levels alone |
| `VCS_STREAM_SYNC_HOOKS=0` | CdStream sync / last position through the IOP (the old, freeze-prone path) |
| `VCS_MUSIC_LOG=1`, `VCS_MUSIC_STOP_WAIT=1` | log the music commands; stop the music at the IOP's IRQ as the emulated IOP does (compared with it) |
| `VCS_SOUND_REPLAY=<record>`, `VCS_SOUND_REPLAY_OUT=<wav>` | feed recorded RPCs to the native driver and write its output (`tools/soundcheck.py`) |
| `PS2X_AUDIO_DUMP=<file.wav>` | everything the audio output played, written as a 48 kHz stereo WAV (both games; a hidden run needs `PS2X_AUDIO=1`) |
| `VCS_USER_TRACKS=<n>` | the Audio > User tracks setting: 0 off, 1-10 instead of station 0-9 (Head Radio ... LCFR), 11 every station |
| `VCS_USER_TRACKS_SHUFFLE=0` | user tracks in name order instead of shuffled |
| `VCS_SFX_LOG=1` | log each effect voice keyed on: `[sfx] key on: sound <id> (bank <b>, index <i>), rate <Hz>` (to find a sound's id for the `sound` directive) |
| `VCS_SKIP_MOVIES=1` | skip the intro movies (the setting's environment form; test runs set it) |
| `LCS_SAVE=0` | no save here / quick save / quick load |
| `VCS_SAVE_SHOTS=0` | no save pictures (both games) |
| `VCS_LOAD_SLOT=<1-8>` | the slot the boot loads |
| `VCS_MODS=0` | the mod loader applies nothing (stock game, `mods/active.txt` kept) |
| `VCS_MODS_DUMP=1` | the mod loader logs the raw tables its LCS field maps come from (weapon records, vehicle model infos, streaming entries, the disc index) |
| `VCS_MODS_CACHE=0` | `model` lines are converted again at this start (the cache in `mods/<mod>/converted/` is rewritten, not read) |

**Folders and test runs**

| Variable | Effect on LCS |
|---|---|
| `VCS_DATA_DIR=<folder>` | the data folder (memory card, caches, `mods/`; default `.../GTAStoriesRecomp/LCS`) |
| `VCS_KEEP_SETTINGS=1` | a test run leaves the player's `vcs.ini` unchanged (hidden runs always do) |
| `VCS_KEYS_HINT=1` / `0` | the first-run "Press F2 for settings..." notice forced (also in a hidden run, also after `hints.keys=1`) / never shown; unset: once, never in hidden runs |
| `PS2X_HIDDEN=1`, `PS2X_HIDDEN_WINDOW=WxH`, `PS2X_CAPTURE_SIZE=WxH` | no visible window; the hidden window's size (Hor+ follows it); screenshot / burst size |
| `PS2X_CONTROL_PORT=<n>` | the control channel's port (`tools/gamectl.py`, `lcs_test.py`) |
| `PS2X_AUDIO=1` | sound on in a hidden run (off by default there) |

**Debugging**

| Variable | Effect on LCS |
|---|---|
| `LCS_UAF_LOG=1` | `lcs_uaf_debug.cpp`: the last 256k heap frees kept with their callers; an element drawn with a broken geometry is logged with the frees that covered it (`LCS_UAF_SKIP=1`: such an element is not drawn) |
| `VCS_WATCH=<lo>,<hi>`, `VCS_RWATCH`, `VCS_WATCH_STACK=1` | write / read watch armed at start (debug/watch.cpp; the rig's `watch` command works for both games). Every store takes the checked path while armed: timing changes |

## Mission smoke test (Windows)

`python tools/lcs_missions.py [--indices 96-117,...] [--secs 100] [--jobs 4]` starts one fresh hidden game per mission
(new game, opening skipped, VIC1's script ended through its mission cleanup = free roam), launches the trainer's mission N, watches 100 s (frame counter,
mission flag, camera, fps, log for `dormant`, screenshots) and writes a table. Run on a055a76 (Windows, 4 in parallel):
all 76 launcher entries (70 story 9, 48-61, 63-117 + 6 side jobs) start and run; no process crash; fps 21-60 (machine shared).

| index | name | result | fps | notes |
|---|---|---|---|---|
| 9-10, 12, 21-22, 48-50, 52-54, 56-59, 61, 63-66, 68-74, 76-82, 84-90, 92-105, 107-110, 112-117 | (all other entries) | OK | 21-60 | 111 aside |
| 11, 14, 51, 67, 75, 83 | Paramedic, Firefighter, Smash and Grab, Salvatore's Salvation, No Son of Mine, Caught In The Act | ENDED | 28-40 | flag 1 -> 0 after 31-88 s (pass / fail: the driver does not play) |
| 111 | No Money, Mo' Problems | ENDED (expected) | 34 | the game's design: after the 8-Ball cutscene the mission wants $20,000 on the player (DONH4 +0x2D9); without them it ends at once (8-ball won't budge unless you pay him $20000), with them it takes them and passes. The launcher's notice says so (`trainer money 20000` first). game-internals.md 11j |
| 91 | Munitions Dump | ~~DORMANT~~ OK | 60 | was 3 of 4 runs: `[guest-branch:missing-target]` JALR at 0x18ACEC, VU0 / VU1 "reserved lower" floods. **Fixed in the test** (below) |
| 60, 106, 55 | The Trouble with Triads, The Shoreside Redemption, Ho Selecta! | ~~FREEZE~~ OK | - | was 3-4 of 7 runs: the EE spinning at pc 0x2E87F0. **Fixed in the test** (below) |

**91 / 60 / 106 / 55 were the test's own doing, not the port's** (game-internals.md 11i): the free-roam shortcut ended VIC1 with
a bare TERMINATE_THIS_SCRIPT, which skips the mission cleanup, so Vincenzo (special character slot 0 = model 109, in his Kuruma
next to Toni) stayed a mission character. A mission loading another special character into slot 0 then freed model 109 under him
(the game removes only deletable peds first), and drawing him read freed memory: a material count from reused memory (the
endless loop at 0x2E87F0) or alpha bytes written through a freed material list (0xFF at random addresses: VU0 micro memory,
the world's sector lists). The PS2 would do the same from that state; normal play ends VIC1 through its cleanup. The test now
writes MISSION_HAS_FINISHED (LCS 0x00DD) + TERMINATE. Same build, 4 in parallel, 60 s, 4 runs each: with the bare TERMINATE 11 of 16 failed (91 1/4 DORMANT, 60 3/4,
106 4/4, 55 3/4 FREEZE); with the cleanup 0 of 16, no `reserved lower` / `dormant` line in any log.

Missions start without the story's globals (no post-VIC1 save needed; 111 needs only $20,000); `--card DIR --slot N` boots a
save instead.


## Draw distance (Enhancements > World, 2026-11-18)

What ends the view in LCS, and what each setting step changes (addresses: [game-internals.md](game-internals.md) 12e):

| Limit | PS2 (100%) | At Draw distance N% |
|---|---|---|
| Far plane / fog (timecycle; noon sunny) | 1,168 m / fog from 14 m | x N/100 up to 6 km, with "Fog and far plane: With draw distance" (Original: unchanged); "+ haze": far plane the same, haze from 14 m complete at 1,690 m (400%) / 1,890 m (1000%) |
| Objects with LOD distances (buildings' props, trees as entities) | LOD distance x LODDistMultiplier, scanned to 300 m | LOD distance x N/100, scanned to 300 m x N/100 (never past the far plane) |
| The level's props (lamp posts, signs, bins, roadside trees): **where they pop in** | 100 m, 300 a frame | 100 m x N/100; past the 300 a second list (up to 1,000 in all, props within 500 m; ~100 extra a frame seen at 1000%), filled only while the frame's DMA buffer keeps what the rest of the frame writes + 128 KB; past 100 m only while 100 entries stay free for the near ones |
| The world's own geometry (cells) | the current cell's lists, already the whole view | unchanged (drawn to the far plane) |
| Model streaming | one model a frame | Fast: up to 24 a frame / 3 ms while models wait (the queue was empty in nearly every frame of the drives: not what objects wait for) |
| Memory | heap nearly full (6-900 KB free) | unchanged; the scale backs off by itself when MakeSpaceFor has to delete drawn objects (never happened in the runs) |
| Render lists (2,000 entities each) | 60-210 used | up to 1,680 seen at 1000%; the scale backs off above 1,700, and nothing is added past 1,900 |

Measured on Windows (quick build, not optimised; the machine shared with other test runs, so frame rates are noisy -
compare within a row). Hidden runs, the free camera at fixed spots, noon, sunny, 4:3; `lcsdraw` counters:

| Spot | Draw | game fps | draws | entities visible | level props drawn past 100 m (a frame) | render worker ms |
|---|---|---|---|---|---|---|
| Portland, Red Light street | 100% | 35.8 | 2,989 | 161 | 0 | 15.1 |
| | 400% | 20.4 | 4,697 | 720 | ~100 | 27.2 |
| | 1000% | 20.7 | 4,858 | 755 | ~100-200 (list cap) | 28.8 |
| Portland, over the bay (to Staunton) | 100% | 32.2 | 1,717 | 81 | 0 | 8.9 |
| | 400% | 31.4 | 2,798 | 935 | | 18.7 |
| | 1000% | 36.3 | 4,158 | 1,634 | | 15.2 |
| Staunton shore (to Portland) | 100% | 58.0 | 339 | 63 | 0 | 1.8 |
| | 400% | 47.5 | 1,383 | 757 | | 6.2 |
| | 1000% | 48.9 | 2,033 | 748 | | 9.1 |
| Staunton, Torrington towers | 100% | 41.6 | 1,664 | 107 | 0 | 8.6 |
| | 400% | 36.8 | 2,341 | 641 | | 11.6 |
| | 1000% | 32.0 | 3,013 | 1,276 | | 14.8 |

With the second prop list and the haze (world2, 2026-11-18; quick build, hidden, noon, 60 fps cap, the machine shared
with other agents' runs; "off" = `LCS_LEVEL_LIST=0`; memory = the game's model heap free / the process's working set):

| View | Draw | fps (list / off) | draws (list / off) | extra props a frame | heap free | working set |
|---|---|---|---|---|---|---|
| Portland over the bay | 100% | 52 / 47 (noise) | 1,689 / 1,722 | 0 | 263 KB | 349 MB |
| | 400% | 58.7 / 58.9 | 2,790 / 2,789 | 0 (the 300 suffice) | 378 KB | 352 MB |
| | 1000% | 46.8 / 49.7 | 5,559 / 4,238 | up to 106 | 146 KB | 369 MB |
| Portland long street | 100% / 400% / 1000% | 60 / 59.7 / 52.5 (off 60 / 58 / 55) | 2,328 / 3,729 / 4,608 (off 3,664 at 1000%) | 0 / 0 / some | 179-315 KB | 398-429 MB |
| Staunton street | 100% / 400% / 1000% | 60 / 28.7 / 40.4 (off 60 / 24.9 / 42.0) | 2,710 / 3,975 / 5,840 | | 12-420 KB | 465-476 MB |

The second list costs ~3 fps at 1000% (limited to props within 500 m: past that they are a few pixels - without the
limit it was 8-11 fps for a 12-134 pixel difference in the screenshots). The frame's DMA buffer kept >= 130 KB free at
its send in these runs (`lcsdraw` dmamin); the guard (`noroom`) only acted at 1000% on the Staunton shore. A freeze
after teleports at 1000% (extras drawn after their DMA buffer was reused, in the frames that draw the level without a
new scan) was fixed the same day: extras only live until their buffer's send, none for 2 s after a camera jump (> 50 m in a
frame), the scale starts at half (at most 4x) after such a jump and creeps back from 2 s on, and it backs off when a
frame leaves the buffer nearly full. The coordinator's teleport script (14 stops, 1000%) now passes with RSL on and
off; the buffer kept >= 91 KB free (before: 5 KB without the second list, 133-166 KB past its end with it).
`lcsdraw hist` lists the last 64 sends. Screenshots:
the haze shots (haze 0.35 and "With draw distance").

Pop-in, the free camera flown at 30 m/s (108 km/h) along fixed routes (Portland 2.1 km, Staunton 2.2 km):

| | 100% PS2 pace | 100% Fast streaming | 400% | 1000% |
|---|---|---|---|---|
| Level props appear at | 100 m | 100 m | 400 m | 1,000 m (as the list allows) |
| Props in view past their draw radius (still to pop in), a frame, Portland | 89 | 87 | 208 (400-1,200 m) | 43 |
| Level props drawn past 100 m, a frame, Portland / Staunton | 0 / 0 | 0 / 0 | 97 / 66 | 138 / 180 |
| Objects in range waiting for their model (frames x objects) | 0 | 0 | 0 | 0 |
| Streaming steps a frame (Fast: extra loads) | 1.000 | 1.004 | 1.006 | 1.005 |
| Memory pressure (drawn objects deleted for room) | 0 | 0 | 0 | 0 |

So the pop-in is the PS2 game's own 100 m radius for the level's props, not streaming: models were never waited for,
and the host's instant reads keep the one-model-a-frame queue empty. At 100% the props still appear at 100 m, as on the
PS2; raise Draw distance to move that line out (400% puts it at 400 m, past what is seen through most streets).

Soak: 1000%, the camera flown round all three islands (5.5 km a lap, the player carried along every 150 m) for
4 laps / 12 minutes of flying (22 km, Shoreside's level loaded and drawn on the way): no crash, no freeze, no memory
pressure, the render lists at most 1,680 of 2,000. Fog weather (far plane 116 m) and midnight (859 m) checked at
100% / 400%: the far plane and fog scale the same way.


## High frame rates

On the PS2 LCS ran at 30 fps, and much of it moves a fixed amount each frame instead of by CTimer's step: above 30 fps
those things ran fast. Each fix below acts only while the frame is not the PS2's (independent timing, or original
timing with a cap of 60) and has a switch (Switches above).

**How it was surveyed** (2026-11-18, the timing agent): the frame counter's 121 readers (0x3D9B6C and gp+0x2C7C)
read one by one; every float field the code adds a constant to and stores back in a function that never reads the
step (`lwc1 / add.s / swc1`); RAM compared at two time steps in one run (`VCS_TIMESTEP=30`, then `lcstimestep 144`:
the same scene, every global and heap word whose rate per game second follows the frame rate); MAIN.SCM's 70 float
adds / subtracts of a constant (missions), each read in context; and a runtime survey of the script VM
(`LCS_SCRIPT_TIMING_LOG=1`: the adds that run on most frames while a mission plays). Things measured with
`tools/lcs_timing_audit.py` (one hidden run, the same scene at 1/30 s and 1/144 s steps; numbers per game second):

| What | 1/30 | 1/144 before | 1/144 now | Fix |
|---|---|---|---|---|
| Scrolling LED sign (Portland, bar 11): letters a second (PS2 3.75) | 3.78 | 17.97 | 3.73 | `lcs_world_timing.cpp` 1: CScrollBar::Update 0x101E00 once per PS2 frame of game time |
| Water texture drift (gp-0x1998) a second | 0.0220 | 0.1037 | 0.0218 | `lcs_world_timing.cpp` 2: the render 0x20DE28's per-frame add scaled by the step |
| HUD flash bits (wanted stars, timers, the radar): bit 8 / bit 4 flips a second (PS2 3.75 / 7.5) | 3.87 / 7.55 | - | 4.26 / 7.46 | `lcs_framerate.cpp`: Render2dStuff sees a 30 Hz counter |
| Bike (PCJ-600, 31 m/s) braking hard 3 s: heading swing | -3.8..0 deg | -27..54 deg | -1.9..0 deg | `lcs_physics_timing.cpp`: the brake wobble's push (ApplyTurnForce ra 0x15BF0C) scaled |
| The same: lean (CBike +0x5AC) during the braking | -0.7..9.1 deg | -34.5..32.3 deg | -1.1..5.4 deg | (the wobble's) |
| Bike turning (A at 31 m/s): lean / heading 0.3 s after the key | -23 / 7 deg | - | -25 / 6 deg | none needed: lean smoothing is pow(k, step) (0x15BC70), the target from the acceleration over the step |
| Player running (airport apron): metres a game second | 5.28 | 0.47-0.55 at 1/240 without the ped patch (fps2, Mac) | 5.28 at 1/144 and 1/240 | `lcs_framerate.cpp`'s host CTimer::Update (0.2 floor) and the `lcs.toml` patches |

Earlier measurements (the Mac runs of 2026-11-17): particles' lifetimes and spawn rates (exhaust 267 ms at 30,
271-282 at 144 with the fix, 58-82 before; rain 1080 / 600 / 60 spawns a second at both), explosions 4100 ms (1917 at
144 before), clouds 900 a second sunny (2667 before), coasting deceleration 4.17 / 4.25 m/s^2 (twice as hard at 60
before), running 5.28 m/s at 1/30 .. 1/240 (0.47 at 240 without the ped patch).

**Found and fixed here**
- *Scrolling LED signs* (GTA III's CScrollBar, 12 x 0x80 at 0x3DABE0, the stock / news tickers): CMovingThings::Update
  0x1013D0 calls Update for a visible bar every frame (and a hidden one on `(frame + i) & 7 == 0`); Update counts +0
  and moves the text on a letter every 8th call (+0x24 / length +0x28). Visibility (+0x68) and brightness (+0x38) come
  from the camera's distance in Update itself (100 / 75 m).
- *Water*: the water render 0x20DE28 (from 0x112418) adds wind (0x3D9E2C) x 0.0005 + 0.0006 to four floats gp-0x1998 /
  -0x1994 / -0x1990 (minus, with sin(t) x 0.0003) / -0x198C (with cos(0.7 t) x 0.0003), t = (game ms & 0xFFF) x
  2 pi / 4096, wrapped at 1 / 0: the texture's drift. Corrected at the next call (the renderer reads them right after
  the adds).
- *Mission scripts* (`lcs_script_timing.cpp`): ROTATE_OBJECT 0x0352 (object, heading, step in degrees, collision) and
  SLIDE_OBJECT 0x0353 (object, x y z, steps x y z, collision) move an object by the step on every call, and missions
  call them once a frame in `WAIT 0` loops until the object arrives (7 missions use them: m68 JDT6, m92, m97, m110
  DONH3, m112 DONH5, m113 DONH6, m115): the steps are scaled (a hook on the object pool's GetAt 0x112030 at return
  addresses 0x2265F4 / 0x226A2C, after the handlers read their arguments into ScriptParams 0x3DF320). Three missions
  ramp those steps a frame at a time; their adds are listed sites done with the constant scaled: JDT6 +0x3C0D
  (G4054 -= 0.05: two objects' rotation slowing from 12 to 4 degrees a frame), DONH5 +0x3C6F (L12 -= 0.02, the same),
  DONH6 +0x1B4A / +0x1B6A (slide steps growing), +0x1B85 / +0x1B8D (SET_OBJECT_ROTATION 0x0661 angles +0.2 a frame),
  +0x1BAF (the pass count L55 to 165: 5.5 s on the PS2). Offsets are from the mission's start: the IP minus
  *0x3D9C24 (0x42946, MAIN.SCM's main part).
- The LCS script VM, for further work: ProcessOneCommand 0x10A7A8 (IP +0x18 relative to ScriptSpace *0x3CF0F4; the
  opcode's hundred: 0x10A9A0, 0x10DA48, 0x10FF30, 0x208400 ...), GTA III's opcode numbers (0x08 - 0x0F add / subtract
  int / float constants to globals / locals, 0x0352 / 0x0353 rotate / slide, 0x03A9 SCRIPT_NAME stored lower case),
  CollectParameters 0x1094C8 (type byte: 1 / 2 zero, 3 / 4 / 5 the top 1 / 2 / 3 bytes of a float, 6 / 9 four bytes, 7
  / 8 int8 / int16, >= 0x0A a variable), GetPointerToScriptVariable 0x1098D8 (< 0x0C timers +0x200 / +0x204, < 0x6C
  locals from the frame base +0x208, < 0xCC local arrays, < 0xE6 globals ((t - 0xCC) << 8 | byte), else global
  arrays). Scripts: +0x10 name, +0x60 locals, +0x210 wake time, +0x219 mission flag (TERMINATE clears the
  mission-running byte 0x3D9C29).

**Checked and left**
- Ped and object animations: the animation code (0x348E10 .. 0x34A178) reads the step; RAM at two steps showed no
  animation time running with the frame rate, and the player runs the same distance a game second at any step.
- Bike lean and steering: on the step already (table). The bike-in-water / lean floors were patched for >100 fps.
- Planes: the flight paths are timed from the ms clock (CPlane::UpdatePlanes 0x261D10); their trails register a point
  every 2000 ms. Frame-counter readers that only stagger work (`(frame + id) & 7`, `% 12` camera tests, the moving
  things' sectors) are left: they spread work, not amounts.
- Collision sparks (0x2C27C0, 4 SPARK_SMALL per scraping contact a frame) live about one frame, so the number on
  screen is the same at any rate; throttling them would thin them out at 144. Left per frame.
- Mission float adds that are per event (fire truck time bonuses +0.5, coordinates nudged -0.9 once, camera offsets
  of TV / VIC7, Vigilante's round-robin indices L6 / L7 / L39..41).
- The water-lapping sound's fade (0x3D554C, +2 / -1 a frame to 75, read by the audio at 0x168C70): 2.5 s on the PS2,
  about 0.5 s at 144. Audio only; open.


## Smoothness (2026-11-19, agent smooth)

The report: in both-cities mode "Vice City's renderer is better, VCS is just smoother". Measured as what the screen
shows, not fps: `VCS_PACE_LOG` (switches above) logs every present, how many game frames each host frame takes, each
game frame's CTimer step and end; a log analyser reports the shown-frame intervals - 1% low
(fps over the slowest 1%), p99, **jumps** (consecutive shown intervals differing by more than a quarter of the median,
a minute: the judder you see at any frame rate) and repeats. Hidden ThinLTO runs, the user's graphics (3x, MSAA 2x,
aniso 2x, vsync off), the free camera flown at 30 m/s with a 0.7 rad pan (LCS: Portland -> Callahan Bridge -> Staunton;
VCS: the cruise route), 45-60 s windows. The PC was shared with other agents' builds and games, so pairs were run
alternately, and a load test adds 16 busy processes at normal priority (a compile).

What it was:
- **Both games pace the same way** - one game frame per shown frame, the CTimer step from the host clock in whole ms
  (`lcs_framerate.cpp hostTicks`, `vcs_framerate.cpp timerTicks`); neither interpolates nor steps at 30 Hz. Not the
  difference.
- **LCS has no headroom at the user's settings** (120 cap, 1000% draw, 300% traffic / peds): the game thread is 8-15 ms a
  frame (VCS at 300%: 3.5-4), with the render worker and GS stage close behind. A quiet PC holds 60 (at 1000% / 300%
  with the fixes: 60.0 fps, 1% low 47, 76 jumps a minute); anything else running - another game, a compile, the
  background city of both-cities mode - turns into missed frames, where VCS still has room. At 300% / 300% and a 60
  cap, one quiet run: LCS 45 fps shown, 1% low 8, 1,418 jumps a minute; VCS 59.9, 1% low 49, 16.
- **A cap the frames don't fit** (120): host-paced frames take 1, 2 or 3 host frames in turn - 1,500-3,300 jumps a
  minute in both games whenever that happens.
- Not the cause (measured or checked): the EE clock's charge (`VCS_EE_CHARGE_SHIFT=6`: no change), 300% traffic on
  its own, timer resolution (`SDL_DelayPrecise`), the mod loader / swim hook / path search (absent from the profiles);
  the game thread's slow frames are mostly waits (for the host tick, for the render worker, EE deadlines), not one
  costly function.

What was done (each with a switch back):
1. **CPU priority** (`VCS_PRIORITY`): the played game runs above normal priority, so background work no longer takes
   time slices from its threads mid-frame; in Stories: both cities the background city runs below normal and swaps
   with the front (`dualworld.cpp applyRole`). Windows only.
2. **Even frame pacing** (Display > Even frame pacing, default on; `VCS_EVEN_PACE`; `vcs_framerate.cpp
   evenPaceUpdate`): when a fifth of the last 24 frames' busy time (game thread work and its waits for the render
   worker / GS stage) passes 95% of the period - or the whole frames pass 1.15x it (the limit after the game thread) -
   the limiter paces at the highest whole fraction of the display refresh the frames fit (240 Hz: 120, 80, 60, 48, 40,
   30), the host loop with it; back up one level after 2 s within 80% of that level's period (8 s or more after a
   step down for whole frames). Every change is a log line (`[framerate] even pacing: 120 -> 80 fps ...`) and the F3
   overlay's info panel shows `PACE 80 fps` while it holds the game below the cap.

Results (LCS 120 cap / 1000% / 300%, two alternating rounds each where two numbers are given):

| | shown fps | 1% low | p99 ms | jumps / min |
|---|---|---|---|---|
| with background load, before | 85.8 / 92.4 | 28.5 / 30.2 | 31.0 / 27.5 | 3,299 / 3,329 |
| + above-normal priority | 100.2 / 99.3 | 38.6 / 38.1 | 17.4 / 18.2 | 1,732 / 1,638 |
| + even pacing (hidden window: 60 Hz levels) | 55.6 / 52.9 | 32.9 / 27.6 | 26.3 / 27.9 | 53 / 129 |
| + even pacing, 240 Hz levels (`VCS_EVEN_PACE_REFRESH=240`) | 70.2 | 35.5 | 24.4 | 168 |
| both cities, LCS in front, before | 57.7 | 14.7 | 57.6 | 3,166 |
| both cities, LCS in front, both fixes | 56.4 / 61.7 | 34.3 / 36.0 | 25.6 / 25.7 | 125 / 245 |
| both cities, VCS in front (300% / 300%), before | 106.7 | 25.0 | 33.3 | 1,719 |
| both cities, VCS in front, priority only (busy-time trigger: VCS's game thread fits 120, its frames don't) | 107.3 | 55.0 | 17.9 | 1,756 |
| both cities, VCS in front, both fixes (with the whole-frame trigger) | 60.0 | 51.0 | 19.2 | 143 |
| LCS alone, 60 cap, quiet PC, both fixes (VCS alone at 300% / 300%, before: 59.9 / 49.1 / 17.7 / 16) | 60.0 | 50.5 | 18.3 | 24 |

Even pacing trades rate for evenness (55-70 fps shown, but ~25x fewer jumps); a player who wants the highest rate
turns it off. Still open: LCS's per-frame cost at 1000% itself (thousands of draws through all three threads; nothing
in the profiles to time-slice: the visibility scan and the draws are per frame), and the background city of both
cities running at the front's cap (it could run at half).


## RSL functions (for an RSL-level render path, 2026-11-18)

LCS draws through the same Rockstar Leeds layer as VCS (game-internals.md 6i, docs/RSL_SUBMISSION.md). VCS's phase 2
(record / replay of the CALLed packets, `vcs_rsl_world.cpp`) runs for LCS since 2026-11-18 (`lcs_rsl.cpp`, below);
VCS's later steps (draw items, state ops, captures) need VCS's state addresses and are not ported. Found (SLUS_214.23,
Ghidra decompiles and disassembly; VCS equivalents by the same code shape):

| LCS | VCS | What | Evidence |
|---|---|---|---|
| `FUN_00321CB0` | `FUN_003298D8` | texture upload: (slot a0, psm a1, log2 w a2, log2 h a3, bytes t0, source t1, destination t2); two REFs into the slot - the destination's transfer header (`0x30000006`, upper `FLUSHE, DIRECTHL 6`) and the pixels (`0x30000000 \| qwc`, upper `NOP, DIRECTHL qwc`) | the tag constants at 0x321F60..0x321FB0; callers 0x29E570, 0x29EB44, 0x29EB78 |
| `0x633C50` | `0x6EE3F0` | the transfer headers' binary tree: 0x70-byte nodes, key at +0x60, left +0x68, right +0x6C; node count `0x3DA1BC`, limit `0x3DA1B8`, pool `gp+0x18C0` | FUN_00321CB0's lookup loop (0x321D68..0x321DB0) |
| `FUN_0029FD40` | `FUN_002F7D58` | model draw: flushes state (`FUN_002A02F0`), sends the queued GS registers, CALLs the shared state packet when bit 31 of `gp+0x188` is set, then `CALL packet` (a0). **Every model and building draw**: `FUN_00154390` walks an object's packet lists (opaque, then alpha) and calls it per packet. LCS has no world REF draw (VCS `FUN_002F7EB0`) | CALL tags at 0x29FE24 / 0x29FE68; callers 0x1544A4, 0x1544E4 |
| `0x43EA70` (+0x14 half, +0x18 cursor, +0x1C top) | `0x520F90` | the frame's packet buffer: two halves of 0xE6670 bytes from +0x20 (ends 0x60B770) | the stores through `lui 0x44; -0x1590` in the draw functions; the kick `FUN_002B5400` (`half * 0xE6670 + 0x20`, 0x2B544C); seen live |
| `0x65EFD8`, count `gp+0x1D0`; dirty `gp+0x188` | `0x729510`, `0x488308`; `0x4882A0` | the queued GS registers (sent as CNT + DIRECTHL) and the dirty word | FUN_0029FD40 0x29FD54..0x29FE08 |
| `FUN_0029FE98` | `FUN_002F8250` | **Im3D's submit**, not the world: (streams, count, mask) - per batch a CNT (STCYCL, UNPACK header) and a REF per vertex stream with its own UNPACK, then MSCAL 6; its callers `FUN_002B1E60` / `FUN_002B2298` build the streams from index lists each frame (not prebuilt: not replayed) | decompile; REF tags at 0x2A0008 / 0x2A001C / 0x2A0088 |

The texture uploads' pixel REFs are already taken generically (chain images, PERFORMANCE.md 34): no hook needed for them.

**The state flush natively (`lcs_rsl.cpp`, 2026-11-18).** FUN_002A02F0 is VCS's FUN_002F8710 instruction for instruction (the recompiled code's listing) at LCS's addresses: dirty bits gp+0x188 = 0x3D7078 (gp 0x3D6EF0; +4 the render context, +8 its packet, +0x10 / +0x18 the instance matrix's REF switch and address), GS register values 0x3939D8.. (FOG, FOGCOL, TEST, 0x15), GS queue 0x65EFD8 counted at gp+0x1D0, bit 0x2000's table 0x66AC60 indexed by 0x3DA0CC, bit 0x8's source 0x66AC90, the viewport block 0x43DA70 and the others at VCS's offsets from it (instance matrix 0x43D9B0); the same VU addresses and MSCALs. Its two +0x50 plug-in callbacks are VCS's two: 0x23E630 -> FUN_0023E5B8 (VCS FUN_001CEF18: CNT 3, FLUSHE, UNPACK V4-32 x3) and 0x356190 -> FUN_00356110 (VCS FUN_0040F6C8: CNT 9 when +0x90 is set, cleared). `vcs_rsl_native.cpp` takes the addresses as a table (`FlushAddrs`, `installOther`). `VCS_RSL_NATIVE_VERIFY=1`: ~12 M flushes (foot, Staunton, a drive, poses, the opening cutscene; with and without the write watch) the same as the original; ~5% still run the original (bit 0x4's FUN_001BA000, context changes with +0x48 / +0x4C callbacks). About 1% of the game thread.

**Model / building packets replayed (`lcs_rsl.cpp`, 2026-11-18; PERFORMANCE.md 36).** VCS's phase 2 through a seam in
`vcs_rsl_world.cpp` (`installOther`: LCS's packet buffer and `FUN_0029FD40`; no world REF draw, no `FUN_002F80A0` twin -
every CALL in the packet buffer is marked anyway): as the chain is copied each CALLed packet gets a MARK; the first time
the render worker records what it does (UNPACK results, MSCALs, GS data, the world handler's meshes), later frames
replay that while its bytes are the same - no VIF parsing, unpacking or world-mesh hashing / conversion for it. With
the write watch an unchanged packet is not even copied (step 4). The texture uploads inside CALLed packets (material
setups) are kept in the recordings as chain images and replayed by reference (the runtime's chain images now work with
an observer installed: `setChainImagesWithObserver`, PS2RECOMP_PATCHES.md). Checked: `VCS_RSL_WORLD_VERIFY=1` (each hit
processed by the packet path and recorded again, compared op by op) - 3.6M+ recordings over the three poses and the
drive, 0 differ; pictures at fixed free-camera poses as the packet path's (static views within the run-to-run noise;
moving traffic aside); a 4-minute drive at 400% (18k frames, recordings 86 MB of the 256 MB bound) and the opening
cutscene clean. **On by default only with the write watch (Windows)**: without it (Android, Linux; `PS2X_WRITE_WATCH=0`
on the PC) every packet is still copied and compared, and the observed chain walk costs the game thread 1-3 ms - a loss
on the PC (Portland 400%: 56 -> 43 fps). `LCS_RSL=1` turns it on there, to measure on a phone.

## Swimming (Enhancements > Gameplay > Swimming, 2026-11-18)

LCS's Toni drowns: in deep water CPed::ProcessBuoyancy (0x1D65E0, called once a frame from CPed::ProcessControl
0x1D6F30) hurts a ped whose head is under the surface by `step * 3` (WEAPONTYPE_DROWNING 0x2B, through
CPed::InflictDamage 0x1C1F78) - he dies in about 2.4 s (measured at (1123, -1260)). VCS's swimming is VCS's own code;
LCS has no swim animations or states.

With **Swimming** on (default off: the original game, and the islands stay out of reach until the story opens them;
forced on when `STORIES_DUALWORLD` is set), `src/game/lcs_swim.cpp` replaces that buoyancy for the player's ped on
foot while his origin is under the surface (the water level computed as CWaterLevel::GetWaterLevel 0x20D970: a 128 x
128 grid of 32 m cells at `*(0x3D5528) + 0x100C`, the levels at `*(... + 4)`, plus the waves):

- he is held at the surface (bobbing with the waves); no drowning, no fall (a fall from 60 m into the sea ends in
  swimming, health 100);
- the game's player control still picks the move state and heading (+0x354 / +0x4E0) from the stick or keys; the
  speed is set from it (a ped without ground is moved by nothing else): with the swim animations as fast as the
  stroke pulls him (its root's travel: breaststroke walking ~1.2 m/s, running ~1.8, crawl sprinting ~3.2), without
  them walk 1.5 / run 2.8 / sprint 4.2 m/s. Turning is held to 2.6 rad/s (the wanted heading +0x4E4, before
  ProcessControl turns towards it) and the speed follows the heading slowly (6% a frame): a turn drifts;
- **the look**: CPed::SetInTheAir (0x1E3410: sets bIsInTheAir and blends FALL_glide 0x8C, every frame a ped is
  neither standing nor in the air) is skipped while he swims - no more arms-spread pose. With the mod **"Swim
  animations (from your VCS disc)"** he plays Vic's own: swim_idle (treading water) still, swim (breaststroke)
  walking or running, swim_fast (crawl) sprinting. Without it the game's own idle / walk / run play slowed (a
  stand-in, mostly under water);
- **no weapons**: CPlayerPed::ProcessPlayerWeapon (0x34AF20) is skipped while he swims - no aiming, no shots (checked:
  a Tec-9 held on fire in the water, 550 rounds before and after; on land it fired 16);
- **the trainer's spawn** makes only a boat while he swims or sits in a boat ("On the water only a boat can be
  spawned"): warped into a
  car, helicopter or plane made in the sea under him he went down with it and drowned inside (a Dodo: health 100 -> 11
  in 5.7 s; the swimming keeps only a ped on foot afloat) - the "player is down" of the crossing stress tests after an
  on-foot arrival in the sea (2026-11-19). The trainer's spawns and the border arrival also refuse / wait with the
  vehicle pool nearly full (game-internals.md, LCS's vehicle pool);
- **out**: where the sea bed rises (a beach) the ground's collision lifts him and he lands as from a jump and runs
  on. **Jump** (Space / Square, CPad +0x26) asks for a ledge 1 m ahead: CWorld::ProcessVerticalLine (0x175CA8; point
  a0, z stop f12, colpoint a1, entity a2, buildings / vehicles / peds / objects / dummies in a3 / t0-t3 = 1 1 0 1 0 as
  CPed::InTheAir calls it; the colpoint's point at +0, its normal at +0x20) from 2.8 m above the water down to it.
  A top 0.15-2.75 m above the water with an upward normal (z > 0.6) is climbed onto (up at 3 m/s, then forwards onto
  it); anything else - open water, a wall with no top in reach, a pier 10 m up - gives a small hop (~1 m). No
  straight-up wall climbs any more (we know of no ladders at LCS's water's edge). **Enter-vehicle** (F / Triangle)
  boards a boat beside him (he stands for that frame: the game refuses it to a ped in the air);
- other peds, the dead, the player in a vehicle and cars in water go through the original code.

**The swim animations, from the player's own VCS disc.** `python tools/stories_pack.py swim <VCS iso>` (the
launcher: Mods sheet > "Swim animations - from your Vice City Stories disc" > Make the swim animations, with the VCS
disc it knows) reads VCS's GAME.DTZ once, takes its swim set (swim_idle, swim, swim_fast, swim_start, swim_to_idle,
swim_jump_st / mid / mid2 / end / miss: 10 animations, 17 KB) and writes the LCS mod `stories_vcs_swim`
(`<LCS data>/mods/stories_vcs_swim/`: manifest.json, files/vcs_swim.anm; active.txt `anims set=swim
file=stories_vcs_swim/files/vcs_swim.anm`). The mod manager lists it like any mod (on / off; off = the stand-in). Made
from the player's disc on the player's computer: never committed or shipped (.gitignore and
`tools/prepublish_check.py` refuse `stories_*` folders and `.anm`). Nothing is read from the VCS disc while playing.
The code is in `tools/stories_swim.py` (the `swim` kind of `stories_pack.py`).

The two games' animation formats (found 2026-11-18 by reading both GAME.DTZ images, then LCS's anim manager in a run):

| | VCS | LCS |
|---|---|---|
| CAnimBlendHierarchy | 0x28: +0 sequences, +4 name[0x14], +0x1C s32 count, +0x20 float length, +0x24 a word LCS lacks (0x734-0x778 in the swim set) | 0x24: +0 sequences, +4 name[0x14], +0x1C s16 count, s16 0xAAAA, +0x20 float length |
| CAnimBlendSequence | 0xC: u16 flags (1 rotation, 2 translation, **8**), u16 frames, keyframes, u32 bone tag | the same, with **0x10** where VCS has 8 (mapped) |
| keyframes | half floats: 10 bytes {quaternion x y z w, time step} or 16 with translation {quaternion, step, x y z}; the first step 0 | the same |
| bone tags | (hash << 16) or bone id: 0 root, 1 pelvis, 2-5 spine / neck / head, 0x15-0x18 and 0x1F-0x22 arms, 0x29-0x2B and 0x33-0x35 legs | the same ids (the swim set binds 20 of the ped's 24 nodes; the other 4 keep the pose of the animations under it) |

LCS's anim manager (read in a run): CAnimManager at `*0x3D9D24`, +0x10 the association groups, 0x14 bytes each {+0
the anim block, +4 the template associations (0x44 each), +8 count, +0xC first id, +0x10 the group's index}
(GetAnimAssociation 0x1F7488 -> CAnimBlendAssocGroup::GetAnimation 0x1B9F40: templates + (id - first) * 0x44). Group
0 = the ped's 169 (ids 0-0xA8), groups 31-59 the ped types' move groups (ids 0-4), the streamed blocks' groups from
60 on; **group 53 is empty** in LCS. An association: +0 next, +4 prev, +8 u16 flags (1 running, 2 loops, 4 delete
when faded out, 0x10 partial, 0x20 movement, 0x40 root translation), +0x10 blend, +0x18 node count, +0x1C nodes,
+0x20 hierarchy, +0x24 blend delta, +0x28 time, +0x2C speed, +0x30 s16 id, +0x32 s16 group. A node (0x1C): theta0,
theta1, frame a, frame b (byte offsets into the keyframes), remaining, sequence, association - one per skeleton frame
(24 for a ped). A clump's associations: `**(clump + 0x18)` (RpAnimBlendClumpGetAssociation 0x146458).

At the first swimming frame `lcs_swim.cpp` builds the pack as **group 53** in guest RAM (the runtime's guest heap,
~25 KB): per animation a hierarchy, its sequences and keyframes as they are, a template association copied from group
0's IDLE_stance (flags: loops; movement + translation when the root moves) and 24 nodes bound by bone tag (the tags
read from group 0's templates); ids 0x1C0-0x1C9 (LCS's own end at 0x1BB). It starts the stroke for the move state with
CAnimManager::BlendAnimation (0x1F7630: f12 delta, a0 clump, a1 group, a2 id) as a **tail call** from the
ProcessBuoyancy replacement (the registers set, `pc` = 0x1F7630, `ra` left: it returns to ProcessControl; the ledge
probe calls ProcessVerticalLine the same way) - host code never calls guest code. While the stroke plays, the game's
own idle / walk / run (its move code keeps blending them back) are held at nothing but kept running; out of the water
they come back and the swim animations fade out and are deleted. (An association left at nothing stops running - its
bit 1 cleared - and the game blended it back in without starting it: on a beach he stood in the run pose going
nowhere; fixed by keeping bit 1.)

Checked in hidden runs (2026-11-18, Windows; `agents\swim2\`): 60 s in deep water; treading water, breaststroke and
crawl seen from the side under the free camera; in from the sea onto the east beach (1520, -400) and on up it; off a
Speeder's deck, back onto the deck with jump from the water (the probe found the deck 0.97 m above the water; climbed)
and aboard with enter-vehicle; against a quay wall with no top in reach (Portland, (1458, -939)): a hop, no climb;
turning; a Tec-9 doesn't fire in the water; the stand-in without the mod; off: drowns as before (100 -> 0 in ~2.5 s).
Not done: a climb-out animation (the jump's pose shows for the half second of a climb; VCS's swim_jump_* are dives),
diving and a breath meter, the Android launcher's button (ModFormats: follow-up; a pack copied into the app's mods/
works the same), a low concrete quay (none found near the tested shores: the boat's deck stood in for the ledge test).

## Known issues / not done yet

- **Shots / muzzle flash another way than Toni faces (report B4, 2026-11-21, seen on the phone; PC not checked)**: not reproduced or pinned down yet. What the code says (decompile of FireInstantHit 0x2A4BF8): the muzzle flash, tracer and shells (0x2A5D30) are drawn along the shot's own line (gun -> end), so a flash pointing away is the shot itself going away, not a render fault. The line comes from one of four branches: with a target and the IK's gun-pointed bit (ped +0x33C bit 0) toward the target; with a target and the bit clear along the right hand bone's X axis as last drawn (VC's FireInstantHit does the same) - the only branch that can point away from both Toni's heading and the target; with no target along his matrix forward. `lcs_aim`'s free aim redirects only the two target branches, and only while its free aim is active (`Gamepad > Free aim` is off by default, so on touch the game's lock-on / hip fire is used). To capture on the phone: `python3 tools/android.py env LCS_SHOT_LOG=1`, fire a few shots hip-fire and locked on with a pistol / SMG, `python3 tools/android.py log` and keep the `[lcs-shot]` lines; then the same with `LCS_SHOT_TO_TARGET=1`.

- **Memory**: no leak found; it levels off (2026-11-18, Windows, hidden, `tools/lcs_memsoak.py`, 24 landmarks a round
  over the three islands, 125 min). Draw distance 100%: private bytes 325 MB at the start, 420 after 15 min, ~510 from
  75 min on (trend over the second half -0.06 MB/min; +/-15 MB wobble as caches cycle). The GS caches (3,000 textures,
  12,000 meshes) are full after ~5 minutes and cycle at their caps (`PS2X_HW_UPSTATS=1`); the chain-image store stays
  at 17 MB (cap 64). With `PS2X_CHAIN_IMAGES=0` it plateaus ~180 MB higher (~695 MB: the GS then keeps its own
  copies), so the interning saves memory. Draw distance 400%: 330 -> 640 MB in 20 min, then +1.5 MB/min falling to
  ~0 (690 MB at 60-70 min, teleports only; 840 MB at 90 min with a car driven at every stop, still +0.8 MB/min
  there, a higher plateau not yet reached). The caps hold, the other maps stay at a few dozen entries. The 3 MB a
  round the QA run saw late in a 16-round loop was the fill of the plateau.

- **Untested**: the story played through by hand (the 76 mission entries only start and run; most were not played to
  their end). Checked on the Mac and on Windows (hidden runs, 2026-11-18): the opening, the first mission, driving,
  death and respawn, a wanted level outside a mission, a real arrest at the Portland police station, bike, boat,
  helicopter, Ammu-Nation's interior, all 24 landmarks on the three islands, F9 / F10, 30-minute drive and idle soaks.
  Real mouse, real pad and visible windows were not used in the Windows runs (injected input; the F2 menu's rows were
  checked through its control command). The Windows build is the quick (unoptimised) one in most numbers here.
- **Draw distance**: at 100% props still appear at 100 m, as on the PS2 (raise the setting to move it). At 1000% in
  the densest views the second prop list takes props within 500 m only and stops where the frame's DMA buffer gets short
  (`lcsdraw` noroom); the farthest props are dropped first. The render worker's time roughly doubles at 400% in dense
  streets. "With draw distance" (no haze) shows the far city in its own darker colours; "+ haze" (default) fades it
  into the fog colour, "Original" keeps the PS2's look. Not checked on Android. (The dark specks in fog were the sky's dark fill and horizon band showing
  through the far edge: fixed, `lcs_sky.cpp`, game-internals.md 6p.)
- **Traffic density**: 200% and 300% both reach the game's own limit of 30 random cars (its generator stops there; the
  setting's car cap of 45 counts mission / parked / police cars too). The ped pool keeps 16 free (drivers).
- **Frame rate in heavy views**: Staunton / driving draw 2,000-2,700 times a frame (VCS's heaviest ~1,950). After
  PERFORMANCE.md 34 (UNPACK fast paths, the GS queue, chain images, ThinLTO) the game thread, the render worker and the
  GS stage take ~4-7 ms each there on the test PC (Staunton ~112 fps at a 120 cap with the PC loaded, 120 when quieter;
  140-190 unlimited). Open: per-draw work on all three, or the RSL-level path (section "RSL functions"); not measured
  on a phone. Under the El in Portland (1000,-400) a cap of 60 once gave 30-40 fps with the old pacing; after the
  host-tick fix it holds 60 (cap 120 / 240 there: 56-59 fps on a shared PC).
- **VU0 programs**: VCS's native VU0 kernels (`src/game/vcs_vu0_native.cpp`, matched by code hash) run for LCS too.
  LCS's RW frustum sphere test 0x29F258 starts VU0 0x0C00 (0x0C10's sphere test on the matrices at vi15+12): interpreted
  it took ~30-45% of the game thread; native since 2026-11-18 (with 0x0CE8, the box test's +12 entry).
  `PS2X_TRACE_VU0=1` lists any program still interpreted, `PS2X_VU0_VERIFY=1` checks the natives.
- **High frame rates, still per frame** (section "High frame rates"): mission scripts beyond the listed sites (the
  survey judged MAIN.SCM's float adds of constants; per-pass int counters only seen as round-robin indices so far, not
  judged mission by mission), collision sparks (left on purpose), the water-lapping sound's fade (0x3D554C, 75 frames:
  4.8x too fast at 144, audio only), rotor blades and the push from a body under a wheel (LCS's twin 0x32D5E8 has no
  damage-multiplier argument: not ported; no flyable helicopter in the story). Not exercised in a run: lightning
  bursts, the water cannon, the radio fix, the rotate / slide steps of JDT6 / DONH5 / DONH6 (the DONH3 slide at 1/144
  never got there in the run). The HUD's blink at 60 / 100 not checked by eye (measured: 3.87 / 7.55 flips a second at
  30, 4.26 / 7.46 at 144, PS2 3.75 / 7.5).
- **Aiming**: a gun on a vehicle (camera mode 45) turns by the gamepad's right stick and fires on R1 only with Gamepad >
  Free aim on (off: the game's own control, the left stick and Circle; the mouse and the left button either way); helicopters' guns are fixed forward (nothing to aim).
  In a drive-by the arm stops 140 deg from ahead (the right arm cannot reach further without the torso). A one-frame
  camera snap may show when free aim is released.
- **Safehouse saves**: a quick save or Save game here is refused inside a safehouse (its own save point is there), and
  a load of a save made outside one clears the "inside a safehouse" global the load's init script sets
  (`lcs_save.cpp`, game-internals.md 11j). Phone calls after a quick load were not seen directly (only their
  condition was read). The old shared test card's slot 8 was itself such a save; its load is repaired automatically.
- **Button prompts** are text names; the PS pad's face buttons stay the PS2 symbols of the game's font.
- **Car paint** ("slightly flatter with the native renderer", QA on the Mac): not reproduced on Windows. One frame, the
  same parked car under the free camera, `lcsrender lit 0` / `1` in the same run (Sentinel, Landstalker, Stretch,
  Esperanto, Police; 12:00 and 18:00): the body pixels are the same within the frame-to-frame noise of the post effects
  (police body region: 1,549 of 42,000 pixels more than 2 off, means equal to 0.2); `PS2X_HLE_VERIFY=1` 0 off; block
  keys off: the same. The only difference: the one-pixel cracks along panel edges sit elsewhere. Not checked on Metal.
- **Police cars**: blue bodies / lights not done (section "Police cars and pressure").
- **Mods**: `maxspeed` on LCS sets the top speed itself (LCS keeps no handling.cfg speed; VCS scales its own);
  a converted car keeps the slot's collision (as on VCS); its wheels are the slot's shared wheel unless the mod
  carries its own (then elements on the car's own wheel nodes); real downloaded mods were run (section "Mods: real PC
  mods"): heavy models (30-80k triangles) are decimated to 3000 / 1500 and some panels show it; a `replace`d file the
  game opened before the disc index existed keeps the disc's bytes (none known); weapon types capped at 36 (table
  length not confirmed); LCS has no flyable aircraft (a plane / heli mod only changes the look of a leftover that
  drives as a car). Section "Mods".
- **Android**: see the "Android" section: all of the later features are unchecked on a phone.
- **The launcher on macOS / Linux** is not checked for LCS (the shared C++ was not compiled there); its first
  Saves > Play from a real LCS slot was not exercised (a planted file only).
- **Sky**: it looks near-white at midday - that is the PS2's look, not a bug (LCS's timecycle asks
  for a pale blue top (173,211,255) over a pale cyan horizon (173,255,250) at 12:00 sunny; fluffy clouds, the sun's glare
  and the trails pass's cyan tint whiten it further; `LCS_RENDER=0` and `PS2X_HW_POSTFX=0` look the same, and PCSX2 /
  console footage shows the same; game-internals.md 6p).

## Mods

The mod loader (`src/game/vcs_mods.{h,cpp}`, docs/ANDROID.md "Mods" for the format and VCS's side) runs on LCS with
LCS's own addresses. It reads `<data>/mods/active.txt` - the LCS data folder's (`%LOCALAPPDATA%\GTAStoriesRecomp\LCS\mods`,
`~/.local/share/GTAStoriesRecomp/LCS/mods`, `VCS_DATA_DIR`'s; Android `files/userdata-lcs/mods`), so each game has its own
mods - and writes `mods/applied.txt` plus `[mods]` log lines. Directives on LCS:

| Directive | LCS | What it does |
|---|---|---|
| `replace disc=<path on the disc> file=<path under mods/>` | works | serves a whole disc file (any size) from the mod: `TEXT/ENGLISH.GXT`, `DATA/MAIN.SCM`, ... (new on both games; VCS: files opened after GAME.DTZ, not checked in a run) |
| `stream index=<n> copy=<m>` / `file=<path>` | works | an IMG entry (model `n` < 4900, texture list 4900 + slot) served from another entry / a file |
| `weapon type=<1..36> <field> <value> ...` | works | `weapon.dat` columns into both weapon tables (types as in the trainer: pistol 17, shotgun 19 ...); `firex` / `firey` / `firez` = the fire offset (muzzle flash) at +0x30 / +0x34 / +0x38 |
| `handling id=<vehicle model> <field> <value> ...` | works | `handling.cfg` values into the slot's record (fields as VCS's, plus `drag`, `inertia`, `collision`, `abs`, `frontlights` / `rearlights` and `flags` / `flagmask` (hex, VC's bit layout), which LCS's record keeps); `maxspeed` = the new top speed in km/h |
| `model id=... dff= txd= [part=<frame>]` | works | a PC model converted into LCS's chunk dialect (below) and streamed in place of the slot's; converted once and cached (`mods/<mod>/converted/`, docs/ANDROID.md "Mods"); a car's dummies move the slot's nodes and its light / seat / exhaust positions and wheel size go into the model info; a bike's forks, handlebars, mudguard and wheels go onto the bike's own nodes; `part=` takes one weapon of a clump holding several |
| `colours id=<vehicle model> pair1 r,g,b,r,g,b ... pair8` | works | a `carcols.dat` line (RGB from the mod's own colour list): each colour to the nearest of the car palette, written as the model info's colour list (+0x170 / +0x178 / +0x180) |
| `anims set=swim file=<.anm>` | works (LCS only) | an animation set the game builds into an animation group of its own: `set=swim` = Vic's swim animations, made from the player's VCS disc by `tools/stories_pack.py swim` (mod `stories_vcs_swim`, section "Swimming") |
| `sound id=<id> file=<audio file>` (or `bank=<b> index=<i>`, or `name=<VAG name prefix>`; optional `gain=<percent>`) | works (LCS only) | an effect sound replaced by a `.wav` (or `.mp3 .ogg .flac`): fitted into the sound's own slot at its own rate (section "Sound mods" below). Speech (bank 2) not supported |
| `test give=<type>` / `spawn=<model>` | works | device tests through LCS's trainer once free play starts |
| `needs file=<path>` | works | a mod kept where it is names its originals; one missing -> the whole mod is skipped (`<id>: skipped: original missing (...)`) |

File values (`dff=`, `txd=`, `file=`, `needs file=`) are a path under `mods/`, an absolute path (desktop "keep where
it is") or `place:<handle>` (Android), optionally `<zip or .vcm>!<entry>`, `%XX`-escaped (`src/game/vcs_modfiles.h`).
Checked 2026-11-18 in hidden LCS runs (agent modplace, `agents\modplace\run\p1..p5`): the box Kuruma read from a zip
kept in place (`testmods\box car pack.zip!Box Pack/kuruma.dff`) and the box pistol from a folder kept in place were
converted and drawn (p1_car.png, p1_aim.png); the next start took both from the cache without reading them (p2: "sources
unchanged, not read"); the zip renamed -> `box_car_pack: skipped: original missing`, the stock Kuruma drawn (p3_car.png),
the other mod applied; `mods.py relocate` with the renamed zip -> drawn again (p4_car.png); `store copy` read it from
`box_car_pack/source/box_car_pack.zip!...` with the original gone, and `place:` handles through `VCS_MODS_PLACES` (p5).

**The mod manager** (desktop launcher *Mods* sheet / `tools/mods.py --game lcs`, Android's Mods category with LCS
chosen; docs/ANDROID.md "Mods") imports into this folder and matches names to LCS's own slots (`tools/modslots/mod_slots.txt`:
vehicle model ids 130-216 by GTA III name / LCS text key, weapon types 1-36 with their model ids, read in a run). Checked
2026-11-18 (hidden run on a data folder made by `tools/mods.py`, mods made from scratch): the manager's `weapon type=17`
/ `18` lines were applied (`applied.txt`: clip 77 (was 17), damage 120 (was 25); .357 clip 9 (was 6)), its handling and
model lines reported as skipped by this build (the manager shows them so); the stock values are kept in `mods/stock.json`.

**Real PC mods (2026-11-18, agent showcase, Windows LTO build, hidden runs)**: published mods, unpacked (`.rar` / `.7z`
with Windows' `tar`) and imported folder by folder with `tools/mods.py import`, then spawned at the airport apron:

| Mod (game it was made for) | Slot | Result |
|---|---|---|
| Ferrari 458 Italia - Undercover Cop by N4SPEED (VC; `Ferrari_458_UC.dff` renamed `banshee.*`, its VC handling line) | Banshee 160 | converted and drawn (body, wheels, paint; README `docs/media/mod_real_ferrari.jpg`); handling applied (`dimension z` skipped) |
| Maibatsu Sanchez, GTA IV style (VC; `sanchez.dff/.txd/.col`, the README's `DIRTBIKE` handling line) | Sanchez 210 | drawn and ridden; the handling line's name `DIRTBIKE` matches no slot: `mods.py retarget <id> 0 sanchez` sets it; `.col` reported as not used; its `extra1` not converted |
| Peugeot 103 (`faggio.dff/.txd`) | Faggio 206 | drawn and ridden |
| M4 (`M4.dff/.txd` + four preview `.png`) | M4 280 | held in hand. The folder import failed on the `.png` screenshots (`1.png is not a .zip, a .vcm, a model or a text file`) until `mods.py` left pictures and `.col` out, as it already did inside a `.zip` |
| Cabby "Capital Spec" by GSC_MODS (GTA III) | Cabbie 166 | **not usable**: only the wheels and the roof sign are drawn (converter: "4 of 20 parts from the mod, 16 left empty; 79 triangles in the body"); the `cabbie.txt` carcols line is reported as not applied (PC palette numbers, no colour list). Open |

Not tried: the GTA 3 HD weapon pack (a modloader install with `.asi` / `.dll` plugins), the SA Impaler, the FN P90.

`tools/gxt_edit.py` changes strings of a Stories GXT in place (same length or shorter), for a `replace` of the text:
`python -I tools/gxt_edit.py extract <iso> ENGLISH.GXT`, then `python -I tools/gxt_edit.py ENGLISH.GXT
mods/<mod>/ENGLISH.GXT FES_NGA="MODS OK!"` (`--find TEXT` lists keys). The input is your own disc's file: never
share or commit it or the result.

**Checked (Windows, hidden runs, 2026-11-18)**, a test mod made from scratch from the player's own disc files - four
pause-menu strings (`FEH_LOA` GAME -> MODS, `FES_LOA` / `FES_NGA` / `FES_DGM` -> MOD LOADR / MODS OK! / HELLO
LCS!!), the pistol's clip 17 -> 77 and damage 25 -> 120, and the pistol's model (274) streamed from the shotgun's
entry (277): the pause menu's tab and Game page show the new text, the HUD shows the pistol with 123-77, Toni holds
the shotgun model; with `VCS_MODS=0` the same card shows GAME / LOAD GAME / NEW GAME / DELETE GAME and 183-17.

**Where LCS differs from VCS** (evidence: disassembly of SLUS_214.23, and `VCS_MODS_DUMP=1` runs):

| What | VCS | LCS |
|---|---|---|
| Hook | GTAG relocation `0x324A50`, tables from the image header | GAME.DTZ's header is laid out differently: the model-info setup `0x31FAF0(count, infos[])` (called at `0x104850` by the GAME.DTZ loader `0x1045A8`, after the relocation `0x22C4E8`, the streaming pointer `0x18A3D0` and the weapon infos `0x34F2E0`) is hooked before it runs; its arguments are the relocated model infos |
| Streaming info | header +0x7C -> +200; 0x18 per entry, +0xC state, +0x10 sector, +0x14 count | `*0x3D0CC8` (gp - 0x6228); 0x14 per entry, same offsets (GetCdPosnAndSize `0x18A390` reads entry + 4 + 0xC / + 0x10); 6197 entries |
| Texture list index | 7534 + model info +0x1E | 4900 (0x1324) + model info +0x1E (RequestModelStream `0x18F31C`); 4900..6099 texture lists |
| IMG | `GTA3PS2.IMG`, read through the file layer (its size clamps a seek: `extendImg`) | `MODELS\GTA3PS2.IMG` (string `0x3AD710`), read by sector through CdStreamRead (`0x18F540`): no clamp, nothing to extend |
| Disc index (for `replace`) | `0x5038A8`, 16-byte entries {name, LSN, size}, count `0x486E8C` | `0x62AA70`, 16-byte entries {name `\TEXT\ENGLISH.GXT;1`, 0, LSN, size}, count `0x3D7C04` (gp + 0xD14); patched at the first open (`0x2E6208`, every disc-file open) after it has entries |
| Weapon tables | header +0xC0 -> two tables, pistol type 19 | `*0x3D9B48` -> two tables (GetWeaponInfo `0x34F2F0` uses the second), pistol type 17; every field 4 bytes lower than VCS's (+0 fire type ... +0x10 clip, +0x14 damage, +0x60 model) |
| Model info name | +0x158 | +0x2E |
| Handling | vehicle model info +0x38 -> record (the gear table first; 0xE0 bytes) | vehicle model info +0x42 = int16 handling id; records from cHandlingDataMgr (`*0x3D0238`, GAME.DTZ header word 0x22, stored by the loader at `0x1047E0`) + 0x20, 0xF0 each, 81, the id at +0x10; field map below |
| Model chunks (`model`) | MDL / XTX as written by `vcs_modconv` | the same relocatable chunks, element, node, geometry, materials and VIF batches (the stock pistol's DMA has VCS's exact layout: positions to VU 1, normals 0x47, UVs 0x8D, MSCAL 6) except: a raster is 8 bytes {texels, flags} (VCS 16), a texture is 0x54 bytes with +0x50 = 1 (VCS 0x50), texels start at +0x40, 8-bit textures carry 0x11 in flag bits 20+, a vehicle node's name pointer is at +0xA4 (VCS +0xA8), and a car has 25 primary paint materials at +0x2C and 25 secondary at +0x90 (VCS 30 + 25). `Options::lcs` |
| Free play (device tests) | TheCamera +0x888 | TheCamera +0x9C (`0x43BC8C`) |

**LCS's handling record** (0xF0 bytes; all 81 records of GAME.DTZ listed with these offsets; `vcs_mods.cpp`
kLcsHandling). The manager is re3's cHandlingDataMgr: 5 floats {0.1, 0.9, 1.0, 0.8, 0.98} (`0x327EEC` reads its +4),
padding, then the records. The model infos hold the id in VC's handling order (LANDSTK 0, IDAHO 1, STINGER 2 ... KURUMA
20; +0x38 is the class: 0 car, 1 boat, 2 train, 3 heli, 4 plane, 5 bike), and a spawned car's record pointer is
vehicle +0x150 (a Kuruma: manager + 0x20 + 20 x 0xF0, read in a run).

| Off | Field | Conversion (as VCS's unless said) |
|---|---|---|
| +0x00 / +0x04 / +0x08 | mass, turn mass, traction multiplier | as is; turn mass from the dimensions or scaled with the mass |
| +0x0C | collision damage multiplier | `collision` cfg x 2000 / mass (0.54 -> 0.675 at 1600 kg, 0.7714 at 1400 kg); kept per kg for a new mass |
| +0x10 / +0x14 | the id / 1 / mass | checked / derived |
| +0x18 | drag multiplier (SA's: 2.5, 2.0 ...) | `drag` as is |
| +0x20..+0x28 | centre of mass x, y, z | as is |
| +0x30 / +0x34 | percent submerged (int) / buoyancy | buoyancy = 100 / submerged x 0.008 x mass (every record) |
| +0x38..+0x7F | gear table, 6 x {max velocity, shift up, shift down} | rebuilt (VC's InitGearRatios) |
| +0x80 / +0x81 / +0x82 | drive (F / R / 4), engine (P / D / E), gears | as is |
| +0x84 / +0x88 | engine acceleration / engine inertia (SA's) | cfg / 2500 / 4 (4WD) or / 2 / `inertia` as is |
| +0x8C / +0x90 / +0x94 | max velocity, cruise (= max / 1.2), reverse | `maxspeed` km/h: max velocity = cfg / 180 (no cfg speed kept: the slot's own max velocity x 180 stands in for VCS's +0xB8), cruise scaled |
| +0x9C / +0xA0 / +0xA4 / +0xA8 | brake (cfg / 2500), brake bias, ABS (byte, 0 in all 81), steering lock | as VCS; `abs` 0 / 1 written as the byte |
| +0xAC / +0xB0 | traction loss / bias | as is |
| +0xB8..+0xCC | suspension force, damping, upper, lower, bias, anti-dive | as is |
| +0xD0 / +0xD4 / +0xD8 / +0xDC | handling flags (VC's bit layout), seat offset, value ($), light types (bytes +0xDC front, +0xDD rear: 0..3) | `flags` / `flagmask` (hex, VC's bits: the manager converts SA's model + handling flags bit by bit); the mod sets the bits of its mask, the vehicle-kind bits 0x8F0000 (bike, heli, plane, boat, sit-in-boat) and LCS's own 0x8000000 / 0x10000000 stay the slot's; `frontlights` / `rearlights` as is |

**Checked (Windows, hidden runs, 2026-11-18)**, mods made from scratch (no third-party file), the free-roam card at
the airport, a Kuruma (model 152) spawned, turned 60 degrees towards the open field, Cross held:
- stock: 36.6 m and 75 km/h after 3.0 s, 123 km/h after 8 s (still rising);
- `handling id=152 accel 40 maxspeed 250`: 42.1 m and 101.5 km/h after 3.0 s (record: acceleration 0.00152 -> 0.008,
  max velocity 0.827 -> 1.389, read back through the car's own pointer);
- `handling id=152 maxspeed 60`: holds 60.0 km/h from 4 s to 8 s.
- `model id=274` / `id=152` with a box .dff / .txd written by a script (a 0.3 m red / white checked box for the pistol,
  a 1.8 x 4.4 m one as the Kuruma's chassis_hi): the Kuruma drawn as the checked box at the airport, the pistol's
  box in Toni's hand, the pistol's HUD icon intact (it is in the same texture list: the 0x54-byte texture and +0x40
  texels were found that way). Second start: `cached` for both, no conversion.

### Mods: real PC mods (2026-11-18, agent realmods)

Eight mods as players download them (GTA III / VC / SA; cars, bikes, weapons; .rar / .7z / .zip, one with nested
.7z, one a modloader folder with .asi / .dll plugins) were imported with `tools/mods.py` (the archive and the unpacked
folder, `copy` and `in_place`) and run in hidden LCS runs from the free-roam card: every car / bike spawned, driven
3 s, shot from three sides with the free camera; every weapon given and shot in Toni's hand; 55-60 fps throughout.
What was found and changed (all generic; the mods are other people's files: never in the repository):

| Found | Change |
|---|---|
| `.rar` / `.7z` refused | unpacked into a temporary folder by 7-Zip (7z / 7zz / its install folder) or bsdtar (Windows 10+'s `tar.exe`, macOS's `tar`), `VCS_UNPACKER` overrides; nothing in them is run; their models go to `files/` (the game can't read inside them); without either tool the old message. Android: still refused |
| a picked folder failed on any picture or `.dll` | programs / plugins (`.exe .dll .asi .cs .lua` ...) and pictures are named in the notes, never read, copied or needed; `.ifp`, `.fxt`, `.col`, `.ipl`, `.img`, sounds get a note each |
| one model three times (PC / Android / alternative-loader versions in folders) | the shallowest, non-phone version is used, the others noted |
| a GTA III-style weapon pack: one `weapons.dff` with every gun (`colt45_l0`, `m16_l0` ...) and a 31 MB `MISC.TXD` | one `model` entry per frame that names a weapon here, `part=<frame>` (the converter takes that frame alone, in its own space) |
| a III / VC car's handling line under a handling id (`DIRTBIKE`) | an IDE line in any text file (`id, model, txd, type, handling id, ...`) maps it to its model; `dirtbike` is also a Sanchez alias |
| a car model named after the real car, not the slot | a `Replace (Banshee)` folder or a readme's "replace(s) X" names the slot; a mod's single car model and its handling / colour lines follow each other |
| a weapon loader's `.xml` (`<logicalid>` = VC's weapon type) next to a renamed gun | a weapon entry from it; a model in the same folder as a placed weapon entry takes its slot |
| several versions for one slot in one mod | the best placed (name, alias, id, readme, mod, nearest) is used, the others set aside ("prefer" picks another) |
| a III taxi drew 4 of 20 parts | the UV set count is the geometry's byte (flags only when it is 0): both textured flags set with one set read every triangle from the wrong place |
| the slot's wing nodes have no elements (LCS's Cabbie) | parts the slot has no element for go into the body |
| an SA car didn't move | SA's suspension limits (0.1 / -0.05, tuned to SA's collision) are not used; the slot's stay |
| an SA car's windows showed dark squares | LCS draws car textures opaque (a stock car texture's alpha is not transparency): see-through materials are laid over a dark tint |
| 30-80k triangle models stopped at 10k of a 1.5k budget | a welded second decimation pass (vertices at one position collapse together, each onto the nearest-UV vertex; borders slide along themselves) |
| car atlases blurred at 64 px | 128 px, all of a car's textures within ten 128 x 128 |
| bikes: the forks, handlebars and wheels were stuck on the body or missing | a part named as one of the slot's own part nodes goes onto that node, moved into its space by the node's whole matrix (LCS's bike nodes are rotated: the Faggio's forks tilted 0.1 rad, chassis_vlo turned 90 degrees) |
| an SA car's own wheel | elements on the car's own wheel_*_dummy nodes (linked as the stock ones: node object list +8, element group list +8 / +0xC with element +0x1C, a render-callback field in the global table), the wheel scaled to 1 m, model info +0x40 (shared wheel id) set to -1 as LCS's bikes have it: other cars keep wheel 254 |
| a second mod on a slot already served: "stock model could not be read" | the stock entry's sector / size is remembered at the first serve |

LCS's vehicle classes (model info +0x38, read in a run): 0 car, 1 boat (Ghost, Speeder, Reefer, Predator: nodes
`boat_hi`, `boat_vlo`, `boat_lights`, Predator also `boat_moving_hi`; a boat mod's `boat_hi` / `boat_vlo` take the
body / far model, other parts by node name; no boat mod was tried), 2 train, 3 heli (two script-only helicopters),
4 plane (the airliner), 5 bike. The Dodo (164), Hunter (213), Maverick (214), Police / VCN Maverick (215 / 216) are VC
leftovers with class 0 and car nodes (doors, chassis, wheel dummies; shared wheel 256 / 257 is used by no real car):
the trainer spawns them, they stand on the ground and drive as cars. The manager says so for a plane / heli mod.
Shared wheel models: 250 sports (Banshee, Stinger, Infernus ...), 251 (Kuruma, Taxi, Manana ...), 252 (Blista, Pony
...), 253 (Cabbie, Stallion, Idaho ...), 254 (Sentinel, Landstalker, Leone Sentinel ...), 255 trucks (Bus, Linerunner
...); bikes -1.

Mod kinds covered by these fixes, beyond the eight: any III / VC / SA car or bike `.dff` + `.txd` (SA 3.6 files with
extra UV sets, night colours, 2dfx and collision plugins are read: the extra parts are ignored), modloader folders,
nested archives, weapon packs in one clump, weapon-loader `.xml`, readme-only installs (lines in the readme), IDE /
carcols / handling lines anywhere in text files. Untested: boats, txd-only retextures (a `.txd` without a model is
noted, not used), wheel packs (noted, not used: they would change every car), IMG-based installs (`.img` noted),
SA cars whose 2dfx lights / night colours matter, peds.

### Sound mods (2026-11-18)

Both live in the native sound driver (`src/game/lcs_sound.cpp`, the host port of CDSTREAM.IRX; offsets below are
the module's), with the work in new files: `lcs_usertracks.cpp` (user tracks), `lcs_sfxmod.cpp` (the `sound`
directive), `lcs_audiofile.cpp` (decoding, resampling, PS2 ADPCM encoding). Decoders are vendored single files in
`third_party/audio_decoders`: `dr_wav.h`, `dr_mp3.h`, `dr_flac.h` (github.com/mackron/dr_libs, public domain or
MIT-0) and `stb_vorbis.c` (github.com/nothings/stb, public domain or MIT); their licence texts are at the end of each
file. Not on VCS (its driver, `vcs_sound.cpp`, is untouched).

**User tracks** (Audio > User tracks, `audio.user_tracks`; `VCS_USER_TRACKS`: 0 off, 1-10 station 0-9, 11 every
station; default LCFR). How LCS plays a station: the game opens music file `n` (server 0x65686578 command 0x80200 |
n, the argument its position in ms) from the module's name table at 0x5884 (40 bytes apart): 0 HEAD, 1 DOUBLE, 2 KJAH,
3 RISE, 4 LIPS, 5 MUNDO, 6 MSX, 7 FLASH, 8 LCJ, 9 LCFR (`\AUDIO\MUSIC\*.VB`); 10-24 ambience / police / taxi, 25-38
`NEWS_A..N`, 39-62 cutscenes. The music thread (0x22E4) reads the file 256 KB at a time into two buffers and feeds
core 1 voices 22 / 23 (32 kHz: pitch 0xAAA set by 0x1D14) through 16 KB rings at SPU2 0x5010 / 0x9010 (module data
0x6AB0 / 0x6AB4), 8 KB per ring IRQ. A .VB is headerless PS2 ADPCM in 16 KB pairs: 8 KB left, 8 KB right. The seam is
the stream's disc read (`readStream`): while the chosen station's stream is open, its bytes come from the player's
files, decoded on a thread (~24 s ahead), resampled to 32 kHz stereo and ADPCM-encoded in the same layout; file sizes,
positions, the wheel, volume and mono stay the game's. What the stream had read but not yet sent to the SPU2 when it
stops is given back, so the music goes on where it was last heard. Files: `<data>/music` (and its subfolders),
shuffled (`VCS_USER_TRACKS_SHUFFLE=0`: by name), read again after each round; one that won't decode is skipped and
logged (`[usertracks] skipped ...`). Not done: an extra wheel position (the wheel's station count is in the game's
code), a track list / skip button, per-track names on the HUD. The news bulletins the game plays between stations'
stretches (file 33 after a car is entered, seen in the runs) are their own files and stay.

**Effect sounds** (`sound id=<id> file=<audio>` | `sound bank=<b> index=<i> file=` | `sound name=<prefix> file=`,
optional `gain=<percent>`; `tools/mods.py sound FILE --id N` / `--sound-name PREFIX --iso ISO` makes the mod,
`tools/mods.py sounds ISO [--find TEXT]` lists every id). The effects are 67 banks, `\AUDIO\SETn\SFXm_PS.RAW` + `.SDT`
(12 bytes a sound: offset in the .RAW, bytes including a 48-byte VAG header, rate). Sound init (0x43C0) puts every .SDT
into one table of 0x161D sounds, ids global (bank m's first id: the constants of 0x3704, `kBankFirstSound`); the loader
(0x4BD8) copies a bank whole into SPU2 RAM at its address (module data 0x6B48: banks 0-1 0x78810, 2 (speech, sound by
sound into slots) 0x2F810, 3-0x27 (3 the menu's sounds, 4-39 the per-vehicle engines; one shared area) 0xD010, 0x28-0x3C 0x26810, 0x3D-0x42 0x68810)
in 0x20800-byte chunks; a voice plays id at bank address + offset + 0x30. The VAG header names the sound at +0x20
(16 characters, e.g. `SFX_COLT45_LEFT.`). The override writes the new sound's ADPCM over the slot in each chunk on its
way to SPU2 RAM (every load of the bank), so the table and addresses stay the game's: decoded, mixed to mono,
resampled to the slot's own rate (the game plays a sound at the table's rate with a small random pitch change: pistol
key-ons at 21744-22733 Hz for its 22050), a leading silent block as the game's, the slot's ending (loop flags 6 / 2 /
3 when the original loops, flag 1 for a one-shot). Longer than the slot -> cut to it (logged); shorter -> the table's
byte count is lowered (the 0xA0B00 length reply). Speech (bank 2) is refused. `VCS_SFX_LOG=1` logs every key-on.

Ids (from the disc's names and key-ons in runs): pistol 99 / 100 (`SFX_COLT45_LEFT` / `RIGHT`, both played per shot,
22050 Hz, 0.92 s), .357 252 / 253 (`SFX_PYTHON_*`), shotgun 275 / 276 (`SFX_SHOTGUN_*`), combat shotgun 284 / 285
(`SFX_SPAS12_*`), AK-47 1 / 3, M60 216 / 217, MP5 225 / 226, Uzi 305 / 306, sniper 281 / 282, rocket 266 / 268, car
horns 43-50 (`SFX_CAR_HORN_56C`, `BMW`, `BUS`, `BUS`, `JEE`, `PIC`, `POR`, `TRU`), police siren 251, idle / rev engine
loops 51-69 / 76-94 (by engine type: `SFX_CAR_IDLE_CAD`... / `SFX_CAR_REV_CADI`...), the per-car engine banks 4-39
(`SFX_CAR_ACCEL...`, ids 5523 on).

**Checked (Windows, hidden runs with `PS2X_AUDIO=1`, `PS2X_AUDIO_DUMP` recordings, 2026-11-18; agent mods3-audio,
`agents\mods3\audio\run`)**, test files made by a script (tones: a 44.1 kHz stereo WAV 440 Hz left / 660 Hz right, a
22.05 kHz mono WAV 880 Hz, a 48 kHz stereo FLAC 523 / 784 Hz, a 0.25 s 1200 Hz WAV beep), the free-roam card, a
Kuruma spawned:
- t2b (default setting, the car tuned LCFR): `[usertracks] station 9 (LCFR) tuned`; the recording has 440 Hz on the
  left and 660 Hz on the right at full strength (Goertzel 2200 each, the other channel's tone < 20) for the 15 s until
  the game's news bulletin (file 33) stopped the station.
- t6 (`VCS_USER_TRACKS=11`, the car tuned Lips 106): the 30 s WAV from 13 to 27 s of the recording, the game's news
  bulletin, then the WAV again from where it stopped (61-73 s: the rest of its 30 s, nothing skipped), then the FLAC
  (74-92 s, 523 / 784 Hz at full strength, the other tones < 80).
- t5 / t2b (`sound name=SFX_COLT45 file=beep.wav` / none), 8 pistol shots: `[sfxmod] sound 99 ... sound 100 ... <-
  beep: 22050 Hz, 0.25 s`, 16 key-ons of 99 / 100; the strongest 1200 Hz in the shots' 14 s: 2326 with the mod, 175
  without (a control frequency, 1500 Hz: 311 / 275).
- MP3 and Ogg files were not played in a run (no encoder at hand to make a test file); the decoders are the upstream
  libraries' unchanged.


### What converts from PC mods, and how it was checked (2026-11-18, agent mods3)

The rules, game by game and field by field: docs/ANDROID.md "Mods", "What converts". LCS's side:

| What | Where in LCS (evidence) |
|---|---|
| handling flags | record +0xD0, VC's bit layout: read from all 81 records in a run (bikes 0x10000 IS_BIKE, helicopters 0x20000, boats 0x80000 (+0x800000 SIT_IN_BOAT on the Speeder), vans 0x20, the Coach 0x40 IS_BUS, the Fire truck 0x4404, the Cheetah 0x01008181, the BF Injection 0x0700a918); 0x8000000 / 0x10000000 are LCS's own (Hunter / Maverick, Stinger / Idaho / FBI car ...) and stay |
| light types | record +0xDC front, +0xDD rear (LANDSTK 0 / 1, PONY 0 / 3, BFINJC 1 / 2) |
| ABS | record +0xA4, a byte (0 in all 81) |
| fire offset | weapon record +0x30 / +0x34 / +0x38 (the pistol 0.3, 0, 0.09; the record also holds the animation times 11/30, 18/30, 14/30 at +0x44.., model 274 at +0x60, model2 -1, slot 3 at +0x68, flags 0x680C0 at +0x6C) |
| car positions | model info +0x50 + k x 0x10: headlights, tail lights, front seat, back seat, exhaust (the Kuruma's 0.645 2.408 -0.186 / 0.821 -2.438 -0.080 / 0.401 0.149 -0.276 / 0.401 -0.824 -0.321 / 0.552 -2.637 -0.557; a bus's back seat 0) |
| wheel scale | model info +0x3C (Kuruma 0.7, Bus 0.9, PCJ-600 0.67), the wheel model +0x40 (int16, Kuruma 251), handling id +0x42 |
| colour list | model info +0x170 8 primary, +0x178 8 secondary palette indices, +0x180 the count (Kuruma 8, Bus 7); a model info is 0x190 bytes |
| car nodes | the Kuruma's: kuruma, chassis_dummy, chassis_vlo (+ its light parts), chassis_hi, steerwheel_dummy, windscreen_dummy, bump_rear_dummy, boot_dummy, door_rr / lr / rf / lf_dummy, wing_rf / lf_dummy, bonnet_dummy, bump_front_dummy, brakelights, wheel_rb / lb / rf / lf_dummy; 20 elements (an intact and a damaged one per part) |

Checked in hidden runs (`agents\mods3\run`; every model and line made from scratch, in the PC games' formats):
- an SA-format handling line on the Kuruma (h2_sa): 90.3 m / 184.6 km/h after 3 s against the stock 36.9 m / 75.3
  km/h (h1_stock); the record read back: flags 0x8002 -> 0x1208180 (SA IS_LOW, DBL_EXHAUST, WHEEL_R_WIDE,
  HALOGEN_LIGHTS, PROC_REARWHEEL_1ST mapped; FORCE_GROUND_CLEARANCE reported), lights 01 01, collision 1.0286.
- a box car with VC part names (c1..c4) and one with SA names (c5sa) in the Kuruma slot: 16 of the slot's 20 parts
  from the mod (the 4-door slot's rear doors empty), 11 nodes moved, 5 positions and the wheel scale 0.84 written; the
  parts drawn where the mod puts them (c3_e_exit1_270: wheels at the mod's wheelbase), the door swings open as Toni
  gets in / out (c2_d_enter5, c5sa_e_exit4), the front wheels steer both ways (c4_b_steerL / c_steerR, the
  look-behind view), the boot swapped to its damaged part later in the same run (c2_a_ahead -> c2_d_enter5; which knock damaged it was not identified, and the drop test d1 flipped the car without breaking a part), the paint
  takes the car's colours (white in c2, beige in c3).
- the same car through `tools/mods.py import` with a `carcols.dat` that has its own colour list (m1): three spawns
  picked the mod's pairs (vehicle +0x1F0: 03 61, 06 00, 02 61), drawn red / white and yellow / black (m1_car1.png).
- weapons (w1..w3): the box pistol fires (183-16), the HUD icon stays; `firex 0.15 firez 0.04` read back in both
  tables. Cutscenes: their scripts in ANIM/CUTS.IMG name their own objects (cscolt45, cs9mm, cstaxi ...), so
  converted weapons and cars are not seen there.
- Not converted (reported in the entry): the mod's own wheel geometry, extras, collision, carcols numbers without a
  colour list, SA's other skill levels, animations, the model and weapon flags of weapon.dat.

## Police cars and pressure-sensitive buttons (2026-11-18)

- LCS calls scePadEnterPressMode on both ports. Setting pad.pressure (Gamepad page, default on): LT / RT report their travel as L2 / R2 pressure (data[8..19] of the pad status; keyboard and digital buttons stay 255). pad.pedals (default off): in a vehicle RT / LT press Cross / Square with that pressure. Measured in a run (Blista from rest, 1.2 s, 5 trials): the same distance at 30% and 100% - LCS's vehicles are not throttle-proportional, so this has no driving effect.
- Police car (model 157, trainer spawn 157): body is already dark blue with a red roof light bar; vehicle colour bytes are at vehicle+0x1F0 / +0x1F1 (palette indices; trainer paint diff). CARCOLS.DAT is not a loose disc file. Blue police cars / lights: not done.

- The siren is CVehicle::PreRender FUN_0012FD58 (model 0x9D = Police, 0x9E, 0x93, 0x8A ... each with its own set at the `uVar1 == 0x9d` branch): vehicle +0x2AD = siren on (poke 1 in a run turns it on); 4 coronas per car, phase `(time + i * 0x40) >> 8 & 3`: phase 0 = RED (255, 0, 0, 255, hard-coded in the FUN_00259710 call), phase 2 = the model's colour, which for the Police is (0, 0, 255) = BLUE, other phases off; and a dynamic light (FUN_002B2638) in the same colours. So the PS2 police lights already flash red and blue; a "Red & blue" option would be the original (verified: a screenshot with the siren on shows the red and blue coronas).
- The body colour is not from the palette for the police: vehicle +0x1F0 / +0x1F1 are palette indices (u8 RGBA x 101 at *(0x3D7388) + 4: 0 (15,15,15), 1 (202,202,202), 79 (14,49,109)); the police car's are 0 / 1 but poking 7 / 7 or 6 / 6 (yellow) changes nothing: its black body is texture (FUN_002B9248 only writes the materials in the model's colour lists, empty for the police). A blue body needs the texture (the car's TXD raster palette) changed: not done.
- Pressure: LCS enters press mode (scePadEnterPressMode, ports 0 and 1) but no throttle difference at 30% vs 100% in 5 trials, and the one reader of pad bytes 8..15 found by a code search was an Adler-32. Nothing in LCS was found to use pressure.

