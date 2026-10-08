#!/usr/bin/env python3
"""Compile the whole corpus to C, both ways, and check the C with clang.

    emitc_sweep.py <RiscOS dir> [--all | --missing] [--out DIR] [--jobs N] [--only TEXT]

By default only SAMPLE is compiled: a dozen units that compile, chosen for
what they exercise, which takes seconds -- the check to run after a change.
--all takes every unit, and minutes (one unit takes its full timeout): the
check before a milestone. --missing takes instead the modules' main units
that corpus_diff.units() hides because their leafname is a GET target
somewhere (`GET Hdr:Wimp` hides s/Wimp): the Kernel, the Wimp, the Filer,
FileSwitch, BASIC105 and the rest of modules_missing.py's 49.

Every unit codegen_sweep.py assembles is compiled with `--emit c`, lifted
(tier 1) and with --no-lift (tier 0), and each C file is checked by clang
with -Wall against rosgd's headers: no errors, no warnings. The report
tallies what compiles, groups what --emit c refuses by cause -- the
front end's gaps -- and measures what lifting did across the units that
compile both ways: flag helpers gone, conditions that name what was
compared rather than the flags, labels and gotos that became ifs and
loops.

The C is derived from RISC OS sources, so it goes to --out (a directory in
/tmp by default), never into this repository.
"""
import argparse
import collections
import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402
from component_flags import (  # noqa: E402
    assembler_flags,
    component_dirs,
    component_options,
    generated_sources,
)
from corpus_diff import units  # noqa: E402
from modules_missing import missing_units  # noqa: E402
from export_hdrs import export_hdrs  # noqa: E402

ROSGD_INCLUDE = os.path.join(os.path.dirname(paths.ROOT), "rosgd", "include")
# The registers of each SWI the typed API defines (rosgd's api/gen.py, run
# by its build): with them, registers stay in C locals across those SWIs.
SWI_REGS = os.path.join(os.path.dirname(paths.ROOT), "rosgd", "build", "gen", "swi_regs.txt")

# Components not compiled, by decision: what they refuse is not a gap in
# the front end. Their units still go through the compiler as
# test material; a refusal among them is reported as decided.
DECIDED = {
    "Sources/HAL/": "not carried: Linux",
    "Sources/HWSupport/BCMSupport": "not carried: Linux",
    "Sources/HWSupport/USB/": "not carried: Linux drives the hardware",
    "Sources/HWSupport/SCSI/": "not carried: Linux drives the hardware",
    "Sources/Lib/SDIOLib": "not carried: Linux drives the hardware",
    "Sources/FileSys/SDFS": "not carried: HostFS is the one filing system",
    "Sources/FileSys/PCCardFS": "not carried: HostFS is the one filing system",
    "Sources/Networking/AUN/Internet": "not carried: networking is Linux's",
    "Sources/Lib/TCPIPLibs": "not carried: networking is Linux's",
    "Sources/HWSupport/VFPSupport/Test": "not carried: a test",
    "Sources/HWSupport/VFPSupport": "reimplemented: floating point is the host's",
    "Sources/Lib/SyncLib": "reimplemented: C11 atomics",
    "Sources/Lib/callx": "reimplemented over the runtime's callbacks",
    "Sources/Programmer/RTSupport": "reimplemented over the runtime's tasks",
    "Sources/Desktop/ShellCLI": "reimplemented",
    "Sources/Networking/URI": "reimplemented",
    "Sources/Toolbox/Toolbox": "C ported natively; its veneers go",
    "Sources/Lib/RISC_OSLib": "not carried for now: C programs are compiled natively",
    "Sources/Lib/AsmUtils": "not carried for now: C programs are compiled natively",
    "Sources/FileSys/FileCore/Tools": "not carried: a tool",
    "Sources/FileSys/ADFS": "not carried: HostFS is the one filing system",
    "Sources/HWSupport/DMA": "not carried: Linux drives the hardware",
    "Sources/HWSupport/Podule": "not carried: Linux drives the hardware",
    "Sources/HWSupport/VCHIQ": "not carried: Linux drives the hardware",
    "Sources/Video/HWSupport/": "not carried: DRMVideo drives the display, over Linux",
    "Sources/HWSupport/Sound/Sound0HAL": "not carried: ALSA",
    "Sources/Programmer/HostFS/s/HAL_HostFS": "not carried: ROSGD's HostFS is over Linux's VFS",
    "Sources/Programmer/HostFS/s/TML_HostFS": "not carried: ROSGD's HostFS is over Linux's VFS",
    "Sources/Kernel/Dev/": "not carried: a test",
    "Sources/Video/Render/Fonts/ROMFonts": "not carried: the fonts are files, through HostFS",
    # First proposals, to be confirmed when taken on.
    "Sources/HWSupport/Sound/Sound1": "leans reimplement (first proposal)",
    "Sources/HWSupport/Sound/Sound2": "leans reimplement (first proposal)",
    "Sources/Internat/Inter": "leans reimplement (first proposal)",
    "Sources/Video/Render/SpriteUtil": "leans reimplement (first proposal)",
    "Sources/Lib/DDTLib": "not carried: the debugger",
    "Sources/Lib/Trace": "not carried: tracing",
}


