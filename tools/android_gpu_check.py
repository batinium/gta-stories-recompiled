#!/usr/bin/env python3
"""[android] Check the launcher's GPU log parsing and tested-phones list (GpuLog.java) on the PC's JVM
(docs/ANDROID_LAUNCHER.md "Untested phones and start-up errors"). Needs a JDK (javac, java); no Android SDK, no device.

    python3 tools/android_gpu_check.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join("io", "github", "batinium", "vcsrecomp")
SOURCES = [os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "GpuLog.java"),
           os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "ReportLog.java"),
           os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "SaveBackup.java"),
           os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "SaveCard.java"),
           os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "PS2Save.java"),
           os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "Lzari.java"),
           os.path.join(ROOT, "android", "app", "src", "test", "java", PKG, "SaveBackupCheck.java"),
           os.path.join(ROOT, "android", "app", "src", "test", "java", PKG, "GpuLogCheck.java"),
           os.path.join(ROOT, "android", "app", "src", "test", "java", PKG, "DeviceProfilesCheck.java")]


def check_native_format():
    """The native heartbeat / watchdog lines (src/android/diagnostics.cpp) still have the fields ReportLog reads."""
    src = open(os.path.join(ROOT, "src", "android", "diagnostics.cpp"), encoding="utf-8").read()
    fields = ["[hb] t=", " frames=", " fps=", " frame_ms_max=", " thermal=", " battery=", " mem_free=", " game=", " driver="]
    pos = src.find(fields[0])
    for f in fields[1:]:
        nxt = src.find(f, pos)
        if pos < 0 or nxt < 0:
            print("FAIL diagnostics.cpp: heartbeat field '%s' missing or out of order" % f.strip())
            return 1
        pos = nxt
    if "[watchdog] no frame for" not in src or "VCS_WATCHDOG" not in src or "/proc/self/task" not in src:
        print("FAIL diagnostics.cpp: watchdog line / switch / thread dump missing")
        return 1
    print("native heartbeat + watchdog format ok")
    return 0


def main():
    if check_native_format():
        return 1
    out = os.path.join(ROOT, "build", "android", "gpu-check")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    subprocess.check_call(["javac", "-encoding", "UTF-8", "--release", "17", "-d", out] + SOURCES)
    cp = ["java", "-cp", out]
    bad = subprocess.call(cp + ["io.github.batinium.vcsrecomp.GpuLogCheck"])
    bad |= subprocess.call(cp + ["io.github.batinium.vcsrecomp.SaveBackupCheck"])
    # the unsupported / untested phone profiles: synthetic logs (tools/fixtures/device_logs) -> what the launcher says
    bad |= subprocess.call(cp + ["io.github.batinium.vcsrecomp.DeviceProfilesCheck",
                                 os.path.join(ROOT, "tools", "fixtures", "device_logs"),
                                 os.path.join(ROOT, "android", "app", "src", "main", "assets", "tested_devices.txt")])
    return bad


if __name__ == "__main__":
    sys.exit(main())
