#!/usr/bin/env python3
"""Run the differential harness across the RISC OS assembler corpus.

For each `s/` unit: stage the component on the emulator's disc, assemble it
with ObjAsm to a listing, assemble it here with `roslist`, and compare the two
with `difftest`.

    corpus_diff.py <RiscOS dir> [--limit N] [--out report.txt]

Both sides fail on the many units whose headers only exist after the build's
`export_hdrs` step. Agreeing to fail is recorded separately from agreeing on a
listing -- a unit where only one side fails is the interesting case.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roshell import Shell  # noqa: E402
import paths  # noqa: E402

HOSTFS = paths.HOSTFS
# Per-process staging. Two runs (or a run and a diagnostic) sharing one
# directory silently overwrite each other's unit, and the resulting numbers are
# meaningless -- the assembler is handed a file from the other process.
STAGE_NAME = f"xc{os.getpid() % 10000}"
STAGE = os.path.join(HOSTFS, STAGE_NAME)
# `Hdr:APCS.Common` resolves through the RISC OS path variable Hdr$Path, not
# through -i. Before the build's export_hdrs runs those headers live in
# HdrSrc/hdr, which serves as the export root here. Staged once.
HDRROOT = os.path.join(HOSTFS, "xh")
HDR_NAME = "xh"
ROSLIST = paths.ROSLIST
DIFFTEST = paths.DIFFTEST
OBJASM = "HostFS::HostFS.$.AcornC/C++.!SetPaths.Lib32.objasm"
# What RiscOS/Env/ROOL/BCM2835.sh selects for a Pi build, and what
# BuildSys ASFLAGS passes on to the assembler.
BUILD_VARS = [("Machine", "RPi"), ("APCS", "APCS-32"), ("UserIF", "Raspberry")]
# For our own driver: one -PD argument per assignment.
PD = [f'{k} SETS "{v}"' for k, v in BUILD_VARS]


# RISC OS truncates a command line at 256 bytes, and the :CHR: encoding is
# verbose, so the tool is copied to a short path and predefines are passed only
# when the unit actually mentions them.
OBJASM_SHORT = "HostFS::HostFS.$.oa"


def chr_literal(text):
    """Encode a string as ObjAsm `:CHR:` concatenation.

    ObjAsm's command-line parser collapses a doubled quote to nothing, so a
    predefine written with nested quotes reaches the expression evaluator with
    the value unquoted, and the value then reads as an undefined symbol. No
    quoting form survives: the RISC OS pipe escape and a backslash fare no
    better, and a single quote is rejected outright. Building the value from
    character codes sidesteps quoting altogether, and is what the corpus runs
    use.
    """
    return ":CC:".join(f":CHR:{ord(c)}" for c in text)


def predefines_for(text):
    """Only the predefines this unit refers to.

    Every argument costs command-line budget, and a self-contained unit
    typically mentions none of them.
    """
    out = []
    for k, v in BUILD_VARS:
        if re.search("[$]?" + re.escape(k) + "[^A-Za-z0-9_]", text):
            out.append(f'-pd "{k} SETS {chr_literal(v)}"')
    return out


def units(root):
    """Assembler translation units.

    Only about a quarter of `s/` files are units: the rest are fragments a unit
    pulls in with GET, carrying neither an AREA nor the macros they use. ObjAsm
    says so plainly when handed one ("Area directive missing").

    Declaring an AREA is necessary but not sufficient, though: a fragment may
    carry one and still be included by a parent, as FileCore00, FileSwBody and
    DualSerial's main all are. So a unit declares an AREA *and* is GET by
    nothing else.
    """
    candidates = []
    included = set()
    for dp, dn, fn in os.walk(os.path.join(root, "Sources")):
        dn[:] = [d for d in dn if d != ".git"]
        for f in sorted(fn):
            path = os.path.join(dp, f)
            try:
                text = open(path, "rb").read().decode("latin-1")
            except OSError:
                continue
            # A GET target counts whichever way round the path is written:
            # `GET s.Foo` and `GET Foo.s` both name Foo. `LNK` counts too --
            # it is a GET that does not come back, and it is how every one
            # of the C library's stubs pulls in the file that does the work.
            #
            # A target ending `.s` is a host-style path, so its leafname is
            # the stem after the last separator: `LNK clib/cl_stub2.s` names
            # cl_stub2. Anything else is read the RISC OS way, where `.` is
            # the separator and `GET ./barrier.hdr` names a header beside
            # `s.barrier` rather than the unit itself.
            for tgt in re.findall(r"^[ 	]+(?:GET|INCLUDE|LNK)[ 	]+(\S+)", text, re.M):
                tgt = tgt.split(":")[-1]
                if tgt.lower().endswith(".s"):
                    included.add(re.split(r"[/\\]", tgt[:-2])[-1].lower())
                    continue
                for part in tgt.split("."):
                    if part and part != "s":
                        included.add(part.lower())
            if os.path.basename(dp) == "s" and re.search(r"^[ 	]+AREA[ 	]", text, re.M):
                candidates.append(path)
    return sorted(p for p in candidates if os.path.basename(p).lower() not in included)


def build_export_root(root):
    """Approximate the build's export_hdrs step.

    Superseded by `export_hdrs.py`, which does it properly: the real phase
    copies a declared list of headers into Global and Interface, and unioning
    every `hdr/` picks a winner at random wherever two components share a
    leafname. This remains only because the emulator harness stages onto a RISC
    OS disc and has not been moved over yet.

    `Hdr:Wimp` lives in Desktop/Wimp/hdr, `Hdr:ModHand` in Kernel/hdr, and so
    on: the export root is the union of every component's `hdr/`, which
    export_hdrs assembles during a real build. Union them here instead.

    First writer wins, so a component's own private header -- reached as
    `hdr.Options` with the component directory earlier on the search path --
    still shadows any namesake here.
    """
    shutil.rmtree(HDRROOT, ignore_errors=True)
    os.makedirs(HDRROOT, exist_ok=True)

    srcs = os.path.join(root, "Sources")
    dirs = []
    for dp, dn, _ in os.walk(srcs):
        dn[:] = [d for d in dn if d != ".git"]
        if os.path.basename(dp) not in ("hdr", "Hdr"):
            continue
        # OSLib ships its own parallel header set under Dist/OSLib/*/oslib/Hdr.
        # Those are OSLib's bindings, not the OS's exported headers, and they
        # carry the same names -- unioning them shadows the real ones.
        if "OSLib" in dp and "Dist" in dp:
            continue
        dirs.append(dp)
    # HdrSrc is the canonical source of the shared headers and the Kernel's are
    # next; everything else fills in behind them.
    def rank(d):
        if "HdrSrc" in d:
            return 0
        if os.sep + "Kernel" + os.sep in d:
            return 1
        return 2
    dirs.sort(key=lambda d: (rank(d), d))

    files = clashes = 0
    for dp in dirs:
        for sub_dp, _, sub_fn in os.walk(dp):
            rel = os.path.relpath(sub_dp, dp)
            dest_dir = HDRROOT if rel == "." else os.path.join(HDRROOT, rel)
            os.makedirs(dest_dir, exist_ok=True)
            for f in sub_fn:
                dest = os.path.join(dest_dir, f)
                if os.path.exists(dest):
                    clashes += 1
                    continue
                shutil.copy2(os.path.join(sub_dp, f), dest)
                files += 1
    print(f"[export root: {files} headers, {clashes} name clashes skipped]", file=sys.stderr)
    return HDRROOT


def stage(unit):
    """Copy the unit's component (s/ and hdr/) onto the emulator's disc."""
    comp = os.path.dirname(os.path.dirname(unit))
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE, ignore_errors=True)
    os.makedirs(os.path.join(STAGE, "s"), exist_ok=True)
    os.makedirs(os.path.join(STAGE, "l"), exist_ok=True)
    os.makedirs(os.path.join(STAGE, "o"), exist_ok=True)
    shutil.copy2(unit, os.path.join(STAGE, "s", os.path.basename(unit)))
    for hd in ("hdr", "Hdr"):
        src = os.path.join(comp, hd)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(STAGE, hd), dirs_exist_ok=True)
    return comp


def configure_paths(sh):
    """Set the search paths the way the build's Env does.

    RiscOS/Env/!Common,feb sets:
        Hdr$Path hdr.,<Hdr$Dir>.Global.,<Hdr$Dir>.Interface.,<Hdr$Dir>.Interface2.

    What matters here is the *order*: the component's own `hdr.` comes first,
    so a private header wins over a namesake elsewhere -- and our export root
    is a union of every component's headers, so namesakes abound.

    The Global/Interface/Interface2 entries are omitted. They are directories
    `export_hdrs` creates and our union does not have, and including them made
    the command long enough to kill the emulator outright when twelve
    instances started at once.
    """
    root = "HostFS::HostFS.$"
    sh.cmd(f"Set Hdr$Path {root}.{STAGE_NAME}.hdr.,{root}.{HDR_NAME}.")


def parse_report(text):
    get = lambda k: next((int(m) for m in re.findall(rf"{k}\s+(\d+)", text)), 0)
    return {
        "rows": get("compared rows"),
        "text": get("text matches"),
        "addr": get("addr matches"),
        "bytes": get("bytes match"),
        "absent": get("bytes not ours"),
        "missing": get(r"rows only ObjAsm listed"),
        "extra": get(r"rows only we listed"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument(
        "--max-gets",
        type=int,
        default=None,
        help="only units with at most this many GET directives. The build's "
             "exported headers cannot be reconstructed outside the build, so "
             "units that need few or none are the ones both sides can assemble.",
    )
    ap.add_argument("--out", default=None)
    ap.add_argument("--jsonl", default=None,
                    help="write one JSON record per unit; aggregation reads "
                         "these rather than re-parsing formatted text")
    ap.add_argument("--emu-dir", default=None,
                    help="emulator instance directory (a farm worker)")
    ap.add_argument("--shared-export", action="store_true",
                    help="the export root is already built; do not rebuild it. "
                         "Workers must not rebuild it concurrently: it is "
                         "shared, and rmtree under a reader breaks the run.")
    ap.add_argument("--shard", default=None,
                    help="i/n -- take every nth unit starting at i")
    a = ap.parse_args()

    us = units(a.root)
    if a.max_gets is not None:
        keep = []
        for u in us:
            t = open(u, "rb").read().decode("latin-1")
            if len(re.findall(r"^\s+(?:GET|INCLUDE)\s+\S+", t, re.M)) <= a.max_gets:
                keep.append(u)
        us = keep
        print(f"[{len(us)} units with <= {a.max_gets} GETs]", file=sys.stderr)
    if a.limit:
        # Spread the sample across the tree rather than taking a prefix.
        step = max(1, len(us) // a.limit)
        us = us[::step][: a.limit]

    if a.shard:
        i, n = (int(x) for x in a.shard.split("/"))
        us = us[i::n]
        print(f"[shard {i}/{n}: {len(us)} units]", file=sys.stderr)

    sh = Shell(quiet=True, cwd=a.emu_dir)
    tot = dict(rows=0, text=0, addr=0, bytes=0, absent=0, missing=0, extra=0)
    both_ok = both_fail = only_objasm = only_ours = 0
    lines = []
    records = []
    try:
        sh.boot()
        sh.cmd("HostFS")
        sh.cmd("Dir HostFS::HostFS.$")
        # Shared headers. Built once by the caller when running a farm --
        # a worker rebuilding it would pull it out from under its peers.
        if not a.shared_export:
            build_export_root(a.root)
        # A 50-character tool path eats a fifth of the command-line budget.
        short = os.path.join(HOSTFS, "oa,ff8")
        if not os.path.isfile(short):
            for cand in ("objasm,ff8", "objasm"):
                srcp = os.path.join(HOSTFS, "AcornC.C++", "!SetPaths", "Lib32", cand)
                if os.path.isfile(srcp):
                    shutil.copy2(srcp, short)
                    break
        configure_paths(sh)
        # `Hdr:Machine.<Machine>` is resolved by the filesystem, so Machine
        # must exist as a RISC OS system variable as well as an ObjAsm one.
        for k, v in BUILD_VARS:
            sh.cmd(f"Set {k} {v}")
        def restart():
            """Bring up a fresh emulator and put it back in a known state.

            An instance can die mid-shard -- a crash, or a command that wedges
            it. Without this the worker loses every remaining unit and the run
            silently under-reports, which is indistinguishable from those units
            failing to assemble.
            """
            nonlocal sh
            try:
                sh.close()
            except Exception:
                pass
            sh = Shell(quiet=True, cwd=a.emu_dir)
            sh.boot()
            sh.cmd("HostFS")
            sh.cmd("Dir HostFS::HostFS.$")
            configure_paths(sh)
            for k, v in BUILD_VARS:
                sh.cmd(f"Set {k} {v}")

        restarts = 0
        t0 = time.time()
        for i, unit in enumerate(us, 1):
            name = os.path.basename(unit)
            # Basenames repeat across the corpus (asm x6, veneer x4), so report
            # rows by their path within Sources. Staging still uses the
            # basename, which is safe because each unit is staged alone.
            label = os.path.relpath(unit, os.path.join(a.root, "Sources"))
            label = label.replace(os.sep + "s" + os.sep, os.sep)
            src_text = open(unit, "rb").read().decode("latin-1")
            stage(unit)
            cmd = (
                f"Run {OBJASM_SHORT} "
                + (" ".join(pds) + " " if (pds := predefines_for(src_text)) else "")
                # `-list <file>`: the long `--list=<file>` form is rejected.
                + f"-o {STAGE_NAME}.o.{name} -list {STAGE_NAME}.l.{name}"
                + f" -i {STAGE_NAME} {STAGE_NAME}.s.{name}"
            )
            try:
                out = sh.cmd(cmd, settle=1.0, max_wait=60.0)
            except Exception as e:
                # The instance died. Restart and carry on: without this the
                # worker loses every remaining unit, and the run under-reports
                # in a way indistinguishable from those units failing.
                print(f"[restart after {os.path.basename(unit)}: {e}]", file=sys.stderr)
                restarts += 1
                restart()
                out = ""
            lst = os.path.join(STAGE, "l", name)
            # HostFS buffers: the emulator finishes and returns to the prompt
            # before Windows sees the file. Under load that gap is wide enough
            # to read as "ObjAsm produced nothing", which is how 213 of 259
            # units were misreported as failures.
            deadline = time.time() + 15.0
            while not os.path.isfile(lst) and time.time() < deadline:
                time.sleep(0.2)
            # ObjAsm writes a listing even for a failed assembly, so the file
            # existing is necessary but not sufficient; its diagnostics decide.
            objasm_err = next(
                (l for l in out.splitlines()
                 if l.startswith(("Error", "Bad ", "Unrecognised"))
                 or "could not be opened" in l),
                "",
            )
            if not os.path.isfile(lst) and not objasm_err:
                objasm_err = "no listing produced"
            objasm_ok = os.path.isfile(lst) and not objasm_err

            r = subprocess.run(
                [ROSLIST, unit, "-I", HDRROOT]
                + [x for d in PD for x in ("-PD", d)],
                capture_output=True, text=True, encoding="latin-1",
            )
            ours_ok = r.returncode == 0

            rec = {
                "unit": label.replace(os.sep, "/"),
                "objasm_ok": bool(objasm_ok),
                "ours_ok": bool(ours_ok),
                "objasm_err": objasm_err[:160],
                "ours_err": (r.stderr.strip().splitlines() or [""])[0][:160],
            }

            if objasm_ok and ours_ok:
                both_ok += 1
                tmp = os.path.join(tempfile.gettempdir(), "ours.lst")
                open(tmp, "w", encoding="latin-1").write(r.stdout)
                d = subprocess.run([DIFFTEST, tmp, lst], capture_output=True, text=True)
                st = parse_report(d.stdout)
                for k in tot:
                    tot[k] += st[k]
                rec.update(st)
                lines.append(
                    f"{label:<52} rows={st['rows']:<5} text={st['text']:<5} "
                    f"addr={st['addr']:<5} bytes={st['bytes']:<5} miss={st['missing']}"
                )
            elif not objasm_ok and not ours_ok:
                both_fail += 1
            elif objasm_ok:
                only_objasm += 1
                lines.append(f"{label:<52} OBJASM OK, WE FAILED: {r.stderr.strip()[:80]}")
            else:
                only_ours += 1
                lines.append(f"{label:<52} WE OK, OBJASM FAILED")

            records.append(rec)
            if i % 10 == 0:
                el = time.time() - t0
                print(f"[{i}/{len(us)}  {el:.0f}s  both_ok={both_ok} both_fail={both_fail}]",
                      file=sys.stderr)
    finally:
        sh.close()
        shutil.rmtree(STAGE, ignore_errors=True)

    pct = lambda x, n: 0.0 if not n else 100.0 * x / n
    report = []
    report.append(f"units attempted      {len(us)}")
    report.append(f"  both assembled     {both_ok}")
    report.append(f"  both failed        {both_fail}   (headers need export_hdrs)")
    report.append(f"  only ObjAsm        {only_objasm}   <- our gaps")
    report.append(f"  only ours          {only_ours}")
    report.append("")
    report.append(f"rows compared        {tot['rows']}")
    report.append(f"  text matches       {tot['text']}  {pct(tot['text'], tot['rows']):5.1f}%")
    report.append(f"  addr matches       {tot['addr']}  {pct(tot['addr'], tot['rows']):5.1f}%")
    report.append(f"  bytes match        {tot['bytes']}  {pct(tot['bytes'], tot['rows']):5.1f}%")
    report.append(f"  bytes awaited      {tot['absent']}  {pct(tot['absent'], tot['rows']):5.1f}%")
    report.append(f"  rows only ObjAsm   {tot['missing']}")
    report.append(f"  rows only ours     {tot['extra']}")
    text = "\n".join(report)
    print(text)
    if a.jsonl:
        with open(a.jsonl, "w", encoding="utf-8") as f:
            for rec in records:
                f.write(json.dumps(rec) + chr(10))
    if a.out:
        open(a.out, "w", encoding="utf-8").write(text + "\n\n" + "\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
