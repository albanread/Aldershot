#!/usr/bin/env python3
"""capps.py -- RISC OS C, staged for ROSGD's x32 build (C applications).

    capps.py stage RISCOS OUT [--rosasm-tools DIR] [--cmhg CMHG] [--cc CC] [--cflags FLAGS]
                              [--contract JSON] [--jobs N]
    capps.py check OUT [--no-audit]    the objects' ELF facts (x32; thread-local
                                       statics in the library and nowhere else),
                                       rlib's ObjAsm in C and rlib.a (capps.mk's),
                                       and the source pass's audit
    capps.py apps OUT --roscc R --rt DIR --rlib ARCHIVE [--spec JSON]
                                       Edit, Draw, Paint and Filer_Action linked
                                       against the library's stubs and rlib.a;
                                       what each takes, in OUT/apps/report.txt
    capps.py compare A B               two builds' objects, byte for byte
    capps.py selftest RISCOS           the header patches refuse a changed
                                       RISC_OSLib; the scan finds Norcroft forms
    capps.py cmhg RISCOS OUT [--cmhg CMHG] [--cc CC] [--cflags F] [--as AS] [--jobs N]
                                       every module description in the sources
                                       through roscc's cmhg.py, both back ends

RISCOS is the RiscOS directory of a source drop (riscos-src/BCM2835/RiscOS,
what the Makefile calls BASICVPFSRC).  OUT is written as the DDE's export
phase would leave it, for clang:

  OUT/inc/C/         the C: path.  RISC_OSLib's own C library headers,
                     ported to parse under clang (patches P1-P14 below);
                     swis.h, generated as the ROM build's makehswis does and
                     checked against api/defs, and rosgdswis.h, the SWIs
                     api/defs has that 5.30's swis.h does not; Global/ and
                     Interface/, by
                     the build's own Hdr2H from the exported asm headers (or
                     the component's hand-written h/ where it exports one);
                     the libraries' exported directories (tboxlibint/,
                     DebugLib/, AsmUtils/ ...)
  OUT/inc/tbox/      the tbox: path (ToolboxLib's headers), also as
                     OUT/inc/C/tboxlibs/ (C:'s Lib$Dir.tboxlibs)
  OUT/inc/RISCOSLIB/ the RISCOSLIB: path (rlib's headers)
  OUT/inc/OS/        the OS: path (OSLib's oslib/ headers)
  OUT/src/           the component sources, c/name -> name.c and
                     h/name -> name.h, with Norcroft's dotted include names
                     ("gadgets.actbut.h") translated, and the patch series
                     (tools/capps_patches.py) applied, then the
                     applications' own (APP_PATCHES below); each module's cmhg
                     header (cmhg -d) beside its sources
  OUT/inc-lib/       the library build's copies of the inc/ headers that
                     declare its statics, thread-local (first on its path)
  OUT/src/Apps/      the applications (Draw, Edit, Paint, FilerAct, Help), for
                     their h/ files; none of their units is built yet
  OUT/hdrunits/      the header groups' units (tools/capps_headers.py, listed
                     in OUT/hdrgroups.json): every staged client header, in a
                     unit the pass parses whether or not a built unit reaches
                     it; Q1's layout probes are made from the same units
  OUT/rules.mk       one rule per object, for capps.mk's sub-make

The tree is first staged as RISC OS has it, with the patch series, in
OUT/pre; the source pass (tools/capps_pass.py: unsigned plain bit-fields,
aligned(4) structures and unions, the library's statics thread-local) reads
clang's AST of every unit there, built and header group alike, and writes
OUT/src, OUT/inc and OUT/inc-lib.  A header staged in several places
(OSLib's in OS: and OS:oslib, ToolboxLib's, rlib's) gets the edits of all
its copies (twin_edits), since include guards let a unit meet only one.
Its edits are kept in OUT/pass.json, keyed by everything they depend on, so
an unchanged tree is not parsed again.

Every file is written only when its bytes change, so make rebuilds only what
a change in riscos-src or in a patch reaches.  Nothing is read from the
host: the C: path is the whole system include path (-nostdinc).

Which files are built is the RISC OS Makefiles' answer, read from them: the
ROM SharedCLibrary's C objects (RM_OBJS), rlib's (RLIB_OBJS), each Toolbox
module's OBJS over its VPATH, tboxlib's, ToolboxLib's (toolboxlib's
ObjFileA-Z list), with each Makefile's CDEFINES, ROMCDEFINES and CINCLUDES.
"""
import collections
import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
ROSGD = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the checkout
import capps_headers  # noqa: E402
import capps_patches  # noqa: E402
import capps_pass  # noqa: E402

# Environment the RISC OS build sets (Env/ROOL/BCM2835.sh) and path macros
# the DDE's makefile fragments define, as far as the Makefiles we read use them.
BUILD_ENV = {
    "SYSTEM": "Ursula", "STYLE": "", "SEP": "/",
    "TBOXINC": "-Itbox:", "OSINC": "-IOS:", "RINC": "-IRISCOSLIB:",
    "CEXPORTDIR": "CExport:", "LIBDIR": "Lib:",
}

# Directories under a component that hold no built source.
SKIP_DIRS = {"test", "tests", "Test", "Tests", "doc", "docs", "Doc", "Docs", "Documents", "unused",
             "utils", "tools", "o", "od", "oz", "odz", "aof", "rm", "linked", "abs", "Resources", ".git"}

# The libraries' exported header directories on the C: path, as their
# Makefiles export them (EXPDIR = <Lib$Dir>.<TARGET>).
C_EXPORTS = {
    "tboxlibint": "Toolbox/tboxlib",
    "DebugLib": "Lib/DebugLib",
    "AsmUtils": "Lib/AsmUtils",
    "callx": "Lib/callx",
    "ConfigLib": "Lib/ConfigLib",
    "SyncLib": "Lib/SyncLib",
    "Unicode": "Lib/UnicodeLib",
    "PlainArgv": "Lib/PlainArgv",
    "ModMalloc": "Lib/ModMalloc",
    "Trace": "Lib/Trace",
    "Wild": "Lib/Wild",
}
DEFMOD_HEADERS = {"ToolAction.h": "Toolbox/ToolAction/Documents/def/ToolAction"}
TBOX_DIRS = ["toolboxlib", "wimplib", "eventlib", "renderlib", "flexlib"]

# rlib's txt engine and the ROM library's C, per the RISC_OSLib Makefile.
RLIB_DFLAGS = ["-DFIELDNUM", "-DBIG_WINDOWS", "-DSETOPTIONS", "-DALLOW_OLD_PATTERNS", "-DSET_MISC_OPTIONS"]

# The ROM SharedCLibrary: its units get the library's flags and the pass's
# thread-local statics; every other component is a client.
LIB_COMPONENTS = {"clib"}

# The ROM's C applications that link rlib (proposal-v3 2.2: Draw, Edit,
# Paint, Filer_Action), compiled as their disc builds are -- each Makefile's
# OBJS and CINCLUDES, clients like rlib -- so that make capps-apps can link
# them against rlib.a and the library (package L4's check; their images and
# discs are packages A2-A5).  Their own ObjAsm is not staged: what the RAM
# image needs of it is in C in rosgd/apps/<component> (Paint's
# asm/BrushTransFunc, Filer_Action's s/allerrs), compiled with the
# component's flags; the ROM build's AppName and ResFiles are package A1a's.
APPS = [("Edit", "Apps/EditApp"), ("Draw", "Apps/Draw"), ("Paint", "Apps/Paint"),
        ("Filer_Action", "Desktop/FilerAct")]

# The applications' own patches, in the patch series' form (tools/
# capps_patches.py: staged path, RISC OS's text, ROSGD's, why) and applied
# with it as the sources are staged, each text exactly once: where an
# application's C, not the libraries', meets what x32 does differently.
#
# Draw's DrawFileIO (package D1): a DrawFile holds two doubles -- the
# options object's grid spacing and the grid object's y spacing -- as the
# FPA stores one, the word with the sign and exponent first, and Draw moves
# them between the file's objects and its view by assignment.  x32's doubles
# are IEEE little-endian, that word second, so each crossing swaps the
# words: read_options and read_grid as a loaded file's objects become the
# view's settings, write_options and write_grid as the view's become a
# saved file's.  They are the only four: the choices file is text (%lf),
# the clipboard and RAM transfers save through the same writers, and
# nothing else reads the objects' doubles.  Without them Draw wrote files
# 5.30's Draw read wrongly, read 5.30's grid spacing 1.0 as 5.3e-315, drew
# no grid and hung the desktop in draw_grid_paint.  (rlib's drawf* members
# take a double only as a scale; Paint's sprites and Edit's files hold
# none.)
FPA_ORDER = ("/* ROSGD (tools/capps.py's APP_PATCHES, package D1): a DrawFile's doubles\n"
             " * are the FPA's, the word holding the sign and exponent first; x32's are\n"
             " * IEEE little-endian, that word second.  The words swapped: each way. */\n"
             "static double fpa_order (double d)\n"
             "{  union { double d; unsigned int w [2]; } in, out;\n"
             "   in.d = d;\n"
             "   out.w [0] = in.w [1];\n"
             "   out.w [1] = in.w [0];\n"
             "   return out.d;\n"
             "}\n\n")
APP_PATCHES = [
    ("Apps/Draw/DrawFileIO.c",
     "static BOOL read_options (diagrec *diag, BOOL cleanpaper)\n",
     FPA_ORDER + "static BOOL read_options (diagrec *diag, BOOL cleanpaper)\n",
     "FPA word order: DrawFile doubles"),
    ("Apps/Draw/DrawFileIO.c",
     "            diag->view->grid.space [1]     = opt->grid.space;\n",
     "            diag->view->grid.space [1]     = fpa_order (opt->grid.space);   /* ROSGD */\n",
     "FPA word order: read_options"),
    ("Apps/Draw/DrawFileIO.c",
     "   opt->grid.space    = diag->view->gridunit [cm].space [0];\n",
     "   opt->grid.space    = fpa_order (diag->view->gridunit [cm].space [0]);   /* ROSGD */\n",
     "FPA word order: write_options"),
    ("Apps/Draw/DrawFileIO.c",
     "            diag->view->gridunit [cm].space [grid_Y] = grid->y_space;\n",
     "            diag->view->gridunit [cm].space [grid_Y] = fpa_order (grid->y_space);   /* ROSGD */\n",
     "FPA word order: read_grid"),
    ("Apps/Draw/DrawFileIO.c",
     "      {  .y_space  = diag->view->gridunit [cm].space [grid_Y],\n",
     "      {  .y_space  = fpa_order (diag->view->gridunit [cm].space [grid_Y]),   /* ROSGD */\n",
     "FPA word order: write_grid"),
]

TOOLBOX_MODULES = ["Toolbox", "Window", "Menu", "IconBar", "ColourDbox", "ColourMenu", "DCS", "FileInfo",
                   "FontDbox", "FontMenu", "PrintDbox", "ProgInfo", "SaveAs", "Scale", "Gadgets", "ToolAction"]


# ---------------------------------------------------------------------------
# small helpers

def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("latin-1")


class Writer:
    """Write-if-changed, and remember every path written (for pruning)."""

    def __init__(self):
        self.written = set()
        self.changed = 0
        self.filters = {}        # absolute path -> bytes -> bytes (the patch series)

    def bytes(self, path, data):
        path = os.path.abspath(path)
        self.written.add(path)
        f = self.filters.pop(path, None)
        if f:
            data = f(data)
        try:
            with open(path, "rb") as f:
                if f.read() == data:
                    return
        except FileNotFoundError:
            os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        self.changed += 1

    def text(self, path, s):
        self.bytes(path, s.encode("latin-1"))


def make_vars(path, env=BUILD_ENV):
    """The variables a RISC OS Makefile assigns: =, ?=, :=, +=; continued lines."""
    vals = {}
    text = read_text(path).replace("\\\r\n", " ").replace("\\\n", " ")
    for line in text.split("\n"):
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*([?:+]?)=\s*(.*)$", line)
        if not m:
            continue
        name, op, val = m.group(1), m.group(2), m.group(3).split("#")[0].strip()
        if op == "?" and name in vals:
            continue
        vals[name] = (vals[name] + " " + val) if op == "+" and name in vals else val

    def expand(s, depth=0):
        if depth > 10:
            return s
        return re.sub(r"\$[{(](\w+)[})]",
                      lambda m: expand(vals.get(m.group(1), env.get(m.group(1), "")), depth + 1), s)
    return {k: expand(v).strip() for k, v in vals.items()}, expand


