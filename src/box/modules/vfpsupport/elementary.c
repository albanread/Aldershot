/* Copyright (c) 2021 RISC OS Open Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is a translation into C of RISC OS Open's VFPSupport source
 * (Sources/HWSupport/VFPSupport: s.Trig64, s.ArcTrig64, s.Power64, s.CMath).
 * Those are in turn translations to AArch32 of the following, whose
 * notices ROOL's source carries and which are kept here.
 *
 *   aocl-libm-ose/src/optmized/sin.c and tan.c
 *       Copyright (C) 2008-2020 Advanced Micro Devices, Inc. All rights
 *       reserved.  SPDX-License-Identifier: BSD-3-Clause
 *   aocl-libm-ose/src/ref/remainder_piby2.c
 *       Copyright (c) 2002-2019 Advanced Micro Devices, Inc.
 *       SPDX-License-Identifier: MIT
 *   openlibm/src/e_asin.c, s_atan.c, e_atan2.c, e_log.c, e_log10.c, e_exp.c
 *       and e_pow.c
 *       Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *       Developed at SunPro, a Sun Microsystems, Inc. business.
 *       (some also Copyright (C) 2004 by Sun Microsystems, Inc.,
 *       developed at SunSoft, a Sun Microsystems, Inc. business.)
 *       Permission to use, copy, modify, and distribute this software is
 *       freely granted, provided that this notice is preserved.
 *
 * The ldexp function follows s.CMath, which is
 *
 * Copyright 2021 RISC OS Open Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Mixed provenance.  Different parts of this file come under different
 * licences: ROOL's BSD notice above for the trigonometric, inverse
 * trigonometric and power code, ROOL's Apache licence for ldexp, and the
 * AMD and Sun notices for the algorithms beneath them.  The notices are kept
 * as ROOL's source carries them.  The full AMD BSD-3-Clause text is not
 * reproduced, because ROOL's source does not carry it.
 */

/* elementary.c: VFPSupport's elementary functions in double precision,
 * as RISC OS 5 computes them (HWSupport/VFPSupport: s/Trig64, s/ArcTrig64,
 * s/Power64, and s/CMath's ldexp). Those sources are translations to
 * AArch32 of AMD's aocl-libm (sin, cos, tan and their reduction by pi/2)
 * and of openlibm (asin, acos, atan, atan2, log, log10, exp, pow). Their
 * results differ from a C library's in the last bit now and then. Near odd
 * multiples of pi/2 they differ by far more. BASIC's STR$ shows seventeen
 * digits, so the differences can be seen.
 *
 * This file is transliterated one instruction at a time. d0-d11 are the
 * VFP registers. a1-a4, v1-v5, ip and lr are the ARM registers. Each
 * operation rounds where the original does. VMLA and VMLS round the
 * product and then the sum. VFused64 (VFMA, or fused64_muladd without
 * VFPv4) rounds once, as fma() does. Each operation also ORs the FPSCR's
 * exceptions in as the hardware sets them (cpu.h). RaiseException is the
 * original's. It puts the flags into the FPSCR, and if a flag's trap is
 * enabled it raises the vector floating point exception.
 *
 * The originals' quirks are kept, because RISC OS 5.30 has them:
 *   - cos's reduction compares the reduced argument's exponent with v3.
 *     sin and tan set v3 to x's exponent, but cos never sets it. It is
 *     the caller's R6.
 *   - tan's third reduction step uses dpi5, whose high word is dpi2's. So
 *     tan near odd multiples of pi/2 is far out. TAN(PI/2) is
 *     16455198023.8.
 *   - Positive subnormals raise underflow and come back unchanged from sin
 *     and tan (cos gives 1). Negative ones take the ordinary path.
 *   - log and log10 of +INF raise invalid operation.
 *   - atan2 checks x for a NaN twice and never checks y. A NaN x gives y.
 *   - pow of -INF to any power except 0, 1 and 2 gives -0. A y from 2^20
 *     up to 2^21 counts as odd for a negative x. At z = 1024 the overflow
 *     test takes z - p_l where openlibm takes z - p_h.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "vfpmath.h"

static inline double K(uint64_t b)
{
    double x;
    memcpy(&x, &b, 8);
    return x;
}

static inline uint64_t B(double x)
{
    uint64_t b;
    memcpy(&b, &x, 8);
    return b;
}

#include "elementary.h"

#define SIGN 0x80000000u
#define LO(x) ((uint32_t)B(x))
#define HI(x) ((uint32_t)(B(x) >> 32))
#define D(lo, hi) K((uint64_t)(hi) << 32 | (uint32_t)(lo))   /* VMOV d, lo, hi */
#define T(t, i) K((t)[i])
#define EXP(hi) ((hi) >> 20 & 0x7FFu)                         /* ExpBits64 */
#define LOADEXP(n) ((uint32_t)(1023 + (n)) << 20)              /* LoadExp64 */
#define U64(hi, lo) ((uint64_t)(hi) << 32 | (uint32_t)(lo))   /* CMP hi; CMPEQ lo */

#define FPSCR_IOC 0x01u
#define FPSCR_DZC 0x02u
#define FPSCR_OFC 0x04u
#define FPSCR_UFC 0x08u
#define FPSCR_IXC 0x10u
#define FPSCR_IDC 0x80u

/* ---- the operations, with their exceptions --------------------------------------------------- */

static inline double vadd(struct ros_cpu *s, double a, double b)
{
    double r = a + b;
    s->fp->fpscr |= ros_vfp_ex2(r, a, b);
    return r;
}

static inline double vsub(struct ros_cpu *s, double a, double b)
{
    double r = a - b;
    s->fp->fpscr |= ros_vfp_ex2(r, a, b);
    return r;
}

static inline double vmul(struct ros_cpu *s, double a, double b)
{
    double r = a * b;
    s->fp->fpscr |= ros_vfp_ex2(r, a, b);
    return r;
}

static inline double vdiv(struct ros_cpu *s, double a, double b)
{
    double r = a / b;
    s->fp->fpscr |= ros_vfp_div_ex(r, a, b);
    return r;
}

static inline double vsqrt(struct ros_cpu *s, double a)
{
    s->fp->fpscr |= ros_vfp_sqrt_ex(a);
    return sqrt(a);
}

static inline double vfma(struct ros_cpu *s, double acc, double x, double y)
{
    double r = fma(x, y, acc);
    s->fp->fpscr |= ros_vfp_ex3(r, x, y, acc);
    return r;
}

/* VCVT.S32.F64, towards zero */
static inline int32_t vcvt_s32(struct ros_cpu *s, double x)
{
    s->fp->fpscr |= ros_vfp_int_ex(x, ROS_ROUND_ZERO, 1);
    return ros_to_int(x, ROS_ROUND_ZERO);
}

#define ADD(a, b) vadd(s, a, b)
#define SUB(a, b) vsub(s, a, b)
#define MUL(a, b) vmul(s, a, b)
#define DIV(a, b) vdiv(s, a, b)
#define SQRT(a) vsqrt(s, a)
#define MLA(d, n, m) ((d) = ADD(d, MUL(n, m)))         /* VMLA d, n, m */
#define MLS(d, n, m) ((d) = SUB(d, MUL(n, m)))         /* VMLS */
#define NMLS(d, n, m) ((d) = ADD(-(d), MUL(n, m)))     /* VNMLS */
#define FUSED(d, n, m) ((d) = vfma(s, d, n, m))        /* VFused64 d, n, m */

/* RaiseException (s/Errors). The flags go into the FPSCR. If the trap of
 * any of them is enabled, the error is raised for the last of the flags in
 * the order IO, DZ, OF, UF, IX, ID. (The original enters the error handler
 * with R12 holding its workspace.) */
static void raise_ex(struct ros_cpu *s, uint32_t flags)
{
    static const char *const what[] = { "invalid operation", "division by zero", "overflow",
                                        "underflow", "inexact operation", "input subnormal" };
    flags &= 0xFF;
    s->fp->fpscr |= flags;
    if (!(flags & s->fp->fpscr >> 8))
        return;
    int n = flags & FPSCR_IDC ? 5 : flags & FPSCR_IXC ? 4 : flags & FPSCR_UFC ? 3
          : flags & FPSCR_OFC ? 2 : flags & FPSCR_DZC ? 1 : 0;
    if (ros_call_depth == 0)
        ros_env_foreground(s);
    ros_raise(ros_error(0x80000600u + (uint32_t)n, "Vector floating point exception: %s", what[n]));
}

