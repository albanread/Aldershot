#!/usr/bin/env python3
"""The International module's tables, as C, from its ObjAsm sources.

    mkinternational.py <Internat/Inter dir> <hdr dir> <out.c>

RISC OS's International module (Internat/Inter) is three ObjAsm files of
tables and a little code.  This reads the tables and writes them as C for
modules/international, nothing retyped:

  s/InterFonts   FontTable: the system font's glyphs, eight bytes each,
                 labelled by their UCS code (U_00C0), some labels aliases
                 of others (U_00A0 * U_0020); then UCSTable, which maps
                 UCS codes to glyphs: ranges (NEWUCS base, top), each a
                 halfword a code, the glyph's offset in FontTable or
                 &FFFF (UNK001) for none, a range's halfwords padded to a
                 word as ObjAsm's DCD pads them.
  s/UCSTables    each alphabet's 256 UCS codes (&FFFFFFFF: none)
  s/InterBody    the names: CountryStrings and AlphabetStrings (a name, a
                 zero and its number, in the order a name is looked for),
                 CountryList and AlphabetList (number to name, in the order
                 a number is looked for), CToATable (a country's alphabet),
                 FontPointers (an alphabet's UCS table) and Alpha2Strings
                 (ISO 3166-1 alpha-2 codes, two characters a country);
                 Digits, the hex digits UTF8's top half is drawn with

and the numbers from hdr/Countries.  The build's choices are the module's
own: NewAccents and DoUTF8 {TRUE}, DoBfont {FALSE}.  FontTable is written
byte for byte as it is assembled, so each glyph's offset is the module's:
Service_International 7 leaves the offset in R4, as RISC OS's does.

The C is derived from RISC OS sources (Apache 2.0); it goes to the build
tree, not the repository.
"""
import os
import re
import sys

FLAGS = {"NewAccents": True, "DoUTF8": True, "DoBfont": False,
         "standalone": False, "international_help": True}


def die(msg):
    sys.exit(f"mkinternational: {msg}")


def code_part(line):
    """The line up to its comment: ';' outside a string"""
    q = False
    for i, c in enumerate(line):
        if c == '"':
            q = not q
        elif c == ";" and not q:
            return line[:i]
    return line


def source(path):
    """The file's lines, comments off, the build's conditionals decided:
    [ Flag, |, ] around the tables (only the flags in FLAGS; anything else
    inside a table is refused)"""
    out, stack = [], []
    for raw in open(path, encoding="latin-1"):
        c = code_part(raw.rstrip("\r\n")).rstrip()
        s = c.strip()
        m = re.match(r"^\[\s*(\S.*)$", s)
        if m and not s.startswith("[ \"") and ":DEF:" not in s and ":LNOT:" not in s:
            cond = m.group(1).strip()
            if cond not in FLAGS:
                # a macro's own test or an unrelated one: kept for the caller
                stack.append(None)
                out.append(c)
                continue
            stack.append(FLAGS[cond])
            continue
        if s == "|" and stack:
            if stack[-1] is None:
                out.append(c)
            else:
                stack[-1] = not stack[-1]
            continue
        if s == "]" and stack:
            if stack.pop() is None:
                out.append(c)
            continue
        if all(x is not False for x in stack):
            out.append(c)
    return out


def countries(hdr):
    """The numbers in hdr/Countries: "^ n", then "# 1" each"""
    syms, at = {}, 0
    for raw in open(os.path.join(hdr, "Countries"), encoding="latin-1"):
        c = code_part(raw)
        m = re.match(r"^\s+\^\s+(\d+)", c)
        if m:
            at = int(m.group(1))
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+#\s+(\d+)", c)
        if m:
            syms[m.group(1)] = at
            at += int(m.group(2))
    return syms


def number(tok, syms):
    t = tok.strip()
    if t.startswith("&"):
        return int(t[1:], 16)
    if re.fullmatch(r"\d+", t):
        return int(t)
    if t in syms:
        return syms[t]
    die(f"unknown value {tok!r}")


