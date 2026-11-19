# Android

The Android port of this fork (`batinium/gta-stories-recompiled`, branch `android`): the same source, SDL3 platform
layer, Vulkan backend and generated game code as the Linux build, as an APK for arm64 phones with Vulkan 1.3.
Everything Android-only is marked `[android]` in the code.

**Players**: the step-by-step guide (install the APK, build your game library on a PC, import it, play) is the
[Player's guide](PLAYERS_GUIDE.md#android-step-by-step). This file is the design record and the developer's notes.

Status: **playable on the test device**. The APK holds no game code: the game is the player's own library
(`libvcs_game.so`, built from their disc), loaded at start ("Runtime / game split" below). The personal build (the game
inside the APK) still exists for development. Without a game library the APK runs the smoke test
(`src/android/smoke_main.cpp`, launcher "Test graphics"): the platform layer, the Vulkan RHI, the presenter and Dear
ImGui on the device, with a moving test picture.

## Distribution: bring your own disc, like the PC

The published APK never contains game code or data, and neither does the repository:

1. **The APK** is the runtime shell: SDL's Java activity, the launcher, the PS2Recomp runtime and our code
   (`libmain.so`), built without `generated/` (`python3 tools/android.py build --release`; a release machine needs no
   disc at all). Safe to share; it is the file attached to a release, signed with the release key ("Release signing").
2. **The game library** (`libvcs_game.so`: the generated C++ from the player's own SLUS_215.90 and its compiled VU1
   code, built with the NDK) is built by the player on their PC with two commands, `python3 tools/recompile.py <iso>`
   then `python3 tools/android.py build-game`, copied to the phone and imported in the launcher (*Disc > Import game
   library…*) into the app's private folder, read-only. `libmain.so` loads it before the game's main runs. It must be
   built from the source of the APK's version (or any with the same `VCS_GAME_ABI`, "Runtime / game split").
3. **The disc** is the player's `.iso`, picked in the launcher and read in place.

The **personal build** (`build --personal`, the game compiled into the APK) is for the developer's own device only:
such an APK contains code from the disc and is never shared or attached anywhere. `tools/prepublish_check.py --apk
<file>` refuses one.

Later, perhaps: the whole build on the phone (clang + lld shipped as `lib*.so`, run from the native library folder).

Not for Google Play: its policy forbids loading executable code from outside the APK. Sideload only.

## Building

**The player's game library** (Windows, macOS or Linux; the README's Player's guide has the details):

```
python3 tools/recompile.py "<your disc>.iso"   # once: the disc's executable -> generated/ (C++), with a host compiler
python3 tools/android.py build-game            # generated/ -> build/android/game/libvcs_game.so, with the NDK
```

`tools/build.py --recompile <iso>` makes the same `generated/` and also builds the PC game (longer; not needed here).
`build-game` configures its own tree (`build/android/game/cmake`) with the SDK's CMake and Ninja and the NDK's
toolchain file, builds `libmain.so` there to link against, and strips the library. Rebuild it (both commands; the
recompile only when its inputs changed: `tools/recompile.py --check`) when an app update changes `VCS_GAME_ABI`.

**The APK and development**:

```
python3 tools/android.py build            # the APK (runtime only, debug key): SDL's Java sources (the release src/platform pins), Gradle, the NDK
python3 tools/android.py build --release  # the same, signed with the release key ("Release signing"): the published APK
python3 tools/android.py install          # adb install --user 0 (the phone's own user, never a work profile)
python3 tools/android.py run              # start it and follow the log (adb logcat -s vcs SDL ...)
python3 tools/android.py all
python3 tools/android.py push-game        # (development) the game library into the app's folder over adb, as an import leaves it
python3 tools/android.py build --personal # the personal build: the game inside the APK (your own device only, never share it)
python3 tools/prepublish_check.py         # before a push or release: no game code, private files or home paths tracked
```

Needs the Android SDK with NDK 28.2.13676358 and CMake 3.31.5 (`sdkmanager "ndk;28.2.13676358" "cmake;3.31.5"`), git
and glslangValidator; the APK also needs a JDK 17+ (`build-game` doesn't) and a POSIX sh for FFmpeg's configure (the
movies; Windows: Git for Windows' sh; "Movies (FFmpeg)" below). Gradle 8.14.3 comes through the wrapper
(`android/gradlew`); outputs go to `build/android/` (Gradle) and `build/android/cxx/` (CMake).

**Windows**: the same commands (`py` or `python` for `python3`). `tools/android.py` finds the SDK in `ANDROID_HOME`,
`ANDROID_SDK_ROOT` or Android Studio's `%LOCALAPPDATA%\Android\Sdk`, uses the `.exe` tools of the SDK's CMake package
(its `ninja.exe` too) and the NDK's `windows-x86_64` prebuilts, runs `gradlew.bat`, and passes paths to CMake with
forward slashes. glslangValidator: the LunarG Vulkan SDK (its installer sets `VULKAN_SDK`; `Bin\glslangValidator.exe`
is found from it) or `VCS_GLSLANG=<path>`. `tools/recompile.py` builds the recompiler with llvm-mingw, downloaded once
into `build/build-tools` (upstream's toolchain, `tools/build.py`'s docstring), so no Visual Studio is needed. Keep the
clone in a short folder (e.g. `C:\vcs`): the build tree's object paths can pass Windows' 260-character limit.
Not yet run on a Windows PC (2026-11-17: read through and the path handling fixed; macOS builds).

- **Libraries**: `libmain.so` (SDL3, the runtime, our code) and `libsmoke.so`, from the repository's own
  `CMakeLists.txt` (the NDK's toolchain; `VCS_BUILD_TOOLS` is off: the recompiler is a host tool), and
  `libc++_shared.so` (`ANDROID_STL=c++_shared`: one C++ runtime for `libmain.so` and the game library beside it). SDL3
  is linked statically and whole (its JNI entry points are only called from Java). The `android/app/build.gradle` passes `FETCHCONTENT_SOURCE_DIR_SDL3`, so CMake and Gradle use the one SDL
  checkout (`build/android/SDL-<tag>`), the C and Java sides of the same release.
- **Logging**: stdout and stderr are piped into logcat under the tag `vcs` (`src/android/android_log.cpp`): the port
  logs with `fprintf(stderr, ...)` everywhere.
- **Calling convention**: `PS2_GUEST_CC` (`preserve_none`) is x86-64 only; arm64 uses the default one, as on macOS.
  The NDK's clang 19 has no `preserve_none` for AArch64, and doesn't need it.
- **SSE**: sse2neon with the precise flags, as on macOS (docs/MACOS.md).
- **Movies (FFmpeg)** (2026-11-18): the boot movies (VCS's STINGS / INTRO.PSS, LCS's RSNY / RSLEEDS / RSNORTH /
  DM.PSS) go through `src/game/vcs_movie.cpp`'s player (`lcs_movie.cpp` for LCS): its own PSS demux, the MPEG-2 video
  decoded by FFmpeg into RGBA (swscale), the sound PCM (SShd type 1, 48 kHz stereo) into the audio output without
  FFmpeg. The runtime's IPU path (`MPEG.cpp`) uses the same decoder. So `libmain.so` gets the macOS recipe
  (`third_party/PS2Recomp/ps2xRuntime/CMakeLists.txt`): FFmpeg 7.1.2's official source release (SHA-256 pinned), only
  the `mpeg2video` decoder, the `mpegvideo` parser and swscale; static, LGPL 2.1+, `--disable-network`,
  `--disable-autodetect` (no hwaccel, no system libraries), no programs / avformat / swresample. Cross-built with the
  NDK's clang (`--target=aarch64-linux-android33`, NEON on) by `ExternalProject` in its source tree, installed once
  into `build/android/ffmpeg/ffmpeg-7.1.2-arm64-v8a-<options hash>/` (with the tarball; `.complete` marks a finished
  install) and imported from there by every later build tree (Gradle's `-DPS2X_FFMPEG_CACHE_DIR`, `-PvcsFfmpegCache=`
  to move it); delete the folder to rebuild. FFmpeg's `configure` needs a POSIX sh: on Windows hosts Git for Windows'
  `usr/bin/sh.exe` (found next to `git`, also put first on PATH for the build), elsewhere `sh`; the NDK's own `make`
  builds it on every host. Windows host (24 threads): configure ~9 min (thousands of forks under Git's sh), make ~35 s;
  once. Without a sh CMake warns and builds without FFmpeg (stub frames, as before). `build-game` passes
  `-DPS2X_ENABLE_FFMPEG=OFF`: its `libmain.so` is only linked against. Size: `libmain.so` +1.5 MB (8.9 -> 10.3 MB),
  the runtime APK 15.4 -> 16.9 MB. Checked on the PC (no phone): the same configuration built for x86-64 Windows
  (llvm-mingw; `--disable-x86asm`, no nasm) and a stand-alone copy of the player's demux / decode / swscale loop on
  LCS's disc decoded RSNY.PSS (168 pictures, 512x448, 29.97 fps, the Rockstar logo correct) and DM.PSS (2794 pictures)
  at ~2,400 pictures/s; the same program links against the arm64 build with the NDK. **On the phone, to check**: the
  LCS (and VCS) boot movies show and play to the end in time with the sound (`[movie] ... pictures shown, ... dropped`
  in the log; few dropped), Start / Cross skip them, Display > Skip intro movies still skips, no crash or stall on the
  way to the title screen, frame rate / heat during DM.PSS (93 s).

### Release signing

`python3 tools/android.py build --release` (and `install --release`, `all --release`) makes
`build/android/gradle/outputs/apk/release/app-release.apk`, signed with **your own key**, named by the environment.

Make the key **once** (keytool comes with the JDK; it asks for a password and a name, any name will do):

```
keytool -genkeypair -keystore ~/keys/vcs-release.jks -alias vcs -keyalg RSA -keysize 4096 -validity 10000
```

Then, for every release build:

```
export VCS_SIGN_STORE=~/keys/vcs-release.jks   # the keystore file, outside the repository
export VCS_SIGN_ALIAS=vcs                      # the key in it
export VCS_SIGN_PASS=...                       # the store's password, also the key's (keytool's PKCS12 default: one password)
python3 tools/android.py build --release
```

(PowerShell: `$env:VCS_SIGN_STORE = "$HOME\keys\vcs-release.jks"` and so on.)

- **Keep the key and its password safe, with a backup**: every later release must be signed with the same key, or
  it won't install over the earlier one (Android asks to uninstall first, which deletes the app's data: saves
  included). The in-app updater relies on that too.
- **Debug-signed installs**: an app installed from `build` (the debug key, `~/.android/debug.keystore`) or a personal
  build can't be updated by a release APK. Uninstall it **once** before the first release install (`adb uninstall
  com.batinium.gtastories`, or the phone's app settings); export the saves first (launcher *Saves > Export as .zip…*)
  and import them again after, and import the game library and pick the disc again. After that, release builds
  install over each other.
- No key or password ever lives in the repository (`.gitignore` keeps `*.jks` / `*.keystore` out; nothing in
  `android/` names one). Without the three variables a debug build works as before and a release build stops at once:
  `tools/android.py` names what is missing before the build, and `android/app/build.gradle` fails `preReleaseBuild`
  with the same list when Gradle is run directly. Checked 2026-11-17: without the variables both stop with the list;
  with a throwaway key, `apksigner verify --print-certs` shows it as the signer.
- Before attaching it to a release: `python3 tools/prepublish_check.py --apk <the apk>` (no game library or
  recompiled code inside), and its SHA-256 (`shasum -a 256` / `sha256sum` / PowerShell `Get-FileHash`) for the release
  text's `sha256: <hex>` line, which the in-app updater reads.

## Releases and updates

Releases are published on the fork, **github.com/batinium/gta-stories-recompiled**, and the launcher updates itself
from them (About > *Check for updates*).

**Versions**: `X.Y.Z-N`. `X.Y.Z` is the upstream version the build is based on (`VERSION`), `N` our build on it
(`android/BUILD_NUMBER`: up by one for each release on the same `X.Y.Z`, back to `1` when `VERSION` changes).
`android/app/build.gradle` makes `versionName = X.Y.Z-N` and `versionCode = ((X*100 + Y)*100 + Z)*1000 + N`
(0.3.0-1 is 300001; X up to 999, Y and Z up to 99, N up to 999), the order the updater compares in
(`UpdateVersion.java`). The tag is **`android-vX.Y.Z-N`**. The fork also carries upstream's plain `vX.Y.Z` tags: the
updater ignores them.

**Making a release** (by hand; nothing in the repository creates tags or releases):

1. On `android`, all committed (the build tag must not say `+dirty`). Bump `android/BUILD_NUMBER` (or set it back to
   `1` after a merge that changed `VERSION`) and commit that.
2. Build the release APK, signed with the release key ("Release signing" above):
   `python3 tools/android.py build --release` -> `build/android/gradle/outputs/apk/release/app-release.apk`. Never
   attach a debug build (another key: it won't install over a release, nor a release over it) and **never a personal
   build** (`--personal`: the game is inside it).
3. Check it: `apksigner verify --print-certs` shows the release key; `aapt2 dump badging` (or the launcher's About
   line once installed) shows `versionName` `X.Y.Z-N`.
4. `shasum -a 256 app-release.apk`.
5. Tag `android-vX.Y.Z-N` on that commit, push the tag, and create the GitHub release from it with the APK attached
   (name it `vcs-recomp-android-X.Y.Z-N.apk`; the updater takes the release's first `.apk` asset). The release notes
   start with a few lines for players (the launcher shows the first three, *What's new* up to fourteen) and carry
   these two lines, each on a line of its own:

   ```
   sha256: <the 64 hex digits from step 4>
   game-abi: <VCS_GAME_ABI from src/android/game_abi.h at that commit>
   ```

   `sha256:` is checked against the download (a mismatch is never installed; without the line only the size is
   checked). `game-abi:` tells players before the download whether their game library still fits: a different number
   means they must rebuild it (`tools/android.py build-game` from the release's tag) and import it again. Without it
   the launcher says it can't tell.
6. A test build for a few people: mark the release **pre-release**: the updater skips pre-releases (and drafts).

**What the launcher does** (`UpdateMenu.java`, rows in About): *Check for updates* sends one unauthenticated
`GET https://api.github.com/repos/batinium/gta-stories-recompiled/releases` with the User-Agent
`vcs-recomp-android-updater` (nothing about the player or the phone), picks the newest release whose tag is
`android-v*` with an `.apk`, not a draft or pre-release, and compares it with the app's `versionName`. No releases:
"no releases yet". GitHub allows 60 such requests an hour per address. *Check at start* (off by default) does the
same once a day when the launcher opens. When a newer one exists, *What's new* (the notes' first lines, the release
page) and *Update* appear. *Update*:

1. Needs "Install unknown apps" allowed for this app: if not, a dialog sends the player to that setting (they turn it on
   themselves) and they choose Update again.
2. Confirms: versions, size, whether the sha256 is checked, and the game library line: it stays (the release's
   `game-abi:` equals the imported library's stamp), or a new one is needed (said before anything is downloaded), or
   none is imported, or this is a personal build (the release is the app alone).
3. Downloads the `.apk` asset (`HttpURLConnection`) into `cache/update/`, checks the size against the release's
   listing and the notes' sha256, then the APK itself: this package, a higher `versionCode`, signed with this app's key
   (`GET_SIGNING_CERTIFICATES`; a rotated key's history counts). Another key: a dialog explains that Android won't
   install it over this app (a debug or own-key build), and how to switch (export saves, uninstall, install the
   release, import saves and the library).
4. Hands it to Android's installer (a `PackageInstaller` session: Android shows its own confirmation). A refusal is
   shown on the row (another key, older version, no space, blocked). On success Android replaces the app and the launcher
   closes: start it again. Settings, saves and the game library stay (the app's data is kept by an update).

`-PvcsUpdateRepo=` (empty) builds an APK without the updater's source (the rows say updates are off);
`-PvcsUpdateRepo=owner/repo` points it at another fork. The permissions this adds: `INTERNET` (used only by the
updater) and `REQUEST_INSTALL_PACKAGES`.

Checks: `python3 tools/android_update_check.py` (plain JVM, no device: tags, order, versionCode, the newest release,
the notes' lines and summary).

## Device requirements

Vulkan 1.3 with dynamic rendering, synchronization2, demote-to-helper, dual-source blend, depth clamp, clip distance and
`VK_KHR_push_descriptor` (`rhi_vulkan.cpp`, pickDevice). Checked 2026-11-17 on an Honor PNM-N49 (Snapdragon SM8850,
Adreno 840, Vulkan 1.4.295): all present, plus `VK_KHR_calibrated_timestamps`.

## The window's surface

Android destroys the activity's native window while it is in the background. The platform layer watches the window
itself (`SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`, `watchNativeWindow` in `platform_sdl3.cpp`): SDL's app events alone
were not enough (2026-11-17, SDL 3.4.18 on the Honor: after Home and back, no `SDL_EVENT_DID_ENTER_FOREGROUND` reached
the loop and the picture stayed black). Gone -> `platform::inBackground()`; back after being gone, or another one ->
`platform::surfaceGeneration()` goes up. The presenter's `beginFrame` then calls `rhi::Device::recreateSurface()`, and
the Vulkan backend drops the swapchain and the surface and makes them again (the swapchain at the next frame's first
use, as after a resize). `VK_ERROR_SURFACE_LOST_KHR` from acquire / present is not reported as a lost device.

**Rotation**: the window is landscape (`SDL_HINT_ORIENTATIONS`: SDL sets the activity's orientation from the window, over
the manifest) and full screen. The panel is portrait, so the surface's `currentTransform` is a rotation while the
swapchain is identity: every acquire / present returns `VK_SUBOPTIMAL_KHR`. That alone no longer makes the swapchain
again (it did, every frame: 627 swapchains in 10 s); the compositor turns the picture. Pre-rotated rendering is on the
work list. (`VCSActivity.applyOrientation` overrides SDL's orientation: a near-square screen either way, a phone-shaped
one landscape unless Portrait mode is on - "Layout editor".)

Checked 2026-11-17 on the Honor (cover screen, 2420x1080): 60 fps, the test picture letterboxed 4:3, Dear ImGui on top
with touch, Home and back: surface generation 1, the picture back.

## Runtime / game split

The default Android build (`VCS_SPLIT` in `CMakeLists.txt`, `PS2X_SPLIT_RUNTIME` in the runtime's) makes two libraries:

- **`libmain.so`** (the APK, `ps2EntryRunner`): SDL3, the PS2Recomp runtime (`ps2_runtime`, `ps2_iop`: linked whole and
  exported, `--whole-archive` / `--export-dynamic`: the game library calls into all of it) and our `src/`. Built
  without `generated/` (an empty folder stands in for it). Checked with `llvm-nm`: no `FUN_` / `fn_` / `vu1c_`
  definitions.
- **`libvcs_game.so`** (`ps2GameLibrary`, `tools/android.py build-game`, `VCS_ANDROID_GAME_LIBRARY=ON`): only
  `generated/ee/*.cpp` (with `register_functions.cpp`), `generated/vu1/vu1_compiled.cpp` and its stamp
  (`src/android/game_library_stamp.cpp`); the same flags, unity batches and precompiled headers as the personal build.
  Linked against `libmain.so` (`DT_NEEDED`): every runtime symbol resolves to it at load. `build-game` configures its
  own tree (`build/android/game/cmake`, the APK's CMake arguments), builds `libmain.so` there to link against, and
  strips the result to `build/android/game/libvcs_game.so`.

**The interface** between them:

- **The function table** stays the game library's (`g_ps2RecompiledFunctionTable*`, defined and filled by the static
  initialiser of `register_functions.cpp`, which is unchanged): the generated code indexes it directly. The runtime's
  code is compiled with `PS2X_SPLIT_RUNTIME`, under which `ps2_runtime.h` turns those names into the fields of
  `g_ps2xGameTable` (a hidden global of `libmain.so`), set by `ps2xAttachGameTable` once the library is loaded
  (`dlsym` of the four table symbols). Before that the table has no slots. `replaceFunction` writes into the game
  library's array, which the generated code reads.
- **Our hooks calling the game's own function** (`src/game/game_functions.h`): `VCS_GAME_FN(FUN_00220c68_0x220c68,
  0x00220C68u)` is the generated function by name when the generated code is linked in (desktop, personal build) and
  `ps2GameFunction<0x00220C68u>` in the split build: `ps2OriginalFunction(address)`, the function the game library
  put at that address. `replaceFunction` keeps the first one it replaces at each slot, so a hook calling its original
  never calls itself. Resolved once per address (a static in the template).
- **The compiled VU1 code** registers itself as before: `vu1_compiled.cpp`'s static initialiser calls
  `vu1RegisterCompiledPieces` (in `libmain.so`) during `dlopen`.
- **Loading** (`src/android/game_main.cpp`, `game_library.cpp`): `SDL_main` reads the library's stamp from the file
  (no game code runs for a refused one), then `dlopen(RTLD_NOW | RTLD_GLOBAL)` and attaches the table, then runs the
  game's main. `VCSActivity` names the file in `VCS_GAME_LIBRARY` (`files/game/libvcs_game.so`, what the launcher's
  import leaves: read-only). A refusal is logged (`[game-library]`), shown in a message box and written to
  `files/game_library_error.txt`, which the launcher shows on its Play row until a new library is imported.

**The version stamp**: `VCS_GAME_ABI` in `src/android/game_abi.h`, a number changed by hand. The game library carries
`vcs-game-library abi=<n> headers=<hash>` in a section of its own (`.vcs_game_stamp`); the APK compiles the number in
(`libmain.so`) and `android/app/build.gradle` reads it from the header into `BuildConfig.VCS_GAME_ABI` for the launcher.
A library with another number (or none) is refused by the launcher (at import and on Play: "Game library built for
another version ... rebuild it with tools/android.py build-game") and by `libmain.so` before loading it. Only the
number decides: players keep their library across app updates for as long as it stays the same. `headers` is a hash
of the runtime headers the generated code includes (`src/android/game_abi.cmake`): a different hash with the same
number only logs a note (a hint to check whether the interface changed), it never refuses.

**When to bump `VCS_GAME_ABI`**: when a change alters what the generated code was compiled against - the list is in
`game_abi.h`: `R5900Context`'s layout, the macros in `ps2_runtime_macros.h`, `PS2Runtime`'s members that the
generated code's inline calls reach and the methods it calls (`dispatchGuestBranch`, `eeCheckpointDue`, ...),
`PS2Memory` / `EeFastClock` inline parts, a syscall or stub removed or changed, the table's symbols, the VU1 pieces'
structures and `VU1Interpreter`'s members reached by `Vu1Compiled`, the recompiler's output, the C++ runtime or the
code generation flags. Never for our own `src/` (hooks, RHI, renderer, Dear ImGui, Android), Java, settings, comments,
or a function / syscall / stub only added to the runtime. **Runtime changes should prefer new state outside
`PS2Runtime` and `R5900Context`** (a global, a side table) over a new member that shifts their layout and forces a bump.

Symbols: default visibility everywhere; the game library binds its own references to itself first (its weak copies of
inline functions), the runtime's to `libmain.so`. Checked 2026-11-17: the game library defines no data that
`libmain.so` also defines (no duplicated static locals of inline functions); `nm` both sides.

## Games: Vice City Stories and Liberty City Stories

The code base runs a second game in bring-up, **GTA: Liberty City Stories** (USA, SLUS-21423, `SYSTEM.CNF` VER 1.01,
executable sha1 `9b021b4c4e42087bb9b374709f26cf67744b75ab`; CMake `VCS_GAME=lcs`, `game/lcs/`, `src/game/lcs_*.cpp`).
On Android **one runtime APK hosts either game**: the game is the game library's, not the APK's.
LCS's own guide (building it on the Mac, its features compared with VCS's, its switches and open issues):
[LCS.md](LCS.md).

- **The table of games**: `src/disc/known_games.h` (id, boot executable, sha1, title; VCS's values are
  `vcs_disc_id.h`'s), the same in `tools/vcs_disc.py` (`KNOWN_GAMES`; `check <iso> --any` accepts either, a result
  names its `game`; without `--any` the PC tools still accept VCS only) and `DiscCheck.java` (`GAMES`). Change all three.
- **The game library names its game**: its stamp is `vcs-game-library abi=<n> headers=<hash> game=<id>`
  (`game_library_stamp.cpp`, `VCS_GAME_ID` = CMake `VCS_GAME`). Adding the field was no interface change (no
  `VCS_GAME_ABI` bump): a stamp without it is VCS's, so libraries built before keep working.
- **The runtime picks the game at run time** (`main.cpp`'s `game()`): in the split runtime from the loaded library
  (`vcs_android::loadedGameId()`, game_library.cpp), elsewhere from CMake `VCS_GAME` as before. That game's ELF is
  staged from the disc and checked by its sha1; the runtime's per-ELF overrides (`applyVcs` / `applyLcs`) then set
  `game::current()`. VCS's hooks in `libmain.so` already call the game by address in the split build
  (`game_functions.h`), so nothing VCS-named is needed for LCS. A disc of the other game stops at `stageElf`
  (exit 4, "SLUS_xxx not found").
- **The launcher has a game switch** (2026-11-18; ANDROID_LAUNCHER.md "The game switch"): *Liberty
  City Stories | Vice City Stories* in the header (tap, Y on a pad, or Play > Game), as the desktop launcher's title
  bar. **LCS is the default** (first, and selected on a fresh install; an existing user's saved choice is kept). **Both games live
  side by side in one app**, each with its own disc (prefs keys, `.lcs` after LCS's), game library
  (`files/game/<id>/libvcs_game.so`), data folder, Saves list and colours; settings (`vcs.ini`), controls, the stats
  overlay and frame generation (one Lossless.dll) are shared. `GameFiles.game` is the chosen game (in a one-game
  personal build `BuildConfig.VCS_GAME`, no switch); `VCSActivity` gets it in its intent (`game`) and sets
  `VCS_GAME_LIBRARY` / `VCS_DATA_DIR` from it. A picked disc goes to its own game's slot (the disc check names the
  game) and an imported library to its stamp's game's folder; either switches to that game. `GameFiles.migrate` moves
  the earlier one-library layout over once (`files/game/libvcs_game.so` to its stamp's folder, an LCS disc kept in
  VCS's keys to LCS's).
- **adb test copies per game**: `Android/data/<app>/files/disc.iso` is VCS's, `disc-lcs.iso` LCS's, used when that
  game has no picked disc.
- **Data per game**: `VCS_DATA_DIR` is `files/userdata` for VCS (as always) and `files/userdata-lcs` for LCS, so a game
  in bring-up never writes to VCS's memory card. `vcs.ini` (settings) is shared. The Saves list follows the chosen game
  (below); Mods are per game: the Mods screen manages the chosen game's `mods` folder.
- **Colours per game** (`Palette.java`, one table for the launcher, the side panel, the stats overlay and the touch
  controls, picked by the game; the launcher crossfades on a switch): VCS's dusk (pink `#FF5AAE`, teal `#3FE0CF` on
  violet `#1A1430`) or LCS's, from its own pause menu and HUD: amber `#F9B035` for the
  primary accent, steel blue `#74AEDA` for the secondary, navy `#0F0F22` glass, near-white `#F4F4F8` text, crimson
  `#AC0000` only in the launcher sky's glow and lifted (`#E5484D`) as the layout editor's outline. Text is always the
  light ink or an accent on the glass, and glass-coloured on an accent fill: WCAG contrast LCS 17.2 (text), 10.2
  (amber), 7.9 (steel blue), 7.0 (error red); VCS 15.8 / 6.2 / 10.8 / 7.1. Never white on amber, never crimson text
  (the desktop LCS menu's dark red was hard to read).

Building (`tools/android.py`; each game its own trees, so the two never rebuild over each other):

```
python3 tools/recompile.py --game lcs --out <dir> "<your LCS iso>"          # once: <dir>/ee (never generated/)
python3 tools/android.py build                                               # the APK: the same for both games
python3 tools/android.py build-game --game lcs --generated <dir>/ee          # build/android/game-lcs/libvcs_game.so
python3 tools/android.py push-game --game lcs                                # (development) LCS's library into the app
python3 tools/android.py build --personal --game lcs --generated <dir>/ee   # (older) a personal LCS-only app: build/android-lcs
python3 tools/android.py install --personal --game lcs
```

`--generated` defaults to `generated/ee` (VCS) and `generated-lcs/ee`. VCS's trees stay where they were
(`build/android/game`, `build/android`). `push-game --game <id>` writes `files/game/<id>/libvcs_game.so`.

**One app, both games** (2026-11-18): the public **runtime APK** (`com.batinium.gtastories`, **"GTA Stories
Recomp"** since 2026-11-18, before "VCS Recomp Android"; app id `com.batinium.gtastories` since 2026-11-19 (the Java package stays `io.github.batinium.vcsrecomp`; the earlier id installs side by side, it is not updated); a `VCS_APP_ID_SUFFIX`
build is "GTA Stories Recompiled (<suffix>)") holds no game code. Each game's code comes as its own **code pack** (the game library `build-game` makes,
`game=<id>` in its stamp), imported in the launcher (Disc > Import game library (code pack)…) next to that game's disc;
the header's switch picks which pack and disc Play uses. The import takes the `.so`, the `.so.gz` that `build-game` writes beside it (about a third of the size: under chat apps' 30 MiB file limit) or a `.zip` holding the `.so`. Packs of both games stay installed side by side. A VCS pack
built before the switch (on the Mac, stamp without `game=`, same `VCS_GAME_ABI` 1) keeps working: it is VCS's, and the
first start moves it from `files/game/libvcs_game.so` to `files/game/vcs/`. A pack is refused only when its
`VCS_GAME_ABI` differs (then rebuild it with `build-game` from this source); none of this work changed the number.
**The runtime APK installs over the existing app ("GTA Stories Recompiled", earlier "VCS Recomp Android") only when signed with the same key** (a debug build:
the same `~/.android/debug.keystore` as the build that is installed); otherwise Android refuses the update
(`INSTALL_FAILED_UPDATE_INCOMPATIBLE`) and uninstalling first deletes the app's data (saves, packs): back them up first
(`run-as` tar, below).
**The earlier separate LCS app** (`--personal --game lcs`: `io.github.batinium.vcsrecomp.lcs`, "LCS Recomp Android",
`applicationIdSuffix '.lcs'`, LCS compiled into `libmain.so`) still builds but is no longer needed. **Moving its LCS
data into the one app** (adb on the phone's main user; both are debug builds, so `run-as` works; quit both apps first;
nothing here deletes the old app's copy):

```
OLD=io.github.batinium.vcsrecomp.lcs; NEW=com.batinium.gtastories
# 1. the memory card and caches (files/userdata-lcs: the same layout in both apps)
adb exec-out run-as $OLD tar -cf - -C files userdata-lcs > lcs-userdata.tar
adb exec-in run-as $NEW sh -c "cd files && tar -xf -" < lcs-userdata.tar
adb shell run-as $NEW ls -R files/userdata-lcs/mc0        # BASLUS-21423GTA30001 with the slots
# 2. a pushed disc: from the old app's Android/data folder to the new one's (start the new app once first, so the
#    folder is its own). A disc picked in the old app's launcher: pick it again in the new one (Disc).
adb shell mv /sdcard/Android/data/$OLD/files/disc-lcs.iso /sdcard/Android/data/$NEW/files/disc-lcs.iso
# 3. LCS's code pack: Disc > Import game library (code pack) (build/android/game-lcs/libvcs_game.so copied to the
#    phone), or python3 tools/android.py push-game --game lcs
# 4. check: open the new app, switch to Liberty City Stories, Saves lists the slots, Play; uninstall the old app only
#    when sure (that deletes its copy)
```

`vcs.ini` is not copied: the one app already has its own (the settings are shared by both games).

Checked on the phone 2026-11-17 (Honor, cover screen): the same debug APK with LCS's library (37 MB, pushed) and
`disc-lcs.iso` played the opening cutscene and reached gameplay (HUD, radar, the first help text) without input; with
VCS's library back (a stamp from before the game field) VCS booted as before. LCS's stats overlay frame rate shows 0:
`vcs::rig::gameFrames()` counted VCS's frames only then (LCS's since `lcs_frame.cpp`).

**LCS's sound and movies** (2026-11-18, desktop; the same code is in `libmain.so`): LCS's IOP modules are VCS's
byte for byte except Rockstar's CDSTREAM.IRX (an earlier build: same three SIF RPC servers, same commands, 67 banks /
65 streams instead of 68 / 112, different speech slots). `src/game/lcs_sound.cpp` is `vcs_sound.cpp`'s native driver
adapted to that build (installed from `applyLcs` via `lcs_audio.cpp`; `VCS_NATIVE_SOUND=0` = the emulated IOP's, the
reference, which also sounds). The boot movies (RSNY / RSLEEDS / RSNORTH / DM.PSS) have VCS's PSS format and go
through `vcs_movie.cpp`'s player (`lcs_movie.cpp`: Play 0x35A020). Checked by replaying the emulated IOP's recorded
RPCs into the native driver (all replies equal, cutscene streams sample-identical) and live dumps. Since the stream
server's fn 3 / 4 are answered on the host too (freeze fix), the emulated IOP is off for LCS as for VCS
(`lcs_overrides.cpp` -> `vcs::retireIop`; `PS2X_IOP=1` = the reference): no IRX module is loaded or run, the
server's init (fn 0) is answered by `vcs_stream.cpp`. Audio > Music / Sound effects volume follow LCS's levels both
ways (`lcs_volume.cpp`: a setting change, in place of a save load's / the defaults' levels, and a step on the pause
menu's Audio page moves the setting; the Sound effects default in an LCS run is LCS's own 96, not VCS's 112).
Unchecked on the phone.

**What's left for LCS on Android** (2026-11-18, after the night's desktop work merged at 3f0d9be: camera / car camera,
widescreen, pool-base + CdStream sync, native sound and movies, draw distance / background pause, settings locks, free
aim + radio wheel + menu mouse). The APK and both game libraries build (NDK, ARM64); nothing of it is checked on the
phone yet.

- **Touch / gamepad equivalents**: the touch camera drag and the gyro arrive as mouse counts, so LCS's mouse camera
  (`lcs_camera` / `lcs_carcam`) should follow them; free aim on a pad / the touch R1 goes through Gamepad > Free aim
  (`pad.free_aim`, shared `vcs_input.cpp` path). The radio wheel, scope zoom and the pause map's zoom are mouse-wheel
  only (no touch control); the pause menu's pointer: see "Tonight's later LCS features on touch" below.
- **Controls' layout state** (`side_panel.cpp` `noteGameState`): aiming, the pause menu and (2026-11-18) "in a
  vehicle" ask LCS's own modules on LCS (also the gyro's "While aiming"). In a vehicle and the free camera go through
  `game::trainer` (`trainer_select.h`): LCS's trainer's `inVehicle`, set at its frame on the game thread; the side
  panel's Free camera row toggles LCS's too. Unchecked on the phone: the pedals / LOCK should replace Cross / Square
  in a car.
- **Side panel, stats overlay, heat step-down on LCS** (fixed 2026-11-18): `main.cpp`'s per-frame UI hook returned
  early for LCS before `vcs_android::panelHostFrame` / `noteGameFrames` / `coolingHostFrame`, so on LCS the side panel
  never became ready (its settings, trainer and save rows did nothing, `locked_settings.txt` was never written), the
  stats overlay's game fps stayed 0 and the step-down to 30 fps when hot never ran. That is why the LCS app showed
  fewer settings than VCS; the Java side (launcher, panel, overlay, LSFG) never looked at the game. Now they run for
  both games. LSFG itself was always game-independent (the Vulkan present; `vcs_framerate.cpp`'s cap at refresh /
  multiplier is shared, LCS's limiter calls it through `framerate_seam`); in the separate `.lcs` app it only looked
  missing because that app had its own private files (no Lossless.dll copied in, no folder grant, its own prefs).
- **Settings**: VCS-only rows are greyed through `settings::lockForRun` in the side panel (flag 4). The launcher's
  pre-launch rows (`VcsIni` / `LauncherActivity.iniItem`) grey the same keys with the game's note: the game writes the
  run's `settings::lockedForRun()` keys ("key TAB note" a line) to `<data>/locked_settings.txt` at its first host frame
  (`side_panel.cpp` `writeLockedSettings`; `<data>` = `VCS_DATA_DIR`, so per game), and `VcsIni.lockedForGame` reads
  the library's game's file. One list (lcs_settings.cpp's), no copy in Java; before a game's first start nothing is
  greyed. Today LCS's only locked key (`enhance.driveby_rate`) is not a launcher row, so nothing shows
  greyed yet: it takes effect as soon as a locked key is one.
- **Tonight's later LCS features on touch** (2026-11-18, build check at 8c62ec7; reasoned from the code, none
  checked on the phone):
  - *Button prompts* (`lcs_prompts`): LCS writes the control's name as text when the help is expanded, for the device
    used last. With the on-screen controls that is the pad, PlayStation family: face buttons as the font's PS2
    symbols, the rest "L1", "R3", "Left stick" - the names on the overlay's buttons, so nothing touch-specific is
    needed. Before the touch-drag fix (Touch controls, "Touch drags are not the keyboard") a camera drag made them key names
    ("Left mouse", "W/S"). A help box keeps the name
    it was expanded with while it is up.
  - *Drive-by aim* (`lcs_driveby`): the camera drag and the gyro arrive as mouse counts, so they aim it through
    `lcs::camera::mouse` -> `driveby::mouse` like a mouse; the optional touch right stick goes through
    `driveby::stick`. It took three fingers at once (L2 / R2 held to look out of a side, the drag, Circle to fire);
    since 2026-11-18 the **look latch** (Controls extras) keeps L2 / R2 held after a tap, so it is the drag and Circle.
    The gyro's "While aiming" counts the drive-by look as aiming (`lcs::driveby::aiming()`: the look camera was aimed
    in the last 150 ms, host clock, so a pause ends it; `side_panel.cpp` STATE_AIMING, LCS only: VCS's drive-by is the
    game's fixed side shot, nothing to aim). Unchecked on the phone.
  - *Pause menu pointer* (`lcs_frontend`: hover, one-click entries, map drag / zoom, Stats / Brief scroll): while the
    pause menu is open the overlay refuses touches that hit no control, so they reach SDL as a synthesised mouse (the
    position is still read: only its motion is dropped for the camera), so a tap should select and click an entry
    and a drag move the map. Not reachable by touch: the map's zoom and the Stats / Brief scroll are mouse-wheel only
    (no pinch / two-finger scroll is turned into wheel notches; the pad's R1 / L1 and D-pad still do them). The
    overlay's own controls cover part of the menu (the D-pad / face buttons drive it as before).
  - *Free camera* (`lcs_freecam`): VCS's input path (`vcs::freecam` answers from LCS's), so the side panel's Free
    camera row, the overlay's stick to move and the drag to look work as on VCS.
  - *Mission launcher, trainer extras* (`lcs_trainer`, `main.cpp` reads `DATA/MAIN.SCM` from the disc): compiled into
    `libmain.so`; the disc is read through the same fd as the ELF, so the list should fill on Android too. The
    trainer is driven by the overlay's D-pad / face buttons as VCS's.
  - *Saves* (safehouse bookkeeping, loaded-game flag): game-thread code only, same on the phone.
  - *Frame rate / timing* (host CTimer, particle / effects timing, up to 100 fps): untested on the phone; at the
    phone's usual 30-60 fps the patches are mostly idle.
- **Movies**: FFmpeg is in `libmain.so` since 2026-11-18 ("Movies (FFmpeg)" under Building), so LCS's boot movies
  (like VCS's) play through `vcs_movie.cpp`'s player; unchecked on the phone.
- **Sound**: `lcs_sound`'s native driver is in `libmain.so` and plays through the same SDL audio path as VCS's;
  unheard on the phone. No emulated IOP for LCS either (its ~0.2 ms a frame on the Mac is gone there too).
- **Saves** (2026-11-18): the side panel's Save game here / Quick save reach `lcs::save::saveHere` / `quickSave` on
  LCS, and a **Quick load** row (LCS only, press twice; VCS loads only from its frontend) `quickLoad` (slot 8): one JNI,
  `side_panel.cpp` `nativeSaveAnywhere` (moved there from `vcs_save_anywhere.cpp`) picks the game (`game::saves`). The rows' help adds
  "Not now: <reason>" from `whyNot()` (`nativeSaveWhyNot`, either game) when the panel opens. The launcher's **Saves**
  list follows the library's game: `GameFiles.dataDir` (`files/userdata-lcs`) and LCS's folder
  `BASLUS-21423GTA30001` (`SaveCard.LCS_GAME_DIR`; slot files `"N: <mission>"`), slot rows start the game from that
  slot (`VCS_LOAD_SLOT`, which `lcs_save.cpp` takes), export works on LCS's folder ("GTA LCS save ..."), import is
  hidden on LCS (its checks are VCS's). Save pictures: LCS takes them too (`game/save_shots.cpp`, below), the Saves list shows them. Unchecked on the phone.
- **Trainer, stats frame counter**: LCS's trainer (`lcs_trainer.cpp`, through `trainer_select.h`) and frame counter
  (`lcs_frame.cpp`: `rig::onGameFrame` + `ps2perf::markGameFrame` at LCS's frame kick) are in `libmain.so`;
  unchecked on the phone (the stats overlay's game fps should no longer read 0). **Mods**: per game (the Mods screen manages the chosen game's `mods` folder); LCS mods unchecked on the phone.
- **Performance**: the generic GS / VU1 path was slow on the phone at bring-up; the LCS renderer (below: textures by
  block, lit models native, compiled VU1, the flip without the CSR fence) is not measured on the phone yet.

### LCS renderer (src/game/lcs_render.cpp)

LCS draws through VCS's native renderer pieces where they apply (`lcs::render::install`, from `applyLcs`;
`LCS_RENDER=0` = none of it):
- **VU1 microcode**: every VCS VU1 upload is byte-identical in LCS's ELF except the skinning routine (VCS 0x3ABAA0 ->
  LCS 0x153110, VU 0x2D28-0x3528). So the world HLE (`vcs_hle_world.cpp`) drew LCS's prelit world natively from the
  start; the lit models (skinned, rigid, the env pass) now too: `vcs::hleworld::setOtherGameCode` (an `[lcs]` seam)
  accepts LCS's images of the reproduced routines. `PS2X_HLE_VERIFY=1`: 4.8M packets bit-exact; skinned packets under
  6 vertices stay on the VU1 (LCS's skinning writes 6 back). `LCS_HLE_LIT=0`: lit models on the VU1.
  `game/lcs/vu1_uploads.txt`: LCS's uploads, so `recompile.py --game lcs` compiles the VU1 code (`<dir>/vu1`).
- **Textures**: LCS sends each texture before its draw into one VRAM area, level 0 in another format, mips and CLUT
  into the same pages; with page-level keys every draw decoded its texture again (~1,100 a frame). Block-level content
  keys (`gsHwSetBlockContentKeys`, runtime; `PS2X_HW_BLOCK_KEYS=0` off).
- **The flip's CSR read** (0x2D9678, VCS 0x370714) without the render-worker fence (`LCS_CSR_FENCE=1` = fence).
- Mac (M5 Pro, 1x, machine loaded by other builds): opening cutscene 10-17 -> 60 fps, gameplay ~20-30 -> 60 (cap);
  in gameplay game thread ~5 ms, GS stage 7-16 ms a frame (by load). Not measured on a phone yet.

**LCS's saves** (2026-11-18, desktop): LCS keeps GTA III's PS2 save code (CMemoryCard and the frontend's memory card
screens), not VCS's save menu, so `vcs_save_anywhere.cpp` does not apply; `src/game/lcs_save.cpp` does the same things
with LCS's own functions, each address commented with its evidence: **Save game here** raises the frontend's save-menu
flag as the safehouse's ACTIVATE_SAVE_MENU does (the game's own Save screen, 8 slots, its questions), **quick save**
(F9, slot 8) chains the frontend's scan / saving flag / SaveSlot calls inside one scePadRead call, **quick load** (F10,
slot 8) makes the frontend's Load Game check and sets the load request the main loop acts on, and `VCS_LOAD_SLOT`
picks the boot's slot as on VCS. Slot files are `"<slot>: <last mission passed>"` (e.g. `1: In the beginning...`) in
`mc0/BASLUS-21423GTA30001`; the card layer's backups cover LCS too. Control channel `lcssave here | quick | load |
state | slots`; `LCS_SAVE=0` turns it off. LCS's load restores only the 541 globals MAIN.SCM lists and restarts every script; the main
script takes the loaded-game path only when global 6 is 1, which the save points' subroutine (SCM 0x177A3) sets with the
spawn (globals 7..9) before its ACTIVATE_SAVE_MENU, so SaveSlot's hook sets them before a quick save / Save game here
(without it a free-roam save loaded as a new game: the opening mission again, the money kept). The Android side panel and the launcher's Saves list reach it (above,
"What's left for LCS on Android") and the trainer menu's Save page uses it too. Unchecked on the phone.


### The trainer in LCS

The trainer menu (F5 / Back + RB, docs/TRAINER.md) works in LCS too: `src/game/lcs_trainer.{h,cpp}` is LCS's side
(its script commands, addresses and lists, each with its evidence), `src/game/trainer_select.h` lets the menu
(`src/ui/trainer_menu.cpp`, three `[lcs]` seams: which trainer, rows the game lacks, the title) call the game's
trainer. The actions run as short scripts on LCS's own script VM, as in VCS, but LCS's VM is laid out differently (GTA
III's command numbers, CRunningScript IP at +0x18, locals at +0x60, a local variable byte 0x0C + n, a player index
apart from the player's char handle), so `lcs_trainer.cpp` has its own StartNewScript; it runs before each
`CTheScripts::Process` (0x10A288), which is also where the rig's game-thread jobs (`poke`, `dump`, `pause`) and
`vcs::rig::gameFrames()` now run in LCS. The control channel's `trainer ...` command has the same verbs.

In LCS: Player (god mode, heal, never wanted, wanted level, money, sprint, police / everyone ignore), Weapons (34
weapons, three loadouts, unlimited ammo, no reload, fast reload, remove), Vehicles (every car, bike, boat and
aircraft but the train, ferry, airliner and RC toys; get in, invincible, repair, flip, boost, paint), Teleport
(24 districts of the three islands, save / return), World (time, freeze, weather, game speed), Camera and Missions
(below). Hidden: the save page's "put me back after loading". Save game is LCS's saves (`lcs_save.h` through
`game::saves` in trainer_select.h): save here, quick save and quick load of slot 8. The teleport spots lie in the
game's own zone boxes (CTheZones, read in a run) at a measured street height, and are reached without `LOAD_SCENE` (a
synchronous stream load hung the game thread once before the freeze fix): the player waits for the area's collision,
then lands. Checked in hidden runs through the control channel (`trainer ...`, memory read back, the game naming each
district on arrival).

- Wanted level: CWanted sits inside the player ped at +0x7F0 (level +0x20); CWanted::SetWantedLevel clamps to
  MaximumWantedLevel (0x3D829C, 4 early in the story), so "set wanted" raises that limit when asked for more (the
  notice says so). During the opening mission the mission's script clears the level every frame: it shows in free
  roam. The saves refuse with stars ("Lose your wanted level first"), as VCS's.
- Camera page: the free camera (`lcs_freecam.{h,cpp}`) replaces CCam::ProcessSpecialHeightRoutines (0x287020, right
  after the active CCam's update in CCamera::Process) and writes LCS's CCam Source / Front / Up / FOV (+0x1D0 / +0x1C0
  / +0x1F0 / +0xE8); `vcs::freecam::flying()` / `pose()` answer from it on LCS (`[lcs]` seam in vcs_freecam.cpp), so
  the input code and the menu are VCS's. "Teleport player here" puts him on the ground below it. Hide HUD skips
  CHud::Draw (lcs_hud.cpp). `LCS_FREECAM=0` leaves the camera unhooked.
- Missions: the list comes from the disc's `DATA/MAIN.SCM` (main.cpp `[lcs]` seam): its mission table (118 missions)
  and each mission's SCRIPT_NAME; a story mission's title is in the main text under that name (VIC1 "Home Sweet
  Home"). Side jobs: Taxi, Paramedic, Vigilante, Firefighter, Pizza Boy, Noodle Punk, started in their vehicle (one is
  spawned). Launched with LOAD_AND_LAUNCH_MISSION_INTERNAL as the game's markers do (Slacker checked in a run).

Later (not built): the app keeps one library per game (`files/game/<id>/`) and a game choice in the launcher instead
of one library at a time; Saves / Mods per game; the public runtime APK then needs nothing per game.

## The game on Android (personal build)

`python3 tools/android.py build --personal` (`-PvcsBundleGame=true`, `VCS_ANDROID_BUNDLE_GAME=ON`) compiles the whole game into
the APK as `libmain.so` (the runtime's `ps2EntryRunner`, a shared library on Android), next to `libsmoke.so`, as before
the split. **Such an APK contains game code from the player's disc: personal use only, never shared.** The launcher
shows "Built into this app" (`BuildConfig.VCS_GAME_BUNDLED`) and loads nothing.

- **Entry point**: SDL's activity calls `SDL_main`; `src/android/game_main.cpp` is it and calls `main.cpp`'s main,
  compiled as `vcs_game_main` on Android (`main=vcs_game_main` on that one file, kept out of the unity batches; the
  property needs `TARGET_DIRECTORY ps2EntryRunner`, as source properties are per directory).
- **Paths**: `/proc/self/exe` is the system's `app_process`, so `exeDirectory()` takes `VCS_APP_DIR` on Android;
  `VCSActivity` sets it to the app's files folder and `VCS_DATA_DIR` to `files/userdata` (memory card, caches).
- **Flags**: Release always (`ReleaseMode.cmake`'s flags are Release-only); `PS2X_CPU=ARM64` is `-march=armv8.2-a` on
  Android (`-mcpu=apple-m1` on macOS); `-ffp-contract=off -fno-strict-aliasing` in every configuration, as on macOS.
- **minSdk 33**: bionic's `backtrace()` (`src/debug/crash.cpp`) starts at Android 13, as do Vulkan 1.3 drivers.
- **Debug tools on arm64 Linux**: the crash handler and the sampling profiler read `uc_mcontext.pc`; bionic has no
  `dladdr1`, so the profiler sizes symbols as on macOS.
- **FFmpeg** (the movies, 2026-11-18): see "Movies (FFmpeg)" under Building.
- **The disc for adb testing**: with none picked in the launcher, `Android/data/<app>/files/disc.iso` is used (adb can
  write there; `cp` on the phone from Download is fast). The launcher shows a personal build and such a disc as ready.
- The libraries stay inside the APK (`extractNativeLibs=false`): SDL loads them by name, and the launcher asks the
  class loader (`findLibrary`), not the folder.

Built 2026-11-17: the whole game compiled with the NDK after four fixes (the two `gregs[REG_RIP]`, `dladdr1`, the
`main` rename's directory scope); 82 MB APK.

## The launcher

`LauncherActivity` (plain Android views, no AndroidX) is the home-screen entry, the phone's counterpart of the desktop
launcher without its build steps. Since 2026-11-17 it is a handheld-console-style menu (categories Play, Disc,
Saves, Settings, Controls, Frame generation, About over an animated dusk background, touch and gamepad) with pre-launch
settings that write `vcs.ini` and the overlay's SharedPreferences: screens, keys and how to extend it in
[ANDROID_LAUNCHER.md](ANDROID_LAUNCHER.md). Its jobs:

1. **Disc**: picked with the system file picker (`ACTION_OPEN_DOCUMENT`), never copied; the read permission is persisted.
   `DiscCheck.java` is `tools/vcs_disc.py check` in Java: ISO 9660 root, `SYSTEM.CNF`'s BOOT2, the SHA-1 of
   `SLUS_215.90`, the same codes and messages (checked 2026-11-17 against the Python on fake images: unsupported
   build, Liberty City Stories, not an ISO, a .chd). The game gets it as `argv[1]` = `/proc/self/fd/N`
   (`VCSActivity.getArguments`), the fd of the document opened in the game's process.
2. **Game library**: imported into `files/game/libvcs_game.so`, checked as an arm64 ELF shared object with this APK's
   game interface stamp ("Runtime / game split"), read-only.
3. **Play** (both ready) or **Test graphics** (the smoke test). The version line carries the build tag
   (`branch@commit+dirty`), also the first line of the game's log.

The game runs in its own process (`:game`) that ends with it: SDL's native side doesn't start twice in one process.
**Back** (button or gesture) opens and closes the side panel (below) in the game: `VCSActivity.dispatchKeyEvent` takes it
before SDL. While a menu is up it goes back there instead, as Escape did: it closes the settings menu or the trainer, and
is Escape in the game's pause menu (`nativeGameState`). The game's pause menu is the panel's "The game's pause menu" row (Escape held a few frames, injected through
`touch_controls.cpp`'s `injectKey`) or Start. In the smoke test (no panel) Back is still SDL's: Escape for a few frames
(`platform_sdl3.cpp`: SDL delivers it as a down and an up within one frame, too short for the keyboard snapshot), which
leaves it.

- **Dialogs on short screens** (0.3.1): every AlertDialog goes through `Dialogs.fit()` after `show()`. A long message
  (the phone build's notes) made the dialog taller than a landscape / folded screen and its button row ("Build") fell
  below the edge. `fit()` keeps the window inside the system bars and cutout and, if it is still taller, gives it an
  exact height so the dialog's own layout shrinks the scrolling message and keeps the buttons; it re-checks on folds and
  turns. A new dialog must go through it too.

## Crash reports

Players report crashes without a PC or adb (0.3.1, after a "crashes on boot" report from a phone we had never run on).

- **The game's log on the phone**: `src/android/android_log.cpp` pipes stdout / stderr into logcat as before and copies
  every line to `files/logs/game.log` (flushed per line; at most 16 MB, then logcat only). The run before is kept as
  `game.prev.log`. The crash handler (`src/debug/crash.cpp`, `[android]` lines) calls `vcs_android_log_drain()` (weak)
  before the process ends, so its report (signal, address, stack) reaches the file.
- **Java exceptions** in the game's process (`:game`) are appended to the same file (`CrashReport.installInGameProcess`,
  from `VCSActivity.onCreate`), then go on to Android's own handler.
- **About > Game log and crash report…** (`CrashReport.java`): the value says how the last run ended (crashed, code pack
  refused, ended normally, ended without a word). *Save to a file…* (the system's file picker, no permission) writes
  the whole report; *Share…* sends it as text (cut to 400 KB, the share intent's limit). The report: phone model, chip
  (`Build.SOC_*`), Android version, app version and build tag, the game library's size, both logs and the app's own
  logcat (`logcat -d`: Android gives an app only its own lines, which include the GPU driver's messages for our process).
  No game data, nothing from the disc.
- What still escapes it: a crash before `SDL_main` starts (the library not loading: Android's own crash dialog; the
  logcat part of the report still has it), and the system's tombstone (not readable by apps).

## Touch controls

An on-screen PS2 pad, `TouchControls.java` (a plain `View` that `VCSActivity` lays over SDL's surface, once `libmain.so`
is loaded), with the native side in `src/android/touch_controls.cpp` (JNI, built into `vcs_platform` on Android so the
smoke test and the game both have it).

- **Layout** (after the earlier port's, in its units of 1/135 inch, anchored to the sides and bottom, centred in the 4:3
  picture's black bars as far as they fit): the left stick high and the D-pad low on the left with L3 between; Triangle /
  Circle / Cross / Square on the right as on the PS2 pad, R3 in the bottom-right corner; L1 / L2 and R1 / R2 in the top
  corners (L2 / R2 outside), Select / Start beside them; four small buttons at the top centre (show / hide, settings,
  trainer, the side panel; three in the smoke test), kept centred. L2 / R2 are the triggers
  (axes at 1), the rest `platform::GamepadButton`s.
- **Look: this port's own**, not the earlier port's (whose layout and behaviour it shares): dark violet glass, teal accents
  on the left and pink on the right; the face buttons as diamond tiles with the PS2 symbols drawn as vectors in their
  colours (lit and haloed while held), the shoulders and Select / Start as tabs slanted outwards with an accent bar, the
  stick as a ring of twelve segments with a knob tethered to its centre, the D-pad as a cross of four arms with a chevron
  each (no base disc), L3 / R3 as hexagons.
- **Settings button** (the gear, top centre): no F2 key on a phone, so a tap sends `nativeToggleMenu` and the settings
  menu (`ui/settings_menu.cpp`, the same as F2 or Back + Start on a gamepad) opens or closes. Its pages work by touch (a
  touch on no control goes to Dear ImGui as a click while it is open): e.g. Display > Presets > Balanced for widescreen
  and twice the resolution (live since 2026-11-18).
- **Trainer button** (the teal spark, right of the gear): no F5 key either, so a tap sends `nativeToggleTrainer` and the
  trainer (`ui/trainer_menu.cpp`, docs/TRAINER.md; F5 or Back + RB) opens or closes, through `takeTrainerToggle` as the
  gear's `takeMenuToggle`. One menu at a time: ignored while the settings menu is open. The trainer reads only keys and
  the pad, and the overlay is a pad, so the **D-pad** selects / changes, **Cross** applies, **Circle** goes back (its
  key line says so while a pad is there: "D-pad / Cross / Circle" for the PlayStation family the overlay reports). While
  it is open the game wants no mouse (`trainerMenuBlocksInput` -> `onHostFrame(menuOpen)` -> `setMouseCaptured(false)`),
  so a touch on no control is not the camera or the stick: it goes on to SDL, which nothing under the trainer uses.
- **A virtual gamepad**: the view decides what each finger holds and sends the pad (buttons, the left stick, triggers)
  through `nativeSetPad`. `platform_sdl3.cpp`'s `snapshotGamepad` merges it with SDL's real gamepad: a button held on
  either, each axis from whichever is tilted further, `gamepadConnected()` while the overlay is shown, the PlayStation
  family when it is the only pad (the button prompts show PS2 symbols), `gamepadName()` "On-screen controls". So the game,
  the free aim, the prompts and the bindings (Gamepad page) need nothing new, and a real controller keeps working beside it.
  An earlier Android port sends the layout to native code and lets it decide the hits because a touch there that hits no control
  is the Switch game's own touchscreen; this game has none, so the hits stay in Java.
- **Camera: the mouse camera, not the right stick.** A drag on the right half (on no control) is mouse motion: the view
  sends it in mouse counts (`nativeLook`, 3 counts per dp: half a turn across a phone's right half at 100% Mouse
  sensitivity) and `takeMouseMotion` adds it to the mouse's, captured or not. Chosen because a finger on glass is a
  position, not a tilt: the mouse camera turns by the distance moved, 1:1 at any frame rate, and already does the work
  on foot, behind vehicles (with the car camera's return), in the scopes and in free aim (docs/CONTROLS.md). A
  drag-as-stick would turn by how far the finger is from where it landed, a rate, and need its own deadzone and curve.
  It needs Keyboard > Mouse camera on (the default) and the Mouse sensitivity / Invert settings apply.
- **Who gets a touch**: while the game wants the mouse captured (`setMouseCaptured`: playing, no menu or pause menu open
  - `noteLookWanted`, read by the view at each touch) every touch is the overlay's: a control, the camera (right half), or
  the stick re-centred under the thumb (left half). Otherwise a touch on no control is refused and goes on to SDL's
  surface: Dear ImGui's menus and the game's pause menu by mouse (vcs_frontend). SDL's mouse motion made from fingers is
  ignored for the camera (`SDL_TOUCH_MOUSEID`). A finger slides between buttons (Square to Cross); the stick's centre
  follows a thumb past its edge; the D-pad has eight ways.
- **Modes** (SharedPreferences `touch_controls`, the launcher's "On-screen controls" card or in the game): **Auto** (the
  default) draws nothing while SDL has a physical gamepad open (`nativeRealGamepad`, polled twice a second), **On**
  always, **Off** never (every touch is SDL's; back on from the launcher). A tap on the top-centre button hides / shows
  the controls; holding it opens the settings: the mode, opacity, size, vibration on press, the stats overlay's switches
  (below), **Edit layout** (below, "Layout editor": each button or group moved, resized, its opacity; kept per screen).
- **Check without the game**: the smoke test's window shows every PS2 button lit while held, L2 / R2, both sticks as
  numbers and dots, and "Test the camera drag (10 s)": the mouse captured for 10 s (as while playing; every touch is the
  overlay's meanwhile) with the motion summed.

- **Touch drags are not the keyboard** (2026-11-18): the button prompts' device (`vcs_input.cpp`: more than 12 mouse
  counts in a read is the keyboard and mouse) counts only a real mouse's motion (`platform::lastRealMouseMotion`, the
  part of `takeMouseMotion` before the overlay's drags are added), so a camera drag no longer switches the prompts to
  key names; with the on-screen controls the prompts stay the pad's (PlayStation: they are its only gamepad). Unchecked
  on the phone.

Open: with Mouse camera off, a drag on the right half is SDL's
(a mouse click for the game, as without the overlay). The game's few right-stick-only uses (if any are found) can use
the optional right stick (Controls extras).

- **While the trainer or the settings menu is open** (`nativeGameState`, polled five times a second) only the D-pad and the
  face buttons are drawn and hit (they drive the trainer); the stick, the shoulders, Select / Start, L3 / R3 would cover
  the trainer's panel on the left. The stats overlay hides meanwhile. (Device test 2026-11-17, cover screen.)
- **The pad's state is sent once laid out**, not at the first touch: before, the trainer's key line read the keyboard's
  names ("Enter: apply") until a control was touched, as the game saw no pad yet.

## Layout editor

The controls' places, per screen (2026-11-17; `TouchControls.java`). Opened from the side panel (Input > Edit layout…)
or the controls' long-press dialog; the game's pad is let go meanwhile.

- **Use**: a toolbar of chips - **Button** / **Group** (what a drag picks: one control, or its group: the left stick with
  L3, the D-pad, the face buttons, R3, each side's shoulders with Select / Start, the right stick; remembered,
  `edit_mode`), **Vehicle** (shows the in-vehicle layout, to place Gas / Brake / the lock; only with Vehicle layout on),
  **HUD** (where the game's HUD is, below), **Reset** (the selection), **Reset all** (this screen; tap twice, "Sure?"),
  **Done** (Back too). Tap a control to select it (yellow), drag it to move it (past 4 dp: a tap doesn't nudge it),
  pinch with a second finger anywhere to resize it (50-200 %); the selection's card has **Size** and **Opacity** sliders
  (opacity as shown, 10-100 %, stored as a factor of the overall Opacity setting). A tap on nothing selects nothing.
  The toolbar sits where it covers no control (the top, the middle or the bottom, in one or two rows, placed when the
  editor opens); the card goes under it, or wherever it doesn't cover the selection. Controls are kept on the screen.
- **Model**: the default layout (below) for the screen, then each group moved / scaled around its centre (`g<k>`), then
  each control moved / scaled / its opacity (by a stable name: `L1`, `L2`, `R1`, `R2`, `SELECT`, `START`, `LSTICK`,
  `L3`, `DPAD`, `TRIANGLE`, `CIRCLE`, `CROSS`, `SQUARE`, `GAS`, `BRAKE`, `LOCK`, `R3`, `RSTICK`). Offsets are in the
  layout's units (1/135 inch), so they follow the global Size only through the default places they start from.
- **Saved** in `touch_controls` as `layout@<key>` = `v2[;legacy];g<k>:dx,dy,scale;...;<NAME>:dx,dy,scale,opacity;...`
  (only what isn't the default), committed at every gesture's end. **The key** is the screen's size in steps of 40 units,
  in the view's orientation (`round(w / u / 40) x round(h / u / 40)`), taken from the largest window
  (`getMaximumWindowMetrics`) so the system bars don't change it, the view's size when the two disagree by more than 15 %
  (just folded / unfolded: the metrics may lag). A foldable's cover screen and inner screen, each held either way, are
  four layouts (the Honor: about 19x9 / 9x19 and 20x18 / 18x20); a configuration change lays the controls out again
  (`screenChanged`), so fold / unfold switches layouts, and back.
- **Taken over**: before v2 the editor moved 7 groups and saved `group<g>@<key>` = "dx dy scale" (the key from the view's
  size). With no `layout@` for a screen, those (under the same key, else the view-size key) are read, saved as v2 with
  `legacy` (that screen keeps the defaults its groups were moved from: no HUD nudge), and left in place; Reset all writes
  an empty v2 layout, so they are not taken over again. **Reset layout** in the side panel (press twice) is Reset all.
- **Defaults, landscape** (and a near-square screen either way): as before ("Touch controls"), then **off the HUD**: with
  the game's radar or money / health under a group (each group's box against the HUD's boxes, the shoulders first), the
  group moves by the least that clears it and stays on the screen (6 units' gap). Not for `legacy` layouts.
- **Defaults, upright phone** (the picture letterboxed at the top with at least 22 % of the height left below it, e.g.
  the cover screen held upright with Portrait mode on): a handheld's layout in the space under the picture -
  L2 L1 / R1 R2 along its top with Select / Start between them, the D-pad high and the left stick lower and inwards on
  the left, the face buttons high and the right stick lower on the right, L3 / R3 in the bottom corners; scaled down to
  fit (290 x 340 units at 100 %). A touch on the picture starts the floating right stick (Camera Drag: the camera drag),
  all of it, not only the right half; below it, the left half starts the
  stick under the thumb as in landscape.

**Portrait mode** (`portrait_outer`, outer screen only, off; side panel Input, launcher Controls): `VCSActivity.applyOrientation`
asks `FULL_USER` on a phone-shaped screen instead of `SENSOR_LANDSCAPE` (a near-square one is always `FULL_USER`). Upright,
the 1080x2420 cover screen is narrower than 3:4: Match window clamps to 3:4 (Vert+) and the picture is letterboxed; the
presenter's `fitRect` puts the frame **at the top** of a target taller than it, on Android (`[android]` in
`src/render/presenter.cpp`: the frame's top at 0, the picture centred in the frame; `vcs_frontend.cpp`'s pause-menu mouse
uses the same `fitRect`, so its hit areas follow). With Android's auto-rotate off, `FULL_USER` keeps the locked rotation.

**The HUD and the buttons**: the HUD's boxes are the radar (37,336 - 120,412 of 640x448, game-internals.md 6f) and the
top-right cluster (money, clock, health, armour, weapon, stars: about 355,14 - 612,120, from the squeeze table's numbers;
approximate), placed as the game places them for `display.hud` and `display.aspect` (read through the panel's store,
`SidePanel.Setting`, rechecked every second): **Keep 4:3** in the frame's middle 4:3, **Screen edges** at the frame's
edges keeping their size, **Stretch** across the frame. On the cover screen in landscape (2420x1080, about 2.24:1) the
black bars beside a 4:3 HUD are wide enough for the default controls: with **Keep 4:3** nothing is under a button. The
**Balanced** preset sets **Screen edges**, which puts the radar under the D-pad and the money / health under R1 / R2 /
Start - the case the nudge above moves them for (the D-pad to the radar's right, the right shoulders below the cluster,
the face buttons and the right stick after them). So: Screen > HUD > Keep 4:3 is the clean fix on a wide phone; the
nudge and the editor's HUD overlay are for Screen edges and for screens without wide bars (16:9 phones, the inner
screen, where the controls are over the picture anyway).

## Controls extras

Beside the pad above, set in the side panel's Controls tab (SharedPreferences `touch_controls`, keys in brackets):

- **Camera** (`camera`: 1 Floating stick, the default, 0 Drag; a stored 2, the old Both, reads as 1): Floating stick is
  a right stick that starts under the thumb anywhere free on the right half, as the left stick does on the left, and
  rests low on the right inside the face buttons (its own layout-editor group); sent as the virtual pad's right axes
  (`nativeSetPad`), so the game's right-stick camera, free aim and the free camera take it as a gamepad's (Gamepad >
  Invert camera, the panel's Invert stick rows, apply). Drag: no stick, a drag on the free right half is the mouse
  camera.
- **Gyro aiming** (`gyro`: 0 Off, 1 While aiming, 2 Always; `gyro_sens` %, `gyro_axis` 0 turn / 1 lean like a wheel,
  `gyro_invert_x`, `gyro_invert_y`): `Gyro.java`, the gyroscope's rate times the time between readings, in the camera
  drag's mouse counts (400 a radian at 100 %, `nativeLook`), so Mouse sensitivity / Invert apply on top and the mouse
  camera, the scopes and free aim all follow. The screen's up / right axes come from the display's rotation. "While
  aiming" is the game's free aim (`vcs::aim::aiming`; LCS's, and its drive-by look, `lcs::driveby::aiming`) or R1 held
  (the lock-on), from `nativeGameState`. Only while the game
  wants the camera (`nativeLookWanted`) and the panel is closed. A rate sensor: nothing drifts, so there is no recentre.
- **Rumble** (`rumble`, on; `rumble_strength` %): `Rumble.java` polls the game's vibration (what
  `platform::gamepadRumble` was last asked for, after Gamepad > Vibration; `Rumble.nativeLevel`) 30 times a second and
  drives the phone's vibrator (`VibratorManager`, amplitude from the stronger motor, a 500 ms one-shot renewed while it
  lasts) while no real gamepad is connected (SDL rumbles that one). Off while the panel pauses the game.
- **Layouts by context** (`context_layouts`, on): in a vehicle (`vcs::trainer::inVehicle`, `nativeGameState`) Cross and
  Square become tall **Gas** and **Brake** pedals under Triangle / Circle; on foot the usual diamond. Changed when no finger
  is down.
- **Accelerator lock** (`cruise_button`, on): a LOCK hexagon beside Triangle in a vehicle: Gas held until Brake or a
  second tap; let go in a menu or a cutscene (`nativeLookWanted` off) and on leaving the vehicle.
- **Sprint toggle** (`sprint_toggle`, off): on foot a tap on Cross holds it (sprint) until the next tap; let go as the
  lock is.
- **Look latch** (`look_latch`, on; 2026-11-18, unchecked on the phone): in the vehicle layout a quick tap (under
  250 ms, the other look button not held) on L2 or R2 keeps it held - looking left / right, for a drive-by (both games
  drive-by with a look button held and Circle; on LCS the drag / gyro / right stick also aim it, `lcs_driveby`) with
  two fingers instead of three. A tap on it again, or a press of the other, lets go (that press then works as usual);
  a hold works as before (L2 + R2 held: look behind). Let go as the lock is (a menu or cutscene, leaving the vehicle).
  The latched button is drawn held. `TouchControls.lookLatch`, in `send()` after the accelerator lock and before
  remapping (so a remapped L2 / R2 latches what it is mapped to).

## Side panel

`SidePanel.java`, a plain view over the game (above the controls), in the overlay's dusk palette (violet glass, teal
focus, pink accents): a card from the right (42 % of the width, 320-440 dp), five tabs, rows of toggles, choices
(‹ value ›), sliders and actions, a line of help under the focused row. Its native side, `src/android/side_panel.cpp`,
is in the game only (`libmain.so`; the smoke test has no panel). It **complements** the game's settings menu (the gear,
F2: every setting by page) and the controls' long-press dialog: both are rows in it, and all three show the same values.

| Open | Close |
|---|---|
| Back (button or gesture); the fourth top button; a swipe in from the right edge (18 dp); a controller's Home / Guide, or Select + Start held 0.7 s | Back; a tap beside it; its ✕; Circle / B or Start; Home again |

- **Focus**: touch (a tap focuses and changes; a choice steps back on its left third; sliders drag), the overlay's pad (its
  controls beside the card still work: a touch on one goes to the overlay), a real controller and a keyboard: D-pad or
  left stick move (repeat while held), left / right change, Cross / A select, Circle / B back (a sub-screen to its tab,
  else close), Start close, L1 / R1 (L2 / R2) tabs. The
  focused row has a teal frame. The pad is read from native code (`nativeNavPad`: the real pad as SDL reads it, before the
  merge, plus the overlay's), so pads SDL drives through HIDAPI work too; a keyboard's keys come through
  `dispatchKeyEvent` (a controller's are left to SDL). While the panel is open the game reads no pad
  (`setPanelOpen` in `touch_controls.cpp`; after it closes, none until everything is let go).
- **Game**: back to the game; **Pause the game** (the game thread holds as with Game > In the background = Pause, the sound
  muted: `hostPaused` in `vcs_input.cpp`; a veil says PAUSED, a tap, Cross or Start resumes); pause while the panel is
  open (off: the game goes on behind it, as in an earlier Android port); the game's pause menu (Escape, as Back used to be);
  **Screenshot** (below); the settings menu (F2), the trainer (F5), the free camera (F6: the overlay's left stick moves, a
  drag or the right stick looks, L2 / R2 down / up), skip intro movies; quit to the launcher (select twice).
- **Perf** (Performance): frame rate cap, game timing, internal resolution, the Balanced / Authentic presets, Show frame rate,
  the performance overlay (F3), a perf CSV (F4, injected); the stats overlay (master, compact, each item); **frame
  generation** (placeholder, below).
- **Screen** (Display): aspect ratio, HUD, cutscene bars, MSAA, texture filtering, Reduce Post Processing Effects. (A Vert+ setting
  joins when it is in the registry: one `setting("<key>")` line in `buildDisplay`.)
- **Input** (was Controls): the controller (below, "Controllers") and **Remap controller…**; the overlay's mode,
  opacity, size, vibration, edit layout ("Layout editor"), **Reset layout** (this screen; press twice), **Portrait mode**
  (`portrait_outer`), **Remap buttons…** (below, "Remapping"), its dialog; camera (floating stick /
  drag), Mouse camera / sensitivity / invert / car sensitivity (shown as Drag ...); gyro aiming; rumble and Gamepad >
  Vibration; layouts by context, the accelerator lock, the look latch, the sprint toggle; free aim as "Free aim (controller / touch)"
  (`pad.free_aim`: the overlay is a virtual gamepad) and "Free aim (mouse)" (`mouse.free_aim`, shown only while an
  external mouse is connected); button prompts.
- **Audio**: master, music, effects, movie volumes, speakers.
- **One store**: the game's settings are read with `nativeSettingInfo` (type, value, range, choices, label, help, flags)
  and changed with `nativeSettingSet` / `nativeSettingPress`, queued and run by `panelHostFrame` on the main thread
  (main.cpp's UI hook, after the menus) through `settings::set`, as the settings menu does: vcs.ini stays the one file,
  Live settings apply at once, Restart ones show **next start** (pink **restart to apply** once changed); one set by its
  environment variable or locked by another is greyed. The values are reread twice a second while open.
- **Screenshot**: `nativeScreenshot` asks the presenter for the picture as shown (the game's menus included, the Android
  views not) into the app's cache; the panel copies it into `Pictures/VCS Recomp` through MediaStore (`IS_PENDING`; no
  storage permission from Android 10 on) and deletes the cache file. A keyboard's F8 still writes `files/screenshots/`.
- **Frame generation**: the panel's rows write SharedPreferences `lsfg` (`MODE_MULTI_PROCESS`): `on`, `mult`, `flow`,
  the same keys the launcher writes and `Lsfg.env` reads, and switch it live (section "Frame generation (LSFG)": also
  with a DLL added since the start).
- **Panel settings**: SharedPreferences `side_panel`: `tab` (the last one), `pause_open`.
- **Layout rules** (2026-11-17, after "it looks cramped" on the phone): tab names are short (**Game · Perf · Screen ·
  Input · Audio**), laid out by their measured width, and the strip shrinks its text (to 9 sp at the least) rather than
  cut one, so none truncates on the cover screen (card 320 dp) or unfolded (42 %); L1 / R1 marks at its ends only while a
  controller is connected. No footer of hints. A row is one line, title and value; a description shows **only under the
  focused row**, at most two lines. Our own rows' texts are short phrases. A row of the game's registry keeps upstream's
  label unless `SidePanel.LABEL` has a shorter / phone-worded one, and its description is `SidePanel.SHORT`'s summary
  for its key, else upstream's help (`settings::Spec::help`) cut to two lines; upstream's strings are never edited for
  the panel. The Remap screens are sub-screens of the Input tab (a "‹ Back" row, Back / Circle).

## Remapping

- **Controller** (side panel > Input > Remap controller…): the same bindings as the PC's Gamepad page, upstream's
  `pad.<id>` Choice settings (`vcs_input.cpp`: a `platform::GamepadButton`, 20 LT, 21 RT, -1 None), so vcs.ini, the
  settings menu (F2 / the gear) and the panel are one system. The keys come from the registry (`nativePadBindings`: the
  `pad.` choices that end with None), each a row: tap it (or Cross / A) and press the controller button within 6 s
  (`nativeRealPad`: the real pad alone, not the overlay; every button binds, Circle and Start too; the press that started
  it is let go first; a touch or Back cancels). The button another PS2 button had is swapped onto it, so one source stays
  one PS2 button (the PC page allows sharing; it still shows here if made there). ◀ ▶ (or a tap without a controller)
  cycles the choices. **Reset to defaults** sets each to its registry default. No new setting, no upstream edit.
- **The overlay follows the controller's bindings**: the overlay sends each PS2 button as its default gamepad source
  (Cross as South), so a remapped controller would have moved the on-screen buttons too. `side_panel.cpp`'s
  `publishTouchBindings` (each host frame) gives `touch_controls.cpp` the map default source -> bound source, and
  `platform_sdl3.cpp` merges `touchPadBound()` instead of `touchPad()`: on-screen Cross stays the PS2's Cross. Identity
  while the settings menu or the trainer is open (they read the plain pad to navigate).
- **On-screen buttons** (Input > Remap buttons…): which PS2 button each of Cross, Circle, Square, Triangle, L1, R1, L2,
  R2, L3, R3, Select, Start presses (SharedPreferences `touch_controls`, `map_<bit>` = bit, `TouchControls.REMAP_BITS`);
  applied last in `TouchControls.send()`, after the sprint toggle and the accelerator lock (they act on the physical
  button), not while the trainer / settings menu is open. The D-pad and the sticks are not remapped; the buttons keep
  their PS2 symbols (the label is the button, not what it presses). Reset to defaults clears the keys.

## Controllers

Upstream's platform layer opens the **first** SDL gamepad and ignores the rest until it goes. On Android that is
fragile: SDL's Android backend (`SDLControllerManager.isDeviceSDLJoystick`) takes every input device with a joystick,
gamepad **or D-pad** source, and gives each an automatic gamepad mapping (`SDL_CreateMappingForAndroidGamepad`), so a
device with arrow keys only (the phone's own key devices, a pad's keyboard / consumer-control node, a BT keyboard or
remote) is a "gamepad" too; if it was first, the real pad was never read and Auto hid the overlay for nothing.
`src/android/gamepads.cpp` (in `vcs_platform`) replaces that on Android:

- **Hints** before `SDL_Init`: `SDL_HINT_TV_REMOTE_AS_JOYSTICK=0` (a device with fewer than two axes and no hat is a
  keyboard to SDL, not a joystick), `SDL_HINT_JOYSTICK_HIDAPI=0` (SDL's own default on Android, made explicit: HIDAPI asks
  for USB permission and takes the device exclusively; SDL hides the Android device of a pad HIDAPI drives, so HIDAPI +
  Android doubles are not possible either way).
- **Every pad** SDL adds is logged (`[gamepad] + id "name" vid:pid type axes hats buttons serial path -> used / ignored`),
  filtered by name (a pad's ` Motion Sensors` / ` Touchpad` / ` IMU` / ` Keyboard` / ` Consumer Control` nodes,
  `uinput-` fingerprint sensors, `gpio`, `-keys`, AVRCP / headset keys) and opened; the same VID / PID + name is one pad
  in the list ("2 devices, read as one"), all its devices read. `VCSActivity` also logs every Android input device with
  a pad / D-pad source (`[gamepad] android device ...`: sources, descriptor) at start: the log to send when a pad shows
  twice.
- **The choice** (SharedPreferences `touch_controls`: `pad_choice` 0 Auto / 1 chosen / 2 touch only, `pad_vid`,
  `pad_pid`, `pad_name`; `Gamepads.java`, pushed with `nativeSetChoice` at start and on change): **Auto** reads the pad
  pressed last (a button, or a stick / trigger past half), the first connected before any; a **chosen** pad is matched by
  VID / PID (by name when both are 0) and Auto is used while it is away; **Touch only** reads none (the overlay shows in
  its Auto mode, the phone vibrates). `platform_sdl3.cpp` asks `activeGamepad()` each frame for `g_gamepad` and gives
  Dear ImGui the same pad (`ImGui_ImplSDL3_SetGamepadMode(Manual)`), so the settings menu follows the choice.
- **Where**: side panel > Input > Controller (the SDL pads, each with a status line: in use / connected / ignored and
  why; rebuilt when one comes or goes), and the launcher's Controls > Controller (Android's `InputDevice` list by the same
  rules, as the launcher has no SDL; a chosen pad not connected shows as "(not connected)").

## Save anywhere (fork addition, all platforms)

The trainer's **Save game** page (F5 or the overlay's trainer button): *Save game here* opens the game's own Save menu
where the player stands, *Quick save* writes slot 8 without it, and a save made here puts the player back at that spot
after loading if the game's load did not (docs/TRAINER.md "Save anywhere"; `src/game/vcs_save_anywhere.{h,cpp}`).
Platform-neutral, so it is not `[android]`: its few lines in upstream files (`vcs_overrides.cpp`, `trainer_menu.cpp`,
`vcs_trainer.{h,cpp}`) are marked `[fork]`. Positions: `files/userdata/save_anywhere.txt` (next to `mc0`), not on the
card.

- **Side panel hook** (to wire): `static native void nativeSaveAnywhere(int what)` in `SidePanel`, JNI
  `Java_io_github_batinium_vcsrecomp_SidePanel_nativeSaveAnywhere` (in `vcs_save_anywhere.cpp`, `__ANDROID__` only):
  0 = Save game here, 1 = quick save (slot 8; confirm in the panel first when slot 8 is taken). Any thread; refusals and
  results are trainer notices on screen. With *pause while open* on, the request waits until the panel closes and the
  game runs (then the menu opens or the save is made).
- Phone checklist (not run yet):
  1. Outside a safehouse, on foot, no wanted level: Trainer > Save game > Save game here: the game's Save menu opens;
     save into a free slot; "Save Successful", the notice "Saved in slot N, here"; back in play where you stood.
  2. Walk / drive far away; pause menu > Game > Load Game > that slot: the load finishes; you are where you saved (the
     log, `adb logcat -s vcs | grep save-anywhere`, says how far the game's load put you and whether the trainer moved
     you). Note which it was (decides whether the restore is needed).
  3. Quick save (slot 8) twice: the second asks for Enter again ("sure?"); then Load Game slot 8: back there.
  4. Refusals: during a mission, with a wanted level, in a car, in a cutscene, while falling: each says why.
  5. After loading a save made here: start the next story mission from its marker, pass it, save at a safehouse (slot 8:
     its position line goes, `save_anywhere.txt`), load it: the safehouse save behaves as before.
  6. Restart the app: the newest save (made here) loads at boot and the player is put back there too.
  7. The card's backups (launcher Saves) still list the slot files made here.

## Stats overlay

What a player tuning the game on a phone wants to see, as a row of small chips at the top centre (under the touch
controls' top buttons): `StatsOverlay.java`, a plain `View` over SDL's surface (under the controls; it takes no touches),
with its native side in `src/android/stats.cpp` (in `vcs_platform`, as `touch_controls.cpp`). **Off by default.**

| Chip | What | From |
|---|---|---|
| FPS | frames presented a second, and the game's own (`· game 30`) | `platform::beginFrame` counts host frames (`noteHostFrame`); main.cpp's UI hook notes `vcs::rig::gameFrames()` (`noteGameFrames`; the smoke test has none and shows the first alone) |
| FRAME | average frame time over the half second, and the longest frame (pink when a hitch: over twice the average and 40 ms) | the same host frames |
| GPU | the GPU's time a frame, averaged over the half second | the Vulkan RHI's timer (below) |
| CPU | each cpufreq cluster's cap / top clock in GHz, the cluster with the most cores first (pink below 85 %) | `/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq`, `cpuinfo_max_freq` |
| HEAT | thermal status (none, light, moderate...) and headroom (pink from moderate or headroom 0.85) | `PowerManager.getCurrentThermalStatus`, `getThermalHeadroom(10)` (every 2 s: NaN when asked more often) |
| BATTERY | temperature, level, charging (plugged in counts) | the sticky `ACTION_BATTERY_CHANGED`, every 2 s |
| MEMORY | this process's resident set (the game runs in `:game`), the phone's RAM in use / total, "low" | `/proc/self/status` VmRSS, `ActivityManager.MemoryInfo`, every 2 s |
| BUILD | the build tag | `BuildConfig.VCS_BUILD_TAG` |

- **Settings**: SharedPreferences `stats_overlay` (`MODE_MULTI_PROCESS`: the launcher and `:game`): a master switch
  (`on`, default off), one per item (`item_<key>`, default on) and `compact` (default on: one line of short values, small,
  what does not fit left out; off: the chips with every detail, in rows - 3 rows took ~20 % of the cover screen's
  height). On the launcher (the "Stats overlay" card: a switch,
  "Choose items…") and in the game (hold the controls' show / hide button), applied at once there.
- **Not the game's own counter**: Display > Show frame rate (`display.fps_counter`) is upstream's single number, the
  game's frames a second, drawn by the game in a corner. This overlay is the phone's view of the whole app; both can be on.
- **Light**: the readings run on a thread of their own (`vcs-stats`) twice a second while the overlay is on and the
  window visible, and are made into text there; the UI thread measures and draws, with nothing allocated in `onDraw`.
  Native side: a few relaxed atomics a frame, published every half second; `nativeFrameStats` reads the last ones.
  While on, the log gets the frame numbers every 15 s (`[stats] ...`).
- **GPU time**: the RHI's existing timestamp timer (`GpuTimer`, `PS2X_GPU_TIMING`'s) always makes its query pools on
  Android (64 timestamps a slot without markers); while the overlay shows the GPU chip (`nativeSetWanted` ->
  `gpuTimingWanted`) `beginFrame` writes a timestamp at the frame's start, present already writes one at its end, and
  `collectTimes` (after the slot's fence, a few frames later) reports the span to `noteGpuFrameMs`. It is the span from
  the frame's first command to its last, gaps between submits included: an upper bound on a tiler. Present is unchanged.
- **Cost of a View over the SurfaceView**: An earlier Android port found that Android views over the game's surface make the app
  window draw a new buffer at each change and the compositor take a second layer, which cost a GPU-bound game frames;
  it draws its overlay into the game's images instead. This one changes twice a second only, and is off by default; if
  it costs frames when on, drawing it through Dear ImGui (or the presenter) is the next step.

## Frame generation (LSFG)

Lossless Scaling's frame generation, as in an earlier Android port: frames made on the GPU between two of the game's, shown in
between, so a 60 fps game shows 120 on a 120 Hz panel (x2), or a 30 fps one 60. **Off by default; needs the player's own
`Lossless.dll`** (from Lossless Scaling, a paid Windows program: its folder under Steam, `steamapps/common/Lossless
Scaling/Lossless.dll`). The DLL holds the shaders (SPIR-V in its resources); it is copied into the app and read at
start, never shipped, never committed.

- **Code**: `src/rhi/vulkan/lsfg/` is a C port of Eden's LSFG (itself from lsfg-vk), **GPL-3.0-or-later**
  (`lsfg/LICENSE.md`, `COPYING`): `lsfg_dll.c` reads the PE's RCDATA (the performance shader set, the native-FP16 twins
  at id + 49, bindings renumbered), `lsfg_chain.c` the compute passes (mipmaps, alpha, beta per game frame; gamma /
  delta / generate per generated frame), `lsfg_pacer.c` how many to generate. Taken unchanged but for the device:
  volk's entry points and `lsfg_vk.h` (in place of the earlier port's `vka.h`). The glue is `src/rhi/vulkan/framegen.{h,cpp}` (the earlier port's
  `vka_framegen.c`: settings, DLL, chain, warm-up, the refresh cap) and `framegen_present.inl`, included inside
  `VulkanDevice` so `rhi_vulkan.cpp` only gains a few one-line calls (marked `[android] LSFG`).
- **Games and platforms**: both games, Android only (2026-11-18). Nothing in it or in the launcher's Frame generation
  category looks at the game; the frame-rate cap's "refresh / multiplier" ceiling (`vcs_framerate.cpp`
  `effectiveFps`) is shared with LCS's limiter (`lcs_framerate.cpp` -> `framerate_seam`). Desktop (Windows, Linux,
  macOS, either game) has none: the option is off there and the pacing needs `VK_GOOGLE_display_timing`.
- **Build switch**: CMake `VCS_LSFG` (`src/rhi/CMakeLists.txt`), default ON on Android, OFF elsewhere (desktop builds
  compile none of it). Gradle: `-PvcsLsfg=false` builds without it (`BuildConfig.VCS_LSFG`; the launcher then says "not
  in this build"). An APK with it is GPL-3.0-or-later as a whole; so is this repository.
- **Settings** (environment, read once when the Vulkan device is made; `VCSActivity` sets them from the launcher before
  `super.onCreate`): `VCS_LSFG=1`, `VCS_LSFG_MULT=2|3|4` (images per game frame), `VCS_LSFG_FLOW=25..100` (the optical
  flow's resolution in %, default 50: cheaper, a little less exact), `VCS_LSFG_DLL=<path>`.
- **Live** (side panel > Perf > Frame generation): on / off, the multiplier and the flow change while playing, in
  either game (`platform::setFrameGen`; a switch makes the swapchain again, the flow a new chain). Since 2026-11-18 also
  with a `Lossless.dll` the launcher got after the game started: the device asks for the shaders' features
  (`shaderFloat16`, storage image formats, `VK_EXT_robustness2` null descriptors, `VK_GOOGLE_display_timing`)
  whenever the GPU has them, DLL or not, and the panel names the launcher's copy (`nativeFrameGenDll` ->
  `platform::setFrameGenDll`) before it switches on; `fgCheckLive` then reads the DLL (once per path) and makes the
  frame generation. `VCS_LSFG_FEATURES=0`: the features only with a DLL at the start (as before). Not checked on a phone.
- **Launcher**: the "Frame generation" category: *Find Lossless.dll in a folder…* (a one-time read grant on one folder,
  `ACTION_OPEN_DOCUMENT_TREE` opened at Documents; searched for the DLL, copied in, searched again by itself when the
  copy is missing) or *Choose Lossless.dll…* (the system picker; copied to
  `files/lsfg/Lossless.dll`, checked as an MZ file of 100 KB or more; picking it turns frame generation on), *Turn on /
  off*, the multiplier (2x / 3x / 4x) and the flow (25 / 50 / 75 / 100 %). SharedPreferences `lsfg`, written with
  `commit()` (the game's `:game` process reads them when it starts). Changes apply at the next start.
- **Where in the frame**: the input is the window's finished back buffer (the presenter's last pass: the letterboxed
  picture and Dear ImGui; the touch overlay is a Java view, not in it). At `present()` (`framegenPresent`) the backend
  blits swapchain image A into the chain's input (RGBA8; the swapchain gets `TRANSFER_SRC` for it), records the passes
  into the frame's command buffer and submits it. Each generated frame then gets an image of its own: acquired, its
  output blitted in, presented; image A, the game's frame, is presented last by `present()` as before. The frame's
  fence is its last submission, so the frame slots and their garbage work as before.
- **Pacing**: the swapchain is **FIFO** while frame generation is on (whatever Display > Vertical sync says) and has one
  image more than the surface's minimum (A and a generated frame's image are held at once). With
  `VK_GOOGLE_display_timing` every image gets a display time one *hold* after the one before (hold = the game's measured
  frame interval / images per frame, snapped to whole refreshes within 0.15, at least one refresh), half a refresh early;
  FIFO's acquires make the game thread wait for the display. Generated frames are only made while there is a refresh
  for them (the earlier port's cap): a 60 fps game on a 60 Hz panel gets none, so the window asks for the output rate
  (`ANativeWindow_setFrameRate` through `platform::setWindowFrameRate`, `rhi::VulkanWindow::setFrameRate`: up at once,
  down after 2 s) and the refresh is read again (`vkGetRefreshCycleDurationGOOGLE`) every 120 frames.
- **With the game's own settings**: Display > Frame rate cap 30 -> x2 shows 60, x4 120; cap 60 -> x2 120 (needs a
  120 Hz panel: the log names the refresh). The game's own frames keep their rate and timing (frame-rate independent or
  not); the input delay grows by about one game frame (the generated frames come from the frame before and this one).
  Higher caps than the panel / multiplier allow just leave nothing to generate. Menus and Dear ImGui are interpolated
  too (they rarely move). Loading screens (> 100 ms frames) reset the pacer: generation stops, then starts again after
  a steady second.
- **Cost**: the chain at the swapchain's size (2420x1080 on the cover screen; a foldable's unfold makes it again,
  ~0.3 s): about 190 images of its own (the earlier port measured ~300 ms to build), the shared passes each game frame, the generate
  passes per generated frame, plus a full-screen blit in and one out per generated frame.

**Log** (`adb logcat -s vcs`): `[android] frame generation asked for: x2, flow 50%, Lossless.dll N bytes` (Java), then
`[lsfg] frame generation: 25 shaders from .../Lossless.dll`, `[lsfg] frame generation on: FIFO present, paced with
VK_GOOGLE_display_timing`, `[lsfg] display refresh 120.00 Hz`, `[lsfg] frame generation: chain for WxH made in N ms`,
`[lsfg] window frame rate 120 fps asked for`, and **`[lsfg] frame generation active: 1 generated frame(s) per game
frame...`** once it runs. Why not: `[lsfg] frame generation unavailable: <reason>` (no Lossless.dll, not a PE / lacks
the shaders, the GPU lacks shaderFloat16 / storage-image features / null descriptors, no TRANSFER_SRC on the window's
images, the chain could not be made, not in this build) or `frame generation on, not generating: the display runs at
60.0 Hz...` / `waiting for a steady frame rate`.

Ran on the test device 2026-11-17 with a player-supplied DLL: frames are generated at 120 Hz when the phone's refresh
rate setting allows it (the system otherwise caps the panel at 60); presents still mix 8 and 16 ms, and the phone runs
hot at 2x resolution.

## Power and heat

Measured on the test phone, 2026-11-17: thermal status *severe* within 1-2 minutes at 2x resolution and 60 fps, the CPU
capped to 1.3 GHz, often on a charger. The GPU took about 10 ms of each 16.7 ms frame. Where the power goes, as far as
the code shows it, from the largest cost down:

1. **Frames nobody sees.** `display.fps` goes up to 240 or Unlimited (`main.cpp`, Frame rate cap), and Vertical sync is
   Off by default outside macOS (`kVsyncDefault`, `main.cpp`). Off means MAILBOX (`rhi_vulkan.cpp`, the swapchain's
   present mode), so present never waits. A phone with a 240 cap showed 111-203 fps on its 120 Hz screen: the GPU drew up
   to twice the frames the panel could show. **Fixed**: on Android the game's frame rate never goes above the panel's
   refresh (`effectiveFps`, `vcs_framerate.cpp`, `[android] power`), or the refresh over the multiplier with frame
   generation. The refresh comes from `VCSActivity` at the start and at each display change (`nativeDisplayRefresh`,
   `power.cpp`). SDL reads it only when the surface changes, so it missed mode switches. 240 / Unlimited / Display
   refresh all behave as "Display refresh" on a phone now.
2. **Internal resolution.** 2x draws four times the pixels of 1x, and the 3D frame is most of the GPU's 10 ms.
   `display.scale` changes live since 2026-11-18 (`gsHwSetInternalScale`: the targets are made again at the new scale
   between two frames; MSAA, texture filtering and game timing too), so a hot phone could step it down while playing
   (not done: the step-down only lowers the frame rate). Suggested: 2x on the folded 2420x1080 screen
   (896 lines on a 1080-line panel). 1x for long sessions or play on a charger. On the unfolded 2172x2352 screen 2x is
   still well under the panel's resolution, and more than 2x is not worth its heat. To be measured: 1x vs 2x, below.
3. **The panel at 120 Hz.** Without frame generation the game asked for no display mode, so a phone whose refresh setting
   is "High" could run the panel and the compositor at 120 Hz for 60 (or 30) frames. **Fixed**: the window asks for the
   host frame rate (`ANativeWindow_setFrameRate`, `voteWindowFrameRate` in `platform_sdl3.cpp`). `VCSActivity` also asks
   for the 60 Hz mode when the cap is 60 or less (`preferDisplayMode`). A higher cap, or frame generation, asks for the
   fastest mode.
4. **Frame pacing spun.** `platform::waitNextFrame` (the main thread, every frame) and the game's limiter at a 30 fps
   cap (`limiterWait` -> `platform::delayPrecise`) used `SDL_DelayPrecise`. In SDL 3.4 (`SDL_timer.c`) that sleeps 1 ms
   at a time, about 16 wake-ups per 60 Hz frame, then spins for up to the worst oversleep it has seen. That keeps a core
   awake and its clock up. **Fixed**: on Android it is one `nanosleep` to the frame's time. Frame deadlines are
   absolute, so a late wake-up (~50-100 us of timer slack) does not add up.
5. **No hints to the system.** **Added**: ADPF (`src/android/power.cpp`). One performance hint session covers the main
   thread, `GameThread`, `RenderWorker` and `GsStage`, found by name in `/proc/self/task`. Its target is the host frame
   period. Each host frame reports the busiest of those threads' CPU time since the last frame, read from per-thread CPU
   clocks. The system can then pick the lowest clocks that keep every thread inside a frame. On Android 15+ the session
   prefers power efficiency while the game is cooling. Setting: Display > Phone heat > Performance hints (on;
   `VCS_ADPF=0` turns it off for one run).
6. **Nothing reacted to heat.** **Added**: Display > Phone heat > When the phone gets hot, "Drop to 30 fps" by default
   (`src/android/cooling.cpp`).
   - **Triggers** at thermal status *moderate* or worse, or when the 10 s headroom forecast reaches 0.95 (1.0 is where
     Android throttles).
   - **While held**, the frame rate cap is 30. It is changed live and not saved: `vcs.ini` keeps the player's cap.
   - **Releases** after a minute of status *none / light* with headroom under 0.80; the player's cap then comes back.
   - **The player's own cap wins.** If the player changes the cap while it is held, the step-down waits 5 minutes before
     acting again.
   - **Where it shows**: the stats overlay's HEAT chip (`· 30 fps`, pink) and the log (`[power] cooling on/off`).

   Thermal readings use `AThermal` (NDK): the status once a second, the headroom every 5 s. The headroom is NaN when
   asked more often.
7. **Running in the background.** Game > In the background was "Keep running", so a game left with Home kept its threads
   and its sound going. **Changed**: Pause is now the default on Android (`main.cpp`, `[android]`). An existing `vcs.ini`
   keeps its saved value; set it in the menu.

Looked at and left alone:
- **Vsync**: with the cap held to the refresh, MAILBOX no longer draws extra frames. Vsync On (FIFO) is the player's
  choice. It may pace more evenly; measure it below.
- **At a 30 fps cap the main loop still presents 60 times a second**: host rate `max(60, cap)`, `applyPresentRate` in
  `vcs_framerate.cpp`. The extra presents redraw only the window pass (the letterboxed picture and Dear ImGui), not the 3D
  frame. Presenting only new frames would mean a runtime change.
- **Bounded spins** of the render worker and the GS stage before they block (`ps2_render_worker.cpp`: 4000 / 2000 /
  4000 `yield`s). On arm64 `yield` is a hint, so these spins are a few microseconds each.
- **paraLLEl-GS**: its spin threads never run on Android (`PS2X_GS=hw`).
- **The EE scheduler** waits on condition variables (`EeScheduler.cpp`: pacing to the VBlank, idle `waitForEvent`). The
  stream reader, the SDL audio callback (about 47 a second) and the log pipe are event driven. The profiler, watchdog
  and rig run only when their environment variables are set.
- **The stats overlay** reads twice a second (sysfs, binder calls) and is off by default. GPU timestamps are written only
  while its GPU chip is shown. MSAA is off by default. No readback per frame unless `PS2X_FRAME_STATS` is set.

The stats overlay's BATTERY chip now also shows the power draw (battery current x voltage). It only means something
unplugged: on a charger it is the charge current.

### Switches for A/B runs: files/env.txt (2026-11-18)

The desktop takes the runtime's switches (`PS2X_*`, `VCS_*`, `LCS_*`, docs/LCS.md "Switches") from the environment; the
phone has none to set. `src/android/game_main.cpp` reads `files/env.txt` (one `NAME=value` a line, `#` comments) before
the game starts and sets each name that is not set already (logged: `[android] env.txt: NAME=value`). With a debug
build (run-as):

```
python3 tools/android.py env PS2X_CHAIN_ASYNC=0 LCS_RSL_NATIVE=0   # write files/env.txt (replaces it)
python3 tools/android.py env                                       # show it
python3 tools/android.py env clear                                 # delete it
```

It takes effect at the next game start (quit the game, Play again). The perf overlay (`graphics.perf_overlay`, F3) has a `THREADS ms worker / gs stage` line since then: with `EE ms` it shows which of the game thread, the render
worker and the GS stage holds a frame back. The ADPF session (Power and heat, item 5) also takes the `ChainWalk` thread
(the asynchronous VIF1 chain walk, PERFORMANCE.md 37).
### Measuring (adb, before / after)

Run each case for 5 minutes from a cool phone (thermal *none*), unplugged, at the same spot in the game, the same screen
(folded or not) and the same brightness. Change one thing at a time: the build, `display.scale` 1x / 2x,
`display.fps` 30 / 60, `display.vsync` 0 / 1, `android.adpf` 0 / 1, `android.hot` 0 / 1 (in `files/vcs.ini`, or in the
settings menu).

```
# the log: [power] lines (ADPF session, thermal changes, every 30 s the busiest thread's ms a frame), [platform] window rate
adb logcat -v time -s vcs | grep -E '\[power\]|\[platform\] window|\[stats\]|\[framerate\]'

# thermal status and the sensors' temperatures (every 30 s)
while true; do date +%T; adb shell dumpsys thermalservice | grep -E 'Thermal Status|mType=(0|1|2|3)' | head -12; sleep 30; done

# power draw: current (uA on most phones; the sign differs) x voltage (uV), every 5 s, unplugged
while true; do echo "$(date +%T) $(adb shell 'cat /sys/class/power_supply/battery/current_now /sys/class/power_supply/battery/voltage_now' | tr '\n' ' ')" \
  | awk '{printf "%s  %.2f W\n", $1, ($2<0?-$2:$2)/1e6*$3/1e6}'; sleep 5; done
# (or: adb shell dumpsys batterystats --reset before a run, then adb shell dumpsys batterystats | grep -A3 'Estimated power')

# the panel's refresh rate while the game runs (60 expected at a 60 cap, without frame generation)
adb shell dumpsys display | grep -iE 'mActiveSfDisplayMode|renderFrameRate|mActiveRenderFrameRate' | head
adb shell dumpsys SurfaceFlinger | grep -iE 'refresh-rate|frame rate' | head

# the hot threads (the game runs in the :game process); -d 5: every 5 s
adb shell top -H -d 5 -n 3 -p $(adb shell pidof com.batinium.gtastories:game)

# CPU clock caps and GPU load / clock (Adreno: kgsl, readable on many phones)
adb shell 'for p in /sys/devices/system/cpu/cpufreq/policy*; do echo $p $(cat $p/scaling_cur_freq $p/scaling_max_freq); done'
adb shell 'cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null'

# frame pacing: the stats overlay (FPS / MS / GPU chips), or frame times from SurfaceFlinger's SurfaceView layer
adb shell dumpsys SurfaceFlinger --list | grep -i vcsrecomp
adb shell dumpsys SurfaceFlinger --latency '<the SurfaceView layer name from the line above>'
```

Note per run: minutes to *moderate* and to *severe*, average watts, fps (shown and the game's), the CPU cap after 5
minutes, and the `[power] last 30 s` lines (busiest thread ms a frame). `dumpsys gfxinfo` covers only the app's Android
views (the overlay and panel), not the game's Vulkan surface: use SurfaceFlinger's latency for the game's frames.

To check on the phone (not measured yet):
- **ADPF**: `[power] ADPF session: 4 threads` in the log. Do the CPU clocks drop with it on and the frame rate hold?
- **Display mode**: the panel at 60 Hz at a 60 cap when the phone's refresh setting is High (`dumpsys display`).
- **The frame rate clamp**: a 240 cap on the 120 Hz screen gives 120, not 111-203.
- **Pacing after the nanosleep change**: the FRAME chip's max, no new hitches.
- **The step-down**: it engages at *moderate* and comes back to 60 after a minute of cool. Does the panel stay cool
  enough at 30?

## Saves: import and export

**Slots with pictures.** The launcher's Saves list starts with the card's slots (`Slot N`, the mission name, the time
of its picture); the focused slot's picture sits over its help text. A PS2 save holds no picture and the game's Save
menu covers the screen as a slot is written, so `save_shots.cpp` takes them before: every
20 s of free play (after 5 s of it, so not a load's fade-in; not in a menu, a cutscene or with the settings / trainer
menu open) into `<data folder>/save_shots/latest.png` (`files/userdata`, LCS: `files/userdata-lcs`; the code is `game/save_shots.cpp`, shared with LCS (`lcs_save.cpp`) and the desktop builds, `VCS_SAVE_SHOTS=0` = none; a pause longer than 0.4 s without game frames stops them), and once more as Save game here / Quick save start
(`fresh.png`). A slot saved gets the matching one as `slot<N>.png`; a safehouse save with no picture from this run
removes the slot's old one. The pictures stay out of the save, so exports are unchanged. Window-sized PNGs, written
off the game thread (`ps2control::requestScreenshot`).

**Start from a slot.** Tapping a slot row asks "Start from slot N?" and starts the game with that slot loaded instead
of the newest (Play's behaviour, unchanged). While the game runs it says so (quit first, or Load Game in the game).
`VCSActivity` passes the intent extra `loadSlot` on as `VCS_LOAD_SLOT=<1..8>`; `vcs_save_anywhere.cpp` hooks the
game's newest-slot choice (`FUN_002762a8`, which compares the slots' header dates with `FUN_00274c98`) and answers
the comparison for the chosen slot - in memory only, the card is never written. Off after the boot's load; a slot not
on the card loads the newest and logs it (`[save-anywhere] boot load: ...`). Any platform can set the variable.

The memory card is a folder tree, `files/userdata/mc0/<save folder>/<files>` (`VCS_DATA_DIR` = `files/userdata`). The
game's save folder is `BASLUS-21590GTA40001`: the data file of the same name (179,350 bytes), `icon.sys`, `vcsicon.ico`
and one file per slot (`1 Soldier`, 98,304 bytes). The launcher's **Saves** category (`SavesMenu.java`) brings saves in
and out with the system file picker (`ACTION_OPEN_DOCUMENT` / `ACTION_CREATE_DOCUMENT`), so no storage permission.
All three rows are greyed out while the `:game` process runs (`VcsIni.gameRunning`), because the running game would
write over the card.

**Import a save…** reads any of these. The parsing (`PS2Save.java`, `Lzari.java`) is ported from
[mymcplus](https://git.sr.ht/~thestr4ng3r/mymcplus) (mymc+ by Florian Märkl, after mymc by Ross Ridge; GPL-3.0):

| Format | Magic | Inside |
|---|---|---|
| Max Drive / AR Max `.max` | `Ps2PowerSave` | LZARI (Okumura), per file `size, name[32]` + data, 16-byte alignment |
| CodeBreaker `.cbs` | `CFU\0` | RC4 with a fixed key state, then zlib; 64-byte file headers |
| EMS / uLaunchELF / PCSX2 `.psu` | none (3 directory entries: folder, `.`, `..`) | the card's 512-byte entries, data padded to 1 KB |
| PS3 `.psv` | `\0VSP`, type 2 | a PS2 save (type 1, PS1, is refused) |
| `.zip` | `PK` | a save folder at any depth (a folder holding `icon.sys`; `tools/backup.py` zips work too), or any file above |

The file name doesn't matter: the contents decide. Then:

- The folder must start with `BASLUS-21590` (the US PS2 disc). Anything else is refused and the player is told what it
  is, for example "This is a save for a PS2 disc from Europe (SLES-54622)…" or "…for the PSP (USA, ULUS10160): PSP
  saves don't work on the PS2 version…". The region comes from the folder name's second letter (A / E / I…) or the PSP
  prefix (UL / UC / NP).
- If the card already has the folder, a dialog shows the slots on the card and in the file, and offers **Replace** or
  **Cancel** (Cancel has the focus). A PS2 game has one save folder with a fixed name, so "keep both" isn't possible.
  The old folder is copied whole (with its `.ps2x-order`) to `files/userdata/backups/<yyyyMMdd-HHmmss>/`. That sits
  next to the runtime's own `auto-*.zip` backups, which the runtime's pruning leaves alone.
- `SaveCard.install` writes the new folder to `files/userdata/.import-tmp/` and then moves it into `mc0` in one step.
  File names are stored as the runtime stores them (`encodeMcName`: `:` and the like as `%XX`, so `1 Kill Phil: Part 2`
  becomes `1 Kill Phil%3A Part 2`). Each file's time comes from the save where the format has one. Last comes
  `.ps2x-order`: `icon.sys`, `*.ico` / `*.icn`, the data file named like the folder, the slots by number, then the rest
  by name. VCS only looks for slots from the sixth listing entry on (game-internals.md 10c). A save copied in by hand
  had `1 Soldier` listed first, and the game never saw it.

**Export as .psu…** writes the folder (in card order, with the real card names) as an EMS `.psu`, named
`GTA VCS save <date of the newest file>.psu`. mymc / mymc+ put it on a PCSX2 `.ps2` card, uLaunchELF copies it to
a real PS2's card, and most PS2 save managers read it. **Export as .zip…** writes `BASLUS-21590GTA40001/<files>` with the host-encoded names, so it unzips on Windows
too. A PC build of the game takes it in its `mc0` folder. Before anything is written, each export is read back through
the importer.

The runtime also sorts files that are missing from a folder's `.ps2x-order` (hand copies) in that same order instead
of by creation time: `MemoryCard.cpp` `listMcDirOrdered`, a row in PS2RECOMP_PATCHES.md.

**Check on the JVM** (no device, no Android SDK): `python3 tools/android_saves_check.py [<saves in> <expected out>]`
compiles `PS2Save` / `Lzari` / `SaveCard` and `android/app/src/test/.../SaveFormatsCheck.java` with the JDK. It
checks name encoding, region messages, card order, `.psu` / `.zip` round trips, a `.psu` inside a zip, and an install
into a temporary card: the order file, the replaced save's backup, and an export from the card read back in. With two
folders it also decodes every sample and compares each file byte for byte with a reference unpack
(`<expected>/<id>/<folder>/<file>`, `<id>` = `name.<id>.ext`). Tested 2026-11-17 on three `.max` and one `.cbs` of VCS
USA, unpacked by mymcplus: all files identical, and they round-trip through `.psu` and `.zip`. mymcplus also reads an
exported `.psu` back identically. Sample saves are other people's files: never commit them.

## Mods

A first mod loader (`src/game/vcs_mods.{h,cpp}`, `src/game/vcs_modconv.{h,cpp}`, `ModFormats.java`,
`ModsMenu.java`) for PC mods of GTA III, Vice City and San Andreas, into the **existing** vehicle and weapon slots:
- **tuning**: `handling.cfg` and `weapon.dat` lines, converted to VCS's units and written into the game's tables;
- **models with their textures**: a mod's `.dff` + `.txd` is converted on the phone at game start into VCS's PS2
  formats and streamed in place of the slot's stock model. Weapons and cars both work (checked on the phone: VC's
  Cosmo Dragoon python in the Equalizer slot, GTA III's Ferrari F50 in the Stinger slot, both drawn textured in game).
Not used yet: collision and a mod's own wheel model (the slot's stays, sized to the mod's wheel). On LCS also: the
mod's light / seat / exhaust dummies, its hinges and its `carcols.dat` colours (below, "What converts").

**The mod manager (both launchers, both games, 2026-11-18).** The desktop launcher's *Mods* sheet (title bar;
`launcher/screens/sheets.cpp` modsSheet, all logic in `tools/mods.py`) and the Android launcher's Mods category
(`ModsMenu.java` + `ModFormats.java`) do the same things on the same storage, per game (the shown game's data folder):
- **Import**: a `.zip`, a `.vcm`, loose files (Android: several picked together; desktop: a file or a folder) -
  `handling.cfg` / `weapon.dat` lines, readmes, a `.dff` with its `.txd`, a `.gxt` (`replace disc=TEXT/<NAME>.GXT`).
  `.rar` / `.7z` are refused with a hint (unpack, import the folder or zip it).
- **Installed**: each mod, On / Off, Remove, and per line or model: what it changes in the player's words (a
  `handling.cfg` / `weapon.dat` line changes how it **drives / fires**, a `.dff` + `.txd` how it **looks**), the slot it goes
  into, **how the slot was found** (*matched by name*, *alias*, *nearest equivalent (class)*, *picked by you*), its state
  from the game's `applied.txt` (applied / skipped and why / applies at the next start), and its values with the stock
  ones the game reported (`(was X)` lines; kept in `mods/stock.json`).
- **Slot picker** (*Change slot*): the shown game's own vehicles / weapons by name (LCS's from LCS, VCS's from VCS).
- **What is modified**: every slot changed, by which mods; **conflicts** (two enabled mods' entries on the same slot and
  kind - a car's handling and its model are different slots) say which wins (the later in load order, value by value)
  and let the player pick one: the others' entries on that slot are *set aside* (`"off": true`, left out of
  `active.txt`).
- **Import or keep where it is** (both launchers, asked each time a mod is added, like the disc image):
  - *Import (the app keeps a copy; you can delete the original)*: `mods/<id>/source/` holds ONE copy of every picked
    file (size and SHA-256 in the manifest). The game reads models and texts straight from there - a loose file, or
    the entry of a `.zip` (stored / deflated) or `.vcm` (`<id>/source/pack.zip!Box%20Pack/kuruma.dff`). `files/` only
    holds what it can't reach there (a `.vcm` inside a zip). Imports from before this kept a second copy in `files/`:
    the manager moves them over once (each `files/` file whose bytes are a source's file or entry gets
    `"from": {"source", "entry"}`, active.txt is rewritten, then the duplicate is deleted; the bytes are the same, so the
    converted-model cache stays valid).
  - *Keep where it is (the app only reads it; don't move or delete it)*: manifest `"storage": "in_place"`; nothing is
    copied, the app keeps the manifest and `converted/` only. Each source records where it is: Android `"uri"` (a
    persisted read permission, `takePersistableUriPermission`, released on Remove / switch), desktop `"path"`.
    Android: ModsMenu writes `mods/places.txt` (`<id>/<n> <uri>`), active.txt names `place:<id>/<n>`;
    `VCSActivity` opens each document at the game's start (as it does the disc) and passes `VCS_MODS_PLACES=<id>/<n>=<fd>
    ...`; the loader reads `/proc/self/fd/<fd>`. Desktop: the absolute path (`%XX` escapes) in active.txt.
  - *Original missing*: each in-place mod's active.txt block starts with `needs file=<path>` lines; one that can't be
    opened makes the game skip the whole mod (`<id>: skipped: original missing (...)` in applied.txt, never a crash).
    The menus show *original missing* (Android: permission gone or the document can't be queried; desktop: the file
    isn't there) or *original changed* (another size / hash) and offer *Find it again* (pick the same file - matched by
    SHA-256 - where it is now) and *Import a copy instead*. A different file is refused ("add it as a new mod").
  - *Switching*: an imported mod can be pointed at its original again (*Use the original instead…*: pick it; the copy is
    deleted after active.txt is rewritten), an in-place one imported (*Import a copy*: read, hash-checked, copied).
  - *Cache*: `converted/<game>-<id>.src` next to `.key` holds the sources' stamps (size, modification time, entry
    name) and the key; when they and the key's game / converter / stock lines still match, the converted model is
    taken without reading the `.dff` / `.txd` at all - a kept zip is not opened at a start that changes nothing
    (`cached (...; sources unchanged, not read)`). A missing source is never served from the cache.
  - Code: `src/game/vcs_modfiles.{h,cpp}` (paths, zip / .vcm entries, stamps; zlib's inflate through
    `platform::inflate`).

**The slot tables are one file**, `tools/modslots/mod_slots.txt` (plain text: `<game> vehicle|weapon <id> [<model id>] |
<model name> | <label> | <class> | <aliases> | <similar>`), read by `tools/mods.py` and by `ModFormats.loadSlots()` (the
APK's `assets/mod_slots.txt`, `android/app/build.gradle` adds `tools/modslots` to the assets). Names are compared
lower-case without punctuation. Aliases: the same vehicle / weapon under GTA III / VC / SA names or the game's own text
key (LCS `LANDSTK`); *similar*: a vehicle / weapon the game lacks, into the nearest of the same class (SA `sultan` ->
VCS Sentinel XS / LCS Kuruma, `desert_eagle` -> Equalizer / .357). LCS's table was read from the game in a run
(model infos `*0x3D8720`, +0x10 type 6, +0x2E key; weapon info `*0x3D9B48` + 4, 0x70 per type, +0x60 model): vehicles
130-216, weapon types 1-36 with their models (pistol 17 / 274, .357 18 / 275, shotgun 19 / 277).
`tools/android_mods_check.py` runs ModFormats on the PC against the same file (both games).

`tools/mods.py --game lcs|vcs list|import [--in-place]|relocate [--copy]|store copy|in_place|enable|disable|remove|retarget|prefer|slots|loader|status [--json]`
(`--data DIR` for another folder; with `VCS_PROGRESS=json` its answer is an `@vcs {"result": ...}` event, which is how the
launcher reads it). Manifests are shared: `loader: 2` adds `game`, per entry `match` and `off`, and `sources` / `files`.

**The launcher's Mods category.** *Import a mod…* (the system file picker, `ACTION_OPEN_DOCUMENT`) takes a `.zip`, a
VC Mod Manager `.vcm` (also inside a zip), or a loose `handling.cfg` / `weapon.dat` / readme. `.rar` / `.7z` are
refused with a hint (unpack on a PC, zip the files). The importer scans every text file for lines in one of the three
`handling.cfg` dialects (told apart by column count: GTA III 31 values, VC 32, SA 34-35; the transmission letters must
sit in columns 15 / 16) and for `weapon.dat` lines (a fire-type word in column 2). It matches the line's name to a VCS
slot of the shown game (`tools/modslots/mod_slots.txt`, above): model names, plus aliases like `police` -> `policem`
(207), `colt45` -> Pistol (19), `python` -> Equalizer (20), and nearest equivalents. When nothing matches (the Audi .vcm's `SABRETUR`, SA's `CADRONA`), the player picks a vehicle or weapon. A mod with
no usable line is refused, and the dialog lists what it holds and why it is unused. Each imported mod gets a row
(`Mod N`: name, On / Off, its slots). Its dialog offers Turn on / off, the slot for each line, Details (the values
used, the columns not used, the notes, and what the game did with it at its last start), and Remove (deletes the
app's copy only). *Mod loader* Off removes `active.txt`, which gives the stock game. *At the last start* shows the
game's `applied.txt`. The side panel (Game section) has a read-only "Mods: N on" line.

**Storage.** `files/userdata/mods/<id>/manifest.json` (name, source file, imported time, enabled, and per line: kind,
dialect, the original line, target slot, the values used, the columns skipped; plus notes; `storage`, `sources` with
`uri` / `path`, `files` with `from`). An imported mod's picked files are kept in `<id>/source/`; a mod kept where it is
has no copy (above). `mods/active.txt` is regenerated on every change: the enabled mods' lines in import order, in PC
units (format in `vcs_mods.h`). The game writes `mods/applied.txt` and logs `[mods]` lines. Mods are third-party files:
they never go into the repository, and `tools/android_mods_check.py` tests the importer on lines made up for the test
(pass mod files as arguments to see what an import would find).

**Runtime.** `FUN_00324A50` relocates a loaded GTAG image. Once GAME.DTZ's image is relocated (recognised by its 7941
model infos at +0x38), `vcs_mods.cpp` reads `active.txt`, converts each value and writes it into the tables in guest
RAM. The first write to each word keeps the original in host memory (`vcs::mods::revert`). The tables come from the
image header: +0x3C model-info pointers (vehicle model info +0x38 = its handling record), +0x7C the streaming object
(+200 the streaming info array), +0xC0 a pair of pointers to the two weapon tables. Each is validated first: the record
is a vehicle's, with a drive letter and 1-5 gears; the weapon table is one whose pistol record holds the stock values.
Values outside a sane range are clamped and the clamp is logged. The GTAG image is loaded fresh every start, so a mod
turned off is simply not applied the next time.

**The handling field map** (offsets in the 0xE0-byte record; all 111 vehicle records were measured from the user's
GAME.DTZ):

| Off | Field | Conversion from handling.cfg | Evidence |
|---|---|---|---|
| +0x00..+0x47 | gear table, 6 x {max velocity, shift up, shift down} | rebuilt with VC's `InitGearRatios` (base = max / 2 / gears, step = (max - base) / gears; shift up = prev + diff x 0.6667, next shift down = prev + diff x 0.42) | reproduces all 111 stock tables from +0x50 / +0x58 / +0x5E to 2e-7 |
| +0x48 | engine acceleration | cfg / 2500 / 4 (4WD) or / 2 | x 2500 every stock value is a round number; with / 4 for 4WD the medians match (4WD 9.6, RWD 10, FWD 7.2); re3's formula |
| +0x50 / +0x54 | max velocity / cruise velocity (= max / 1.2) | **approximate**: both scaled by new cfg speed / the slot's cfg speed | max / cruise = 1.2 and cfg / 180 - cruise = 0.01 x an integer in all 111 records (VC's drag loop); the loop's drag input is not in the record, so it can't be recomputed |
| +0x58 | reverse velocity | kept (-0.2 in all) | |
| +0x5C / +0x5D / +0x5E | drive (F / R / 4), engine (P / D / E), gears | as is (gears 1-5) | ASCII letters in every record |
| +0x60 | brake deceleration | cfg / 2500 | x 2500 gives round cfg numbers (9.1, 8.0, 13.1...); read through the vehicle's handling pointer (vehicle +0x160) at 0x339EA4 |
| +0x64 / +0x68 | brake bias, steering lock (degrees) | as is | 0.44-0.6; 27-40 degrees; +0x68 read at 0x33A5A0 |
| +0x6C / +0x70 | traction loss, traction bias | as is | VC's field order after the steering lock; internals: +0x6C is what a skidding wheel loses |
| +0x74 / +0x78 | suspension force, damping | as is | read at 0x339434 / 0x339BA0 |
| +0x7C..+0x88 | suspension upper, lower, bias, anti-dive (VC only) | as is | VC order, values like VC's |
| +0x8C / +0x90 | seat offset, value ($, int) | as is | |
| +0xA0..+0xA8 | centre of mass | as is | |
| +0xB0 / +0xB4 | percent submerged (int) / buoyancy | buoyancy = 100 / submerged x 0.008 x mass | the formula reproduces all 111 records |
| +0xB8 | max speed in cfg units | as is | 180, 200, 160... in km/h-like cfg units |
| +0xBC / +0xC0 | mass / turn mass | turn mass: SA's column; GTA III / VC: (dimx^2 + dimy^2) x mass / 12 (VC); a new mass alone scales it | ADMIRAL 4056.4 and POLICEM 4351.7 come out of round dimensions (2.1 x 5.4, 2.2 x 5.0); internals: mass is read as such |
| +0xC4 | traction multiplier | as is | read at 0x33A380 and multiplied by 0.004 x 0.25: reVC's `0.004 * fTractionMultiplier / 4` |
| not written | +0x94 lights, +0x98 collision damage, +0xC8, +0xCC / +0xD0 flags, ABS | | units or bit meanings not confirmed |

Per mod the details list the columns not used (ABS, collision damage, flags, lights; SA's drag, inertia, high-speed
damping, anim group). Bikes, boats and aircraft have the same base record (their extra records were not looked at);
a car's line on a boat slot is allowed but makes little sense.

**Weapons.** Two tables of 40 x 0x70 bytes (GTAG +0x3D7530 / +0x3EE410, pointed to from header +0xC0); they differ
in a few values (unarmed range 30 / 10, some fire offsets), so both are written. Per record: +4 fire type, +8 range,
+0xC firing rate, +0x10 reload, +0x14 clip, +0x18 damage (ints from +0xC), +0x1C speed, +0x20 radius, +0x24
lifespan, +0x28 spread: `weapon.dat` columns C-K (the pistol record holds VC's range 30, clip 17, damage 25). Fire
type, offsets, animation timings, models, slot and flags are kept (they drive animations, sounds and the HUD). A float
field the weapon doesn't use (-1 in stock) is left alone.

**Save safety.** The loader only changes values in the GTAG tables, in RAM, after GAME.DTZ is loaded. It creates no
model id, weapon type or streaming index, and writes nothing to the disc, the memory card or GAME.DTZ. Handling and
weapon tables are not part of a save: the GTAG image is 6.5 MB of static game data and a slot file is 98,304 bytes of
game state. A save holds stock model ids and weapon types only (the slots exist with or without mods), so loading
it with mods off, or after a mod is removed, gives the stock car or weapon. Vehicles already spawned keep a pointer
to their handling record. Mass and turn mass are copied at creation (VC behaviour), so they show on new spawns.
Colours are not touched, so a garaged car's stored colour indices stay valid. A converted model only replaces the
bytes a stock entry streams; the slot's id, model info, collision and handling record stay, so a save that stores the
car or weapon loads the stock one when the mod is off.

**Models and textures.** VCS streams models (`ldm`, MDL) and texture lists (`xet`, XTX) from `GTA3PS2.IMG` through
the streaming info array: index = model id for models, **7534** + the model info's texture slot (+0x1E) for its texture
list (checked on the disc: ADMIRAL 0xA7 -> 7701, the pistol 0x125 -> 7827; MOD_LOADER_RESEARCH.md said 7539).
- *Serving.* An entry is repointed (+0x10 sector, +0x14 count) past the end of the disc, into a range a disc wrapper
  (`ModDisc` in vcs_mods.cpp, installed with `ps2_disc::setImage`) serves from host memory; every other read goes to
  the player's disc. The game reads the IMG through its own file layer (FUN_00220EC8, handle table 0x5058A8), which
  clamps a seek to the file's size: without more, a redirected entry hung the game when requested. So the IMG's size
  is raised in the disc index (0x5038A8) and in an open handle to cover the served range. Proven with stock data first
  (`stream index=347 copy=346`: the pistol's model and textures served from host memory in the Equalizer slot, loaded
  and held in game), then with converted models.
- *The converter* (`vcs_modconv.cpp`, plain C++; `tools/modconv_check.cpp` runs it on the PC on the player's own
  stock entries and a mod's files and reads the result back) reads a RenderWare `.dff` (GTA III / VC / SA clumps: frame
  list with names, geometry with UVs / normals / materials / texture names; chunks that run past their parent, as in
  truncated mod files, are read as far as they go) and a `.txd` (D3D8 / D3D9: PAL4 / PAL8, 8888 / 888, 565 / 1555 /
  4444, DXT1 / 3 / 5). It is built around the slot's stock entry (the "donor", read from the player's disc at start):
  - *MDL.* A relocatable chunk {ident `ldm\0`, shrink 0, file size, data size, relocation table, count, global table,
    numFuncs}: every pointer is a chunk offset listed in the relocation table; the global table (right after the
    data, numFuncs entries) points at the render-callback field of each element (+0x24), which the game's loader
    fills in - a car has one per element, and keeping only one crashed the game on a JALR to 0x1. Weapon chunks
    (numFuncs 1): element pointer array, materials, element (0x30: node, link into the node's object list, geometry,
    model id at +40), node (0xB0, the donor's), geometry. Strings (texture names) after the relocation table.
  - *Geometry* (RslGeometry 0x20 + sPs2Geometry 0x40 + 0x30 per mesh + DMA): one mesh and one DMA packet (`ret`
    tag) per material. Each packet is VIF batches of at most 70 vertices, overlapping by 2, exactly as the stock
    ones: UNPACK V4-32 of {0, 0, n, 0x8000|n} to VU address 0; STMASK 0x40404040 / STROW 0 and UNPACK V3-16 of
    positions to 1; STMASK 0x50505050 / STROW (0, 0, 0xFF, 0) and UNPACK V2-8 unsigned UVs to 0x8D; UNPACK V3-8 normals
    to 0x47; MSCAL 6; padded to 16 bytes. Positions: int16 / 32767.5 x scale + centre, the scale a power of two per
    axis (as stock); UVs: uint8 / 127.5 x the mesh's uvScale, shifted by whole tiles; normals int8 / 128. Triangle
    strips (greedy, the start triangle tried in three rotations) joined by degenerate triangles, each strip starting
    on an even index so the winding holds; batches start on even indices too. Bounds: sphere, quantised box.
  - *Cars* keep the **whole stock chunk** (nodes with VC's names - chassis_dummy, door_lf_dummy, bump_front_dummy...,
    wheel_*_dummy -, the per-part elements with flag 4 = intact / 0 = damaged, the vehicle data {element group,
    extras, 30 primary / 25 secondary paint materials}) and append new geometries: each element gets the PC part of
    its name (`<part>_hi_ok` / `_hi_dam`, `chassis_hi` plus unnamed body panels, `chassis_vlo`), moved into the
    part node's space; parts the mod lacks get an empty geometry. Wheels are not in the car chunk (the slot's wheels
    stay, which also covers mods made for a PC wheel pack). Paint: materials coloured 60,255,0 / 255,0,175 (VC's
    convention) become the primary / secondary lists, with the stock paint's environment map.
  - *XTX.* The donor's textures (HUD icon...) are kept; the model's are box-filtered to powers of two (weapons <= 128,
    cars <= 64 px), median-cut to 256 colours, written PSMT8 with a CSM1 CLUT (index bits 3 / 4 swapped), alpha 0..0x80;
    raster {0, 0x250000 | width, data, flags = log2 w | log2 h << 6 | 8 << 12 | 1 << 20}.
  - *Budgets.* Quadric edge-collapse decimation (half-edge collapses, borders and UV seams kept) to 1500 triangles
    for a weapon, 3000 for a car (shared between the parts in proportion), the low-detail body 300-400. Sizes seen:
    the python 29.7 KB model + 19.8 KB textures (stock 8 + 4 KB), the F50 179 KB + 24 KB (stock 51 + 10 KB; the
    stock car's own geometry stays in the chunk unused). Both loaded and drew fine; no size limit was hit.
- *In the launcher* a mod's `.dff` / `.txd` become a "model" entry (a car when the model's frames say `chassis` or the
  name is a vehicle's, else a weapon), stored in `mods/<id>/files/`; `model id=<n> dff= txd=` in `active.txt`.
- *The converted-model cache* (both games, every platform): a model is converted at the first start that uses it and
  kept next to the mod, then read back on later starts (`[mods] ... model <n>: cached (...)` in the log and
  `applied.txt`; `converted (saved to ...)` when it was converted). Layout, in the data folder the runtime uses (its
  `mods/`, whatever the app's folder is named):
  `mods/<mod folder>/converted/<game>-<model id>.mdl` (the model chunk served for the slot), `.xet` (its texture list),
  `.txt` (the converter's report), `.key` (what the output depends on, text: `game`, `model`, `converter`
  (modconv::kVersion), `tris`, `tex`, `scale`, then `dff`, `txd`, `stock model`, `stock textures` each as an FNV-1a 64
  hash and a size). `<mod folder>` is the first path component of the `dff=` path; `<game>` is `vcs` or `lcs`. Any
  difference in the key (another source file, slot, budget, converter version or disc) converts again and rewrites
  the four files; the `.key` is written last, so a half-written cache is never used. The cache is enough to run the
  model only together with the `model` line and the `.dff` / `.txd` (the key hashes them): a mod manager can show the
  folder and its size and delete it (one conversion more on the next start), not replace the source files with it.
  `VCS_MODS_CACHE=0` converts again on every start. (Before this, the conversion ran at each start: 25 ms for the
  python on the phone.)
- *Device tests* (not for players): `test give=<weapon type>` / `test spawn=<vehicle model>` lines in `active.txt`
  hand the weapon / spawn the car once free play starts (no script camera for 3 s), logging `[mods] free play:
  testing`, and the served entry's load state is logged.

**Whole disc files** (`replace disc=TEXT/ENGLISH.GXT file=<mod>/ENGLISH.GXT`, both games): the game's own disc index
(VCS 0x5038A8, LCS 0x62AA70) gets the file's LSN and size repointed into ModDisc's host-memory range, so the next
open of that name reads the mod's bytes, any size. `tools/gxt_edit.py` makes a text mod from the player's own GXT.
Checked on LCS; on VCS it is applied at GAME.DTZ's relocation, so only files opened after that (not checked in a run).

**Liberty City Stories.** The same loader runs on LCS from LCS's own data folder (`files/userdata-lcs/mods`, the
launcher's Mods screen's with LCS chosen): every directive works there. `handling` uses LCS's own 0xF0-byte record
(reached through a handling id in the model info; `maxspeed` is the new top speed, LCS keeps no cfg speed; `drag`,
`inertia`, `collision` are LCS-only fields), and `model` converts into LCS's chunk dialect (`Options::lcs`: 8-byte
rasters, 0x54-byte textures, texels at +0x40, node names at +0xA4, 25 + 25 paint materials). LCS's addresses, the
field map and the checks: docs/LCS.md "Mods".

**Sounds and your own music (LCS only, 2026-11-18; docs/LCS.md "Sound mods").** Both are done in LCS's native sound
driver, so they work the same on Android (not heard on a phone yet):
- `sound id=<n> file=<audio file>` (also `bank=<b> index=<i>`, or `name=<prefix of the sound's name on the disc>`,
  optional `gain=<percent>`): an effect sound - a gunshot, a horn, an engine - replaced by a `.wav` (or `.mp3`, `.ogg`,
  `.flac`). It plays at the original sound's own rate and must fit its slot: a longer file is cut to the original's
  length, a shorter one simply ends sooner; a looping sound (engines, sirens) loops. Speech can't be replaced.
  The pistol is ids 99 + 100, the shotgun 275 + 276 (`tools/mods.py sounds <iso>` lists them all). The desktop's
  `tools/mods.py sound FILE --id N` makes such a mod; the Android ModsMenu has no entry for it yet.
- **User tracks**: `.mp3`, `.ogg`, `.flac` or `.wav` files in the LCS data folder's `music` folder (Android
  `files/userdata-lcs/music`) play as a radio station, like Vice City's MP3 player but in a station's place: the setting
  Audio > User tracks picks which station (default LCFR, the talk station), every station, or Off. The station keeps its
  name on the wheel; its DJ and adverts go. News bulletins and mission music stay the game's. Nothing changes while the
  folder is empty. Android has no folder picker for it yet (the files have to be copied into the app's folder).

**What converts (2026-11-18, agent mods3; the manager's rules are shared: `tools/mods.py` = `ModFormats.java`).**
- `handling.cfg`: each line in its own game's columns (gtamods.com wiki "Handling.cfg": GTA III 31 values, VC 32 with
  the anti-dive, SA 34 / 35 with turn mass, drag, engine inertia, high-speed damping, two hex flag words and the anim
  group). Passed on: every value with a place in the game - mass, turn mass (or the dimensions), drag, centre of mass,
  percent submerged, traction x 3, gears, max speed, acceleration, inertia, drive, engine, brakes, ABS, steering lock,
  suspension x 6, seat offset, collision damage, value, the light types and the flags. The flags travel as one word in
  VC's bit layout plus a mask (GTA III 16 bits, VC 27, SA its two words mapped bit by bit to the VC bit of the same
  meaning: IS_VAN, IS_LOW, REVERSE_BONNET, NO_DOORS, DBL_EXHAUST, HALOGEN_LIGHTS, NPC_ANTI_ROLL -> NONPLAYER_STABILISER,
  WHEEL_R_WIDE -> FAT_REARW, OFFROAD_ABILITY -> GOOD_INSAND ...; SA bits with no VC meaning - hydraulics, NOS, axles,
  LOW_RIDER - are reported). The game keeps the slot's vehicle-kind bits (bike, heli, plane, boat). Skipped and
  reported: GTA III / VC's z dimension, SA's high-speed damping and anim group. LCS writes them all (its record keeps
  VC's flag bits); VCS reports `collision`, `drag`, `inertia`, `abs`, the lights and the flags as not mapped (its
  record's places for them are not located).
- `weapon.dat`: GTA III (22 columns) and VC (26) lines: range, rate, reload, clip, damage, speed, radius, life span,
  spread and the fire offset (`firex/y/z`: where the shot and the muzzle flash leave the gun); SA's `$` gun lines: the
  damage range, clip, damage and muzzle flash offset of the std skill line (the poor / pro lines are noted, not used).
  Fire type, animations, models, slot and flags stay the slot's.
- `carcols.dat`: a car line becomes the slot's colour list when the mod brings its own `col` list (the numbers are
  indices into it): each colour goes to the nearest colour of the game's palette, up to 8 pairs (LCS; SA's `car4`
  lines use their first two colours). Without a `col` list the numbers mean the PC game's palette, which isn't here:
  reported as not applied.
- Car models: the body (`chassis_hi`, SA `chassis`), every part the slot has by name - doors, bonnet, boot, bumpers,
  wings, windscreen - with its damaged twin (GTA III / VC `<part>_hi_ok` / `_hi_dam`, SA `<part>_ok` / `_dam`), the far
  model (`chassis_vlo`), the paint (materials in 60,255,0 / 255,0,175 take the car's primary / secondary colour; SA's
  third / fourth colours take those too, its light markers are drawn white). The mod's dummies: the slot's door /
  wheel / bonnet / boot / bumper / windscreen nodes move to the mod's frames of the same name (doors swing on the mod's
  hinges, wheels sit and steer where the mod puts them; the converter does it for both games, checked on LCS); on LCS
  `headlights`, `taillights`, `ped_frontseat`,
  `ped_backseat` and `exhaust` go into the model info, and the wheel's size becomes the wheel scale. Not converted: the
  mod's own wheel geometry (the slot's wheel model is shared with other cars), extras (`extra1..6`), collision, prelit
  colours, SA's extra plugins. Parts the slot has and the mod lacks are left empty (a 2-door mod in a 4-door slot has
  no rear doors).
- Weapon models: drawn in the hand at the PC model's size (both are in metres), the HUD icon stays the slot's (it lives
  in the same texture list and is kept). Cutscenes use their own objects (`cscolt45`, `cs9mm` ... named in LCS's
  ANIM/CUTS.IMG scripts), not the weapon slots, so a converted gun is not seen in cutscenes.

**Not done / limits.** VCS: the colour list is in the model info (+0x161 count, +0x163 pairs of palette indices) but
its RGB palette is not located, so `colours` is LCS only (searched 2026-11-18 in a run: no table of 4-byte colours
with one alpha, 100+ entries and a taxi yellow at index 6 in 0x100000-0x2000000; the per-car pairs reach index 241).
VCS's vehicle model info (read in a run, all 111): +0x54 the class (0 car, 6 bike, 8 BMX), +0x58 / +0x5C the front /
rear wheel scale (SA's two, equal in every stock one: Admiral 0.7, Faggio 0.46, Linerunner 1.1, Rhino 1.5), +0x60 ..
+0xC0 SA's dummy positions 0x10 apart (headlights, tail lights, two secondary lights, front seat +0xA0, back seat
+0xB0, exhaust +0xC0): the converter's light / seat / exhaust positions and wheel size now apply on VCS too (the
"(was X)" values in applied.txt are those read). Not located on VCS: the shared wheel model id (no int16 with a few
wheel ids for the cars and -1 for the bikes in the first 0x200 bytes), so a mod's own wheel is LCS only. VCS's stock
car textures are 4-bit (16 colours, up to 128 x 128); converted ones (8-bit, 64 px) look mottled on some VCS cars
(a III taxi, an SA sedan; a VC sports car's paint-only body looks right), not yet explained.
Max speed is approximate on VCS. Values apply at the next start (no live toggling). Models: collision, prelit /
night colours and SA's extra plugins are not converted; a mod's own wheel is used on LCS only (elements on the car's
wheel nodes, docs/LCS.md "Mods: real PC mods"); bikes convert (their forks / handlebars / wheels onto the bike's own
nodes), boats map `boat_hi` / `boat_vlo` (no boat mod tried); peds, flyable aircraft (LCS has none) and add-on (new)
slots are not done. On Android `.rar` / `.7z` are still refused (the desktop manager unpacks them with 7-Zip or
bsdtar), and ModFormats.java lacks the desktop's newer import rules (IDE lines, weapon-loader .xml, weapon packs,
readme slot hints, versions in folders: tools/mods.py). The strips are about 2.1 vertices a triangle (stock ~1.8), so models are larger than stock. Add-on vehicles or
weapons are not possible without breaking save safety (internal research notes).

