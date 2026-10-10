#!/usr/bin/env python3
"""mkbreakoutsprites.py OUT -- !Breakout's !Sprites: the application's icon,
!breakout and sm!breakout, drawn here (34 x 34 and 17 x 17 pixels, 32 bpp
with a 1 bpp mask, as ports/rospdf/tools/mksprites.py makes !PDF's): a dark
screen with rows of coloured bricks, a bat and a ball."""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "mksprites", os.path.join(HERE, "..", "ports", "rospdf", "tools", "mksprites.py"))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)

ROWS = [(230, 60, 60), (240, 140, 40), (230, 210, 50), (70, 190, 70), (60, 140, 230)]


def icon(scale):
    n = 34 // scale
    px = {}
    for y in range(n):
        for x in range(n):
            px[(x, y)] = (20, 20, 50)                       # the screen
    for x in range(n):                                      # its frame
        px[(x, 0)] = px[(x, n - 1)] = (150, 150, 170)
    for y in range(n):
        px[(0, y)] = px[(n - 1, y)] = (150, 150, 170)
    bw = max(2, 6 // scale)
    for r, col in enumerate(ROWS):                          # the bricks
        y0 = 3 // scale + r * max(2, 3 // scale)
        for x in range(2, n - 2):
            if (x - 2) % (bw + 1) != bw and y0 + 1 < n:
                px[(x, y0)] = col
                if scale == 1:
                    px[(x, y0 + 1)] = col
    by = n - 4 // scale - 1                                 # the bat
    for x in range(n // 2 - 6 // scale, n // 2 + 6 // scale):
        px[(x, by)] = (210, 210, 230)
        if scale == 1:
            px[(x, by - 1)] = (210, 210, 230)
    bx, byy = n // 2 + 5 // scale, n // 2 + 4 // scale      # the ball
    for dy in range(-1, 2) if scale == 1 else [0]:
        for dx in range(-1, 2) if scale == 1 else [0]:
            px[(bx + dx, byy + dy)] = (255, 255, 255)
    return n, px


def sprite(name, n, px):
    mk.W = mk.H = n
    return mk.sprite(name, px)


def main():
    a = sprite("!breakout", *icon(1))
    b = sprite("sm!breakout", *icon(2))
    body = a + b
    open(sys.argv[1], "wb").write(struct.pack("<III", 2, 16, 16 + len(body)) + body)


if __name__ == "__main__":
    main()
