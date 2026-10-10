/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_dynarea.c: the free pool and dynamic areas (runtime/dynarea.c):
 * area 6 read every way, an area made and changed against it, application
 * space grown from it and given back (a slot of task 0's own, and a Wimp
 * node), the change signature, the kernel's errors, OS_Memory 8, the
 * system areas 0-5 and OS_Memory 16.  What is made is removed, and zero
 * page's words are put back. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/dynarea.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check
#define APLWORK (ROS_ZEROPAGE + 0x368u)
#define MEMLIMIT (ROS_ZEROPAGE + 0x11Cu)

static int call(uint32_t swi, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, swi);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v;
}

static uint32_t freepool(void)
{
    uint32_t r[10] = { 6 };
    call(XOS_ReadDynamicArea, r);
    return r[1];
}

/* The free pool is the box's real free memory (dynarea.c).  Sizes take
 * none of it, and it moves with whatever else the box does.  So a figure
 * read twice agrees within a margin of 4MB, and not to the byte. */
static int near(uint32_t a, uint32_t b)
{
    return (a > b ? a - b : b - a) < (4u << 20);
}

static uint32_t errnum(const uint32_t r[10]) { return ros_ld32(r[0]); }
static const char *errtext(const uint32_t r[10]) { return ros_ptr(r[0] + 4); }

