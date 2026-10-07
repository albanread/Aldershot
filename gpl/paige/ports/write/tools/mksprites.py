#!/usr/bin/env python3
"""mksprites.py OUT [TOOLS] -- !Write's !Sprites: the application's
icon, its file type's (&0A0) and RTF's (&C32), drawn here; and TOOLS,
its Sprites file (the Toolbox loads it for the application's windows):
the document toolbar's bold, italic and underline, and its four
alignments, 22 pixels square, each with its selected look (name + "p").

New-format sprites 34 x 34 pixels at 90 dpi, 32 bpp (&00BBGGRR) with a
1 bpp mask, as ports/rospdf/tools/mksprites.py makes !PDF's: a white page
with its corner folded and lines of text, a bold "W" at its head; the
application's has a pen across the page.
"""
import struct
import sys

W = H = 34
MODE = 6 << 27 | 90 << 14 | 90 << 1 | 1     # 32 bpp, 90 x 90 dpi

WHITE = (0xFF, 0xFF, 0xFF)
GREY = (0x60, 0x60, 0x60)
FOLD = (0xC8, 0xC8, 0xC8)
INK = (0x20, 0x30, 0x80)
LINE = (0x90, 0x98, 0xA8)
PEN = (0xC0, 0x70, 0x10)
NIB = (0x30, 0x30, 0x30)

W_GLYPH = ["10001", "10001", "10101", "10101", "01010"]


LETTERS = {
    "R": ["110", "101", "110", "101", "101"],
    "T": ["111", "010", "010", "010", "010"],
    "F": ["111", "100", "110", "100", "100"],
}


def page(app, word=None):
    px = {}
    x0, x1, y0, y1, corner = 6, 27, 1, 32, 7
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if x > x1 - corner + (y - y0):
                continue
            edge = x in (x0, x1) or y in (y0, y1) or x == x1 - corner + (y - y0)
            px[x, y] = GREY if edge else WHITE
    for y in range(y0, y0 + corner):
        for x in range(x1 - corner + (y - y0), x1 + 1):
            if x - (x1 - corner) <= (y - y0) + 1:
                px[x, y] = FOLD if (x, y) not in px or px[x, y] == WHITE else px[x, y]
        px[x1 - corner + (y - y0), y] = GREY
        px[x1, y0 + corner - 1] = GREY
    if word:                                        # letters, 3 x 5 doubled
        for i, ch in enumerate(word):
            for gy, row in enumerate(LETTERS[ch]):
                for gx, bit in enumerate(row):
                    if bit == "1":
                        for dx in (0, 1):
                            for dy in (0, 1):
                                px[8 + i * 6 + gx * 2 + dx, 4 + gy * 2 + dy] = PEN
    else:
        for gy, row in enumerate(W_GLYPH):          # the W, doubled
            for gx, bit in enumerate(row):
                if bit == "1":
                    for dx in (0, 1):
                        for dy in (0, 1):
                            px[9 + gx * 2 + dx, 4 + gy * 2 + dy] = INK
    for y in range(16, 31, 3):                       # text lines
        for x in range(x0 + 3, x1 - 3 - (5 if y == 28 else 0)):
            px[x, y] = LINE
    if app:                                          # a pen, top right to bottom left
        for k in range(16):
            x, y = 30 - k, 8 + k
            for t in (-1, 0, 1):
                if 0 <= x + t < W:
                    px[x + t, y] = PEN
        for k in range(3):
            px[14 - k, 24 + k] = NIB
            px[15 - k, 24 + k] = NIB
    return px


TOOL = 22
TOOL_INK = (0x10, 0x10, 0x10)
TOOL_GLYPHS = {
    "tb_bold": [
        "..........",
        "#######...",
        ".##...##..",
        ".##...##..",
        ".##..##...",
        ".######...",
        ".##...##..",
        ".##....##.",
        ".##....##.",
        ".##...##..",
        "#######...",
    ],
    "tb_italic": [
        "..........",
        "....####..",
        ".....##...",
        ".....##...",
        "....##....",
        "....##....",
        "....##....",
        "...##.....",
        "...##.....",
        "...##.....",
        "..####....",
    ],
    "tb_under": [
        "##....##..",
        ".#....#...",
        ".#....#...",
        ".#....#...",
        ".#....#...",
        ".#....#...",
        ".#....#...",
        "..####....",
        "..........",
        "#########.",
        "..........",
    ],
}
# the alignments: five lines, each (start, end) of 18
TOOL_LINES = {
    "tb_left": [(0, 18), (0, 12), (0, 16), (0, 10), (0, 18)],
    "tb_centre": [(0, 18), (3, 15), (1, 17), (4, 14), (0, 18)],
    "tb_right": [(0, 18), (6, 18), (2, 18), (8, 18), (0, 18)],
    "tb_full": [(0, 18), (0, 18), (0, 18), (0, 18), (0, 10)],
}


TOOL_OFF = (0xDD, 0xDD, 0xDD)     # the Wimp's colour 1, the button's own
TOOL_ON = (0x90, 0xA8, 0xD0)      # main.c's TOOL_ON_BG


def tool(name, on=False):
    """a toolbar button's picture: black on a solid ground, grey, or (on)
    blue for its selected look.  The grounds are solid so that main.c can
    paint B, I and U over these in the Font Manager's anti-aliased type
    as Write starts; the pixel letters here are what is seen without it."""
    px = {}
    for y in range(TOOL):
        for x in range(TOOL):
            px[x, y] = TOOL_ON if on else TOOL_OFF
    if name in TOOL_GLYPHS:
        rows = TOOL_GLYPHS[name]
        for gy, row in enumerate(rows):
            for gx, c in enumerate(row):
                if c == "#":
                    for dx in (0, 1):
                        for dy in (0, 1):
                            if name == "tb_under" and dy and gy == 9:
                                continue
                            px[1 + gx * 2 + dx, gy * 2 + dy] = TOOL_INK
    else:
        for i, (a, b) in enumerate(TOOL_LINES[name]):
            for x in range(2 + a, 2 + b):
                for dy in (0, 1):
                    px[x, 2 + i * 4 + dy] = TOOL_INK
    return px


def sprite(name, px, w=W, h=H):
    img = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px.get((x, y), (0, 0, 0))
            img += bytes((r, g, b, 0))
    mask_words = (w + 31) // 32
    mask = bytearray()
    for y in range(h):
        bits = 0
        for x in range(w):
            if (x, y) in px:
                bits |= 1 << x
        mask += bits.to_bytes(mask_words * 4, "little")
    head = 44
    size = head + len(img) + len(mask)
    nm = name.encode("latin-1")[:12].ljust(12, b"\0")
    return struct.pack("<I", size) + nm + struct.pack(
        "<IIIIIII", w - 1, h - 1, 0, 31, head, head + len(img), MODE) + img + mask


def spritefile(path, sprites):
    body = b"".join(sprites)
    data = struct.pack("<III", len(sprites), 16, 16 + len(body)) + body
    with open(path, "wb") as fh:
        fh.write(data)


def main():
    spritefile(sys.argv[1], [sprite("!write", page(True)), sprite("file_0a0", page(False)),
                             sprite("file_c32", page(False, "RTF"))])
    if len(sys.argv) > 2:
        names = list(TOOL_GLYPHS) + list(TOOL_LINES)
        spritefile(sys.argv[2], [sprite(n, tool(n), TOOL, TOOL) for n in names]
                   + [sprite(n + "p", tool(n, True), TOOL, TOOL) for n in names])


if __name__ == "__main__":
    main()
