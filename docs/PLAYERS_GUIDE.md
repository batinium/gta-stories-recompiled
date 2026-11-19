# Player's guide

Everything about installing and running GTA Stories Recompiled, in detail. The short version is in the
[README](../README.md#get-it).

## What you need

You need **your own PS2 disc image** (`.iso`) of **Liberty City Stories, USA, SLUS-21423** (for Vice City Stories on
Android: USA, SLUS-21590, v1.00). Nothing from the game is in this repository or in any release: the game's code is
built on your computer from your disc, and the game streams everything else from the `.iso`. PSP versions can't work
(a different game build for different hardware).

## Windows, macOS, Linux

1. Download the release for your system from [Releases](https://github.com/batinium/gta-stories-recompiled/releases):
   `GTA-Stories-Recompiled-<version>-windows.zip` (unzip it into a short folder, e.g. `C:\Games\GTAStories`: not in
   Downloads, OneDrive or Program Files; the game is built inside that folder and needs about 4 GB there),
   `-macos-arm64.dmg` (Apple Silicon; drag the app to Applications; the first start needs *System Settings > Privacy &
   Security > Open Anyway*) or `-x86_64.AppImage` (glibc 2.35+: Ubuntu 22.04, Debian 12 and newer).
2. Start **GTAStoriesLauncher** (Windows: `GTAStoriesLauncher.exe` in the unzipped
   `GTA-Stories-Recompiled-<version>-windows` folder; it isn't signed, so if Windows says *Windows protected your PC*,
   choose *More info > Run anyway*). It opens on Liberty City Stories.
3. Choose or drop your `.iso`, press **Build the game** (the first time 5 to 60 minutes, depending on the PC; on
   Windows the compiler, CMake and Ninja download themselves, about 250 MB once, so be online; the launcher lists the packages to install
   on Linux and macOS), then **Play**. The game reads the `.iso` while you play: leave it where it was chosen.

*Game settings* on the launcher's Home sets draw distance, resolution, frame rate and the rest before you play; in the
game **F2** opens the settings, **F5** the trainer, **F6** the free camera. The launcher updates itself from Releases.

**GTA Stories (both cities, Windows, experimental).** With both discs chosen (Liberty City Stories and Vice City
Stories) and both games built, the launcher's third tab, **GTA Stories**, plays them together as one world: cross by
the bridge, by boat or by air, with your car, money, health, armour and weapons. Vic Vance is the hero in both cities
(or Toni), Vice City Stories' vehicles can be spawned in Liberty City, and one set of settings covers both. The extra
packs it needs are made from your discs automatically before **Play**. Details and limits: [STORIES.md](STORIES.md).

From source instead: `git clone https://github.com/batinium/gta-stories-recompiled`, then `python tools/build.py --launcher`
and run `GTAStoriesLauncher`, or without the launcher
`python tools/build.py --game lcs --recompile "<your LCS .iso>"` and `build/lcs/lcs` (Windows: `build\lcs\lcs.exe`).
Always say `--game lcs`: plain `tools/build.py` builds Vice City Stories. More: [LAUNCHER.md](LAUNCHER.md),
[LCS.md](LCS.md), [LINUX.md](LINUX.md), [MACOS.md](MACOS.md).

## Android

Android 13+, ARM64, a GPU with Vulkan 1.3 (recent Adreno GPUs; others untested), about 3 GB free per game. One app,
**GTA Stories Recompiled**, plays both games (Liberty City Stories first, Vice City Stories one tap away).

1. Install the APK from [Releases](https://github.com/batinium/gta-stories-recompiled/releases) (no game inside it).
2. Copy your `.iso` to the phone. In the app: pick the game in the header, then *Disc > Choose disc image…*.
3. **Easiest (no PC, experimental):** *Disc > Build code pack on this phone (experimental)…*. The app builds the
   game's **code pack** (`libvcs_game.so`) from your disc, on the phone: 5-30 minutes, ~1 GB free while it runs, keep
   it charging. Then *Play*.
4. **Or on a PC** (the tried path, faster): build the code pack once from your disc (step 2 below), copy it to the phone
   and *Disc > Import game library (code pack)…*.

#### Build on the phone (experimental)

*Disc > Build code pack on this phone (experimental)…* (needs that game's disc chosen first):

- The first time it downloads a compiler once (~70 MB): Termux's official clang/LLVM packages from
  packages-cf.termux.dev, checked against SHA-256 sums inside the app. The dialog lists them and their licences
  (LLVM: Apache-2.0 with LLVM exception; libiconv LGPL-2.1, zlib, zstd, libxml2, libffi, xz under permissive
  licences). Nothing of the game is downloaded and nothing from your disc leaves the phone.
- Then it reads the game's executable from your `.iso`, recompiles it to C++ and compiles that, all in the app's
  private folder; a notification shows the progress (*Cancel* there). The screen may turn off. If the phone gets too
  hot it pauses; if the app is stopped, tap the row again: it goes on where it was.
- At the end it checks and installs the pack exactly like an import, deletes everything else, and *Play* is ready.
  The compiler (~220 MB) stays for the other game or the next update; *Disc > Delete the code pack compiler* removes it.
- Needs a phone with 6 GB+ RAM. If it fails, the row shows why; the PC build (below) is the fallback.


### Android, step by step

Three pieces end up on your phone: **the app** (the APK from Releases: no game in it), **your code pack** (the game
library `libvcs_game.so`: a game's code, recompiled from your disc on your PC; one per game) and **your disc image**
(`.iso`, one per game).

### 1. Install the app

1. Download the APK from [Releases](https://github.com/batinium/gta-stories-recompiled/releases). Each release lists the
   APK's `sha256:`; you can compare it (`certutil -hashfile <apk> SHA256` on Windows, `shasum -a 256 <apk>` on macOS,
   `sha256sum <apk>` on Linux).
2. Open it on the phone and allow installing from that app (Android asks once). Or from a PC: `adb install <apk>`.
3. Had you installed a build you made yourself before? Release APKs are signed with a different key: export your saves
   (*Saves > Export as .zip…*), uninstall the old app once, install the release, then import the saves again.

The launcher has *Play*, *Disc*, *Saves*, *Settings*, *Controls*, *Frame generation* and *About*, and a switch in the
header: **Liberty City Stories | Vice City Stories**. *Play > Test graphics* checks Vulkan, touch and the screen without
the game.

### 2. Build your code pack on a PC

(Skip this if you built it on the phone: *Build on the phone (experimental)* above.)

Windows, macOS (Apple Silicon) or Linux (x86-64), **once per game**; after an app update you only redo it when the
release notes (or the app) say the game library must be rebuilt.

**Install (once):**

| What | Windows | macOS | Linux |
|---|---|---|---|
| Python 3 | [python.org](https://www.python.org) (tick "Add to PATH") | comes with the Command Line Tools | your distribution's `python3` |
| git | [git-scm.com](https://git-scm.com) | comes with the Command Line Tools | `git` |
| A compiler for the recompiler | nothing: downloaded by the build (llvm-mingw) | Apple's Command Line Tools: `xcode-select --install` | clang 19+ and lld, plus the packages `python3 tools/build.py --check-deps` lists ([LINUX.md](LINUX.md)) |
| glslangValidator | the [Vulkan SDK](https://vulkan.lunarg.com) | `brew install glslang`, or none: the recompile step builds one | `glslang-tools` |
| JDK 17 (for the SDK's `sdkmanager`) | [Adoptium Temurin 17](https://adoptium.net) | the same, or `brew install openjdk@17` | `openjdk-17-jdk` |
| The Android SDK with **NDK 28.2.13676358** and **CMake 3.31.5** | see below | see below | see below |

The Android SDK: either [Android Studio](https://developer.android.com/studio) (*SDK Manager > SDK Tools*, tick *Show
Package Details*, pick NDK 28.2.13676358 and CMake 3.31.5), or only the command-line tools, then
`sdkmanager "ndk;28.2.13676358" "cmake;3.31.5"`. The SDK is found in `ANDROID_HOME` or Android Studio's usual folder
(`%LOCALAPPDATA%\Android\Sdk`, `~/Library/Android/sdk`, `~/Android/Sdk`).

**Build** (use `python` or `py` instead of `python3` on Windows):

```
git clone --branch <the release's tag, e.g. android-v0.3.0-1> https://github.com/batinium/gta-stories-recompiled
cd gta-stories-recompiled

# Liberty City Stories
python3 tools/recompile.py --game lcs --out lcs-generated "path/to/Grand Theft Auto - Liberty City Stories (USA).iso"
python3 tools/android.py build-game --game lcs --generated lcs-generated/ee

# Vice City Stories (if you want it too)
python3 tools/recompile.py "path/to/Grand Theft Auto - Vice City Stories (USA).iso"
python3 tools/android.py build-game --game vcs
```

- `recompile.py` reads the game's executable from your disc and translates it into C++. It checks that the disc is the
  right one.
- `build-game` compiles that C++ for Android: `build/android/game-lcs/libvcs_game.so` (Liberty City Stories) or
  `build/android/game/libvcs_game.so` (Vice City Stories), plus a `.so.gz` beside it (about a third of the size). It is
  made from your disc: keep it for yourself, don't share it.
- Use the source of the **same release as your APK** (the tag above): a code pack built from other source may not
  match the app, and the app then says so instead of starting.
- On Windows, clone into a short folder (e.g. `C:\gta`): the build's paths can pass Windows' 260-character limit.
- It takes a while the first time (thousands of generated files); later runs only redo what changed.

### 3. Copy to the phone

Copy two files per game to the phone, e.g. into *Download* (USB file transfer, or `adb push <file> /sdcard/Download/`):
the game's `libvcs_game.so` (or the `.so.gz`) and that game's `.iso`.

### 4. Import and pick the disc

In the app, pick the game in the header, then under **Disc**:

1. **Choose disc image…**: pick your `.iso`. The app checks it and reads it where it is (it is not copied); a disc of the
   other game switches to that game.
2. **Import game library (code pack)…**: pick `libvcs_game.so` (or the `.so.gz`, or a `.zip` holding it). The app checks
   that it matches this version and copies it into its private folder (you can delete the copy in *Download*).

### 5. Play

**Play > Play** shows *✓ Ready* when the disc and the code pack are ready. A good start: *Settings > Graphics preset >
Balanced* (the screen's shape, 2x resolution, 60 fps). In the game, **Back** opens the side panel (pause, settings,
quick save, quit to the launcher). If the app says the code pack was built for another version: build it again from
the source of your APK's release (step 2) and import it again.
