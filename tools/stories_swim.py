#!/usr/bin/env python3
"""The "swim" kind of tools/stories_pack.py (docs/LCS.md "Swimming"): Vice City Stories' swimming animations, read
once from the player's own VCS disc and written as a local LCS mod. Standard library only. Nothing here is game data: the pack is made on the
player's computer from the player's disc, lives in the game's data folder (<data>/mods/stories_<from>_<kind>/) and is
never committed or shipped (.gitignore and tools/prepublish_check.py refuse stories_* mod folders and *.anm).

    python tools/stories_pack.py swim VCS_ISO [--data DIR] [--json]
        [lcs] Vice City Stories' swimming animations (swim_idle - treading water -, swim - breaststroke -,
        swim_fast - crawl -, swim_start / swim_to_idle, swim_jump_*) from VCS's GAME.DTZ, converted to LCS's
        animation layout -> <LCS data>/mods/stories_vcs_swim/files/vcs_swim.anm and the mod's manifest; the mod
        manager lists it as "Swim animations (from your VCS disc)" (on / off like any mod) and active.txt gets
        `anims set=swim file=stories_vcs_swim/files/vcs_swim.anm`, which src/game/lcs_swim.cpp loads.
    python tools/stories_pack.py swim VCS_ISO --dump      list the swim animations found (no files written)

(tools/stories_pack.py's `swim` subcommand: add_parser() and run() here.)

How the animations are stored (found 2026-11-18 by comparing both games' GAME.DTZ; docs/LCS.md "Swimming"):
GAME.DTZ is a zlib stream of a "GATG" memory image whose pointers are image offsets (relocated at load). Both games
keep their animations in it as CAnimBlendHierarchy records - VCS 0x28 bytes {+0 sequences, +4 name[0x14], +0x1C
s32 sequence count, +0x20 float length, +0x24 a word LCS doesn't have}, LCS 0x24 bytes {+0 sequences, +4 name[0x14],
+0x1C s16 count, +0x1E s16 0xAAAA, +0x20 float length} - each sequence 0xC bytes {u16 flags, u16 frames, keyframes,
u32 bone tag}. The keyframes are the same in both: half floats, 10 bytes {quaternion x y z w, time step} or, with
flag 2, 16 bytes {quaternion, step, translation x y z}; the first step is 0, the steps add up to the length
(or less: a bone may stop early). Flag 1 =
rotation, 2 = translation; VCS marks its sequences 0x08 where LCS marks them 0x10 (mapped). The bone tags are the
same (both peds share the skeleton's ids: 0 root, 1 pelvis, ... the same hash in the upper half).

.anm (this tool's own container, little endian):
    "STANIM01", u32 animation count, then per animation:
        char name[24], u32 hint (bit 0: loops), float length, u32 sequence count, then per sequence:
            u16 flags (LCS's), u16 frames, u32 bone tag, u32 bytes, the keyframes (padded to 4)
"""
import argparse
import json
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MAGIC = b"STANIM01"
SWIM_MOD_ID = "stories_vcs_swim"
SWIM_MOD_NAME = "Swim animations (from your VCS disc)"
SWIM_FILE = "vcs_swim.anm"
# the animations taken (VCS's swim set); hint bit 0: loops
SWIM_ANIMS = {"swim_idle": 1, "swim": 1, "swim_fast": 1, "swim_start": 0, "swim_to_idle": 0, "swim_jump_st": 0,
              "swim_jump_mid": 1, "swim_jump_mid2": 0, "swim_jump_end": 0, "swim_jump_miss": 0}


class PackError(Exception):
    pass


def read_disc_file(iso, path):
    import vcs_disc
    img = vcs_disc.IsoImage(iso)
    rec = img.find(path)
    if not rec or rec["dir"]:
        raise PackError("%s: no %s on this disc" % (iso, path))
    return img.read_file(rec)