def region(lines, start, stop):
    """The lines after the label start up to (not including) the first
    line stop() accepts"""
    for i, l in enumerate(lines):
        if re.match(rf"^{start}\b", l):
            body = [lines[i]]
            for m in lines[i + 1:]:
                if stop(m):
                    return body
                body.append(m)
            die(f"no end to {start}")
    die(f"no {start}")


# ---- InterFonts --------------------------------------------------------------------------------

def font_table(lines):
    """FontTable's bytes as assembled, and each label's offset"""
    body = region(lines, "FontTable", lambda l: l.strip().startswith("MACRO"))
    data, offsets, aliases = bytearray(), {}, {}
    for l in body[1:]:
        if not l.strip():
            continue
        m = re.match(r"^([A-Z]_[0-9A-F]{4})\s*=\s*(.*)$", l)
        if m:
            b = [number(x, {}) for x in m.group(2).split(",")]
            if len(b) != 8 or any(x > 255 for x in b):
                die(f"a glyph is not eight bytes: {l}")
            offsets[m.group(1)] = len(data)
            data += bytes(b)
            continue
        m = re.match(r"^([A-Z]_[0-9A-F]{4})\s*\*\s*([A-Z]_[0-9A-F]{4})\s*$", l)
        if m:
            aliases[m.group(1)] = m.group(2)
            continue
        die(f"not understood in FontTable: {l!r}")
    for a in aliases:
        t = a
        for _ in range(8):
            t = aliases.get(t, t)
        if t not in offsets:
            die(f"{a} is an alias of nothing")
        offsets[a] = offsets[t]
    return bytes(data), offsets


def ucs_table(lines, offsets):
    """UCSTable's ranges: (base, top, [offset or 0xFFFF])"""
    body = region(lines, "UCSTable", lambda l: l.strip().startswith("LNK"))
    ranges, cur = [], None
    for l in body[1:]:
        s = l.strip()
        if not s:
            continue
        words = s.split(None, 1)
        if words[0] == "NEWUCS":
            v = [number(x, {}) for x in words[1].split(",")]
            cur = (v[0], v[-1], [])
            ranges.append(cur)
        elif words[0] == "UCS":
            for name in (x.strip() for x in words[1].split(",")):
                if name == "UNK001":
                    cur[2].append(0xFFFF)
                elif name in offsets:
                    cur[2].append(offsets[name])
                else:
                    die(f"UCSTable names no glyph {name}")
        elif words[0] == "DCD" and number(words[1], {}) == 0xFFFFFFFF:
            break
        else:
            die(f"not understood in UCSTable: {l!r}")
    for base, top, e in ranges:
        if len(e) != top - base + 1:
            die(f"range &{base:X}-&{top:X} has {len(e)} entries")
        if any(o > 0x7FFF and o != 0xFFFF for o in e):
            die("a glyph offset has bit 15 set")
    return ranges


def ucs_tables(path):
    """s/UCSTables: label -> 256 words"""
    tables, cur = {}, None
    for l in source(path):
        s = l.strip()
        if not s or s == "END":
            continue
        if l[0] not in " \t":
            cur = s
            tables[cur] = []
            continue
        if s.startswith("&"):
            tables[cur] += [number(x, {}) for x in s[1:].split(",")]
            continue
        die(f"not understood in UCSTables: {l!r}")
    for k, v in tables.items():
        if len(v) != 256:
            die(f"{k} has {len(v)} entries")
    return tables


# ---- InterBody ---------------------------------------------------------------------------------