/* sin, cos, tan of an infinity or a NaN: raise invalid operation and give a
 * quiet NaN */
static double nan_or_inf(struct ros_cpu *s, uint32_t a1, uint32_t a2)
{
    uint32_t v2 = (a1 | a2 << 12) == 0 ? a2 | 1u << 19 : a2;
    raise_ex(s, FPSCR_IOC);
    return D(a1, v2);
}

/* ---- sin, cos, tan (s/Trig64) -------------------------------------------------------------------- */

/* DivRem10 is exact for what RemainderPiBy2 gives it */

/* RemainderPiBy2: reduces x >= 5e6 to [-pi/4, pi/4] using the bits of 2/pi.
 * The result is the doubledouble (d0, d1) and the quadrant is in *region. */
static void remainder_piby2(struct ros_cpu *s, double *pd0, double *pd1, uint32_t *region)
{
    double d0 = *pd0, d1, d2, d3, d4, d5, d6, d7;
    uint32_t v1 = LO(d0), v2 = HI(d0);
    int32_t first = (int32_t)EXP(v2) - 1023;            /* unbiased exponent */
    v2 = (v2 & 0x000FFFFFu) | 0x00100000u;             /* normalised |x| */
    uint32_t resexp = (uint32_t)first % 10;
    first = (int32_t)((uint32_t)first / 10);
    uint64_t mant = U64(v2, v1), carry = 0;
    uint16_t res[20];
    res[19] = 0;
    for (int32_t i = first + 18; i >= first; i--) {
        uint64_t u = trig_pibits[i] * mant + carry;
        res[i - first] = (uint16_t)(u & 0x3FF);
        carry = u >> 10;
    }
    uint32_t ltb = (((uint32_t)res[0] << 10 | res[1]) >> (9 - resexp)) & 7;
    uint32_t a4 = ltb >> 1;
    if (ltb & 1)
        a4 = (a4 + 1) & 3;
    *region = a4;

    /* Reconstruct the result */
    uint32_t a2 = res[1];
    if (ltb & 1)
        a2 = ~a2;
    mant = (((uint32_t)1 << (10 - resexp)) - 1) & a2;
    int i = 1;
    while (mant < 0x0020000000000000u && i < 18) {
        i++;
        uint32_t lr = res[i];
        if (ltb & 1)
            lr = ~lr & 0x3FF;
        mant = mant << 10 | lr;
    }
    uint32_t lr = res[i + 1];
    if (ltb & 1)
        lr = ~lr;
    uint64_t highbitsrr = (uint64_t)(uint32_t)(lr << 22) << 32;
    int32_t rexp = 52 + (int32_t)resexp - i * 10;
    while (mant >= 0x0020000000000000u) {
        rexp++;
        highbitsrr = highbitsrr >> 1 | (mant & 1) << 63;
        mant >>= 1;
    }
    /* rexp and mant together */
    a4 = (uint32_t)(rexp + 1023);
    uint32_t v3 = (uint32_t)mant, v4 = (uint32_t)(mant >> 32);
    v4 = (v4 & 0x000FFFFFu) | a4 << 20;
    if (ltb & 1)
        v4 |= SIGN;                                     /* negated mantissa, negated x */
    d2 = D(v3, v4);                                     /* x */
    /* rexp and highbitsrr together */
    uint32_t v5 = (uint32_t)highbitsrr, v6 = (uint32_t)(highbitsrr >> 32);
    v5 = v5 >> 12 | v6 << 20;
    v6 >>= 12;                                          /* one place too far */
    a4 = (a4 - 53) << 20;
    d4 = D(0, a4);
    d3 = D(v5, a4 | v6);                                /* xx */
    d3 = SUB(d3, d4);                                   /* less the implicit bit */
    d3 = ADD(d3, d3);                                   /* for the shift too far */
    if (ltb & 1)
        d3 = -d3;

    /* (x, xx) times pi/2 in extra precision: (r, rr) */
    d6 = trig_piby2_lead;
    v3 &= 0xF8000000u;
    d5 = D(v3, v4);                                     /* hx */
    d4 = SUB(d2, d5);                                   /* tx */
    d0 = MUL(d6, d2);                                   /* c */
    d6 = trig_piby2_part1;
    d7 = trig_piby2_part2;
    d1 = MUL(d6, d5);
    d1 = SUB(d1, d0);
    MLA(d1, d6, d4);
    d6 = trig_piby2_lead;
    MLA(d1, d7, d5);
    MLA(d1, d7, d4);
    d7 = trig_piby2_part3;
    d3 = MUL(d6, d3);
    MLA(d3, d7, d2);
    d1 = ADD(d1, d3);
    d2 = ADD(d0, d1);                                   /* r = c + cc */
    d0 = SUB(d0, d2);
    d1 = ADD(d0, d1);                                   /* rr = (c - r) + cc */
    *pd0 = d2;
    *pd1 = d1;
}

/* SinRangeReduce, CosRangeReduce: reduces |x| > pi/4 (bits a2:a1) to
 * (r, rr) and returns the quadrant. v3 should be x's exponent. */
static uint32_t range_reduce(struct ros_cpu *s, double *pd0, double *pd1, uint32_t a1, uint32_t a2,
                             uint32_t v3)
{
    double d0 = *pd0, d1, d2, d3, d4, d5, d6, d7;
    uint32_t v2;
    if (U64(a2, a1) >= B(trig_d5e6)) {
        remainder_piby2(s, &d0, &d1, &v2);
    } else {
        d2 = trig_d2uponpi;
        d7 = trig_d18p52;
        d2 = MUL(d0, d2);                               /* |x| * 2/pi */
        d5 = ADD(d2, d7);
        v2 = LO(d5) & 3;                                /* region */
        d5 = SUB(d5, d7);
        d6 = trig_dpi1;
        d7 = trig_dpi2;
        d3 = MUL(d5, d6);
        d3 = SUB(d0, d3);                               /* rhead */
        d4 = MUL(d5, d7);                               /* rtail */
        d0 = SUB(d3, d4);                               /* r */
        if (v3 - EXP(HI(d0)) > 15) {
            d6 = trig_dpi3;
            d7 = trig_dpi4;
            d2 = MUL(d5, d6);
            d6 = d3;
            d3 = SUB(d6, d2);                           /* rhead */
            d4 = SUB(d6, d3);
            d4 = SUB(d4, d2);
            d6 = MUL(d5, d7);
            d4 = SUB(d6, d4);                           /* rtail */
            d0 = SUB(d3, d4);                           /* r */
        }
        d3 = SUB(d3, d0);
        d1 = SUB(d3, d4);                               /* rr = (rhead - r) - rtail */
    }
    *pd0 = d0;
    *pd1 = d1;
    return v2;
}

/* sin's polynomials on the doubledouble (r, rr) in quadrant v2, and the
 * sign. v1 is x's. */
