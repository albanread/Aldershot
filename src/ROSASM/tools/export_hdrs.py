#!/usr/bin/env python3
"""Build the header tree the RISC OS build's `export_hdrs` phase produces.

    export_hdrs.py <RiscOS dir> [--build BCM2835] [--dest DIR]

Most components cannot be assembled from a clean checkout, because the headers
they `GET` do not exist until the build has run its export phase. That phase is
not a copy of every `hdr/` directory -- it is an explicit, declared list, and
the difference matters. Two components may each have an `hdr/Options` and only
one of them exports it; guessing by union picks a winner at random.

What the real build does, from `Env/!Common,feb`:

    set Hdr$Path  hdr.,<Hdr$Dir>.Global.,<Hdr$Dir>.Interface.,<Hdr$Dir>.Interface2.

Four places, searched in that order:

* `hdr.` is the component's own directory, so a private header always wins.
* `Global` is filled by `HdrSrc`, whose Makefile lists every file by name --
  about sixty of them, several parameterised by the build: `Machine.<Machine>`,
  `HALSize.<HALSize>`, `ImageSize.<ImageSize>`, `APCS.<APCS>`, `UserIF.<UserIF>`.
* `Interface` is filled by each component declaring what it exports. There are
  three spellings, because there are three standard makefile fragments:
  `AAsmModule` reads `HEADER1`..`HEADER8`, `CModule` reads `ASMHDRS` as a
  space-separated list, and a component with a long list writes `EXPORTS`
  instead, as the Kernel does for its twenty. All three copy from `hdr.`.
  A few write the copy themselves -- SpriteExtend's `export` rule is
  `${CP} hdr.SprExtend ${EXP_HDR}.SprExtend`, which the Pinboard needs --
  and those lines are read too.
* `Interface2` is created and never written to. It stays empty here too.

Together that is 61 global and 71 interface headers for a BCM2835 build.

The parameterised names are why a checkout alone cannot satisfy
`GET Hdr:HALSize.<HALSize>`: the filename is built from a build variable, and
those live in `Env/ROOL/<build>`, which this reads rather than hardcodes.

Two things are reported rather than guessed at. Four components have an
`export_hdrs_custom` rule doing something no pattern describes. And three
headers -- `ADFSErr`, `RAMFSErr`, `SCSIFSErr` -- are not copied at all: the
build assembles `s.<name>` and links it `-bin` into a text file, so they exist
only after a build step this does not run.
"""
import argparse
import os
import re
import shutil
import sys

# `set Name Value`, the RISC OS environment file's one interesting line.
SET = re.compile(r"^\s*[Ss]et\s+(\w+)\s+(\S+)\s*$", re.M)
# `HEADER1 = leafname`, with the usual makefile spacing. AAsmModule documents
# three and its GNU counterpart handles eight, so the number is not bounded.
HEADER = re.compile(r"^HEADER[0-9]+\s*[?:]?=\s*(\S+)", re.M)
# `ASMHDRS = one two three`, CModule's spelling of the same thing, over as
# many backslashed lines as it likes.
ASMHDRS = re.compile(r"^ASMHDRS\s*[?:]?=\s*((?:.*\\\r?\n)*.*)$", re.M)
# `NAME = value` for the handful we substitute into a header name. `?=` is
# the common spelling -- the Wimp writes `TARGET   ?= Wimp` -- and reading only
# a bare `=` leaves `HEADER1 = ${TARGET}` unresolved.
ASSIGN = re.compile(r"^(COMPONENT|TARGET)\s*[?:]?=\s*(\S+)", re.M)
# An `EXPORTS = ...` list, continued over as many backslashed lines as it likes.
EXPORTS = re.compile(r"^EXPORTS\s*[?:]?=\s*((?:.*\\\r?\n)*.*)$", re.M)
# `${CP} hdr.SprExtend ${EXP_HDR}.SprExtend`: an export rule that copies a
# header itself, as SpriteExtend's does (and the PDumpers', from `^.hdr`).
# The source is RISC OS dotted, relative to the makefile.
COPY = re.compile(r"^\s+\$\{CP\}\s+(\S+)\s+\$\{EXP_HDR\}\.(\S+)", re.M)


def read(path):
    """A source file as text, whatever its line endings and encoding."""
    with open(path, "rb") as f:
        return f.read().decode("latin-1")


