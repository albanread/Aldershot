#!/usr/bin/env python3
"""A territory's definition, as C, from RISC OS's territory module sources.

    mkterritory.py <TerritoryModule dir> <hdr dir> <Territory> <out.c>

RISC OS builds each territory module (Internat/Territory/TerritoryModule)
from one ObjAsm file per territory, s/<Territory>, which sets the
territory's values -- its number, alphabet, calendar, time zones, formats
and symbols -- and defines its tables as macros (DoToLowerTable ...), and
the tables every territory shares (s/Tables: ControlTable, DigitTable,
XDigitTable).  This reads both, with the numbers from hdr/Countries, and
writes them as a struct territory_def for modules/territory: the tables
are RISC OS's own, byte for byte, nothing retyped.

The ObjAsm understood is what those files use: SETS, SETA, SETL and "*"
with expressions of numbers (decimal, &hex, 2_binary), names, + - * and
:OR: :AND: :SHL:, "$Name" substituted in strings; the macros' labels, DCB
(strings with \\\\ for a backslash, and numbers) and DCD.  Anything else in
a table is an error, never guessed past.

The C is derived from RISC OS sources (Apache 2.0); it goes to the build
tree, not the repository.
"""
import os
import re
import sys

# hdr/Territory's WriteDirection_* (the manager's exported header)
WRITE_DIRECTION = {
    "WriteDirection_LeftToRight": 0, "WriteDirection_RightToLeft": 1,
    "WriteDirection_UpToDown": 0, "WriteDirection_DownToUp": 2,
    "WriteDirection_HorizontalLines": 0, "WriteDirection_VerticalLines": 4,
}

# The tables a territory defines (label: bytes), the property tables in
# Territory_CharacterPropertyTable's order (hdr/Territory Property_*)
BYTE_TABLES = ["ToLowerTable", "ToUpperTable", "ToControlTable", "ToPlainTable",
               "ToValueTable", "ToRepresentationTable", "SortValueTable",
               "ToPlainForCollateTable"]
PROPERTIES = ["ControlTable", "UppercaseTable", "LowercaseTable", "AlphaTable",
              "PunctuationTable", "SpaceTable", "DigitTable", "XDigitTable",
              "AccentedTable", "ForwardFlowTable", "BackwardFlowTable"]
FLAGS = ["CollateLatin1Ligatures", "CollateOELigatures", "CollateDanishAA",
         "CollateThornAsTH", "CollateGermanSharpS", "CollateAccentsBackwards",
         "JapaneseEras"]


def die(msg):
    sys.exit(f"mkterritory: {msg}")


def lines(path):
    """The file's lines, comments off, a line ending "\\" joined to the next"""
    held = ""
    for raw in open(path, encoding="latin-1"):
        c = code_part(raw.rstrip("\n").rstrip("\r")).rstrip()
        if c.endswith("\\"):
            held += c[:-1] + " "
            continue
        yield held + c
        held = ""


def code_part(line):
    """The line up to its comment: ';' outside a string"""
    q = False
    for i, c in enumerate(line):
        if c == '"':
            q = not q
        elif c == ";" and not q:
            return line[:i]
    return line


def operands(text):
    """Comma-separated operands, strings kept whole"""
    out, cur, q = [], "", False
    for c in text:
        if c == '"':
            q = not q
            cur += c
        elif c == "," and not q:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def string(tok, syms):
    """An ObjAsm string literal: "\\\\" a backslash, $Name substituted"""
    if not (len(tok) >= 2 and tok[0] == '"' and tok[-1] == '"'):
        die(f"not a string: {tok}")
    body = tok[1:-1].replace("\\\\", "\\")
    return re.sub(r"\$([A-Za-z_][A-Za-z0-9_]*)\.?", lambda m: str(syms[m.group(1)]), body)


def evaluate(expr, syms):
    """An ObjAsm numeric expression"""
    e = expr.strip()
    e = re.sub(r"\$([A-Za-z_][A-Za-z0-9_]*)", lambda m: str(syms[m.group(1)]), e)
    e = e.replace(":OR:", "|").replace(":AND:", "&").replace(":SHL:", "<<")
    e = re.sub(r"2_([01]+)", lambda m: str(int(m.group(1), 2)), e)
    e = re.sub(r"&([0-9A-Fa-f]+)", lambda m: str(int(m.group(1), 16)), e)

    def name(m):
        n = m.group(0)
        if n in syms:
            return str(syms[n])
        die(f"unknown name {n} in {expr!r}")
    e = re.sub(r"[A-Za-z_][A-Za-z0-9_]*", name, e)
    if not re.fullmatch(r"[0-9\s|&<()+*\-]+", e):
        die(f"cannot evaluate {expr!r}")
    return eval(e)                                          # digits and operators only