static double sin_quadrant(struct ros_cpu *s, double d0, double d1, uint32_t v1, uint32_t v2)
{
    double d2, d3, d4, d5, d6, d7, d8;
    d2 = MUL(d0, d0);                                   /* r^2 */
    if (v2 & 1) {                                       /* odd (cosine) quadrant */
        d3 = T(trig_dCcos, 0), d4 = T(trig_dCcos, 1), d5 = T(trig_dCcos, 2);
        d6 = T(trig_dCcos, 3), d7 = T(trig_dCcos, 4), d8 = T(trig_dCcos, 5);
        d1 = MUL(d1, d0);                               /* rr*r */
        d0 = MUL(d2, d2);                               /* r^4 */
        FUSED(d7, d2, d8);
        FUSED(d6, d2, d7);
        FUSED(d5, d2, d6);
        MLA(d4, d2, d5);
        MLA(d3, d2, d4);
        d7 = trig_dhalf;
        d6 = trig_done;
        d3 = MUL(d0, d3);                               /* poly */
        d2 = MUL(d7, d2);                               /* s = 1/2 * r^2 */
        d4 = SUB(d2, d6);                               /* t = s - 1.0 */
        d5 = ADD(d6, d4);
        d5 = SUB(d5, d2);
        d5 = SUB(d5, d1);
        d5 = ADD(d5, d3);
        d0 = SUB(d5, d4);          /* ((((1.0 + t) - s) - rr*r) + poly) - t */
    } else {                                            /* even (sine) quadrant */
        d3 = T(trig_dCsin, 0), d4 = T(trig_dCsin, 1), d5 = T(trig_dCsin, 2);
        d6 = T(trig_dCsin, 3), d7 = T(trig_dCsin, 4), d8 = T(trig_dCsin, 5);
        FUSED(d7, d2, d8);
        FUSED(d6, d2, d7);
        FUSED(d5, d2, d6);
        MLA(d4, d2, d5);                                /* poly */
        d4 = -d4;
        d6 = MUL(d2, d0);                               /* r^3 */
        d7 = trig_dhalf;
        d7 = MUL(d7, d1);                               /* s = 1/2 * rr */
        MLA(d7, d6, d4);                                /* s - r^3 * poly */
        d5 = MUL(d2, d7);
        d5 = SUB(d5, d1);
        d3 = MUL(d3, d6);                               /* C1 * r^3 */
        d3 = SUB(d5, d3);
        d0 = SUB(d0, d3);          /* r - ((r^2 * (s - r^3 * poly)) - rr) - C1 * r^3 */
    }
    uint32_t a3 = v1 & v2 >> 1;
    uint32_t a4 = (~(v2 >> 1) & ~v1) | a3;
    if (!(a4 & 1))
        d0 = -d0;
    return d0;
}

double ros_vfp_sin(struct ros_cpu *s, double x)
{
    double d0 = x, d1, d2, d3, d4, d5, d6, d7;
    uint32_t a1 = LO(d0), a2 = HI(d0);
    if ((a1 | a2 << 1) == 0)
        return d0;                                      /* sin(+-0) = +-0 */
    uint32_t v3 = EXP(a2);
    if (v3 == 0x7FF)
        return nan_or_inf(s, a1, a2);
    if (U64(a2, a1) < U64(1u << 20, 0)) {               /* subnormal, too small to scale */
        raise_ex(s, FPSCR_UFC);
        return d0;
    }
    uint32_t v1 = a2 >> 31;
    a2 &= ~SIGN;
    if (U64(a2, a1) > B(trig_dpiby4)) {
        d0 = fabs(d0);
        uint32_t v2 = range_reduce(s, &d0, &d1, a1, a2, v3);
        return sin_quadrant(s, d0, d1, v1, v2);
    }
    if (U64(a2, a1) >= U64(LOADEXP(-13), 0)) {
        d2 = T(trig_dCsin, 0), d3 = T(trig_dCsin, 1), d4 = T(trig_dCsin, 2);
        d5 = T(trig_dCsin, 3), d6 = T(trig_dCsin, 4), d7 = T(trig_dCsin, 5);
        d1 = MUL(d0, d0);                               /* x^2 */
        FUSED(d6, d1, d7);
        FUSED(d5, d1, d6);
        FUSED(d4, d1, d5);
        MLA(d3, d1, d4);
        MLA(d2, d1, d3);
        d1 = MUL(d1, d2);
        MLA(d0, d0, d1);                                /* x + x * (x^2 * poly) */
        return d0;
    }
    if (U64(a2, a1) <= U64(LOADEXP(-27), 0))
        return d0;                                      /* sin(x) = x */
    d6 = trig_dminussixth;
    d2 = MUL(d0, d0);
    d1 = MUL(d2, d6);
    MLA(d0, d1, d0);                                    /* x - x^3/6 */
    return d0;
}

double ros_vfp_cos(struct ros_cpu *s, double x)
{
    double d0 = x, d1, d2, d3, d4, d5, d6, d7;
    uint32_t a1 = LO(d0), a2 = HI(d0);
    if ((a1 | a2 << 1) == 0)
        return trig_done;                               /* cos(+-0) = 1 */
    uint32_t v3 = s->r[6];                              /* never set: the caller's */
    if (EXP(a2) == 0x7FF)
        return nan_or_inf(s, a1, a2);
    if (U64(a2, a1) < U64(1u << 20, 0)) {
        raise_ex(s, FPSCR_UFC);
        return trig_done;
    }
    a2 &= ~SIGN;
    if (U64(a2, a1) > B(trig_dpiby4)) {
        d0 = fabs(d0);
        uint32_t region = range_reduce(s, &d0, &d1, a1, a2, v3) + 1;   /* + pi/2 */
        return sin_quadrant(s, d0, d1, 0, region & 3);
    }
    if (U64(a2, a1) >= U64(LOADEXP(-13), 0)) {
        d2 = T(trig_dCcos, 0), d3 = T(trig_dCcos, 1), d4 = T(trig_dCcos, 2);
        d5 = T(trig_dCcos, 3), d6 = T(trig_dCcos, 4), d7 = T(trig_dCcos, 5);
        d1 = MUL(d0, d0);                               /* x^2 */
        FUSED(d6, d1, d7);
        FUSED(d5, d1, d6);
        FUSED(d4, d1, d5);
        d5 = trig_dhalf;
        d6 = trig_done;
        MLA(d3, d1, d4);
        MLA(d2, d1, d3);
        NMLS(d5, d1, d2);
        d1 = MUL(d1, d5);                               /* x^2 * (powers 2+) */
        d0 = ADD(d6, d1);                               /* 1 + x^2 * (powers 2+) */
        return d0;
    }
    if (U64(a2, a1) <= U64(LOADEXP(-27), 0))
        return trig_done;                               /* cos(x) = 1 */
    d6 = trig_dhalf;
    d2 = MUL(d0, d0);
    d0 = trig_done;
    MLS(d0, d2, d6);                                    /* 1 - x^2/2 */
    return d0;
}

/* TanPiBy4: tan of the doubledouble (d0, d1) in [-pi/4, pi/4], or -1/tan */
static double tan_piby4(struct ros_cpu *s, double d0, double d1, uint32_t recip)
{
    double d2 = trig_d0p68, d3, d4, d5, d6, d7, d8, d9;
    int transform;
    if (d0 > d2) {
        transform = 1;
        d0 = -d0;
        d1 = -d1;
    } else if (d0 >= -d2) {
        transform = 0;
    } else {
        transform = -1;
    }
    if (transform) {
        d6 = trig_dpiby4;
        d7 = trig_dpiby4r;
        d0 = ADD(d6, d0);
        d3 = ADD(d7, d1);
        d0 = ADD(d0, d3);                   /* (piby4 +- x) + (piby4r +- xx) */
        d1 = 0.0;
    }
    /* Remez's approximation to tan(x+xx) on [0, 0.68] */
    d3 = T(trig_dCtan, 0), d4 = T(trig_dCtan, 1), d5 = T(trig_dCtan, 2), d6 = T(trig_dCtan, 3);
    d7 = T(trig_dCtan, 4), d8 = T(trig_dCtan, 5), d9 = T(trig_dCtan, 6);
    d2 = MUL(d0, d1);
    d2 = ADD(d2, d2);
    MLA(d2, d0, d0);                                    /* r = x^2 + 2 * x * xx */
    FUSED(d8, d9, d2);
    FUSED(d7, d8, d2);
    FUSED(d6, d7, d2);                                  /* denominator */
    FUSED(d4, d5, d2);
    FUSED(d3, d4, d2);                                  /* numerator */
    d4 = DIV(d3, d6);
    d4 = MUL(d2, d4);
    MLA(d1, d0, d4);                                    /* (t1, t2) = (d0, d1) */
    d7 = trig_done;
    d3 = ADD(d0, d1);
    if (transform) {
        d5 = recip == 0 ? ADD(d7, d3) : SUB(d3, d7);
        d3 = DIV(d3, d5);
        d3 = ADD(d3, d3);
        d0 = recip == 0 ? SUB(d7, d3) : SUB(d3, d7);
        if (transform == -1)
            d0 = -d0;
        return d0;
    }
    if (recip == 0)
        return d3;
    /* -1.0/(t1 + t2), accurately */
    d4 = D(0, HI(d3));                                  /* z1 */
    d5 = SUB(d4, d0);
    d5 = SUB(d1, d5);                                   /* z2 */
    d3 = DIV(d7, d3);
    d2 = -d3;                                           /* -1/(t1 + t2) */
    d3 = D(0, HI(d2));
    FUSED(d7, d3, d4);
    FUSED(d7, d3, d5);
    FUSED(d3, d2, d7);
    return d3;
}