def build_variables(root, build):
    """The build's variables, from `Env/ROOL/<build>`.

    RISC OS filetype suffixes mean the file is `BCM2835,feb`, so the name is
    matched by prefix.
    """
    envdir = os.path.join(root, "Env", "ROOL")
    for name in sorted(os.listdir(envdir)):
        stem = name.split(",")[0]
        if stem.lower() == build.lower():
            v = dict(SET.findall(read(os.path.join(envdir, name))))
            v.setdefault("APCS", "APCS-32")
            return v
    raise SystemExit(f"no environment for build {build!r} in {envdir}")


def substitute(name, variables):
    """Replace `<Var>` with the build's value for it."""
    def one(m):
        key = m.group(1)
        if key not in variables:
            raise KeyError(key)
        return variables[key]

    return re.sub(r"<(\w+)>", one, name)


def riscos_path(dotted):
    """`CPU.Arch` names the file `CPU/Arch`."""
    return dotted.replace(".", os.sep)


# Where a declared export lands, by the macro naming it. `HDRDIR` is what
# HdrSrc calls the global directory; `EXP_HDR` is what every other component
# calls the interface one. Anything else in an EXPORTS list -- a C header, a
# library -- is not on Hdr$Path and is not our business.
DESTINATIONS = {"${HDRDIR}.": "Global", "${EXP_HDR}.": "Interface"}


