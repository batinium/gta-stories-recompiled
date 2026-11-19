#!/usr/bin/env python3
"""Other-game asset packs for "Stories: both cities" (docs/STORIES.md "Cross-game vehicles"). Standard library only.

Liberty City Stories and Vice City Stories use one engine family: their vehicle models are the same relocatable PS2
chunks (MDL / XTX, docs/LCS.md "Where LCS differs from VCS"). This tool reads the OTHER game's assets ONCE from the
player's own disc image and writes them into the running game's mod folder as a mod pack the mod loader already
understands: each vehicle as a PC-format model (.dff + .txd, decoded from the PS2 chunks here) that the game's
converter (src/game/vcs_modconv.cpp) turns into its own dialect on a stock donor car, cached, like any PC car mod.
Nothing is read from the disc while playing. The pack is the player's own data: it must never be committed, shared
or shipped (.gitignore and tools/prepublish_check.py refuse `stories_vcs_*` / `stories_lcs_*` folders).

    python tools/stories_pack.py list  <other game's iso>                     its vehicles (id, name, class, wheel)
    python tools/stories_pack.py build <other game's iso> --game lcs [--data DIR]
                    [--vehicles DELUXO,SABRETB,...] [--slot NAME=ID ...]       the pack, as mod stories_<src>_vehicles
    python tools/stories_pack.py dump  <other game's iso> NAME OUTDIR          one vehicle's .dff / .txd (checks)
    python tools/stories_pack.py swim  <VCS iso> [--data DIR] [--dump]          [lcs] VCS's swimming animations as
                    mod stories_vcs_swim ("Swim animations (from your VCS disc)"; tools/stories_swim.py, lcs_swim.cpp)
    python tools/stories_pack.py hero  <VCS iso> --game lcs [--data DIR] [--force] [--dump DIR]
                    [lcs] Vic Vance as LCS's player model: mod stories_vcs_hero (tools/stories_hero.py, stories_hero.cpp)

--game is the game the pack is FOR (lcs: a Vice City Stories disc is read). The pack lands in <data>/mods/
stories_<src>_vehicles/ (manifest.json + files/), and mods.py's active.txt includes it; --slot binds a vehicle to a
model id of the running game (default: the spare slots, SPARE_SLOTS). Without --slot nothing is bound: the files wait
for the handover (dualworld's crossover seam binds them at run time; docs/STORIES.md).

Kinds: "vehicles" here; "swim" (the animations, stories_vcs_swim) in tools/stories_swim.py; "hero" (the other game's
player model, stories_vcs_hero: docs/STORIES.md "One hero in both cities") in tools/stories_hero.py.
"""
import argparse
import json
import re
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vcs_disc  # noqa: E402

SECTOR = 2048

# Where each game keeps its files, and the GAME.DTZ layouts (docs/game-internals.md, docs/LCS.md "Mods"; evidence:
# src/game/vcs_mods.cpp Profile). The DTZ is zlib; its inflated image's pointers are image-relative.
#   +0x38 number of model infos, +0x3C CModelInfo pointer array (both games)
#   VCS: +0x7C streaming object, +200 its info array, 0x18 an entry; texture lists from 7534; name +0x158
#   LCS: +0x7C the info array itself, 0x14 an entry; texture lists from 4900; name +0x2E (read 2026-11-18: entry
#        152 (KURUMA) = sector 4586, 38 sectors in MODELS/GTA3PS2.IMG)
GAMES = {
    "vcs": {"dtz": "PS2/GAME.DTZ", "img": "GTA3PS2.IMG", "stride": 0x18, "tex_base": 7534, "name": 0x158,
            "node_name": 0xA8, "primary": (0x2C, 30), "secondary": (0xA4, 25), "first": 170, "last": 280,
            # vehicle model info (0x2A0): class +0x54, wheel scale +0x58 (front) / +0x5C (rear), the shared wheel
            # model +0x270 (int16; DELUXO 324, SABRE 329, BANSHEE 323; read 2026-11-18: values 319..329 or -1 over the
            # 111 vehicles), handling record pointer +0x38
            "cls": 0x54, "wheel_scale": 0x58, "wheel": 0x270,
            # simple model info (0x40): +0x28 -> its LOD atomics (the wheels 319..329: 0x4C5190 for 324)
            "atoms": 0x28},
    "lcs": {"dtz": "CHK/PS2/GAME.DTZ", "img": "MODELS/GTA3PS2.IMG", "stride": 0x14, "tex_base": 0x1324, "name": 0x2E,
            "node_name": 0xA4, "primary": (0x2C, 25), "secondary": (0x90, 25), "first": 130, "last": 216,
            # vehicle model info (0x190): class +0x38, wheel scale +0x3C, wheel model +0x40 (int16), handling id +0x42
            "cls": 0x38, "wheel_scale": 0x3C, "wheel": 0x40,
            # simple model info: +0x24 -> its LOD atomics (the wheels 250..257: 0x4A1C4C for 250)
            "atoms": 0x24},
}
# vehicle classes (model info "cls" byte) per game. LCS: 0 car, 1 boat, 2 train, 3 heli, 4 plane, 5 bike (docs/LCS.md
# "Mods"); VCS (read 2026-11-18 over its 111 vehicles): 0 car (71), 1 boat (14), 2 jet ski, 4 heli (10), 5 plane
# (hovercraft, biplane, airliner), 6 bike (9), 8 BMX (2), 9 quad bike
CLASSES = {"lcs": {0: "car", 1: "boat", 2: "train", 3: "heli", 4: "plane", 5: "bike", 6: "ferry"},
           "vcs": {0: "car", 1: "boat", 2: "boat", 4: "heli", 5: "plane", 6: "bike", 8: "bike", 9: "car"}}

