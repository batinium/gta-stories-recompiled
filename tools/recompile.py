#!/usr/bin/env python3
"""Recompile the game: your ISO -> generated C++ (generated/ee, generated/vu1).

    python tools/recompile.py ["path/to/Grand Theft Auto - Vice City Stories (USA).iso"]

Without an argument the disc is taken from VCS_ISO, iso=... in build/vcs.ini or vcs.ini, or a '*Vice City
Stories*.iso' in the repository root. `--check` says whether generated/ is current (generated/recompile_stamp.json: a
hash of the recompiler, its config and the function map). Uses the committed function map (game/vcs); Ghidra is only needed to redo the analysis
(tools/ghidra_analyze.py). To recompile and build in one go: python tools/build.py --recompile

Another game (game/<game>/: lcs, Liberty City Stories): --game lcs [--out <folder>] recompiles into <folder>/ee and
<folder>/vu1 (default generated-lcs/; never the repository's generated/, which is VCS's), with its own stamp
(<folder>/recompile_stamp.json over game/lcs and the recompiler: `--game lcs [--out <folder>] --check`). The disc: the
argument, LCS_ISO, iso= in build/lcs/vcs.ini or a '*Liberty City Stories*.iso' in the repository root. To recompile and
build in one go: python tools/build.py --game lcs --recompile <your LCS iso> (docs/LCS.md).
"""
import argparse
import filecmp
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _game_from_argv():
    """[lcs] --game before toolchain is imported: it picks the game's build folder (toolchain.GAME)."""
    for i, arg in enumerate(sys.argv[1:], 1):
        if arg == "--game" and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
        if arg.startswith("--game="):
            return arg.partition("=")[2]
    return None


if __name__ == "__main__":
    # [android] No --game: the disc's own boot file (SYSTEM.CNF) names the game; --game against the disc is an error
    import preflight
    _game, _error = preflight.game_from_args(sys.argv[1:])
    if _error:
        sys.exit("[recompile] " + _error)
    if _game:
        os.environ["VCS_TOOLS_GAME"] = _game
import toolchain as tc  # noqa: E402

ELF = os.path.join(tc.BUILD, "elf", "SLUS_215.90")
GENERATED = os.path.join(tc.ROOT, "generated")
STAMP = os.path.join(GENERATED, "recompile_stamp.json")

# What the generated code depends on besides the disc: the recompiler (and the runtime headers it compiles in: the
# stub list), the config and function map, and the scripts that translate and sync. A change to any of them means a
# recompile; nothing else does (the runtime and src/ are compiled, not generated).
STAMP_INPUTS = [
    "game/vcs",
    "third_party/PS2Recomp/ps2xRecomp",
    "third_party/PS2Recomp/ps2xRuntime/include/ps2_call_list.h",
    "third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_calls.h",
    "third_party/PS2Recomp/ps2xRuntime/include/runtime/ps2_address.h",
    "third_party/PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h",
    "tools/recompile.py",
    "tools/vu1_recomp.py",
    "tools/sync_generated.py",
    "tools/make_config.py",
]


def step(title, stage_id=None):
    print(f"[recompile] {title}")
    if stage_id:
        tc.stage(stage_id, title[0].upper() + title[1:])


def generated_root(game="vcs", override=None):
    """[lcs] The folder of a game's generated code (ee/, vu1/, recompile_stamp.json): `override` - the folder or its
    ee/ - else generated/ (VCS) or generated-<game>/."""
    if override:
        path = os.path.abspath(os.path.expanduser(override)).rstrip(os.sep)
        return os.path.dirname(path) if os.path.basename(path) == "ee" else path
    return GENERATED if game == "vcs" else os.path.join(tc.ROOT, "generated-" + game)


def cmake_game_defines(build_dir, game, root):
    """[lcs] The CMake values a build tree needs for `game` with its generated code in `root`, only those its cache
    lacks (tc.configure re-runs CMake whenever it is given any). VCS with generated/ needs none in a fresh tree."""
    want = {"VCS_GAME": game, "VCS_GENERATED_DIR": os.path.join(root, "ee").replace(os.sep, "/")}
    default = {"VCS_GAME": "vcs", "VCS_GENERATED_DIR": os.path.join(GENERATED, "ee").replace(os.sep, "/")}
    out = {}
    for key, value in want.items():
        have = tc.cached_value(build_dir, key)
        have = have if have is not None else default[key]
        same = os.path.realpath(have) == os.path.realpath(value) if key == "VCS_GENERATED_DIR" else have == value
        if not same:
            out[key] = value
    return out


