#!/usr/bin/env python3
"""mkromapps.py [--stamps H] [--image COMPONENT=IMAGE]... SOURCES OUT COMPONENT...
-- the ROM's applications, as files for ResourceFS.
mkromapps.py --modwrap H SOURCES COMPONENT -- a C application's module's
title and help string, as ModuleWrap makes them, and its program's name in
ResourceFS, for its native module.

RISC OS's ROM holds its applications in ResourceFS: Resources:$.Apps.!App
is what the Apps icon opens, and Resources:$.Resources.App holds the
program and its resources.  A BASIC application (BuildSys/Makefiles/
BasicApp) gets there in two phases of the ROM build:

  resources  RES_FILES into Resources.<COMPONENT> (the Messages module's
             files): the program, TARGET, made from its sources, and the
             rest found in the component's Resources directory; Messages
             with its _Version filled from VersionNum (Build:AwkVers),
             datestamped as its source (touch -r LocalRes:Messages)
  rom        RESAPP_FILES into Apps.!<COMPONENT>, a module of their own
             (modgen): the application directory, whose ROM !Run sets
             <App>$Path to itself and Resources:$.Resources.<App>. and runs
             <App>:!RunLink -- which RUNs the program where ResourceFS
             holds it (PAGE at its address, FileSwitch's ReadFSHandle)

Each file is looked for in Resources.UK.ROM, Resources.ROM, Resources.UK,
Resources in turn (InstRes -I, the locale UK).  The program is its sources
in bas, appended (FAppend) when there are several, then BasCrunch -1
(tools/bascrunch.py: BASIC's own TEXTLOAD and CRUNCH).  Squish, which the
ROM build runs after that (unless SQUISHED=no), is not done: it is a BASIC
program with ARM code of its own, which cannot run here; the program is
the same program, with its names and lines as written.

A C application (BuildSys/Makefiles/CApp, include CApp: Apps/EditApp)
gets there as RISC OS's ROM build makes it too: RES_FILES into
Resources.<COMPONENT> (the Messages module's files, in the same search
order; Messages through AwkVers, datestamped as its source), and
RESAPP_FILES into Apps.!<COMPONENT> (ResGen's block, which RISC OS's
ModuleWrap module registers; here the one ResourceFS block holds them
all).  The program is not a file there: RISC OS links it into that
module, "!<COMPONENT>", whose *Desktop_<COMPONENT> runs it.  ROSGD's
program is an x32 image instead (roscc link --clib), which --image
COMPONENT=IMAGE puts beside the resources as Resources.<COMPONENT>.<TARGET>
(type &FF8), dated as the newest of the sources its Makefile's OBJS name
(c/<obj>); ROSGD's native module of that title (modules/modulewrap) runs
it from there.  The ROM !Run changes only where the ARM program's
assumptions are in it (ROM_CHANGES below: each changed line must be in the
source exactly once, or this stops).  --modwrap writes the module's title
and help string as ModuleWrap builds them (RISC_OSLib's s/modulewrap:
"!<Name>", then a tab, two if the name is shorter than 7, then
"<Module_MajorVersion> (<Module_Date>)" from the component's VersionNum),
and the program's name, Resources:$.Resources.<COMPONENT>.<TARGET>, which
the module runs by that name, not through <App>$Path.

An older C application's Makefile (include StdTools and ModuleLibs, no
CApp: Apps/Draw, Apps/Paint) names its ROM files in its resources rule,
which the ROM build's resources phase runs: each "${CP} <source>
<destination> ${CPFLAGS}" line copies a file of the component (RISC OS's
dotted name, ${RDIR} Resources, ${LDIR} Resources.<locale>, the locale
UK) into the Messages module's files, <resource$dir>.Apps.!<App> (its ROM
!Boot, !Help and ROM !Run) or <resource$dir>.Resources.<COMPONENT> (its
Templates, and ${MSGS}, Resources.GenMessage: the rule that makes it runs
Build:AwkVers on the locale's Messages -- datestamped here as that source).
Its ModuleWrap module (WRAPPER, RISC_OSLib's s/ModuleWrap) registers more
with ResourceFS: asm/ResFiles's "ResourceFile <source>, <name>" lines
(Hdr:ResourceFS's macro: the file dated as its source, attributes WR/ --
Draw's Sprites).  The program, again, is ROSGD's x32 image beside them,
Resources.<COMPONENT>.!RunImage (the disc build's name, abs.!RunImage,
installed as !RunImage), dated as the newest of the sources its OBJS
(o.<obj>) name; and --modwrap gives the module's title and help string
from asm/AppName's ApplicationName and VersionNum, as for a CApp.

A C module that ModuleWrap makes a task of (BuildSys/Makefiles/CModule,
MODULEWRAP = yes: Desktop/FilerAct, the one there is) is ModuleWrap's other
shape, its FilerAct switch (s/AppName's "GBLL FilerAct"): no application
directory; the module Filer_Action, its command *Filer_Action, which the
Filer Wimp_StartTasks for each operation; and CModule's resources rule's
files in Resources.<TARGET> (RESFSDIR, the Messages module's): Messages
(LocalRes:Messages, CmdHelp appended -- FAppend, the ROM build's CMDHELP:
its help texts end with a NUL, as 5.30's ROM's do) and INSTRES_FILES
(Templates), dated as their sources;
s/ResFiles registers nothing (its lines are comments).  ROSGD's program is
again an x32 image beside them, Resources.<TARGET>.!RunImage, dated as the
newest of its OBJS' C sources; --modwrap gives the module's title and help
string as ModuleWrap's FilerAct switch has them ("Filer_Action", a tab, the
version), its command's help and syntax from the tokens HFACFAC and SFACFAC.

COMPONENT is a directory under SOURCES (Apps/Chars); its Makefile gives
COMPONENT, TARGET, SRCS, RES_FILES, RESAPP_FILES and INSTAPP_VERSION, as
BasicApp takes them (and OBJS, as CApp does), or the resources rule above.  Files are written under OUT
as tools/mkresources.py reads them -- a ",ttt" suffix for a file type,
none for text -- each datestamped as its source, as the ROM build keeps
them.  --stamps writes a C header of each file's ResourceFS path and
datestamp (the load address's low byte and the exec address, as
tools/mkresources.py makes them), for the self-test
(boot/selftest_resfiler.c): the dates are the source files', so whatever
riscos-src's unpacking gave them.  OUT/.attributes gives each file's
attributes, as 5.30's ROM has them (ATTR_WR below), which
tools/mkresources.py puts in the block.
"""
import os
import re
import sys

