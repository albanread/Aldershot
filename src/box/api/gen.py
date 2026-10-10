#!/usr/bin/env python3
"""Generate ROSGD's typed call API from the SWI definition files.

    api/gen.py OUTDIR            reads the TOML in api/defs

One definition, every boundary.  For each
SWI this writes:

  api.h       the typed functions, in two forms -- x<name>() returns an
              os_error pointer, <name>() raises it -- and the SWI numbers;
  api_gen.c   the thunks between the register block and the typed call:

    native SWIs      compiled code -> ros_thunk_<SWI>(state) -> x<name>(typed)
                     the runtime writes x<name>(); the thunk unpacks
                     registers by their roles, pointers zero-extended into
                     host pointers, and packs results and flags back
    module SWIs      native code -> x<name>(typed) -> register block ->
                     ros_swi() -> the compiled module's SWI entry
                     the reverse thunk, so a native caller cannot tell that
                     the implementation is compiled ObjAsm

  native_swis.txt  each native SWI's number and name, from which
              `rosasm --emit c --swis` binds compiled code straight to
              its thunk
  swi_regs.txt     each SWI's registers, read and written, from which
              `rosasm --emit c --swi-regs` keeps registers in C locals
              across the SWI

A SWI whose implementation is still to come is `impl = "planned"`, in its
module or on the SWI: its contract is written -- numbers, registers, types
-- but nothing calls or implements it yet, so it gets its numbers and its
registers and no functions.  A SWI's `reads` and `writes` list every
register it may read and write, for any reason: a multiplexer needs them
unless its ops cover every reason, and each op must keep within them.
`count` makes a range of numbers one SWI (OS_WriteI).

Multiplexers are split: one typed function per reason, and one thunk that
selects among them by the selector register.

Every parameter, result and flag name is checked before anything is written:
a C identifier, not a keyword, used once per operation, and not a variable
the generated code itself declares where that name appears (check_names).
"""

import glob
import os
import re
import sys
import tomllib

X_BIT = 0x20000

# type -> (C type, class)
TYPES = {
    "u8": ("uint8_t", "int"),
    "u32": ("uint32_t", "int"),
    "i32": ("int32_t", "int"),
    "bool": ("int", "int"),
    "str": ("char *", "ptr"),
    "ptr<u8>": ("uint8_t *", "ptr"),
    "ptr<u32>": ("uint32_t *", "ptr"),
    "ptr<void>": ("void *", "ptr"),
    "ptr<os_error>": ("os_error *", "ptr"),
    # arena addresses that native code passes on but never dereferences:
    # code (a routine compiled code or a native entry stands at) and
    # workspace pointers handed back to their owners
    "code": ("uint32_t", "int"),
    "addr": ("uint32_t", "int"),
}


def snake(name):
    """OS_ReadMonotonicTime -> os_read_monotonic_time; OS_Write0 -> os_write0."""
    prefix, _, rest = name.partition("_")
    rest = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", rest).lower()
    return f"{prefix.lower()}_{rest}"


def decl(t, name):
    """A declaration: "void *" and "block" make "void *block", not "void * block"."""
    return f"{t}{name}" if t.endswith("*") else f"{t} {name}"


def ctype(p, out=False):
    t, cls = TYPES[p["type"]]
    if cls == "ptr" and (p.get("const") or p["type"] == "str") and not out:
        t = "const " + t
    return t


class Op:
    """One typed function: a whole SWI, or one reason of a multiplexer."""

    def __init__(self, swi, fn, params, results, flags, reason=None):
        self.swi = swi
        self.fn = fn
        self.params = params
        self.results = results
        self.flags = flags
        self.reason = reason

    def signature(self, x=True):
        args = [decl(ctype(p), p['name']) for p in self.params]
        args += [decl(ctype(r, out=True) + ("*" if ctype(r, out=True).endswith("*") else " *"), r['name'])
                 for r in self.results]
        args += [f"int *{f['name']}" for f in self.flags]
        name = ("x" if x else "") + self.fn
        ret = "os_error *" if x else "void "
        return f"{ret}{name}({', '.join(args) or 'void'})"

    def call_args(self):
        return [p["name"] for p in self.params] + \
               [r["name"] for r in self.results] + [f["name"] for f in self.flags]


