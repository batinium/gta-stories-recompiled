#!/usr/bin/env python3
"""[android] Checks that run before the long PC-side steps, so a missing tool or a too-long path is one clear message
at the start instead of a failure half an hour in (docs/ANDROID.md, Windows).

    python3 tools/preflight.py [--apk]      # the same as: python3 tools/android.py preflight

Three things, all stdlib only (tools/recompile.py imports this before tools/toolchain.py):
  - identify_disc / resolve_game: which game a disc image is (SYSTEM.CNF's boot file, read with tools/vcs_disc.py; no
    game data is written to disk), so `recompile.py <iso>` needs no --game.
  - path_problem / require_short_path: on Windows, would the build's deepest file pass the 260-character limit?
  - missing_tools / require_tools: Python, Android SDK, NDK, CMake, glslangValidator, git (and a JDK 17+ for the APK),
    all listed at once with how to get each one.
Every check takes its environment as arguments (a Host), so it is testable on another platform
(tools/preflight_check.py).
"""
import ntpath
import os
import posixpath
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

NDK_VERSION = "28.2.13676358"  # = tools/android.py (kept in step by preflight_check.py)
CMAKE_VERSION = "3.31.5"
MIN_PYTHON = (3, 8)
MIN_JDK = 17
WIN_PATH_LIMIT = 260  # MAX_PATH, counting the terminating NUL: 259 characters of path

GAME_BOOT = {"vcs": "SLUS_215.90", "lcs": "SLUS_214.23"}
GAME_NAME = {"vcs": "Vice City Stories", "lcs": "Liberty City Stories"}


# --------------------------------------------------------------------------------------------- the disc

def identify_disc(iso):
    """What `iso` is: (game, boot, error). game: "vcs" / "lcs" or "" (another game, or not a disc); boot: the boot
    file from SYSTEM.CNF ("" if unreadable); error: a message when it is no usable disc ("" when it is VCS or LCS, or
    when the file does not exist: the caller's own lookup says that). A VCS / LCS disc with an unsupported
    executable build still names its game (the build step reports the build)."""
    import vcs_disc
    result = vcs_disc.check(iso, tuple(vcs_disc.KNOWN_GAMES))
    code = result.get("code")
    if code == "not_found":
        return "", "", ""
    boot = result.get("boot", "")
    game = result.get("game") or next((g for g, b in GAME_BOOT.items() if b == boot.upper()), "")
    if game in GAME_BOOT:
        return game, boot, ""
    if code == "not_iso":
        return "", "", "%s: %s" % (iso, result["message"])
    if code == "not_ps2":
        return "", boot, "%s: %s" % (iso, result["message"])
    what = vcs_disc.OTHER_RELEASES.get(boot.upper(), boot or "another game")
    if boot and what != boot:
        what = "%s (%s)" % (what, boot)
    return "", boot, "this disc is %s: expected VCS (%s) or LCS (%s)" % (
        what, GAME_BOOT["vcs"], GAME_BOOT["lcs"])


def resolve_game(iso, given):
    """The game to build for `iso`: (game, error). `given` is --game ("" / None: not given). Without --game the
    disc's own game; with it, it must agree with the disc. iso None / not a file: (given or "", "")."""
    if not iso or not os.path.isfile(iso):
        return given or "", ""
    game, boot, error = identify_disc(iso)
    if error:
        return given or "", error
    if given and game and given != game:
        return given, ("--game %s, but this disc is %s (%s). Use --game %s, or leave --game out and the game is "
                       "taken from the disc" % (given, GAME_NAME[game], boot, game))
    return given or game, ""


def game_from_args(argv):
    """tools/recompile.py's start (before toolchain is imported: --game picks its build folder). Returns (the game
    to set, error). Reads the positional disc and --game out of argv without the real parser."""
    import argparse
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("iso", nargs="?")
    ap.add_argument("--game")
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    try:
        a, _ = ap.parse_known_args(argv)
    except SystemExit:
        return None, ""  # the real parser reports it
    if a.check:
        return a.game, ""
    return resolve_game(a.iso, a.game)


# --------------------------------------------------------------------------------------------- path length

