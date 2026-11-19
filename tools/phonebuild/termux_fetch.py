#!/usr/bin/env python3
"""[phonebuild] Download official Termux packages (packages-cf.termux.dev, termux-main, stable) for one CPU
architecture, check each against the repository index's SHA256 and unpack its data.tar.* (prototype toolchain for
docs/PHONE_BUILD.md; nothing here is ever committed or shipped).

    python3 -I tools/phonebuild/termux_fetch.py <dest> <aarch64|x86_64> clang,lld,libllvm,libc++,libcompiler-rt,\
ndk-sysroot,zlib,zstd,libxml2,ncurses,libiconv,liblzma,libandroid-glob,libffi

<dest>/Packages_<arch> must be the index: https://packages-cf.termux.dev/apt/termux-main/dists/stable/main/binary-<arch>/Packages
Unpacks into <dest>/<arch>/root (symbolic links are listed in <dest>/<arch>/symlinks.txt, not made: Windows), and
appends "<package> <version> <url> sha256=<hash>" lines to <dest>/<arch>/SOURCES.txt. Standard library only.
"""
import hashlib
import io
import os
import re
import sys
import tarfile
import urllib.request

BASE = "https://packages-cf.termux.dev/apt/termux-main/"


def main():
    dest, arch, names = sys.argv[1], sys.argv[2], sys.argv[3].split(",")
    with open(os.path.join(dest, "Packages_" + arch), encoding="utf-8") as f:
        index = f.read()
    packages = {}
    for block in index.split("\n\n"):
        d = dict(re.findall(r"^([\w-]+): (.*)$", block, re.M))
        if "Package" in d:
            packages[d["Package"]] = d
    root = os.path.join(dest, arch, "root")
    os.makedirs(root, exist_ok=True)
    for name in names:
        d = packages[name]
        url = BASE + d["Filename"]
        # the CDN refuses Python's default User-Agent
        data = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "Debian APT-HTTP/1.3"})).read()
        sha = hashlib.sha256(data).hexdigest()
        if sha != d["SHA256"]:
            sys.exit("%s: sha256 %s, the index says %s" % (name, sha, d["SHA256"]))
        with open(os.path.join(dest, arch, "SOURCES.txt"), "a") as f:
            f.write("%s %s %s sha256=%s\n" % (name, d["Version"], url, sha))
        if data[:8] != b"!<arch>\n":
            sys.exit(name + ": not a .deb")
        off = 8
        while off < len(data):  # the .deb's ar members: debian-binary, control.tar.*, data.tar.*
            member = data[off:off + 16].decode().strip()
            size = int(data[off + 48:off + 58].decode().strip())
            body = data[off + 60:off + 60 + size]
            off += 60 + size + (size & 1)
            if not member.startswith("data.tar"):
                continue
            with tarfile.open(fileobj=io.BytesIO(body)) as t:
                for m in t.getmembers():
                    if m.issym() or m.islnk():
                        with open(os.path.join(dest, arch, "symlinks.txt"), "a") as f:
                            f.write("%s -> %s\n" % (m.name, m.linkname))
                        continue
                    t.extract(m, root, filter="data")
        print(arch, name, d["Version"], len(data) >> 10, "KB")


if __name__ == "__main__":
    main()
