/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's kernel source
 * (Sources/Kernel: s.HeapSort, s.MoreSWIs).
 */

/* heapsort.c: OS_HeapSort, OS_HeapSort32 and OS_CRC, reimplemented.
 *
 * Written from the kernel's Kernel/s/HeapSort and Kernel/s/MoreSWIs. The
 * sort is Knuth's algorithm H as the kernel has it, step for step. A heap
 * sort is not stable, so which of two equal elements lands first is the
 * algorithm's choice. A program may depend on it. The Wimp sorts its sprite
 * pool's names and its tasks' message lists this way.
 *
 * OS_HeapSort32 takes R0 = n and R1 pointing to an array of n words.
 * R2 selects the comparison:
 *   0  cardinals
 *   1  integers
 *   2 and 3  pointers to cardinals and to integers
 *   4  pointers to strings, case insensitive
 *   5  pointers to strings, case sensitive
 *   or the address of a procedure.
 * The procedure is entered with R0 and R1 holding the two words and
 * R12 = R3. It returns LT (N != V) if the first sorts before the second.
 * R7 holds flags. Bit 30 builds the array first, of pointers to R0 blocks of
 * R5 bytes starting at R4. Bit 31 afterwards puts the blocks themselves in
 * the order the pointers are in. It does this through a temporary slot, which
 * is ScratchSpace, or R6 when bit 29 is set or a block is larger than
 * ScratchSpace. OS_HeapSort is the old form. R1's top three bits are the
 * flags, and bit 30 implies bit 31. Every register comes back as it went in.
 */
#include <ctype.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"

#define ERR_BAD_PARAMETERS 0x1EAu

/* One comparison: whether a sorts before b. */
static int before(struct ros_cpu *s, uint32_t proc, uint32_t wp, uint32_t a, uint32_t b)
{
    switch (proc) {
    case 2:
        a = ros_ld32(a), b = ros_ld32(b);
        /* fall through */
    case 0:
        return a < b;
    case 3:
        a = ros_ld32(a), b = ros_ld32(b);
        /* fall through */
    case 1:
        return (int32_t)a < (int32_t)b;
    case 4:
    case 5:
        for (;; a++, b++) {
            uint32_t x = ros_ld8(a), y = ros_ld8(b);
            if (proc == 4) {                    /* the kernel's LowerCase: A-Z only */
                if (x >= 'A' && x <= 'Z')
                    x += 32;
                if (y >= 'A' && y <= 'Z')
                    y += 32;
            }
            if (x != y)
                return x < y;
            if (x <= ' ' - 1)
                return 0;                       /* equal: GE */
        }
    default: {                                  /* the caller's procedure */
        struct ros_cpu c = *s;
        c.r[0] = a;
        c.r[1] = b;
        c.r[12] = wp;
        c.r[14] = ROS_RETURN_TO_NATIVE;
        ros_call(&c, proc);
        if (c.r[15] != ROS_RETURN_TO_NATIVE)
            ros_bad_return(&c, ROS_RETURN_TO_NATIVE);
        return c.n != c.v;
    }
    }
}

static void copy(uint32_t to, uint32_t from, uint32_t n)
{
    memmove(ros_ptr(to), ros_ptr(from), n);
}

void ros_thunk_OS_HeapSort32(struct ros_cpu *s)
{
    uint32_t n = s->r[0], list = s->r[1], proc = s->r[2], wp = s->r[3];
    uint32_t blocks = s->r[4], size = s->r[5], temp = s->r[6], flags = s->r[7];
    s->v = 0;
    if (n < 2)
        return;
    if (flags & (1u << 30))                     /* build the array of pointers */
        for (uint32_t i = 0; i < n; i++)
            ros_st32(list + 4 * i, blocks + i * size);

    /* Algorithm H, R(1..n) at list - 4 */
    uint32_t base = list - 4;
#define R(i) ros_ld32(base + 4 * (i))
#define SET(i, v) ros_st32(base + 4 * (i), (v))
    uint32_t l = n / 2 + 1, r = n, K, Rv, i, j;
    for (;;) {
        if (l != 1) {                           /* H2 */
            l--;
            Rv = R(l);
            K = Rv;
        } else {
            Rv = R(r);
            K = Rv;
            SET(r, R(1));
            r--;
            if (r == 1)
                SET(1, Rv);
        }
        if (r == 1)
            break;
        j = l;                                  /* H3 */
        for (;;) {
            i = j;                              /* H4 */
            j <<= 1;
            if (j > r)
                break;                          /* to H8 */
            if (j < r && before(s, proc, wp, R(j), R(j + 1)))
                j++;                            /* H5 */
            if (!before(s, proc, wp, K, R(j)))
                break;                          /* H6: to H8 */
            SET(i, R(j));                       /* H7 */
        }
        SET(i, Rv);                             /* H8 */
    }
#undef R
#undef SET

    if (!(flags & (1u << 31)))
        return;
    /* Put the blocks in the pointers' order, one cycle at a time. Each
     * pointer is cleared when it has been dealt with. */
    uint32_t slot = (flags & (1u << 29)) || size > ROS_SCRATCH_SIZE ? temp : ROS_SCRATCH_BASE;
    uint32_t end = list + 4 * n;
    for (uint32_t first = list; first < end; ) {
        uint32_t cur = blocks + (first - list) / 4 * size, item = first;
        copy(slot, cur, size);
        for (;;) {
            uint32_t next = ros_ld32(item);
            ros_st32(item, 0);
            item = list + (next - blocks) / size * 4;
            if (item == first) {
                copy(cur, slot, size);
                break;
            }
            copy(cur, next, size);
            cur = next;
        }
        do
            first += 4;
        while (first < end && ros_ld32(first) == 0);
    }
}

void ros_thunk_OS_HeapSort(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1], r7 = s->r[7];
    uint32_t flags = r1 & (3u << 29);
    if (flags & (1u << 30))
        flags |= 1u << 31;
    s->r[1] = r1 & ~(7u << 29);
    s->r[7] = flags;
    ros_thunk_OS_HeapSort32(s);
    s->r[1] = r1;
    s->r[7] = r7;
}

/* OS_CRC: R0 is the CRC so far, R1 the start, R2 the end and R3 the step.
 * The step may be negative. The end must be met exactly. This is the
 * kernel's CRC-16 with pattern &A001, taken two bytes at a time. */
void ros_thunk_OS_CRC(struct ros_cpu *s)
{
    uint32_t crc = s->r[0], a = s->r[1], end = s->r[2], step = s->r[3];
    if (step == 0) {
        ros_swi_fail(s, ros_error(ERR_BAD_PARAMETERS, "Parameters not recognised"));
        return;
    }
    while (a != end) {
        crc ^= ros_ld8(a);
        a += step;
        unsigned bits = 8;
        if (a != end) {
            crc ^= ros_ld8(a) << 8;
            a += step;
            bits = 16;
        }
        while (bits--)
            crc = crc & 1 ? (crc >> 1) ^ 0xA001u : crc >> 1;
    }
    s->r[0] = crc;
    s->v = 0;
}
