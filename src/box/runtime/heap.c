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
 * (Sources/Kernel: s.HeapMan, s.ArthurSWIs).
 */

/* heap.c: OS_Heap, reimplemented.
 *
 * Written from the kernel's heap manager (Kernel/s/HeapMan). A heap has the
 * kernel's layout, byte for byte, because programs make their own heaps in
 * their own memory and some read them. The layout is:
 *
 *   - A 16-byte descriptor. It holds "Heap", then the offsets of the first
 *     free block, of the first byte never used (the base), and of the end.
 *   - Each block holds its size, header word included, then its data.
 *   - A free block holds its link and its size. The link is the offset from
 *     this block to the next free block, or 0 at the end. The free list is
 *     in address order.
 *
 * The algorithms are the kernel's too, so blocks land where RISC OS puts
 * them.
 *   - Allocation is first fit. It splits a free block from its end. It
 *     takes a leftover of 8 bytes or less into the allocation.
 *   - Frees join their neighbours and the base.
 *   - ExtendBlock grows into the block after, then into the one before. It
 *     moves the block only when neither will do.
 *   - Aligned claims follow the kernel's rules for the free blocks they
 *     leave.
 * The errors have the kernel's numbers and texts.
 *
 * The RMA is such a heap (rma.c), so OS_Heap on it works as it did.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"

#define MAGIC 0x70616548u           /* "Heap" */
#define H_FREE 4u
#define H_BASE 8u
#define H_END 12u
#define H_SIZE 16u
#define FRE_BLKSIZE 8u
#define MIN_FRAG 8u

#define ERR_BAD_REASON 0x180u
#define ERR_INIT       0x181u
#define ERR_BAD_DESC   0x182u
#define ERR_BAD_LINK   0x183u
#define ERR_ALLOC      0x184u
#define ERR_NOT_BLOCK  0x185u
#define ERR_BAD_EXTEND 0x186u
#define ERR_SHRINK     0x187u

static os_error *err(uint32_t n)
{
    switch (n) {
    case ERR_BAD_REASON: return ros_error(n, "Bad reason code");
    case ERR_INIT: return ros_error(n, "Can't initialise heap");
    case ERR_BAD_DESC: return ros_error(n, "Bad heap descriptor");
    case ERR_BAD_LINK: return ros_error(n, "Heap corrupted");
    case ERR_ALLOC: return ros_error(n, "Not enough memory (in heap)");
    case ERR_NOT_BLOCK: return ros_error(n, "Not a heap block");
    case ERR_BAD_EXTEND: return ros_error(n, "Invalid heap extension");
    default: return ros_error(n, "Can't shrink heap any further");
    }
}

static uint32_t L(uint32_t a) { return ros_ld32(a); }
static void S(uint32_t a, uint32_t v) { ros_st32(a, v); }

/* The kernel's OS_ValidateAddress, for the regions the arena maps. These
 * include the part of a dynamic area in use. An example is the system sprite
 * area, which *SLoad and *SSave read and write through FileSwitch. They also
 * include the screen, as much as the mode maps, which a program may save
 * with OS_File 10. */
int ros_arena_valid(uint32_t start, uint32_t end)
{
    static const uint32_t fixed[][2] = {
        { ROS_RMA_BASE, ROS_RMA_BASE + ROS_RMA_SIZE },
        { ROS_SYSHEAP_BASE, ROS_SYSHEAP_BASE + ROS_SYSHEAP_SIZE },
        /* the SVC stack less its guard page (arena.h; hosted, none) */
        { ROS_SVCSTACK_BASE, ROS_SVCSTACK_GUARD ? ROS_SVCSTACK_GUARD_AT : ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE },
        { ROS_SVCSTACK_GUARD_AT + ROS_SVCSTACK_GUARD, ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE },
        { ROS_ZEROPAGE, ROS_ZEROPAGE + ROS_ZEROPAGE_SIZE - 1 },
    };
    if (end < start)
        return 0;
    uint32_t slot = ros_slot_current ? ros_slot_current->size : 0;
    if (start >= ROS_APP_BASE && end <= ROS_APP_BASE + slot)   /* not the stack's guard */
        return !ros_slot_guard_overlaps(start, end);
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (start >= fixed[i][0] && end <= fixed[i][1])
            return 1;
    if (start >= ROS_SCREEN_BASE && end <= ROS_SCREEN_BASE + ros_vdu_screen_size())
        return 1;
    return ros_dynarea_contains(start, end);
}