# Names the generated code gives its own variables, and where; a parameter,
# result or flag of the same name in the same function would clash with it.
# Socket_Creat's result was once "s", and its thunk did not compile.
#   the native thunk:   s (the register block), e, outer_sp -- beside the
#                       results and flags, which it declares as locals
#                       (its parameters are read from s, never named);
#   the raising form:   e -- beside every name, passed on by name;
#   the reverse thunk:  s (a local) -- beside the parameters.
THUNK_LOCALS = {"s", "e", "outer_sp"}
RAISING_LOCALS = {"e"}
REVERSE_LOCALS = {"s"}
C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
    "_Complex", "_Imaginary", "_Alignas", "_Alignof", "_Atomic", "_Generic",
    "_Noreturn", "_Static_assert", "_Thread_local", "bool", "true", "false",
}


def check_names(swi, op):
    """Every name an operation gives the generated C: a C identifier, not a
    keyword, used once, and not a variable the generated code declares in a
    function where the name also appears."""
    seen = set()
    reverse = swi["impl"].startswith("module:")
    for kind, items in (("parameter", op.params), ("result", op.results), ("flag", op.flags)):
        clash = set(RAISING_LOCALS)
        if kind != "parameter":
            clash |= THUNK_LOCALS
        elif reverse:
            clash |= REVERSE_LOCALS
        for item in items:
            name = item["name"]
            where = f"api: {swi['name']}: {op.fn}: {kind} {name!r}"
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                sys.exit(f"{where} is not a C identifier")
            if name in C_KEYWORDS:
                sys.exit(f"{where} is a C keyword")
            if name in clash:
                sys.exit(f"{where} would clash with a variable the generated code "
                         f"declares ({', '.join(sorted(clash))}): rename it")
            if name in seen:
                sys.exit(f"{where} is named twice")
            seen.add(name)


def load(defs_dir):
    swis = []
    for path in sorted(glob.glob(os.path.join(defs_dir, "*.toml"))):
        with open(path, "rb") as fh:
            d = tomllib.load(fh)
        impl = d["module"]["impl"]
        for s in d.get("swi", []):
            s.setdefault("impl", impl)
            s["module"] = d["module"]["name"]
            s["chunk"] = d["module"].get("chunk")
            s["source"] = os.path.basename(path)
            prefix = s["name"].split("_", 1)[0].lower()
            if s.get("raw"):
                # Its registers pass straight through (OS_CallAVector,
                # OS_ServiceCall): no typed form; the runtime writes the thunk.
                s["ops"] = []
            elif s.get("kind") == "multiplexer":
                s["ops"] = [Op(s, f"{prefix}_{o['fn']}", o.get("params", []),
                               o.get("results", []), o.get("flags_out", []),
                               reason=o["reason"]) for o in s.get("ops", [])]
            else:
                s["ops"] = [Op(s, snake(s["name"]), s.get("params", []),
                               s.get("results", []), s.get("flags_out", []))]
            for op in s["ops"]:
                check_names(s, op)
                for key, regs in (("reads", op.params), ("writes", op.results)):
                    if key in s and not {r["reg"] for r in regs} <= set(s[key]):
                        sys.exit(f"api: {s['name']}: {op.fn} uses registers outside its {key}")
            swis.append(s)
    return swis


def planned(s):
    return s["impl"] == "planned"


