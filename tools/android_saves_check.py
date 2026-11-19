#!/usr/bin/env python3
"""[android] Check the Android launcher's save import / export code on the PC's JVM (docs/ANDROID.md "Saves: import and
export"). Needs a JDK (javac, java); no Android SDK, no device.

    python3 tools/android_saves_check.py                          built-in checks (names, order, round trips, install)
    python3 tools/android_saves_check.py <saves in> <expected>    also decode every save in <saves in> and compare each
                                                                  file with <expected>/<id>/<save folder>/<file>

<id> is the second dot-separated part of an input's name (game.14651.max -> 14651). Keep sample saves out of the
repository: they are other people's files.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join("io", "github", "batinium", "vcsrecomp")
MAIN = os.path.join(ROOT, "android", "app", "src", "main", "java", PKG)
TEST = os.path.join(ROOT, "android", "app", "src", "test", "java", PKG)
SOURCES = [os.path.join(MAIN, n) for n in ("Lzari.java", "PS2Save.java", "SaveCard.java")] + \
          [os.path.join(TEST, "SaveFormatsCheck.java")]


def main():
    out = os.path.join(ROOT, "build", "android", "saves-check")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    subprocess.check_call(["javac", "-encoding", "UTF-8", "--release", "17", "-d", out] + SOURCES)
    return subprocess.call(["java", "-cp", out, "io.github.batinium.vcsrecomp.SaveFormatsCheck"] + sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