sys.dont_write_bytecode = True             # no __pycache__ in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bascrunch  # noqa: E402

SEARCH = ["Resources/UK/ROM", "Resources/ROM", "Resources/UK", "Resources"]


def makefile_text(path):
    """A Makefile's text, its continued lines joined"""
    return open(path, encoding="latin-1").read().replace("\\\n", " ")


def makefile_vars(path):
    """The simple assignments (=, ?=, override) of a component's Makefile,
    and its kind: "BasicApp" or "CApp" (which it includes), or "StdTools"
    (include StdTools and ModuleLibs, and a resources rule: Draw's, Paint's)"""
    text = makefile_text(path)
    found = {}
    for line in text.split("\n"):
        m = re.match(r"\s*(?:override\s+)?([A-Z_]+)\s*\??=\s*(.*?)\s*$", line)
        if m and not line.lstrip().startswith("#"):
            found[m.group(1)] = m.group(2)
    kind = re.search(r"^include\s+(BasicApp|CApp)\s*$", text, re.M)
    if kind:
        return found, kind.group(1)
    if re.search(r"^include\s+CModule\s*$", text, re.M) and found.get("MODULEWRAP") == "yes":
        return found, "CModule"
    if all(re.search(r"^include\s+%s\s*$" % n, text, re.M) for n in ("StdTools", "ModuleLibs")) and \
            "resources" in makefile_rules(path):
        return found, "StdTools"
    raise SystemExit(f"mkromapps: {path}: not a BASIC or C application (include BasicApp, CApp; "
                     "StdTools and ModuleLibs with a resources rule; or CModule, MODULEWRAP = yes)")


