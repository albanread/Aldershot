/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_modules.c: OS_Heap and OS_Module, against the kernel's own
 * (Kernel/s/HeapMan, ModHand). They are used through the SWIs, and the
 * heap's bytes are read as the kernel lays them out.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

static uint32_t text;               /* arena: names and commands */

static int swi7(uint32_t n, uint32_t r[7])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 7 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 7 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[7])
{
    return ((os_error *)ros_ptr(r[0]))->errnum;
}

static int heap(uint32_t reason, uint32_t hpd, uint32_t r2, uint32_t r3, uint32_t out[7])
{
    uint32_t r[7] = { reason, hpd, r2, r3, 0, 0, 0 };
    int v = swi7(XOS_Heap, r);
    memcpy(out, r, sizeof r);
    return v;
}

static int module(uint32_t reason, const char *name, uint32_t r2, uint32_t r3, uint32_t out[7])
{
    if (name)
        strcpy(ros_ptr(text), name);
    uint32_t r[7] = { reason, name ? text : 0, r2, r3, 0, 0, 0 };
    int v = swi7(XOS_Module, r);
    memcpy(out, r, sizeof r);
    return v;
}

static uint32_t cli(const char *cmd)
{
    strcpy(ros_ptr(text), cmd);
    uint32_t r[7] = { text, 0, 0, 0, 0, 0, 0 };
    return swi7(XOS_CLI, r) ? errnum(r) : 0;
}

/* A native module to watch a module's life: its incarnations, and the
 * services it is given. */
/* What a command prints (WrchV), for *Help */
static char printed[512];
static unsigned nprinted;
static int capture(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (nprinted < sizeof printed - 1)
        printed[nprinted++] = (char)s->r[0];
    return ROS_VECTOR_CLAIM;
}

static unsigned inits, finals, post_inits, post_finals, services;
static os_error *life_init(struct ros_module *m, const char *tail)
{
    (void)tail;
    inits++;
    uint32_t *ws = ros_rma_alloc(8);
    ros_st32(m->private_word, ros_addr(ws));    /* freed by the kernel at death */
    return NULL;
}

static os_error *life_final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    finals++;
    return NULL;
}

static void life_service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] == 0x12345)
        services++;
    if (s->r[1] == 0xDA && s->r[2] && strcmp(ros_ptr(s->r[2]), "T0Demo") == 0)
        post_inits++;
    if (s->r[1] == 0xDB && s->r[2] && strcmp(ros_ptr(s->r[2]), "T0Demo") == 0)
        post_finals++;
}

static struct ros_module life = {
    .title = "Lifecycle",
    .help = "Lifecycle\t1.23 (25 Sep 2026)",
    .init = life_init,
    .final = life_final,
    .service = life_service,
};

