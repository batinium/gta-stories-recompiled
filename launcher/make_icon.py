#!/usr/bin/env python3
"""Draws GTA Stories Recompiled's icon: Liberty City at night from the water - a suspension bridge's tower close on the left,
its cable sagging past a big amber moon to a far tower, a lit skyline behind the deck, a crimson sky over navy water
and the moon broken up on the waves. Colours are LCS's (docs/LCS.md: amber #F9B035, crimson #AC0000, navy). And
lcs.exe's (src/lcs.ico): the same scene by day, in amber with a pale sun, so the launcher and the game don't look alike
in Explorer or the taskbar. vcs.exe keeps upstream's own icon (src/vcs.ico, drawn by upstream's make_icon.py, kept as
it is). Our own drawing from plain shapes: no game, Rockstar or console art. Standard library only.

    python launcher/make_icon.py            writes launcher/launcher.ico (16, 20, 24, 32, 40, 48, 64, 256 px),
                                            launcher/launcher.png (256 px: the Linux .desktop entry's and the
                                            AppImage's icon), src/lcs.ico (the same sizes, by day) and Android's
                                            adaptive icon (android/app/src/main/res/drawable/ic_launcher_*.xml)
    python launcher/make_icon.py --png DIR  also writes each size as DIR/launcher_<size>.png and DIR/lcs_<size>.png
    python launcher/make_icon.py --header OUT.h   only the X11 window icon (_NET_WM_ICON: 32, 64, 128 px as
                                            ARGB) as a C header - launcher/CMakeLists.txt runs it on Linux
    python launcher/make_icon.py --icns OUT.icns  only the macOS app icon (PNG entries 16 - 256 px) - the
                                            launcher's CMakeLists.txt runs it on macOS for GTAStoriesLauncher.app

One scene for every output: it is laid out in Android's 108 x 108 adaptive-icon grid (the 66 dp safe circle in the
middle, the 72 dp square around it what a launcher shows) from rectangles, circles, polygons and quadratic Bezier
strokes, which this script both rasterises (the desktop icons: the 72 dp square in a rounded square) and writes as
Android vector path data, so the two never drift apart. Small sizes draw less: no lit windows or bridge hangers up to
32 px, no stars below 64.
"""
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def rgb(v):
    return ((v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255)


def hash01(n):
    """The same 'random' numbers every time, in [0, 1)."""
    n &= 0xFFFFFFFF
    n ^= n >> 16
    n = (n * 0x7FEB352D) & 0xFFFFFFFF
    n ^= n >> 15
    n = (n * 0x846CA68B) & 0xFFFFFFFF
    n ^= n >> 16
    return (n & 0xFFFFFF) / 16777216


# ---------------------------------------------------------------------------------------------------------- the scene
# Fills: ("solid", rgb), ("vgrad", y0, y1, [(offset, rgb)]), ("radial", cx, cy, r, [(offset, rgb, alpha)]).
# Primitives: ("rect", x0, y0, x1, y1), ("circle", cx, cy, r), ("poly", [points]), ("quad", p0, p1, p2, width) - a
# stroked quadratic Bezier (butt caps). A shape: dict(name, prims, fill, alpha, mono, min_size): mono is its alpha in
# the monochrome layer (None: left out), min_size the smallest raster size that draws it.

NIGHT = dict(
    sky=[(0, 0x1C1838), (0.42, 0x4A1630), (0.78, 0xB02620), (1, 0xF2683A)],
    sea=[(0, 0x7A2222), (0.3, 0x3A1428), (1, 0x14102A)],
    far=0x6A1E2E, city_top=0x5A2648, city_bottom=0x2A1230, rim=0xF07A4A, window=0xFFC560, bridge=0x07061A,
    moon=[(0, 0xFFD98A), (0.55, 0xF9B035), (1, 0xF08A1C)], glow=0xF9B035, shimmer=0xF9B035, stars=True)
DAY = dict(
    sky=[(0, 0xFFE2A6), (0.6, 0xFBBE4E), (1, 0xF07A26)],
    sea=[(0, 0xB8301A), (1, 0x5A0A14)],
    far=0xD0602A, city_top=0xC0442A, city_bottom=0x8A2018, rim=0xFFE0A0, window=0xFFE6B0, bridge=0x5E0812,
    moon=[(0, 0xFFFBEF), (1, 0xFFF0D0)], glow=0xFFF4DC, shimmer=0xFFE6B0, stars=False)

HORIZON = 68.0
MOON = (68.0, 35.5, 9.5)
# the skyline: (left, right, top, crown) - crown 0 flat, 1 a setback, 2 two setbacks and a spire, 3 a pyramid roof
BUILDINGS = [(13.5, 19.5, 50, 1), (19.5, 25.5, 43, 0), (25.5, 31.5, 53, 3),
             (46.5, 51.5, 46, 0), (51.5, 57.5, 37, 2), (57.5, 62.5, 49, 1), (62.5, 68.0, 53, 0),
             (68.0, 73.0, 47, 3), (73.0, 79.0, 41, 1), (84.5, 90.0, 46, 0), (90.0, 96.0, 52, 1)]


def parabola(xa, ya, xb, yb, ym):
    """The hanging cable y = a (x - xm)^2 + ym through (xa, ya) and (xb, yb), lowest at ym; returns its exact
    quadratic Bezier and (xm, a, ym)."""
    ra, rb = math.sqrt(ym - ya), math.sqrt(ym - yb)
    xm = (xa * rb + xb * ra) / (ra + rb)
    a = (ya - ym) / (xa - xm) ** 2
    cx = (xa + xb) / 2
    cy = ya + 2 * a * (xa - xm) * (cx - xa)
    return ((xa, ya), (cx, cy), (xb, yb)), (xm, a, ym)


def far_row():
    rects, x, i = [], 0.0, 0
    while x < 108:
        w = 3.0 + 3.5 * hash01(3 * 97 + i * 13)
        h = 1.5 + 5.5 * hash01(3 * 97 + i * 13 + 5)
        rects.append(("rect", x, HORIZON - h, min(108.0, x + w + 0.05), HORIZON + 0.5))
        x += w
        i += 1
    return rects


def skyline():
    bodies, rims, windows = [], [], []
    for b, (x0, x1, top, crown) in enumerate(BUILDINGS):
        w = x1 - x0
        bodies.append(("rect", x0, top, x1, HORIZON + 0.6))
        tops = [(x0, x1, top)]
        if crown == 1:
            bodies.append(("rect", x0 + w * 0.2, top - 2.6, x1 - w * 0.2, top + 0.1))
            tops.append((x0 + w * 0.2, x1 - w * 0.2, top - 2.6))
        elif crown == 2:
            bodies += [("rect", x0 + w * 0.15, top - 3.0, x1 - w * 0.15, top + 0.1),
                       ("rect", x0 + w * 0.32, top - 6.0, x1 - w * 0.32, top + 0.1),
                       ("rect", x0 + w * 0.5 - 0.45, top - 11.0, x0 + w * 0.5 + 0.45, top + 0.1)]
            tops += [(x0 + w * 0.15, x1 - w * 0.15, top - 3.0), (x0 + w * 0.32, x1 - w * 0.32, top - 6.0)]
        elif crown == 3:  # clockwise, as the rectangles are (one fill rule for all)
            bodies.append(("poly", [(x0, top + 0.1), (x0 + w / 2, top - 3.6), (x1, top + 0.1)]))
        rims += [("rect", a, y, c, y + 0.75) for a, c, y in tops]  # the sky's glow on the roofs
        cols = max(1, int((w - 1.6) / 1.9))  # lit windows: a grid, about 40 % on
        left = x0 + (w - (cols - 1) * 1.9 - 0.9) / 2
        y, row = top + 2.2, 0
        while y < HORIZON - 2.0:
            for c in range(cols):
                if hash01(b * 131 + row * 17 + c + 3) < 0.42:
                    wx = round(left + c * 1.9, 2)
                    windows.append(("rect", wx, round(y, 2), round(wx + 0.9, 2), round(y + 1.25, 2)))
            y += 2.7
            row += 1
    return bodies, rims, windows


def shimmer(cx, y0):
    """The moon on the water: broken dashes down a column, narrower and fainter going out."""
    rows = ((0.0, 16.0, 1.5, 0.9, 0.62), (3.6, 12.0, 1.3, 0.72, 0.3), (7.0, 8.5, 1.15, 0.55, 0.7),
            (10.2, 5.5, 1.0, 0.42, 0.4), (13.2, 3.0, 0.9, 0.32, None))
    out = []
    for dy, w, th, alpha, split in rows:
        x0, x1 = cx - w / 2, cx + w / 2
        pieces = [(x0, x1)] if split is None else [(x0, x0 + w * split - 0.9), (x0 + w * split + 0.9, x1)]
        out.append(([("rect", round(a, 2), y0 + dy, round(b, 2), round(y0 + dy + th, 2)) for a, b in pieces], alpha))
    return out


def scene(p):
    """(background shapes, foreground shapes) for a palette."""
    S = lambda name, prims, fill, alpha=1.0, mono=None, min_size=0: dict(
        name=name, prims=prims, fill=fill, alpha=alpha, mono=mono, min_size=min_size)
    solid = lambda v: ("solid", rgb(v))
    bg = [S("the sky: night up high, a crimson glow down to the horizon", [("rect", 0, 0, 108, HORIZON)],
            ("vgrad", 0, HORIZON, [(o, rgb(c)) for o, c in p["sky"]])),
          S("the water", [("rect", 0, HORIZON, 108, 108)],
            ("vgrad", HORIZON, 108, [(o, rgb(c)) for o, c in p["sea"]])),
          S("the far shore: a low row of buildings in the haze", far_row(), solid(p["far"]), 0.9)]
    if p["stars"]:
        bg.append(S("a few stars", [("circle", x, y, r) for x, y, r in
                                    ((27, 25, 0.55), (23, 36, 0.4), (49, 23, 0.45), (86, 27, 0.4), (57, 29, 0.3))],
                    ("solid", rgb(0xF4E8E0)), 0.7, min_size=64))

    mx, my, mr = MOON
    g = rgb(p["glow"])
    R = mr + 12
    bodies, rims, windows = skyline()
    deck = 65.4  # the deck hides the horizon line
    T, F = 27.0, 47.5  # the near and far towers' tops
    main, (xm, a, ym) = parabola(39.0, T, 82.4, F, 63.0)
    side, (xs, as_, ys) = parabola(39.0, T, -6.0, 66.0, 66.4)
    tail, _ = parabola(82.4, F, 112.0, 66.0, 66.4)
    hangers = []
    for x in [48.6 + 4.6 * k for k in range(7)]:
        y = a * (x - xm) ** 2 + ym
        hangers.append(("rect", round(x - 0.42, 2), round(y, 2), round(x + 0.42, 2), deck + 0.2))
    for x in [29.4 - 4.6 * k for k in range(7)]:
        y = as_ * (x - xs) ** 2 + ys
        hangers.append(("rect", round(x - 0.42, 2), round(y, 2), round(x + 0.42, 2), deck + 0.2))
    steel = [("rect", 0, deck, 108, HORIZON + 0.6),
             # the near tower: two legs into the water, a cap, two cross beams
             ("rect", 33.2, T, 36.6, 92), ("rect", 41.4, T, 44.8, 92), ("rect", 32.2, T - 1.9, 45.8, T + 1.5),
             ("rect", 35.6, T + 10.0, 42.4, T + 12.4), ("rect", 35.6, T + 23.0, 42.4, T + 25.2),
             # the far tower
             ("rect", 79.2, F, 80.9, HORIZON), ("rect", 83.9, F, 85.6, HORIZON), ("rect", 78.6, F - 1.6, 86.2, F + 0.4),
             ("rect", 80.4, F + 6.5, 84.4, F + 7.7),
             ("quad",) + main + (1.9,), ("quad",) + side + (1.9,), ("quad",) + tail + (1.5,)]
    fg = [S("the moon's glow", [("circle", mx, my, R)],
            ("radial", mx, my, R, [(0, g, 0.42), (mr / R, g, 0.42), (0.62, g, 0.16), (1, g, 0.0)])),
          S("the moon", [("circle", mx, my, mr)], ("vgrad", my - mr, my + mr, [(o, rgb(c)) for o, c in p["moon"]]),
            mono=0.4),
          S("the skyline", bodies, ("vgrad", 36, HORIZON, [(0, rgb(p["city_top"])), (1, rgb(p["city_bottom"]))]),
            mono=0.62),
          S("the sky's light on the roofs", rims, solid(p["rim"]), 0.85),
          S("lit windows", windows, solid(p["window"]), 0.9, min_size=40)]
    for i, (prims, alpha) in enumerate(shimmer(mx, HORIZON + 2.2)):
        fg.append(S("the moon on the water" if i == 0 else None, prims, solid(p["shimmer"]), alpha, mono=alpha * 0.55))
    fg += [S("the bridge: deck, towers and cables", steel, solid(p["bridge"]), mono=1.0),
           S("the hangers", hangers, solid(p["bridge"]), mono=1.0, min_size=40)]
    return bg, fg


# ------------------------------------------------------------------------------------------ [stories] the mixed logo
# GTA Stories (both cities at once, docs/STORIES.md): the two icons this project has had, joined - Liberty City's
# suspension bridge and amber moon at night on the left (the scene above), its deck crossing the water to Vice City's
# striped neon sun and a palm at dusk on the right (the earlier Android icon, commit de712bd). The skies and the water
# blend from one city's colours into the other's between x 44 and 64. The launcher draws the same shapes itself
# (launcher/screens/stories.cpp storiesLogo, from these numbers) for the GTA Stories tab; this writes it as PNGs
# (--stories DIR) to look at, or to become the app icon one day (not done: the app icon stays the bridge and moon).
STORIES_SKY_L = [(0, 0x1C1838), (0.42, 0x4A1630), (0.78, 0xB02620), (1, 0xF2683A)]
STORIES_SKY_R = [(0, 0x120C2A), (0.45, 0x2A1446), (0.8, 0x6A2462), (1, 0xE0508E)]
STORIES_SEA_L = [(0, 0x7A2222), (0.3, 0x3A1428), (1, 0x14102A)]
STORIES_SEA_R = [(0, 0x4A1A56), (0.3, 0x2A1446), (1, 0x0E0A22)]
STORIES_BLEND = (44.0, 64.0)
STORIES_MOON = (41.0, 37.0, 7.5)
STORIES_SUN = (74.0, 59.5, 13.0)
STORIES_TOWER = 27.0  # the near tower's centre (its top: 30)
STORIES_PALM = 0x0C0820
# the palm: its trunk (a tapered quadratic: base, control, top, width at the base, at the top) and fronds (each a
# tapered quadratic from the crown)
STORIES_TRUNK = ((80.5, 89.0), (81.5, 70.0), (77.5, 50.5), 2.6, 1.5)
STORIES_CROWN = (77.5, 50.0)
STORIES_FRONDS = [((64.5, 55.5), (69.5, 44.5)), ((65.5, 44.0), (71.0, 41.5)), ((73.5, 37.0), (75.0, 40.5)),
                  ((85.5, 39.5), (82.0, 39.0)), ((90.0, 47.0), (85.5, 41.5)), ((87.5, 55.5), (85.5, 48.0))]


def stories_scene():
    """The mixed logo's shapes (one list, back to front)."""
    S = lambda name, prims, fill, alpha=1.0, min_size=0: dict(
        name=name, prims=prims, fill=fill, alpha=alpha, mono=None, min_size=min_size)
    solid = lambda v: ("solid", rgb(v))
    vg = lambda y0, y1, stops: ("vgrad", y0, y1, [(o, rgb(c)) for o, c in stops])
    x0, x1 = STORIES_BLEND
    shapes = [
        S("the sky: Liberty City's night on the left, Vice City's dusk on the right", [("rect", 0, 0, 108, HORIZON)],
          ("hblend", x0, x1, vg(0, HORIZON, STORIES_SKY_L), vg(0, HORIZON, STORIES_SKY_R))),
        S("the water, the same", [("rect", 0, HORIZON, 108, 108)],
          ("hblend", x0, x1, vg(HORIZON, 108, STORIES_SEA_L), vg(HORIZON, 108, STORIES_SEA_R))),
        S("stars", [("circle", x, y, r) for x, y, r in
                    ((23, 25, 0.5), (31, 22, 0.4), (52, 24, 0.45), (60, 31, 0.35), (86, 26, 0.45), (69, 21, 0.4))],
          solid(0xF4E8E0), 0.7, min_size=64)]
    # the moon and its glow
    mx, my, mr = STORIES_MOON
    g = rgb(0xF9B035)
    R = mr + 10
    shapes += [S("the moon's glow", [("circle", mx, my, R)],
                 ("radial", mx, my, R, [(0, g, 0.4), (mr / R, g, 0.4), (0.62, g, 0.14), (1, g, 0.0)])),
               S("the moon", [("circle", mx, my, mr)], vg(my - mr, my + mr, NIGHT["moon"]))]
    # the sun: solid above its middle, then stripes thinning toward the horizon (de712bd's)
    sx, sy, sr = STORIES_SUN
    shapes.append(S("the sun's glow", [("circle", sx, sy, sr + 9)],
                    ("radial", sx, sy, sr + 9, [(0, rgb(0xFF5AAE), 0.35), (sr / (sr + 9), rgb(0xFF5AAE), 0.3),
                                                (1, rgb(0xFF5AAE), 0.0)])))
    bands = [(sy - sr, sy - 1.0), (sy + 0.4, sy + 2.6), (sy + 3.6, sy + 5.4), (sy + 6.3, sy + 7.7)]
    sun = []
    for b0, b1 in bands:
        b1 = min(b1, HORIZON)
        if b1 <= b0:
            continue
        # the disc's slice between b0 and b1: a polygon along the circle
        pts = []
        for i in range(17):
            y = b0 + (b1 - b0) * i / 16
            pts.append((sx + math.sqrt(max(0.0, sr * sr - (y - sy) ** 2)), y))
        for i in range(17):
            y = b1 - (b1 - b0) * i / 16
            pts.append((sx - math.sqrt(max(0.0, sr * sr - (y - sy) ** 2)), y))
        sun.append(("poly", pts))
    shapes.append(S("the neon sun, setting in stripes", sun,
                    vg(sy - sr, HORIZON, [(0, 0xFFD27A), (0.5, 0xFF8A7A), (1, 0xFF5AAE)])))
    # Liberty City's skyline behind the bridge, low (the left of the icon)
    bodies, rims = [], []
    for x0_, x1_, top, crown in ((10.0, 16.0, 50.0, 1), (16.0, 22.0, 46.0, 0), (33.0, 38.5, 55.0, 3), (38.5, 44.0, 51.0, 1),
                                 (44.0, 49.0, 57.0, 0), (49.0, 54.0, 60.0, 0)):
        w = x1_ - x0_
        bodies.append(("rect", x0_, top, x1_, HORIZON + 0.6))
        rims.append(("rect", x0_, top, x1_, top + 0.75))
        if crown == 1:
            bodies.append(("rect", x0_ + w * 0.2, top - 2.4, x1_ - w * 0.2, top + 0.1))
            rims.append(("rect", x0_ + w * 0.2, top - 2.4, x1_ - w * 0.2, top - 1.65))
        elif crown == 3:
            bodies.append(("poly", [(x0_, top + 0.1), (x0_ + w / 2, top - 3.4), (x1_, top + 0.1)]))
    shapes += [S("Liberty City's skyline", bodies, vg(44, HORIZON, [(0, 0x5A2648), (1, 0x2A1230)])),
               S("the sky's light on its roofs", rims, solid(0xF07A4A), 0.85)]
    # the light on the water: the moon's broken amber dashes, the sun's pink stripes, a teal wave
    for i, (prims, alpha) in enumerate(shimmer(mx, HORIZON + 2.2)):
        shapes.append(S("the moon on the water" if i == 0 else None, [p for p in prims], solid(0xF9B035), alpha * 0.9))
    for k, (w, dy, a) in enumerate(((20.0, 2.0, 0.55), (15.0, 5.0, 0.42), (10.5, 8.4, 0.32), (6.5, 12.0, 0.24))):
        shapes.append(S("the sun on the water" if k == 0 else None,
                        [("rect", sx - w / 2, HORIZON + dy, sx + w / 2, HORIZON + dy + 1.2)], solid(0xFF5AAE), a))
    waves = [("quad", (58 + 6 * i, 78.0), (61 + 6 * i, 76.0 if i % 2 == 0 else 80.0), (64 + 6 * i, 78.0), 1.3)
             for i in range(6)]
    shapes.append(S("a wave", waves, solid(0x3FE0CF), 0.85, min_size=40))
    # the bridge: the near tower on the left, its cables, the deck crossing the water to Vice City's shore
    T = 30.0
    deck = 65.4
    tx = STORIES_TOWER
    main, (xm, a, ym) = parabola(tx + 3.0, T, 70.0, deck + 0.2, deck + 0.6)  # it meets the deck at its end
    side, (xs, as_, ys) = parabola(tx - 3.0, T, -6.0, 66.0, 66.4)
    hangers = []
    for x in [tx + 7.0 + 4.4 * k for k in range(8)]:
        y = a * (x - xm) ** 2 + ym
        if y < deck - 0.8:
            hangers.append(("rect", round(x - 0.4, 2), round(y, 2), round(x + 0.4, 2), deck + 0.2))
    for x in [tx - 7.0 - 4.4 * k for k in range(5)]:
        y = as_ * (x - xs) ** 2 + ys
        hangers.append(("rect", round(x - 0.4, 2), round(y, 2), round(x + 0.4, 2), deck + 0.2))
    steel = [("rect", 0, deck, 72.0, HORIZON + 0.6),
             ("rect", tx - 4.4, T, tx - 1.6, 84.0), ("rect", tx + 1.6, T, tx + 4.4, 84.0),
             ("rect", tx - 5.4, T - 1.9, tx + 5.4, T + 1.5),
             ("rect", tx - 1.8, T + 10.0, tx + 1.8, T + 12.2), ("rect", tx - 1.8, T + 22.0, tx + 1.8, T + 24.0),
             ("quad",) + main + (1.7,), ("quad",) + side + (1.7,)]
    shapes += [S("the bridge", steel, solid(0x07061A)), S("its hangers", hangers, solid(0x07061A), min_size=40)]
    # the towers' red lights
    shapes.append(S("the tower's light", [("circle", tx, T - 2.6, 0.7)], solid(0xFF3B2B), 0.9, min_size=48))
    # the palm against the sun
    (b, c, t, w0, w1) = STORIES_TRUNK
    palm = [("taper", b, c, t, w0, w1)]
    for end, ctrl in STORIES_FRONDS:
        palm.append(("taper", STORIES_CROWN, ctrl, end, 2.0, 0.35))
    palm += [("circle", 76.8, 51.6, 1.0), ("circle", 78.6, 51.9, 0.9)]
    shapes.append(S("the palm", palm, solid(STORIES_PALM)))
    return shapes


def render_stories(size):
    """The mixed logo as a desktop icon (the 72 dp view in a rounded square), PNG bytes."""
    return render(size, shapes=stories_scene())


# ------------------------------------------------------------------------------------------------------- rasteriser
def bezier(p0, p1, p2, n=32):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in (i / n for i in range(n + 1))]