def reg_masks(s):
    """(reads, writes) as masks, or None where they are not all known."""
    ins = 1 << s["selector"] if s.get("kind") == "multiplexer" else 0
    outs = 1   # R0 may always come back as an error
    if "reads" in s or "writes" in s:
        for r in s.get("reads", []):
            ins |= 1 << r
        for r in s.get("writes", []):
            outs |= 1 << r
        return ins, outs
    # A multiplexer's ops may not cover every reason; a raw SWI passes its
    # registers straight through.
    if not s["ops"] or s.get("kind") == "multiplexer":
        return None
    for op in s["ops"]:
        for r in op.params:
            ins |= 1 << r["reg"]
        for r in op.results:
            outs |= 1 << r["reg"]
    return ins, outs


# ---- the header -----------------------------------------------------------

def header(swis):
    out = ["/* api.h -- ROSGD's typed call API.  Generated by api/gen.py from",
           " * the TOML in api/defs: do not edit.  x<name>() returns an error, <name>()",
           " * raises it. */",
           "#ifndef ROSGD_API_H", "#define ROSGD_API_H", "",
           "#include <stdint.h>", '#include "rosgd/error.h"', "",
           "#define ROS_X_BIT 0x20000u", ""]
    for s in swis:
        out.append(f"#define {s['name']} 0x{s['number']:05X}u")
        out.append(f"#define X{s['name']} 0x{s['number'] | X_BIT:05X}u")
    out.append("")
    for s in swis:
        if planned(s):
            continue
        out.append(f"/* {s['name']} &{s['number']:X} -- {s['impl']} ({s['source']}) */")
        for op in s["ops"]:
            if op.reason is not None:
                out.append(f"/*   reason {op.reason} */")
            out.append(op.signature(x=True) + ";")
            out.append(op.signature(x=False) + ";")
        out.append("")
    raw = [s for s in swis if s.get("raw")]
    if raw:
        out.append("/* SWIs whose registers pass straight through: the runtime's thunks. */")
        out.append("struct ros_cpu;")
        for s in raw:
            out.append(f"void ros_thunk_{s['name']}(struct ros_cpu *s);")
        out.append("")
    for mod, mswis in native_modules(swis).items():
        out.append(f"/* {mod}, a native module: its SWI chunk's thunks, by offset. */")
        out.append("struct ros_cpu;")
        out.append(f"extern void (*const ros_swi_thunks_{mod}[])(struct ros_cpu *s);")
        out.append(f"extern const char *const ros_swi_names_{mod}[];")
        out.append(f"extern const unsigned ros_swi_count_{mod};")
        out.append("")
    out += ["/* A native SWI, as the runtime's dispatcher sees it. */",
            "struct ros_cpu;",
            "struct ros_swi_native {",
            "    uint32_t number;",
            "    const char *name;",
            "    void (*thunk)(struct ros_cpu *s);",
            "};",
            "extern const struct ros_swi_native ros_native_swis[];",
            "extern const unsigned ros_native_swi_count;",
            "", "#endif", ""]
    return "\n".join(out)


def kernel_natives(swis):
    """The kernel's own SWIs that the runtime implements: dispatched by number."""
    return [s for s in swis if s["impl"] == "native" and s["chunk"] is None]


def native_modules(swis):
    """Native modules' SWIs, by module: dispatched by chunk, like any module's."""
    out = {}
    for s in swis:
        if s["impl"] == "native" and s["chunk"] is not None:
            out.setdefault(s["module"], []).append(s)
    return out


# ---- thunks: compiled code -> a native typed function ----------------------

def unpack_param(p):
    reg, t = p["reg"], ctype(p)
    cls = TYPES[p["type"]][1]
    if cls == "int":
        return f"({t})s->r[{reg}]"
    if p.get("null_ok"):
        return f"({t})(s->r[{reg}] ? ros_ptr(s->r[{reg}]) : NULL)"
    return f"({t})ros_ptr(s->r[{reg}])"


