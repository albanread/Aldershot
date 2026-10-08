#!/usr/bin/env python3
"""What the build hands the assembler, component by component: flags, and the
source it generates first.

    component_flags.py <RiscOS dir> [--build BCM2835] [--component Kernel]

The corpus harness has been assembling every unit with the same ten
predefines, which is not what the build does. Two other things reach the
assembler, and without them some components cannot assemble at all: the
Kernel's `hdr/Options` reads `FreezeDevRel` unguarded, and nothing in the
sources defines it.

The first source is the component list, `BuildSys/Components/ROOL/<build>`,
whose `-options` field carries makefile macro assignments:

    Kernel       -at 0xFC010000 -options ASFLAGS="-PD \\"CMOS_Override SETS ...\\""
    FPEmulator   -options FPE_APCS=3/32bit FPEANCHOR=High
    SCSIFiler    -options ASFLAGS="-PD \\"SCSI SETL {TRUE}\\"" TEMPLATES=yes

The second is the component's own makefile, which weaves those macros into
its `ASFLAGS`:

    ASFLAGS += -PD "FreezeDevRel SETL {${FREEZE_DEV_REL}}" ...
    ASFLAGS += -PD "FPEAnchorType SETS \\"${FPEANCHOR}\\""

So the two are not alternatives -- the component list sets macros and the
makefile spends them -- and both are collected here. Where a macro has no
value from the component list, the makefile's own `?=` default is used, which
is how `FREEZE_DEV_REL` comes out FALSE.

Makefile conditionals are honoured, because some of these flags are meant for
a build we are not doing:

    ifeq ($(DEBUG),TRUE)
    ASFLAGS += -PD "DEBUGLIB SETL {TRUE}"
    endif

Taking that line regardless turns on RTSupport's debug code, which is not what
a ROM build assembles.

## Quoting

The component list is quoted twice over, because its value passes through the
build environment and then through make before reaching the assembler. The
makefile's own `ASFLAGS` is quoted once, having only make to get through. Both
end at the same place: an argument like

    CMOS_Override SETS "= FileLangCMOS,fsnumber_SDFS,CDROMFSCMOS,&C0"

which the Kernel substitutes straight into its default CMOS table -- `=` being
ObjAsm's `DCB` -- to boot from SD rather than ADFS.
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hosttools  # noqa: E402
from export_hdrs import read, riscos_path  # noqa: E402

# `ASFLAGS = ...` or `ASFLAGS += ...`, over as many backslashed lines as it
# likes. The `+=` is what makes order matter.
ASFLAGS = re.compile(r"^ASFLAGS\s*(\+?)=\s*((?:.*\\\r?\n)*.*)$", re.M)
# `NAME ?= value` and `NAME = value`, for the macros ASFLAGS interpolates.
MACRO = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*([?:+]?)=\s*(.*)$")
# The conditionals worth reading: everything else in a makefile is about how
# to build, not about what the assembler is told.
IFEQ = re.compile(r"^\s*(ifeq|ifneq)\s*(.*)$")
IFDEF = re.compile(r"^\s*(ifdef|ifndef)\s+(\S+)")
ELSE = re.compile(r"^\s*else\b(.*)$")
ENDIF = re.compile(r"^\s*endif\b")
# A component line: a name, then any number of `-flag value` pairs.
COMPONENT_LINE = re.compile(r"^(\S+)\s*(.*)$")


def split_quoted(text):
    """Split on whitespace, keeping `"..."` together and honouring `\\"`."""
    out, cur, quoted, i = [], "", False, 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            cur += text[i : i + 2]
            i += 2
            continue
        if c == '"':
            quoted = not quoted
            cur += c
        elif c.isspace() and not quoted:
            if cur:
                out.append(cur)
                cur = ""
        else:
            cur += c
        i += 1
    if cur:
        out.append(cur)
    return out


def unquote_once(value):
    """Strip one layer of quoting, as passing through one program would."""
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
    return re.sub(r"\\(.)", r"\1", value)


def component_options(root, build):
    """Every component's `-options` macros, from the build's component list."""
    path = os.path.join(root, "BuildSys", "Components", "ROOL", build)
    if not os.path.isfile(path):
        return {}
    out = {}
    for line in read(path).splitlines():
        line = line.strip()
        if not line or line.startswith(("#", "%")):
            continue
        m = COMPONENT_LINE.match(line)
        if not m:
            continue
        name, rest = m.group(1), m.group(2)
        macros = {}
        if "-options" in rest:
            for token in split_quoted(rest.split("-options", 1)[1]):
                if "=" not in token:
                    continue
                k, v = token.split("=", 1)
                macros[k] = unquote_once(v)
        out[name] = macros
    return out


def component_dirs(root):
    """Where each component lives, from the `COMPONENT` its makefile declares.

    The name in the component list is not the directory name: `WindowManager`
    is `Desktop/Wimp` and `FPEmulator` is `HWSupport/FPASC`. What ties them
    together is the makefile saying `COMPONENT = WindowManager`.
    """
    out = {}
    for dp, dn, fn in os.walk(os.path.join(root, "Sources")):
        dn[:] = [d for d in dn if d != ".git"]
        for f in sorted(fn):
            if f != "Makefile" and not f.endswith(".mk"):
                continue
            for line in logical_lines(read(os.path.join(dp, f))):
                m = MACRO.match(line)
                if m and m.group(1) == "COMPONENT":
                    out.setdefault(m.group(3).strip(), dp.replace("\\", "/"))
        # A directory named for the component works too, where no makefile
        # says otherwise.
        out.setdefault(os.path.basename(dp), dp.replace("\\", "/"))
    return out


def component_of(unit, options, dirs):
    """Which component a unit belongs to: the deepest directory that owns it.

    Deepest, because components nest -- `SCSI/SCSIFiler` is the filer, not the
    driver above it.
    """
    unit = unit.replace("\\", "/")
    best, best_len = None, -1
    for name in options:
        d = dirs.get(name)
        if d and unit.startswith(d + "/") and len(d) > best_len:
            best, best_len = name, len(d)
    return best


def logical_lines(text):
    """The makefile's lines, with backslash continuations joined."""
    out, cur = [], ""
    for line in text.splitlines():
        if line.endswith("\\"):
            cur += line[:-1] + " "
            continue
        out.append(cur + line)
        cur = ""
    if cur:
        out.append(cur)
    return out


