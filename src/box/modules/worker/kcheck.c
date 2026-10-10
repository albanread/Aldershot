/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* kcheck.c: the checker that Worker_LoadKernel runs over an application
 * kernel's code. It covers AArch64 (the Apple Silicon box) and x86-64 (the
 * Intel box). It guarantees that the code that runs is the
 * code that was checked.
 *
 * A kernel is pure. It computes over its argument words and the buffers
 * they name, and then returns. The checker reads the code once, from start
 * to end, one instruction at a time. It refuses these:
 *
 *   - A system call or a trap into the OS: svc, hvc, smc and hlt; syscall,
 *     sysenter, int n, int1 and iret. (brk, udf, int3 and ud2 are allowed.
 *     They fault, which fails the job.)
 *   - An instruction that reads or writes a system register or the
 *     machine's state. On AArch64 these are mrs, msr, sys, sysl and the
 *     rest of the system space, except the hints and the barriers. On
 *     x86-64 they are mov to or from a segment or control register, pushf
 *     and popf, in and out, cli, sti, hlt, rdmsr, wrmsr, fxrstor, ldmxcsr,
 *     xsave, xrstor, wrfsbase and its kin.
 *   - On AArch64, x18 in any general register field. x18 is the static
 *     base, which a kernel neither has nor may write. On x86-64, the %fs
 *     and %gs prefixes, which give the thread pointer and the static base.
 *   - An indirect branch or call. On AArch64 these are br, blr, and ret to
 *     any register but x30. On x86-64 they are jmp and call through a
 *     register or memory, far ones, and far returns.
 *   - A PC-relative reference outside the kernel's own image, which
 *     includes a branch outside its code. On x86-64, also an absolute
 *     memory address (moffs, or a disp32 with no base). Code can reach
 *     memory only through the addresses its arguments give it, or its own
 *     read-only data.
 *
 * x86-64 instruction lengths are decoded as roscc's src/x86dec.rs decodes
 * them, and the tables are the same. AArch64 words are fixed width and are
 * classified by their encoding groups (Arm ARM, C4.1). What the checker
 * cannot know is what memory the kernel writes. It does not stop a kernel
 * from writing any arena memory it can address, because a kernel is the
 * application's own code, trusted as the application is.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kcheck.h"

static uint32_t refuse(char *why, size_t size, uint32_t off, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (why && size)
        vsnprintf(why, size, fmt, ap);
    va_end(ap);
    return off + 1;
}

#define F(w, at, n) (((w) >> (at)) & ((1u << (n)) - 1u))

static int64_t sext(uint64_t v, unsigned bits)
{
    uint64_t m = 1ull << (bits - 1);
    v &= (m << 1) - 1;
    return (int64_t)((v ^ m) - m);
}

/* ---- AArch64 ------------------------------------------------------------------------ */

struct a64 {
    uint32_t base, end, lo, hi;
    char *why;
    size_t size;
};

/* Returns 1, with why written, if a checked field is x18. Otherwise 0. */
static int x18(const struct a64 *k, uint32_t w, int fields)
{
    /* fields: 1 Rd/Rt (0-4), 2 Rn (5-9), 4 Rt2/Ra (10-14), 8 Rm/Rs (16-20) */
    static const unsigned at[4] = { 0, 5, 10, 16 };
    for (unsigned i = 0; i < 4; i++)
        if ((fields & (1 << i)) && F(w, at[i], 5) == 18) {
            snprintf(k->why, k->size, "uses x18 (%08X), the static base: a kernel has no statics", w);
            return 1;
        }
    return 0;
}

static int target(const struct a64 *k, uint32_t w, int64_t t, int code)
{
    if (code ? (t >= k->base && t < k->end && !(t & 3)) : (t >= k->lo && t < k->hi))
        return 0;
    snprintf(k->why, k->size, "%s outside the kernel (%08X, to &%llX)",
             code ? "branches" : "refers to memory", w, (unsigned long long)(uint32_t)t);
    return 1;
}

