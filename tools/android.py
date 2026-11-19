#!/usr/bin/env python3
"""[android] Build, install and run the Android app (docs/ANDROID.md).

    python3 tools/android.py build [--release]   # SDL's Java sources, then Gradle: build/android/gradle/outputs/apk/...
                                                 # --release: signed with your key (VCS_SIGN_STORE / _ALIAS / _PASS)
                                                 # the APK holds no game code (the runtime / game split)
    python3 tools/android.py build --personal    # the personal build instead: the game inside the APK (never share it)
    python3 tools/android.py build-game          # your game library from generated/: build/android/game/libvcs_game.so,
                                                 # to import in the app (Disc > Import game library)
    python3 tools/android.py push-game           # (development) that library straight into the app's folder over adb
    python3 tools/android.py build-game --game lcs --generated <dir>/ee
                                                 # [lcs] Liberty City Stories' library (tools/recompile.py --game lcs --out
                                                 # <dir>): build/android/game-lcs/libvcs_game.so; the same APK runs it
                                                 # (push-game --game lcs: that one onto the phone)
    python3 tools/android.py build --personal --game lcs --generated <dir>/ee
                                                 # [lcs] a personal LCS build in its own tree, build/android-lcs
                                                 # (install --personal --game lcs)
    python3 tools/android.py install [--release] # adb install to the phone's main user (never a work profile)
    python3 tools/android.py run                 # start the app and follow its log (adb logcat -s vcs SDL)
    python3 tools/android.py all [--release]     # build + install + run
    python3 tools/android.py log                 # only the log

Needs the Android SDK (ANDROID_HOME, else ~/Library/Android/sdk, ~/Android/Sdk or %LOCALAPPDATA%\\Android\\Sdk) with
NDK 28.2.13676358 and CMake 3.31.5 (sdkmanager "ndk;28.2.13676358" "cmake;3.31.5"), git and glslangValidator (the
Vulkan shaders: VCS_GLSLANG, PATH, the Vulkan SDK's bin, or the copy the macOS build makes); build also needs a JDK 17+
and a POSIX sh for FFmpeg's configure (the movies; built once into build/android/ffmpeg - on Windows Git for Windows'
sh, found next to git; without one the APK builds without movies).
The APK never needs generated code: build-game needs it (python3 tools/recompile.py <your iso> first).
Windows, macOS and Linux (x86-64 hosts, as the NDK; Apple Silicon through the NDK's universal binaries).
"""
import argparse
import glob
import gzip
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANDROID = os.path.join(ROOT, "android")
OUT = os.path.join(ROOT, "build", "android")
PACKAGE = "com.batinium.gtastories"
ACTIVITY = PACKAGE + "/.LauncherActivity"


