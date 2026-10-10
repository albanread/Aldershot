/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* rma.c -- the relocatable module area: a RISC OS heap (heap.c).
 *
 * The RMA is a heap in the kernel's format, so OS_Heap works on it and
 * OS_Module 5 describes it as it always has. Its descriptor sits 12 bytes
 * in, so the first block's data is 32-aligned. Blocks claimed as the kernel
 * claims them, 32n bytes with the header included, keep every block's data
 * on a 32-byte boundary. Native code's blocks are claimed aligned to 8, so
 * 64-bit values may live in them.
 *
 * The heap ends where the RMA does, as the kernel's does. Dynamic area 1's
 * size (runtime/dynarea.c) is the heap's end, and it is a page to start
 * with. A claim that finds no room grows the heap and is tried again. The
 * growth is the kernel's DoRMAHeapOpWithExtension: OS_ChangeDynamicArea on
 * area 1 by what is missing at its top, plus 8. So OS_Module 5 describes
 * the free space inside the RMA as it is (the Task Manager's "Free in
 * Module area"), not the 256 MB of address space it may grow into. Memory
 * past the heap's base is never touched, so pages are supplied only as the
 * heap grows into them.
 */
#include "rosgd/arena.h"
#include "rosgd/dynarea.h"
#include "rosgd/heap.h"
#include "rosgd/rma.h"

#define HPD (ROS_RMA_BASE + 12)
#define PAGE 4096u

uint32_t ros_rma_heap(void)
{
    if (ros_ld32(HPD) != 0x70616548u)
        ros_heap_init(HPD, PAGE - 12);
    return HPD;
}

uint32_t ros_rma_size(void)
{
    return ros_ld32(ros_rma_heap() + 12) + 12;
}

int ros_rma_grow(uint32_t need)
{
    uint32_t hpd = ros_rma_heap();
    uint32_t base = ros_ld32(hpd + 8), end = ros_ld32(hpd + 12);
    uint32_t more = (need > end - base ? need - (end - base) : 0) + 8;
    uint32_t size = end + 12;
    if (more > ROS_RMA_SIZE - size)
        return -1;
    uint32_t want = (size + more + PAGE - 1) & ~(PAGE - 1);
    if (want > ROS_RMA_SIZE || want - size > ros_freepool_bytes())
        return -1;
    ros_st32(hpd + 12, want - 12);
    return 0;
}

void *ros_rma_alloc(uint32_t size)
{
    uint32_t a;
    if (!size)
        size = 1;
    if (ros_heap_get_aligned(ros_rma_heap(), size, 8, 0, HPD, &a) &&
        (ros_rma_grow(size + 8 + 8) ||
         ros_heap_get_aligned(ros_rma_heap(), size, 8, 0, HPD, &a)))
        return NULL;
    return ros_ptr(a);
}

int ros_rma_free(void *p)
{
    if (!ros_in_arena(p))
        return -1;
    return ros_heap_free(ros_rma_heap(), ros_addr(p)) ? -1 : 0;
}

void ros_rma_stats(uint32_t *used, uint32_t *free_bytes)
{
    uint32_t largest, total;
    uint32_t hpd = ros_rma_heap();
    ros_heap_describe(hpd, &largest, &total);
    *free_bytes = total;
    *used = ros_ld32(hpd + 12) - 16 - total;
}
