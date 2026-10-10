/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2016 Castle Technology Ltd
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
 * (Sources/Kernel: s.AMBControl.AMB, s.AMBControl.allocate,
 * s.AMBControl.deallocate, s.AMBControl.growshrink, s.AMBControl.mapslot,
 * s.AMBControl.mapsome, s.AMBControl.readinfo, s.AMBControl.handler).
 */

/* amb.c: OS_AMBControl: application memory blocks, the slots the Wimp
 * gives its tasks (Kernel/s/AMBControl), implemented natively over the
 * arena's slots.
 *
 * A node is a memfd (arena.h, "application slots"). Mapping one in is an
 * mmap at &8000. In AMB, mapslot rewrote the page table. Linux's demand
 * paging does what AMB's lazy map-in did by hand. The reasons are:
 *
 *   0  allocate. This makes a node of R1 pages, which is then the one
 *      mapped in. On exit R2 is its handle and R1 the pages it has.
 *      Suppose no pages are asked for and none is mapped, while the
 *      program that was running when the Wimp started still has its own
 *      application space (task.h). Then the node is that space, taken over
 *      in place. The Wimp gives a task a slot this way (getnullslot, then a
 *      grow) when its slot size is set. It so adopts that program's memory
 *      as its slot, and nothing is copied.
 *   1  deallocate R2. If it was mapped in, nothing is mapped afterwards.
 *   2  grow or shrink R2 to R1 pages. On exit R1 is the pages it has and R3
 *      the pages it had. At none pages it is freed and R2 is 0.
 *   3  map R2 in at &8000 (R1 0 or &8000) or out (R1 -1). With bit 8
 *      (mapsome, Wimp_TransferBlock's mapenoughslot), map R4 pages of it,
 *      from page R3, at R1. Or map those pages out with R1 -1.
 *   4  read R2. On exit R1 is where it is mapped (-1 if out) and R3 its
 *      pages.
 *   5  lazy map-in. R1 is 1 for on, 0 for off and -1 to read. On exit R1 is
 *      the setting. Linux pages on demand whatever it says.
 *   9  On exit R2 is the handle mapped in (0 if none) and R3 how many nodes
 *      there are.
 *
 * Mapping a node in or out, or resizing the mapped one, sets AplWorkSize and
 * MemLimit in zero page to the top of it. The top is &8000 when none is
 * mapped. The kernel's mapslot, growp and shrinkp do the same. Handles come
 * off a free list as the kernel's do. They are 1, 2, 3 at first, and the
 * last one freed comes next.
 *
 * RISC OS 5.30 does not check a bad handle or a reason code past 9, and it
 * crashes. ValidateAMBHandles is off, and the dispatch's range check
 * compares its labels the wrong way round. Here those are errors:
 * "AMBControl bad handle" and "bad AMBControl reason code".
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/dynarea.h"
#include "rosgd/error.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"

#define PAGE          4096u
#define NODES         256u                  /* AMBInitialMaxNodes: handles 1-255 */
#define ZP_MEMLIMIT   0x11Cu                /* KernelWS MemLimit */
#define ZP_APLWORK    0x368u                /* KernelWS AplWorkSize */
#define ERR_BAD_HANDLE 0x1F8u               /* ErrorNumber_AMBBadHandle */
#define ERR_BAD_REASON 0x1F9u               /* ErrorNumber_AMBBadReason */
#define MAPSOME       0x100u                /* AMBControl_MapSlot_MapSome */
#define PARTS         4u                    /* mapsome ranges a node keeps */

struct node {
    int used;
    int adopted;                    /* the first program's own memory (task.h) */
    struct ros_slot slot;
    struct { uint32_t addr, size; } part[PARTS];   /* mapsome ranges, to map out */
};

static struct node nodes[NODES];
static uint32_t free_next[NODES];           /* [0] is the head, as in the kernel */
static uint32_t mapped;                     /* the handle mapped in whole */
static uint32_t count;
static int lazy = 1;

static void app_top(uint32_t end)
{
    ros_st32(ROS_ZEROPAGE + ZP_APLWORK, end);
    ros_st32(ROS_ZEROPAGE + ZP_MEMLIMIT, end);
}