def makefile_rules(path):
    """A Makefile's explicit rules: {target, as written: [its recipe's
    lines]}"""
    rules, cur = {}, None
    for line in makefile_text(path).split("\n"):
        if line.startswith("\t") and cur is not None:
            rules[cur].append(line.strip())
            continue
        cur = None
        m = re.match(r"^([^\s#:=][^:=]*?)\s*:(?!=)", line)
        if m:
            cur = m.group(1)
            rules[cur] = []
    return rules


def expand(text, v, what):
    """${NAME} and $(NAME) in text, from the Makefile's assignments v, as
    make expands them; LOCALE is the ROM build's, UK"""
    def one(m):
        name = m.group(1) or m.group(2)
        if name == "LOCALE":
            return "UK"
        if name not in v:
            raise SystemExit(f"mkromapps: {what}: ${{{name}}} is not the Makefile's")
        return expand(v[name], v, what)
    return re.sub(r"\$\{(\w+)\}|\$\((\w+)\)", one, text)


# The ROM !Run's changes for an x32 image, as tools/mkdiscapp.py's for the
# disc !Run: component: {leaf: [(RISC OS's line, ROSGD's lines)]}.
#
# Edit (Apps/EditApp): no "Run Edit:Export" -- Export is BASIC that CALLs
# ARM code to find ARM BASIC's tokeniser and detokeniser and leave their
# addresses in Edit$Tokenise and Edit$Detokenise, which ROSGD's BASIC is
# not.  In its place both are set to -1, BASIC mode off (txtedit's "not
# known"), as the disc !Run has them; BASIC mode is package A8's.  Export
# stays in Resources.Edit, as 5.30's ROM has it, never run.  The rest is
# RISC OS's ROM !Run byte for byte: its WimpSlot -min 40k and -max 12k are
# a ModuleWrap program's (code in the ROM, statics in the RMA); the native
# !Edit module's *Desktop_Edit gives the x32 image the slot it needs
# (modules/modulewrap).
#
# Draw (Apps/Draw): none.  Its ROM !Run has no ARM code to run (no Export,
# no RMEnsure), and its WIMPSlot lines, -min 96K then -min 32K -max 32K,
# are a ModuleWrap program's, which the native !Draw module's *Desktop_Draw
# replaces with the 640K the x32 image needs, as *Desktop_Edit does Edit's.
#
# Paint (Apps/Paint): none, as Draw's.  Its ROM !Run (Resources.ROM.!Run)
# runs !Boot and Desktop_Paint, its WimpSlots -min 68K, then -min 12K -max
# 12K, a ModuleWrap program's that *Desktop_Paint replaces.
ROM_CHANGES = {
    # !Chars' ROM !Run gives it 16K, then !RunLink RUNs the program in place
    # (PAGE at its ResourceFS address): BASIC105's room, whose workspace
    # ends below &8F00; BASICVFP's -- the ROM's only BASIC -- reaches &9818,
    # so with 16K Wimp_StartTask never came back.  20K is the least that
    # starts it; 24K leaves room (the grand-design session's diagnosis)
    "Apps/Chars": {
        "!Run": [(b"WimpSlot -min 16k -max 16k", [b"WimpSlot -min 24k -max 24k"])],
    },
    "Apps/EditApp": {
        "!Run": [(b"Run Edit:Export", [b"SetEval Edit$Tokenise -1", b"SetEval Edit$Detokenise -1"])],
    },
}


def change(component, leaf, data):
    """The source file's lines with ROM_CHANGES applied; each changed line
    must be there once"""
    edits = ROM_CHANGES.get(component, {}).get(leaf)
    if not edits:
        return data
    lines = data.split(b"\n")
    for old, new in edits:
        at = [i for i, line in enumerate(lines) if line.rstrip(b"\r") == old]
        if len(at) != 1:
            raise SystemExit(f"mkromapps: {component}'s ROM {leaf} has {len(at)} lines {old.decode()!r}, "
                             "not 1: riscos-src changed; look at ROM_CHANGES again")
        lines[at[0]:at[0] + 1] = new
    return b"\n".join(lines)