def condition_holds(kind, rest, macros):
    """Evaluate an `ifeq`/`ifneq`/`ifdef`/`ifndef`.

    An unknown macro is empty, which is what make does and what makes
    `ifeq ($(DEBUG),TRUE)` false for a build that never set DEBUG.
    """
    if kind in ("ifdef", "ifndef"):
        got = bool(macros.get(rest.strip(), ""))
        return got if kind == "ifdef" else not got
    rest = rest.strip()
    if rest.startswith("(") and rest.endswith(")"):
        parts = rest[1:-1].split(",", 1)
    else:
        parts = re.findall(r'"([^"]*)"', rest)
    if len(parts) != 2:
        return True
    a, b = (expand(p.strip(), macros) for p in parts)
    # `$(NAME)` as well as `${NAME}`, since makefiles use both.
    a, b = (re.sub(r"\$\((\w+)\)", lambda m: macros.get(m.group(1), ""), x) for x in (a, b))
    return (a == b) if kind == "ifeq" else (a != b)


def walk(text, macros, supplied):
    """Read one makefile in order, returning the ASFLAGS words it sets.

    Assignments and `ASFLAGS +=` both depend on position, and both are skipped
    inside a conditional that does not hold. A macro the component list
    supplied is never overwritten: those arrive on the make command line.
    """
    words, taken = [], []
    for line in logical_lines(text):
        m = IFEQ.match(line) or IFDEF.match(line)
        if m:
            active = all(taken) and condition_holds(m.group(1), m.group(2), macros)
            taken.append(active)
            continue
        if ELSE.match(line) and taken:
            taken[-1] = all(taken[:-1]) and not taken[-1]
            continue
        if ENDIF.match(line) and taken:
            taken.pop()
            continue
        if not all(taken):
            continue
        m = MACRO.match(line)
        if not m:
            continue
        name, op, value = m.group(1), m.group(2), m.group(3).strip()
        if name == "ASFLAGS":
            value = expand(value, macros)
            if "${" in value or "$(" in value:
                continue
            try:
                got = shlex.split(value)
            except ValueError:
                continue
            words = got if op == "" else words + got
            continue
        if name in supplied:
            continue
        if op == "?" and macros.get(name):
            continue
        macros[name] = expand(value, macros) if op != "+" else macros.get(name, "") + " " + value
    return words


