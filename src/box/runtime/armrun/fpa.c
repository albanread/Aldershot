/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fpa.c: the FPA, native, for the ARM container.
 *
 * An ARM binary's FPA instructions are coprocessor instructions on CP1 and
 * CP2. The engine hands each one, as it translates it, to the coprocessors
 * armrun.cpp installs. They make it a direct call to fpa_execute with the
 * instruction rebuilt from its fields. No emulator module runs. The state is
 * the task's struct ros_fp, which holds FPA's eight registers as doubles and
 * the FPSR. Translated code and the native C library use the same state. The
 * arithmetic is ROSGD's FPA everywhere, as rosasm's lifter renders it. It is
 * double precision, with a result rounded to single where the instruction
 * says single, and the transcendentals are C's.
 *
 * Exceptions follow the FPSR. Its cumulative flags (bits 0-4: invalid
 * operation, division by zero, overflow, underflow, inexact) are set from
 * the host's IEEE flags. An exception whose trap is enabled (bits 16-20)
 * gives FPEmulator's error instead, &80000200 + n, "Floating point
 * exception: ...". The caller raises it.
 *
 * The encodings (cp1, bit 4 clear for CDP, set for a register transfer):
 *   dyadic   cccc 1110 oooo eNNN 0DDD 0001 gRR0 iMMM   (oooo: ADF MUF SUF RSF DVF
 *                                                       RDF POW RPW RMF FML FDV FRD POL)
 *   monadic  cccc 1110 oooo e000 1DDD 0001 gRR0 iMMM   (MVF MNF ABS RND SQT LOG LGN
 *                                                       EXP SIN COS TAN ASN ACS ATN URD NRM)
 *   FLT      cccc 1110 0000 eNNN dddd 0001 gRR1 0000   (Fn = Rd)
 *   FIX      cccc 1110 0001 0000 dddd 0001 0RR1 iMMM   (Rd = Fm)
 *   WFS RFS  0010 / 0011;  WFC RFC  0100 / 0101
 *   CMF CNF CMFE CNFE  1001 / 1011 / 1101 / 1111, Rd = pc: the flags
 *   LDF STF  cccc 110P UyWL nnnn YDDD 0001 offset     (yY: S D E P)
 *   LFM SFM  the same on cp2, yY the count (4 1 2 3)
 * e g the precision (00 single, 01 double, 10 extended); RR the rounding
 * (nearest, +inf, -inf, zero); i an immediate Fm: 0 1 2 3 4 5 0.5 10.
 */
#include "fpa.h"

#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rosgd/cpu.h"

#define FPSR_AC   (1u << 12)

enum { PREC_S, PREC_D, PREC_E };

static const double constant[8] = { 0, 1, 2, 3, 4, 5, 0.5, 10 };

static uint32_t ld32(uintptr_t base, uint32_t a)
{
    uint32_t v;
    memcpy(&v, (void *)(base + a), 4);
    return v;
}

static void st32(uintptr_t base, uint32_t a, uint32_t v)
{
    memcpy((void *)(base + a), &v, 4);
}

/* The memory formats (runtime/fp.c's, over the engine's base) */
static double load(uintptr_t base, uint32_t a, int words)
{
    if (words == 1) {
        float f;
        uint32_t w = ld32(base, a);
        memcpy(&f, &w, 4);
        return f;
    }
    if (words == 2) {                   /* a double is stored high word first */
        uint64_t b = (uint64_t)ld32(base, a) << 32 | ld32(base, a + 4);
        double d;
        memcpy(&d, &b, 8);
        return d;
    }
    /* extended: sign and exponent, then the 64-bit mantissa, integer bit explicit */
    uint32_t w0 = ld32(base, a), hi = ld32(base, a + 4), lo = ld32(base, a + 8);
    uint32_t exp = w0 & 0x7FFF;
    uint64_t mant = (uint64_t)hi << 32 | lo;
    double x;
    if (exp == 0x7FFF)
        x = (mant << 1) ? NAN : INFINITY;
    else if (exp == 0 && mant == 0)
        x = 0;
    else
        x = ldexp((double)mant, (int)exp - 16383 - 63);
    return (w0 >> 31) ? -x : x;
}