def find(comp, leaf):
    """leaf in the component's Resources, in InstRes's order: (path, type
    suffix or "")"""
    for d in SEARCH:
        full = os.path.join(comp, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            base, _, suffix = name.partition(",")
            if base == leaf and os.path.isfile(os.path.join(full, name)):
                return os.path.join(full, name), suffix
    raise SystemExit(f"mkromapps: {comp}: no {leaf} in {', '.join(SEARCH)}")


def version_num(comp, *names):
    """VersionNum's #defines of those names, as strings"""
    got = {}
    for line in open(os.path.join(comp, "VersionNum"), encoding="latin-1"):
        words = line.split()
        if len(words) > 1 and words[0] == "#define" and words[1] in names and '"' in line:
            got[words[1]] = line.split('"')[1]
    return tuple(got[n] for n in names)


def version(comp):
    """VersionNum's Module_FullVersion and Module_ApplicationDate"""
    return version_num(comp, "Module_FullVersion", "Module_ApplicationDate")


def help_message(data, token):
    """Token's message in a Messages file as the kernel prints a
    ModuleWrap command's help from it: MessageTrans_Lookup with R2 = 0 gives
    the text in place, and OS_PrettyPrint reads it on to a NUL, not to its
    line's end (Kernel s/SysComms, International_Help) -- LF is a separator
    to it.  Edit's Messages ends EditHelp and EditSyntax with a NUL; Draw's
    does not, so 5.30's *Help Desktop_Draw prints on to the file's end (the
    ROM's ResourceFS block pads it with one), its _Version line too."""
    at = 0
    for line in data.split(b"\n"):
        name, colon, _ = line.partition(b":")
        if colon and token.encode() in name.split(b"/"):
            start = at + len(name) + 1
            end = data.find(b"\0", start)
            return data[start:end if end >= 0 else len(data)]
        at += len(line) + 1
    raise SystemExit(f"mkromapps: no {token} in its Messages")


def c_string(data):
    """bytes as a C string literal's contents"""
    out = ""
    for c in data:
        ch = chr(c)
        out += {"\\": "\\\\", '"': '\\"', "\t": "\\t", "\n": "\\n", "\r": "\\r"}.get(ch) or \
            (ch if 32 <= c < 127 else "\\%03o" % c)
    return out


def modwrap(sources, component, dest):
    """The module ModuleWrap makes of a C application (RISC_OSLib's
    s/modulewrap, s/AppName: ApplicationName is the COMPONENT, or an older
    one's asm/AppName's; ApplicationVersion "$Module_MajorVersion
    ($Module_Date)"): its title and help string, and its command's help and
    syntax -- International_Help: the tokens <Name>Help and <Name>Syntax in
    Resources:$.Resources.<Name>.Messages (help_message) -- and where build()
    puts ROSGD's program, Resources:$.Resources.<Name>.<TARGET> (!RunImage for
    an older one's), which the module runs by that name, as a C header"""
    comp = os.path.join(sources, component)
    v, kind = makefile_vars(os.path.join(comp, "Makefile"))
    if kind == "CModule":
        return modwrap_fileract(comp, component, v, dest)
    if kind not in ("CApp", "StdTools"):
        raise SystemExit(f"mkromapps: {component}: not a C application")
    name = v["COMPONENT"]
    appname = os.path.join(comp, "asm", "AppName")      # an older one's: ApplicationName SETS "<Name>"
    if kind == "StdTools" and os.path.isfile(appname):
        m = re.search(r'^ApplicationName\s+SETS\s+"([^"]+)"', open(appname, encoding="latin-1").read(), re.M)
        if not m:
            raise SystemExit(f"mkromapps: {component}: asm/AppName sets no ApplicationName")
        name = m.group(1)
    target = "!RunImage" if kind == "StdTools" else v.get("TARGET", name)   # as build() names it
    major, date = version_num(comp, "Module_MajorVersion", "Module_Date")
    help_ = "!" + name + "\t" + ("\t" if len(name) < 7 else "") + f"{major} ({date})"
    tag = re.sub(r"[^A-Z0-9]", "_", name.upper())
    esc = help_.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
    if kind == "StdTools":                  # the Messages the ROM holds
        msgs = next(d for dotted, _, _, d in stdtools_files(comp, component, v)
                    if dotted == f"Resources.{v['COMPONENT']}.Messages")
    else:
        msgs = open(find(comp, "Messages")[0], "rb").read()
        if "Messages" in v.get("INSTAPP_VERSION", "").split():
            msgs = insert_version(msgs, *version(comp))
    lines = [f"/* generated by tools/mkromapps.py --modwrap: do not edit.  {component}'s module,",
             " * as ModuleWrap makes it (RISC_OSLib's s/modulewrap, s/AppName), and its",
             " * command's help and syntax as the kernel prints them from its Messages. */",
             f'#define MODWRAP_{tag}_NAME "{name}"',
             f'#define MODWRAP_{tag}_TITLE "!{name}"',
             f'#define MODWRAP_{tag}_HELP "{esc}"',
             '#define MODWRAP_%s_VERSION "%s (%s)"' % ((tag,) + version(comp)),
             f'#define MODWRAP_{tag}_CMDHELP "{c_string(help_message(msgs, name + "Help"))}"',
             f'#define MODWRAP_{tag}_CMDSYNTAX "{c_string(help_message(msgs, name + "Syntax"))}"',
             f'#define MODWRAP_{tag}_IMAGE "Resources:$.Resources.{name}.{target}"']
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    with open(dest, "w") as fh:
        fh.write("\n".join(lines) + "\n")


def cmodule_messages(comp):
    """A ModuleWrap C module's Messages as the ROM holds it (CModule's
    resources rule, CMDHELP on: LocalRes:Messages, LocalRes:CmdHelp
    appended -- FAppend).  FilerAct's CmdHelp ends HFACFAC and SFACFAC with
    a NUL, as Edit's Messages does, so *Help prints each to its end"""
    data = open(find(comp, "Messages")[0], "rb").read()
    try:
        return data + open(find(comp, "CmdHelp")[0], "rb").read()
    except SystemExit:
        return data


def is_fileract(comp):
    """ModuleWrap's FilerAct switch: s/AppName declares FilerAct"""
    appname = os.path.join(comp, "s", "AppName")
    return os.path.isfile(appname) and re.search(r"^\s+GBLL\s+FilerAct\b", open(appname, encoding="latin-1").read(),
                                                 re.M) is not None


def modwrap_fileract(comp, component, v, dest):
    """--modwrap for ModuleWrap's FilerAct shape (s/modulewrap's FilerAct
    switch): the module Filer_Action, its help string "Filer_Action", a tab,
    "<Module_MajorVersion> (<Module_Date>)"; its one command *Filer_Action,
    help and syntax HFACFAC and SFACFAC (International_Help) in
    Resources:$.Resources.FilerAct.Messages; its program ROSGD's image,
    Resources:$.Resources.<TARGET>.!RunImage; FilerAct$Path's default"""
    if not is_fileract(comp):
        raise SystemExit(f"mkromapps: {component}: a ModuleWrap C module, but not FilerAct's shape "
                         "(s/AppName declares no FilerAct)")
    target = v.get("TARGET", v["COMPONENT"])
    major, date = version_num(comp, "Module_MajorVersion", "Module_Date")
    help_ = f"Filer_Action\t{major} ({date})"
    msgs = cmodule_messages(comp)
    lines = [f"/* generated by tools/mkromapps.py --modwrap: do not edit.  {component}'s module,",
             " * as ModuleWrap's FilerAct switch makes it (RISC_OSLib's s/modulewrap, its",
             " * s/AppName), and its command's help and syntax as the kernel prints them",
             " * from its Messages. */",
             '#define MODWRAP_FILERACT_TITLE "Filer_Action"',
             '#define MODWRAP_FILERACT_COMMAND "Filer_Action"',
             f'#define MODWRAP_FILERACT_HELP "{c_string(help_.encode())}"',
             '#define MODWRAP_FILERACT_VERSION "%s (%s)"' % version(comp),
             f'#define MODWRAP_FILERACT_CMDHELP "{c_string(help_message(msgs, "HFACFAC"))}"',
             f'#define MODWRAP_FILERACT_CMDSYNTAX "{c_string(help_message(msgs, "SFACFAC"))}"',
             f'#define MODWRAP_FILERACT_PATH_VAR "{target}$Path"',
             f'#define MODWRAP_FILERACT_PATH "Resources:$.Resources.{target}."',
             f'#define MODWRAP_FILERACT_IMAGE "Resources:$.Resources.{target}.!RunImage"']
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    with open(dest, "w") as fh:
        fh.write("\n".join(lines) + "\n")


def insert_version(data, full, date):
    """Build:AwkVers on a Messages file: its _Version line made
    "_Version:<version> (<date>)", or that line added"""
    out, done = [], False
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    for line in lines:
        if line.startswith(b"_Version"):
            out.append(b"_Version:%s (%s)" % (full.encode(), date.encode()))
            done = True
        else:
            out.append(line)
    if not done:
        out.append(b"_Version:%s (%s)" % (full.encode(), date.encode()))
    return b"".join(line + b"\n" for line in out)


EPOCH_1900 = 2208988800                 # 1900 to 1970, seconds (tools/mkresources.py)
written = []                            # (ResourceFS path, stamp in centiseconds since 1900)
attributes = []                         # (path, attributes), for OUT/.attributes

# Each file's attributes as RISC OS 5.30's ROM has them (its Apps viewer,
# OS_GBPB 10 on the farm): WR/, owner read and write, for what the
# Messages module and a BASIC application's modgen block hold; WR/R, public
# read too, for a C application's directory, ResGen's block in its
# ModuleWrap module.  The ROM build's tools give them, not the sources.
ATTR_WR, ATTR_WR_R = 0x03, 0x13


def write(path, data, stamp, attr=ATTR_WR):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)
    os.utime(path, (stamp, stamp))
    written.append((path, (int(os.stat(path).st_mtime) + EPOCH_1900) * 100))
    attributes.append((path, attr))