def strings(lines, label, syms):
    """CountryStrings or AlphabetStrings: (label, name, number) until '= 0'"""
    out = []
    started = False
    for l in lines:
        if not started:
            if re.match(rf"^{label}\b", l):
                started = True
            continue
        s = l.strip()
        if not s:
            continue
        m = re.match(r'^(String\w+)\s*=\s*"([^"]*)"\s*,\s*0\s*,\s*(\w+)\s*$', l)
        if m:
            out.append((m.group(1), m.group(2), number(m.group(3), syms)))
            continue
        if re.fullmatch(r"=\s*0", s):
            return out
        die(f"not understood in {label}: {l!r}")
    die(f"no {label}")


def inter_table(lines, label, syms):
    """An InterTable (CountryList, AlphabetList, FontPointers): (number, name)
    in order, to InterEnd"""
    out = []
    started = False
    for l in lines:
        if not started:
            if re.match(rf"^{label}\s+InterTable\b", l):
                started = True
            continue
        s = l.strip()
        if not s:
            continue
        m = re.match(r"^InterEntry\s+(\w+)\s*,\s*(\w+)$", s)
        if m:
            if m.group(2) != "UNK001":
                out.append((number(m.group(1), syms), m.group(2)))
            continue
        if s == "InterEnd":
            return out
        die(f"not understood in {label}: {l!r}")
    die(f"no {label}")


def byte_pairs(lines, label, syms):
    """CToATable: (country, alphabet) until '= 0'"""
    out = []
    started = False
    for l in lines:
        if not started:
            if re.match(rf"^{label}\b", l):
                started = True
            continue
        s = l.strip()
        if not s:
            continue
        m = re.match(r"^=\s*(\w+)\s*,\s*(\w+)$", s)
        if m:
            out.append((number(m.group(1), syms), number(m.group(2), syms)))
            continue
        if re.fullmatch(r"=\s*0", s):
            return out
        die(f"not understood in {label}: {l!r}")
    die(f"no {label}")


def alpha2(lines):
    out = ""
    started = False
    for l in lines:
        if not started:
            if re.match(r"^Alpha2Strings\b", l):
                started = True
            continue
        s = l.strip()
        m = re.match(r'^DCB\s+"([^"]*)"$', s)
        if m:
            out += m.group(1)
            continue
        if s == "ALIGN":
            break
        die(f"not understood in Alpha2Strings: {l!r}")
    if len(out) != 200:
        die(f"Alpha2Strings has {len(out)} characters")
    return out