/* One instruction at pc. Returns 0 if it passes, or 1 if it is refused,
 * with why written. */
static int a64_one(const struct a64 *k, uint32_t w, uint32_t pc)
{
    unsigned op0 = F(w, 25, 4);
    unsigned V = F(w, 26, 1);
    if (op0 == 0) {
        if ((w & 0xFFFF0000u) == 0)
            return 0;                   /* udf: a trap, the job fails */
        snprintf(k->why, k->size, "an SME or reserved instruction (%08X)", w);
        return 1;
    }
    if (op0 == 1 || op0 == 2 || op0 == 3) {
        snprintf(k->why, k->size, "%s (%08X)", op0 == 2 ? "an SVE instruction" : "not an instruction", w);
        return 1;
    }
    if (op0 == 8 || op0 == 9) {         /* data processing, immediate */
        if (F(w, 23, 3) <= 1) {         /* adr, adrp */
            int64_t imm = sext(F(w, 5, 19) << 2 | F(w, 29, 2), 21);
            int64_t t = (w >> 31) ? (int64_t)(pc & ~0xFFFu) + imm * 4096 : (int64_t)pc + imm;
            if ((w >> 31) && t >= (int64_t)(k->lo & ~0xFFFu) && t < k->hi)
                return x18(k, w, 1);    /* the page holds part of the image */
            return target(k, w, t, 0) || x18(k, w, 1);
        }
        if (F(w, 23, 3) == 5)
            return x18(k, w, 1);        /* movz, movn, movk: an immediate */
        return x18(k, w, 1 | 2 | (F(w, 23, 3) == 7 ? 8 : 0));  /* extr: Rm */
    }
    if (op0 == 10 || op0 == 11) {       /* branches, exceptions, system */
        if ((w & 0x7C000000u) == 0x14000000u)                           /* b, bl */
            return target(k, w, (int64_t)pc + sext(F(w, 0, 26), 26) * 4, 1);
        if ((w & 0x7E000000u) == 0x34000000u)                           /* cbz, cbnz */
            return x18(k, w, 1) || target(k, w, (int64_t)pc + sext(F(w, 5, 19), 19) * 4, 1);
        if ((w & 0x7E000000u) == 0x36000000u)                           /* tbz, tbnz */
            return x18(k, w, 1) || target(k, w, (int64_t)pc + sext(F(w, 5, 14), 14) * 4, 1);
        if ((w & 0xFF000000u) == 0x54000000u)                           /* b.cond */
            return target(k, w, (int64_t)pc + sext(F(w, 5, 19), 19) * 4, 1);
        if ((w & 0xFF000000u) == 0xD4000000u) {
            if ((w & 0xFFE0001Fu) == 0xD4200000u)
                return 0;                                               /* brk: a trap */
            snprintf(k->why, k->size, "a system call or exception (%08X: svc, hvc, smc, hlt)", w);
            return 1;
        }
        if ((w & 0xFFC00000u) == 0xD5000000u) {
            if ((w & 0xFFFFF01Fu) == 0xD503201Fu)
                return 0;                                               /* the hints: nop, yield */
            if ((w & 0xFFFFF01Fu) == 0xD503301Fu && (F(w, 5, 3) == 2 || F(w, 5, 3) == 4 ||
                                                       F(w, 5, 3) == 5 || F(w, 5, 3) == 6))
                return 0;                                               /* clrex, dsb, dmb, isb */
            snprintf(k->why, k->size, "a system instruction or register (%08X: mrs, msr, sys)", w);
            return 1;
        }
        if ((w & 0xFE000000u) == 0xD6000000u) {
            if (w == 0xD65F03C0u)
                return 0;                                               /* ret */
            snprintf(k->why, k->size, "an indirect branch (%08X: br, blr, or ret to another register "
                     "than x30)", w);
            return 1;
        }
        snprintf(k->why, k->size, "not a known branch (%08X)", w);
        return 1;
    }
    if ((op0 & 5) == 4) {               /* loads and stores */
        unsigned b2928 = F(w, 28, 2), b24 = F(w, 24, 1), b21 = F(w, 21, 1), b1110 = F(w, 10, 2);
        switch (b2928) {
        case 0:
            if (V)                      /* ld1-ld4, st1-st4: Rn, and Rm post-indexed */
                return x18(k, w, 2 | (F(w, 23, 1) ? 8 : 0));
            return x18(k, w, 1 | 2 | 4 | 8);    /* exclusive, ordered, compare and swap */
        case 1:
            if (!b24) {                 /* a literal load */
                int prfm = F(w, 30, 2) == 3 && !V;
                return target(k, w, (int64_t)pc + sext(F(w, 5, 19), 19) * 4, 0) ||
                       (!V && !prfm && x18(k, w, 1));
            }
            return x18(k, w, 1 | 2 | 8);        /* the rest: every register */
        case 2:                         /* pairs */
            return x18(k, w, 2 | (V ? 0 : 1 | 4));
        default:
            if (b24 || !b21)            /* an immediate offset, any indexing */
                return x18(k, w, 2 | (V ? 0 : 1));
            if (b1110 == 2)             /* a register offset */
                return x18(k, w, 2 | 8 | (V ? 0 : 1));
            if (b1110 == 0)             /* the atomics */
                return x18(k, w, 1 | 2 | 8);
            return x18(k, w, 1 | 2);    /* ldraa, ldrab */
        }
    }
    if ((op0 & 7) == 5) {               /* data processing, registers */
        int fields = 1 | 2 | 8;
        if (F(w, 28, 1)) {
            unsigned op = F(w, 21, 4);
            if (op == 0 && F(w, 10, 6))
                fields &= ~8;           /* rmif, setf: no Rm */
            else if (op == 2 && F(w, 11, 1))
                fields &= ~8;           /* ccmp, ccmn with an immediate */
            else if (op == 6 && F(w, 30, 1))
                fields &= ~8;           /* one source */
            else if (op & 8)
                fields |= 4;            /* three sources: Ra */
        }
        return x18(k, w, fields);
    }
    /* SIMD and floating point: general registers only in the moves and
     * conversions between the two files */
    if ((w & 0x5F000000u) == 0x1E000000u) { /* scalar FP (M = 0, S = 0) */
        unsigned op = F(w, 16, 3);
        if (F(w, 21, 1) && F(w, 10, 6) == 0)                /* to and from integers */
            return x18(k, w, op == 2 || op == 3 || op == 7 ? 2 : 1);
        if (!F(w, 21, 1))                                   /* to and from fixed point */
            return x18(k, w, op == 2 || op == 3 ? 2 : op <= 1 ? 1 : 0);
        return 0;
    }
    if ((w & 0x9FE08400u) == 0x0E000400u && !F(w, 29, 1)) {    /* SIMD copy */
        unsigned imm4 = F(w, 11, 4);
        if (imm4 == 1 || imm4 == 3)
            return x18(k, w, 2);        /* dup, ins from a general register */
        if (imm4 == 5 || imm4 == 7)
            return x18(k, w, 1);        /* smov, umov */
    }
    return 0;
}

