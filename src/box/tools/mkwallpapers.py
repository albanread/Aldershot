#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read
"""mkwallpapers.py [--out DIR] [--check] [--png DIR] -- the box's tileable wallpapers.

Each is a sprite file of one seamless 128 x 128 sprite, 8 bpp with a 256
entry palette, 90 dpi, which !Appearance's Wallpaper page lists and the
Pinboard tiles with *Backdrop -Tile.  They are computed, not painted: the
same bytes every run.  Every term of every pattern repeats on 128 pixels
(periodic value noise, lines and cells whose period divides 128), so the
tile has no seam.

    Grey        the desktop's own tile (desktopart.py's), bevelled 64 px tiles
    Linen       woven threads, warm white
    Paper       a fine grain with a few fibres, cream
    Slate       a dark blue-grey stone with faint veins
    Denim       a diagonal twill in indigo
    Parchment   a mottled sepia
    Graph       graph paper: pale blue lines every 16 pixels, stronger every 64
    Dots        a dot grid on a pale ground
    Carbon      a dark carbon fibre weave
    Stripes     soft vertical pastel stripes
    Weave       a tan basket weave
    Blueprint   white lines on a deep blue grid
    Baize       green felt

A tile is a scalar field, 0 to 1 at each pixel, and a ramp of colours that
the field is mapped through to give the palette.  DIR defaults to
disc/Resources/Wallpapers, one file NAME,ff9 for each.  --png writes a
2 x 2 tiling of each as a PNG too, to look for seams.  --check exits 1 if
a file is not what this would write.
"""
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import desktopart  # noqa: E402

N = 128
DEFAULT = os.path.join(os.path.dirname(HERE), "disc", "Resources", "Wallpapers")


# ---- periodic noise -------------------------------------------------------------------

def lattice(nx, ny, seed):
    vals = []
    s = seed
    for _ in range(nx * ny):
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        vals.append((s >> 8) / float(1 << 23))
    return lambda i, j: vals[(j % ny) * nx + (i % nx)]


def smooth(t):
    return t * t * (3 - 2 * t)


def vnoise(x, y, nx, ny, f):
    """Value noise with nx x ny cells over the tile: it repeats at N"""
    gx, gy = x * nx / N, y * ny / N
    i, j = int(gx), int(gy)
    fx, fy = smooth(gx - i), smooth(gy - j)
    a, b, c, d = f(i, j), f(i + 1, j), f(i, j + 1), f(i + 1, j + 1)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def grain(x, y, seed):
    """A hash of the pixel: white noise, with the tile's period by being per pixel"""
    h = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFF) / 65535.0


def clamp(v):
    return 0.0 if v < 0 else 1.0 if v > 1 else v


# ---- the patterns: (x, y) -> 0..1 ---------------------------------------------------