def write_attributes(out):
    """OUT/.attributes: each file's path under OUT and its attributes, in
    hex, as tools/mkresources.py reads them"""
    with open(os.path.join(out, ".attributes"), "w") as fh:
        for path, attr in sorted(attributes):
            fh.write("%s %02X\n" % (os.path.relpath(path, out).replace(os.sep, "/"), attr))


def write_stamps(dest, out):
    lines = ["/* generated by tools/mkromapps.py --stamps: do not edit.  The ROM's",
             " * applications' files, as Resources: has them, and their datestamps:",
             " * the load address's low byte and the exec address. */",
             "static const struct { const char *path; uint32_t load_lo, exec; } romapps_stamps[] = {"]
    for path, stamp in sorted(written):
        rel = os.path.relpath(path, out).split(os.sep)
        leaf = rel[-1][:-4] if len(rel[-1]) > 4 and rel[-1][-4] == "," else rel[-1]
        name = "Resources:$." + ".".join(rel[:-1] + [leaf])
        lines.append('    { "%s", 0x%02Xu, 0x%08Xu },' % (name, stamp >> 32 & 0xFF, stamp & 0xFFFFFFFF))
    lines.append("};")
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    with open(dest, "w") as fh:
        fh.write("\n".join(lines) + "\n")


RESOURCE_DIR = "<resource$dir>."           # the Messages module's files: Resources:$