int ros_arena_readable(uint32_t start, uint32_t end)
{
    if (ros_arena_valid(start, end))
        return 1;
    return end >= start && start >= ROS_ROM_BASE && end <= ROS_ROM_BASE + ROS_ROM_SIZE;
}

/* OS_ValidateAddress, as the kernel's (Kernel/s/ArthurSWIs). The range
 * [start, end) is valid if it is wholly accessible. That covers the slot, the
 * heaps, the SVC stack, zero page, the ROM, ScratchSpace and the part of a
 * dynamic area in use. It is also valid if it is screen memory. Otherwise it
 * is valid if a module claims Service_ValidateAddress for it. The Wimp checks
 * every window handle this way. */
os_error *xos_validate_address(uint32_t start, uint32_t end, int *invalid)
{
    int ok = ros_arena_readable(start, end) || ros_dynarea_contains(start, end) ||
             (start >= ROS_SCRATCH_BASE && end <= ROS_SCRATCH_BASE + ROS_SCRATCH_SIZE && end >= start) ||
             (start >= ROS_SCREEN_BASE && end <= ROS_SCREEN_BASE + ros_vdu_screen_size() && end >= start);
    if (!ok) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = 0x6D;                          /* Service_ValidateAddress */
        s.r[2] = start;
        s.r[3] = end;
        ros_service_call(&s);
        ok = s.r[1] == 0;
    }
    *invalid = !ok;
    return NULL;
}

static int valid_hpd(uint32_t hpd)
{
    if ((hpd & 3) || !ros_arena_valid(hpd, hpd + H_SIZE + FRE_BLKSIZE) || L(hpd) != MAGIC)
        return 0;
    return ros_arena_valid(hpd, hpd + L(hpd + H_END));
}

/* ---- the reasons ---- */

os_error *ros_heap_init(uint32_t hpd, uint32_t size)
{
    if ((int32_t)size < (int32_t)(H_SIZE + FRE_BLKSIZE) || !ros_arena_valid(hpd, hpd + size))
        return err(ERR_INIT);
    S(hpd, MAGIC);
    S(hpd + H_FREE, 0);
    S(hpd + H_BASE, H_SIZE);
    S(hpd + H_END, size);
    return NULL;
}

os_error *ros_heap_describe(uint32_t hpd, uint32_t *largest, uint32_t *total)
{
    if (!valid_hpd(hpd))
        return err(ERR_BAD_DESC);
    uint32_t base = L(hpd + H_BASE), max = L(hpd + H_END) - base, sum = max;
    uint32_t limit = hpd + base, tp = hpd + H_FREE, bp = L(tp);
    while (bp) {
        if ((int32_t)bp < 0)
            return err(ERR_BAD_LINK);
        tp += bp;
        if (tp >= limit)
            return err(ERR_BAD_LINK);
        uint32_t s = L(tp + 4);
        if (s > max)
            max = s;
        sum += s;
        bp = L(tp);
    }
    if ((int32_t)max > 0)
        max -= 4;
    *largest = max, *total = sum;
    return NULL;
}

os_error *ros_heap_get(uint32_t hpd, uint32_t size, uint32_t *addr)
{
    *addr = 0;
    if (!valid_hpd(hpd))
        return err(ERR_BAD_DESC);
    if ((int32_t)size <= 0)
        return err(ERR_ALLOC);
    uint32_t need = (size + 3 + 4) & ~3u, prev = hpd + H_FREE, blk;
    for (uint32_t tp; (tp = L(prev)) != 0; prev = blk) {
        blk = prev + tp;
        uint32_t fsize = L(blk + 4);
        if (fsize < need)
            continue;
        uint32_t left = fsize - need;
        if (left != 0 && left < MIN_FRAG + 1) {
            need += left;               /* no silly little blocks */
            left = 0;
        }
        uint32_t result;
        if (left >= FRE_BLKSIZE) {      /* split: ours is the end part */
            S(blk + 4, left);
            result = blk + left;
        } else {
            need += left;
            uint32_t next = L(blk);
            S(prev, next ? next + tp : 0);
            result = blk;
        }
        S(result, need);
        *addr = result + 4;
        return NULL;
    }
    uint32_t base = L(hpd + H_BASE), top = base + need;
    if (top > L(hpd + H_END))
        return err(ERR_ALLOC);
    S(hpd + H_BASE, top);
    S(hpd + base, need);
    *addr = hpd + base + 4;
    return NULL;
}

