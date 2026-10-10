/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_fp.c: floating point, compiled from ObjAsm and lifted to C
 * expressions (modules/fptest), against the same computations in C.
 *
 * The comparison is bit for bit.  The lifted code uses the same IEEE
 * operations and the same C library as the C here, so every result must
 * match exactly.  That includes NaNs, infinities and signed zeros.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

static int same(double x, double y)
{
    return memcmp(&x, &y, sizeof x) == 0;
}

static int same_f(float x, float y)
{
    return memcmp(&x, &y, sizeof x) == 0;
}

void ros_selftest_fp(void)
{
    ros_console_printf("rosgd: self-test -- FPA and VFP, lifted to C expressions\n");
    static const struct { double a, b, c; } cases[] = {
        { 3.0, 4.0, 2.5 },
        { -1.25, 0.5, -3.0 },
        { 0.1, 0.2, -0.0 },
        { 1e-300, 1e300, NAN },
        { 3.141592653589793, -2.718281828459045, INFINITY },
        { 12345678.9, -0.001, -INFINITY },
    };
    const uint64_t third_bits = 0x3FD5555555555555ull;
    double third;
    memcpy(&third, &third_bits, 8);
    uint8_t *block = ros_rma_alloc(128);
    uint32_t at = ros_addr(block);
    unsigned fpa_ok = 0, vfp_ok = 0, n = sizeof cases / sizeof cases[0];
    char what[160];

    for (unsigned i = 0; i < n; i++) {
        double a = cases[i].a, b = cases[i].b, c = cases[i].c;
        uint32_t r = 0;

        /* FPA: doubles high word first. */
        ros_fpa_std(at, a);
        ros_fpa_std(at + 8, b);
        ros_fpa_std(at + 16, c);
        os_error *e = xfptest_fpa(block, &r);
        int ok = !e && same(ros_fpa_ldd(at + 32), sqrt(a * a + b * b)) &&
                 same(ros_fpa_ldd(at + 40), sin(a) * cos(b) + third) &&
                 same(ros_fpa_ldd(at + 48), atan2(b, a)) &&
                 same(ros_fpa_ldd(at + 56), !(c >= 0.0) ? 0.0 : c) &&
                 same_f((float)ros_fpa_lds(at + 64), (float)(a + b)) &&
                 (int32_t)r == ros_to_int(a * 10.0 * 10.0, ROS_ROUND_ZERO);
        if (ok)
            fpa_ok++;
        else
            ros_console_printf("  ..    FPA case %u (a %g, b %g, c %g): %s, r %d\n", i, a, b, c,
                               e ? e->errmess : "a result differs", (int32_t)r);

        /* VFP: doubles little-endian. */
        memcpy(block, &a, 8);
        memcpy(block + 8, &b, 8);
        memcpy(block + 16, &c, 8);
        e = xfptest_vfp(block, &r);
        double hyp, f, twice, clamp;
        float single;
        memcpy(&hyp, block + 32, 8);
        memcpy(&f, block + 40, 8);
        memcpy(&twice, block + 48, 8);
        memcpy(&clamp, block + 56, 8);
        memcpy(&single, block + 64, 4);
        ok = !e && same(hyp, sqrt(a * a + b * b)) && same(f, fma(a, b, 1.0)) &&
             same(twice, a * 2.0) && same(clamp, c >= 0.0 ? c : 0.0) &&
             same_f(single, (float)a + (float)b) && (int32_t)r == ros_to_int(a, ROS_ROUND_ZERO);
        if (ok)
            vfp_ok++;
        else
            ros_console_printf("  ..    VFP case %u (a %g, b %g, c %g): %s, r %d\n", i, a, b, c,
                               e ? e->errmess : "a result differs", (int32_t)r);
    }
    snprintf(what, sizeof what, "FPA lifted to C: %u input sets, every result bit-exact", n);
    check(fpa_ok == n, what, "%u of %u", fpa_ok, n);
    snprintf(what, sizeof what, "VFP lifted to C: %u input sets, every result bit-exact", n);
    check(vfp_ok == n, what, "%u of %u", vfp_ok, n);
    ros_rma_free(block);

    /* STFP and LDFP's packed decimal, as FPA lays it out: d0.d1...d18 x
     * 10^exponent, with d0-d2 in word 0's bottom twelve bits.  The FPE
     * writes seventeen digits, and RISC OS 5.30's STR$ shows them all. */
    static const struct { double x; uint32_t w[3]; } packed[] = {
        { 2.5, { 0x00000250, 0, 0 } },
        { 42, { 0x00001420, 0, 0 } },
        { 123456, { 0x00005123, 0x45600000, 0 } },
        { -0.0625, { 0xC0002625, 0, 0 } },
        { 1.0 / 3, { 0x40001333, 0x33333333, 0x33333100 } },
        { 3.141592653589793, { 0x00000314, 0x15926535, 0x89793100 } },  /* 3.1415926535897931 */
        { -1e-5, { 0xC0005100, 0, 0x00000100 } },                       /* -1.0000000000000001E-5 */
        { 1e38, { 0x00037999, 0x99999999, 0x99999800 } },               /* past 2^63 */
        { 5e-324, { 0x40324494, 0x06564584, 0x12465400 } },             /* the least denormal */
    };
    uint32_t pk = ros_addr(ros_rma_alloc(16));
    unsigned pk_ok = 0, pn = sizeof packed / sizeof packed[0];
    for (unsigned i = 0; i < pn; i++) {
        ros_fpa_stp(pk, packed[i].x);
        uint32_t *w = ros_ptr(pk);
        if (w[0] == packed[i].w[0] && w[1] == packed[i].w[1] && w[2] == packed[i].w[2] &&
            ros_fpa_ldp(pk) == packed[i].x)
            pk_ok++;
        else
            ros_console_printf("  ..    packed %g: %08X %08X %08X, back %.17g\n", packed[i].x, w[0],
                               w[1], w[2], ros_fpa_ldp(pk));
    }
    check(pk_ok == pn, "FPA packed decimal: STFP's layout, and LDFP back exactly", "%u of %u", pk_ok,
          pn);
    ros_rma_free(ros_ptr(pk));
}