static uint32_t check_a64(const uint8_t *code, uint32_t len, uint32_t base, uint32_t lo, uint32_t hi,
                          char *why, size_t size)
{
    struct a64 k = { base, base + len, lo, hi, why, size };
    if ((base & 3) || (len & 3))
        return refuse(why, size, 0, "AArch64 code must be whole words, word aligned");
    for (uint32_t off = 0; off < len; off += 4) {
        uint32_t w = (uint32_t)code[off] | (uint32_t)code[off + 1] << 8 | (uint32_t)code[off + 2] << 16 |
                     (uint32_t)code[off + 3] << 24;
        if (a64_one(&k, w, base + off))
            return off + 1;
    }
    return 0;
}

/* ---- x86-64 ------------------------------------------------------------------------- */

/* The kinds of immediate (as in roscc's x86dec.rs) */
enum { I_NONE, I_B, I_W, I_Z, I_V, I_WB, I_MOFFS };

/* The one-byte opcode map. Returns 1 and sets *modrm and *imm for a valid
 * opcode. Returns 0 for a prefix, an escape or an invalid opcode. */
static int one_byte(uint8_t op, int *modrm, int *imm)
{
    *modrm = 0, *imm = I_NONE;
    if (op <= 0x3F) {
        switch (op & 7) {
        case 0: case 1: case 2: case 3: *modrm = 1; return 1;
        case 4: *imm = I_B; return 1;
        case 5: *imm = I_Z; return 1;
        default: return 0;
        }
    }
    if (op >= 0x50 && op <= 0x5F) return 1;
    switch (op) {
    case 0x63: *modrm = 1; return 1;
    case 0x68: *imm = I_Z; return 1;
    case 0x69: *modrm = 1, *imm = I_Z; return 1;
    case 0x6A: *imm = I_B; return 1;
    case 0x6B: *modrm = 1, *imm = I_B; return 1;
    case 0x80: *modrm = 1, *imm = I_B; return 1;
    case 0x81: *modrm = 1, *imm = I_Z; return 1;
    case 0x83: *modrm = 1, *imm = I_B; return 1;
    case 0xA8: *imm = I_B; return 1;
    case 0xA9: *imm = I_Z; return 1;
    case 0xC0: case 0xC1: *modrm = 1, *imm = I_B; return 1;
    case 0xC2: case 0xCA: *imm = I_W; return 1;
    case 0xC3: case 0xC9: case 0xCB: case 0xCC: case 0xCF: return 1;
    case 0xC6: *modrm = 1, *imm = I_B; return 1;
    case 0xC7: *modrm = 1, *imm = I_Z; return 1;
    case 0xC8: *imm = I_WB; return 1;
    case 0xCD: *imm = I_B; return 1;
    case 0xD7: return 1;
    case 0xE8: case 0xE9: *imm = I_Z; return 1;
    case 0xEB: *imm = I_B; return 1;
    case 0xF1: case 0xF4: case 0xF5: return 1;
    case 0xF6: case 0xF7: case 0xFE: case 0xFF: *modrm = 1; return 1;
    }
    if (op >= 0x6C && op <= 0x6F) return 1;
    if (op >= 0x70 && op <= 0x7F) { *imm = I_B; return 1; }
    if (op >= 0x84 && op <= 0x8F) { *modrm = 1; return 1; }
    if ((op >= 0x90 && op <= 0x99) || (op >= 0x9B && op <= 0x9F)) return 1;
    if (op >= 0xA0 && op <= 0xA3) { *imm = I_MOFFS; return 1; }
    if ((op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF)) return 1;
    if (op >= 0xB0 && op <= 0xB7) { *imm = I_B; return 1; }
    if (op >= 0xB8 && op <= 0xBF) { *imm = I_V; return 1; }
    if (op >= 0xD0 && op <= 0xD3) { *modrm = 1; return 1; }
    if (op >= 0xD8 && op <= 0xDF) { *modrm = 1; return 1; }
    if (op >= 0xE0 && op <= 0xE7) { *imm = I_B; return 1; }
    if (op >= 0xEC && op <= 0xEF) return 1;
    if (op >= 0xF8 && op <= 0xFD) return 1;
    return 0;
}

