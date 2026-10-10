#!/usr/bin/env python3
"""rommodule.py ROM TITLE OUT -- one module out of a RISC OS ROM image, as the
ROM holds it: the module chain after the kernel, each module preceded by a
word giving its length (that word included).  For the ARM container's
emulated modules (runtime/armrun/arm/README.md): RISC OS 5.30's own
SharedCLibrary, which ARM clients use (design 25 §6a).  Prints the module's
title, help string, length and SHA-256."""
import hashlib
import struct
import sys

rom = open(sys.argv[1], "rb").read()
want = sys.argv[2].encode() + b"\0"


def cstr(at):
    return rom[at:rom.index(b"\0", at)].decode("latin-1")


found = None
for start in range(4, len(rom) - 0x34, 4):
    to = struct.unpack_from("<I", rom, start + 0x10)[0]
    if to and start + to + len(want) <= len(rom) and rom[start + to:start + to + len(want)] == want:
        size = struct.unpack_from("<I", rom, start - 4)[0] - 4
        if 0x34 < size < 0x400000:
            found = (start, size)
            break
if not found:
    sys.exit(f"{sys.argv[1]}: no module titled {sys.argv[2]}")
start, size = found
mod = rom[start:start + size]
open(sys.argv[3], "wb").write(mod)
help_off = struct.unpack_from("<I", mod, 0x14)[0]
print(f"{cstr(start + 0x10 and start + struct.unpack_from('<I', mod, 0x10)[0])} | "
      f"{cstr(start + help_off).replace(chr(9), ' ')} | {size} bytes | sha256 {hashlib.sha256(mod).hexdigest()}")