def vcs_image(iso):
    """VCS's GAME.DTZ, inflated (the GATG image)"""
    import vcs_disc
    r = vcs_disc.check(iso, games=("vcs",))
    if not r.get("ok"):
        raise PackError("%s is not the supported Vice City Stories disc: %s" % (iso, r.get("message")))
    img = zlib.decompress(read_disc_file(iso, "PS2/GAME.DTZ"))
    if img[:4] != b"GATG":
        raise PackError("GAME.DTZ: not a GATG image")
    return img


def vcs_hierarchies(img, wanted):
    """{name: (length, [(flags, frames, tag, keyframe bytes)])} for the VCS hierarchies named in `wanted`"""
    out = {}
    size = len(img)
    for name in wanted:
        key = name.encode() + b"\0"
        pos = 0
        while True:
            at = img.find(key, pos)
            if at < 0:
                break
            pos = at + 1
            rec = at - 4
            if rec < 0 or rec % 4 or rec + 0x28 > size:
                continue
            seqs, = struct.unpack_from("<I", img, rec)
            count, length = struct.unpack_from("<if", img, rec + 0x1C)
            if not (0 < count <= 64 and 0.0 < length < 60.0 and 0 < seqs and seqs + count * 12 <= size):
                continue
            if img[rec + 4 + len(key):rec + 0x18].strip(b"\0"):
                continue  # not a clean 0x14-byte name field
            parts, ok = [], True
            for i in range(count):
                flags, frames, kf, tag = struct.unpack_from("<HHII", img, seqs + i * 12)
                stride = 16 if flags & 2 else 10
                if flags & ~0x0B or not flags & 1 or not 0 < frames < 4096 or kf + frames * stride > size:
                    ok = False
                    break
                data = img[kf:kf + frames * stride]
                # the steps (half floats, the first 0) add up to at most the hierarchy's length (a bone may stop
                # early): a check that this is a keyframe block
                steps = [struct.unpack_from("<e", data, j * stride + 8)[0] for j in range(frames)]
                if steps[0] != 0.0 or min(steps) < 0.0 or sum(steps) > length + 0.02:
                    ok = False
                    break
                parts.append(((flags & 3) | (0x10 if flags & 8 else 0), frames, tag, data))
            if ok:
                out[name] = (length, parts)
                break
    return out


def pack(anims, hints):
    b = bytearray(MAGIC + struct.pack("<I", len(anims)))
    for name, (length, seqs) in anims.items():
        b += name.encode().ljust(24, b"\0")[:24] + struct.pack("<IfI", hints.get(name, 0), length, len(seqs))
        for flags, frames, tag, data in seqs:
            b += struct.pack("<HHII", flags, frames, tag, len(data)) + data + b"\0" * (-len(data) % 4)
    return bytes(b)


def unpack(data):
    """the .anm back (a check of what was written; lcs_swim.cpp reads it the same way)"""
    if data[:8] != MAGIC:
        raise PackError("not a Stories animation pack")
    n, = struct.unpack_from("<I", data, 8)
    p, out = 12, {}
    for _ in range(n):
        name = data[p:p + 24].split(b"\0")[0].decode()
        hint, length, count = struct.unpack_from("<IfI", data, p + 24)
        p += 36
        seqs = []
        for _ in range(count):
            flags, frames, tag, size = struct.unpack_from("<HHII", data, p)
            p += 12
            seqs.append((flags, frames, tag, data[p:p + size]))
            p += size + (-size % 4)
        out[name] = (hint, length, seqs)
    return out


