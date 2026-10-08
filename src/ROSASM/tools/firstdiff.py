#!/usr/bin/env python3
"""Where each unit first stops matching ObjAsm, and which line that was.

    firstdiff.py <RiscOS dir> <collected dir> [--only TEXT] [--limit N]

`aofdiff --against` says how many words differ. That number is often a
consequence rather than a cause: a single word inserted or dropped early on
puts every branch after it out by one, and a unit reads as fifteen hundred
differing words for want of one character.

So this looks for the first word that differs, decides whether what follows
is a shift -- the same words, one place along -- and names the source line
our own layout put there. That line is where the fix is.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aofdiff  # noqa: E402
from corpus_diff import units  # noqa: E402


def words(path, area=None):
    """Each area's words, keyed by area name."""
    out = subprocess.run(
        [aofdiff.AOFDUMP, path, "-x"], capture_output=True, text=True
    ).stdout
    data, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^\*\* Area \d+: (\S+)", line)
        if m:
            cur = m.group(1)
            data[cur] = []
        m = re.match(r"^\s+&([0-9A-F]{4,8}): ([0-9A-F]{8})", line)
        if m and cur:
            data[cur].append(int(m.group(2), 16))
    return data


def source_lines(path):
    """Our own map, as address -> the line that put something there."""
    at = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^\d+\s+([0-9A-F]{8})\s+(\d+)\s+(\S+)\s+(.*)$", line)
        if m:
            at.setdefault((int(m.group(2)), int(m.group(1), 16)),
                          f"{m.group(3)}  {m.group(4).rstrip()}")
    return at


WINDOW = 8


def shifted(a, b, i):
    """Does one of them have an extra word here, matching from then on?"""
    if a[i:i + WINDOW] == b[i + 1:i + 1 + WINDOW]:
        return f"they have one word more here: {b[i]:08X}"
    if a[i + 1:i + 1 + WINDOW] == b[i:i + WINDOW]:
        return f"we have one word more here: {a[i]:08X}"
    return None


def diverges(a, b):
    """The first index where they differ, and what shape the difference is."""
    for i in range(min(len(a), len(b))):
        if a[i] == b[i]:
            continue
        return i, shifted(a, b, i) or f"ours {a[i]:08X}, theirs {b[i]:08X}"
    if len(a) != len(b):
        return min(len(a), len(b)), f"{4 * len(a)} bytes against {4 * len(b)}"
    return None, None


def first_shift(a, b):
    """Where one of them first gains a word on the other.

    A branch whose target moved differs long before the thing that moved it,
    so the first difference is usually a symptom. This is the cause.
    """
    for i in range(min(len(a), len(b)) - WINDOW):
        if a[i] != b[i]:
            why = shifted(a, b, i)
            if why:
                return i, why
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("ref")
    ap.add_argument("--build", default="BCM2835")
    ap.add_argument("--only", help="units whose path contains this")
    ap.add_argument("--limit", type=int)
    a = ap.parse_args()

    setup = aofdiff.Setup(a.root, a.build, stage=False)
    us = aofdiff.chosen(units(a.root), a.only)
    if a.limit:
        us = us[: a.limit]
    tmp = tempfile.gettempdir()
    mine = os.path.join(tmp, "firstdiff.o")
    map_path = os.path.join(tmp, "firstdiff.map")
    shown = 0
    for unit in us:
        ref, _ = aofdiff.reference_paths(a.ref, a.root, unit)
        if not os.path.isfile(ref):
            continue
        label = os.path.relpath(unit, os.path.join(a.root, "Sources"))
        predefines, generated = setup.inputs_for(unit)
        comp = os.path.dirname(os.path.dirname(unit))
        for p in (mine, map_path):
            if os.path.isfile(p):
                os.unlink(p)
        args = [aofdiff.ROSASM, unit, "-I", comp, "-I", os.path.join(comp, "hdr")]
        for d in setup.hdrdirs:
            args += ["-I", d]
        args += generated
        for pd in predefines:
            args += ["-PD", pd]
        args += ["-o", mine, "--map", map_path]
        subprocess.run(args, capture_output=True, text=True)
        if not os.path.isfile(mine):
            continue
        ours, theirs = words(mine), words(ref)
        at = source_lines(map_path) if os.path.isfile(map_path) else {}
        for n, (name, ws) in enumerate(ours.items()):
            if name not in theirs:
                continue
            i, why = diverges(ws, theirs[name])
            if i is None:
                continue
            print(f"{label}")
            print(f"    {name}+{4 * i:04X}  {why}")
            where = at.get((n, 4 * i), "")
            if where:
                print(f"    {where}")
            j, shift = first_shift(ws, theirs[name])
            if j is not None and j != i:
                print(f"    and one of them gains a word at {name}+{4 * j:04X}: {shift}")
                w2 = at.get((n, 4 * j), "")
                if w2:
                    print(f"    {w2}")
            shown += 1
            break
    print()
    print(f"{shown} units differ")


if __name__ == "__main__":
    main()
