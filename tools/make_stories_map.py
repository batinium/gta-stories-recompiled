#!/usr/bin/env python3
"""Schematic map of GTA Stories: Liberty City and Vice City side by side with the bridge between them.

    python tools/make_stories_map.py --lcs-ram LCS_RAM.bin --vcs-ram VCS_RAM.bin [--out docs/images/stories_map.png]

No game art is used and none is bundled. The two land shapes are made from the positions of each game's own AI path
nodes (roads and pavements), read from a 32 MB RAM dump of the running game (the PS2 build, taken in a session
with the world loaded). The dots are grown and smoothed into a filled shape, so the coastlines are recognisable
but it is a drawing, not the game's map. Needs Python, Pillow and numpy.

Layout of the nodes in the dump (see docs/game-internals.md):
  LCS  ThePaths = u32 at 0x3D0314; +0 -> nodes (0x14 each, x / y / z int16 / 8 at +4 / +6 / +8); +0x14 = count.
  VCS  the same table sits at 0x1A44D68 (count at +0x14); its nodes (0x1A4E0B0 + 6, 10 bytes each) are
       {id, flags, x, y, z} with x / y / z int16 / 8.
The bridge numbers come from src/game/stories_bridge.cpp and src/game/dualworld.cpp: both cities share one
coordinate space, LCS world x = VCS x + 3800 (Vice City lies east of Liberty City), the border is LCS x 1800, the
bridge is a straight line at y -407 from the landing ramp in Portland (LCS x 1466) to Escobar's seawall
(VCS x -1833 = LCS x 1967).
"""
import argparse
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OFFSET = 3800.0  # LCS x = VCS x + OFFSET
BORDER = 1800.0  # LCS x
BRIDGE_Y = -407.0
BRIDGE_X0 = 1466.0  # Portland landing ramp (LCS x)
BRIDGE_X1 = -1833.0 + OFFSET  # Escobar seawall (VCS -1833)

X0, X1, Y0, Y1 = -1900.0, 5500.0, -2150.0, 1950.0  # the frame, in the shared (LCS) coordinates
W, H = 1600, 900
SS = 2  # supersampling

SEA = (14, 33, 52)
SEA_GRID = (22, 46, 70)
LCS_LAND = (88, 108, 128)   # Liberty City: cool slate
LCS_EDGE = (150, 175, 198)
VCS_LAND = (206, 120, 120)  # Vice City: warm coral
VCS_EDGE = (240, 176, 160)
BRIDGE = (255, 214, 10)
TEXT = (236, 240, 244)
DIM = (150, 168, 186)


def lcs_nodes(path):
    d = open(path, "rb").read()
    p = struct.unpack_from("<I", d, 0x3D0314)[0] & 0x1FFFFFF
    nodes = struct.unpack_from("<I", d, p)[0] & 0x1FFFFFF
    n = struct.unpack_from("<I", d, p + 0x14)[0]
    a = np.frombuffer(d, dtype="<i2", count=n * 10, offset=nodes).reshape(n, 10)
    return a[:, 2] / 8.0, a[:, 3] / 8.0, a[:, 4] / 8.0  # +4 / +6 / +8 bytes


def vcs_nodes(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0x1A44D68 + 0x14)[0]
    a = np.frombuffer(d, dtype="<i2", count=n * 5, offset=0x1A4E0B0 + 6).reshape(n, 5)
    return a[:, 2] / 8.0, a[:, 3] / 8.0, a[:, 4] / 8.0


def clean(x, y, z):
    # z is not used: in the VCS records its high bits hold flags
    ok = ((np.abs(x) > 1) | (np.abs(y) > 1)) & (np.abs(x) < 4000) & (np.abs(y) < 4000)
    return x[ok], y[ok]


def land_mask(x, y, scale, size):
    """Grow the node dots into a smooth filled shape (a mask of `size` pixels)."""
    m = Image.new("L", size, 0)
    dr = ImageDraw.Draw(m)
    r = 38.0 * scale  # a dot covers 38 m: bridges the gaps between nodes
    px = (x - X0) * scale
    py = (Y1 - y) * scale
    for a, b in zip(px, py):
        dr.ellipse((a - r, b - r, a + r, b + r), fill=255)
    m = m.filter(ImageFilter.GaussianBlur(r * 0.9)).point(lambda v: 255 if v > 90 else 0)
    # close small inland holes (parks, lakes with no roads): fill everything not connected to the border
    inv = m.point(lambda v: 255 - v)
    ImageDraw.floodfill(inv, (0, 0), 128)
    holes = inv.point(lambda v: 255 if v == 255 else 0)
    m = Image.composite(Image.new("L", size, 255), m, holes)
    return m.filter(ImageFilter.GaussianBlur(1.2)).point(lambda v: 255 if v > 128 else 0)