def fail(message):
    print("[android] " + message, file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    print("[android] $ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        fail("failed (%d): %s" % (r.returncode, cmd[0]))
    return r


WINDOWS = os.name == "nt"


def exe(path):
    """`path` with the host's executable suffix."""
    return path + ".exe" if WINDOWS else path


def cmake_path(path):
    """A path for a CMake -D value: forward slashes (a backslash starts an escape in CMake's strings)."""
    return path.replace("\\", "/")


def sdk_dir():
    local = os.environ.get("LOCALAPPDATA")
    for d in (os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
              os.path.expanduser("~/Library/Android/sdk"), os.path.expanduser("~/Android/Sdk"),
              os.path.join(local, "Android", "Sdk") if local else None):
        if d and os.path.isdir(d):
            return d
    fail("no Android SDK: set ANDROID_HOME (Android Studio puts it in ~/Library/Android/sdk, ~/Android/Sdk or "
         "%LOCALAPPDATA%\\Android\\Sdk)")


def adb():
    path = exe(os.path.join(sdk_dir(), "platform-tools", "adb"))
    return path if os.path.exists(path) else (shutil.which("adb") or fail("no adb (platform-tools)"))


def sdl_tag():
    """The SDL3 release src/platform/CMakeLists.txt fetches: the Java side must be the same version as the C side."""
    with open(os.path.join(ROOT, "src", "platform", "CMakeLists.txt"), encoding="utf-8") as f:
        text = f.read()
    m = re.search(r'FetchContent_Declare\(\s*SDL3\b.*?GIT_TAG\s+"([^"]+)"', text, re.S)
    if not m:
        fail("no SDL3 GIT_TAG in src/platform/CMakeLists.txt")
    return m.group(1)


def fetch_sdl():
    tag = sdl_tag()
    dest = os.path.join(OUT, "SDL-" + tag)
    if not os.path.isdir(os.path.join(dest, "android-project")):
        os.makedirs(OUT, exist_ok=True)
        run(["git", "clone", "--quiet", "--depth", "1", "-b", tag, "https://github.com/libsdl-org/SDL.git", dest])
    return dest


def glslang():
    found = os.environ.get("VCS_GLSLANG") or shutil.which("glslangValidator") or shutil.which("glslang")
    if not found and os.environ.get("VULKAN_SDK"):  # the LunarG Vulkan SDK (its Windows installer sets VULKAN_SDK)
        found = exe(os.path.join(os.environ["VULKAN_SDK"], "Bin" if WINDOWS else "bin", "glslangValidator"))
    if not found or not os.path.exists(found):
        # the macOS build's own copy (tools/shader_tools.py, built by its first configure)
        built = exe(os.path.join(os.environ.get("VCS_BUILD_DIR") or os.path.join(ROOT, "build"), "build-tools",
                                 "shader-tools", "build", "bin", "glslang"))
        found = built if os.path.exists(built) else None
    if not found:
        fail("no glslangValidator: brew install glslang / apt install glslang-tools / on Windows the Vulkan SDK "
             "(vulkan.lunarg.com), or VCS_GLSLANG=<path>")
    return found


SIGN_VARS = ("VCS_SIGN_STORE", "VCS_SIGN_ALIAS", "VCS_SIGN_PASS")


def check_signing():
    """The release APK is signed with the user's own key, named by the environment (never kept in the repository);
    android/app/build.gradle checks it too, this only says so before the long build."""
    missing = [v for v in SIGN_VARS if not os.environ.get(v)]
    store = os.environ.get("VCS_SIGN_STORE")
    if store and not os.path.isfile(store):
        missing.append("VCS_SIGN_STORE: no file " + store)
    if missing:
        fail("--release needs your signing key in the environment: " + ", ".join(missing) +
             " (docs/ANDROID.md, Release signing; a debug build needs none)")


PHONEBUILD = os.path.join(OUT, "phonebuild")  # android/app/build.gradle adds its jniLibs/ and assets/


def headers_hash():
    """[phonebuild] src/android/game_abi.cmake's VCS_GAME_HEADERS_HASH: sha256 of the headers' bytes as lowercase hex
    (CMake's file(READ HEX), concatenated), first 16 digits. The phone build stamps its pack with it."""
    with open(os.path.join(ROOT, "src", "android", "game_abi.cmake"), encoding="utf-8") as f:
        text = f.read()
    block = re.search(r"set\(VCS_GAME_INTERFACE_HEADERS(.*?)\)", text, re.S).group(1)
    rt = os.path.join(ROOT, "third_party", "PS2Recomp", "ps2xRuntime")
    hexes = []
    for name in re.findall(r'"([^"]+)"', block):
        path = name.replace("${CMAKE_SOURCE_DIR}", ROOT).replace("${_vcs_rt}", rt)
        with open(path, "rb") as f:
            hexes.append(f.read().hex())
    import hashlib
    return hashlib.sha256("".join(hexes).encode("ascii")).hexdigest()[:16]


def stage_phonebuild():
    """[phonebuild] "Build code pack on this phone": the recompiler and the VU1 translator for arm64 (built with the
    NDK, as jniLibs: libps2recomp.so, libvu1recomp.so, run from nativeLibraryDir), and as assets/phonebuild/: the runtime
    headers the generated code includes (runtime.zip), the NDK's arm64 sysroot (sysroot.zip), each game's recompiler
    config (game/<id>/), the headers hash and the pinned Termux compiler packages (termux.txt). No game code."""
    import zipfile
    sdk = sdk_dir()
    ndk = os.path.join(sdk, "ndk", NDK_VERSION)
    cmake_bin = os.path.join(sdk, "cmake", CMAKE_VERSION, "bin")
    tree = os.path.join(PHONEBUILD, "recomp")
    if not os.path.isfile(os.path.join(tree, "build.ninja")):
        run([exe(os.path.join(cmake_bin, "cmake")), "-S", os.path.join(ROOT, "tools", "phonebuild", "recomp_android"),
             "-B", tree, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + cmake_path(exe(os.path.join(cmake_bin, "ninja"))),
             "-DCMAKE_TOOLCHAIN_FILE=" + cmake_path(os.path.join(ndk, "build", "cmake", "android.toolchain.cmake")),
             "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-33", "-DANDROID_STL=c++_static",
             "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_CPU=ARM64"])
    run([exe(os.path.join(cmake_bin, "cmake")), "--build", tree, "--target", "ps2_recomp", "vu1_recomp"])
    libs = os.path.join(PHONEBUILD, "jniLibs", "arm64-v8a")
    os.makedirs(libs, exist_ok=True)
    for src, name in (("ps2xRecomp/ps2_recomp", "libps2recomp.so"), ("vu1_recomp", "libvu1recomp.so")):
        run([ndk_tool(sdk, "llvm-strip"), "-o", os.path.join(libs, name), os.path.join(tree, *src.split("/"))])
    assets = os.path.join(PHONEBUILD, "assets", "phonebuild")
    shutil.rmtree(assets, ignore_errors=True)
    os.makedirs(assets)
    rt = os.path.join(ROOT, "third_party", "PS2Recomp")
    sse2neon = os.path.join(tree, "_deps", "sse2neon-src", "sse2neon.h")
    with zipfile.ZipFile(os.path.join(assets, "runtime.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for base, prefix in ((os.path.join(rt, "ps2xRuntime", "include"), "ps2xRuntime/include"),
                             (os.path.join(rt, "ps2xIOP", "include"), "ps2xIOP/include"),
                             (os.path.join(rt, "ps2xRuntime", "src", "lib", "Kernel", "Stubs"), "kernel/Stubs")):
            for d, _, files in os.walk(base):
                for f in files:
                    if f.endswith((".h", ".hpp", ".inl")):
                        full = os.path.join(d, f)
                        z.write(full, prefix + "/" + os.path.relpath(full, base).replace(os.sep, "/"))
        for f in ("game_abi.h", "game_library_stamp.cpp"):
            z.write(os.path.join(ROOT, "src", "android", f), "src/android/" + f)
        z.write(sse2neon, "sse2neon.h")
    host = {"darwin": "darwin-x86_64", "win32": "windows-x86_64"}.get(sys.platform, "linux-x86_64")
    llvm = os.path.join(ndk, "toolchains", "llvm", "prebuilt", host)
    sysroot = os.path.join(llvm, "sysroot", "usr")
    other = ("arm-linux-androideabi", "i686-linux-android", "x86_64-linux-android", "riscv64-linux-android")
    with zipfile.ZipFile(os.path.join(assets, "sysroot.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        inc = os.path.join(sysroot, "include")
        for d, dirs, files in os.walk(inc):
            dirs[:] = [x for x in dirs if x not in other]
            for f in files:
                full = os.path.join(d, f)
                z.write(full, "usr/include/" + os.path.relpath(full, inc).replace(os.sep, "/"))
        lib33 = os.path.join(sysroot, "lib", "aarch64-linux-android", "33")
        for f in ("crtbegin_so.o", "crtend_so.o", "libc.so", "libm.so", "libdl.so", "liblog.so"):
            z.write(os.path.join(lib33, f), "usr/lib/aarch64-linux-android/33/" + f)
        clang_lib = os.path.join(llvm, "lib", "clang")
        unwind = os.path.join(clang_lib, os.listdir(clang_lib)[0], "lib", "linux", "aarch64", "libunwind.a")
        z.write(unwind, "usr/lib/aarch64-linux-android/libunwind.a")
        builtins = os.path.join(os.path.dirname(os.path.dirname(unwind)), "libclang_rt.builtins-aarch64-android.a")
        z.write(builtins, "usr/lib/aarch64-linux-android/libclang_rt.builtins-aarch64-android.a")
        notice = os.path.join(ndk, "NOTICE")
        if os.path.isfile(notice):
            z.write(notice, "NOTICE.ndk.txt")
    for game in GAMES:
        dest = os.path.join(assets, "game", game)
        os.makedirs(dest)
        for f in (game + ".toml", "functions.csv", "vu1_uploads.txt"):
            src = os.path.join(ROOT, "game", game, f)
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(dest, f))
    with open(os.path.join(assets, "headers_hash.txt"), "w", encoding="ascii") as f:
        f.write(headers_hash() + chr(10))
    shutil.copyfile(os.path.join(ROOT, "tools", "phonebuild", "termux_packages.txt"), os.path.join(assets, "termux.txt"))
    print("[android] phone build files staged in " + PHONEBUILD)


def build(release, personal=False, game="vcs", generated=None, stories=False):
    if release:
        check_signing()
    if stories:  # [stories] both games' libstories_<game>.so packaged: a personal build of its own (build/android-stories)
        missing = [p for p in (stories_library(g) for g in GAMES) if not os.path.isfile(p)]
        if missing:
            fail("--stories needs both games built for it first (build-game --game lcs --stories, build-game --game vcs "
                 "--stories): missing " + ", ".join(missing))
    gen = generated_dir(game, generated)
    if personal and not os.path.isfile(os.path.join(gen, "register_functions.cpp")):
        fail("--personal needs the generated code in %s (%s)" % (gen, RECOMPILE_HINT[game]))
    if not personal and game != "vcs":
        print("[android] note: the runtime APK is the same for every game (the game library names its game); "
              "--game only changes build-game / push-game and --personal")
    sdk = sdk_dir()
    with open(os.path.join(ANDROID, "local.properties"), "w", encoding="utf-8") as f:
        f.write("sdk.dir=" + sdk.replace("\\", "\\\\") + "\n")
    sdl = fetch_sdl()
    gradlew = os.path.join(ANDROID, "gradlew.bat" if WINDOWS else "gradlew")
    task = "assembleRelease" if release else "assembleDebug"
    props = ["-PvcsBundleGame=" + ("true" if personal else "false")]
    if personal:  # [lcs] the bundled game, its generated code and its own build tree
        props += ["-PvcsGame=" + game, "-PvcsGeneratedDir=" + cmake_path(gen),
                  "-PvcsOutDir=" + cmake_path(out_dir(game, personal))]
    elif stories:
        props += ["-PvcsStoriesLibs=" + cmake_path(STORIES_LIBS), "-PvcsOutDir=" + cmake_path(STORIES_OUT)]
    stage_phonebuild()  # [phonebuild] the on-phone code pack build's tools and files (docs/PHONE_BUILD.md)
    run([gradlew, "-p", ANDROID, task, "-PvcsSdlDir=" + sdl, "-PvcsGlslang=" + glslang()] + props + ["--console=plain"])
    print("[android] ok: " + apk_path(release, game, personal, stories))
    if personal or stories:
        print("[android] a PERSONAL build: this APK contains game code from your disc. Never share it.")


NDK_VERSION = "28.2.13676358"
CMAKE_VERSION = "3.31.5"
GENERATED = os.path.join(ROOT, "generated", "ee")
GAMES = ("vcs", "lcs")  # [lcs] CMake VCS_GAME; src/disc/known_games.h
RECOMPILE_HINT = {"vcs": "python3 tools/build.py --recompile <your iso>",
                  "lcs": "python3 tools/recompile.py --game lcs --out <dir> <your LCS iso>, then --generated <dir>/ee"}


def generated_dir(game, override=None):
    """[lcs] The game's generated EE code: --generated, else generated/ee (VCS) or generated-<game>/ee."""
    if override:
        return os.path.abspath(override)
    return GENERATED if game == "vcs" else os.path.join(ROOT, "generated-" + game, "ee")


def game_dir(game):
    """[lcs] Each game's library tree: build/android/game (VCS, as before), build/android/game-lcs."""
    return os.path.join(OUT, "game" if game == "vcs" else "game-" + game)


def game_library(game):
    return os.path.join(game_dir(game), "libvcs_game.so")


def out_dir(game, personal):
    """[lcs] Gradle's folder: build/android; a personal build of another game has its own (build/android-lcs), so the
    two don't rebuild over each other. The runtime APK is the same for every game: build/android."""
    return OUT if game == "vcs" or not personal else OUT + "-" + game


def ndk_tool(sdk, name):
    ndk = os.path.join(sdk, "ndk", NDK_VERSION)
    host = {"darwin": "darwin-x86_64", "win32": "windows-x86_64"}.get(sys.platform, "linux-x86_64")
    path = exe(os.path.join(ndk, "toolchains", "llvm", "prebuilt", host, "bin", name))
    return path if os.path.exists(path) else fail("no %s in NDK %s (%s)" % (name, NDK_VERSION, path))


def build_game(game="vcs", generated=None):
    """The player's game library: the generated code (generated/ee, generated/vu1) compiled with the NDK as
    libvcs_game.so, linked against the runtime's libmain.so (built here too, the same sources as the APK's). Its
    interface stamp (src/android/game_abi.h) must match the APK's: build it from the source of the APK's version.
    [lcs] The stamp also names the game (VCS_GAME): the same APK then boots that game's ELF from the disc."""
    gen = generated_dir(game, generated)
    if not os.path.isfile(os.path.join(gen, "register_functions.cpp")):
        fail("no generated code in %s: %s first" % (gen, RECOMPILE_HINT[game]))
    sdk = sdk_dir()
    ndk = os.path.join(sdk, "ndk", NDK_VERSION)
    cmake_bin = os.path.join(sdk, "cmake", CMAKE_VERSION, "bin")
    cmake = exe(os.path.join(cmake_bin, "cmake"))
    ninja = exe(os.path.join(cmake_bin, "ninja"))  # the SDK's CMake package brings its Ninja
    for path, what in ((ndk, "NDK " + NDK_VERSION), (cmake, "CMake " + CMAKE_VERSION), (ninja, "Ninja (CMake package)")):
        if not os.path.exists(path):
            fail("no %s in the SDK (sdkmanager \"ndk;%s\" \"cmake;%s\")" % (what, NDK_VERSION, CMAKE_VERSION))
    sdl = fetch_sdl()
    tree = os.path.join(game_dir(game), "cmake")
    os.makedirs(tree, exist_ok=True)
    # the same configuration as the APK's (android/app/build.gradle), plus the game library
    if WINDOWS and len(tree) > 60:
        print("[android] note: %s is a long path; if the build fails on a missing file, clone the repository to a "
              "short folder such as C:\\vcs (Windows' 260-character path limit)" % tree)
    run([cmake, "-S", ROOT, "-B", tree, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + cmake_path(ninja),
         "-DCMAKE_TOOLCHAIN_FILE=" + cmake_path(os.path.join(ndk, "build", "cmake", "android.toolchain.cmake")),
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-33", "-DANDROID_STL=c++_shared",
         "-DCMAKE_BUILD_TYPE=Release", "-DVCS_GLSLANG=" + cmake_path(glslang()),
         "-DFETCHCONTENT_SOURCE_DIR_SDL3=" + cmake_path(sdl),
         "-DPS2X_LTO=OFF", "-DPS2X_CPU=ARM64", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF",
         "-DVCS_ANDROID_BUNDLE_GAME=OFF", "-DVCS_ANDROID_GAME_LIBRARY=ON",
         # the libmain.so built here is only linked against (the APK brings its own): no FFmpeg build for the player
         "-DPS2X_ENABLE_FFMPEG=OFF",
         "-DVCS_GAME=" + game, "-DVCS_GENERATED_DIR=" + cmake_path(gen)])
    run([cmake, "--build", tree, "--target", "ps2GameLibrary"])
    built = os.path.join(tree, "third_party", "PS2Recomp", "ps2xRuntime", "libvcs_game.so")
    if not os.path.isfile(built):
        fail("the build made no " + built)
    # symbols for the dynamic linker stay (dladdr, the crash handler); the rest goes
    library = game_library(game)
    run([ndk_tool(sdk, "llvm-strip"), "--strip-unneeded", "-o", library, built])
    print("[android] ok: %s (%s, %d MB)" % (library, game, os.path.getsize(library) >> 20))
    # [lcs] the same, gzipped: the launcher's import takes either (a third of the size: chat apps' file limits)
    with open(library, "rb") as src, gzip.open(library + ".gz", "wb", compresslevel=9) as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
    print("[android] ok: %s.gz (%d MB)" % (library, os.path.getsize(library + ".gz") >> 20))
    print("[android] your game library: game code from your disc, for your own phone only. Copy it to the phone and "
          "import it in the app (Disc > Import game library), or: python3 tools/android.py push-game")


STORIES_OUT = os.path.join(ROOT, "build", "android-stories")  # [stories] the both-cities APK's Gradle / CMake folder
STORIES_LIBS = os.path.join(OUT, "stories-libs")  # [stories] arm64-v8a/libstories_<game>.so, packaged by build --stories
# [stories] the guest's undefined symbols that may resolve to libmain.so: SDL, and the JNI glue left out of the guest
# (src/platform/CMakeLists.txt: touch_controls, stats, gamepads, power - namespace vcs_android)
STORIES_HOST_IMPORTS = ("SDL_", "_ZN11vcs_android")


def stories_library(game):
    return os.path.join(STORIES_LIBS, "arm64-v8a", "libstories_%s.so" % game)


def dynamic_symbols(sdk, library, defined):
    out = subprocess.run([ndk_tool(sdk, "llvm-nm"), "-D", "--defined-only" if defined else "--undefined-only",
                          "--format=just-symbols", library], capture_output=True, text=True, check=True).stdout
    return {line.strip() for line in out.splitlines() if line.strip()}


def build_game_stories(game="vcs", generated=None):
    """[stories] Both cities on Android (docs/ANDROID.md "Both cities on Android"): the game as libstories_<game>.so, a
    whole runtime with the generated code inside, everything local but stories_game_main (src/stories/game_so.map), SDL
    and the JNI glue left to the host libmain.so. Game code inside: for your own phone only, never shared."""
    gen = generated_dir(game, generated)
    if not os.path.isfile(os.path.join(gen, "register_functions.cpp")):
        fail("no generated code in %s: %s first" % (gen, RECOMPILE_HINT[game]))
    sdk = sdk_dir()
    ndk = os.path.join(sdk, "ndk", NDK_VERSION)
    cmake_bin = os.path.join(sdk, "cmake", CMAKE_VERSION, "bin")
    cmake = exe(os.path.join(cmake_bin, "cmake"))
    ninja = exe(os.path.join(cmake_bin, "ninja"))
    sdl = fetch_sdl()
    tree = os.path.join(OUT, "stories-" + game)
    os.makedirs(tree, exist_ok=True)
    run([cmake, "-S", ROOT, "-B", tree, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + cmake_path(ninja),
         "-DCMAKE_TOOLCHAIN_FILE=" + cmake_path(os.path.join(ndk, "build", "cmake", "android.toolchain.cmake")),
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-33", "-DANDROID_STL=c++_shared",
         "-DCMAKE_BUILD_TYPE=Release", "-DVCS_GLSLANG=" + cmake_path(glslang()),
         "-DFETCHCONTENT_SOURCE_DIR_SDL3=" + cmake_path(sdl),
         "-DPS2X_LTO=OFF", "-DPS2X_CPU=ARM64", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF",
         "-DVCS_ANDROID_BUNDLE_GAME=OFF", "-DVCS_ANDROID_GAME_LIBRARY=OFF", "-DVCS_ANDROID_STORIES_GUEST=ON",
         "-DPS2X_ENABLE_FFMPEG=OFF", "-DVCS_LSFG=OFF",  # the host's movies / frame generation are not the guest's
         "-DVCS_GAME=" + game, "-DVCS_GENERATED_DIR=" + cmake_path(gen)])
    run([cmake, "--build", tree, "--target", "ps2EntryRunner"])
    built = os.path.join(tree, "third_party", "PS2Recomp", "ps2xRuntime", "libstories_%s.so" % game)
    if not os.path.isfile(built):
        fail("the build made no " + built)
    library = stories_library(game)
    os.makedirs(os.path.dirname(library), exist_ok=True)
    run([ndk_tool(sdk, "llvm-strip"), "--strip-unneeded", "-o", library, built])
    exported = dynamic_symbols(sdk, library, True)
    if exported != {"stories_game_main"}:
        fail("%s exports more than stories_game_main: %s" % (library, ", ".join(sorted(exported - {"stories_game_main"})[:20])))
    # the guest's imports that libmain.so also defines must be SDL's or the glue's: anything else would bind to the
    # host's runtime at load (libmain.so is in the global group)
    hosts = (glob.glob(os.path.join(OUT, "gradle", "intermediates", "cxx", "**", "arm64-v8a", "libmain.so"), recursive=True) +
             glob.glob(os.path.join(STORIES_OUT, "gradle", "intermediates", "cxx", "**", "arm64-v8a", "libmain.so"), recursive=True))
    if hosts:
        host = max(hosts, key=os.path.getmtime)
        wrong = sorted(n for n in dynamic_symbols(sdk, library, False) & dynamic_symbols(sdk, host, True)
                       if not n.startswith(STORIES_HOST_IMPORTS))
        if wrong:
            fail("%s would bind %d of the host's runtime symbols (%s): leave them in the guest" %
                 (library, len(wrong), ", ".join(wrong[:20])))
        print("[android] imports checked against " + host)
    else:
        print("[android] note: no libmain.so built yet (python3 tools/android.py build): imports not checked")
    print("[android] ok: %s (%s, %d MB) - game code from your disc: your own phone only, never share it"
          % (library, game, os.path.getsize(library) >> 20))


def push_game(game="vcs"):
    """Development: the game library into the app's private folder as the launcher's import leaves it (read-only).
    [lcs] Each game its own folder (files/game/<id>/, GameFiles.gameLibrary): the other game's stays."""
    library = game_library(game)
    if not os.path.isfile(library):
        fail("no %s: python3 tools/android.py build-game --game %s first" % (library, game))
    a = adb()
    tmp = "/data/local/tmp/libvcs_game.so"
    run([a, "push", library, tmp])
    lib = "files/game/%s/libvcs_game.so" % game
    script = ("mkdir -p files/game/%s && chmod 644 %s 2>/dev/null; "
              "cp %s %s && chmod 444 %s && rm -f files/game_library_error.txt && ls -l %s"
              % (game, lib, tmp, lib, lib, lib))
    run([a, "shell", "run-as", PACKAGE, "sh", "-c", "'" + script + "'"])
    run([a, "shell", "rm", "-f", tmp])


def apk_path(release, game="vcs", personal=False, stories=False):
    kind = "release" if release else "debug"
    base = STORIES_OUT if stories else out_dir(game, personal)
    return os.path.join(base, "gradle", "outputs", "apk", kind, "app-%s.apk" % kind)


def install(release, game="vcs", personal=False):
    apk = apk_path(release, game, personal)
    if not os.path.exists(apk):
        fail("no %s: build first" % apk)
    # --user 0: the phone's own user, never a work profile (Island / Shelter) that also takes installs
    run([adb(), "install", "-r", "--user", "0", apk])


def set_env(pairs):
    """[android] files/env.txt: NAME=value switches the game reads at its start (game_main.cpp), for A/B runs on the
    phone (debug build: run-as). No pairs: show it; "clear": delete it. Takes effect at the next game start."""
    a = adb()
    if not pairs:
        run([a, "shell", "run-as", PACKAGE, "sh", "-c", "'cat files/env.txt 2>/dev/null || echo \"(no files/env.txt)\"'"])
        return
    if pairs == ["clear"]:
        run([a, "shell", "run-as", PACKAGE, "rm", "-f", "files/env.txt"])
        return
    for kv in pairs:
        if "=" not in kv or not kv.split("=", 1)[0].replace("_", "").isalnum() or "'" in kv:
            fail("env: NAME=value pairs (no quotes), or clear: %r" % kv)
    body = "\\n".join(pairs) + "\\n"
    run([a, "shell", "run-as", PACKAGE, "sh", "-c", "'printf \"%s\" > files/env.txt && cat files/env.txt'" % body])

def log():
    subprocess.run([adb(), "logcat", "-v", "time", "-s", "vcs", "SDL", "SDL/APP", "AndroidRuntime", "DEBUG", "libc"])


def start():
    a = adb()
    run([a, "logcat", "-c"])
    run([a, "shell", "am", "start", "--user", "0", "-n", ACTIVITY])
    log()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["build", "build-game", "push-game", "install", "run", "all", "log", "env"])
    p.add_argument("--release", action="store_true", help="the release APK, signed with your key (VCS_SIGN_STORE, VCS_SIGN_ALIAS, VCS_SIGN_PASS)")
    p.add_argument("--personal", action="store_true", help="build: the game inside the APK (a personal build, never shared)")
    p.add_argument("--stories", action="store_true",
                   help="[stories] build-game: the game as libstories_<game>.so; build: the both-cities APK with both "
                        "(a personal build, never shared; docs/ANDROID.md 'Both cities on Android')")
    p.add_argument("--game", choices=GAMES, default="vcs",
                   help="[lcs] build-game / push-game / --personal: which game (default vcs; lcs = Liberty City Stories)")
    p.add_argument("--generated", help="the game's generated EE code (default generated/ee; generated-lcs/ee for lcs)")
    p.add_argument("pairs", nargs="*", help="env: NAME=value switches for files/env.txt (none: show it; clear: delete it)")
    a = p.parse_args()
    if a.command in ("build", "all"):
        build(a.release, a.personal, a.game, a.generated, a.stories)
    if a.command == "build-game":
        if a.stories:
            build_game_stories(a.game, a.generated)
        else:
            build_game(a.game, a.generated)
    if a.command == "push-game":
        push_game(a.game)
    if a.command in ("install", "all"):
        install(a.release, a.game, a.personal)
    if a.command in ("run", "all"):
        start()
    if a.command == "log":
        log()
    if a.command == "env":
        set_env(a.pairs)


if __name__ == "__main__":
    main()