def dotted_file(comp, dotted, what):
    """A file of the component that a Makefile names RISC OS's way
    (Resources.UK.Templates): (its path, its type suffix or "")"""
    *dirs, leaf = dotted.split(".")
    d = os.path.join(comp, *dirs)
    for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        base, _, suffix = name.partition(",")
        if base == leaf and os.path.isfile(os.path.join(d, name)):
            return os.path.join(d, name), suffix
    raise SystemExit(f"mkromapps: {what}: no {dotted} in {comp}")


def stdtools_files(comp, component, v):
    """An older C application's ROM files (StdTools, above): [(its name
    under Resources:$, dotted; its source; its type suffix; its bytes)] --
    the resources rule's copies, in its order, then its ModuleWrap module's
    ResourceFile lines"""
    rules = makefile_rules(os.path.join(comp, "Makefile"))
    msgs = expand(v["MSGS"], v, component) if "MSGS" in v else None
    got = []
    for line in rules["resources"]:
        if not re.match(r"\$[{(]CP[})]\s", line):
            continue                            # MKDIR, ECHO, "|"
        m = re.match(r"^\$[{(]CP[})]\s+(\S+)\s+(\S+)\s+\$[{(]CPFLAGS[})]$", line)
        if not m:
            raise SystemExit(f"mkromapps: {component}'s resources rule: {line!r} is not "
                             "${CP} <from> <to> ${CPFLAGS}")
        src, dst = expand(m.group(1), v, component), expand(m.group(2), v, component)
        if not dst.startswith(RESOURCE_DIR):
            raise SystemExit(f"mkromapps: {component}'s resources rule copies to {dst}, not <resource$dir>")
        if src == msgs:
            # ${MSGS}: ${LDIR}.Messages VersionNum -- ${MSGVERSION} ${LDIR}.Messages > $@
            recipe = next((r for k, r in rules.items() if k in ("${MSGS}", "$(MSGS)")), [])
            mm = re.match(r"^\$[{(]MSGVERSION[})]\s+(\S+)\s*>\s*\$@$", recipe[0] if len(recipe) == 1 else "")
            if not mm or "Build:AwkVers" not in v.get("MSGVERSION", ""):
                raise SystemExit(f"mkromapps: {component}: {src} is not Build:AwkVers of a Messages file")
            path, suffix = dotted_file(comp, expand(mm.group(1), v, component), component)
            data = insert_version(open(path, "rb").read(), *version(comp))
        else:
            path, suffix = dotted_file(comp, src, component)
            data = open(path, "rb").read()
        got.append((dst[len(RESOURCE_DIR):], path, suffix, data))
    resfiles = os.path.join(comp, "asm", "ResFiles")   # s.ModuleWrap's GET ResFiles.s
    if "ModuleWrap" in v.get("WRAPPER", "") and os.path.isfile(resfiles):
        for line in open(resfiles, encoding="latin-1"):
            m = re.match(r"^\s+ResourceFile\s+([^,\s]+)\s*,\s*(\S+)", line.split(";")[0])
            if m:
                path, suffix = dotted_file(comp, m.group(1), component)
                got.append((m.group(2), path, suffix, open(path, "rb").read()))
    return got


