#!/usr/bin/env python3
"""Build game/vcs/vcs.toml for ps2_recomp from the two analysis passes.

- Ghidra (tools/ghidra_analyze.py): function boundaries + code labels
  (game/vcs/functions.csv) and exact syscall-wrapper names.
- ps2_analyzer: SCE SDK library identification (stubs, entry points),
  MMIO access sites and jump tables.

Where both name a function, Ghidra's syscall wrappers win: the analyzer's SDK
matcher occasionally maps a name onto the C wrapper around the syscall, and
stubbing that would skip game-side bookkeeping.

Ghidra's export also lists every branch-target label (entry_*, caseD_*,
default*) as its own record. Recompiling those as separate functions made
67k files and duplicated code, so only real functions are kept in
game/vcs/functions.csv; ps2_recomp discovers the internal entry points it
needs (resume points, jump targets) by itself.

    python tools/make_config.py ghidra/export/ghidra_export.toml build/analyzer/analyzer.toml

Another game (bring-up without Ghidra): its map from tools/find_functions.py in place of Ghidra's export, its files
in game/<game>/ (<game>.toml, functions.csv, overrides.toml):

    python tools/make_config.py --game lcs --elf SLUS_214.23 --functions map.csv - analyzer.toml
"""
import glob
import os
import re
import sys
import tomllib

ROOT_OUT = "game/vcs/vcs.toml"
CSV_OUT = "game/vcs/functions.csv"
EXTRA_CSV = "game/vcs/extra_functions.csv"  # tools/find_missing_functions.py
LABEL_PREFIXES = ("entry_", "caseD_", "default", "LAB_", "switchD_")
OVERRIDES = "game/vcs/overrides.toml"


def selectors(lst):
    out = {}
    for sel in lst:
        name, _, addr = sel.partition("@")
        out[int(addr, 16) if addr else None] = name
    return out


def q(s):
    return '"' + s.replace("\\", "\\\\") + '"'


def filter_csv(src):
    rows, dropped = [], 0
    with open(src) as f:
        header = f.readline()
        for line in f:
            if line.startswith(LABEL_PREFIXES):
                dropped += 1
                continue
            rows.append(line)
    kept, extra = len(rows), 0
    try:
        with open(EXTRA_CSV) as ex:
            ex.readline()
            for line in ex:
                rows.append(line)
                extra += 1
    except FileNotFoundError:
        pass
    rows.sort(key=lambda line: int(line.split(",")[1], 16))
    with open(CSV_OUT, "w", newline="\n") as out:
        out.write(header)
        out.writelines(rows)
    print("wrote %s: %d functions + %d extra (%d label records dropped)" % (CSV_OUT, kept, extra, dropped))


RUNTIME_KERNEL = "third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel"


def todo_handlers():
    """Runtime stub handlers whose body is only TODO_NAMED (they throw when called)."""
    todo = set()
    for f in glob.glob(os.path.join(RUNTIME_KERNEL, "**", "*.cpp"), recursive=True):
        src = open(f, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"void\s+(\w+)\s*\(uint8_t\s*\*\s*rdram[^)]*\)\s*\{(.*?)\n    \}", src, re.S):
            body = m.group(2)
            if "TODO_NAMED" in body and body.count(";") <= 2:
                todo.add(m.group(1))
    return todo


GAME, ELF_NAME = "vcs", "SLUS_215.90"


def set_game(game, elf):
    """game/<game>/ instead of game/vcs/ (a second game's bring-up)"""
    global GAME, ELF_NAME, ROOT_OUT, CSV_OUT, EXTRA_CSV, OVERRIDES
    GAME, ELF_NAME = game, elf
    ROOT_OUT = "game/%s/%s.toml" % (game, game)
    CSV_OUT = "game/%s/functions.csv" % game
    EXTRA_CSV = "game/%s/extra_functions.csv" % game
    OVERRIDES = "game/%s/overrides.toml" % game