# LCS car slots a crossed-over car borrows: each has a handling record of its own (no other model shares its id), so
# the other game's handling can go into it. The VC leftovers (Hunter / Mavericks 213-216, Dodo 164) are NOT usable:
# their handling records are helicopters' / a plane's and they fly (hidden run t1, 2026-11-18). ESPRIT 187 (id 60),
# SHELBY 185 (58), PONTIAC 186 (59), HOTROD 189 (62), SINDACO 190 (63), FORELLI 191 (64): LCS's special / mission cars.
# A bound slot's own car looks like the other game's car while the pack binds it. VCS: none chosen yet.
SPARE_SLOTS = {"lcs": [187, 185, 186, 189, 190, 191]}

# packs tools/stories_auto.py makes before Play besides its own list (same dict layout as its PACKS)
AUTO_PACKS = [
    {"kind": "hero", "game": "lcs", "src": "vcs", "mod": "stories_vcs_hero",
     "args": ["hero", "{iso}", "--game", "lcs", "--data", "{data}"], "title": "Vic as the player in Liberty City",
     "tools": ["stories_hero.py"]},
    {"kind": "hero", "game": "vcs", "src": "lcs", "mod": "stories_lcs_hero",
     "args": ["hero", "{iso}", "--game", "vcs", "--data", "{data}"], "title": "Toni as the player in Vice City",
     "tools": ["stories_hero.py"]},
]

# tools/stories_auto.py (packs made before Play): the directions this tool makes (src, dst) - the source game's disc
# read into the destination game's mods - and the pack format's version (bumped when vehicles.txt changes: 2 = the
# donor and colour columns), which makes existing packs again
DIRECTIONS = {("vcs", "lcs"), ("lcs", "vcs")}
PACK_VERSION = 4  # 4: LCS textures read with mipmaps / swizzle


def supports(src, dst):
    return (src, dst) in DIRECTIONS


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def i16(b, o):
    return struct.unpack_from("<h", b, o)[0]


def f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def cstr(b, o, n=32):
    if o <= 0 or o >= len(b):
        return ""
    return b[o:o + n].split(b"\0")[0].decode("latin1")


# ---- the other game's disc ------------------------------------------------------------------------------------------

class Disc:
    """The other game's GAME.DTZ image and GTA3PS2.IMG entries, read from the player's disc image"""

    def __init__(self, iso):
        self.iso = vcs_disc.IsoImage(iso)
        self.game = None
        for g, d in GAMES.items():
            rec = self.iso.find(d["dtz"])
            if rec and not rec["dir"]:
                self.game, self.g = g, d
                self.dtz = zlib.decompress(self.iso.read_file(rec))
                break
        if not self.game:
            raise SystemExit("error: %s is neither Liberty City Stories nor Vice City Stories (no GAME.DTZ)" % iso)
        if self.dtz[:4] not in (b"GATG", b"GTAG"):
            raise SystemExit("error: GAME.DTZ is not a GTAG image")
        img = self.iso.find(self.g["img"])
        self.img_lba = img["lba"]
        b = self.dtz
        self.count, self.infos = u32(b, 0x38), u32(b, 0x3C)
        self.stream = u32(b, u32(b, 0x7C) + 200) if self.game == "vcs" else u32(b, 0x7C)

    def info(self, i):
        return u32(self.dtz, self.infos + 4 * i) if 0 <= i < self.count else 0

    def name(self, i):
        p = self.info(i)
        return cstr(self.dtz, p + self.g["name"], 24) if p else ""

    def entry(self, idx):
        e = self.stream + idx * self.g["stride"]
        return u32(self.dtz, e + 0x10), u32(self.dtz, e + 0x14)

    def blob(self, idx):
        sector, n = self.entry(idx)
        if n == 0 or sector == 0xFFFFFFFF:
            return b""
        return self.iso.read_sectors(self.img_lba + sector, n)

    def vehicles(self):
        out = []
        for i in range(self.count):
            p = self.info(i)
            if p and self.dtz[p + 0x10] == 6:
                b, g = self.dtz, self.g
                out.append({"id": i, "name": self.name(i), "class": CLASSES[self.game].get(u32(b, p + g["cls"]) & 0xFF, "?"),
                            "wheel": i16(b, p + g["wheel"]), "wheel_scale": round(f32(b, p + g["wheel_scale"]), 3),
                            "txd": u16(b, p + 0x1E)})
        return out

    def find(self, name):
        for v in self.vehicles():
            if v["name"].lower() == name.lower():
                return v
        return None


# ---- PS2 geometry (both games: docs/ANDROID.md "Mods: models", the converter's writeGeometry) -------------------------

