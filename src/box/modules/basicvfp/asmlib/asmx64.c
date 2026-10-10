/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asmx64.c: an x86-64 encoder for BBC BASIC's inline assembler.
 *
 * It reads Intel syntax (the LLVM MC flavour). One line goes in and 1 to 15
 * bytes come out. Immediates and displacements go through the eval callback,
 * so that BBC BASIC can supply expressions and labels. The rules that choose
 * between disp8 and disp32, imm8 and imm32, rel8 and rel32, and the compact
 * VEX form reproduce the choices MC makes. The frozen corpus
 * oracles/WRASM/corpus/x86_64.tsv gates byte identity.
 *
 * It works in two steps. The parser reads the prefixes, the mnemonic and the
 * operands. Then asmx64_emit encodes them using the tables.
 */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

#include "asm.h"
#include "asmx64.h"

enum {
    XR_G, XR_X, XR_Y, XR_SEG, XR_RIP, XR_Z, XR_GH,
};

enum { XOP_REG, XOP_MEM, XOP_IMM, XOP_NONE };

typedef struct xop {
    int t;
    int reg;
    int rclass;
    int size;
    int base, index, scale;     /* base: -1 for none, -2 for rip */
    int msize;
    int64_t disp;
    int disp_known;             /* as imm_known */
    int disp_named;             /* it names something, so [rip + label] means that address */
    int has_disp;               /* the operand wrote a displacement, as in [rip + label] */
    int explicit_size;
    int64_t imm;
    int imm_known;              /* 0 no, 1 yes, 2 yes but it is a forward reference */
    const char *b, *e;
} xop;

static int bad(asm_ctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->ekind = ASM_EK_SYNTAX;
    return ASM_ESYNTAX;
}

static int span_eq(const char *b, const char *e, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(e - b) == n && memcmp(b, s, n) == 0;
}

static const struct { const char *n; int reg, size, cls; } regs64[] = {
    { "rax", 0, 8, XR_G }, { "rcx", 1, 8, XR_G }, { "rdx", 2, 8, XR_G },
    { "rbx", 3, 8, XR_G }, { "rsp", 4, 8, XR_G }, { "rbp", 5, 8, XR_G },
    { "rsi", 6, 8, XR_G }, { "rdi", 7, 8, XR_G },
    { "r8", 8, 8, XR_G }, { "r9", 9, 8, XR_G }, { "r10", 10, 8, XR_G },
    { "r11", 11, 8, XR_G }, { "r12", 12, 8, XR_G }, { "r13", 13, 8, XR_G },
    { "r14", 14, 8, XR_G }, { "r15", 15, 8, XR_G },
    { "eax", 0, 4, XR_G }, { "ecx", 1, 4, XR_G }, { "edx", 2, 4, XR_G },
    { "ebx", 3, 4, XR_G }, { "esp", 4, 4, XR_G }, { "ebp", 5, 4, XR_G },
    { "esi", 6, 4, XR_G }, { "edi", 7, 4, XR_G },
    { "r8d", 8, 4, XR_G }, { "r9d", 9, 4, XR_G }, { "r10d", 10, 4, XR_G },
    { "r11d", 11, 4, XR_G }, { "r12d", 12, 4, XR_G }, { "r13d", 13, 4, XR_G },
    { "r14d", 14, 4, XR_G }, { "r15d", 15, 4, XR_G },
    { "ax", 0, 2, XR_G }, { "cx", 1, 2, XR_G }, { "dx", 2, 2, XR_G },
    { "bx", 3, 2, XR_G }, { "sp", 4, 2, XR_G }, { "bp", 5, 2, XR_G },
    { "si", 6, 2, XR_G }, { "di", 7, 2, XR_G },
    { "r8w", 8, 2, XR_G }, { "r9w", 9, 2, XR_G }, { "r10w", 10, 2, XR_G },
    { "r11w", 11, 2, XR_G }, { "r12w", 12, 2, XR_G }, { "r13w", 13, 2, XR_G },
    { "r14w", 14, 2, XR_G }, { "r15w", 15, 2, XR_G },
    { "al", 0, 1, XR_G }, { "cl", 1, 1, XR_G }, { "dl", 2, 1, XR_G },
    { "bl", 3, 1, XR_G }, { "spl", 4, 1, XR_G }, { "bpl", 5, 1, XR_G },
    { "sil", 6, 1, XR_G }, { "dil", 7, 1, XR_G },
    { "r8b", 8, 1, XR_G }, { "r9b", 9, 1, XR_G }, { "r10b", 10, 1, XR_G },
    { "r11b", 11, 1, XR_G }, { "r12b", 12, 1, XR_G }, { "r13b", 13, 1, XR_G },
    { "r14b", 14, 1, XR_G }, { "r15b", 15, 1, XR_G },
    { "ah", 4, 1, XR_GH }, { "ch", 5, 1, XR_GH }, { "dh", 6, 1, XR_GH },
    { "bh", 7, 1, XR_GH },
    { "rip", 0, 8, XR_RIP },
    { "es", 0, 2, XR_SEG }, { "cs", 1, 2, XR_SEG }, { "ss", 2, 2, XR_SEG },
    { "ds", 3, 2, XR_SEG }, { "fs", 4, 2, XR_SEG }, { "gs", 5, 2, XR_SEG },
};

static int parse_xreg(const char *b, const char *e, xop *o)
{
    char nm[8];
    size_t k = (size_t)(e - b);
    if (k == 0 || k >= sizeof nm)
        return 0;
    if (k >= 4 && b[0] == 'x' && b[1] == 'm' && b[2] == 'm') {
        int n = 0, d = 0;
        for (const char *p = b + 3; p < e; p++)
            if (isdigit((unsigned char)*p)) { n = n * 10 + (*p - '0'); d++; }
            else return 0;
        if (!d || n > 15)
            return 0;
        o->t = XOP_REG;
        o->rclass = XR_X;
        o->reg = n;
        o->size = 16;
        return 1;
    }
    if (k >= 4 && b[0] == 'z' && b[1] == 'm' && b[2] == 'm') {
        int n = 0, d = 0;
        for (const char *p = b + 3; p < e; p++)
            if (isdigit((unsigned char)*p)) { n = n * 10 + (*p - '0'); d++; }
            else return 0;
        if (!d || n > 31)
            return 0;
        o->t = XOP_REG;
        o->rclass = XR_Z;
        o->reg = n;
        o->size = 64;
        return 1;
    }
    if (k >= 4 && b[0] == 'y' && b[1] == 'm' && b[2] == 'm') {
        int n = 0, d = 0;
        for (const char *p = b + 3; p < e; p++)
            if (isdigit((unsigned char)*p)) { n = n * 10 + (*p - '0'); d++; }
            else return 0;
        if (!d || n > 15)
            return 0;
        o->t = XOP_REG;
        o->rclass = XR_Y;
        o->reg = n;
        o->size = 32;
        return 1;
    }
    memcpy(nm, b, k);
    nm[k] = 0;
    for (unsigned i = 0; i < sizeof regs64 / sizeof regs64[0]; i++)
        if (!strcmp(nm, regs64[i].n)) {
            o->t = XOP_REG;
            o->reg = regs64[i].reg;
            o->rclass = regs64[i].cls;
            o->size = regs64[i].size;
            return 1;
        }
    return 0;
}

static int reg_then_junk(const char *b, const char *e)
{
    const char *q = b;
    while (q < e && isalnum((unsigned char)*q))
        q++;
    xop r;
    memset(&r, 0, sizeof r);
    if (q == b || q == e || !parse_xreg(b, q, &r))
        return 0;
    return *q != '_' && *q != '%' && *q != '$' && *q != '(' && *q != '.' && *q != '`';
}

static int evalspan(asm_ctx *c, const char *b, const char *e, int64_t *v, int *known,
                    int *named)
{
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    while (b < e && (*b == '+' || *b == '-') && b + 1 < e && isspace((unsigned char)b[1])) {
        /* "+ sym": the sign may be separated from the term. Keep it for evaluation. */
        break;
    }
    if (b < e && *b == '#')
        b++;
    *known = 1;
    int r = c->eval ? c->eval(c->ud, b, e, v, c->err, sizeof c->err)
                    : asm_eval_literal(NULL, b, e, v, c->err, sizeof c->err);
    if (r < 0)
        return ASM_ESYNTAX;
    if (named)
        *named = (r & ASM_EVAL_NAMED) != 0 || r == 1;
    r &= ~ASM_EVAL_NAMED;
    if (r == 1)
        *known = 0, *v = 0;
    else if (r == 2)
        *known = 2;
    return 0;
}

/* The end of a term in a memory operand. It is the next + or - outside
 * parentheses and strings. A sign on a number's exponent does not count. */
static const char *term_end(const char *p, const char *e)
{
    int depth = 0, quote = 0;
    for (const char *q = p; q < e; q++) {
        if (quote) {
            if (*q == '"')
                quote = 0;
            continue;
        }
        if (*q == '"')
            quote = 1;
        else if (*q == '(')
            depth++;
        else if (*q == ')')
            depth--;
        else if ((*q == '+' || *q == '-') && depth <= 0 && q > p &&
                 !((q[-1] == 'e' || q[-1] == 'E') && q - 1 > p && isdigit((unsigned char)q[-2])))
            return q;
    }
    return e;
}

/* Parses "[base + index*scale + disp]", with the terms in any order.
 * The displacement is whatever is not a register. If its terms stand
 * together, as in [rdi + rcx*4 + table - 8], it is evaluated once as one
 * expression. Otherwise each term is evaluated and the results are summed,
 * as in [table + rdi]. With rip as the base, the displacement is the
 * address the operand refers to. This is how MC treats a label, and a BASIC
 * label is the address it names. The displacement stored is that address
 * less the address of the next instruction (asmx64_insn). [rip] alone means
 * the next instruction. */
