#!/usr/bin/env python3
"""Function map for a stripped PS2 ELF without Ghidra: a first map for bring-up of another game.

Writes the recompiler's map (Name,Start,End,Size, as tools/ghidra_analyze.py exports) from heuristics:
  - starts: the entry point, every JAL target in .text, code addresses built with lui + addiu/ori, and words in the
    data sections pointing into .text - the last two only where the address looks like a function start (a stack
    frame set up there, or the previous function's `jr ra` + delay slot / alignment padding just before it), so
    jump-table case labels don't split a function;
  - each function ends at the next start (its trailing padding included).

    python -I tools/find_functions.py <elf> <out.csv>
    python -I tools/find_functions.py <elf> <out.csv> --compare <reference functions.csv>   (how close it gets)

Ghidra's map (tools/ghidra_analyze.py) stays the reference; this is the fallback when it isn't installed.
"""
import argparse
import bisect
import csv
import struct
import sys

DATA_SECTIONS = (".data", ".rodata", ".sdata", ".vudata")
JR_RA = 0x03E00008
NOP = 0


def sections(d):
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
    base = secs[shstrndx][4]
    out = {}
    for s in secs:
        name = d[base + s[0]:d.index(b"\0", base + s[0])].decode("ascii", "replace")
        out[name] = {"addr": s[3], "off": s[4], "size": s[5], "type": s[1]}
    return out


def is_frame(w):
    """addiu sp, sp, -N (or daddiu)"""
    op = w >> 26
    return op in (0x09, 0x19) and (w >> 21) & 31 == 29 and (w >> 16) & 31 == 29 and (w & 0x8000)


def is_dead_epilogue(w):
    """addiu sp, sp, +N: left between functions where the compiler dropped an epilogue (never reached)"""
    return w >> 16 == 0x27BD and not (w & 0x8000)