def decided(unit):
    return next((why for k, why in DECIDED.items() if unit.startswith(k)), None)


# Units that compile both ways, and what each is here for.
SAMPLE = [
    "Sources/HWSupport/Buffers/s/Buffers",  # loops in loops, gotos to shared error code
    "Sources/Programmer/Obey/s/Obey",  # an irreducible routine: stays flat
    "Sources/Kernel/Dev/HeapTest/s/asm",  # irreducible; the most gotos
    "Sources/Lib/RISC_OSLib/rlib/s/bastxt",  # a goto inside a statement: stays flat
    "Sources/HAL/HAL_BCM2835/s/memcpy",  # conditional loads, block transfers
    "Sources/Networking/AUN/Net/s/netasm",
    "Sources/Desktop/Desktop/s/Desktop",
    "Sources/HWSupport/Sound/Voices/WaveSynth/s/WaveSynth",
    "Sources/FileSys/ResourceFS/ResFiler/s/ResFiler",
    "Sources/HWSupport/SystemDevs/s/SystemDevs",
    "Sources/HWSupport/SerialSpt/s/SerialSpt",
    "Sources/Lib/RISC_OSLib/kernel/s/swiv",
]

# What lifting is measured by, counted in the code (after the ROM image).
MEASURES = {
    "flag helpers (ros_subs, ros_adds, ros_logic, ...)": r"ros_(subs|adds|logic|adcs|sbcs)\(",
    "conditions on the state's flags": r"s->[nzcv]\b|ros_cond\(",
    "statements": r";\s*(/\*|$)",
    "registers in the state block (R[n])": r"\bR\[\d+\]",
    "variables (vN)": r"^\s*v\d+ = ",
    "labels": r"^\s*(?!default:)[A-Za-z_]\w*:(?!:)",
    "gotos": r"\bgoto\b",
    "loops (do, while, for)": r"^\s*(do \{|while \(|for \(;;\))",
    # The author's whole-line comments, at the code's indentation (an
    # instruction's own comment sits in the column at 48).
    "whole-line comments": r"^(?:    ){0,11}(?:/\*(?! ----)| \*)",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--build", default="BCM2835")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "rosasm-emitc"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", help="units whose path contains this")
    ap.add_argument("--all", action="store_true", help="every unit, not the sample")
    ap.add_argument("--missing", action="store_true",
                    help="the modules' main units units() hides (modules_missing.py), not the sample")
    ap.add_argument("--report", metavar="REPORT_JSON", help="summarise an earlier run's report, compiling nothing")
    a = ap.parse_args()
    if a.report:
        with open(a.report) as f:
            summarise(json.load(f), os.path.dirname(os.path.abspath(a.report)))
        return

    variables, hdrroot = export_hdrs(a.root, os.path.join(tempfile.gettempdir(), "rosasm-export"), a.build)
    options = component_options(a.root, a.build)
    dirs = component_dirs(a.root)
    stage = os.path.join(tempfile.gettempdir(), "rosasm-generated")
    predefs = [f'{k} SETS "{v}"' for k, v in sorted(variables.items())]

    def args_for(unit):
        comp = os.path.dirname(os.path.dirname(unit))
        args = [paths.ROSASM, unit, "-I", comp, "-I", os.path.join(comp, "hdr")]
        for d in hdrroot:
            args += ["-I", d]
        for p in predefs:
            args += ["-PD", p]
        u = unit.replace("\\", "/")
        args += assembler_flags(a.root, u, options, dirs)
        args += generated_sources(a.root, u, options, dirs, stage, hdrroot)
        return args

    def one(unit):
        rel = os.path.relpath(unit, a.root)
        res = {"unit": rel}
        stem = os.path.splitext(os.path.basename(unit))[0].lower()
        for mode, extra in (("lift", []), ("tier0", ["--no-lift"])):
            d = os.path.join(a.out, rel.replace("/", "_"), mode)
            os.makedirs(d, exist_ok=True)
            c = os.path.join(d, f"rom_{stem}.c")
            regs = ["--swi-regs", SWI_REGS] if os.path.exists(SWI_REGS) else []
            cmd = args_for(unit) + ["--emit", "c", "--rom-base", "0xFC100000"] + regs + extra + ["-o", c]
            t0 = time.time()
            try:
                p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            except subprocess.TimeoutExpired:
                res[mode] = "timed out"
                continue
            res[mode + "_secs"] = round(time.time() - t0, 2)
            if p.returncode != 0:
                # A warning's source lines follow it, indented; they go with it,
                # or FontManager's forty `LDM` lines fill the report and the
                # reason for the refusal falls off the end.
                lines, quiet = [], False
                for l in p.stderr.strip().splitlines():
                    if not l[:1].isspace():
                        quiet = "warning:" in l or "info:" in l
                    if not quiet:
                        lines.append(l)
                res[mode] = "refused: " + " | ".join(lines)[:600]
                continue
            cc = subprocess.run(
                ["clang", "-std=c11", "-fsyntax-only", "-Wall", "-Wno-unused-label", "-Wno-unused-function",
                 "-Wno-infinite-recursion", "-I", ROSGD_INCLUDE, "-I", d, c],
                capture_output=True, text=True, timeout=300,
            )
            res[mode] = "clang: " + cc.stderr.strip()[:600] if cc.returncode or cc.stderr.strip() else "ok"
        return res

    us = missing_units(a.root) if a.missing else units(a.root)
    if a.only:
        us = [u for u in us if a.only in u]
    elif not (a.all or a.missing):
        us = [u for u in us if os.path.relpath(u, a.root) in SAMPLE]
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        results = list(pool.map(one, us))
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(results, f, indent=1)
    summarise(results, a.out)