def polygons(prim):
    kind = prim[0]
    if kind == "rect":
        _, x0, y0, x1, y1 = prim
        return [[(x0, y0), (x1, y0), (x1, y1), (x0, y1)]]
    if kind == "circle":
        _, cx, cy, r = prim
        n = max(32, int(r * 8))
        return [[(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]]
    if kind == "poly":
        return [prim[1]]
    if kind in ("quad", "taper"):  # the stroke's outline: one side out, the other back (taper: its width goes w0 -> w1)
        if kind == "quad":
            _, p0, p1, p2, w = prim
            w0 = w1 = w
        else:
            _, p0, p1, p2, w0, w1 = prim
        pts = bezier(p0, p1, p2)
        L, R = [], []
        for i, p in enumerate(pts):
            a, b = pts[max(0, i - 1)], pts[min(len(pts) - 1, i + 1)]
            dx, dy = b[0] - a[0], b[1] - a[1]
            d = math.hypot(dx, dy) or 1
            w = w0 + (w1 - w0) * i / (len(pts) - 1)
            nx, ny = -dy / d * w / 2, dx / d * w / 2
            L.append((p[0] + nx, p[1] + ny))
            R.append((p[0] - nx, p[1] - ny))
        return [L + R[::-1]]
    raise ValueError(kind)


SUB = 5  # sample rows per pixel; across, coverage is exact


def coverage(polys, size, tf):
    """{row: {x: coverage}} of polygons (nonzero rule) mapped to pixels by tf."""
    edges = []
    for poly in polys:
        pp = [tf(p) for p in poly]
        for k in range(len(pp)):
            (x0, y0), (x1, y1) = pp[k], pp[(k + 1) % len(pp)]
            if y0 != y1:
                d = 1
                if y0 > y1:
                    x0, y0, x1, y1, d = x1, y1, x0, y0, -1
                edges.append((y0, y1, x0, (x1 - x0) / (y1 - y0), d))
    out = {}
    if not edges:
        return out
    for row in range(max(0, int(min(e[0] for e in edges))), min(size - 1, int(max(e[1] for e in edges))) + 1):
        cov = {}
        live = [e for e in edges if e[0] < row + 1 and e[1] > row]
        for s in range(SUB):
            y = row + (s + 0.5) / SUB
            xs = sorted((e[2] + (y - e[0]) * e[3], e[4]) for e in live if e[0] <= y < e[1])
            wind = 0
            for j in range(len(xs) - 1):
                wind += xs[j][1]
                a, b = max(0.0, xs[j][0]), min(float(size), xs[j + 1][0])
                if wind == 0 or b <= a:
                    continue
                ia, ib = int(a), min(int(b), size - 1)
                if ia == ib:
                    cov[ia] = cov.get(ia, 0) + (b - a) / SUB
                    continue
                cov[ia] = cov.get(ia, 0) + (ia + 1 - a) / SUB
                for i in range(ia + 1, ib):
                    cov[i] = cov.get(i, 0) + 1.0 / SUB
                if b > ib:
                    cov[ib] = cov.get(ib, 0) + (min(b, ib + 1) - ib) / SUB
        if cov:
            out[row] = cov
    return out


def paint(fill, x, y):
    """(rgb, alpha) of a fill at (x, y) in the icon's grid."""
    if fill[0] == "solid":
        return fill[1], 1.0
    if fill[0] == "hblend":  # [stories] two fills, the left one up to x0, the right one from x1, smoothly between
        _, x0, x1, left, right = fill
        u = min(1.0, max(0.0, (x - x0) / (x1 - x0)))
        u = u * u * (3 - 2 * u)
        (ca, aa), (cb, ab) = paint(left, x, y), paint(right, x, y)
        return tuple(p + (q - p) * u for p, q in zip(ca, cb)), aa + (ab - aa) * u
    if fill[0] == "vgrad":
        _, y0, y1, stops = fill
        t = min(1.0, max(0.0, (y - y0) / (y1 - y0)))
        stops = [(o, c, 1.0) for o, c in stops]
    else:
        _, cx, cy, r, stops = fill
        t = min(1.0, math.hypot(x - cx, y - cy) / r)
    for (t0, c0, a0), (t1, c1, a1) in zip(stops, stops[1:]):
        if t <= t1:
            u = (t - t0) / ((t1 - t0) or 1)
            return tuple(p + (q - p) * u for p, q in zip(c0, c1)), a0 + (a1 - a0) * u
    return stops[-1][1], stops[-1][2]


def rounded_square(size):
    r = 0.19 * size  # the desktop icon's corners
    pts = []
    for cx, cy, a0 in ((size - r, r, -math.pi / 2), (size - r, size - r, 0), (r, size - r, math.pi / 2), (r, r, math.pi)):
        pts += [(cx + r * math.cos(a0 + math.pi / 2 * i / 16), cy + r * math.sin(a0 + math.pi / 2 * i / 16))
                for i in range(17)]
    return [pts]


def render(size, pal=NIGHT, pixels_only=False, shapes=None):
    """The desktop icon: the launcher's 72 dp view of the scene in a rounded square, as PNG bytes (or raw RGBA).
    shapes: another scene's (back to front) instead of the bridge's with `pal` ([stories] the mixed logo)."""
    view, span = 18.0, 72.0
    sc = size / span
    tf = lambda p: ((p[0] - view) * sc, (p[1] - view) * sc)
    n = size * size
    R, G, B, A = [0.0] * n, [0.0] * n, [0.0] * n, [0.0] * n
    if shapes is None:
        bg, fg = scene(pal)
        shapes = bg + fg
    for shape in shapes:
        if size < shape["min_size"]:
            continue
        polys = [poly for prim in shape["prims"] for poly in polygons(prim)]
        for row, cov in coverage(polys, size, tf).items():
            yu = view + (row + 0.5) / sc
            for i, c in cov.items():
                col, fa = paint(shape["fill"], view + (i + 0.5) / sc, yu)
                al = min(1.0, c) * shape["alpha"] * fa
                if al <= 0:
                    continue
                k = row * size + i
                R[k] = col[0] * al + R[k] * (1 - al)
                G[k] = col[1] * al + G[k] * (1 - al)
                B[k] = col[2] * al + B[k] * (1 - al)
                A[k] = al + A[k] * (1 - al)
    mask = coverage(rounded_square(size), size, lambda p: p)
    rows = []
    for y in range(size):
        row = bytearray([0])  # PNG filter: none
        m = mask.get(y, {})
        for x in range(size):
            k = y * size + x
            a = A[k] * min(1.0, m.get(x, 0.0))
            c = (R[k] / A[k], G[k] / A[k], B[k] / A[k]) if A[k] > 0 else (0, 0, 0)
            row += bytes([int(min(1, max(0, v)) * 255 + 0.5) for v in c] + [int(min(1, a) * 255 + 0.5)])
        rows.append(bytes(row))
    if pixels_only:
        return b"".join(r[1:] for r in rows)  # RGBA, no filter bytes

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


# --------------------------------------------------------------------------------------------- Android vector layers
def num(v):
    s = "%.2f" % v
    s = s.rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def path_data(prims):
    out = []
    for prim in prims:
        kind = prim[0]
        if kind == "rect":
            _, x0, y0, x1, y1 = prim
            out.append("M%s,%sH%sV%sH%sZ" % (num(x0), num(y0), num(x1), num(y1), num(x0)))
        elif kind == "circle":
            _, cx, cy, r = prim
            out.append("M%s,%sa%s,%s 0,1 1,%s,0a%s,%s 0,1 1,-%s,0Z" % (
                num(cx - r), num(cy), num(r), num(r), num(2 * r), num(r), num(r), num(2 * r)))
        elif kind == "poly":
            pts = prim[1]
            out.append("M" + "L".join("%s,%s" % (num(x), num(y)) for x, y in pts) + "Z")
        elif kind == "quad":
            _, p0, p1, p2, _w = prim
            out.append("M%s,%sQ%s,%s %s,%s" % tuple(num(v) for v in p0 + p1 + p2))
    return " ".join(out)


def argb(c, a=1.0):
    return "#%02X%02X%02X%02X" % tuple(int(min(1, max(0, v)) * 255 + 0.5) for v in (a,) + tuple(c))


def wrap(text, indent, width=118):
    """Path data on lines of at most `width` characters."""
    lines, line = [], ""
    for tok in text.split(" "):
        if line and len(indent) + len(line) + 1 + len(tok) > width:
            lines.append(line)
            line = tok
        else:
            line = (line + " " + tok) if line else tok
    lines.append(line)
    return ("\n" + indent).join(lines)


def vector_paths(shapes, mono=False):
    out = []
    ind = "        "
    for s in shapes:
        alpha = s["mono"] if mono else s["alpha"]
        if alpha is None:
            continue
        if s["name"]:
            out.append("    <!-- %s -->" % s["name"])
        fills = [p for p in s["prims"] if p[0] != "quad"]
        strokes = [p for p in s["prims"] if p[0] == "quad"]
        fill = ("solid", (1.0, 1.0, 1.0)) if mono else s["fill"]
        if fills:
            data = wrap(path_data(fills), ind + "    ")
            if fill[0] == "solid":
                out.append('    <path android:fillColor="%s"\n%sandroid:pathData="%s" />' % (
                    argb(fill[1], alpha), ind, data))
            else:
                if fill[0] == "vgrad":
                    _, y0, y1, stops = fill
                    grad = ('android:type="linear"\n%s        android:startX="54" android:startY="%s"\n'
                            '%s        android:endX="54" android:endY="%s"' % (ind, num(y0), ind, num(y1)))
                    items = [(o, c, alpha) for o, c in stops]
                else:
                    _, cx, cy, r, stops = fill
                    grad = ('android:type="radial"\n%s        android:centerX="%s" android:centerY="%s"\n'
                            '%s        android:gradientRadius="%s"' % (ind, num(cx), num(cy), ind, num(r)))
                    items = [(o, c, a * alpha) for o, c, a in stops]
                body = "\n".join('%s        <item android:offset="%s" android:color="%s" />' % (ind, num(o), argb(c, a))
                                 for o, c, a in items)
                out.append('    <path android:pathData="%s">\n%s<aapt:attr name="android:fillColor">\n'
                           '%s    <gradient\n%s        %s>\n%s\n%s    </gradient>\n%s</aapt:attr>\n    </path>' % (
                               data, ind, ind, ind, grad, body, ind, ind))
        for q in strokes:
            out.append('    <path android:strokeColor="%s" android:strokeWidth="%s"\n%sandroid:pathData="%s" />' % (
                argb(fill[1], alpha), num(q[4]), ind, path_data([q])))
    return "\n".join(out)


VECTOR_HEAD = """<?xml version="1.0" encoding="utf-8"?>
<!-- [android] %s
     Generated by launcher/make_icon.py (the scene's single source; edit it there and rerun) - do not edit. -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt"
    android:width="108dp"
    android:height="108dp"
    android:viewportWidth="108"
    android:viewportHeight="108">
"""


def write_android(folder):
    bg, fg = scene(NIGHT)
    files = {
        "ic_launcher_background.xml": ("The app icon's background (our own drawing, no game or console art): Liberty "
                                       "City's night sky,\n     crimson at the horizon, over navy water and a low far "
                                       "shore.", vector_paths(bg)),
        "ic_launcher_foreground.xml": ("The app icon's foreground (our own drawing, no game or console art): a "
                                       "suspension bridge's\n     tower and cable against a big amber moon and a lit "
                                       "skyline, the moon broken on the water. The moon, the\n     cable's sag and "
                                       "the reflection keep inside the adaptive icon's 66 dp safe circle; the near "
                                       "tower,\n     the deck and the skyline's ends run past it on purpose, cut by "
                                       "the mask like a photo's edge.", vector_paths(fg)),
        "ic_launcher_monochrome.xml": ("The app icon for themed icons (Android 13+ tints it): the bridge in full, "
                                       "the skyline, moon and\n     its reflection fainter so the bridge reads "
                                       "against them.", vector_paths(fg, mono=True)),
    }
    for name, (comment, body) in files.items():
        path = os.path.join(folder, name)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(VECTOR_HEAD % comment + body + "\n</vector>\n")
        print("wrote " + os.path.normpath(path))


# ----------------------------------------------------------------------------------------------------------- writers
def write_header(path):
    """_NET_WM_ICON's layout: width, height, then ARGB pixels, one per `unsigned long` (Xlib's 32-bit format), for
    each size."""
    words = []
    for s in (32, 64, 128):
        rgba = render(s, pixels_only=True)
        words += [s, s]
        for i in range(0, len(rgba), 4):
            r, g, b, a = rgba[i:i + 4]
            words.append(a << 24 | r << 16 | g << 8 | b)
    lines = ["// Generated by launcher/make_icon.py --header - do not edit.", "#pragma once",
             "static const unsigned long g_launcher_icon[] = {"]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def write_icns(path):
    """Apple's icon container: 'icns', its length, then (type, length, PNG) entries."""
    entries = b""
    for kind, size in ((b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128), (b"ic08", 256)):
        data = render(size)
        entries += kind + struct.pack(">I", 8 + len(data)) + data
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"icns" + struct.pack(">I", 8 + len(entries)) + entries)
    os.replace(tmp, path)