/* Find an allocated block, as an offset. Also find the free entry before it
 * (tp, which is 4 for none) and the one before that (work). */
static os_error *find_block(uint32_t hpd, uint32_t addr, uint32_t *a_out, uint32_t *tp_out,
                            uint32_t *work_out)
{
    if (!valid_hpd(hpd))
        return err(ERR_BAD_DESC);
    uint32_t a = addr - hpd - 4, tp = H_FREE, work = 0, after, link;
    for (;;) {
        link = L(hpd + tp);
        if (!link) {
            after = L(hpd + H_BASE);
            break;
        }
        uint32_t next = link + tp;
        if (next > a) {
            after = next;
            break;
        }
        work = tp;
        tp = next;
    }
    uint32_t bp = tp == H_FREE ? H_SIZE : tp + L(hpd + tp + 4);
    while ((int32_t)bp < (int32_t)after) {
        if (bp == a) {
            *a_out = a, *tp_out = tp, *work_out = work;
            return NULL;
        }
        uint32_t s = L(hpd + bp);
        if (s < 4)
            break;                      /* the kernel would loop here: the heap is corrupt */
        bp += s;
    }
    return err(ERR_NOT_BLOCK);
}

/* The kernel's FreeChunkWithConcatenation. */
static void free_chunk(uint32_t hpd, uint32_t a, uint32_t tp, uint32_t work)
{
    uint32_t size = L(hpd + a), eob = a + size;
    uint32_t next = L(hpd + tp) + tp;
    if (next == eob) {                  /* join the free block after */
        S(hpd + a, L(hpd + next + 4) + size);
        uint32_t l = L(hpd + next);
        S(hpd + tp, l ? next + l - tp : 0);
    }
    if (tp != H_FREE) {
        uint32_t psize = L(hpd + tp + 4);
        if (tp + psize == a) {          /* and the one before */
            psize += L(hpd + a);
            S(hpd + tp + 4, psize);
            uint32_t base = L(hpd + H_BASE);
            if (base == tp + psize) {   /* now against the base: give it back */
                S(hpd + H_BASE, base - psize);
                S(hpd + work, 0);
            }
            return;
        }
    }
    size = L(hpd + a);
    uint32_t base = L(hpd + H_BASE);
    if (a + size == base) {
        S(hpd + H_BASE, base - size);
        return;
    }
    S(hpd + a + 4, size);               /* onto the free list, in order */
    uint32_t pl = L(hpd + tp);
    S(hpd + a, pl ? pl - a + tp : 0);
    S(hpd + tp, a - tp);
}

os_error *ros_heap_free(uint32_t hpd, uint32_t addr)
{
    uint32_t a, tp, work;
    os_error *e = find_block(hpd, addr, &a, &tp, &work);
    if (!e)
        free_chunk(hpd, a, tp, work);
    return e;
}

os_error *ros_heap_block_size(uint32_t hpd, uint32_t addr, uint32_t *size)
{
    uint32_t a, tp, work;
    os_error *e = find_block(hpd, addr, &a, &tp, &work);
    if (!e)
        *size = L(hpd + a);
    return e;
}

/* The preceding free block (tp, and before it work) gives up its end. The
 * left bytes stay free. The block moves down to meet them, grown by grow. */
static uint32_t take_preceder(uint32_t hpd, uint32_t a, uint32_t tp, uint32_t work,
                              uint32_t left, uint32_t grow)
{
    if (left >= FRE_BLKSIZE) {
        S(hpd + tp + 4, left);
    } else {
        grow += left;                   /* the rest dies with it */
        left = 0;
        uint32_t l = L(hpd + tp);
        S(hpd + work, l ? l + tp - work : 0);
    }
    uint32_t nb = tp + left, bsize = L(hpd + a);
    S(hpd + nb, bsize + grow);
    memmove(ros_ptr(hpd + nb + 4), ros_ptr(hpd + a + 4), bsize - 4);
    return hpd + nb + 4;
}

