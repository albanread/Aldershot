#!/usr/bin/env python3
"""The AAsmModule components whose main unit corpus_diff.units() misses.

    modules_missing.py <RiscOS dir>      one unit a line, sorted

corpus_diff.units() drops any file whose leafname matches a GET target
anywhere in the corpus, so `GET Hdr:Wimp` hides `s/Wimp`. That hides the
Kernel, the Wimp, the Filer, Switcher, Free, FileSwitch, ResourceFS,
MsgTrans, BASIC105, BlendTable, ITable and DrawMod among 49 modules. This
finds each AAsmModule component's main unit from its Makefile instead
(ROM_SOURCE, else TARGET, else COMPONENT, matched against s/ without
regard to case) and lists the ones units() does not.

emitc_sweep.py --missing compiles them.  A component whose main unit
cannot be found is reported on stderr, with the names tried.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from corpus_diff import units  # noqa: E402


def module_units(root):
    """Each AAsmModule component's main unit, found from its Makefile."""
    out = []
    for dp, dn, fn in os.walk(os.path.join(root, "Sources")):
        dn[:] = sorted(d for d in dn if d != ".git")
        if "Makefile" not in fn or not os.path.isdir(os.path.join(dp, "s")):
            continue
        mk = open(os.path.join(dp, "Makefile"), encoding="latin-1").read()
        if not re.search(r"^include\s+AAsmModule", mk, re.M):
            continue
        cands = []
        m = re.search(r"^ROM_SOURCE\s*\??=\s*(\S+)", mk, re.M)
        if m:
            s = m.group(1)
            s = s[2:] if s.startswith("s.") else s[:-2] if s.endswith(".s") else s
            cands.append(s)
        for v in ("TARGET", "COMPONENT"):
            m = re.search(rf"^{v}\s*\??=\s*(\S+)", mk, re.M)
            if m:
                cands.append(m.group(1))
        files = {f.lower(): f for f in os.listdir(os.path.join(dp, "s"))}
        main = next((files[c.lower()] for c in cands if c.lower() in files), None)
        if not main:
            print("?", os.path.relpath(dp, root), cands, file=sys.stderr)
            continue
        out.append(os.path.join(dp, "s", main))
    return out


def missing_units(root):
    """The main units module_units() finds that units() does not, sorted."""
    have = set(units(root))
    return sorted(u for u in module_units(root) if u not in have)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    print("\n".join(missing_units(sys.argv[1])))
