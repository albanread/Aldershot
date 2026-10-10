#!/usr/bin/env python3
"""defmod.py -- OSLib's DefMod, the C header half (`defmod -h`), for ROSGD.

    defmod.py < Module.def > Module.h

A component that describes its SWIs in DefMod's language (ToolAction's
Documents/def/ToolAction) has its C header made from it by the build
(`${DEFMOD} -h > h.ToolAction < ${DEF_TOOLACTION}`).  DefMod exists in the
source drop only as an ARM binary, so this writes the same header from the
same description, in DefMod's layout and with its naming:

  - SWI names and reason codes: `#define ToolAction_SetIdent 0x140140`, and
    the X form for a SWI with its own number;
  - structures and types: `ToolAction_Object = .Struct (...)` becomes
    `struct toolaction_object`, `.Bits` bits, `.Int` int, `.Bool` osbool,
    `.Ref .String` char *, a named type its lower-case name;
  - constants: `ToolAction_IsText = Gadget_Flags: 0x02` becomes
    `#define toolaction_IS_TEXT ((gadget_flags) 0x2u)`.

Not written: the veneers' prototypes (`xtoolaction_set_ident()`), which go
with DefMod's -l library; the OSLib veneer package (L6) makes those.
Constructs outside the subset (.Union, arrays, .Asm) stop it with an error
rather than being skipped.
"""
import re
import sys

TOKEN = re.compile(r'\s*(//[^\n]*|"(?:[^"\\]|\\.)*"|0x[0-9A-Fa-f]+\*?|\d+\*?|\.?[A-Za-z_][A-Za-z0-9_]*|->|[;,=:()#!*\[\]])')


def tokens(text):
    out, pos = [], 0
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            if text[pos:].strip() == "":
                break
            raise SystemExit("defmod: cannot read at %r" % text[pos:pos + 30])
        pos = m.end()
        t = m.group(1)
        if not t.startswith("//"):
            out.append(t)
    return out


def words(name):
    """CamelCase -> words: ToolActionOutOfMemory -> Tool Action Out Of Memory."""
    return re.findall(r"[A-Z]+(?=[A-Z][a-z]|\d|$)|[A-Z]?[a-z]+|\d+|[A-Z]+", name)


def split(name):
    pre, _, rest = name.partition("_")
    return (pre, rest) if rest else ("", pre)


def type_name(name):
    pre, rest = split(name)
    return (pre.lower() + "_" if pre else "") + "_".join(w.lower() for w in words(rest))


def const_name(name):
    pre, rest = split(name)
    return (pre.lower() + "_" if pre else "") + "_".join(w.upper() for w in words(rest))


BASE = {".Bits": "bits", ".Int": "int", ".Bool": "osbool", ".Byte": "byte", ".Char": "char",
        ".Short": "short", ".String": "char", ".Data": "void"}


class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if want is not None and tok != want:
            raise SystemExit("defmod: expected %r, found %r" % (want, tok))
        self.i += 1
        return tok

    def desc(self):
        if self.peek() and self.peek().startswith('"'):
            return self.take()[1:-1]
        return None

    def ctype(self):
        """A type expression, as C: returns (decl prefix, is_struct fields or None)."""
        tok = self.take()
        if tok == ".Ref":
            inner, fields = self.ctype()
            if fields is not None:
                raise SystemExit("defmod: .Ref .Struct is outside the subset")
            return inner + " *", None
        if tok == ".Struct":
            base = None
            if self.peek() == ":":
                self.take(":")
                base = self.take()
            self.take("(")
            fields = []
            while True:
                ft, sub = self.ctype()
                if sub is not None:
                    raise SystemExit("defmod: nested .Struct is outside the subset")
                self.take(":")
                fname = self.take()
                if self.peek() == "[":
                    raise SystemExit("defmod: arrays are outside the subset")
                self.desc()
                fields.append((ft, fname))
                if self.peek() == ",":
                    self.take(",")
                    continue
                break
            self.take(")")
            return base, fields
        if tok in BASE:
            return BASE[tok], None
        if tok.startswith("."):
            raise SystemExit("defmod: type %s is outside the subset" % tok)
        return type_name(tok), None


