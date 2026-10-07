#!/usr/bin/env python3
"""swis.py RISCOS OUT -- <swis.h> for POSIX applications (design 22, U-g).

posix/riscos/swis.h.in with the SWI names put in: RISC OS's, as its build's
makehswis makes them from the OS's asm headers (RISCOS is a source drop's
RiscOS directory, the Makefile's BASICVPFSRC), then ROSGD's own from
api/defs -- the lists the C applications' <swis.h> and <rosgdswis.h> have
(tools/capps.py's make_swis_h, over rosasm's header export).  OUT is the
file to write.
"""
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(os.path.dirname(HERE))


def swi_names(riscos):
    """[(name, number)]: RISC OS's in makehswis's order, then ROSGD's"""
    sys.path.insert(0, os.path.join(ROSGD, "tools"))
    sys.path.insert(0, os.path.join(ROSGD, "..", "rosasm", "tools"))
    from export_hdrs import export_hdrs
    import capps
    with tempfile.TemporaryDirectory() as tmp:
        _, hdr_dirs = export_hdrs(riscos, os.path.join(tmp, "hdr"), quiet=True)
        swis, _, _, _, extra = capps.make_swis_h(riscos, hdr_dirs,
                                                 os.path.join(riscos, "Sources/Lib/RISC_OSLib/clib/h"))
    names = []
    for text in (swis, extra):
        names += [(m.group(1), int(m.group(2), 16))
                  for m in re.finditer(r"^#define (\w+) 0x([0-9a-f]+)$", text, re.M)]
    return names


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    riscos, out = sys.argv[1:]
    if not os.path.isdir(os.path.join(riscos, "Sources", "Lib", "RISC_OSLib")):
        raise SystemExit(f"swis.py: no RISC OS sources at {riscos}")
    names = swi_names(riscos)
    with open(os.path.join(HERE, "swis.h.in")) as f:
        head = f.read()
    lines = [f"/* SWI names: RISC OS's and ROSGD's ({len(names)}) */\n"]
    for n, v in names:
        lines.append(f"#undef {n}\n#define {n} 0x{v:x}\n")
    marker = "/* @SWI_NAMES@ */\n"
    if marker not in head:
        raise SystemExit("swis.py: swis.h.in has no @SWI_NAMES@ marker")
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        f.write(head.replace(marker, "".join(lines)))
    os.replace(tmp, out)
    print(f"swis.py: {len(names)} SWI names in {out}")


if __name__ == "__main__":
    main()