def countries(hdr):
    """TerritoryNum_* and ISOAlphabet_* from hdr/Countries: "^ n" then "# 1" each"""
    syms, at = {}, 0
    for line in lines(os.path.join(hdr, "Countries")):
        c = code_part(line)
        m = re.match(r"^\s+\^\s+(\d+)", c)
        if m:
            at = int(m.group(1))
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+#\s+(\d+)", c)
        if m:
            syms[m.group(1)] = at
            at += int(m.group(2))
    return syms


def parse(path, syms, tables, flags):
    """A territory file or s/Tables: its settings into syms, its tables
    (macro bodies or plain) into tables"""
    labels = []
    for line in lines(path):
        c = code_part(line)
        if not c.strip():
            continue
        words = c.split(None, 2)
        if c[0] not in " \t":                               # a label, or a setting
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+(SETS|SETA|SETL|\*)\s+(.*)$", c)
            if m:
                n, op, v = m.group(1), m.group(2), m.group(3).strip()
                if op == "SETS":
                    syms[n] = string(v, syms)
                elif op == "SETL":
                    flags[n] = v == "{TRUE}"
                else:
                    syms[n] = evaluate(v, syms)
                continue
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*$", c)
            if m:
                # labels with no data between them name the same table
                if labels and tables[labels[0]]:
                    labels = []
                labels.append(m.group(1))
                for l in labels:
                    tables.setdefault(l, [])
                continue
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+(DCB|DCD)\s+(.*)$", c)
            if m:
                labels = [m.group(1)]
                tables.setdefault(m.group(1), [])
                words = [m.group(2), m.group(3)]
            else:
                labels = []
                continue
        else:
            words = c.strip().split(None, 1)
        op = words[0].upper()
        if op in ("MEND", "MACRO", "LTORG", "END", "ALIGN") or op.startswith("DO") or op == "LNK":
            labels = []
            continue
        if op not in ("DCB", "DCD") or not labels:
            labels = [] if op not in ("DCB", "DCD") else labels
            continue
        data = []
        for tok in operands(words[1]):
            if tok.startswith('"'):
                if op != "DCB":
                    die(f"a string in a DCD: {line}")
                data += [b for b in string(tok, syms).encode("latin-1")]
            elif op == "DCB":
                data.append(evaluate(tok, syms) & 0xFF)
            else:
                data.append(evaluate(tok, syms) & 0xFFFFFFFF)
        for l in labels:
            tables[l].append((op, data))


def byte_table(tables, name, size):
    parts = tables.get(name)
    if not parts:
        die(f"no table {name}")
    out = []
    for op, data in parts:
        if op != "DCB":
            die(f"{name} is not bytes")
        out += data
    if len(out) != size:
        die(f"{name} has {len(out)} bytes, not {size}")
    return out


def word_table(tables, name):
    parts = tables.get(name)
    if not parts:
        die(f"no table {name}")
    out = []
    for op, data in parts:
        if op != "DCD":
            die(f"{name} is not words")
        out += data
    if len(out) != 8:
        die(f"{name} has {len(out)} words, not 8")
    return out


def c_string(s):
    out = '"'
    for b in s.encode("latin-1"):
        out += chr(b) if 32 <= b < 127 and chr(b) not in '"\\?' else f"\\{b:03o}"
    return out + '"'


