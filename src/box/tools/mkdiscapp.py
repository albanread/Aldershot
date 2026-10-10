#!/usr/bin/env python3
"""mkdiscapp.py [--check] SOURCES COMPONENT IMAGE OUT -- a RISC OS C
application's disc directory, as its build installs it, with its x32 image.

    tools/mkdiscapp.py $RISCOSSRC/Sources Apps/EditApp build/capps/edit/'!RunImage,ff8' build/edit/'!Edit'

RISC OS installs a C application (BuildSys/Makefiles/CApp, install_app)
by copying its Makefile's INSTAPP_FILES into the application directory,
each found in Resources.<LOCALE> then Resources (InstRes -I; the locale
UK), TARGET (!RunImage) from the link, and Messages, when INSTAPP_VERSION
names it, through Build:AwkVers: its _Version line made "<version>
(<date>)" from VersionNum (tools/mkromapps.py's insert_version).  This
does the same, with IMAGE -- roscc link --clib's x32 !RunImage -- as
TARGET, and writes OUT (replaced whole): each file with its type as a
",ttt" suffix, none for text, as HostFS keeps them, datestamped as its
source.  Two runs give the same bytes.

For an x32 image, what an application's disc !Run says changes where the
ARM image's assumptions are in it, and nowhere else.  CHANGES says which
lines, for each component: the lines as RISC OS has them, and what ROSGD's
!Run has in their place.  The source !Run must hold exactly those lines,
or this stops (riscos-src changed: look again).  A file INSTAPP_FILES
names that only the replaced lines used is left out (DROP), and said so.

Edit (Apps/EditApp; package A3, first light):
  - WimpSlot 640K, not 184K: the x32 image and crt0_x32's 256K root stack
    need about 440K before the heap; at 184K the library's start fails with
    "Not enough memory for C library" (C01).  flex grows the slot beyond
    it as files load (Wimp_SlotSize), as on RISC OS.
  - No "Run Edit:Export": Export is BASIC that CALLs ARM code to find
    ARM BASIC's tokeniser and detokeniser and leave their addresses in
    Edit$Tokenise and Edit$Detokenise, which ROSGD's BASIC is not.  In
    its place both are set to -1, BASIC mode off (txtedit's "not known"),
    so a BASIC file loads and saves as its bytes; unset, Edit would read
    them as 0, BASIC mode on, and call rlib's bastxt (rosgd/rlib/bastxt.c:
    an error).  BASIC mode is package A8's.  Export is not staged.

--check runs the self-checks: the changes against the sources, AwkVers on
a Messages file, and two stagings byte for byte.
"""
import os
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True             # no __pycache__ in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkromapps  # noqa: E402

SEARCH = ["Resources/UK", "Resources"]     # InstRes -I Resources.UK,Resources (no USERIF)

# component: ({leaf: [(RISC OS's line, ROSGD's lines)]}, files left out)
CHANGES = {
    "Apps/EditApp": ({
        "!Run": [
            (b"WimpSlot -min 184k -max 184k", [b"WimpSlot -min 640k -max 640k"]),
            (b"Run Edit:Export", [b"SetEval Edit$Tokenise -1", b"SetEval Edit$Detokenise -1"]),
        ]}, ["Export"]),
}


def makefile_vars(path):
    """A CApp Makefile's simple assignments (=, ?=, override)"""
    import re
    text = open(path, encoding="latin-1").read().replace("\\\n", " ")
    found = {}
    for line in text.split("\n"):
        m = re.match(r"\s*(?:override\s+)?([A-Z_]+)\s*\??=\s*(.*?)\s*$", line)
        if m and not line.lstrip().startswith("#"):
            found[m.group(1)] = m.group(2)
    if not re.search(r"^include\s+CApp\s*$", text, re.M):
        raise SystemExit(f"mkdiscapp: {path}: not a C application (include CApp)")
    return found