def font(px, bold=False):
    for name in (("segoeuib.ttf" if bold else "segoeui.ttf"), ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"),
                 "arialbd.ttf" if bold else "arial.ttf"):
        try:
            return ImageFont.truetype(name, px)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lcs-ram", required=True, help="RAM dump of Liberty City Stories (PS2)")
    ap.add_argument("--vcs-ram", required=True, help="RAM dump of Vice City Stories (PS2)")
    ap.add_argument("--out", default="docs/images/stories_map.png")
    ap.add_argument("--small", default="docs/images/stories_map_small.png", help="an 800 px wide copy (empty = none)")
    a = ap.parse_args()

    big = (W * SS, H * SS)
    scale = min((W * SS - 120 * SS) / (X1 - X0), (H * SS - 200 * SS) / (Y1 - Y0))
    cw, ch = (X1 - X0) * scale, (Y1 - Y0) * scale
    ox, oy = (big[0] - cw) / 2, (big[1] - ch) / 2 + 20 * SS

    def P(x, y):
        return ox + (x - X0) * scale, oy + (Y1 - y) * scale

    cs = (int(cw) + 1, int(ch) + 1)
    lx, ly = clean(*lcs_nodes(a.lcs_ram))
    vx, vy = clean(*vcs_nodes(a.vcs_ram))
    vx = vx + OFFSET
    ml = land_mask(lx, ly, scale, cs)
    mv = land_mask(vx, vy, scale, cs)
    # keep each city on its own side of the border
    bx = int((BORDER - X0) * scale)
    ImageDraw.Draw(ml).rectangle((bx, 0, cs[0], cs[1]), fill=0)
    ImageDraw.Draw(mv).rectangle((0, 0, bx, cs[1]), fill=0)

    img = Image.new("RGB", big, SEA)
    dr = ImageDraw.Draw(img)
    for gx in range(-1000, 6000, 1000):
        if X0 <= gx <= X1:
            dr.line([P(gx, Y0), P(gx, Y1)], fill=SEA_GRID, width=SS)
    for gy in range(-2000, 2001, 1000):
        dr.line([P(X0, gy), P(X1, gy)], fill=SEA_GRID, width=SS)

    def paint(mask, fill, edge):
        edge_m = mask.filter(ImageFilter.MaxFilter(2 * SS + 1))
        img.paste(Image.new("RGB", cs, edge), (int(ox), int(oy)), edge_m)
        img.paste(Image.new("RGB", cs, fill), (int(ox), int(oy)), mask)

    paint(ml, LCS_LAND, LCS_EDGE)
    paint(mv, VCS_LAND, VCS_EDGE)

    # the border, faint and dashed
    y = Y0
    while y < Y1:
        dr.line([P(BORDER, y), P(BORDER, min(y + 60, Y1))], fill=(210, 220, 232), width=SS)
        y += 120
    # the bridge: a dark casing, then the bright line, with round end marks
    p0, p1 = P(BRIDGE_X0, BRIDGE_Y), P(BRIDGE_X1, BRIDGE_Y)
    dr.line([p0, p1], fill=(40, 30, 0), width=11 * SS)
    dr.line([p0, p1], fill=BRIDGE, width=7 * SS)
    for p in (p0, p1):
        r = 9 * SS
        dr.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=BRIDGE, outline=(40, 30, 0), width=2 * SS)

    fb, fm, fs = font(46 * SS, True), font(26 * SS, True), font(20 * SS)

    def text(xy, s, f, fill, anchor="la"):
        dr.text(xy, s, font=f, fill=fill, anchor=anchor)

    def pill(xy, s, f, fill, anchor="mm"):
        l, t, r, b = dr.textbbox(xy, s, font=f, anchor=anchor)
        pad = 7 * SS
        dr.rounded_rectangle((l - pad, t - pad, r + pad, b + pad), radius=6 * SS, fill=(10, 24, 40), outline=fill, width=SS)
        dr.text(xy, s, font=f, fill=fill, anchor=anchor)

    text(P(-700, 1250), "LIBERTY CITY", fb, TEXT, "mm")
    text(P(-700, 1050), "Liberty City Stories", fs, (205, 218, 230), "mm")
    text(P(5100, 1780), "VICE CITY", fb, TEXT, "mm")
    text(P(5100, 1600), "Vice City Stories", fs, (255, 226, 218), "mm")
    bm = P((BRIDGE_X0 + BRIDGE_X1) / 2, BRIDGE_Y)
    pill((bm[0], bm[1] - 60 * SS), "BRIDGE", fm, BRIDGE)
    pill(P(BRIDGE_X0 - 120, BRIDGE_Y - 520), "Portland", fs, TEXT)
    pill(P(BRIDGE_X1 + 520, BRIDGE_Y - 520), "Escobar International", fs, TEXT)
    bp = P(BORDER, Y1)
    text((bp[0], bp[1] + 6 * SS), "border", fs, DIM, "mt")
    text((40 * SS, 28 * SS), "GTA Stories Recompiled", font(34 * SS, True), TEXT)
    text((40 * SS, 72 * SS), "Where the bridge joins Liberty City and Vice City", fs, DIM)

    # scale bar (1000 units, about 1 km) and compass
    sx, sy = P(X0 + 120, Y0 + 130)
    dr.line([(sx, sy), (sx + 1000 * scale, sy)], fill=TEXT, width=3 * SS)
    for t in (0, 1):
        dr.line([(sx + t * 1000 * scale, sy - 8 * SS), (sx + t * 1000 * scale, sy + 8 * SS)], fill=TEXT, width=3 * SS)
    text((sx + 500 * scale, sy - 14 * SS), "1 km", fs, TEXT, "ms")
    cx, cy = W * SS - 90 * SS, 120 * SS
    dr.polygon([(cx, cy - 46 * SS), (cx - 14 * SS, cy + 8 * SS), (cx, cy), (cx + 14 * SS, cy + 8 * SS)], fill=TEXT)
    text((cx, cy - 54 * SS), "N", fm, TEXT, "ms")
    text((W * SS - 30 * SS, H * SS - 18 * SS),
         "Schematic: land shapes drawn from path-node positions, no game artwork", font(15 * SS), DIM, "rs")

    out = img.resize((W, H), Image.LANCZOS)
    out.save(a.out, optimize=True)
    if a.small:
        out.resize((800, 450), Image.LANCZOS).save(a.small, optimize=True)
    print("wrote", a.out, "bridge px", p0, p1)


if __name__ == "__main__":
    main()
