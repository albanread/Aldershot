#!/usr/bin/env python3
"""elftext.py ELF OUT -- the executable section of a 32-bit ELF object (rosasm
--elf's one code area) as a flat file: an ARM Absolute's bytes, for the ARM
container's tests (tests/armrun).  It refuses an object with relocations: the
code must be position-independent, as it runs where it is loaded."""
import struct
import sys

elf = open(sys.argv[1], "rb").read()
if elf[:4] != b"\x7fELF" or elf[4] != 1:
    sys.exit(f"{sys.argv[1]}: not a 32-bit ELF file")
shoff, = struct.unpack_from("<I", elf, 32)
shentsize, shnum = struct.unpack_from("<HH", elf, 46)
text = None
for i in range(shnum):
    _, typ, flags, _, off, size = struct.unpack_from("<IIIIII", elf, shoff + i * shentsize)
    if typ in (9, 4):
        sys.exit(f"{sys.argv[1]}: has relocations; the code must be position-independent")
    if typ == 1 and flags & 4:
        if text is not None:
            sys.exit(f"{sys.argv[1]}: more than one code area")
        text = elf[off:off + size]
if text is None:
    sys.exit(f"{sys.argv[1]}: no code area")
open(sys.argv[2], "wb").write(text)
