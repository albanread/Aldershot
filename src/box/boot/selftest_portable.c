/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_portable.c: Portable, reimplemented (modules/portable): what
 * must always hold. ReadFeatures says Idle alone. Speed keeps its setting
 * by (old AND R1) EOR R0. Idle comes back, having let queued work run. The
 * rest fail with the module's numbers and texts (s/StPortable). */
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "selftest.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[8]) { return ((os_error *)ros_ptr(r[0]))->errnum; }

static int ran;
static void work(void *arg, uint32_t info) { (void)arg, (void)info; ran = 1; }

void ros_selftest_portable(void)
{
    uint32_t f[8] = { 0, 0x12345678u, 2, 3 };
    int vf = swi(XPortable_ReadFeatures, f);
    check(!vf && f[1] == 0x10 && f[0] == 0 && f[2] == 2 && f[3] == 3,
          "Portable_ReadFeatures -- Idle (bit 4) alone, no CPU clock device; the rest kept",
          "V%d R1 &%X", vf, f[1]);

    uint32_t s1[8] = { 1, 0 }, s2[8] = { 0, 0xFFFFFFFFu }, s3[8] = { 0, 0 };
    int v1 = swi(XPortable_Speed, s1), v2 = swi(XPortable_Speed, s2), v3 = swi(XPortable_Speed, s3);
    check(!v1 && s1[0] == 0 && s1[1] == 1 && !v2 && s2[0] == 1 && s2[1] == 1 && !v3 &&
              s3[0] == 1 && s3[1] == 0,
          "Portable_Speed -- (old AND R1) EOR R0: set slow, read, set fast; R0 the old, R1 the new",
          "%u %u / %u %u / %u %u", s1[0], s1[1], s2[0], s2[1], s3[0], s3[1]);

    /* Idle: queued work runs, as the interrupt ends a WFI; with none, it
     * comes back within the tick */
    ran = 0;
    ros_post(work, NULL, 0);
    uint32_t i1[8] = { 0 };
    int vi1 = swi(XPortable_Idle, i1);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t i2[8] = { 0 };
    int vi2 = swi(XPortable_Idle, i2);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(!vi1 && ran && !vi2 && ms < 500,
          "Portable_Idle -- queued work runs; with none it comes back within the tick",
          "V%d ran %d; V%d after %ld ms", vi1, ran, vi2, ms);

    uint32_t sp2[8] = { 0 }, en[8] = { -1u, 1 }, en0[8] = { -1u, 0 }, rv[8] = { 0 }, rs[8] = { 0 },
             ct[8] = { 0 };
    int e1 = swi(XPortable_Speed2, sp2), e2 = swi(XPortable_EnumerateBMU, en),
        e3 = swi(XPortable_EnumerateBMU, en0), e4 = swi(XPortable_ReadBMUVariable, rv),
        e5 = swi(XPortable_ReadSensor, rs), e6 = swi(XPortable_Control, ct);
    check(e1 && errnum(sp2) == 0xB48 && e2 && errnum(en) == 0xB4A && e3 && errnum(en0) == 0xB4B &&
              e4 && errnum(rv) == 0xB4B && e5 && errnum(rs) == 0xB4C && e6 && errnum(ct) == 0x1E6,
          "Portable_Speed2, EnumerateBMU, ReadBMUVariable, ReadSensor, Control -- no clock, no "
          "BMU, no sensor: &B48, &B4A/&B4B, &B4B, &B4C, &1E6", NULL);
}
