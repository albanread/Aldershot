/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_osbyte.c: OS_Byte and OS_Word, against the kernel's own
 * (Kernel/s/PMF/osbyte, osword). They are used through the SWIs, as programs
 * call them. OS_ReadSysInfo (runtime/sysinfo.c) is checked too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/ticker.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

static void wait_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    ROS_BLOCKING(nanosleep(&ts, NULL));
}

/* OS_Byte, R0-R2 in and out; V back. */
static int byte(uint32_t r[3])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 3 * sizeof r[0]);
    ros_swi(&s, XOS_Byte);
    memcpy(r, s.r, 3 * sizeof r[0]);
    return s.v;
}

static int word(uint32_t reason, uint32_t block)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = reason, s.r[1] = block;
    ros_swi(&s, XOS_Word);
    return s.v;
}

static int sysinfo(uint32_t r[5])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 5 * sizeof r[0]);
    ros_swi(&s, XOS_ReadSysInfo);
    memcpy(r, s.r, 5 * sizeof r[0]);
    return s.v;
}

static uint64_t ld5(uint32_t a)
{
    uint64_t v = 0;
    for (int i = 4; i >= 0; i--)
        v = v << 8 | ros_ld8(a + (uint32_t)i);
    return v;
}

static void st5(uint32_t a, uint64_t v)
{
    for (int i = 0; i < 5; i++, v >>= 8)
        ros_st8(a + (uint32_t)i, (uint32_t)(v & 0xFF));
}

static unsigned events[32];
static int eventv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] < 32)
        events[s->r[0]]++;
    return ROS_VECTOR_PASS;
}

static unsigned bytev_calls;
static int bytev(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] != 250)
        return ROS_VECTOR_PASS;
    bytev_calls++;
    s->r[1] = 0x5A;
    return ROS_VECTOR_CLAIM;
}

/* OS_Reset's R0, as its hook saw it */
static uint32_t reset_r0;
static unsigned resets;

static void reset_seen(uint32_t r0)
{
    reset_r0 = r0;
    resets++;
}