def unixify(src_root, dst_root, w, rel_base=""):
    """X/c/name -> X/name.c, X/h/name -> X/name.h; also VersionNum."""
    n = 0
    for dp, dn, fn in os.walk(src_root):
        dn[:] = sorted(d for d in dn if d not in SKIP_DIRS)
        rel = os.path.relpath(dp, src_root)
        parts = [] if rel == "." else rel.split(os.sep)
        if parts and parts[-1] in ("c", "h"):
            ext, base = parts[-1], parts[:-1]
            for f in sorted(fn):
                if f.startswith("."):
                    continue
                dst = os.path.join(dst_root, rel_base, *base, f + "." + ext)
                w.bytes(dst, translate_includes(open(os.path.join(dp, f), "rb").read()))
                n += 1
        elif "VersionNum" in fn:
            w.bytes(os.path.join(dst_root, rel_base, *parts, "VersionNum"),
                    open(os.path.join(dp, "VersionNum"), "rb").read())
    return n


_DOTTED = re.compile(rb'(#\s*include\s*")([A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+)\.h"')


def translate_includes(data):
    """Norcroft's filename translation: "gadgets.actbut.h" names gadgets/h/actbut."""
    return _DOTTED.sub(lambda m: m.group(1) + m.group(2).replace(b".", b"/") + b'.h"', data)


# ---------------------------------------------------------------------------
# the C library headers: P1-P14

def _must(name, tag, t, old, new, regex=False, flags=0):
    """One of a header's patches, which must meet its text: a change upstream
    stops the stage rather than leaving Norcroft's syntax behind."""
    if regex:
        t2, n = re.subn(old, new, t, flags=flags)
    else:
        n = t.count(old)
        t2 = t.replace(old, new)
    if not n:
        raise SystemExit("capps: %s.h: patch %s no longer matches (%s): RISC_OSLib changed"
                         % (name, tag, old[:60].replace("\n", "\\n")))
    return t2


def port_clib_header(name, t):
    """RISC_OSLib's clib/h/<name>, made to parse under clang for x32.

    P1  #pragma include_only_once -> #pragma once (and force_top_level dropped)
    P2  typedef char *__va_list[1] (stdio), __va_listwc (wchar)
                                  -> __builtin_va_list: x32's va_list is a structure
    P3  stdarg.h                  -> written here over clang's builtins
    P4  math.h's classification macros (___typeof, ___assert) -> _Generic
    P5  __caller_narrow           -> removed (a Norcroft calling hint)
    P6  offsetof (___type)        -> __builtin_offsetof
    P7  fabs/fabsf (__abs)        -> __builtin_fabs/__builtin_fabsf
    P8  abs/labs (__abs)          -> __builtin_abs/__builtin_labs
    P9  INFINITY/NAN (0f_ literals), isless... (__less operators) -> clang built-ins
    P10 complex.h's _Imaginary (Norcroft ___i) -> clang's _Complex I, creal/cimag,
        CMPLX by __builtin_complex (clang has no imaginary types, as C11 allows)
    P11 tgmath.h (Norcroft's ___select) -> written here (tgmath_header): the same
        macros, each a _Generic over the f/double/l and complex names RISC_OSLib's
        list gives (imaginary arguments, which clang has not, dropped)
    P12 stdatomic.h: _Atomic(X) is X, as for Norcroft, so the header's own
        _Generic dispatch to _kernel_atomic_* sees the plain types (clang drops
        _Atomic from a _Generic controlling expression)
    P13 setjmp.h: setjmp declared returns_twice.  -ffreestanding (the flag
        table's) turns clang's builtins off, and with them its knowledge that
        setjmp returns twice: without the attribute a caller's registers may
        be kept across the second return, and the call may even be a tail
        call (Norcroft knows setjmp by name)
    P14 math.h: a client's __d_ and __r_ names -> the library's functions
        (MATH_MAP).  math.h makes floor(x) __d_floor(x), atan(x) __d_atan(x),
        sinf(x) __r_sin(x) and so on: names Norcroft compiles inline, as FPA
        instructions, which no library exports.  To clang they are calls, so
        a client's each becomes the entry that does what the instruction
        does -- floor, atan, sinf -- (5.30's RISC_OSLib defines those entries
        as the inline forms: math.c's "double (floor)(double x) { return
        floor(x); }").  Not for the library's own build (SHARED_C_LIBRARY),
        whose C calls the __d_ names and has them (rosgd/clib/maths.c)

    P4 and P6-P14 must each meet their text (_must); after staging,
    scan_norcroft fails the stage on any Norcroft form left in inc/C.
    """
    t = t.replace("#pragma include_only_once", "#pragma once").replace("#pragma force_top_level", "")
    t = re.sub(r"typedef\s+char\s*\*\s*(__va_list\w*)\s*\[1\]\s*;", r"typedef __builtin_va_list \1;", t)
    t = t.replace("__caller_narrow", "")
    if name == "math":
        t = _must(name, "P4", t, r"#define __assertfp\(r\).*?\n\n", "", True, re.S)
        t = _must(name, "P4", t, r"#define __classmacro\(fn,r\) \(__assertfp\(r\),\\\n.*?\n.*?\n",
                  "#define __classmacro(fn,r) _Generic((r), float: __##fn##f((float)(r)), "
                  "default: __##fn##d((double)(r)))\n", True, re.S)
        t = _must(name, "P7", t, "((void) sizeof (fabs)(x), __abs (double) (x))", "__builtin_fabs(x)")
        t = _must(name, "P7", t, "((void) sizeof (fabsf)(x), __abs (float) (x))", "__builtin_fabsf(x)")
        t = _must(name, "P9", t, "0f_7F800000", "__builtin_inff()")
        t = _must(name, "P9", t, "0f_7FC00001", '__builtin_nanf("0x400001")')
        for op, b in (("unordered", "isunordered"), ("greaterequal", "isgreaterequal"),
                      ("lessequal", "islessequal"), ("lessgreater", "islessgreater"),
                      ("greater", "isgreater"), ("less", "isless")):
            t = _must(name, "P9", t, "((x) __%s (y))" % op, "__builtin_%s((x),(y))" % b)
        end = "#ifdef __FP_FAST_FMAF\n#  define fmaf(x,y,z) __r_fma(x,y,z)\n#endif\n#endif\n"
        t = _must(name, "P14", t, end, end + math_map())
    if name == "stddef":
        t = _must(name, "P6", t, r"#define offsetof\(type, member\) \\\n.*?\n(?=\S|$)", "", True, re.S)
        t += "\n#undef offsetof\n#define offsetof(type, member) __builtin_offsetof(type, member)\n"
    if name == "stdlib":
        t = _must(name, "P8", t, "((void) sizeof (abs)(j), __abs (int) (j))", "__builtin_abs(j)")
        t = _must(name, "P8", t, "((void) sizeof (labs)(j), __abs (long int) (j))", "__builtin_labs(j)")
    if name == "complex":
        t = _must(name, "P10", t, "#define _Imaginary_I ___i", "#define _Imaginary_I (__extension__ 1.0iF)")
        t = _must(name, "P10", t, "#define _Complex_I   (0.0F+___i)",
                  "#define _Complex_I   (__extension__ 1.0iF)")
        t = _must(name, "P10", t, r"#define I\s+_Imaginary_I", "#define I            _Complex_I", True)
        for fn, bi in (("cimag", "__builtin_cimag"), ("cimagf", "__builtin_cimagf"), ("cimagl", "__builtin_cimagl")):
            t = _must(name, "P10", t, r"#define %s\(z\) .*" % fn, "#define %s(z) %s(z)" % (fn, bi), True)
        for fn, bi in (("conj", "__builtin_conj"), ("conjf", "__builtin_conjf"), ("conjl", "__builtin_conjl")):
            t = _must(name, "P10", t, r"#define %s\(z\) .*" % fn, "#define %s(z) %s(z)" % (fn, bi), True)
        t = _must(name, "P10", t, r"#define CMPLX\(x,y\)\s+.*",
                  "#define CMPLX(x,y)  __builtin_complex((double)(x), (double)(y))", True)
        t = _must(name, "P10", t, r"#define CMPLXF\(x,y\)\s+.*",
                  "#define CMPLXF(x,y) __builtin_complex((float)(x), (float)(y))", True)
        t = _must(name, "P10", t, r"#define CMPLXL\(x,y\)\s+.*",
                  "#define CMPLXL(x,y) __builtin_complex((long double)(x), (long double)(y))", True)
    if name == "tgmath":
        t = tgmath_header(t)
    if name == "setjmp":
        t = _must(name, "P13", t, "int setjmp(jmp_buf /*env*/);",
                  "int setjmp(jmp_buf /*env*/) __attribute__((returns_twice)); /* ROSGD (capps.py P13) */")
    if name == "stdatomic":
        t = _must(name, "P12", t,
                  "#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 201112) || "
                  "defined(__STDC_NO_ATOMICS__)\n#define _Atomic(X) X",
                  "#if 1 /* ROSGD (capps.py P12): as for Norcroft, so the _Generic below sees the "
                  "plain type */\n#define _Atomic(X) X")
    return t


# P14: Norcroft's inline names in math.h, each the library's entry for it
MATH_MAP = [("acos", "acos"), ("asin", "asin"), ("atan", "atan"), ("cos", "cos"), ("sin", "sin"),
            ("tan", "tan"), ("log", "log"), ("lg10", "log10"), ("exp", "exp"), ("abs", "fabs"),
            ("sqrt", "sqrt"), ("floor", "floor"), ("ceil", "ceil"), ("trunc", "trunc"), ("rint", "rint"),
            ("lrint", "lrint"), ("rem", "remainder"), ("fma", "fma")]


def math_map():
    lines = ["\n/* ROSGD (capps.py P14): Norcroft compiles the __d_ and __r_ names inline,",
             " * as FPA instructions; to clang they are calls, and no library exports",
             " * them.  A client's each is the library's entry that does what the",
             " * instruction does (5.30's are those instructions: math.c's",
             " * \"double (floor)(double x) { return floor(x); }\").  The library's own",
             " * build (SHARED_C_LIBRARY) has the names themselves. */",
             "#ifndef SHARED_C_LIBRARY"]
    lines += ["#define __d_%s %s" % (n, f) for n, f in MATH_MAP]
    lines += ["#define __r_%s %sf" % (n, f) for n, f in MATH_MAP]
    lines += ["#endif", ""]
    return "\n".join(lines)


_TG_DEF = re.compile(r"^#define (\w+)\(([^)]*)\)\s+(_TG[12CIZ]?)\((.*)\)\s*$")