def macros_of(texts, supplied):
    """A first look at the macros, so conditionals have something to read.

    Read without regard to conditionals, because a macro's value is usually
    set once and the `?=` defaults are what matter; `walk` refines this as it
    goes.
    """
    macros = {}
    for text in texts:
        for line in logical_lines(text):
            m = MACRO.match(line)
            if not m or m.group(1) == "ASFLAGS":
                continue
            name, op, value = m.group(1), m.group(2), m.group(3).strip()
            if op == "?" and name in macros:
                continue
            macros[name] = value
    macros.update(supplied)
    return macros


def expand(text, macros, seen=()):
    """Replace `${NAME}` until nothing is left that we know."""
    for _ in range(8):
        before = text
        text = re.sub(
            r"\$\{(\w+)\}", lambda m: macros.get(m.group(1), m.group(0)), text
        )
        if text == before:
            break
    return text


def assembler_flags(root, unit, options, dirs, log=None):
    """The `-PD` and `-I` arguments the build would add for this unit.

    Anything else in `ASFLAGS` -- `-Stamp`, `-quit`, `-NoWarn`, `-APCS` -- is
    about how the assembler is driven rather than what it assembles, and is
    dropped.
    """
    name = component_of(unit, options, dirs)
    if name is None:
        return []
    supplied = options.get(name, {})
    comp = dirs[name]
    if not os.path.isdir(comp):
        return []

    texts = []
    for f in sorted(os.listdir(comp)):
        if f == "Makefile" or f.endswith(".mk"):
            texts.append(read(os.path.join(comp, f)))
    macros = macros_of(texts, supplied)

    # The component list's ASFLAGS has been through one more program than the
    # makefile's, so it carries one more layer of quoting.
    words = []
    if "ASFLAGS" in supplied:
        words += shlex.split(expand(supplied["ASFLAGS"], macros))
    for text in texts:
        words += walk(text, dict(macros), supplied)

    out, i = [], 0
    while i < len(words):
        w = words[i]
        if w.lower() in ("-pd", "-predefine") and i + 1 < len(words):
            out += ["-PD", words[i + 1]]
            i += 2
            continue
        if w.lower() == "-i" and i + 1 < len(words):
            out += ["-I", riscos_dir(comp, words[i + 1])]
            i += 2
            continue
        if w.lower().startswith("-i") and len(w) > 2:
            out += ["-I", riscos_dir(comp, w[2:])]
            i += 1
            continue
        i += 1
    return out


def riscos_dir(comp, path):
    """`^` is the parent directory, and `.` separates directories."""
    parts = path.replace(".", "/").split("/")
    here = comp
    for p in parts:
        if p == "^":
            here = os.path.dirname(here)
        elif p:
            here = os.path.join(here, p)
    return here


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--build", default="BCM2835")
    ap.add_argument("--component", help="show just this one")
    a = ap.parse_args()

    options = component_options(a.root, a.build)
    dirs = component_dirs(a.root)
    named = {k: v for k, v in options.items() if v}
    print(f"{len(options)} components, {len(named)} with -options\n")
    for name, macros in sorted(named.items()):
        if a.component and name != a.component:
            continue
        print(f"{name}:")
        for k, v in macros.items():
            print(f"  {k:14} {v}")
    print()
    # What a unit in each component would actually be given.
    for name in sorted(options):
        if a.component and name != a.component:
            continue
        d = dirs.get(name)
        if not d:
            # Plenty of components in the list are not in this source drop.
            continue
        found = None
        for dp, dn, fn in os.walk(d):
            dn[:] = [x for x in dn if x != ".git"]
            if os.path.basename(dp) == "s" and fn:
                found = os.path.join(dp, sorted(fn)[0]).replace("\\", "/")
                break
        if not found:
            continue
        flags = assembler_flags(a.root, found, options, dirs)
        if flags:
            print(f"{name} ({os.path.relpath(d, a.root)}) assembles with:")
            for i in range(0, len(flags), 2):
                print(f"  {flags[i]} {flags[i + 1]!r}")


