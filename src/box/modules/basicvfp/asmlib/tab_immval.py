#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read
"""tab_immval.py: the changes that asma64_tab.c needs after gen_a64_tab.py.

gen_a64_tab.py makes an entry's immediate a field only where the corpus
varies it. Where every golden of an entry has the same value, that value is
in the template. The entry then encoded that value whatever the operand
said. For example, "ext v0.16b, v1.16b, v2.16b, #3" was encoded as #4.
This script writes each such entry's value into the table (imm_fixed and
immval). Then asma64_table takes the entry only for that value, and any
other value gives "no encoder". It also changes asma64_table itself in
three ways:

  - It takes an entry with a fixed immediate only for that value.
  - It clears a register's field before it puts the register there. A
    template can hold its golden's register. For example, "ld1 {v0.16b},
    [x0], x2" held x2 whatever the operand said.
  - It returns -4 instead of -3 when the mnemonic has entries but none for
    these operands. BASIC makes a first pass over a statement with all its
    values 0, and it takes -4 to mean a known mnemonic (asma64_mv.c,
    ASM_EK_FORM).

Run after gen_a64_tab.py (idempotent):

    tab_immval.py GEN_A64_TAB_DIR CORPUS asma64_tab.c
"""
import re
import sys

sys.path.insert(0, sys.argv[1])
import gen_a64_tab as gen          # noqa: E402

corpus, tab = sys.argv[2], sys.argv[3]
values = {}
for line in open(corpus):
    p = line.rstrip("\n").split("\t")
    if len(p) < 2 or not p[1].strip():
        continue
    mn, shape, infos, ops = gen.shape_of(p[0].strip())
    if "?" in shape:
        continue
    for j, i in enumerate(infos):
        if i.get("t") == "imm":
            values.setdefault((mn, shape), set()).add(i["v"])
            break

src = open(tab).read()
if "imm_fixed" not in src:
    src = src.replace("    int8_t imm_scale;\n} a64tent;",
                      "    int8_t imm_scale;\n"
                      "    int8_t imm_fixed;  /* immval is the only immediate (tab_immval.py) */\n"
                      "    int64_t immval;\n} a64tent;")
ENTRY = re.compile(r'(    \{ "([^"]+)", "([^"]*)", 0x([0-9A-F]+)u, 0x([0-9A-F]+)u, (-?\d+), (-?\d+), (-?\d+), '
                   r'(-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+))( \},)')
fixed = 0


def one(m):
    global fixed
    mn, shape, immask, immop = m.group(2), m.group(3), int(m.group(5), 16), int(m.group(9))
    v = values.get((mn, shape))
    if immask or immop < 0 or "imm" not in shape.split(",") or not v or len(v) != 1:
        return m.group(0)
    (val,) = v
    if isinstance(val, float):
        return m.group(0)
    fixed += 1
    return f"{m.group(1)}, 1, {val}{m.group(14)}"


src = ENTRY.sub(one, src)


def patch(src, old, new, what):
    """Replaces old with new, once. It is fine if new is already there."""
    if new in src:
        return src
    if src.count(old) != 1:
        sys.exit(f"tab_immval: {what}: the generator's output changed")
    return src.replace(old, new)


src = patch(src, " * oracles/MRASM/corpus/aarch64.tsv.  Do not edit by hand.\n",
            " * oracles/MRASM/corpus/aarch64.tsv, then tab_immval.py.  Do not edit by\n"
            " * hand.\n", "the header")
src = patch(src, """            continue;
        uint32_t w = E->tmpl;
""", """            continue;
        /* an immediate the template holds: that one only (tab_immval.py) */
        if (E->imm_fixed) {
            int j = 0;
            while (j < n && o[j].t != A64_O_IMM)
                j++;
            if (j == n || o[j].imm_known != 1 || o[j].imm != E->immval)
                continue;
        }
        uint32_t w = E->tmpl;
""", "the fixed immediate")
src = patch(src, """        int slot = 0;
        for (int i = 0; i < n && slot < 3; i++) {
            if (o[i].t == A64_O_REG &&
                !(o[i].kind == RK_V && o[i].vidx >= 0))
                w |= (uint32_t)o[i].regn << slotlo[slot++];
            else if (o[i].t == A64_O_MEM)
                w |= (uint32_t)o[i].basereg << slotlo[slot++];
        }
""", """        int slot = 0;
        /* each slot's field cleared first: a template can hold its
         * golden's register there (ld1's post-increment register) */
        for (int i = 0; i < n && slot < 3; i++) {
            if (o[i].t == A64_O_REG &&
                !(o[i].kind == RK_V && o[i].vidx >= 0)) {
                w = (w & ~(31u << slotlo[slot])) | (uint32_t)o[i].regn << slotlo[slot];
                slot++;
            } else if (o[i].t == A64_O_MEM) {
                w = (w & ~(31u << slotlo[slot])) | (uint32_t)o[i].basereg << slotlo[slot];
                slot++;
            }
        }
""", "the register fields")
src = patch(src, """    (void)base;
    return -3;   /* no table entry */
""", """    /* no entry for these operands: -4 if the mnemonic has others (its
     * form, or an immediate its entries do not hold), -3 if none */
    for (unsigned t = 0; t < sizeof a64table / sizeof a64table[0]; t++)
        if (!strcmp(a64table[t].mn, base))
            return -4;
    return -3;
""", "no entry")
open(tab, "w").write(src)
print(f"tab_immval: {fixed} entries take one immediate only")