void ros_selftest_dynarea(void)
{
    uint32_t apl = ros_ld32(APLWORK), lim = ros_ld32(MEMLIMIT);
    uint32_t total = (uint32_t)ros_mem_total();

    /* area 6 */
    uint32_t r6[10] = { 6, 0, 0x12345678u }, r86[10] = { 0x86 };
    int v6 = call(XOS_ReadDynamicArea, r6), v86 = call(XOS_ReadDynamicArea, r86);
    check(!v6 && !v86 && r6[0] == 0 && near(r6[1], ros_freepool_bytes()) && r6[1] && !(r6[1] & 0xFFF) &&
              r6[2] == 0x12345678u && r86[2] == total,
          "OS_ReadDynamicArea 6 -- the free pool: base 0, its size in pages; R2 kept; with &80 the "
          "machine's memory as its maximum", "V%d base &%X size &%X R2 &%X max &%X of &%X", v6,
          r6[0], r6[1], r6[2], r86[2], total);
    uint32_t i6[10] = { 2, 6 };
    int vi = call(XOS_DynamicArea, i6);
    check(!vi && i6[2] == r6[1] && i6[3] == 0 && i6[4] == 0x100032u && i6[5] == total && i6[6] == 0 &&
              !strcmp(ros_ptr(i6[8]), "Free pool"),
          "OS_DynamicArea 2, 6 -- flags &100032 (a physical pool), max the memory, \"Free pool\"",
          "V%d size &%X flags &%X max &%X", vi, i6[2], i6[4], i6[5]);
    uint32_t f5[10] = { 5, 0xFFFFFFFFu }, f27[10] = { 27, 0xFFFFFFFFu };
    call(XOS_DynamicArea, f5);
    call(XOS_DynamicArea, f27);
    check(near(f5[2], r6[1]) && near(f27[2] * 4096, f5[2]),
          "OS_DynamicArea 5 and 27 -- the free pool and what shrinkable areas could give, bytes and "
          "pages", "5: &%X 27: &%X free &%X", f5[2], f27[2], r6[1]);
    uint32_t a1[10] = { 0xFFFFFFFFu };
    call(XOS_ReadDynamicArea, a1);
    check(a1[0] == 0x8000 && a1[1] == ros_ld32(APLWORK) - 0x8000 && a1[2] == 0x5FFF8000u,
          "OS_ReadDynamicArea -1 -- application space: &8000, AplWorkSize-&8000, max &5FFF8000 (1.5 GB)",
          "&%X &%X &%X", a1[0], a1[1], a1[2]);

    /* an area made, changed, listed, removed */
    uint32_t sig[10] = { 6 };
    call(XOS_DynamicArea, sig);
    uint32_t before = freepool();
    uint32_t mk[10] = { 0, 0xFFFFFFFFu, 16384 + 1, 0xFFFFFFFFu, 0x80, 0x100000, 0, 0xFFFFFFFFu, 0 };
    int vmk = call(XOS_DynamicArea, mk);
    uint32_t n = mk[1], base = mk[3];
    uint32_t made = freepool();
    uint32_t od1[10] = { 6 };
    call(XOS_DynamicArea, od1);
    uint32_t info[10] = { 2, n };
    call(XOS_DynamicArea, info);
    char hex[9];
    for (int i = 0; i < 8; i++)
        hex[i] = "0123456789ABCDEF"[(n >> (28 - 4 * i)) & 15];
    hex[8] = 0;
    check(!vmk && n >= 0x100 && near(before, made) && od1[1] == n && (od1[2] & 15) == 1 &&
              info[2] == 20480 && info[3] == base && info[4] == 0x80 && info[5] == 0x100000 &&
              info[7] == base && !strcmp(ros_ptr(info[8]), hex),
          "OS_DynamicArea 0 -- 16K+1 made as 5 pages, none of the free pool's until used; 6 says "
          "created; no name, its number in hex; R7 -1, the base", "V%d n &%X fell %d sig %u,&%X size %u ws &%X '%s'",
          vmk, n, (int)(before - made), od1[1], od1[2], info[2], info[7], (char *)ros_ptr(info[8]));

    uint32_t g1[10] = { n, 1 };
    int vg1 = call(XOS_ChangeDynamicArea, g1);
    uint32_t grown = freepool();
    uint32_t od2[10] = { 6 };
    call(XOS_DynamicArea, od2);
    uint32_t gb[10] = { n, 0x200000 };
    int vgb = call(XOS_ChangeDynamicArea, gb);
    uint32_t sb[10] = { n, (uint32_t)-0x100000 };
    int vsb = call(XOS_ChangeDynamicArea, sb);
    uint32_t shrunk = freepool();
    check(!vg1 && g1[0] == n && g1[1] == 4096 && near(made, grown) && od2[1] == n && (od2[2] & 15) == 4 &&
              vgb && errnum(gb) == 0x1C1 && gb[1] == 0 &&
              vsb && errnum(sb) == 0x1C1 && sb[1] == 24576 && near(shrunk, made) &&
              !strcmp(errtext(sb), "Memory cannot be moved"),
          "OS_ChangeDynamicArea -- by 1 a page, from the free pool; past the maximum none, &1C1; a "
          "shrink past its size what there is, &1C1", "R1 %u fell %d; big V%d R1 %u; shrink V%d R1 %u",
          g1[1], (int)(made - grown), vgb, gb[1], vsb, sb[1]);

    uint32_t e28[10] = { 28, 0xFFFFFFFFu, 0 }, found28 = 0, pages28 = 0;
    for (int guard = 0; guard < 600; guard++) {
        e28[0] = 28, e28[2] = 0;                /* R2 flags: none */
        if (call(XOS_DynamicArea, e28))
            break;
        if (e28[1] == 0xFFFFFFFFu || e28[1] <= 6)
            break;
        if (e28[1] == n)
            found28 = 1, pages28 = e28[5];
    }
    uint32_t e3[10] = { 3, 0xFFFFFFFFu }, seen6 = 0, seen3 = 0;
    for (int guard = 0; guard < 600 && !call(XOS_DynamicArea, e3) && e3[1] != 0xFFFFFFFFu; guard++)
        seen6 |= e3[1] == 6, seen3 |= e3[1] == 3;
    check(found28 && pages28 == 256 && e28[1] == 0xFFFFFFFFu && seen6 && seen3,
          "OS_DynamicArea 28 and 3 -- 28 finds it (max 256 pages) and no system area; 3 finds areas 3 "
          "and 6 too", "found %u max %u last &%X; 3: %u %u", found28, pages28, e28[1], seen6, seen3);

    uint32_t rm[10] = { 1, n }, rm2[10] = { 1, n };
    int vrm = call(XOS_DynamicArea, rm), vrm2 = call(XOS_DynamicArea, rm2);
    uint32_t od3[10] = { 6 };
    call(XOS_DynamicArea, od3);
    check(!vrm && vrm2 && errnum(rm2) == 0x105 && od3[1] == 0xFFFFFFFFu && (od3[2] & 15) == 6 &&
              near(freepool(), shrunk),
          "OS_DynamicArea 1 -- removed, the free pool as before; again &105; 6 says several changes, "
          "resized and removed", "V%d again V%d &%X; 6: &%X &%X", vrm, vrm2, errnum(rm2), od3[1], od3[2]);

    /* application space: a slot of task 0's own, then a Wimp node */
    before = freepool();
    uint32_t none[10] = { 6, 4096 };
    int vnone = call(XOS_ChangeDynamicArea, none);
    uint32_t sl[10] = { 6, (uint32_t)-8192 };
    int vsl = call(XOS_ChangeDynamicArea, sl);
    uint32_t top = ros_ld32(APLWORK), lm = ros_ld32(MEMLIMIT), fell = before - freepool();
    ros_st32(0x8000, 0xC1C1C1C1u);
    ros_st32(0x9FFC, 0xC2C2C2C2u);
    uint32_t back[10] = { 6, 8192 };
    int vback = call(XOS_ChangeDynamicArea, back);
    check(vnone && errnum(none) == 0x1C1 && none[1] == 0 && !vsl && sl[1] == 8192 && top == 0xA000 &&
              lm == 0xA000 && near(fell, 0) && !vback && back[1] == 8192 &&
              ros_ld32(APLWORK) == 0x8000 && near(freepool(), before),
          "OS_ChangeDynamicArea 6 -- no application space to take: &1C1; by -8K a slot of task 0's "
          "own, AplWorkSize and MemLimit &A000; by +8K it goes back",
          "none V%d R1 %u; -8K V%d R1 %u top &%X lim &%X fell %d; +8K V%d R1 %u", vnone, none[1], vsl,
          sl[1], top, lm, (int)fell, vback, back[1]);

    before = freepool();
    uint32_t al[10] = { 0, 4 };
    call(XOS_AMBControl, al);
    uint32_t nodefell = before - freepool();
    uint32_t gn[10] = { 6, (uint32_t)-4096 };
    int vgn = call(XOS_ChangeDynamicArea, gn);
    uint32_t rd[10] = { 4, 0, al[2] };
    call(XOS_AMBControl, rd);
    uint32_t slot[10] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
    call(XWimp_SlotSize, slot);
    uint32_t f5b[10] = { 5, 0xFFFFFFFFu, 0, 0 };
    call(XOS_DynamicArea, f5b);
    uint32_t sn[10] = { 6, 12288 };
    int vsn = call(XOS_ChangeDynamicArea, sn);
    uint32_t rd2[10] = { 4, 0, al[2] };
    call(XOS_AMBControl, rd2);
    uint32_t fr[10] = { 1, 0, al[2] };
    call(XOS_AMBControl, fr);
    check(near(nodefell, 0) && !vgn && gn[1] == 4096 && rd[3] == 5 && ros_ld32(APLWORK) == 0x8000 &&
              near(slot[2], f5b[2]) && !vsn && sn[1] == 12288 && rd2[3] == 2 && near(freepool(), before),
          "OS_ChangeDynamicArea 6 with a Wimp node mapped -- the node grows and shrinks, lazily, the "
          "free pool the box's real free memory; Wimp_SlotSize's R2 is OS_DynamicArea 5's",
          "fell %d; -4K V%d R1 %u pages %u; slot R2 &%X next &%X da5 &%X; +12K V%d R1 %u pages %u", (int)nodefell,
          vgn, gn[1], rd[3], slot[2], slot[1], f5b[2], vsn, sn[1], rd2[3]);

    /* A node's pages being moved is reported as the kernel's PMP handler
     * reports it: area 6's resize bit is set, with handle -1
     * (OS_DynamicArea 6). */
    uint32_t sg0[10] = { 6 };
    call(XOS_DynamicArea, sg0);                 /* read and cleared */
    uint32_t pa[10] = { 0, 1 };
    int vpa = call(XOS_AMBControl, pa);
    uint32_t pg[10] = { 2, 3, pa[2] }, sg1[10] = { 6 };
    int vpg = !vpa && pa[2] ? call(XOS_AMBControl, pg) : 1;
    call(XOS_DynamicArea, sg1);
    if (!vpa && pa[2]) {
        uint32_t pd[10] = { 1, 0, pa[2] };
        call(XOS_AMBControl, pd);
    }
    check(!vpa && pa[2] && !vpg && pg[1] == 3 && (sg1[2] & 4) && sg1[1] == 0xFFFFFFFFu,
          "OS_AMBControl 2 -- a node's pages moved: area 6's resize bit, handle -1 (PMPMemoryMoved)",
          "alloc V%d h%u; grow V%d R1 %u; signature R1 &%X R2 &%X", vpa, pa[2], vpg, pg[1],
          sg1[1], sg1[2]);

    /* OS_DynamicArea 8's clamps, as 5.30 applies them to a new area.  A
     * number in the quick handles' range gives "already exists". */
    uint32_t cl[10] = { 8, 0x200000u, 0x300000u, 0x80000u };
    call(XOS_DynamicArea, cl);                  /* R3 under 1M: ignored */
    uint32_t cl2[10] = { 8, 0, 0, 0 };
    call(XOS_DynamicArea, cl2);                 /* reads them back */
    uint32_t mk1[10] = { 0, 0xFFFFFFFFu, 4096, 0xFFFFFFFFu, 0x80, 0xFFFFFFFFu, 0, 0, 0 };
    uint32_t mk2[10] = { 0, 0xFFFFFFFFu, 4096, 0xFFFFFFFFu, 0x80, 0x1000000u, 0, 0, 0 };
    int vk1 = call(XOS_DynamicArea, mk1), vk2 = call(XOS_DynamicArea, mk2);
    uint32_t rk1[10] = { 2, mk1[1] }, rk2[10] = { 2, mk2[1] };
    if (!vk1)
        call(XOS_DynamicArea, rk1);
    if (!vk2)
        call(XOS_DynamicArea, rk2);
    uint32_t qh[10] = { 0, 0x150, 4096, 0xFFFFFFFFu, 0x80, 0x100000u, 0, 0, 0 };
    int vqh = call(XOS_DynamicArea, qh);
    for (int k = 0; k < 2; k++) {
        uint32_t rm[10] = { 1, k ? mk2[1] : mk1[1] };
        if (!(k ? vk2 : vk1))
            call(XOS_DynamicArea, rm);
    }
    uint32_t unclamp[10] = { 8, 0x08000000u, 0x08000000u, 0x08000000u };
    call(XOS_DynamicArea, unclamp);
    check(cl2[1] == 0x200000u && cl2[2] == 0x300000u && cl2[3] == 0x08000000u && !vk1 &&
              rk1[5] == 0x200000u && !vk2 && rk2[5] == 0x300000u && vqh && errnum(qh) == 0x1C4,
          "OS_DynamicArea 8's clamps -- R5 -1 by the first, a size by the second, under 1M "
          "ignored; a quick-handle number &1C4",
          "clamps &%X &%X &%X; maxima &%X &%X; &150 V%d &%X", cl2[1], cl2[2], cl2[3], rk1[5],
          rk2[5], vqh, vqh ? errnum(qh) : 0);

    /* errors, and OS_Memory 8 */
    uint32_t b29[10] = { 29 }, r99[10] = { 99 }, c99[10] = { 99, 4096 }, c2[10] = { 2, 0x80000000u };
    int vb29 = call(XOS_DynamicArea, b29), vr99 = call(XOS_ReadDynamicArea, r99);
    int vc99 = call(XOS_ChangeDynamicArea, c99), vc2 = call(XOS_ChangeDynamicArea, c2);
    check(vb29 && errnum(b29) == 0x180 && !strcmp(errtext(b29), "Bad reason code") &&
              vr99 && errnum(r99) == 0x105 && !strcmp(errtext(r99), "Unknown dynamic area") &&
              vc99 && errnum(c99) == 0x1C1 && c99[1] == 0 && vc2 && errnum(c2) == 0x1C1 && c2[1] == 0,
          "the kernel's errors -- OS_DynamicArea 29 &180, area 99 &105, changing it or the screen "
          "&1C1 with R1 0", "&%X &%X &%X &%X", errnum(b29), errnum(r99), errnum(c99), errnum(c2));
    uint32_t m1[10] = { 8 + (1 << 8) }, m6[10] = { 8 + (6 << 8) }, mb[10] = { 1 };
    int vm1 = call(XOS_Memory, m1), vm6 = call(XOS_Memory, m6), vmb = call(XOS_Memory, mb);
    check(!vm1 && m1[1] * m1[2] == total && m1[2] == 4096 && vm6 && errnum(m6) == 0x1EA &&
              vmb && errnum(mb) == 0x180,
          "OS_Memory 8 -- DRAM the machine's pages; kind 6 &1EA; reason 1 &180",
          "V%d %u x %u; &%X &%X", vm1, m1[1], m1[2], errnum(m6), errnum(mb));

    /* the system areas, 0-5, and OS_Memory 16 */
    static const char *const titles[6] = { "System heap", "Module area", "Screen memory",
                                           "System sprites", "Font cache", "RAM disc" };
    static const uint32_t flags[6] = { 0, 0, 0x120u, 0, 2, 0x100122u };
    int sys_ok = 1;
    uint32_t bad = 0;
    for (uint32_t n = 0; n < 6; n++) {
        uint32_t d2[10] = { 2, n }, rd[10] = { n | 0x80u }, d24[10] = { 24, n };
        int v2 = call(XOS_DynamicArea, d2), vr = call(XOS_ReadDynamicArea, rd);
        int v24 = call(XOS_DynamicArea, d24);
        if (v2 || vr || v24 || strcmp(ros_ptr(d2[8]), titles[n]) || d2[4] != flags[n] ||
            rd[0] != d2[3] || rd[1] != d2[2] || rd[2] != d2[5] || (d2[2] & 0xFFF) ||
            d24[6] != d2[2] / 4096 || d24[7] != d2[5] / 4096 || d24[8] != d2[8] ||
            (n != 2 && d2[7] != d2[3]))
            sys_ok = 0, bad = n;
    }
    uint32_t sh[10] = { 0x80 }, rma[10] = { 0x81 }, sc[10] = { 0x82 };
    call(XOS_ReadDynamicArea, sh);
    call(XOS_ReadDynamicArea, rma);
    call(XOS_ReadDynamicArea, sc);
    uint32_t vv = ros_addr(ros_rma_alloc(8));      /* TotalScreenSize */
    ros_st32(vv, 150), ros_st32(vv + 4, 0xFFFFFFFFu);
    uint32_t rv[10] = { vv, vv };
    call(XOS_ReadVduVariables, rv);
    uint32_t tss = ros_ld32(vv);
    ros_rma_free(ros_ptr(vv));
    check(sys_ok && sh[0] == ROS_SYSHEAP_BASE && ros_ld32(sh[0]) == 0x70616548u &&
              sh[1] == ros_ld32(sh[0] + 12) && sh[2] == ROS_SYSHEAP_SIZE && rma[0] == ROS_RMA_BASE &&
              rma[1] >= 4096 && rma[2] == ROS_RMA_SIZE && sc[0] == ROS_SCREEN_BASE &&
              sc[1] >= ((tss + 4095) & ~4095u) && sc[2] == ROS_SCREEN_SIZE,
          "the system areas 0-5 -- titles and flags as 5.30's; OS_ReadDynamicArea, OS_DynamicArea "
          "2 and 24 agree; the system heap a heap its size long, the RMA and screen where ROSGD has them",
          "bad area %u; heap &%X &%X &%X; RMA &%X &%X &%X; screen &%X &%X &%X (mode &%X)", bad,
          sh[0], sh[1], sh[2], rma[0], rma[1], rma[2], sc[0], sc[1], sc[2], tss);
    /* The RMA's free space is inside it, as the kernel's heap ends where the
     * area does.  OS_Module 5's free and largest figures lie within
     * OS_ReadDynamicArea 1's size.  (That is the Task Manager's "Free in
     * Module area", which is 24K on the farm and not the 256 MB the area
     * may grow into.)  A claim bigger than what is free grows the RMA by
     * pages and succeeds. */
    uint32_t d5[10] = { 5 };
    int v5 = call(XOS_Module, d5);
    uint32_t r1[10] = { 1 };
    call(XOS_ReadDynamicArea, r1);
    uint32_t big = d5[3] + 64 * 1024;
    uint32_t claim[10] = { 6, 0, 0, big };
    int vcl = call(XOS_Module, claim);
    uint32_t r2[10] = { 1 };
    call(XOS_ReadDynamicArea, r2);
    uint32_t release[10] = { 7, 0, claim[2] };
    int vfr = vcl ? 1 : call(XOS_Module, release);
    check(!v5 && d5[3] < r1[1] && d5[2] <= d5[3] && !vcl && claim[2] >= ROS_RMA_BASE &&
              claim[2] + big <= ROS_RMA_BASE + r2[1] && r2[1] > r1[1] && !(r2[1] & 0xFFF) && !vfr,
          "the RMA: OS_Module 5's free space and largest block inside the area's size, as the "
          "kernel's; a claim past them grows it, in pages, and is had",
          "free %u largest %u of %u; claimed %u at &%X (V%d), the area then %u; freed V%d", d5[3],
          d5[2], r1[1], big, claim[2], vcl, r2[1], vfr);

    uint32_t ma[10] = { 16 + (0xF << 8) }, mz[10] = { 16 + (0xC << 8) }, ms[10] = { 16 + (3 << 8) };
    uint32_t m0[10] = { 16 }, m18[10] = { 16 + (18 << 8) };
    int vma = call(XOS_Memory, ma), vmz = call(XOS_Memory, mz), vms = call(XOS_Memory, ms);
    int vm0 = call(XOS_Memory, m0), vm18 = call(XOS_Memory, m18);
    check(!vma && ma[0] == 0xF10u && ma[1] == 0x4000u && ma[2] == 0x4000u && ma[3] == 0x4000u &&
              !vmz && mz[1] == ROS_ZEROPAGE && mz[3] == mz[2] && !vms &&
              ms[1] == ROS_SVCSTACK_BASE && ms[2] == ROS_SVCSTACK_SIZE && vm0 &&
              errnum(m0) == 0x1EA && vm18 && errnum(m18) == 0x1EA,
          "OS_Memory 16 -- ScratchSpace &4000 16K, zero page &FFFF0000, the SVC stack ROSGD's; "
          "area 0 and 18 &1EA", "V%d &%X &%X &%X; zp &%X; svc &%X &%X; &%X &%X", vma, ma[1], ma[2],
          ma[3], mz[1], ms[1], ms[2], vm0 ? errnum(m0) : 0, vm18 ? errnum(m18) : 0);

    ros_st32(APLWORK, apl);
    ros_st32(MEMLIMIT, lim);
}
