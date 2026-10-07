/* cs-boxshim.c */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Copyright (C) 2026 the PipeDream box port */

/* The box-port shim: what the RISC OS build found elsewhere.
 *
 *  - zig clang's remapped libm calls (__d_floor and friends), which the
 *    DDE build satisfied from the SharedCLibrary and FreeBSD msun (not
 *    shipped in the repository): floor/ceil/trunc by bit manipulation,
 *    atan by CORDIC (one bit of angle per step, so 60-odd steps reach
 *    double precision)
 *  - clang's complex helpers __muldc3/__divdc3 (compiler-rt's canonical
 *    algorithms), which native double _Complex arithmetic lowers to
 *  - the Report and RPCEmu debug SWI veneers report.c declares (the
 *    Norcroft __swi attribute form does not exist on clang)
 */

#include "swis.h"

#include "kernel.h"

#include <stdint.h>
#include <stdio.h>

typedef union { double d; uint64_t u; } du_t;

/* ---- floor / ceil / trunc ------------------------------------------- */

static double frac_clear(double x)
{
    du_t v; v.d = x;
    int e = (int)((v.u >> 52) & 0x7FF);
    if(e >= 0x433) return x;                    /* integral, inf or nan */
    uint64_t mask = e <= 52 ? (((1ULL << 52) - 1) >> (52 - e)) : (uint64_t)0;
    du_t t; t.d = x; t.u &= ~mask;
    return t.d;
}

double __d_floor(double x)
{
    double t = frac_clear(x);
    if(t != x && x < 0.0)                       /* fraction, negative: down one */
    {
        du_t u; u.d = t; u.u -= 1;
        return u.d;
    }
    return t;
}

double __d_ceil(double x)
{
    double t = frac_clear(x);
    if(t != x && x > 0.0)
    {
        du_t u; u.d = t; u.u += 1;
        return u.d;
    }
    return t;
}

double __d_trunc(double x)
{
    return frac_clear(x);
}

/* sqrt by Newton from a halved-exponent seed; six steps reach double
 * precision (the freestanding build has no libm to call) */
static double __d_sqrt_approx(double x)
{
    if(x <= 0.0) return 0.0;
    du_t v; v.d = x;
    v.u = (v.u >> 1) + (1ULL << 61);
    double r = v.d;
    for(int i = 0; i < 6; i++)
        r = 0.5 * (r + x / r);
    return r;
}

/* ---- atan by CORDIC --------------------------------------------------- */

static const double atan_pi = 3.14159265358979323846;

