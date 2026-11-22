#!/usr/bin/env python3
"""[android] See what the launcher shows on a phone nobody has tested, without owning one: put a synthetic device log
(tools/fixtures/device_logs/<name>.log) in the debug `.test` app as its last game log, forget which notices were shown,
start the launcher and save a screenshot (build/android/device-sim/<name>.png).

    python3 tools/android_device_sim.py list
    python3 tools/android_device_sim.py run mali_g715_runs_untested [adreno840_ok ...]   # or: run all
    python3 tools/android_device_sim.py run all --keep      # leave the last fixture in place (default: put the app's own log back)

The phone must be connected and unlocked, with the `.test` app installed (VCS_APP_ID_SUFFIX=.test python3 tools/android.py
build; install). It only reads / replaces files/logs/game.log and shared_prefs/device.xml of that app; nothing is tapped.
The notice and the start-up error dialog come from the log's GPU (DeviceNotice / GpuLog): the screenshot is what a player
with that phone would see on opening the app. Expectations for the same logs are checked off-device by
tools/android_gpu_check.py.
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tools", "fixtures", "device_logs")
OUT = os.path.join(ROOT, "build", "android", "device-sim")
PKG = "com.batinium.gtastories.test"
ACT = PKG + "/io.github.batinium.vcsrecomp.LauncherActivity"


def adb(*a, stdin=None, binary=False):
    r = subprocess.run(["adb"] + list(a), stdin=stdin, capture_output=True, timeout=60)
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")


def runas(cmd, stdin=None):
    return adb("shell", "run-as", PKG, "sh", "-c", "'%s'" % cmd, stdin=stdin)


def screenshot(path):
    """The lit display's picture: with several displays the first may be black (a folded phone's inner screen)."""
    ids = [l.split()[1] for l in adb("shell", "dumpsys", "SurfaceFlinger", "--display-id").splitlines() if l.startswith("Display ")]
    best = b""
    for i in ids or [None]:
        png = adb("exec-out", "screencap", *(["-d", i] if i else []), "-p", binary=True)
        png = png[png.find(b"\x89PNG"):] if b"\x89PNG" in png else b""
        if len(png) > len(best):
            best = png
    open(path, "wb").write(best)


def focus():
    return [l for l in adb("shell", "dumpsys", "window").splitlines() if "mCurrentFocus" in l and "null" not in l]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    keep = "--keep" in sys.argv
    names = sorted(f[:-4] for f in os.listdir(FIX) if f.endswith(".log"))
    if not args or args[0] == "list":
        print("\n".join(names))
        return 0
    if args[0] != "run":
        print(__doc__)
        return 2
    picked = names if args[1:] == ["all"] else args[1:]
    for n in picked:
        if n not in names:
            print("no fixture", n)
            return 2
    if PKG not in adb("shell", "pm", "list", "packages", PKG):
        print("the debug app", PKG, "is not installed on the connected phone")
        return 1
    os.makedirs(OUT, exist_ok=True)
    runas("cp files/logs/game.log files/logs/game.log.sim-backup 2>/dev/null; true")
    for n in picked:
        text = open(os.path.join(FIX, n + ".log"), encoding="utf-8").read()
        body = text.split("\n", 1)[1]  # without the '# expect:' line
        tmp = os.path.join(OUT, n + ".txt")
        open(tmp, "w", encoding="utf-8").write(body)
        with open(tmp, "rb") as fh:
            runas("mkdir -p files/logs; cat > files/logs/game.log", stdin=fh)
        runas("rm -f files/logs/game.prev.log shared_prefs/device.xml")  # forget the GPU and the shown notices
        adb("shell", "am", "force-stop", PKG)
        adb("shell", "am", "start", "--user", "0", "-n", ACT)
        time.sleep(4)
        if not any(PKG in l for l in focus()):
            print(n, ": the app is not in front (phone in use or locked?), no screenshot")
            continue
        out = os.path.join(OUT, n + ".png")
        screenshot(out)
        print(n, "->", os.path.relpath(out, ROOT))
        os.remove(tmp)
    if not keep:
        runas("mv files/logs/game.log.sim-backup files/logs/game.log 2>/dev/null; rm -f files/logs/game.log.sim-backup")
    adb("shell", "am", "force-stop", PKG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
