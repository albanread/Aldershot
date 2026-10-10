/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_int.c: integer code lifted to C expressions (tier 1), against
 * the same ObjAsm compiled instruction by instruction (tier 0).
 *
 * modules/inttest is compiled twice: as IntTest, lifted, and as IntTest0
 * with --no-lift. Each routine is called in both with the same random
 * registers, flags and memory. Every register, every flag and every
 * byte of the buffer it works on must come out the same. Tier 0 is exact
 * by construction, because each instruction's A32 semantics is one statement.
 * So this checks the lifter's folding, its flag analysis and its planning
 * on inputs no one chose.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rom_inttest.h"
#include "rom_inttest0.h"
#include "selftest.h"

#define check ros_check

#define TRIALS 3000
#define BUF    512

static const struct {
    const char *name;
    uint32_t lifted, tier0;
} routines[] = {
#define R(n) { #n, INTTEST_##n, INTTEST0_##n }
    R(Arith), R(Shifts), R(AddSub), R(Logic), R(Conds), R(Chains), R(FlagsIn),
    R(Loops), R(Search), R(Memory), R(Blocks), R(Mults), R(Wide), R(Calls),
    R(Switch), R(Strlen), R(Divide), R(Consts), R(Psr), R(CondMem), R(GuardLd),
    R(Across), R(Chain), R(Same), R(Idioms), R(Names), R(Frame), R(Slot), R(Outer), R(Args),
#undef R
};

static uint32_t seed = 0x9E3779B9;

static uint32_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* A register's value. A quarter of the time it is an edge case and another
 * quarter a byte, so that compares come out equal and small counts happen.
 * The rest of the time it is anything. */
static uint32_t pick(void)
{
    static const uint32_t edge[] = {
        0, 1, 2, 3, 4, 10, 20, 31, 32, 33, 97, 99, 100, 122, 0x7F, 0x80, 0xFF, 0x100,
        0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFF00,
    };
    switch (rnd() % 4) {
    case 0:
        return edge[rnd() % (sizeof edge / sizeof edge[0])];
    case 1:
        return rnd() & 0xFF;
    default:
        return rnd();
    }
}

/* Whether two registers hold the same. They are equal, or they are addresses
 * at the same offset into the two images (a BL's return address, say). */
static int same_reg(uint32_t a, uint32_t b)
{
    return a == b || (a - rom_inttest_base < rom_inttest_size && b - rom_inttest0_base < rom_inttest0_size &&
                      a - rom_inttest_base == b - rom_inttest0_base);
}

/* Run compiled code; on a raise, its error number, else 0. */
static uint32_t run(struct ros_cpu *s, uint32_t entry)
{
    struct ros_handler h;
    if (ROS_TRY(&h)) {
        ros_call(s, entry);
        ros_handler_pop(&h);
        return 0;
    }
    return h.error->errnum ? h.error->errnum : 1;
}

void ros_selftest_int(void)
{
    ros_console_printf("rosgd: self-test -- integer code lifted to C, against tier 0\n");
    uint8_t *buf = ros_rma_alloc(BUF);
    uint32_t at = ros_addr(buf);
    static uint8_t init[BUF], lifted[BUF];

    for (unsigned f = 0; f < sizeof routines / sizeof routines[0]; f++) {
        unsigned bad = 0;
        char first[240] = "";
        for (unsigned t = 0; t < TRIALS; t++) {
            struct ros_cpu in, a, b;
            ros_cpu_enter(&in);
            for (unsigned r = 0; r < 13; r++)
                in.r[r] = pick();
            in.r[8] = at;
            unsigned flags = rnd();
            in.n = flags & 1;
            in.z = flags >> 1 & 1;
            in.c = flags >> 2 & 1;
            in.v = flags >> 3 & 1;
            for (unsigned k = 0; k < BUF; k++)
                init[k] = (uint8_t)(rnd() % 3 == 0 ? 0 : rnd());
            init[255] = 0;

            a = in;
            memcpy(buf, init, BUF);
            uint32_t ea = run(&a, routines[f].lifted);
            memcpy(lifted, buf, BUF);

            b = in;
            memcpy(buf, init, BUF);
            uint32_t eb = run(&b, routines[f].tier0);

            int regs = 1;
            for (unsigned r = 0; r < 16; r++)
                regs &= same_reg(a.r[r], b.r[r]);
            int same = ea == eb && a.n == b.n && a.z == b.z && a.c == b.c && a.v == b.v && regs &&
                       memcmp(lifted, buf, BUF) == 0;
            if (!same && bad++ == 0) {
                int n = snprintf(first, sizeof first, "trial %u:", t);
                for (unsigned r = 0; r < 16 && n < (int)sizeof first; r++)
                    if (!same_reg(a.r[r], b.r[r]))
                        n += snprintf(first + n, sizeof first - n, " r%u &%08X, tier 0 &%08X", r,
                                      a.r[r], b.r[r]);
                if (a.n != b.n || a.z != b.z || a.c != b.c || a.v != b.v)
                    n += snprintf(first + n, sizeof first - n, " NZCV %u%u%u%u, tier 0 %u%u%u%u",
                                  a.n, a.z, a.c, a.v, b.n, b.z, b.c, b.v);
                if (memcmp(lifted, buf, BUF) != 0)
                    n += snprintf(first + n, sizeof first - n, " memory differs");
                if (ea != eb)
                    snprintf(first + n, sizeof first - n, " raised &%X, tier 0 &%X", ea, eb);
            }
        }
        char what[96];
        snprintf(what, sizeof what, "%s: %u random inputs, lifted as tier 0 computes",
                 routines[f].name, TRIALS);
        check(bad == 0, what, "%u differ; first, %s", bad, first);
    }
}
