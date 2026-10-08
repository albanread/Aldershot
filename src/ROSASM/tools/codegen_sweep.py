#!/usr/bin/env python3
"""Assemble the whole corpus with rosasm and tally what code generation does.

    codegen_sweep.py <RiscOS dir> [--limit N] [--jobs N] [--out report.txt]

No emulator: this is our own pipeline end to end -- expand, lower, legalize,
encode, write AOF -- run over every unit `corpus_diff.py` recognises. The
differential harness answers "does our listing match ObjAsm's"; this answers
"how much of the corpus reaches an object file at all, and what stops the
rest".

Failures are grouped by cause rather than counted, because the interesting
output is the shortlist of things to fix next.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402
from corpus_diff import units  # noqa: E402
from component_flags import (  # noqa: E402
    assembler_flags,
    component_dirs,
    component_options,
    generated_sources,
)
from export_hdrs import components, export_hdrs  # noqa: E402

# Overridable, by $ROSASM and $AOFDUMP, so a sweep can run against a build
# other than the one in `target/release` the emulator harness may be
# executing. Each is resolved on its own: deriving one from the other by
# substituting `rosasm.exe` for `aofdump.exe` only worked on Windows.
ROSASM = paths.ROSASM
AOFDUMP = paths.AOFDUMP
# Filled from the build's own environment file once the export tree is built.
PD = []


def classify(err):
    """Group a failure by cause, keeping the part that identifies it."""
    m = re.search(r"cannot find ('?[^'\n]+'?)", err)
    if m:
        return "cannot find " + m.group(1).strip("'")
    for pat, label in [
        (r"unclosed conditional", "unclosed conditional"),
        (r"the encoder rejected", "encoder rejected the lowered assembly"),
        (r"assertion failed", "assertion failed"),
        (r"unknown AREA attribute", "unknown AREA attribute"),
        (r"undefined symbol ([^\s,]+)", "undefined symbol"),
        # Why an object was not written, as opposed to the per-instruction
        # warnings above it. The pool one is separate because it is not the
        # encoder's limitation but the source's layout: an LTORG moves it.
        (r"the literal pool is \d+ bytes away", "literal pool out of an LDR's reach"),
        (
            r"its operands are not ones I know how to encode",
            "operands the encoder does not know",
        ),
        (r"could not be encoded", "instruction not encodable"),
    ]:
        if re.search(pat, err):
            return label
    # Whatever it was, it was not a warning or a note. Those precede the real
    # message and are not why anything failed, so taking the first line as the
    # cause reported an FPA unit as "workspace is 00000B00 bytes".
    first = next(
        (
            l
            for l in err.splitlines()
            if l.strip() and not re.search(r"\b(warning|info|note):", l)
        ),
        "",
    )
    return first.strip()[:80] or "failed with no message"


# Warnings the driver prints per instruction. These do not stop the object
# being written -- the instruction becomes a zero word and the addresses
# around it stay put -- but they say what would not execute.
UNSUPPORTED = re.compile(r"rosasm: [^\n]*?:\d+: ([A-Z][A-Z0-9.]*)[: ]")
UNHANDLED_RELOC = re.compile(r"unhandled relocation type (\d+)")


def assemble(unit):
    """Run the whole pipeline on one unit. Returns a result dict."""
    comp = os.path.dirname(os.path.dirname(unit))
    args = [ROSASM, unit, "-I", comp, "-I", os.path.join(comp, "hdr")]
    if ALLOW[0]:
        args.append("--allow-unencodable")
    for d in HDRROOT[0]:
        args += ["-I", d]
    for pd in PD:
        args += ["-PD", pd]
    # What the build itself would add for this component: the Kernel cannot
    # assemble without them, because `hdr/Options` reads `FreezeDevRel` and
    # nothing in the sources defines it.
    args += assembler_flags(ROOT[0], unit.replace("\\", "/"), OPTIONS[0], DIRS[0])
    # Source the build generates before it assembles: the Kernel's help text
    # is tokenised into `s.TokHelpSrc`, which one of its files GETs.
    args += generated_sources(
        ROOT[0], unit.replace("\\", "/"), OPTIONS[0], DIRS[0], STAGE[0], HDRROOT[0]
    )
    with tempfile.TemporaryDirectory() as tmp:
        args += ["-o", os.path.join(tmp, "out.o")]
        try:
            p = subprocess.run(args, capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            return {"unit": unit, "ok": False, "why": "timed out"}
        except OSError as e:
            return {"unit": unit, "ok": False, "why": f"cannot run rosasm: {e}"}
        out, err = p.stdout, p.stderr
        ok = p.returncode == 0
        size = os.path.getsize(os.path.join(tmp, "out.o")) if ok else 0
        # An object that will not read back is not an object. This catches a
        # header that disagrees with the bytes it describes, which the
        # assembler's own exit status cannot.
        unreadable = None
        if ok:
            d = subprocess.run(
                [AOFDUMP, os.path.join(tmp, "out.o")], capture_output=True, text=True
            )
            if d.returncode != 0:
                tail = d.stderr.strip().splitlines()
                unreadable = tail[-1] if tail else "unreadable"
    if unreadable:
        return {"unit": unit, "ok": False, "why": f"does not read back: {unreadable}"}
    m = re.search(r"(\d+) bytes in (\d+) area", out)
    return {
        "unit": unit,
        "ok": ok,
        "why": None if ok else classify(err),
        "bytes": int(m.group(1)) if m else 0,
        "areas": int(m.group(2)) if m else 0,
        "object": size,
        "err": err,
        "unsupported": [m.group(1) for m in UNSUPPORTED.finditer(err)],
        "relocs": [m.group(1) for m in UNHANDLED_RELOC.finditer(err)],
    }


# Whether an object with instructions missing from it counts as one.
# It does not, by default: that is what the sweep is measuring.
ALLOW = [False]

HDRROOT = [None]
ROOT = [None]
OPTIONS = [{}]
DIRS = [{}]
STAGE = [None]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--build", default="BCM2835")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--out")
    ap.add_argument(
        "--allow-unencodable",
        action="store_true",
        help="count a unit as assembled with instructions missing from it",
    )
    ap.add_argument(
        "--only",
        help="assemble only units whose path contains this, and print what "
        "each one said in full",
    )
    a = ap.parse_args()
    ALLOW[0] = a.allow_unencodable

    # The emulator's disc is not involved here, so the export tree goes
    # somewhere local rather than into hostfs. This is the build's export_hdrs
    # phase: an explicit list of headers into Global and Interface, not a union
    # of every hdr/ directory.
    variables, HDRROOT[0] = export_hdrs(
        a.root, os.path.join(tempfile.gettempdir(), "rosasm-export"), a.build
    )
    in_build = components(a.root, a.build)
    ROOT[0] = a.root
    OPTIONS[0] = component_options(a.root, a.build)
    DIRS[0] = component_dirs(a.root)
    STAGE[0] = os.path.join(tempfile.gettempdir(), "rosasm-generated")
    with_flags = sum(1 for v in OPTIONS[0].values() if v)
    print(
        f"[{len(OPTIONS[0])} components, {with_flags} with build options]",
        file=sys.stderr,
    )
    # Every one of them, because the headers name several in filenames --
    # `Hdr:HALSize.<HALSize>` cannot be found without knowing HALSize is 64K.
    PD[:] = [f'{k} SETS "{v}"' for k, v in sorted(variables.items())]
    us = units(a.root)
    if a.only:
        needle = a.only.replace("\\", "/").lower()
        us = [u for u in us if needle in u.replace("\\", "/").lower()]
        if not us:
            raise SystemExit(f"no unit matches {a.only!r}")
    if a.limit:
        us = us[: a.limit]
    print(f"{len(us)} units, {a.jobs} jobs", flush=True)

    results = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        futures = {pool.submit(assemble, u): u for u in us}
        for n, f in enumerate(concurrent.futures.as_completed(futures), 1):
            results.append(f.result())
            if n % 25 == 0:
                print(f"  {n}/{len(us)}", flush=True)

    if a.only:
        # One unit at a time is for reading, so nothing is grouped away.
        for r in sorted(results, key=lambda r: r["unit"]):
            print(f"=== {os.path.relpath(r['unit'], a.root)}")
            print("    " + ("assembled" if r["ok"] else "FAILED"))
            for line in (r.get("err") or "").strip().splitlines():
                print(f"    {line}")
        return 0 if all(r["ok"] for r in results) else 1

    ok = [r for r in results if r["ok"]]
    lines = []
    lines.append(f"units            {len(results)}")
    lines.append(f"assembled        {len(ok)} ({100 * len(ok) / max(1, len(results)):.1f}%)")
    lines.append(f"code bytes       {sum(r['bytes'] for r in ok)}")
    lines.append(f"areas            {sum(r['areas'] for r in ok)}")

    unsupported = collections.Counter(m for r in results for m in r.get("unsupported", []))
    if unsupported:
        lines.append("")
        lines.append(
            f"instructions with no equivalent on this target "
            f"({sum(unsupported.values())} in all, each of which will trap):"
        )
        for m, n in unsupported.most_common(20):
            lines.append(f"  {n:5d}  {m}")

    relocs = collections.Counter(m for r in results for m in r.get("relocs", []))
    if relocs:
        lines.append("")
        lines.append("relocation types not translated:")
        for m, n in relocs.most_common(20):
            lines.append(f"  {n:5d}  ELF type {m}")

    # A unit whose component this build does not contain was never going to
    # assemble: nothing exported the headers it wants.
    outside = [
        r
        for r in results
        if not r["ok"]
        and in_build
        and not (set(r["unit"].replace("\\", "/").split("/")) & in_build)
    ]
    if outside:
        lines.append("")
        lines.append(
            f"of the {len(results) - len(ok)} failures, {len(outside)} are components "
            f"this build does not contain"
        )

    failed = collections.Counter(r["why"] for r in results if not r["ok"])
    if failed:
        lines.append("")
        lines.append("failures by cause:")
        for why, n in failed.most_common(40):
            lines.append(f"  {n:5d}  {why}")

    report = "\n".join(lines)
    print()
    print(report)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(report + "\n\n")
            for r in sorted(results, key=lambda r: r["unit"]):
                mark = "ok  " if r["ok"] else "FAIL"
                f.write(f"{mark} {r['unit']}  {r['why'] or str(r['bytes']) + ' bytes'}\n")
        print(f"\nwritten to {a.out}")


if __name__ == "__main__":
    main()