static void init(void)
{
    if (free_next[0])
        return;
    for (uint32_t h = 0; h < NODES - 1; h++)
        free_next[h] = h + 1;
}

static struct node *node(uint32_t h)
{
    return h >= 1 && h < NODES && nodes[h].used ? &nodes[h] : NULL;
}

static uint32_t pages(const struct node *n)
{
    return n->slot.size / PAGE;
}

static os_error *bad_handle(void)
{
    return ros_error(ERR_BAD_HANDLE, "AMBControl bad handle");
}

static void map_out(void)
{
    ros_slot_unmap();
    mapped = 0;
    app_top(ROS_APP_BASE);
}

static os_error *map_in(uint32_t h)
{
    struct node *n = &nodes[h];
    if (n->slot.size ? ros_slot_map(&n->slot) != 0 : (ros_slot_unmap(), 0))
        return ros_error(ERR_BAD_HANDLE, "AMBControl cannot map the slot");
    mapped = h;
    app_top(ROS_APP_BASE + n->slot.size);
    return NULL;
}

static uint32_t most_pages(void)
{
    return (ROS_APP_LIMIT - ROS_APP_BASE) / PAGE;
}

static void drop_parts(struct node *n)
{
    for (unsigned i = 0; i < PARTS; i++)
        if (n->part[i].size) {
            ros_arena_unmap(n->part[i].addr, n->part[i].size);
            n->part[i].size = 0;
        }
}

/* Freeing a node frees its memory, except for the adopted program's. That
 * goes back to being the program's application space. On RISC OS it was
 * never the Wimp's to free. The last task's closedown frees every slot while
 * the program that started the Wimp runs on (BASIC after its
 * Wimp_CloseDown). */
static void deallocate(uint32_t h)
{
    struct node *n = &nodes[h];
    drop_parts(n);
    if (n->adopted) {
        int was_mapped = mapped == h;
        const struct ros_slot *own = ros_task_restore_adopted(&n->slot);
        if (was_mapped) {
            mapped = 0;
            if (ros_slot_map(own) == 0)
                app_top(ROS_APP_BASE + own->size);
        }
        n->slot = (struct ros_slot){ -1, 0 };
    } else {
        if (mapped == h)
            map_out();
        ros_slot_destroy(&n->slot);
    }
    memset(n, 0, sizeof *n);
    free_next[h] = free_next[0];
    free_next[0] = h;
    count--;
}

/* How many pages a node may grow to. The kernel's growpages takes pages
 * from the free pool until there are none. A node here is lazy (arena.h).
 * Its pages are taken as they are touched, so it may be as big as the map
 * allows (the caller's most_pages), whatever is free. */
static uint32_t can_have(uint32_t want, uint32_t was)
{
    (void)was;
    return want;
}

uint32_t ros_amb_bytes(void)
{
    uint32_t n = 0;
    for (uint32_t h = 1; h < NODES; h++)
        if (nodes[h].used)
            n += ros_slot_used(&nodes[h].slot);
    return n;
}

int ros_amb_mapped(void)
{
    return mapped != 0;
}

int ros_amb_resize_mapped(uint32_t bytes)
{
    struct node *n = node(mapped);
    if (!n)
        return -1;
    if (bytes == n->slot.size)
        return 0;
    uint32_t was = n->slot.size;
    ros_slot_unmap();               /* all of it, at the size it was mapped */
    if (ros_slot_resize(&n->slot, bytes) != 0) {
        map_in(mapped);
        return -1;
    }
    ros_dynarea_pmp_moved((int32_t)bytes - (int32_t)was);
    return map_in(mapped) ? -1 : 0;
}

