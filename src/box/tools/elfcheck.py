#!/usr/bin/env python3
"""elfcheck.py -- every ELF image in a tree is the box's machine.

    tools/elfcheck.py --machine x86_64|aarch64 TREE... [--remove]

An x86-64 image on the Apple Silicon box's share or in its ROM cannot run
there, and one would get there silently: disc/ carries !PipeDream's x32
!RunImage, which run-vz.sh's refresh of the share copied back over the
A64X32 one (design 26 P6).  This finds every file that starts with ELF's
magic and names each whose e_machine is not the box's -- x32 and LP64
x86-64 images are both EM_X86_64 (62), A64X32 and LP64 AArch64 both
EM_AARCH64 (183).  With --remove, each such file is deleted.  Exit status
0 when none is left, 1 when any was found (and not removed), 2 on misuse.
"""
import argparse
import os
import struct
import sys

MACHINES = {"x86_64": 62, "aarch64": 183}
NAMES = {62: "x86-64", 183: "AArch64", 40: "ARM (AArch32)"}


def machine(path):
    """e_machine of the ELF image at path, or None if it isn't one"""
    try:
        with open(path, "rb") as f:
            head = f.read(20)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return None
    order = "<" if head[5] == 1 else ">"
    return struct.unpack_from(order + "H", head, 18)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--machine", required=True, choices=sorted(MACHINES))
    ap.add_argument("--remove", action="store_true")
    ap.add_argument("trees", nargs="+")
    args = ap.parse_args()
    want = MACHINES[args.machine]
    found = left = images = 0
    for tree in args.trees:
        for root, _, files in os.walk(tree):
            for name in sorted(files):
                path = os.path.join(root, name)
                if os.path.islink(path):
                    continue
                m = machine(path)
                if m is None:
                    continue
                images += 1
                if m == want:
                    continue
                found += 1
                what = NAMES.get(m, "machine %d" % m)
                if args.remove:
                    os.remove(path)
                    print("elfcheck: %s: %s, not %s: removed" % (path, what, NAMES[want]), file=sys.stderr)
                else:
                    left += 1
                    print("elfcheck: %s: %s, not %s" % (path, what, NAMES[want]), file=sys.stderr)
    print("elfcheck: %d ELF image(s) in %s, %s" % (images, " ".join(args.trees),
          "all %s" % NAMES[want] if not found else "%d not %s%s" % (found, NAMES[want],
          ", removed" if args.remove else "")))
    return 1 if left else 0


if __name__ == "__main__":
    sys.exit(main())
