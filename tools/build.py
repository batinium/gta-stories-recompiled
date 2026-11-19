#!/usr/bin/env python3
"""Build the game (configures CMake the first time).

    python tools/build.py                       build vcs (build/vcs[.exe], with its DLLs and vcs.ini)
    python tools/build.py --recompile [ISO]     recompile from your disc first, then build - the whole pipeline
    python tools/build.py --recompile-if-needed [ISO]   the same, recompiling only when generated/ is not current
                                                (generated/recompile_stamp.json; what the launcher runs)
    python tools/build.py --launcher            build the launcher (GTAStoriesLauncher(.exe) in the repository root; no disc)
    python tools/build.py --install-deps        Linux: install the missing packages (the desktop's password prompt)
    python tools/build.py --progress json       also print `@vcs {...}` events for the launcher (toolchain.py)
    python tools/build.py --quick               no link-time optimisation: fast rebuilds while changing code
    python tools/build.py --iop-tests           build and run the IOP / SPU2 unit tests (build/iop_tests, no disc)
    python tools/build.py --check-deps          Linux: list the missing packages and the command that installs them
    python tools/build.py --opt none|lto|pgo    optimisation of the game, kept until changed: lto (default; ThinLTO,
                                                the game to play), none (= --quick), pgo (ThinLTO + profile-guided:
                                                trains first when build/pgo/vcs.profdata is missing or with --train;
                                                training needs the rig's card copies, build/rig/saves)
    python tools/build.py --game lcs [--generated DIR] [--recompile ISO] [--quick]
                                                [lcs] Liberty City Stories: build/lcs/lcs, from generated-lcs/ (or DIR:
                                                the folder tools/recompile.py --game lcs --out wrote, or its ee/);
                                                its own CMake tree and vcs.ini in build/lcs, so VCS's build is
                                                untouched. Says when the generated code is stale (game/lcs or the
                                                recompiler changed since it was made): --recompile[-if-needed] then

The optimisation is kept in the CMake cache: after --quick every build stays quick until `--opt lto`. Switching
recompiles the whole game (~6-10 min). The last line of each build says which one was built.

Progress: every build step shows `[build] <target> done/total %, elapsed, ~time left` (Ninja's step count; redrawn in
place on a terminal, a line per 5% / minute otherwise); CMake configure and the recompiler show their elapsed time.
With link-time optimisation the game's link is a step of its own (minutes: the whole game is optimised there), timed
against the last one (link_times.json in the build folder; docs/LAUNCHER.md).

PGO training: an instrumented build in build/pgo-gen plays tools/bench.py (city spots, an AI drive: from the rig's Fort
Baxter card copy, or a new game when there is none) and the reference scenes' boot / flyover / cutscene
(tools/scenes.py); the profiles go to build/pgo/vcs.profdata. A profile
from older code still works: the functions that changed are optimised as without one.

ISO defaults to VCS_ISO, iso=... in build/vcs.ini or vcs.ini, or a '*Vice City Stories*.iso' in the repository root.
A successful game build writes build/build_info.json (source version, optimisation, disc) - the launcher compares it
with the source to know whether the game needs building.
Windows: builds with llvm-mingw (clang, lld, libc++ and the Windows headers), downloaded once into build/build-tools -
nothing to install. VCS_TOOLCHAIN=msvc builds with Visual Studio 2022+ instead ("Desktop development with C++" and "C++
Clang tools for Windows"; its environment is set up automatically).
CMake 3.21+ and Ninja 1.10+: the first that runs on PATH or wherever installers put them (Visual Studio's, Kitware's,
winget, scoop, Chocolatey, pip, CLion...), else downloaded once into build/build-tools (toolchain.build_tools);
VCS_CMAKE / VCS_NINJA=<path> choose them.
Linux: clang 19+ with lld, glslang and the development packages SDL3 and FFmpeg need (docs/LINUX.md); --check-deps
lists what is missing. The first configure checks them too.
"""
import argparse
import filecmp
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _game_from_argv():
    """[lcs] --game before toolchain is imported: it picks the game's build folder (toolchain.GAME: build/lcs)."""
    for i, arg in enumerate(sys.argv[1:], 1):
        if arg == "--game" and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
        if arg.startswith("--game="):
            return arg.partition("=")[2]
    return None


