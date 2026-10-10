#!/usr/bin/env python3
"""corpus_scan.py -- A32 opcode survey for the ARM container (design 25, sprint A0).

Answers the corpus questions the front end is scoped by: does this
binary use the FPA (FPCP, CP1/CP2 -- the FPE module must run in the
task), VFP (CP10/CP11), NEON (Advanced SIMD space), or Thumb (entered
via BLX immediate, the only static signal)?

A word-level survey, not a disassembly: it scans every word and reports
counts and first offsets.  Literal pools and data can masquerade as
code, so read the report as a survey, not a verdict; a handful of hits
at pool-shaped offsets is noise, thousands are an instruction set.

Usage:
    corpus_scan.py FILE [FILE ...] [--start N] [--max-show K]
    corpus_scan.py DIR [DIR ...]   # scans every !RunImage and *,ff8 beneath
"""

import argparse
import sys
from pathlib import Path

FPA = "FPA (FPCP cp1/cp2 -- the FPE module runs in the task)"
VFP = "VFP (cp10/cp11)"
NEON = "NEON / Advanced SIMD"
BLX = "Thumb entry (BLX immediate)"


def classify(w: int):
    """Yield the sets a 32-bit word might belong to."""
    out = []
    cp = (w >> 8) & 0xF
    if (w & 0x0F000000) == 0x0E000000:          # MRC/MCR
        if cp in (1, 2):
            out.append(FPA)
        if cp in (10, 11):
            out.append(VFP)
    elif (w & 0x0E000000) == 0x0C000000:        # CDP/LDC/STC
        if cp in (1, 2):
            out.append(FPA)
        if cp in (10, 11):
            out.append(VFP)
    if (w & 0xFE000000) == 0xF2000000 or (w & 0xFF100000) == 0xF4000000:
        out.append(NEON)
    if (w & 0xFE000000) == 0xFA000000:          # BLX immediate: ARMv5+, to Thumb
        out.append(BLX)
    return out


def scan(path: Path, start: int, max_show: int):
    data = path.read_bytes()
    counts, firsts = {}, {}
    for off in range(start, len(data) - 3, 4):
        w = int.from_bytes(data[off:off + 4], "little")
        for kind in classify(w):
            counts[kind] = counts.get(kind, 0) + 1
            firsts.setdefault(kind, []).append(hex(off))
    words = max(0, (len(data) - start) // 4)
    print(f"{path}  ({words} words from +{hex(start)})")
    if not counts:
        print("    none of FPA / VFP / NEON / BLX-imm")
        return
    for kind in (FPA, VFP, NEON, BLX):
        if kind in counts:
            show = ", ".join(firsts[kind][:max_show])
            more = " ..." if len(firsts[kind]) > max_show else ""
            print(f"    {kind}: {counts[kind]}  first: {show}{more}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--start", type=lambda x: int(x, 0), default=0,
                    help="byte offset to scan from (default 0; use 0x20 for an &FF8 with header)")
    ap.add_argument("--max-show", type=int, default=4)
    args = ap.parse_args()
    for p in args.paths:
        if p.is_dir():
            for f in sorted(p.rglob("*")):
                if f.name == "!RunImage" or f.suffix in (".ff8", ",ff8") or ",ff8" in f.name:
                    scan(f, args.start, args.max_show)
        elif p.is_file():
            scan(p, args.start, args.max_show)
        else:
            print(f"{p}: not found", file=sys.stderr)


if __name__ == "__main__":
    main()
