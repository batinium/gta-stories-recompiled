#!/usr/bin/env python3
"""The "hero" kind of tools/stories_pack.py (docs/STORIES.md "One hero in both cities"): the other game's hero, all
his outfits, read once from the player's own disc and translated into this game's model dialect, written as a local
mod. Standard library only. Nothing here is game data: the pack is made on the player's computer from the player's
disc, lives in the game's data folder (<data>/mods/stories_<from>_hero/) and is never committed or shipped
(.gitignore and tools/prepublish_check.py refuse stories_* mod folders and *.mdl / *.xtx).

    python tools/stories_pack.py hero <VCS iso> --game lcs [--data DIR] [--json] [--force] [--dump OUTDIR]
        Vic Vance for Liberty City: mod stories_vcs_hero
    python tools/stories_pack.py hero <LCS iso> --game vcs [...]
        Toni Cipriani for Vice City: mod stories_lcs_hero
  files/hero_<outfit>.mdl / .xtx for each of the hero's outfits, hero.mdl / .xtx (his default), outfits.txt (this
  game's outfit name -> the hero's outfit files: VIC_FOR_LCS / TONI_FOR_VCS below); active.txt gets
  `hero name=vic|toni mdl=... xtx=... outfits=...`, which src/game/stories_hero.cpp serves in place of the game's own
  hero when the "Stories: hero" option (STORIES_HERO) asks for him. --dump: the files only, into OUTDIR.

Idempotent and versioned: a pack already made from the same disc and this tool's text with the same PACK_VERSION is
left alone (--force makes it again); the launcher runs it before Play (stories_pack.AUTO_PACKS).

The model format (found 2026-11-18/19 by comparing both games' player chunks; aap's storiesconv `rsl.h` was read as a
format reference, none of its code is used): a model chunk is a relocatable "ldm" chunk (0x20-byte header: ident,
0, file size, data size, relocation table, count, global table, function count << 16; every pointer an offset from
the chunk's start, listed in the relocation table). A ped's chunk holds {+0x20 collision model, +0x24 element group}.
Both games use the same element group, element, geometry, material, skin and VIF / DMA layout (skinned batches:
positions V3-16 @1, UVs V2-8 @0x55, normals V3-8 @0x2B, weights V4-32 @0x7F with the bone index x 4 in the low byte,
MSCAL 6), so the geometry is copied as it is. What differs:
    node (frame)       VCS 0xB0: +0x9C 0, +0xA0 0, +0xA4 anim tree, +0xA8 name; no bone ids; header 0x0180AA00
                       LCS 0xB0: +0x9C bone id, +0xA0 anim tree, +0xA4 name, +0xA8 0; header 0x0C000000
    anim tree          VCS 0x38: +0x28 matrices (made at run time), +0x30 node infos, +0x34 root node
                       LCS 0x60: +0x28 matrices (in the chunk: 0x40 per node), +0x30 node infos, +0x34 root node,
                       +0x38 / +0x3C key frame size 0x24, +0x50 the tree itself
    node infos         8 bytes {bone id, index, flags (1 pop, 2 push), pad, frame}: the same
    skeleton           VCS 25 bones (a jaw, 6, after the head; toes 255), LCS 24 (toes 54 / 55). Both games'
                       animations reach the bones BY PLACE: the skeleton is rebuilt as the target game's own, node
                       for node (the jaw left out for LCS - its vertices to the head - or put in for VCS)
    collision model    VCS +0x30 boxes, +0x32 triangles, +0x34 spheres (s16), +0x38.. pointers
                       LCS +0x30 spheres, +0x32 boxes, +0x34 triangles (s16), +0x3C.. pointers
    element / group    the object header bytes (element LCS 0x01050001 / VCS 0x0004AA01); the element hangs from the
                       skeleton's root node in LCS, from a node of its own beside the skeleton in VCS
    skin               LCS fills "used bones" (= bones), VCS 0
Textures ("xet" chunks): VCS's raster is 16 bytes {0, 0x450000 | w / 2 (4-bit) or 0x250000 | w (8-bit), texels,
flags}, its texture 0x60 bytes with +0x50 = 1, texels from +0x50, one linear level each; LCS's raster 8 bytes {texels,
flags}, its texture 0x54 (+0x50 = 1), texels from +0x40, mipmapped (flags bits 20-23 the levels, the palette after
the last, bit 24 + k: level k swizzled in PSMT8's order, 4-bit ones too over one index a pixel). Vic's linear
textures work in LCS as they are; Toni's go to VCS as their first level, unswizzled.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PACK_VERSION = 6  # 6: VCS's 0x60-byte textures; 4: a jaw for Toni in VCS; 2: every outfit (outfits.txt); 3: LCS's swizzled, mipmapped textures
HERO_FILES = ("hero.mdl", "hero.xtx")

# bone ids by VCS node name (its player's skeleton; pNodeInfo order checked against them)
VCS_BONES = {"root": 0, "pelvis": 1, "spine": 2, "spine1": 3, "neck": 4, "head": 5, "jaw": 6,
             "bip01_r_clavicle": 21, "r_upperarm": 22, "r_forearm": 23, "r_hand": 24, "r_finger": 25,
             "bip01_l_clavicle": 31, "l_upperarm": 32, "l_forearm": 33, "l_hand": 34, "l_finger": 35,
             "l_thigh": 41, "l_calf": 42, "l_foot": 43, "l_toe0": 54,
             "r_thigh": 51, "r_calf": 52, "r_foot": 53, "r_toe0": 55}
# LCS's node names for those ids (as in its own player chunk)
LCS_NAMES = {0: "Root", 1: "Pelvis", 2: "Spine", 3: "Spine1", 4: "Neck", 5: "Head", 6: "Jaw",
             21: "Bip01 R Clavicle", 22: "R UpperArm", 23: "R Forearm", 24: "R Hand", 25: "R Finger",
             31: "Bip01 L Clavicle", 32: "L UpperArm", 33: "L Forearm", 34: "L Hand", 35: "L Finger",
             41: "L Thigh", 42: "L Calf", 43: "L Foot", 54: "L Toe0",
             51: "R Thigh", 52: "R Calf", 53: "R Foot", 55: "R Toe0"}


# LCS's ped top node modelling matrix (its player's and other peds': rows right, up, at, pos; w = flags)
LCS_TOP = struct.pack("<12f", -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0) + struct.pack("<3fI", 0.0, 0.982, -0.022, 1)

LCS_ROOT = struct.pack("<12f", 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0) + struct.pack("<3fI", 0.0, 0.0, 0.0, 1)


class PackError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def cstr(b, o, n=32):
    if o <= 0 or o >= len(b):
        return ""
    return bytes(b[o:o + n]).split(b"\0")[0].decode("latin1")


class Chunk:
    """A relocatable chunk being rebuilt: data bytes, the set of pointer fields, the global table entries"""

    def __init__(self, src):
        if src[:4] not in (b"ldm\0", b"xet\0"):
            raise PackError("not a model / texture chunk")
        self.ident = u32(src, 0)
        data_size, rel, n = u32(src, 12), u32(src, 16), u32(src, 20)
        glob, nfun = u32(src, 24), u32(src, 28) >> 16
        self.b = bytearray(src[:data_size])
        self.relocs = set()
        self.globals = [u32(src, glob + 4 * k) for k in range(nfun)] if glob else []
        tail_at = rel + 4 * n  # the strings after the relocation table (material names): moved into the data
        tail = bytes(src[tail_at:u32(src, 8)])
        self.align(16)
        tail_new = len(self.b)
        self.b += tail
        glob_end = glob + 4 * nfun if glob else 0
        for k in range(n):
            at = u32(src, rel + 4 * k)
            if glob and glob <= at < glob_end:
                continue  # the global table's own entries: written again at the end
            if at >= data_size:
                raise PackError("a relocated field outside the data (0x%x)" % at)
            v = u32(self.b, at)
            if v >= tail_at:
                struct.pack_into("<I", self.b, at, v - tail_at + tail_new)
            self.relocs.add(at)

    def align(self, n):
        while len(self.b) % n:
            self.b.append(0)

    def reserve(self, size, align=16):
        self.align(align)
        at = len(self.b)
        self.b += bytes(size)
        return at

    def put(self, at, v):
        struct.pack_into("<I", self.b, at, v & 0xFFFFFFFF)

    def ptr(self, at, v):
        self.put(at, v)
        if v:
            self.relocs.add(at)
        else:
            self.relocs.discard(at)

    def string(self, s):
        at = len(self.b)
        self.b += s.encode("latin1") + b"\0"
        return at

    def finish(self):
        self.align(16)
        data_size = len(self.b)
        glob = 0
        if self.globals:
            glob = len(self.b)
            for g in self.globals:
                self.relocs.add(len(self.b))
                self.b += struct.pack("<I", g)
        rel = len(self.b)
        relocs = sorted(self.relocs)
        for r in relocs:
            self.b += struct.pack("<I", r)
        struct.pack_into("<8I", self.b, 0, self.ident, 0, len(self.b), data_size, rel, len(relocs), glob,
                         len(self.globals) << 16)
        return bytes(self.b)


# ---- VCS -> LCS ----------------------------------------------------------------------------------------------------

def vcs_nodes(m, top):
    """[(offset, name, parent index)] depth-first in the chunk's sibling order"""
    out = []

    def walk(n, parent, depth):
        guard = 0
        while n and guard < 64 and depth < 32:
            guard += 1
            out.append((n, cstr(m, u32(m, n + 0xA8), 32).lower(), parent))
            walk(u32(m, n + 0x90), len(out) - 1, depth + 1)
            n = u32(m, n + 0x94)
    walk(top, -1, 0)
    return out