if __name__ == "__main__" and _game_from_argv():
    os.environ["VCS_TOOLS_GAME"] = _game_from_argv()
import toolchain as tc  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recompile", nargs="?", const="", metavar="ISO",
                    help="run tools/recompile.py first (disc image optional, see above)")
    ap.add_argument("--target", default="ps2EntryRunner", help="CMake target (default: the game, ps2EntryRunner)")
    ap.add_argument("--reconfigure", action="store_true", help="run CMake configure again")
    ap.add_argument("-v", "--verbose", action="store_true", help="show all build output")
    ap.add_argument("--iop-tests", action="store_true", help="build and run the ps2xIOP unit tests (SPU2) instead")
    ap.add_argument("--opt", choices=("none", "lto", "pgo"), help="optimisation of the game (kept until changed)")
    ap.add_argument("--quick", action="store_true", help="same as --opt none: fast rebuilds, slower game")
    ap.add_argument("--train", action="store_true", help="with --opt pgo: train a new profile first")
    ap.add_argument("--check-deps", action="store_true", help="Linux: check the build dependencies and stop")
    ap.add_argument("--install-deps", action="store_true",
                    help="Linux: install the missing packages (pkexec: the desktop's password prompt) and stop")
    ap.add_argument("--recompile-if-needed", nargs="?", const="", metavar="ISO",
                    help="recompile first only when the generated code is not current")
    ap.add_argument("--dll", action="store_true",
                    help="Windows: the game as <game>_game.dll plus stories.exe, the one-process host for both cities "
                         "(docs/STORIES.md 'One process'; use a build folder of its own: VCS_BUILD_DIR)")
    ap.add_argument("--oneproc", action="store_true",
                    help="Windows: both games as DLLs plus stories.exe in <build>/oneproc/{lcs,vcs} (two build trees "
                         "of their own), for tools/stories.py --oneproc and the launcher's 'One program' switch")
    ap.add_argument("--generated-lcs", metavar="DIR", help="with --oneproc: LCS's generated code (default generated-lcs/)")
    ap.add_argument("--generated-vcs", metavar="DIR", help="with --oneproc: VCS's generated code (default generated/)")
    ap.add_argument("--launcher", action="store_true", help="build the launcher (GTAStoriesLauncher.exe) instead of the game")
    ap.add_argument("--progress", choices=("text", "json"), default="text", help="json: events for the launcher too")
    ap.add_argument("--game", choices=tc.GAMES, default="vcs",
                    help="[lcs] the game: vcs (default) or lcs (Liberty City Stories, built in build/lcs)")
    ap.add_argument("--generated", metavar="DIR",
                    help="[lcs] the game's generated code: the folder recompile.py wrote (or its ee/); default "
                         "generated/ (vcs), generated-lcs/ (lcs)")
    a = ap.parse_args()
    if a.game != tc.GAME:  # build.py imported as a module with another game: the folders would be the wrong ones
        ap.error("--game %s, but the tools were set up for %s" % (a.game, tc.GAME))
    if a.game != "vcs" and (a.launcher or a.iop_tests or a.opt == "pgo"):
        ap.error("--launcher, --iop-tests and --opt pgo are VCS's (without --game)")
    if a.progress == "json":
        tc.set_json_progress()
    if a.quick:
        if a.opt not in (None, "none"):
            ap.error("--quick is --opt none; it can't be combined with --opt " + a.opt)
        a.opt = "none"
    if a.recompile is not None and a.recompile_if_needed is not None:
        ap.error("--recompile and --recompile-if-needed: one or the other")

    if a.oneproc:
        sys.exit(build_oneproc(a))
    env = tc.build_env()
    if a.check_deps:
        if tc.IS_WINDOWS:
            sys.exit(0)
        game = tc.check_linux_deps(env)
        launcher = tc.check_linux_deps(env, tc.LAUNCHER_LINUX_DEPS)
        sys.exit(0 if game and launcher else 1)
    if a.install_deps:
        sys.exit(install_deps(env))
    tc.fix_future_files()  # else Ninja re-runs CMake until it gives up (a release zip from another time zone)
    if a.launcher:  # its own packages, checked in build_launcher - not the game's (CI builds it on a bare system)
        sys.exit(0 if build_launcher(env, a.verbose) else 1)
    if not tc.IS_WINDOWS and not os.path.isfile(os.path.join(tc.BUILD_DIR, "build.ninja")) and not tc.check_linux_deps(env):
        tc.fail("missing build dependencies (above)")
    if a.iop_tests:
        sys.exit(iop_tests(env))
    import recompile
    gen = recompile.generated_root(a.game, a.generated)  # [lcs] generated/ (vcs), generated-lcs/ or --generated
    if a.game != "vcs" and os.path.normcase(gen) == os.path.normcase(recompile.GENERATED):
        ap.error("--generated: generated/ is VCS's code; LCS's goes elsewhere (default generated-lcs/)")
    other = {} if a.game == "vcs" else {"game": a.game, "out_dir": gen}
    iso_arg = a.recompile if a.recompile is not None else a.recompile_if_needed
    if a.recompile_if_needed is not None:
        why = recompile.why_recompile(a.game, gen)
        if why:
            print("[build] recompiling: " + why)
            recompile.recompile(a.recompile_if_needed or None, env, a.verbose, **other)
        else:
            print("[build] generated code is current: no recompile")
            tc.find_iso(a.recompile_if_needed or None)  # the disc is still checked (and remembered below)
    elif a.recompile is not None:
        recompile.recompile(a.recompile or None, env, a.verbose, **other)
    elif not os.path.isdir(os.path.join(gen, "ee")):
        print("[build] no generated code yet: run with --recompile (only the tools will be built)" +
              ("" if a.game == "vcs" else " - looked in " + tc.show(gen)))
    stale = None
    if a.game != "vcs" and a.recompile is None and a.recompile_if_needed is None and \
            os.path.isdir(os.path.join(gen, "ee")):
        # [lcs] VCS's builds leave this to --recompile-if-needed (the launcher); LCS's say it every time: its function
        # map still changes as bugs in it are found, and a build from the old code looks like it worked
        stale = recompile.why_recompile(a.game, gen)
        if stale:
            print("[build] WARNING: the generated code in %s is stale: %s. Building it anyway; recompile with "
                  "--recompile <your LCS iso> (or --recompile-if-needed)" % (tc.show(gen), stale))

    defines = None
    if a.opt == "none":
        defines = {"PS2X_LTO": "OFF", "PS2X_PGO": ""}
    elif a.opt == "lto":
        defines = {"PS2X_LTO": "THIN", "PS2X_PGO": ""}
    elif a.opt == "pgo":
        if a.train or not os.path.isfile(PROFDATA):
            train(env, a.verbose)
        defines = {"PS2X_LTO": "THIN", "PS2X_PGO": PROFDATA.replace(os.sep, "/")}
    # [lcs] the game and its generated code (VCS_GAME / VCS_GENERATED_DIR), only when the tree's cache differs: VCS
    # without --generated passes nothing, as before
    if a.game != "vcs" or a.generated or (tc.cached_value(tc.BUILD_DIR, "VCS_GAME") or "vcs") != "vcs":
        game_defines = recompile.cmake_game_defines(tc.BUILD_DIR, a.game, gen)
        if game_defines:
            defines = dict(defines or {}, **game_defines)
    # [stories] the game as a DLL for stories.exe (kept in the tree's cache: switched off again only by a build without
    # --dll in that tree)
    if a.dll or (tc.cached_value(tc.BUILD_DIR, "VCS_GAME_DLL") or "OFF").upper() in ("ON", "1", "TRUE"):
        if a.dll and not tc.IS_WINDOWS:
            ap.error("--dll is for Windows")
        defines = dict(defines or {}, VCS_GAME_DLL="ON" if a.dll else "OFF")
    tc.configure(env, force=a.reconfigure or configured_without_game(gen), defines=defines)
    print(f"[build] building {a.target}")
    tc.stage("compile", "Compiling the game" if a.target == "ps2EntryRunner" else "Building " + a.target)
    if not tc.cmake_build(env, a.target, "build.log", a.verbose, link=game_link_stage() if a.target == "ps2EntryRunner" else None):
        sys.exit(1)
    if a.dll and a.target == "ps2EntryRunner":
        if not tc.cmake_build(env, "stories", "build_stories.log", a.verbose):
            sys.exit(1)
        dll = os.path.join(os.path.dirname(GAME_EXE), a.game + "_game.dll")
        if not os.path.isfile(dll):
            tc.fail("build finished but " + tc.show(dll) + " is missing", "build_failed")
        print("[build] ok: " + tc.show(dll) + " and stories.exe (" + built_opt() + ")")
        return
    if a.target == "ps2EntryRunner":
        if not os.path.isfile(GAME_EXE):
            tc.fail("build finished but " + tc.show(GAME_EXE) + " is missing", "build_failed")
        if a.game == "vcs":
            move_from_old_place()
        iso = remember_iso(iso_arg or None)
        write_build_info(iso, a.game, gen)
        print("[build] ok: " + tc.show(GAME_EXE) + " (" + built_opt() + ")")
        if stale:
            print("[build] WARNING: built from stale generated code (%s): recompile" % stale)
        tc.event(result="ok", exe=tc.show(GAME_EXE).replace(os.sep, "/"), opt=built_opt())