def main(ghidra_toml, analyzer_toml, functions=None):
    # "-": no Ghidra export (no syscall-wrapper names from it); the map is `functions` (tools/find_functions.py)
    g = tomllib.load(open(ghidra_toml, "rb")) if ghidra_toml != "-" else {"general": {"ghidra_output": functions}}
    filter_csv(functions or g["general"]["ghidra_output"])
    a = tomllib.load(open(analyzer_toml, "rb"))
    try:
        ov = tomllib.load(open(OVERRIDES, "rb"))
    except FileNotFoundError:
        ov = {}

    gstubs = selectors(g["general"].get("stubs", []))
    astubs = selectors(a["general"].get("stubs", []))
    gnames = set(gstubs.values())
    if ghidra_toml == "-":
        # no Ghidra names: the overrides' named functions play their part (the kernel's syscall wrappers there), so the
        # analyzer's same-named matches elsewhere - C functions around those syscalls - are recompiled, not stubbed
        # (kernel names only: libcdvd / libfileio entries keep both copies stubbed, as VCS's config has them)
        gnames |= {sel.partition("@")[0] for sel in ov.get("stubs", []) if not sel.startswith("sce")}

    stubs, dropped = dict(gstubs), []
    for addr, name in astubs.items():
        if addr in stubs:
            continue
        if name in gnames:
            dropped.append("%s@0x%08X" % (name, addr))
            continue
        stubs[addr] = name
    # Never bind a function to a handler that only throws: recompile the game's own copy.
    todo = todo_handlers()
    for addr, name in list(stubs.items()):
        if name in todo or name.lstrip("_") in todo:
            print("  not stubbing %s@0x%08X (runtime handler is TODO)" % (name, addr))
            del stubs[addr]
    for sel in ov.get("drop_stubs", []):
        name, _, addr = sel.partition("@")
        stubs.pop(int(addr, 16), None)
    for sel in ov.get("stubs", []):
        name, _, addr = sel.partition("@")
        stubs[int(addr, 16)] = name

    entry_points = sorted(set(a["general"].get("entry_points", [])) | set(ov.get("entry_points", [])))
    skip = sorted(set(ov.get("skip", [])))

    lines = [
        "# Generated by tools/make_config.py - edit %s instead." % OVERRIDES,
        "[general]",
        "input = " + q("build/elf/" + ELF_NAME),
        "output = " + q("generated/ee"),
        "ghidra_output = " + q(CSV_OUT),
        "single_file_output = false",
        "output_worker_threads = 0",
        "patch_syscalls = false",
        "patch_cop0 = true",
        "patch_cache = true",
        "stubs = [",
    ]
    lines += ["  %s," % q("%s@0x%08X" % (n, addr)) for addr, n in sorted(stubs.items())]
    lines += ["]", "entry_points = ["]
    lines += ["  %s," % q(e) for e in entry_points]
    lines += ["]", "skip = ["]
    lines += ["  %s," % q(e) for e in skip]
    lines += ["]", ""]

    if a.get("mmio"):
        lines.append("[mmio]")
        for k, v in sorted(a["mmio"].items(), key=lambda kv: int(kv[0], 16)):
            lines.append("%s = %s" % (q(k), q(v)))
        lines.append("")

    # Instruction patches only come from our overrides (the analyzer found none).
    if ov.get("patches"):
        lines.append("[patches]")
        lines.append("instructions = [")
        for p in ov["patches"]:
            lines.append('  { address = %s, value = %s },' % (q(p["address"]), q(p["value"])))
        lines.append("]")
        lines.append("")

    for t in a.get("jump_tables", {}).get("table", []):
        lines.append("[[jump_tables.table]]")
        lines.append("address = %s" % q(t["address"]))
        lines.append("entries = [")
        for e in t["entries"]:
            lines.append('  { index = %d, target = %s },' % (e["index"], q(e["target"])))
        lines.append("]")
        lines.append("")

    open(ROOT_OUT, "w", newline="\n").write("\n".join(lines))
    print("wrote %s: %d stubs, %d entry points, %d skips" % (ROOT_OUT, len(stubs), len(entry_points), len(skip)))
    for d in dropped:
        print("  dropped analyzer stub (Ghidra has this name elsewhere):", d)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ghidra_toml", help="Ghidra's export, or - for none (then --functions)")
    ap.add_argument("analyzer_toml")
    ap.add_argument("--game", default="vcs")
    ap.add_argument("--elf", default="SLUS_215.90", help="the game's executable name (build/elf/<elf>)")
    ap.add_argument("--functions", help="a function map to use in place of Ghidra's (tools/find_functions.py)")
    args = ap.parse_args()
    if args.ghidra_toml == "-" and not args.functions:
        sys.exit("make_config: without a Ghidra export, give --functions")
    if args.game != "vcs" or args.elf != "SLUS_215.90":
        set_game(args.game, args.elf)
    main(args.ghidra_toml, args.analyzer_toml, args.functions)