def tgmath_header(t):
    """P11: RISC_OSLib's tgmath.h as _Generic.  The macros, their names and
    which arguments are generic come from RISC_OSLib's own list (the part
    before its Norcroft inlining section); the selection is C99's 7.22:
    any long double argument -> the l function, else any double or integer
    -> the unsuffixed one, else the f one; any complex argument (for the
    functions with a complex counterpart) -> the c-prefixed one."""
    head = t.split("#ifndef __TGMATH_NO_INLINING")[0]
    out = ["/* tgmath.h -- ROSGD's (capps.py P11): RISC_OSLib's type-generic macros,\n"
           " * each a _Generic over the functions <math.h> and <complex.h> declare.\n"
           " * Norcroft's ___select and imaginary types, which clang has not, are gone:\n"
           " * an imaginary argument is a complex one here, as <complex.h>'s I is. */\n"
           "#pragma once\n#ifndef __tgmath_h\n#define __tgmath_h\n\n"
           "#include <math.h>\n#include <complex.h>\n\n"
           "/* an argument's type for the selection: integers are double */\n"
           "#define __tg_r(x) _Generic((x), float: 0.0f, float _Complex: 0.0f, long double: 0.0L, \\\n"
           "                          long double _Complex: 0.0L, default: 0.0)\n"
           "#define __tg_c(x) _Generic((x), float: 0.0f, long double: 0.0L, \\\n"
           "                          float _Complex: (float _Complex)0, \\\n"
           "                          double _Complex: (double _Complex)0, \\\n"
           "                          long double _Complex: (long double _Complex)0, default: 0.0)\n\n"]
    n = 0
    for line in head.split("\n"):
        m = _TG_DEF.match(line)
        if not m or m.group(1).startswith("_"):
            continue                                # not a public macro (_TGC, _TGI)
        macro, params, kind, args = m.group(1), m.group(2), m.group(3), [a.strip() for a in m.group(4).split(",")]
        params = [p.strip() for p in params.split(",")]
        if kind in ("_TG", "_TG1", "_TG2"):
            base, generic = args[0], args[1:]
            if kind == "_TG1":
                generic = generic[:1]
            elif kind == "_TG2":
                generic = generic[:2]
            sel = " + ".join("__tg_r(%s)" % a for a in generic)
            body = "_Generic(%s, float: %sf, long double: %sl, default: %s)" % (sel, base, base, base)
        else:
            if kind == "_TGZ":
                real, cplx, generic = args[0], args[2], args[3:]
            else:                                   # _TGC, _TGI
                real, cplx, generic = args[0], "c" + args[0], args[1:]
            sel = " + ".join("__tg_c(%s)" % a for a in generic)
            body = ("_Generic(%s, float: %sf, long double: %sl, float _Complex: %sf, \\\n"
                    "         double _Complex: %s, long double _Complex: %sl, default: %s)"
                    % (sel, real, real, cplx, cplx, cplx, real))
        out.append("#undef %s\n#define %s(%s) %s(%s)\n" % (macro, macro, ", ".join(params), body,
                                                            ", ".join(params)))
        n += 1
    if n < 50:
        raise SystemExit("capps: tgmath.h: patch P11 found %d type-generic macros: RISC_OSLib changed" % n)
    out.append("\n#endif\n")
    return "".join(out)


# Norcroft's forms that clang does not parse, which the ported headers must
# not keep: its internal operators and literals, pragmas and calling hints.
_NORCROFT = re.compile(r"___\w+|\b__abs\b|\b0f_[0-9A-Fa-f]+|__caller_narrow|include_only_once|"
                       r"force_top_level|\) __(?:less|greater|lessequal|greaterequal|lessgreater|"
                       r"unordered) \(|char\s*\*\s*__va_list|_Imaginary\b")


def scan_norcroft(cinc, names):
    """Each ported clib header, its comments aside, free of Norcroft's forms
    (complex.h's `#define imaginary _Imaginary`, the C99 spelling, is kept:
    only a use of it fails, under clang as it should)."""
    bad = []
    for n in sorted(names):
        p = os.path.join(cinc, n + ".h")
        if not os.path.isfile(p):
            continue
        t = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", "", m.group(0)), read_text(p), flags=re.S)
        for i, line in enumerate(t.split("\n"), 1):
            line = re.sub(r"//.*", "", line)
            if re.match(r"\s*#\s*(define|undef)\s+imaginary\b", line):
                continue
            for m in _NORCROFT.findall(line):
                bad.append("%s.h:%d: %s" % (n, i, m))
    if bad:
        raise SystemExit("capps: Norcroft forms left in the ported headers:\n  " + "\n  ".join(bad[:30]))


STDARG = """/* stdarg.h -- ROSGD's, over clang's builtins (capps.py P3).
 * RISC OS's is Norcroft's: va_list a char *[1] walked by ___type.  On x32
 * va_list is the ABI's structure; <stdio.h>, <wchar.h> and <swis.h> name
 * the same type (P2). */
#pragma once
#ifndef __stdarg_h
#define __stdarg_h
typedef __builtin_va_list va_list;
#define va_start(ap, parmN) __builtin_va_start(ap, parmN)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)
#define va_copy(dest, src)  __builtin_va_copy(dest, src)
#define __va_copy(dest, src) __builtin_va_copy(dest, src)
#endif
"""


# ---------------------------------------------------------------------------
# swis.h: makehswis's list of headers, then api/defs

def parse_addswi(paths, swiname, swibase):
    """The SWI names AddSWI declares in ObjAsm headers, in makehswis's order.

    Follows the assembler's storage counter as the headers drive it:
    `SWIClass SETS`, `^ <expr>` (a chunk base, a SWI already named, &hex or
    decimal, joined by + and -), and `AddSWI name[, value]`.  A `^` whose
    expression it cannot evaluate stops the stage rather than guessing."""
    names = collections.OrderedDict()
    unknown = []
    # the assembler's other symbols, as the headers define them (EQU and *);
    # ModHand's chunk size, which makehswis GETs after the SWI headers
    syms = {"Module_SWIChunkSize": 0x40}

    def value(tok):
        if re.fullmatch(r"&[0-9A-Fa-f]+", tok):
            return int(tok[1:], 16)
        if re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", tok):
            return int(tok, 0)
        if tok.endswith("SWI_Base") and tok[:-5] in swibase:
            return swibase[tok[:-5]]
        if tok in names:
            return names[tok]
        return syms.get(tok)

    def evaluate(expr, cls=None):
        total = 0
        for sign, term in re.findall(r"([+-]?)\s*([^+-]+)", expr.strip()):
            prod = 1
            for f in term.split("*"):
                v = value(f.strip())
                if v is None:
                    return None
                prod *= v
            total += -prod if sign == "-" else prod
        return total

    for p in paths:
        cls, cur = None, 0
        for line in read_text(p).split("\n"):
            l2 = line.split(";")[0]
            m = re.match(r"^(\w+)\s+(?:EQU|\*)\s+(.+?)\s*$", l2)
            if m:
                v = evaluate(m.group(2))
                if v is not None:
                    syms[m.group(1)] = v
                continue
            m = re.match(r'^\s*SWIClass\s+SETS\s+"([^"]+)"', l2)
            if m:
                cls = m.group(1)
                continue
            m = re.match(r"^\s*SWIClass\s+SETS\s+(\w+SWI)_Name", l2)
            if m:
                cls, cur = swiname.get(m.group(1)), swibase.get(m.group(1), 0)
                continue
            m = re.match(r"^\s*\^\s*([^,]+?)\s*$", l2)
            if m and cls:
                v = evaluate(m.group(1), cls)
                if v is None:
                    unknown.append("%s: ^ %s" % (os.path.basename(p), m.group(1)))
                    cls = None      # the counter is lost: no AddSWI until the next SWIClass
                else:
                    cur = v
                continue
            m = re.match(r"^\s*AddSWI\s+(\w+)\s*(?:,\s*(\S+))?", l2)
            if m and cls:
                v = evaluate(m.group(2), cls) if m.group(2) else cur
                if v is None:
                    raise SystemExit("capps: swis.h: %s: cannot evaluate %s" % (p, line.strip()))
                names[cls + "_" + m.group(1)] = v   # makehswis #undefs first: the last wins
                cur = v + 1
            elif re.match(r"^\s*AddSWI\s", l2) and not cls:
                raise SystemExit("capps: swis.h: %s: AddSWI with no SWIClass or a lost counter: %s"
                                 % (p, line.strip()))
    return names, unknown


def custom_export(src, name):
    """A header a component exports by a rule of its own, which the export
    tree does not model: `${EXP_HDR}.<name>: hdr.<name>` then a copy
    (RISC_OSLib's SharedCLib)."""
    rule = re.compile(r"^\$\{EXP_HDR\}\." + re.escape(name) + r"\s*:\s*hdr\." + re.escape(name) + r"\s*$", re.M)
    for dp in _component_dirs(src):
        mk = os.path.join(dp, "Makefile")
        p = os.path.join(dp, "hdr", name)
        if os.path.isfile(p) and rule.search(read_text(mk)):
            return p
    return None


def make_swis_h(riscos, hdr_dirs, clib_h):
    src = os.path.join(riscos, "Sources")
    glob_dir = hdr_dirs[0]

    def find(h):
        for d in hdr_dirs:
            p = os.path.join(d, h.replace(".", "/"))
            if os.path.isfile(p):
                return p
        return None

    swiname, swibase = {}, {}
    swis_hdr = os.path.join(glob_dir, "SWIs")
    for line in read_text(swis_hdr).split("\n"):
        m = re.match(r'^(\w+SWI)_Name\s+SETS\s+"([^"]+)"', line)
        if m:
            swiname[m.group(1)] = m.group(2)
        m = re.match(r"^(\w+SWI)_Base\s+EQU\s+&([0-9A-Fa-f]+)", line)
        if m:
            swibase[m.group(1)] = int(m.group(2), 16)
    # makehswis: GET Hdr:SWIs (and three more with no AddSWI), then
    # swioptions.s, which SWIOptions,feb makes of every Hdr: header present
    order = ["SWIs"]
    opts = read_text(os.path.join(src, "Lib/RISC_OSLib/SWIOptions,feb"))
    order += re.findall(r"IfThere\s+Hdr:(\S+)\s+Then", opts)
    paths, missing = [], []
    for h in order:
        p = find(h) or custom_export(src, h)
        (paths if p else missing).append(p or h)
    names, unknown = parse_addswi(paths, swiname, swibase)
    # ROSGD's own definitions: every SWI api/defs names must agree
    defs, conflicts, added = {}, [], []
    for f in sorted(os.listdir(os.path.join(ROSGD, "api", "defs"))):
        if not f.endswith(".toml"):
            continue
        with open(os.path.join(ROSGD, "api", "defs", f), "rb") as fh:
            d = tomllib.load(fh)
        for s in d.get("swi", []):
            if "name" in s and "number" in s:
                defs[s["name"]] = (s["number"], f)
    for n, (v, f) in sorted(defs.items()):
        if n in names:
            if names[n] != v:
                conflicts.append("%s: RISC OS headers &%X, api/defs/%s &%X" % (n, names[n], f, v))
        else:
            names[n] = v
            added.append(n)
    # a `^` the parser cannot follow (error bases, workspace layouts) only
    # matters if an AddSWI comes after it, and that stops parse_addswi
    if conflicts:
        raise SystemExit("capps: swis.h: api/defs disagrees with RISC OS:\n  " + "\n  ".join(conflicts))
    # swis.h is RISC OS's: makehswis's names only.  ROSGD's own SWIs (its
    # test modules, PTY, Socket, ...) are in <rosgdswis.h>, for a program
    # that wants them by name.
    head = port_clib_header("swis", read_text(os.path.join(src, "Lib/RISC_OSLib/h/swisheader")))
    out = [head, "\n/* SWI names: makehswis's headers (%d) -- capps.py */\n" % (len(names) - len(added))]
    for k, v in names.items():
        if k not in added:
            out.append("#undef %s\n#define %s 0x%x\n" % (k, k, v))
    out.append("\n#endif\n")
    extra = ["/* rosgdswis.h -- ROSGD's SWIs that RISC OS 5.30's <swis.h> does not name:\n"
             " * api/defs, checked against RISC OS's headers (capps.py).  Include it after\n"
             " * <swis.h>. */\n#pragma once\n#include \"swis.h\"\n"]
    for k in added:
        extra.append("#undef %s\n#define %s 0x%x\n" % (k, k, names[k]))
    return "".join(out), len(names) - len(added), added, missing, "".join(extra)


# ---------------------------------------------------------------------------
# Global/ and Interface/ C headers, as the export phase makes them

