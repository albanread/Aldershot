/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_squash.c: Squash, reimplemented (modules/squash): what must
 * always hold. Its SWIs make RISC OS 5.30's bytes, and unmake them. The
 * lengths and checksums here are of what the farm's Squash made of the same
 * inputs (tests/desktop/squash, compress.bas's sets). They fail
 * as RISC OS's do. The Desktop's banner sprites unsquash as the
 * Desktop unsquashes them. tests/desktop/squash holds the rest, register
 * by register, against the farm.
 */
#include <string.h>

#include "resourcefs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
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

static uint32_t errnum(const uint32_t r[8])
{
    return ((const os_error *)ros_ptr(r[0]))->errnum;
}

/* The probes' data (compress.bas): xorshift32 from &12345678 plus the kind */
static uint32_t xs(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    return x ^ x << 5;
}

static void text(uint8_t *p, uint32_t n)
{
    static const char *const words[16] = {
        "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ", "RISC ",
        "OS ", "squash ", "compress ", "module ", "workspace ", "and ", ". ",
    };
    uint32_t x = 0x12345678u + 1, i = 0;
    while (i < n) {
        x = xs(x);
        for (const char *w = words[x & 15]; *w && i < n; w++)
            p[i++] = (uint8_t)*w;
    }
}

static void noise(uint8_t *p, uint32_t n)
{
    uint32_t x = 0x12345678u + 2;
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(x = xs(x));
}

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    while (n--)
        h = (h ^ *p++) * 0x01000193u;
    return h;
}

/* A whole compression at one go: R0 flags 0, room as given.  The bytes
 * made, or -1 */
static uint32_t squash(uint32_t ws, uint32_t in, uint32_t n, uint32_t out, uint32_t room)
{
    uint32_t r[8] = { 0, ws, in, n, out, room };
    if (swi(XSquash_Compress, r) || r[0] != 0 || r[2] != in + n || r[3] != 0 ||
        r[1] != ws || r[4] - out + r[5] != room)
        return (uint32_t)-1;
    return r[4] - out;
}

/* A compression a little at a time, as the probes do it: 777 bytes in and
 * 333 out a call.  The bytes made, or -1 */
static uint32_t squash_bits(uint32_t ws, uint32_t in, uint32_t n, uint32_t out)
{
    uint32_t done = 0, made = 0, flags = 0;
    for (int calls = 0; calls < 2000; calls++) {
        uint32_t avail = n - done < 777 ? n - done : 777;
        uint32_t more = done + 777 >= n ? 0 : 2;
        uint32_t r[8] = { flags | more, ws, in + done, avail, out + made, 333 };
        if (swi(XSquash_Compress, r))
            return (uint32_t)-1;
        done = r[2] - in, made = r[4] - out, flags = 1;
        if (r[0] == 0)
            return done == n ? made : (uint32_t)-1;
    }
    return (uint32_t)-1;
}

/* Decompressions: flags 4 (the Desktop's), 0, and 100 bytes in and 200
 * out a call, the unused input given again; whether each gives n bytes
 * equal to the input's */
static int unsquash(uint32_t ws, uint32_t in, uint32_t len, uint32_t back, const uint8_t *want,
                    uint32_t n)
{
    int ok = 1;
    for (uint32_t flags = 0; flags <= 4; flags += 4) {
        memset(ros_ptr(back), 0, n);
        uint32_t r[8] = { flags, ws, in, len, back, n };
        ok &= !swi(XSquash_Decompress, r) && r[0] == 0 && r[3] == 0 && r[5] == 0 &&
              memcmp(ros_ptr(back), want, n) == 0;
    }
    memset(ros_ptr(back), 0, n);
    uint32_t done = 0, made = 0, flags = 0, calls = 0, status = 1;
    while (status != 0 && calls++ < 20000) {
        uint32_t avail = len - done < 100 ? len - done : 100;
        uint32_t more = done + avail >= len ? 0 : 2;
        uint32_t room = n - made < 200 ? n - made : 200;
        uint32_t r[8] = { flags | more, ws, in + done, avail, back + made, room };
        if (swi(XSquash_Decompress, r))
            return 0;
        status = r[0], done = r[2] - in, made = r[4] - back, flags = 1;
    }
    return ok && made == n && done == len && memcmp(ros_ptr(back), want, n) == 0;
}