def linen():
    warp = lattice(N, 1, 21)
    weft = lattice(1, N, 22)
    slub = lattice(64, 4, 23)
    slub2 = lattice(4, 64, 24)

    def f(x, y):
        over = ((x // 2) + (y // 2)) & 1             # which thread shows, in 2 pixel steps
        if over:
            t = 0.5 + 0.34 * (warp(x, 0) - 0.5) + 0.20 * (vnoise(x, y, 64, 4, slub) - 0.5)
        else:
            t = 0.5 + 0.34 * (weft(0, y) - 0.5) + 0.20 * (vnoise(x, y, 4, 64, slub2) - 0.5)
        t -= 0.10 * (1 if (x + y) & 1 else 0) * 0.5   # the cross-over's fine shadow
        return clamp(t + 0.10 * (grain(x, y, 3) - 0.5))
    return f, [(0, (0xC4, 0xBC, 0xAA)), (1, (0xF1, 0xEC, 0xDF))]


def paper():
    fib = lattice(32, 8, 31)
    soft = lattice(4, 4, 32)

    def f(x, y):
        t = 0.55 + 0.30 * (vnoise(x, y, 32, 8, fib) - 0.5) + 0.20 * (vnoise(x, y, 4, 4, soft) - 0.5)
        return clamp(t + 0.16 * (grain(x, y, 5) - 0.5))
    return f, [(0, (0xD6, 0xD1, 0xC2)), (1, (0xF4, 0xF0, 0xE4))]


def slate():
    big = lattice(4, 4, 41)
    mid = lattice(16, 16, 42)
    vein = lattice(2, 32, 43)

    def f(x, y):
        t = (0.42 * vnoise(x, y, 4, 4, big) + 0.26 * vnoise(x, y, 16, 16, mid) +
             0.22 * vnoise(x, y, 2, 32, vein))
        return clamp(0.15 + 1.1 * t + 0.10 * (grain(x, y, 7) - 0.5))
    return f, [(0, (0x2B, 0x35, 0x3F)), (1, (0x5C, 0x6A, 0x76))]


def denim():
    yarn = lattice(N, 1, 51)
    cross = lattice(1, N, 52)
    cloud = lattice(8, 8, 53)

    def f(x, y):
        d = (x + y) & 3                              # a 3 over 1 twill running up to the right
        base = 0.78 if d == 0 else 0.42 if d == 1 else 0.30
        t = base + 0.18 * (yarn(x, 0) - 0.5) + 0.12 * (cross(0, y) - 0.5) + 0.20 * (vnoise(x, y, 8, 8, cloud) - 0.5)
        return clamp(t + 0.10 * (grain(x, y, 9) - 0.5))
    return f, [(0, (0x1B, 0x2E, 0x4D)), (0.6, (0x34, 0x54, 0x80)), (1, (0x74, 0x92, 0xBC))]


def parchment():
    a = lattice(4, 4, 61)
    b = lattice(8, 8, 62)
    c = lattice(32, 32, 63)

    def f(x, y):
        t = 0.50 * vnoise(x, y, 4, 4, a) + 0.30 * vnoise(x, y, 8, 8, b) + 0.12 * vnoise(x, y, 32, 32, c)
        return clamp(0.05 + 1.05 * t + 0.07 * (grain(x, y, 11) - 0.5))
    return f, [(0, (0xBF, 0xA2, 0x6C)), (0.5, (0xDE, 0xC8, 0x92)), (1, (0xF2, 0xE6, 0xC0))]


def graph():
    def f(x, y):
        t = 0.0
        if x % 16 == 0 or y % 16 == 0:
            t = 0.45
        if x % 64 == 0 or y % 64 == 0:
            t = 0.80
        return clamp(t + 0.03 * (grain(x, y, 13) - 0.5))
    return f, [(0, (0xF8, 0xF7, 0xF1)), (0.45, (0xBE, 0xD2, 0xE8)), (1, (0x76, 0x9C, 0xCB))]


def dots():
    soft = lattice(4, 4, 71)

    def f(x, y):
        dx, dy = (x % 16) - 7.5, (y % 16) - 7.5
        r = math.hypot(dx, dy)
        dot = clamp(2.6 - r)                          # a soft disc of radius 2.6
        return clamp(0.5 * dot + 0.12 * (vnoise(x, y, 4, 4, soft) - 0.5) + 0.02)
    return f, [(0, (0xE9, 0xEB, 0xEE)), (0.25, (0xDD, 0xE1, 0xE7)), (1, (0x9A, 0xA5, 0xB6))]


def carbon():
    sheen = lattice(2, 2, 81)

    def f(x, y):
        bx, by = x // 8, y // 8
        if (bx + by) & 1:
            u = (y % 8) / 8.0                         # strands run across: shaded down the block
            line = 0.5 + 0.5 * math.cos(2 * math.pi * ((y % 2) / 2.0))
        else:
            u = (x % 8) / 8.0
            line = 0.5 + 0.5 * math.cos(2 * math.pi * ((x % 2) / 2.0))
        shade = 0.5 + 0.5 * math.sin(math.pi * (u * 2 % 1.0))     # a strand per 4 pixels
        t = 0.30 + 0.38 * shade * (0.80 + 0.20 * line) + 0.20 * (vnoise(x, y, 2, 2, sheen) - 0.5)
        return clamp(t)
    return f, [(0, (0x0C, 0x0D, 0x10)), (0.6, (0x2E, 0x31, 0x36)), (1, (0x58, 0x5C, 0x63))]


def stripes():
    def f(x, y):
        t = 0.5 + 0.34 * math.sin(2 * math.pi * x / 32.0) + 0.16 * math.sin(2 * math.pi * x / 128.0 + 0.7)
        return clamp(t + 0.02 * (grain(x, y, 17) - 0.5))
    return f, [(0, (0xC4, 0xD2, 0xE2)), (0.5, (0xDC, 0xE5, 0xEF)), (1, (0xF0, 0xF3, 0xF8))]


def weave():
    fibre_h = lattice(32, N, 91)
    fibre_v = lattice(N, 32, 92)

    def f(x, y):
        bx, by = x // 16, y // 16
        if (bx + by) & 1:
            u = (y % 4 + 0.5) / 4.0                   # four strands across the block
            fib = vnoise(x, y, 32, 128, fibre_h)
        else:
            u = (x % 4 + 0.5) / 4.0
            fib = vnoise(x, y, 128, 32, fibre_v)
        round_ = math.sin(math.pi * u)                # each strand is round: lit in the middle
        t = 0.28 + 0.48 * round_ + 0.20 * (fib - 0.5)
        return clamp(t + 0.04 * (grain(x, y, 19) - 0.5))
    return f, [(0, (0x5E, 0x45, 0x2C)), (0.55, (0xA5, 0x82, 0x58)), (1, (0xD8, 0xBC, 0x8C))]


def blueprint():
    cloud = lattice(4, 4, 101)

    def f(x, y):
        t = 0.10 + 0.07 * (vnoise(x, y, 4, 4, cloud) - 0.5)
        if x % 16 == 0 or y % 16 == 0:
            t = 0.45
        if x % 64 in (0, 1) or y % 64 in (0, 1):
            t = 0.80
        return clamp(t + 0.03 * (grain(x, y, 23) - 0.5))
    return f, [(0, (0x16, 0x35, 0x6C)), (0.12, (0x1B, 0x40, 0x80)), (0.45, (0x4A, 0x7C, 0xC2)),
               (1, (0xBC, 0xD8, 0xF4))]


def baize():
    cloud = lattice(4, 4, 111)
    mid = lattice(16, 16, 112)

    def f(x, y):
        t = 0.50 + 0.24 * (vnoise(x, y, 4, 4, cloud) - 0.5) + 0.16 * (vnoise(x, y, 16, 16, mid) - 0.5)
        return clamp(t + 0.34 * (grain(x, y, 29) - 0.5))
    return f, [(0, (0x12, 0x4A, 0x2A)), (0.5, (0x24, 0x6B, 0x3E)), (1, (0x40, 0x92, 0x5A))]


PATTERNS = [("Linen", linen), ("Paper", paper), ("Slate", slate), ("Denim", denim),
            ("Parchment", parchment), ("Graph", graph), ("Dots", dots), ("Carbon", carbon),
            ("Stripes", stripes), ("Weave", weave), ("Blueprint", blueprint), ("Baize", baize)]


def ramp_palette(stops):
    """256 palette words (&BBGGRR00) from (position, (r, g, b)) stops"""
    pal = []
    for i in range(256):
        p = i / 255.0
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p <= p1 or (p1, c1) == stops[-1]:
                k = clamp((p - p0) / (p1 - p0)) if p1 > p0 else 0.0
                r, g, b = (int(round(c0[n] + (c1[n] - c0[n]) * k)) for n in range(3))
                break
        pal.append(b << 24 | g << 16 | r << 8)
    return pal


def render(name, make):
    f, stops = make()
    pix = [[int(round(f(x, y) * 255)) for x in range(N)] for y in range(N)]
    pal = ramp_palette(stops)
    return pix, pal, desktopart.area([desktopart.sprite(name.lower(), N, N, 8, desktopart.MODE_8BPP, pal, pix)])


def files():
    """(NAME, file bytes, RGB rows) for each wallpaper"""
    out = []
    grey = desktopart.area([desktopart.tile()])
    out.append(("Grey", grey, None))
    for name, make in PATTERNS:
        pix, pal, data = render(name, make)
        rgb = [[((pal[i] >> 8) & 255, (pal[i] >> 16) & 255, (pal[i] >> 24) & 255) for i in row] for row in pix]
        out.append((name, data, rgb))
    return out


def png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def grey_rgb(data):
    sp = desktopart.read_sprites(data)[0][1]
    pal = [(sp[44 + 8 * i + 1], sp[44 + 8 * i + 2], sp[44 + 8 * i + 3]) for i in range(256)]
    img = sp[44 + 2048:]
    return [[pal[img[y * N + x]] for x in range(N)] for y in range(N)]


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if a != "--check"]
    out, pngs = DEFAULT, None
    while args:
        a = args.pop(0)
        if a == "--out":
            out = args.pop(0)
        elif a == "--png":
            pngs = args.pop(0)
        else:
            sys.exit(__doc__)
    bad = 0
    for name, data, rgb in files():
        path = os.path.join(out, name + ",ff9")
        if check:
            cur = open(path, "rb").read() if os.path.exists(path) else b""
            if cur != data:
                print(f"mkwallpapers: {path} differs")
                bad = 1
            continue
        os.makedirs(out, exist_ok=True)
        open(path, "wb").write(data)
        if pngs:
            os.makedirs(pngs, exist_ok=True)
            if rgb is None:
                rgb = grey_rgb(data)
            png(os.path.join(pngs, name + ".png"), [row + row for row in rgb] * 2)
    sys.exit(bad)


if __name__ == "__main__":
    main(sys.argv[1:])
