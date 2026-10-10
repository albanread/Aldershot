/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* asma64_mv.c: the second part of the AArch64 encoder. It handles loads,
 * stores and scalar floating point.
 *
 * The constants come from the MRASM corpus goldens (see tools/gen_a64_tab.py
 * and oracles/MRASM/corpus/aarch64.tsv). Anything not handled here falls
 * through to the generated table in asma64_tab.c.
 */
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

#include "asm.h"
#include "asma64.h"
#include "asma64_int.h"

#define F(v, lo, n) (((uint32_t)((uint64_t)(v) & ((1ull << (n)) - 1))) << (lo))

int asma64_table(asm_ctx *c, const char *name, const char *base,
                 const char *sfx, a64opnd *o, int n,
                 uint8_t *out, size_t cap, size_t *outlen);

static int bad(asm_ctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->ekind = ASM_EK_SYNTAX;
    return ASM_ESYNTAX;
}

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

static int is_gpr(const a64opnd *o, int i)
{
    return o[i].t == A64_O_REG && (o[i].kind == RK_X || o[i].kind == RK_W);
}

/* Returns prfm's operation. It is a name (pldl1keep and so on) or #imm5.
 * Returns -1 if it is neither. */
int asma64_prfop(const a64opnd *o)
{
    static const struct { const char *n; int v; } pf[] = {
        { "pldl1keep", 0 }, { "pldl1strm", 1 }, { "pldl2keep", 2 },
        { "pldl2strm", 3 }, { "pldl3keep", 4 }, { "pldl3strm", 5 },
        { "plil1keep", 8 }, { "plil1strm", 9 }, { "plil2keep", 10 },
        { "plil2strm", 11 }, { "plil3keep", 12 }, { "plil3strm", 13 },
        { "pstl1keep", 16 }, { "pstl1strm", 17 }, { "pstl2keep", 18 },
        { "pstl2strm", 19 }, { "pstl3keep", 20 }, { "pstl3strm", 21 },
    };
    for (unsigned i = 0; i < sizeof pf / sizeof pf[0]; i++) {
        size_t k = strlen(pf[i].n);
        if ((size_t)(o->e - o->b) == k && !memcmp(o->b, pf[i].n, k))
            return pf[i].v;
    }
    if (o->t == A64_O_IMM && o->imm_known)
        return (int)(o->imm & 31);
    return -1;
}

/* ---------------- ldr/str ---------------- */

typedef struct {
    uint32_t size, opc, V;
    int scale;
    int kind;           /* 0 means an integer, sized by the destination. Otherwise an RK_* kind. */
} lsinfo;

