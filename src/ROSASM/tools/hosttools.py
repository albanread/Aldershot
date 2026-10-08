#!/usr/bin/env python3
"""Build the RISC OS build's own utilities to run natively.

    hosttools.py --list
    hosttools.py tokenise

The build does not only assemble and link. It runs a dozen small utilities
that turn one kind of file into another, and some of their output is source
that the assembler then reads -- the Kernel's `s.TokHelpSrc` is made by
running `tokenise` over `HelpStrs`, and without it the Kernel cannot be
assembled at all.

Those utilities ship in `Library/Build` as ARM binaries, which is no use on a
development machine. But most of them have source, in the `BuildHost` product
tree, as git submodules of `RiscOS/Tools/Sources`. They are small, plain C --
`tokenise` is 440 lines of stdio and string.h with no libraries -- and they
compile and run natively as they stand.

## What is here, and what is not

The GNU ports under `Tools/Sources/GNU` -- gawk, sed, grep, diff, find, flex,
bison -- are RISC OS builds of tools a development machine already has. They
are listed for completeness and never built.

The DDE tools are in no repository, because the DDE is closed source. They are
not a gap to be filled by fetching something -- they are the work: `objasm` is
one of them and this project is its replacement, `cc` is roscc in the parent
tree, and `link` is the next one that matters, since nothing past an object
file happens without it. `romlinker`, which joins linked modules into a ROM,
does have source and is already cloned alongside these.

## Building them

Two allowances are made for source written for Norcroft in the eighties, and
neither changes what the program does:

* `memset(tokens, NULL, sizeof tokens)` passes a pointer where a fill byte
  belongs. It is zero either way, and clang's refusal is downgraded rather
  than the upstream source being edited.
* `printf("%d", size)` on a `size_t`, on an allocation-failure path.

Nothing here modifies the checked-out sources: they are submodules of an
upstream repository, and a local edit would be lost or confusing.
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
BUILDHOST = paths.BUILDHOST
BINDIR = os.path.join(HERE, "hostbin")
CLANG = paths.CLANG

# Flags that let source written for Norcroft through a modern compiler without
# touching it. See the module docstring for what each one forgives.
CFLAGS = [
    "-O2",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-Wno-error=int-conversion",
    "-Wno-format",
    "-Wno-deprecated-declarations",
]

# name -> (submodule path under BuildHost, what it does)
TOOLS = {
    "tokenise": ("RiscOS/Tools/Sources/tokenise", "help text to tokenised assembler"),
    "stripdepnd": ("RiscOS/Tools/Sources/stripdepnd", "tidies generated dependency files"),
    "squeeze": ("RiscOS/Tools/Sources/squeeze", "compresses an absolute image"),
    "modsqz": ("RiscOS/Tools/Sources/modsqz", "compresses a module"),
    "unmodsqz": ("RiscOS/Tools/Sources/unmodsqz", "expands a compressed module"),
    "rompress": ("RiscOS/Tools/Sources/rompress", "compresses a ROM image"),
    "translate": ("RiscOS/Tools/Sources/Translate", "character set translation"),
    "filecrc": ("RiscOS/Tools/Sources/FileCRC", "checksums a file"),
    "togpa": ("RiscOS/Tools/Sources/ToGPA", "converts to GPA symbol format"),
    "romunjoin": ("RiscOS/Tools/Sources/ROMUnjoin", "splits a joined ROM"),
    "toansi": ("RiscOS/Tools/Sources/toansi", "K&R C to ANSI"),
    "topcc": ("RiscOS/Tools/Sources/topcc", "ANSI C to K&R"),
}

# In the repositories but pointless to build: a development machine has these.
GNU_PORTS = ["bison", "flex", "gawk", "grep", "sed", "diff", "find", "textutils", "ident"]

# The closed-source DDE. Not a gap to fetch: this is the re-implementation
# list, and where each one stands.
DDE_REIMPLEMENT = {
    "objasm": "rosasm, this repository",
    "cc": "roscc, in the parent tree",
    "link": "not started -- the next one that matters",
    "libfile": "not started -- collects objects into a library",
    "cmhg": "not started -- C module header generator",
    "binaof": "not started -- wraps a binary as an AOF area",
    "binasm": "not started -- binary to assembler source",
    "aoftoc": "not started -- AOF to C source",
    "modgen": "not started -- generates a module header",
}


def source_dir(name):
    return os.path.join(BUILDHOST, TOOLS[name][0].replace("/", os.sep))


def is_cloned(name):
    d = source_dir(name)
    return os.path.isdir(d) and bool(os.listdir(d))


def clone(name):
    """Fetch the submodule, which the parent checkout registers but omits."""
    path = TOOLS[name][0]
    # No BuildHost checkout at all, rather than one missing this submodule.
    # Worth its own answer, because `git` with a `cwd` that does not exist
    # raises out of `build`'s error handling instead of returning non-zero,
    # and a caller assembling a corpus loses the whole run over one unit.
    if not os.path.isdir(BUILDHOST):
        print(f"[{BUILDHOST}: no BuildHost checkout]", file=sys.stderr)
        return False
    print(f"[fetching {path}]", file=sys.stderr)
    r = subprocess.run(
        ["git", "submodule", "update", "--init", path],
        cwd=BUILDHOST,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(r.stderr.strip(), file=sys.stderr)
    return is_cloned(name)


def build(name, force=False):
    """Compile one tool, returning the path to it, or None.

    Every one of these is a handful of C files under `c/`, RISC OS style with
    no extension, and no libraries beyond the C one.
    """
    if name not in TOOLS:
        raise SystemExit(f"{name}: not a tool this knows about")
    exe = os.path.join(BINDIR, name + paths.EXE)
    if os.path.isfile(exe) and not force:
        return exe
    if not is_cloned(name) and not clone(name):
        print(
            f"[{name}: no source. It is a submodule of {BUILDHOST}; fetch it with\n"
            f"    git -C {BUILDHOST} submodule update --init {TOOLS[name][0]}]",
            file=sys.stderr,
        )
        return None
    src = sorted(glob.glob(os.path.join(source_dir(name), "c", "*")))
    src = [f for f in src if os.path.isfile(f)]
    if not src:
        print(f"[{name}: no C sources under c/]", file=sys.stderr)
        return None
    os.makedirs(BINDIR, exist_ok=True)
    cmd = [CLANG, *CFLAGS, "-o", exe, "-x", "c", *src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(exe):
        print(f"[{name}: did not build]", file=sys.stderr)
        for line in r.stderr.splitlines():
            if "error" in line:
                print(f"  {line}", file=sys.stderr)
        return None
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tool", nargs="*", help="tools to build; all cloned ones if none")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    if a.list:
        print("RISC OS build utilities, from the BuildHost tree:\n")
        for name, (_, what) in sorted(TOOLS.items()):
            if os.path.isfile(os.path.join(BINDIR, name + paths.EXE)):
                state = "built"
            elif is_cloned(name):
                state = "source here"
            else:
                state = "not cloned"
            print(f"  {name:12} {state:12} {what}")
        print(f"\nAlready on a development machine, never built: {', '.join(GNU_PORTS)}")
        print()
        print("The DDE is closed source, so these are re-implemented, not fetched:")
        for name, state in DDE_REIMPLEMENT.items():
            print(f"  {name:12} {state}")
        return

    names = a.tool or [n for n in TOOLS if is_cloned(n)]
    for name in names:
        exe = build(name, a.force)
        print(f"{name:12} {exe or 'unavailable'}")


if __name__ == "__main__":
    main()