double __d_atan(double x)
{
    static const double atab[64] = {
        7.85398163397448278999e-01, 4.63647609000806116214e-01,
        2.44978663126864154173e-01, 1.24354994546761435030e-01,
        6.24188099959573501093e-02, 3.12398184302046304529e-02,
        1.56237286204768108084e-02, 7.81234106010111114969e-03,
        3.90623013196697182264e-03, 1.95312251647881829657e-03,
        9.76562189559319407820e-04, 4.88281211130847972443e-04,
        2.44140624948821808161e-04, 1.22070312499487673719e-04,
        6.10351562496245991587e-05, 3.05175781249899009894e-05,
        1.52587890625004618315e-05, 7.62939453125004563396e-06,
        3.81469726562501371742e-06, 1.90734863281250675822e-06,
        9.53674316406253516994e-07, 4.76837158203130653058e-07,
        2.38418579101560517082e-07, 1.19209289550782886181e-07,
        5.96046447753906263151e-08, 2.98023223876953142686e-08,
        1.49011611938476557403e-08, 7.45058059692382787884e-09,
        3.72529029846191405626e-09, 1.86264514923078578398e-09,
        9.31322574615478521261e-10, 4.65661287307739275614e-10,
        2.32830643653869630324e-10, 1.16415321826934843700e-10,
        5.82076609134674161316e-11, 2.91038304567337057715e-11,
        1.45519152283668579402e-11, 7.27595761418342320336e-12,
        3.63797880709171179664e-12, 1.81898940354585561819e-12,
        9.09494701772928249766e-13, 4.54747350886464128641e-13,
        2.27373675443232065155e-13, 1.13686837721616028757e-13,
        5.68434188608080167582e-14, 2.84217094304040066931e-14,
        1.42108547152020036402e-14, 7.10542735760100184363e-15,
        3.55271367880050094573e-15, 1.77635683940025048454e-15,
        8.88178419700125157488e-16, 4.44089209850062591335e-16,
        2.22044604925031311128e-16, 1.11022302462515660074e-16,
        5.55111512312578316394e-17, 2.77555756156289161482e-17,
        1.38777878078144579767e-17, 6.93889390390722886129e-18,
        3.46944695195361437271e-18, 1.73472347597680717031e-18,
        8.67361737988403583453e-19, 4.33680868994201792537e-19,
        2.16840434497100893419e-19, 1.08420217248550446088e-19,
        5.42101086242752228268e-20, 2.71050543121376112936e-20
    };
    static const double ktab[64] = {
        1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625, 0.0078125,
        0.00390625, 0.001953125, 0.0009765625, 0.00048828125,
        0.000244140625, 0.0001220703125, 6.103515625e-05, 3.0517578125e-05,
        1.52587890625e-05, 7.62939453125e-06, 3.814697265625e-06, 1.9073486328125e-06,
        9.5367431640625e-07, 4.76837158203125e-07, 2.384185791015625e-07, 1.1920928955078125e-07,
        5.9604644775390625e-08, 2.9802322387695312e-08, 1.4901161193847656e-08, 7.450580596923828e-09,
        3.725290298461914e-09, 1.862645149230957e-09, 9.313225746154785e-10, 4.656612873076904e-10,
        2.328306436538452e-10, 1.164153218269226e-10, 5.820766091346131e-11, 2.910383045673065e-11,
        1.455191522836533e-11, 7.275957614182654e-12, 3.637978807091327e-12, 1.818989403545663e-12,
        9.094947017728316e-13, 4.54747350864158e-13, 2.27373675432079e-13, 1.136868377160395e-13,
        5.684341885801976e-14, 2.842170942900988e-14, 1.421085471450494e-14, 7.10542735725247e-15,
        3.552713678626235e-15, 1.7763568393131175e-15, 8.881784196565587e-16, 4.440892098282794e-16,
        2.220446049141397e-16, 1.1102230245706985e-16, 5.5511151228534925e-17, 2.7755575614267463e-17,
        1.3877787807133731e-17, 6.938893903566866e-18, 3.469446951783433e-18, 1.7347234758917164e-18,
        8.673617379458582e-19, 4.336808689729291e-19, 2.1684043448646454e-19, 1.0842021724323227e-19
    };

    du_t v; v.d = x;
    int neg = (int)(v.u >> 63);
    v.u &= ~(1ULL << 63);
    x = v.d;

    if(x != x) return x;                        /* nan */
    if(x > 1.0e308)                             /* beyond: pi/2 */
    {
        double half = atan_pi / 2.0;
        return neg ? -half : half;
    }

    double angle;
    int complement = 0;
    if(x > 1.0)
    {
        complement = 1;
        x = 1.0 / x;
    }

    double y = x, z = 1.0;
    angle = 0.0;
    for(int i = 0; i < 64; i++)
    {
        if(y <= 0.0) break;
        double y2 = y + z * ktab[i];
        double z2 = z - y * ktab[i];
        if(y2 <= 0.0) break;                    /* reached the axis */
        angle += atab[i];
        y = y2; z = z2;
        /* renormalise: CORDIC's vector grows by sqrt(1+k^2) each step;
         * dividing keeps y/z stable without needing the gain product */
        double n = 1.0 / __d_sqrt_approx(y * y + z * z);
        y *= n; z *= n;
    }

    if(complement)
        angle = atan_pi / 2.0 - angle;

    return neg ? -angle : angle;
}