void ros_selftest_modules(void)
{
    uint32_t r[7];
    text = ros_addr(ros_rma_alloc(256));
    uint32_t hpd = ros_addr(ros_rma_alloc(4096 + 64));
    hpd = (hpd + 31) & ~31u;

    /* ---- a heap: made, described, the kernel's layout ---- */
    int i1 = heap(0, hpd, 0, 20, r) && errnum(r) == 0x181;
    int i2 = !heap(0, hpd, 0, 4096, r) && ros_ld32(hpd) == 0x70616548u &&
             ros_ld32(hpd + 4) == 0 && ros_ld32(hpd + 8) == 16 && ros_ld32(hpd + 12) == 4096;
    int i3 = !heap(1, hpd, 0, 0, r) && r[2] == 4096 - 16 - 4 && r[3] == 4096 - 16;
    int i4 = heap(1, hpd + 4, 0, 0, r) && errnum(r) == 0x182 && heap(99, hpd, 0, 0, r) &&
             errnum(r) == 0x180;
    check(i1 && i2 && i3 && i4,
          "OS_Heap 0, 1 -- \"Heap\", free, base, end; described; &181, &182, &180", NULL);

    /* ---- claims: from the base, then first fit from the free list ---- */
    heap(2, hpd, 0, 100, r);
    uint32_t a = r[2];
    heap(2, hpd, 0, 1, r);
    uint32_t b = r[2];
    heap(2, hpd, 0, 200, r);
    uint32_t c = r[2];
    heap(2, hpd, 0, 40, r);
    uint32_t d = r[2];
    heap(6, hpd, a, 0, r);
    uint32_t asize = r[3];
    heap(6, hpd, b, 0, r);
    int g1 = a == hpd + 20 && asize == 104 && r[3] == 8 && b == a + 104 && c == b + 8 &&
             d == c + 204 && heap(2, hpd, 0, 0, r) && errnum(r) == 0x184 && r[2] == 0;
    heap(3, hpd, c, 0, r);              /* free the 204: a split takes its end */
    heap(2, hpd, 0, 50, r);
    int g2 = r[2] == c - 4 + (204 - 56) + 4 && ros_ld32(c - 4 + 4) == 204 - 56;
    heap(3, hpd, r[2], 0, r);           /* back together: 204 again */
    heap(2, hpd, 0, 192, r);            /* 196 of 204: the 8 left over goes with it */
    uint32_t e = r[2];
    heap(6, hpd, e, 0, r);
    int g3 = e == c && r[3] == 204;
    heap(3, hpd, e, 0, r);
    check(g1 && g2 && g3,
          "OS_Heap 2, 6 -- sizes with their header; splits give the end; 8 bytes left join "
          "the claim", "a &%X b &%X c &%X d &%X", a - hpd, b - hpd, c - hpd, d - hpd);

    /* ---- frees: joined with neighbours, and with the base ---- */
    heap(3, hpd, a, 0, r);
    heap(3, hpd, b, 0, r);              /* a+b join: 112 */
    int f1 = ros_ld32(hpd + 4) == (a - 4) - (hpd + 4) && ros_ld32(a - 4 + 4) == 112 + 204;
    heap(3, hpd, d, 0, r);              /* the last block: the base comes down, all free */
    int f2 = ros_ld32(hpd + 8) == 16 && ros_ld32(hpd + 4) == 0;
    int f3 = heap(3, hpd, hpd + 21, 0, r) && errnum(r) == 0x185;
    check(f1 && f2 && f3, "OS_Heap 3 -- freed blocks join; the last goes back to the base; "
                          "&185 not a block", "free &%X", ros_ld32(hpd + 4));

    /* ---- ExtendBlock: in place, by moving, and shrinking ---- */
    heap(2, hpd, 0, 60, r);
    a = r[2];
    memset(ros_ptr(a), 0xA5, 60);
    heap(4, hpd, a, 100, r);            /* against the base: grows where it is */
    int x1 = r[2] == a && ros_ld32(a - 4) == 164;
    heap(2, hpd, 0, 60, r);
    b = r[2];
    heap(4, hpd, a, 400, r);            /* a used block after: it moves, data and all */
    uint32_t moved = r[2];
    int x2 = moved != a && ((uint8_t *)ros_ptr(moved))[59] == 0xA5 && ros_ld32(moved - 4) == 564;
    heap(4, hpd, b, -4, r);             /* 4 bytes, a used block after: refused, quietly */
    int x3 = !heap(4, hpd, moved, -4, r) && ros_ld32(moved - 4) == 560;
    heap(4, hpd, b, -64, r);            /* shrunk to nothing: R2 = -1 */
    int x4 = r[2] == 0xFFFFFFFFu;
    heap(3, hpd, moved, 0, r);
    check(x1 && x2 && x3 && x4,
          "OS_Heap 4 -- grows in place, moves with its data, 4-byte shrinks only into free "
          "space, gone at nothing", NULL);

    /* ---- ExtendHeap, and aligned claims ---- */
    int h1 = !heap(5, hpd, 0, 64, r) && r[3] == 64 && ros_ld32(hpd + 12) == 4096 + 64;
    heap(2, hpd, 0, 100, r);
    a = r[2];
    int h2 = heap(5, hpd, 0, (uint32_t)-5000, r) && errnum(r) == 0x187 &&
             ros_ld32(hpd + 12) == ros_ld32(hpd + 8);
    heap(3, hpd, a, 0, r);
    heap(5, hpd, 0, 4000, r);           /* within the block it lives in */
    uint32_t q[7] = { 7, hpd, 256, 100, 0, 0, 0 };
    int qv = swi7(XOS_Heap, q);
    int h3 = !qv && (q[2] & 255) == 0;
    uint32_t q2[7] = { 7, hpd, 16, 40, 64, 0, 0 };
    swi7(XOS_Heap, q2);
    int h4 = (q2[2] & 15) == 0 && (q2[2] & ~63u) == ((q2[2] + 39) & ~63u);
    check(h1 && h2 && h3 && h4,
          "OS_Heap 5, 7 -- the heap grows, shrinks to its base with &187; aligned, and within "
          "a boundary", "&%X &%X", q[2], q2[2]);

    /* ---- the RMA, as OS_Module claims from it ---- */
    int mv = module(6, NULL, 0, 100, r);
    uint32_t blk = r[2];
    /* 32n bytes, and a free block's leftover of 8 or less, which the
     * kernel's heap gives with it (heap.c) */
    uint32_t got = mv ? 0 : ros_ld32(blk - 4);
    int m1 = !mv && got % 32 <= 8 && got >= 104;
    uint32_t h[7] = { 6, ros_rma_heap(), blk, 0, 0, 0, 0 };
    swi7(XOS_Heap, h);
    int m2 = h[3] == ros_ld32(blk - 4);
    module(13, NULL, blk, 200, r);
    uint32_t grown = r[2];
    int m3 = ros_ld32(grown - 4) >= 304;
    int m4 = !module(7, NULL, grown, 0, r) && module(7, NULL, grown, 0, r) && errnum(r) == 0x185;
    uint32_t al[7] = { 24, 0, 0, 100, 256, 0, 0 };
    swi7(XOS_Module, al);
    int m5 = (al[2] & 255) == 0;
    module(7, NULL, al[2], 0, r);
    al[0] = 24, al[4] = 3;
    int m6 = swi7(XOS_Module, al) && errnum(al) == 0x117 && !module(5, NULL, 0, 0, r) && r[3] > 0;
    check(m1 && m2 && m3 && m4 && m5 && m6,
          "OS_Module 5, 6, 7, 13, 24 -- RMA blocks of 32n bytes, OS_Heap reads them, aligned "
          "claims", "%d%d%d%d%d%d, block &%X of %u bytes, aligned &%X",
          m1, m2, m3, m4, m5, m6, blk, got, al[2]);

    /* ---- names, enumeration, the ROM ---- */
    int n1 = !module(18, "buff.", 0, 0, r) && r[3] &&
             strcmp(ros_ptr(r[3] + ros_ld32(r[3] + 0x10)), "BufferManager") == 0 &&
             strcmp(ros_ptr(r[5]), "Base") == 0;
    int n2 = module(18, "NoSuchModule", 0, 0, r) && errnum(r) == 0x102 &&
             strcmp(((os_error *)ros_ptr(r[0]))->errmess, "Module NoSuchModule not found") == 0 &&
             module(18, "NoSuchModule 1.00", 0, 0, r) &&
             strcmp(((os_error *)ros_ptr(r[0]))->errmess, "Module NoSuchModule 1.00 not found") == 0;
    unsigned count = 0;
    uint32_t e12[7] = { 12, 0, 0, 0, 0, 0, 0 };
    uint32_t first = 0;
    while (!swi7(XOS_Module, e12) && count < 256) {
        if (!count)
            first = e12[3];
        count++;
        e12[0] = 12;
    }
    int n3 = count >= 7 && strcmp(ros_ptr(first + ros_ld32(first + 0x10)), "UtilityModule") == 0 &&
             errnum(e12) == 0x108;
    unsigned roms = 0, active = 0;
    uint32_t bmversion = 0;
    for (uint32_t e20[7] = { 20, 0, (uint32_t)-1, 0, 0, 0, 0 }; !swi7(XOS_Module, e20);
         e20[0] = 20, e20[2] = (uint32_t)-1) {
        roms++;
        active += e20[4] == 1;
        if (strcmp(ros_ptr(e20[3]), "BufferManager") == 0)
            bmversion = e20[6];
    }
    check(n1 && n2 && n3 && roms == active && roms >= 7 && bmversion == 0x3900,
          "OS_Module 12, 18, 20 -- names abbreviated, not found &102 (the name to its end); "
          "the chain; the ROM with versions", "%u modules, %u ROM, BufferManager &%X", count, roms, bmversion);

    /* ---- a module's life: incarnations, services, death ---- */
    ros_module_add(&life, "");
    int l1 = inits == 1 && !module(14, "Lifecycle%Two", 0, 0, r) && inits == 2 &&
             module(14, "Lifecycle%Two", 0, 0, r) && errnum(r) == 0x10B &&
             module(14, "Lifecycle", 0, 0, r) && errnum(r) == 0x10A;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x12345;
    ros_service_call(&s);
    int l2 = services == 2;
    module(18, "Lifecycle", 0, 0, r);   /* the newest is preferred */
    int l3 = strcmp(ros_ptr(r[5]), "Two") == 0 && !module(16, "Lifecycle%Base", 0, 0, r) &&
             !module(18, "Lifecycle", 0, 0, r) && strcmp(ros_ptr(r[5]), "Base") == 0;
    strcpy(ros_ptr(text + 128), "Three");
    uint32_t rn[7] = { 15, text, text + 128, 0, 0, 0, 0 };
    strcpy(ros_ptr(text), "Lifecycle%Two");
    int l4 = !swi7(XOS_Module, rn) && !module(18, "Lifecycle%Three", 0, 0, r) && r[2] == 1;
    int l5 = !module(4, "Lifecycle%Base", 0, 0, r) && finals == 1 &&
             !module(18, "Lifecycle", 0, 0, r) && strcmp(ros_ptr(r[5]), "Three") == 0 &&
             !module(4, "Lifecycle", 0, 0, r) && finals == 2 &&
             module(18, "Lifecycle", 0, 0, r) && errnum(r) == 0x102;
    check(l1 && l2 && l3 && l4 && l5,
          "OS_Module 14, 15, 16, 4 -- incarnations made, each served, preferred, renamed, "
          "killed", "%u inits, %u finals, %u services", inits, finals, services);

    /* ---- a ROM module killed, and started again ---- */
    ros_module_add(&life, "");          /* to watch the services */
    uint32_t sum[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int k1 = !cli("RMKill T0Demo") && post_finals == 1 &&
             swi7(0x000E0000u, sum) && errnum(sum) == 0x1E6;
    uint32_t e20[7] = { 20, 0, (uint32_t)-1, 0, 0, 0, 0 };
    unsigned dormant = 0;
    while (!swi7(XOS_Module, e20)) {
        dormant += e20[4] == 0 && strcmp(ros_ptr(e20[3]), "T0Demo") == 0;
        e20[0] = 20, e20[2] = (uint32_t)-1;
    }
    uint32_t words[2] = { 3, 4 };
    uint32_t wa = ros_addr(ros_rma_alloc(8));
    memcpy(ros_ptr(wa), words, 8);
    uint32_t sum2[7] = { wa, 2, 0, 0, 0, 0, 0 };
    int k2 = dormant == 1 && !cli("RMReInit T0Demo") && post_inits == 1 &&
             !swi7(0x000E0000u, sum2) && sum2[0] == 7;
    ros_rma_free(ros_ptr(wa));
    int k3 = cli("RMKill NoSuchModule") == 0x102 && cli("RMEnsure BufferManager 0.00") == 0 &&
             cli("RMEnsure Lifecycle 9.99") == 0x10F &&
             cli("RMEnsure NoSuchModule 1.00 Set Test$RME missing") == 0;
    /* The kernel's module is the OS's version, 5.30: apps' !Run files
     * gate on it (RMEnsure UtilityModule 3.10) */
    int k5 = cli("RMEnsure UtilityModule 3.10") == 0 && cli("RMEnsure UtilityModule 5.30") == 0 &&
             cli("RMEnsure UtilityModule 5.31") == 0x10F;
    uint32_t v[7] = { 0, 0, 0, 0, 0, 0, 0 };
    strcpy(ros_ptr(text + 128), "Test$RME");
    v[0] = text + 128, v[1] = text, v[2] = 200, v[4] = 0;
    int k4 = !swi7(XOS_ReadVarVal, v) && v[2] == 7;
    cli("Unset Test$RME");
    check(k1 && k2 && k3 && k4,
          "*RMKill, *RMReInit -- a ROM module dies, is dormant, starts again; *RMEnsure",
          "post-final %u post-init %u dormant %u", post_finals, post_inits, dormant);
    check(k5, "*RMEnsure UtilityModule -- 3.10 and 5.30 met, 5.31 not: the kernel's version is the OS's",
          "UtilityModule's version is not 5.30");

    /* FPEmulator, a stub (modules/fpemulator, #114): stock !Run files'
     * RMEnsure FPEmulator 4.03 passes, as on 5.30, whose ROM has 4.39;
     * its help string, *Help FPEmulator and FPEmulator_Version 5.30's */
    int fp1 = cli("RMEnsure FPEmulator 4.03") == 0 && cli("RMEnsure FPEmulator 4.39") == 0 &&
             cli("RMEnsure FPEmulator 4.40") == 0x10F &&
             cli("RMEnsure FPEmulator 4.03 Error This application requires FPEmulator 4.03 or later") == 0;
    int fp2 = !module(18, "FPEmulator", 0, 0, r) &&
             strcmp(ros_ptr(r[3] + ros_ld32(r[3] + 0x14)), "FPEmulator\t4.39 (30 Mar 2024) (1.13CELM)") == 0 &&
             strcmp(ros_ptr(r[3] + ros_ld32(r[3] + 0x10)), "FPEmulator") == 0;
    uint32_t fv[7] = { 0 }, fl[7] = { 0 }, fd[7] = { 0 }, fx[7] = { 0 };
    int fp3 = !swi7(0x20000u | 0x40480u, fv) && fv[0] == 439 && !swi7(0x20000u | 0x40484u, fl) &&
             fl[0] == 136 && !swi7(0x20000u | 0x40481u, fd) && fd[0] == 0xFFFFFFFFu &&
             swi7(0x20000u | 0x4048Au, fx) && errnum(fx) == 0x1E6;
    strcpy(ros_ptr(text), "Help FPEmulator");
    uint32_t hc[7] = { text };
    nprinted = 0;
    ros_vector_claim_native(0x03u, capture, 0);
    int hv = swi7(0x20000u | 0x05u, hc);         /* OS_CLI */
    ros_vector_release_native(0x03u, capture, 0);
    printed[nprinted] = 0;
    char lines[512];
    size_t n = 0;
    for (const char *p = printed; *p && n < sizeof lines - 1; p++)   /* VDU 14, 15 and CRs gone */
        if (*p != 14 && *p != 15 && *p != 13)
            lines[n++] = *p;
    lines[n] = 0;
    int fp4 = !hv && strcmp(lines, "==> Help on keyword FPEmulator\n"
                                  "Module is: FPEmulator      4.39 (30 Mar 2024) (1.13CELM)\n") == 0;
    check(fp1 && fp2 && fp3 && fp4,
          "FPEmulator, a stub (#114) -- RMEnsure FPEmulator 4.03 and 4.39 met, 4.40 not; its help string, "
          "*Help FPEmulator and FPEmulator_Version (439), ContextLength (136), DeactivateContext (-1) "
          "5.30's; SWI 10 out of range",
          "RMEnsure %d, help string %d, SWIs %d (%u %u &%X), *Help %d '%s'", fp1, fp2, fp3, fv[0], fl[0], fd[0],
          fp4, lines);

    /* ---- clearing, and what cannot be done ---- */
    int c1 = !module(9, NULL, 0, 0, r) && module(18, "Lifecycle", 0, 0, r) &&
             !module(18, "BufferManager", 0, 0, r) && !module(18, "T0Demo", 0, 0, r);
    /* OS_Module 1 loads a module file now (#23; tests/capps/ctest/rmload.py
     * runs CTest through *RMLoad): one not there is FileSwitch's error */
    int c2 = module(1, "NoSuchModuleFile", 0, 0, r) && errnum(r) == 0xD6 &&
             module(23, NULL, 0, 0, r) && errnum(r) == 0x105;
    check(c1 && c2,
          "OS_Module 9 clears all but the ROM's; loading a file not there, its error; 23 unknown",
          "&%X", errnum(r));

    /* A wrapped ROM module (ROSGDROM) whose relocation offset is near 4 GB:
     * refused as a module, and nothing written outside its block */
    uint8_t *wrap = ros_rma_alloc(96);
    uint32_t wr[7] = { 11, ros_addr(wrap), 96, 0, 0, 0, 0 };
    memset(wrap, 0, 96);
    memcpy(wrap, "ROSGDROM", 8);
    uint32_t hdr[6] = { 0x01000000u, 0x40, 1, 0, 0, 0xFFFFFFFCu };    /* link, size, n, nl, nb, offset */
    memcpy(wrap + 8, hdr, sizeof hdr);
    int wv = swi7(XOS_Module, wr);
    check(wv, "OS_Module 11 of a wrapped ROM module with a relocation offset past its end: refused",
          "returned %d", wv);
    ros_rma_free(wrap);

    ros_rma_free(ros_ptr(text));
}
