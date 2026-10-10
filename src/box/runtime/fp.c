/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fp.c: FPA's 12-byte extended format, to and from C doubles.
 *
 * ROSGD computes FPA's extended precision in double. What is left of the
 * format is memory. These are LDFE and STFE, and LFM and SFM, which move
 * registers 12 bytes at a time. A double goes out exactly and comes back
 * unchanged. An extended value from elsewhere is rounded to nearest.
 *
 * The layout is as follows. Word 0 holds the sign (bit 31) and the biased
 * exponent (bits 14-0, bias 16383). Words 1 and 2 hold the 64-bit mantissa,
 * high then low, with its integer bit explicit (bit 31 of word 1).
 */
#include <math.h>
#include <string.h>

#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"

void ros_fpa_ste(uint32_t a, double x)
{
    uint32_t sign = signbit(x) ? 0x80000000u : 0;
    uint32_t exp = 0;
    uint64_t mant = 0;
    if (isnan(x)) {
        uint64_t b;
        memcpy(&b, &x, 8);
        exp = 0x7FFF;
        mant = 0x8000000000000000ull | (b & 0x000FFFFFFFFFFFFFull) << 11 | 1ull << 62;
    } else if (isinf(x)) {
        exp = 0x7FFF;
    } else if (x != 0) {
        int e;
        double m = frexp(fabs(x), &e);          /* x = m * 2^e, m in [0.5, 1) */
        exp = (uint32_t)(e - 1 + 16383);
        mant = (uint64_t)ldexp(m, 64);          /* exact: 53 bits, J at bit 63 */
    }
    ros_st32(a, sign | exp);
    ros_st32(a + 4, (uint32_t)(mant >> 32));
    ros_st32(a + 8, (uint32_t)mant);
}

double ros_fpa_lde(uint32_t a)
{
    uint32_t w0 = ros_ld32(a);
    uint64_t mant = (uint64_t)ros_ld32(a + 4) << 32 | ros_ld32(a + 8);
    int exp = (int)(w0 & 0x7FFF);
    double x;
    if (exp == 0x7FFF)
        x = mant << 1 ? NAN : INFINITY;         /* the integer bit aside */
    else
        x = ldexp((double)mant, exp - 16383 - 63);
    return w0 & 0x80000000u ? -x : x;
}

/* FPA packed decimal is the twelve-byte form that STFP and LDFP move. It is
 * also the form that BASIC's own reader and writer walk. Word 0 holds the
 * sign (bit 31), the exponent's sign (bit 30), the exponent as four BCD
 * digits (thousands at bit 24 down to units at bit 12) and the first three of
 * the nineteen mantissa digits (bits 11-8, 7-4, 3-0). Words 1 and 2 hold the
 * other sixteen, most significant first (bits 31-28 down). The value is
 * d0.d1d2...d18 x 10^exponent. For example, 2.5 is the digits 25 with
 * exponent 0.
 *
 * Both directions follow the FPE (FPASC coresrc/s/ldst, STFConvert_Packed
 * and LDFConvert_Packed), in its extended precision. So BASIC prints and
 * reads numbers digit for digit as RISC OS does. STFP writes seventeen
 * significant digits, and the last two are always 0.
 */
static unsigned fpa_digit_bit(int i)            /* bit index across the three words */
{
    return i < 3 ? (unsigned)(8 - 4 * i) : i < 11 ? (unsigned)(60 - 4 * (i - 3))
                                                 : (unsigned)(92 - 4 * (i - 11));
}

/* An extended value as the FPE holds one: a 64-bit mantissa with its
 * integer bit at bit 63, times 2^(exp - 63). Products and quotients are
 * rounded to nearest, ties to even, as MUFE and DVFE round them. */
struct ext {
    int exp;
    uint64_t mant;
};

static struct ext ext_up(struct ext r, int up)
{
    if (up && ++r.mant == 0) {
        r.mant = 1ull << 63;
        r.exp++;
    }
    return r;
}

static struct ext ext_mul(struct ext a, struct ext b)                   /* MUFE */
{
    unsigned __int128 p = (unsigned __int128)a.mant * b.mant;
    struct ext r = { a.exp + b.exp, 0 };
    if (p >> 127)
        r.exp++;
    else
        p <<= 1;
    r.mant = (uint64_t)(p >> 64);
    uint64_t rest = (uint64_t)p;
    return ext_up(r, rest > 1ull << 63 || (rest == 1ull << 63 && (r.mant & 1)));
}