static int ls_encode(asm_ctx *c, const lsinfo *li, const a64opnd *o, int n,
                     uint8_t *out, size_t cap, size_t *outlen)
{
    int memi = n - 1;
    while (memi > 0 && o[memi].t != A64_O_MEM)
        memi--;
    if (memi <= 0)
        return bad(c, "memory operand expected");
    const a64opnd *m = &o[memi];
    if (memi + 1 < n && o[memi + 1].t == A64_O_IMM && !m->wb_pre &&
        !m->wb_post && !m->idx_valid) {
        /* "ldr x0, [x1], #8" is post-indexed. The offset is a separate
         * operand. */
        a64opnd *mm = (a64opnd *)m;
        mm->disp = o[memi + 1].imm;
        mm->disp_known = o[memi + 1].imm_known;
        mm->wb_post = 1;
    }
    uint32_t size = li->size, opc = li->opc, V = li->V;
    int scale = li->scale;
    int Rt = o[0].regn, Rn = m->basereg;

    if (li->kind == 0)
        size = o[0].kind == RK_X ? 3 : 2;
    if (li->kind == 0)
        scale = 1 << size;
    else if (li->kind == RK_Q)
        scale = 16;
    if (li->kind == -1)
        opc = o[0].kind == RK_X ? 2 : 3;

    if (m->idx_valid) {
        uint32_t option = 3;  /* LSL */
        uint32_t S = 0;
        int lg = scale == 16 ? 4 : scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
        if (m->idxsh >= 0) {
            /* The amount is 0 or the log2 of the access's size. S says
             * which. For a byte, S says whether an amount is written at
             * all, as MC does. */
            int amt = m->idxsh_amt_known ? m->idxsh_amt : 0;
            if (m->idxsh_is_shift && m->idxsh != SHK_LSL)
                return asm__fail(c, ASM_EK_SHIFT, "an index takes lsl or an extend");
            if (amt != 0 && amt != lg)
                asm__soft(c, ASM_EK_SHIFT, "index shift must be #0 or #%d", lg);
            option = m->idxsh_is_shift ? 3 : (uint32_t)m->idxsh;
            S = lg ? amt != 0 : (m->idxsh_is_shift || m->idxsh_amt_known);
        }
        /* uxtw and sxtw extend a w register. lsl, uxtx and sxtx take an x register. */
        if ((option & 3) != (m->idxkind == RK_W ? 2u : 3u) ||
            (m->idxkind != RK_W && m->idxkind != RK_X))
            return asm__fail(c, ASM_EK_REGISTER, "bad index register");
        uint32_t w = F(size, 30, 2) | F(V, 26, 1) | 0x38200800u |
            F(opc, 22, 2) | F(m->idxreg, 16, 5) | F(option, 13, 3) | F(S, 12, 1) |
            F(Rn, 5, 5) | F(Rt, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (m->wb_pre || m->wb_post) {
        int64_t imm9 = m->disp_known ? m->disp : 0;
        if (m->disp_known && (imm9 < -256 || imm9 > 255))
            asm__soft(c, ASM_EK_OFFSET, "writeback offset out of range");
        uint32_t mode = m->wb_pre ? 3u : 1u;
        uint32_t w = F(size, 30, 2) | F(V, 26, 1) | 0x38000000u |
            F(opc, 22, 2) | F(imm9, 12, 9) | F(mode, 10, 2) |
            F(Rn, 5, 5) | F(Rt, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    {
        int64_t d = m->disp_known ? m->disp : 0;
        if (!m->disp_known || (d >= 0 && (d & (scale - 1)) == 0 &&
                               (d >> __builtin_ctz(scale)) <= 4095)) {
            uint32_t imm12 = m->disp_known && d > 0 ? (uint32_t)(d >> __builtin_ctz(scale)) : 0;
            uint32_t w = F(size, 30, 2) | F(V, 26, 1) | 0x39000000u |
                F(opc, 22, 2) | F(imm12, 10, 12) | F(Rn, 5, 5) | F(Rt, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (d < -256 || d > 255)
            asm__soft(c, ASM_EK_OFFSET, "offset out of range");
        uint32_t w = F(size, 30, 2) | F(V, 26, 1) | 0x38000000u |
            F(opc, 22, 2) | F(d, 12, 9) | F(0, 10, 2) |
            F(Rn, 5, 5) | F(Rt, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
}

static const lsinfo lstab[] = {
    { 3, 0, 0, 8,  0 },   /* str x/w */
    { 3, 1, 0, 8,  0 },   /* ldr x/w */
    { 0, 0, 0, 1,  RK_W },   /* strb */
    { 0, 1, 0, 1,  RK_W },   /* ldrb */
    { 1, 0, 0, 2,  RK_W },   /* strh */
    { 1, 1, 0, 2,  RK_W },   /* ldrh */
    { 2, 2, 0, 4,  RK_X },   /* ldrsw */
    { 0, 0, 0, 1, -1 },   /* ldrsb: opc is set by the destination */
    { 1, 0, 0, 2, -1 },   /* ldrsh */
    { 2, 0, 1, 4,  RK_S },   /* str s */
    { 2, 1, 1, 4,  RK_S },   /* ldr s */
    { 3, 0, 1, 8,  RK_D },   /* str d */
    { 3, 1, 1, 8,  RK_D },   /* ldr d */
    { 0, 2, 1, 16, RK_Q },   /* str q / v */
    { 0, 3, 1, 16, RK_Q },   /* ldr q / v */
};

/* Encodes the by-element forms. These are the vector form
 * (mul v0.4s, v1.4s, v2.s[3]), the long form (smull v0.4s, v1.4h, v2.h[1],
 * where smull2 takes the upper half), and the scalar floating-point form
 * (fmul s0, s1, v2.s[3]).
 *
 *   0 Q U 01111 size L M Rm opcode H 0 Rn Rd     (scalar: 01 U 11111 ...)
 *
 * The element's index is in H:L:M for .h (Rm is then v0-v15), H:L for .s
 * and H for .d. The function returns 1 when the instruction is not one of
 * these. The table then handles it. */
static int by_element(asm_ctx *c, const char *base, const a64opnd *o, int n,
                      uint8_t *out, size_t cap, size_t *outlen)
{
    static const struct { const char *n; uint32_t u, opc; int kind; } t[] = {
        /* kind 0 is integer (h, s), 1 is long (from h, s), 2 is floating point */
        { "mul", 0, 8, 0 }, { "mla", 1, 0, 0 }, { "mls", 1, 4, 0 },
        { "sqdmulh", 0, 12, 0 }, { "sqrdmulh", 0, 13, 0 },
        { "smull", 0, 10, 1 }, { "umull", 1, 10, 1 }, { "smlal", 0, 2, 1 },
        { "umlal", 1, 2, 1 }, { "smlsl", 0, 6, 1 }, { "umlsl", 1, 6, 1 },
        { "sqdmull", 0, 11, 1 }, { "sqdmlal", 0, 3, 1 }, { "sqdmlsl", 0, 7, 1 },
        { "fmla", 0, 1, 2 }, { "fmls", 0, 5, 2 }, { "fmul", 0, 9, 2 }, { "fmulx", 1, 9, 2 },
    };
    char b[16];
    size_t bl = strlen(base);
    int upper = 0;                      /* smull2 and friends */
    if (bl >= sizeof b)
        return 1;
    memcpy(b, base, bl + 1);
    if (bl > 1 && b[bl - 1] == '2')
        b[bl - 1] = 0, upper = 1;
    int k = -1;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(b, t[i].n))
            k = (int)i;
    if (k < 0 || n != 3 || o[2].t != A64_O_REG || o[2].kind != RK_V || o[2].vidx < 0 ||
        o[2].vlanes || o[0].t != A64_O_REG || o[1].t != A64_O_REG)
        return 1;
    if (upper && t[k].kind != 1)
        return 1;
    int es = o[2].vbits;                /* the element's size */
    int scalar = o[0].kind != RK_V;
    uint32_t Q = 0;
    if (scalar) {
        /* Only floating point, sqdmulh and sqrdmulh are scalar. All the
         * registers have the same size. */
        int rs = o[0].kind == RK_H ? 16 : o[0].kind == RK_S ? 32 : o[0].kind == RK_D ? 64 : 0;
        if (!rs || o[1].kind != o[0].kind || rs != es || t[k].kind == 1 ||
            (t[k].kind == 0 && t[k].opc < 12))
            return 1;
    } else {
        if (o[1].kind != RK_V || o[0].vidx >= 0 || o[1].vidx >= 0)
            return 1;
        int dbits = o[0].vbits * o[0].vlanes, nbits = o[1].vbits * o[1].vlanes;
        if (t[k].kind == 1) {
            /* The destination's elements are twice the size of the source's. */
            if (o[0].vbits != 2 * es || dbits != 128 || o[1].vbits != es ||
                nbits != (upper ? 128 : 64))
                return 1;
            Q = (uint32_t)upper;
        } else {
            if (o[0].vbits != es || o[1].vbits != es || o[0].vlanes != o[1].vlanes ||
                (dbits != 64 && dbits != 128) || (es == 64 && dbits != 128))
                return 1;
            Q = dbits == 128;
        }
    }
    uint32_t size, H, L = 0, M = 0, rm = (uint32_t)o[2].regn;
    int idx = o[2].vidx;
    if (t[k].kind == 2) {
        /* For floating point, size is 1:sz for .s and .d, and 00 for .h. */
        if (es == 16)
            size = 0;
        else if (es == 32 || es == 64)
            size = es == 32 ? 2 : 3;
        else
            return 1;
    } else {
        if (es != 16 && es != 32)
            return 1;
        size = es == 16 ? 1 : 2;
    }
    if (idx >= (es == 16 ? 8 : es == 32 ? 4 : 2))
        return asm__fail(c, ASM_EK_IMMEDIATE, "%s: element index out of range", base);
    if (es == 16) {
        if (rm > 15)
            return asm__fail(c, ASM_EK_REGISTER, "%s: v0-v15 for a .h element", base);
        H = (uint32_t)idx >> 2, L = ((uint32_t)idx >> 1) & 1, M = (uint32_t)idx & 1;
    } else if (es == 32)
        H = (uint32_t)idx >> 1, L = (uint32_t)idx & 1, M = rm >> 4;
    else
        H = (uint32_t)idx, M = rm >> 4;
    uint32_t w = (scalar ? 0x5F000000u : 0x0F000000u) | F(Q, 30, 1) | F(t[k].u, 29, 1) |
                 F(size, 22, 2) | F(L, 21, 1) | F(M, 20, 1) | F(rm, 16, 4) |
                 F(t[k].opc, 12, 4) | F(H, 11, 1) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
    return w32(c, w, out, cap, outlen);
}

int asma64_mem_fp_vec(asm_ctx *c, const char *name, const char *base,
                      const char *sfx, a64opnd *o, int n,
                      uint8_t *out, size_t cap, size_t *outlen)
{
    (void)name;
    uint32_t w;

    {
        static const char *const vnames[] = { "ins", "dup", "umov", "smov",
                                               "mov", "fmov", NULL };
        int vskip = 0;
        for (const char *const *vn = vnames; *vn; vn++)
            if (!strcmp(base, *vn))
                vskip = 1;
        if (!vskip)
            for (int i = 0; i < n; i++)
                if (o[i].t == A64_O_REG && o[i].kind == RK_V)
                    goto vectbl;
    }

    if (!strcmp(base, "ldr") || !strcmp(base, "str") ||
        !strcmp(base, "ldrb") || !strcmp(base, "strb") ||
        !strcmp(base, "ldrh") || !strcmp(base, "strh") ||
        !strcmp(base, "ldrsw") || !strcmp(base, "ldrsb") ||
        !strcmp(base, "ldrsh")) {
        int idx = -1;
        if ((!strcmp(base, "ldr") || !strcmp(base, "str")) && n >= 1 &&
            o[0].t == A64_O_REG &&
            (o[0].kind == RK_S || o[0].kind == RK_D || o[0].kind == RK_Q ||
             o[0].kind == RK_V)) {
            if (o[0].kind == RK_V) {
                if (!o[0].vbits || !o[0].vlanes)
                    return bad(c, "vector arrangement needed");
                idx = (o[0].vlanes * o[0].vbits == 128) ? 13 : 11;
            } else {
                idx = o[0].kind == RK_S ? 9 : o[0].kind == RK_D ? 11 : 13;
            }
            if (base[0] == 'l') {
                idx++;                  /* in the table, the load follows its store */
            }
        }
        if (idx < 0) {
            if (!strcmp(base, "strb")) idx = 2;
            else if (!strcmp(base, "ldrb")) idx = 3;
            else if (!strcmp(base, "strh")) idx = 4;
            else if (!strcmp(base, "ldrh")) idx = 5;
            else if (!strcmp(base, "ldrsw")) idx = 6;
            else if (!strcmp(base, "ldrsb")) idx = 7;
            else if (!strcmp(base, "ldrsh")) idx = 8;
            else {
                if (n < 1 || !is_gpr(o, 0))
                    return bad(c, "integer register expected");
                idx = base[0] == 'l' ? 1 : 0;
            }
        }
        return ls_encode(c, &lstab[idx], o, n, out, cap, outlen);
    }

    if (!strcmp(base, "ldur") || !strcmp(base, "stur") ||
        !strcmp(base, "ldursb") || !strcmp(base, "ldursh") ||
        !strcmp(base, "ldursw") || !strcmp(base, "prfm")) {
        int memi = n - 1;
        if (memi < 1 || o[memi].t != A64_O_MEM)
            return bad(c, "memory operand expected");
        const a64opnd *m = &o[memi];
        if (m->wb_pre || m->wb_post || m->idx_valid)
            return bad(c, "simple address expected");
        uint32_t size = 0, opc = 1, V = 0;
        int64_t imm9 = m->disp_known ? m->disp : 0;
        if (m->disp_known && (imm9 < -256 || imm9 > 255))
            asm__soft(c, ASM_EK_OFFSET, "offset out of range");
        int Rt = o[0].regn;
        if (!strcmp(base, "prfm")) {
            /* prfm op, [xn{, #imm}] has an unsigned immediate scaled by 8 */
            int64_t d = m->disp_known ? m->disp : 0;
            if (m->disp_known && (d < 0 || d > 32760 || (d & 7)))
                asm__soft(c, ASM_EK_OFFSET, "prfm offset out of range");
            int op = asma64_prfop(&o[0]);
            if (op < 0)
                return bad(c, "bad prefetch operation");
            uint32_t opv = (uint32_t)op;
            w = 0xF9800000u | F(opv >> 3, 30, 2) | F(0, 23, 1) |
                F(opv & 7, 0, 3) | F(d ? d / 8 : 0, 10, 12) | F(m->basereg, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
        int is_ld = base[1] == 'd';
        if (!strcmp(base, "ldur") || !strcmp(base, "stur")) {
            if (o[0].kind == RK_X) { size = 3; opc = is_ld; }
            else if (o[0].kind == RK_W) { size = 2; opc = is_ld; }
            else if (o[0].kind == RK_S) { size = 2; V = 1; opc = is_ld; }
            else if (o[0].kind == RK_D) { size = 3; V = 1; opc = is_ld; }
            else if (o[0].kind == RK_Q || o[0].kind == RK_V) { size = 0; V = 1; opc = 2 + is_ld; }
            else return bad(c, "bad register");
        } else if (!strcmp(base, "ldursw")) {
            size = 2; opc = 2;
            if (o[0].kind != RK_X) return bad(c, "x register expected");
        } else {
            size = !strcmp(base, "ldursb") ? 0 : 1;
            opc = o[0].kind == RK_X ? 2 : 1;
        }
        w = F(size, 30, 2) | F(V, 26, 1) | 0x38000000u |
            F(opc, 22, 2) | F(imm9, 12, 9) | F(m->basereg, 5, 5) | F(Rt, 0, 5);
        return w32(c, w, out, cap, outlen);
    }

    if (!strcmp(base, "ldp") || !strcmp(base, "stp") || !strcmp(base, "ldpsw")) {
        if (n != 3 || o[2].t != A64_O_MEM) {
            if (n == 4 && o[2].t == A64_O_MEM && o[3].t == A64_O_IMM) {
                a64opnd *mm = &o[2];
                mm->disp = o[3].imm;
                mm->disp_known = o[3].imm_known;
                mm->wb_post = 1;
                n = 3;
            } else
                return bad(c, "%s wants (rt1, rt2, mem)", base);
        }
        int L = base[0] == 'l';
        /* These are the signed-offset forms with L clear (STP). L is added below. */
        uint32_t b, V = 0, scale;
        if (!strcmp(base, "ldpsw")) {
            if (o[0].kind != RK_X)
                return bad(c, "ldpsw wants x registers");
            b = 0x69000000u; scale = 4;
        }
        else if (o[0].kind == RK_X) { b = 0xA9000000u; scale = 8; }
        else if (o[0].kind == RK_W) { b = 0x29000000u; scale = 4; }
        else if (o[0].kind == RK_S) { b = 0x2D000000u; V = 1; scale = 4; }
        else if (o[0].kind == RK_D) { b = 0x6D000000u; V = 1; scale = 8; }
        else if (o[0].kind == RK_Q) { b = 0xAD000000u; V = 1; scale = 16; }
        else return bad(c, "bad register pair");
        if (o[0].kind != o[1].kind)
            return bad(c, "register size mismatch");
        const a64opnd *m = &o[2];
        int64_t imm7 = m->disp_known ? m->disp / (int64_t)scale : 0;
        if (m->wb_pre) b += 0x00800000u;
        else if (m->wb_post) b -= 0x00800000u;
        if (m->disp_known && (imm7 < -64 || imm7 > 63 || m->disp % (int64_t)scale))
            asm__soft(c, ASM_EK_OFFSET, "pair offset out of range");
        w = b | F(L, 22, 1) | F(V, 26, 1) | F(imm7, 15, 7) |
            F(o[1].regn, 10, 5) | F(m->basereg, 5, 5) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }

    /* ---- exclusives ---- */
    {
        /* These are the exclusives and the load-acquire and store-release
         * forms. A b or h at the end means a byte or a halfword (ldarb,
         * stlxrh). Pairs have no such suffix. */
        static const char *const excl[] = {
            "ldxr", "ldaxr", "ldxp", "ldaxp", "stxr", "stlxr", "stxp", "stlxp",
            "ldar", "stlr", "ldapr", NULL,
        };
        char eb[8];
        size_t bl = strlen(base);
        int bsz = -1;                   /* the size from a b or h suffix, or -1 for none */
        if (bl > 4 && bl < sizeof eb && (base[bl - 1] == 'b' || base[bl - 1] == 'h') &&
            base[bl - 2] != 'p') {
            memcpy(eb, base, bl - 1);
            eb[bl - 1] = 0;
            bsz = base[bl - 1] == 'b' ? 0 : 1;
        } else if (bl < sizeof eb)
            memcpy(eb, base, bl + 1);
        else
            eb[0] = 0;
        int isex = 0;
        for (const char *const *x = excl; *x; x++)
            if (!strcmp(eb, *x))
                isex = 1;
        if (isex) {
            int L = eb[0] == 'l';
            int A = !strcmp(eb, "ldaxr") || !strcmp(eb, "ldaxp") || !strcmp(eb, "stlxr") ||
                    !strcmp(eb, "stlxp");
            int pair = eb[strlen(eb) - 1] == 'p';
            int memi = n - 1;
            if (memi < 1 || o[memi].t != A64_O_MEM)
                return bad(c, "memory operand expected");
            const a64opnd *m = &o[memi];
            /* The (first) register transferred. For a store-exclusive it
             * comes after the status register. */
            int rt0 = !L && strcmp(eb, "stlr") ? 1 : 0;
            if (rt0 >= memi || !is_gpr(o, rt0) || (!L && rt0 == 1 && !is_gpr(o, 0)))
                return bad(c, "bad %s operands", base);
            if (pair ? (bsz >= 0 || memi != rt0 + 2 || o[rt0 + 1].kind != o[rt0].kind)
                     : memi != rt0 + 1)
                return bad(c, "bad %s operands", base);
            if (bsz >= 0 && o[rt0].kind != RK_W)
                return bad(c, "%s wants a w register", base);
            uint32_t size = bsz >= 0 ? (uint32_t)bsz : o[rt0].kind == RK_X ? 3 : 2;
            if (!strcmp(eb, "ldapr")) {
                /* LDAPR is 38BFC000 | size<<30 */
                w = F(size, 30, 2) | 0x38BFC000u | F(m->basereg, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            if (!strcmp(eb, "ldar") || !strcmp(eb, "stlr")) {
                /* LDAR is 08DFFC00 | size<<30 and STLR is 089FFC00 | size<<30 */
                w = F(size, 30, 2) | 0x089FFC00u | F(L, 22, 1) |
                    F(m->basereg, 5, 5) | F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            /* LDXR is 085F7C00 and STXR is 08007C00. Rs and Rt2 are 11111
             * unless used. A pair's size is 1:sz. */
            w = F(size, 30, 2) | (L ? 0x085F7C00u : 0x08007C00u) |
                F(A, 15, 1) | F(pair, 21, 1);
            if (!L)
                w |= F(o[0].regn, 16, 5);   /* Rs */
            if (pair)
                w = (w & ~0x7C00u) | F(o[rt0 + 1].regn, 10, 5);     /* Rt2 */
            w |= F(m->basereg, 5, 5) | F(o[rt0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
    }

    /* ---- atomics ---- */
    {
        char root[12];
        if (strlen(base) >= sizeof root)
            return bad(c, "bad mnemonic");
        strcpy(root, base);
        int a = 0, l = 0, al = 0;
        for (;;) {
            size_t k = strlen(root);
            if (k >= 4 && !strcmp(root + k - 2, "al")) { al = 1; root[k - 2] = 0; break; }
            if (k >= 3 && root[k - 1] == 'a') { a = 1; root[k - 1] = 0; break; }
            if (k >= 3 && root[k - 1] == 'l') { l = 1; root[k - 1] = 0; break; }
            break;
        }
        static const struct { const char *n; uint32_t opc; } at[] = {
            { "ldadd", 0 << 12 }, { "ldclr", 1 << 12 }, { "ldeor", 2 << 12 },
            { "ldset", 3 << 12 }, { "ldsmax", 4 << 12 }, { "ldsmin", 5 << 12 },
            { "ldumax", 6 << 12 }, { "ldumin", 7 << 12 }, { "swp", 8 << 12 },
        };
        for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
            if (strcmp(root, at[i].n))
                continue;
            if (n != 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || o[2].t != A64_O_MEM)
                return bad(c, "%s wants (rs, rt, [xn])", base);
            uint32_t sz = o[0].kind == RK_X ? 3 : 2;
            w = F(sz, 30, 2) | 0x38200000u | F(a | al, 23, 1) | F(l | al, 22, 1) |
                F(o[0].regn, 16, 5) | at[i].opc | F(o[2].basereg, 5, 5) |
                F(o[1].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (!strcmp(root, "cas")) {
            if (n != 3 || !is_gpr(o, 0) || !is_gpr(o, 1) || o[2].t != A64_O_MEM)
                return bad(c, "cas wants (xs, xt, [xn])");
            uint32_t sz = o[0].kind == RK_X ? 3 : 2;
            w = F(sz, 30, 2) | 0x08A07C00u | F(al, 23, 1) | F(l | al, 22, 1) |
                F(l | al, 15, 1) |
                F(o[0].regn, 16, 5) | F(o[2].basereg, 5, 5) | F(o[1].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
    }

    /* ---- scalar FP ---- */
    if (!strcmp(base, "fmov")) {
        extern int asma64_fp_imm(asm_ctx *c, const char *b, const char *e,
                                 uint32_t *lo, uint32_t *mid, uint32_t *hi);
        uint32_t flo, fmid, fhi;
        if (n == 2 && o[0].t == A64_O_REG && o[1].t == A64_O_REG &&
            (o[0].kind == RK_S || o[0].kind == RK_D) && o[0].kind == o[1].kind) {
            int ft = o[0].kind == RK_D;
            w = 0x1E204000u | F(ft, 22, 1) | F(o[1].regn, 5, 5) |
                F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (n == 2 && o[0].t == A64_O_REG && o[1].t == A64_O_IMM &&
            (o[0].kind == RK_S || o[0].kind == RK_D) && o[1].imm_known == 2) {
            int ft = o[0].kind == RK_D;
            if (asma64_fp_imm(c, o[1].b, o[1].e, &flo, &fmid, &fhi))
                return ASM_ESYNTAX;
            w = 0x1E201000u | F(ft, 22, 1) | F(fhi, 20, 1) | F(fmid, 17, 3) |
                F(flo, 13, 4) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        /* general register to or from floating point */
        if (n == 2 && o[0].t == A64_O_REG && o[1].t == A64_O_REG &&
            ((o[0].kind == RK_S || o[0].kind == RK_D) !=
             (o[1].kind == RK_S || o[1].kind == RK_D))) {
            int fi = (o[0].kind == RK_S || o[0].kind == RK_D) ? 0 : 1;
            int gi = 1 - fi;
            int sf = o[gi].kind == RK_X;
            int ft = o[fi].kind == RK_D;
            if ((o[gi].kind != RK_W && o[gi].kind != RK_X))
                return bad(c, "bad fmov operands");
            if (ft == 0 && sf)
                return bad(c, "fmov s/w size mismatch");
            if (ft == 1 && !sf)
                return bad(c, "fmov d needs x");
            int tofp = fi == 0;
            w = 0x1E260000u | F(ft, 22, 1) | F(sf, 31, 1) |
                F(tofp, 16, 1) |
                F(o[0].regn, 0, 5) | F(o[1].regn, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
        /* fmov v0.d[0], x0   and   fmov x0, v0.d[0] */
        if (n == 2 && o[0].kind == RK_V && o[0].vidx == 0 &&
            (o[1].kind == RK_X || o[1].kind == RK_W)) {
            w = 0x1E270000u | F(1, 22, 1) | F(o[1].kind == RK_X, 31, 1) |
                F(o[0].regn, 0, 5) | F(o[1].regn, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (n == 2 && o[1].kind == RK_V && o[1].vidx == 0 &&
            (o[0].kind == RK_X || o[0].kind == RK_W)) {
            w = 0x1E260000u | F(1, 22, 1) | F(o[0].kind == RK_X, 31, 1) |
                F(o[1].regn, 0, 5) | F(o[0].regn, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
        return bad(c, "bad fmov operands");
    }

    /* ---- element moves: ins, dup, umov, smov and the mov aliases ---- */
    /* mov wd, vn.s[i] and mov xd, vn.d[i] are umov. mov vd.16b, vn.16b is orr. */
    if (!strcmp(base, "mov") && n == 2 && is_gpr(o, 0) && o[1].t == A64_O_REG &&
        o[1].kind == RK_V && o[1].vidx >= 0 && !o[1].vlanes) {
        if (!(o[0].kind == RK_W && o[1].vbits == 32) && !(o[0].kind == RK_X && o[1].vbits == 64))
            return bad(c, "mov from an element: w and .s, or x and .d");
        base = "umov";
    }
    if (!strcmp(base, "mov") && n == 2 && o[0].t == A64_O_REG && o[0].kind == RK_V &&
        o[0].vidx < 0 && o[1].t == A64_O_REG && o[1].kind == RK_V && o[1].vidx < 0) {
        if (o[0].vbits != 8 || o[1].vbits != 8 || o[0].vlanes != o[1].vlanes ||
            (o[0].vlanes != 8 && o[0].vlanes != 16))
            return bad(c, "mov between vectors: .8b or .16b");
        w = 0x0EA01C00u | F(o[0].vlanes == 16, 30, 1) | F(o[1].regn, 16, 5) |
            F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "ins") || (!strcmp(base, "mov") && n == 2 &&
        ((o[0].kind == RK_V) || (o[1].kind == RK_V)))) {
        /* ins vd.[idx], vn.[idx2]   or   ins vd.[idx], wn */
        if (o[0].kind != RK_V || o[0].vidx < 0 || !o[0].vbits || o[0].vlanes ||
            o[0].vidx >= 128 / o[0].vbits)
            return bad(c, "bad ins");
        int lg = o[0].vbits == 8 ? 0 : o[0].vbits == 16 ? 1 :
                 o[0].vbits == 32 ? 2 : 3;
        uint32_t imm5 = (1u << lg) | ((uint32_t)o[0].vidx << (lg + 1));
        if (o[1].t == A64_O_REG && (o[1].kind == RK_W || o[1].kind == RK_X)) {
            /* INS (general). For example ins v0.d[1], x2 is 0x4E181C40. It
             * takes an x register for .d and a w register otherwise. */
            if ((o[1].kind == RK_X) != (lg == 3))
                return bad(c, "bad ins");
            w = 0x4E001C00u |
                F(imm5, 16, 5) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (o[1].kind == RK_V && o[1].vidx >= 0 && o[1].vbits == o[0].vbits &&
            !o[1].vlanes && o[1].vidx < 128 / o[1].vbits) {
            /* INS (element). For example ins v0.s[1], v1.s[2] is 0x6E0C4420.
             * imm5 carries the destination index and imm4 the source's. */
            w = 0x6E000400u |
                F(imm5, 16, 5) | F((uint32_t)o[1].vidx << lg, 11, 4) |
                F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        return bad(c, "bad ins");
    }
    if (!strcmp(base, "dup") && n == 2 && o[0].kind == RK_V && o[0].vbits) {
        int lg = o[0].vbits == 8 ? 0 : o[0].vbits == 16 ? 1 :
                 o[0].vbits == 32 ? 2 : 3;
        int Q = (o[0].vbits * o[0].vlanes) == 128;
        if (o[1].kind == RK_V && o[1].vidx >= 0) {
            int lg2 = o[1].vbits == 8 ? 0 : o[1].vbits == 16 ? 1 :
                      o[1].vbits == 32 ? 2 : 3;
            if (o[1].vbits != o[0].vbits || o[1].vidx >= 128 / o[1].vbits)
                return bad(c, "bad dup element");
            uint32_t imm5 = (1u << lg2) | ((uint32_t)o[1].vidx << (lg2 + 1));
            w = 0x0E000400u | F(Q, 30, 1) |
                F(imm5, 16, 5) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (o[1].kind == RK_W || o[1].kind == RK_X) {
            /* imm5 is the destination's own element marker. A dup from a
             * general register always reads lane 0 of the source width. */
            uint32_t imm5 = 1u << lg;
            w = 0x0E000C00u | F(Q, 30, 1) |
                F(imm5, 16, 5) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        return bad(c, "bad dup");
    }
    /* umov takes a w register from .b, .h or .s, and an x register from .d
     * (Q set). smov takes a w register from .b or .h, and an x register from
     * .b, .h or .s (Q set). */
    if (!strcmp(base, "umov") && n == 2 && o[1].kind == RK_V && o[1].vidx >= 0) {
        int lg = o[1].vbits == 8 ? 0 : o[1].vbits == 16 ? 1 :
                 o[1].vbits == 32 ? 2 : 3;
        int Q = o[0].kind == RK_X;
        if (!is_gpr(o, 0) || o[1].vlanes || o[1].vbits == 0 || (Q ? lg != 3 : lg == 3) ||
            o[1].vidx >= 128 / o[1].vbits)
            return bad(c, "bad umov operands");
        uint32_t imm5 = (1u << lg) | ((uint32_t)o[1].vidx << (lg + 1));
        w = 0x0E003C00u | F(Q, 30, 1) |
            F(imm5, 16, 5) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "smov") && n == 2 && o[1].kind == RK_V && o[1].vidx >= 0) {
        int lg = o[1].vbits == 8 ? 0 : o[1].vbits == 16 ? 1 :
                 o[1].vbits == 32 ? 2 : 3;
        int Q = o[0].kind == RK_X;
        if (!is_gpr(o, 0) || o[1].vlanes || lg > (Q ? 2 : 1) || o[1].vidx >= 128 / o[1].vbits)
            return bad(c, "bad smov operands");
        uint32_t imm5 = (1u << lg) | ((uint32_t)o[1].vidx << (lg + 1));
        w = 0x0E002C00u | F(Q, 30, 1) |
            F(imm5, 16, 5) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    {
        static const struct { const char *n; uint32_t op; } t3[] = {
            { "fadd", 0x1E202800u }, { "fsub", 0x1E203800u },
            { "fmul", 0x1E200800u }, { "fdiv", 0x1E201800u },
            { "fmax", 0x1E204800u }, { "fmin", 0x1E205800u },
            { "fmaxnm", 0x1E206800u }, { "fminnm", 0x1E207800u },
            { "fnmul", 0x1E208800u },
        };
        for (unsigned i = 0; i < sizeof t3 / sizeof t3[0]; i++) {
            if (strcmp(base, t3[i].n) || n != 3)
                continue;
            int ft = o[0].kind == RK_S ? 0 : o[0].kind == RK_D ? 1 : -1;
            if (ft < 0 || o[1].kind != o[0].kind || o[2].kind != o[0].kind)
                return bad(c, "matching fp registers expected");
            w = t3[i].op | F(ft, 22, 1) | F(o[2].regn, 16, 5) |
                F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
    }
    {
        static const struct { const char *n; uint32_t op; } t2[] = {
            { "fabs", 0x1E20C000u }, { "fneg", 0x1E214000u },
            { "fsqrt", 0x1E21C000u },
        };
        for (unsigned i = 0; i < sizeof t2 / sizeof t2[0]; i++) {
            if (strcmp(base, t2[i].n) || n != 2)
                continue;
            int ft = o[0].kind == RK_S ? 0 : o[0].kind == RK_D ? 1 : -1;
            if (ft < 0 || o[1].kind != o[0].kind)
                return bad(c, "matching fp registers expected");
            w = t2[i].op | F(ft, 22, 1) | F(o[1].regn, 5, 5) |
                F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
    }
    {
        static const struct { const char *n; uint32_t op; } tm[] = {
            { "fmadd", 0x1F000000u }, { "fmsub", 0x1F008000u },
            { "fnmadd", 0x1F200000u }, { "fnmsub", 0x1F208000u },
        };
        for (unsigned i = 0; i < sizeof tm / sizeof tm[0]; i++) {
            if (strcmp(base, tm[i].n) || n != 4)
                continue;
            int ft = o[0].kind == RK_S ? 0 : o[0].kind == RK_D ? 1 : -1;
            if (ft < 0 || o[1].kind != o[0].kind || o[2].kind != o[0].kind ||
                o[3].kind != o[0].kind)
                return bad(c, "matching fp registers expected");
            w = tm[i].op | F(ft, 22, 1) | F(o[2].regn, 16, 5) | F(o[3].regn, 10, 5) |
                F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
    }
    if (!strcmp(base, "fcmp") || !strcmp(base, "fcmpe")) {
        int E = base[4] == 'e';
        if (n == 1) {
            int ft = o[0].kind == RK_S ? 0 : o[0].kind == RK_D ? 1 : -1;
            if (ft < 0)
                return bad(c, "fp register expected");
            w = 0x1E202008u | F(ft, 22, 1) | F(E, 4, 1) | F(o[0].regn, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (n == 2 && o[0].t == A64_O_IMM) {
            int ft = o[1].kind == RK_S ? 0 : o[1].kind == RK_D ? 1 : -1;
            int iszero = o[0].imm_known == 2;
            if (!iszero && o[0].imm != 0)
                return bad(c, "only #0.0 allowed");
            w = 0x1E202008u | F(ft, 22, 1) | F(E, 4, 1) | F(o[1].regn, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (n == 2 && o[1].t == A64_O_IMM) {
            int ft = o[0].kind == RK_S ? 0 : o[0].kind == RK_D ? 1 : -1;
            int iszero = o[1].imm_known == 2;
            if (!iszero && o[1].imm != 0)
                return bad(c, "only #0.0 allowed");
            w = 0x1E202008u | F(ft, 22, 1) | F(E, 4, 1) | F(o[0].regn, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
        if (n == 2) {
            int ft = o[0].kind == RK_S ? 0 : o[0].kind == RK_D ? 1 : -1;
            if (ft < 0 || o[1].kind != o[0].kind)
                return bad(c, "matching fp registers expected");
            w = 0x1E202000u | F(ft, 22, 1) | F(E, 4, 1) | F(o[1].regn, 16, 5) |
                F(o[0].regn, 5, 5);
            return w32(c, w, out, cap, outlen);
        }
    }
    if (!strcmp(base, "fcsel") && n == 4) {
        int ft = o[0].kind == RK_S ? 0 : o[0].kind == RK_D ? 1 : -1;
        if (ft < 0 || o[1].kind != o[0].kind || o[2].kind != o[0].kind)
            return bad(c, "matching fp registers expected");
        int cond;
        if (o[3].t == A64_O_SFX && o[3].is_shift == 2)
            cond = o[3].shx;
        else if (!asma64_is_cond(sfx, sfx + strlen(sfx), &cond))
            return bad(c, "fcsel needs a condition");
        w = 0x1E200C00u | F(ft, 22, 1) | F(o[2].regn, 16, 5) | F(cond, 12, 4) |
            F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }
    if (!strcmp(base, "fcvt") && n == 2) {
        /* FCVT is 1E224000. The source's type is at 23:22 and the result's is
         * at 16:15 (S is 00, D is 01, H is 11). */
        int ty[2];
        for (int i = 0; i < 2; i++)
            ty[i] = o[i].kind == RK_S ? 0 : o[i].kind == RK_D ? 1 : o[i].kind == RK_H ? 3 : -1;
        if (ty[0] < 0 || ty[1] < 0 || ty[0] == ty[1])
            return bad(c, "unsupported fcvt widths");
        w = 0x1E224000u | F(ty[1], 22, 2) | F(ty[0], 15, 2) |
            F(o[0].regn, 0, 5) | F(o[1].regn, 5, 5);

        return w32(c, w, out, cap, outlen);
    }
    {
        static const struct { const char *n; uint32_t op; int r; } t[] = {
            { "scvtf", 0x1E220000u, 0 }, { "ucvtf", 0x1E230000u, 0 },
            { "fcvtzs", 0x1E380000u, 1 }, { "fcvtzu", 0x1E390000u, 1 },
            { "fcvtas", 0x1E240000u, 1 }, { "fcvtau", 0x1E250000u, 1 },
            { "fcvtms", 0x1E300000u, 1 }, { "fcvtmu", 0x1E310000u, 1 },
            { "fcvtns", 0x1E200000u, 1 }, { "fcvtnu", 0x1E210000u, 1 },
            { "fcvtps", 0x1E280000u, 1 }, { "fcvtpu", 0x1E290000u, 1 },
            { "frintn", 0x1E244000u, 2 }, { "frinta", 0x1E264000u, 2 },
            { "frintp", 0x1E24C000u, 2 }, { "frintm", 0x1E254000u, 2 },
            { "frintz", 0x1E25C000u, 2 }, { "frinti", 0x1E27C000u, 2 },
            { "frintx", 0x1E274000u, 2 },
        };
        static const struct { const char *n; uint32_t b32, b64; } fx[] = {
            { "scvtf", 0x1E020000u, 0x1E420000u },
            { "ucvtf", 0x1E030000u, 0x1E430000u },
            { "fcvtzs", 0x1E180000u, 0x1E580000u },
            { "fcvtzu", 0x1E190000u, 0x1E590000u },
        };
        for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
            if (strcmp(base, fx[i].n) || n != 3 || o[2].t != A64_O_IMM)
                continue;
            int fi = (o[0].kind == RK_S || o[0].kind == RK_D) ? 0 : 1;
            int gi = 1 - fi;
            int ft = o[fi].kind == RK_S ? 0 : 1;
            int sf = o[gi].kind == RK_X;
            if (!o[2].imm_known || o[2].imm < 1 || o[2].imm > 64)
                return bad(c, "bad fbits");
            w = (ft ? fx[i].b64 : fx[i].b32) | F(sf, 31, 1) |
                F(64 - o[2].imm, 10, 6) | F(o[1].regn, 5, 5) | F(o[0].regn, 0, 5);
            return w32(c, w, out, cap, outlen);
        }
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            if (strcmp(base, t[i].n))
                continue;
            if (t[i].r == 2) {
                if (n != 2 || o[0].kind != o[1].kind ||
                    (o[0].kind != RK_S && o[0].kind != RK_D))
                    return bad(c, "matching fp registers expected");
                int ft = o[0].kind == RK_S ? 0 : 1;
                w = t[i].op | F(ft, 22, 1) | F(o[1].regn, 5, 5) |
                    F(o[0].regn, 0, 5);
                return w32(c, w, out, cap, outlen);
            }
            if (n != 2 && n != 3)
                return bad(c, "bad operands");
            int fi = t[i].r ? 1 : 0, gi = t[i].r ? 0 : 1;
            int ft = o[fi].kind == RK_S ? 0 : o[fi].kind == RK_D ? 1 : -1;
            if (ft < 0)
                return bad(c, "fp register expected");
            int sf = o[gi].kind == RK_X;
            if (o[gi].kind != RK_W && o[gi].kind != RK_X)
                return bad(c, "integer register expected");
            w = t[i].op | F(ft, 22, 1) | F(sf, 31, 1) |
                F(o[0].regn, 0, 5) | F(o[1].regn, 5, 5);
            if (n == 3) {
                if (o[2].t != A64_O_IMM || !o[2].imm_known ||
                    o[2].imm < 0 || o[2].imm > 63)
                    return bad(c, "bad fbits");
                w |= F(o[2].imm, 10, 6);
            }
            return w32(c, w, out, cap, outlen);
        }
    }

    /* ---- mrs / msr ---- */
    if (!strcmp(base, "msr") && n == 2 && o[1].t == A64_O_IMM &&
        o[0].t == A64_O_SFX && o[0].is_shift == 3) {
        static const struct { const char *n; uint32_t b; uint32_t op2; } pim[] = {
            { "daifset", 0xD5034000u, 6 }, { "daifclr", 0xD5034000u, 7 },
            { "spsel", 0xD5104000u, 3 }, { "uaopset", 0xD5144000u, 0 },
        };
        for (unsigned i = 0; i < sizeof pim / sizeof pim[0]; i++) {
            size_t k = strlen(pim[i].n);
            if ((size_t)(o[0].e - o[0].b) == k && !memcmp(o[0].b, pim[i].n, k)) {
                if (!o[1].imm_known || o[1].imm < 0 || o[1].imm > 15)
                    return bad(c, "bad pstate immediate");
                w = pim[i].b | F(o[1].imm, 8, 4) | F(pim[i].op2, 5, 3) | 31u;
                return w32(c, w, out, cap, outlen);
            }
        }
    }
    if (!strcmp(base, "mrs") || !strcmp(base, "msr")) {
        static const struct { const char *n; uint32_t o0, o1, cn, cm, o2; } named[] = {
            { "nzcv", 3, 3, 4, 2, 0 }, { "daif", 3, 3, 4, 2, 1 },
            { "fpcr", 3, 3, 4, 4, 0 }, { "fpsr", 3, 3, 4, 4, 1 },
            { "currentel", 3, 3, 4, 2, 2 }, { "spsel", 3, 3, 4, 2, 0 },
            { "daifset", 3, 3, 4, 2, 6 }, { "daifclr", 3, 3, 4, 2, 7 },
            { "spsr_irq", 3, 3, 4, 3, 0 }, { "elr_irq", 3, 3, 4, 3, 1 },
            { "spsr_abt", 3, 3, 4, 5, 0 }, { "elr_abt", 3, 3, 4, 5, 1 },
            { "spsr_und", 3, 3, 4, 6, 0 }, { "elr_und", 3, 3, 4, 6, 1 },
            { "spsr_fiq", 3, 3, 4, 7, 0 }, { "elr_fiq", 3, 3, 4, 7, 1 },
            { "tpidr_el0", 3, 3, 13, 0, 2 }, { "tpidrro_el0", 3, 3, 13, 3, 3 },
            { "tpidr_el1", 3, 0, 13, 0, 2 },
            { "cntpct_el0", 3, 3, 14, 0, 1 }, { "cntvct_el0", 3, 3, 14, 0, 2 },
            { "cntp_tval_el0", 3, 3, 14, 2, 0 }, { "cntp_ctl_el0", 3, 3, 14, 2, 1 },
            { "cntp_cval_el0", 3, 3, 14, 2, 2 },
            { "pmccntr_el0", 3, 3, 9, 13, 0 }, { "ctr_el0", 3, 3, 0, 0, 1 },
            { "dczid_el0", 3, 3, 0, 0, 7 }, { "midr_el1", 3, 0, 0, 0, 0 },
            { "mpidr_el1", 3, 0, 0, 0, 5 },
        };
        uint32_t op0 = 3, op1 = 3, crn = 4, crm = 2, op2 = 0;
        const a64opnd *reg = NULL;
        const char *sb, *se;
        if (!strcmp(base, "mrs")) {
            if (n != 2 || !is_gpr(o, 0))
                return bad(c, "mrs wants (xt, sysreg)");
            reg = &o[0];
            sb = o[1].b; se = o[1].e;
        } else {
            if (n != 2 || !is_gpr(o, 1))
                return bad(c, "msr wants (sysreg, xt)");
            reg = &o[1];
            sb = o[0].b; se = o[0].e;
        }
        while (sb < se && isspace((unsigned char)*sb)) sb++;
        while (se > sb && isspace((unsigned char)se[-1])) se--;
        int found = 0;
        for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++) {
            size_t k = strlen(named[i].n);
            if ((size_t)(se - sb) == k && !memcmp(sb, named[i].n, k)) {
                op0 = named[i].o0; op1 = named[i].o1; crn = named[i].cn;
                crm = named[i].cm; op2 = named[i].o2;
                found = 1;
                break;
            }
        }
        if (!found) {
            unsigned a, b, cc, d, e2;
            if (sb < se && (*sb == 's' || *sb == 'S') &&
                sscanf(sb + 1, "%u_%u_C%u_C%u_%u", &a, &b, &cc, &d, &e2) == 5 &&
                (a == 2 || a == 3)) {
                op0 = a; op1 = b; crn = cc; crm = d; op2 = e2;
                found = 1;
            }
        }
        if (!found)
            return bad(c, "unknown system register");
        w = (!strcmp(base, "mrs") ? 0xD5320000u : 0xD5100000u) |
            F(op0 - 2, 19, 2) | F(op1, 16, 3) | F(crn, 12, 4) |
            F(crm, 8, 4) | F(op2, 5, 3) | F(reg->regn, 0, 5);
        return w32(c, w, out, cap, outlen);
    }

    /* Everything else goes to the generated table: NEON, crc, crypto and
     * element moves. */
vectbl:
    {
        int rc = by_element(c, base, o, n, out, cap, outlen);
        if (rc != 1)
            return rc;
    }
    {
        extern int asma64_table(asm_ctx *c, const char *name, const char *base,
                                const char *sfx, a64opnd *o, int n,
                                uint8_t *out, size_t cap, size_t *outlen);
        int rc = asma64_table(c, name, base, sfx, o, n, out, cap, outlen);
        if (rc == -3)
            return asm__fail(c, ASM_EK_MNEMONIC, "%s: no encoder for these operands", base);
        if (rc == -4)
            return asm__fail(c, ASM_EK_FORM, "%s: no form for these operands", base);
        return rc;
    }
}

/* Encodes a floating-point immediate from the set that MC can encode. On
 * success it returns 0 and sets the three fields: the sign (1 bit), the
 * exponent (3 bits, signed, and 1 is added to it) and the fraction (4 bits).
 * The value is (-1)^s * (16+f)/16 * 2^(e_signed+1). */
int asma64_fp_imm(asm_ctx *c, const char *b, const char *e,
                  uint32_t *flo, uint32_t *fmid, uint32_t *fhi)
{
    char buf[40];
    size_t k = (size_t)(e - b);
    if (k >= sizeof buf)
        return bad(c, "bad float");
    while (b < e && (*b == '#' || isspace((unsigned char)*b)))
        b++, k--;
    memcpy(buf, b, k);
    buf[k] = 0;
    double d = strtod(buf, NULL);
    if (d == 0.0 || d != d)
        return bad(c, "float not encodable");
    int neg = d < 0;
    double a = neg ? -d : d;
    for (int e3 = 3; e3 >= -4; e3--) {
        double n = a * 16.0;
        int e = e3 + 1;
        for (int k = 0; k < e; k++) n /= 2;
        for (int k = 0; k > e; k--) n *= 2;
        if (n >= 16.0 && n <= 31.0 && n == (double)(int)n) {
            *fhi = (uint32_t)neg;
            *fmid = (uint32_t)(e3 & 7);
            *flo = (uint32_t)((int)n - 16);
            return 0;
        }
    }
    return bad(c, "float not encodable");
}