#define XMEM_TERMS 8
static int parse_xmem(asm_ctx *c, const char *b, const char *e, xop *o)
{
    o->t = XOP_MEM;
    o->base = o->index = -1;
    o->scale = 1;
    o->disp = 0;
    o->disp_known = 1;
    b++;
    if (e > b && e[-1] == ']')
        e--;
    const char *vb[XMEM_TERMS], *ve[XMEM_TERMS];   /* the displacement's terms, signed */
    int nt = 0, runs = 0, inrun = 0;
    for (const char *t = b; t < e;) {
        const char *te = term_end(t, e);
        const char *ts = t, *tz = te;
        if (ts < tz && (*ts == '+' || *ts == '-'))
            ts++;
        while (ts < tz && isspace((unsigned char)*ts)) ts++;
        while (tz > ts && isspace((unsigned char)tz[-1])) tz--;
        int neg = t < te && *t == '-';
        if (ts == tz)
            return bad(c, "missing term");
        xop r1;
        memset(&r1, 0, sizeof r1);
        const char *star = memchr(ts, '*', (size_t)(tz - ts));
        int isreg = 0;
        if (star) {
            /* reg*scale or scale*reg. Any other product belongs to the
             * displacement. */
            const char *l1 = star, *r0 = star + 1;
            while (l1 > ts && isspace((unsigned char)l1[-1])) l1--;
            while (r0 < tz && isspace((unsigned char)*r0)) r0++;
            const char *ss = NULL, *se = NULL;
            if (parse_xreg(ts, l1, &r1))
                ss = r0, se = tz;
            else if (parse_xreg(r0, tz, &r1))
                ss = ts, se = l1;
            if (ss) {
                if (r1.rclass != XR_G || neg)
                    return asm__fail(c, ASM_EK_REGISTER, "index register expected");
                if (o->index != -1)
                    return asm__fail(c, ASM_EK_REGISTER, "too many registers");
                int sc = 0, d = 0;
                for (const char *q = ss; q < se; q++)
                    if (isdigit((unsigned char)*q)) { sc = sc > 9999 ? sc : sc * 10 + (*q - '0'); d++; }
                    else if (!isspace((unsigned char)*q)) return bad(c, "bad scale");
                if (!d || (sc != 1 && sc != 2 && sc != 4 && sc != 8))
                    return bad(c, "bad scale");
                o->index = r1.reg;
                o->scale = sc;
                isreg = 1;
            }
        } else if (parse_xreg(ts, tz, &r1) && (r1.rclass == XR_G || r1.rclass == XR_RIP)) {
            if (neg)
                return asm__fail(c, ASM_EK_REGISTER, "a register cannot be subtracted");
            if (r1.rclass == XR_RIP) {
                if (o->base != -1 || o->index != -1)
                    return asm__fail(c, ASM_EK_REGISTER, "too many registers");
                o->base = -2;
            } else if (o->base == -1 && o->index == -1) {
                o->base = r1.reg;
            } else if (o->index == -1 && o->base != -2) {
                o->index = r1.reg;
                o->scale = 1;
            } else
                return asm__fail(c, ASM_EK_REGISTER, "too many registers");
            isreg = 1;
        }
        if (isreg)
            inrun = 0;
        else {
            if (nt == XMEM_TERMS)
                return bad(c, "too many terms");
            vb[nt] = (*t == '+' || *t == '-') ? t : ts;
            ve[nt] = tz;
            nt++;
            if (!inrun)
                runs++;
            inrun = 1;
        }
        t = te;
    }
    o->has_disp = nt > 0;
    /* rsp cannot be an index, because SIB index 100 means none. [rax + rsp]
     * is taken as [rsp + rax], as MC takes it. A scaled rsp is an error, and
     * so is rsp as both base and index. */
    if (o->index == 4) {
        if (o->scale != 1 || o->base == 4 || o->base == -2)
            return asm__fail(c, ASM_EK_REGISTER, "rsp cannot be an index");
        o->index = o->base;
        o->base = 4;
    }
    if (runs == 1) {
        int rc = evalspan(c, vb[0], ve[nt - 1], &o->disp, &o->disp_known, &o->disp_named);
        if (rc)
            return rc;
    } else if (nt) {
        int64_t sum = 0;
        int known = 1;
        for (int i = 0; i < nt; i++) {
            int64_t v;
            int k, nm;
            int rc = evalspan(c, vb[i], ve[i], &v, &k, &nm);
            if (rc)
                return rc;
            sum += v;
            o->disp_named |= nm;
            if (k == 0 || known == 0)
                known = 0;
            else if (k == 2)
                known = 2;
        }
        o->disp = sum;
        o->disp_known = known;
    }
    /* A displacement is 32 bits, sign-extended. For [rip + label] it is the
     * label less the next instruction, and that is checked when it is
     * known. */
    if (!(o->base == -2 && o->disp_named) && (o->disp < INT32_MIN || o->disp > INT32_MAX))
        asm__soft(c, ASM_EK_OFFSET, "displacement out of range");
    return 0;
}

typedef struct {
    uint8_t b[20];
    size_t n;
    size_t rip_at;              /* 1 more than where a rip-relative disp32 went, or 0 */
    int64_t rip_to;             /* the address that disp32 refers to */
} ob;

static void ob1(ob *o, uint8_t v)
{
    if (o->n < sizeof o->b)
        o->b[o->n++] = v;
}

static void ob4le(ob *o, uint32_t v)
{
    ob1(o, (uint8_t)v);
    ob1(o, (uint8_t)(v >> 8));
    ob1(o, (uint8_t)(v >> 16));
    ob1(o, (uint8_t)(v >> 24));
}

static void ob8le(ob *o, uint64_t v)
{
    ob4le(o, (uint32_t)v);
    ob4le(o, (uint32_t)(v >> 32));
}

static void emit_rex(ob *out, int w, int r, int x, int b)
{
    ob1(out, 0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0));
}

/* The ModRM, SIB and displacement machinery. The REX flags come first,
 * because they precede the opcode. Then the caller emits the opcodes. Then
 * rm_tail emits the ModRM byte, the SIB byte and the displacement. */
typedef struct {
    int mod, rmv, need_sib, ss, sib_base, sib_index;
    int rip, abs, reg3, force_rex;
    int rip_to;                 /* the displacement is an address (see asmx64_insn) */
    int nobase;                 /* an index and no base: SIB base 101 and a disp32 */
    int64_t disp;
} rmplan;

static int rm_plan(const xop *m, int *rexr, int *rexx, int *rexb, rmplan *P)
{
    *rexx = 0;
    *rexb = 0;
    memset(P, 0, sizeof *P);
    P->sib_base = -1;
    P->sib_index = -1;
    if (m->t == XOP_REG) {
        P->mod = 3;
        P->rmv = m->reg & 7;
        *rexb = (m->reg & 8) != 0;
        P->force_rex = (m->rclass == XR_G && m->size == 1 && (m->reg & 7) >= 4);
        return 0;
    }
    int base = m->base, index = m->index, scale = m->scale;
    P->rip = base == -2;
    P->disp = m->disp;
    /* [rip + label] means the label's address. [rip + 16] is a plain
     * displacement. This is how MC reads them (see ASM_EVAL_NAMED in
     * asm.h). */
    P->rip_to = P->rip && m->has_disp && m->disp_known && m->disp_named;
    if (base >= 0)
        *rexb = (base & 8) != 0;
    if (index >= 0)
        *rexx = (index & 8) != 0;
    if (!P->rip && base == -1 && index == -1) {
        P->abs = 1;
        return 0;
    }
    /* rm = 100 means a SIB byte follows. That holds for r12 as for rsp,
     * because REX.B only extends the base that the SIB byte names. */
    P->need_sib = P->rip ? 0 : (index >= 0 || (base >= 0 && (base & 7) == 4) || base == -1);
    if (P->rip)
        P->rmv = 5;
    else if (!P->need_sib) {
        P->rmv = base & 7;
    }
    int mod;
    if (P->rip)
        mod = 0;
    else if (m->disp_known != 1)
        mod = 2;
    else if (m->disp == 0 && (base & 7) != 5)
        mod = 0;
    else if (m->disp >= -128 && m->disp <= 127)
        mod = 1;
    else
        mod = 2;
    if (!P->rip && (base & 7) == 5 && mod == 0)
        mod = 1;
    /* An index and no base is encoded as mod 00 with SIB base 101. That
     * means [index*s + disp32], and the displacement is always four bytes.
     * With mod 01 or 10, base 101 is rbp. */
    if (!P->rip && base == -1)
        mod = 0, P->nobase = 1;
    P->mod = mod;
    if (P->need_sib) {
        P->rmv = 4;
        P->ss = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
        P->sib_base = (base < 0) ? 5 : (base & 7);
        P->sib_index = (index < 0) ? 4 : (index & 7);
    } else if (!P->rip) {
        P->rmv = base & 7;
    }
    return 0;
}

static void rm_tail(ob *out, const rmplan *P)
{
    if (P->abs) {
        ob1(out, (uint8_t)(P->reg3 << 3) | 0x04);
        ob1(out, 0x25);
        ob4le(out, (uint32_t)P->disp);
        return;
    }
    ob1(out, (uint8_t)(P->mod << 6) | (uint8_t)(P->reg3 << 3) | (uint8_t)P->rmv);
    if (P->need_sib)
        ob1(out, (uint8_t)((P->ss << 6) | (P->sib_index << 3) | P->sib_base));
    if (P->nobase) {
        ob4le(out, (uint32_t)P->disp);
        return;
    }
    if (P->rip) {
        if (P->rip_to) {
            out->rip_at = out->n + 1;
            out->rip_to = P->disp;
        }
        ob4le(out, (uint32_t)P->disp);
        return;
    }
    if (P->mod == 1)
        ob1(out, (uint8_t)P->disp);
    else if (P->mod == 2)
        ob4le(out, (uint32_t)P->disp);
}

static void emit_abs(ob *out, const xop *m, int regv, int rexr, int rexw)
{
    if (rexw | rexr)
        emit_rex(out, rexw, rexr, 0, 0);
    ob1(out, (uint8_t)(regv << 3) | 4);
    ob1(out, 0x25);
    ob4le(out, (uint32_t)m->disp);
}

/* A list of opcode bytes. Nothing uses this type. */
typedef struct {
    const uint8_t *ops;
    int nops;
} opbytes;

/* Returns any error from the call it wraps. */
#define TRY(x)                                                                \
    do {                                                                      \
        int rc_ = (x);                                                        \
        if (rc_)                                                              \
            return rc_;                                                       \
    } while (0)

/* a general register, rax..r15b or ah..bh */
#define GPR(x) ((x).rclass == XR_G || (x).rclass == XR_GH)

static int opsize3(const xop *a, const xop *b, const xop *d)
{
    const xop *arr[3] = { a, b, d };
    for (int i = 0; i < 3; i++) {
        const xop *o = arr[i];
        if (o && o->t == XOP_REG && GPR(*o))
            return o->size;
        if (o && o->t == XOP_MEM && o->msize)
            return o->msize;
    }
    return 4;
}

/* ---- generic emitter: prefixes, REX, opcodes, modrm tail ---- */
static int emit_op_rm(asm_ctx *c, ob *out, const uint8_t *ops, int nops,
                      int szprefix, int rexw, const xop *rm, const xop *reg,
                      int regv_override)
{
    rmplan P;
    int rexr = 0, rexx = 0, rexb = 0;
    rm_plan(rm, &rexr, &rexx, &rexb, &P);
    P.reg3 = reg ? (reg->reg & 7) : regv_override;
    if (reg)
        rexr = (reg->reg & 8) != 0;
    /* spl, bpl, sil and dil need a REX prefix in either field. Without one,
     * those register numbers mean ah, ch, dh and bh, and those cannot be
     * used with a REX prefix. */
    if (reg && reg->t == XOP_REG && reg->rclass == XR_G && reg->size == 1 && reg->reg >= 4 &&
        reg->reg < 8)
        P.force_rex = 1;
    int rex = rexw | rexr | rexx | rexb | P.force_rex;
    if (rex && ((reg && reg->t == XOP_REG && reg->rclass == XR_GH) ||
                (rm->t == XOP_REG && rm->rclass == XR_GH)))
        return asm__fail(c, ASM_EK_REGISTER, "ah, bh, ch or dh with a REX prefix");
    /* a lock prefix, which goes after what the caller put first */
    if (szprefix)
        ob1(out, (uint8_t)szprefix);
    if (rex)
        emit_rex(out, rexw, rexr, rexx, rexb);
    for (int i = 0; i < nops; i++)
        ob1(out, ops[i]);
    rm_tail(out, &P);
    return 0;
}

/* ---- VEX ---- */
static int vex_emit(ob *out, int map, int pp, int w, int l, int rexr, int rexx,
                    int rexb, int vvvv, const uint8_t *ops, int nops,
                    const xop *rm, int regfield)
{
    /* MC stores a used source register complemented. It stores 1111 when
     * the field is unused. This was checked against clang and the corpus. */
    int vf = vvvv < 0 ? 15 : (~vvvv & 15);    /* -1 means no source register */
    int need3 = rexx | rexb | w | (map != 1);
    out->n = 0;
    if (need3) {
        uint8_t b1 = (uint8_t)((rexr ? 0 : 0x80) | (rexx ? 0 : 0x40) |
                               (rexb ? 0 : 0x20) | map);
        uint8_t b2 = (uint8_t)((w ? 0x80 : 0) | (vf << 3) |
                               (l ? 4 : 0) | pp);
        ob1(out, 0xC4);
        ob1(out, b1);
        ob1(out, b2);
    } else {
        uint8_t b1 = (uint8_t)((rexr ? 0 : 0x80) | (vf << 3) |
                               (l ? 4 : 0) | pp);
        ob1(out, 0xC5);
        ob1(out, b1);
    }
    for (int i = 0; i < nops; i++)
        ob1(out, ops[i]);
    /* modrm tail: reg field = dst, rm = last operand */
    {
        rmplan P;
        int rr2, rx2, rb2;
        rm_plan(rm, &rr2, &rx2, &rb2, &P);
        P.reg3 = regfield & 7;
        rm_tail(out, &P);
    }
    return 0;
}

/* EVEX, using the WRASM rules.
 *   P0 = R~ X~ B~ R'~ 00 mm
 *   P1 = W ~vvvv 1 pp
 *   P2 = 0 L'L 0 V~' 000
 * X and B come from the rm operand. For a register they are bit 4 and bit 3.
 * For memory they are bit 3 of the index and of the base. Bit 4 of the reg
 * operand goes in R'. */