def write_ico(path, images):
    header = struct.pack("<HHH", 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    entries = b""
    for s, data in zip(SIZES, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + entries + b"".join(images))
    print("wrote " + os.path.normpath(path))


def main():
    if "--stories" in sys.argv:  # [stories] the mixed logo, to look at (the app icon is not changed)
        folder = sys.argv[sys.argv.index("--stories") + 1]
        os.makedirs(folder, exist_ok=True)
        for s in (16, 18, 24, 32, 48, 64, 72, 128, 256, 512):
            with open(os.path.join(folder, "stories_%d.png" % s), "wb") as f:
                f.write(render_stories(s))
        print("wrote " + os.path.join(folder, "stories_<size>.png"))
        return
    if "--icns" in sys.argv:
        write_icns(sys.argv[sys.argv.index("--icns") + 1])
        return
    if "--header" in sys.argv:
        write_header(sys.argv[sys.argv.index("--header") + 1])
        return
    for name, pal, ico, png in (("launcher", NIGHT, os.path.join(HERE, "launcher.ico"), os.path.join(HERE, "launcher.png")),
                                ("lcs", DAY, os.path.join(ROOT, "src", "lcs.ico"), None)):
        images = [render(s, pal) for s in SIZES]
        if "--png" in sys.argv:
            folder = sys.argv[sys.argv.index("--png") + 1]
            os.makedirs(folder, exist_ok=True)
            for s, data in zip(SIZES, images):
                with open(os.path.join(folder, "%s_%d.png" % (name, s)), "wb") as f:
                    f.write(data)
        write_ico(ico, images)
        if png:
            with open(png, "wb") as f:
                f.write(images[-1])
            print("wrote " + os.path.normpath(png))
    write_android(os.path.join(ROOT, "android", "app", "src", "main", "res", "drawable"))


if __name__ == "__main__":
    main()