void ros_selftest_osbyte(void)
{
    uint32_t r[3];
    uint32_t block = ros_addr(ros_rma_alloc(32));

    /* ---- OS_Byte 0 ---- */
    r[0] = 0, r[1] = 1, r[2] = 0;
    int v = byte(r);
    uint32_t e0[3] = { 0, 0, 0 };
    int v0 = byte(e0);
    const os_error *fx0 = v0 ? ros_ptr(e0[0]) : NULL;
    check(!v && r[1] == 6 && fx0 && fx0->errnum == 0xF7 &&
              strcmp(fx0->errmess, "RISC OS 5.30 (15 May 2024)") == 0,
          "OS_Byte 0 -- the OS version, 6; with R1 0, the version as an error, &F7, RISC OS "
          "5.30's: \"RISC OS 5.30 (15 May 2024)\" (the Task Manager's Info: 5.30 (15-May-24))",
          "%u; \"%s\"", r[1], fx0 ? fx0->errmess : "no error");

    /* ---- OS_Reset: Service_PreReset, then a restart, or with '&OFF' a
     * power off. Here its hook runs instead ---- */
    reset_r0 = 0xFFFFFFFFu, resets = 0;
    ros_reset_hook = reset_seen;
    struct ros_cpu rs;
    ros_cpu_enter(&rs);
    rs.r[0] = 0;
    ros_swi(&rs, XOS_Reset);
    uint32_t first = reset_r0;
    int v1 = rs.v;
    ros_cpu_enter(&rs);
    rs.r[0] = 0x46464F26u;
    ros_swi(&rs, XOS_Reset);
    ros_reset_hook = NULL;
    check(!v1 && !rs.v && resets == 2 && first == 0 && reset_r0 == 0x46464F26u,
          "OS_Reset -- a SWI the kernel has: a restart (R0 0), a power off (R0 '&OFF')",
          "%u resets, R0 &%X then &%X, V %d %d", resets, first, reset_r0, v1, rs.v);

    /* ---- events: semaphores, and VSync at 50 Hz ---- */
    ros_vector_claim_native(ROS_EVENTV, eventv, 0);
    r[0] = 14, r[1] = 4, r[2] = 0;
    byte(r);
    uint32_t was = r[1];
    /* Counted against the time actually waited (50 Hz, one every 20 ms):
     * never more, as they do not bunch after a busy spell; as few as half
     * on a loaded host, missed ones being lost (ticker.h) */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    memset(events, 0, sizeof events);
    wait_ms(200);
    unsigned vs = events[4];
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r[0] = 13, r[1] = 4;
    byte(r);
    uint32_t disabled_from = r[1];
    unsigned after = events[4];
    wait_ms(60);
    unsigned ms = (unsigned)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    unsigned lo = ms / 40, hi = (ms + 19) / 20 + 1;
    check(was == 0 && vs >= lo && vs <= hi && disabled_from == 1 && events[4] == after,
          "OS_Byte 14 / 13 -- Event 4, VSync, at 50 Hz while enabled, none after",
          "old %u, %u VSyncs in %u ms, disabled from %u", was, vs, ms, disabled_from);
    r[0] = 13, r[1] = 30;
    byte(r);
    uint32_t r13 = r[1];
    r[0] = 14, r[1] = 30;
    byte(r);
    uint32_t r14 = r[1];
    r[0] = 14, r[1] = 40;
    byte(r);
    check(r13 == 0 && r14 == 0xFF && r[1] == 0xFF,
          "OS_Byte 13 on a disabled event wraps it to &FF, which then stays; 32 up is &FF",
          "%X %X %X", r13, r14, r[1]);

    /* ---- OS_Byte 19: the next VSync, even with interrupts off ---- */
    uint32_t n = ros_vsync_count();
    xos_int_off();
    r[0] = 19;
    byte(r);
    xos_int_on();
    check(ros_vsync_count() == n + 1 || ros_vsync_count() == n + 2,
          "OS_Byte 19 -- waits for the next VSync, with interrupts off too", "%u -> %u", n,
          ros_vsync_count());

    /* ---- the interval timer: Event 5 as it passes zero ---- */
    r[0] = 14, r[1] = 5;
    byte(r);
    events[5] = 0;
    st5(block, 0xFFFFFFFFFFull - 4);          /* -5: zero five ticks from now */
    word(4, block);
    wait_ms(150);
    word(3, block);
    uint64_t t = ld5(block);
    r[0] = 13, r[1] = 5;
    byte(r);
    check(events[5] == 1 && t >= 8 && t <= 14,
          "OS_Word 4 / 3 -- the interval timer, Event 5 once as it passes zero", "%u events, %llu",
          events[5], (unsigned long long)t);
    ros_vector_release_native(ROS_EVENTV, eventv, 0);

    /* ---- the system clock ---- */
    st5(block, 1000);
    word(2, block);
    wait_ms(100);
    word(1, block);
    t = ld5(block);
    check(t >= 1008 && t <= 1013, "OS_Word 2 / 1 -- the system clock counts centiseconds",
          "%llu", (unsigned long long)t);

    /* ---- the real-time clock: the host's ---- */
    ros_st8(block, 3);
    word(14, block);
    uint64_t cs = ld5(block);
    long long unix_s = (long long)(cs / 100) - 2208988800LL;
    check(llabs(unix_s - (long long)time(NULL)) <= 2,
          "OS_Word 14, 3 -- five bytes of centiseconds since 1900, the host's time", NULL);
    ros_st8(block, 0);
    word(14, block);
    const char *str = ros_ptr(block);
    check(str[3] == ',' && str[15] == '.' && str[18] == ':' && str[21] == ':' && str[24] == 13,
          "OS_Word 14, 0 -- \"Day,dd Mon yyyy.hh:mm:ss\", CR-terminated", "%.24s", str);
    ros_st8(block, 1);
    word(14, block);
    uint32_t min = ros_ld8(block + 5);
    check((min >> 4) <= 5 && (min & 15) <= 9, "OS_Word 14, 1 -- the time in BCD", NULL);

    /* ---- the vectors, and what no one knows ---- */
    ros_vector_claim_native(0x06, bytev, 0);
    r[0] = 250, r[1] = 0, r[2] = 0;
    v = byte(r);
    ros_vector_release_native(0x06, bytev, 0);
    check(!v && r[1] == 0x5A && bytev_calls == 1, "OS_Byte through ByteV: a claimant answers",
          NULL);
    r[0] = 43, r[1] = 0, r[2] = 0;                      /* (&A8-&FF are variables) */
    v = byte(r);
    check(v && ((os_error *)ros_ptr(r[0]))->errnum == 0xFE,
          "OS_Byte nobody knows -- Service_UKByte, then \"Bad command\", &FE", NULL);
    check(!word(99, block), "OS_Word nobody knows -- Service_UKWord, and no error", NULL);

    /* ---- OS_ReadSysInfo: no hardware to report ---- */
    uint32_t q2[5] = { 2 }, q5[5] = { 5, 7, 7 }, q8[5] = { 8, 7, 7 }, q14[5] = { 14 };
    int v2 = sysinfo(q2), v5 = sysinfo(q5), v8 = sysinfo(q8), v14 = sysinfo(q14);
    check(!v2 && q2[0] == 0xFFFFFF00u && !q2[1] && !q2[2] && !q2[3] && !q2[4] && !v5 && !q5[0] &&
              !q5[1] && !v8 && !q8[0] && !q8[1] && !q8[2] && !v14 && !q14[0],
          "OS_ReadSysInfo 2, 5, 8, 14 -- no chips, no machine ID, an unspecified platform, no IIC",
          NULL);
    ros_st32(block, 3), ros_st32(block + 4, 16), ros_st32(block + 8, 0xFFFFFFFFu);
    ros_st32(block + 16, 0x55), ros_st32(block + 20, 0x55), ros_st32(block + 24, 0x55);
    uint32_t q6[5] = { 6, block, block + 16 }, q6s[5] = { 6, 0, 17 };
    int v6 = sysinfo(q6), v6s = sysinfo(q6s);
    check(!v6 && !ros_ld32(block + 16) && !ros_ld32(block + 20) && ros_ld32(block + 24) == 0x55 &&
              !v6s && q6s[2] == 0,
          "OS_ReadSysInfo 6 -- kernel values, each 0: \"no longer meaningful\"", NULL);
    uint32_t q9[5] = { 9, 0 }, q9d[5] = { 9, 2 }, q9x[5] = { 9, 3 };
    int v9 = sysinfo(q9), v9d = sysinfo(q9d), v9x = sysinfo(q9x);
    const char *name = q9[0] ? ros_ptr(q9[0]) : "", *date = q9d[0] ? ros_ptr(q9d[0]) : "";
    check(!v9 && !strcmp(name, "ROSGD 0.01") && !v9d && strlen(date) == 24 && date[3] == ',' &&
              date[15] == '.' && !v9x && q9x[0] == 0,
          "OS_ReadSysInfo 9 -- the OS name, the build date as OS_Word 14 has it, no dealer",
          "\"%s\" \"%s\"", name, date);
    uint32_t q13[5] = { 13, 0 }, q16[5] = { 16 }, q10[5] = { 10 };
    int v13 = sysinfo(q13), v16 = sysinfo(q16), v10 = sysinfo(q10);
    check(!v13 && q13[0] == 1 && v16 && ((os_error *)ros_ptr(q16[0]))->errnum == 0x1EC && v10,
          "OS_ReadSysInfo 13 -- wide key numbers; 10 and 16 -- \"Unknown OS_ReadSysInfo call\"",
          NULL);
    ros_rma_free(ros_ptr(block));
}