def stamp_inputs(game="vcs"):
    """STAMP_INPUTS for `game`: its own game/<game> folder instead of game/vcs."""
    return ["game/" + game if rel == "game/vcs" else rel for rel in STAMP_INPUTS]


def inputs_hash(game="vcs"):
    """SHA-1 over the contents of STAMP_INPUTS (line endings normalised: a checkout with CRLF is the same source)."""
    h = hashlib.sha1()
    for rel in stamp_inputs(game):
        path = os.path.join(tc.ROOT, rel)
        files = [path] if os.path.isfile(path) else sorted(
            os.path.join(d, f) for d, _, names in os.walk(path) for f in names if "__pycache__" not in d)
        for f in files:
            h.update(os.path.relpath(f, tc.ROOT).replace(os.sep, "/").encode())
            with open(f, "rb") as fh:
                h.update(fh.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def read_stamp(root=GENERATED):
    try:
        with open(os.path.join(root, "recompile_stamp.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def why_recompile(game="vcs", root=None):
    """None when generated/ came from this source's recompiler and config, else the reason (for people and the
    launcher). The disc doesn't count: only one build of it is supported, and recompile checks that.
    [lcs] Another game: its folder (generated_root), its game/<game> in the hash."""
    root = root or generated_root(game)
    if not os.path.isfile(os.path.join(root, "ee", "register_functions.cpp")):
        return "no generated code yet"
    needs_vu1 = game == "vcs" or os.path.isfile(os.path.join(tc.ROOT, "game", game, "vu1_uploads.txt"))
    if needs_vu1 and not os.path.isfile(os.path.join(root, "vu1", "vu1_compiled.cpp")):
        return "no compiled VU1 microcode yet"
    stamp = read_stamp(root)
    if not stamp:
        return "the generated code has no stamp (made before stamps existed)"
    if stamp.get("game", "vcs") != game:
        return "the generated code is %s's, not %s's" % (stamp.get("game", "vcs"), game)
    if stamp.get("inputs") != inputs_hash(game):
        return "the recompiler, its config or the function map changed" + ("" if game == "vcs" else " (game/%s)" % game)
    return None


def write_stamp(game="vcs", root=GENERATED):
    os.makedirs(root, exist_ok=True)
    info = {"inputs": inputs_hash(game), "source": tc.source_version(), "time": int(time.time())}
    if game != "vcs":
        info["game"] = game  # [lcs]
    with open(os.path.join(root, "recompile_stamp.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1)


def for_recompiler(path):
    """A path for ps2_recomp's config, relative to the build folder it runs in (cwd=tc.BUILD). The recompiler opens
    files with narrow (ANSI code page) paths on Windows: in a folder with non-English letters ("E:\\Spiele Über",
    "C:\\Users\\Jürgen") the UTF-8 path from the .toml became mojibake and the ELF "could not load" (2026-11-18). The
    path from the build folder to its files is plain ASCII; another drive (a build folder elsewhere) keeps it absolute."""
    try:
        path = os.path.relpath(path, tc.BUILD)
    except ValueError:
        pass
    return path.replace(os.sep, "/")


def toml_paths(config, input_path, output_path):
    """The config's input, output and Ghidra map paths, for a recompiler started in tc.BUILD (for_recompiler)."""
    config = re.sub(r'(?m)^output = .*$', lambda _: 'output = "%s"' % for_recompiler(output_path), config)
    config = re.sub(r'(?m)^input = .*$', lambda _: 'input = "%s"' % for_recompiler(input_path), config)
    # ghidra_output is relative to the repository (game/<game>/functions.csv)
    return re.sub(r'(?m)^ghidra_output = "([^"]+)"', lambda m: 'ghidra_output = "%s"' % for_recompiler(
        m.group(1) if os.path.isabs(m.group(1)) else os.path.join(tc.ROOT, m.group(1))), config)


def run_logged(cmd, log, what, cwd=tc.ROOT):
    code = tc.run_with_progress(cmd, log, "[recompile] " + what, cwd=cwd)
    if code != 0:
        print(tc.tail(log))
        tc.fail(f"{what} failed, {tc.describe_exit(code)} (full log: {tc.show(log)})",
                tc.exit_failure(code) or "recompiler_failed", log)


# Games other than VCS: the executable on their disc (game/<game>/<game>.toml is made for that build)
OTHER_GAMES = {"lcs": "SLUS_214.23"}


def recompile(iso, env=None, verbose=False, game="vcs", out_dir=None):
    env = env or tc.build_env()
    if game != "vcs":
        return recompile_other(iso, env, verbose, game, out_dir)
    iso = tc.find_iso(iso)
    os.makedirs(os.path.dirname(ELF), exist_ok=True)

    step("extracting the executable from " + os.path.basename(iso), "disc")
    if subprocess.run(tc.PY + [tc.tool("vcs_disc.py"), "elf", iso, ELF], cwd=tc.ROOT).returncode != 0:
        tc.fail("could not extract SLUS_215.90 from " + iso, "wrong_disc")

    # Always (re)build the recompiler: it compiles in the runtime's stub list (ps2_call_list.h), so a stale
    # binary silently ignores new stubs.
    tc.configure(env)  # its own stage when it runs (the first time)
    step("building the recompiler", "recompiler")
    if not tc.cmake_build(env, "ps2_recomp", "build_recomp.log", verbose):
        tc.fail("recompiler build failed", "recompiler_failed", os.path.join(tc.BUILD, "build_recomp.log"))

    export = os.path.join(tc.ROOT, "ghidra", "export", "ghidra_export.toml")
    analyzer = os.path.join(tc.BUILD, "analyzer", "analyzer.toml")
    if os.path.isfile(export) and os.path.isfile(analyzer):
        step("merging the analysis exports into game/vcs/vcs.toml")
        subprocess.run(tc.PY + [tc.tool("make_config.py"), export, analyzer], cwd=tc.ROOT)
    else:
        step("analysis exports not present: using the committed game/vcs/vcs.toml")

    # Generate into a scratch dir, then copy only changed files so rebuilds stay incremental.
    step("translating MIPS to C++", "translate")
    fresh = os.path.join(tc.BUILD, "generated_fresh")
    shutil.rmtree(fresh, ignore_errors=True)
    with open(os.path.join(tc.ROOT, "game", "vcs", "vcs.toml"), encoding="utf-8") as f:
        config = f.read()
    config = toml_paths(config, ELF, fresh)
    fresh_toml = os.path.join(tc.BUILD, "vcs_fresh.toml")
    with open(fresh_toml, "w", encoding="utf-8") as f:
        f.write(config)
    log = os.path.join(tc.BUILD, "recomp.log")
    run_logged([tc.RECOMP_EXE, for_recompiler(fresh_toml)], log, "recompiler", cwd=tc.BUILD)
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Warnings:" in line:
                print("  " + line.strip())
    subprocess.run(tc.PY + [tc.tool("sync_generated.py"), fresh, os.path.join(tc.ROOT, "generated", "ee")], cwd=tc.ROOT, check=True)

    # VU1 microcode -> C++ (runs instead of the VU1 interpreter while VU1 memory holds exactly that code)
    step("compiling VU1 microcode", "vu1")
    out = os.path.join(tc.BUILD, "vu1", "vu1_compiled.cpp")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    result = subprocess.run(tc.PY + [tc.tool("vu1_recomp.py"), ELF, out, os.path.join(tc.ROOT, "game", "vcs", "vu1_uploads.txt")],
                            cwd=tc.ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-2000:] + result.stderr[-2000:])
        tc.fail("vu1_recomp failed", "recompiler_failed")
    lines = result.stdout.strip().splitlines()
    if lines:
        print("  " + lines[-1])
    dest = os.path.join(tc.ROOT, "generated", "vu1", "vu1_compiled.cpp")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if not os.path.isfile(dest) or not filecmp.cmp(out, dest, shallow=False):
        shutil.copyfile(out, dest)
    write_stamp()
    step("done")


def recompile_other(iso, env, verbose, game, out_dir):
    """Another game: its executable from `iso`, game/<game>/<game>.toml, generated code in out_dir/ee (and
    out_dir/vu1 when game/<game>/vu1_uploads.txt exists; out_dir defaults to generated-<game>/) and its stamp
    (out_dir/recompile_stamp.json). No analysis merge."""
    if game not in OTHER_GAMES:
        tc.fail("unknown game %s (known: vcs, %s)" % (game, ", ".join(OTHER_GAMES)))
    if not iso:
        if tc.GAME != game:
            tc.fail("--game %s needs the disc image" % game)
        iso = tc.find_iso(None)  # LCS_ISO, iso= in build/lcs/vcs.ini, *Liberty City Stories*.iso
    out_dir = generated_root(game, out_dir)
    if os.path.normcase(out_dir) == os.path.normcase(GENERATED):
        tc.fail("--out must not be the repository's generated/ folder (VCS's code)")
    elf_name = OTHER_GAMES[game]
    elf = os.path.join(tc.BUILD, "elf", elf_name)
    os.makedirs(os.path.dirname(elf), exist_ok=True)
    step("extracting %s from %s" % (elf_name, os.path.basename(iso)), "disc")
    if subprocess.run(tc.PY + [tc.tool("vcs_disc.py"), "extract", iso, elf_name, elf], cwd=tc.ROOT).returncode != 0:
        tc.fail("could not extract %s from %s" % (elf_name, iso), "wrong_disc")
    # the game's own build tree (tc.GAME, build/lcs) is set up for the game and this folder from the start
    tc.configure(env, defines=cmake_game_defines(tc.BUILD_DIR, game, out_dir) if tc.GAME == game else None)
    step("building the recompiler", "recompiler")
    if not tc.cmake_build(env, "ps2_recomp", "build_recomp.log", verbose):
        tc.fail("recompiler build failed", "recompiler_failed", os.path.join(tc.BUILD, "build_recomp.log"))
    step("translating MIPS to C++", "translate")
    fresh = os.path.join(tc.BUILD, "generated_fresh_" + game)
    shutil.rmtree(fresh, ignore_errors=True)
    with open(os.path.join(tc.ROOT, "game", game, game + ".toml"), encoding="utf-8") as f:
        config = f.read()
    config = toml_paths(config, elf, fresh)
    fresh_toml = os.path.join(tc.BUILD, game + "_fresh.toml")
    with open(fresh_toml, "w", encoding="utf-8") as f:
        f.write(config)
    log = os.path.join(tc.BUILD, "recomp_%s.log" % game)
    run_logged([tc.RECOMP_EXE, for_recompiler(fresh_toml)], log, "recompiler", cwd=tc.BUILD)
    subprocess.run(tc.PY + [tc.tool("sync_generated.py"), fresh, os.path.join(out_dir, "ee")], cwd=tc.ROOT, check=True)
    uploads = os.path.join(tc.ROOT, "game", game, "vu1_uploads.txt")
    if os.path.isfile(uploads):
        step("compiling VU1 microcode", "vu1")
        dest = os.path.join(out_dir, "vu1", "vu1_compiled.cpp")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        subprocess.run(tc.PY + [tc.tool("vu1_recomp.py"), elf, dest, uploads], cwd=tc.ROOT, check=True)
    write_stamp(game, out_dir)
    step("done: " + out_dir)
    return out_dir


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", nargs="?", help="your Vice City Stories (USA) disc image (--game lcs: Liberty City Stories)")
    ap.add_argument("-v", "--verbose", action="store_true", help="show all build output")
    ap.add_argument("--game", choices=("vcs",) + tuple(OTHER_GAMES),
                    help="vcs or lcs (default: the game the disc is, read from its SYSTEM.CNF; else vcs)")
    ap.add_argument("--out", help="with --game lcs: the folder for its generated code (default generated-lcs/; "
                                  "not generated/)")
    ap.add_argument("--check", action="store_true", help="only say whether the generated code is current (exit 0) or not (1)")
    a = ap.parse_args()
    a.game = a.game or tc.GAME  # [android] tc.GAME: the game picked from the disc above (else vcs)
    if a.out and a.game == "vcs":
        ap.error("--out is for --game lcs (VCS's generated code is generated/)")
    if a.check:
        why = why_recompile(a.game, generated_root(a.game, a.out))
        print("[recompile] " + ("needed: " + why if why else "generated code is current"))
        sys.exit(1 if why else 0)
    import preflight  # [android] Windows: the build tree's longest path against the 260-character limit
    preflight.require_short_path(tc.ROOT, "recompile", build_dir=tc.BUILD)
    tc.fix_future_files()  # as build.py: else Ninja re-runs CMake until it gives up
    recompile(a.iso, verbose=a.verbose, game=a.game, out_dir=a.out)


if __name__ == "__main__":
    main()
