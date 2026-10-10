/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_amb.c: OS_AMBControl (runtime/amb.c), the slots the Wimp gives
 * its tasks.  Task 0 has no slot of its own, so the nodes here are mapped
 * at &8000 and taken away again, and the words they move are put back. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check
#define APLWORK (ROS_ZEROPAGE + 0x368u)
#define MEMLIMIT (ROS_ZEROPAGE + 0x11Cu)

static int amb(uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, XOS_AMBControl);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

void ros_selftest_amb(void)
{
    uint32_t apl = ros_ld32(APLWORK), lim = ros_ld32(MEMLIMIT);

    uint32_t r[8] = { 9 };
    amb(r);
    check(r[2] == 0 && r[3] == 0, "OS_AMBControl 9 -- none mapped, no nodes", "R2 %u R3 %u", r[2], r[3]);

    /* 0: a node, mapped in at once; the next handle off the list */
    uint32_t a[8] = { 0, 4 };
    int va = amb(a);
    ros_st32(0x8000, 0xA1A1A1A1u);
    ros_st32(0x8000 + 4 * 4096 - 4, 0xA2A2A2A2u);
    uint32_t i1[8] = { 4, 0, a[2] };
    amb(i1);
    check(!va && a[2] == 1 && a[1] == 4 && ros_ld32(APLWORK) == 0xC000 && ros_ld32(MEMLIMIT) == 0xC000 &&
              i1[1] == 0x8000 && i1[3] == 4,
          "OS_AMBControl 0, 4 -- a node of 4 pages, handle 1, mapped at &8000; AplWorkSize, MemLimit &C000",
          "V%d R1 %u R2 %u top &%X read &%X %u", va, a[1], a[2], ros_ld32(APLWORK), i1[1], i1[3]);

    uint32_t b[8] = { 0, 2 };
    amb(b);
    int fresh = ros_ld32(0x8000) == 0;
    ros_st32(0x8000, 0xB1B1B1B1u);
    uint32_t i2[8] = { 4, 0, a[2] };
    amb(i2);
    check(b[2] == 2 && fresh && ros_ld32(APLWORK) == 0xA000 && i2[1] == 0xFFFFFFFFu,
          "OS_AMBControl 0 -- a second node, handle 2, replaces the first at &8000, which reads as mapped out",
          "R2 %u fresh %d top &%X", b[2], fresh, ros_ld32(APLWORK));

    /* 3: whole slots in and out, their contents kept */
    uint32_t m[8] = { 3, 0, a[2] };
    amb(m);
    int back = ros_ld32(0x8000) == 0xA1A1A1A1u && ros_ld32(0x8000 + 4 * 4096 - 4) == 0xA2A2A2A2u;
    uint32_t q[8] = { 9 };
    amb(q);
    check(back && ros_ld32(APLWORK) == 0xC000 && q[2] == 1 && q[3] == 2,
          "OS_AMBControl 3 -- the first mapped back, its words still there; 9 reads handle 1 of 2",
          "back %d top &%X R2 %u R3 %u", back, ros_ld32(APLWORK), q[2], q[3]);

    /* 3 with mapsome: some of the second, higher up, beside the first */
    uint32_t ms[8] = { 0x103, 0x40000, b[2], 0, 1 };
    int vs = amb(ms);
    int seen = ros_ld32(0x40000) == 0xB1B1B1B1u;
    uint32_t over[8] = { 0x103, 0x40000, b[2], 1, 2 };
    int vo = amb(over);
    uint32_t msout[8] = { 0x103, 0xFFFFFFFFu, b[2], 0, 1 };
    amb(msout);
    check(!vs && seen && vo && ros_ld32(0x8000) == 0xA1A1A1A1u,
          "OS_AMBControl &103 -- a page of the second mapped at &40000 and out again; past its end, V",
          "V%d seen %d over V%d", vs, seen, vo);

    /* 2: grow and shrink */
    uint32_t g[8] = { 2, 8, a[2] };
    amb(g);
    ros_st32(0x8000 + 8 * 4096 - 4, 0xA3A3A3A3u);
    check(g[1] == 8 && g[3] == 4 && ros_ld32(APLWORK) == 0x10000 && ros_ld32(0x8000) == 0xA1A1A1A1u,
          "OS_AMBControl 2 -- the mapped node grows from 4 to 8 pages in place; AplWorkSize &10000",
          "R1 %u R3 %u top &%X", g[1], g[3], ros_ld32(APLWORK));
    uint32_t z[8] = { 2, 0, b[2] };
    amb(z);
    uint32_t q2[8] = { 9 };
    amb(q2);
    check(z[1] == 0 && z[2] == 0 && z[3] == 2 && q2[3] == 1,
          "OS_AMBControl 2, 0 -- shrunk to nothing, a node is freed: R2 0, one node left",
          "R1 %u R2 %u R3 %u nodes %u", z[1], z[2], z[3], q2[3]);

    /* 1, and the handles coming back last freed first */
    uint32_t d[8] = { 1, 0, a[2] };
    int vd = amb(d);
    uint32_t q3[8] = { 9 };
    amb(q3);
    check(!vd && q3[2] == 0 && q3[3] == 0 && ros_ld32(APLWORK) == 0x8000 && ros_ld32(MEMLIMIT) == 0x8000,
          "OS_AMBControl 1 -- the mapped node freed: none mapped, AplWorkSize and MemLimit &8000",
          "V%d R2 %u R3 %u top &%X", vd, q3[2], q3[3], ros_ld32(APLWORK));
    uint32_t n1[8] = { 0, 1 }, n2[8] = { 0, 1 };
    amb(n1);
    amb(n2);
    uint32_t f1[8] = { 1, 0, n1[2] }, f2[8] = { 1, 0, n2[2] };
    amb(f2);
    amb(f1);
    check(n1[2] == 1 && n2[2] == 2, "OS_AMBControl 0 -- handles come back last freed first: 1, then 2",
          "%u %u", n1[2], n2[2]);

    /* errors */
    uint32_t e1[8] = { 1, 0, 99 }, e2[8] = { 7 }, e3[8] = { 200 };
    int v1 = amb(e1), v2 = amb(e2), v3 = amb(e3);
    check(v1 && ros_ld32(e1[0]) == 0x1F8 && v2 && ros_ld32(e2[0]) == 0x1F9 && v3 && ros_ld32(e3[0]) == 0x1F9 &&
              !strcmp(ros_ptr(e2[0] + 4), "reserved AMBControl reason code"),
          "OS_AMBControl -- a bad handle &1F8; reasons 6-8 reserved, past 9 bad, &1F9",
          "V%d %d %d", v1, v2, v3);

    ros_st32(APLWORK, apl);
    ros_st32(MEMLIMIT, lim);
}