def remap_weights(m, geom, remap):
    """the bone index in every skinned vertex weight (V4-32 at VU 0x7F: low byte = index x 4) through `remap`"""
    ps2 = geom + 0x20
    dma = ps2 + u16(m, ps2 + 26)
    for s in range(u32(m, ps2 + 16) >> 20):
        tag = dma + u32(m, ps2 + 0x40 + s * 0x30 + 28)
        end = tag + 16 + (u32(m, tag) & 0xFFFF) * 16
        at = tag + 8
        while at < end:
            code = u32(m, at)
            cmd = code >> 24 & 0x7F
            at += 4
            if cmd & 0x60 == 0x60:
                vn, vl = cmd >> 2 & 3, cmd & 3
                num = code >> 16 & 0xFF or 256
                size = (num * (vn + 1) * (32 >> vl) + 31) // 32 * 4
                if cmd == 0x6C and code & 0x3FF == 0x7F:
                    for k in range(num * 4):
                        w = u32(m, at + 4 * k)
                        if w & 0xFF:
                            idx = (w & 0xFF) >> 2
                            struct.pack_into("<I", m, at + 4 * k, (w & ~0xFF) | remap.get(idx, idx) << 2)
                at += size
            elif cmd == 0x20:
                at += 4
            elif cmd in (0x30, 0x31):
                at += 16