os_error *ros_heap_extend_block(uint32_t hpd, uint32_t *addr, int32_t by)
{
    uint32_t a, tp, work;
    os_error *e = find_block(hpd, *addr, &a, &tp, &work);
    if (e)
        return e;
    uint32_t d = ((uint32_t)by + 3) & ~3u;
    if (d == 0)
        return NULL;
    if ((int32_t)d < 0) {               /* shrink */
        uint32_t s = 0u - d, bsize = L(hpd + a), left = bsize - s;
        if ((int32_t)left <= 4) {
            *addr = 0xFFFFFFFFu;        /* the block is gone */
            free_chunk(hpd, a, tp, work);
            return NULL;
        }
        if (s <= 4) {                   /* 4 bytes: allowed only against free space */
            uint32_t nf = L(hpd + tp);
            nf = nf ? nf + tp : L(hpd + H_BASE);
            int32_t gap = (int32_t)(nf - a - s);
            if (gap > (int32_t)left)
                return NULL;            /* a used block after: refused quietly */
            if (gap < (int32_t)left)
                return err(ERR_BAD_LINK);
        }
        S(hpd + a, left);
        S(hpd + a + left, s);
        free_chunk(hpd, a + left, tp, work);
        return NULL;
    }

    uint32_t end = a + L(hpd + a), link = L(hpd + tp), base = L(hpd + H_BASE);
    uint32_t bp = link ? link + tp : base;
    uint32_t avail = 0;
    int after = end == bp;
    if (after) {
        avail = bp != base ? L(hpd + bp + 4) : (L(hpd + H_END) - bp) & ~3u;
        if ((int32_t)avail >= (int32_t)d) {
            uint32_t nsize = L(hpd + a) + d;
            S(hpd + a, nsize);
            if (bp == base) {
                S(hpd + H_BASE, a + nsize);
                return NULL;
            }
            uint32_t rem = avail - d;
            if ((int32_t)rem > 4) {     /* the free block after shrinks */
                uint32_t nl = L(hpd + tp) + d;
                S(hpd + tp, nl);
                uint32_t nfb = tp + nl, l = L(hpd + bp);
                S(hpd + nfb + 4, rem);
                S(hpd + nfb, l ? l - d : 0);
            } else {
                if (rem == 4)
                    S(hpd + a, nsize + 4);
                uint32_t l = L(hpd + bp);
                S(hpd + tp, l ? l + bp - tp : 0);
            }
            return NULL;
        }
    }
    /* Try the block before, alone or together with the space after. */
    if (tp != H_FREE && tp + L(hpd + tp + 4) == a) {
        uint32_t need = after ? d - avail : d;
        int32_t left = (int32_t)(L(hpd + tp + 4) - need);
        if (left >= 0) {
            if (after) {                /* take all the space after too */
                if (bp == base)
                    S(hpd + H_BASE, bp + avail);
                else {
                    uint32_t l = L(hpd + bp);
                    S(hpd + tp, l ? l + bp - tp : 0);
                }
            }
            *addr = take_preceder(hpd, a, tp, work, (uint32_t)left, d);
            return NULL;
        }
    }
    /* Neither will do. Allocate a new block, copy the data and free the old
     * one. */
    uint32_t bsize = L(hpd + a), fresh;
    if ((e = ros_heap_get(hpd, bsize + d - 4, &fresh)))
        return e;
    memmove(ros_ptr(fresh), ros_ptr(*addr), bsize - 4);
    ros_heap_free(hpd, *addr);
    *addr = fresh;
    return NULL;
}

os_error *ros_heap_extend_heap(uint32_t hpd, int32_t by, uint32_t *changed)
{
    *changed = 0;
    if (!valid_hpd(hpd))
        return err(ERR_BAD_DESC);
    if (by < 0)
        by += 3;
    uint32_t d = (uint32_t)by & ~3u, end = L(hpd + H_END), nend = end + d;
    uint32_t base = L(hpd + H_BASE);
    if ((int32_t)base > (int32_t)nend) {
        S(hpd + H_END, base);           /* as far as it will go */
        *changed = end - base;
        return err(ERR_SHRINK);
    }
    if (!ros_arena_valid(hpd, hpd + nend))
        return err(ERR_BAD_EXTEND);
    S(hpd + H_END, nend);
    *changed = d;
    return NULL;
}

/* The kernel's GetAreaAligned. The data is aligned to align, which is a
 * power of 2 and at least 4. If boundary is not 0, the data lies within one
 * boundary. The skew moves the point that the alignment is measured from
 * (GetSkewAligned). */