_VMA_TAIL = ("_deps/vulkanmemoryallocator-subbuild/vulkanmemoryallocator-populate-prefix/src/"
             "vulkanmemoryallocator-populate-stamp/vulkanmemoryallocator-populate-gitclone-lastrun.txt")
# The deepest files each step is known to make, relative to the repository root (measured on a finished build: the
# FetchContent stamp of the Vulkan Memory Allocator dependency is the longest file in every CMake tree).
# "recompile" and "build" share the desktop tree, tc.BUILD/release (llvm-mingw); build_dir replaces "build". Its
# deepest file measured 2026-11-22 is 159 characters after '<build>/' (CMake hashes object paths past 250), so the
# shared estimate below (179) keeps a margin. Object files are not counted: CMake shortens them itself.
_DESKTOP = ["build/lcs/release/" + _VMA_TAIL]
_DEEPEST = {
    "recompile": _DESKTOP,
    "build": _DESKTOP,  # tools/build.py (the desktop game)
    "build-game": ["build/android/game-lcs/cmake/" + _VMA_TAIL],
    "apk": ["build/android/cxx/RelWithDebInfo/xxxxxxxx/arm64-v8a/" + _VMA_TAIL],  # Gradle's CMake tree
}
_OBJECT = "build/android/game-lcs/cmake/third_party/PS2Recomp/ps2xRuntime/CMakeFiles/ps2xRuntime.dir/"


def longest_path(root, step, build_dir=None, generated_dir=None):
    """(length, path): the longest path `step` ("recompile", "build-game", "apk") is expected to create under `root`.
    build_dir: the build folder if it is not <root>/build; generated_dir: where the generated code is (its longest
    file name makes an object path)."""
    base = build_dir or os.path.join(root, "build")
    paths = []
    for rel in _DEEPEST[step]:
        head, _, tail = rel.partition("/")
        paths.append(base + "/" + tail)
    if step in ("build-game", "apk") and generated_dir and os.path.isdir(generated_dir):
        names = [n for n in os.listdir(generated_dir) if n.endswith((".cpp", ".h"))]
        if names:
            longest = max(names, key=len)
            paths.append(base + "/" + _OBJECT.partition("/")[2] + "generated/ee/" + longest + ".obj")
    best = max(paths, key=len)
    return len(best), best


def path_problem(root, step, windows=None, build_dir=None, generated_dir=None, limit=WIN_PATH_LIMIT):
    """None when the build tree fits, else a message. Only Windows has the limit (`windows` overrides for tests)."""
    if windows is None:
        windows = os.name == "nt"
    if not windows:
        return None
    length, path = longest_path(root, step, build_dir, generated_dir)
    margin = 8  # object file names and other files the estimate does not know
    if length + margin < limit:
        return None
    # the advice names the folder that is too long: the build folder when it was set apart (VCS_BUILD_DIR), else the
    # repository (a build folder inside it moves with it)
    base = os.path.abspath(build_dir) if build_dir else None
    inside = base is None or os.path.normcase(base).startswith(os.path.normcase(os.path.abspath(root)) + os.sep)
    anchor = root if inside else base
    room = limit - 1 - margin - (length - len(anchor))
    if inside:
        advice = ("Move the whole folder to a short one, for example C:\\gta (the repository path may be at most about "
                  "%d characters; yours is %d), and run the command again from there." % (max(room, 0), len(anchor)))
    else:
        advice = ("Use a shorter build folder (VCS_BUILD_DIR), for example C:\\gta\\build (it may be at most about %d "
                  "characters; yours is %d: %s)." % (max(room, 0), len(anchor), anchor))
    return ("this folder is too deep for Windows' %d-character path limit: the build would create a path of about %d "
            "characters (%s ...).\n  %s" % (limit, length + margin, path[:70], advice))


def require_short_path(root, step, windows=None, build_dir=None, generated_dir=None):
    message = path_problem(root, step, windows, build_dir, generated_dir)
    if message:
        print("[preflight] " + message, file=sys.stderr)
        sys.exit(1)


# --------------------------------------------------------------------------------------------- tools