def ped_vcs_to_lcs(src):
    """VCS's ped model chunk -> LCS's dialect (bytes, report lines)"""
    c = Chunk(src)
    m = c.b
    group = u32(m, 0x24)
    if m[group] != 2:
        raise PackError("the model is not an element group")
    top = u32(m, group + 4)
    elem_link = u32(m, group + 8)
    if elem_link == group + 8 or u32(m, elem_link) != group + 8:
        raise PackError("the model has not exactly one element")
    elem = elem_link - 0x1C
    if m[elem] != 1:
        raise PackError("no element in the group")
    tree = u32(m, elem + 0x2C)
    nodes = vcs_nodes(m, top)
    holder = u32(m, elem + 4)  # the node the element hangs from (VCS "male_base01": dropped)
    rootk = next((k for k, (n, _, _) in enumerate(nodes) if u32(m, n + 0xA4) == tree), -1)
    if rootk < 0 or not tree:
        raise PackError("no skeleton (anim tree) in the model")
    keep = [k for k, (n, _, _) in enumerate(nodes) if n != holder or k in (0, rootk)]
    report = []
    # bone ids: by name, checked against the tree's node infos in their (children reversed) depth-first order
    ninfo, count = u32(m, tree + 0x30), u32(m, tree + 4)
    order = []

    def rev_dfs(k):
        order.append(k)
        kids = [j for j in keep if nodes[j][2] == k]
        for j in reversed(kids):
            rev_dfs(j)
    rev_dfs(rootk)
    if len(order) != count:
        raise PackError("skeleton has %d nodes, its anim tree %d" % (len(order), count))
    ids = {}
    for i, k in enumerate(order):
        name = nodes[k][1]
        if name not in VCS_BONES:
            raise PackError("unknown bone %r" % name)
        bid = VCS_BONES[name]
        tid = m[ninfo + 8 * i]
        if tid != 255 and tid != bid:
            raise PackError("bone %r: id %d in the tree, %d expected" % (name, tid, bid))
        ids[k] = bid
    # Bones LCS's skeleton lacks (VCS's jaw) are left out and their vertices go to the parent: LCS's animations
    # reach the bones by their place in the skeleton (a run with the jaw kept moved every bone after it: legs up),
    # so the skeleton must be LCS's own, node for node: 24 bones in its order.
    flags = [m[ninfo + 8 * i + 2] for i in range(count)]
    parent_of = {}
    stack, cur = [], -1
    for i in range(count):
        parent_of[i] = cur
        f = flags[i] & 3
        if f == 1:
            cur = stack.pop() if stack else -1
        elif f == 0:
            cur = i
        elif f == 2:
            stack.append(cur)
            cur = i
    drop = [i for i, k in enumerate(order) if ids[k] not in LCS_NAMES or ids[k] == 6]
    for i in drop:
        kids = [j for j in range(count) if parent_of[j] == i]
        if kids or not flags[i] & 1:
            raise PackError("bone %d (%s) can't be left out" % (ids[order[i]], nodes[order[i]][1]))
        p = parent_of[i]
        if p >= 0 and [j for j in range(count) if parent_of[j] == p] == [i]:
            flags[p] |= 1  # its parent is now a leaf
    remap, newi = {}, 0
    for i in range(count):
        if i in drop:
            continue
        remap[i] = newi
        newi += 1
    for i in drop:
        remap[i] = remap[parent_of[i]]
    dropped_nodes = [order[i] for i in drop]
    keep = [k for k in keep if k not in dropped_nodes]
    order = [k for i, k in enumerate(order) if i not in drop]
    old_count, count = count, len(order)
    nin = c.reserve(8 * count)
    for i in range(old_count):
        if i in drop:
            continue
        j = remap[i]
        m[nin + 8 * j] = ids[order[j]]
        m[nin + 8 * j + 1] = j
        m[nin + 8 * j + 2] = flags[i]
        m[nin + 8 * j + 3] = 0
    ninfo = nin
    if drop:
        report.append("left out: %s (its vertices to its parent)" % ", ".join(nodes[k][1] for k in dropped_nodes))
    # new nodes (LCS 0xB0)
    new = {k: c.reserve(0xB0) for k in keep}
    names = {k: c.string(LCS_NAMES[ids[k]]) for k in keep if k in ids}
    for k in keep:
        n, name, parent = nodes[k]
        at = new[k]
        while parent >= 0 and parent not in new:
            parent = nodes[parent][2]
        c.put(at, 0x03000000 if k == 0 else 0x0C000000)
        c.ptr(at + 4, new[parent] if parent >= 0 else 0)
        lst = elem + 8 if k == rootk else at + 8
        c.ptr(at + 8, lst)
        c.ptr(at + 12, lst)
        m[at + 0x10:at + 0x90] = m[n + 0x10:n + 0x90]
        if k == 0:  # LCS's top node: turned 180 degrees about y (every LCS ped's; VCS's scene_root is identity)
            m[at + 0x10:at + 0x50] = LCS_TOP
        elif k == rootk:  # LCS's root bone frame (its animations leave the root's rotation to it)
            m[at + 0x10:at + 0x50] = LCS_ROOT
        kids = [j for j in keep if nodes[j][2] == k]
        c.ptr(at + 0x90, new[kids[0]] if kids else 0)
        sib = [j for j in keep if nodes[j][2] == nodes[k][2] and j > k]
        c.ptr(at + 0x94, new[sib[0]] if sib and parent >= 0 else 0)
        c.ptr(at + 0x98, new[0])
        c.put(at + 0x9C, ids.get(k, -1))
        c.ptr(at + 0xA0, 0)  # the tree, below
        c.ptr(at + 0xA4, names.get(k, 0))
        c.put(at + 0xA8, 0)
    # the anim tree (LCS 0x60) and its matrices
    t = c.reserve(0x60)
    m[t:t + 0x28] = m[tree:tree + 0x28]
    c.put(t + 4, count)
    mats = c.reserve(0x40 * count, 64)
    c.ptr(t + 0x28, mats)
    c.put(t + 0x2C, 0)
    c.ptr(t + 0x30, ninfo)
    c.ptr(t + 0x34, new[rootk])
    c.put(t + 0x38, 0x24)
    c.put(t + 0x3C, 0x24)
    c.ptr(t + 0x50, t)
    c.ptr(new[rootk] + 0xA0, t)
    for k in range(count):
        c.ptr(ninfo + 8 * k + 4, 0)
    # the element and the group
    c.put(elem, 0x01050001)
    c.ptr(elem + 4, new[rootk])
    c.ptr(elem + 8, new[rootk] + 8)
    c.ptr(elem + 12, new[rootk] + 8)
    c.put(elem + 0x10, 0)
    c.ptr(elem + 0x2C, t)
    c.put(group, 0x00000002)
    c.ptr(group + 4, new[0])
    # geometry: skin "used bones", strip headers +24
    geom = u32(m, elem + 0x14)
    skin = u32(m, geom + 0x18)
    if skin:
        if u32(m, skin) != old_count:
            raise PackError("skin has %d bones, skeleton %d" % (u32(m, skin), old_count))
        inv_old = u32(m, skin + 12)
        inv = c.reserve(0x40 * count)
        for i in range(old_count):
            if i not in drop:
                m[inv + 0x40 * remap[i]:inv + 0x40 * remap[i] + 0x40] = m[inv_old + 0x40 * i:inv_old + 0x40 * i + 0x40]
        c.ptr(skin + 12, inv)
        c.put(skin, count)
        c.put(skin + 4, count)
    remap_weights(m, geom, remap)
    ps2 = geom + 0x20
    strips = u32(m, ps2 + 16) >> 20
    for s in range(strips):
        c.put(ps2 + 0x40 + s * 0x30 + 24, 0xFFFFFFF8)
    # collision model (LCS layout)
    col = u32(m, 0x20)
    if col:
        nc = c.reserve(0x60)
        m[nc:nc + 0x30] = m[col:col + 0x30]
        boxes, tris, spheres = struct.unpack_from("<hhh", m, col + 0x30)
        struct.pack_into("<hhhBB", m, nc + 0x30, spheres, boxes, tris, 0, m[col + 0x36])
        m[nc + 0x38:nc + 0x3C] = b"\x00\x80\x1c\xc0"  # as LCS's own player
        for k in range(7):
            c.ptr(nc + 0x3C + 4 * k, u32(m, col + 0x38 + 4 * k) if (col + 0x38 + 4 * k) in c.relocs else 0)
        c.ptr(0x20, nc)
    report.append("model: %d bones (ids %s), %d strips, %d bytes" % (
        count, ",".join(str(ids[k]) for k in order), strips, len(c.b)))
    return c.finish(), report


