#!/usr/bin/env python3
"""mkmanual.py SRCDIR OUT -- a StrongHelp manual (type &3D6) from a
directory of page files: BOX's own manuals, docs/manuals/<Name>, made into
disc/Utilities/!Manuals/Root/<Name>,3d6 by make manuals.

Every file under SRCDIR is an entry of the manual by its name: a page (a
text file), or with a ",ttt" suffix a file of that type (a sprite file,
",ff9", or a Draw file, ",aff").  A subdirectory is a directory of the
manual, as in the OS manual's "Squash_".  Files and directories whose
names start with "." are left out.  The format is StrongHelp's manual
format; tests/stronghelp/mkman.py is the same writer, for tests.

Every entry is dated by EPOCH (the environment's SOURCE_DATE_EPOCH, else
the time now), so the same pages make the same manual."""
import os, re, struct, sys, time

def pad(b):
    return b + b"\0" * (-len(b) % 4)

def riscos_time(t):
    """Unix seconds to RISC OS centiseconds since 1900, 40 bits"""
    return (int(t) + 2208988800) * 100 & 0xFFFFFFFFFF

def load_exec(ftype, stamp):
    return 0xFFF00000 | ftype << 8 | stamp >> 32, stamp & 0xFFFFFFFF

def build(src, stamp):
    out = bytearray(b"\0" * 44)

    def put_dir(path):
        entries = []
        for leaf in sorted(os.listdir(path), key=str.lower):
            if leaf.startswith("."):
                continue
            full = os.path.join(path, leaf)
            if os.path.isdir(full):
                off, size = put_dir(full)
                load, exe = load_exec(0xFFD, stamp)
                entries.append((off, load, exe, size, 0x100 | 0x13, leaf))
                continue
            m = re.match(r"(.*),([0-9a-fA-F]{3})$", leaf)
            name, ftype = (m.group(1), int(m.group(2), 16)) if m else (leaf, 0xFFF)
            data = open(full, "rb").read()
            if ftype == 0xFFF:                  # pages: LF line ends, no trailing spaces
                data = data.replace(b"\r\n", b"\n")
            off = len(out)
            out.extend(pad(b"DATA" + struct.pack("<I", 8 + len(data)) + data))
            load, exe = load_exec(ftype, stamp)
            entries.append((off, load, exe, 8 + len(data), 0x13, name))
        body = b""
        for off, load, exe, ln, fl, name in entries:
            body += struct.pack("<6I", off, load, exe, ln, fl, 0) + pad(name.encode("latin-1") + b"\0")
        off = len(out)
        size = 12 + len(body)
        out.extend(b"DIR$" + struct.pack("<II", size, size) + body)
        return off, size

    root, size = put_dir(src)
    load, exe = load_exec(0xFFD, stamp)
    out[0:44] = (b"HELP" + struct.pack("<IIi", 44, 290, -1)
                 + struct.pack("<6I", root, load, exe, size, 0x100 | 0x13, 0) + b"$\0\0\0")
    return bytes(out)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[0])
    src, dst = sys.argv[1:]
    if not os.path.isfile(os.path.join(src, "!Root")):
        sys.exit("mkmanual: %s has no !Root page" % src)
    stamp = riscos_time(os.environ.get("SOURCE_DATE_EPOCH") or time.time())
    data = build(src, stamp)
    open(dst, "wb").write(data)
    print("mkmanual: %s, %d bytes" % (dst, len(data)))
