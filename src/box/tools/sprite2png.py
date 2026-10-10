#!/usr/bin/env python3
"""sprite2png.py <spritefile> <out.png> [name]: a RISC OS sprite file's first
sprite (or the one named) as a PNG -- 1/2/4/8 bpp paletted (its own palette,
or the default desktop/256-colour one), 16 bpp (1:5:5:5) and 32 bpp (&BBGGRR);
old mode numbers and RISC OS 5 mode words.  A screen saved with *ScreenSave
or OS_SpriteOp 2 is such a file (tools/desksnap.sh makes one).  Needs no
PIL: the PNG is written here, RGB, 8 bits a channel, masks ignored."""
import struct
import sys
import zlib

WIMP16 = [0xFFFFFF, 0xDDDDDD, 0xBBBBBB, 0x999999, 0x777777, 0x555555, 0x333333, 0x000000,
          0x004499, 0xEEEE00, 0x00CC00, 0xDD0000, 0xEEEEBB, 0x558800, 0xFFBB00, 0x00BBFF]


def default_palette(bpp):
    if bpp == 1:
        return [0xFFFFFF, 0x000000]
    if bpp == 2:
        return [0xFFFFFF, 0xBBBBBB, 0x777777, 0x000000]
    if bpp == 4:
        return WIMP16
    pal = []                        # the 256-colour mode's: bits 0-1 a tint,
    for i in range(256):            # 2 R2, 3 B2, 4 R3, 5 G2, 6 G3, 7 B3
        t = i & 3
        r = ((i >> 2) & 1 | ((i >> 4) & 1) << 1) << 2 | t
        g = ((i >> 5) & 1 | ((i >> 6) & 1) << 1) << 2 | t
        b = ((i >> 3) & 1 | ((i >> 7) & 1) << 1) << 2 | t
        pal.append((r * 17) << 16 | (g * 17) << 8 | (b * 17))
    return pal


def bpp_of(mode):
    if mode < 256:
        old = {0: 1, 1: 2, 2: 4, 4: 1, 5: 2, 8: 2, 9: 4, 10: 8, 12: 4, 13: 8, 15: 8, 16: 4,
               18: 1, 19: 2, 20: 4, 21: 8, 24: 8, 25: 1, 26: 2, 27: 4, 28: 8, 31: 8, 32: 8}
        return old.get(mode, 8)
    t = (mode >> 27) & 31
    if t == 15:                     # RISC OS 5 extended: log2bpp in bits 20-23
        return 1 << ((mode >> 20) & 15)
    return {1: 1, 2: 2, 3: 4, 4: 8, 5: 16, 6: 32, 7: 32}[t]


def png(path, w, h, rows):
    def chunk(k, d):
        c = struct.pack(">I", len(d)) + k + d
        return c + struct.pack(">I", zlib.crc32(k + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__.split(":")[0])
    data = open(sys.argv[1], "rb").read()
    want = sys.argv[3].lower() if len(sys.argv) > 3 else None
    count, first, _ = struct.unpack_from("<III", data, 0)
    at = first - 4
    for _ in range(count):
        nxt, = struct.unpack_from("<I", data, at)
        name = data[at + 4:at + 16].split(b"\0")[0].decode("latin-1").lower()
        if want is None or name == want:
            break
        at += nxt
    else:
        sys.exit(f"no sprite {want}")
    (nxt, wwords, hm1, fbit, lbit, img, mask, mode) = struct.unpack_from("<I12xIIIIIII", data, at)
    bpp = bpp_of(mode)
    width = ((wwords + 1) * 32 - fbit - (31 - lbit)) // bpp
    height = hm1 + 1
    stride = (wwords + 1) * 4
    pal = None
    if bpp <= 8:
        n = 1 << bpp
        if img > 44:
            words = (img - 44) // 4
            p = struct.unpack_from(f"<{words}I", data, at + 44)
            pal = [(p[2 * i] >> 8) & 0xFFFFFF for i in range(min(n, words // 2))]
            pal = [((c & 0xFF) << 16) | (c & 0xFF00) | (c >> 16) for c in pal]   # &BBGGRR
            if len(pal) < n:            # 8 bpp with 16 entries: the standard 256
                pal = default_palette(bpp)
        else:
            pal = default_palette(bpp)
    rows = []
    for y in range(height):
        base = at + img + y * stride
        line = data[base:base + stride]
        out = bytearray()
        for x in range(width):
            bit = fbit + x * bpp
            if bpp == 32:
                v, = struct.unpack_from("<I", line, bit // 8)
                out += bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF))
            elif bpp == 16:
                v, = struct.unpack_from("<H", line, bit // 8)
                r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
                out += bytes((r * 255 // 31, g * 255 // 31, b * 255 // 31))
            else:
                v = (line[bit // 8] >> (bit % 8)) & ((1 << bpp) - 1)
                c = pal[v] if v < len(pal) else 0
                out += bytes(((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF))
        rows.append(bytes(out))
    png(sys.argv[2], width, height, rows)
    print(f"{sys.argv[2]}: {width}x{height}, {bpp} bpp, mode &{mode:X}")


if __name__ == "__main__":
    main()