double ros_vfp_tan(struct ros_cpu *s, double x)
{
    double d0 = x, d1, d2, d3, d4, d5, d6, d7;
    uint32_t a1 = LO(d0), a2 = HI(d0);
    if ((a1 | a2 << 1) == 0)
        return d0;                                      /* tan(+-0) = +-0 */
    uint32_t v3 = EXP(a2);
    if (v3 == 0x7FF)
        return nan_or_inf(s, a1, a2);
    if (U64(a2, a1) < U64(1u << 20, 0)) {
        raise_ex(s, FPSCR_UFC);
        return d0;
    }
    uint32_t v1 = a2 >> 31;
    a2 &= ~SIGN;
    if (U64(a2, a1) > B(trig_dpiby4)) {
        d0 = fabs(d0);
        uint32_t v2;
        if (U64(a2, a1) >= B(trig_d5e5)) {
            remainder_piby2(s, &d0, &d1, &v2);
        } else {
            /* subtract multiples of pi/2 */
            d2 = trig_d2uponpi;
            d7 = trig_d18p52;
            d2 = MUL(d0, d2);                           /* |x| * 2/pi */
            d5 = ADD(d2, d7);
            v2 = LO(d5) & 3;                            /* region */
            d5 = SUB(d5, d7);
            d6 = trig_dpi1;
            d7 = trig_dpi2;
            d3 = MUL(d5, d6);
            d3 = SUB(d0, d3);                           /* rhead */
            d4 = MUL(d5, d7);                           /* rtail */
            d0 = SUB(d3, d4);                           /* r */
            uint32_t ip = v3 - EXP(HI(d0));             /* exponent difference */
            if (ip > 15) {
                d6 = trig_dpi3;                         /* big: the next double down */
                d7 = trig_dpi4;
                d2 = MUL(d5, d6);
                d6 = d3;
                d3 = SUB(d6, d2);
                d4 = SUB(d6, d3);
                d4 = SUB(d4, d2);
                d6 = MUL(d5, d7);
                d4 = SUB(d6, d4);
                if (ip > 48) {
                    d6 = trig_dpi5;                     /* really big: the next again */
                    d7 = trig_dpi6;
                    d2 = MUL(d5, d6);
                    d6 = d3;
                    d3 = SUB(d6, d2);
                    d4 = SUB(d6, d3);
                    d4 = SUB(d4, d2);
                    d6 = MUL(d5, d7);
                    d4 = SUB(d6, d4);
                }
                d0 = SUB(d3, d4);                       /* r */
            }
            d3 = SUB(d3, d0);
            d1 = SUB(d3, d4);                           /* rr = (rhead - r) - rtail */
        }
        d0 = tan_piby4(s, d0, d1, v2 & 1);
        if (v1 & 1)
            d0 = -d0;
        return d0;
    }
    if (U64(a2, a1) >= U64(LOADEXP(-13), 0))
        return tan_piby4(s, d0, 0.0, 0);
    if (U64(a2, a1) <= U64(LOADEXP(-27), 0))
        return d0;                                      /* tan(x) = x */
    d6 = trig_dthird;
    d2 = MUL(d0, d0);
    d3 = MUL(d2, d0);
    MLA(d0, d3, d6);                                    /* x + x^3/3 */
    return d0;
}

/* ---- asin, acos, atan, atan2 (s/ArcTrig64) --------------------------------------------------- */

/* asin's p(z)/q(z) */
static double asin_pq(struct ros_cpu *s, double d0)
{
    double d1, d2, d3, d4, d5, d6, d7;
    d2 = T(arc_dPSin, 0), d3 = T(arc_dPSin, 1), d4 = T(arc_dPSin, 2);
    d5 = T(arc_dPSin, 3), d6 = T(arc_dPSin, 4), d7 = T(arc_dPSin, 5);
    d1 = arc_done;
    FUSED(d6, d0, d7);
    FUSED(d5, d0, d6);
    FUSED(d4, d0, d5);
    MLA(d3, d0, d4);
    MLA(d2, d0, d3);
    d2 = MUL(d0, d2);                                   /* p */
    d4 = T(arc_dQSin, 0), d5 = T(arc_dQSin, 1), d6 = T(arc_dQSin, 2), d7 = T(arc_dQSin, 3);
    FUSED(d6, d0, d7);
    FUSED(d5, d0, d6);
    FUSED(d4, d0, d5);
    MLA(d1, d0, d4);                                    /* q */
    return DIV(d2, d1);
}

double ros_vfp_asin(struct ros_cpu *s, double x)
{
    double d0 = x, d1, d2, d3, d4, d5, d6, d7;
    uint32_t a1 = LO(d0), a2 = HI(d0);
    if ((a1 | a2 << 1) == 0)
        return d0;                                      /* asin(+-0) = +-0 */
    uint32_t v1 = a2 >> 31;
    a2 &= ~SIGN;
    uint32_t a3 = LO(arc_done), a4 = HI(arc_done);
    if (U64(a2, a1) >= U64(a4, a3)) {
        if (U64(a2, a1) != U64(a4, a3)) {
            raise_ex(s, FPSCR_IOC);
            return arc_dqnan;                           /* |x| > 1 */
        }
        d0 = arc_dpiby2;                                /* asin(+-1) = +-pi/2 */
    } else if (U64(a2, a1) < U64(LOADEXP(-1), 0)) {
        if (U64(a2, a1) < U64(LOADEXP(-26), 0)) {
            raise_ex(s, FPSCR_IXC);
            return d0;                                  /* asin(x) = x */
        }
        /* 2^-26 <= |x| < 0.5 */
        d7 = asin_pq(s, MUL(d0, d0));
        MLA(d0, d0, d7);                                /* x + x * p/q */
        return d0;
    } else {
        /* 0.5 <= |x| < 1 */
        d2 = arc_dhalf;
        d1 = D(a3, a4);                                 /* 1.0 */
        d3 = fabs(d0);
        d3 = SUB(d1, d3);
        d0 = MUL(d3, d2);                               /* t = 1/2 * (1 - |x|) */
        d1 = d0;
        d0 = asin_pq(s, d0);
        d3 = arc_dpiby2;
        d4 = arc_dpiby2r;
        d2 = SQRT(d1);                                  /* s */
        if (a2 >= 0x3FEF3333u) {                        /* |x| >= 0.975 */
            MLA(d2, d2, d0);
            d2 = ADD(d2, d2);
            d5 = SUB(d2, d4);
            d0 = SUB(d3, d5);                           /* pi/2 - 2 * (s + s * r) */
        } else {
            d5 = D(0, HI(d2));                          /* w */
            d6 = ADD(d2, d5);
            MLS(d1, d5, d5);
            d1 = DIV(d1, d6);                           /* c */
            d3 = arc_dpiby4;
            d1 = ADD(d1, d1);
            d4 = SUB(d4, d1);
            d6 = MUL(d2, d0);
            d6 = ADD(d6, d6);
            d6 = SUB(d6, d4);                           /* p */
            d5 = ADD(d5, d5);
            d4 = SUB(d3, d5);                           /* q = pi/4 - 2 * w */
            d2 = SUB(d6, d4);
            d0 = SUB(d3, d2);                           /* pi/4 - (p - q) */
        }
    }
    if (v1 & 1)
        d0 = -d0;
    return d0;
}

