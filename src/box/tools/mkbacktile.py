#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read
"""mkbacktile.py -- the sprite that hands the desktop's background to the host.

The Metal display's backdrop layer (qemu/ui/metal.m) shows through every
guest pixel that is tagged "below" and is the backdrop key colour.  The tag
lives in the transfer byte of a 32 bpp pixel, the top byte of the word,
which everything the OS draws leaves at 0:

    bits 7-6  layer tag: 10 below, 01 above (reserved), 00 and 11 nothing
    bits 5-0  reserved, 0

This writes a 32 bpp sprite whose every pixel is the key colour, #B7C0B4,
with the transfer byte &80.  Tiled as the Pinboard's backdrop it paints the
tag across the whole background:

    Backdrop -Tile <BoxDisc$Dir>.Resources.Pinboard.BackTile

The kernel's sprite plot and the Wimp's block copies keep all 32 bits, so
the tag survives redraws and window moves.  With no backdrop layer (another
front end, or no backdrop= option) the tile is simply the sage ground.

KEY and TAG_BELOW must stay in step with METAL_BACKDROP_KEY (0x00B4C0B7 as
0x00BBGGRR) and the decode in qemu/ui/metal.m, which tests
(byte 3 & 0xC0) == 0x80 and the colour bits.  !Appearance's Composite
switch plays this file.

256 pixels is the size because tiling makes one sprite plot per tile.  The
file is 262 KB but one colour, so the disc's zip barely grows.

    mkbacktile.py [--out FILE] [--size 256] [--check]

The default FILE is disc/Resources/Pinboard/BackTile,ff9.  --check exits 1
if the file is not what this would write.
"""
import argparse
import os
import struct
import sys

KEY = (0xB7, 0xC0, 0xB4)      # R, G, B: METAL_BACKDROP_KEY in qemu/ui/metal.m
TAG_BELOW = 0x80              # transfer byte: layer tag 10, reserved bits 0
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(os.path.dirname(HERE), "disc", "Resources", "Pinboard", "BackTile,ff9")


def tile(size):
    r, g, b = KEY
    word = struct.pack("<I", r | (g << 8) | (b << 16) | (TAG_BELOW << 24))
    body = word * (size * size)
    hdr = bytearray(44)
    struct.pack_into("<i", hdr, 0, 44 + len(body))          # to the next sprite
    struct.pack_into("<12s", hdr, 4, b"backtile")
    struct.pack_into("<7i", hdr, 16,
                     size - 1,                              # width in words - 1
                     size - 1,                              # height - 1
                     0, 31,                                 # first and last bit used
                     44, 44,                                # image; mask = image: none
                     1 | (90 << 1) | (90 << 14) | (6 << 27))  # 32 bpp, 90 dpi
    sprite = bytes(hdr) + body
    return struct.pack("<III", 1, 16, 16 + len(sprite)) + sprite


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=DEFAULT, help="the sprite file to write (,ff9)")
    ap.add_argument("--size", type=int, default=256, help="tile side in pixels (default 256)")
    ap.add_argument("--check", action="store_true", help="exit 1 if the file differs")
    a = ap.parse_args()
    data = tile(a.size)
    if a.check:
        cur = open(a.out, "rb").read() if os.path.exists(a.out) else b""
        sys.exit(0 if cur == data else 1)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    open(a.out, "wb").write(data)
    print(f"mkbacktile: {a.out}: {a.size}x{a.size} 32bpp, #{KEY[0]:02X}{KEY[1]:02X}{KEY[2]:02X}, "
          f"transfer byte &{TAG_BELOW:02X}, {len(data)} bytes")


if __name__ == "__main__":
    main()
