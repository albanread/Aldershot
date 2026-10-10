#!/usr/bin/env python3
"""mkbrowsersprites.py OUT -- the browser's sprites (34 x 34 and 17 x 17
pixels, 32 bpp with a 1 bpp mask, as ports/rospdf/tools/mksprites.py makes
!PDF's):

  !browser, sm!browser    the application's icon: a globe, blue sea, green
                          land, with its meridians and equator
  file_faf, small_faf     an HTML page's: that globe on a page, which the
                          Filer shows for one when WaylandWindows has put
                          these in the Wimp's pool (*IconSprites)
  file_b28, small_b28     a URL file's -- an address saved out of a browser
                          window -- the same page, on blue

!Browser's !Sprites is this file; so is the ROM's
Resources:$.Resources.Browser.Sprites, which the browser's RISC OS side
loads (modules/waylandwin/browser.c)."""
import importlib.util
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "mksprites", os.path.join(HERE, "..", "ports", "rospdf", "tools", "mksprites.py"))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)


def icon(n):
    px = {}
    c = (n - 1) / 2
    r = n / 2 - 0.5
    for y in range(n):
        for x in range(n):
            dx, dy = (x - c) / r, (y - c) / r
            d = dx * dx + dy * dy
            if d > 1.0:
                continue                                   # outside: masked
            if d > 0.80:
                px[(x, y)] = (20, 60, 120)                 # the rim
                continue
            shade = int(40 * (1 - d))
            col = (40 + shade, 110 + shade, 210 + shade // 2)    # the sea
            # land: two blobs, as a child draws continents
            if (dx + 0.25) ** 2 + (dy + 0.25) ** 2 < 0.13 or (dx - 0.35) ** 2 + (dy - 0.3) ** 2 < 0.08:
                col = (70 + shade, 170 + shade, 80)
            # the meridians and the equator
            lon = math.asin(max(-1.0, min(1.0, dx / math.sqrt(max(1e-6, 1 - dy * dy)))))
            if abs(dy) < 0.6 / r or any(abs(lon - m) < 0.9 / r for m in (-0.8, 0.0, 0.8)):
                col = tuple(min(255, v + 70) for v in col)
            px[(x, y)] = col
    return n, px


def page(n, paper=(255, 255, 255)):
    """A file's icon: a page with its top right corner turned down, the
    globe on it (RISC OS's file_ttt shape)."""
    px = {}
    l, r, t, b = n // 8, n - n // 8, 1, n - 1
    fold = max(3, n // 4)
    edge = (90, 90, 90)
    turned = tuple(v * 4 // 5 for v in paper)
    for y in range(t, b):
        for x in range(l, r):
            if y - t < fold and r - x <= fold - (y - t):
                continue                                   # the corner that is gone
            on_fold = y - t < fold and r - x == fold - (y - t) + 1
            if x in (l, r - 1) or y in (t, b - 1) or on_fold:
                px[(x, y)] = edge
            elif y - t < fold and r - x < fold:
                px[(x, y)] = turned
            else:
                px[(x, y)] = paper
    # the globe in the middle, a little below centre, the application's colours
    gn = max(7, n // 2 + n // 8)
    _, g = icon(gn)
    ox, oy = (n - gn) // 2, (n - gn) // 2 + n // 8
    for (x, y), col in g.items():
        if l < x + ox < r - 1 and t < y + oy < b - 1:
            px[(x + ox, y + oy)] = col
    return n, px


def sprite(name, n, px):
    mk.W = mk.H = n
    return mk.sprite(name, px)


def main():
    blue = (205, 225, 255)
    sprites = (sprite("!browser", *icon(34)), sprite("sm!browser", *icon(17)),
               sprite("file_faf", *page(34)), sprite("small_faf", *page(17)),
               sprite("file_b28", *page(34, blue)), sprite("small_b28", *page(17, blue)))
    body = b"".join(sprites)
    open(sys.argv[1], "wb").write(
        struct.pack("<III", len(sprites), 16, 16 + len(body)) + body)


if __name__ == "__main__":
    main()