def number(tok):
    return int(tok.rstrip("*"), 0)


def hexu(v):
    return "0x%Xu" % v


def generate(text):
    p = Parser(tokens(text))
    title, needs, swis, structs, typedefs, consts = None, [], [], [], [], []
    while p.peek() is not None:
        kw = p.take()
        if kw == "TITLE":
            title = p.take()
            p.take(";")
        elif kw == "AUTHOR":
            p.take()
            p.take(";")
        elif kw == "NEEDS":
            needs.append(p.take())
            while p.peek() == ",":
                p.take(",")
                needs.append(p.take())
            p.take(";")
        elif kw == "CONST":
            while True:
                name = p.take()
                p.take("=")
                ty, _ = p.ctype()
                p.take(":")
                v = number(p.take())
                p.desc()
                consts.append((const_name(name), ty, v))
                if p.peek() is None or p.take() == ";":   # the last list may end the file
                    break
        elif kw == "TYPE":
            while True:
                name = p.take()
                if p.peek() == "=":
                    p.take("=")
                    ty, fields = p.ctype()
                    if fields is not None:
                        structs.append((type_name(name), ty, fields))
                    else:
                        typedefs.append((type_name(name), ty))
                p.desc()
                if p.peek() is None or p.take() == ";":   # the last list may end the file
                    break
        elif kw == "SWI":
            while True:
                name = p.take()
                p.take("=")
                p.take("(")
                num, reason, depth = None, None, 1
                while depth:
                    tok = p.take()
                    if tok == "(":
                        depth += 1
                    elif tok == ")":
                        depth -= 1
                    elif tok == "NUMBER":
                        num = number(p.take())
                    elif tok == "#" and reason is None:
                        reason = number(p.take())
                swis.append((name, num, reason))
                if p.peek() is None or p.take() == ";":   # the last list may end the file
                    break
        else:
            raise SystemExit("defmod: %r is outside the subset" % kw)

    g = title.lower()
    h = ["#ifndef %s_H" % g, "#define %s_H" % g, "",
         "/* C header file for %s" % title,
         " * written by ROSGD's tools/defmod.py (DefMod -h) from its description",
         " */", ""]
    for n in ["Types"] + needs:
        h += ["#ifndef %s_H" % n.lower(), '#include "oslib/%s.h"' % n.lower(), "#endif", ""]
    if "Toolbox" in needs:
        h += ["#ifndef gadget_H", '#include "oslib/gadget.h"', "#endif", ""]
    h += ["/**********************************", " * SWI names and SWI reason codes *",
          " **********************************/"]
    for name, num, reason in swis:
        v = reason if reason is not None else num
        h += ["#undef  %s" % name, "#define %-40s0x%X" % (name, v)]
        if reason is None:
            h += ["#undef  X%s" % name, "#define %-40s0x%X" % ("X" + name, v | 0x20000)]
    if structs:
        h += ["", "/************************************", " * Structure and union declarations *",
              " ************************************/"]
        for n, _, _ in structs:
            h.append("typedef struct %-32s%s;" % (n, n))
    h += ["", "/********************", " * Type definitions *", " ********************/"]
    for n, base, fields in structs:
        body = ["      %s%s%s;" % (ft, "" if ft.endswith("*") else " ", fn) for ft, fn in fields]
        if base:
            body.insert(0, "      %s_MEMBERS" % const_name(base))
        body[0] = "   {  " + body[0].lstrip()
        h += ["struct %s" % n] + body + ["   };", ""]
    for n, ty in typedefs:
        h.append("typedef %s %s;" % (ty, n))
    h += ["", "/************************", " * Constant definitions *", " ************************/"]
    for n, ty, v in consts:
        val = {"bits": hexu(v), "int": str(v)}.get(ty, "((%s) %s)" % (ty, hexu(v)))
        h.append("#define %-40s%s" % (n, val))
    h += ["", "#endif"]
    return "\n".join(h) + "\n"


if __name__ == "__main__":
    sys.stdout.write(generate(sys.stdin.read()))
