/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_wimpswis.c: the kernel SWIs the Window Manager needed and did
 * not have: OS_ValidateAddress (every window handle is checked with it),
 * OS_ReadMemMapInfo (slots are sized in pages), OS_HeapSort and
 * OS_HeapSort32 (its sprite names and message lists), and OS_CRC. The SWI
 * ring (switrace.h) that found them is checked too, and so is the compiled
 * Wimp's own jump table, which the dispatcher must know row by row. Also
 * checked is *IconSprites before the Wimp has its RAM sprite area.
 *
 * The sorts' and CRCs' expected values are RISC OS 5.30's, from the farm.
 * Duplicates land where the kernel's heap sort puts them. */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rom_wimp.h"
#include "selftest.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? 1 : s.c ? 2 : 0;
}

static int words(uint32_t a, const int32_t *want, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if ((int32_t)ros_ld32(a + 4 * i) != want[i])
            return 0;
    return 1;
}

static int last_line(const char *line, void *ctx)
{
    snprintf(ctx, 256, "%s", line);
    return 0;
}

void ros_selftest_wimpswis(void)
{
    uint8_t *mem = ros_rma_alloc(1024);
    uint32_t a = ros_addr(mem), b = a + 256, s = a + 512;

    /* OS_ValidateAddress: the RMA, the ROM, ScratchSpace; not the gap
     * between the ROM and IO, nor past the slot */
    uint32_t r[8] = { a, a + 64 };
    int rma = swi(XOS_ValidateAddress, r);
    uint32_t r2[8] = { ROS_ROM_BASE, ROS_ROM_BASE + 16 };
    int rom = swi(XOS_ValidateAddress, r2);
    uint32_t r3[8] = { ROS_SCRATCH_BASE, ROS_SCRATCH_BASE + 256 };
    int scratch = swi(XOS_ValidateAddress, r3);
    uint32_t r4[8] = { 0xC0000000u, 0xC0000010u };
    int io = swi(XOS_ValidateAddress, r4);
    check(rma == 0 && rom == 0 && scratch == 0 && io == 2,
          "OS_ValidateAddress -- the RMA, the ROM and ScratchSpace valid (C clear); where IO space was, not",
          "%d %d %d %d", rma, rom, scratch, io);

    uint32_t m[8] = { 0 };
    int mv = swi(XOS_ReadMemMapInfo, m);
    check(!mv && m[0] == 4096 && m[1] > 0 && m[1] <= 0x7FFFF,
          "OS_ReadMemMapInfo -- 4K pages; the RAM in them, under 2 GB", "V%d R0 %u R1 %u", mv, m[0], m[1]);

    /* integers with duplicates, as cardinals (type 0) and integers (1) */
    static const int32_t data[12] = { 5, -3, 7, 5, 0, -3, 2147483647, -2147483647 - 1, 7, 1, 5, 9 };
    static const int32_t as_card[12] = { 0, 1, 5, 5, 5, 7, 7, 9, 2147483647, -2147483647 - 1, -3, -3 };
    static const int32_t as_int[12] = { -2147483647 - 1, -3, -3, 0, 1, 5, 5, 5, 7, 7, 9, 2147483647 };
    int ok[2];
    for (uint32_t type = 0; type < 2; type++) {
        memcpy(ros_ptr(b), data, sizeof data);
        uint32_t h[8] = { 12, b, type };
        swi(XOS_HeapSort32, h);
        ok[type] = words(b, type ? as_int : as_card, 12) && h[1] == b && h[2] == type;
    }
    check(ok[0] && ok[1], "OS_HeapSort32 0, 1 -- cardinals, integers, duplicates, extremes; registers kept",
          "%d %d", ok[0], ok[1]);

    /* strings by pointer, case insensitive (4) and sensitive (5): which of
     * equal strings comes first is the heap sort's */
    static const char *const names[8] = { "banana", "Apple", "apple", "Cherry", "banana", "APPLE", "cherry", "b" };
    static const int32_t insens[8] = { 13, 7, 33, 46, 26, 0, 39, 19 };
    static const int32_t sens[8] = { 33, 7, 19, 13, 46, 0, 26, 39 };
    uint32_t q = s, ptrs[8];
    for (unsigned i = 0; i < 8; i++) {
        strcpy(ros_ptr(q), names[i]);
        ptrs[i] = q;
        q += (uint32_t)strlen(names[i]) + 1;
    }
    int str[2];
    for (uint32_t type = 4; type < 6; type++) {
        for (unsigned i = 0; i < 8; i++)
            ros_st32(b + 4 * i, ptrs[i]);
        uint32_t h[8] = { 8, b, type };
        swi(XOS_HeapSort32, h);
        const int32_t *want = type == 4 ? insens : sens;
        str[type - 4] = 1;
        for (unsigned i = 0; i < 8; i++)
            str[type - 4] &= ros_ld32(b + 4 * i) - s == (uint32_t)want[i];
    }
    check(str[0] && str[1], "OS_HeapSort32 4, 5 -- strings by pointer, case insensitive and sensitive, "
          "equals in the kernel's order", "%d %d", str[0], str[1]);

    /* blocks of key and tag, by the old API, its flags in R1's top bits (so
     * its array below &20000000: here in ScratchSpace, clear of the temp
     * slot at its start): the pointers built (bit 30), the blocks put in
     * their order (bit 31, implied); then HeapSort32 with its own temp slot */
    static const int32_t keys[14] = { 3, 1, 1, 2, 3, 3, 2, 4, 1, 5, 3, 6, 0, 7 };
    static const int32_t sorted[14] = { 0, 7, 1, 5, 1, 2, 2, 4, 3, 6, 3, 3, 3, 1 };
    memcpy(ros_ptr(a), keys, sizeof keys);
    uint32_t low = ROS_SCRATCH_BASE + 0x1000;
    uint32_t h[8] = { 7, low | 1u << 30, 3, 0, a, 8 };
    swi(XOS_HeapSort, h);
    int blocks = words(a, sorted, 14) && h[1] == (low | 1u << 30);
    for (int32_t i = 0; i < 7; i++) {
        ros_st32(a + 8 * i, 7 - i);
        ros_st32(a + 8 * i + 4, i);
    }
    static const int32_t sorted32[14] = { 1, 6, 2, 5, 3, 4, 4, 3, 5, 2, 6, 1, 7, 0 };
    uint32_t h32[8] = { 7, b, 3, 0, a, 8, s, 7u << 29 };
    swi(XOS_HeapSort32, h32);
    int blocks32 = words(a, sorted32, 14);
    check(blocks && blocks32, "OS_HeapSort, OS_HeapSort32 -- blocks sorted through their pointers, built "
          "and applied; HeapSort32's temp slot", "%d %d", blocks, blocks32);

    /* OS_CRC: odd and even lengths, a step of 2, backwards, continued; 0 refused */
    strcpy(ros_ptr(s), "The quick brown fox jumps over the lazy dog");
    uint32_t c1[8] = { 0, s, s + 43, 1 }, c2[8] = { 0, s, s + 42, 1 }, c3[8] = { 0, s, s + 42, 2 };
    uint32_t c4[8] = { 0, s + 42, s, (uint32_t)-1 }, c5[8] = { 0x1234, s, s + 10, 1 }, c6[8] = { 0, s, s + 10, 0 };
    swi(XOS_CRC, c1), swi(XOS_CRC, c2), swi(XOS_CRC, c3), swi(XOS_CRC, c4), swi(XOS_CRC, c5);
    int refused = swi(XOS_CRC, c6) == 1;
    check(c1[0] == 0xFCDF && c2[0] == 0x1E36 && c3[0] == 0xB739 && c4[0] == 0x9047 && c5[0] == 0x49BB && refused,
          "OS_CRC -- the kernel's CRC-16 (&A001): 43 and 42 bytes, step 2, step -1, continued; step 0 refused",
          "&%X &%X &%X &%X &%X %d", c1[0], c2[0], c3[0], c4[0], c5[0], refused);

    /* The ring: the last SWI is there, with its registers, and five the
     * same in a row are one record, x5. */
    for (int i = 0; i < 5; i++) {
        uint32_t t[8] = { 0 };
        swi(XOS_ReadMonotonicTime, t);
    }
    char line[256] = "";
    ros_switrace_recent(1, last_line, line);
    check(strstr(line, "OS_ReadMonotonicTime") != NULL && strstr(line, "->") != NULL &&
              strstr(line, "x5 to") != NULL,
          "SWI ring -- the latest call recorded, with its result; repeats folded into it (x5)",
          "\"%s\"", line);

    /* Wimp_OpenWindow's stacking (Wimp02, openwlp3) jumps to a row of
     * openwlp3_jumptable with a computed MOV pc. Each of its four rows is
     * compiled code. The last, method 3, is behind -3, as iconising a
     * window opens it, or a foreground window opened behind a backdrop
     * window (Wimp02: standard and backdrop windows use method 0). It is `B
     * openwlp3_skip_to_bottom`, the label after it. That is a branch to the
     * next instruction, and so a no-op to rosasm, which had left it out
     * ('Call to &FC407998, which is not compiled code'; desk sweep F03). */
    unsigned rows = 0;
    for (uint32_t i = 0; i < 4; i++)
        rows += ros_code_lookup(WIMP_openwlp3_jumptable + 4 * i) != NULL;
    uint32_t last = ros_ld32(WIMP_openwlp3_jumptable + 12);
    check(rows == 4 && last == 0xEAFFFFFFu,
          "Wimp: every row of openwlp3_jumptable compiled code -- method 3 (behind -3, iconising) "
          "a B to the next instruction", "%u of 4 rows; the last &%08X", rows, last);

    /* *IconSprites before any task has started the Wimp (a !Boot run at
     * the desktop's start, before its first task): the RAM sprite area is
     * made then, and the file named is merged into it. Making the area
     * had overwritten the name with the area's ("File 'WSpr' not found",
     * #109). The file is the ROM's Pinboard tile, and its first sprite is
     * then found in the RAM area. */
    static const char tile[] = "Resources:$.Resources.Pinboard.Tile";
    uint32_t w0[8] = { 0 };
    swi(XWimp_BaseOfSprites, w0);
    snprintf(ros_ptr(a), 256, "IconSprites %s", tile);
    uint32_t c0[8] = { a };
    int cli = swi(XOS_CLI, c0);
    char why[64] = "";
    if (cli == 1)
        snprintf(why, sizeof why, "%s", (const char *)ros_ptr(c0[0] + 4));
    uint32_t w1[8] = { 0 };
    swi(XWimp_BaseOfSprites, w1);
    snprintf(ros_ptr(a), 256, "%s", tile);
    uint32_t f5[8] = { 5, a };
    int found = swi(XOS_File, f5) == 0 && f5[0] == 1;
    uint32_t nfile = 0, nram = 0;
    int named = 0;
    char first[13] = "";
    if (found && w1[1]) {
        uint8_t *file = ros_rma_alloc(f5[4]);
        uint32_t f16[8] = { 16, a, ros_addr(file), 0 };
        if (file && swi(XOS_File, f16) == 0) {
            nfile = ros_ld32(ros_addr(file));
            uint32_t sp = ros_addr(file) + ros_ld32(ros_addr(file) + 4) - 4;
            memcpy(first, ros_ptr(sp + 4), 12);
            first[12] = 0;
            nram = ros_ld32(w1[1] + 4);
            snprintf(ros_ptr(b), 16, "%s", first);
            uint32_t so[8] = { 0x118, w1[1], b };
            named = swi(XOS_SpriteOp, so) == 0;
        }
        if (file)
            ros_rma_free(file);
    }
    check(cli == 0 && w1[1] != 0 && nfile > 0 && nram >= nfile && named,
          "Wimp: *IconSprites before the first task -- the RAM sprite area made, the file's sprites "
          "merged into it", "RAM area &%X then &%X; %s; %u sprites in the file, %u in the area; "
          "\"%s\" %s", w0[1], w1[1], cli ? why : "no error", nfile, nram, first,
          named ? "there" : "not there");

    ros_rma_free(mem);
}
