#!/usr/bin/env python3
"""romwrap.py ROM OUTDIR TITLE[=LEAF]... -- RISC OS 5.30's own ROM modules,
for the box to load and run as ARM code (the ARM container, design 25;
runtime/armrun/arm/README.md).

A ROM module is linked where the ROM holds it: its address constants are
absolute, and a copy loaded elsewhere must have them moved.  Each word of
the module that points into [its ROM address, its end + 1 MB] -- a C
module's zero-initialised statics end past its image -- is such a constant
(as an instruction it would be an unconditional coprocessor one, which
these modules never hold -- the rule the emulated SharedCLibrary's loader
uses, box.c).  This finds them once, here, so the list can be read and
checked, and writes the module wrapped:

    +0   "ROSGDROM"
    +8   the ROM address the module is linked at
    +12  the module's length
    +16  n, the number of words to move
    +20  m, the number of words that address SharedCLibrary
    +24  b, the number of branches into SharedCLibrary
    +28  n word offsets, from the module's start
    then m pairs: a word's offset, and the address it holds less the
         library's ROM address (a C module built for the ROM calls the
         library's module entries directly, in the ROM beside it)
    then b pairs: a B's or BL's offset, and its target less the library's
         ROM address -- the ROM build's C calls the library's functions
         with BL (and tail-calls them with B, conditional or not), across
         the ROM: those whose target is an instruction of the library
    then the module, as the ROM holds it

The file is a Module (&FFA): *RMLoad and RMEnsure's loads take it, and the
box's loader (runtime/module.c) moves the words by as much as the module
moved and runs it under the container.  OUTDIR/LEAF,ffa is written for
each TITLE (LEAF defaults to TITLE; a "/" in LEAF makes subdirectories).
Prints each module's help string, length, words moved and SHA-256.
"""
import hashlib
import os
import struct
import sys

rom = open(sys.argv[1], "rb").read()
out = sys.argv[2]
ROM_BASE = 0xFC000000


def cstr(at):
    return rom[at:rom.index(b"\0", at)].decode("latin-1")


def chain():
    """The ROM's module chain: each module preceded by its length word"""
    for p in range(0x10000, len(rom) - 8, 4):
        mods, q = [], p
        while q < len(rom) - 8:
            n = struct.unpack_from("<I", rom, q - 4)[0]
            if n < 0x40 or n > 0x400000:
                break
            t = struct.unpack_from("<I", rom, q + 0x10)[0]
            if not t or t >= n:
                break
            mods.append((q, n - 4, cstr(q + t)))
            q += n
        if len(mods) > 50:
            return mods
    sys.exit("romwrap: no module chain found")


mods = {title: (at, size) for at, size, title in chain()}
SCL_AT, SCL_SIZE = mods["SharedCLibrary"]
SCL = ROM_BASE + SCL_AT
def branch(image, link, off):
    """A B or BL's target, and whether it is unconditional; None if not one"""
    w = struct.unpack_from("<I", image, off)[0]
    if (w >> 25) & 7 != 5 or (w >> 28) == 0xF:
        return None
    d = w & 0xFFFFFF
    d -= 0x1000000 if d & 0x800000 else 0
    return link + off + 8 + 4 * d - SCL, (w >> 28) == 0xE


def into_library(image, link, off):
    """A B or BL's target in the library, if it is one of its instructions"""
    b = branch(image, link, off)
    if b and 0 <= b[0] < SCL_SIZE and (struct.unpack_from("<I", rom, SCL_AT + b[0])[0] >> 28) == 0xE:
        return b[0]
    return None


for spec in sys.argv[3:]:
    title, _, leaf = spec.partition("=")
    leaf = leaf or title
    if title not in mods:
        sys.exit(f"romwrap: no module {title} in the ROM")
    at, size = mods[title]
    image = rom[at:at + size]
    link = ROM_BASE + at
    # into the module, or past its end by up to 1 MB: a C module's
    # zero-initialised statics' limit is after its image (the ROM holds only
    # their initial values' part), and SharedCLibrary sizes them from it
    moves = [off for off in range(0, size - 3, 4)
             if 0 <= struct.unpack_from("<I", image, off)[0] - link <= size + 0x100000]
    beyond = [struct.unpack_from("<I", image, off)[0] - link - size for off in moves
              if struct.unpack_from("<I", image, off)[0] - link > size]
    clib = [(off, struct.unpack_from("<I", image, off)[0] - SCL) for off in range(0, size - 3, 4)
            if 0 <= struct.unpack_from("<I", image, off)[0] - SCL < SCL_SIZE and off not in moves]
    # B/BLs into the library, conditional or not
    branches = [(off, into_library(image, link, off)) for off in range(0, size - 3, 4)
                if off not in moves and into_library(image, link, off) is not None]
    path = os.path.join(out, leaf.replace("/", os.sep) + ",ffa")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(b"ROSGDROM" + struct.pack("<IIIII", link, size, len(moves), len(clib), len(branches)))
        fh.write(struct.pack(f"<{len(moves)}I", *moves))
        for off, to in clib + branches:
            fh.write(struct.pack("<II", off, to))
        fh.write(image)
    help_ = cstr(at + struct.unpack_from("<I", image, 0x14)[0]).replace("\t", " ")
    print(f"{leaf:22} {help_:50} {size:7} bytes, {len(moves):4} words moved, "
          f"{len(beyond)} past its end (up to +{max(beyond) if beyond else 0}), {len(clib)} + {len(branches)} B/BLs into the library, "
          f"sha256 {hashlib.sha256(image).hexdigest()[:16]}")
