"""[phonebuild] (Windows) Time compiling unity batches of the Android game library with the NDK clang (same command as ninja),
with flag variants. Peak RSS via GetProcessMemoryInfo on the still-open process handle (docs/PHONE_BUILD.md, measurements).

    python tools/phonebuild/measure_tu.py <build tree with compile_commands.json> <obj dir> <variant> <i,j,...> [jobs]
variant: base (as built), nog (no -g), O1, nomisched (no -g, -mllvm -enable-misched=false)."""
import ctypes, ctypes.wintypes as wt, json, os, re, subprocess, sys, time, concurrent.futures as cf

class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("a", ctypes.c_size_t), ("b", ctypes.c_size_t),
                ("c", ctypes.c_size_t), ("d", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]

def peak(p):
    m = PMC(); m.cb = ctypes.sizeof(m)
    ctypes.windll.psapi.GetProcessMemoryInfo(int(p._handle), ctypes.byref(m), m.cb)
    return m.PeakWorkingSetSize >> 20

tree, out, variant = sys.argv[1], sys.argv[2], sys.argv[3]
idx = [int(x) for x in sys.argv[4].split(",")]
jobs = int(sys.argv[5]) if len(sys.argv) > 5 else 1
cc = {os.path.basename(e["file"]): e["command"] for e in json.load(open(os.path.join(tree, "compile_commands.json")))
      if "Unity" in e["file"] and ("ps2GameLibrary.dir" in e["command"] or "ps2EntryRunner" not in e["command"])}
os.makedirs(out, exist_ok=True)

def one(i):
    cmd = cc["unity_%d_cxx.cxx" % i]
    cmd = re.sub(r" -o \S+", " -o " + os.path.join(out, "u%d_%s.o" % (i, variant)).replace("\\", "/"), cmd)
    if variant != "base":
        cmd = cmd.replace(" -g ", " ")
    if variant == "nomisched":
        cmd = cmd.replace(" -c ", " -mllvm -enable-misched=false -c ", 1)
    if variant in ("O1", "O0"):
        cmd = cmd.replace(" -O3 ", " ").replace(" -O2 ", " -%s " % variant)
    t = time.time()
    p = subprocess.Popen(cmd, cwd=tree)
    p.wait()
    dt = time.time() - t
    o = os.path.join(out, "u%d_%s.o" % (i, variant))
    return i, round(dt, 1), peak(p), os.path.getsize(o) >> 10 if os.path.exists(o) else -1, p.returncode

with cf.ThreadPoolExecutor(jobs) as ex:
    for r in ex.map(one, idx):
        print(variant, "unity_%d: %.1fs peakRSS %d MB obj %d KB rc %d" % (r[0], r[1], r[2], r[3], r[4]), flush=True)