static struct ext ext_div(struct ext a, struct ext b)                   /* DVFE */
{
    unsigned __int128 rem = a.mant, d = b.mant;
    struct ext r = { a.exp - b.exp, 0 };
    if (rem < d) {
        rem <<= 1;
        r.exp--;
    }
    for (int i = 0; i < 64; i++) {              /* the integer bit first */
        r.mant <<= 1;
        if (rem >= d) {
            rem -= d;
            r.mant |= 1;
        }
        rem <<= 1;
    }
    int half = rem >= d;
    if (half)
        rem -= d;
    return ext_up(r, half && (rem != 0 || (r.mant & 1)));
}

static struct ext ext_int(uint64_t n)           /* n != 0, exactly */
{
    int z = __builtin_clzll(n);
    return (struct ext){ 63 - z, n << z };
}

static const struct ext ten = { 3, 0xA000000000000000ull };

/* PowersOfTenTable: 10^0 to 10^27, all exact (5^27 < 2^64) */
static struct ext small_power(unsigned n)
{
    uint64_t five = 1;
    for (unsigned i = 0; i < n; i++)
        five *= 5;
    struct ext p = ext_int(five);
    p.exp += (int)n;
    return p;
}

/* MakePowerOfTenInF1: shift n right until it is in the table. Then work
 * back, squaring and multiplying by ten for each bit shifted out. */
static struct ext power_of_ten(unsigned n)
{
    int shifts = 0;
    while (n >> shifts >= 28)
        shifts++;
    struct ext p = small_power(n >> shifts);
    while (shifts-- > 0) {
        p = ext_mul(p, p);
        if (n >> shifts & 1)
            p = ext_mul(p, ten);
    }
    return p;
}

static struct ext any_power(unsigned n)
{
    return n < 28 ? small_power(n) : power_of_ten(n);
}

/* STFD's rounding of an extended value to double, to nearest */
static double ext_double(struct ext v)
{
    int e = v.exp;                              /* value = mant x 2^(e - 63) */
    int drop = e >= -1022 ? 11 : 11 + (-1022 - e);
    if (drop > 65)
        return 0;
    unsigned __int128 m = (unsigned __int128)v.mant << 1;   /* a spare bit for drop 65 */
    drop++;
    uint64_t kept = (uint64_t)(m >> drop);
    unsigned __int128 rest = m & (((unsigned __int128)1 << drop) - 1),
                      halfway = (unsigned __int128)1 << (drop - 1);
    if (rest > halfway || (rest == halfway && (kept & 1)))
        kept++;
    /* kept x 2^(e - 63 + drop - 1): a carry to 2^53 still converts exactly */
    return ldexp((double)kept, e - 64 + drop);
}

/* LDFConvert_Packed, and STFD's rounding to double. *over is set if the
 * value went past a double's range. */
static double ldp(uint32_t a, int *over)
{
    uint32_t w[3] = { ros_ld32(a), ros_ld32(a + 4), ros_ld32(a + 8) };
    int neg = (w[0] & 0x80000000u) != 0;
    /* LDFConvert_Packed_Digits: take the exponent and the mantissa as
     * integers, whatever the nibbles hold. */
    int e = 0;
    for (int b = 24; b >= 12; b -= 4)
        e = e * 10 + (int)((w[0] >> b) & 0xF);
    uint64_t m = 0;
    for (int i = 0; i < 19; i++) {
        unsigned b = fpa_digit_bit(i);
        m = m * 10 + ((w[b >> 5] >> (b & 31)) & 0xF);
    }
    double x;
    *over = 0;
    if (e >= 0x4100) {                          /* infinities and NaNs */
        x = m ? NAN : INFINITY;
    } else if (m == 0) {
        x = 0;
    } else {
        int r = (w[0] & (1u << 30) ? -e : e) - 18;
        struct ext v = ext_int(m);
        if (r > 4096)
            x = INFINITY;
        else if (r < -4096)
            x = 0;
        else {
            if (r > 0)
                v = ext_mul(v, any_power((unsigned)r));
            else if (r < 0)
                v = ext_div(v, any_power((unsigned)-r));
            x = v.exp > 1023 ? INFINITY : ext_double(v);
        }
        *over = isinf(x);
    }
    return neg ? -x : x;
}

double ros_fpa_ldp(uint32_t a)
{
    int over;
    return ldp(a, &over);
}

/* LDFP in compiled code (rosasm's lifting), with the program's R10-R12.
 * If the value is too big for a double, it traps when the FPSR enables the
 * overflow exception. BASIC enables it. On RISC OS the STFD after the LDFP
 * is what traps. The FPE raises its error from the program's registers, so
 * the error handler is entered with them (FPASC vensrc/riscos/end,
 * handle_exception). */