class Host:
    """The machine as the checks see it; the defaults are the real one, tests pass fakes."""

    def __init__(self, platform=None, env=None, exists=None, which=None, python=None, java_major=None, machine=None):
        self.platform = platform or sys.platform
        self.env = os.environ if env is None else env
        self.exists = exists or os.path.exists
        self.which = which or shutil.which
        self.python = python or sys.version_info[:2]
        self._java_major = java_major or _java_major
        self.machine = (machine or _platform_machine()).lower()

    @property
    def windows(self):
        return self.platform == "win32"

    @property
    def mac(self):
        return self.platform == "darwin"

    @property
    def arm64(self):
        return self.machine in ("arm64", "aarch64")

    def join(self, *parts):
        return (ntpath if self.windows else posixpath).join(*parts)

    def java_major(self, java):
        return self._java_major(java)


def _platform_machine():
    import platform
    return platform.machine() or "x86_64"


# --------------------------------------------------------------------------------------------- the toolchain folder
# tools/toolchain_fetch.py (python3 tools/android.py setup) downloads what is missing into ONE folder per user, shared
# by every copy of the repository / release (the NDK is about 1 GB to download and 2.8 GB unpacked: not again per
# release): ~/.vcs-toolchain (%USERPROFILE%\.vcs-toolchain), or VCS_ANDROID_TOOLCHAIN. Nothing is written anywhere
# else and nothing needs admin rights. Every check below also looks there, so after a fetch the build just works.

TOOLCHAIN_ENV = "VCS_ANDROID_TOOLCHAIN"


def toolchain_dir(host):
    given = host.env.get(TOOLCHAIN_ENV)
    if given:
        return given
    home = (host.env.get("USERPROFILE") if host.windows else None) or host.env.get("HOME") or \
        host.env.get("USERPROFILE") or os.path.expanduser("~")
    return host.join(home, ".vcs-toolchain")


def toolchain_paths(host):
    """Where tools/toolchain_fetch.py puts each tool inside the toolchain folder."""
    t = toolchain_dir(host)
    exe = ".exe" if host.windows else ""
    jdk = host.join(t, "jdk-17")
    return {
        "dir": t,
        "sdk": host.join(t, "android-sdk"),
        # macOS' JDK is a bundle: its home is Contents/Home
        "jdk": host.join(jdk, "Contents", "Home") if host.mac else jdk,
        "glslang": [host.join(t, "glslang", "bin", "glslangValidator" + exe), host.join(t, "glslang", "bin", "glslang" + exe)],
        "git": host.join(t, "mingit", "cmd", "git.exe") if host.windows else None,
    }


def setup_command(host):
    """The command that fetches what is missing, as the user types it on this machine."""
    return "python tools\\android.py setup" if host.windows else "python3 tools/android.py setup"