double ros_vfp_acos(struct ros_cpu *s, double x)
{
    double d0 = x;
    uint32_t a1 = LO(d0), a2 = HI(d0);
    uint32_t v1 = a2 >> 31;
    a2 &= ~SIGN;
    uint32_t a3 = LO(arc_done), a4 = HI(arc_done);
    if (U64(a2, a1) < U64(a4, a3))
        return SUB(arc_dpiby2, ros_vfp_asin(s, d0));    /* pi/2 - asin(x) */
    if (U64(a2, a1) > U64(a4, a3)) {
        raise_ex(s, FPSCR_IOC);
        return arc_dqnan;                               /* |x| > 1 */
    }
    return v1 & 1 ? arc_dpi : D(a3, a3);                /* acos(-1) = pi, acos(1) = 0 */
}

double ros_vfp_atan(struct ros_cpu *s, double x)
{
    double d0 = x, d1, d2, d3, d4, d5, d6, d7, d8;
    uint32_t a1 = LO(d0), a2 = HI(d0);
    int32_t id;
    uint32_t v1 = a2 >> 31;
    a2 &= ~SIGN;
    if (U64(a2, a1) > U64(0x7FF00000u, 0))
        return d0;                                      /* NaNs propagated */
    if (a2 >= LOADEXP(66)) {
        d6 = T(arc_dtanid, 6);
        d7 = T(arc_dtanid, 7);
        d0 = ADD(d6, d7);
        if (v1 & 1)
            d0 = -d0;
        return d0;
    }
    if (U64(a2, a1) < U64(LOADEXP(-27), 0)) {
        raise_ex(s, FPSCR_IXC);
        return d0;                                      /* atan(x) = x */
    }
    if (U64(a2, a1) < U64(0x3FDC0000u, 0)) {            /* 7/16 */
        id = -1;
    } else {
        d7 = arc_done;
        d0 = fabs(d0);
        if (U64(a2, a1) < U64(0x3FF30000u, 0)) {        /* 19/16 */
            if (U64(a2, a1) < U64(0x3FE60000u, 0)) {    /* 11/16 */
                id = 0;
                d5 = ADD(d7, d7);
                d4 = ADD(d5, d0);
                d5 = MUL(d5, d0);
                d5 = SUB(d5, d7);                       /* (t - 0.5) / (1 + t/2), doubled */
            } else {
                id = 1;
                d4 = ADD(d7, d0);
                d5 = SUB(d0, d7);                       /* (t - 1) / (1 + t) */
            }
            d0 = DIV(d5, d4);
        } else if (U64(a2, a1) >= U64(0x40038000u, 0)) { /* 39/16 */
            id = 3;
            d0 = DIV(d7, d0);
            d0 = -d0;                                   /* -1/t */
        } else {
            id = 2;
            d6 = arc_d1point5;
            d4 = MUL(d6, d0);
            d4 = ADD(d7, d4);
            d5 = SUB(d0, d6);                           /* (t - 1.5) / (1 + 1.5*t) */
            d0 = DIV(d5, d4);
        }
    }
    /* the polynomial */
    d1 = MUL(d0, d0);                                   /* t^2 */
    d2 = MUL(d1, d1);                                   /* t^4 */
    d3 = T(arc_dCatanE, 0), d4 = T(arc_dCatanE, 1), d5 = T(arc_dCatanE, 2);
    d6 = T(arc_dCatanE, 3), d7 = T(arc_dCatanE, 4), d8 = T(arc_dCatanE, 5);
    FUSED(d7, d2, d8);
    FUSED(d6, d2, d7);
    FUSED(d5, d2, d6);
    MLA(d4, d2, d5);
    MLA(d3, d2, d4);
    d3 = MUL(d1, d3);                                   /* s1 */
    d4 = T(arc_dCatanO, 0), d5 = T(arc_dCatanO, 1), d6 = T(arc_dCatanO, 2);
    d7 = T(arc_dCatanO, 3), d8 = T(arc_dCatanO, 4);
    FUSED(d7, d2, d8);
    FUSED(d6, d2, d7);
    MLA(d5, d2, d6);
    MLA(d4, d2, d5);
    d4 = MUL(d2, d4);                                   /* s2 */
    d2 = ADD(d3, d4);
    d2 = MUL(d0, d2);                                   /* t * (s1 + s2) */
    if (id < 0)
        return SUB(d0, d2);
    /* a doubledouble add of atan(id) */
    d6 = T(arc_dtanid, 2 * id);
    d7 = T(arc_dtanid, 2 * id + 1);
    d3 = SUB(d2, d7);
    d3 = SUB(d3, d0);
    d0 = SUB(d6, d3);
    if (v1 & 1)
        d0 = -d0;
    return d0;
}

double ros_vfp_atan2(struct ros_cpu *s, double y, double x)
{
    double d0 = y, d1 = x, d2;
    uint32_t a1 = LO(d0), a2 = HI(d0), a3 = LO(d1), a4 = HI(d1);
    const uint32_t inf = 0x7FFu << 20;
    if (U64(a4 & ~SIGN, a3) > U64(inf, 0))
        return d0;              /* meant for y a NaN. A NaN x gives y, and y is never checked. */
    if ((a1 | a2 << 1) == 0) {
        d0 = a4 & SIGN ? arc_dpi : fabs(d0);            /* atan2(+-0, x) = +-0 or +-pi */
    } else if ((a3 | a4 << 1) == 0) {
        d0 = arc_dpiby2;                                /* atan2(y, 0) = +-pi/2 */
    } else if (U64(a2 & ~SIGN, a1) == U64(inf, 0)) {    /* y = INF */
        if (U64(a4 & ~SIGN, a3) != U64(inf, 0)) {
            d0 = arc_dpiby2;
        } else {
            d0 = arc_dpiby4;                            /* atan2(+-INF, +INF) = +-pi/4 */
            d1 = arc_dpiby2;
            if (a4 & SIGN)
                d0 = ADD(d0, d1);                       /* atan2(+-INF, -INF) = +-3*pi/4 */
        }
    } else if (U64(a4 & ~SIGN, a3) == U64(inf, 0)) {    /* x = INF */
        d0 = a4 & SIGN ? arc_dpi : D(0, 0);
    } else {
        /* y/x */
        int32_t ip = (int32_t)EXP(a2) - (int32_t)EXP(a4);
        uint32_t v1 = a2 >> 31;
        if (a4 & SIGN)
            v1 |= 2;                                    /* b1 x's sign, b0 y's */
        if (ip > 60) {
            d0 = arc_dpiby2;                            /* |y/x| > 2^60 */
            v1 &= 1;
        } else if ((v1 & 2) && ip < -60) {
            d0 = D(0, 0);                               /* |y|/x < -2^-60 */
        } else {
            d0 = DIV(d0, d1);
            d0 = fabs(d0);
            d0 = ros_vfp_atan(s, d0);
        }
        if (v1 <= 1)
            return v1 == 1 ? -d0 : d0;                  /* atan2(-,+), atan2(+,+) */
        d1 = arc_dpi;
        d2 = arc_dpir;
        d2 = SUB(d0, d2);
        return v1 < 3 ? SUB(d1, d2) : SUB(d2, d1);      /* atan2(+,-), atan2(-,-) */
    }
    if (a2 & SIGN)
        d0 = -d0;                                       /* y's sign */
    return d0;
}

/* ---- log, log10, exp, pow (s/Power64), ldexp (s/CMath) --------------------------------------- */

/* The part of log and log10 before their polynomials: x is split as
 * 2^k * (1 + f). For the special cases it returns 0 with the result in
 * *r. */