## Both cities on Android (prototype)

Status: **design + a build-only prototype** (2026-11-19, branch `droid2`; nothing run on a phone). The PC's "One
program" mode (docs/STORIES.md: `stories.exe` with `lcs_game.dll` + `vcs_game.dll`) carried over to the APK: Liberty
City Stories in front, Vice City Stories behind it in the same `:game` process, Vice City composited on the horizon.

### (a) Two games in one process: the libraries

**The problem.** On the PC each game DLL holds a whole runtime (a DLL has its own copy of everything static). On Android
the runtime is `libmain.so` and a code pack (`libvcs_game.so`) holds only the generated code, linked against it: there
is one runtime per process, and it is one game's (`g_ps2xGameTable`, `PS2Runtime`, the renderer, the GS state, the
settings, every hook's static). Two code packs cannot share it. Worse, `libmain.so` is linked `-z global`
(`DF_1_GLOBAL`, ps2xRuntime/CMakeLists.txt: so that a pack's own copies of the runtime's inline globals bind to the
runtime's) and `--export-dynamic` with every runtime object: it sits in the **global group**, which bionic searches
**before** a library's own local group. So any other library that exports the runtime's symbols with default
visibility (and is not `-Bsymbolic`) would have its own references to them bound to `libmain.so`'s copy at load: two
"runtimes" that are really one. `RTLD_LOCAL` alone does not help (it only keeps the new library out of the global
group; its own lookups still start there).

**What works: a self-contained library per game, everything local.** The same as the PC DLL: each game is built as
`libstories_<game>.so` (`tools/android.py build-game --game <id> --stories`) = the runtime + our `src/` + the generated
code + VU1, the way the personal build makes `libmain.so`, but:

- a **version script** (`src/stories/game_so.map`): `stories_game_main` is the only exported symbol, everything else
  `local`. A local definition is bound at static link time; the dynamic linker never looks the name up, so the global
  `libmain.so` cannot capture it, and the two games' identical names (86 generated `FUN_` names, every runtime and
  ImGui symbol) never meet. Stronger than `-Bsymbolic` (which binds own references first but still exports, so a later
  library could bind to it). Loaded with `dlopen(RTLD_NOW | RTLD_LOCAL)` by the host.