def find(elf):
    d = open(elf, "rb").read()
    secs = sections(d)
    text = secs[".text"]
    lo, hi = text["addr"], text["addr"] + text["size"]
    n = text["size"] // 4
    words = struct.unpack_from("<%dI" % n, d, text["off"])

    def word(a):
        return words[(a - lo) // 4] if lo <= a < hi else None

    def after_return(a):
        """the previous function ends right before `a`: jr ra + delay slot, then only nop padding"""
        p = a - 4
        while p >= lo and word(p) == NOP and a - p <= 64:
            p -= 4
        # p is the last non-nop word (or a nop delay slot just counted as padding)
        # delay slot at p (jr ra at p-4), or the delay slot was a nop (jr ra at p, its nop before `a`) - never `a`
        # itself in a delay slot (jr ra at a-4: an epilogue's addiu sp, which code pointers can name)
        if p - 4 >= lo and word(p - 4) == JR_RA:
            return True
        return p + 4 < a and word(p) == JR_RA

    def plausible(a):
        return lo <= a < hi and a % 4 == 0 and word(a) != NOP and (is_frame(word(a)) or after_return(a))

    entry = struct.unpack_from("<I", d, 24)[0]
    starts = {lo, entry}
    jal = set()
    for i, w in enumerate(words):
        if w >> 26 == 0x03:  # jal
            t = ((lo + 4 * i + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if lo <= t < hi:
                jal.add(t)
    starts |= jal

    # lui rX, hi ; then addiu/ori rY, rX, lo within a few instructions
    built = set()
    for i, w in enumerate(words):
        if w >> 26 != 0x0F:
            continue
        rt, upper = (w >> 16) & 31, (w & 0xFFFF) << 16
        for j in range(i + 1, min(i + 9, n)):
            v = words[j]
            op = v >> 26
            if op in (0x09, 0x0D) and (v >> 21) & 31 == rt:
                imm = v & 0xFFFF
                built.add((upper + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)) & 0xFFFFFFFF)
                break
    starts |= {a for a in built if plausible(a)}

    data_ptrs = set()
    for name in DATA_SECTIONS:
        s = secs.get(name)
        if not s or s["type"] != 1:
            continue
        blob = d[s["off"]:s["off"] + s["size"]]
        for i in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, i)[0]
            if lo <= v < hi and v % 4 == 0:
                data_ptrs.add(v)
    starts |= {a for a in data_ptrs if plausible(a)}

    # code reached only by a fall-through after a function's return: a frame set up right after jr ra + delay slot
    for i in range(n - 2):
        if words[i] == JR_RA:
            a = lo + 4 * (i + 2)
            while a < hi and word(a) == NOP:
                a += 4
            if a < hi and is_frame(word(a)):
                starts.add(a)

    # Code after a return that nothing branches over: a function of its own (leaf functions reached only through
    # tables, syscall wrappers). Iterated, since each new start limits the next one's branch scan.
    for _ in range(4):
        ordered = sorted(starts)
        added = 0
        for k, a in enumerate(ordered):
            end = ordered[k + 1] if k + 1 < len(ordered) else hi
            reach = a  # the furthest address a branch / jump in [a, i] goes to
            i = a
            while i < end - 4:
                w = word(i)
                op = w >> 26
                t = None
                if op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17) or (op == 0x01 and (w >> 16) & 0x1F in (0, 1, 2, 3, 16, 17, 18, 19)) \
                        or (op == 0x11 and (w >> 21) & 0x1F == 8) or (op == 0x12 and (w >> 21) & 0x1F == 8):
                    off = w & 0xFFFF
                    t = i + 4 + ((off - 0x10000 if off & 0x8000 else off) << 2)
                elif op == 0x02:  # j (not a call): a jump inside the function, or a tail call elsewhere
                    t = ((i + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if not (a <= t < end):
                        t = None
                if t is not None and t > reach:
                    reach = t
                if w == JR_RA and reach <= i + 4:
                    c = i + 8
                    while c < end and (word(c) == NOP or is_dead_epilogue(word(c))):
                        c += 4
                    if c < end and c not in starts:
                        starts.add(c)
                        added += 1
                        break
                i += 4
        if not added:
            break

    ordered = sorted(starts)
    return [(a, ordered[k + 1] if k + 1 < len(ordered) else hi) for k, a in enumerate(ordered)]


def write(funcs, out):
    with open(out, "w", newline="\n") as f:
        f.write("Name,Start,End,Size\n")
        for a, e in funcs:
            f.write("fn_%08X,0x%08X,0x%08X,%d\n" % (a, a, e, e - a))
    print("wrote %s: %d functions" % (out, len(funcs)))


def compare(funcs, ref):
    rows = [r for r in csv.DictReader(open(ref)) if not r["Name"].startswith(("entry_", "caseD_", "default", "LAB_", "switchD_"))]
    want = {int(r["Start"], 16) for r in rows}
    have = {a for a, _ in funcs}
    print("reference %d starts; found %d: %d match, %d missed, %d extra (%.1f%% recall, %.1f%% precision)" % (
        len(want), len(have), len(want & have), len(want - have), len(have - want),
        100.0 * len(want & have) / max(1, len(want)), 100.0 * len(want & have) / max(1, len(have))))
    missed = sorted(want - have)
    print("first missed:", " ".join("%08X" % a for a in missed[:12]))
    extra = sorted(have - want)
    print("first extra: ", " ".join("%08X" % a for a in extra[:12]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--compare", help="a reference map (Ghidra's) to measure against")
    ap.add_argument("--gaps-of", metavar="CSV", help="write only the starts outside this map's functions (Ghidra's): "
                    "extra functions for make_config (game/<game>/extra_functions.csv)")
    ap.add_argument("--merge", metavar="CSV", help="with --gaps-of: also these starts (tools/find_missing_functions.py's)")
    a = ap.parse_args()
    funcs = find(a.elf)
    if a.gaps_of:
        ranges = sorted((int(r["Start"], 16), int(r["End"], 16)) for r in csv.DictReader(open(a.gaps_of))
                        if not r["Name"].startswith(("entry_", "caseD_", "default", "LAB_", "switchD_")))
        starts = [r[0] for r in ranges]

        def covered(x):
            i = bisect.bisect_right(starts, x) - 1
            return i >= 0 and ranges[i][0] <= x < ranges[i][1]

        mine = {f[0] for f in funcs}
        if a.merge:
            mine |= {int(r["Start"], 16) for r in csv.DictReader(open(a.merge))}
        # each extra ends at the next start of either map
        every = sorted(set(starts) | mine)
        extra = []
        for f0 in sorted(mine):
            if not covered(f0):
                j = bisect.bisect_right(every, f0)
                extra.append((f0, every[j] if j < len(every) else funcs[-1][1]))
        print("%d starts outside %s" % (len(extra), a.gaps_of))
        funcs = extra
    write(funcs, a.out)
    if a.compare:
        compare(funcs, a.compare)


if __name__ == "__main__":
    sys.exit(main())
