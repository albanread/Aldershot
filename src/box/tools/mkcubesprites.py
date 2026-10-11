#!/usr/bin/env python3
"""mkcubesprites.py -- the Cube theme's icons (design 35): square,
bevelled grey tiles, each with an emblem, the drives as dark disk units
with a slot and a light, in the spirit of a 1990s Unix workstation's
desktop.  Drawn here, pixel by pixel; nothing is copied from anyone's
artwork.

    tools/mkcubesprites.py ROMSPRITES OUT

ROMSPRITES is the ROM's Resources.Wimp.Sprites; OUT gets a copy of it with
the sprites named below replaced (or added) by 32 bpp tiles: a theme's
Sprites take the ROM sprite area's place, so the copy keeps every other
icon the desktop needs.  Large icons are 34 pixels square, small ones 17,
at 90 dpi (2 OS units a pixel), as the ROM's are.
"""

import struct
import sys

LIGHT, FACE, SHADE, DARK = (0xF4, 0xF4, 0xF4), (0xA0, 0xA0, 0xA0), (0x60, 0x60, 0x60), (0x00, 0x00, 0x00)


class Canvas:
    def __init__(self, n):
        self.n = n
        self.px = [[FACE] * n for _ in range(n)]

    def set(self, x, y, c):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1):
            for x in range(x0, x1):
                self.set(x, y, c)

    def frame(self, x0, y0, x1, y1, c):
        for x in range(x0, x1):
            self.set(x, y0, c)
            self.set(x, y1 - 1, c)
        for y in range(y0, y1):
            self.set(x0, y, c)
            self.set(x1 - 1, y, c)

    def bevel(self, x0, y0, x1, y1, depth, raised=True):
        lt, dk = (LIGHT, DARK) if raised else (DARK, LIGHT)
        for k in range(depth):
            for x in range(x0 + k, x1 - k):
                self.set(x, y0 + k, lt)
                self.set(x, y1 - 1 - k, dk)
            for y in range(y0 + k, y1 - k):
                self.set(x0 + k, y, lt)
                self.set(x1 - 1 - k, y, dk)


def tile(n):
    c = Canvas(n)
    c.bevel(0, 0, n, n, 2 if n >= 24 else 1)
    return c


def s(n, v):
    """a measure on a 34-pixel tile, scaled to n"""
    return max(1, round(v * n / 34))


def drive(n, light=(0x40, 0xE0, 0x60)):
    """a disk unit: two bays, each with a slot and a light"""
    c = tile(n)
    x0, y0, x1, y1 = s(n, 4), s(n, 7), n - s(n, 4), n - s(n, 7)
    c.rect(x0, y0, x1, y1, (0x26, 0x26, 0x2A))
    c.bevel(x0, y0, x1, y1, 1)
    mid = (y0 + y1) // 2
    c.rect(x0 + 1, mid, x1 - 1, mid + 1, (0x50, 0x50, 0x56))
    for top in (y0, mid):
        sy = top + (mid - y0) // 2 - s(n, 1)
        c.rect(x0 + s(n, 3), sy, x1 - s(n, 9), sy + max(2, s(n, 2)), (0x05, 0x05, 0x05))
        c.rect(x1 - s(n, 6), sy, x1 - s(n, 3), sy + max(2, s(n, 2)), light)
    return c


def folder(n):
    c = tile(n)
    body, edge = (0xD8, 0xB8, 0x70), (0x3A, 0x2C, 0x10)
    c.rect(s(n, 5), s(n, 8), s(n, 15), s(n, 12), body)
    c.rect(s(n, 5), s(n, 11), n - s(n, 5), n - s(n, 7), body)
    for k in range(max(1, s(n, 2))):
        c.frame(s(n, 5) + k, s(n, 11) + k, n - s(n, 5) - k, n - s(n, 7) - k, edge)
    return c


def application(n):
    c = tile(n)
    x0, y0, x1, y1 = s(n, 5), s(n, 6), n - s(n, 5), n - s(n, 6)
    c.rect(x0, y0, x1, y1, (0xF8, 0xF8, 0xF8))
    c.rect(x0, y0, x1, y0 + s(n, 6), (0x10, 0x10, 0x10))
    c.rect(x0 + s(n, 3), y0 + s(n, 9), x1 - s(n, 3), y0 + s(n, 11), (0x60, 0x60, 0x60))
    c.rect(x0 + s(n, 3), y0 + s(n, 13), x1 - s(n, 8), y0 + s(n, 15), (0x60, 0x60, 0x60))
    for k in range(max(1, s(n, 2))):
        c.frame(x0 + k, y0 + k, x1 - k, y1 - k, DARK)
    return c