- **SDL3 stays one copy, `libmain.so`'s.** SDL's Java side (`SDLActivity`) talks to the native SDL it loaded through
  JNI (`JNI_OnLoad`, `nativeSetupJNI`, the surface callbacks): a second SDL inside a guest library would never be
  set up (no JavaVM, no activity class) and its video init would fail. The guest is linked without SDL (headers only)
  and with `-z undefs`; its `SDL_*` imports resolve at load to `libmain.so`, which exports SDL (linked whole, default
  visibility) and is in the global group. So both games share one SDL: one window, one event queue, one audio
  subsystem - what one surface wants anyway, but it means the two games must not both pump events or both create a
  window (d).
- **The JNI glue stays one copy too**: `touch_controls.cpp`, `stats.cpp`, `gamepads.cpp`, `power.cpp` are left out of
  the guest (CMake `VCS_ANDROID_STORIES_GUEST`), so `vcs_android::touchPad()` & co. resolve to `libmain.so`'s, where
  Java's `native` calls land (ART binds them by name in the libraries `System.loadLibrary` loaded: `libmain.so`). The
  on-screen pad, camera drags, the controller chooser and the stats overlay therefore reach the guest without a
  bridge. (`side_panel.cpp` / `cooling.cpp` stay in the guest as private copies: the side panel's Java calls land in
  `libmain.so`'s copy, whose game never runs: the panel is inert in this mode - work list.)
- **A guard, since `-z undefs` turns off the linker's check**: `build-game --stories` lists the guest's undefined
  symbols (`llvm-nm -D --undefined-only`) and fails if one of them is defined by `libmain.so` and is neither `SDL_*`
  nor one of the glue functions: such a symbol would silently bind to the host's runtime.
- **The environment** (each game its own `PS2X_CONTROL_PORT`, `VCS_DATA_DIR`, `STORIES_DUALWORLD`, the settings'
  writes): `-Wl,--wrap=getenv,--wrap=setenv,--wrap=unsetenv` in the guest, the overlay of `src/stories/game_dll.cpp`
  (`src/stories/game_so.cpp`); lld's ELF `--wrap` covers every object linked in (the runtime's archives too), and SDL's
  own `SDL_getenv` stays the process's (it is in `libmain.so`). Log lines get `[LCS] ` / `[VCS] ` via `--wrap=fprintf,
  vfprintf,fputs` (stderr is already piped to logcat by the host).
- C++ runtime: `c++_shared` stays one copy (allocator, exceptions; a guest's own `type_info` objects are local, an
  exception is thrown and caught inside one guest).

Why not keep the code packs as they are (runtime-free) and load the runtime twice? A second copy of the runtime as its
own library (another soname, linked without SDL) loaded `RTLD_GLOBAL` before a pack would capture that pack's
references - but bionic has no way to take a library out of the global group again, so the second pack would bind to
the first copy too; and a pack's `DT_NEEDED libmain.so` always finds the host by soname. Doable only with private
linker namespaces (`android_create_namespace`: not in the NDK, platform-private). So the prototype uses **separate
builds** of each game for this mode; it is a personal build (game code inside), never shared. Distributing it later
means a "stories pack" per game that the player builds (`build-game --stories`), same as a code pack but ~10 MB bigger
(the runtime inside), with its own stamp (work list).

### (b) The shared block in one process

`dualworld.cpp`'s `Shared` (the camera, clock, handover, settings sync and the picture slots: 3 x 3840x2160 RGBA, ~100
MB of address space) was a named mapping (`CreateFileMappingW`; `shm_open` elsewhere) and `openShared()` returned
nullptr on Android (bionic has no `shm_open`). In one process the block is plain memory: the host (`libmain.so`)
reserves it once with `mmap(MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)` (only the pages touched become resident: at a
960x540 picture ~6 MB) and hands its address to both games in their environment (`STORIES_DW_BLOCK=<address>:<size>`).
`openShared()` takes that first on every platform (size checked against `sizeof(Shared)`; a smaller block: off, logged),
so the rest of dualworld (seqlocks, atomics, `magic` set by the first of the pair) is unchanged. A process-global could
not work: each guest has its own copy of every global (that is the point of (a)). The "other process alive" checks
(`pid`, `OpenProcess`) already answer "alive" on Android; the game-frame / alive timestamps still catch a dead game
thread.

### (c) Graphics: the picture of the city behind

- **The game behind renders offscreen.** The Vulkan RHI already has it: `DeviceDesc::offscreen` (no surface, no
  swapchain, the back buffer a texture; `present::init` sets it from `platform::windowHidden()`). Each game has its own
  `VkInstance` + `VkDevice` (each guest its own RHI) on the one GPU: legal, both from `libvulkan.so`.
- **The first step is the CPU path that exists**: the background's `gs_hw` frame sink blits its world (without HUD) into
  a picture, `readTexture`s it and copies it into the block's slots; the front uploads the newest one and draws it with
  `ps_dw_backdrop` (compiled to SPIR-V like every shader: `gs_shaders.cpp` has `spirv/gs_dw_backdrop_ps.h`), with the
  depth-correct composite and the reprojection. Nothing in that path is D3D11-only (the shared-texture path is: it
  needs `createSharedTexture`, null on Vulkan, so it falls back by itself). Cost on a phone, estimated from the PC's
  numbers (1x: 1.9 ms sink / 0.6 ms composite; the sink's `readTexture` waits for the GPU): the picture's size is the
  front's internal resolution; on a phone at 1x (~853x480) that is 1.6 MB a frame, a few ms on the background's render
  thread and ~1 ms on the front's. Cap it (`STORIES_DW_MAX=1280x720`) so a 2x front does not double it.
- **Later, on the GPU** (in order of effort): (1) `VK_KHR_external_memory_fd` / `VK_ANDROID_external_memory_android_hardware_buffer`:
  the background allocates the picture as an `AHardwareBuffer` (two, as the D3D11 path's slots), the front imports it
  into its own device; sync with an exported semaphore / sync fd (`VK_KHR_external_semaphore_fd`) or a fence wait on
  the background side before the slot is published. Adreno drivers on Android 13+ have both extensions. This is
  `rhi::Device::createSharedTexture` / `openSharedTexture` / `lockShared` implemented for Vulkan; `gs_hw`'s GPU sink /
  source then work unchanged. (2) One `VkDevice` for both: a host-created device handed to both RHIs, two queues (or
  one, externally synchronised); saves memory, but the RHI's device ownership and its per-device caches would need
  refactoring - Stage 3 (internal design) territory.

### (d) Threads, the one window, input, audio

- **Threads**: SDL's Java side runs `SDL_main` on its `SDLThread`. The host (game_main.cpp) runs the **front** game
  there (as today), and starts the **background** game on a pthread of its own (8 MB stack), after the front's SDL
  window exists (it waits for `SDL_GetWindows()` to list one, up to 20 s). Each game then makes its own threads as
  usual (game thread, render worker, GS, audio).
- **One window**: Android has one surface per activity, and SDL on Android refuses a second window ("Android only
  supports one window"). The background game runs **headless** (`PS2X_HEADLESS=1`, platform_sdl3.cpp `[stories]`):
  no `SDL_CreateWindow`, no video init, no event pump, `windowHidden()` true (the RHI goes offscreen), its size from
  `PS2X_HIDDEN_WINDOW`, Dear ImGui with its own context and no SDL backend (frames still begun and ended: the UI
  code runs, drawn offscreen). Its `setWindowShown` (dualworld's front swap) does nothing.
- **Input to the front only**: only the front pumps SDL's queue (the background never calls `SDL_PollEvent`, so it
  can't take the front's events). The JNI-fed state (touch pad, drags, gamepads) is `libmain.so`'s, read by both, but
  the background already ignores input by its role (dualworld, as on the PC).
- **Audio from the front only**: both open an SDL audio stream on the one shared SDL (SDL3 mixes several streams on
  one device); the background's game is muted while behind (dualworld / `ps2AudioMuted`, as on the PC).
- **Crossing over is off in the prototype** (`STORIES_DW_HANDOVER=0` for both): a crossing swaps the front, and on
  Android that means moving the one surface from one game's `VkDevice` to the other's: the leaving game drops its
  swapchain and goes offscreen, the arriving one makes a surface on the `ANativeWindow` and starts pumping events
  (only one swapchain may own a window at a time: `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`). Needs an RHI switch
  offscreen <-> on screen at run time (`recreateSurface` exists for the surface's loss) and the pump handed over with
  the role. Work list.

### (e) Memory and CPU budget

- **PC numbers**: 300-900 MB working set per game (docs/PERFORMANCE.md: ~245 MB at start, growing with the RSL
  recordings up to their 256 MB limit; plus the renderer's targets at its internal resolution). Vice City in the
  background cost ~0.7 GB and about one CPU core (docs/STORIES.md).
- **On the phone (estimate)**: per game the guest's 32 MB of PS2 RAM, the runtime (GS memory, caches, the RSL
  recordings: `VCS_RSL_RECORDINGS_MB`, 256 by default, the host sets 96 for the background), Vulkan memory for its
  targets at 1x-2x, and its code (~40-50 MB mapped from the APK, clean pages). Expect ~0.6-1.0 GB for the front and
  ~0.5-0.8 GB for the background: **1.1-1.8 GB** for the process. Android 13+ phones with 8-12 GB give a foreground
  app 3-5 GB before the low-memory killer looks at it: fits, with room. The two `VkDevice`s each keep their own
  pipelines and caches (tens of MB).
- **CPU**: on the phone LCS alone keeps ~2 big cores busy (game thread + render worker) at 60 fps. The background adds
  its own game thread and render worker: on a Snapdragon 8 Gen 2/3 class SoC (1 prime + 4-5 performance + 2-3
  efficiency cores) that fits, but **heat** is the limit: a second game is ~1.5-2x the power, so the thermal step-down
  (`cooling.cpp`) comes sooner. Mitigations, in order: the background's internal resolution at 1x and its picture
  capped at 1280x720; its RSL limit lowered; later, the background rendered every second frame (its picture is
  reprojected anyway) and its game thread at 30 fps.
- **Go / no-go risks**: (1) two Vulkan devices on Adreno in one process (legal; untested here); (2) heat and
  frame pacing on the phone (the front must keep 60); (3) the readback's GPU wait on a tiler GPU (a full flush each
  frame on the background's queue; it is the background's thread that waits, not the front's); (4) the crossing
  (surface hand-over) not done; (5) distribution: self-contained "stories packs" instead of the runtime-free code
  packs.

### The prototype (what is built)

Built 2026-11-19 on Windows: `libstories_lcs.so` 45 MB, `libstories_vcs.so` 62 MB (each exports only
`stories_game_main`; DT_NEEDED only the system libraries and `libc++_shared.so`; of their imports, the only names
`libmain.so` also defines are `SDL_*` and 29 `vcs_android::` glue functions; Vulkan through the guest's own volk, no
`vk*` imports), the both-cities APK 49 MB. **Unchecked on a phone.** To check first: both libraries load (`[stories]
loaded ...` in logcat), `[VCS] [platform] headless` and `[present] Vulkan` twice, `[dualworld] the host's block`, Liberty
City playable with touch and a pad, Vice City on the horizon from Portland's east shore after about a minute, the
frame rate and heat with both running.


