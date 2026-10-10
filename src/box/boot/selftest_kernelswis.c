/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_kernelswis.c: kernel SWIs that the desktop calls, and that ROSGD
 * answered "not known" or "not implemented": OS_ScreenMode 4-6 and its
 * unknown reasons, OS_DynamicArea 5 and 27 (the free memory) and its
 * unknown reasons, OS_SynchroniseCodeAreas, OS_Memory 8, and
 * OS_PlatformFeatures (which was not known at all, #67). The answers are the
 * box's, not the Pi's (runtime/sysinfo.c). What RISC OS 5.30 does is in
 * tests/kernelswis/expected (the farm). These are the same facts, checked in
 * the box: the registers and the error numbers. Reasons that 5.30's kernel
 * has and ROSGD does not do yet give "not implemented" (&1E7) and not the
 * kernel's "Bad reason code", so a gap shows as one.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cmos.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check

/* R0-R7 in, then out; the error number, or 0 */
static uint32_t call(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static int kept(const uint32_t r[8], unsigned from)
{
    for (unsigned i = from; i < 8; i++)
        if (r[i] != i)
            return 0;
    return 1;
}

/* A reason's error: its number, R1-R3 kept */
static int refused(uint32_t n, uint32_t r0, uint32_t err)
{
    uint32_t r[8] = { r0, 1, 2, 3, 4, 5, 6, 7 };
    return call(n, r) == err && kept(r, 1);
}

/* OS_Hardware: R0-R9 in, then out; the error number, or 0 */
static uint32_t hw(uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, XOS_Hardware);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

/* OS_Hardware (runtime/hardware.c): the HAL entries the box answers, the
 * kernel's errors, and the device table */
static void hardware(void)
{
    uint32_t rate[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 19 };
    uint32_t period[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 20 };
    uint32_t rd[10] = { 0, 0, 0, 0, 4, 5, 6, 7, 0, 21 };
    uint32_t delay[10] = { 2000, 0, 0, 0, 0, 0, 0, 0, 0, 22 };
    uint32_t cpus[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 56 };
    uint32_t name[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 97 };
    uint32_t nvt[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 23 };
    int ok = !hw(rate) && rate[0] == 1000000 && !hw(period) && period[0] == 10000 &&
             !hw(rd) && rd[0] < 10000 && rd[4] == 4 && rd[7] == 7 && rd[8] == 0 && rd[9] == 21 &&
             !hw(delay) && !hw(cpus) && cpus[0] >= 1 && !hw(nvt) && (nvt[0] & 0xFF) == 3 &&
             !hw(name) && !strcmp(ros_ptr(name[0]), ros_platform_name());
    /* NVMemoryRead agrees with OS_Byte 161's CMOS */
    uint8_t *buf = ros_rma_alloc(16);
    uint32_t nvr[10] = { 0x80, ros_addr(buf), 16, 0, 0, 0, 0, 0, 0, 29 };
    ok = ok && !hw(nvr) && nvr[0] == 16 && buf[0] == ros_cmos_read(0x80) && buf[15] == ros_cmos_read(0x8F);
    ros_rma_free(buf);
    uint32_t none[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };            /* IRQEnable */
    uint32_t look[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 19 };
    uint32_t big[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 9999 };
    uint32_t reason[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 6, 0 };
    int bad = hw(none) == 0xC61 && hw(look) == 0xC61 && hw(big) == 0xC61 && hw(reason) == 0xC60;

    /* Devices: two of type &5A5A (none such), versions 1.0 and 2.0; the
     * newest first, the chrono order the other way, version 1 callers see
     * only the first, removed ones gone */
    uint32_t *d = ros_rma_alloc(2 * 64);
    memset(d, 0, 2 * 64);
    uint32_t a = ros_addr(d), b = a + 64;
    d[0] = 0x5A5A, d[2] = 0x10000;
    d[16] = 0x5A5A, d[18] = 0x20000;
    uint32_t add1[10] = { a, 0, 0, 0, 0, 0, 0, 0, 2 }, add2[10] = { b, 0, 0, 0, 0, 0, 0, 0, 2 };
    int dev = !hw(add1) && !hw(add2);
    uint32_t e1[10] = { 0xFFFF5A5Au, 0, 0, 0, 0, 0, 0, 0, 4 };
    dev = dev && !hw(e1) && e1[2] == b && e1[1] != 0xFFFFFFFFu;
    uint32_t e2[10] = { 0xFFFF5A5Au, e1[1], 0, 0, 0, 0, 0, 0, 4 };
    dev = dev && !hw(e2) && e2[2] == a;
    uint32_t e3[10] = { 0xFFFF5A5Au, e2[1], 0, 0, 0, 0, 0, 0, 4 };
    dev = dev && !hw(e3) && e3[1] == 0xFFFFFFFFu;
    uint32_t c1[10] = { 0xFFFF5A5Au, 0, 0, 0, 0, 0, 0, 0, 5 };
    dev = dev && !hw(c1) && c1[2] == a;
    uint32_t v1[10] = { 0x00015A5Au, 0, 0, 0, 0, 0, 0, 0, 4 };
    dev = dev && !hw(v1) && v1[2] == a;
    uint32_t rm1[10] = { a, 0, 0, 0, 0, 0, 0, 0, 3 }, rm2[10] = { b, 0, 0, 0, 0, 0, 0, 0, 3 };
    dev = dev && !hw(rm1) && !hw(rm2);
    uint32_t e4[10] = { 0xFFFF5A5Au, 0, 0, 0, 0, 0, 0, 0, 4 };
    dev = dev && !hw(e4) && e4[1] == 0xFFFFFFFFu;
    ros_rma_free(d);
    check(ok && bad && dev, "OS_Hardware: the counter, NVMemory, CPUs and platform name by "
          "CallHAL; absent entries, LookupRoutine and reason 6 refused as the kernel does; "
          "devices added, enumerated (both orders, by version) and removed",
          "entries %d (rate %u period %u read %u cpus %u nv &%X name %s); errors %d; devices %d",
          ok, rate[0], period[0], rd[0], cpus[0], nvt[0],
          name[0] ? (const char *)ros_ptr(name[0]) : "-", bad, dev);
}

void ros_selftest_kernelswis(void)
{
    hardware();

    /* OS_UpdateMEMC (#120): the soft copy as 5.30 keeps it. It is &400 at the
     * start, the top 12 bits are &036 once written, and bit 10 (video DMA)
     * is back on. */
    {
        uint32_t a[8] = { 0, 0, 2, 3, 4, 5, 6, 7 }, b[8] = { 0x300, 0x700, 2, 3, 4, 5, 6, 7 };
        uint32_t c[8] = { 0, 0, 2, 3, 4, 5, 6, 7 }, d[8] = { 0, 0xFFFFFFFFu, 2, 3, 4, 5, 6, 7 };
        int e = call(XOS_UpdateMEMC, a) || call(XOS_UpdateMEMC, b) || call(XOS_UpdateMEMC, c);
        d[0] = a[0];
        e = e || call(XOS_UpdateMEMC, d);
        uint32_t f[8] = { 0, 0, 2, 3, 4, 5, 6, 7 };
        e = e || call(XOS_UpdateMEMC, f);
        check(!e && (a[0] & 0x000FFFFFu) == 0x400 && b[0] == ((a[0] & 0xFFFFF) | 0x3600000u) &&
              c[0] == 0x3600300u && d[0] == 0x3600300u && f[0] == 0x3600400u && kept(f, 2),
              "OS_UpdateMEMC: the MEMC soft copy as 5.30's, bit 10 set back", "%08X %08X %08X %08X %08X",
              a[0], b[0], c[0], d[0], f[0]);
    }

    /* OS_ScreenMode 4: R1 1, R2 1 whatever is asked; 5 and 6 do nothing */
    uint32_t r[8] = { 4, 0xFFFFFFFFu, 0xFFFFFFFFu, 3, 4, 5, 6, 7 };
    uint32_t e = call(XOS_ScreenMode, r);
    int ok = !e && r[0] == 4 && r[1] == 1 && r[2] == 1 && kept(r, 3);
    uint32_t r5[8] = { 5, 1, 2, 3, 4, 5, 6, 7 }, r6[8] = { 6, 1, 2, 3, 4, 5, 6, 7 };
    ok = ok && !call(XOS_ScreenMode, r5) && r5[0] == 5 && kept(r5, 1);
    ok = ok && !call(XOS_ScreenMode, r6) && r6[0] == 6 && kept(r6, 1);
    static const uint32_t sm_bad[] = { 7, 8, 9, 10, 12, 16, 63, 69, 128, 255 };
    int bad = 1;
    for (unsigned i = 0; i < sizeof sm_bad / sizeof sm_bad[0]; i++)
        bad = bad && refused(XOS_ScreenMode, sm_bad[i], 0x1F2);
    check(ok && bad, "OS_ScreenMode 4, 5, 6; 7-10, 12, 16 up \"Unknown OS_ScreenMode reason code\"",
          "4: error &%X R0-R2 %u %u %u; 5, 6 %s; unknown reasons %s", e, r[0], r[1], r[2],
          ok ? "ok" : "wrong", bad ? "ok" : "wrong");

    /* OS_DynamicArea 5, 27: the free memory in bytes and in pages is the same
     * figure. It is the box's real free memory, which an area's size takes
     * none of until its pages are used (dynarea.c). Also an unknown area and
     * unknown reasons. */
    uint32_t b[8] = { 5, 0xFFFFFFFFu, 0xFFFFFFFFu, 3, 4, 5, 6, 7 };
    uint32_t p[8] = { 27, 0xFFFFFFFFu, 0xFFFFFFFFu, 3, 4, 5, 6, 7 };
    uint32_t eb = call(XOS_DynamicArea, b), ep = call(XOS_DynamicArea, p);
    ok = !eb && !ep && b[0] == 5 && p[0] == 27 && b[1] == 0xFFFFFFFFu && kept(b, 3) && kept(p, 3) &&
         p[2] > 1024 && (b[2] & 0xFFF) == 0 &&
         (b[2] > (p[2] >= 1u << 19 ? 0x7FFFF000u : p[2] << 12)
              ? b[2] - (p[2] >= 1u << 19 ? 0x7FFFF000u : p[2] << 12)
              : (p[2] >= 1u << 19 ? 0x7FFFF000u : p[2] << 12) - b[2]) < (4u << 20);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = 0xFFFFFFFFu, s.r[2] = 64 * 4096, s.r[3] = 0xFFFFFFFFu, s.r[4] = 0;
    s.r[5] = 128 * 4096, s.r[6] = 0, s.r[7] = 0, s.r[8] = 0;
    ros_swi(&s, XOS_DynamicArea);
    int made = !s.v, info = 0;
    uint32_t area = s.r[1], base = s.r[3], less = 0, with_area = 0;
    if (made) {
        uint32_t q[8] = { 27, 0xFFFFFFFFu, 0, 3, 4, 5, 6, 7 }, qa[8] = { 27, area, 0, 3, 4, 5, 6, 7 };
        if (!call(XOS_DynamicArea, q))
            less = p[2] > q[2] ? p[2] - q[2] : q[2] - p[2];
        with_area = !call(XOS_DynamicArea, qa);
        ros_cpu_enter(&s);                  /* 24, PMP_GetInfo: R2-R8 */
        s.r[0] = 24, s.r[1] = area;
        ros_swi(&s, XOS_DynamicArea);
        info = !s.v && s.r[2] == 64 * 4096 && s.r[3] == base && s.r[5] == 128 * 4096 &&
               s.r[6] == 64 && s.r[7] == 128;
        uint32_t rm[8] = { 1, area, 0, 0, 0, 0, 0, 0 };
        call(XOS_DynamicArea, rm);
    }
    uint32_t u[8] = { 5, 0x7FFFFF00u, 0xFFFFFFFFu, 3, 4, 5, 6, 7 };
    int unknown = call(XOS_DynamicArea, u) == 0x105 && u[1] == 0x7FFFFF00u && u[2] == 0xFFFFFFFFu;
    static const uint32_t da_area[] = { 1, 2, 24, 27 };   /* each "Unknown dynamic area" */
    for (unsigned i = 0; i < sizeof da_area / sizeof da_area[0]; i++) {
        uint32_t t[8] = { da_area[i], 0x7FFFFF00u, 0xFFFFFFFFu, 3, 4, 5, 6, 7 };
        unknown = unknown && call(XOS_DynamicArea, t) == 0x105;
    }
    uint32_t rd[8] = { 0x7FFFFF00u, 1, 2, 3, 4, 5, 6, 7 }, cd[8] = { 0x7FFFFF00u, 4096, 2, 3, 4, 5, 6, 7 };
    unknown = unknown && call(XOS_ReadDynamicArea, rd) == 0x105 && call(XOS_ChangeDynamicArea, cd) == 0x1C1;
    static const uint32_t da_bad[] = { 11, 15, 19, 29, 64, 255, 0xFFFFFFFFu };
    bad = 1;
    for (unsigned i = 0; i < sizeof da_bad / sizeof da_bad[0]; i++)
        bad = bad && refused(XOS_DynamicArea, da_bad[i], 0x180);
    static const uint32_t da_gap[] = { 9, 10, 20, 21, 22, 23, 25, 26 };
    int gap = 1;
    for (unsigned i = 0; i < sizeof da_gap / sizeof da_gap[0]; i++)
        gap = gap && refused(XOS_DynamicArea, da_gap[i], 0x1E7);
    check(ok && made && less < 1024 && with_area && info && unknown && bad && gap,
          "OS_DynamicArea 5 and 27, the free memory; 24; \"Unknown dynamic area\" (&105, "
          "OS_ChangeDynamicArea's &1C1); \"Bad reason code\"; 9, 10, 20-23, 25, 26 not implemented",
          "5: error &%X R2 &%X; 27: error &%X R2 %u; an area of 64 pages: made %d, %u moved, "
          "named %d, 24 %d; unknown area %d; unknown reasons %d; not implemented %d", eb, b[2], ep,
          p[2], made, less, with_area, info, unknown, bad, gap);

    /* OS_SynchroniseCodeAreas: no error, every register kept */
    uint32_t y0[8] = { 0, 1, 2, 3, 4, 5, 6, 7 }, y1[8] = { 1, 0x8000, 0x800C, 3, 4, 5, 6, 7 };
    ok = !call(XOS_SynchroniseCodeAreas, y0) && kept(y0, 0) &&
         !call(XOS_SynchroniseCodeAreas, y1) && y1[0] == 1 && y1[1] == 0x8000 && y1[2] == 0x800C &&
         kept(y1, 3);
    check(ok, "OS_SynchroniseCodeAreas: all of memory, a range; no error, registers kept", " ");

    /* OS_Memory 8: pages and the page size, with R0 kept. Also bad types and
     * unknown reasons. */
    uint32_t mm[8] = { 0x108, 0xFFFFFFFFu, 0xFFFFFFFFu, 3, 4, 5, 6, 7 };
    uint32_t vr[8] = { 0x208, 0xFFFFFFFFu, 0xFFFFFFFFu, 3, 4, 5, 6, 7 };
    uint32_t page_size, total;
    xos_read_mem_map_info(&page_size, &total);
    e = call(XOS_Memory, mm);
    ok = !e && mm[0] == 0x108 && mm[1] == total && mm[2] == 4096 && kept(mm, 3) &&
         !call(XOS_Memory, vr) && vr[0] == 0x208 && vr[1] == 0 && vr[2] == 4096;
    static const uint32_t m_type[] = { 8, 0x608, 0x708, 0x1108 };
    int types = 1;
    for (unsigned i = 0; i < sizeof m_type / sizeof m_type[0]; i++) {
        uint32_t t[8] = { m_type[i], 1, 2, 3, 4, 5, 6, 7 };
        types = types && call(XOS_Memory, t) == 0x1EA;
    }
    static const uint32_t m_bad[] = { 1, 2, 5, 10, 11, 25, 63, 66, 255, 0x301 };
    bad = 1;
    for (unsigned i = 0; i < sizeof m_bad / sizeof m_bad[0]; i++)
        bad = bad && refused(XOS_Memory, m_bad[i], 0x180);
    static const uint32_t m_gap[] = { 0, 6, 7, 9, 12, 13, 14, 15, 17, 18, 19, 21, 22, 23, 24,
                                      64, 65, 0x100 };
    gap = 1;
    for (unsigned i = 0; i < sizeof m_gap / sizeof m_gap[0]; i++)
        gap = gap && refused(XOS_Memory, m_gap[i], 0x1E7);
    check(ok && types && bad && gap, "OS_Memory 8 (DRAM, VRAM); bad types \"Parameters not "
          "recognised\"; unknown reasons \"Bad reason code\"; 0, 6, 7, 9, 12-15, 17-19, 21-24, "
          "64, 65 not implemented",
          "8 DRAM: error &%X R0 &%X R1 %u (want %u) R2 %u; VRAM R1 %u; types %d; reasons %d; "
          "not implemented %d", e, mm[0], mm[1], total, mm[2], vr[1], types, bad, gap);

    /* OS_Memory 20, Compatibility: the read-only page that RISC OS can keep at
     * &0 is not here and will not be put here. So asking gives 0, and
     * turning it off gives 0 because it already is. Asking for it gives -1,
     * as 5.30 answers when it cannot. There is no error either way.
     * Anything but -1, 0 or 1 is "Parameters not recognised". Area 16 says
     * the same. */
    uint32_t cp[3][8] = { { 20, 0xFFFFFFFFu }, { 20, 0 }, { 20, 1 } };
    static const uint32_t cp_want[3] = { 0, 0, 0xFFFFFFFFu };
    int compat = 1;
    for (unsigned i = 0; i < 3; i++)
        compat = compat && !call(XOS_Memory, cp[i]) && cp[i][1] == cp_want[i];
    uint32_t cbad[8] = { 20, 7 };
    uint32_t a16[8] = { 16 | (16 << 8) }, a14[8] = { 16 | (14 << 8) };
    compat = compat && call(XOS_Memory, cbad) == 0x1EA &&
             !call(XOS_Memory, a16) && a16[1] == 0 && a16[2] == 0x1000 && a16[3] == 0 &&
             !call(XOS_Memory, a14) && a14[1] == 0x2000 && a14[2] == 0x1000 &&
             a14[3] == (ROS_DEBUGGER_MAPPED ? 0x1000u : 0u);
    check(compat, "OS_Memory 20: the page at &0 is off (0), stays off (0), and will not be put "
          "there (-1), none an error; a bad state refused; area 16 reports it off, and area 14 "
          "DebuggerSpace mapped, as 5.30 reports its own",
          "compat %d", compat);

    /* OS_PlatformFeatures (#67): reason 0 gives the code features: a 32-bit
     * OS, no 26-bit mode, no SWP or LDREX, no physical pages, unknown
     * reasons fixed, and no interrupt-delay routine. Reason 32 gives no
     * processor vectors. Reason 34 gives every ARM instruction group known
     * absent. Reason 35 gives no exclusive-monitor routine. Reasons 1, 36
     * and 99 give the kernel's "Unknown ... reason code", &1F0. */
    uint32_t f0[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    e = call(XOS_PlatformFeatures, f0);
    const uint32_t want_set = 0x80000000u | 0x400000u | 0x800u | 0x80u | 0x40u;
    ok = !e && (f0[0] & want_set) == want_set && !(f0[0] & 0x3000u) && f0[1] == 0;
    uint32_t f32[8] = { 32, 1, 2, 3, 4, 5, 6, 7 };
    ok = ok && !call(XOS_PlatformFeatures, f32) && f32[0] == 0 && f32[1] == 0;
    uint32_t f33[8] = { 33, 9, 2, 3, 4, 5, 6, 7 };      /* a level no CPU has */
    ok = ok && !call(XOS_PlatformFeatures, f33) && f33[0] == 0 && f33[1] == 0 && f33[4] == 0;
    uint32_t f34a[8] = { 34, 6, 2, 3, 4, 5, 6, 7 }, f34b[8] = { 34, 999, 2, 3, 4, 5, 6, 7 };
    uint32_t f34p[8] = { 34, 0xFFFFFFFFu, 2, 3, 4, 5, 6, 7 };
    ok = ok && !call(XOS_PlatformFeatures, f34a) && f34a[0] == 0 &&
         !call(XOS_PlatformFeatures, f34b) && f34b[0] == 0xFFFFFFFFu &&
         !call(XOS_PlatformFeatures, f34p) && f34p[0] == 0 && f34p[3] == 0 &&
         f34p[4] == 0xFFFFFFFFu && f34p[5] == 0xFFFFFFFFu && f34p[6] == 1 && f34p[7] == 0;
    uint32_t f35[8] = { 35, 1, 2, 3, 4, 5, 6, 7 };
    ok = ok && !call(XOS_PlatformFeatures, f35) && f35[1] == 0;
    bad = refused(XOS_PlatformFeatures, 1, 0x1F0) && refused(XOS_PlatformFeatures, 36, 0x1F0) &&
          refused(XOS_PlatformFeatures, 99, 0x1F0);
    uint32_t c0[8] = { 33, 0, 2, 3, 4, 5, 6, 7 };
    call(XOS_PlatformFeatures, c0);
    check(ok && bad, "OS_PlatformFeatures 0, 32-35 answered for the box; 1, 36, 99 \"Unknown "
          "OS_PlatformFeatures reason code\" (#67)",
          "0: error &%X flags &%X R1 &%X; unknown reasons %d; L1 cache type %u, D %u/%u, I %u/%u",
          e, f0[0], f0[1], bad, c0[0], c0[1], c0[2], c0[3], c0[4]);
}