static void store(uintptr_t base, uint32_t a, int words, double x)
{
    if (words == 1) {
        float f = (float)x;
        uint32_t w;
        memcpy(&w, &f, 4);
        st32(base, a, w);
        return;
    }
    if (words == 2) {
        uint64_t b;
        memcpy(&b, &x, 8);
        st32(base, a, (uint32_t)(b >> 32));
        st32(base, a + 4, (uint32_t)b);
        return;
    }
    uint32_t sign = signbit(x) ? 0x80000000u : 0, exp = 0;
    uint64_t mant = 0;
    if (isnan(x)) {
        exp = 0x7FFF, mant = 0xC000000000000000ull;
    } else if (isinf(x)) {
        exp = 0x7FFF;
    } else if (x != 0) {
        int e;
        double m = frexp(fabs(x), &e);
        exp = (uint32_t)(e - 1 + 16383);
        mant = (uint64_t)ldexp(m, 64);
    }
    st32(base, a, sign | exp);
    st32(base, a + 4, (uint32_t)(mant >> 32));
    st32(base, a + 8, (uint32_t)mant);
}

static double round_prec(double x, int prec)
{
    return prec == PREC_S ? (double)(float)x : x;
}

static int fe_mode(unsigned rr)
{
    static const int m[4] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
    return m[rr & 3];
}

/* Moves the host's IEEE flags since fe_begin into the FPSR. Returns 0, or the
 * error number of the first trap that is enabled, taking the exceptions in
 * FPEmulator's order of numbers: invalid, overflow, divide by zero,
 * underflow, inexact. */
static uint32_t fpsr_flags(struct ros_fp *fp)
{
    static const struct { int fe; uint32_t bit, err; } ex[5] = {
        { FE_INVALID, 0, 0x80000200u },   { FE_OVERFLOW, 2, 0x80000201u },
        { FE_DIVBYZERO, 1, 0x80000202u }, { FE_UNDERFLOW, 3, 0x80000203u },
        { FE_INEXACT, 4, 0x80000204u },
    };
    uint32_t err = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (!fetestexcept(ex[i].fe))
            continue;
        if (fp->fpsr & (1u << (16 + ex[i].bit))) {
            if (!err)
                err = ex[i].err;
        } else {
            fp->fpsr |= 1u << ex[i].bit;
        }
    }
    return err;
}

static double operand_m(const struct ros_fp *fp, uint32_t w)
{
    return (w & 8) ? constant[w & 7] : fp->f[w & 7];
}

static uint32_t cmp_flags(const struct ros_fp *fp, double a, double b)
{
    int un = isnan(a) || isnan(b);
    uint32_t n = !un && a < b, z = !un && a == b;
    uint32_t c = (!un && a >= b) || (un && (fp->fpsr & FPSR_AC));
    return n << 31 | z << 30 | c << 29 | (uint32_t)un << 28;
}

static double dyadic(unsigned op, double x, double y)
{
    switch (op) {
    case 0: return x + y;                           /* ADF */
    case 1: return x * y;                           /* MUF */
    case 2: return x - y;                           /* SUF */
    case 3: return y - x;                           /* RSF */
    case 4: return x / y;                           /* DVF */
    case 5: return y / x;                           /* RDF */
    case 6: return pow(x, y);                       /* POW */
    case 7: return pow(y, x);                       /* RPW */
    case 8: return remainder(x, y);                 /* RMF */
    case 9: return x * y;                           /* FML */
    case 10: return x / y;                          /* FDV */
    case 11: return y / x;                          /* FRD */
    case 12: return atan2(y, x);                    /* POL */
    }
    return NAN;
}

static double monadic(unsigned op, unsigned rr, double x)
{
    switch (op) {
    case 0: case 15: return x;                      /* MVF NRM */
    case 1: return -x;                              /* MNF */
    case 2: return fabs(x);                         /* ABS */
    case 3: case 14:                                /* RND URD */
        return rr == 0 ? nearbyint(x) : rr == 1 ? ceil(x) : rr == 2 ? floor(x) : trunc(x);
    case 4: return sqrt(x);                         /* SQT */
    case 5: return log10(x);                        /* LOG */
    case 6: return log(x);                          /* LGN */
    case 7: return exp(x);                          /* EXP */
    case 8: return sin(x);                          /* SIN */
    case 9: return cos(x);                          /* COS */
    case 10: return tan(x);                         /* TAN */
    case 11: return asin(x);                        /* ASN */
    case 12: return acos(x);                        /* ACS */
    case 13: return atan(x);                        /* ATN */
    }
    return NAN;
}