- `VCS_ANDROID_STORIES_GUEST` (CMake, Android): the game as `libstories_<game>.so` (everything above), built by
  `python tools/android.py build-game --game lcs|vcs --stories [--generated <dir>/ee]` into
  `build/android/stories-<game>/`.
- `python tools/android.py build --stories`: the APK with both guests packaged (`-PvcsStoriesLibs=build/android/stories-libs`):
  a **personal build** (game code inside; never shared), `BuildConfig.VCS_STORIES` true.
- The host (`src/android/stories_host.cpp`, in `libmain.so`): `VCS_STORIES=1` in the environment (VCSActivity, from the
  launcher's game selector (header switch, Play > Game, Y): its third choice "Both cities", shown only in a `--stories` build) -> it reserves the block, copies
  the background's card and settings into `files/stories/`, loads both guests and starts Vice City behind and Liberty
  City in front. The two discs come as `argv[1]` (LCS) and `argv[2]` (VCS). The game behind gets its own
  `VCS_APP_DIR` (`files/stories`), card (`files/stories/vcs-background`, a copy of `files/userdata/mc0` the first
  time) and settings (`files/stories/vcs-background.ini`, a copy of `vcs.ini` at each start, never written back).
- What the game behind leaves alone, since the JNI-fed state is one copy in `libmain.so`: the camera drags and the
  mouse (`takeMouseMotion`), the on-screen settings / trainer buttons (`takeMenuToggle` / `takeTrainerToggle`), the
  vibrator, the stats overlay's frame counts, the side panel's game state and bindings, the heat step-down, the ADPF
  reports (`[stories]` seams in platform_sdl3.cpp, main.cpp, settings_menu.cpp, trainer_menu.cpp).
- Not in this mode (yet): crossing over, the side panel (inert: its Java calls reach `libmain.so`'s copy), frame
  generation (the guests are built without LSFG), the boot movies (built without FFmpeg: skipped), Stories' hero
  (`STORIES_HERO=city`) and cross-game vehicles (no packs on the phone), the stats overlay's GPU time (both games'
  frames counted).
- Switches (files/env.txt, read by the host): `STORIES_DW_OFFSET`, `STORIES_DW_BORDER`, `STORIES_DW_MAX` (1280x720),
  `STORIES_DW_SIZE` (the background's size, 1280x720), `STORIES_BACK_RSL_MB` (96), `VCS_STORIES_LCS_LIBRARY` /
  `VCS_STORIES_VCS_LIBRARY` (another library file).

## From an earlier Android port

An earlier Android recompilation port solved several of the same
problems. Taken so far: the `:game` process ended on exit, the build tag in the UI and log, keep-screen-on, the touch
controls' layout, modes and layout editor (above), release signing from the environment (above), LSFG frame
generation (above). To take:

- **The runtime / game split** (done differently, "Runtime / game split" above): its JNI shim calls into the host
  through `#pragma weak` symbols and stamps versions into the APK (`buildConfigField`) that the launcher compares with
  the user's files before starting. Here the game library links against the runtime and carries one hand-bumped
  interface number, checked by the launcher and the runtime.
- **Lifecycle**: `Native.lifecycle(...)` from onStart / onResume / onPause / onStop, an optional Paused overlay,
  audio focus.
- **GPU**: custom Adreno drivers (Turnip) through libadrenotools, from the app's private folder; render thread pinned
  to the fast cores, re-applied after every foreground change (Android resets affinity).
- **Release**: scripts that keep game names out of public commits, a pre-push scan.

## Work list

- [x] NDK build of the platform layer, RHI and presenter; the smoke test APK
- [x] Surface recreation after the background
- [x] The game with the NDK (personal build: libmain.so in the APK); `main.cpp`'s paths under the app's folders
- [x] First run of the game on the phone
- [x] FFmpeg for Android (the movies; 2026-11-18, built and the decode checked on the PC, unchecked on the phone:
      "Movies (FFmpeg)")
- [x] Launcher: the disc picked and checked in place, the game library imported read-only
- [x] Launcher redesign: category bar + item list, wave background, pre-launch settings (vcs.ini + prefs), Lossless.dll
      folder grant, delete the adb test copy, adaptive icon (ANDROID_LAUNCHER.md); to check on the phone, folded and
      unfolded, by touch and gamepad
- [x] Saves: import (.max, .cbs, .psu, .psv, .zip) and export (.psu, .zip) in the launcher, with backup and card order
      (above); to check on the phone
- [x] Load the imported game library (the runtime / game split, the interface stamp): "Runtime / game split"
- [x] Build the code pack on the phone (no PC), experimental: *Disc > Build code pack on this phone*, both games,
      Termux's clang downloaded on first use; proven in the app on the emulator, unchecked on a phone
      ([PHONE_BUILD.md](PHONE_BUILD.md))
- [x] A second game, Liberty City Stories: one APK, the library names its game ("Games"); reached gameplay on the phone
- [x] LCS on Android: a code pack (library) per game in the app and the launcher's game switch, Mods per game, LCS's
      colours (`Palette`), the side panel / stats / heat step-down running on LCS (2026-11-18, built, unchecked on the
      phone)
- [ ] LCS on Android: movies (FFmpeg in the APK since 2026-11-18), sound on the phone
- [ ] **Phone checklist, one app for both games** (2026-11-18 build):
      1. Install over "GTA Stories Recompiled" (earlier "VCS Recomp Android"; same package; the signing key must be the one it was installed with, else
         `INSTALL_FAILED_UPDATE_INCOMPATIBLE`: don't uninstall, that deletes its data - use the old key). The first start
         moves VCS's library to `files/game/vcs/` (`[launcher] game library moved` in the log); VCS plays as before.
      2. The header switch: tap Liberty City Stories, the colours crossfade to amber / steel blue / navy and every
         row's text stays readable (help text, values, greyed rows, folded and unfolded); Y on a pad switches; Play >
         Game too; the switch is greyed while a game runs.
      3. LCS: Disc shows LCS's disc (or `disc-lcs.iso`), the library row LCS's ("✓ Imported: Liberty City Stories" after Disc > Import
         game library (code pack)), Mods shows LCS's own list, Saves lists LCS's slots (after the data move above).
      4. In LCS: Back opens the side panel with **every** tab's settings (Display, Performance, Controls, Audio, Game)
         answering; Drive-by fire rate greyed with LCS's reason; Quick save / Save here / Quick load; the trainer.
      5. LCS's stats overlay: the game fps is no longer 0; CPU / GPU / heat chips as on VCS; the panel and overlay in
         LCS's colours, the touch controls' accents amber (right) / steel blue (left), the layout editor's outline.
      6. LSFG in LCS: Frame generation on (the same Lossless.dll as VCS), the log's `[lsfg] frame generation active`,
         x2 at a 60 cap on a 120 Hz panel; the panel's frame generation rows live.
      7. Back to VCS: its colours, its disc / library / saves untouched.
- [ ] The game thread while in the background (pause the EE; audio off)
- [x] Leaving the game: Quit to the launcher in the side panel (Back opens the panel)
- [x] Side panel (Game / Performance / Display / Controls / Audio, the game's settings through its registry, screenshot
      to Pictures/, pause, touch / overlay pad / controller / keyboard focus); to check on the phone
- [x] Controls extras: right stick, gyro aiming, rumble on the phone, layouts by context, accelerator lock, sprint toggle;
      to check on the phone (the gyro's axis signs in both landscape rotations)
- [x] Side panel declutter (short tabs that never cut, no footer, one-line rows, a description under the focused row
      only, short summaries for the game's settings); to check on the phone, folded and unfolded
- [x] Remapping: the controller (upstream's Gamepad bindings, by pressing) and the on-screen buttons; the overlay follows
      the controller's bindings; to check on the phone with a controller
- [x] Controllers: one pad read (Auto / chosen / touch only), non-pads ignored, a pad's devices grouped, every device
      logged; to check on the phone with a Bluetooth pad (and read the `[gamepad]` log lines if one still shows twice)
- [ ] Frame generation rows in the panel: wire `framegen` prefs to the LSFG environment
- [x] Touch controls: a virtual pad merged with a real one, drag on the right half as the mouse camera, Auto / On /
      Off, layout editor (built; to check on the phone with the smoke test, then in the game)
- [x] Layout editor v2: each button or group moved / resized, opacity per button, per screen and orientation (old group
      layouts taken over), Reset per screen, the HUD overlay and nudge, a portrait layout and Portrait mode on the outer
      screen (the picture at the top); to check on the phone, folded and unfolded, both ways round
- [x] Trainer button on the touch overlay (D-pad / Cross / Circle drive the trainer); to check on the phone
- [x] Save anywhere (trainer Save game page; JNI hook for the panel): built; to check on the phone (checklist above),
      then a panel row calling `nativeSaveAnywhere`
- [x] Stats overlay (frame rate, frame / GPU time, CPU caps, thermals, battery, memory, build tag); to check on the
      phone, including whether the View costs frames
- [x] Release signing from the environment (`VCS_SIGN_STORE` / `_ALIAS` / `_PASS`), no key in the repository
- [ ] Custom GPU drivers (libadrenotools), core affinity for the game / render threads
- [ ] Rotation: render pre-rotated (`currentTransform`) instead of the compositor turning every frame
- [ ] Performance on tile-based GPUs (load / store ops, pass splits)
- [x] Power and heat: frame rate held to the refresh, 60 Hz panel, no pacing spin, ADPF hints, step-down to 30 fps
      when hot, background pause by default ("Power and heat"); to measure on the phone
- [x] LSFG frame generation (Lossless Scaling, the player's own Lossless.dll): built (`VCS_LSFG`), launcher card
- [ ] Both cities on Android (prototype, 2026-11-19, built, unchecked): on the phone; then the crossing (the surface
      handed between the two games' devices), the GPU picture (AHardwareBuffer), the side panel, distributable
      "stories packs" ("Both cities on Android")
- [ ] LSFG on the phone: the smoke test and the game at 30 / 60 fps caps; check the pacing (display timing, the
      window's frame rate on the LTPO panel), the cost at 2420x1080 and with flow 25-100 %
- [ ] LSFG: only the game's picture as input (its own resolution, cheaper; Dear ImGui drawn over each frame) instead of
      the back buffer; a target-rate mode (the earlier port's `DM_LSFG_TARGET`); settings changed while the game runs