def c_export_header(riscos, hdr_dirs, kind, base, hdr2h, tmp, cmhg=None, cinc=None):
    """C:Global/<base>.h or C:Interface/<base>.h: the component's hand-written
    h/ where its Makefile exports one (HDRS) -- with, where it names the
    header CMHGAUTOHDR, the SWI names from its cmhg -d header appended, as
    BuildSys's CModule does -- else Hdr2H of the asm header, with the h/ part
    first where the Makefile FAppends the two."""
    src = os.path.join(riscos, "Sources")
    # the exported asm header: the directory the C name gives first, then Hdr$Path's order
    order = [d for d in hdr_dirs if os.path.basename(d) == kind] + hdr_dirs
    asm = next((os.path.join(d, base) for d in order if os.path.isfile(os.path.join(d, base))), None)
    # the component that owns the header: one with hdr/<base> or h/<base>
    owners = []
    for dp in _component_dirs(src):
        if os.path.isfile(os.path.join(dp, "h", base)) or os.path.isfile(os.path.join(dp, "hdr", base)):
            owners.append(dp)
    for dp in owners:
        mk = os.path.join(dp, "Makefile")
        if not os.path.isfile(mk) or not os.path.isfile(os.path.join(dp, "h", base)):
            continue
        mt = read_text(mk)
        hdrs = " ".join(re.findall(r"^\s*HDRS\s*[+:?]?=\s*(.*)$", mt, re.M))
        if re.search(r"\b" + re.escape(base) + r"\b", hdrs):
            text = read_text(os.path.join(dp, "h", base))
            mv, _ = make_vars(mk)
            if mv.get("CMHGAUTOHDR") == base and cmhg:
                return text + cmhg_swi_block(dp, mv, tmp, cmhg, cinc), "h/ (HDRS) + cmhg's SWIs (CMHGAUTOHDR)"
            return text, "h/ (HDRS)"
    if asm is None:
        return None, "missing"
    # Hdr2H makes the include guard from the output path it is given: a
    # name relative to tmp, so the guard is GLOBAL_CMOS_H-like (RISC OS's
    # build gives the same), not the build tree's absolute path (#40)
    name = kind + "_" + base
    out = os.path.join(tmp, name)
    r = subprocess.run(["perl", os.path.abspath(hdr2h), os.path.abspath(asm), name], cwd=tmp,
                       capture_output=True, text=True)
    if r.returncode or not os.path.isfile(out):
        raise SystemExit("capps: Hdr2H failed on %s: %s" % (asm, r.stderr.strip()[:200]))
    gen = read_text(out)
    for dp in owners:
        mk = os.path.join(dp, "Makefile")
        hp = os.path.join(dp, "h", base)
        if os.path.isfile(hp) and os.path.isfile(mk) and \
                re.search(r"F[Aa][Pp][Pp][Ee][Nn][Dd].*\b" + re.escape(base) + r"\b", read_text(mk)):
            return "#pragma once\n" + read_text(hp) + "\n" + gen, "h/ + Hdr2H (FAppend)"
    return gen, "Hdr2H"