def decode_geometry(m, geom, node_name_ok=True):
    """An RslGeometry (pointers relative to `m`) -> (verts [(p, n, uv)], tris [(a, b, c, mat)], material pointers)"""
    if m[geom] != 8:
        raise ValueError("not a geometry at 0x%x" % geom)
    mat_list, n_mats = u32(m, geom + 12), u32(m, geom + 16)
    mats = [u32(m, mat_list + 4 * k) for k in range(n_mats)]
    ps2 = geom + 0x20
    n_strips = u32(m, ps2 + 16) >> 20
    dma = ps2 + u16(m, ps2 + 26)
    scale = struct.unpack_from("<3f", m, ps2 + 40)
    centre = struct.unpack_from("<3f", m, ps2 + 52)
    verts, tris = [], []
    for s in range(n_strips):
        mh = ps2 + 0x40 + s * 0x30
        uv_scale = struct.unpack_from("<2f", m, mh + 16)
        mat = u16(m, mh + 34)
        tag = dma + u32(m, mh + 28)
        end = tag + 16 + (u32(m, tag) & 0xFFFF) * 16
        at = tag + 8
        pos = nrm = uvs = None
        n = 0

        def flush():
            if not pos or n < 3:
                return
            base = len(verts)
            for k in range(n):
                p = tuple(centre[c] + pos[k][c] / 32767.5 * scale[c] for c in range(3))
                nn = tuple(x / 127.0 for x in nrm[k]) if nrm else (0.0, 0.0, 1.0)
                uv = (uvs[k][0] / 127.5 * uv_scale[0], uvs[k][1] / 127.5 * uv_scale[1]) if uvs else (0.0, 0.0)
                verts.append((p, nn, uv))
            for k in range(2, n):
                a, b, c = base + k - 2, base + k - 1, base + k
                if k & 1:
                    a, b = b, a
                pa, pb, pc = verts[a][0], verts[b][0], verts[c][0]
                if pa == pb or pb == pc or pa == pc:
                    continue  # the degenerate triangles that join strips
                # winding from the vertex normals (strip parity is lost at batch joins)
                e1 = [pb[i] - pa[i] for i in range(3)]
                e2 = [pc[i] - pa[i] for i in range(3)]
                fn = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                vn = [verts[a][1][i] + verts[b][1][i] + verts[c][1][i] for i in range(3)]
                if sum(fn[i] * vn[i] for i in range(3)) < 0:
                    a, b = b, a
                tris.append((a, b, c, mat))

        while at < end:
            code = u32(m, at)
            cmd = code >> 24 & 0x7F
            at += 4
            if cmd & 0x60 == 0x60:  # UNPACK
                vn, vl = cmd >> 2 & 3, cmd & 3
                num = code >> 16 & 0xFF or 256
                addr = code & 0x3FF
                comps, bits = vn + 1, 32 >> vl
                size = (num * comps * bits + 31) // 32 * 4
                data = m[at:at + size]
                at += size
                if cmd == 0x6C:  # V4-32: the batch header (vertex count)
                    flush()
                    pos = nrm = uvs = None
                    n = 0
                elif cmd == 0x79 and addr == 1:  # V3-16 positions
                    pos = [struct.unpack_from("<3h", data, k * 6) for k in range(num)]
                    n = num
                elif cmd == 0x6A and addr == 0x47:  # V3-8 normals (signed)
                    nrm = [struct.unpack_from("<3b", data, k * 3) for k in range(num)]
                elif cmd == 0x76 and addr == 0x8D:  # V2-8 texture coordinates (unsigned)
                    uvs = [struct.unpack_from("<2B", data, k * 2) for k in range(num)]
            elif cmd == 0x20:  # STMASK
                at += 4
            elif cmd in (0x30, 0x31):  # STROW / STCOL
                at += 16
        flush()
    return verts, tris, mats


# ---- PS2 textures (XTX / "xet" lists) -------------------------------------------------------------------------------

def unswizzle8(buf, w, h):
    """one index a pixel, stored in PSMT8's swizzled order -> linear (the PS2 GS's PSMT8 page / block / column layout)"""
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            block = (y & ~0xF) * w + (x & ~0xF) * 2
            swap = (((y + 2) >> 2) & 1) * 4
            py = (((y & ~3) >> 1) + (y & 1)) & 7
            col = py * w * 2 + ((x + swap) & 7) * 4
            out[y * w + x] = buf[block + col + ((y >> 1) & 1) + ((x >> 2) & 2)]
    return bytes(out)