/* ---- complex helpers (compiler-rt's canonical algorithms) ------------- */

typedef struct { double r, i; } dc3_t;

static int is_d_nan(double x)  { du_t v; v.d = x; return ((v.u >> 52) & 0x7FF) == 0x7FF && (v.u & ((1ULL << 52) - 1)) != 0; }
static int is_d_inf(double x)  { du_t v; v.d = x; return ((v.u >> 52) & 0x7FF) == 0x7FF && (v.u & ((1ULL << 52) - 1)) == 0; }
static double d_inf(void)      { du_t v; v.u = 0x7FF0000000000000ULL; return v.d; }
static double d_nan(void)      { du_t v; v.u = 0x7FF8000000000000ULL; return v.d; }
static double d_copysign(double x, double y) { du_t a, b; a.d = x; b.d = y; a.u = (a.u & ~(1ULL << 63)) | (b.u & (1ULL << 63)); return a.d; }

dc3_t __muldc3(double a, double b, double c, double d)
{
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    dc3_t z;
    z.r = ac - bd;
    z.i = ad + bc;
    if(is_d_nan(z.r) && is_d_nan(z.i))
    {
        if((is_d_inf(a) || is_d_inf(b)) && (c == 0.0 || d == 0.0))
        { z.r = d_inf(); z.i = d_inf(); return z; }
        if((is_d_inf(c) || is_d_inf(d)) && (a == 0.0 || b == 0.0))
        { z.r = d_inf(); z.i = d_inf(); return z; }
        if(is_d_nan(ac) && is_d_nan(bd))
        { z.r = d_nan() + d_nan(); z.i = z.r; return z; }
        if(is_d_nan(ad) && is_d_nan(bc))
        { z.r = d_nan() + d_nan(); z.i = z.r; return z; }
        if(is_d_nan(z.i))
        {
            double rr = is_d_nan(z.r) ? 0.0 : z.r;
            z.r = rr;
        }
    }
    return z;
}

dc3_t __divdc3(double a, double b, double c, double d)
{
    double sc = 1.0;
    du_t v; v.d = c;
    int ex = (int)((v.u >> 52) & 0x7FF);
    v.d = d;
    int ey = (int)((v.u >> 52) & 0x7FF);
    int logbw = ex > ey ? ex : ey;
    /* scale the denominator's parts down when they are large, to keep
     * the products in range (compiler-rt's ilogb guard, simplified) */
    if(logbw > 0x3F0)
    {
        du_t s; s.u = (uint64_t)(1023 - (logbw - 0x3F0)) << 52;
        sc = s.d;
    }
    double cc = c * sc, dd = d * sc;
    double denom = cc * cc + dd * dd;
    dc3_t z;
    z.r = (a * cc + b * dd) * sc / denom;
    z.i = (b * cc - a * dd) * sc / denom;
    if((is_d_nan(z.r) && is_d_nan(z.i)) || denom == 0.0)
    {
        if(denom == 0.0 && (a != 0.0 || b != 0.0))
        { z.r = d_inf(); z.i = d_inf(); }
        else if((is_d_inf(a) || is_d_inf(b)) && !is_d_inf(cc) && !is_d_inf(dd))
        {
            a = is_d_inf(a) ? d_copysign(1.0, a) : d_copysign(0.0, a);
            b = is_d_inf(b) ? d_copysign(1.0, b) : d_copysign(0.0, b);
            z.r = d_inf() * (a * cc + b * dd) * sc / denom;
            z.i = d_inf() * (b * cc - a * dd) * sc / denom;
        }
    }
    return z;
}

/* ---- the Report and RPCEmu debug veneers ------------------------------- */

int __swi_XReport_Text0(const char *s)
{
    return _swix(0x54C80 | (1u << 17), _IN(0), s) ? -1 : 0;
}

int __swi_XRPCEmu_Debug(const char *s)
{
    return _swix(0x56AC2 | (1u << 17), _IN(0), s) ? -1 : 0;
}
