#!/usr/bin/env python3
"""[phonebuild] Refresh tools/phonebuild/termux_packages.txt (the compiler packages the app downloads, pinned by
SHA-256) from Termux's repository indexes:

    curl -o Packages_aarch64 https://packages-cf.termux.dev/apt/termux-main/dists/stable/main/binary-aarch64/Packages
    curl -o Packages_x86_64  https://packages-cf.termux.dev/apt/termux-main/dists/stable/main/binary-x86_64/Packages
    python3 tools/phonebuild/termux_pins.py Packages_aarch64 Packages_x86_64

Then rebuild the APK (tools/android.py build stages the file as assets/phonebuild/termux.txt). Standard library only.
"""
import os
import re
import sys

PACKAGES = [("clang", "Apache-2.0_WITH_LLVM-exception"), ("lld", "Apache-2.0_WITH_LLVM-exception"),
            ("libllvm", "Apache-2.0_WITH_LLVM-exception"), ("libc++", "Apache-2.0_WITH_LLVM-exception"),
            ("zlib", "Zlib"), ("zstd", "BSD-3-Clause"), ("libxml2", "MIT"), ("libiconv", "LGPL-2.1-or-later"),
            ("liblzma", "0BSD"), ("libandroid-glob", "BSD-3-Clause"), ("libffi", "MIT")]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "termux_packages.txt")


def main():
    with open(OUT, encoding="utf-8") as f:
        header = [line for line in f if line.startswith("#")]
    lines = []
    for arch, index in (("aarch64", sys.argv[1]), ("x86_64", sys.argv[2])):
        packages = {}
        with open(index, encoding="utf-8") as f:
            for block in f.read().split("\n\n"):
                d = dict(re.findall(r"^([\w+-]+): (.*)$", block, re.M))
                if "Package" in d:
                    packages[d["Package"]] = d
        for name, licence in PACKAGES:
            d = packages[name]
            lines.append(" ".join((arch, name, d["Version"], d["Size"], d["SHA256"], d["Filename"], licence)))
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(header)
        f.write("\n".join(lines) + "\n")
    print("%d packages -> %s" % (len(lines), OUT))


if __name__ == "__main__":
    main()