def export_declared(mk, hdrdir, variables, log, root):
    """Copy the files a Makefile's `EXPORTS` list names.

    HdrSrc lists sixty-odd global headers this way and the Kernel another
    twenty interface ones, both as `${MACRO}.dotted.name`, and both take the
    source from their own `hdr/` under the same relative name.
    """
    text = read(mk)
    m = EXPORTS.search(text)
    if not m:
        return 0
    src_hdr = os.path.join(os.path.dirname(mk), "hdr")
    n = 0
    for e in m.group(1).replace("\\", " ").split():
        where = next((d for p, d in DESTINATIONS.items() if e.startswith(p)), None)
        if where is None:
            continue
        dotted = e.split(".", 1)[1] if "." in e else ""
        try:
            dotted = substitute(dotted, variables)
        except KeyError as k:
            log.append(f"{where}: {dotted} needs build variable {k}, which is not set")
            continue
        rel = riscos_path(dotted)
        src = os.path.join(src_hdr, rel)
        if not os.path.isfile(src):
            log.append(
                f"{os.path.relpath(os.path.dirname(mk), root)}: exports {dotted}, "
                f"which its hdr/ does not hold -- generated during the build, not copied"
            )
            continue
        dst = os.path.join(hdrdir, where, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        n += 1
    return n


def export_interface(root, hdrdir, variables, log):
    """Copy each component's declared headers, into `Hdr/Interface`.

    Two ways of declaring them: `HEADERn = leafname`, which the standard rules
    copy, and an `EXPORTS` list, which a component with more than three writes
    instead.
    """
    dest = os.path.join(hdrdir, "Interface")
    os.makedirs(dest, exist_ok=True)
    n = 0
    custom = []
    for dp, dn, fn in os.walk(os.path.join(root, "Sources")):
        dn[:] = [d for d in dn if d != ".git"]
        # A component with several targets keeps the module's rules in a
        # secondary makefile and delegates to it: HostFS declares its two
        # assembler headers in `mod.mk`, not in `Makefile`.
        makefiles = sorted(f for f in fn if f == "Makefile" or f.endswith(".mk"))
        texts = {f: read(os.path.join(dp, f)) for f in makefiles}
        # `COMPONENT` and `TARGET` are read across the whole component, because
        # MimeMap sets one in its Makefile and names `${TARGET}` in another.
        names = {}
        for t in texts.values():
            names.update(dict(ASSIGN.findall(t)))
        # Both standard fragments say `TARGET ?= ${COMPONENT}`, and MimeMap
        # relies on it: it names `${TARGET}` having only set COMPONENT.
        names.setdefault("TARGET", names.get("COMPONENT", ""))
        for name, text in texts.items():
            if "export_hdrs_custom" in text:
                where = os.path.relpath(dp, root)
                if where not in custom:
                    custom.append(where)
            n += export_declared(os.path.join(dp, name), hdrdir, variables, log, root)
            n += export_named(dp, text, names, dest, log, root)
            n += export_copied(dp, text, dest)
    return n, custom


def export_copied(dp, text, dest):
    """Copy the headers a makefile's own rule copies into `${EXP_HDR}`."""
    n = 0
    for src, leaf in COPY.findall(text):
        src = os.path.join(dp, *[".." if p == "^" else p for p in src.split(".")])
        if not os.path.isfile(src):
            continue
        dst = os.path.join(dest, riscos_path(leaf))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        n += 1
    return n


def export_named(dp, text, names, dest, log, root):
    """Copy the headers a makefile names, by either of the two spellings."""
    n = 0
    if True:
        leaves = list(HEADER.findall(text))
        for m in ASMHDRS.findall(text):
            leaves.extend(m.replace("\\", " ").split())
        for leaf in leaves:
            for k, v in names.items():
                leaf = leaf.replace("${" + k + "}", v)
            if "$" in leaf:
                log.append(f"{os.path.relpath(dp, root)}: cannot resolve header {leaf}")
                continue
            src = os.path.join(dp, "hdr", riscos_path(leaf))
            if not os.path.isfile(src):
                log.append(
                    f"{os.path.relpath(dp, root)}: exports {leaf}, which its hdr/ does "
                    f"not hold -- generated during the build, not copied"
                )
                continue
            dst = os.path.join(dest, riscos_path(leaf))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            n += 1
    return n


def export_hdrs(root, dest, build="BCM2835", quiet=False):
    """Build the export tree. Returns (variables, [Global, Interface]).

    The two directories come back in `Hdr$Path` order, so a caller can hand
    them straight to the assembler as search paths -- after the component's own
    `hdr/`, which the assembler adds itself.
    """
    variables = build_variables(root, build)
    hdrdir = os.path.join(dest, variables["APCS"], "Hdr")
    shutil.rmtree(hdrdir, ignore_errors=True)
    os.makedirs(hdrdir, exist_ok=True)

    # The tree ships a small `Export` of its own -- `Hdr:ShareD` and little
    # else -- holding headers from components this source drop does not
    # contain. The build checks it out and adds to it, so start there.
    seeded = 0
    shipped = os.path.join(root, "Export", variables["APCS"], "Hdr")
    if os.path.isdir(shipped):
        for dp, dn, fn in os.walk(shipped):
            dn[:] = [d for d in dn if d != ".git"]
            for f in fn:
                src = os.path.join(dp, f)
                dst = os.path.join(hdrdir, os.path.relpath(src, shipped))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                seeded += 1

    log = []
    i, custom = export_interface(root, hdrdir, variables, log)
    g = len(
        [
            f
            for _, _, fs in os.walk(os.path.join(hdrdir, "Global"))
            for f in fs
        ]
    )
    i -= g
    # Created by HdrSrc and never written to; the search path names it, so it
    # exists here too rather than being a missing directory.
    os.makedirs(os.path.join(hdrdir, "Interface2"), exist_ok=True)

    if not quiet:
        print(
            f"[export_hdrs: {g} global, {i} interface, {seeded} already exported, "
            f"for {build}]",
            file=sys.stderr,
        )
        for line in log:
            print(f"[export_hdrs: {line}]", file=sys.stderr)
        if custom:
            print(
                f"[export_hdrs: {len(custom)} components have an export_hdrs_custom "
                f"rule this does not run: {', '.join(sorted(custom))}]",
                file=sys.stderr,
            )
    return variables, [
        os.path.join(hdrdir, "Global"),
        os.path.join(hdrdir, "Interface"),
        os.path.join(hdrdir, "Interface2"),
    ]


def components(root, build):
    """The components this build contains, from `BuildSys/Components/ROOL`.

    A source drop holds more than any one build uses: `Filter` and `ADFSFiler`
    are in the tree and not in a BCM2835 ROM. Their headers are never exported
    for this target, so failing to assemble them is the build's own answer, not
    a gap in the assembler.
    """
    path = os.path.join(root, "BuildSys", "Components", "ROOL", build)
    if not os.path.isfile(path):
        return set()
    names = set()
    for line in read(path).splitlines():
        line = line.strip()
        if not line or line.startswith(("#", "%")):
            continue
        names.add(line.split()[0])
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="the RiscOS directory, holding Sources and Env")
    ap.add_argument("--build", default="BCM2835")
    ap.add_argument("--dest", help="where to put Export (default: alongside Sources)")
    a = ap.parse_args()
    dest = a.dest or os.path.join(a.root, "Export")
    variables, paths = export_hdrs(a.root, dest, a.build)
    print("build variables:")
    for k, v in sorted(variables.items()):
        print(f"  {k:12} {v}")
    print("Hdr$Path, after the component's own hdr/:")
    for p in paths:
        print(f"  {p}")


if __name__ == "__main__":
    main()
