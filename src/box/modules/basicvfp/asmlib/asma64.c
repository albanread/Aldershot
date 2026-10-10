/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asma64.c: an AArch64 (ARM64) encoder for BBC BASIC's inline assembler.
 *
 * It takes one line and produces four bytes. The bytes are identical to
 * LLVM MC's wherever both accept the text. Operand immediates go through the
 * eval callback, so the caller can supply full BASIC expressions and labels.
 *
 * The encoding reference is the frozen corpus
 * oracles/MRASM/corpus/aarch64.tsv. It has 1,181 goldens, produced from the
 * LLVM MC disassembler. The same corpus checks the MRASM/JASM encoder.
 */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "asm.h"
#include "asma64.h"
#include "asma64_int.h"

#define F(v, lo, n) (((uint32_t)((uint64_t)(v) & ((1ull << (n)) - 1))) << (lo))

/* ---------------- lexing ---------------- */

static void skipws(const char **p, const char *e)
{
    while (*p < e && isspace((unsigned char)**p))
        (*p)++;
}

static int span_eq(const char *b, const char *e, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(e - b) == n && memcmp(b, s, n) == 0;
}

/* ---------------- registers ---------------- */

static int parse_arr(const char *b, const char *e, int *bits, int *lanes)
{
    static const struct { const char *n; int bits, lanes; } tab[] = {
        { "8b", 8, 8 }, { "16b", 8, 16 }, { "4h", 16, 4 }, { "8h", 16, 8 },
        { "2s", 32, 2 }, { "4s", 32, 4 }, { "1d", 64, 1 }, { "2d", 64, 2 },
        { "1q", 128, 1 },
        { "b", 8, 0 }, { "h", 16, 0 }, { "s", 32, 0 }, { "d", 64, 0 },
        { "q", 128, 0 },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (span_eq(b, e, tab[i].n)) {
            *bits = tab[i].bits;
            *lanes = tab[i].lanes;
            return 1;
        }
    return 0;
}

/* Parses a register such as "x3", "wzr", "v0.4s" or "v0.d[1]" from [b,e).
 * It advances *pp. */
static int parse_reg(const char **pp, const char *e, int *regn, int *kind,
                     int *vbits, int *vlanes, int *vidx)
{
    const char *p = *pp;
    *vidx = -1;
    if (p >= e)
        return 0;
    int k = RK_NONE;
    if (e - p >= 3 && p[0] == 'w' && p[1] == 's' && p[2] == 'p' &&
        (e - p == 3 || !isalnum((unsigned char)p[3]))) {
        /* wsp */
        *pp = p + 3; *regn = 31; *kind = RK_W; return 1;
    }
    if (e - p >= 2 && p[0] == 's' && p[1] == 'p' &&
        (e - p == 2 || (!isalnum((unsigned char)p[2]) && p[2] != '.'))) {
        *pp = p + 2; *regn = 31; *kind = RK_X; return 1;
    }
    if (*p == 'x') k = RK_X;
    else if (*p == 'w') k = RK_W;
    else if (*p == 'b') k = RK_B;
    else if (*p == 'h') k = RK_H;
    else if (*p == 's') k = RK_S;
    else if (*p == 'd') k = RK_D;
    else if (*p == 'q') k = RK_Q;
    else if (*p == 'v') k = RK_V;
    else return 0;
    p++;
    int n = 0, digits = 0;
    while (p < e && isdigit((unsigned char)*p)) {
        n = n > 9999 ? n : n * 10 + (*p - '0');     /* saturates: no wrap to a small number */
        p++;
        digits++;
    }
    *vbits = *vlanes = 0;
    if (k == RK_V) {
        if (!digits)
            return 0;
        if (p < e && *p == '.') {
            const char *dot = ++p;
            while (p < e && isalnum((unsigned char)*p))
                p++;
            if (!parse_arr(dot, p, vbits, vlanes))
                return 0;
        }
    } else if (!digits) {
        if ((k == RK_X || k == RK_W) && span_eq(p, e, "zr")) { p = e; n = 31; }
        else if ((k == RK_X || k == RK_W) && span_eq(p, e, "sp")) { p = e; n = 31; }
        else return 0;
    } else if (n > 31)
        return 0;
    if (k == RK_V && p < e && *p == '[') {
        p++;
        int idx = 0, d2 = 0;
        while (p < e && isdigit((unsigned char)*p)) {
            idx = idx > 9999 ? idx : idx * 10 + (*p - '0');
            p++; d2++;
        }
        if (!d2 || p >= e || *p != ']')
            return 0;
        p++;
        *vidx = idx;
    }
    *pp = p;
    *regn = n;
    *kind = k;
    return 1;
}

/* ---------------- operands ---------------- */

/* Splits the operands at the commas that are outside brackets, parentheses
 * and strings (see asm__item_end). */
static int split_ops(const char *b, const char *e, a64opnd *ops, int maxops)
{
    int n = 0;
    for (const char *start = b;;) {
        const char *p = asm__item_end(start, e);
        if (n == maxops)
            return -1;
        memset(&ops[n], 0, sizeof ops[n]);
        ops[n].b = start;
        ops[n].e = p;
        ops[n].base_x = ops[n].idxreg = -1;
        ops[n].idxsh = -1;
        ops[n].idxsh_amt_known = 0;
        ops[n].wb_pre = ops[n].wb_post = 0;
        ops[n].vidx = -1;
        ops[n].imm_known = ops[n].disp_known = 1;
        n++;
        if (p == e)
            return n;
        start = p + 1;
    }
}

static int parse_imm(asm_ctx *c, const char *b, const char *e, int64_t *v,
                     int *known)
{
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (b < e && *b == '#')
        b++;
    while (b < e && isspace((unsigned char)*b)) b++;
    *known = 1;
    if (b == e)
        return asm__fail(c, ASM_EK_SYNTAX, "missing operand");
    int r = c->eval ? c->eval(c->ud, b, e, v, c->err, sizeof c->err)
                    : asm_eval_literal(NULL, b, e, v, c->err, sizeof c->err);
    if (r == 1)
        *known = 0, *v = 0;
    else if (r < 0)
        return ASM_ESYNTAX;
    return 0;
}

static int sfx_word(const char *s, const char *e, int *is_shift, int *shx)
{
    struct { const char *n; int sh, x; } tab[] = {
        { "lsl", 1, SHK_LSL }, { "lsr", 1, SHK_LSR }, { "asr", 1, SHK_ASR },
        { "ror", 1, SHK_ROR }, { "msl", 1, 4 },
        { "uxtb", 0, EXK_UXTB }, { "uxth", 0, EXK_UXTH }, { "uxtw", 0, EXK_UXTW },
        { "uxtx", 0, EXK_UXTX }, { "sxtb", 0, EXK_SXTB }, { "sxth", 0, EXK_SXTH },
        { "sxtw", 0, EXK_SXTW }, { "sxtx", 0, EXK_SXTX },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (span_eq(s, e, tab[i].n)) {
            *is_shift = tab[i].sh;
            *shx = tab[i].x;
            return 1;
        }
    return 0;
}

static int parse_opnd(asm_ctx *c, a64opnd *o)
{
    const char *p = o->b, *e = o->e;
    while (p < e && isspace((unsigned char)*p)) p++;
    while (e > p && isspace((unsigned char)e[-1])) e--;
    o->e = e;
    if (p == e) {
        snprintf(c->err, sizeof c->err, "empty operand");
        return ASM_ESYNTAX;
    }
    o->b = p;
    if (*p == '{') {
        /* register list */
        p++;
        for (;;) {
            while (p < e && isspace((unsigned char)*p)) p++;
            const char *q = p;
            int tvb = 0, tvl = 0;
            if (!parse_reg(&q, e, &o->listn[o->listcnt], &(int){0},
                           &tvb, &tvl, &(int){0}) ||
                o->listcnt >= 4)
                return snprintf(c->err, sizeof c->err, "register list expected"),
                       ASM_ESYNTAX;
            if (o->listcnt == 0) {
                o->vbits = tvb;
                o->vlanes = tvl;
            }
            o->listcnt++;
            p = q;
            while (p < e && isspace((unsigned char)*p)) p++;
            if (p < e && *p == ',') { p++; continue; }
            break;
        }
        while (p < e && isspace((unsigned char)*p)) p++;
        if (p >= e || *p != '}')
            return snprintf(c->err, sizeof c->err, "missing }"), ASM_ESYNTAX;
        p++;
        o->t = A64_O_REG;
        o->regn = o->listn[0];
        o->kind = RK_V;
        /* arrangement may follow the list for ldN */
        if (p < e && *p == '.') {
            const char *dot = ++p;
            while (p < e && isalnum((unsigned char)*p))
                p++;
            if (!parse_arr(dot, p, &o->vbits, &o->vlanes))
                return snprintf(c->err, sizeof c->err, "bad arrangement"),
                       ASM_ESYNTAX;
        }
        if (p != e)
            return snprintf(c->err, sizeof c->err, "junk after register list"),
                   ASM_ESYNTAX;
        return 0;
    }
    if (*p == '[') {
        o->t = A64_O_MEM;
        p++;
        while (p < e && isspace((unsigned char)*p)) p++;
        const char *q = p;
        if (!parse_reg(&q, e, &o->basereg, &o->base_x, &(int){0}, &(int){0}, &(int){0}))
            return asm__fail(c, ASM_EK_REGISTER, "base register expected");
        p = q;
        while (p < e && isspace((unsigned char)*p)) p++;
        if (p < e && *p == ',') {
            p++;
            while (p < e && isspace((unsigned char)*p)) p++;
            const char *q2 = p;
            int r2, k2;
            if (parse_reg(&q2, e, &r2, &k2, &(int){0}, &(int){0}, &(int){0})) {
                o->idxreg = r2;
                o->idx_valid = 1;
                o->idxkind = k2;
                p = q2;
                while (p < e && isspace((unsigned char)*p)) p++;
                if (p < e && *p == ',') {
                    const char *s = ++p;
                    while (p < e && isspace((unsigned char)*p)) p++;
                    s = p;
                    while (p < e && isalpha((unsigned char)*p)) p++;
                    if (!sfx_word(s, p, &o->idxsh_is_shift, &o->idxsh))
                        return snprintf(c->err, sizeof c->err, "bad index extend"),
                               ASM_ESYNTAX;
                    while (p < e && isspace((unsigned char)*p)) p++;
                    if (p < e && *p == '#') {
                        p++;
                        const char *db = p;
                        while (p < e && *p != ']' && !isspace((unsigned char)*p)) p++;
                        int64_t tv; int tk;
                        int rc2 = parse_imm(c, db, p, &tv, &tk);
                        if (rc2) return rc2;
                        o->idxsh_amt = (int)tv;
                        o->idxsh_amt_known = tk;
                    }
                }
            } else {
                const char *db = p;
                while (p < e && *p != ']') p++;
                if (p == e)
                    return snprintf(c->err, sizeof c->err, "missing ]"), ASM_ESYNTAX;
                int rc = parse_imm(c, db, p, &o->disp, &o->disp_known);
                if (rc) return rc;
            }
            while (p < e && isspace((unsigned char)*p)) p++;
        }
        if (p >= e || *p != ']')
            return snprintf(c->err, sizeof c->err, "missing ]"), ASM_ESYNTAX;
        p++;
        while (p < e && isspace((unsigned char)*p)) p++;
        if (p < e && *p == '!') {
            o->wb_pre = 1;
            p++;
        }
        while (p < e && isspace((unsigned char)*p)) p++;
        if (p < e && *p == ',') {
            int rc = parse_imm(c, p + 1, e, &o->disp, &o->disp_known);
            if (rc) return rc;
            o->wb_post = 1;
            p = e;
        }
        if (p != e)
            return snprintf(c->err, sizeof c->err, "junk after memory operand"),
                   ASM_ESYNTAX;
        return 0;
    }
    /* A shift or extend suffix standing alone, with an optional amount. A
     * bare condition word (csel's fourth operand) is stored in the same
     * way. */
    {
        const char *s = p;
        while (p < e && isalpha((unsigned char)*p)) p++;
        int condok = 0;
        if (p == e && s < p)
            condok = asma64_is_cond(s, p, &o->shx);
        if (condok) {
            o->t = A64_O_SFX;
            o->is_shift = 2;    /* condition */
            return 0;
        }
        if (p > s && sfx_word(s, p, &o->is_shift, &o->shx)) {
            const char *q = p;
            while (q < e && isspace((unsigned char)*q)) q++;
            if (q == e) {         /* bare suffix */
                o->t = A64_O_SFX;
                o->amt_known = 0;
                return 0;
            }
            if (*q == '#' || isdigit((unsigned char)*q)) {
                int64_t amt;
                int known;
                int rc = parse_imm(c, q, e, &amt, &known);
                if (rc) return rc;
                o->t = A64_O_SFX;
                o->shx = o->shx;
                o->amt = amt;
                o->amt_known = known;
                return 0;
            }
        }
        p = s;
    }
    /* register? */
    {
        const char *q = p;
        if (parse_reg(&q, e, &o->regn, &o->kind, &o->vbits, &o->vlanes, &o->vidx)) {
            if (q == e) {
                o->t = A64_O_REG;
                return 0;
            }
            /* "x1 ]" is a register followed by junk. A name that begins
             * like a register ("x1count") is not junk. It is BASIC's
             * variable. */
            if (!isalnum((unsigned char)*q) && *q != '_' && *q != '%' && *q != '$' &&
                *q != '(' && *q != '.' && *q != '`')
                return asm__fail(c, ASM_EK_SYNTAX, "junk after register");
        }
    }
    /* A bare word names something, such as a barrier option or a system
     * register. */
    {
        const char *q2 = o->b;
        while (q2 < e && (isalnum((unsigned char)*q2) || *q2 == '_'))
            q2++;
        if (q2 == e && !isdigit((unsigned char)o->b[0])) {
            o->t = A64_O_SFX;
            o->is_shift = 3;    /* named word */
            return 0;
        }
    }
    /* A floating-point literal. Keep its span and flag it. */
    {
        const char *q2 = o->b;
        if (q2 < e && *q2 == '#')
            q2++;
        if (q2 < e && (isdigit((unsigned char)*q2) ||
                       (*q2 == '-' && q2 + 1 < e && isdigit((unsigned char)q2[1])) ||
                       (*q2 == '.' && q2 + 1 < e && isdigit((unsigned char)q2[1])))) {
            for (const char *r = q2; r < e; r++)
                if (*r == '.') {
                    o->t = A64_O_IMM;
                    o->imm_known = 2;   /* float span */
                    o->imm = 0;
                    return 0;
                }
        }
    }
    o->t = A64_O_IMM;
    return parse_imm(c, o->b, e, &o->imm, &o->imm_known);
}

/* ---------------- helpers ---------------- */

static int w32(asm_ctx *c, uint32_t w, uint8_t *out, size_t cap, size_t *outlen)
{
    (void)c;
    if (cap < 4)
        return ASM_ESPACE;
    out[0] = (uint8_t)w;
    out[1] = (uint8_t)(w >> 8);
    out[2] = (uint8_t)(w >> 16);
    out[3] = (uint8_t)(w >> 24);
    *outlen = 4;
    return ASM_OK;
}

/* Encodes a logical immediate as N:immR:immS (see the ARM Architecture
 * Reference Manual). Returns 0 if the value cannot be encoded. */
static int logimm(uint64_t v, int sf, uint32_t *n, uint32_t *immr, uint32_t *imms)
{
    if (!sf) {
        if (v >> 32)
            return 0;
        v |= v << 32;               /* replicate for the search */
    }
    if (v == 0 || v == ~0ull)
        return 0;
    for (unsigned len = 1; len <= (sf ? 6u : 5u); len++) {
        unsigned esize = 1u << len;
        uint64_t mask = esize == 64 ? ~0ull : ((1ull << esize) - 1);
        uint64_t r = v & mask;
        int ok = 1;
        for (unsigned off = esize; off < 64; off += esize)
            if (((v >> off) & mask) != r)
                ok = 0;
        if (!ok)
            continue;
        if (r == 0 || r == mask)
            continue;
        unsigned w = __builtin_popcountll(r);
        uint64_t run = ((uint64_t)1 << w) - 1;
        /* Find the rotation that brings the run of ones to bit 0. This
         * includes a run that wraps round the element's top bit
         * (0x80000001). */
        unsigned rot = 0;
        for (; rot < esize; rot++)
            if ((rot ? (((r >> rot) | (r << (esize - rot))) & mask) : r) == run)
                break;
        if (rot == esize)
            continue;
        *n = len == 6;
        *imms = ((0x3fu & ~((1u << (len + 1)) - 1)) | (w - 1)) & 0x3f;
        *immr = (esize - rot) & (esize - 1);
        return 1;
    }
    return 0;
}

static int bad(asm_ctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->ekind = ASM_EK_SYNTAX;
    return ASM_ESYNTAX;
}

/* Says whether operand i is an X or W register. */
static int is_gpr(const a64opnd *o, int i)
{
    return o[i].t == A64_O_REG && (o[i].kind == RK_X || o[i].kind == RK_W);
}

/* the stack pointer, not the zero register that shares its number */
static int is_sp(const a64opnd *o)
{
    const char *b = o->b, *e = o->e;
    while (e > b && isspace((unsigned char)e[-1]))
        e--;
    return (e - b == 2 && !memcmp(b, "sp", 2)) || (e - b == 3 && !memcmp(b, "wsp", 3));
}

/* The sf bit (64-bit form), taken from operand 0. */
static int xfl(const a64opnd *o)
{
    return o[0].kind == RK_X;
}

/* The size in bytes of the register in operand 0. It is 8 for an X register
 * and 4 for a W register. */
static int xsc(const a64opnd *o)
{
    return o[0].kind == RK_X ? 8 : 4;
}

/* Turns an operand that names a place into a value. The places are a
 * branch's target, the targets of adr and adrp, and a literal load's. A
 * BASIC label is a bare word ("b loop"). The operand parser takes it for a
 * name, such as a barrier option or a condition. Where a place is wanted,
 * it is a value. Returns 0 or an error. */
static int target(asm_ctx *c, a64opnd *o)
{
    if (o->t == A64_O_IMM)
        return 0;
    /* A name that looks like a register ("b s2") is BASIC's variable here.
     * The ARM assembler's B takes any name as an expression. */
    if (o->t != A64_O_SFX && !(o->t == A64_O_REG && o->kind != RK_V && o->vidx < 0))
        return asm__fail(c, ASM_EK_SYNTAX, "a target address expected");
    o->t = A64_O_IMM;
    o->imm_known = 1;
    return parse_imm(c, o->b, o->e, &o->imm, &o->imm_known);
}

/* Works out a branch's offset. It is the target address less the branch's
 * own address. AArch64's PC is the address of the instruction. It is not 8
 * ahead as the ARM's is. The field is `bits` wide, signed, in words. A
 * target out of its reach is a value error. The ARM assembler always
 * reports this for B ("Bad address offset"). A target that is not known yet
 * (0, or a forward label in the corpus) gives an offset of 0. Returns 0 or
 * an error. */
static int branch_disp(asm_ctx *c, a64opnd *o, int bits, int64_t *disp)
{
    int rc = target(c, o);
    if (rc)
        return rc;
    /* A target at an unaligned P% (a label after data) is taken as the word
     * that the next instruction is aligned to. The ARM assembler's B takes
     * it in the same way (s.Assembler CASMB2: "-3 deals with unaligned
     * destination"). */
    *disp = o->imm_known == 1 ? ((asm_target(c, o->imm) + 3) & ~(int64_t)3) - (int64_t)c->addr : 0;
    int64_t reach = (int64_t)1 << (bits + 1);           /* bytes */
    if (*disp < -reach || *disp >= reach)
        asm__soft(c, ASM_EK_OFFSET | ASM_SOFT_ALWAYS, "branch out of range");
    return 0;
}

/* Works out a literal load's offset (ldr, ldrsw and prfm). The offset is
 * PC-relative, in words, and 19 bits signed. It comes from the target's
 * address. A target out of reach, or not word-aligned, is a value error. As
 * for the ARM assembler's LDR label, it is reported only with OPT's errors
 * bit. */
static int literal_disp(asm_ctx *c, a64opnd *o, int64_t *disp)
{
    int rc = target(c, o);
    if (rc)
        return rc;
    *disp = o->imm_known == 1 ? asm_target(c, o->imm) - (int64_t)c->addr : 0;
    if ((*disp & 3) || *disp < -(1 << 20) || *disp >= (1 << 20))
        asm__soft(c, ASM_EK_OFFSET, "literal out of range");
    return 0;
}

static uint32_t condbits_(const char *b, const char *e, int *ok);

#include <stdarg.h>

/* ---------------- the dispatcher ---------------- */

int asma64_dispatch(asm_ctx *c, const char *mn, const char *mne,
                    a64opnd *o, int n, uint8_t *out, size_t cap, size_t *outlen)
{
    char name[32];
    size_t nl = (size_t)(mne - mn);
    if (nl >= sizeof name)
        return bad(c, "mnemonic too long");
    memcpy(name, mn, nl);
    name[nl] = 0;

    /* ---- literal loads: ldr, ldrsw and prfm with a target and no [...] ---- */
    if ((!strcmp(name, "ldr") || !strcmp(name, "ldrsw") || !strcmp(name, "prfm")) &&
        n == 2 && o[1].t != A64_O_MEM && o[1].t != A64_O_REG) {
        uint32_t opc;
        uint32_t rt = (uint32_t)o[0].regn;
        if (!strcmp(name, "prfm")) {
            int op = asma64_prfop(&o[0]);
            if (op < 0)
                return bad(c, "bad prefetch operation");
            opc = 0xD8000000u;
            rt = (uint32_t)op;
        } else if (o[0].t != A64_O_REG)
            return asm__fail(c, ASM_EK_REGISTER, "register expected");
        else if (!strcmp(name, "ldrsw")) {
            if (o[0].kind != RK_X)
                return asm__fail(c, ASM_EK_REGISTER, "x register expected");
            opc = 0x98000000u;
        } else if (o[0].kind == RK_W) opc = 0x18000000u;
        else if (o[0].kind == RK_X) opc = 0x58000000u;
        else if (o[0].kind == RK_S) opc = 0x1C000000u;
        else if (o[0].kind == RK_D) opc = 0x5C000000u;
        else if (o[0].kind == RK_Q) opc = 0x9C000000u;
        else
            return asm__fail(c, ASM_EK_REGISTER, "bad register");
        int64_t d;
        int rc = literal_disp(c, &o[1], &d);
        if (rc)
            return rc;
        return w32(c, opc | F(d >> 2, 5, 19) | F(rt, 0, 5), out, cap, outlen);
    }

    /* ---- ext: the byte index is an immediate that the table does not vary ---- */
    if (!strcmp(name, "ext") && n == 4 && o[0].t == A64_O_REG && o[0].kind == RK_V &&
        o[1].t == A64_O_REG && o[2].t == A64_O_REG && o[3].t == A64_O_IMM) {
        int q = o[0].vbits * o[0].vlanes == 128;
        if (o[0].vbits != 8 || (o[0].vlanes != 8 && o[0].vlanes != 16))
            return bad(c, "ext wants 8b or 16b");
        int64_t idx = o[3].imm_known == 1 ? o[3].imm : 0;
        if (idx < 0 || idx >= (q ? 16 : 8))
            asm__soft(c, ASM_EK_IMMEDIATE, "ext index out of range");
        return w32(c, 0x2E000000u | F(q, 30, 1) | F(o[2].regn, 16, 5) | F(idx, 11, 4) |
                   F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5), out, cap, outlen);
    }

    /* ---- vector shifts by an immediate. immh:immb is esize + shift for a
     * left shift and 2*esize - shift for a right shift. The table holds one
     * of each. ---- */
    {
        static const struct { const char *n; uint32_t op; int right; } vs[] = {
            { "shl", 0x0F005400u, 0 }, { "sli", 0x2F005400u, 0 },
            { "sqshl", 0x0F007400u, 0 }, { "uqshl", 0x2F007400u, 0 },
            { "sshr", 0x0F000400u, 1 }, { "ushr", 0x2F000400u, 1 },
            { "ssra", 0x0F001400u, 1 }, { "usra", 0x2F001400u, 1 },
            { "srshr", 0x0F002400u, 1 }, { "urshr", 0x2F002400u, 1 },
            { "srsra", 0x0F003400u, 1 }, { "ursra", 0x2F003400u, 1 },
            { "sri", 0x2F004400u, 1 },
        };
        for (unsigned i = 0; i < sizeof vs / sizeof vs[0]; i++) {
            if (strcmp(name, vs[i].n) || n != 3 || o[0].t != A64_O_REG || o[0].kind != RK_V ||
                o[1].t != A64_O_REG || o[1].kind != RK_V || o[2].t != A64_O_IMM ||
                !o[0].vlanes || o[0].vbits != o[1].vbits || o[0].vlanes != o[1].vlanes)
                continue;
            int es = o[0].vbits, q = es * o[0].vlanes == 128;
            if (es > 64 || (es == 64 && !q))
                return bad(c, "%s: bad arrangement", name);
            int64_t sh = o[2].imm_known == 1 ? o[2].imm : 0;
            if (vs[i].right ? (sh < 1 || sh > es) : (sh < 0 || sh >= es))
                asm__soft(c, ASM_EK_SHIFT, "shift out of range");
            int64_t f = vs[i].right ? 2 * es - sh : es + sh;
            return w32(c, vs[i].op | F(q, 30, 1) | F(f, 16, 7) | F(o[1].regn, 5, 5) |
                       F(o[0].regn, 0, 5), out, cap, outlen);
        }
    }

    /* ---- movi, mvni, and orr/bic with an immediate, to a vector. This works
     * out the byte and cmode from the element size and shift. ---- */
    if ((!strcmp(name, "movi") || !strcmp(name, "mvni") || !strcmp(name, "orr") ||
         !strcmp(name, "bic")) && (n == 2 || n == 3) && o[0].t == A64_O_REG &&
        o[0].kind == RK_V && o[0].vlanes && o[1].t == A64_O_IMM &&
        (n == 2 || (o[2].t == A64_O_SFX && o[2].is_shift == 1))) {
        int es = o[0].vbits, q = es * o[0].vlanes == 128;
        int logic = name[0] == 'o' || name[0] == 'b';            /* orr, bic */
        int op = name[1] == 'v' || name[0] == 'b';               /* mvni, bic */
        int msl = n == 3 && o[2].shx == 4;
        int64_t amt = n == 3 ? (o[2].amt_known ? o[2].amt : 0) : 0;
        int64_t v = o[1].imm_known == 1 ? o[1].imm : 0;
        uint32_t cmode, imm8 = (uint32_t)v & 0xFF;
        if (n == 3 && o[2].shx != SHK_LSL && !msl)
            return bad(c, "%s: lsl or msl", name);
        if (es == 8 && !logic && !op && !amt)
            cmode = 14;
        else if (es == 64 && !strcmp(name, "movi") && !amt) {
            /* each byte is 00 or FF, and gives one bit of imm8 */
            uint64_t u = (uint64_t)v;
            imm8 = 0;
            for (int b = 0; b < 8; b++) {
                unsigned by = (unsigned)(u >> 8 * b) & 0xFF;
                if (by != 0 && by != 0xFF)
                    asm__soft(c, ASM_EK_IMMEDIATE, "movi: each byte 0 or &FF");
                imm8 |= (by != 0) << b;
            }
            cmode = 14;
            op = 1;
        } else if (es == 16 && !msl && (amt == 0 || amt == 8))
            cmode = 8 | (uint32_t)(amt / 8) << 1 | (uint32_t)logic;
        else if (es == 32 && !msl && (amt == 0 || amt == 8 || amt == 16 || amt == 24))
            cmode = (uint32_t)(amt / 8) << 1 | (uint32_t)logic;
        else if (es == 32 && msl && !logic && (amt == 8 || amt == 16))
            cmode = 12 | (uint32_t)(amt == 16);
        else
            return bad(c, "%s: no such immediate form", name);
        if (es != 64 && (v < 0 || v > 255))
            asm__soft(c, ASM_EK_IMMEDIATE, "%s: immediate out of range", name);
        return w32(c, 0x0F000400u | F(q, 30, 1) | F(op, 29, 1) | F(imm8 >> 5, 16, 3) |
                   F(cmode, 12, 4) | F(imm8, 5, 5) | F(o[0].regn, 0, 5), out, cap, outlen);
    }

    /* Vector mnemonics that share names with integer operations go straight
     * to asma64_mem_fp_vec() in asma64_mv.c, and to the generated table. */
    for (int i = 0; i < n; i++)
        if (o[i].t == A64_O_REG && o[i].kind == RK_V)
            return asma64_mem_fp_vec(c, name, name, strchr(name, '.') ? strchr(name, '.') + 1 : "", o, n, out, cap, outlen);
    const char *dot = strchr(name, '.');
    char base[24];
    size_t bl = dot ? (size_t)(dot - name) : nl;
    if (bl >= sizeof base)
        return bad(c, "mnemonic too long");
    memcpy(base, name, bl);
    base[bl] = 0;
    const char *sfx = dot ? dot + 1 : "";

    uint32_t w;

    /* ---- branches ---- */
    if (!strcmp(base, "b") && !dot) {
        if (n != 1)
            return bad(c, "b wants a target");
        int64_t d;
        int rc = branch_disp(c, &o[0], 26, &d);
        if (rc)
            return rc;
        w = 0x14000000u | F(d >> 2, 0, 26);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "bl")) {
        if (n != 1)
            return bad(c, "bl wants a target");
        int64_t d;
        int rc = branch_disp(c, &o[0], 26, &d);
        if (rc)
            return rc;
        w = 0x94000000u | F(d >> 2, 0, 26);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "b")) {   /* b.cond: suffix */
        int cond, ok;
        cond = condbits_(sfx, sfx + strlen(sfx), &ok);
        if (!ok)
            return asm__fail(c, ASM_EK_MNEMONIC, "bad condition %s", name);
        if (n != 1)
            return bad(c, "b.cond wants a target");
        int64_t d;
        int rc = branch_disp(c, &o[0], 19, &d);
        if (rc)
            return rc;
        w = 0x54000000u | F(d >> 2, 5, 19) | F(cond, 0, 4);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "cbz") || !strcmp(base, "cbnz")) {
        if (n != 2 || !is_gpr(o, 0))
            return bad(c, "%s wants (reg, target)", base);
        int64_t d;
        int rc = branch_disp(c, &o[1], 19, &d);
        if (rc)
            return rc;
        w = (o[0].kind == RK_X ? 0xB4000000u : 0x34000000u) |
            (base[2] == 'n' ? 0x01000000u : 0) |
            F(d >> 2, 5, 19) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "tbz") || !strcmp(base, "tbnz")) {
        if (n != 3 || !is_gpr(o, 0))
            return bad(c, "%s wants (reg, bit, target)", base);
        if (o[1].t != A64_O_IMM || !o[1].imm_known)
            return bad(c, "bit number must be known");
        int64_t b = o[1].imm;
        if (b < 0 || b > 63 || (o[0].kind == RK_W && b > 31))
            return asm__fail(c, ASM_EK_IMMEDIATE, "bit out of range");
        int64_t d;
        int rc = branch_disp(c, &o[2], 14, &d);
        if (rc)
            return rc;
        w = 0x36000000u | F(b >> 5, 31, 1) | (base[2] == 'n' ? 0x01000000u : 0) |
            F(b & 31, 19, 5) | F(d >> 2, 5, 14) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "eret") && !dot && n == 0)
        return w32(c, 0xD69F03E0u, out, cap, outlen);
    if ((!strcmp(base, "ret") || !strcmp(base, "br") || !strcmp(base, "blr")) && !dot) {
        uint32_t rn = 30;               /* ret's default: the link register */
        if (n == 1) {
            if (!is_gpr(o, 0) || o[0].kind != RK_X || is_sp(&o[0]))
                return asm__fail(c, ASM_EK_REGISTER, "%s wants an x register", base);
            rn = (uint32_t)o[0].regn;
        } else if (n != 0 || strcmp(base, "ret"))
            return bad(c, "%s wants a register", base);
        w = (base[0] == 'r' ? 0xD65F0000u : base[1] == 'r' ? 0xD61F0000u : 0xD63F0000u) |
            F(rn, 5, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "mov") && !dot && n == 2 && is_gpr(o, 0) && is_gpr(o, 1)) {
        if (o[0].kind != o[1].kind)
            return asm__fail(c, ASM_EK_REGISTER, "register size mismatch");
        int sf = o[0].kind == RK_X;
        if (is_sp(&o[0]) || is_sp(&o[1]))   /* add rd, rn, #0 */
            w = (sf ? 0x91000000u : 0x11000000u) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
        else                                /* orr rd, zr, rm */
            w = (sf ? 0xAA0003E0u : 0x2A0003E0u) | F(o[1].regn, 16, 5) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "adr") || !strcmp(base, "adrp")) {
        if (n != 2 || !is_gpr(o, 0))
            return bad(c, "%s wants (reg, label)", base);
        int rc = target(c, &o[1]);
        if (rc)
            return rc;
        int64_t tgt = o[1].imm_known == 1 ? asm_target(c, o[1].imm) : (int64_t)c->addr;
        int64_t d;
        if (!strcmp(base, "adrp")) {
            /* The offset is the target's 4K page less this instruction's
             * page, counted in pages. Any address in the page will do, as
             * it does for MC's adrp x0, sym. */
            d = (tgt >> 12) - (int64_t)(c->addr >> 12);
            if (d < -(1 << 20) || d >= (1 << 20))
                asm__soft(c, ASM_EK_OFFSET, "adrp out of range");
            w = 0x90000000u;
        } else {
            d = tgt - (int64_t)c->addr;
            if (d < -(1 << 20) || d >= (1 << 20))
                asm__soft(c, ASM_EK_OFFSET, "adr out of range");
            w = 0x10000000u;
        }
        /* immlo is the offset's bits 1:0, at 30:29. immhi is bits 20:2, at 23:5. */
        w |= F(d, 29, 2) | F(d >> 2, 5, 19) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }

    /* ---- add/sub immediate ---- */
    {
        static const struct { const char *n; int op, S; int rd0; } t[] = {
            { "add", 0, 0, 0 }, { "adds", 0, 1, 0 }, { "sub", 1, 0, 0 },
            { "subs", 1, 1, 0 }, { "cmp", 1, 1, 1 }, { "cmn", 0, 1, 1 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(base, t[i].n))
                continue;
            int rn0 = t[i].rd0 ? 0 : (n >= 2 && is_gpr(o, 0) && is_gpr(o, 1) ? 1 : -1);
            if (t[i].rd0) {
                if (n != 2 || !is_gpr(o, 0) || o[1].t != A64_O_IMM)
                    goto notimm;
            } else {
                if (n < 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || o[2].t != A64_O_IMM ||
                    o[0].kind != o[1].kind)
                    goto notimm;
            }
            {
                int sf = xfl(o);
                int64_t imm = t[i].rd0 ? o[1].imm : o[2].imm;
                int known = t[i].rd0 ? o[1].imm_known : o[2].imm_known;
                int sh = 0;
                int nops = t[i].rd0 ? n : n;
                if (!t[i].rd0 && n == 4) {
                    if (o[3].t != A64_O_SFX || !o[3].is_shift || o[3].shx != SHK_LSL)
                        return bad(c, "bad shift on immediate");
                    /* lsl #0 or #12 */
                    if (o[3].amt_known && o[3].amt != 0 && o[3].amt != 12)
                        asm__soft(c, ASM_EK_SHIFT, "shift must be #0 or #12");
                    sh = !o[3].amt_known || o[3].amt != 0;
                }
                int op = t[i].op;
                if (known != 1)
                    imm = 0;            /* a name in the corpus that is not known yet */
                else if (!sh) {
                    /* This follows MC. #-n is the other operation's #n.
                     * A multiple of 4096 up to &FFF000 is #n, lsl #12. */
                    if (imm < 0 && imm >= -0xFFF000) {
                        imm = -imm;
                        op ^= 1;
                    }
                    if (imm > 4095 && !(imm & 0xFFF) && imm <= 0xFFF000) {
                        imm >>= 12;
                        sh = 1;
                    }
                }
                if (imm < 0 || imm > 4095)
                    asm__soft(c, ASM_EK_IMMEDIATE, "immediate out of range");
                w = (sf ? 0x91000000u : 0x11000000u) | F(op, 30, 1) |
                    F(t[i].S, 29, 1) | F(sh, 22, 1) | F(imm, 10, 12);
                if (t[i].rd0)
                    w |= F(o[0].regn, 5, 5) | F(31, 0, 5);      /* Rd = xzr */
                else
                    w |= F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                (void)nops;
                return w32(c, w, out, cap, outlen);
            }
        notimm:;
        }
    }

    /* ---- add/sub shifted / extended register ---- */
    {
        static const struct { const char *n; int op, S; int rd0; } t[] = {
            { "add", 0, 0, 0 }, { "adds", 0, 1, 0 }, { "sub", 1, 0, 0 },
            { "subs", 1, 1, 0 }, { "cmp", 1, 1, 1 }, { "cmn", 0, 1, 1 },
            { "neg", 1, 0, 2 }, { "negs", 1, 1, 2 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(base, t[i].n))
                continue;
            int rd, rn, rmi;
            if (t[i].rd0 == 1) {
                int suffix = n == 3 && o[2].t == A64_O_SFX;
                if ((n != 2 && !suffix) || !is_gpr(o, 0) || !is_gpr(o, 1))
                    return bad(c, "%s wants two registers", base);
                rd = 31; rn = 0; rmi = 1;   /* Rd = xzr */
                if (o[0].kind != o[1].kind && !(suffix && !o[2].is_shift))
                    return asm__fail(c, ASM_EK_REGISTER, "register size mismatch");
            } else if (t[i].rd0 == 2) {
                if (n < 2 || !is_gpr(o, 0) || !is_gpr(o, 1))
                    return bad(c, "%s wants two registers", base);
                rd = 0; rn = 31; rmi = 1;   /* Rn = xzr */
                if (o[0].kind != o[1].kind)
                    return bad(c, "register size mismatch");
            } else {
                if (n < 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || !is_gpr(o, 2))
                    return bad(c, "%s wants three registers", base);
                int extend = n > 3 && o[3].t == A64_O_SFX && !o[3].is_shift;
                if (o[0].kind != o[1].kind || (o[0].kind != o[2].kind && !extend))
                    return asm__fail(c, ASM_EK_REGISTER, "register size mismatch");
                rd = 0; rn = 1; rmi = 2;
            }
            /* With sp as Rd or Rn, use the extended-register form, uxtx
             * (uxtw for W). MC writes "add x0, sp, x1" this way. The shifted
             * form reads register 31 as the zero register. */
            if (t[i].rd0 != 2 && n == rmi + 1 && (is_sp(&o[rn]) || (t[i].rd0 == 0 && is_sp(&o[rd])))) {
                int sf = o[rn].kind == RK_X;
                w = (sf ? 0x8B200000u : 0x0B200000u) | F(t[i].op, 30, 1) | F(t[i].S, 29, 1) |
                    F(o[rmi].regn, 16, 5) | F(sf ? 3 : 2, 13, 3) | F(o[rn].regn, 5, 5) |
                    F(t[i].rd0 == 1 ? 31 : o[rd].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            /* neg's Rn is xzr. It is not an operand. */
            int sf = (rn == 31 ? o[rd] : o[rn]).kind == RK_X;
            w = (sf ? 0x8B000000u : 0x0B000000u) | F(t[i].op, 30, 1) |
                F(t[i].S, 29, 1);
            w |= F(rn == 31 ? 31 : o[rn].regn, 5, 5) | F(t[i].rd0 == 1 ? 31 : o[rd].regn, 0, 5) |
                 F(o[rmi].regn, 16, 5);
            if (n > rmi + 1 && o[rmi + 1].t == A64_O_SFX) {
                const a64opnd *s = &o[rmi + 1];
                if (s->is_shift) {
                    if (n != rmi + 2)
                        return bad(c, "bad operands");
                    int64_t amt = s->amt_known ? s->amt : 0;
                    if (sf == 0 && s->shx == SHK_ROR)
                        return bad(c, "ror needs 64-bit");
                    if (s->amt_known && (amt < 0 || amt >= (sf ? 64 : 32)))
                        return bad(c, "shift out of range");
                    w |= F(s->shx == SHK_LSL ? 0 : s->shx == SHK_LSR ? 1 :
                           s->shx == SHK_ASR ? 2 : 3, 22, 2) | F(amt, 10, 6);
                } else {
                    /* extended register */
                    if (o[rmi].kind == RK_X && s->shx != EXK_UXTX &&
                        s->shx != EXK_SXTX && s->shx != EXK_SXTW && s->shx != EXK_UXTW &&
                        s->shx != EXK_SXTH && s->shx != EXK_UXTH &&
                        s->shx != EXK_SXTB && s->shx != EXK_UXTB)
                        return bad(c, "bad extend");
                    int64_t amt = s->amt_known ? s->amt : 0;
                    if (n != rmi + 2 && !(n == rmi + 2))
                        return bad(c, "bad operands");
                    if (s->amt_known && (amt < 0 || amt > 4))
                        return bad(c, "extend shift out of range");
                    /* the extended form: bit 21, the option at 15:13 */
                    w |= 0x00200000u | F(s->shx, 13, 3) | F(amt, 10, 3);
                }
            } else if (n != rmi + 1)
                return bad(c, "bad operand count");
            return w32(c, w, out, cap, outlen);
        }
    }

    /* ---- logical immediate ---- */
    {
        static const struct { const char *n; int opc; int rd0; int not_; } t[] = {
            { "and", 0, 0, 0 }, { "orr", 1, 0, 0 }, { "eor", 2, 0, 0 },
            { "ands", 3, 0, 0 }, { "tst", 3, 1, 0 },
            { "bic", 0, 0, 1 }, { "orn", 1, 0, 1 }, { "eon", 2, 0, 1 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(base, t[i].n))
                continue;
            int rn0, rdi, immi;
            if (t[i].rd0) {
                if (n != 2 || !is_gpr(o, 0) || o[1].t != A64_O_IMM)
                    goto notlog;
                rn0 = 0; rdi = -1; immi = 1;
            } else {
                if (n != 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || o[2].t != A64_O_IMM)
                    goto notlog;
                if (o[0].kind != o[1].kind)
                    goto notlog;
                rn0 = 1; rdi = 0; immi = 2;
            }
            {
                int sf = xfl(o);
                int64_t imm = o[immi].imm;
                if (o[immi].imm_known != 1)
                    imm = 1;            /* a name in the corpus that is not known yet */
                if (t[i].not_)
                    imm = ~imm;
                if (!sf && imm >= -0x80000000LL && imm <= 0xFFFFFFFFLL)
                    imm &= 0xFFFFFFFF;
                uint32_t nn = 0, ir = 0, is = 0;
                if (!logimm((uint64_t)imm, sf, &nn, &ir, &is))
                    asm__soft(c, ASM_EK_IMMEDIATE, "immediate not a bitmask");
                w = (sf ? 0x92000000u : 0x12000000u) | F(t[i].opc, 29, 2) |
                    F(nn, 22, 1) | F(ir, 16, 6) | F(is, 10, 6) |
                    F(o[rn0].regn, 5, 5);
                if (rdi >= 0)
                    w |= F(o[rdi].regn, 0, 5);
                else
                    w |= F(31, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
        notlog:;
        }
    }

    /* ---- logical shifted register (and aliases) ---- */
    {
        static const struct { const char *n; int opc, N; int rd0; int rn31; } t[] = {
            { "and", 0, 0, 0, 0 }, { "bic", 0, 1, 0, 0 },
            { "orr", 1, 0, 0, 0 }, { "orn", 1, 1, 0, 0 },
            { "eor", 2, 0, 0, 0 }, { "eon", 2, 1, 0, 0 },
            { "ands", 3, 0, 0, 0 }, { "bics", 3, 1, 0, 0 },
            { "tst", 3, 0, 1, 0 }, { "mvn", 1, 1, -1, 31 },
            { "mov", 1, 0, 0, 31 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(base, t[i].n))
                continue;
            if (!strcmp(base, "mov") && n == 2 && o[1].t == A64_O_IMM)
                continue;   /* the immediate form is handled below */
            int rd, rn, rm;
            if (t[i].rd0 == 1) {         /* tst Rn, Rm */
                if (n != 2 || !is_gpr(o, 0) || !is_gpr(o, 1))
                    return bad(c, "tst wants two registers");
                rd = -1; rn = 0; rm = 1;
                if (o[0].kind != o[1].kind)
                    return bad(c, "register size mismatch");
            } else if (t[i].rd0 == -1) { /* mvn Rd, Rm */
                if (n != 2 || !is_gpr(o, 0) || !is_gpr(o, 1))
                    return bad(c, "mvn wants two registers");
                rd = 0; rn = 31; rm = 1;
                if (o[0].kind != o[1].kind)
                    return bad(c, "register size mismatch");
            } else {
                if (n < 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || !is_gpr(o, 2))
                    return bad(c, "%s wants three registers", base);
                if (o[0].kind != o[1].kind || o[0].kind != o[2].kind)
                    return bad(c, "register size mismatch");
                rd = 0; rn = 1; rm = 2;
            }
            int sf = o[rd >= 0 ? rd : rn].kind == RK_X;
            w = (sf ? 0x8A000000u : 0x0A000000u) | F(t[i].opc, 29, 2) |
                F(t[i].N, 21, 1) | F(o[rm].regn, 16, 5) | F(t[i].rn31 ? 31 : o[rn].regn, 5, 5);
            if (rd >= 0)
                w |= F(o[rd].regn, 0, 5);
            else
                w |= F(31, 0, 5);
            if (n > rm + 1 && o[rm + 1].t == A64_O_SFX) {
                const a64opnd *s = &o[rm + 1];
                if (!s->is_shift || n != rm + 2)
                    return bad(c, "bad shift");
                int64_t amt = s->amt_known ? s->amt : 0;
                if (s->amt_known && (amt < 0 || amt >= (sf ? 64 : 32)))
                    return bad(c, "shift out of range");
                w |= F(s->shx == SHK_LSL ? 0 : s->shx == SHK_LSR ? 1 :
                       s->shx == SHK_ASR ? 2 : 3, 22, 2) | F(amt, 10, 6);
            } else if (n != rm + 1)
                return bad(c, "bad operand count");
            return w32(c, w, out, cap, outlen);
        }
    }

    /* ---- move wide ---- */
    if (!strcmp(base, "movz") || !strcmp(base, "movn") || !strcmp(base, "movk")) {
        if (n < 2 || n > 3 || !is_gpr(o, 0) || o[1].t != A64_O_IMM)
            return bad(c, "%s wants (reg, imm[, lsl #n])", base);
        int sf = xfl(o);
        int64_t imm = o[1].imm;
        if (o[1].imm_known && (imm < 0 || imm > 0xFFFF))
            asm__soft(c, ASM_EK_IMMEDIATE, "immediate out of range");
        int64_t hw = 0;
        if (n == 3) {
            if (o[2].t != A64_O_SFX || !o[2].is_shift || o[2].shx != SHK_LSL)
                return bad(c, "only lsl allowed");
            hw = o[2].amt_known ? o[2].amt : 0;
            if (o[2].amt_known && ((hw & 15) || hw < 0 || hw > (sf ? 48 : 16)))
                return asm__fail(c, ASM_EK_SHIFT, "shift must be 0/16/32/48");
            hw >>= 4;
        }
        w = (sf ? 0x92800000u : 0x12800000u) |
            F(!strcmp(base, "movn") ? 0 : !strcmp(base, "movz") ? 2 : 3, 29, 2) |
            F(hw, 21, 2) | F(imm, 5, 16) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    /* mov reg,imm follows MC's order of preference: movz with hw 0, movn
     * with hw 0, movz with any hw, movn with any hw, then orr-immediate. */
    if (!strcmp(base, "mov") && n == 2 && is_gpr(o, 0) && o[1].t == A64_O_IMM) {
        int sf = xfl(o);
        int64_t v = o[1].imm;
        if (!sf && o[1].imm_known && v >= -0x80000000LL && v <= 0xFFFFFFFFLL) {
            /* A W register takes the value's 32 bits, however BASIC signs
             * them. The order is movz (hw 0, then 1), movn (hw 0, then 1),
             * then a bitmask, as MC picks. */
            uint32_t x = (uint32_t)v, nx = ~x;
            if (!(x >> 16))
                w = 0x52800000u | F(x, 5, 16);
            else if (!(x & 0xFFFF))
                w = 0x52A00000u | F(x >> 16, 5, 16);
            else if (!(nx >> 16))
                w = 0x12800000u | F(nx, 5, 16);
            else if (!(nx & 0xFFFF))
                w = 0x12A00000u | F(nx >> 16, 5, 16);
            else {
                uint32_t nn, ir, is;
                if (logimm(x, 0, &nn, &ir, &is))
                    w = 0x32000000u | F(ir, 16, 6) | F(is, 10, 6) | F(31, 5, 5);
                else {
                    asm__soft(c, ASM_EK_IMMEDIATE, "mov immediate not encodable");
                    w = 0x52800000u | F(x, 5, 16);
                }
            }
            return w32(c, w | F(o[0].regn, 0, 5), out, cap, outlen);
        }
        uint64_t u = (uint64_t)v;
        int64_t lim = sf ? -1 : 0xFFFFFFFF;
        uint64_t notv = ~(uint64_t)v;
        if (!sf)
            notv &= 0xFFFFFFFF;
        if (!o[1].imm_known)
            u = 0;
        if (o[1].imm_known) {
            if (v >= 0 && v <= 0xFFFF) {
                w = (sf ? 0xD2800000u : 0x52800000u) |
                    F(v, 5, 16) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            if (v == lim || (v < 0 && (~notv) <= 0 && notv <= 0xFFFF)) {
                /* v is -1, or its upper bits are all ones and its low 16 bits are the immediate */
                if (v < 0 && (uint64_t)~v <= (sf ? ~0ull >> 48 : 0xFFFFull) && notv >= ~(uint64_t)(sf ? 0xFFFFull << 48 : 0)) {
                    /* not encodable by movn with hw 0 */
                }
            }
            if (v < 0 && notv <= 0xFFFF) {
                w = (sf ? 0x92800000u : 0x12800000u) |
                    F(notv & 0xFFFF, 5, 16) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            if (v >= 0 && !(u & ~0xFFFFull)) {
                w = (sf ? 0xD2800000u : 0x52800000u) |
                    F(u, 5, 16) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            /* a single 16-bit chunk at some hw */
            for (int hw = 3; hw >= 1; hw--) {
                uint64_t chunk = (u >> (16 * hw)) & 0xFFFF;
                if (chunk && !(u & ~(0xFFFFull << (16 * hw)))) {
                    w = (sf ? 0x92800000u : 0x12800000u) | F(2, 29, 2) |
                        F(hw, 21, 2) | F(chunk, 5, 16) | F(o[0].regn, 0, 5);
                    return w32(c, w, out, cap, outlen);
                }
            }
            for (int hw = 3; hw >= 0; hw--) {
                uint64_t chunk = (notv >> (16 * hw)) & 0xFFFF;
                if (chunk && !(notv & ~(0xFFFFull << (16 * hw)))) {
                    w = (sf ? 0x92800000u : 0x12800000u) |
                        F(hw, 21, 2) | F(chunk, 5, 16) | F(o[0].regn, 0, 5);
                    return w32(c, w, out, cap, outlen);
                }
            }
        } else {
            w = (sf ? 0xD2800000u : 0x52800000u) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        (void)lim;
        {
            uint32_t nn, ir, is;
            if (logimm((uint64_t)v & (sf ? ~0ull : 0xFFFFFFFFull), sf, &nn, &ir, &is)) {
                w = (sf ? 0xB2000000u : 0x32000000u) | F(nn, 22, 1) |
                    F(ir, 16, 6) | F(is, 10, 6) | F(31, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
        }
        /* No single instruction makes this value. It is the ARM assembler's
         * "Bad immediate constant", reported only with OPT's errors bit. */
        asm__soft(c, ASM_EK_IMMEDIATE, "mov immediate not encodable");
        w = (sf ? 0xD2800000u : 0x52800000u) | F(v, 5, 16) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }

    /* ---- multiply ---- */
    {
        static const struct { const char *n; uint32_t opcmask; int nargs; int ra0; } t[] = {
            { "madd", 0x1B000000u, 4, 0 }, { "msub", 0x1B008000u, 4, 0 },
            { "mul", 0x1B007C00u, 3, -1 }, { "mneg", 0x1B00FC00u, 3, -1 },
            { "smull", 0x9B207C00u, 3, -1 }, { "umull", 0x9BA07C00u, 3, -1 },
            { "smnegl", 0x9B20FC00u, 3, -1 }, { "umnegl", 0x9BA0FC00u, 3, -1 },
            { "smaddl", 0x9B200000u, 4, 0 }, { "umaddl", 0x9BA00000u, 4, 0 },
            { "smsubl", 0x9B208000u, 4, 0 }, { "umsubl", 0x9BA08000u, 4, 0 },
            { "smulh", 0x9B407C00u, 3, -1 }, { "umulh", 0x9BC07C00u, 3, -1 },
            { "sdiv", 0x1AC00C00u, 3, -2 },
            { "udiv", 0x1AC00800u, 3, -2 },
            { "lslv", 0x1AC02000u, 3, -2 }, { "lsrv", 0x1AC02400u, 3, -2 },
            { "asrv", 0x1AC02800u, 3, -2 }, { "rorv", 0x1AC02C00u, 3, -2 },
            { "lsl", 0x1AC02000u, 3, -2 }, { "lsr", 0x1AC02400u, 3, -2 },
            { "asr", 0x1AC02800u, 3, -2 }, { "ror", 0x1AC02C00u, 3, -2 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(base, t[i].n))
                continue;
            if (t[i].ra0 == -2) {
                /* The register-register form with two sources. lsl, lsr,
                 * asr and ror use it only if the third operand is a register.
                 * Otherwise they are bitfield aliases. */
                if (n != 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || !is_gpr(o, 2))
                    goto nextmul;
                int sf = xfl(o);
                if (!strcmp(base, "ror") || !strcmp(base, "lsl") ||
                    !strcmp(base, "lsr") || !strcmp(base, "asr")) {
                    if (o[0].kind != o[1].kind || o[0].kind != o[2].kind)
                        goto nextmul;
                }
                w = t[i].opcmask | (sf ? 0x80000000u : 0) |
                    F(o[2].regn, 16, 5) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            if (t[i].nargs == 4) {
                if (n != 4)
                    return bad(c, "%s wants four operands", base);
                if (!is_gpr(o, 0) || !is_gpr(o, 1) || !is_gpr(o, 2) || !is_gpr(o, 3))
                    return bad(c, "%s wants registers", base);
                int sf = xfl(o);
                w = t[i].opcmask | F(o[2].regn, 16, 5) | F(o[3].regn, 10, 5) |
                    F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                if (sf)
                    w |= 0x80000000u;
                if (!strcmp(base, "smull") || !strcmp(base, "umull") ||
                    !strcmp(base, "smulh") || !strcmp(base, "umulh") ||
                    !strcmp(base, "smaddl") || !strcmp(base, "umsubl") ||
                    !strcmp(base, "umaal"))
                    ;
                return w32(c, w, out, cap, outlen);
            }
            /* the three-operand madd form, with Ra = 31 */
            if (n != 3)
                return bad(c, "%s wants three registers", base);
            {
                int sf = xfl(o);
                uint32_t opc = t[i].opcmask;
                if (t[i].ra0 == -1 && (opc & 0x1F007C00u) == 0x1B007C00u)
                    ; /* the mul, mneg and smull variants already carry Ra = 31 */
                w = opc | F(o[2].regn, 16, 5) | F(31, 10, 5) |
                    F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                if (sf)
                    w |= 0x80000000u;
                return w32(c, w, out, cap, outlen);
            }
        nextmul:;
        }
    }

    /* ---- bitfield ---- */
    {
        static const struct { const char *n; uint32_t opc; } t[] = {
            { "sbfm", 0x13000000u }, { "bfm", 0x33000000u },
            { "ubfm", 0x53000000u },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
            if (!strcmp(base, t[i].n)) {
                if (n != 4 || !is_gpr(o, 0) || !is_gpr(o, 1) ||
                    o[2].t != A64_O_IMM || o[3].t != A64_O_IMM)
                    return bad(c, "%s wants (rd, rn, #i, #j)", base);
                int sf = xfl(o);
                w = t[i].opc | F(sf, 31, 1) | F(sf, 22, 1) |
                    F(o[2].imm, 16, 6) | F(o[3].imm, 10, 6) |
                    F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
    }
    /* The bitfield aliases are sbfx, ubfx, bfxil, bfi, sbfiz and ubfiz, the
     * sxt and uxt forms, and the shifts. */
    {
        static const struct { const char *n; int isb; } t[] = {
            { "sbfx", 1 }, { "ubfx", 0 }, { "bfxil", 2 }, { "bfi", 2 },
            { "sbfiz", 1 }, { "ubfiz", 0 }, { "lsl", 0 }, { "lsr", 0 },
            { "asr", 1 }, { "ror", 2 }, { "sxtb", 1 }, { "sxth", 1 },
            { "sxtw", 1 }, { "uxtb", 0 }, { "uxth", 0 }, { "uxtw", 0 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(base, t[i].n))
                continue;
            int sf = -1;
            /* The register and immediate forms are told apart by their operands. */
            if (!strcmp(base, "lsl") || !strcmp(base, "lsr") ||
                !strcmp(base, "asr") || !strcmp(base, "ror")) {
                if (!(n == 3 && is_gpr(o, 0) && is_gpr(o, 1) && o[2].t == A64_O_IMM))
                    goto nextbf;   /* the register form is handled above */
                sf = xfl(o);
                int64_t s = o[2].imm;
                int width = sf ? 64 : 32;
                if (o[2].imm_known && (s < 0 || s >= width))
                    return bad(c, "shift out of range");
                uint32_t immr, imms, opc, N;
                if (!strcmp(base, "lsl")) {
                    opc = 0x53000000u; /* UBFM */
                    immr = (uint32_t)((width - s) & (width - 1));
                    imms = (uint32_t)(width - 1 - s);
                } else if (!strcmp(base, "lsr")) {
                    opc = 0x53000000u;
                    immr = (uint32_t)s;
                    imms = (uint32_t)(width - 1);
                } else if (!strcmp(base, "asr")) {
                    opc = 0x13000000u;
                    immr = (uint32_t)s;
                    imms = (uint32_t)(width - 1);
                } else {
                    /* extr rd, rn, rn, #s */
                    w = 0x13800000u | F(sf, 31, 1) | F(sf, 22, 1) | F(o[1].regn, 16, 5) |
                        F(s, 10, 6) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                    return w32(c, w, out, cap, outlen);
                }
                N = sf ? 1 : 0;
                w = opc | F(sf, 31, 1) | F(N, 22, 1) | F(immr, 16, 6) |
                    F(imms, 10, 6) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            if (!strcmp(base, "sxtb") || !strcmp(base, "sxth") ||
                !strcmp(base, "sxtw") || !strcmp(base, "uxtb") ||
                !strcmp(base, "uxth") || !strcmp(base, "uxtw")) {
                if (n != 2 || !is_gpr(o, 0) || !is_gpr(o, 1))
                    return bad(c, "%s wants two registers", base);
                sf = o[0].kind == RK_X;
                int src32 = o[1].kind == RK_W;
                if ((o[0].kind == RK_W) && (o[1].kind == RK_X))
                    return bad(c, "bad register combination");
                uint32_t imms;
                if (!strcmp(base, "sxtb") || !strcmp(base, "uxtb")) imms = 7;
                else if (!strcmp(base, "sxth") || !strcmp(base, "uxth")) imms = 15;
                else imms = 31;
                w = (!strcmp(base, "sxtb") || !strcmp(base, "sxth") ||
                     !strcmp(base, "sxtw")) ? 0x13000000u : 0x53000000u;
                w |= F(sf, 31, 1) | F(sf, 22, 1) | F(0, 16, 6) | F(imms, 10, 6) |
                     F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                (void)src32;
                return w32(c, w, out, cap, outlen);
            }
            /* sbfx, ubfx, bfxil, bfi, sbfiz and ubfiz */
            {
                if (n != 4 || !is_gpr(o, 0) || !is_gpr(o, 1) ||
                    o[2].t != A64_O_IMM || o[3].t != A64_O_IMM)
                    goto nextbf;
                sf = xfl(o);
                int width = sf ? 64 : 32;
                int64_t a = o[2].imm, b = o[3].imm;
                if (!o[2].imm_known || !o[3].imm_known)
                    goto nextbf;
                uint32_t immr, imms;
                uint32_t opc = !strcmp(base, "sbfx") || !strcmp(base, "sbfiz") ?
                               0x13000000u :
                               !strcmp(base, "ubfx") || !strcmp(base, "ubfiz") ?
                               0x53000000u : 0x33000000u;
                if (!strcmp(base, "sbfx") || !strcmp(base, "ubfx") ||
                    !strcmp(base, "bfxil")) {
                    immr = (uint32_t)a;
                    imms = (uint32_t)(a + b - 1);
                } else {    /* bfi, sbfiz and ubfiz */
                    immr = (uint32_t)((width - a) & (width - 1));
                    imms = (uint32_t)(b - 1);
                }
                if (imms >= (uint32_t)width)
                    return bad(c, "bitfield out of range");
                w = opc | F(sf, 31, 1) | F(sf, 22, 1) | F(immr, 16, 6) |
                    F(imms, 10, 6) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
        nextbf:;
        }
    }
    if (!strcmp(base, "extr") && n == 4) {
        if (!is_gpr(o, 0) || !is_gpr(o, 1) || !is_gpr(o, 2) || o[3].t != A64_O_IMM)
            return bad(c, "extr wants (rd, rn, rm, #lsb)");
        int sf = xfl(o);
        w = (sf ? 0x93C00000u : 0x13800000u) | F(sf, 31, 1) | F(sf, 22, 1) |
            F(o[2].regn, 16, 5) | F(o[3].imm, 10, 6) | F(o[1].regn, 5, 5) |
            F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }

    /* ---- data processing, one source ---- */
    {
        static const struct { const char *n; int op; } t[] = {
            { "rbit", 0 }, { "rev16", 1 }, { "rev32", -2 }, { "rev", -3 },
            { "clz", 4 }, { "cls", 5 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
            if (!strcmp(base, t[i].n)) {
                if (n != 2 || !is_gpr(o, 0) || !is_gpr(o, 1) ||
                    o[0].kind != o[1].kind)
                    return bad(c, "%s wants two same-size registers", base);
                int sf = xfl(o);
                int op = t[i].op < 0 ? (sf ? -t[i].op : -t[i].op - 1) : t[i].op;
                w = (sf ? 0xDAC00000u : 0x5AC00000u) | F(op, 10, 6) |
                    F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
    }

    /* ---- conditional select / compare ---- */
    {
        int cond = -1, ok;
        if (n >= 1 && o[n - 1].t == A64_O_SFX && o[n - 1].is_shift == 2 &&
            (!strcmp(base, "csel") || !strcmp(base, "csinc") ||
             !strcmp(base, "csinv") || !strcmp(base, "csneg") ||
             !strcmp(base, "cset") || !strcmp(base, "csetm") ||
             !strcmp(base, "cinc") || !strcmp(base, "cinv") ||
             !strcmp(base, "cneg") || !strcmp(base, "ccmp") ||
             !strcmp(base, "ccmn"))) {
            cond = o[n - 1].shx;
            n--;    /* the condition is not counted as an operand */
        }
        if (!strcmp(base, "csel") || !strcmp(base, "csinc") ||
            !strcmp(base, "csinv") || !strcmp(base, "csneg")) {
            ok = cond >= 0;
            if (!ok)
                cond = condbits_(sfx, sfx + strlen(sfx), &ok);
            if (!ok)
                return bad(c, "%s needs a condition", base);
            if (n != 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || !is_gpr(o, 2))
                return bad(c, "%s wants (rd, rn, rm, cond)", base);
            int sf = xfl(o);
            uint32_t b = !strcmp(base, "csel") ? 0x1A800000u :
                         !strcmp(base, "csinc") ? 0x1A800400u :
                         !strcmp(base, "csinv") ? 0x5A800000u : 0x5A800400u;
            w = b | F(sf, 31, 1) | F(o[2].regn, 16, 5) | F(cond, 12, 4) |
                F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (!strcmp(base, "cset") || !strcmp(base, "csetm")) {
            ok = cond >= 0;
            if (!ok)
                cond = condbits_(sfx, sfx + strlen(sfx), &ok);
            if (!ok)
                return bad(c, "%s needs a condition", base);
            if (n != 1 || !is_gpr(o, 0))
                return bad(c, "%s wants one register", base);
            int sf = xfl(o);
            int inv = cond ^ 1;
            w = (!strcmp(base, "csetm") ? (sf ? 0xDA800000u : 0x5A800000u)
                                        : (sf ? 0x9A800400u : 0x1A800400u)) |
                F(31, 16, 5) | F(inv, 12, 4) | F(31, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (!strcmp(base, "cinc") || !strcmp(base, "cinv") || !strcmp(base, "cneg")) {
            ok = cond >= 0;
            if (!ok)
                cond = condbits_(sfx, sfx + strlen(sfx), &ok);
            if (!ok)
                return bad(c, "%s needs a condition", base);
            if (n != 2 || !is_gpr(o, 0) || !is_gpr(o, 1))
                return bad(c, "%s wants (rd, rn, cond)", base);
            int sf = xfl(o);
            int inv = cond ^ 1;
            uint32_t b = !strcmp(base, "cinc") ? 0x1A800400u :
                         !strcmp(base, "cinv") ? 0x5A800000u : 0x5A800400u;
            w = b | F(sf, 31, 1) | F(o[1].regn, 16, 5) | F(inv, 12, 4) |
                F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (!strcmp(base, "ccmp") || !strcmp(base, "ccmn")) {
            ok = cond >= 0;
            if (!ok)
                cond = condbits_(sfx, sfx + strlen(sfx), &ok);
            if (!ok)
                return bad(c, "%s needs a condition", base);
            int op = !strcmp(base, "ccmp");
            if (n != 3 || !is_gpr(o, 0) || o[2].t != A64_O_IMM ||
                !o[2].imm_known || o[2].imm < 0 || o[2].imm > 15)
                return bad(c, "%s wants (rn, x, nzcv, cond)", base);
            int sf = o[0].kind == RK_X;
            if (o[1].t == A64_O_REG) {
                if (!is_gpr(o, 1))
                    return bad(c, "bad operand 2");
                /* CCMN is 3A400000 and CCMP is 7A400000 (op is bit 30). The sf bit is bit 31. */
                w = (sf ? 0xBA400000u : 0x3A400000u) | (op ? 0x40000000u : 0) |
                    F(o[1].regn, 16, 5) | F(cond, 12, 4) |
                    F(o[2].imm, 0, 4) | F(o[0].regn, 5, 5);
            } else {
                if (!o[1].imm_known || o[1].imm < 0 || o[1].imm > 31)
                    return bad(c, "bad immediate");
                w = (sf ? 0xBA400800u : 0x3A400800u) | (op ? 0x40000000u : 0) |
                    F(o[1].imm, 16, 5) | F(cond, 12, 4) |
                    F(o[2].imm, 0, 4) | F(o[0].regn, 5, 5);
            }
            return w32(c, w, out, cap, outlen);
        }
    }

    /* ---- system ---- */
    {
        if (!strcmp(base, "svc")) {
            if (n != 1 || o[0].t != A64_O_IMM)
                return bad(c, "svc wants an immediate");
            if (o[0].imm < 0 || o[0].imm > 0xFFFF)
                asm__soft(c, ASM_EK_IMMEDIATE, "svc: immediate out of range");
            w = 0xD4000001u | F(o[0].imm & 0xFFFF, 5, 16);
            return w32(c, w, out, cap, outlen);
        }
        if (!strcmp(base, "brk")) {
            if (n != 1 || o[0].t != A64_O_IMM)
                return bad(c, "brk wants an immediate");
            if (o[0].imm < 0 || o[0].imm > 0xFFFF)
                asm__soft(c, ASM_EK_IMMEDIATE, "brk: immediate out of range");
            w = 0xD4200000u | F(o[0].imm & 0xFFFF, 5, 16);
            return w32(c, w, out, cap, outlen);
        }
        if (!strcmp(base, "hlt")) {
            if (n != 1 || o[0].t != A64_O_IMM)
                return bad(c, "hlt wants an immediate");
            if (o[0].imm < 0 || o[0].imm > 0xFFFF)
                asm__soft(c, ASM_EK_IMMEDIATE, "hlt: immediate out of range");
            w = 0xD4400000u | F(o[0].imm & 0xFFFF, 5, 16);
            return w32(c, w, out, cap, outlen);
        }
        if (!strcmp(base, "nop") || !strcmp(base, "yield") || !strcmp(base, "wfe") ||
            !strcmp(base, "wfi") || !strcmp(base, "sev") || !strcmp(base, "sevl")) {
            static const struct { const char *n; uint32_t op; } t[] = {
                { "nop", 0 }, { "yield", 1 }, { "wfe", 2 }, { "wfi", 3 },
                { "sev", 4 }, { "sevl", 5 },
            };
            for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
                if (!strcmp(base, t[i].n)) {
                    if (n != 0)
                        return bad(c, "%s takes no operands", base);
                    w = 0xD503201Fu | F(t[i].op, 5, 6);
                    return w32(c, w, out, cap, outlen);
                }
        }
        if (!strcmp(base, "dsb") || !strcmp(base, "dmb") || !strcmp(base, "isb")) {
            static const struct { const char *n; uint32_t v; } barr[] = {
                { "oshld", 1 }, { "oshst", 2 }, { "osh", 3 }, { "nshld", 5 },
                { "nshst", 6 }, { "nsh", 7 }, { "ishld", 9 }, { "ishst", 10 },
                { "ish", 11 }, { "ld", 13 }, { "st", 14 }, { "sy", 15 },
            };
            uint32_t crm = 15;
            if (n == 1) {
                if (o[0].t == A64_O_IMM && o[0].imm_known)
                    crm = (uint32_t)(o[0].imm & 0xF);
                else if (o[0].t == A64_O_SFX && o[0].is_shift == 3) {
                    crm = 15;
                    for (unsigned bi = 0; bi < sizeof barr / sizeof barr[0]; bi++) {
                        size_t k = strlen(barr[bi].n);
                        if ((size_t)(o[0].e - o[0].b) == k &&
                            !memcmp(o[0].b, barr[bi].n, k)) {
                            crm = barr[bi].v;
                            break;
                        }
                    }
                } else
                    return bad(c, "bad barrier option");
            } else if (n != 0)
                return bad(c, "%s wants zero or one operand", base);
            if (!strcmp(base, "isb"))
                w = 0xD5033FDFu;
            else
                w = (!strcmp(base, "dsb") ? 0xD503309Fu : 0xD50330BFu) |
                    F(crm, 8, 4);
            return w32(c, w, out, cap, outlen);
        }
    }

    /* Everything else is handled in asma64_mem_fp_vec() (in asma64_mv.c). */
    return asma64_mem_fp_vec(c, name, base, sfx, o, n, out, cap, outlen);
}

/* ---------------- conditions and the entry point ---------------- */

static uint32_t condbits_(const char *b, const char *e, int *ok)
{
    static const struct { const char *n; int v; } tab[] = {
        { "eq", 0 }, { "ne", 1 }, { "cs", 2 }, { "hs", 2 }, { "cc", 3 },
        { "lo", 3 }, { "mi", 4 }, { "pl", 5 }, { "vs", 6 }, { "vc", 7 },
        { "hi", 8 }, { "ls", 9 }, { "ge", 10 }, { "lt", 11 }, { "gt", 12 },
        { "le", 13 }, { "al", 14 }, { "nv", 15 },
    };
    *ok = 1;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (span_eq(b, e, tab[i].n))
            return tab[i].v;
    *ok = 0;
    return 0;
}

int asma64_is_cond(const char *b, const char *e, int *cond)
{
    int ok;
    uint32_t v = condbits_(b, e, &ok);
    if (ok)
        *cond = (int)v;
    return ok;
}

int asma64_insn(asm_ctx *c, const char *text, size_t len,
                uint8_t *out, size_t cap, size_t *outlen)
{
    const char *p = text, *e = text + len;
    skipws(&p, e);
    const char *ms = p;
    while (p < e && (isalnum((unsigned char)*p) || *p == '.'))
        p++;
    const char *me = p;
    if (ms == me) {
        snprintf(c->err, sizeof c->err, "no mnemonic");
        return ASM_ESYNTAX;
    }
    a64opnd ops[6];
    int nops = split_ops(p, e, ops, 6);
    if (nops < 0) {
        snprintf(c->err, sizeof c->err, "too many operands");
        return ASM_ESYNTAX;
    }
    while (nops > 0) {
        const char *q = ops[nops - 1].b;
        while (q < ops[nops - 1].e && isspace((unsigned char)*q))
            q++;
        if (q == ops[nops - 1].e)
            nops--;
        else
            break;
    }
    for (int i = 0; i < nops; i++) {
        int rc = parse_opnd(c, &ops[i]);
        if (rc)
            return rc;
    }
    return asma64_dispatch(c, ms, me, ops, nops, out, cap, outlen);
}