def find(comp, leaf):
    """leaf in the component's Resources, in InstRes's order: (path, type suffix or "")"""
    for d in SEARCH:
        full = os.path.join(comp, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            base, _, suffix = name.partition(",")
            if base == leaf and os.path.isfile(os.path.join(full, name)):
                return os.path.join(full, name), suffix
    raise SystemExit(f"mkdiscapp: {comp}: no {leaf} in {', '.join(SEARCH)}")


def change(component, leaf, data):
    """The source file's lines with CHANGES applied; each changed line must be there once"""
    edits = CHANGES.get(component, ({}, []))[0].get(leaf)
    if not edits:
        return data
    lines = data.split(b"\n")
    for old, new in edits:
        at = [i for i, line in enumerate(lines) if line.rstrip(b"\r") == old]
        if len(at) != 1:
            raise SystemExit(f"mkdiscapp: {component}'s {leaf} has {len(at)} lines {old.decode()!r}, "
                             "not 1: riscos-src changed; look at CHANGES again")
        i = at[0]
        lines[i:i + 1] = new
    return b"\n".join(lines)


def stage(sources, component, image, out):
    comp = os.path.join(sources, component)
    v = makefile_vars(os.path.join(comp, "Makefile"))
    target = v.get("TARGET", "!RunImage")
    drop = CHANGES.get(component, ({}, []))[1]
    versioned = v.get("INSTAPP_VERSION", "").split()
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    said = []
    for leaf in v.get("INSTAPP_FILES", "").split():
        if leaf in drop:
            said.append(f"{leaf} left out (mkdiscapp.py's CHANGES)")
            continue
        if leaf == target:
            dest = os.path.join(out, leaf + ",ff8")
            shutil.copyfile(image, dest)
            st = os.stat(image)
            os.utime(dest, (st.st_mtime, st.st_mtime))
            continue
        path, suffix = find(comp, leaf)
        data = open(path, "rb").read()
        if leaf == "Messages" and "Messages" in versioned:
            data = mkromapps.insert_version(data, *mkromapps.version(comp))
        data = change(component, leaf, data)
        dest = os.path.join(out, leaf + ("," + suffix if suffix else ""))
        with open(dest, "wb") as fh:
            fh.write(data)
        st = os.stat(path)
        os.utime(dest, (st.st_mtime, st.st_mtime))
    return said


def digest(tree):
    import hashlib
    h = hashlib.sha256()
    for root, dirs, files in os.walk(tree):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            h.update(os.path.relpath(p, tree).encode() + b"\0" + open(p, "rb").read())
    return h.hexdigest()


def check(sources):
    """The self-checks, on the sources' Edit and a stand-in image"""
    bad = []
    comp = os.path.join(sources, "Apps/EditApp")
    run = open(find(comp, "!Run")[0], "rb").read()
    got = change("Apps/EditApp", "!Run", run)
    want = [line for line in run.split(b"\n")]
    i = want.index(b"WimpSlot -min 184k -max 184k")
    want[i] = b"WimpSlot -min 640k -max 640k"
    i = want.index(b"Run Edit:Export")
    want[i:i + 1] = [b"SetEval Edit$Tokenise -1", b"SetEval Edit$Detokenise -1"]
    if got != b"\n".join(want):
        bad.append("!Run: not the source's with the two changes")
    import difflib
    ops = [(i2 - i1, j2 - j1) for tag, i1, i2, j1, j2 in
           difflib.SequenceMatcher(None, run.split(b"\n"), got.split(b"\n"), autojunk=False).get_opcodes()
           if tag != "equal"]
    if ops != [(2, 3)]:      # WimpSlot and Export, next to each other: 2 lines for 3
        bad.append(f"!Run: changed {ops} (lines of RISC OS's, ROSGD's), not its two for three")
    try:
        change("Apps/EditApp", "!Run", run.replace(b"184k", b"200k"))
        bad.append("a changed source !Run was taken")
    except SystemExit:
        pass
    m = mkromapps.insert_version(b"# Messages\n_Version:Substituted at build time\nX:y\n", "1.76", "15-Jun-26")
    if m != b"# Messages\n_Version:1.76 (15-Jun-26)\nX:y\n":
        bad.append(f"AwkVers: {m!r}")
    if mkromapps.insert_version(b"X:y\n", "1.0", "1-Jan-00") != b"X:y\n_Version:1.0 (1-Jan-00)\n":
        bad.append("AwkVers: no _Version line, none added")
    with tempfile.TemporaryDirectory() as t:
        img = os.path.join(t, "img")
        with open(img, "wb") as fh:
            fh.write(b"\x7fELF stand-in")
        stage(sources, "Apps/EditApp", img, os.path.join(t, "a"))
        stage(sources, "Apps/EditApp", img, os.path.join(t, "b"))
        if digest(os.path.join(t, "a")) != digest(os.path.join(t, "b")):
            bad.append("two stagings differ")
        names = sorted(os.listdir(os.path.join(t, "a")))
        if names != sorted(["!Boot,feb", "!Help", "!Run,feb", "!RunImage,ff8", "!Sprites,ff9",
                            "!Sprites22,ff9", "Messages", "Templates,fec"]):
            bad.append(f"files: {names}")
        msg = open(os.path.join(t, "a", "Messages"), "rb").read()
        full, date = mkromapps.version(comp)
        if (b"\n_Version:%s (%s)\n" % (full.encode(), date.encode())) not in msg:
            bad.append("Messages: no _Version from VersionNum")
    for b in bad:
        print("mkdiscapp check: " + b)
    print(f"mkdiscapp check: {'all pass' if not bad else '%d FAIL' % len(bad)}")
    return 1 if bad else 0


def main(argv):
    if argv[:1] == ["--check"] and len(argv) == 2:
        return check(argv[1])
    if len(argv) != 4:
        raise SystemExit(__doc__)
    sources, component, image, out = argv
    said = stage(sources, component, image, out)
    comp = os.path.join(sources, component)
    full, date = mkromapps.version(comp)
    print(f"mkdiscapp: {out}: {component} {full} ({date}), {len(os.listdir(out))} files"
          + "".join("; " + s for s in said))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