def page(n, mark):
    c = tile(n)
    x0, y0, x1, y1 = s(n, 7), s(n, 4), n - s(n, 7), n - s(n, 4)
    c.rect(x0, y0, x1, y1, (0xFC, 0xFC, 0xF8))
    for k in range(max(1, s(n, 2))):
        c.frame(x0 + k, y0 + k, x1 - k, y1 - k, (0x30, 0x30, 0x30))
    ink = (0x20, 0x20, 0x20)
    w2 = max(2, s(n, 2))
    if mark == "text":
        for k in range(4):
            y = y0 + s(n, 6) + k * s(n, 5)
            c.rect(x0 + s(n, 4), y, x1 - s(n, 4) - (s(n, 5) if k == 3 else 0), y + w2, ink)
    elif mark == "basic":
        c.rect(x0 + s(n, 4), y0 + s(n, 6), x0 + s(n, 4) + w2 + 1, y1 - s(n, 6), (0x20, 0x40, 0xC0))
        for k in range(3):
            y = y0 + s(n, 6) + k * s(n, 7)
            c.rect(x0 + s(n, 9), y, x1 - s(n, 4), y + w2, ink)
    elif mark == "obey":
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        c.rect(cx - s(n, 6), cy - w2 // 2 - 1, cx + s(n, 6), cy + w2 // 2 + 1, ink)
        c.rect(cx - w2 // 2 - 1, cy - s(n, 6), cx + w2 // 2 + 1, cy + s(n, 6), ink)
    elif mark == "sprite":
        for k, col in enumerate([(0xE0, 0x30, 0x30), (0x30, 0xB0, 0x40), (0x30, 0x50, 0xE0)]):
            c.rect(x0 + s(n, 3) + k * s(n, 6), y1 - s(n, 13), x0 + s(n, 8) + k * s(n, 6), y1 - s(n, 5), col)
    elif mark == "data":
        for k in range(3):
            y = y0 + s(n, 6) + k * s(n, 7)
            for x in range(x0 + s(n, 4), x1 - s(n, 4), max(3, s(n, 4))):
                c.rect(x, y, x + w2, y + w2, ink)
    elif mark == "config":
        c.rect(x0 + s(n, 3), y0 + s(n, 7), x1 - s(n, 3), y0 + s(n, 13), (0x26, 0x26, 0x2A))
        c.rect(x0 + s(n, 5), y0 + s(n, 9), x0 + s(n, 10), y0 + s(n, 11), (0x40, 0xE0, 0x60))
    return c


ICONS = {
    "harddisc": lambda n: drive(n),
    "floppydisc": lambda n: drive(n, (0xE0, 0xB0, 0x40)),
    "cddisc": lambda n: drive(n, (0x60, 0x90, 0xE0)),
    "network": lambda n: drive(n, (0xE0, 0x70, 0x40)),
    "fileserver": lambda n: drive(n, (0xE0, 0x70, 0x40)),
    "directory": folder,
    "directoryo": folder,
    "application": application,
    "file_xxx": lambda n: page(n, None),
    "file_fff": lambda n: page(n, "text"),
    "file_ffb": lambda n: page(n, "basic"),
    "file_feb": lambda n: page(n, "obey"),
    "file_ff9": lambda n: page(n, "sprite"),
    "file_ffd": lambda n: page(n, "data"),
    "file_ff2": lambda n: page(n, "config"),
}
SMALL = {"small_dir": folder, "small_diro": folder, "small_app": application,
         "small_xxx": lambda n: page(n, None), "small_fff": lambda n: page(n, "text"),
         "small_ffb": lambda n: page(n, "basic"), "small_feb": lambda n: page(n, "obey")}

MODE_32BPP_90DPI = (6 << 27) | (90 << 14) | (90 << 1) | 1


def sprite_bytes(name, canvas):
    n = canvas.n
    img = bytearray()
    for y in range(n):                      # the top row first
        for x in range(n):
            r, g, b = canvas.px[y][x]
            img += struct.pack("<I", r | g << 8 | b << 16)
    nm = name.encode("latin-1")[:12].ljust(12, b"\0")
    head = struct.pack("<I", 44 + len(img)) + nm + struct.pack(
        "<IIIIIII", n - 1, n - 1, 0, 31, 44, 44, MODE_32BPP_90DPI)
    return head + bytes(img)


def read_sprites(path):
    d = open(path, "rb").read()
    count, first, free = struct.unpack("<III", d[:12])
    off, out = first - 4, []
    for _ in range(count):
        nxt = struct.unpack("<I", d[off:off + 4])[0]
        name = d[off + 4:off + 16].split(b"\0")[0].decode("latin-1")
        out.append((name, d[off:off + nxt]))
        off += nxt
    return out


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    sprites = read_sprites(argv[0])
    made = {name: sprite_bytes(name, f(34)) for name, f in ICONS.items()}
    made.update({name: sprite_bytes(name, f(17)) for name, f in SMALL.items()})
    out, seen = [], set()
    for name, raw in sprites:
        out.append(made.get(name, raw))
        seen.add(name)
    out += [raw for name, raw in made.items() if name not in seen]
    body = b"".join(out)
    open(argv[1], "wb").write(struct.pack("<III", len(out), 16, 16 + len(body)) + body)
    print(f"mkcubesprites: {len(out)} sprites, {len(made)} of them tiles, -> {argv[1]}")


if __name__ == "__main__":
    main(sys.argv[1:])
