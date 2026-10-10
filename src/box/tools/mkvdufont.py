#!/usr/bin/env python3
"""mkvdufont.py VDUFONTL1 OUT.c -- the kernel's system font, as C.

VDUFONTL1 is Kernel/s/vdu/vdufontl1 from RISC OS 5's sources (Apache 2.0,
Copyright 1996 Acorn Computers Ltd): HardFont, characters 32 to 255, eight
bytes each, the top row first, bit 7 the leftmost pixel.  The output is
committed (runtime/vdu/font.c), so a build needs no RISC OS sources; run this
again only to take a newer font.
"""
import re
import sys


def main():
    src, dest = sys.argv[1], sys.argv[2]
    rows = []
    started = False
    for line in open(src, encoding="latin-1"):
        if line.startswith("HardFont"):
            started = True
            continue
        if not started:
            continue
        m = re.match(r"\s*=\s*((?:&[0-9A-Fa-f]{2},){7}&[0-9A-Fa-f]{2})\s*;?(.*)", line)
        if m:
            bytes_ = [int(b[1:], 16) for b in m.group(1).split(",")]
            name = m.group(2).strip().encode("ascii", "replace").decode()
            rows.append((bytes_, name))
    if len(rows) != 224:
        sys.exit(f"mkvdufont: {len(rows)} characters, want 224")
    hdr = ['/* Copyright 1996 Acorn Computers Ltd', ' *', ' * Licensed under the Apache License, Version 2.0 (the "License");', ' * you may not use this file except in compliance with the License.', ' * You may obtain a copy of the License at', ' *', ' *     http://www.apache.org/licenses/LICENSE-2.0', ' *', ' * Unless required by applicable law or agreed to in writing, software', ' * distributed under the License is distributed on an "AS IS" BASIS,', ' * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.', ' * See the License for the specific language governing permissions and', ' * limitations under the License.', ' *', " * This file is a conversion into C of the font data in RISC OS Open's Kernel", ' * source (Sources/Kernel: s.vdu.vdufontl1).', ' */']
    out = hdr + ["/* font.c -- the kernel's system font, characters 32 to 255. It is generated", " * by tools/mkvdufont.py from RISC OS 5's Kernel/s/vdu/vdufontl1. Each", ' * character is eight bytes, with the top row first and bit 7 the leftmost', ' * pixel. */',
           "#include <stdint.h>", "",
           "const uint8_t ros_vdu_hard_font[224][8] = {"]
    for i, (b, name) in enumerate(rows):
        out.append("    { " + ", ".join(f"0x{x:02X}" for x in b) + f" }},  /* {32 + i:3d} {name[:48]} */")
    out.append("};")
    open(dest, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
