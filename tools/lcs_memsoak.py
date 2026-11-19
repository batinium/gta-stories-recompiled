#!/usr/bin/env python3
"""LCS memory soak (Windows): does the process level off while teleporting / driving?

    python tools/lcs_memsoak.py NAME PORT MODE MINUTES [--out DIR] [--card DIR] [--exe EXE] [K=V ...]

MODE tele: boot the card copy, god mode, loop over every landmark (all three islands), 4 s each.
MODE mix:  the same, plus a spawned car driven ~20 s at each landmark.
K=V go to the game's environment (VCS_DRAW_DISTANCE=400, PS2X_CHAIN_IMAGES=0, ...).
Every 30 s a row in DIR/NAME.csv: private bytes, working set (MB), game frames, position, round (landmark loop).
Needs tools/lcs_test.py (the winqa harness). Prints a least-squares verdict (MB/min) over the second half.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcs_test  # noqa: E402


class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]


def mem(pid):
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
    h = k32.OpenProcess(0x1000 | 0x0400, False, pid)  # QUERY_LIMITED | QUERY
    if not h:
        return None
    m = PMC()
    m.cb = ctypes.sizeof(m)
    ok = psapi.GetProcessMemoryInfo(h, ctypes.byref(m), m.cb)
    k32.CloseHandle(h)
    return (m.PrivateUsage / 2 ** 20, m.WorkingSetSize / 2 ** 20) if ok else None


def verdict(path):
    rows = []
    for ln in open(path).read().splitlines()[1:]:
        p = ln.split(",")
        try:
            rows.append((float(p[0]), float(p[1])))
        except ValueError:
            pass
    if len(rows) < 6:
        return "too few samples"
    h = rows[len(rows) // 2:]
    n = len(h)
    mx = sum(r[0] for r in h) / n
    my = sum(r[1] for r in h) / n
    sxx = sum((r[0] - mx) ** 2 for r in h)
    slope = sum((r[0] - mx) * (r[1] - my) for r in h) / sxx * 60 if sxx else 0.0
    return "private %.0f -> %.0f MB (max %.0f); 2nd-half trend %+.2f MB/min" % (rows[0][1], rows[-1][1],
                                                                                max(r[1] for r in rows), slope)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("port", type=int)
    ap.add_argument("mode", choices=["tele", "mix"])
    ap.add_argument("minutes", type=float)
    ap.add_argument("--out", default=os.path.join("build", "memsoak"))
    ap.add_argument("--card", default=None, help="folder holding mc0 with a free-roam save in slot 8")
    ap.add_argument("--exe", default=None)
    ap.add_argument("env", nargs="*")
    a = ap.parse_intermixed_args()
    os.makedirs(a.out, exist_ok=True)
    csvp = os.path.join(a.out, a.name + ".csv")
    env = {"VCS_DRAW_DISTANCE": "100"}
    env.update(dict(x.split("=", 1) for x in a.env))
    g = lcs_test.Lcs.start(a.port, os.path.join(a.out, "data_" + a.name), card=a.card, slot=8 if a.card else None,
                           exe=a.exe, seconds=int(a.minutes * 60 + 900), env=env)
    if a.card:
        g.wait_ingame()
    else:
        g.new_game()
        g.pass_first_mission()
    t0 = time.time()
    state = {"stop": False, "round": 0}
    f = open(csvp, "w")
    f.write("t_s,private_mb,ws_mb,frames,x,y,round\n")

    def sampler():
        while not state["stop"]:
            m = mem(g.pid)
            if m is None:
                f.write("%d,dead,,,,,\n" % (time.time() - t0))
                f.flush()
                return
            try:
                s = g.state()
                fr, p = s.get("frame"), s.get("pos") or (0, 0, 0)
            except Exception:
                fr, p = "", (0, 0, 0)
            f.write("%d,%.1f,%.1f,%s,%.0f,%.0f,%d\n" % (time.time() - t0, m[0], m[1], fr, p[0], p[1], state["round"]))
            f.flush()
            time.sleep(30)

    threading.Thread(target=sampler, daemon=True).start()
    try:
        g.trainer("toggle god 1")
        lm = [int(x.split(":")[0]) for x in g.trainer("list landmarks").split(";") if ":" in x]
        t_end = t0 + a.minutes * 60
        while time.time() < t_end and g.alive():
            for n in lm:
                if time.time() >= t_end or not g.alive():
                    break
                g.raw("trainer setwanted 0")
                g.landmark(n)
                if a.mode == "tele":
                    time.sleep(4)
                else:
                    time.sleep(3)
                    g.raw("trainer spawn 152")
                    time.sleep(2)
                    g.key("W", True)
                    for i in range(20):
                        time.sleep(1)
                        if i % 5 == 3:
                            g.hold("D", 0.5)
                    g.key("W", False)
                    g.press("F", 300)
            state["round"] += 1
    finally:
        state["stop"] = True
        time.sleep(1)
        try:
            g.quit()
        except Exception:
            pass
        f.close()
    print(a.name, verdict(csvp))


if __name__ == "__main__":
    main()