void ros_selftest_squash(void)
{
    struct ros_module *m = NULL;
    for (struct ros_module *i = ros_module_first(); i; i = i->next)
        if (i->title && strcmp(i->title, "Squash") == 0)
            m = i;
    uint32_t c1[8] = { 8, (uint32_t)-1, 2, 3, 4, 5 }, c2[8] = { 8, 1001 };
    uint32_t c3[8] = { 8, 0x7FFFFFFF }, d1[8] = { 8, 1000, 2, 3, 4, 5 };
    int sizes = !swi(XSquash_Compress, c1) && !swi(XSquash_Compress, c2) &&
                !swi(XSquash_Compress, c3) && !swi(XSquash_Decompress, d1);
    check(m && ros_module_version(m) == 0x3100 && m->swi_chunk == 0x42700 && sizes &&
          c1[0] == 31744 && c1[1] == (uint32_t)-1 && c1[2] == 2 && c1[5] == 5 &&
          c2[1] == 1513 && c3[1] == 0x4000000Au && d1[0] == 17408 && d1[1] == (uint32_t)-1 &&
          d1[2] == 2 && d1[5] == 5,
          "Squash 0.31, native, SWIs at &42700 -- the workspaces (31744, 17408) and the most "
          "output, 12 + 3n/2 in 32 bits, as RISC OS 5.30 gives them", "%d; %u %u %u %u",
          sizes, c1[0], c2[1], c3[1], d1[0]);

    uint32_t ws = ros_addr(ros_rma_alloc(32 * 1024)), in = ros_addr(ros_rma_alloc(70000));
    uint32_t out = ros_addr(ros_rma_alloc(110000)), back = ros_addr(ros_rma_alloc(70000));
    if (!ws || !in || !out || !back) {
        check(0, "Squash: RMA for the tests", NULL);
        return;
    }

    /* 30000 bytes of text, 70000 random: the fast algorithm (room for the
     * worst case), the restartable one (a byte less), and that a little at
     * a time; RISC OS 5.30's lengths and checksums */
    static const struct {
        void (*make)(uint8_t *, uint32_t);
        uint32_t n, fast, fast_sum, state, state_sum;
    } sets[] = {
        { text, 30000, 7048, 0x5CCC3CB6u, 6599, 0xA3EB7A79u },
        { noise, 70000, 95870, 0xBB288C2Bu, 98898, 0x1C3F96ABu },
    };
    int made = 1, unmade = 1;
    for (unsigned k = 0; k < 2; k++) {
        uint32_t n = sets[k].n;
        sets[k].make(ros_ptr(in), n);
        uint8_t *p = ros_ptr(out);
        uint32_t f = squash(ws, in, n, out, 3 + n * 3 / 2);
        made &= f == sets[k].fast && fnv(p, f) == sets[k].fast_sum;
        unmade &= unsquash(ws, out, f, back, ros_ptr(in), n);
        uint32_t s = squash(ws, in, n, out, 3 + n * 3 / 2 - 1);
        made &= s == sets[k].state && fnv(p, s) == sets[k].state_sum;
        unmade &= unsquash(ws, out, s, back, ros_ptr(in), n);
        s = squash_bits(ws, in, n, out);
        made &= s == sets[k].state && fnv(p, s) == sets[k].state_sum;
    }
    /* 13 bytes of text, whole */
    static const uint8_t short13[18] = { 0x1F, 0x9D, 0x8C, 0x6A, 0xEA, 0xB4, 0x81, 0x33, 0x07,
                                         0x84, 0x18, 0x39, 0x6F, 0xEE, 0xB8, 0x01, 0x11, 0x07 };
    text(ros_ptr(in), 13);
    made &= squash(ws, in, 13, out, 100) == 18 && memcmp(ros_ptr(out), short13, 18) == 0;
    check(made, "Squash_Compress -- RISC OS 5.30's bytes: fast (the table cleared when full) "
          "and restartable (cleared after 32K), whole and 777 in / 333 out at a time", NULL);
    check(unmade, "Squash_Decompress -- each back to its input: fast, whole, and 100 in / 200 "
          "out at a time", NULL);

    /* As the Desktop does it (its s/Desktop): the file's header off, into
     * a sprite area, R0 bit 2 */
    uint32_t file = ros_resourcefs_find("Resources:$.Resources.Desktop.Sprites");
    int desk = 0;
    if (file) {
        uint32_t len = ros_ld32(file) - 4, size = ros_ld32(file + 8);
        uint32_t area = ros_addr(ros_rma_alloc(size + 4));
        uint32_t r[8] = { 4, ws, file + 4 + 20, len - 20, area + 4, size };
        desk = area && !swi(XSquash_Decompress, r) && r[0] == 0 && r[3] == 0 && r[5] == 0 &&
               ros_ld32(area + 4) == 2 && ros_ld32(area + 8) == 16 &&
               ros_ld32(area + 12) == size + 4;
        if (area)
            ros_rma_free(ros_ptr(area));
    }
    check(desk, "The Desktop's banner sprites unsquash as the Desktop unsquashes them -- two "
          "sprites, the area full", NULL);

    uint32_t e1[8] = { 9, ws, in, 10, out, 100 }, e2[8] = { 0, ws, in, 10, 0xFFFFFF00u, 100 };
    uint32_t e3[8] = { 0, ws, out, 18, back, 100 }, e4[8] = { 0x10, ws, in, 10, out, 100 };
    uint32_t e5[8] = { 0 }, e6[8] = { 1, ws, in, 10, out, 100 };
    /* the fast decompressor reads R2 + 3 whatever R3 is: here unmapped */
    uint32_t e7[8] = { 4, ws, 0x1000, 2, out, 100 };
    memcpy(ros_ptr(out), short13, 18);
    ros_st8(out + 2, 0x8D);                         /* the header's bits byte, wrong */
    uint32_t n[7] = { 0 };
    n[0] = swi(XSquash_Compress, e1) ? errnum(e1) : 0;
    n[1] = swi(XSquash_Compress, e2) ? errnum(e2) : 0;
    n[2] = swi(XSquash_Decompress, e3) ? errnum(e3) : 0;
    n[3] = swi(XSquash_Decompress, e4) ? errnum(e4) : 0;
    ros_st32(ws, 0xE);          /* not starting; where to go on from: 7, nowhere */
    n[4] = swi(XSquash_Compress, e6) ? errnum(e6) : 0;
    n[5] = swi(0x62702, e5) ? errnum(e5) : 0;
    n[6] = swi(XSquash_Decompress, e7) ? errnum(e7) : 0;
    int errs = n[0] == 0x924 && n[1] == 0x921 && n[2] == 0x922 && e3[3] == 18 &&
               n[3] == 0x924 && n[4] == 0x923 && n[5] == 0x1E6 && n[6] == 0x921 &&
               strcmp(((os_error *)ros_ptr(e1[0]))->errmess,
                      "Bad parameters for module Squash") == 0;
    check(errs, "Squash's errors -- Bad parameters, address, input and workspace (&924, &921, "
          "&922, &923), and &1E6 past the chunk", "&%X &%X &%X &%X &%X &%X &%X", n[0], n[1],
          n[2], n[3], n[4], n[5], n[6]);

    ros_rma_free(ros_ptr(ws)), ros_rma_free(ros_ptr(in));
    ros_rma_free(ros_ptr(out)), ros_rma_free(ros_ptr(back));
}