def build_oneproc(a):
    """[stories] --oneproc: lcs_game.dll and vcs_game.dll (each in a CMake tree of its own: the two games' generated
    code) and stories.exe, in <build>/oneproc/lcs and <build>/oneproc/vcs - where tools/stories.py --oneproc and the
    launcher look. The normal games (build/vcs.exe, build/lcs/lcs.exe) are not touched."""
    if not tc.IS_WINDOWS:
        print("[build] --oneproc is for Windows")
        return 1
    base = os.path.join(tc.BUILD if os.environ.get("VCS_BUILD_DIR") else tc._BASE_BUILD, "oneproc")
    env = dict(os.environ)
    env.pop("VCS_TOOLS_GAME", None)
    # one llvm-mingw for both trees (else each tree's build-tools would download its own)
    env["VCS_LLVM_MINGW"] = os.path.dirname(tc.llvm_mingw_bin())
    opt = ["--quick"] if a.quick else (["--opt", a.opt] if a.opt else [])
    for game, gen in (("lcs", a.generated_lcs), ("vcs", a.generated_vcs)):
        env["VCS_BUILD_DIR"] = os.path.join(base, game)
        cmd = [sys.executable, os.path.abspath(__file__), "--game", game, "--dll"] + opt + (["--generated", gen] if gen else [])
        if a.reconfigure:
            cmd.append("--reconfigure")
        print("[build] one program: %s into %s" % (game, tc.show(env["VCS_BUILD_DIR"])), flush=True)
        if subprocess.call(cmd, env=env) != 0:
            return 1
    print("[build] ok: %s (tools/stories.py --oneproc; the launcher's GTA Stories > One program)" % tc.show(base))
    return 0