def digits(lines):
    """DoDefineUTF8's Digits: sixteen hex digits, five rows of four bits"""
    out = []
    started = False
    for l in lines:
        s = l.strip()
        if not started:
            m = re.match(r"^Digits\s*=\s*(.*)$", l)
            if not m:
                continue
            started = True
            s = "= " + m.group(1)
        if s == "ALIGN":
            break
        m = re.match(r"^=\s*(.*)$", s)
        if not m:
            die(f"not understood in Digits: {l!r}")
        out.append([number(x, {}) for x in m.group(1).split(",")])
    if len(out) != 16 or any(len(d) != 5 or max(d) > 15 for d in out):
        die("Digits is not sixteen digits of five rows")
    return out


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    if len(sys.argv) != 4:
        die("usage: mkinternational.py <Inter dir> <hdr dir> <out.c>")
    inter, hdr, out = sys.argv[1:]
    syms = countries(hdr)
    fonts = source(os.path.join(inter, "s", "InterFonts"))
    body = source(os.path.join(inter, "s", "InterBody"))
    glyphs, offsets = font_table(fonts)
    ranges = ucs_table(fonts, offsets)
    tables = ucs_tables(os.path.join(inter, "s", "UCSTables"))
    cstrings = strings(body, "CountryStrings", syms)
    astrings = strings(body, "AlphabetStrings", syms)
    clist = inter_table(body, "CountryList", syms)
    alist = inter_table(body, "AlphabetList", syms)
    fptrs = inter_table(body, "FontPointers", syms)
    ctoa = byte_pairs(body, "CToATable", syms)
    a2 = alpha2(body)
    hexdigits = digits(body)

    by_label = {lab: (name, num) for lab, name, num in cstrings + astrings}

    def listed(entries, kind):
        rows = []
        for num, name in entries:
            lab = "String" + name
            if lab not in by_label:
                die(f"{kind} names no string {lab}")
            rows.append((num, by_label[lab][0]))
        return rows

    w = []
    w.append("/* international_tables.c -- the International module's tables, generated by")
    w.append(" * tools/mkinternational.py from RISC OS 5's Internat/Inter (s/InterFonts,")
    w.append(" * s/UCSTables, s/InterBody) and hdr/Countries: Copyright 1996-1998 Acorn")
    w.append(" * Computers Ltd, Apache License 2.0.  Do not edit. */")
    w.append('#include "international.h"')
    w.append("")
    w.append(f"const uint8_t intl_font_table[{len(glyphs)}] = {{")
    for i in range(0, len(glyphs), 8):
        w.append("    " + ", ".join(f"0x{b:02X}" for b in glyphs[i:i + 8]) + ",")
    w.append("};")
    w.append(f"const uint32_t intl_font_table_size = {len(glyphs)};")
    w.append("")
    flat = []
    w.append("const struct intl_ucs_range intl_ucs_ranges[] = {")
    for base, top, e in ranges:
        w.append(f"    {{ 0x{base:04X}, 0x{top:04X}, {len(flat)} }},")
        flat += e
    w.append("};")
    w.append(f"const unsigned intl_ucs_range_count = {len(ranges)};")
    w.append(f"const uint16_t intl_ucs_glyphs[{len(flat)}] = {{")
    for i in range(0, len(flat), 8):
        w.append("    " + ", ".join(f"0x{o:04X}" for o in flat[i:i + 8]) + ",")
    w.append("};")
    w.append("")
    w.append(f"const uint32_t intl_alphabet_ucs[{len(fptrs)}][256] = {{")
    for num, lab in fptrs:
        if lab not in tables:
            die(f"FontPointers names no table {lab}")
        w.append(f"    {{   /* {lab}, {num} */")
        t = tables[lab]
        for i in range(0, 256, 8):
            w.append("        " + ", ".join(f"0x{x:08X}u" for x in t[i:i + 8]) + ",")
        w.append("    },")
    w.append("};")
    w.append("const struct intl_number intl_font_pointers[] = {")
    for i, (num, lab) in enumerate(fptrs):
        w.append(f"    {{ {num}, {i} }},   /* {lab} */")
    w.append("    { -1, 0 },")
    w.append("};")
    w.append("")
    for cname, rows in (("intl_country_strings", cstrings), ("intl_alphabet_strings", astrings)):
        w.append(f"const struct intl_name {cname}[] = {{")
        for _, name, num in rows:
            w.append(f"    {{ {c_string(name)}, {num} }},")
        w.append("    { 0, 0 },")
        w.append("};")
    for cname, rows in (("intl_country_list", listed(clist, "CountryList")),
                        ("intl_alphabet_list", listed(alist, "AlphabetList"))):
        w.append(f"const struct intl_name {cname}[] = {{")
        for num, name in rows:
            w.append(f"    {{ {c_string(name)}, {num} }},")
        w.append("    { 0, 0 },")
        w.append("};")
    w.append("const uint8_t intl_country_alphabet[][2] = {")
    for c, a in ctoa:
        w.append(f"    {{ {c}, {a} }},")
    w.append("    { 0, 0 },")
    w.append("};")
    w.append(f"const char intl_alpha2[201] = {c_string(a2)};")
    w.append("const uint8_t intl_digits[16][5] = {")
    for d in hexdigits:
        w.append("    { " + ", ".join(f"0x{x:X}" for x in d) + " },")
    w.append("};")
    text = "\n".join(w) + "\n"
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as fh:
        fh.write(text)


if __name__ == "__main__":
    main()