static int evex_emit(ob *out, int map, int pp, int w, int ll, int rexr,
                     int rexx, int rexb, int regr2, int vvvv, uint8_t op,
                     const xop *rm, int regfield)
{
    (void)rexr; (void)rexb;
    int vhi = vvvv < 0 ? 0 : (vvvv >> 4) & 1;
    int vf = vvvv < 0 ? 15 : (~vvvv & 15);
    out->n = 0;
    uint8_t p0 = (uint8_t)((((regfield >> 3) & 1) ^ 1) << 7 |
                           (rexx ^ 1) << 6 | (rexb ^ 1) << 5 |
                           (regr2 ^ 1) << 4 | (map & 3));
    uint8_t p1 = (uint8_t)((w ? 0x80 : 0) | (vf << 3) | 0x04 | pp);
    uint8_t p2 = (uint8_t)((ll & 3) << 5 | ((vhi ^ 1) << 3));
    ob1(out, 0x62);
    ob1(out, p0);
    ob1(out, p1);
    ob1(out, p2);
    ob1(out, op);
    rmplan P;
    int r2, x2, b2;
    rm_plan(rm, &r2, &x2, &b2, &P);
    P.reg3 = regfield & 7;
    rm_tail(out, &P);
    return 0;
}

/* ---------------- mnemonic tables ---------------- */

struct aluinfo { const char *n; int col; };
static const struct aluinfo alutab[] = {
    { "add", 0x00 }, { "or", 0x08 }, { "adc", 0x10 }, { "sbb", 0x18 },
    { "and", 0x20 }, { "sub", 0x28 }, { "xor", 0x30 }, { "cmp", 0x38 },
};

