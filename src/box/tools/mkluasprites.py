#!/usr/bin/env python3
"""mkluasprites.py OUT -- the Lua file type's (&18C) icon, drawn here: a
white page with its top right corner folded and a navy band across it,
"Lua" in white on the band, as ports/rospdf/tools/mksprites.py makes the
PDF file type's.  Loaded by the disc's !Boot (IconSprites), so a Lua
file in the Filer shows as one."""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "mksprites", os.path.join(HERE, "..", "ports", "rospdf", "tools", "mksprites.py"))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)

W = H = 34

# a 3 x 5 font, mksprites.py's shape
GLYPHS = {
    "L": ["100", "100", "100", "100", "111"],
    "U": ["101", "101", "101", "101", "111"],
    "A": ["010", "101", "111", "101", "101"],
}

NAVY = (0x10, 0x30, 0x80)
WHITE = (0xFF, 0xFF, 0xFF)
GREY = (0x60, 0x60, 0x60)
FOLD = (0xC8, 0xC8, 0xC8)
MOON = (0x80, 0x90, 0xD0)


def page():
    px = {}
    x0, x1, y0, y1, corner = 6, 27, 1, 32, 7
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if x > x1 - corner + (y - y0):          # the folded corner, cut away
                continue
            edge = x in (x0, x1) or y in (y0, y1) or x == x1 - corner + (y - y0)
            px[x, y] = GREY if edge else WHITE
    for y in range(y0, y0 + corner):                # the fold itself
        for x in range(x1 - corner + (y - y0), x1 + 1):
            if x - (x1 - corner) <= (y - y0) + 1 and y - y0 < corner:
                px[x, y] = FOLD if (x, y) not in px or px[x, y] == WHITE else px[x, y]
        px[x1 - corner + (y - y0), y] = GREY
        px[x1, y0 + corner - 1] = GREY
    for y in range(13, 20):                         # the band
        for x in range(x0 + 1, x1):
            px[x, y] = NAVY
    for i, ch in enumerate("LUA"):                  # the letters
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit == "1":
                    px[9 + i * 5 + gx, 14 + gy] = WHITE
    cx, cy, r = 20, 25, 4                           # Lua's moon, beneath
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                px[x, y] = MOON
    for x in range(cx - 2, cx + 3):                 # its crescent cut
        for y in range(cy - 3, cy + 4):
            if (x - cx + 2) ** 2 + (y - cy) ** 2 <= (r + 1) ** 2:
                if (x, y) in px and px[x, y] == MOON:
                    del px[x, y]
    return px


def main():
    mk.W = mk.H = 34
    body = mk.sprite("file_18c", page())
    open(sys.argv[1], "wb").write(struct.pack("<III", 1, 16, 16 + len(body)) + body)


if __name__ == "__main__":
    main()
