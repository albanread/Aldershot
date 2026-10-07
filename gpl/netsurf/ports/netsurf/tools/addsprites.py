#!/usr/bin/env python3
"""addsprites.py TO FROM NAME... -- sprites NAME... copied from sprite file FROM
into sprite file TO (both RISC OS sprite files, ,ff9), replacing any of the
same name.  !NetSurf's !Sprites gets the HTML file's icons, file_faf and
small_faf, from RISC OS 5's own desktop sprites (Wimp's DiscSprites), so
HTML files show as web pages in the Filer once !NetSurf's !Boot has run."""
import struct
import sys


def read(path):
    d = open(path, "rb").read()
    n, first, _free = struct.unpack("<III", d[:12])
    off, out = first - 4, []
    for _ in range(n):
        size = struct.unpack("<I", d[off:off + 4])[0]
        out.append((d[off + 4:off + 16].split(b"\0")[0].decode("latin-1").lower(), d[off:off + size]))
        off += size
    return out


def main():
    to, frm, names = sys.argv[1], sys.argv[2], [n.lower() for n in sys.argv[3:]]
    have = read(to)
    add = {n: b for n, b in read(frm) if n in names}
    missing = [n for n in names if n not in add]
    if missing:
        raise SystemExit("addsprites: not in %s: %s" % (frm, " ".join(missing)))
    sprites = [(n, b) for n, b in have if n not in add] + [(n, add[n]) for n in names]
    body = b"".join(b for _, b in sprites)
    open(to, "wb").write(struct.pack("<III", len(sprites), 16, 16 + len(body) - 4 + 4) + body)


if __name__ == "__main__":
    main()