/* The two-byte opcode map, for opcodes after a 0F escape */
static int two_byte(uint8_t op, int *modrm, int *imm)
{
    *modrm = 0, *imm = I_NONE;
    if (op <= 0x03 || op == 0x0D || (op >= 0x10 && op <= 0x1F) || (op >= 0x20 && op <= 0x23) ||
        (op >= 0x28 && op <= 0x2F) || (op >= 0x40 && op <= 0x6F) || (op >= 0x74 && op <= 0x76) ||
        op == 0x78 || op == 0x79 || (op >= 0x7C && op <= 0x7F) || (op >= 0x90 && op <= 0x9F) ||
        op == 0xA3 || op == 0xA5 || op == 0xAB || (op >= 0xAD && op <= 0xAF) ||
        (op >= 0xB0 && op <= 0xB9) || (op >= 0xBB && op <= 0xBF) || op == 0xC0 || op == 0xC1 ||
        op == 0xC3 || op == 0xC7 || op >= 0xD0) {
        *modrm = 1;
        return 1;
    }
    if ((op >= 0x05 && op <= 0x09) || op == 0x0B || op == 0x0E || (op >= 0x30 && op <= 0x35) ||
        op == 0x37 || op == 0x77 || (op >= 0xA0 && op <= 0xA2) || (op >= 0xA8 && op <= 0xAA) ||
        (op >= 0xC8 && op <= 0xCF))
        return 1;
    if (op == 0x0F || (op >= 0x70 && op <= 0x73) || op == 0xA4 || op == 0xAC || op == 0xBA ||
        op == 0xC2 || (op >= 0xC4 && op <= 0xC6)) {
        *modrm = 1, *imm = I_B;
        return 1;
    }
    if (op >= 0x80 && op <= 0x8F) {
        *imm = I_Z;
        return 1;
    }
    return 0;
}

