#!/usr/bin/env python3
"""Change strings in a Stories GXT (the TABL / TKEY / TDAT layout with 16-bit characters: LCS's TEXT/ENGLISH.GXT,
GTA III-style) and write a new file, for the mod loader's `file` directive (src/game/vcs_mods.h).

    python -I tools/gxt_edit.py <in.gxt> <out.gxt> KEY=text [KEY=text ...]     change the MAIN table's KEY
    python -I tools/gxt_edit.py <in.gxt> --find TEXT                           list MAIN keys whose text holds TEXT
    python -I tools/gxt_edit.py extract <iso> <out.gxt>                        TEXT/ENGLISH.GXT from your own disc

The new text is written in place: it may be as long as the old one or shorter (the rest of the old text's room is
filled with NULs), never longer - the file's offsets stay as they are. Characters are written as their 16-bit code
(the game's font maps its own symbols on some codes; plain ASCII letters, digits and punctuation are safe).

The input is the player's own disc file and the output is derived from it: keep both out of the repository.
"""
import os
import struct
import sys


def tables(data):
    if data[:4] != b"TABL":
        raise SystemExit("not a TABL GXT (LCS / GTA III layout)")
    size, = struct.unpack_from("<I", data, 4)
    out = []
    for i in range(size // 12):
        name, offset = struct.unpack_from("<8sI", data, 8 + i * 12)
        out.append((name.rstrip(b"\0").decode("ascii", "replace"), offset))
    return out


def main_entries(data):
    """{key: (absolute offset of the text, its length in characters)} of the MAIN table"""
    name, offset = tables(data)[0]
    if name != "MAIN" or data[offset:offset + 4] != b"TKEY":
        raise SystemExit("the first table is not MAIN with a TKEY block")
    keys_size, = struct.unpack_from("<I", data, offset + 4)
    tdat = offset + 8 + keys_size
    if data[tdat:tdat + 4] != b"TDAT":
        raise SystemExit("no TDAT after MAIN's TKEY")
    base = tdat + 8
    out = {}
    for i in range(keys_size // 12):
        text_off, key = struct.unpack_from("<I8s", data, offset + 8 + i * 12)
        start = base + text_off
        end = start
        while data[end:end + 2] != b"\0\0":
            end += 2
        out[key.rstrip(b"\0").decode("ascii", "replace")] = (start, (end - start) // 2)
    return out


def text_at(data, start, length):
    return data[start:start + length * 2].decode("utf-16-le", "replace")


def main(argv):
    if len(argv) >= 3 and argv[0] == "extract":  # the repository's ISO 9660 reader does it
        import subprocess
        return subprocess.call([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "vcs_disc.py"),
                                "extract", argv[1], "TEXT/ENGLISH.GXT", argv[2]])
    if len(argv) < 2:
        raise SystemExit(__doc__)
    data = bytearray(open(argv[0], "rb").read())
    entries = main_entries(data)
    if argv[1] == "--find":
        needle = argv[2].lower() if len(argv) > 2 else ""
        for key, (start, length) in sorted(entries.items()):
            text = text_at(data, start, length)
            if needle in text.lower():
                print("%-8s %r" % (key, text))
        return 0
    out = argv[1]
    for change in argv[2:]:
        key, sep, text = change.partition("=")
        if not sep or key not in entries:
            raise SystemExit("%s: no such key in MAIN (use --find)" % key)
        start, length = entries[key]
        if len(text) > length:
            raise SystemExit("%s: %r is longer than the stock text %r (%d characters at most)" %
                             (key, text, text_at(data, start, length), length))
        old = text_at(data, start, length)
        data[start:start + length * 2] = text.encode("utf-16-le").ljust(length * 2, b"\0")
        print("%s: %r -> %r" % (key, old, text))
    with open(out, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (out, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