if __name__ == "__main__":
    main()


# ------------------------------------------------------- generated source

# Components already tokenised this run: the same help text serves every unit
# in the component, and there are eighteen of them.
_TOKENISED = {}
# The sweeps assemble on threads, and two units of one component may ask at
# once: the first records an answer before it has made the file, so without
# this the second assembled with nothing to GET (the Kernel's "cannot find
# s.TokHelpSrc", now and then, under --jobs).
_TOKENISE_LOCK = threading.Lock()


def generated_sources(root, unit, options, dirs, stage, hdrdirs, log=None):
    """Run the build steps that make assembler source, returning `-I` paths:
    `_generated_sources`, one caller at a time."""
    with _TOKENISE_LOCK:
        return _generated_sources(root, unit, options, dirs, stage, hdrdirs, log)


def _generated_sources(root, unit, options, dirs, stage, hdrdirs, log=None):
    """Run the build steps that make assembler source, returning `-I` paths.

    One step so far. A component that sets `TOKHELPSRC = ${TOKENSOURCE}` has
    its help text put through `tokenise` before anything is assembled, and the
    result -- `s.TokHelpSrc` by default -- is `GET` by a source file that will
    not assemble without it. Eighteen components do this, the Kernel among
    them.

    The output goes to a staging directory rather than into the component, so
    a checkout stays as it was found; an `-I` pointing at that directory is
    what makes `GET s.TokHelpSrc` resolve.
    """
    name = component_of(unit, options, dirs)
    if name is None:
        return []
    if name in _TOKENISED:
        return _TOKENISED[name]

    comp = dirs[name]
    texts = [
        read(os.path.join(comp, f))
        for f in sorted(os.listdir(comp))
        if f == "Makefile" or f.endswith(".mk")
    ]
    macros = macros_of(texts, options.get(name, {}))
    _TOKENISED[name] = []
    if "TOKHELPSRC" not in macros or not macros.get("HELPSRC"):
        return []

    helpsrc = os.path.join(comp, riscos_path(macros["HELPSRC"]))
    if not os.path.isfile(helpsrc):
        if log is not None:
            log.append(f"{name}: HELPSRC {macros['HELPSRC']} is not there")
        return []
    out = os.path.join(stage, name, riscos_path(macros.get("TOKENSOURCE", "s.TokHelpSrc")))
    os.makedirs(os.path.dirname(out), exist_ok=True)

    # Without tokenise (no BuildHost checkout), the help text as it is.
    # Tokenising only compresses it: *Help expands the tokens again, so
    # the text printed is the same.  A module with international help has
    # nothing to compress -- its help is Messages tokens (HFLROPD) -- and
    # assembles to the same bytes.
    tokenise = hosttools.build("tokenise")
    if tokenise is None:
        if log is not None:
            log.append(f"{name}: tokenise is not built; its help text is used untokenised")
        shutil.copyfile(helpsrc, out)
        _TOKENISED[name] = ["-I", os.path.join(stage, name)]
        return _TOKENISED[name]

    # `TOKENS ?= Hdr:Tokens`, which lives in the export tree like any header.
    tokens = macros.get("TOKENS", "Hdr:Tokens").split(":")[-1]
    found = None
    for d in list(hdrdirs) + [os.path.join(comp, "hdr")]:
        p = os.path.join(d, riscos_path(tokens))
        if os.path.isfile(p):
            found = p
            break
    if found is None:
        if log is not None:
            log.append(f"{name}: cannot find the token table {tokens}")
        return []

    r = subprocess.run(
        [tokenise, found, helpsrc, out], capture_output=True, text=True
    )
    if r.returncode != 0 or not os.path.isfile(out):
        if log is not None:
            log.append(f"{name}: tokenise failed: {r.stdout.strip()} {r.stderr.strip()}")
        return []
    _TOKENISED[name] = ["-I", os.path.join(stage, name)]
    return _TOKENISED[name]