def cmhg_swi_block(dp, mv, tmp, cmhg, cinc):
    """CModule's ${CMHGAUTOHDR}_h_: from the cmhg -d header, every range of
    lines from one matching ".ifndef <CMHGFILE_SWIPREFIX>" to the next
    matching "endif" (its awk range, /start/,/end/) -- the SWI names and
    numbers.  CMHGFILE and CMHGFILE_SWIPREFIX default as CModule's do, to
    ${TARGET}Hdr and ${TARGET}, TARGET to COMPONENT.  The description's
    includes are found on the staged C: path (cinc): the Global/ headers are
    written before any Interface/ one."""
    target = mv.get("TARGET") or mv.get("COMPONENT", "")
    cf = mv.get("CMHGFILE") or (target + "Hdr")
    prefix = mv.get("CMHGFILE_SWIPREFIX") or target
    hd = os.path.join(tmp, "cmhgautohdr_" + cf + ".h")
    r = subprocess.run([sys.executable, cmhg, os.path.join(dp, "cmhg", cf), "-o", hd[:-2] + ".c", "-d", hd,
                        "-I", dp] + (["-I", cinc] if cinc else []), capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("capps: cmhg failed on %s:\n%s" % (os.path.join(dp, "cmhg", cf), r.stderr))
    block, on = [], False
    for line in read_text(hd).split("\n"):
        if not on:
            on = re.search(r".ifndef " + re.escape(prefix), line) is not None
        if on:
            block.append(line)
            if "endif" in line:
                on = False
    if not block:
        raise SystemExit("capps: %s: no #ifndef %s block in cmhg's header" % (cf, prefix))
    return "\n".join(block) + "\n"


_COMPONENT_DIRS = None


def _component_dirs(src):
    global _COMPONENT_DIRS
    if _COMPONENT_DIRS is None:
        _COMPONENT_DIRS = []
        for dp, dn, fn in os.walk(src):
            dn[:] = sorted(d for d in dn if d not in SKIP_DIRS and d not in ("c", "s", "h", "hdr"))
            if "Makefile" in fn:
                _COMPONENT_DIRS.append(dp)
    return _COMPONENT_DIRS


# ---------------------------------------------------------------------------
# the components: which objects, which flags

class Unit:
    def __init__(self, comp, name, src, flags):
        self.comp, self.name, self.src, self.flags = comp, name, src, flags


def c_path(out):
    """C: as Env/!Common sets C$Path: CLib, RISC_OSLib (rlib's headers), the C
    export directory, Lib -- the first, third and fourth merged in inc/C."""
    return [os.path.join(out, "inc", "C"), os.path.join(out, "inc", "RISCOSLIB")]


def include_flags(spec, compdir, out):
    """A Makefile's CINCLUDES ('-Itbox:,C:', '-I${CEXPORTDIR} -IOS:') as -I paths."""
    inc = os.path.join(out, "inc")
    paths = []
    for tok in spec.split():
        if not tok.startswith("-I"):
            continue
        for p in tok[2:].split(","):
            p = p.strip()
            if not p:
                continue
            if p in ("@", "."):
                paths.append(compdir)
            elif p.startswith("^."):
                paths.append(os.path.join(os.path.dirname(compdir), p[2:].replace(".", "/")))
            elif ":" in p:
                pfx, rest = p.split(":", 1)
                rest = rest.strip("/").replace(".", "/")
                # <cexport$dir> and <Lib$Dir> are two of C$Path's places, merged in inc/C
                root = {"C": "C", "tbox": "tbox", "OS": "OS", "RISCOSLIB": "RISCOSLIB",
                        "CExport": "C", "Lib": "C"}.get(pfx)
                if root is None:
                    raise SystemExit("capps: unknown path %s in %r" % (p, spec))
                if pfx in ("C", "Lib") and rest == "tboxlibs":
                    root, rest = "tbox", ""
                if pfx == "C" and not rest:
                    paths += c_path(out)
                else:
                    paths.append(os.path.join(inc, root, rest) if rest else os.path.join(inc, root))
            else:
                paths.append(os.path.join(compdir, p))
    return paths


def units(riscos, out):
    """The units, with OUT's final paths; a source exists if it was staged (OUT/pre)."""
    src = os.path.join(riscos, "Sources")
    s = lambda *p: os.path.join(out, "src", *p)  # noqa: E731
    staged = lambda p: os.path.isfile(os.path.join(out, "pre", os.path.relpath(p, out)))  # noqa: E731
    cinc = c_path(out)
    res = []

    def add(comp, dirs, names, defs, incs, extra=None):
        for n in names:
            for d in dirs:
                c = os.path.join(d, n + ".c")
                if staged(c):
                    res.append(Unit(comp, n, c, defs + (extra or {}).get(n, []) + ["-I" + i for i in incs]))
                    break

    # RISC_OSLib: the ROM SharedCLibrary's C (RM_OBJS: .c.rm_o) and rlib (RLIB_OBJS: .c.o_rl)
    mv, _ = make_vars(os.path.join(src, "Lib/RISC_OSLib/Makefile"))
    rm = [o.split(".", 1)[1] for o in mv["RM_OBJS"].split()]
    rl = [o.split(".", 1)[1] for o in mv["RLIB_OBJS"].split()]
    lib = s("Lib/RISC_OSLib")
    lib_inc = [os.path.join(out, "inc-lib", os.path.relpath(p, os.path.join(out, "inc"))) for p in cinc]
    add("clib", [lib], rm, ["-DDDE", "-D__ARM", "-DSHARED_C_LIBRARY", "-DUROM"], [lib] + lib_inc + cinc,
        {"armsys": ['-DLIB_SHARED="Shared "'], "string": ["-DSEPARATE_MEMCPY", "-DSEPARATE_MEMSET"]})
    add("rlib", [lib + "/rlib"], rl, ["-DDDE"] + RLIB_DFLAGS, [lib + "/rlib"] + cinc,
        {"trace": ["-DTRACE"]})

    # tboxlib and ToolboxLib: CLibrary components
    for comp, rel in (("tboxlib", "Toolbox/tboxlib"), ("eventlib", "Toolbox/ToolboxLib/eventlib"),
                      ("flexlib", "Toolbox/ToolboxLib/flexlib"), ("renderlib", "Toolbox/ToolboxLib/renderlib")):
        mv, _ = make_vars(os.path.join(src, rel, "Makefile"))
        d = s(rel)
        dirs = [d] + [os.path.join(d, v) for v in mv.get("VPATH", "").split()]
        add(comp, dirs, mv.get("OBJS", "").split(), mv.get("CDEFINES", "").split(),
            [d] + include_flags(mv.get("CINCLUDES", ""), d, out) + cinc)
    rel = "Toolbox/ToolboxLib/toolboxlib"
    d = s(rel)
    objs = re.findall(r"sources\.(\w+)\.o\.(\w+)", read_text(os.path.join(src, rel, "ObjFileA-Z")))
    mv, _ = make_vars(os.path.join(src, rel, "Makefile"))
    for sub, n in objs:
        c = os.path.join(d, "sources", sub, n + ".c")
        if staged(c):
            res.append(Unit("toolboxlib", sub + "/" + n, c,
                            ["-I" + i for i in [d] + include_flags(mv.get("CINCLUDES", ""), d, out) + cinc]))

    # the Toolbox modules: CModule, ROM build
    for m in TOOLBOX_MODULES:
        rel = "Toolbox/" + m
        mv, _ = make_vars(os.path.join(src, rel, "Makefile"))
        d = s(rel)
        dirs = [d] + [os.path.join(d, v) for v in mv.get("VPATH", "").split()]
        defs = (mv.get("CDEFINES", "") + " " + mv.get("ROMCDEFINES", "")).split()
        add(m, dirs, mv.get("OBJS", "").split(), defs,
            [d] + include_flags(mv.get("CINCLUDES", ""), d, out) + cinc)

    # the applications: a Makefile's OBJS name objects "o.Name" or "Name", in
    # any case (RISC OS's names are case-insensitive: Paint's o.colourpanel
    # is c.ColourPanel); a name with no C source (Paint's BrushTransFunc,
    # Filer_Action's allerrs: ObjAsm) is not a C unit
    staged_as = {r: "Apps/" + n for n, r in capps_headers.APPS.items()}
    for comp, rel in APPS:
        mv, _ = make_vars(os.path.join(src, rel, "Makefile"))
        d = s(staged_as[rel])
        pre_d = os.path.join(out, "pre", os.path.relpath(d, out))
        have = {f[:-2].lower(): f[:-2] for f in (os.listdir(pre_d) if os.path.isdir(pre_d) else [])
                if f.endswith(".c")}
        names = [have.get(o.split(".", 1)[-1].lower()) for o in mv.get("OBJS", "").split()]
        incs = [d] + include_flags(mv.get("CINCLUDES", ""), d, out) + cinc
        add(comp, [d], [n for n in names if n], mv.get("CDEFINES", "").split(), incs)
        # and its ObjAsm, in C (rosgd/ports/<component> lower-cased, D14)
        own = os.path.join(ROSGD, "ports", comp.lower())
        for f in sorted(os.listdir(own)) if os.path.isdir(own) else []:
            if f.endswith(".c"):
                res.append(Unit(comp, f[:-2], os.path.join(own, f),
                                mv.get("CDEFINES", "").split() + ["-I" + i for i in incs]))

    # the flag table's own probe (tests/capps)
    res.append(Unit("probe", "flags", os.path.join(ROSGD, "tests", "capps", "flags.c"), ["-I" + i for i in cinc]))
    return res


def exported_libs(riscos, out):
    """{library exported to C: (C_EXPORTS): (its Makefile's include path,
    OUT-relative; the headers it exports, its HDRS, or None for all)}.
    tboxlib exports every header it has (HDRS is OBJHDRS and all of h/)."""
    src = os.path.join(riscos, "Sources")
    res = {}
    for lib, rel in C_EXPORTS.items():
        mk = os.path.join(src, rel, "Makefile")
        mv = make_vars(mk)[0] if os.path.isfile(mk) else {}
        d = os.path.join(out, "inc", "C", lib)
        path = [os.path.relpath(p, out) for p in include_flags(mv.get("CINCLUDES", ""), d, out)]
        hdrs = None if lib == "tboxlibint" else mv.get("HDRS", "").split()
        res[lib] = (path, None if hdrs is None else ["inc/C/%s/%s.h" % (lib, h) for h in hdrs])
    return res


def cmhg_headers(riscos, out, cmhg, w):
    """Each Toolbox module's cmhg -d header, beside its staged sources (h.<CMHGFILE>)."""
    src = os.path.join(riscos, "Sources")
    made = []
    for m in TOOLBOX_MODULES:
        rel = "Toolbox/" + m
        mv, _ = make_vars(os.path.join(src, rel, "Makefile"))
        target = mv.get("TARGET") or mv.get("COMPONENT") or m
        cf = mv.get("CMHGFILE") or target + "Hdr"
        cp = os.path.join(src, rel, "cmhg", cf)
        if not os.path.isfile(cp):
            continue
        tmpd = os.path.join(out, "cmhg", m)
        os.makedirs(tmpd, exist_ok=True)
        r = subprocess.run([sys.executable, cmhg, cp, "-o", os.path.join(tmpd, cf + ".c"),
                            "-d", os.path.join(tmpd, cf + ".h"), "-I", os.path.join(src, rel),
                            "-I", os.path.join(out, "inc", "C")],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("capps: cmhg failed on %s:\n%s" % (cp, r.stderr))
        w.bytes(os.path.join(out, "src", rel, cf + ".h"), open(os.path.join(tmpd, cf + ".h"), "rb").read())
        made.append(rel + "/" + cf)
    return made


# ---------------------------------------------------------------------------

def app_filters(pre):
    """APP_PATCHES as the writer's filters: {absolute staged path: bytes ->
    bytes}, each text there exactly once (a change upstream stops the stage)"""
    by = {}
    for rel, old, new, why in APP_PATCHES:
        by.setdefault(os.path.abspath(os.path.join(pre, "src", rel)), []).append((rel, old, new, why))

    def make(items):
        def f(data):
            t = data.decode("latin-1")
            for rel, old, new, why in items:
                n = t.count(old)
                if n != 1:
                    raise SystemExit("capps: patch for %s (%s): the text occurs %d times, not once" % (rel, why, n))
                t = t.replace(old, new)
            return t.encode("latin-1")
        return f
    return {p: make(items) for p, items in by.items()}


def stage(riscos, out, rosasm_tools, cmhg, cc, cflags, contract, jobs):
    """Stage under OUT/.stage.lock: two stagings of one OUT at once (make -j
    with two targets that each stage) would write its files, and Hdr2H its
    temporaries, over each other.  The second waits, then finds its work
    done (every file is written only if its bytes change)."""
    os.makedirs(os.path.abspath(out), exist_ok=True)
    with open(os.path.join(os.path.abspath(out), ".stage.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        _stage(riscos, out, rosasm_tools, cmhg, cc, cflags, contract, jobs)


def _stage(riscos, out, rosasm_tools, cmhg, cc, cflags, contract, jobs):
    riscos, out = os.path.abspath(riscos), os.path.abspath(out)
    pre = os.path.join(out, "pre")      # as RISC OS has it, with the patch series; the pass reads this
    src = os.path.join(riscos, "Sources")
    rel_out = os.path.relpath(out, ROSGD)
    w = Writer()
    report = []

    # 1. the asm header export, exactly as rosasm's sweep builds it
    sys.path.insert(0, rosasm_tools)
    from export_hdrs import export_hdrs
    _, hdr_dirs = export_hdrs(riscos, os.path.join(out, "hdr"), quiet=True)

    # 2. sources, with the patch series applied as they are written, and
    # the applications' own patches after it
    w.filters = capps_patches.filters(pre, riscos)
    for path, f in app_filters(pre).items():
        g = w.filters.get(path)
        w.filters[path] = (lambda data, f=f, g=g: f(g(data))) if g else f
    npatch = len(capps_patches.patches(riscos)) + len(APP_PATCHES)
    n = unixify(os.path.join(src, "Lib/RISC_OSLib"), os.path.join(pre, "src"), w, "Lib/RISC_OSLib")
    n += unixify(os.path.join(src, "Toolbox"), os.path.join(pre, "src"), w, "Toolbox")
    # the applications, staged as src/Apps/<name> (tools/capps_headers.py's
    # APPS): their h/ files for the header groups, and the C that units()
    # compiles for APPS below
    for name, rel in capps_headers.APPS.items():
        n += unixify(os.path.join(src, rel), os.path.join(pre, "src"), w, "Apps/" + name)
    report.append("sources: %d files unixified" % n)

    # 3. C: -- the C library headers, ported
    cinc = os.path.join(pre, "inc", "C")
    clibh = os.path.join(src, "Lib/RISC_OSLib/clib/h")
    for f in sorted(os.listdir(clibh)):
        if f.startswith("."):
            continue
        if f == "stdarg":
            w.text(os.path.join(cinc, "stdarg.h"), STDARG)
            continue
        if f == "swis":
            continue          # generated below, as the ROM build makes it
        w.text(os.path.join(cinc, f + ".h"), port_clib_header(f, read_text(os.path.join(clibh, f))))
    swis, nhdr, added, missing, extra = make_swis_h(riscos, hdr_dirs, clibh)
    w.text(os.path.join(cinc, "swis.h"), swis)
    w.text(os.path.join(cinc, "rosgdswis.h"), extra)
    scan_norcroft(cinc, [f for f in os.listdir(clibh) if not f.startswith(".")])
    report.append("swis.h: %d names from makehswis's headers; rosgdswis.h: %d more from api/defs "
                  "(%s); headers absent: %s"
                  % (nhdr, len(added), " ".join(added) or "-", " ".join(missing) or "none"))

    # the libraries' exports on C:, tbox:, RISCOSLIB:, OS:
    for name, rel in C_EXPORTS.items():
        if os.path.isdir(os.path.join(src, rel)):
            unixify(os.path.join(src, rel), cinc, w, name)
    for sub in TBOX_DIRS:
        d = os.path.join(src, "Toolbox/ToolboxLib", sub, "h")
        for f in sorted(os.listdir(d)):
            if not f.startswith("."):
                data = translate_includes(open(os.path.join(d, f), "rb").read())
                w.bytes(os.path.join(pre, "inc", "tbox", f + ".h"), data)
                # and as C:tboxlibs/: C$Path ends in <Lib$Dir>., where
                # ToolboxLib exports itself as tboxlibs (Help's "tboxlibs/toolbox.h")
                w.bytes(os.path.join(pre, "inc", "C", "tboxlibs", f + ".h"), data)
    d = os.path.join(src, "Lib/RISC_OSLib/rlib/h")
    for f in sorted(os.listdir(d)):
        if not f.startswith("."):
            w.bytes(os.path.join(pre, "inc", "RISCOSLIB", f + ".h"), translate_includes(open(os.path.join(d, f), "rb").read()))
    osl = os.path.join(src, "Lib/OSLib/Dist/OSLib")
    if os.path.isdir(osl):
        for grp in sorted(os.listdir(osl)):
            hd = os.path.join(osl, grp, "oslib", "h")
            if os.path.isdir(hd):
                for f in sorted(os.listdir(hd)):
                    if not f.startswith("."):
                        data = open(os.path.join(hd, f), "rb").read()
                        w.bytes(os.path.join(pre, "inc", "OS", "oslib", f + ".h"), data)
                        w.bytes(os.path.join(pre, "inc", "OS", f + ".h"), data)
    # headers DefMod makes from a component's description, exported to the
    # C export directory (ToolAction's h.ToolAction rule)
    import defmod
    for name, rel in DEFMOD_HEADERS.items():
        w.text(os.path.join(cinc, name), defmod.generate(read_text(os.path.join(src, rel))))

    # 5. the patch series: every patch must have met its file
    if w.filters:
        raise SystemExit("capps: patches for files not staged: %s" % ", ".join(sorted(w.filters)))
    report.append("patches: %d applied" % npatch)

    # 6. Global/ and Interface/: every one the staged C includes
    want = set()
    inc_re = re.compile(rb'#\s*include\s*[<"]((?:Global|Interface)/[A-Za-z0-9_/]+)\.h[">]')
    cmhg_srcs = [os.path.join(src, "Toolbox", m, "cmhg") for m in TOOLBOX_MODULES]
    for root in [os.path.join(pre, "src"), os.path.join(pre, "inc")] + cmhg_srcs:
        for dp, dn, fn in os.walk(root):
            for f in fn:
                if f.endswith((".c", ".h")) or os.path.basename(dp) == "cmhg":
                    for m in inc_re.finditer(open(os.path.join(dp, f), "rb").read()):
                        want.add(m.group(1).decode())
    tmp = os.path.join(out, "tmp")
    os.makedirs(tmp, exist_ok=True)
    kinds = collections.Counter()
    for h in sorted(want):
        kind, base = h.split("/", 1)
        text, how = c_export_header(riscos, hdr_dirs, kind, base.replace("/", "."), os.path.join(
            riscos, "Library/Build/Hdr2H,102"), tmp, cmhg, cinc)
        if text is None:
            report.append("C:%s.h: no source header" % h)
            continue
        w.text(os.path.join(cinc, h + ".h"), text)
        kinds[how] += 1
    report.append("Global/Interface headers: %d (%s)" % (sum(kinds.values()),
                                                        ", ".join("%s %d" % kv for kv in sorted(kinds.items()))))

    # 6b. cmhg headers (they include Global/ headers too)
    made = cmhg_headers(riscos, pre, cmhg, w)
    report.append("cmhg headers: %d" % len(made))

    # 7. the rules
    us = units(riscos, out)
    lines = ["# generated by tools/capps.py stage -- the RISC OS Makefiles' objects, one rule each",
             "CAPPS_OUT := %s" % rel_out, ""]
    by = collections.OrderedDict()
    for u in us:
        by.setdefault(u.comp, []).append(u)
    objs_all = []
    for comp, ul in by.items():
        objs = []
        for u in ul:
            o = os.path.join(rel_out, "obj", comp, u.name.replace("/", "_") + ".o")
            srel = os.path.relpath(u.src, ROSGD)
            objs.append(o)
            # the flags are in the rules and the table: a change to either rebuilds
            lines.append("%s: %s $(CAPPS_OUT)/rules.mk capps.mk" % (o, srel))
            lines.append("\t@mkdir -p $(@D)")
            lines.append("\t$(CAPPS_CC) $(CAPPS_CFLAGS)%s -ffile-prefix-map=$(CURDIR)/$(CAPPS_OUT)/=capps/ "
                         "-ffile-prefix-map=$(CAPPS_OUT)/=capps/ -ffile-prefix-map=$(CURDIR)/=rosgd/ "
                         "%s -MMD -MF $@.d -c $< -o $@" % (
                             " $(CAPPS_LIB_CFLAGS)" if comp in LIB_COMPONENTS else "",
                             " ".join(_mk_rel(f) for f in u.flags)))
        lines.append("CAPPS_OBJS_%s := %s" % (comp, " ".join(objs)))
        lines.append("")
        objs_all += objs
    lines.append("CAPPS_COMPONENTS := %s" % " ".join(by))
    lines.append("CAPPS_OBJS := %s" % " ".join(objs_all))
    lines.append("-include $(CAPPS_OBJS:=.d)")
    w.text(os.path.join(out, "rules.mk"), "\n".join(lines) + "\n")
    report.append("units: %s" % ", ".join("%s %d" % (c, len(ul)) for c, ul in by.items()))
    prune(w.written, os.path.join(pre, "src"), os.path.join(pre, "inc"))

    # 8. the header groups (tools/capps_headers.py): every staged header a
    # client can include, in a unit the pass parses, whether or not a built
    # unit reaches it; rules.mk builds none of them
    tbpaths = {}
    for u in us:
        rel = os.path.dirname(os.path.relpath(u.src, out))
        if rel.startswith("src/Toolbox/") and rel not in tbpaths:
            tbpaths[rel] = [os.path.relpath(f[2:], out) for f in u.flags if f.startswith("-I")]
    hgroups, hexcluded = capps_headers.groups(out, pre, shlex.split(cc), shlex.split(cflags),
                                              exported_libs(riscos, out), tbpaths, jobs, sorted(w.written))
    hunits = [Unit("hdr", g.name, capps_headers.unit_path(out, g), ["-I" + os.path.join(out, d) for d in g.path])
              for g in hgroups]
    report.append("header groups: %d units over %d headers; %d headers excluded (hdrgroups.json)"
                  % (len(hgroups), sum(len(g.headers) for g in hgroups), len(hexcluded)))
    for h, why in sorted(hexcluded.items()):
        report.append("header excluded: %s: %s" % (h, why))

    # 9. the source pass: OUT/pre -> OUT/src, OUT/inc, OUT/inc-lib
    general, library, stats = source_pass(out, pre, us + hunits, cc, cflags, contract, jobs, sorted(w.written))
    ntwins, ncopied = twin_edits(pre, sorted(w.written), general)
    fw = pass_output(pre, out, sorted(w.written), general, library)
    report.append("pass: %(bitfields)d bit-fields made unsigned, %(records)d structures and unions aligned(4), "
                  "%(statics)d library static declarations thread-local, in %(files)d files%(cached)s" % stats)
    report.append("pass: %d edits copied between byte-identical copies of a header (%d sets of twins)"
                  % (ncopied, ntwins))
    report.append("pass: " + pass_notes(stats["notes"]))
    report.append("files written: %d changed of %d staged; %d changed of %d after the pass"
                  % (w.changed, len(w.written), fw.changed, len(fw.written)))
    print("\n".join("capps: " + r for r in report))


def twins(pre, staged):
    """The staged headers that are byte-identical copies of each other, as
    [[OUT-relative path]]: OSLib's in OS: and OS:oslib, ToolboxLib's in
    tbox:, C:tboxlibs and its own sources, rlib's in RISCOSLIB: and its own."""
    by = collections.defaultdict(list)
    for p in staged:
        rel = os.path.relpath(p, pre)
        if rel.startswith(("src" + os.sep, "inc" + os.sep)) and rel.endswith(".h"):
            with open(p, "rb") as f:
                by[hashlib.sha256(f.read()).digest()].append(rel)
    return [sorted(v) for _, v in sorted(by.items()) if len(v) > 1]


def twin_edits(pre, staged, general):
    """Give every copy of a twinned header the pass's edits of all of them
    (B and A, which depend on the text alone).  Include guards keep a unit
    from reading both copies, so the pass can see a record in one and never
    in the other (OSLib's colourpicker.h in OS: after its oslib/ twin); a
    client that includes the other first must get the same layout.
    -> (sets of twins, edits copied)"""
    sets, copied = twins(pre, staged), 0
    for rels in sets:
        union = set()
        for r in rels:
            union |= {tuple(e) for e in general.get(r, [])}
        for r in rels:
            have = {tuple(e) for e in general.get(r, [])}
            if have != union:
                copied += len(union - have)
                general[r] = sorted(union)
    return len(sets), copied


def pass_output(pre, out, staged, general, library):
    """Write the pass's output: every staged file under pre/src and pre/inc
    to out/, with its edits.  A library edit in a header under inc/ (which
    clients read too) goes to a copy in out/inc-lib/ instead."""
    fw = Writer()
    for p in staged:
        rel = os.path.relpath(p, pre)
        if not rel.startswith(("src" + os.sep, "inc" + os.sep)):
            continue
        t = read_text(p)
        g, lib = general.get(rel, []), library.get(rel, [])
        if lib and rel.startswith("inc" + os.sep):
            fw.text(os.path.join(out, "inc-lib", rel[4:]), capps_pass.apply(t, g + lib))
            lib = []
        fw.text(os.path.join(out, rel), capps_pass.apply(t, g + lib))
    prune(fw.written, os.path.join(out, "src"), os.path.join(out, "inc"), os.path.join(out, "inc-lib"))
    return fw


def pass_notes(notes):
    return "%d of the edits spelled in macro bodies; enum bit-fields left as clang reads them: %d" % (
        notes.get("macro", 0), notes.get("enum", 0))


def prune(written, *roots):
    """Remove what an earlier stage wrote and this one did not (a deleted source)."""
    for root in roots:
        for dp, dn, fn in os.walk(root):
            for f in fn:
                p = os.path.abspath(os.path.join(dp, f))
                if p not in written:
                    os.remove(p)


def _digest(parts, paths):
    h = hashlib.sha256()
    for x in parts:
        h.update(repr(x).encode() + b"\0")
    for p in paths:
        h.update(p.encode() + b"\0")
        with open(p, "rb") as f:
            h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()


def _pass_units(us, cc, cflags, frm, to):
    """capps_pass's view of the units: clang's command with OUT's paths moved to `to`."""
    mapping = [(os.path.join(frm, d), os.path.join(to, d)) for d in ("src", "inc-lib", "inc")]
    return [capps_pass.Unit("%s/%s" % (u.comp, u.name), u.comp in LIB_COMPONENTS,
                            capps_pass.unit_argv(cc, cflags, u.flags, u.src, mapping)) for u in us]


def exported_statics(contract):
    """The contract's exported statics (roscc/tools/clibspec/clib-contract.json):
    in the client's block, so thread-local in the library whatever their type."""
    with open(contract) as f:
        c = json.load(f)
    return sorted(e["name"] for e in c["x32"]["exported"])


def compiler_version(cc):
    """What the compiler says it is (clang --version): a new zig's clang can
    read the same source differently."""
    r = subprocess.run(cc + ["--version"], capture_output=True, text=True)
    return r.stdout.strip() or r.stderr.strip()


def source_pass(out, pre, us, cc, cflags, contract, jobs, staged):
    """Run capps_pass over OUT/pre, or reuse OUT/pass.json when nothing it
    depends on has changed: the staged files, the units and their flags, the
    header groups' units (OUT/hdrunits), the compiler (its --version too) and
    the table, the contract's exports, and the pass itself -- capps_pass.py,
    and capps.py, which lists the library's components and maps the units'
    paths, and capps_headers.py.  (The audit's cache is keyed on this key as
    well.)"""
    cc, cflags = shlex.split(cc), shlex.split(cflags)
    exported = exported_statics(contract)
    unit_desc = [(u.comp, u.name, os.path.relpath(u.src, out) if u.src.startswith(out) else u.src,
                  [f.replace(out, "OUT") for f in u.flags]) for u in us]
    # the header groups' units are written by capps_headers.py, not staged:
    # their text is part of the key too
    hunits = sorted(u.src for u in us if u.comp == "hdr")
    key = _digest([cc, cflags, compiler_version(cc), exported, unit_desc],
                  [os.path.join(HERE, "capps_pass.py"), os.path.abspath(__file__),
                   os.path.join(HERE, "capps_headers.py")] + hunits + staged)
    cache = os.path.join(out, "pass.json")
    try:
        with open(cache) as f:
            c = json.load(f)
        if c.get("key") == key:
            st = c["stats"]
            st["notes"], st["cached"] = collections.Counter(st["notes"]), " (unchanged: pass.json)"
            return ({k: [tuple(e) for e in v] for k, v in c["general"].items()},
                    {k: [tuple(e) for e in v] for k, v in c["library"].items()}, st)
    except (OSError, ValueError, KeyError):
        pass
    general, library, notes, st = capps_pass.run(_pass_units(us, cc, cflags, out, pre), pre, set(exported), jobs)
    bad = [n for n in notes if n[0] == "unhandled"]
    if bad:
        raise SystemExit("capps pass: cannot edit:\n  " + "\n  ".join("%s@%d: %s" % (f, o, m) for _, f, o, m in bad))
    st["cached"] = ""
    with open(cache + ".tmp", "w") as f:
        json.dump({"key": key, "stats": st, "general": general, "library": library, "notes": notes,
                   "audit": {"cc": cc, "cflags": cflags, "exported": exported,
                             "units": [(u.comp, u.name, u.src, u.flags) for u in us]}},
                  f, indent=0, sort_keys=True)
    os.replace(cache + ".tmp", cache)
    return general, library, st


def _mk_rel(flag):
    if flag.startswith("-I"):
        return "-I" + os.path.relpath(flag[2:], ROSGD)
    return "'" + flag + "'" if '"' in flag else flag


# ---------------------------------------------------------------------------
# check and compare

# The relocation types an object may have: what roscc's x32 link applies
# without a GOT (roscc/src/x32.rs; GOTPCRELX relaxed to a direct address),
# and in the library TPOFF32 as well -- local-exec's %fs:x@tpoff, which
# roscc's library link rewrites to %gs.  Every other type is refused: the
# other TLS models' (TLSGD, GOTTPOFF, TLSDESC ... and APX's CODE_n_ forms),
# a GOT's, a dynamic linker's.  Names from the x86-64 psABI.
RELOCS_ALLOWED = {0, 1, 2, 4, 10, 11, 12, 13, 14, 15, 24, 41, 42}
RELOCS_ALLOWED_LIB = RELOCS_ALLOWED | {23}
RELOC_NAMES = {3: "GOT32", 5: "COPY", 6: "GLOB_DAT", 7: "JUMP_SLOT", 8: "RELATIVE", 9: "GOTPCREL",
               16: "DTPMOD64", 17: "DTPOFF64", 18: "TPOFF64", 19: "TLSGD", 20: "TLSLD", 21: "DTPOFF32",
               22: "GOTTPOFF", 23: "TPOFF32", 25: "GOTOFF64", 26: "GOTPC32", 27: "GOT64", 28: "GOTPCREL64",
               29: "GOTPC64", 30: "GOTPLT64", 31: "PLTOFF64", 32: "SIZE32", 33: "SIZE64",
               34: "GOTPC32_TLSDESC", 35: "TLSDESC_CALL", 36: "TLSDESC", 37: "IRELATIVE", 38: "RELATIVE64",
               43: "CODE_4_GOTPCRELX", 44: "CODE_4_GOTTPOFF", 45: "CODE_4_GOTPC32_TLSDESC",
               46: "CODE_5_GOTPCRELX", 47: "CODE_5_GOTTPOFF", 48: "CODE_5_GOTPC32_TLSDESC",
               49: "CODE_6_GOTPCRELX", 50: "CODE_6_GOTTPOFF", 51: "CODE_6_GOTPC32_TLSDESC"}


def elf32_facts(path, lib):
    """An object's faults, and its thread-local bytes.  Every object: ELF32
    x86-64 relocatable, no sanitizer or stack-protector references.  A client
    object has no thread-local storage; a library object has no writable data
    but thread-local storage (no .data, .bss or common), reached local-exec."""
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF":
        return "not ELF", 0
    if d[4] != 1:
        return "not ELFCLASS32", 0
    e_type, e_machine = struct.unpack_from("<HH", d, 16)
    if e_type != 1 or e_machine != 62:
        return "type %d machine %d" % (e_type, e_machine), 0
    shoff, = struct.unpack_from("<I", d, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 46)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    shstr = secs[shstrndx]
    name = lambda off, tab: d[tab[4] + off:d.index(b"\0", tab[4] + off)].decode()  # noqa: E731
    bad, tls = [], 0
    for s in secs:
        sn = name(s[0], shstr)
        if s[2] & 0x400:               # SHF_TLS
            if not lib:
                bad.append("TLS section " + sn)
            elif s[8] > 4:
                # roscc lays the statics out at RISC OS's 4-byte offsets:
                # code must not assume more (movaps on a 16-aligned array)
                bad.append("TLS section %s aligned %d, not 4" % (sn, s[8]))
            else:
                tls += s[5]
        elif lib and s[2] & 3 == 3 and s[5]:   # SHF_WRITE | SHF_ALLOC, not TLS
            bad.append("writable data not thread-local: %s (%d bytes)" % (sn, s[5]))
        if s[1] == 2:                  # SHT_SYMTAB
            strtab = secs[s[6]]
            for i in range(s[5] // 16):
                st_name, _, _, info, _, shndx = struct.unpack_from("<IIIBBH", d, s[4] + i * 16)
                if shndx == 0 and st_name:
                    sym = name(st_name, strtab)
                    if sym.startswith(("__ubsan", "__stack_chk", "__asan", "__morestack", "__tls_get_addr")):
                        bad.append("undefined " + sym)
                if not lib and info & 0xF == 6:      # STT_TLS
                    bad.append("TLS symbol " + name(st_name, strtab))
                if lib and shndx == 0xFFF2:
                    bad.append("common symbol " + name(st_name, strtab))
        if s[1] == 4:                  # SHT_RELA
            for i in range(s[5] // 12):
                info, = struct.unpack_from("<I", d, s[4] + i * 12 + 4)
                t = info & 0xFF
                if t not in (RELOCS_ALLOWED_LIB if lib else RELOCS_ALLOWED):
                    what = RELOC_NAMES.get(t, "type %d" % t)
                    bad.append("%s relocation %s in %s" % ("TLS" if t == 23 or "TLS" in what or "TPOFF" in what
                                                           or "DTP" in what else "refused", what, sn))
    return "; ".join(sorted(set(bad))) or None, tls


def rules_objects(out):
    """{component: [object path]} from OUT/rules.mk, the objects the build
    makes (a stale .o of a unit dropped upstream is not counted)."""
    comps = collections.OrderedDict()
    with open(os.path.join(out, "rules.mk")) as f:
        for line in f:
            m = re.match(r"^CAPPS_OBJS_(\w+) := (.*)$", line)
            if m:
                comps[m.group(1)] = [os.path.join(ROSGD, o) for o in m.group(2).split()]
    return comps


def ar_members(path):
    """A GNU archive's members, [(name, bytes)], in order: its symbol index
    and long-name table read for naming only."""
    d = open(path, "rb").read()
    if d[:8] != b"!<arch>\n":
        raise SystemExit("capps check: %s: not an ar archive" % path)
    off, longnames, members = 8, b"", []
    while off + 60 <= len(d):
        name, size = d[off:off + 16].decode().rstrip(), int(d[off + 48:off + 58])
        data = d[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name == "//":
            longnames = data
        elif name in ("/", "/SYM64/"):
            continue
        elif name.startswith("/"):
            n = int(name[1:])
            members.append((longnames[n:longnames.index(b"\n", n)].decode().rstrip("/"), data))
        else:
            members.append((name.rstrip("/"), data))
    return members


def check_rlib(out):
    """rlib's ObjAsm in C (rosgd/rlib, capps.mk's OUT/obj/rlib-own) are
    clients' objects like rlib's own; OUT/lib/rlib.a holds rlib's objects,
    in the Makefile's order, then these, each byte for byte."""
    archive = os.path.join(out, "lib", "rlib.a")
    od = os.path.join(out, "obj", "rlib-own")
    got = ar_members(archive) if os.path.isfile(archive) else []
    rl = rules_objects(out).get("rlib", [])
    # these in the archive's order after rlib's (capps.mk's RLIB_OWN_SRCS)
    own = [os.path.join(od, n) for n, _ in got[len(rl):]]
    have = sorted(f for f in os.listdir(od) if f.endswith(".o")) if os.path.isdir(od) else []
    bad = []
    if sorted(os.path.basename(o) for o in own) != have or not have:
        bad.append("%s: rlib's ObjAsm in C: %s built, %s archived" % (
            os.path.relpath(od, out), " ".join(have) or "none", " ".join(os.path.basename(o) for o in own) or "none"))
    for o in own:
        e, _ = elf32_facts(o, False) if os.path.isfile(o) else ("not built", 0)
        if e:
            bad.append("%s: %s" % (os.path.relpath(o, out), e))
    want = rl + own
    if [n for n, _ in got] != [os.path.basename(o) for o in want]:
        bad.append("%s: members %s, not rlib's objects then %s" % (
            archive, " ".join(n for n, _ in got) or "none", " ".join(os.path.basename(o) for o in own)))
    else:
        bad += ["%s(%s): not %s's bytes" % (archive, n, os.path.relpath(o, out))
                for (n, data), o in zip(got, want) if not os.path.isfile(o) or open(o, "rb").read() != data]
    for b in bad:
        print("capps check: " + b)
    print("capps check: rlib's ObjAsm in C: %d objects, clients' (no TLS); %s: %d members, rlib's %d then these: %s"
          % (len(own), os.path.relpath(archive, out), len(got), len(want) - len(own), "fail" if bad else "all"))
    return 1 if bad else 0


def elf32_symbols(d, absolute=True):
    """An ELF32 relocatable's global names: (defined, needed) -- defined in a
    section, absolute (unless absolute=False) or common; needed: undefined
    and not weak."""
    shoff, = struct.unpack_from("<I", d, 32)
    shentsize, shnum = struct.unpack_from("<HH", d, 46)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    defined, needed = set(), set()
    for s in secs:
        if s[1] != 2:                  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for i in range(1, s[5] // 16):
            st_name, _, _, info, _, shndx = struct.unpack_from("<IIIBBH", d, s[4] + i * 16)
            n = d[strtab[4] + st_name:d.index(b"\0", strtab[4] + st_name)].decode()
            if not n or info >> 4 == 0:  # local
                continue
            if shndx and (absolute or shndx != 0xFFF1):     # SHN_ABS
                defined.add(n)
            elif info >> 4 == 1:
                needed.add(n)
    return defined, needed


def apps(out, roscc, rt, rlib, spec=None, abi="x32"):
    """Edit, Draw, Paint and Filer_Action (APPS) linked as disc applications
    by roscc link --clib -- the library's stubs and crt0_x32 -- against
    rlib.a, which roscc takes members from as a Unix linker does; nothing may
    be undefined, and nothing else is linked.  Writes OUT/apps/<App>,ff8,
    <App>.why (roscc --why-extract) and report.txt: each image, its objects,
    the rlib members it takes, the library's entries it calls and which of
    those the image (spec) has as traps."""
    comps = rules_objects(out)
    d = os.path.join(out, "apps")
    os.makedirs(d, exist_ok=True)
    # the library's entries: the stubs' names, less their absolute ones (the
    # signal markers, __SIG_DFL and the rest: values, not entries)
    # (abi: x32, or a64x32 for the Apple Silicon box, whose runtime objects
    # roscc finds by $ROSCC_A64X32_RT)
    stub_obj = open(os.path.join(rt, "roclib_%s.o" % abi), "rb").read()
    stubs, _ = elf32_symbols(stub_obj, absolute=False)
    values = elf32_symbols(stub_obj)[0] - stubs
    traps = set()
    if spec and os.path.isfile(spec):
        traps = {u["name"] for u in json.load(open(spec))["undefined"]}
    members = {}
    for n, data in ar_members(rlib):
        members["%s(%s)" % (rlib, n)] = elf32_symbols(data)
    lines, bad = [], 0
    for comp, rel in APPS:
        objs = comps.get(comp, [])
        image, why = os.path.join(d, comp + ",ff8"), os.path.join(d, comp + ".why")
        for f in (image, why):
            if os.path.exists(f):
                os.remove(f)
        r = subprocess.run([roscc, "link", "--clib", "-o", image, "--why-extract", why] + objs + [rlib],
                           capture_output=True, text=True,
                           env=dict(os.environ, **{"ROSCC_%s_RT" % abi.upper(): rt}))
        lines.append("%s (%s): %d objects of its own: %s" % (comp, rel, len(objs),
                                                             " ".join(os.path.basename(o)[:-2] for o in objs)))
        if r.returncode:
            bad += 1
            lines += ["  DOES NOT LINK:"] + ["    " + l.strip() for l in (r.stderr + r.stdout).strip().split("\n")]
            continue
        taken = [l.split("\t")[1] for l in open(why).read().split("\n")[1:] if l]
        # what the application and its rlib members need of the rest
        own = [elf32_symbols(open(o, "rb").read()) for o in objs] + [members[t] for t in taken]
        defined = set().union(*(x[0] for x in own))
        needed = set().union(*(x[1] for x in own)) - defined
        lib = sorted(needed & stubs)
        rest = sorted(needed - stubs - values - {"_start"})
        m = re.search(r"(\d+) bytes loaded, (\d+) in memory", r.stdout)
        lines.append("  linked: %s, %s bytes loaded, %s in memory" % (os.path.relpath(image, out),
                                                                     m.group(1) if m else "?", m.group(2) if m else "?"))
        rl = sorted(t.split("(", 1)[1][:-1] for t in taken if t.startswith(rlib + "("))
        lines.append("  rlib.a: %d of %d members: %s" % (len(rl), len(ar_members(rlib)), " ".join(x[:-2] for x in rl)))
        lines.append("  the library: %d entries: %s" % (len(lib), " ".join(lib)))
        lines.append("  of them traps in the image (%s): %s" % (os.path.basename(spec) if spec else "no spec",
                                                                " ".join(sorted(set(lib) & traps)) or "none"))
        if needed & values:
            lines.append("  the stubs' absolute values: " + " ".join(sorted(needed & values)))
        if rest:
            lines.append("  other names the link resolved: " + " ".join(rest))
    text = "\n".join(lines) + "\n"
    with open(os.path.join(d, "report.txt"), "w") as f:
        f.write(text)
    print(text, end="")
    print("capps apps: %d of %d link against rlib.a and the library with nothing undefined%s; %s"
          % (len(APPS) - bad, len(APPS), "" if not bad else " (%d FAIL)" % bad,
             os.path.relpath(os.path.join(d, "report.txt"), ROSGD)))
    return 1 if bad else 0


def check(out, audit_too=True):
    comps = rules_objects(out)
    objs = [(c, o) for c, ol in comps.items() for o in ol]
    bad, tls, nlib = [], 0, 0
    for c, o in objs:
        lib = c in LIB_COMPONENTS
        e, t = elf32_facts(o, lib) if os.path.isfile(o) else ("not built", 0)
        tls += t
        nlib += lib
        if e:
            bad.append((o, e))
    for o, e in bad:
        print("capps check: %s: %s" % (os.path.relpath(o, out), e))
    print("capps check: %d objects, ELF32 x86-64 relocatable, no sanitizer or stack-protector references; "
          "the library's %d: no writable data but thread-local (%d bytes), 4-aligned, local-exec; the rest: no TLS: %s"
          % (len(objs), nlib, tls, "all" if not bad else "%d fail" % len(bad)))
    print("capps check: " + ", ".join("%s %d" % (c, len(ol)) for c, ol in sorted(comps.items())))
    rc = 1 if bad or not objs else 0
    rc = check_rlib(out) or rc
    rc = check_paths(out) or rc
    return (audit(out) if audit_too else 0) or rc


def check_paths(out):
    """No staged header spells where the tree lives -- the checkout's path,
    as it is or as Hdr2H makes a guard of it (#40): the box's initramfs
    carries OUT/inc, and two checkouts of one commit must give the same.
    -> 1 if one does"""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    spell = set()
    for d in (os.path.abspath(out), here):
        spell |= {d.encode(), re.sub(r"[^\w]", "_", d.upper()).encode()}
    found = []
    for dp, dn, fn in os.walk(os.path.join(out, "inc")):
        dn.sort()
        for f in sorted(fn):
            p = os.path.join(dp, f)
            with open(p, "rb") as fh:
                data = fh.read()
            if any(s in data for s in spell):
                found.append(os.path.relpath(p, out))
    for f in found:
        print("capps check: %s: spells the tree's path" % f)
    print("capps check: staged headers that spell the tree's path: %s" % (len(found) or "none"))
    return 1 if found else 0


def audit(out, jobs=None):
    """The source pass over its own output (OUT/src, OUT/inc, OUT/inc-lib),
    with every unit's flags: it must find nothing to do.  That proves the pass
    idempotent, and that no plain int bit-field is signed, no structure or
    union is unaligned, and no library static is writable and not
    thread-local, as every unit sees them.  Cached in OUT/pass-audit.json."""
    with open(os.path.join(out, "pass.json")) as f:
        pj = json.load(f)
    a = pj["audit"]
    us = [Unit(c, n, s, fl) for c, n, s, fl in a["units"]]
    files = sorted(os.path.join(dp, f) for root in ("src", "inc", "inc-lib")
                   for dp, dn, fn in os.walk(os.path.join(out, root)) for f in fn)
    key = _digest([pj["key"]], [os.path.join(HERE, "capps_pass.py")] + files)
    cache = os.path.join(out, "pass-audit.json")
    try:
        with open(cache) as f:
            c = json.load(f)
        if c.get("key") == key:
            print("capps audit: %s (unchanged: pass-audit.json)" % c["result"])
            return c["rc"]
    except (OSError, ValueError, KeyError):
        pass
    general, library, notes, st = capps_pass.run(_pass_units(us, a["cc"], a["cflags"], out, out), out,
                                                 set(a["exported"]), jobs or os.cpu_count() or 1)
    left = [(f, e) for d in (general, library) for f, es in d.items() for e in es]
    unhandled = [n for n in notes if n[0] == "unhandled"]
    for f, e in left[:20]:
        print("capps audit: the pass would still edit %s@%d: %r" % (f, e[0], e[2]))
    for _, f, o, m in unhandled[:20]:
        print("capps audit: %s@%d: %s" % (f, o, m))
    rc = 1 if left or unhandled else 0
    result = ("%d units: the pass finds nothing to do (idempotent); enum bit-fields left as clang reads them: %d"
              % (len(us), st["notes"].get("enum", 0))) if not rc \
        else "%d edits and %d unhandled remain" % (len(left), len(unhandled))
    print("capps audit: " + result)
    with open(cache, "w") as f:
        json.dump({"key": key, "rc": rc, "result": result}, f)
    return rc


def compare(a, b):
    def tree(root):
        r = {}
        # the objects rules.mk makes, then rlib's ObjAsm in C and rlib.a (capps.mk)
        extra = sorted(os.path.join(os.path.abspath(root), "obj", "rlib-own", f)
                       for f in os.listdir(os.path.join(root, "obj", "rlib-own"))
                       if f.endswith(".o")) if os.path.isdir(os.path.join(root, "obj", "rlib-own")) else []
        for p in [p for ol in rules_objects(root).values() for p in ol] + extra + \
                [os.path.join(os.path.abspath(root), "lib", "rlib.a")]:
            r[os.path.relpath(p, os.path.abspath(root))] = (
                hashlib.sha256(open(p, "rb").read()).hexdigest() if os.path.isfile(p) else "missing")
        return r
    ta, tb = tree(a), tree(b)
    diff = sorted(k for k in set(ta) | set(tb) if ta.get(k) != tb.get(k))
    for k in diff[:20]:
        print("capps compare: differs: %s" % k)
    h = hashlib.sha256("".join(k + ta[k] for k in sorted(ta)).encode()).hexdigest()
    print("capps compare: %d objects, %d differ; build digest %s" % (len(ta), len(diff), h[:16]))
    return 1 if diff or not ta else 0


def selftest(riscos):
    """The loud failures, made to happen: each header patch against its
    header with the patched text gone, and the scan against a header left
    as Norcroft wrote it."""
    import tempfile
    clibh = os.path.join(riscos, "Sources", "Lib", "RISC_OSLib", "clib", "h")
    cases = [("math", "0f_7F800000", "P9"), ("math", "#define __assertfp(r)", "P4"),
             ("stddef", "#define offsetof(type, member)", "P6"),
             ("stdlib", "__abs (int) (j)", "P8"), ("complex", "#define _Imaginary_I ___i", "P10"),
             ("tgmath", "#define acos(z)", None), ("stdatomic", "(__STDC_VERSION__ < 201112)", "P12"),
             ("setjmp", "int setjmp(jmp_buf /*env*/);", "P13"),
             ("math", "#  define fmaf(x,y,z) __r_fma(x,y,z)", "P14")]
    bad = []
    for name, text, tag in cases:
        t = read_text(os.path.join(clibh, name))
        if text not in t:
            bad.append("%s: %r not in RISC_OSLib's header" % (name, text))
            continue
        port_clib_header(name, t)                       # as it is: accepted
        try:
            if tag is None:                             # P11 counts its macros
                t = re.sub(r"(?m)^#define \w+\([^)]*\)\s+_TG.*$", "", t)
            port_clib_header(name, t.replace(text, "#define ROSGD_CHANGED_UPSTREAM"))
            bad.append("%s: %s accepted a changed header" % (name, tag or "P11"))
        except SystemExit as e:
            if (tag or "P11") not in str(e):
                bad.append("%s: refused, but not by %s: %s" % (name, tag or "P11", e))
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "math.h"), "w", encoding="latin-1") as fh:
            fh.write(read_text(os.path.join(clibh, "math")))  # not ported
        try:
            scan_norcroft(d, ["math"])
            bad.append("scan_norcroft passed Norcroft's math.h")
        except SystemExit as e:
            if "0f_7F800000" not in str(e) or "__abs" not in str(e):
                bad.append("scan_norcroft: %s" % e)
        with open(os.path.join(d, "math.h"), "w", encoding="latin-1") as fh:
            fh.write(port_clib_header("math", read_text(os.path.join(clibh, "math"))))
        scan_norcroft(d, ["math"])                      # ported: clean
    for b in bad:
        print("capps selftest: " + b)
    print("capps selftest: %d header patches refuse a changed header; the scan finds Norcroft's "
          "forms: %s" % (len(cases), "ok" if not bad else "%d wrong" % len(bad)))
    return 1 if bad else 0


# ---------------------------------------------------------------------------
# cmhg: roscc's cmhg.py over every module description in the sources

def cmhg_sweep(riscos, out, cmhg, cc, cflags, ascc, jobs=8):
    """Every .cmhg in the sources (a file in a cmhg/ directory) through both
    of cmhg.py's back ends: the x32 C compiled with the flag table, the
    AArch32 assembler assembled.  The Global/ and Interface/ headers they
    include are made as the export phase makes them.  Report in
    OUT/cmhg/report.txt; the tally is the package T1b gate (68 of 70, the
    two using library-enter-code refused)."""
    riscos, out = os.path.abspath(riscos), os.path.abspath(out)
    src = os.path.join(riscos, "Sources")
    sys.path.insert(0, os.path.join(ROSGD, "..", "rosasm", "tools"))
    from export_hdrs import export_hdrs
    _, hdr_dirs = export_hdrs(riscos, os.path.join(out, "hdr"), quiet=True)
    base = os.path.join(out, "cmhg")
    inc = os.path.join(base, "inc")
    tmp = os.path.join(out, "tmp")
    os.makedirs(tmp, exist_ok=True)
    files = []
    for dp, dn, fn in os.walk(src):
        dn.sort()
        if os.path.basename(dp) == "cmhg":
            files += [os.path.join(dp, f) for f in sorted(fn) if not f.startswith(".")]
    inc_re = re.compile(rb'#\s*include\s*[<"]((?:Global|Interface)/[A-Za-z0-9_/]+)\.h[">]')
    staged = set()
    hdr2h = os.path.join(riscos, "Library/Build/Hdr2H,102")
    for f in files:
        for m in inc_re.finditer(open(f, "rb").read()):
            h = m.group(1).decode()
            if h in staged:
                continue
            kind, hb = h.split("/", 1)
            text, how = c_export_header(riscos, hdr_dirs, kind, hb.replace("/", "."), hdr2h, tmp)
            if text is not None:
                os.makedirs(os.path.dirname(os.path.join(inc, h + ".h")), exist_ok=True)
                with open(os.path.join(inc, h + ".h"), "w") as fh:
                    fh.write(text)
            staged.add(h)

    def one(f):
        rel = os.path.relpath(f, src)
        ident = rel.replace("/", "_")
        comp = os.path.dirname(os.path.dirname(f))
        row = {"file": rel}
        for target, ext in (("x32", ".c"), ("aarch32", ".s")):
            d = os.path.join(base, target, ident)
            os.makedirs(d, exist_ok=True)
            o, hh = os.path.join(d, "Hdr" + ext), os.path.join(d, "Hdr.h")
            r = subprocess.run([sys.executable, cmhg, "--target", target, f, "-o", o, "-d", hh,
                                "-I", comp, "-I", inc, "-I", os.path.join(out, "inc", "C")],
                               capture_output=True, text=True)
            if r.returncode:
                row[target] = "refused" if ": refused: " in r.stderr else "failed"
                row["why"] = r.stderr.strip().split("\n")[-1]
                continue
            if target == "x32":
                cmd = cc.split() + cflags.split() + ["-I" + d, "-I" + os.path.join(out, "inc", "C"),
                                                     "-c", o, "-o", os.path.join(d, "Hdr.o")]
            else:
                cmd = ascc.split() + ["-c", o, "-o", os.path.join(d, "Hdr.o")]
            c = subprocess.run(cmd, capture_output=True, text=True)
            row[target] = "built" if c.returncode == 0 else "generated, not built"
            if c.returncode:
                row["why"] = " ".join(l for l in c.stderr.strip().split("\n") if "error" in l)[:300]
        return row

    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        rows = list(pool.map(one, files))
    lines = ["# roscc cmhg.py over the sources' module descriptions (capps.py cmhg)", ""]
    for r in rows:
        lines.append("%-66s x32 %-8s aarch32 %-8s %s" % (r["file"], r.get("x32"), r.get("aarch32"),
                                                        r.get("why", "")))
    n = len(rows)
    both = sum(1 for r in rows if r.get("x32") == "built" and r.get("aarch32") == "built")
    refused = [r for r in rows if r.get("x32") == "refused"]
    other = [r for r in rows if r not in refused and not (r.get("x32") == "built" and r.get("aarch32") == "built")]
    lines += ["", "files %d; generated and built for both targets %d; refused %d; other %d"
              % (n, both, len(refused), len(other))]
    with open(os.path.join(base, "report.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    for r in refused + other:
        print("capps cmhg: %s: %s" % (r["file"], r.get("why")))
    print("capps cmhg: %d files, %d generated and built for x32 and AArch32, %d refused, %d other"
          % (n, both, len(refused), len(other)))
    return 0 if not other else 1


def main(argv):
    if len(argv) >= 3 and argv[0] == "stage":
        opts = dict(zip(argv[3::2], argv[4::2]))
        if "--cflags" not in opts:
            raise SystemExit("capps: stage needs --cc and --cflags (capps.mk's table) for the source pass")
        stage(argv[1], argv[2], opts.get("--rosasm-tools", os.path.join(ROSGD, "..", "rosasm", "tools")),
              opts.get("--cmhg", os.path.join(ROSGD, "..", "roscc", "tools", "cmhg.py")),
              opts.get("--cc", "zig clang"), opts["--cflags"],
              opts.get("--contract", os.path.join(ROSGD, "..", "roscc", "tools", "clibspec", "clib-contract.json")),
              int(opts.get("--jobs", os.cpu_count() or 1)))
        return 0
    if len(argv) in (2, 3) and argv[0] == "check":
        return check(argv[1], audit_too="--no-audit" not in argv[2:])
    if len(argv) >= 2 and argv[0] == "apps":
        opts = dict(zip(argv[2::2], argv[3::2]))
        return apps(argv[1], opts["--roscc"], opts["--rt"], opts["--rlib"], opts.get("--spec"),
                    opts.get("--abi", "x32"))
    if len(argv) >= 3 and argv[0] == "cmhg":
        opts = dict(zip(argv[3::2], argv[4::2]))
        return cmhg_sweep(argv[1], argv[2],
                          opts.get("--cmhg", os.path.join(ROSGD, "..", "roscc", "tools", "cmhg.py")),
                          opts.get("--cc", "zig clang"), opts.get("--cflags", ""),
                          opts.get("--as", "zig clang -target armv7a-none-eabi"),
                          int(opts.get("--jobs", "8")))
    if len(argv) == 3 and argv[0] == "compare":
        return compare(argv[1], argv[2])
    if len(argv) == 2 and argv[0] == "selftest":
        return selftest(argv[1])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