/* Condition code names and their numbers */
static int ccnum(const char *n)
{
    static const struct { const char *n; int v; } t[] = {
        { "o", 0 }, { "no", 1 }, { "b", 2 }, { "c", 2 }, { "nae", 2 },
        { "ae", 3 }, { "nb", 3 }, { "nc", 3 }, { "e", 4 }, { "z", 4 },
        { "ne", 5 }, { "nz", 5 }, { "be", 6 }, { "na", 6 }, { "a", 7 },
        { "nbe", 7 }, { "s", 8 }, { "ns", 9 }, { "p", 10 }, { "pe", 10 },
        { "np", 11 }, { "po", 11 }, { "l", 12 }, { "nge", 12 }, { "ge", 13 },
        { "nl", 13 }, { "le", 14 }, { "ng", 14 }, { "g", 15 }, { "nle", 15 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(n, t[i].n))
            return t[i].v;
    return -1;
}

/* SSE and AVX table: name, map 0F opcode, pp and kind */
enum {
    SK_RM,      /* op xmm, xmm/mem (reg=dst, rm=src) */
    SK_MR,      /* op xmm/mem, xmm (store-style: rm=dst, reg=src) */
    SK_RMI,     /* op xmm, xmm/mem, imm8 */
    SK_GPR_RM,  /* op xmm, r32/mem (movd etc) */
    SK_GPR2,    /* movd xmm, r32 (load) / r32, xmm (store) */
};

typedef struct {
    const char *mn;
    uint8_t op;         /* 0F opcode (load form) */
    uint8_t op2;        /* store form (same as op for non-mov ops) */
    uint8_t pp;         /* 0 none, 1 66, 2 F3, 3 F2 */
    uint8_t kind;
    uint8_t vex;        /* also has a VEX form */
    uint8_t map38;      /* 0F 38 three-byte map */
} sseform;

static const sseform ssetab[] = {
    { "movaps", 0x28, 0x29, 0, 0, 0, 0 }, { "movups", 0x10, 0x11, 0, 0, 0, 0 },
    { "movapd", 0x28, 0x29, 1, 0, 0, 0 }, { "movupd", 0x10, 0x11, 1, 0, 0, 0 },
    { "movss", 0x10, 0x11, 2, 0, 0, 0 }, { "movsd", 0x10, 0x11, 3, 0, 0, 0 },
    { "movdqa", 0x6F, 0x7F, 1, 0, 0, 0 }, { "movdqu", 0x6F, 0x7F, 2, 0, 0, 0 },
    { "movlps", 0x12, 0x13, 0, 0, 0, 0 }, { "movhps", 0x16, 0x17, 0, 0, 0, 0 },
    { "movlhps", 0x16, 0x16, 0, 0, 0, 0 }, { "movhlps", 0x12, 0x12, 0, 0, 0, 0 },
    { "addps", 0x58, 0x58, 0, 0, 0, 0 }, { "addpd", 0x58, 0x58, 1, 0, 0, 0 },
    { "addss", 0x58, 0x58, 2, 0, 0, 0 }, { "addsd", 0x58, 0x58, 3, 0, 0, 0 },
    { "subps", 0x5C, 0x5C, 0, 0, 0, 0 }, { "subpd", 0x5C, 0x5C, 1, 0, 0, 0 },
    { "subss", 0x5C, 0x5C, 2, 0, 0, 0 }, { "subsd", 0x5C, 0x5C, 3, 0, 0, 0 },
    { "mulps", 0x59, 0x59, 0, 0, 0, 0 }, { "mulpd", 0x59, 0x59, 1, 0, 0, 0 },
    { "mulss", 0x59, 0x59, 2, 0, 0, 0 }, { "mulsd", 0x59, 0x59, 3, 0, 0, 0 },
    { "divps", 0x5E, 0x5E, 0, 0, 0, 0 }, { "divpd", 0x5E, 0x5E, 1, 0, 0, 0 },
    { "divss", 0x5E, 0x5E, 2, 0, 0, 0 }, { "divsd", 0x5E, 0x5E, 3, 0, 0, 0 },
    { "maxps", 0x5F, 0x5F, 0, 0, 0, 0 }, { "maxpd", 0x5F, 0x5F, 1, 0, 0, 0 },
    { "maxss", 0x5F, 0x5F, 2, 0, 0, 0 }, { "maxsd", 0x5F, 0x5F, 3, 0, 0, 0 },
    { "minps", 0x5D, 0x5D, 0, 0, 0, 0 }, { "minpd", 0x5D, 0x5D, 1, 0, 0, 0 },
    { "minss", 0x5D, 0x5D, 2, 0, 0, 0 }, { "minsd", 0x5D, 0x5D, 3, 0, 0, 0 },
    { "sqrtps", 0x51, 0x51, 0, 0, 0, 0 }, { "sqrtpd", 0x51, 0x51, 1, 0, 0, 0 },
    { "sqrtss", 0x51, 0x51, 2, 0, 0, 0 }, { "sqrtsd", 0x51, 0x51, 3, 0, 0, 0 },
    { "rcpps", 0x53, 0x53, 0, 0, 0, 0 }, { "rsqrtps", 0x52, 0x52, 0, 0, 0, 0 },
    { "andps", 0x54, 0x54, 0, 0, 0, 0 }, { "andpd", 0x54, 0x54, 1, 0, 0, 0 },
    { "andnps", 0x55, 0x55, 0, 0, 0, 0 }, { "andnpd", 0x55, 0x55, 1, 0, 0, 0 },
    { "orps", 0x56, 0x56, 0, 0, 0, 0 }, { "orpd", 0x56, 0x56, 1, 0, 0, 0 },
    { "xorps", 0x57, 0x57, 0, 0, 0, 0 }, { "xorpd", 0x57, 0x57, 1, 0, 0, 0 },
    { "comiss", 0x2F, 0x2F, 0, 0, 0, 0 }, { "comisd", 0x2F, 0x2F, 1, 0, 0, 0 },
    { "ucomiss", 0x2E, 0x2E, 0, 0, 0, 0 }, { "ucomisd", 0x2E, 0x2E, 1, 0, 0, 0 },
    { "cvtdq2ps", 0x5B, 0x5B, 0, 0, 0, 0 }, { "cvtps2pd", 0x5A, 0x5A, 0, 0, 0, 0 },
    { "cvtdq2pd", 0xE6, 0xE6, 2, 0, 0, 0 },
    { "cvtpd2ps", 0x5A, 0x5A, 1, 0, 0, 0 }, { "cvtps2dq", 0x5B, 0x5B, 1, 0, 0, 0 },
    { "cvttps2dq", 0x5B, 0x5B, 2, 0, 0, 0 },
    { "cvtss2sd", 0x5A, 0x5A, 2, 0, 0, 0 }, { "cvtsd2ss", 0x5A, 0x5A, 3, 0, 0, 0 },
    { "cvtsi2ss", 0x2A, 0x2A, 2, 0, 0, 0 }, { "cvtsi2sd", 0x2A, 0x2A, 3, 0, 0, 0 },
    { "cvttss2si", 0x2C, 0x2C, 2, 0, 0, 0 }, { "cvttsd2si", 0x2C, 0x2C, 3, 0, 0, 0 },
    { "cvtss2si", 0x2D, 0x2D, 2, 0, 0, 0 }, { "cvtsd2si", 0x2D, 0x2D, 3, 0, 0, 0 },
    { "pxor", 0xEF, 0xEF, 1, 0, 1, 0 }, { "por", 0xEB, 0xEB, 1, 0, 1, 0 },
    { "pand", 0xDB, 0xDB, 1, 0, 1, 0 }, { "pandn", 0xDF, 0xDF, 1, 0, 1, 0 },
    { "paddb", 0xFC, 0xFC, 1, 0, 1, 0 }, { "paddw", 0xFD, 0xFD, 1, 0, 1, 0 },
    { "paddd", 0xFE, 0xFE, 1, 0, 1, 0 }, { "paddq", 0xD4, 0xD4, 1, 0, 1, 0 },
    { "psubb", 0xF8, 0xF8, 1, 0, 1, 0 }, { "psubw", 0xF9, 0xF9, 1, 0, 1, 0 },
    { "psubd", 0xFA, 0xFA, 1, 0, 1, 0 }, { "psubq", 0xFB, 0xFB, 1, 0, 1, 0 },
    { "pcmpeqb", 0x74, 0x74, 1, 0, 1, 0 }, { "pcmpeqw", 0x75, 0x75, 1, 0, 1, 0 },
    { "pcmpeqd", 0x76, 0x76, 1, 0, 1, 0 },
    { "pmullw", 0xD5, 0xD5, 1, 0, 1, 0 }, { "pmulld", 0x40, 0x40, 1, 0, 1, 1 },
    { "unpcklps", 0x14, 0x14, 0, 0, 1, 0 }, { "unpckhps", 0x15, 0x15, 0, 0, 1, 0 },
    { "unpcklpd", 0x14, 0x14, 1, 0, 1, 0 }, { "unpckhpd", 0x15, 0x15, 1, 0, 1, 0 },
    { "shufps", 0xC6, 0xC6, 0, 2, 0, 0 }, { "shufpd", 0xC6, 0xC6, 1, 2, 0, 0 },
    { "pshufd", 0x70, 0x70, 1, 2, 1, 0 },
    { "movd", 0x6E, 0x7E, 1, 0, 1, 0 },
    { "movq", 0x6E, 0x7E, 1, 0, 0, 0 },
    { NULL, 0, 0, 0, 0, 0, 0 },
};

/* VEX forms, with three operands (dst, src1, src2): name, opcode, pp and W */
typedef struct {
    const char *mn;
    uint8_t op;
    uint8_t pp;
    uint8_t w;
    uint8_t kind;       /* 0: xmm,xmm,xmm/m; 1: xmm,xmm/m (vvvv=1111);
                           2: xmm,xmm,xmm/m,imm8; 3: xmm,xmm,r/m (general register) */
    uint8_t map;        /* VEX mmmmm: 1 = 0F, 2 = 0F38 */
} vexform;

static const vexform vextab[] = {
    { "vaddps", 0x58, 0, 0, 0 }, { "vaddpd", 0x58, 1, 0, 0 },
    { "vaddss", 0x58, 2, 0, 0 }, { "vaddsd", 0x58, 3, 0, 0 },
    { "vsubps", 0x5C, 0, 0, 0 }, { "vsubpd", 0x5C, 1, 0, 0 },
    { "vsubss", 0x5C, 2, 0, 0 }, { "vsubsd", 0x5C, 3, 0, 0 },
    { "vmulps", 0x59, 0, 0, 0 }, { "vmulpd", 0x59, 1, 0, 0 },
    { "vmulss", 0x59, 2, 0, 0 }, { "vmulsd", 0x59, 3, 0, 0 },
    { "vdivps", 0x5E, 0, 0, 0 }, { "vdivpd", 0x5E, 1, 0, 0 },
    { "vdivss", 0x5E, 2, 0, 0 }, { "vdivsd", 0x5E, 3, 0, 0 },
    { "vmaxps", 0x5F, 0, 0, 0 }, { "vmaxpd", 0x5F, 1, 0, 0 },
    { "vmaxss", 0x5F, 2, 0, 0 }, { "vmaxsd", 0x5F, 3, 0, 0 },
    { "vminps", 0x5D, 0, 0, 0 }, { "vminpd", 0x5D, 1, 0, 0 },
    { "vminss", 0x5D, 2, 0, 0 }, { "vminsd", 0x5D, 3, 0, 0 },
    { "vsqrtps", 0x51, 0, 0, 1 }, { "vsqrtpd", 0x51, 1, 0, 1 },
    { "vsqrtss", 0x51, 2, 0, 0 }, { "vsqrtsd", 0x51, 3, 0, 0 },
    { "vandps", 0x54, 0, 0, 0 }, { "vandpd", 0x54, 1, 0, 0 },
    { "vandnps", 0x55, 0, 0, 0 }, { "vandnpd", 0x55, 1, 0, 0 },
    { "vorps", 0x56, 0, 0, 0 }, { "vorpd", 0x56, 1, 0, 0 },
    { "vxorps", 0x57, 0, 0, 0 }, { "vxorpd", 0x57, 1, 0, 0 },
    { "vpand", 0xDB, 1, 0, 0 }, { "vpandn", 0xDF, 1, 0, 0 },
    { "vpor", 0xEB, 1, 0, 0 }, { "vpxor", 0xEF, 1, 0, 0 },
    { "vpaddb", 0xFC, 1, 0, 0 }, { "vpaddw", 0xFD, 1, 0, 0 },
    { "vpaddd", 0xFE, 1, 0, 0 }, { "vpaddq", 0xD4, 1, 0, 0 },
    { "vpsubb", 0xF8, 1, 0, 0 }, { "vpsubw", 0xF9, 1, 0, 0 },
    { "vpsubd", 0xFA, 1, 0, 0 }, { "vpsubq", 0xFB, 1, 0, 0 },
    { "vpcmpeqb", 0x74, 1, 0, 0 }, { "vpcmpeqw", 0x75, 1, 0, 0 },
    { "vpcmpeqd", 0x76, 1, 0, 0 },
    { "vpmullw", 0xD5, 1, 0, 0 }, { "vpmulld", 0x40, 1, 0, 0, 2 },
    { "vunpcklps", 0x14, 0, 0, 0 }, { "vunpckhps", 0x15, 0, 0, 0 },
    { "vunpcklpd", 0x14, 1, 0, 0 }, { "vunpckhpd", 0x15, 1, 0, 0 },
    { "vshufps", 0xC6, 0, 0, 2 }, { "vshufpd", 0xC6, 1, 0, 2 },
    { "vpshufd", 0x70, 1, 0, 2 },
    { "vcvtdq2ps", 0x5B, 0, 0, 1 }, { "vcvtps2pd", 0x5A, 0, 0, 1 },
    { "vcvtpd2ps", 0x5A, 1, 0, 1 }, { "vcvtps2dq", 0x5B, 1, 0, 1 },
    { "vcvttps2dq", 0x5B, 2, 0, 1 },
    { "vcvtss2sd", 0x5A, 2, 0, 0 }, { "vcvtsd2ss", 0x5A, 3, 0, 0 },
    { "vcvtsi2ss", 0x2A, 2, 0, 3 }, { "vcvtsi2sd", 0x2A, 3, 0, 3 },
    { "vcvttss2si", 0x2C, 2, 0, 1 }, { "vcvttsd2si", 0x2C, 3, 0, 1 },
    { "vrcpps", 0x53, 0, 0, 1 }, { "vrsqrtps", 0x52, 0, 0, 1 },
    { "vmovaps", 0x28, 0, 0, 4 }, { "vmovaps", 0x29, 0, 0, 5 },
    { "vmovups", 0x10, 0, 0, 4 }, { "vmovups", 0x11, 0, 0, 5 },
    { "vmovapd", 0x28, 1, 0, 4 }, { "vmovapd", 0x29, 1, 0, 5 },
    { "vmovupd", 0x10, 1, 0, 4 }, { "vmovupd", 0x11, 1, 0, 5 },
    { "vmovss", 0x10, 2, 0, 6 }, { "vmovss", 0x11, 2, 0, 7 },
    { "vmovsd", 0x10, 3, 0, 6 }, { "vmovsd", 0x11, 3, 0, 7 },
    { "vmovdqa", 0x6F, 1, 0, 4 }, { "vmovdqa", 0x7F, 1, 0, 5 },
    { "vmovdqu", 0x6F, 2, 0, 4 }, { "vmovdqu", 0x7F, 2, 0, 5 },
    { NULL, 0, 0, 0, 0 },
};

/* ---------------- values and their fields ---------------- */

/* Checks an immediate for an operation of sz bytes.
 * For sz of 1, 2 or 4 the field is that size, and the value may be signed or
 * unsigned. 0xFFFFFFFF is a 32-bit -1, as MC takes it. It is made negative
 * here so that the short forms see it.
 * For sz of 8 the field is 32 bits, sign-extended.
 * A value that does not fit is a value error. It is reported like the ARM
 * assembler's "Bad immediate constant", with the errors bit of OPT. */
static void imm_fit(asm_ctx *c, xop *o, int sz)
{
    if (o->t != XOP_IMM || !o->imm_known)
        return;
    if (sz == 8) {
        if (o->imm < INT32_MIN || o->imm > INT32_MAX)
            asm__soft(c, ASM_EK_IMMEDIATE, "immediate out of range");
        return;
    }
    int64_t full = (int64_t)1 << (8 * sz);
    if (o->imm < -(full / 2) || o->imm >= full)
        asm__soft(c, ASM_EK_IMMEDIATE, "immediate out of range");
    else if (o->imm >= full / 2)
        o->imm -= full;
}

/* Accepts a value in [lo, hi]. Any other value is a value error of the given kind. */
static void in_range(asm_ctx *c, const xop *o, int64_t lo, int64_t hi, int kind)
{
    if (o->t == XOP_IMM && o->imm_known && (o->imm < lo || o->imm > hi))
        asm__soft(c, kind, "%s out of range",
                  kind == ASM_EK_SHIFT ? "shift" : "immediate");
}

/* A jump's or call's rel32. A target out of reach is a value error that
 * BASIC always reports, as it does for the ARM B instruction ("Bad address
 * offset"). */
static void rel32(asm_ctx *c, int64_t d)
{
    if (d < INT32_MIN || d > INT32_MAX)
        asm__soft(c, ASM_EK_OFFSET | ASM_SOFT_ALWAYS, "branch out of range");
}

/* An imm8, as shufps and pshufd take: a byte, signed or unsigned */
static uint8_t imm8(asm_ctx *c, const xop *o)
{
    in_range(c, o, -128, 255, ASM_EK_IMMEDIATE);
    return (uint8_t)o->imm;
}

/* ---------------- the dispatcher ---------------- */

int asmx64_emit(asm_ctx *c, const char *mn, xop *o, int n, int prf, ob *out)
{
    int rep = prf & 2, repne = prf & 4;     /* asmx64_insn puts lock first */
    int sz;

    /* ALU */
    for (unsigned i = 0; i < sizeof alutab / sizeof alutab[0]; i++) {
        if (strcmp(mn, alutab[i].n))
            continue;
        sz = opsize3(&o[0], n > 1 ? &o[1] : NULL, NULL);
        if (n == 2 && o[0].t == XOP_REG && o[1].t == XOP_REG &&
            GPR(o[0]) && GPR(o[1])) {
            /* MC: register-register always r/m <- r (dst in rm) */
            uint8_t ops[1] = { (uint8_t)(alutab[i].col + (sz == 1 ? 0 : 1)) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], &o[1], 0);
        }
        if (n == 2 && o[0].t == XOP_REG && GPR(o[0]) &&
            o[1].t == XOP_MEM) {
            uint8_t ops[1] = { (uint8_t)(alutab[i].col + (sz == 1 ? 2 : 3)) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[1], &o[0], 0);
        }
        if (n == 2 && o[0].t == XOP_MEM && o[1].t == XOP_REG &&
            GPR(o[1])) {
            uint8_t ops[1] = { (uint8_t)(alutab[i].col + (sz == 1 ? 0 : 1)) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], &o[1], 0);
        }
        if (n == 2 && o[0].t == XOP_MEM && o[1].t == XOP_IMM) {
            imm_fit(c, &o[1], sz);
            int use83 = sz != 1 && o[1].imm_known == 1 &&
                        o[1].imm >= -128 && o[1].imm <= 127;
            uint8_t ops[1] = { (uint8_t)(use83 ? 0x83 : (sz == 1 ? 0x80 : 0x81)) };
            TRY(emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8, &o[0],
                       NULL, alutab[i].col >> 3));
            if (sz == 1 || use83)
                ob1(out, (uint8_t)o[1].imm);
            else if (sz == 2)
                ob1(out, (uint8_t)o[1].imm), ob1(out, (uint8_t)(o[1].imm >> 8));
            else
                ob4le(out, (uint32_t)o[1].imm);
            return 0;
        }
        if (n == 2 && o[0].t == XOP_REG && GPR(o[0]) &&
            o[1].t == XOP_IMM) {
            imm_fit(c, &o[1], sz);
            int use83 = sz != 1 && o[1].imm_known == 1 &&
                        o[1].imm >= -128 && o[1].imm <= 127;
            if (o[0].reg == 0 && sz == 1 && GPR(o[0])) {
                /* al, imm8: MC's short form */
                ob1(out, (uint8_t)(alutab[i].col + 4));
                ob1(out, (uint8_t)o[1].imm);
                return 0;
            }
            if (o[0].reg == 0 && !use83 && sz != 1) {
                /* ax/eax/rax, imm: MC's short form */
                if (sz == 2)
                    ob1(out, 0x66);
                if (sz == 8)
                    ob1(out, 0x48);
                ob1(out, (uint8_t)(alutab[i].col + 5));
                if (sz == 2)
                    ob1(out, (uint8_t)o[1].imm), ob1(out, (uint8_t)(o[1].imm >> 8));
                else
                    ob4le(out, (uint32_t)o[1].imm);
                return 0;
            }
            uint8_t ops[1] = { (uint8_t)(use83 ? 0x83 : (sz == 1 ? 0x80 : 0x81)) };
            TRY(emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8, &o[0],
                       NULL, alutab[i].col >> 3));
            if (sz == 1 || use83)
                ob1(out, (uint8_t)o[1].imm);
            else if (sz == 2)
                ob1(out, (uint8_t)o[1].imm), ob1(out, (uint8_t)(o[1].imm >> 8));
            else
                ob4le(out, (uint32_t)o[1].imm);
            return 0;
        }
        return bad(c, "bad %s operands", mn);
    }

    /* test */
    if (!strcmp(mn, "test")) {
        sz = opsize3(&o[0], n > 1 ? &o[1] : NULL, NULL);
        if (n == 2 && o[1].t == XOP_REG && GPR(o[1]) &&
            (o[0].t == XOP_REG || o[0].t == XOP_MEM)) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0x84 : 0x85) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], &o[1], 0);
        }
        if (n == 2 && o[0].t == XOP_REG && GPR(o[0]) &&
            o[1].t == XOP_REG && GPR(o[1])) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0x84 : 0x85) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[1], &o[0], 0);
        }
        if (n == 2)
            imm_fit(c, &o[1], sz);
        if (n == 2 && o[1].t == XOP_IMM && o[0].t == XOP_REG &&
            GPR(o[0]) && o[0].reg == 0 && sz != 2) {
            if (sz == 8)
                ob1(out, 0x48);
            ob1(out, (uint8_t)(sz == 1 ? 0xA8 : 0xA9));
            if (sz == 1)
                ob1(out, (uint8_t)o[1].imm);
            else
                ob4le(out, (uint32_t)o[1].imm);
            return 0;
        }
        if (n == 2 && o[1].t == XOP_IMM && (o[0].t == XOP_REG || o[0].t == XOP_MEM)) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xF6 : 0xF7) };
            TRY(emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8, &o[0],
                       NULL, 0));
            if (sz == 1)
                ob1(out, (uint8_t)o[1].imm);
            else if (sz == 2) {
                ob1(out, (uint8_t)o[1].imm);
                ob1(out, (uint8_t)(o[1].imm >> 8));
            } else
                ob4le(out, (uint32_t)o[1].imm);
            return 0;
        }
        return bad(c, "bad test operands");
    }

    /* mov */
    if (!strcmp(mn, "mov")) {
        sz = opsize3(&o[0], n > 1 ? &o[1] : NULL, NULL);
        if (n == 2 && o[0].t == XOP_REG && GPR(o[0]) &&
            o[1].t == XOP_MEM) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0x8A : 0x8B) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[1], &o[0], 0);
        }
        if (n == 2 && (o[0].t == XOP_REG && GPR(o[0])) &&
            o[1].t == XOP_REG && GPR(o[1])) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0x88 : 0x89) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], &o[1], 0);
        }
        if (n == 2 && o[0].t == XOP_MEM && o[1].t == XOP_REG &&
            GPR(o[1])) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0x88 : 0x89) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], &o[1], 0);
        }
        if (n == 2 && o[0].t == XOP_MEM && o[1].t == XOP_IMM) {
            imm_fit(c, &o[1], sz);
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xC6 : 0xC7) };
            TRY(emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8, &o[0], NULL, 0));
            if (sz == 1)
                ob1(out, (uint8_t)o[1].imm);
            else if (sz == 2) {
                ob1(out, (uint8_t)o[1].imm);
                ob1(out, (uint8_t)(o[1].imm >> 8));
            } else
                ob4le(out, (uint32_t)o[1].imm);
            return 0;
        }
        if (n == 2 && o[0].t == XOP_REG && GPR(o[0]) &&
            o[1].t == XOP_IMM) {
            /* The B8+r form takes an immediate, and movabs forces imm64.
             * A 64-bit register takes C7's sign-extended imm32 where the
             * value fits in it. It does the same where the value is not yet
             * settled, because its size must not change (see asm.h). MC
             * treats a symbol this way. It uses movabs only for a 64-bit
             * value that comes from a variable. */
            int is64 = o[0].size == 8 && !strcmp(mn, "movabs");
            if (o[0].size == 8 && o[1].imm_known == 1 &&
                (o[1].imm < -0x80000000LL || o[1].imm > 0x7FFFFFFFLL))
                is64 = 1;
            if (!o[1].imm_known)
                is64 = 0;
            if (!is64)
                imm_fit(c, &o[1], sz);  /* movabs skips the check: any 64 bits */
            /* mov r64, imm-fits-int32 uses C7 /0 (MC), not B8+r */
            if (o[0].size == 8 && !is64) {
                uint8_t ops[1] = { 0xC7 };
                TRY(emit_op_rm(c, out, ops, 1, 0, 1, &o[0], NULL, 0));
                ob4le(out, (uint32_t)o[1].imm);
                return 0;
            }
            if (sz == 2)
                ob1(out, 0x66);
            /* REX: W for imm64, B for r8-r15, and plain for spl-dil */
            if (is64 || (o[0].reg & 8) ||
                (sz == 1 && o[0].rclass == XR_G && o[0].reg >= 4))
                emit_rex(out, is64, 0, 0, (o[0].reg & 8) != 0);
            ob1(out, (uint8_t)((sz == 1 ? 0xB0 : 0xB8) + (o[0].reg & 7)));
            if (sz == 1)
                ob1(out, (uint8_t)o[1].imm);
            else if (sz == 2) {
                ob1(out, (uint8_t)o[1].imm);
                ob1(out, (uint8_t)(o[1].imm >> 8));
            } else if (is64)
                ob8le(out, (uint64_t)o[1].imm);
            else
                ob4le(out, (uint32_t)o[1].imm);
            return 0;
        }
        if (n == 2 && o[0].t == XOP_REG && o[0].rclass == XR_SEG &&
            (o[1].t == XOP_REG && GPR(o[1]) || o[1].t == XOP_MEM)) {
            uint8_t ops[1] = { 0x8E };
            return emit_op_rm(c, out, ops, 1, 0, 0, &o[1], &o[0], 0);;
        }
        if (n == 2 && o[1].t == XOP_REG && o[1].rclass == XR_SEG &&
            (o[0].t == XOP_REG && GPR(o[0]) || o[0].t == XOP_MEM)) {
            uint8_t ops[1] = { 0x8C };
            return emit_op_rm(c, out, ops, 1, 0, 0, &o[0], &o[1], 0);;
        }
        return bad(c, "bad mov operands");
    }
    if (!strcmp(mn, "movabs") && n == 2 && o[0].t == XOP_REG &&
        o[0].rclass == XR_G && o[0].size == 8 && o[1].t == XOP_IMM) {
        emit_rex(out, 1, 0, 0, (o[0].reg & 8) != 0);
        ob1(out, (uint8_t)(0xB8 + (o[0].reg & 7)));
        ob8le(out, (uint64_t)o[1].imm);
        return 0;
    }

    if (!strcmp(mn, "lea") && n == 2 && o[0].t == XOP_REG &&
        o[1].t == XOP_MEM) {
        uint8_t ops[1] = { 0x8D };
        return emit_op_rm(c, out, ops, 1, o[0].size == 2 ? 0x66 : 0,
                          o[0].size == 8, &o[1], &o[0], 0);
    }

    if (!strcmp(mn, "xchg") && n == 2 && o[0].t == XOP_REG &&
        o[1].t == XOP_REG && o[0].rclass == XR_G && o[1].rclass == XR_G &&
        (o[0].reg == 0 || o[1].reg == 0) && o[0].size == o[1].size && o[0].size != 1) {
        int other = o[0].reg == 0 ? o[1].reg : o[0].reg;
        int szx = o[0].size;
        if (szx == 2)
            ob1(out, 0x66);
        if ((szx == 8 && other != 0) || (other & 8))    /* xchg rax, rax is 90, as MC has it */
            emit_rex(out, szx == 8, 0, 0, (other & 8) != 0);
        ob1(out, (uint8_t)(0x90 + (other & 7)));
        return 0;
    }
    if (!strcmp(mn, "xchg") && n == 2) {
        sz = opsize3(&o[0], &o[1], NULL);
        if (o[0].t == XOP_REG && o[1].t == XOP_REG) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0x86 : 0x87) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[1], &o[0], 0);
        }
        if (o[0].t == XOP_MEM && o[1].t == XOP_REG &&
            o[1].rclass == XR_G) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0x86 : 0x87) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], &o[1], 0);
        }
        return bad(c, "bad xchg operands");
    }

    if (!strcmp(mn, "push") && n == 1) {
        if (o[0].t == XOP_REG && o[0].rclass == XR_G && o[0].size == 8) {
            if (o[0].reg & 8)
                ob1(out, 0x41);
            ob1(out, (uint8_t)(0x50 + (o[0].reg & 7)));
            return 0;
        }
        if (o[0].t == XOP_REG && o[0].rclass == XR_SEG) {
            if (o[0].reg < 4)
                return bad(c, "push of this segment register is not valid in 64-bit mode");
            ob1(out, 0x66);             /* as LLVM MC writes push fs and push gs */
            ob1(out, 0x0F);
            ob1(out, o[0].reg == 4 ? 0xA0 : 0xA8);
            return 0;
        }
        if (o[0].t == XOP_IMM) {
            imm_fit(c, &o[0], 8);       /* 64 bits are pushed from an imm32, sign-extended */
            if (o[0].imm_known == 1 && o[0].imm >= -128 && o[0].imm <= 127) {
                ob1(out, 0x6A);
                ob1(out, (uint8_t)o[0].imm);
            } else {
                ob1(out, 0x68);
                ob4le(out, (uint32_t)o[0].imm);
            }
            return 0;
        }
        if (o[0].t == XOP_MEM) {
            uint8_t ops[1] = { 0xFF };
            return emit_op_rm(c, out, ops, 1, o[0].msize == 2 ? 0x66 : 0,
                              0, &o[0], NULL, 6);
        }
    }
    if (!strcmp(mn, "pop") && n == 1) {
        if (o[0].t == XOP_REG && o[0].rclass == XR_G && o[0].size == 8) {
            if (o[0].reg & 8)
                ob1(out, 0x41);
            ob1(out, (uint8_t)(0x58 + (o[0].reg & 7)));
            return 0;
        }
        if (o[0].t == XOP_MEM) {
            uint8_t ops[1] = { 0x8F };
            return emit_op_rm(c, out, ops, 1, o[0].msize == 2 ? 0x66 : 0,
                              0, &o[0], NULL, 0);
        }
    }

    if (!strcmp(mn, "imul")) {
        sz = opsize3(&o[0], n > 1 ? &o[1] : NULL, NULL);
        if (n == 1 && (o[0].t == XOP_REG || o[0].t == XOP_MEM)) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xF6 : 0xF7) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0,
                              sz == 8, &o[0], NULL, 5);
        }
        if (n == 2 && o[0].t == XOP_REG && o[1].t != XOP_IMM) {
            uint8_t o2[2] = { 0x0F, 0xAF };
            return emit_op_rm(c, out, o2, 2, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[1], &o[0], 0);
        }
        if (n == 3 && o[2].t == XOP_IMM) {
            imm_fit(c, &o[2], sz);
            int use6B = o[2].imm_known == 1 && o[2].imm >= -128 && o[2].imm <= 127;
            uint8_t o2[1] = { (uint8_t)(use6B ? 0x6B : 0x69) };
            TRY(emit_op_rm(c, out, o2, 1, sz == 2 ? 0x66 : 0, sz == 8,
                       &o[1], &o[0], 0));
            if (use6B)
                ob1(out, (uint8_t)o[2].imm);
            else if (sz == 2)           /* a 16-bit operation has an imm16 */
                ob1(out, (uint8_t)o[2].imm), ob1(out, (uint8_t)(o[2].imm >> 8));
            else
                ob4le(out, (uint32_t)o[2].imm);
            return 0;
        }
        return bad(c, "bad imul operands");
    }

    /* F6/F7 /digit group */
    {
        static const struct { const char *n; int digit; } grp3[] = {
            { "not", 2 }, { "neg", 3 }, { "mul", 4 }, { "imul", 5 },
            { "div", 6 }, { "idiv", 7 },
        };
        for (unsigned i = 0; i < sizeof grp3 / sizeof grp3[0]; i++) {
            if (strcmp(mn, grp3[i].n) || n != 1)
                continue;
            sz = opsize3(&o[0], NULL, NULL);
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xF6 : 0xF7) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], NULL, grp3[i].digit);
        }
    }

    /* shifts */
    {
        static const struct { const char *n; int digit; } grp2[] = {
            { "rol", 0 }, { "ror", 1 }, { "rcl", 2 }, { "rcr", 3 },
            { "shl", 4 }, { "sal", 4 }, { "shr", 5 }, { "sar", 7 },
        };
        for (unsigned i = 0; i < sizeof grp2 / sizeof grp2[0]; i++) {
            if (strcmp(mn, grp2[i].n) || n < 2)
                continue;
            sz = opsize3(&o[0], NULL, NULL);
            /* The count is a byte, as MC takes it. The CPU masks it to 5
             * bits, or 6 bits for a 64-bit operation. A larger or negative
             * count gives the ARM assembler's "Bad shift" error. */
            in_range(c, &o[1], 0, 255, ASM_EK_SHIFT);
            if (o[1].t == XOP_IMM && o[1].imm_known == 1 && o[1].imm == 1) {
                uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xD0 : 0xD1) };
                TRY(emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0,
                           sz == 8, &o[0], NULL, grp2[i].digit));
                return 0;
            }
            if (o[1].t == XOP_IMM) {
                uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xC0 : 0xC1) };
                TRY(emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0,
                           sz == 8, &o[0], NULL, grp2[i].digit));
                ob1(out, (uint8_t)o[1].imm);
                return 0;
            }
            if (o[1].t == XOP_REG && o[1].reg == 1 && o[1].size == 1) {
                uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xD2 : 0xD3) };
                return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0,
                                  sz == 8, &o[0], NULL, grp2[i].digit);
            }
            return bad(c, "bad shift operands");
        }
    }

    /* bt family */
    {
        static const struct { const char *n; uint8_t op, digit; } btt[] = {
            { "bt", 0xA3, 4 }, { "bts", 0xAB, 5 }, { "btr", 0xB3, 6 },
            { "btc", 0xBB, 7 },
        };
        for (unsigned i = 0; i < sizeof btt / sizeof btt[0]; i++) {
            if (strcmp(mn, btt[i].n) || n != 2)
                continue;
            sz = opsize3(&o[0], &o[1], NULL);
            if (o[1].t == XOP_IMM) {
                in_range(c, &o[1], 0, 255, ASM_EK_IMMEDIATE);   /* the bit number is a byte, as MC takes it */
                uint8_t ops[2] = { 0x0F, 0xBA };
                TRY(emit_op_rm(c, out, ops, 2, sz == 2 ? 0x66 : 0,
                           sz == 8, &o[0], NULL, btt[i].digit));
                ob1(out, (uint8_t)o[1].imm);
                return 0;
            }
            uint8_t ops[2] = { 0x0F, btt[i].op };
            return emit_op_rm(c, out, ops, 2, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], &o[1], 0);
        }
    }

    /* bsf/bsr/popcnt/lzcnt/tzcnt */
    {
        static const struct { const char *n; uint8_t op; uint8_t pp; } t[] = {
            { "bsf", 0xBC, 0 }, { "bsr", 0xBD, 0 },
            { "tzcnt", 0xBC, 2 }, { "lzcnt", 0xBD, 2 },
            { "popcnt", 0xB8, 2 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(mn, t[i].n) || n != 2)
                continue;
            sz = opsize3(&o[0], &o[1], NULL);
            rmplan P;
            int rexr = 0, rexx = 0, rexb = 0;
            rm_plan(&o[1], &rexr, &rexx, &rexb, &P);
            P.reg3 = o[0].reg & 7;
            rexr = (o[0].reg & 8) != 0;
            out->n = 0;
            if (t[i].pp == 2)
                ob1(out, 0xF3);
            if (sz == 2)
                ob1(out, 0x66);
            if ((sz == 8) | rexr | rexx | rexb)
                emit_rex(out, sz == 8, rexr, rexx, rexb);
            ob1(out, 0x0F);
            ob1(out, t[i].op);
            rm_tail(out, &P);
            return 0;
        }
    }

    /* movzx / movsx / movsxd */
    if (!strcmp(mn, "movzx") || !strcmp(mn, "movsx") || !strcmp(mn, "movsxd")) {
        if (n != 2 || o[0].t != XOP_REG)
            return bad(c, "bad %s operands", mn);
        int srcsz = o[1].t == XOP_REG ? o[1].size : o[1].msize;
        uint8_t op = !strcmp(mn, "movsxd") ? 0x63 :
                     !strcmp(mn, "movzx") ? (srcsz == 1 ? 0xB6 : 0xB7) :
                     (srcsz == 1 ? 0xBE : 0xBF);
        if (!strcmp(mn, "movsxd")) {
            return emit_op_rm(c, out, &op, 1, 0, 1, &o[1], &o[0], 0);;
        }
        uint8_t ops[2] = { 0x0F, op };
        return emit_op_rm(c, out, ops, 2, o[0].size == 2 ? 0x66 : 0,
                          o[0].size == 8, &o[1], &o[0], 0);
    }

    /* setcc / cmovcc */
    {
        int iset = !strncmp(mn, "set", 3) && ccnum(mn + 3) >= 0;
        int iscmov = !strncmp(mn, "cmov", 4) && ccnum(mn + 4) >= 0;
        if (iset && n == 1) {
            uint8_t ops[2] = { 0x0F, (uint8_t)(0x90 + ccnum(mn + 3)) };
            return emit_op_rm(c, out, ops, 2, 0, 0, &o[0], NULL, 0);;
        }
        if (iscmov && n == 2) {
            sz = opsize3(&o[0], &o[1], NULL);
            uint8_t ops[2] = { 0x0F, (uint8_t)(0x40 + ccnum(mn + 4)) };
            return emit_op_rm(c, out, ops, 2, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[1], &o[0], 0);
        }
    }

    /* jcc / jmp / call */
    {
        int isjcc = mn[0] == 'j' && strcmp(mn, "jmp") != 0 && ccnum(mn + 1) >= 0;
        if (isjcc && n == 1 && o[0].t == XOP_IMM) {
            int cc = ccnum(mn + 1);
            int64_t d = o[0].imm_known ? asm_target(c, o[0].imm) - (int64_t)(c->addr + 2) : 0x7FFFFFFF;
            if (o[0].imm_known == 1 && d >= -128 && d <= 127) {   /* not forward (asm.h) */
                ob1(out, (uint8_t)(0x70 + cc));
                ob1(out, (uint8_t)d);
                return 0;
            }
            d = o[0].imm_known ? asm_target(c, o[0].imm) - (int64_t)(c->addr + 6) : 0;
            rel32(c, d);
            uint8_t ops[2] = { 0x0F, (uint8_t)(0x80 + cc) };
            ob1(out, ops[0]);
            ob1(out, ops[1]);
            ob4le(out, (uint32_t)d);
            return 0;
        }
        if (!strcmp(mn, "jmp") && n == 1 && o[0].t == XOP_IMM) {
            int64_t d = o[0].imm_known ? asm_target(c, o[0].imm) - (int64_t)(c->addr + 2) : 0x7FFFFFFF;
            if (o[0].imm_known == 1 && d >= -128 && d <= 127) {   /* not forward (asm.h) */
                ob1(out, 0xEB);
                ob1(out, (uint8_t)d);
                return 0;
            }
            d = o[0].imm_known ? asm_target(c, o[0].imm) - (int64_t)(c->addr + 5) : 0;
            rel32(c, d);
            ob1(out, 0xE9);
            ob4le(out, (uint32_t)d);
            return 0;
        }
        if (!strcmp(mn, "jmp") && n == 1 && (o[0].t == XOP_REG || o[0].t == XOP_MEM)) {
            uint8_t ops[1] = { 0xFF };
            sz = opsize3(&o[0], NULL, NULL);
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, 0, &o[0], NULL, 4);
        }
        if (!strcmp(mn, "call") && n == 1 && o[0].t == XOP_IMM) {
            int64_t d = o[0].imm_known ? asm_target(c, o[0].imm) - (int64_t)(c->addr + 5) : 0;
            rel32(c, d);
            ob1(out, 0xE8);
            ob4le(out, (uint32_t)d);
            return 0;
        }
        if (!strcmp(mn, "call") && n == 1 && (o[0].t == XOP_REG || o[0].t == XOP_MEM)) {
            uint8_t ops[1] = { 0xFF };
            return emit_op_rm(c, out, ops, 1, 0, 0, &o[0], NULL, 2);
        }
    }

    /* ret / leave / nop / zero-operand */
    {
        static const struct { const char *n; uint8_t b[4]; int k; int immsz; } t[] = {
            { "ret", { 0xC3 }, 1, 0 },
            { "ret", { 0xC2 }, 1, 2 },
            { "leave", { 0xC9 }, 1, 0 },
            { "int3", { 0xCC }, 1, 0 },
            { "int", { 0xCD }, 1, 1 },
            { "into", { 0xCE }, 1, 0 },
            { "hlt", { 0xF4 }, 1, 0 },
            { "cmc", { 0xF5 }, 1, 0 },
            { "clc", { 0xF8 }, 1, 0 },
            { "stc", { 0xF9 }, 1, 0 },
            { "cli", { 0xFA }, 1, 0 },
            { "sti", { 0xFB }, 1, 0 },
            { "cld", { 0xFC }, 1, 0 },
            { "std", { 0xFD }, 1, 0 },
            { "nop", { 0x90 }, 1, 0 },
            { "pause", { 0xF3, 0x90 }, 2, 0 },
            { "syscall", { 0x0F, 0x05 }, 2, 0 },
            { "cpuid", { 0x0F, 0xA2 }, 2, 0 },
            { "rdtsc", { 0x0F, 0x31 }, 2, 0 },
            { "endbr64", { 0xF3, 0x0F, 0x1E, 0xFA }, 4, 0 },
            { "ud2", { 0x0F, 0x0B }, 2, 0 },
            { "mfence", { 0x0F, 0xAE, 0xF0 }, 3, 0 },
            { "lfence", { 0x0F, 0xAE, 0xE8 }, 3, 0 },
            { "sfence", { 0x0F, 0xAE, 0xF8 }, 3, 0 },
            { "cdq", { 0x99 }, 1, 0 },
            { "cqo", { 0x99 }, 1, 0 },
            { "cwde", { 0x98 }, 1, 0 },
            { "cdqe", { 0x98 }, 1, 0 },
            { "sahf", { 0x9E }, 1, 0 },
            { "lahf", { 0x9F }, 1, 0 },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(mn, t[i].n))
                continue;
            if (t[i].immsz == 0 ? (n != 0) : (n != 1))
                continue;
            if (!strcmp(mn, "int") && n == 1 && o[0].t == XOP_IMM && o[0].imm_known == 1 &&
                o[0].imm == 3) {
                ob1(out, 0xCC);         /* MC assembles "int 3" as int3 */
                return 0;
            }
            if (!strcmp(mn, "cqo"))
                ob1(out, 0x48);
            if (!strcmp(mn, "cdqe"))
                ob1(out, 0x48);
            for (int k = 0; k < t[i].k; k++)
                ob1(out, t[i].b[k]);
            if (t[i].immsz == 1 && n == 1 && o[0].t == XOP_IMM) {
                in_range(c, &o[0], -128, 255, ASM_EK_IMMEDIATE);
                ob1(out, (uint8_t)o[0].imm);
            }
            if (t[i].immsz == 2 && n == 1 && o[0].t == XOP_IMM) {
                in_range(c, &o[0], -32768, 65535, ASM_EK_IMMEDIATE);
                ob1(out, (uint8_t)o[0].imm);
                ob1(out, (uint8_t)(o[0].imm >> 8));
            }
            return 0;
        }
    }

    /* inc / dec */
    if (!strcmp(mn, "inc") || !strcmp(mn, "dec")) {
        int digit = mn[0] == 'i' ? 0 : 1;
        sz = opsize3(&o[0], NULL, NULL);
        if (n == 1 && (o[0].t == XOP_REG || o[0].t == XOP_MEM)) {
            uint8_t ops[1] = { (uint8_t)(sz == 1 ? 0xFE : 0xFF) };
            return emit_op_rm(c, out, ops, 1, sz == 2 ? 0x66 : 0, sz == 8,
                              &o[0], NULL, digit);
        }
    }

    /* xadd / cmpxchg / bswap / movbe */
    if (!strcmp(mn, "xadd") && n == 2) {
        sz = opsize3(&o[0], &o[1], NULL);
        uint8_t ops[2] = { 0x0F, (uint8_t)(sz == 1 ? 0xC0 : 0xC1) };
        return emit_op_rm(c, out, ops, 2, sz == 2 ? 0x66 : 0, sz == 8,
                          &o[0], &o[1], 0);
    }
    if (!strcmp(mn, "cmpxchg") && n == 2) {
        sz = opsize3(&o[0], &o[1], NULL);
        uint8_t ops[2] = { 0x0F, (uint8_t)(sz == 1 ? 0xB0 : 0xB1) };
        return emit_op_rm(c, out, ops, 2, sz == 2 ? 0x66 : 0, sz == 8,
                          &o[0], &o[1], 0);
    }
    if (!strcmp(mn, "bswap") && n == 1 && o[0].t == XOP_REG) {
        if (o[0].size == 8 || (o[0].reg & 8))
            emit_rex(out, o[0].size == 8, 0, 0, (o[0].reg & 8) != 0);
        ob1(out, 0x0F);
        ob1(out, (uint8_t)(0xC8 + (o[0].reg & 7)));
        return 0;
    }
    if (!strcmp(mn, "movbe") && n == 2) {
        sz = opsize3(&o[0], &o[1], NULL);
        int load = o[0].t == XOP_REG;
        uint8_t ops[3] = { 0x0F, 0x38, (uint8_t)(load ? 0xF0 : 0xF1) };
        const xop *rm = load ? &o[1] : &o[0];
        const xop *reg = load ? &o[0] : &o[1];
        return emit_op_rm(c, out, ops, 3, sz == 2 ? 0x66 : 0, sz == 8,
                          rm, reg, 0);;
    }

    /* string ops */
    {
        static const struct { const char *n; int sz; uint8_t op; } t[] = {
            { "movsb", 1, 0xA4 }, { "movsw", 2, 0xA5 }, { "movsd", 4, 0xA5 },
            { "movsq", 8, 0xA5 },
            { "stosb", 1, 0xAA }, { "stosw", 2, 0xAB }, { "stosd", 4, 0xAB },
            { "stosq", 8, 0xAB },
            { "lodsb", 1, 0xAC }, { "lodsw", 2, 0xAD }, { "lodsd", 4, 0xAD },
            { "lodsq", 8, 0xAD },
            { "scasb", 1, 0xAE }, { "scasw", 2, 0xAF }, { "scasd", 4, 0xAF },
            { "scasq", 8, 0xAF },
            { "cmpsb", 1, 0xA6 }, { "cmpsw", 2, 0xA7 }, { "cmpsd", 4, 0xA7 },
            { "cmpsq", 8, 0xA7 },
            { "insb", 1, 0x6C }, { "outsb", 1, 0x6E },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(mn, t[i].n) || n != 0)
                continue;
            if (rep)
                ob1(out, 0xF3);
            else if (repne)
                ob1(out, 0xF2);
            if (t[i].sz == 2)
                ob1(out, 0x66);
            if (t[i].sz == 8)
                ob1(out, 0x48);
            ob1(out, t[i].op);
            return 0;
        }
    }

    /* SSE: legacy prefix, REX, 0F, opcode, modrm */
    for (unsigned i = 0; ssetab[i].mn; i++) {
        if (strcmp(mn, ssetab[i].mn) || n < 2)
            continue;
        const sseform *f = &ssetab[i];
        if ((!strcmp(f->mn, "movd") || !strcmp(f->mn, "movq")) &&
            ((o[0].t == XOP_REG && o[0].rclass == XR_G &&
              o[1].t == XOP_REG && o[1].rclass == XR_X) ||
             (o[0].t == XOP_REG && o[0].rclass == XR_X &&
              o[1].t == XOP_REG && o[1].rclass == XR_G) ||
             (!strcmp(f->mn, "movd") &&
              ((o[0].t == XOP_MEM && o[0].explicit_size) ||
               (o[1].t == XOP_MEM && o[1].explicit_size)) &&
              (o[0].t != XOP_REG || o[0].rclass != XR_X) &&
              (o[1].t != XOP_REG || o[1].rclass != XR_X)))) {
            /* movd and movq between xmm and a general register or memory.
             * movq between xmm and xmm or memory is handled below, because
             * it needs F3 and 66 prefixes in the two directions. */
            int is64 = !strcmp(f->mn, "movq") ||
                       (o[0].t == XOP_MEM && o[0].msize == 8) ||
                       (o[1].t == XOP_MEM && o[1].msize == 8) ||
                       (o[0].rclass == XR_G && o[0].size == 8) ||
                       (o[1].rclass == XR_G && o[1].size == 8);
            if (!strcmp(f->mn, "movd"))
                is64 = 0;
            const xop *xv = (o[0].rclass == XR_X) ? &o[0] : &o[1];
            const xop *gv = (xv == &o[0]) ? &o[1] : &o[0];
            int to_xmm = xv == &o[0];
            rmplan P;
            int rexr = 0, rexx = 0, rexb = 0;
            rm_plan(gv, &rexr, &rexx, &rexb, &P);
            P.reg3 = xv->reg & 7;
            rexr = (xv->reg & 8) != 0;
            out->n = 0;
            ob1(out, 0x66);
            if (is64 | rexr | rexx | rexb)
                emit_rex(out, is64, rexr, rexx, rexb);
            ob1(out, 0x0F);
            ob1(out, to_xmm ? 0x6E : 0x7E);
            rm_tail(out, &P);
            return 0;
        }
        if (!strcmp(f->mn, "movq") && (o[0].rclass == XR_X) &&
            (o[1].rclass == XR_X ||
             (o[1].t == XOP_MEM && (!o[1].explicit_size || o[1].msize == 8)))) {
            rmplan P;
            int rexr = 0, rexx = 0, rexb = 0;
            rm_plan(&o[1], &rexr, &rexx, &rexb, &P);
            P.reg3 = o[0].reg & 7;
            rexr = (o[0].reg & 8) != 0;
            out->n = 0;
            ob1(out, 0xF3);
            if (rexr | rexx | rexb)
                emit_rex(out, 0, rexr, rexx, rexb);
            ob1(out, 0x0F);
            ob1(out, 0x7E);
            rm_tail(out, &P);
            return 0;
        }
        if (!strcmp(f->mn, "movq") && o[1].rclass == XR_X &&
            o[0].t == XOP_MEM) {
            rmplan P;
            int rexr = 0, rexx = 0, rexb = 0;
            rm_plan(&o[0], &rexr, &rexx, &rexb, &P);
            P.reg3 = o[1].reg & 7;
            rexr = (o[1].reg & 8) != 0;
            out->n = 0;
            ob1(out, 0x66);
            if (rexr | rexx | rexb)
                emit_rex(out, 0, rexr, rexx, rexb);
            ob1(out, 0x0F);
            ob1(out, 0xD6);
            rm_tail(out, &P);
            return 0;
        }
        if (f->kind == SK_RM || f->kind == SK_MR) {
            const xop *regop, *rm;
            if (strstr(f->mn, "2si") && o[0].t == XOP_REG &&
                o[0].rclass == XR_G) {
                regop = &o[0];      /* gpr destination in the reg field */
                rm = &o[1];
            } else {
                regop = (o[0].t == XOP_REG && (o[0].rclass == XR_X || o[0].rclass == XR_Y)) ? &o[0] : &o[1];
                rm = (regop == &o[0]) ? &o[1] : &o[0];
            }
            int store = (o[0].t == XOP_MEM);
            int rexw = 0;
            if (!strncmp(f->mn, "cvtsi2", 6))
                rexw = (rm->t == XOP_REG ? rm->size == 8 :
                        rm->t == XOP_MEM ? rm->msize == 8 : 0);
            else if (strstr(f->mn, "2si"))
                rexw = o[0].t == XOP_REG && o[0].rclass == XR_G &&
                       o[0].size == 8;
            rmplan P;
            int rexr = 0, rexx = 0, rexb = 0;
            rm_plan(rm, &rexr, &rexx, &rexb, &P);
            P.reg3 = regop->reg & 7;
            rexr = (regop->reg & 8) != 0;
            out->n = 0;
            if (f->pp == 1)
                ob1(out, 0x66);
            else if (f->pp == 2)
                ob1(out, 0xF3);
            else if (f->pp == 3)
                ob1(out, 0xF2);
            if (rexw | rexr | rexx | rexb)
                emit_rex(out, rexw, rexr, rexx, rexb);
            ob1(out, 0x0F);
            if (f->map38)
                ob1(out, 0x38);
            ob1(out, store ? f->op2 : f->op);
            rm_tail(out, &P);
            return 0;
        }
        if (f->kind == SK_RMI) {
            const xop *regop = &o[0];
            const xop *rm = &o[1];
            rmplan P;
            int rexr = 0, rexx = 0, rexb = 0;
            rm_plan(rm, &rexr, &rexx, &rexb, &P);
            P.reg3 = regop->reg & 7;
            rexr = (regop->reg & 8) != 0;
            out->n = 0;
            if (f->pp == 1)
                ob1(out, 0x66);
            else if (f->pp == 2)
                ob1(out, 0xF3);
            else if (f->pp == 3)
                ob1(out, 0xF2);
            if (rexr | rexx | rexb)
                emit_rex(out, 0, rexr, rexx, rexb);
            ob1(out, 0x0F);
            if (f->map38)
                ob1(out, 0x38);
            ob1(out, f->op);
            rm_tail(out, &P);
            ob1(out, imm8(c, &o[2]));
            return 0;
        }
        if (f->kind == SK_GPR_RM) {
            /* movd/movq: xmm <-> r/m */
            int is64 = (o[0].t == XOP_MEM ? o[0].msize :
                        o[1].t == XOP_MEM ? o[1].msize :
                        (o[0].rclass == XR_G ? o[0].size : o[1].size)) == 8;
            const xop *xv = (o[0].rclass == XR_X) ? &o[0] : &o[1];
            const xop *gv = (xv == &o[0]) ? &o[1] : &o[0];
            int to_xmm = xv == &o[0];
            rmplan P;
            int rexr = 0, rexx = 0, rexb = 0;
            rm_plan(gv, &rexr, &rexx, &rexb, &P);
            P.reg3 = xv->reg & 7;
            rexr = (xv->reg & 8) != 0;
            out->n = 0;
            ob1(out, 0x66);
            if (is64 | rexr | rexx | rexb)
                emit_rex(out, is64, rexr, rexx, rexb);
            ob1(out, 0x0F);
            ob1(out, to_xmm ? 0x6E : 0x7E);
            rm_tail(out, &P);
            return 0;
        }
    }

    /* VEX */
    for (unsigned i = 0; vextab[i].mn; i++) {
        if (strcmp(mn, vextab[i].mn))
            continue;
        const vexform *f = &vextab[i];
        int l256 = (o[0].rclass == XR_Y) || (n > 1 && o[1].rclass == XR_Y) ||
                   (n > 2 && o[2].t == XOP_REG && o[2].rclass == XR_Y) ||
                   (n > 2 && o[2].t == XOP_MEM && o[2].msize == 32);
        int anyz = 0;
        for (int i = 0; i < n; i++)
            if (o[i].t == XOP_REG && o[i].rclass == XR_Z)
                anyz = 1;
        size_t mnl = strlen(f->mn);
        int evexw = mnl >= 2 &&
                    (!strcmp(f->mn + mnl - 2, "pd") ||
                     !strcmp(f->mn + mnl - 2, "sd") ||
                     !strcmp(f->mn, "vpaddq") || !strcmp(f->mn, "vpsubq"));
        if (anyz && f->kind == 2 && (n == 3 || n == 4)) {
            int three = (n == 3);
            const xop *rm = three ? &o[1] : &o[2];
            int vvvv = three ? -1 : o[1].reg;
            int rexx = 0, rexb = 0;
            if (rm->t == XOP_REG)
                rexx = (rm->reg >> 4) & 1, rexb = (rm->reg >> 3) & 1;
            else if (rm->t == XOP_MEM) {
                if (rm->base >= 0)
                    rexb = (rm->base & 8) != 0;
                if (rm->index >= 0)
                    rexx = (rm->index & 8) != 0;
            }
            int rc = evex_emit(out, f->map == 2 ? 2 : 1, f->pp, evexw, 2,
                               0, rexx, rexb, (o[0].reg >> 4) & 1, vvvv,
                               f->op, rm, o[0].reg);
            ob1(out, imm8(c, three ? &o[2] : &o[3]));
            return rc;
        }
        if (anyz && f->kind >= 4 && n == 2 &&
            ((o[0].t == XOP_REG) == (f->kind == 4 || f->kind == 6))) {
            int load = (o[0].t == XOP_REG);
            const xop *rm = load ? &o[1] : &o[0];
            const xop *regs = load ? &o[0] : &o[1];
            int rexx = 0, rexb = 0;
            if (rm->t == XOP_REG)
                rexx = (rm->reg >> 4) & 1, rexb = (rm->reg >> 3) & 1;
            else if (rm->t == XOP_MEM) {
                if (rm->base >= 0)
                    rexb = (rm->base & 8) != 0;
                if (rm->index >= 0)
                    rexx = (rm->index & 8) != 0;
            }
            return evex_emit(out, 1, f->pp, evexw, 2, 0, rexx, rexb,
                             (regs->reg >> 4) & 1, -1, f->op, rm, regs->reg);
        }
        if (anyz && f->kind == 1 && n == 2) {
            const xop *rm = &o[1];
            int rexx = 0, rexb = 0;
            if (rm->t == XOP_REG)
                rexx = (rm->reg >> 4) & 1, rexb = (rm->reg >> 3) & 1;
            else if (rm->t == XOP_MEM) {
                if (rm->base >= 0)
                    rexb = (rm->base & 8) != 0;
                if (rm->index >= 0)
                    rexx = (rm->index & 8) != 0;
            }
            return evex_emit(out, 1, f->pp, evexw, 2, 0, rexx, rexb,
                             (o[0].reg >> 4) & 1, -1, f->op, rm, o[0].reg);
        }
        if (anyz && f->kind <= 3 && n == 3) {
            int vvvv = f->kind == 1 ? -1 : o[1].reg;
            const xop *rm = &o[2];
            int rexx = 0, rexb = 0;
            if (rm->t == XOP_REG)
                rexx = (rm->reg >> 4) & 1, rexb = (rm->reg >> 3) & 1;
            else if (rm->t == XOP_MEM) {
                if (rm->base >= 0)
                    rexb = (rm->base & 8) != 0;
                if (rm->index >= 0)
                    rexx = (rm->index & 8) != 0;
            }
            int rc = evex_emit(out, f->map == 2 ? 2 : 1, f->pp, evexw, 2,
                               0, rexx, rexb, (o[0].reg >> 4) & 1, vvvv,
                               f->op, rm, o[0].reg);
            if (f->kind == 2)
                ob1(out, imm8(c, &o[3]));
            return rc;
        }
        if (f->kind <= 3 && (n == 3 || (f->kind == 2 && n == 4))) {
            /* Operands are dst, src1 (reg), src2 (r/m) and an optional imm8.
             * Commutative operations swap the two register sources when that
             * avoids a 3-byte VEX. This is MC's rule. */
            static const char *const comm[] = {
                "vadd", "vmul", "vand", "vor", "vxor", "vpand", "vpor",
                "vpxor", "vpadd", "vpcmpeq", "vpmullw", NULL,
            };
            int iscomm = 0;
            size_t ml2 = strlen(f->mn);
            int scalar = ml2 >= 2 &&
                         (!strcmp(f->mn + ml2 - 2, "ss") ||
                          !strcmp(f->mn + ml2 - 2, "sd"));
            for (const char *const *cn = comm; *cn; cn++)
                if (!strncmp(f->mn, *cn, strlen(*cn))) {
                    size_t k = strlen(*cn);
                    /* "vand" must not match "vandn". Scalar operations never swap. */
                    if ((strcmp(*cn, "vand") || !f->mn[k] || f->mn[k] == 'p') &&
                        (strcmp(*cn, "vpand") || f->mn[k] != 'n') && !scalar)
                        iscomm = 1;
                }
            /* rm gets the register without the high bit and vvvv gets the other */
            if (iscomm && o[1].t == XOP_REG && o[2].t == XOP_REG &&
                (o[2].reg & 8) && !(o[1].reg & 8)) {
                xop tmp = o[1];
                o[1] = o[2];
                o[2] = tmp;
            }
            uint8_t ops[1] = { f->op };
            int vvvv = f->kind == 1 || f->kind == 3 ? -1 : o[1].reg;
            const xop *rm = (f->kind == 2 && n == 3) ? &o[1] : &o[2];
            if (f->kind == 2 && n == 3)
                vvvv = -1;
            int rexx = 0, rexb = 0;
            if (rm->t == XOP_REG)
                rexb = (rm->reg & 8) != 0;
            else if (rm->t == XOP_MEM) {
                if (rm->base >= 0)
                    rexb = (rm->base & 8) != 0;
                if (rm->index >= 0)
                    rexx = (rm->index & 8) != 0;
            }
            if (f->kind == 3) {
                /* general register source: W is ignored and the size comes from the operand */
            }
            int rc = vex_emit(out, f->map ? f->map : 1, f->pp,
                              f->w && f->kind != 3 ? 1 : 0,
                              l256, (o[0].reg & 8) != 0, rexx, rexb, vvvv,
                              ops, 1, rm, o[0].reg);
            if (f->kind == 2)
                ob1(out, imm8(c, n == 4 ? &o[3] : &o[2]));
            return rc;
        }
        if (f->kind <= 3 && n == 2) {
            /* two-operand VEX form (dst, rm) with vvvv=1111 */
            uint8_t ops[1] = { f->op };
            const xop *rm = &o[1];
            int rexx = 0, rexb = 0;
            if (rm->t == XOP_REG)
                rexb = (rm->reg & 8) != 0;
            else if (rm->t == XOP_MEM) {
                if (rm->base >= 0)
                    rexb = (rm->base & 8) != 0;
                if (rm->index >= 0)
                    rexx = (rm->index & 8) != 0;
            }
            int rc = vex_emit(out, f->map ? f->map : 1, f->pp,
                              f->w && f->kind != 3 ? 1 : 0,
                              l256, (o[0].reg & 8) != 0, rexx, rexb, -1,
                              ops, 1, rm, o[0].reg);
            if (f->kind == 2 && n > 2)
                ob1(out, imm8(c, &o[2]));
            return rc;
        }
        /* Kinds 4 to 7 are load and store moves. The direction comes from
         * the operands. A register to register move takes the store opcode
         * with the roles swapped. MC does this for pairs like 0F10 and 0F11. */
        if (f->kind >= 4 && n == 2 &&
            ((o[0].t == XOP_REG) == (f->kind == 4 || f->kind == 6))) {
            uint8_t opbyte = f->op;
            int load = (o[0].t == XOP_REG);
            const xop *rm = load ? &o[1] : &o[0];
            const xop *regs = load ? &o[0] : &o[1];
            if (load && o[1].t == XOP_REG && (f->kind == 4 || f->kind == 5) &&
                (o[1].reg & 8) && !(o[0].reg & 8)) {
                opbyte = f->op == 0x10 ? 0x11 : f->op == 0x28 ? 0x29 :
                         f->op == 0x6F ? 0x7F : f->op;
                rm = &o[0];
                regs = &o[1];
            }
            uint8_t ops[1] = { opbyte };
            int rexx = 0, rexb = 0;
            if (rm->t == XOP_REG)
                rexb = (rm->reg & 8) != 0;
            else if (rm->t == XOP_MEM) {
                if (rm->base >= 0)
                    rexb = (rm->base & 8) != 0;
                if (rm->index >= 0)
                    rexx = (rm->index & 8) != 0;
            }
            return vex_emit(out, 1, f->pp, 0, l256, (regs->reg & 8) != 0,
                            rexx, rexb, -1, ops, 1, rm, regs->reg);
        }
    }

    return asm__fail(c, ASM_EK_MNEMONIC, "%s: not supported yet", mn);
}

