#!/usr/bin/env python3
"""Compile one corpus unit into a ROSGD ROM image, as the sweep compiles it.

    rom_unit.py <RiscOS dir> <unit> <rom base> <gen dir> <rosasm> <out.c>

The unit is relative to the RISC OS dir (Sources/.../s/GetAll).  Headers,
predefinitions and generated sources are what emitc_sweep.py gives it;
the SWIs and their registers are rosgd's, from <gen dir>.  rosgd's
Makefile builds BlendTable this way.

ROM_UNIT_SOURCE, if set, is the file compiled in the unit's place -- a
copy of it changed for the ROM (rosgd's ITable: its one import from
AsmUtils made a routine of its own) -- with the unit's own directories,
flags and headers.

The C is derived from RISC OS sources: it goes to <out.c> in the build
tree, never into this repository.
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from component_flags import (  # noqa: E402
    assembler_flags,
    component_dirs,
    component_options,
    generated_sources,
)
from export_hdrs import export_hdrs  # noqa: E402


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    # A directory of its own for the exported headers and generated sources:
    # make -j runs several units at once, and a shared one is overwritten
    # under them.
    with tempfile.TemporaryDirectory(prefix="rom_unit-") as tmp:
        sys.exit(compile_unit(tmp, *sys.argv[1:]))


def compile_unit(tmp, root, unit, base, gen, rosasm, out):
    variables, hdrroot = export_hdrs(root, os.path.join(tmp, "rosasm-export"), "BCM2835")
    options = component_options(root, "BCM2835")
    dirs = component_dirs(root)
    stage = os.path.join(tmp, "rosasm-generated")
    u = os.path.join(root, unit)
    comp = os.path.dirname(os.path.dirname(u))
    args = [rosasm, os.environ.get("ROM_UNIT_SOURCE", u), "-I", comp, "-I", os.path.join(comp, "hdr")]
    for d in hdrroot:
        args += ["-I", d]
    for k, v in sorted(variables.items()):
        args += ["-PD", f'{k} SETS "{v}"']
    args += assembler_flags(root, u, options, dirs)
    args += generated_sources(root, u, options, dirs, stage, hdrroot)
    args += os.environ.get("ROM_UNIT_EXTRA", "").split()
    args += ["--emit", "c", "--rom-base", base,
             "--swis", os.path.join(gen, "native_swis.txt"),
             "--swi-regs", os.path.join(gen, "swi_regs.txt"), "-o", out]
    r = subprocess.run(args, capture_output=True, text=True)
    sys.stdout.write(r.stdout[-3000:])
    sys.stderr.write(r.stderr[-3000:])
    return r.returncode

if __name__ == "__main__":
    main()
