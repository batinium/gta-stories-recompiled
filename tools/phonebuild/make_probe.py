#!/usr/bin/env python3
"""[phonebuild] Add an Android-hosted clang + lld (Termux's packages, unpacked by termux_fetch.py) to an APK as
lib/<abi>/lib*.so, so the package manager extracts them into the app's nativeLibraryDir, the one place an app that
targets API 29+ may exec() files from (docs/PHONE_BUILD.md, "Running a compiler inside an app").

    python3 tools/phonebuild/make_probe.py <abi> <termux usr dir> <apk to append to>

clang-21 becomes libclang.so, lld liblld.so. The package manager extracts only lib*.so names, so the versioned
libraries are renamed (libz.so.1 -> libz1.so) in the file name and in the DT_NEEDED / SONAME strings (same length or
shorter, NUL padded). Prototype only: the real builder uses our own LLVM build (one static binary, no renames).
Standard library only.
"""
import os
import re
import sys
import zipfile

RENAME = {b"libz.so.1\0": b"libz1.so\0\0", b"libzstd.so.1\0": b"libzstd1.so\0\0",
          b"libxml2.so.16\0": b"libxml216.so\0\0", b"liblzma.so.5\0": b"liblzma5.so\0\0"}


def main():
    abi, usr, apk = sys.argv[1:4]
    lib, bin_ = os.path.join(usr, "lib"), os.path.join(usr, "bin")
    files = {"libclang.so": os.path.join(bin_, "clang-21"), "liblld.so": os.path.join(bin_, "lld")}
    for name in ("libclang-cpp.so", "libLLVM.so", "libc++_shared.so", "libffi.so", "libiconv.so", "libandroid-glob.so"):
        files[name] = os.path.join(lib, name)
    for f in os.listdir(lib):  # libz.so.1.4 -> libz1.so ...
        m = re.match(r"(lib[\w+-]+)\.so\.(\d+)\.[\d.]+$", f)
        if m and m.group(1) in ("libz", "libzstd", "libxml2", "liblzma"):
            files[m.group(1) + m.group(2) + ".so"] = os.path.join(lib, f)
    with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
        for name, src in sorted(files.items()):
            with open(src, "rb") as f:
                data = f.read()
            for a, b in RENAME.items():
                data = data.replace(a, b)
            z.writestr("lib/%s/%s" % (abi, name), data)
            print(abi, name, len(data) >> 10, "KB")


if __name__ == "__main__":
    main()