void ros_thunk_OS_AMBControl(struct ros_cpu *s)
{
    uint32_t reason = s->r[0] & 0xFF, h = s->r[2];
    os_error *e = NULL;
    struct node *n;
    s->v = 0;
    init();
    switch (reason) {
    case 0: {                                       /* allocate */
        uint32_t want = can_have(s->r[1] > most_pages() ? most_pages() : s->r[1], 0);
        uint32_t got = free_next[0];
        if (!got || got >= NODES) {
            e = ros_error(0x1F7, "AMBControl handles exhausted");
            break;
        }
        n = &nodes[got];
        if (s->r[1] && want == 0) {             /* pool empty: handle 0, no error */
            s->r[1] = 0, s->r[2] = 0;           /* (AMBControl allocate, alloc_zeropages) */
            break;
        }
        if (want == 0 && !mapped && ros_task_take_adopted(&n->slot)) {
            want = n->slot.size / PAGE;         /* adopted, in place */
            n->adopted = 1;
        } else if (ros_slot_create(&n->slot, want * PAGE) != 0) {
            e = ros_error(0x1F7, "Not enough memory for an application slot");
            break;
        }
        free_next[0] = free_next[got];
        n->used = 1;
        count++;
        if ((e = map_in(got)))
            break;
        s->r[1] = want, s->r[2] = got;
        break;
    }
    case 1:                                         /* deallocate */
        if (!node(h)) {
            e = bad_handle();
            break;
        }
        deallocate(h);
        break;
    case 2: {                                       /* grow or shrink */
        if (!(n = node(h))) {
            e = bad_handle();
            break;
        }
        uint32_t was = pages(n);
        uint32_t want = can_have(s->r[1] > most_pages() ? most_pages() : s->r[1], was);
        s->r[3] = was;
        if (want == was) {
            s->r[1] = was;
            break;
        }
        if (want == 0 && ros_slot_window_floor(&n->slot)) {
            s->r[1] = was;                  /* shared (arena.h): not shrunk away */
            break;
        }
        if (want == 0) {
            deallocate(h);
            s->r[1] = 0, s->r[2] = 0;
            break;
        }
        if (ros_slot_resize(&n->slot, want * PAGE) != 0) {
            s->r[1] = was;
            break;
        }
        ros_dynarea_pmp_moved((int32_t)(want - was) * (int32_t)PAGE);
        /* Map the mapped node again at its new size. A mapping keeps the
         * length it was made with, and on macOS the slot is a new object.
         * A node the Wimp made empty (getnullslot) and then grew had no
         * mapping at all. */
        if (mapped == h && (e = map_in(h)))
            break;
        s->r[1] = want;
        break;
    }
    case 3:                                         /* map a slot, or some of one */
        if (!(n = node(h))) {
            e = bad_handle();
            break;
        }
        if (s->r[0] & MAPSOME) {
            uint32_t at = s->r[1] ? s->r[1] : ROS_APP_BASE, from = s->r[3], many = s->r[4];
            if (from + many > pages(n) || from + many < from) {
                s->v = 1;                           /* no error block: the Wimp reports it */
                break;
            }
            if (s->r[1] == 0xFFFFFFFFu) {
                for (unsigned i = 0; i < PARTS; i++)
                    if (n->part[i].size) {
                        ros_arena_unmap(n->part[i].addr, n->part[i].size);
                        n->part[i].size = 0;
                    }
                break;
            }
            unsigned i = 0;
            while (i < PARTS - 1 && n->part[i].size)
                i++;
            if (n->part[i].size)
                ros_arena_unmap(n->part[i].addr, n->part[i].size);
            if (many && ros_arena_map_fd(at, many * PAGE, n->slot.fd, (uint64_t)from * PAGE) != 0) {
                s->v = 1;
                break;
            }
            n->part[i].addr = at, n->part[i].size = many * PAGE;
            break;
        }
        if (s->r[1] == 0xFFFFFFFFu) {
            if (mapped == h)
                map_out();
        } else if (mapped != h) {
            e = map_in(h);
        }
        break;
    case 4:                                         /* read */
        if (!(n = node(h))) {
            e = bad_handle();
            break;
        }
        s->r[1] = mapped == h ? ROS_APP_BASE : 0xFFFFFFFFu;
        s->r[3] = pages(n);
        break;
    case 5:                                         /* lazy map-in */
        if (s->r[1] != 0xFFFFFFFFu)
            lazy = s->r[1] != 0;
        s->r[1] = (uint32_t)lazy;
        break;
    case 9:                                         /* what is mapped, how many */
        s->r[2] = mapped, s->r[3] = count;
        break;
    case 6:
    case 7:
    case 8:
        e = ros_error(ERR_BAD_REASON, "reserved AMBControl reason code");
        break;
    default:
        e = ros_error(ERR_BAD_REASON, "bad AMBControl reason code");
        break;
    }
    if (e)
        ros_swi_fail(s, e);
}