uint64_t fpa_execute(struct ros_fp *fp, uintptr_t base, uint32_t word, uint32_t arg, uint32_t *error)
{
    *error = 0;
    unsigned cp = (word >> 8) & 0xF;
    unsigned rr = (word >> 5) & 3;
    int prec = (int)(((word >> 18) & 2) | ((word >> 7) & 1));     /* e (bit 19), g (bit 7) */

    if ((word & 0x0E000000u) == 0x0C000000u) {                    /* LDC/STC: LDF STF LFM SFM */
        int load_ = (word >> 20) & 1;
        unsigned fd = (word >> 12) & 7, yY = ((word >> 21) & 2) | ((word >> 15) & 1);
        if (cp == 1) {
            static const int words[4] = { 1, 2, 3, 3 };
            if (yY == 3) {                                        /* packed decimal */
#ifdef ROS_FPA_PACKED                                             /* the runtime's (runtime/fp.c) */
                if (load_)
                    fp->f[fd] = ros_fpa_ldp(arg);
                else
                    ros_fpa_stp(arg, fp->f[fd]);
                return 0;
#else
                *error = 0x80000000u;
                return 0;
#endif
            }
            if (load_)
                fp->f[fd] = load(base, arg, words[yY]);
            else
                store(base, arg, words[yY], round_prec(fp->f[fd], yY == 0 ? PREC_S : PREC_D));
            return 0;
        }
        static const unsigned count[4] = { 4, 1, 2, 3 };          /* LFM SFM */
        for (unsigned i = 0; i < count[yY]; i++) {
            unsigned r = (fd + i) & 7;
            if (load_)
                fp->f[r] = load(base, arg + 12 * i, 3);
            else
                store(base, arg + 12 * i, 3, fp->f[r]);
        }
        return 0;
    }

    unsigned opc = (word >> 20) & 0xF;
    int saved = fegetround();
    feclearexcept(FE_ALL_EXCEPT);
    uint64_t result = 0;
    if (!(word & 0x10)) {                                         /* CDP: arithmetic */
        unsigned fd = (word >> 12) & 7;
        double x;
        if (rr)
            fesetround(fe_mode(rr));
        if (word & 0x8000) {                                      /* monadic */
            x = monadic(opc, rr, operand_m(fp, word));
        } else {
            x = dyadic(opc, fp->f[(word >> 16) & 7], operand_m(fp, word));
            if (opc >= 9 && opc <= 11)
                prec = PREC_S;                                    /* the fast ones: single */
        }
        fp->f[fd] = round_prec(x, prec);
        if (rr)
            fesetround(saved);
    } else {
        switch (opc) {
        case 0x0: {                                               /* FLT */
            double x = (double)(int32_t)arg;
            if (prec == PREC_S) {
                if (rr)
                    fesetround(fe_mode(rr));
                x = (double)(float)x;
                if (rr)
                    fesetround(saved);
            }
            fp->f[(word >> 16) & 7] = x;
            break;
        }
        case 0x1:                                                 /* FIX */
            result = (uint32_t)ros_to_int(operand_m(fp, word), (int)rr);
            if (isnan(operand_m(fp, word)) || fabs(operand_m(fp, word)) >= 2147483648.0)
                feraiseexcept(FE_INVALID);
            break;
        case 0x2:                                                 /* WFS */
            fp->fpsr = (fp->fpsr & 0xFF000000u) | (arg & 0x00FFFFFFu);
            break;
        case 0x3:                                                 /* RFS */
            result = fp->fpsr;
            break;
        case 0x4:                                                 /* WFC: the control register, */
            break;                                                /* which no emulator has here */
        case 0x5:                                                 /* RFC */
            result = 0;
            break;
        case 0x9: case 0xB: case 0xD: case 0xF: {                 /* CMF CNF CMFE CNFE */
            double a = fp->f[(word >> 16) & 7], b = operand_m(fp, word);
            if (opc == 0xB || opc == 0xF)
                b = -b;
            if ((opc == 0xD || opc == 0xF) && (isnan(a) || isnan(b)))
                feraiseexcept(FE_INVALID);
            result = cmp_flags(fp, a, b);
            break;
        }
        default:
            *error = 0x80000000u;                                 /* not an FPA instruction */
            return 0;
        }
    }
    uint32_t trap = fpsr_flags(fp);
    feclearexcept(FE_ALL_EXCEPT);
    *error = trap;
    return result;
}

const char *fpa_error_text(uint32_t err)
{
    switch (err) {
    case 0x80000200u: return "Floating point exception: invalid operation";
    case 0x80000201u: return "Floating point exception: overflow";
    case 0x80000202u: return "Floating point exception: divide by zero";
    case 0x80000203u: return "Floating point exception: underflow";
    case 0x80000204u: return "Floating point exception: inexact operation";
    }
    return "Internal error: undefined instruction";
}