os_error *ros_heap_get_aligned(uint32_t hpd, uint32_t size, uint32_t align, uint32_t boundary,
                               uint32_t skew, uint32_t *addr)
{
    *addr = 0;
    if (!valid_hpd(hpd))
        return err(ERR_BAD_DESC);
    if ((int32_t)size <= 0)
        return err(ERR_ALLOC);
    size = (size + 3) & ~3u;
    uint32_t bp = align - 1;
    if (bp & align)
        return err(ERR_ALLOC);
    if ((int32_t)bp < 3)
        bp = 3;
    uint32_t rel = skew - hpd;
    if (rel & 3)
        return err(ERR_ALLOC);
    uint32_t r0 = boundary - 1;
    if (r0 & boundary)
        return err(ERR_ALLOC);
    if (r0 != 0xFFFFFFFFu && (r0 < bp || boundary < size))
        return err(ERR_ALLOC);

    uint32_t prev = hpd + H_FREE;
    for (uint32_t tp; (tp = L(prev)) != 0;) {
        uint32_t blk = prev + tp, fend = L(blk + 4) + blk;
        uint32_t w = blk + 4 + bp + rel, ba = blk + rel;
        for (;;) {
            w &= ~bp;
            uint32_t g = w - ba;
            if (g != 4 && (int32_t)g < (int32_t)(FRE_BLKSIZE + 4)) {
                w += bp << 1;           /* no room for a free block before */
                continue;
            }
            uint32_t wl = w - rel;
            if (fend < wl + size)
                goto next_block;
            if (r0 != 0xFFFFFFFFu && (w & r0) + size - 1 > r0) {
                w = (w + r0) & ~r0;     /* across a boundary: to the next */
                continue;
            }
            w = wl;
            break;
        }
        {
            uint32_t bsize = size + 4, start = w - 4;
            uint32_t gap = fend - (start + bsize);
            if ((int32_t)gap < (int32_t)FRE_BLKSIZE) {
                bsize += gap;
            } else {                    /* a free block after ours */
                uint32_t nb = fend - gap, l = L(blk);
                S(nb + 4, gap);
                S(nb, l ? l + blk - nb : 0);
                S(blk, nb - blk);
            }
            uint32_t pre = start - blk;
            if (pre) {
                S(blk + 4, pre);
            } else {
                uint32_t l = L(blk);
                S(blk - tp, l ? l + tp : 0);
            }
            S(start, bsize);
            *addr = start + 4;
            return NULL;
        }
    next_block:
        prev = blk;
    }

    uint32_t w = hpd + L(hpd + H_BASE) + rel, t = w + 4 + bp;
    for (;;) {
        t &= ~bp;
        uint32_t g = t - w;
        if (g != 4 && (int32_t)g < (int32_t)(FRE_BLKSIZE + 4)) {
            t += bp << 1;
            continue;
        }
        if (r0 != 0xFFFFFFFFu && (t & r0) + size - 1 > r0) {
            t = (t + r0) & ~r0;
            continue;
        }
        break;
    }
    t -= rel;
    w -= rel;
    uint32_t top = t + size - hpd;
    if ((int32_t)top > (int32_t)L(hpd + H_END))
        return err(ERR_ALLOC);
    S(t - 4, size + 4);
    S(hpd + H_BASE, top);
    uint32_t start = t - 4, pre = start - w;
    if (pre) {                          /* the gap before is a free block */
        S(w + 4, pre);
        S(w, 0);
        S(prev, w - prev);
    }
    *addr = t;
    return NULL;
}

/* ---- the SWI ---- */

void ros_thunk_OS_Heap(struct ros_cpu *s)
{
    uint32_t hpd = s->r[1];
    os_error *e = NULL;
    switch (s->r[0]) {
    case 0:
        e = ros_heap_init(hpd, s->r[3]);
        break;
    case 1:
        e = ros_heap_describe(hpd, &s->r[2], &s->r[3]);
        break;
    case 2:
        e = ros_heap_get(hpd, s->r[3], &s->r[2]);
        break;
    case 3:
        e = ros_heap_free(hpd, s->r[2]);
        break;
    case 4:
        e = ros_heap_extend_block(hpd, &s->r[2], (int32_t)s->r[3]);
        break;
    case 5:
        e = ros_heap_extend_heap(hpd, (int32_t)s->r[3], &s->r[3]);
        break;
    case 6:
        e = ros_heap_block_size(hpd, s->r[2], &s->r[3]);
        break;
    case 7:
    case 8:
        e = ros_heap_get_aligned(hpd, s->r[3], s->r[2], s->r[4], s->r[0] == 8 ? s->r[5] : hpd,
                                 &s->r[2]);
        break;
    default:
        e = err(ERR_BAD_REASON);
        break;
    }
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}