def decode_xet(x, game, head=0x20):
    """{name (lower case): (w, h, rgba bytes)} from a texture list: a chunk's (at +0x20), or one in GAME.DTZ's image
    (head: the list's address; its +8 links the textures' +8)"""
    out = {}
    if head == 0x20 and x[:4] != b"xet\0":
        return out
    link = u32(x, head + 8)
    guard = 0
    while link != head + 8 and guard < 512 and 0 < link < len(x):
        guard += 1
        t = link - 8
        name = cstr(x, t + 16, 32).lower()
        ras = u32(x, t)
        if game == "lcs":
            data, flags = u32(x, ras), u32(x, ras + 4)
        else:
            data, flags = u32(x, ras + 8), u32(x, ras + 12)
        w, h, depth = 1 << (flags & 63), 1 << (flags >> 6 & 63), flags >> 12 & 63
        rgba = bytearray(w * h * 4)
        # [bikehero] LCS's textures carry mipmaps (flags bits 20-23: the levels, one after the other) with the palette
        # AFTER the last level, and bit 24 + k marks level k as stored in PSMT8's swizzled order (4-bit ones too, one
        # index a pixel). Reading the palette at w * h and the texels as linear made every LCS vehicle texture noise in
        # Vice City (a Sanchez with static for a body). VCS's are one linear level, palette right after it.
        levels, swz = 1, False
        if game == "lcs":
            levels = max(1, flags >> 20 & 15)
            swz = bool(flags >> 24 & 1) and w >= 16 and h >= 4
        total = sum(max(1, w >> k) * max(1, h >> k) * depth // 8 for k in range(levels))
        if depth == 8 and data + total + 1024 <= len(x):
            pal = x[data + total:data + total + 1024]
            idx = unswizzle8(x[data:data + w * h], w, h) if swz else x[data:data + w * h]
            for i in range(w * h):
                v = idx[i]
                k = (v & ~0x18) | ((v & 0x08) << 1) | ((v & 0x10) >> 1)  # CSM1 order (as the converter writes it)
                c = pal[k * 4:k * 4 + 4]
                rgba[i * 4:i * 4 + 4] = bytes((c[0], c[1], c[2], min(255, c[3] * 2)))
        elif depth == 4 and data + total + 64 <= len(x):
            pal = x[data + total:data + total + 64]
            raw = x[data:data + w * h // 2]
            idx = bytes((raw[i // 2] >> ((i & 1) * 4)) & 15 for i in range(w * h))
            if swz:
                idx = unswizzle8(idx, w, h)
            for i in range(w * h):
                c = pal[idx[i] * 4:idx[i] * 4 + 4]
                rgba[i * 4:i * 4 + 4] = bytes((c[0], c[1], c[2], min(255, c[3] * 2)))
        else:
            link = u32(x, link)
            continue
        if name:
            out[name] = (w, h, bytes(rgba))
        link = u32(x, link)
    return out


# ---- PC RenderWare writers (what the converter reads: vcs_modconv.cpp readDffParts / readTxd) ------------------------

RW_VERSION = 0x1803FFFF  # 3.6.0.3 (no ambient / specular / diffuse in the geometry, as the converter reads it)


def chunk(kind, body):
    return struct.pack("<III", kind, len(body), RW_VERSION) + body


def write_txd(textures):
    natives = b""
    for name, (w, h, rgba) in sorted(textures.items()):
        px = bytearray(len(rgba))
        px[0::4], px[1::4], px[2::4], px[3::4] = rgba[2::4], rgba[1::4], rgba[0::4], rgba[3::4]  # BGRA
        st = struct.pack("<II", 8, 0x1106) + name.encode()[:31].ljust(32, b"\0") + b"".ljust(32, b"\0")
        st += struct.pack("<IIHHBBBB", 0x0500, 1, w, h, 32, 1, 4, 0) + struct.pack("<I", len(px)) + bytes(px)
        natives += chunk(0x15, chunk(0x1, st) + chunk(0x3, b""))
    return chunk(0x16, chunk(0x1, struct.pack("<HH", len(textures), 0)) + natives + chunk(0x3, b""))


def write_dff(frames, atomics):
    """frames: [(name, matrix 12 floats, parent)], atomics: [(frame, verts, tris, materials [(rgba, texture)])]"""
    fl = struct.pack("<I", len(frames))
    for name, mtx, parent in frames:
        fl += struct.pack("<12fiI", *mtx, parent, 0)
    exts = b"".join(chunk(0x3, chunk(0x253F2FE, name.encode())) for name, _, _ in frames)
    frame_list = chunk(0xE, chunk(0x1, fl) + exts)
    geoms = b""
    for _, verts, tris, mats in atomics:
        mats = mats or [((255, 255, 255, 255), "")]
        st = struct.pack("<HBBIII", 0x0074, 1, 0, len(tris), len(verts), 1)  # textured, normals, light, modulate
        st += b"".join(struct.pack("<2f", *v[2]) for v in verts)
        st += b"".join(struct.pack("<4H", b, a, mt, c) for a, b, c, mt in tris)  # RW: v1, v0, material, v2
        st += struct.pack("<4fII", 0, 0, 0, 10, 1, 1)
        st += b"".join(struct.pack("<3f", *v[0]) for v in verts)
        st += b"".join(struct.pack("<3f", *v[1]) for v in verts)
        ml = struct.pack("<I", len(mats)) + struct.pack("<i", -1) * len(mats)
        mbody = b""
        for rgba, tex in mats:
            ms = struct.pack("<I4BII", 0, *rgba, 0, 1 if tex else 0) + struct.pack("<3f", 1, 1, 1)
            t = b""
            if tex:
                t = chunk(0x6, chunk(0x1, struct.pack("<I", 0x1106)) + chunk(0x2, tex.encode() + b"\0") +
                          chunk(0x2, b"\0") + chunk(0x3, b""))
            mbody += chunk(0x7, chunk(0x1, ms) + t + chunk(0x3, b""))
        geoms += chunk(0xF, chunk(0x1, st) + chunk(0x8, chunk(0x1, ml) + mbody) + chunk(0x3, b""))
    geom_list = chunk(0x1A, chunk(0x1, struct.pack("<I", len(atomics))) + geoms)
    atoms = b""
    for k, (frame, _, _, _) in enumerate(atomics):
        atoms += chunk(0x14, chunk(0x1, struct.pack("<IIII", frame, k, 5, 0)) + chunk(0x3, b""))
    body = chunk(0x1, struct.pack("<III", len(atomics), 0, 0)) + frame_list + geom_list + atoms + chunk(0x3, b"")
    return chunk(0x10, body)


IDENTITY = (1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)


def vehicle_to_pc(disc, veh):
    """One vehicle of the other game -> (.dff bytes, .txd bytes, report lines)"""
    g = disc.g
    m = disc.blob(veh["id"])
    if m[:4] != b"ldm\0":
        raise ValueError("%s: no model chunk" % veh["name"])
    x = disc.blob(g["tex_base"] + veh["txd"])
    textures = decode_xet(x, disc.game)
    report = []
    shared_lists = {}

    def shared(name):
        """A texture the car takes from the game's resident vehicle textures (VCS: xv_vehiclegeneric_sl, the licence
        plates, the wheels' ...): the list in GAME.DTZ's image that holds a texture of that name"""
        if name in textures:
            return True
        b = disc.dtz
        at = b.find(name.encode() + b"\0")
        while at >= 16:
            t = at - 16
            head = u32(b, t + 4)
            if 0 < head < len(b):
                if head not in shared_lists:
                    try:
                        shared_lists[head] = decode_xet(b, disc.game, head)
                    except (struct.error, IndexError):
                        shared_lists[head] = {}
                if name in shared_lists[head]:
                    textures[name] = shared_lists[head][name]
                    return True
            at = b.find(name.encode() + b"\0", at + 1)
        return False
    # the paint: materials in the vehicle's primary / secondary lists take the PC marker colours (60,255,0 /
    # 255,0,175), so the converter makes them the target car's paint
    paint = {}
    for which, (off, count) in (("p", g["primary"]), ("s", g["secondary"])):
        for k in range(count):
            p = u32(m, off + 4 * k)
            if p:
                paint[p] = which
    group = u32(m, 0x20)
    frames, atomics = [], []
    names_seen = []

    def material(ptr):
        tex = cstr(m, u32(m, ptr), 32).lower() if u32(m, ptr) else ""
        rgba = tuple(m[ptr + 4:ptr + 8])
        if paint.get(ptr) and rgba[3] < 120:
            # a see-through material in a paint list (LCS's cars' glass): its own colour, not the paint marker (the
            # VCS converter left the marker green on an LCS Kuruma's windows: hidden run v2)
            paint.pop(ptr)
        if paint.get(ptr) == "p":
            rgba = (60, 255, 0, 255)
        elif paint.get(ptr) == "s":
            rgba = (255, 0, 175, 255)
        else:
            rgba = (rgba[0], rgba[1], rgba[2], min(255, rgba[3] * 2) if rgba[3] <= 128 else rgba[3])
        if tex and not shared(tex):
            report.append("texture %s not found" % tex)
            tex = ""
        return rgba, tex

    def add_atomic(frame, geom):
        verts, tris, mats = decode_geometry(m, geom)
        atomics.append((frame, verts, tris, [material(p) for p in mats]))

    def walk(n, parent, depth):
        guard = 0
        while n and guard < 256 and depth < 32:
            guard += 1
            nm_at = u32(m, n + g["node_name"])
            name = cstr(m, nm_at, 32).lower() if nm_at else ""
            if name.startswith("chassis_vlo") and "_" not in name[11:]:
                name = "chassis_vlo"  # VCS's SABRETB: "chassis_vlosjl" (LCS's chassis_vlo_headlight stays itself)
            mtx = struct.unpack_from("<3f", m, n + 0x10) + struct.unpack_from("<3f", m, n + 0x20) + \
                struct.unpack_from("<3f", m, n + 0x30) + struct.unpack_from("<3f", m, n + 0x40)
            if parent < 0:
                mtx = IDENTITY
            me = len(frames)
            frames.append((name, mtx, parent))
            names_seen.append(name)
            link, k = u32(m, n + 8), 0
            while link and link != n + 8 and k < 16:
                k += 1
                e = link - 8
                if m[e] == 1:
                    intact = (m[e + 2] & 4) != 0
                    geom = u32(m, e + 20)
                    if name.endswith("_dummy"):
                        part = name[:-6] + ("_hi_ok" if intact else "_hi_dam")
                        frames.append((part, IDENTITY, me))
                        add_atomic(len(frames) - 1, geom)
                    elif intact:  # chassis_hi, chassis_vlo, a bike's own part nodes, engine / dash ...
                        add_atomic(me, geom)
                link = u32(m, link)
            walk(u32(m, n + 0x90), me, depth + 1)
            n = u32(m, n + 0x94)
            if depth == 0:
                break

    walk(u32(m, group + 4), -1, 0)
    # the shared wheel (resident in GAME.DTZ: model info +0x28 -> its LOD atomics, the first is the detailed one),
    # scaled to the car's wheel scale, under wheel_rf_dummy: the converter takes it as the car's own wheel
    wheel = veh["wheel"]
    wp = disc.info(wheel) if wheel > 0 else 0
    rf = next((i for i, f in enumerate(frames) if f[0] == "wheel_rf_dummy"), -1)
    if wp and rf >= 0:
        b = disc.dtz
        lods = u32(b, wp + g["atoms"])
        atom = u32(b, lods) if lods else 0
        if atom and b[atom] == 1:
            verts, tris, mats = decode_geometry(b, u32(b, atom + 20))
            s = veh["wheel_scale"] or 0.7
            verts = [(tuple(c * s for c in p), nn, uv) for p, nn, uv in verts]
            wmats = []
            for p in mats:
                # a resident model's material points at its texture (the name at +16), not at a name
                tex = cstr(b, u32(b, p) + 16, 32).lower() if u32(b, p) else ""
                if tex and not shared(tex):
                    report.append("wheel texture %s not found: drawn untextured" % tex)
                    tex = ""
                wmats.append((tuple(b[p + 4:p + 8]), tex))
            frames.append(("wheel", IDENTITY, rf))
            atomics.append((len(frames) - 1, verts, tris, wmats))
            report.append("wheel: model %d, %d triangles, scale %.2f" % (wheel, len(tris), s))
    # the shared grunge (VCS xv_vehiclegrunge_sl: grey 160 with a band of dirt at the bottom) is the texture of every
    # paint material: the game's own cars draw it faint, the converted car drew its dirt as hard white / black speckles
    # (the Deluxo's sills, a PCJ-600 all over). Its contrast is halved around its grey.
    for tn in [t for t in textures if "grunge" in t]:
        w, h, px = textures[tn]
        soft = bytearray(px)
        for i in range(0, len(soft), 4):
            for k in range(3):
                soft[i + k] = max(0, min(255, 160 + (soft[i + k] - 160) // 2))
        textures[tn] = (w, h, bytes(soft))
        report.append("texture %s: contrast halved (dirt)" % tn)
    tri_count = sum(len(a[2]) for a in atomics)
    report.append("%s: %d frames (%s), %d parts, %d triangles, %d textures" % (
        veh["name"], len(frames), " ".join(n for n in names_seen if n), len(atomics), tri_count, len(textures)))
    return write_dff(frames, atomics), write_txd(textures), report


# ---- the pack ---------------------------------------------------------------------------------------------------------

def data_dir(game, override=None):
    """The game's data folder as the game picks it (backup.data_dir's rule, for either game in one process)"""
    if override:
        return override
    if os.environ.get("VCS_DATA_DIR"):
        return os.environ["VCS_DATA_DIR"]
    if sys.platform == "win32":
        base = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "GTAStoriesRecomp")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support/GTAStoriesRecomp")
    else:
        xdg = os.environ.get("XDG_DATA_HOME", "")
        base = os.path.join(xdg if xdg.startswith("/") else os.path.expanduser("~/.local/share"), "GTAStoriesRecomp")
    return os.path.join(base, game.upper())


ID_BASE = 100000  # src/game/lcs_crossover.h kIdBase


def bound_map(game, data=None):
    """{the other game's model id: this game's slot} of the packs in <data>/mods (stories_<src>_vehicles manifests,
    "bound"): the border handover's car map uses it so a crossing car stays that car (tools/stories.py)"""
    out = {}
    mods = os.path.join(data_dir(game, data), "mods")
    for src in GAMES:
        path = os.path.join(mods, "stories_%s_vehicles" % src, "manifest.json")
        try:
            with open(path, encoding="utf-8") as f:
                m = json.load(f)
        except (OSError, ValueError):
            continue
        if m.get("enabled", True):
            # every packed vehicle: bound by the game on demand (src/game/lcs_crossover.h: 100000 + its model id)
            out.update({int(v["id"]): ID_BASE + int(v["id"]) for v in m.get("vehicles", [])})
            out.update({int(k): int(v) for k, v in m.get("bound", {}).items()})  # --slot: a fixed slot
    return out


def vehicle_labels(game):
    """{model id: the label players know (mod_slots.txt column 3: "Deluxo", "Sabre Turbo")}"""
    out = {}
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modslots", "mod_slots.txt")
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^%s vehicle (\d+) \| (.*)$" % game, line.strip())
            if m:
                cols = [c.strip() for c in m.group(2).split("|")]
                if len(cols) > 1 and cols[1]:
                    out[int(m.group(1))] = cols[1]
    return out


def own_names(game):
    """The running game's vehicle names and aliases (tools/modslots/mod_slots.txt:
    `<game> vehicle <id> | <model name> | <label> | <class> | <names> | <similar>`)"""
    names = []
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modslots", "mod_slots.txt")
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.startswith(game + " vehicle "):
                continue
            cols = [c.strip() for c in line.split("|")]
            if len(cols) >= 5:
                names += [cols[1]] + cols[4].split()
    return [re.sub(r"[^a-z0-9]", "", n.lower()) for n in names]


def lcs_handling_fields(disc, veh):
    """[crossover] LCS's handling record as handling.cfg fields (the inverse of vcs_mods.cpp applyHandling with
    kLcsHandling): the model info's handling id (+0x42, a byte) into cHandlingDataMgr's records (GAME.DTZ header word
    0x22 = +0xA8 -> the manager; records from +0x20, 0xF0 each, the record's +0x10 its id)"""
    b = disc.dtz
    mgr = u32(b, 0xA8)
    hid = b[disc.info(veh["id"]) + 0x42]
    r = mgr + 0x20 + hid * 0xF0
    if not 0 < r < len(b) - 0xF0 or u32(b, r + 0x10) != hid:
        return {}
    f = lambda o: f32(b, r + o)  # noqa: E731
    drive = chr(b[r + 0x80]) if 32 < b[r + 0x80] < 127 else "R"
    out = {"mass": f(0x00), "turnmass": f(0x04), "comx": f(0x20), "comy": f(0x24), "comz": f(0x28),
           "submerged": u32(b, r + 0x30), "tractionmult": f(0x08), "tractionloss": f(0xAC), "tractionbias": f(0xB0),
           "gears": b[r + 0x82], "maxspeed": f(0x8C) * 180.0,
           "accel": f(0x84) * 2500.0 * (4.0 if drive == "4" else 2.0), "brake": f(0x9C) * 2500.0,
           "brakebias": f(0xA0), "steer": f(0xA8), "suspforce": f(0xB8), "suspdamp": f(0xBC), "suspupper": f(0xC0),
           "susplower": f(0xC4), "suspbias": f(0xC8), "antidive": f(0xCC), "seatoffset": f(0xD4),
           "value": u32(b, r + 0xD8)}
    if not (50 <= out["mass"] <= 50000 and 1 <= out["gears"] <= 5):
        return {}
    return {k: (round(v, 5) if isinstance(v, float) else v) for k, v in out.items()}


def handling_fields(disc, veh):
    """The vehicle's handling record as handling.cfg-unit fields of the mod loader's `handling` directive (the inverse
    of vcs_mods.cpp applyHandling). VCS only for now: its record (model info +0x38 -> the gear table first; field map
    docs/ANDROID.md "Mods") - DELUXO reads mass 1600, accel 0.002 (x 2500 x 2 = 10), max velocity 0.973 (x 180 = 175
    km/h), brake 0.004 (x 2500 = 10), steering 35."""
    if disc.game == "lcs":
        return lcs_handling_fields(disc, veh)
    b = disc.dtz
    r = u32(b, disc.info(veh["id"]) + 0x38)
    if not 0 < r < len(b) - 0xE0:
        return {}
    f = lambda o: f32(b, r + o)  # noqa: E731
    drive = chr(b[r + 0x5C]) if 32 < b[r + 0x5C] < 127 else "R"
    out = {"mass": f(0xBC), "turnmass": f(0xC0), "comx": f(0xA0), "comy": f(0xA4), "comz": f(0xA8),
           "submerged": u32(b, r + 0xB0), "tractionmult": f(0xC4), "tractionloss": f(0x6C), "tractionbias": f(0x70),
           "gears": b[r + 0x5E], "maxspeed": f(0x50) * 180.0,
           "accel": f(0x48) * 2500.0 * (4.0 if drive == "4" else 2.0), "brake": f(0x60) * 2500.0,
           "brakebias": f(0x64), "steer": f(0x68), "suspforce": f(0x74), "suspdamp": f(0x78), "suspupper": f(0x7C),
           "susplower": f(0x80), "suspbias": f(0x84), "antidive": f(0x88), "seatoffset": f(0x8C),
           "value": u32(b, r + 0x90)}
    if not (50 <= out["mass"] <= 50000 and 1 <= out["gears"] <= 5):
        return {}
    return {k: (round(v, 5) if isinstance(v, float) else v) for k, v in out.items()}


def palette(disc):
    """The game's car colour palette, [(r, g, b)]. VCS: RGB triples in GAME.DTZ, found by its first three colours
    (15,15,15 / 188,188,180 / 42,119,161; 102 of them, then zeros: image 0x175080 in the user's disc, 2026-11-18). LCS:
    header +0x78 -> 4 bytes, then RGBA (vcs_mods.cpp applyColours: *0x3D7388 + 4 in a run; 101 colours)."""
    b = disc.dtz
    if disc.game == "vcs":
        at = b.find(bytes([15, 15, 15, 188, 188, 180, 42, 119, 161]))
        if at < 0:
            return []
        out = []
        while len(out) < 128 and b[at + 3 * len(out):at + 3 * len(out) + 3] != b"\0\0\0":
            out.append(tuple(b[at + 3 * len(out):at + 3 * len(out) + 3]))
        return out
    at = u32(b, 0x78) + 4
    return [tuple(b[at + 4 * i:at + 4 * i + 3]) for i in range(101)] if 0 < at < len(b) - 404 else []


def colour_pairs(disc, veh, pal):
    """The vehicle's paint colour pairs as RGB (the mod loader's `colours` directive: pair<n> r,g,b,r,g,b). VCS model
    info +0x161 the count, +0x163 {primary, secondary} index pairs (ADMIRAL 8: 10,10 74,74 ...; CHEETAH 54,1 14,1 ...;
    DELUXO 7; read 2026-11-18); LCS +0x170 8 primary, +0x178 8 secondary, +0x180 the count."""
    b, p = disc.dtz, disc.info(veh["id"])
    if not p or not pal:
        return []
    if disc.game == "vcs":
        n = b[p + 0x161]
        idx = [(b[p + 0x163 + 2 * k], b[p + 0x164 + 2 * k]) for k in range(min(n, 8))]
    else:
        n = b[p + 0x180]
        idx = [(b[p + 0x170 + k], b[p + 0x178 + k]) for k in range(min(n, 8))]
    return [pal[a] + pal[c] for a, c in idx if a < len(pal) and c < len(pal)]


def cmd_list(iso):
    d = Disc(iso)
    print("%s: %d model infos" % (d.game, d.count))
    for v in d.vehicles():
        print("%4d %-10s %-5s wheel %4d x %.2f" % (v["id"], v["name"], v["class"], v["wheel"], v["wheel_scale"]))


def cmd_dump(iso, name, out):
    d = Disc(iso)
    v = d.find(name)
    if not v:
        raise SystemExit("error: no vehicle %s" % name)
    dff, txd, report = vehicle_to_pc(d, v)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, v["name"].lower() + ".dff"), "wb") as f:
        f.write(dff)
    with open(os.path.join(out, v["name"].lower() + ".txd"), "wb") as f:
        f.write(txd)
    print("\n".join(report))


def cmd_build(iso, game, data, wanted, slots, auto=False):
    d = Disc(iso)
    if d.game == game:
        raise SystemExit("error: that is %s's own disc; give the OTHER game's" % game)
    mod_id = "stories_%s_vehicles" % d.game
    mods = os.path.join(data_dir(game, data), "mods")
    folder = os.path.join(mods, mod_id)
    os.makedirs(os.path.join(folder, "files"), exist_ok=True)
    vehicles = [v for v in d.vehicles() if v["class"] in ("car", "bike", "boat", "heli", "plane")]
    # LCS's helicopters (HUNTER, MAVERIC, POLMAV, VCNMAV 213-216, VC leftovers) and DODO are class 0 (car) in their
    # model infos - automobiles with a helicopter / plane handling record; the mod slot table's class says what they are
    # (the other game takes an aircraft donor for them)
    import stories
    table = stories.load_vehicles().get(d.game, {})
    for v in vehicles:
        if v["class"] == "car" and table.get(v["id"], {}).get("class") in ("heli", "plane"):
            v["class"] = table[v["id"]]["class"]
    if wanted:
        names = {w.lower() for w in wanted}
        vehicles = [v for v in vehicles if v["name"].lower() in names]
    files, entries, index, bound = [], [], [], {}
    pal = palette(d)
    if auto:  # the other game's vehicles without a namesake here, in order, onto the spare slots
        own = {n.lower() for n in own_names(game)}
        free = [s for s in SPARE_SLOTS.get(game, []) if s not in slots.values()]
        for v in vehicles:
            if v["class"] == "car" and re.sub(r"[^a-z0-9]", "", v["name"].lower()) not in own and v["name"].upper() not in slots and free:
                slots[v["name"].upper()] = free.pop(0)
    for v in vehicles:
        try:
            dff, txd, report = vehicle_to_pc(d, v)
        except (ValueError, struct.error, IndexError) as e:
            print("[pack] %s: skipped (%s)" % (v["name"], e))
            continue
        base = v["name"].lower()
        for ext, data_ in (("dff", dff), ("txd", txd)):
            with open(os.path.join(folder, "files", base + "." + ext), "wb") as f:
                f.write(data_)
            files.append({"name": base + "." + ext})
        hand = handling_fields(d, v)
        index.append({"name": v["name"], "id": v["id"], "class": v["class"], "dff": base + ".dff", "txd": base + ".txd",
                      "wheel_scale": v["wheel_scale"], "handling": hand, "colours": colour_pairs(d, v, pal)})
        print("[pack] " + "; ".join(report))
        if v["name"].upper() in slots:
            slot = slots[v["name"].upper()]
            entries.append({"kind": "model", "target": slot,
                            "fields": {"dff": base + ".dff", "txd": base + ".txd"}, "source": v["name"]})
            if hand:
                entries.append({"kind": "handling", "target": slot, "fields": hand, "source": v["name"]})
            bound[v["id"]] = slot
    manifest = {"id": mod_id, "name": "%s vehicles (from your %s disc)" % (
        "Vice City Stories" if d.game == "vcs" else "Liberty City Stories", d.game.upper()),
        "game": game, "kind": "stories_pack", "from": d.game, "imported": int(time.time()), "enabled": True,
        "storage": "copy", "sources": [], "files": files, "entries": entries, "vehicles": index,
        # the other game's model id -> this game's slot that holds it (tools/stories.py: the border handover's car map)
        "bound": {str(k): s for k, s in bound.items()}}
    with open(os.path.join(folder, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    with open(os.path.join(folder, "vehicles.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=2)
    # vehicles.txt: the same for the game (src/game/lcs_crossover.cpp: the trainer's list, spawning, the border), one per
    # line, tab-separated: other game's model id, class, label, dff, txd (paths under mods/), handling fields "k v ...",
    # the donor (below), the paint colour pairs "r,g,b,r,g,b ..." (the other game's palette)
    labels = vehicle_labels(d.game)
    # + the donor: the running game's nearest vehicle of the same class (tools/stories.py nearest_map), whose stock
    # model a packed vehicle is converted on and whose model info / handling record its new model id starts from
    import stories
    nearest = stories.nearest_map(d.game, game)
    with open(os.path.join(folder, "vehicles.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# stories_pack.py: id, class, label, dff, txd, handling, donor, colours (tab-separated)\n")
        for v in index:
            hand = " ".join("%s %s" % kv for kv in v.get("handling", {}).items())
            f.write("\t".join([str(v["id"]), v["class"], labels.get(v["id"], v["name"].title()),
                                 "%s/files/%s" % (mod_id, v["dff"]), "%s/files/%s" % (mod_id, v["txd"]), hand,
                                 str(nearest.get(v["id"], 0)),
                                 " ".join(",".join(str(c) for c in pair) for pair in v.get("colours", []))]) + "\n")
    # active.txt through the mod manager (its other mods kept)
    os.environ["VCS_TOOLS_GAME"] = game
    import mods as mm
    store = mm.Store(game, os.path.dirname(mods))
    store.write_active()
    print("[pack] %d vehicles in %s (%d bound to slots)" % (len(index), folder, len(entries)))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("iso")
    p = sub.add_parser("dump")
    p.add_argument("iso")
    p.add_argument("name")
    p.add_argument("out")
    p = sub.add_parser("build")
    p.add_argument("iso")
    p.add_argument("--game", required=True, choices=("lcs", "vcs"))
    p.add_argument("--data")
    p.add_argument("--vehicles")
    p.add_argument("--slot", action="append", default=[], help="NAME=MODEL_ID")
    p.add_argument("--auto", action="store_true", help="bind the other game's cars without a namesake here to the "
                   "spare slots (SPARE_SLOTS)")
    import stories_swim
    stories_swim.add_parser(sub)
    import stories_hero
    stories_hero.add_parser(sub)
    a = ap.parse_args(argv)
    if a.cmd == "swim":
        sys.exit(stories_swim.run(a))
    if a.cmd == "hero":
        sys.exit(stories_hero.run(a))
    if a.cmd == "list":
        cmd_list(a.iso)
    elif a.cmd == "dump":
        cmd_dump(a.iso, a.name, a.out)
    else:
        slots = {}
        for s in a.slot:
            n, _, i = s.partition("=")
            slots[n.upper()] = int(i)
        cmd_build(a.iso, a.game, a.data, a.vehicles.split(",") if a.vehicles else None, slots, a.auto)


if __name__ == "__main__":
    main(sys.argv[1:])
