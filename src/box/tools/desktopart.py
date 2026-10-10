#!/usr/bin/env python3
"""desktopart.py [--check] WIMPSPRITES PINBOARDTILE -- ROSGD's own desktop art, drawn here.

    taskmanager, switcher
               the Task Manager's icon-bar sprite (the Switcher's icondef names
               "taskmanager"; "switcher" is its older name), replaced in WIMPSPRITES (the
               Wimp's sprite pool, resources/Resources/Wimp/Sprites,ff9) by a
               grey cog: the raspberry's own format -- 42 x 42 pixels, 4 bpp
               with a 16-entry palette of greys and a 1 bpp mask, 90 dpi --
               anti-aliased against the icon bar's grey, lit from the top left
    Tile       the desktop's wallpaper, PINBOARDTILE (resources/Resources/
               Pinboard/Tile,ff9): a seamless 128 x 128 grey tile, 8 bpp with a
               256-grey palette, 90 dpi, which the desktop's start gives
               Pinboard as *Backdrop -Tile

Both are computed, not painted: the same bytes every run, so the ROM stays
reproducible and the art can be changed here and regenerated.  --check
exits 1 if either file is not what this would write."""
import math
import struct
import sys

ICONS = ("taskmanager", "switcher")     # the Task Manager's icon bar sprite, and its old name
MODE_4BPP = (3 << 27) | (90 << 14) | (90 << 1) | 1     # new-format mode words, 90 x 90 dpi
MODE_8BPP = (4 << 27) | (90 << 14) | (90 << 1) | 1


def sprite(name, w, h, bpp, mode, palette, pixels, mask=None):
    """One sprite: header, palette (each entry twice, &BBGGRR00), image rows
    word-aligned, then a 1 bpp mask (new-format sprites' kind), rows word-aligned"""
    row_bits = w * bpp
    words = (row_bits + 31) // 32
    lastbit = (row_bits - 1) % 32
    img = bytearray()
    for y in range(h):
        bits = 0
        for x in range(w):
            bits |= pixels[y][x] << (x * bpp)
        img += bits.to_bytes(words * 4, "little")
    pal = b"".join(struct.pack("<II", c, c) for c in palette)
    msk = b""
    if mask is not None:
        mwords = (w + 31) // 32
        for y in range(h):
            bits = 0
            for x in range(w):
                bits |= (1 if mask[y][x] else 0) << x
            msk += bits.to_bytes(mwords * 4, "little")
    head = 44 + len(pal)
    size = head + len(img) + len(msk)
    return (struct.pack("<I", size) + name.encode("latin-1").ljust(12, b"\0") +
            struct.pack("<7I", words - 1, h - 1, 0, lastbit, head,
                        head + len(img) if mask is not None else head, mode) + pal + bytes(img) + msk)


def grey(v):
    v = max(0, min(255, int(round(v))))
    return v << 24 | v << 16 | v << 8           # &BBGGRR00


def area(sprites):
    """A sprite file: the area's count, first and free offsets, then the sprites"""
    body = b"".join(sprites)
    return struct.pack("<III", len(sprites), 16, 16 + len(body)) + body


def read_sprites(data):
    n, first, _ = struct.unpack("<3I", data[:12])
    off, out = first - 4, []
    for _ in range(n):
        size, = struct.unpack("<I", data[off:off + 4])
        name = data[off + 4:off + 16].split(b"\0")[0].decode("latin-1")
        out.append((name, data[off:off + size]))
        off += size
    return out


# ---- the cog -----------------------------------------------------------------------