static int log_split(struct ros_cpu *s, double *pd0, double *pd4, uint32_t *hx, double *r)
{
    double d0 = *pd0, d1, d2;
    uint32_t a1 = LO(d0), a2 = HI(d0), v1 = 0x7FFu << 20;
    uint32_t flags = FPSCR_IOC;
    if (a2 == v1 && a1 == 0) {
        /* log(+INF): invalid, and +INF */
    } else if ((a1 | a2 << 1) == 0) {
        v1 |= SIGN;                                     /* log(+-0) = -INF */
        flags = FPSCR_DZC;
    } else if (U64(a2 & ~SIGN, a1) > U64(v1, 0)) {
        *r = d0;                                        /* NaNs propagated */
        return 0;
    } else if (a2 & SIGN) {
        v1 |= 1u << 19;                                 /* x < 0: a quiet NaN */
    } else {
        /* x is not 0, NaN, nor INF, and is positive */
        int32_t k = 0;
        d1 = pow_done;
        uint32_t ip = LOADEXP(-1022);
        if (U64(a2, a1) < U64(ip, 0)) {
            d2 = pow_dtwo54;
            d0 = MUL(d0, d2);
            a1 = LO(d0), a2 = HI(d0);
            k -= 54;                                    /* scaled up by 2^54 */
        }
        k += (int32_t)EXP(a2) - 1023;
        ip -= 1;                                        /* &FFFFF */
        a2 &= ip;
        uint32_t a4 = (0x95F64u + a2) & 1u << 20;
        ip = a2 | (a4 ^ HI(d1));
        d0 = D(LO(d0), ip);                             /* normalised to x or x/2 */
        if (a4)
            k += 1;
        *pd4 = (double)k;
        *pd0 = SUB(d0, d1);                             /* f */
        *hx = a2;
        return 1;
    }
    raise_ex(s, flags);
    *r = D(0, v1);
    return 0;
}

/* log and log10's polynomial: R, with s in *ps */
static double log_poly(struct ros_cpu *s, double d0, double *ps)
{
    double d1 = pow_done, d2, d3, d5, d6, d7, d8, d9, d10;
    d7 = T(pow_dClogO, 0), d8 = T(pow_dClogO, 1), d9 = T(pow_dClogO, 2), d10 = T(pow_dClogO, 3);
    d2 = ADD(d1, d1);                                   /* 2 */
    d3 = ADD(d2, d0);
    d3 = DIV(d0, d3);                                   /* s = f / (2 + f) */
    d5 = MUL(d3, d3);                                   /* s^2 */
    d6 = MUL(d5, d5);                                   /* s^4 */
    FUSED(d9, d6, d10);
    MLA(d8, d6, d9);
    MLA(d7, d6, d8);
    d7 = MUL(d5, d7);
    d8 = T(pow_dClogE, 0), d9 = T(pow_dClogE, 1), d10 = T(pow_dClogE, 2);
    FUSED(d9, d6, d10);
    MLA(d8, d6, d9);
    d8 = MUL(d6, d8);
    *ps = d3;
    return ADD(d7, d8);                                 /* R */
}

double ros_vfp_log10(struct ros_cpu *s, double x)
{
    double d0 = x, d1, d2, d3, d4, d5, d6, d7, r;
    uint32_t hx;
    if (!log_split(s, &d0, &d4, &hx, &r))
        return r;
    d1 = log_poly(s, d0, &d3);
    d2 = pow_dhalf;
    d5 = pow_dlog2;
    d6 = pow_dlog2r;
    d2 = MUL(d0, d2);
    d2 = MUL(d0, d2);                                   /* 1/2 * f^2 */
    d7 = ADD(d2, d1);
    d7 = MUL(d3, d7);
    d1 = SUB(d0, d2);                                   /* hi */
    d1 = D(0, HI(d1));
    d0 = SUB(d0, d1);
    d0 = SUB(d0, d2);
    d2 = pow_d1uponln10;
    d3 = pow_d1uponln10r;
    d0 = ADD(d0, d7);                                   /* lo */
    /* (hi, lo) from base e to 10 */
    d5 = MUL(d4, d5);
    d7 = ADD(d0, d1);
    d7 = MUL(d7, d3);
    MLA(d7, d0, d2);
    MLA(d7, d4, d6);
    MLA(d5, d1, d2);
    return ADD(d5, d7);
}

double ros_vfp_log(struct ros_cpu *s, double x)
{
    double d0 = x, d1, d2, d3, d4, d5, d6, d7, r;
    uint32_t hx;
    if (!log_split(s, &d0, &d4, &hx, &r))
        return r;
    d1 = log_poly(s, d0, &d3);
    d2 = pow_dhalf;
    int32_t i = (int32_t)((hx - 0x6147Au) | (0x6B851u - hx));
    d5 = pow_dln2_20;
    d6 = pow_dln2_20r;
    if (i > 0) {
        d2 = MUL(d0, d2);
        d2 = MUL(d0, d2);                               /* 1/2 * f^2 */
        d7 = ADD(d2, d1);
        d7 = MUL(d3, d7);
        MLA(d7, d4, d6);
        d1 = SUB(d2, d7);
    } else {
        d2 = SUB(d0, d1);
        d1 = MUL(d2, d3);                               /* s * (f - R) */
        MLS(d1, d4, d6);
    }
    d5 = MUL(d5, d4);
    d1 = SUB(d1, d0);
    return SUB(d5, d1);                                 /* k * log(2) - (... - f) */
}

double ros_vfp_exp(struct ros_cpu *s, double y)
{
    double d0 = y, d1, d2, d3, d4, d5, d6, d7, d8;
    uint32_t ip = 0x7FFu << 20, a1 = LO(d0), a2 = HI(d0), a3 = a2 & ~SIGN;
    if (U64(a3, a1) > U64(ip, 0))
        return d0;                                      /* NaNs propagated */
    if (U64(a3, a1) == U64(ip, 0))
        return a2 & SIGN ? D(0, 0) : d0;                /* e^-INF = 0, e^+INF = +INF */
    d1 = pow_dexpmax;
    d2 = pow_dexpmin;
    if (d0 > d1) {
        raise_ex(s, FPSCR_OFC);                         /* will overflow */
        return D(0, ip);
    }
    if (d0 < d2) {
        d0 = SUB(d0, d0);
        raise_ex(s, FPSCR_UFC);                         /* will underflow */
        return d0;
    }
    if (a2 == LOADEXP(0) && a1 == 0)
        return pow_de1;
    int32_t k = 0;
    d2 = D(0, 0);                                       /* hi */
    d1 = d2;                                            /* lo */
    if (a3 <= 0x3FD62E42u) {                            /* |y| <= 1/2 ln(2) */
        if (a3 < LOADEXP(-28)) {
            if (a1 | a2 << 1)
                raise_ex(s, FPSCR_IXC);
            d1 = pow_done;
            return ADD(d1, d0);                         /* 1 + y */
        }
    } else {
        d5 = pow_dln2_20;
        d6 = pow_dln2_20r;
        if (a3 < 0x3FF0A2B2u) {                         /* |y| < 3/2 ln(2) */
            if (a2 & SIGN) {
                d5 = -d5;
                d6 = -d6;
            }
            d2 = SUB(d0, d5);                           /* hi */
            d1 = d6;                                    /* lo */
            k = a2 & SIGN ? -1 : 1;
        } else {
            d3 = pow_dhalf;
            d4 = pow_d1uponln2;
            if (a2 & SIGN)
                d3 = -d3;
            MLA(d3, d4, d0);
            k = vcvt_s32(s, d3);                        /* towards zero */
            d3 = (double)k;
            d5 = MUL(d3, d5);
            d2 = SUB(d0, d5);                           /* hi */
            d1 = MUL(d3, d6);                           /* lo */
        }
        d0 = SUB(d2, d1);                               /* y = hi - lo */
    }
    /* 0 <= |y| <= 1/2 ln(2) */
    d3 = MUL(d0, d0);
    d4 = T(pow_dCexp, 0), d5 = T(pow_dCexp, 1), d6 = T(pow_dCexp, 2);
    d7 = T(pow_dCexp, 3), d8 = T(pow_dCexp, 4);
    FUSED(d7, d3, d8);
    FUSED(d6, d3, d7);
    MLA(d5, d3, d6);
    MLA(d4, d3, d5);
    d5 = pow_done;
    d6 = ADD(d5, d5);                                   /* 2 */
    d4 = MUL(d4, d3);
    d3 = SUB(d0, d4);                                   /* poly */
    if (k == 0) {
        d4 = SUB(d3, d6);
        d7 = MUL(d0, d3);
        d7 = DIV(d7, d4);
        d7 = SUB(d7, d0);
        return SUB(d5, d7);                 /* 1 - ((y * poly)/(poly - 2) - y) */
    }
    d4 = SUB(d6, d3);
    d7 = MUL(d0, d3);
    d7 = DIV(d7, d4);
    d1 = SUB(d1, d7);
    d1 = SUB(d1, d2);
    d1 = SUB(d5, d1);                   /* 1 - ((lo - (y * poly)/(2 - poly)) - hi) */
    /* times 2^k */
    if (k == 1024) {
        d4 = pow_d2pow1023;
        d1 = MUL(d1, d6);
        return MUL(d1, d4);
    }
    a3 = LOADEXP(0) + ((uint32_t)k << 20);
    if (k < -1021)
        a3 += 1000u << 20;                              /* 2^(k+1000) */
    d2 = D(0, a3);
    d0 = MUL(d1, d2);
    if (k < -1021) {
        d2 = D(0, LOADEXP(-1000));
        d0 = MUL(d0, d2);
    }
    return d0;
}