def c_bytes(data, indent="        "):
    rows = []
    for i in range(0, len(data), 16):
        rows.append(indent + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return "\n".join(rows)


def main():
    src, hdr, territory, out = sys.argv[1:5]
    syms = countries(hdr)
    syms.update(WRITE_DIRECTION)
    syms["Territory"] = territory
    flags = {f: False for f in FLAGS}
    syms["IMESWIChunk"] = 0
    tables = {}
    parse(os.path.join(src, "s", territory), syms, tables, flags)
    # s/Territory: Latin1-like alphabets collate the fi and fl ligatures
    flags["CollateLatin1Ligatures"] = syms["AlphNum"] in (
        syms["ISOAlphabet_Latin1"], syms["ISOAlphabet_Latin5"], syms["ISOAlphabet_Latin9"])
    parse(os.path.join(src, "s", "Tables"), syms, tables, flags)

    zones = []
    for i in range(syms["NumberOfTZ"]):
        k = str(i)
        zones.append((syms["NODST" + k], syms["DST" + k], syms["NODSTOffset" + k],
                      syms["DSTOffset" + k]))
    groupings = {}
    for g in ("Grouping", "MGrouping"):
        groupings[g] = [evaluate(t, syms) & 0xFF for t in syms[g].split(",")]

    o = []
    o.append(f"/* territory_{territory.lower()}.c -- the {territory} territory, generated by "
             "tools/mkterritory.py")
    o.append(f" * from RISC OS's Internat/Territory/TerritoryModule (s/{territory}, s/Tables): "
             "do not edit. */")
    o.append('#include "territory.h"')
    o.append("")
    o.append(f"const struct territory_def territory_{territory.lower()} = {{")
    o.append(f"    .title = {c_string(territory)},")
    o.append(f"    .help = {c_string(syms['Help'])},")
    o.append(f"    .number = {syms['TerrNum']},")
    o.append(f"    .alphabet = {syms['AlphNum']},")
    o.append(f"    .alphabet_name = {c_string(syms['AlphabetName'])},")
    o.append(f"    .write_direction = {syms['WriteDir']},")
    o.append(f"    .ime_swi_chunk = {syms['IMESWIChunk']},")
    cal = ["FirstWorkDay", "LastWorkDay", "NumberOfMonths", "MaxAMPMLength", "MaxWELength",
           "MaxW3Length", "MaxDYLength", "MaxSTLength", "MaxMOLength", "MaxM3Length",
           "MaxTZLength"]
    o.append("    .calendar = { " + ", ".join(str(syms[c]) for c in cal) + " },")
    o.append(f"    .max_tz_length = {syms['MaxTZLength']},")
    o.append(f"    .zone_count = {len(zones)},")
    o.append("    .zones = {")
    for std, dst, so, do in zones:
        o.append(f"        {{ {c_string(std)}, {c_string(dst)}, {so}, {do} }},")
    o.append("    },")
    o.append(f"    .date_format = {c_string(syms['DateFormat'])},")
    o.append(f"    .time_format = {c_string(syms['TimeFormat'])},")
    o.append(f"    .date_and_time = {c_string(syms['DateAndTime'])},")
    o.append("    .symbols = {")
    for n in ("Decimal", "Thousand", "IntCurr", "Currency", "MDecimal", "MThousand",
              "MPositive", "MNegative", "ListSymbol"):
        o.append(f"        .{n.lower()} = {c_string(syms[n])},")
    for g in ("Grouping", "MGrouping"):
        o.append(f"        .{g.lower()} = {{ {', '.join(str(b) for b in groupings[g])} }},")
        o.append(f"        .{g.lower()}_length = {len(groupings[g])},")
    nums = ["int_frac_digits", "frac_digits", "p_cs_precedes", "p_sep_by_space", "n_cs_precedes",
            "n_sep_by_space", "p_sign_posn", "n_sign_posn"]
    o.append("        .numbers = { " + ", ".join(str(syms[n]) for n in nums) + " },")
    o.append("    },")
    for f in FLAGS:
        field = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", f).lower()
        o.append(f"    .{field} = {1 if flags[f] else 0},")
    sizes = {"ToRepresentationTable": 16}
    for t in BYTE_TABLES:
        field = {"ToLowerTable": "to_lower", "ToUpperTable": "to_upper",
                 "ToControlTable": "to_control", "ToPlainTable": "to_plain",
                 "ToValueTable": "to_value", "ToRepresentationTable": "representation",
                 "SortValueTable": "sort_value", "ToPlainForCollateTable": "plain_for_collate"}[t]
        o.append(f"    .{field} = {{")
        o.append(c_bytes(byte_table(tables, t, sizes.get(t, 256))))
        o.append("    },")
    o.append("    .properties = {")
    for p in PROPERTIES:
        words = word_table(tables, p)
        o.append(f"        {{ {', '.join(f'0x{w:08X}u' for w in words)} }},     /* {p} */")
    o.append("    },")
    o.append("};")
    o.append("")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as fh:
        fh.write("\n".join(o))


if __name__ == "__main__":
    main()