static int legacy_prefix(uint8_t b)
{
    return b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
           b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67;
}

/* One instruction at code[0, avail). Returns its length, or 0 with why
 * written. */
static unsigned x64_one(const uint8_t *c, uint32_t avail, uint32_t pc, uint32_t base, uint32_t end,
                        uint32_t lo, uint32_t hi, char *why, size_t size)
{
#define AT(n) ((n) < avail ? c[(n)] : (snprintf(why, size, "the code ends inside an instruction"), -1))
#define NO(...) do { snprintf(why, size, __VA_ARGS__); return 0; } while (0)
    unsigned i = 0;
    int seg = 0, opsize = 0, addr32 = 0, rex_w = 0, rep = 0;
    for (;;) {
        int b = AT(i);
        if (b < 0)
            return 0;
        if (legacy_prefix((uint8_t)b)) {
            if (b == 0x64 || b == 0x65)
                seg = b;
            else if (b == 0x66)
                opsize = 1;
            else if (b == 0x67)
                addr32 = 1;
            else if (b == 0xF3)
                rep = 1;
            rex_w = 0;
            i++;
        } else if ((b & 0xF0) == 0x40) {
            rex_w = (b & 8) != 0;
            i++;
            int n = AT(i);
            if (n < 0)
                return 0;
            if (legacy_prefix((uint8_t)n) || (n & 0xF0) == 0x40)
                continue;
            break;
        } else {
            break;
        }
        if (i >= 15)
            NO("more than 14 prefixes");
    }
    if (seg)
        NO("a %s segment prefix: the thread pointer and the static base are not a kernel's",
           seg == 0x64 ? "%fs" : "%gs");
    int op = AT(i);
    if (op < 0)
        return 0;
    i++;
    int modrm = 0, imm = I_NONE, f6f7 = 0, map = 0, op2 = -1;
    if (op == 0x0F) {
        op2 = AT(i);
        if (op2 < 0)
            return 0;
        i++;
        if (op2 == 0x38 || op2 == 0x3A) {
            if (AT(i) < 0)
                return 0;
            i++;
            modrm = 1, imm = op2 == 0x3A ? I_B : I_NONE;
            map = op2 == 0x38 ? 2 : 3;
        } else {
            if (!two_byte((uint8_t)op2, &modrm, &imm))
                NO("0F %02X: not an instruction in 64-bit mode", op2);
            map = 1;
        }
    } else if (op == 0xC4 || op == 0xC5) {
        int vmap = 1;
        if (op == 0xC5) {
            if (AT(i) < 0)
                return 0;
            i++;
        } else {
            int p1 = AT(i);
            if (p1 < 0 || AT(i + 1) < 0)
                return 0;
            i += 2;
            vmap = p1 & 0x1F;
        }
        int vop = AT(i);
        if (vop < 0)
            return 0;
        i++;
        if (vmap == 1) {
            if (vop == 0x77)
                modrm = 0;
            else
                modrm = 1, imm = (vop >= 0x70 && vop <= 0x73) || vop == 0xC2 || (vop >= 0xC4 && vop <= 0xC6)
                                     ? I_B : I_NONE;
            if (vop == 0xAE)
                NO("VEX 0F AE (vldmxcsr, vstmxcsr): the floating point control is the worker's");
        } else if (vmap == 2) {
            modrm = 1;
        } else if (vmap == 3) {
            modrm = 1, imm = I_B;
        } else {
            NO("VEX map %d: not decoded", vmap);
        }
        map = 4;                        /* VEX: no further checks on the opcode */
    } else if (op == 0x62) {
        NO("EVEX (62): not decoded");
    } else if (op == 0x8F && AT(i) >= 0 && ((c[i] >> 3) & 7) != 0) {
        NO("XOP (8F with a map): not decoded");
    } else {
        if (!one_byte((uint8_t)op, &modrm, &imm))
            NO("%02X: not an instruction in 64-bit mode", op);
        f6f7 = op == 0xF6 || op == 0xF7;
    }
    unsigned mod = 0, reg = 0, rm = 0, ripdisp = 0, absolute = 0;
    unsigned disp_at = 0, dsize = 0;
    if (modrm) {
        int m = AT(i);
        if (m < 0)
            return 0;
        i++;
        mod = (unsigned)m >> 6, reg = ((unsigned)m >> 3) & 7, rm = (unsigned)m & 7;
        if (f6f7 && reg < 2)
            imm = op == 0xF6 ? I_B : I_Z;   /* test r/m, imm */
        if (mod != 3) {
            dsize = mod == 1 ? 1 : mod == 2 ? 4 : 0;
            if (rm == 4) {
                int sib = AT(i);
                if (sib < 0)
                    return 0;
                i++;
                if (mod == 0 && (sib & 7) == 5)
                    dsize = 4, absolute = 1;    /* no base: disp32 (+ an index) */
            } else if (mod == 0 && rm == 5) {
                dsize = 4, ripdisp = 1;         /* %rip + disp32 */
            }
            if (dsize) {
                disp_at = i;
                i += dsize;
            }
        }
    }
    unsigned isize = 0;
    switch (imm) {
    case I_B: isize = 1; break;
    case I_W: isize = 2; break;
    case I_WB: isize = 3; break;
    case I_Z: isize = opsize ? 2 : 4; break;
    case I_V: isize = rex_w ? 8 : opsize ? 2 : 4; break;
    case I_MOFFS: isize = addr32 ? 4 : 8; break;
    }
    unsigned imm_at = i;
    i += isize;
    if (i > 15)
        NO("%u bytes: longer than an instruction can be", i);
    if (i > avail)
        NO("the code ends inside an instruction");
    uint32_t next = pc + i;

    /* what it is */
    if (map == 0) {
        switch (op) {
        case 0xCD: NO("int &%02X: a system call", c[imm_at]);
        case 0xF1: NO("int1: a trap into the OS");
        case 0xCF: NO("iret");
        case 0xCA: case 0xCB: NO("a far return");
        case 0xF4: NO("hlt");
        case 0xFA: case 0xFB: NO("cli or sti: the machine's state");
        case 0x9C: case 0x9D: NO("pushf or popf: the flags register (AC, DF) is the worker's");
        case 0x8C: case 0x8E: NO("a move to or from a segment register");
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: NO("an absolute memory address (moffs)");
        case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: NO("in or out: a port");
        case 0xFF:
            if (reg >= 2 && reg <= 5)
                NO("an indirect %s (FF /%u): only direct branches within the kernel", reg <= 3 ? "call" : "jmp", reg);
            break;
        case 0xE8: case 0xE9: case 0xEB:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
            if (opsize)
                NO("a branch with a 16-bit target (66 prefix)");
            int64_t d = isize == 1 ? (int8_t)c[imm_at]
                                   : (int32_t)((uint32_t)c[imm_at] | (uint32_t)c[imm_at + 1] << 8 |
                                               (uint32_t)c[imm_at + 2] << 16 | (uint32_t)c[imm_at + 3] << 24);
            int64_t t = (int64_t)next + d;
            if (t < base || t >= end)
                NO("branches outside the kernel's code (to &%llX)", (unsigned long long)(uint32_t)t);
            break;
        }
        }
    } else if (map == 1) {
        switch (op2) {
        case 0x00: case 0x01: NO("0F %02X: a system instruction (descriptor tables, swapgs, xgetbv)", op2);
        case 0x05: NO("syscall: a system call");
        case 0x34: NO("sysenter: a system call");
        case 0x07: case 0x35: NO("sysret or sysexit");
        case 0x06: case 0x08: case 0x09: case 0x30: case 0x32: case 0x33: case 0x37:
            NO("0F %02X: a privileged or system instruction (wrmsr, rdmsr, rdpmc, invd)", op2);
        case 0x20: case 0x21: case 0x22: case 0x23: NO("a move to or from a control or debug register");
        case 0x78: case 0x79: NO("vmread or vmwrite");
        case 0xA0: case 0xA1: case 0xA8: case 0xA9: NO("push or pop %%fs or %%gs");
        case 0xAE:
            if (mod == 3 && rep)
                NO("rdfsbase, rdgsbase, wrfsbase or wrgsbase: the thread pointer and the static base");
            if (mod != 3 && (reg == 1 || reg == 2 || reg == 5 || reg == 4))
                NO("fxrstor, ldmxcsr, xsave or xrstor: the floating point control is the worker's");
            break;
        default:
            if (op2 >= 0x80 && op2 <= 0x8F) {
                if (opsize)
                    NO("a branch with a 16-bit target (66 prefix)");
                int32_t d = (int32_t)((uint32_t)c[imm_at] | (uint32_t)c[imm_at + 1] << 8 |
                                      (uint32_t)c[imm_at + 2] << 16 | (uint32_t)c[imm_at + 3] << 24);
                int64_t t = (int64_t)next + d;
                if (t < base || t >= end)
                    NO("branches outside the kernel's code (to &%llX)", (unsigned long long)(uint32_t)t);
            }
        }
    }
    if (absolute)
        NO("an absolute memory address (a displacement with no base register)");
    if (ripdisp) {
        int32_t d = (int32_t)((uint32_t)c[disp_at] | (uint32_t)c[disp_at + 1] << 8 |
                              (uint32_t)c[disp_at + 2] << 16 | (uint32_t)c[disp_at + 3] << 24);
        int64_t t = (int64_t)next + d;
        if (t < lo || t >= hi)
            NO("refers to memory outside the kernel ([rip + d] is &%llX)", (unsigned long long)(uint32_t)t);
    }
    return i;
#undef AT
#undef NO
}

static uint32_t check_x64(const uint8_t *code, uint32_t len, uint32_t base, uint32_t lo, uint32_t hi,
                          char *why, size_t size)
{
    for (uint32_t off = 0; off < len;) {
        unsigned n = x64_one(code + off, len - off, base + off, base, base + len, lo, hi, why, size);
        if (!n)
            return off + 1;
        off += n;
    }
    return 0;
}

uint32_t wk_check(unsigned machine, const uint8_t *code, uint32_t len, uint32_t base, uint32_t lo,
                  uint32_t hi, char *why, size_t size)
{
    if (why && size)
        why[0] = 0;
    if (!len)
        return refuse(why, size, 0, "no code");
    if (machine == WK_EM_AARCH64)
        return check_a64(code, len, base, lo, hi, why, size);
    if (machine == WK_EM_X86_64)
        return check_x64(code, len, base, lo, hi, why, size);
    return refuse(why, size, 0, "not a machine the box has");
}