def xtx_vcs_to_lcs(src):
    """VCS's texture list chunk -> LCS's dialect (bytes, report lines)"""
    if src[:4] != b"xet\0":
        raise PackError("not a texture list")
    texs = []
    link, guard = u32(src, 0x28), 0
    while link != 0x28 and guard < 64:
        guard += 1
        t = link - 8
        ras = u32(src, t)
        data, flags = u32(src, ras + 8), u32(src, ras + 12)
        w, h, d = 1 << (flags & 63), 1 << (flags >> 6 & 63), flags >> 12 & 63
        if flags >> 20 & 15 != 1:
            raise PackError("texture %s has mipmaps (flags %08x)" % (cstr(src, t + 16), flags))
        size = w * h * d // 8 + (64 if d == 4 else 1024 if d == 8 else 0)
        texs.append((bytes(src[t + 16:t + 48]), bytes(src[data:data + size]), flags))
        link = u32(src, link)
    out = bytearray(0x40)
    relocs = []
    lst = 0x20
    out[lst] = 6
    struct.pack_into("<I", out, lst + 16, 1)
    placed = []
    for name, px, flags in texs:
        while len(out) % 16:
            out.append(0)
        placed.append(len(out))
        out += px
    while len(out) % 16:
        out.append(0)
    tex_at = []
    for _ in texs:
        tex_at.append(len(out))
        out += bytes(0x54)
        struct.pack_into("<I", out, tex_at[-1] + 0x50, 1)
    ras_at = []
    for _ in texs:
        ras_at.append(len(out))
        out += bytes(8)

    def ptr(at, v):
        struct.pack_into("<I", out, at, v)
        relocs.append(at)
    n = len(texs)
    for i, (name, px, flags) in enumerate(texs):
        t = tex_at[i]
        ptr(t, ras_at[i])
        ptr(t + 4, lst)
        ptr(t + 8, tex_at[i + 1] + 8 if i + 1 < n else lst + 8)
        ptr(t + 12, tex_at[i - 1] + 8 if i > 0 else lst + 8)
        out[t + 16:t + 48] = name
        ptr(ras_at[i], placed[i])
        struct.pack_into("<I", out, ras_at[i] + 4, flags)
    ptr(lst + 8, tex_at[0] + 8 if n else lst + 8)
    ptr(lst + 12, tex_at[-1] + 8 if n else lst + 8)
    while len(out) % 16:
        out.append(0)
    data_size = len(out)
    rel = len(out)
    for r in sorted(relocs):
        out += struct.pack("<I", r)
    struct.pack_into("<8I", out, 0, 0x00746578, 0, len(out), data_size, rel, len(relocs), 0, 0)
    return bytes(out), ["textures: " + ", ".join("%s %dx%d/%d" % (nm.split(b"\0")[0].decode("latin1"), 1 << (f & 63),
                                                                    1 << (f >> 6 & 63), f >> 12 & 63)
                                                  for nm, _, f in texs)]


# ---- LCS -> VCS ----------------------------------------------------------------------------------------------------