def cog(name):
    W = H = 42
    cx = cy = 20.5
    R_OUT, R_ROOT, R_HOLE, TEETH = 20.0, 15.4, 5.8, 8
    BG = 0xDD                                     # the icon bar's grey, for the edges

    def inside(x, y):
        dx, dy = x - cx, y - cy
        r = math.hypot(dx, dy)
        if r < R_HOLE:
            return False
        if r <= R_ROOT:
            return True
        if r > R_OUT:
            return False
        a = (math.atan2(dy, dx) / (2 * math.pi) * TEETH) % 1.0   # position within a tooth period
        half = 0.31 - 0.09 * (r - R_ROOT) / (R_OUT - R_ROOT)      # teeth taper outwards
        return abs(a - 0.5) < half

    def shade(x, y):
        dx, dy = x - cx, y - cy
        r = math.hypot(dx, dy) or 1.0
        light = (-dx - dy) / (r * math.sqrt(2))                   # lit from the top left (y down)
        v = 150 + 40 * light * min(1.0, r / R_ROOT)
        if abs(r - R_HOLE) < 1.4:                                 # the hole's rim, bevelled the other way
            v = 150 - 45 * light
        if r > R_ROOT - 3.2 and r <= R_ROOT:                      # a groove round the hub
            v -= 22
        return v

    S = 4
    pix, lum = [], []
    for y in range(H):
        prow, lrow = [], []
        for x in range(W):
            cov, tot = 0, 0.0
            for sy in range(S):
                for sx in range(S):
                    px, py = x + (sx + 0.5) / S, y + (sy + 0.5) / S
                    if inside(px, py):
                        cov += 1
                        tot += shade(px, py)
            c = cov / (S * S)
            v = (tot / cov if cov else BG) * c + BG * (1 - c)
            # a dark edge: covered pixels next to uncovered ones
            prow.append(c)
            lrow.append(v)
        pix.append(prow)
        lum.append(lrow)
    mask = [[c >= 0.35 for c in row] for row in pix]
    edge = [[mask[y][x] and any(not (0 <= y + j < H and 0 <= x + i < W and mask[y + j][x + i])
                                for i, j in ((1, 0), (-1, 0), (0, 1), (0, -1)))
             for x in range(W)] for y in range(H)]
    levels = [0x38 + i * 0x0C for i in range(16)]                 # &38..&E4, 16 greys
    img = []
    for y in range(H):
        row = []
        for x in range(W):
            v = lum[y][x] - (52 if edge[y][x] else 0)
            row.append(min(range(16), key=lambda i: abs(levels[i] - v)))
        img.append(row)
    return sprite(name, W, H, 4, MODE_4BPP, [grey(v) for v in levels], img, mask)


# ---- the tile ------------------------------------------------------------------------

def tile():
    N = 128
    # periodic value noise, three octaves, and bevelled 64 px tiles: seamless because every term repeats on 128
    def lattice(n, seed):
        vals = []
        s = seed
        for _ in range(n * n):
            s = (s * 1103515245 + 12345) & 0x7FFFFFFF
            vals.append((s >> 8) / float(1 << 23))
        return lambda i, j: vals[(j % n) * n + (i % n)]

    def smooth(t):
        return t * t * (3 - 2 * t)

    def noise(x, y, n, f):
        gx, gy = x * n / N, y * n / N
        i, j = int(gx), int(gy)
        fx, fy = smooth(gx - i), smooth(gy - j)
        a, b, c, d = f(i, j), f(i + 1, j), f(i, j + 1), f(i + 1, j + 1)
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

    octaves = [(4, lattice(4, 7), 0.5), (16, lattice(16, 11), 0.3), (64, lattice(64, 13), 0.2)]
    img = []
    for y in range(N):
        row = []
        for x in range(N):
            n = sum(w * noise(x, y, k, f) for k, f, w in octaves)
            v = 0x78 + (n - 0.5) * 34                              # around the desktop's &77
            sx, sy = x % 64, y % 64                                # 64 px tiles, bevelled
            if sx == 0 or sy == 0:
                v -= 26                                            # the joint
            elif sx == 1 or sy == 1:
                v += 16                                            # lit top and left edges
            elif sx == 63 or sy == 63:
                v -= 12                                            # shaded bottom and right edges
            row.append(max(0, min(255, int(round(v)))))
        img.append(row)
    return sprite("tile", N, N, 8, MODE_8BPP, [grey(i) for i in range(256)], img)


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if a != "--check"]
    if len(args) != 2:
        sys.exit(__doc__)
    wimp, tilefile = args
    old = open(wimp, "rb").read()
    sprites = [(n, cog(n) if n in ICONS else b) for n, b in read_sprites(old)]
    if sum(n in ICONS for n, _ in sprites) != len(ICONS):
        sys.exit(f"desktopart: {wimp} lacks one of {ICONS}")
    new_wimp = area([b for _, b in sprites])
    new_tile = area([tile()])
    if check:
        cur_tile = open(tilefile, "rb").read() if __import__("os").path.exists(tilefile) else b""
        sys.exit(0 if (new_wimp == old and new_tile == cur_tile) else 1)
    open(wimp, "wb").write(new_wimp)
    open(tilefile, "wb").write(new_tile)


if __name__ == "__main__":
    main(sys.argv[1:])
