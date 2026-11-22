#!/usr/bin/env python3
"""[android] Check the touch overlay's gesture timing (TouchGestures.java) on the PC's JVM (docs/ANDROID.md
"Controls extras: gestures"). Needs a JDK (javac, java); no Android SDK, no device, no network.

    python3 tools/android_gestures_check.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join("io", "github", "batinium", "vcsrecomp")
SOURCES = [os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "TouchGestures.java"),
           os.path.join(ROOT, "android", "app", "src", "test", "java", PKG, "TouchGesturesCheck.java")]


def main():
    out = os.path.join(ROOT, "build", "android", "gestures-check")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    subprocess.check_call(["javac", "-encoding", "UTF-8", "--release", "17", "-d", out] + SOURCES)
    return subprocess.call(["java", "-cp", out, "io.github.batinium.vcsrecomp.TouchGesturesCheck"])


if __name__ == "__main__":
    sys.exit(main())