def newest_source(comp, objs):
    """The datestamp of the newest C source a Makefile's OBJS name (Edit's
    "edit", Draw's "o.Draw"; RISC OS's names in any case)"""
    have = {f.lower(): f for f in os.listdir(os.path.join(comp, "c"))}
    srcs = [have.get(o.split(".", 1)[-1].lower()) for o in objs.split()]
    return max(os.stat(os.path.join(comp, "c", f)).st_mtime for f in srcs if f)


def build(sources, out, component, images):
    comp = os.path.join(sources, component)
    v, kind = makefile_vars(os.path.join(comp, "Makefile"))
    name = v["COMPONENT"]
    target = v.get("TARGET", name)
    srcs = [os.path.join(comp, "bas", s) for s in v.get("SRCS", target).split()]
    res_dir = os.path.join(out, "Resources", name)
    app_dir = os.path.join(out, "Apps", "!" + name)

    if kind == "CModule":
        # ModuleWrap's FilerAct shape: Resources.<TARGET>, no application
        image = images.pop(component, None)
        if image is None:
            raise SystemExit(f"mkromapps: {component} is a C module: --image {component}=IMAGE")
        if not is_fileract(comp):
            raise SystemExit(f"mkromapps: {component}: not ModuleWrap's FilerAct shape")
        res_dir = os.path.join(out, "Resources", target)
        write(os.path.join(res_dir, "!RunImage,ff8"), open(image, "rb").read(),
              newest_source(comp, v.get("OBJS", "")))
        write(os.path.join(res_dir, "Messages"), cmodule_messages(comp), os.stat(find(comp, "Messages")[0]).st_mtime)
        for leaf in v.get("INSTRES_FILES", "").split():
            path, suffix = find(comp, leaf)
            write(os.path.join(res_dir, leaf + ("," + suffix if suffix else "")), open(path, "rb").read(),
                  os.stat(path).st_mtime)
        return

    if kind in ("CApp", "StdTools"):
        image = images.pop(component, None)
        if image is None:
            raise SystemExit(f"mkromapps: {component} is a C application: --image {component}=IMAGE")
        if kind == "StdTools":
            target = "!RunImage"                # abs.!RunImage, installed as !RunImage
        write(os.path.join(res_dir, target + ",ff8"), open(image, "rb").read(),
              newest_source(comp, v.get("OBJS", "")))

    if kind == "StdTools":
        for dotted, path, suffix, data in stdtools_files(comp, component, v):
            parts = dotted.split(".")
            if parts[0] == "Apps":              # the ROM !Run, changed where the image needs it
                data = change(component, parts[-1], data)
            write(os.path.join(out, *parts[:-1], parts[-1] + ("," + suffix if suffix else "")), data,
                  os.stat(path).st_mtime)
        return

    for leaf in v.get("RES_FILES", "").split():
        if leaf == target and kind == "BasicApp":
            prog = b"".join(open(s, "rb").read() for s in srcs)
            if not bascrunch.is_program(prog):
                prog = bascrunch.program(bascrunch.textload(prog))
            stamp = max(os.stat(s).st_mtime for s in srcs)
            write(os.path.join(res_dir, leaf + ",ffb"), bascrunch.crunch(prog, 0xFFFFFFFF), stamp)
            continue
        path, suffix = find(comp, leaf)
        data = open(path, "rb").read()
        if leaf == "Messages" and "Messages" in v.get("INSTAPP_VERSION", "").split():
            data = insert_version(data, *version(comp))
        write(os.path.join(res_dir, leaf + ("," + suffix if suffix else "")), data,
              os.stat(path).st_mtime)

    for leaf in v.get("RESAPP_FILES", "").split():
        path, suffix = find(comp, leaf)
        write(os.path.join(app_dir, leaf + ("," + suffix if suffix else "")),
              change(component, leaf, open(path, "rb").read()), os.stat(path).st_mtime,
              ATTR_WR_R if kind == "CApp" else ATTR_WR)


def main():
    args = sys.argv[1:]
    stamps, images, wrap = None, {}, None
    while args[:1] in (["--stamps"], ["--image"], ["--modwrap"]) and len(args) > 1:
        if args[0] == "--stamps":
            stamps = args[1]
        elif args[0] == "--modwrap":
            wrap = args[1]
        else:
            component, _, image = args[1].partition("=")
            images[component] = image
        args = args[2:]
    if wrap:
        if len(args) != 2:
            raise SystemExit(__doc__)
        modwrap(args[0], args[1], wrap)
        return
    if len(args) < 3:
        raise SystemExit(__doc__)
    sources, out = args[0], args[1]
    for component in args[2:]:
        build(sources, out, component, images)
    if images:
        raise SystemExit(f"mkromapps: --image for {', '.join(images)}, not a component here")
    write_attributes(out)
    if stamps:
        write_stamps(stamps, out)


if __name__ == "__main__":
    main()
