#!/usr/bin/env python3
"""[android] Check the Android updater's version and release-notes code (UpdateVersion.java) on the PC's JVM
(docs/ANDROID.md "Releases and updates"). Needs a JDK (javac, java); no Android SDK, no device, no network.

    python3 tools/android_update_check.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join("io", "github", "batinium", "vcsrecomp")
SOURCES = [os.path.join(ROOT, "android", "app", "src", "main", "java", PKG, "UpdateVersion.java"),
           os.path.join(ROOT, "android", "app", "src", "test", "java", PKG, "UpdateVersionCheck.java")]


def main():
    out = os.path.join(ROOT, "build", "android", "update-check")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    subprocess.check_call(["javac", "-encoding", "UTF-8", "--release", "17", "-d", out] + SOURCES)
    return subprocess.call(["java", "-cp", out, "io.github.batinium.vcsrecomp.UpdateVersionCheck"])


if __name__ == "__main__":
    sys.exit(main())