def summarise(results, out):
    print(f"{len(results)} units")
    for mode in ("lift", "tier0"):
        tally = collections.Counter(r.get(mode, "?").split(":")[0] for r in results)
        print(f"  {mode:6s} " + ", ".join(f"{k} {n}" for k, n in sorted(tally.items())))
    for r in results:
        for mode in ("lift", "tier0"):
            if r.get(mode, "").startswith("clang"):
                print(f"  clang, {mode}: {r['unit']}: {r[mode].splitlines()[0][:160]}")

    causes = collections.Counter()
    by_decision = collections.Counter()
    for r in results:
        v = r.get("lift", "")
        if v.startswith("refused") and decided(r["unit"]):
            by_decision[decided(r["unit"])] += 1
            continue
        if v.startswith("refused"):
            m = re.findall(r"rosasm: [^|]*", v)
            k = m[0] if m else v
            k = re.sub(r"^rosasm: \S+:\d+ \(&[0-9A-F]+\): ", "", k)
            k = re.sub(r"area \S+ offset &[0-9A-F]+: ", "", k)
            causes[k.replace("rosasm: ", "").strip()[:100]] += 1
    if by_decision:
        print(f"refused, but not compiled by decision ({sum(by_decision.values())}):")
        for k, n in by_decision.most_common():
            print(f"  {n:4d}  {k}")
    print(f"refused, by cause ({sum(causes.values())}):")
    for k, n in causes.most_common(12):
        print(f"  {n:4d}  {k}")

    both = [r for r in results if r.get("lift") == "ok" and r.get("tier0") == "ok"]
    totals = {"lift": collections.Counter(), "tier0": collections.Counter()}
    for r in both:
        for mode in ("lift", "tier0"):
            f = glob.glob(os.path.join(out, r["unit"].replace("/", "_"), mode, "*.c"))[0]
            src = open(f, errors="replace").read()
            code = src[src.find("#pragma STDC"):]
            for k, p in MEASURES.items():
                totals[mode][k] += len(re.findall(p, code, re.M))
    print(f"lifting, over the {len(both)} units that compile both ways:")
    for k in MEASURES:
        t0, t1 = totals["tier0"][k], totals["lift"][k]
        print(f"  {k:50s} tier 0 {t0:7d}   lifted {t1:7d}")
    print(f"the C is in {out}")


if __name__ == "__main__":
    main()