double ros_fpa_ldp_at(uint32_t a, uint32_t r10, uint32_t r11, uint32_t r12)
{
    int over;
    double x = ldp(a, &over);
    if (over && ros_fp_current && (ros_fp_current->fpsr & ROS_FPSR_OFE)) {
        if (ros_call_depth == 0) {
            struct ros_cpu fg = { .r = { [10] = r10, [11] = r11, [12] = r12 } };
            ros_env_foreground(&fg);
        }
        ros_raise(ros_error(0x80000201, "Floating point exception: overflow"));
    }
    return x;
}

void ros_fpa_stp(uint32_t a, double x)
{
    uint32_t w[3] = { __builtin_signbit(x) ? 0x80000000u : 0, 0, 0 };
    if (isinf(x)) {
        w[0] |= 0x0FFFF800;                     /* a double's, with its units bit */
    } else if (isnan(x)) {
        /* Take the fraction below the quiet bit and reorder it so that its
         * low bits come first. It becomes a nineteen-digit integer. */
        uint64_t b;
        memcpy(&b, &x, 8);
        uint64_t f = b & 0x000FFFFFFFFFFFFFull, n = (f & 0x1FFFFFFF) << 22 | (f >> 29 & 0x3FFFFF);
        w[0] |= 0x0FFFF800;
        for (int i = 18; i >= 0; i--, n /= 10) {
            unsigned bit = fpa_digit_bit(i);
            w[bit >> 5] |= (uint32_t)(n % 10) << (bit & 31);
        }
    } else if (x != 0) {
        int e2;
        double fr = frexp(fabs(x), &e2);       /* 2^(e2-1) <= |x| < 2^e2 */
        struct ext v = { e2 - 1, (uint64_t)ldexp(fr, 64) };
        /* The decimal exponent, either the true one or one too big. It is
         * e2 x log10(2), with log10(2) taken to 18 bits and rounded down
         * (up when e2 is positive). */
        int t = e2 * (e2 > 0 ? 0x13442 : 0x13441);
        int e10 = t >= 0 ? t >> 18 : -((-t + 0x3FFFF) >> 18);
        if (e10) {
            unsigned n = (unsigned)(e10 > 0 ? e10 : -e10);
            struct ext p;
            if (n < 28) {
                p = small_power(n);
            } else {                            /* 10^(n/2), then 10^(n - n/2) */
                p = power_of_ten(n >> 1);
                v = e10 > 0 ? ext_div(v, p) : ext_mul(v, p);
                if (n & 1)
                    p = ext_mul(p, ten);
            }
            v = e10 > 0 ? ext_div(v, p) : ext_mul(v, p);
        }
        /* Now 0.1 <= v < 10. Take it as fixed point, with four integer
         * bits and 92 of fraction, and extract a digit at a time. A value
         * under 1 has one digit less. */
        const unsigned __int128 one = (unsigned __int128)1 << 92, all = ((unsigned __int128)1 << 96) - 1;
        int s = 3 - v.exp;
        unsigned __int128 f = s >= 0 && s < 96 ? ((unsigned __int128)v.mant << 32) >> s : 0;
        if (f < one) {
            e10--;
            f = f * 10 & all;
        }
        unsigned d[17];
        for (int i = 0; i < 17; i++) {
            if (i)
                f = f * 10 & all;
            d[i] = (unsigned)(f >> 92);
            f &= one - 1;
        }
        /* Round up past a half. Exactly a half is left alone. */
        if (f >> 91 & 1 && f & ((one >> 1) - 1)) {
            int i = 16;
            while (i >= 0 && d[i] == 9)
                d[i--] = 0;
            if (i >= 0) {
                d[i]++;
            } else {
                d[0] = 1;
                e10++;
            }
        }
        if (e10 < 0) {
            w[0] |= 1u << 30;
            e10 = -e10;
        }
        w[0] |= (uint32_t)(e10 / 1000 % 10) << 24 | (uint32_t)(e10 / 100 % 10) << 20
              | (uint32_t)(e10 / 10 % 10) << 16 | (uint32_t)(e10 % 10) << 12;
        for (int i = 0; i < 17; i++) {
            unsigned bit = fpa_digit_bit(i);
            w[bit >> 5] |= (uint32_t)d[i] << (bit & 31);
        }
    }
    ros_st32(a, w[0]);
    ros_st32(a + 4, w[1]);
    ros_st32(a + 8, w[2]);
}
