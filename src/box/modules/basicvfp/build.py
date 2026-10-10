#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read
"""Builds rom_basicvfp.c: RISC OS 5.31's BASIC, the VFP build, compiled from
ObjAsm by rosasm. A unit can be named to build another build of the same
sources.

    build.py <RiscOS tree> <rosasm> <out.c> [<unit> <rom base>]

The unit is BASICVFP (at &FC100000) unless one is named. BASIC105 is the
ROM's own BASIC with 5-byte reals. It is built the same way.

The component's own Makefile hands the source to ObjAsmVFP with
`VFPAssembler SETL {TRUE}`. s.VFPData is generated first, by running
VFPLib's GenData program under BASIC itself. No BASIC runs in this build, so
the generated file is staged here as modules/basicvfp/VFPData. The exported
headers that the sources GET are rebuilt into a cache beside this script.
This uses the same export_hdrs that the corpus sweep uses, and does not
assume that the headers already exist.

RISC OS 5 sources are ROOL's, under Apache 2.0. The generated C is a
derivative of them and stays in this private tree (ROSGD notice 4).
"""
import atexit
import fcntl
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(os.path.dirname(HERE))
DEVROOT = os.path.dirname(os.path.dirname(ROSGD))          # .../RISCOSDEV
TOOLCHAIN = os.path.dirname(ROSGD)                      # .../A7232ToolChain
ROSASM = os.path.join(TOOLCHAIN, "rosasm")
sys.path.insert(0, os.path.join(ROSASM, "tools"))

BASIC = os.path.join(sys.argv[1], "Sources", "Programmer", "BASIC")
rosasm = sys.argv[2]
out = sys.argv[3]

# The exported headers, cached. These are Global and Interface, as the
# build's export_hdrs phase would have them. make -j builds BASICVFP and
# BASIC105 at once. The first to arrive builds the cache under a lock and
# the other waits for it.
cache = os.path.join(HERE, "export-cache")
from export_hdrs import export_hdrs          # noqa: E402
marker = os.path.join(cache, ".built")
with open(os.path.join(HERE, ".export-cache.lock"), "w") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    if not os.path.exists(marker):
        variables, hdrroot = export_hdrs(sys.argv[1], cache, "BCM2835")
        os.makedirs(cache, exist_ok=True)
        with open(marker + ".new", "w") as f:
            for k, v in sorted(variables.items()):
                f.write(f"{k}={v}\n")
            for d in hdrroot:
                f.write(f"hdr={d}\n")
        os.replace(marker + ".new", marker)
    fcntl.flock(lock, fcntl.LOCK_UN)
variables, hdrroot = {}, []
with open(marker) as f:
    for line in f:
        k, _, v = line.strip().partition("=")
        if k == "hdr":
            hdrroot.append(v)
        else:
            variables[k] = v

# s.VFPData, as GenData wrote it under RISC OS BASIC. It is staged in a
# directory of this run's own, because the other build may be reading its
# copy.
stage = tempfile.mkdtemp(prefix="basicvfp-stage-")
atexit.register(shutil.rmtree, stage, True)
os.makedirs(os.path.join(stage, "s"))
shutil.copyfile(os.path.join(HERE, "VFPData"), os.path.join(stage, "s", "VFPData"))

unit = sys.argv[4] if len(sys.argv) > 4 else "BASICVFP"
rom_base = sys.argv[5] if len(sys.argv) > 5 else "0xFC100000"
args = [rosasm, os.path.join(BASIC, "s", unit),
        "-I", BASIC, "-I", os.path.join(BASIC, "hdr"),
        "-I", stage]
for d in hdrroot:
    args += ["-I", d]
for k, v in sorted(variables.items()):
    args += ["-PD", f'{k} SETS "{v}"']
args += ["-PD", "VFPAssembler SETL {TRUE}"]
# The SWI tables are beside the output, in the Makefile's $(GEN). That is
# build/gen or build/aarch64/gen. It is not always build/gen, which a fresh
# arm64 checkout lacks and which belongs to the x86 build (#76).
gen = os.path.dirname(os.path.abspath(out))
# --poll-loops makes the interpreter's loops safe points for the runtime's
# background work. Escape then stops a program's loop that never calls the
# OS (REPEAT:UNTIL FALSE), as an interrupt does on RISC OS.
args += ["--emit", "c", "--rom-base", rom_base, "--poll-loops",
         "--swis", os.path.join(gen, "native_swis.txt"),
         "--swi-regs", os.path.join(gen, "swi_regs.txt"),
         "-o", out]
r = subprocess.run(args, capture_output=True, text=True)
sys.stdout.write(r.stdout[-4000:])
sys.stderr.write(r.stderr if os.environ.get("ROSASM_DEBUG_ESCAPE") else r.stderr[-4000:])
sys.exit(r.returncode)