VCS_NAMES = {v: k for k, v in VCS_BONES.items()}
VCS_SCENE_ROOT = struct.pack("<12f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0) + struct.pack("<3fI", 0.0, 0.0, 0.0, 1)


def lcs_nodes(m, top):
    out = []

    def walk(n, parent, depth):
        guard = 0
        while n and guard < 64 and depth < 32:
            guard += 1
            out.append((n, cstr(m, u32(m, n + 0xA4), 32), parent))
            walk(u32(m, n + 0x90), len(out) - 1, depth + 1)
            n = u32(m, n + 0x94)
    walk(top, -1, 0)
    return out


def ped_lcs_to_vcs(src):
    """LCS's ped model chunk -> VCS's dialect (bytes, report lines). The skeleton stays LCS's 24 bones (VCS's
    animations reach them in the same order; the jaw, which LCS lacks, is simply not there)."""
    c = Chunk(src)
    m = c.b
    group = u32(m, 0x24)
    if m[group] != 2:
        raise PackError("the model is not an element group")
    top = u32(m, group + 4)
    elem_link = u32(m, group + 8)
    if elem_link == group + 8 or u32(m, elem_link) != group + 8:
        raise PackError("the model has not exactly one element")
    elem = elem_link - 0x1C
    tree = u32(m, elem + 0x2C)
    nodes = lcs_nodes(m, top)
    rootk = next((k for k, (n, _, _) in enumerate(nodes) if u32(m, n + 0xA0) == tree), -1)
    if rootk < 0 or not tree:
        raise PackError("no skeleton (anim tree) in the model")
    ninfo, count = u32(m, tree + 0x30), u32(m, tree + 4)
    # VCS's skeleton has a jaw (bone 6) after the head, which LCS's lacks; VCS's animations reach the bones by their
    # place (as LCS's: see ped_vcs_to_lcs), so one is put in: a child of the head, at the head, with no vertices
    ids = [m[ninfo + 8 * i] for i in range(count)]
    flags = [m[ninfo + 8 * i + 2] for i in range(count)]
    if 6 not in ids and 5 in ids:
        h = ids.index(5)
        if flags[h] & 3 != 3:
            raise PackError("the head is not a leaf of the skeleton (flags %d)" % flags[h])
        ids.insert(h + 1, 6)
        flags.insert(h + 1, 1)  # the head's only child, last: pop
        flags[h] = 2  # push: the head has a child now and siblings after it
        head_node = next(k for k, (n, _, _) in enumerate(nodes) if i32(m, n + 0x9C) == 5)
        nodes.append((nodes[head_node][0], "jaw", head_node))
        jaw_at = h + 1
    else:
        jaw_at = -1
    old_count, count = count, len(ids)
    nin = c.reserve(8 * count)
    for i in range(count):
        m[nin + 8 * i:nin + 8 * i + 4] = bytes((255 if ids[i] in (54, 55) else ids[i], i, flags[i], 0xAA))
    ninfo = nin
    remap = {i: (i + 1 if jaw_at >= 0 and i >= jaw_at else i) for i in range(old_count)}
    geom0 = u32(m, elem + 0x14)
    skin0 = u32(m, geom0 + 0x18)
    if skin0 and jaw_at >= 0:
        inv_old = u32(m, skin0 + 12)
        inv = c.reserve(0x40 * count)
        for i in range(old_count):
            m[inv + 0x40 * remap[i]:inv + 0x40 * remap[i] + 0x40] = m[inv_old + 0x40 * i:inv_old + 0x40 * i + 0x40]
        m[inv + 0x40 * jaw_at:inv + 0x40 * jaw_at + 0x40] = m[inv_old + 0x40 * (jaw_at - 1):inv_old + 0x40 * jaw_at]
        c.ptr(skin0 + 12, inv)
        c.put(skin0, count)
        remap_weights(m, geom0, remap)
    new = {k: c.reserve(0xB0) for k in range(len(nodes))}
    holder = c.reserve(0xB0)  # VCS: the element hangs from a node of its own beside the skeleton
    names = {}
    for k, (n, name, parent) in enumerate(nodes):
        bid = i32(m, n + 0x9C)
        names[k] = c.string("jaw" if name == "jaw" and k == len(nodes) - 1 else
                            VCS_NAMES.get(bid, name.lower().replace(" ", "_")) if k else "scene_root")
    hname = c.string("hero_base01")
    for k, (n, name, parent) in enumerate(nodes):
        at = new[k]
        c.put(at, 0x0380AA00 if k == 0 else 0x0180AA00)
        c.ptr(at + 4, new[parent] if parent >= 0 else 0)
        c.ptr(at + 8, at + 8)
        c.ptr(at + 12, at + 8)
        m[at + 0x10:at + 0x90] = m[n + 0x10:n + 0x90]
        if k == 0:
            m[at + 0x10:at + 0x50] = VCS_SCENE_ROOT
        elif name == "jaw" and k == len(nodes) - 1:
            m[at + 0x10:at + 0x50] = VCS_SCENE_ROOT  # (identity: at the head)
        kids = [j for j in range(len(nodes)) if nodes[j][2] == k]
        first = holder if k == 0 else (new[kids[0]] if kids else 0)
        c.ptr(at + 0x90, first)
        sib = [j for j in range(len(nodes)) if nodes[j][2] == parent and j > k]
        c.ptr(at + 0x94, new[sib[0]] if sib and parent >= 0 else 0)
        c.ptr(at + 0x98, new[0])
        c.put(at + 0x9C, 0)
        c.put(at + 0xA0, 0)
        c.ptr(at + 0xA4, 0)
        c.ptr(at + 0xA8, names[k])
        c.put(at + 0xAC, 0)
    kids0 = [j for j in range(len(nodes)) if nodes[j][2] == 0]
    c.put(holder, 0x0380AA00)
    c.ptr(holder + 4, new[0])
    c.ptr(holder + 8, elem + 8)
    c.ptr(holder + 12, elem + 8)
    m[holder + 0x10:holder + 0x50] = VCS_SCENE_ROOT
    m[holder + 0x50:holder + 0x90] = VCS_SCENE_ROOT
    c.ptr(holder + 0x94, new[kids0[0]] if kids0 else 0)
    c.ptr(holder + 0x98, new[0])
    c.ptr(holder + 0xA8, hname)
    t = c.reserve(0x38)
    m[t:t + 0x28] = m[tree:tree + 0x28]
    c.put(t + 4, count)
    c.ptr(t + 0x30, ninfo)
    c.ptr(t + 0x34, new[rootk])
    c.ptr(new[rootk] + 0xA4, t)
    c.put(elem, 0x0004AA01)
    c.ptr(elem + 4, holder)
    c.ptr(elem + 8, holder + 8)
    c.ptr(elem + 12, holder + 8)
    c.put(elem + 0x10, 0)
    c.ptr(elem + 0x2C, t)
    c.put(group, 0x0000AA02)
    c.ptr(group + 4, new[0])
    geom = u32(m, elem + 0x14)
    skin = u32(m, geom + 0x18)
    if skin:
        c.put(skin + 4, 0)
    col = u32(m, 0x20)
    if col:
        nc = c.reserve(0x60)
        m[nc:nc + 0x30] = m[col:col + 0x30]
        spheres, boxes, tris = struct.unpack_from("<hhh", m, col + 0x30)
        struct.pack_into("<hhhBB", m, nc + 0x30, boxes, tris, spheres, m[col + 0x37], 0)
        for k in range(7):
            c.ptr(nc + 0x38 + 4 * k, u32(m, col + 0x3C + 4 * k) if (col + 0x3C + 4 * k) in c.relocs else 0)
        c.put(nc + 0x5C, 0xAAAAAAAA)
        c.ptr(0x20, nc)
    strips = u32(m, geom + 0x20 + 16) >> 20
    return c.finish(), ["model: %d bones, %d strips, %d bytes" % (count, strips, len(c.b))]


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


def xtx_lcs_to_vcs(src):
    """LCS's texture list chunk -> VCS's dialect: each texture's first mipmap level (bytes, report lines)"""
    if src[:4] != b"xet\0":
        raise PackError("not a texture list")
    texs = []
    link, guard = u32(src, 0x28), 0
    while link != 0x28 and guard < 64:
        guard += 1
        t = link - 8
        ras = u32(src, t)
        data, flags = u32(src, ras), u32(src, ras + 4)
        w, h, d = 1 << (flags & 63), 1 << (flags >> 6 & 63), flags >> 12 & 63
        if d not in (4, 8):
            raise PackError("texture %s: %d-bit" % (cstr(src, t + 16), d))
        # LCS's textures carry mipmaps (flags bits 20-23: the levels, all after each other) with the palette after the
        # last level, and bit 24 + k marks level k as swizzled - PSMT8's order, for 4-bit ones too over one index a
        # pixel (checked on Toni's textures, 2026-11-19). VCS's are one linear level (bits 20+ = 1), palette after it.
        levels = max(1, flags >> 20 & 15)
        total = sum((w >> k) * (h >> k) * d // 8 for k in range(levels))
        pal = bytes(src[data + total:data + total + (64 if d == 4 else 1024)])
        level, at = 0, data  # the first level (VCS draws one)
        lw, lh = w >> level, h >> level
        raw = bytes(src[at:at + lw * lh * d // 8])
        idx = raw if d == 8 else bytes((raw[i // 2] >> ((i & 1) * 4)) & 15 for i in range(lw * lh))
        if flags >> (24 + level) & 1 and lw >= 16 and lh >= 4:
            idx = unswizzle8(idx, lw, lh)
        px = idx if d == 8 else bytes(idx[i] | idx[i + 1] << 4 for i in range(0, lw * lh, 2))
        lflags = (flags & 0xFFFFF & ~0xFFF) | (flags & 63) - level | ((flags >> 6 & 63) - level) << 6
        texs.append((bytes(src[t + 16:t + 48]), px + pal, lflags | 1 << 20,
                     (0x45 << 16 | lw // 2) if d == 4 else (0x25 << 16 | lw)))
        link = u32(src, link)
    out = bytearray(0x50)
    relocs = []
    lst = 0x20
    out[lst], out[lst + 1] = 6, 0x86
    struct.pack_into("<I", out, lst + 16, 1)
    placed = []
    for _, px, _, _ in texs:
        while len(out) % 16:
            out.append(0)
        placed.append(len(out))
        out += px
    while len(out) % 16:
        out.append(0)
    # VCS's texture is 0x60 bytes with +0x50 = 1 (every one of its own; with 0x50-byte ones only the list's first
    # texture drew, the others showed the first's texels: a hidden run, 2026-11-19)
    tex_at = [len(out) + 0x60 * i for i in range(len(texs))]
    out += bytes(0x60 * len(texs))
    for t in tex_at:
        struct.pack_into("<I", out, t + 0x50, 1)
    ras_at = [len(out) + 16 * i for i in range(len(texs))]
    out += bytes(16 * len(texs))

    def ptr(at, v):
        struct.pack_into("<I", out, at, v)
        relocs.append(at)
    n = len(texs)
    for i, (name, _, flags, unk2) in enumerate(texs):
        t = tex_at[i]
        ptr(t, ras_at[i])
        ptr(t + 4, lst)
        ptr(t + 8, tex_at[i + 1] + 8 if i + 1 < n else lst + 8)
        ptr(t + 12, tex_at[i - 1] + 8 if i > 0 else lst + 8)
        out[t + 16:t + 48] = name
        struct.pack_into("<I", out, ras_at[i] + 4, unk2)
        ptr(ras_at[i] + 8, placed[i])
        struct.pack_into("<I", out, ras_at[i] + 12, flags)
    ptr(lst + 8, tex_at[0] + 8 if n else lst + 8)
    ptr(lst + 12, tex_at[-1] + 8 if n else lst + 8)
    while len(out) % 16:
        out.append(0)
    data_size = rel = len(out)
    for r in sorted(relocs):
        out += struct.pack("<I", r)
    struct.pack_into("<8I", out, 0, 0x00746578, 0, len(out), data_size, rel, len(relocs), 0, 0)
    return bytes(out), ["textures: " + ", ".join("%s %dx%d/%d" % (nm.split(b"\0")[0].decode("latin1"), 1 << (f & 63),
                                                                    1 << (f >> 6 & 63), f >> 12 & 63)
                                                  for nm, _, f, _ in texs)]


LCS_OUTFIT = "plr"  # Toni's outfit the pack takes (the one a free-roam save requested first: RequestSpecialModel(0, "plr"))


def lcs_player(d, outfit=LCS_OUTFIT):
    """LCS's player model `outfit` (the special-model directory: streamInfo + 0x1E328 -> {entries, max, count}, 0x20
    bytes {sector, sectors, name[24]}) and its texture list (the texture pool's slot of that name: the pool header
    {slots, flags, count} in GAME.DTZ, 0x1C bytes a slot, the name at +8; stream index 4900 + slot) -> (mdl, xtx)"""
    b = d.dtz
    dirp = u32(b, d.stream + 0x1E328)
    entries, count = u32(b, dirp), u32(b, dirp + 8)
    mdl = None
    for k in range(min(count, 4096)):
        e = entries + k * 0x20
        if cstr(b, e + 8, 24).lower() == outfit:
            mdl = d.iso.read_sectors(d.img_lba + u32(b, e), u32(b, e + 4))
    if not mdl or mdl[:4] != b"ldm\0":
        raise PackError("no player model %r on this disc" % outfit)
    # the pool: model 0's texture slot (+0x1E) is the default outfit's ("player"); its header points at slot 0
    txd0 = u16(b, d.info(0) + 0x1E)
    at = b.find(b"player\0")
    while at >= 0:
        base = at - 8 - txd0 * 0x1C
        hdr = b.find(struct.pack("<I", base)) if base > 0 else -1
        while hdr >= 0:
            n = u32(b, hdr + 8)
            if hdr % 4 == 0 and txd0 < n < 10000 and 0 < u32(b, hdr + 4) < len(b) - n:
                for k in range(n):
                    if b[u32(b, hdr + 4) + k] & 0x80 == 0 and cstr(b, base + k * 0x1C + 8, 24).lower() == outfit:
                        xtx = d.blob(d.g["tex_base"] + k)
                        if xtx[:4] == b"xet\0":
                            return mdl, xtx
            hdr = b.find(struct.pack("<I", base), hdr + 1)
        at = b.find(b"player\0", at + 1)
    raise PackError("no texture list %r on this disc" % outfit)


# ---- the pack ------------------------------------------------------------------------------------------------------

def source_stamp(iso):
    """the disc (size, date) and this tool's text: either changed -> the pack is made again"""
    st = os.stat(iso)
    with open(os.path.abspath(__file__), "rb") as f:
        tool = hashlib.sha256(f.read()).hexdigest()[:16]
    return "%d:%d:%s" % (st.st_size, int(st.st_mtime), tool)


# Outfits: each of this game's outfit names -> the other hero's outfit worn for it (docs/STORIES.md "One hero in both
# cities", the outfit tables). Names not listed (LCS's unused player2..player12) take the default (the first entry).
# LCS's outfits (their texture names): player mafia suit (the default model), plr leather jacket, plr2 lawyer suit,
# plr3 traveller, plr4 Chinese jacket, plr5 robbery mask, plr6 tuxedo, plr7 Guardian Angels, plr8 Elvis, plr9 Cluckin'
# Bell mascot, plr10 boxers, plr11 Avenging Angels (goggles), plr12 dragon jacket, plr13 jeans and T-shirt, plr14
# tracksuit, plr15 smart short sleeves, plr16 smart long sleeves.
# VCS's (models 0, 155..167): plr army fatigues (the default), plr2 polo and jeans, plr3 shirt and chain, plr4 vest and
# jeans, plr5 shirt and trousers, plr6 pastel suit, plr7 tracksuit, plr8 hoodie, plr9 vest over a shirt, plr10 shirt and
# tie, plr11 smuggler, plr12 Lance's shirt, plr13 relaxed shirt and light jeans, plr14 wetsuit.
VIC_FOR_LCS = {"plr": "plr2", "player": "plr2", "plr2": "plr10", "plr3": "plr13", "plr4": "plr3", "plr5": "plr8",
               "plr6": "plr6", "plr7": "plr9", "plr8": "plr12", "plr9": "plr5", "plr10": "plr4", "plr11": "plr",
               "plr12": "plr3", "plr13": "plr2", "plr14": "plr7", "plr15": "plr11", "plr16": "plr6"}
TONI_FOR_VCS = {"plr": "plr", "plr2": "plr13", "plr3": "plr12", "plr4": "plr3", "plr5": "plr15", "plr6": "plr2",
                "plr7": "plr14", "plr8": "plr5", "plr9": "plr7", "plr10": "plr16", "plr11": "plr4", "plr12": "plr8",
                "plr13": "plr3", "plr14": "plr10"}
VCS_OUTFITS = ["plr"] + ["plr%d" % i for i in range(2, 15)]  # models 0, 155..167; texture lists 5, 153..165


def vcs_outfit(d, name):
    """VCS's outfit `name` -> (mdl, xtx): plr = model 0, plrN = model 153 + N (their model info's +0x1E: the list)"""
    mid = 0 if name == "plr" else 153 + int(name[3:])
    mdl = d.blob(mid)
    if mdl[:4] != b"ldm\0":
        raise PackError("no model for outfit %s (model %d)" % (name, mid))
    xtx = d.blob(d.g["tex_base"] + u16(d.dtz, d.info(mid) + 0x1E))
    if xtx[:4] != b"xet\0":
        raise PackError("no textures for outfit %s" % name)
    return mdl, xtx


def hero_from_disc(iso, game):
    """the other game's hero's outfits translated: (src game, {outfit: (mdl, xtx)}, {slot: outfit}, report)"""
    import stories_pack as sp
    d = sp.Disc(iso)
    if d.game == game:
        raise PackError("that is %s's own disc; give the OTHER game's" % game.upper())
    table = VIC_FOR_LCS if d.game == "vcs" else TONI_FOR_VCS
    outfits, report = {}, []
    for name in dict.fromkeys(table.values()):
        try:
            if d.game == "vcs":
                mdl, xtx = vcs_outfit(d, name)
                m, r1 = ped_vcs_to_lcs(mdl)
                x, r2 = xtx_vcs_to_lcs(xtx)
            else:
                mdl, xtx = lcs_player(d, name)
                m, r1 = ped_lcs_to_vcs(mdl)
                x, r2 = xtx_lcs_to_vcs(xtx)
        except (PackError, struct.error, IndexError, ValueError) as e:
            report.append("outfit %s: left out (%s)" % (name, e))
            continue
        outfits[name] = (m, x)
        report.append("outfit %s: %s" % (name, "; ".join(r1 + r2)))
    default = next(iter(table.values()))
    if default not in outfits:
        raise PackError("the hero's default outfit %s could not be made" % default)
    slots = {slot: (o if o in outfits else default) for slot, o in table.items()}
    return d.game, outfits, slots, report


HEROES = {"lcs": ("stories_vcs_hero", "vic", "Vic Vance (from your VCS disc)", "Vice City Stories", "Toni"),
          "vcs": ("stories_lcs_hero", "toni", "Toni Cipriani (from your LCS disc)", "Liberty City Stories", "Vic")}


def cmd_hero(iso, game, data_dir=None, dump=None, force=False):
    mod_id, hero, name, other, own = HEROES[game]
    if dump is None:
        if data_dir is None:
            os.environ["VCS_TOOLS_GAME"] = game
            import backup
            data_dir = backup.data_dir()
        man = os.path.join(data_dir, "mods", mod_id, "manifest.json")
        try:
            with open(man, encoding="utf-8") as f:
                old = json.load(f)
            files_ok = all(os.path.isfile(os.path.join(data_dir, "mods", mod_id, "files", f["name"]))
                           for f in old.get("files", [])) and old.get("files")
            if (not force and files_ok and old.get("pack_version") == PACK_VERSION and
                    old.get("source_stamp") == source_stamp(iso)):
                return {"mod": mod_id, "name": old.get("name"), "unchanged": True,
                        "dir": os.path.dirname(man)}
        except (OSError, ValueError):
            pass
    src, outfits, slots, report = hero_from_disc(iso, game)
    default = slots["plr"]
    files = {}
    for o, (m, x) in outfits.items():
        files["hero_%s.mdl" % o] = m
        files["hero_%s.xtx" % o] = x
    files[HERO_FILES[0]], files[HERO_FILES[1]] = outfits[default]
    # outfits.txt (src/game/stories_hero.cpp): "<this game's outfit name> <model file> <texture file>" per line
    lines = ["# tools/stories_pack.py hero: this game's outfit -> the hero's outfit worn for it (files under mods/)"]
    for slot, o in sorted(slots.items()):
        lines.append("%s %s/files/hero_%s.mdl %s/files/hero_%s.xtx" % (slot, mod_id, o, mod_id, o))
    lines.append("* %s/files/%s %s/files/%s" % (mod_id, HERO_FILES[0], mod_id, HERO_FILES[1]))
    files["outfits.txt"] = ("\n".join(lines) + "\n").encode()
    report.append("outfits: " + ", ".join("%s->%s" % kv for kv in sorted(slots.items())))
    if dump:
        os.makedirs(dump, exist_ok=True)
        for n, b in files.items():
            with open(os.path.join(dump, n), "wb") as f:
                f.write(b)
        return {"report": report, "dir": dump}
    os.environ["VCS_TOOLS_GAME"] = game
    import mods
    store = mods.Store(game, data_dir)
    d = os.path.join(store.dir, mod_id, "files")
    os.makedirs(d, exist_ok=True)
    for n, b in files.items():
        tmp = os.path.join(d, n + ".part")
        with open(tmp, "wb") as f:
            f.write(b)
        os.replace(tmp, os.path.join(d, n))
    try:
        enabled = store.get(mod_id).get("enabled", True)
    except mods.ModError:
        enabled = True
    e = mods.entry("hero", name.split(" (")[0] + " as the player", "hero", "hero name=" + hero, os.path.basename(iso))
    e["target"], e["match"], e["fields"] = 0, "picked", {"name": hero, "mdl": HERO_FILES[0], "xtx": HERO_FILES[1],
                                                         "outfits": "outfits.txt"}
    m = {"loader": 2, "game": game, "id": mod_id, "name": name, "source": os.path.basename(iso),
         "imported": int(time.time() * 1000), "enabled": enabled, "storage": "copy", "entries": [e],
         "notes": ["Made on this computer from your own %s disc (tools/stories_pack.py hero): %s can be the player "
                   "here (launcher: Stories: hero), in %d of his own outfits. Cutscenes keep %s." % (
                       other, name.split(" ")[0], len(outfits), own)],
         "sources": [], "files": [{"name": n, "size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
                                  for n, b in sorted(files.items())],
         "generated": "stories_pack", "pack_version": PACK_VERSION, "source_stamp": source_stamp(iso), "from": src,
         "outfits": slots}
    store.save(m)
    store.write_active()
    return {"mod": mod_id, "name": name, "dir": d, "report": report}


def add_parser(sub):
    p = sub.add_parser("hero", help="[lcs] the other game's player model as this game's (mods/stories_vcs_hero)")
    p.add_argument("iso", help="the OTHER game's disc image (Vice City Stories for --game lcs)")
    p.add_argument("--game", default="lcs", choices=("lcs", "vcs"))
    p.add_argument("--data", help="the game's data folder (default: VCS_DATA_DIR, else the game's own)")
    p.add_argument("--dump", metavar="DIR", help="only write the two chunks to DIR")
    p.add_argument("--force", action="store_true", help="make it again even if it is up to date")
    p.add_argument("--json", action="store_true")


def run(a):
    try:
        out = cmd_hero(a.iso, a.game, a.data, a.dump, a.force)
    except (PackError, OSError, struct.error, IndexError, ValueError) as e:
        out = {"error": str(e)}
    if os.environ.get("VCS_PROGRESS") == "json":
        if "error" in out:
            print("@vcs " + json.dumps({"error": "stories_pack", "message": out["error"]}), flush=True)
        print("@vcs " + json.dumps({"result": out}), flush=True)
    elif a.json:
        print(json.dumps(out))
    elif "error" in out:
        print("error:", out["error"])
    elif out.get("unchanged"):
        print("up to date:", out["dir"])
    else:
        for line in out.get("report", []):
            print("[hero] " + line)
        print("written:", out["dir"])
    return 1 if "error" in out else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split(chr(10) * 2)[0])
    add_parser(ap.add_subparsers(dest="cmd", required=True))
    sys.exit(run(ap.parse_args()))