# the game binary: vcs (build/vcs, or VCS_EXE), [lcs] lcs in the game's own folder (build/lcs/lcs)
GAME_EXE = tc.VCS_EXE if tc.GAME == "vcs" else tc.game_exe(tc.BUILD_DIR)


BUILD_INFO = os.path.join(tc.BUILD, "build_info.json")


def write_build_info(iso, game="vcs", gen=None):
    """build/build_info.json: what the game in build/ was built from. The launcher compares it with the source
    (tools/doctor.py) to tell "Play" from "Update the build". [lcs] build/lcs/build_info.json for LCS."""
    import recompile
    stamp = recompile.read_stamp(gen or recompile.GENERATED) or {}
    info = {"source": tc.source_version(), "generated_inputs": stamp.get("inputs", ""), "opt": built_opt(),
            "cpu": tc.cached_value(tc.BUILD_DIR, "PS2X_CPU") or "AVX2", "iso": iso or "", "time": int(time.time())}
    if game != "vcs":
        info.update(game=game, generated=gen)
    with open(BUILD_INFO, "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1)


LAUNCHER_BUILD = os.path.join(tc.BUILD, "launcher")


def build_launcher(env, verbose):
    """launcher/ is a CMake project of its own (no generated code, no disc): build/launcher, the exe in the
    repository root."""
    tc.stage("launcher", "Building the launcher")
    if not tc.IS_WINDOWS and not tc.check_linux_deps(env, tc.LAUNCHER_LINUX_DEPS):
        tc.fail("missing packages for the launcher (above)", "missing_packages")
    if tc.drop_foreign_cache(LAUNCHER_BUILD, os.path.join(tc.ROOT, "launcher")) or tc.stale_tools(LAUNCHER_BUILD) or \
            tc.drop_other_compiler(LAUNCHER_BUILD, env) is not None or tc.python_changed(LAUNCHER_BUILD) or \
            not os.path.isfile(os.path.join(LAUNCHER_BUILD, "build.ninja")):
        cmd = ["cmake", "-S", os.path.join(tc.ROOT, "launcher"), "-B", LAUNCHER_BUILD, "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release"] + tc.launcher_compiler_defines(env) + tc.deps_defines() + \
              tc.python_defines()
        if tc.IS_MACOS:  # its Metal shader's compilers (built once from pinned sources)
            import shader_tools
            cmd += shader_tools.cmake_defines(env)
        if os.environ.get("VCS_WAYLAND_PROTOCOLS_DIR"):  # a release built on an older distribution (CI)
            cmd.append("-DVCS_WAYLAND_PROTOCOLS_DIR=" + os.environ["VCS_WAYLAND_PROTOCOLS_DIR"])
        log = os.path.join(tc.BUILD, "configure_launcher.log")
        if tc.run_with_progress(cmd, log, "[build] configure launcher", env=env) != 0:
            print(tc.tail(log, 30))
            tc.fail("CMake configure of the launcher failed (build/configure_launcher.log)", tc.configure_failure(log), log)
    if not tc.cmake_build(env, "GTAStoriesLauncher", "build_launcher.log", verbose, build_dir=LAUNCHER_BUILD):
        return False
    exe = "GTAStoriesLauncher.app" if tc.IS_MACOS else tc.exe("GTAStoriesLauncher")
    print("[build] ok: " + exe)
    tc.event(result="ok", exe=exe)
    return True


def install_deps(env):
    """Linux: the packages the game's build (and the launcher's) lacks, installed through pkexec - the desktop's own
    password prompt, so the launcher can do it without a terminal. Without a polkit agent (or pkexec) it fails with
    no_polkit and the command to run in a terminal; a refused prompt is elevation_denied."""
    if tc.IS_WINDOWS:
        tc.fail("--install-deps is for Linux (Windows: tools/setup_vs.py install)", "unsupported")
    if tc.IS_MACOS:
        return install_clt()
    tc.stage("packages", "Installing the packages the build needs")
    missing = tc.linux_missing(env, quiet=True) + tc.linux_missing(env, tc.LAUNCHER_LINUX_DEPS)
    if not missing:
        print("[deps] nothing to install: every build dependency is there")
        tc.event(result="ok", installed=[])
        return 0
    packages = [p for _, p in missing]
    for label, pkgs in missing:
        print("[deps] missing: %s (%s)" % (label, pkgs))
    terminal = " ".join(tc.linux_packages_command(missing))
    if not shutil.which("pkexec"):
        tc.fail("pkexec isn't installed: run this in a terminal, then check again:\n    " + terminal, "no_polkit")
    cmd = tc.linux_packages_command(missing, elevate="pkexec")
    if "--llvm-repo" in cmd:
        print("[deps] this release's clang is too old for the game: LLVM's repository (apt.llvm.org) is added first")
    print("[deps] " + " ".join(cmd))
    code = subprocess.call(cmd)
    if code == 126:  # the prompt was dismissed
        tc.fail("the password prompt was cancelled; nothing was installed", "elevation_denied")
    if code == 127:  # no authentication agent, or not authorised
        tc.fail("no password prompt could be shown (no polkit agent): run this in a terminal, then check again:\n    " +
                terminal, "no_polkit")
    if code != 0:
        tc.fail("the package manager stopped with exit code %d (above)" % code, "install_failed")
    still = tc.linux_missing(env, quiet=True) + tc.linux_missing(env, tc.LAUNCHER_LINUX_DEPS)
    if still:
        tc.fail("still missing after the install: " + ", ".join(label for label, _ in still), "install_failed")
    print("[deps] installed: " + " ".join(packages))
    tc.event(result="ok", installed=packages)
    return 0


def install_clt():
    """macOS: Apple's Command Line Tools through xcode-select --install - macOS shows its own installer and the call
    returns at once; the launcher checks again when the player says it has finished (result "started")."""
    tc.stage("packages", "Installing Apple's Command Line Tools")
    missing = tc.macos_missing()
    if not missing:
        print("[deps] nothing to install: the Command Line Tools are there (%s)" % tc.macos_developer_dir())
        tc.event(result="ok", installed=[])
        return 0
    print("[deps] " + " ".join(tc.CLT_INSTALL))
    code = subprocess.call(tc.CLT_INSTALL)
    if code != 0 and tc.macos_missing():
        tc.fail("xcode-select --install stopped with exit code %d: install the Command Line Tools from "
                "https://developer.apple.com/download/all/ (or Xcode), then check again" % code, "install_failed")
    print("[deps] macOS' installer is open: finish it, then check again")
    tc.event(result="started", installed=["Command Line Tools"])
    return 0


def configured_without_game(gen=None):
    """True when build/release was configured before the generated code existed. A first --recompile does that
    (recompile.py configures to build the recompiler), and CMake then leaves the game out (VCS_HAVE_GENERATED in
    CMakeLists.txt) until a configure sees generated/ee - without one the build stops at "unknown target
    'ps2EntryRunner'"."""
    if not os.path.isfile(os.path.join(gen or os.path.join(tc.ROOT, "generated"), "ee", "register_functions.cpp")):
        return False
    try:
        with open(os.path.join(tc.BUILD_DIR, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            return any(line.strip() == "PS2X_BUILD_RUNTIME:BOOL=OFF" for line in f)
    except OSError:
        return False


def move_from_old_place():
    """vcs.exe used to be built in build/release/third_party/PS2Recomp/ps2xRuntime; it is now at the top of build/.
    The player's vcs.ini moves with it (when build/ has none yet), and the old exe goes, so nobody starts a stale one."""
    new_dir = os.path.dirname(tc.game_exe(tc.BUILD_DIR))
    if os.path.normcase(new_dir) == os.path.normcase(tc.OLD_GAME_DIR):
        return
    old_ini, new_ini = os.path.join(tc.OLD_GAME_DIR, "vcs.ini"), os.path.join(new_dir, "vcs.ini")
    if os.path.isfile(old_ini) and not os.path.isfile(new_ini):
        shutil.copy2(old_ini, new_ini)
        print("[build] settings moved: " + tc.show(old_ini) + " -> " + tc.show(new_ini))
    for name in ("vcs.exe", "vcs.pdb", "vcs", "vcs.ini"):
        path = os.path.join(tc.OLD_GAME_DIR, name)
        # the old vcs.ini only once build/'s is the same file (a different one stays for the player to look at)
        if os.path.isfile(path) and (name != "vcs.ini" or (os.path.isfile(new_ini) and filecmp.cmp(path, new_ini, shallow=False))):
            try:
                os.remove(path)
                print("[build] removed the old " + tc.show(path) + " (the game is now in build/)")
            except OSError as e:
                print("[build] could not remove the old " + tc.show(path) + ": " + str(e))


def remember_iso(given):
    """Writes iso=<disc> into the vcs.ini next to vcs.exe when it has none, so the exe started on its own (a double
    click) finds the disc the build used. An iso= already there is the player's and stays. Returns the disc (None when
    none is known)."""
    try:
        iso = tc.find_iso(given)
    except SystemExit:
        return None  # no disc known here: vcs.exe looks next to itself, or is given one
    ini = os.path.join(os.path.dirname(GAME_EXE), "vcs.ini")
    lines = []
    if os.path.isfile(ini):
        with open(ini, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    if any(line.strip().lower().startswith("iso=") for line in lines):
        return iso
    with open(ini, "w", encoding="utf-8") as f:
        f.write("\n".join(lines + ["iso=" + iso]) + "\n")
    print("[build] disc for %s: %s (iso= in %s)" % (os.path.basename(GAME_EXE) if tc.GAME != "vcs" else "vcs.exe",
                                                     iso, tc.show(ini)))
    return iso


def game_link_stage():
    """(kind, title) of the game's link when it is a step of its own (toolchain.cmake_build): with link-time
    optimisation it is the longest step of a build. A quick build's link takes seconds: no stage."""
    lto = tc.cached_value(tc.BUILD_DIR, "PS2X_LTO") or "OFF"
    if lto.upper() not in ("THIN", "FULL"):
        return None
    pgo = tc.cached_value(tc.BUILD_DIR, "PS2X_PGO") or ""
    if pgo and pgo != "GEN":
        return "pgo", "Linking and optimising the game (ThinLTO + profile)"
    return lto.lower(), "Linking and optimising the game (%s)" % ("ThinLTO" if lto.upper() == "THIN" else "LTO")


def built_opt():
    """The optimisation the CMake cache of build/release holds, for the last line of a build."""
    cache = {}
    try:
        with open(os.path.join(tc.BUILD_DIR, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            for line in f:
                key, sep, value = line.partition("=")
                if sep and key.split(":")[0] in ("PS2X_LTO", "PS2X_PGO"):
                    cache[key.split(":")[0]] = value.strip()
    except OSError:
        return "optimisation unknown"
    cpu = tc.cached_value(tc.BUILD_DIR, "PS2X_CPU") or "AVX2"
    cpu = "" if cpu == "AVX2" else ", for %s CPUs" % cpu
    if cache.get("PS2X_PGO") and cache["PS2X_PGO"] != "GEN":
        return "optimised: ThinLTO + profile-guided" + cpu
    if cache.get("PS2X_LTO") == "THIN":
        return "optimised: ThinLTO" + cpu
    if cache.get("PS2X_LTO") == "FULL":
        return "optimised: full LTO" + cpu
    return "quick build, not optimised - python tools/build.py %s--opt lto for the game to play" % (
        "" if tc.GAME == "vcs" else "--game %s " % tc.GAME) + cpu


PGO_DIR = os.path.join(tc.BUILD, "pgo")
PROFDATA = os.path.join(PGO_DIR, "vcs.profdata")
PGO_BUILD = os.path.join(tc.BUILD, "pgo-gen")


def train(env, verbose):
    """Instrumented build (build/pgo-gen), training runs, build/pgo/vcs.profdata."""
    import glob
    import shutil
    tc.configure(env, build_dir=PGO_BUILD, defines={"PS2X_LTO": "OFF", "PS2X_PGO": "GEN"})
    print("[build] building the instrumented game (build/pgo-gen)")
    if not tc.cmake_build(env, "ps2EntryRunner", "build_pgo_gen.log", verbose, build_dir=PGO_BUILD):
        sys.exit(1)
    raw = os.path.join(PGO_DIR, "raw")
    shutil.rmtree(raw, ignore_errors=True)
    os.makedirs(raw)
    run_env = dict(os.environ)
    run_env["VCS_EXE"] = tc.game_exe(PGO_BUILD)
    run_env["LLVM_PROFILE_FILE"] = os.path.join(raw, "vcs-%p.profraw")
    for cmd in ([tool("bench.py"), "--name", "pgo-train"], [tool("scenes.py"), "capture", os.path.join(PGO_DIR, "scenes")]):
        print("[build] training: " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
        subprocess.call([sys.executable] + cmd, env=run_env)
    profiles = glob.glob(os.path.join(raw, "*.profraw"))
    if not profiles:
        tc.fail("training wrote no profiles (build/pgo/raw)")
    profdata = tc.llvm_profdata(env)
    subprocess.check_call([profdata, "merge", "-o", PROFDATA] + profiles, env=env)
    print("[build] profile: " + tc.show(PROFDATA) + " from %d runs" % len(profiles))


def tool(name):
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), name)


def iop_tests(env):
    """ps2xIOP on its own with PS2X_IOP_BUILD_TESTS=ON in build/iop_tests; runs the SPU2 checks."""
    out = os.path.join(tc.BUILD, "iop_tests")
    if not os.path.isfile(os.path.join(out, "build.ninja")) or tc.stale_tools(out) or \
            tc.drop_other_compiler(out, env) is not None:
        subprocess.check_call(tc.resolve(["cmake", "-S", os.path.join(tc.ROOT, "third_party", "PS2Recomp", "ps2xIOP"),
                                          "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                          "-DPS2X_IOP_BUILD_TESTS=ON", "-DPS2X_CPU=" + tc.check_cpu()] +
                                         tc.compiler_defines(env), env), env=env)
    if not tc.cmake_build(env, "ps2_iop_spu2_tests", "build_iop_tests.log", build_dir=out):
        return 1
    return subprocess.call([tc.exe(os.path.join(out, "ps2_iop_spu2_tests"))])


if __name__ == "__main__":
    main()