/* LDExp (s/CMath) */
static double ldexp_cm(struct ros_cpu *s, double d0, int32_t v1)
{
    if (v1 == 0)
        return d0;
    uint32_t a1 = LO(d0), a2 = HI(d0);
    int32_t ip = (int32_t)EXP(a2);
    if (ip == 0x7FF)
        return d0;                                      /* NaN or INF */
    if (ip == 0) {                                      /* subnormal or zero */
        if ((a1 | a2 << 1) == 0)
            return d0;
        ip += 1;
        uint32_t lr = a2 & SIGN;
        int more;
        do {
            more = !(a2 & 1u << 19);                    /* the fraction's top bit */
            a2 = a1 >> 31 | a2 << 1;
            a1 <<= 1;
            ip -= 1;
        } while (more);
        a2 = (a2 & ~SIGN) | lr;
    }
    int32_t a4 = ip + v1;
    if (v1 > 0x1000 || a4 >= 0x7FF) {                   /* overflowed */
        raise_ex(s, FPSCR_OFC | FPSCR_IXC);
        return a2 & SIGN ? -cm_dblinf : cm_dblinf;
    }
    if (v1 < -0x1000 || a4 <= -53) {                    /* total underflow */
        raise_ex(s, FPSCR_UFC | FPSCR_IXC);
        return D(0, a2 & SIGN);
    }
    int normal = a4 > 0;
    if (!normal)
        a4 += 53;
    a2 = (a2 & ~0x7FF00000u) | (uint32_t)a4 << 20;
    d0 = D(a1, a2);
    if (normal)
        return d0;
    d0 = MUL(d0, cm_twom53);                            /* subnormal, rounded to nearest */
    if (d0 == 0)
        raise_ex(s, FPSCR_UFC | FPSCR_IXC);
    return d0;
}