int asm_eval_literal(void *ud, const char *b, const char *e, int64_t *out,
                     char *err, size_t errcap);

int asm__hexval(int ch);

/* ---------------- front end: parse one line ---------------- */

int asmx64_insn(asm_ctx *c, const char *text, size_t len,
                uint8_t *out, size_t cap, size_t *outlen)
{
    const char *p = text, *e = text + len;
    while (p < e && isspace((unsigned char)*p))
        p++;
    int prf_lock = 0, prf_rep = 0, prf_repne = 0;
    for (;;) {
        const char *w = p;
        while (p < e && isalpha((unsigned char)*p))
            p++;
        if (span_eq(w, p, "lock")) { prf_lock = 1; while (p < e && isspace((unsigned char)*p)) p++; continue; }
        if (span_eq(w, p, "rep")) { prf_rep = 1; while (p < e && isspace((unsigned char)*p)) p++; continue; }
        if (span_eq(w, p, "repne") || span_eq(w, p, "repnz")) { prf_repne = 1; while (p < e && isspace((unsigned char)*p)) p++; continue; }
        if (span_eq(w, p, "data16")) { while (p < e && isspace((unsigned char)*p)) p++; continue; }
        p = w;
        break;
    }
    while (p < e && isspace((unsigned char)*p))
        p++;
    const char *ms = p;
    while (p < e && (isalnum((unsigned char)*p) || *p == '.'))
        p++;
    const char *me = p;
    if (ms == me)
        return bad(c, "no mnemonic");
    char mn[24];
    size_t ml = (size_t)(me - ms);
    if (ml >= sizeof mn)
        return bad(c, "mnemonic too long");
    memcpy(mn, ms, ml);
    mn[ml] = 0;

    xop ops[4];
    int nops = 0;
    const char *start = p;
    for (const char *q = asm__item_end(p, e);; q = asm__item_end(q + 1, e)) {
        {
            if (nops == 4)
                return bad(c, "too many operands");
            xop *o = &ops[nops];
            memset(o, 0, sizeof *o);
            o->base = o->index = -1;
            const char *b = start, *e2 = q;
            while (b < e2 && isspace((unsigned char)*b)) b++;
            while (e2 > b && isspace((unsigned char)e2[-1])) e2--;
            if (b != e2) {
                nops++;
                static const struct { const char *n; int s; } sk[] = {
                    { "byte", 1 }, { "word", 2 }, { "dword", 4 },
                    { "qword", 8 }, { "xmmword", 16 }, { "ymmword", 32 },
                };
                int psz = 0;
                for (unsigned i = 0; i < sizeof sk / sizeof sk[0]; i++) {
                    size_t k = strlen(sk[i].n);
                    if ((size_t)(e2 - b) > k + 4 && !memcmp(b, sk[i].n, k) &&
                        b[k] == ' ') {
                        const char *pk = b + k + 1;
                        if (pk + 3 <= e2 && !memcmp(pk, "ptr", 3) &&
                            (pk + 3 == e2 || pk[3] == ' ' || pk[3] == '[')) {
                            psz = sk[i].s;
                            b = pk + 3;
                            while (b < e2 && (*b == ' ' || *b == '\t')) b++;
                            break;
                        }
                    }
                }
                if (*b == '[') {
                    int rc2 = parse_xmem(c, b, e2, o);
                    if (rc2)
                        return rc2;
                    o->msize = psz;
                    o->explicit_size = psz != 0;
                } else {
                    if (psz)
                        return bad(c, "ptr without memory operand");
                    if (parse_xreg(b, e2, o))
                        ;
                    else if (reg_then_junk(b, e2))
                        return bad(c, "junk after register");
                    else {
                        int rc3 = evalspan(c, b, e2, &o->imm, &o->imm_known, NULL);
                        if (rc3)
                            return rc3;
                        o->t = XOP_IMM;
                    }
                }
            }
            start = q + 1;
            if (q == e)
                break;
        }
    }

    ob o;
    memset(&o, 0, sizeof o);
    int rc = asmx64_emit(c, mn, ops, nops,
                         prf_lock | (prf_rep ? 2 : 0) | (prf_repne ? 4 : 0), &o);
    if (rc)
        return rc;
    if (prf_lock && o.n < sizeof o.b) {
        memmove(o.b + 1, o.b, o.n++);
        o.b[0] = 0xF0;
        if (o.rip_at)
            o.rip_at++;
    }
    if (o.n > cap)
        return ASM_ESPACE;
    if (o.rip_at) {
        int64_t rd = asm_target(c, o.rip_to) - (int64_t)(c->addr + o.n);
        if (rd < INT32_MIN || rd > INT32_MAX)
            asm__soft(c, ASM_EK_OFFSET, "address out of reach");
        uint32_t d = (uint32_t)rd;
        for (int i = 0; i < 4; i++)
            o.b[o.rip_at - 1 + i] = (uint8_t)(d >> 8 * i);
    }
    memcpy(out, o.b, o.n);
    *outlen = o.n;
    return ASM_OK;
}
