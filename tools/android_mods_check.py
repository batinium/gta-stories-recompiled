#!/usr/bin/env python3
"""[android] Check the launcher's mod importer (ModFormats.java) on the PC's JVM (docs/ANDROID.md "Mods"). Needs a JDK
(javac, java); no Android SDK, no device.

    python3 tools/android_mods_check.py                 built-in checks (dialects, zip, .vcm, slot matching)
    python3 tools/android_mods_check.py <mod files>...  also import each file (.zip / .vcm / .cfg / .txt) and print
                                                        what the loader would use

Keep mods out of the repository: they are other people's files.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join("io", "github", "batinium", "vcsrecomp")
MAIN = os.path.join(ROOT, "android", "app", "src", "main", "java", PKG)
TEST = os.path.join(ROOT, "android", "app", "src", "test", "java", PKG)
SOURCES = [os.path.join(MAIN, "ModFormats.java"), os.path.join(TEST, "ModFormatsCheck.java")]


def main():
    out = os.path.join(ROOT, "build", "android", "mods-check")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    subprocess.check_call(["javac", "-encoding", "UTF-8", "--release", "17", "-d", out] + SOURCES)
    slots = os.path.join(ROOT, "tools", "modslots", "mod_slots.txt")  # the slot tables, shared with tools/mods.py
    return subprocess.call(["java", "-cp", out, "io.github.batinium.vcsrecomp.ModFormatsCheck", "--slots", slots]
                           + [os.path.abspath(a) for a in sys.argv[1:]])


if __name__ == "__main__":
    sys.exit(main())