double ros_vfp_pow(struct ros_cpu *s, double x, double y)
{
    double d0 = x, d1 = y, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11;
    uint32_t a1 = LO(d0), a2 = HI(d0), a3 = LO(d1), a4 = HI(d1), ip, lr;
    if ((a3 | a4 << 1) == 0)
        return pow_done;                                /* anything^0 = 1 */
    ip = LOADEXP(0);
    if (a2 == ip && a1 == 0)
        return d0;                                      /* 1^anything = 1 */
    if (a4 == ip && a3 == 0)
        return d0;                                      /* anything^1 = itself */
    uint32_t absa2 = a2 & ~SIGN, absa4 = a4 & ~SIGN, yisint = 0, scalen, v5;
    ip = 0x7FFu << 20;
    if (U64(absa2, a1) > U64(ip, 0) || U64(absa4, a3) > U64(ip, 0))
        return ADD(d0, d1);                             /* NaNs propagated */

    /* x is not 1, y is not 0 or 1, neither is a NaN */
    if (a2 & SIGN) {
        if (absa4 >= LOADEXP(53)) {
            yisint = 2;                                 /* |y| >= bits in the mantissa */
        } else if (absa4 >= LOADEXP(0)) {
            lr = (absa4 >> 20) - (LOADEXP(0) >> 20) + 1 + 11;   /* sign and exponent too */
            uint32_t lost, hi, lo;
            if (lr < 32) {
                lost = a4 >> (32 - lr) & 1;
                hi = a4 << lr | a3 >> (32 - lr);
                lo = a3 << lr;
            } else {
                ip = lr - 32;
                lost = ip ? a3 >> (32 - ip) & 1 : 1;    /* by 0: RSBS's carry */
                hi = ip < 32 ? a3 << ip : 0;
                lo = 0;
            }
            if ((lo | hi) == 0)
                yisint = lost ? 1 : 2;                  /* no fraction: odd or even */
        }
    }
    ip = 0x7FFu << 20;

    /* y's interesting values */
    if (a3 == 0) {
        uint32_t one = LOADEXP(0);
        if (absa4 == ip) {                              /* y = +-INF */
            if (U64(absa2, a1) == U64(one, 0))
                return D(a3, ip | 1u << 19);            /* +-1^+-INF: a quiet NaN */
            if (U64(absa2, a1) > U64(one, 0))
                a4 ^= SIGN;
            return a4 & SIGN ? D(a3, ip) : D(a3, a3);
        }
        if (a4 == LOADEXP(1))
            return MUL(d0, d0);                         /* x^2 */
        if (a4 == LOADEXP(-1) && !(a2 & SIGN))
            return SQRT(d0);                            /* x^(1/2), x positive */
    }
    /* x's interesting values */
    if (a1 == 0) {
        if (absa2 == ip) {                              /* x = +-INF */
            d0 = (a2 | a4) & SIGN ? D(a1, a1) : D(a1, ip);
            if (a2 & SIGN)
                d0 = -d0;
            return d0;
        }
        if ((a2 << 1) == 0) {
            if (a2 & SIGN) {                            /* x = -0 */
                if (a4 & SIGN) {
                    d0 = D(a1, ip);
                    raise_ex(s, FPSCR_DZC);
                } else {
                    d0 = fabs(d0);
                }
                if (yisint == 1)
                    d0 = -d0;
                return d0;
            }
            if (a4 & SIGN) {                            /* x = +0 */
                d0 = D(a1, ip);
                raise_ex(s, FPSCR_DZC);
            } else {
                d0 = D(a1, a1);
            }
            return d0;
        }
    }

    /* x and y are not 0, 1, +-INF or NaN */
    d0 = fabs(d0);
    if ((int32_t)(yisint + (uint32_t)((int32_t)a2 >> 31)) < 0) {
        d0 = D(yisint, ip | 1u << 19);                  /* -ve^nonint: a quiet NaN */
        raise_ex(s, FPSCR_IOC);
        return d0;
    }
    uint32_t v4 = LOADEXP(0);
    if (absa4 > LOADEXP(64))
        goto big;                                       /* will over or underflow */
    if (absa4 <= LOADEXP(31))
        goto normal;
    if (absa2 == v4)
        goto near_one;
big:
    if (absa2 < v4)
        a4 ^= SIGN;
    if (a4 & SIGN) {
        d0 = pow_dtiny;
        raise_ex(s, FPSCR_UFC);
    } else {
        d0 = pow_dhuge;
        raise_ex(s, FPSCR_OFC);
    }
    d0 = MUL(d0, d0);
    goto sign;

near_one:
    /* |y| > 2^31, |x| very close to 1: log(x) by its Taylor series */
    d5 = T(pow_dCtaylor, 0), d6 = T(pow_dCtaylor, 1), d7 = T(pow_dCtaylor, 2);
    d4 = pow_dunity;
    d4 = SUB(d0, d4);                                   /* t */
    MLS(d6, d4, d7);
    MLS(d5, d4, d6);
    d5 = MUL(d4, d5);
    d5 = MUL(d4, d5);
    d3 = pow_d1uponln2;
    d6 = pow_d1uponln2s;
    d7 = pow_d1uponln2sr;
    d6 = MUL(d6, d4);                                   /* to base 2 */
    d4 = MUL(d4, d7);
    MLS(d4, d5, d3);
    d2 = ADD(d6, d4);
    d2 = D(0, HI(d2));
    d3 = SUB(d2, d6);
    d3 = SUB(d4, d3);
    goto product;                                       /* (t1, t2) in d2, d3 */

normal:
    /* |y| <= 2^31 */
    scalen = 0;
    if (absa2 < 1u << 20) {                             /* subnormal */
        d7 = pow_d2pow53;
        scalen -= 53;
        d0 = MUL(d0, d7);
        absa2 = HI(d0);
    }
    v5 = EXP(absa2) - 1023;
    scalen += v5;
    v5 = absa2 & 0x000FFFFFu;
    absa2 = v5 | 1023u << 20;                           /* normalised x */
    a3 = 0;                                             /* the interval */
    if (v5 > 0x3988Eu) {                                /* sqrt(3/2) */
        if (v5 < 0xBB67Au) {                            /* sqrt(3) */
            a3 = 1;
        } else {
            scalen += 1;
            absa2 -= 1u << 20;
        }
    }
    d0 = D(LO(d0), absa2);
    /* ss = s_h + s_l = (x - 1)/(x + 1) or (x - 1.5)/(x + 1.5) */
    d6 = pow_dunity;
    d7 = a3 == 0 ? d6 : pow_d3over2;
    d4 = SUB(d0, d7);
    d5 = ADD(d0, d7);
    d5 = DIV(d6, d5);
    d2 = MUL(d4, d5);                                   /* ss */
    d3 = D(0, HI(d2));                                  /* s_h */
    lr = ((absa2 >> 1) | 0x20000000u) + (1u << 19) + (a3 << 18);
    d6 = D(0, lr);
    d7 = SUB(d6, d7);
    d7 = SUB(d0, d7);
    MLS(d4, d3, d6);
    MLS(d4, d3, d7);
    d4 = MUL(d5, d4);                                   /* s_l */
    /* log(ax) */
    d6 = T(pow_dClogx, 0), d7 = T(pow_dClogx, 1), d8 = T(pow_dClogx, 2);
    d9 = T(pow_dClogx, 3), d10 = T(pow_dClogx, 4), d11 = T(pow_dClogx, 5);
    d5 = MUL(d2, d2);                                   /* ss^2 */
    FUSED(d10, d5, d11);
    FUSED(d9, d5, d10);
    FUSED(d8, d5, d9);
    MLA(d7, d5, d8);
    MLA(d6, d5, d7);
    d6 = MUL(d6, d5);
    d6 = MUL(d6, d5);                                   /* poly */
    d7 = ADD(d3, d2);
    MLA(d6, d4, d7);
    d5 = MUL(d3, d3);                                   /* s_h^2 */
    d7 = pow_d3over2;
    d7 = ADD(d7, d7);                                   /* 3 */
    d8 = ADD(d7, d5);
    d8 = ADD(d8, d6);
    d8 = D(0, HI(d8));
    d9 = SUB(d8, d7);
    d9 = SUB(d9, d5);
    d9 = SUB(d6, d9);
    d10 = MUL(d3, d8);
    d11 = MUL(d4, d8);
    MLA(d11, d9, d2);
    d4 = ADD(d10, d11);
    d4 = D(0, HI(d4));
    d5 = SUB(d4, d10);
    d5 = SUB(d11, d5);
    d6 = pow_d2o3ln2;
    d7 = pow_d2o3ln2_28;
    d8 = pow_d2o3ln2_28r;
    if (a3 != 0) {
        d9 = pow_dCdp;
        d10 = pow_dCdpr;
    } else {
        d9 = D(a3, a3);
        d10 = D(a3, a3);
    }
    d7 = MUL(d7, d4);
    d8 = MUL(d8, d4);
    MLA(d8, d5, d6);
    d8 = ADD(d8, d10);
    d6 = (double)(int32_t)scalen;
    d2 = ADD(d7, d8);
    d2 = ADD(d2, d9);
    d2 = ADD(d2, d6);
    d2 = D(0, HI(d2));
    d4 = SUB(d2, d6);
    d4 = SUB(d4, d9);
    d4 = SUB(d4, d7);
    d3 = SUB(d8, d4);                                   /* (t1, t2) in d2, d3 */

product:
    /* y as y1 + y2, and (y1 + y2) * (t1 + t2) */
    d0 = D(0, a4);                                      /* y's high word */
    d4 = SUB(d1, d0);
    d4 = MUL(d4, d2);
    MLA(d4, d1, d3);                                    /* product lo */
    d5 = MUL(d0, d2);                                   /* product hi */
    d0 = ADD(d4, d5);                                   /* z */
    a3 = LO(d0), a4 = HI(d0);
    absa4 = a4 & ~SIGN;
    if (!(a4 & SIGN)) {
        if (U64(a4, a3) < U64(0x40900000u, 0))
            goto two;
        if (U64(a4, a3) == U64(0x40900000u, 0)) {       /* z = 1024: the tie breaker */
            d6 = pow_dovftie;
            d6 = ADD(d6, d4);
            d7 = SUB(d0, d4);
            if (!(d6 > d7))
                goto two;
        }
        raise_ex(s, FPSCR_OFC);                         /* z > 1024 */
        d0 = pow_dhuge;
        d0 = MUL(d0, d0);
        goto sign;
    }
    if (absa4 < 0x4090CC00u)
        goto two;
    if (U64(a4, a3) == U64(0xC090CC00u, 0)) {           /* z = -1075: the tie breaker */
        d7 = SUB(d0, d5);
        if (d4 > d7)
            goto two;
    }
    raise_ex(s, FPSCR_UFC);
    d0 = pow_dtiny;
    d0 = MUL(d0, d0);
    goto sign;

two:
    /* 2^(prodhi + prodlo) */
    {
        int32_t k = (int32_t)EXP(a4) - 1023;
        if (absa4 <= LOADEXP(-1)) {
            scalen = 0;
        } else {                                        /* |z| > 0.5 */
            lr = 1u << 20;
            v5 = (uint32_t)(k + 1);
            scalen = a4 + (v5 < 32 ? lr >> v5 : 0);     /* the new scale */
            uint32_t ek = ((scalen & ~SIGN) >> 20) - 1023;   /* its exponent */
            lr -= 1;                                    /* &FFFFF */
            v5 = scalen & ~(ek < 32 ? lr >> ek : 0);
            d2 = D(0, v5);
            scalen = ((scalen & lr) | 1u << 20) >> (20 - ek);
            if (a4 & SIGN)
                scalen = -scalen;
            d5 = SUB(d5, d2);
        }
    }
    d1 = pow_dln2;
    d2 = pow_dln2_32;
    d3 = pow_dln2_32r;
    d0 = ADD(d4, d5);
    d0 = D(0, HI(d0));
    d6 = SUB(d0, d5);
    d6 = SUB(d4, d6);
    d1 = MUL(d1, d6);
    MLA(d1, d0, d3);
    d3 = MUL(d0, d2);
    d2 = ADD(d3, d1);                                   /* z = u + v */
    d4 = SUB(d2, d3);
    d1 = SUB(d1, d4);
    d3 = T(pow_dCpow, 0), d4 = T(pow_dCpow, 1), d5 = T(pow_dCpow, 2);
    d6 = T(pow_dCpow, 3), d7 = T(pow_dCpow, 4);
    d0 = MUL(d2, d2);                                   /* z^2 */
    FUSED(d6, d0, d7);
    FUSED(d5, d0, d6);
    FUSED(d4, d0, d5);
    MLA(d3, d0, d4);
    d3 = MUL(d0, d3);
    d3 = SUB(d2, d3);                                   /* poly */
    d7 = pow_dunity;
    d6 = ADD(d7, d7);                                   /* 2 */
    MLA(d1, d2, d1);
    d4 = MUL(d2, d3);
    d5 = SUB(d3, d6);
    d4 = DIV(d4, d5);
    d4 = SUB(d4, d1);                                   /* r */
    d2 = SUB(d4, d2);
    d2 = SUB(d7, d2);                                   /* 1 - (r - z) */
    /* the exponent */
    a3 = LO(d2);
    a4 = HI(d2) + (scalen << 20);
    if ((int32_t)a4 >> 20 > 0)
        d0 = D(a3, a4);
    else
        d0 = ldexp_cm(s, d2, (int32_t)scalen);          /* subnormal */

sign:
    if ((a2 & SIGN) && yisint == 1)
        d0 = -d0;                                       /* -ve^oddint = -ve */
    return d0;
}