def write_swim_mod(data_dir, blob, iso):
    """the pack as a mod in <data>/mods/stories_vcs_swim/ (tools/mods.py's manifest layout), active.txt rewritten"""
    os.environ["VCS_TOOLS_GAME"] = "lcs"
    import mods
    store = mods.Store("lcs", data_dir)
    d = os.path.join(store.dir, SWIM_MOD_ID, "files")
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, SWIM_FILE + ".part")
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, os.path.join(d, SWIM_FILE))
    try:
        m = store.get(SWIM_MOD_ID)
        enabled = m.get("enabled", True)
    except mods.ModError:
        enabled = True
    e = mods.entry("anims", "VCS swim animations", "anims", "anims set=swim file=%s" % SWIM_FILE,
                   os.path.basename(iso))
    e["target"], e["match"], e["fields"] = 0, "picked", {"set": "swim", "file": SWIM_FILE}
    m = {"loader": 2, "game": "lcs", "id": SWIM_MOD_ID, "name": SWIM_MOD_NAME, "source": os.path.basename(iso),
         "imported": int(time.time() * 1000), "enabled": enabled, "storage": "copy", "entries": [e],
         "notes": ["Made on this computer from your own Vice City Stories disc (tools/stories_pack.py swim): "
                   "Toni swims with Vic's swimming animations. Off: the stand-in pose."],
         "sources": [], "files": [{"name": SWIM_FILE, "size": len(blob), "sha256": mods.sha256(blob)}],
         "generated": "stories_pack"}
    store.save(m)
    store.write_active()
    return os.path.join(d, SWIM_FILE)


def cmd_swim(iso, data_dir=None, dump=False):
    img = vcs_image(iso)
    anims = vcs_hierarchies(img, list(SWIM_ANIMS))
    missing = [n for n in SWIM_ANIMS if n not in anims]
    if "swim_idle" in missing or "swim" in missing:
        raise PackError("the swim animations were not found in this disc's GAME.DTZ (missing: %s)" % ", ".join(missing))
    info = {n: {"length": round(l, 3), "sequences": len(s), "frames": max(x[1] for x in s),
                "bytes": sum(len(x[3]) for x in s)} for n, (l, s) in anims.items()}
    if dump:
        return {"animations": info, "missing": missing}
    blob = pack(anims, SWIM_ANIMS)
    assert set(unpack(blob)) == set(anims)
    if data_dir is None:
        os.environ["VCS_TOOLS_GAME"] = "lcs"
        import backup
        data_dir = backup.data_dir()
    path = write_swim_mod(data_dir, blob, iso)
    return {"mod": SWIM_MOD_ID, "name": SWIM_MOD_NAME, "file": path, "bytes": len(blob), "animations": info,
            "missing": missing}


def add_parser(sub):
    p = sub.add_parser("swim", help="[lcs] VCS's swimming animations as an LCS mod (mods/stories_vcs_swim)")
    p.add_argument("iso", help="your Vice City Stories (USA) disc image")
    p.add_argument("--data", help="LCS's data folder (default: VCS_DATA_DIR, else the game's own)")
    p.add_argument("--dump", action="store_true", help="only list what was found")
    p.add_argument("--json", action="store_true")


def run(a):
    try:
        out = cmd_swim(a.iso, a.data, a.dump)
    except (PackError, OSError, zlib.error) as e:
        out = {"error": str(e)}
    if os.environ.get("VCS_PROGRESS") == "json":  # the launcher: an error event, then the result
        if "error" in out:
            print("@vcs " + json.dumps({"error": "stories_pack", "message": out["error"]}), flush=True)
        print("@vcs " + json.dumps({"result": out}), flush=True)
    elif a.json:
        print(json.dumps(out))
    else:
        if "error" in out:
            print("error:", out["error"])
        else:
            for n, i in out["animations"].items():
                print("%-16s %5.3f s  %2d bones  %3d frames  %6d bytes" % (n, i["length"], i["sequences"], i["frames"],
                                                                        i["bytes"]))
            if out.get("missing"):
                print("not found:", ", ".join(out["missing"]))
            if out.get("file"):
                print("written:", out["file"], "(%d bytes) - mod %r" % (out["bytes"], out["name"]))
    return 1 if "error" in out else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split(chr(10) * 2)[0])
    add_parser(ap.add_subparsers(dest="cmd", required=True))
    sys.exit(run(ap.parse_args()))