def native_call(op, indent):
    pad = " " * indent
    lines = []
    for r in op.results:
        lines.append(f"{pad}{decl(ctype(r, out=True), r['name'])} = 0;")
    for f in op.flags:
        lines.append(f"{pad}int {f['name']} = 0;")
    args = [unpack_param(p) for p in op.params]
    args += [f"&{r['name']}" for r in op.results] + [f"&{f['name']}" for f in op.flags]
    # Native code that calls back into compiled code must build on the SVC
    # stack where this caller left it, not over its frames (cpu.h).
    lines.append(f"{pad}uint32_t outer_sp = ros_svc_sp_enter(s);")
    lines.append(f"{pad}os_error *e = x{op.fn}({', '.join(args)});")
    lines.append(f"{pad}ros_svc_sp = outer_sp;")
    lines.append(f"{pad}if (e) {{")
    lines.append(f"{pad}    ros_swi_fail(s, e);")
    lines.append(f"{pad}    return;")
    lines.append(f"{pad}}}")
    for r in op.results:
        if TYPES[r["type"]][1] == "int":
            lines.append(f"{pad}s->r[{r['reg']}] = (uint32_t){r['name']};")
        else:
            lines.append(f"{pad}s->r[{r['reg']}] = {r['name']} ? ros_addr({r['name']}) : 0;")
    for f in op.flags:
        lines.append(f"{pad}s->{f['flag']} = {f['name']} != 0;")
    lines.append(f"{pad}s->v = 0;")
    return lines


def thunk(s):
    lines = [f"void ros_thunk_{s['name']}(struct ros_cpu *s)", "{"]
    if s.get("kind") == "multiplexer":
        sel = s["selector"]
        lines.append(f"    switch (s->r[{sel}]) {{")
        for op in s["ops"]:
            lines.append(f"    case {op.reason}: {{")
            lines += native_call(op, 8)
            lines.append("        return;")
            lines.append("    }")
        # Each multiplexer has its own "unknown reason" error in RISC OS
        # (hdr/NewErrors); the definition names it.
        bad = s.get("bad_reason", {"errnum": "ROS_ERR_UNIMPLEMENTED",
                                   "message": f"Unknown {s['name']} call"})
        errnum = bad["errnum"] if isinstance(bad["errnum"], str) else f"0x{bad['errnum']:X}u"
        lines.append("    default:")
        lines.append(f"        ros_swi_fail(s, ros_error({errnum}, \"%s\", \"{bad['message']}\"));")
        lines.append("    }")
    else:
        lines += native_call(s["ops"][0], 4)
    lines.append("}")
    return lines


# ---- reverse thunks: a native caller -> a compiled module -------------------

def reverse(op):
    s = op.swi
    lines = [op.signature(x=True), "{", "    struct ros_cpu s;", "    ros_cpu_enter(&s);"]
    if op.reason is not None:
        lines.append(f"    s.r[{s['selector']}] = {op.reason};")
    for p in op.params:
        if TYPES[p["type"]][1] == "int":
            lines.append(f"    s.r[{p['reg']}] = (uint32_t){p['name']};")
        else:
            lines.append(f"    s.r[{p['reg']}] = {p['name']} ? ros_addr({p['name']}) : 0;")
    lines.append(f"    ros_swi(&s, X{s['name']});")
    lines.append("    if (s.v)")
    lines.append("        return (os_error *)ros_ptr(s.r[0]);")
    for r in op.results:
        if TYPES[r["type"]][1] == "int":
            lines.append(f"    if ({r['name']}) *{r['name']} = ({ctype(r, out=True)})s.r[{r['reg']}];")
        else:
            lines.append(f"    if ({r['name']}) *{r['name']} = ({ctype(r, out=True)})ros_ptr(s.r[{r['reg']}]);")
    for f in op.flags:
        lines.append(f"    if ({f['name']}) *{f['name']} = (int)s.{f['flag']};")
    lines.append("    return NULL;")
    lines.append("}")
    return lines