def _java_major(java):
    """The major version `java -version` reports (1.8 -> 8), None if it cannot be run."""
    try:
        r = subprocess.run([java, "-version"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r'version "(\d+)(?:\.(\d+))?', r.stderr + r.stdout)
    if not m:
        return None
    major = int(m.group(1))
    return int(m.group(2) or 0) if major == 1 else major


def sdk_candidates(host):
    """Where the Android SDK may be: ANDROID_HOME, ANDROID_SDK_ROOT, Android Studio's defaults (tools/android.py)."""
    local = host.env.get("LOCALAPPDATA")
    home = host.env.get("HOME") or host.env.get("USERPROFILE") or ""
    return [d for d in (host.env.get("ANDROID_HOME"), host.env.get("ANDROID_SDK_ROOT"),
                        host.join(home, "Library", "Android", "sdk") if home else None,
                        host.join(home, "Android", "Sdk") if home else None,
                        host.join(local, "Android", "Sdk") if local else None,
                        toolchain_paths(host)["sdk"]) if d]


def sdk_complete(host, sdk):
    """Has `sdk` the pinned NDK and CMake (with its Ninja)?"""
    exe = ".exe" if host.windows else ""
    cmake_bin = host.join(sdk, "cmake", CMAKE_VERSION, "bin")
    return (host.exists(host.join(sdk, "ndk", NDK_VERSION)) and host.exists(host.join(cmake_bin, "cmake" + exe))
            and host.exists(host.join(cmake_bin, "ninja" + exe)))


def find_sdk(host):
    """The SDK the build uses: the first candidate with the NDK and CMake (so a fetched toolchain SDK wins over an
    Android Studio SDK without them), else the first one that exists (its report names what it lacks)."""
    found = [d for d in sdk_candidates(host) if host.exists(d)]
    for d in found:
        if sdk_complete(host, d):
            return d
    return found[0] if found else None


def _install_hint(host, windows, mac, linux):
    return windows if host.windows else mac if host.platform == "darwin" else linux


def missing_tools(host=None, apk=False, root=None):
    """[(what, how to get it)] for everything a build needs that this machine lacks. apk: also the JDK."""
    host = host or Host()
    problems = []
    if tuple(host.python) < MIN_PYTHON:
        problems.append(("Python %d.%d or newer (this is %d.%d)" % (MIN_PYTHON + tuple(host.python)[:2]),
                         _install_hint(host, "https://www.python.org/downloads/ (tick 'Add python.exe to PATH'), "
                                       "or: winget install Python.Python.3.12", "brew install python",
                                       "apt install python3")))
    sdk = find_sdk(host)
    sdkmanager = None
    if sdk:
        for rel in (("cmdline-tools", "latest", "bin", "sdkmanager"), ("tools", "bin", "sdkmanager")):
            cand = host.join(sdk, *rel) + (".bat" if host.windows else "")
            if host.exists(cand):
                sdkmanager = cand
                break
        pkg_cmd = ('"%s" "ndk;%s" "cmake;%s"' % (sdkmanager, NDK_VERSION, CMAKE_VERSION)) if sdkmanager else (
            "Android Studio > Settings > Android SDK > SDK Tools > tick 'Show Package Details', install NDK (Side by "
            "side) %s and CMake %s" % (NDK_VERSION, CMAKE_VERSION))
        ndk = host.join(sdk, "ndk", NDK_VERSION)
        if not host.exists(ndk):
            problems.append(("Android NDK %s (not in %s)" % (NDK_VERSION, sdk), pkg_cmd))
        cmake_bin = host.join(sdk, "cmake", CMAKE_VERSION, "bin")
        exe = ".exe" if host.windows else ""
        if not host.exists(host.join(cmake_bin, "cmake" + exe)) or not host.exists(host.join(cmake_bin, "ninja" + exe)):
            problems.append(("Android SDK CMake %s with its Ninja (not in %s)" % (CMAKE_VERSION, sdk), pkg_cmd))
    else:
        problems.append(("Android SDK (looked in ANDROID_HOME, ANDROID_SDK_ROOT and Android Studio's default folder)",
                         "install Android Studio (https://developer.android.com/studio), start it once so it "
                         "downloads the SDK, then install NDK %s and CMake %s (Settings > Android SDK > SDK Tools > "
                         "'Show Package Details'); or the command-line tools "
                         "(https://developer.android.com/studio#command-line-tools-only) and "
                         "sdkmanager \"ndk;%s\" \"cmake;%s\". If the SDK is somewhere else, set ANDROID_HOME to it" % (
                             NDK_VERSION, CMAKE_VERSION, NDK_VERSION, CMAKE_VERSION)))
    if not _find_glslang(host, root):
        problems.append(("glslangValidator (compiles the Vulkan shaders)", _install_hint(
            host, "install the LunarG Vulkan SDK (https://vulkan.lunarg.com/sdk/home#windows); its installer sets "
            "VULKAN_SDK, so open a NEW terminal afterwards. Or VCS_GLSLANG=<full path of glslangValidator.exe>",
            "brew install glslang  (or VCS_GLSLANG=<path>)", "apt install glslang-tools  (or VCS_GLSLANG=<path>)")))
    if not find_git(host):
        problems.append(("git (fetches SDL3 and the other dependencies)", _install_hint(
            host, "winget install --id Git.Git -e   (or https://git-scm.com/download/win)",
            "xcode-select --install  (or brew install git)", "apt install git")))
    if apk:
        java = _find_java(host)
        version = host.java_major(java) if java else None
        if not java or version is None:
            problems.append(("a JDK %d or newer (for Gradle; none found: JAVA_HOME / PATH)" % MIN_JDK, _install_hint(
                host, "winget install EclipseAdoptium.Temurin.17.JDK   (or Android Studio's bundled JDK: set JAVA_HOME "
                "to its jbr folder, e.g. C:\\Program Files\\Android\\Android Studio\\jbr)",
                "brew install --cask temurin@17", "apt install openjdk-17-jdk")))
        elif version < MIN_JDK:
            problems.append(("a JDK %d or newer (found Java %d at %s)" % (MIN_JDK, version, java), _install_hint(
                host, "winget install EclipseAdoptium.Temurin.17.JDK, then set JAVA_HOME to it",
                "brew install --cask temurin@17, then set JAVA_HOME", "apt install openjdk-17-jdk, then set JAVA_HOME")))
    return problems


def _find_glslang(host, root=None):
    exe = ".exe" if host.windows else ""
    cands = [host.env.get("VCS_GLSLANG")]
    if host.env.get("VULKAN_SDK"):
        cands.append(host.join(host.env["VULKAN_SDK"], "Bin" if host.windows else "bin", "glslangValidator" + exe))
    if root:  # the macOS build's own copy (tools/shader_tools.py)
        cands.append(host.join(host.env.get("VCS_BUILD_DIR") or host.join(root, "build"), "build-tools",
                               "shader-tools", "build", "bin", "glslang" + exe))
    for c in cands:
        if c and host.exists(c):
            return c
    found = host.which("glslangValidator") or host.which("glslang")
    if found:
        return found
    for c in toolchain_paths(host)["glslang"]:  # tools/toolchain_fetch.py's copy
        if host.exists(c):
            return c
    return None


def find_glslang(host, root=None):
    return _find_glslang(host, root)


def find_git(host):
    """git on PATH, else (Windows) the MinGit tools/toolchain_fetch.py fetched."""
    found = host.which("git")
    if found:
        return found
    mingit = toolchain_paths(host)["git"]
    return mingit if mingit and host.exists(mingit) else None


def java_candidates(host):
    """JAVA_HOME's java, the toolchain folder's JDK 17, java on PATH (in that order)."""
    exe = "java.exe" if host.windows else "java"
    cands = []
    if host.env.get("JAVA_HOME"):
        cands.append(host.join(host.env["JAVA_HOME"], "bin", exe))
    cands.append(host.join(toolchain_paths(host)["jdk"], "bin", exe))
    cands = [c for c in cands if host.exists(c)]
    on_path = host.which("java")
    if on_path:
        cands.append(on_path)
    return cands


def _find_java(host):
    """The java the build would use: the first candidate that is 17 or newer, else the first one at all."""
    cands = java_candidates(host)
    for c in cands:
        version = host.java_major(c)
        if version is not None and version >= MIN_JDK:
            return c
    return cands[0] if cands else None


def format_problems(problems, host=None):
    host = host or Host()
    lines = ["[preflight] %d thing%s missing for the build:" % (len(problems), "" if len(problems) == 1 else "s"),
             "  Quickest: run: %s" % setup_command(host),
             "  (it lists what it would download and the size, asks first, then fetches the missing tools from their "
             "official sources into %s; nothing is installed system-wide)" % toolchain_dir(host)]
    for i, (what, how) in enumerate(problems, 1):
        lines.append("  %d. %s\n     or get it yourself: %s" % (i, what, how))
    lines.append("Then run the same command again (after installing something yourself, open a new terminal first).")
    return "\n".join(lines)


def require_tools(host=None, apk=False, root=None):
    """Exit 1 with one message listing everything missing; else return."""
    host = host or Host()
    problems = missing_tools(host, apk, root)
    if problems:
        print(format_problems(problems, host), file=sys.stderr)
        sys.exit(1)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    require_tools(apk="--apk" in argv, root=root)
    require_short_path(root, "apk" if "--apk" in argv else "build-game")
    print("[preflight] ok: Python, Android SDK, NDK %s, CMake %s, glslangValidator, git%s found; path length fine" % (
        NDK_VERSION, CMAKE_VERSION, ", JDK" if "--apk" in argv else ""))


if __name__ == "__main__":
    main()
