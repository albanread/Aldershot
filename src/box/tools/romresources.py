#!/usr/bin/env python3
"""romresources.py ROM OUTDIR DIR... -- files out of a RISC OS ROM's
ResourceFS, as RISC OS 5.30's emulated ROM modules want them (tools/romwrap.py,
runtime/armrun/arm/README.md): each file under Resources:$.Resources.DIR
for each DIR, written to OUTDIR/DIR/<leaf>,<type> -- the type from the
file's load address (a Messages file is text, &FFF; a Toolbox Res file
&FAE).  The ROM's ResourceFS blocks hold entries of +0 the offset to the
next, +4 load, +8 exec, +12 length, +16 attributes, +20 the name, then the
length + 4 and the data (Kernel's s/ResourceFS).  Prints each file."""
import os
import re
import struct
import sys

rom = open(sys.argv[1], "rb").read()
out = sys.argv[2]
for d in sys.argv[3:]:
    pat = re.compile(rb"Resources\." + re.escape(d.encode()) + rb"\.([A-Za-z0-9_]+)\x00")
    for m in pat.finditer(rom):
        h = m.start() - 20
        nxt, load, exe, length, attr = struct.unpack_from("<5I", rom, h)
        if not (0 < nxt < 0x100000 and length < 0x100000 and (load >> 20) == 0xFFF):
            continue
        name_end = m.end()
        data_at = (name_end + 3) & ~3
        size = struct.unpack_from("<I", rom, data_at)[0] - 4
        if size != length:
            continue
        data = rom[data_at + 4:data_at + 4 + length]
        ftype = (load >> 8) & 0xFFF
        path = os.path.join(out, d, f"{m.group(1).decode()},{ftype:03x}")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(data)
        print(f"Resources.{d}.{m.group(1).decode():10} &{ftype:03X} {length:6} bytes")