def source(swis):
    out = ["/* api_gen.c -- generated by api/gen.py from the TOML in api/defs: do not edit. */",
           "#include <stddef.h>", "",
           '#include "rosgd/api.h"', '#include "rosgd/cpu.h"', '#include "rosgd/swi.h"', ""]
    for s in swis:
        if s["impl"] == "native" and not s.get("raw"):
            out += thunk(s) + [""]
    natives = kernel_natives(swis)
    out.append("const struct ros_swi_native ros_native_swis[] = {")
    for s in natives:
        out.append(f"    {{ 0x{s['number']:05X}u, \"{s['name']}\", ros_thunk_{s['name']} }},")
    out.append("};")
    out.append(f"const unsigned ros_native_swi_count = {len(natives)};")
    out.append("")
    for mod, mswis in native_modules(swis).items():
        chunk = mswis[0]["chunk"]
        count = max(s["number"] - chunk for s in mswis) + 1
        by_offset = {s["number"] - chunk: s for s in mswis}
        prefix = mswis[0]["name"].split("_", 1)[0]
        out.append(f"void (*const ros_swi_thunks_{mod}[])(struct ros_cpu *s) = {{")
        for i in range(count):
            s = by_offset.get(i)
            out.append(f"    ros_thunk_{s['name']}," if s else "    NULL,")
        out.append("};")
        out.append(f"const char *const ros_swi_names_{mod}[] = {{")
        for i in range(count):
            s = by_offset.get(i)
            out.append(f"    \"{s['name'][len(prefix) + 1:]}\"," if s else "    \"\",")
        out.append("};")
        out.append(f"const unsigned ros_swi_count_{mod} = {count};")
        out.append("")
    for s in swis:
        if s["impl"].startswith("module:"):
            for op in s["ops"]:
                out += reverse(op) + [""]
    # the raising forms, for every operation
    for s in swis:
        if planned(s):
            continue
        for op in s["ops"]:
            out.append(op.signature(x=False))
            out.append("{")
            out.append(f"    os_error *e = x{op.fn}({', '.join(op.call_args())});")
            out.append("    if (e)")
            out.append("        ros_raise(e);")
            out.append("}")
            out.append("")
    return "\n".join(out)


def main():
    outdir = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    swis = load(os.path.join(here, "defs"))
    os.makedirs(os.path.join(outdir, "rosgd"), exist_ok=True)
    with open(os.path.join(outdir, "rosgd", "api.h"), "w") as fh:
        fh.write(header(swis))
    with open(os.path.join(outdir, "api_gen.c"), "w") as fh:
        fh.write(source(swis))
    # For the ObjAsm compiler (rosasm --emit c --swis): the SWIs compiled
    # code may call straight through their thunks.
    with open(os.path.join(outdir, "native_swis.txt"), "w") as fh:
        # Only the kernel's: a module's SWIs go through its chunk, so that
        # calling one whose module is absent fails as it should.
        for s in kernel_natives(swis):
            fh.write(f"0x{s['number']:05X} {s['name']}\n")
    # For the ObjAsm compiler (rosasm --emit c --swi-regs): the registers
    # each SWI reads and writes.  A SWI preserves the rest, as RISC OS
    # documents; R0 may always come back as an error.  Raw SWIs pass their
    # registers straight through, so are left out: all of them, unknown.
    with open(os.path.join(outdir, "swi_regs.txt"), "w") as fh:
        for s in swis:
            m = reg_masks(s)
            if m is None:
                continue
            for k in range(s.get("count", 1)):
                fh.write(f"0x{s['number'] + k:05X} in=0x{m[0]:04X} out=0x{m[1]:04X} {s['name']}\n")
    ops = sum(len(s["ops"]) for s in swis if not planned(s))
    later = sum(1 for s in swis if planned(s))
    print(f"api: {len(swis)} SWIs ({later} planned), {ops} typed operations -> {outdir}")


if __name__ == "__main__":
    main()
