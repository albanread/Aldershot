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
 * (Sources/Kernel: s.ChangeDyn, s.NewReset, s.MemInfo, s.PMF.osinit).
 */

/* dynarea.c: dynamic areas and the free pool (Kernel/s/ChangeDyn).
 *
 * RISC OS keeps its variable-sized memory in dynamic areas. Every page that
 * is not in an area or in an application slot is in the free pool, which is
 * area 6. The areas and the slots grow from the free pool and shrink back
 * into it. ROSGD's arena reserves the areas' address range from ROS_DA_BASE
 * (arena.h). The arena's memory is Linux's, given a page at a time as it is
 * touched. So in the box the free pool is what Linux has free. That is
 * MemAvailable, at most what the slot pool can still give (arena.c), in 4K
 * pages and at most 2 GB, as OS_ReadMemMapInfo reports. The size of a slot
 * or an area takes nothing from the pool until the memory is used. Hosted,
 * the free pool is a count. It is the machine's memory less the dynamic
 * areas here, including the system ones, and less the pages touched in every
 * application slot (amb.c's nodes and a task's own: task.c). The RMA counts
 * as far as its heap ends. A claim that finds no room moves that end on, as
 * the kernel's RMA grows (rma.c). The RMA is never shrunk behind its users'
 * backs.
 *
 * The system areas, 0 to 5, are 5.30's (Kernel/s/NewReset and PMF/osinit),
 * with its titles and flags. ROSGD answers for their handlers with none
 * (R6 0).
 *   area 0, "System heap": a heap in the kernel's format at
 *     ROS_SYSHEAP_BASE. It has 32 MB of address space, as on 5.30. Its size
 *     is its extent (hpdEnd), and it is one page to start. ROSGD keeps the
 *     kernel's own state on the host, so nothing claims from it.
 *     OS_ChangeDynamicArea moves its end. A shrink stops at the blocks in
 *     use (PreShrink_Heap).
 *   area 1, "Module area": the RMA (rma.c) at ROS_RMA_BASE, with 256 MB.
 *     Its size is its heap's end (hpdEnd, from the area's base). It is a
 *     page to start, and grows by claims that find no room, as the kernel's
 *     does. A grow moves the end on. A shrink stops at the heap's top block.
 *   area 2, "Screen memory": the screen at ROS_SCREEN_BASE, with 64 MB. Its
 *     size is what the mode needs (TotalScreenSize), because only the
 *     mode's size is mapped. It cannot be grown, and a shrink stops at the
 *     mode's size. Flags are &120. That is 5.30's &160 less
 *     DynAreaFlags_DoublyMapped, since ROSGD maps the screen once. So
 *     OS_ReadDynamicArea's base is the screen's start, as a doubly mapped
 *     area's first copy is. Its workspace is VduDriverWorkSpace, as in 5.30.
 *   area 4, "Font cache": 32 MB of address space after the RAM disc's. At
 *     the start, CMOS FontSize's 4K pages of it are in use, as 5.30's
 *     InitHostedDAs makes it. FontManager keeps its cache on the host. The
 *     area is resized as 5.30's handler does it. A shrink goes down to the
 *     minimum that Font_ChangeArea -1 gives, and Font_ChangeArea is told the
 *     new size.
 *   area 5, "RAM disc": a physical memory pool (flags &100122) with 1 MB of
 *     logical space, as in 5.30, and no pages. ROSGD has no RAMFS, so the
 *     area cannot grow ("Memory cannot be moved").
 * These areas cannot be removed ("Unknown dynamic area") or renumbered.
 *
 *   area 3, "System sprites": 16 MB of address space at the start of the
 *     range. Part of it is in use, and that size is the sprite area's saEnd.
 *     The kernel's post-grow and post-shrink handlers keep it so (ChangeDyn
 *     6821-6875). A shrink stops at the sprites in use (saFree), or goes to
 *     nothing when there are none. ROSGD_SPRITESIZE (in bytes, default 256K)
 *     is its size at start, as CMOS SpriteSize is on RISC OS.
 *   area 6, the free pool: base 0, flags &100032 (a physical memory pool,
 *     neither cacheable nor bufferable, with no user access) and a maximum
 *     of the machine's memory, as 5.30 reports it. Growing it shrinks
 *     application space. Shrinking it grows application space.
 *   area -1, application space (OS_ReadDynamicArea only): its base is
 *     &8000. It reports the current size (AplWorkSize) and the most there
 *     can be.
 *   areas that OS_DynamicArea makes: they are numbered from &100, or as
 *     asked. They are placed upward after the font cache, or in the space a
 *     removed area gave back if one fits. Each is as big as its maximum, and
 *     part of that is in use. Their titles are copied up to a 0 byte (31
 *     characters at most), and a CR is kept as the kernel keeps it. If no
 *     title is asked for, the area number in hex is the title.
 *
 * Application space is the Wimp node mapped in (OS_AMBControl, a physical
 * memory pool on 5.30), or else the running task's own slot. Changing it
 * asks first, as CheckAppSpace does. If the current object is in application
 * space, the ask is UpCall_MovingMemory, which must be claimed. Otherwise it
 * is Service_Memory, which must not be claimed. BASIC claims the latter while
 * it runs, so a program's memory is not taken from under it.
 *
 * OS_ChangeDynamicArea moves whole pages. A grow is all or nothing. A shrink
 * moves what it can and says "Memory cannot be moved" if that is less than
 * asked. Service_MemoryMoved follows each, as the kernel issues it. It is
 * also issued for a Wimp node's pages, whichever way they change
 * (Wimp_SlotSize, OS_AMBControl 2, area 6 while a node is mapped), because
 * the kernel's PMP handler reports every move (PMPMemoryMoved: R2 -1, and
 * area 6's resize bit with handle -1). OS_DynamicArea 6 reports changes to
 * the areas numbered above 6: created, removed, resized and renumbered. It
 * uses the kernel's signature bits, with the initial grow and the final
 * shrink left out. Reasons 5 and 27 count in what shrinkable areas could
 * give up (their handlers' TestShrink).
 *
 * The OS_DynamicArea reasons here are 0 to 8, 24, 27 and 28. These are the
 * ones that the Wimp and the Task Manager use. Reasons 9 and 10 (sparse
 * areas), 20 (LocateAddress), 21 to 23 and 25 (the physical memory pools)
 * and 26 (AplSpaceLimit) exist in 5.30's kernel. Here they give
 * "OS_DynamicArea <n> is not implemented" (&1E7), so a gap shows as one.
 * The rest, 11 to 19 (ROL's) and 29 up, give the kernel's "Bad reason code".
 * Enumeration (reason 3) lists the system areas and the free pool among the
 * rest, in the kernel's title order. Reasons 7 and 28 leave them out. A new
 * area's maximum follows OS_DynamicArea 8's clamps. Handlers are not called
 * to grow or shrink an area. Only TestShrink is called.
 *
 * The errors have the kernel's numbers and texts (Kernel Messages).
 */
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && !defined(ROS_ARENA_HOSTED)
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cmos.h"
#include "rosgd/cpu.h"
#include "rosgd/dynarea.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/heap.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"

#define PAGE            4096u
#define AREA_SYSHEAP    0u
#define AREA_RMA        1u
#define AREA_SCREEN     2u
#define AREA_SPRITES    3u
#define AREA_FONTS      4u
#define AREA_RAMDISC    5u
#define AREA_FREEPOOL   6u
#define AREA_APPSPACE   0xFFFFFFFFu     /* ChangeDyn_AplSpace */
#define MAX_SYSTEM      6u              /* ChangeDyn_MaxArea */
#define RETURN_MAX      0x80u           /* ReadDyn_ReturnMax */
#define NEW_AREAS       0x100u          /* DynArea_NewAreas */
#define QUICK_HANDLES   256u            /* DynArea_NumQHandles */
#define SPRITES_MAX     0x01000000u
#define FONTS_MAX       0x02000000u     /* 5.30's: 32 MB */
#define RAMDISC_SPACE   0x00100000u     /* 5.30's: PMPRAMFS_Size pages */
#define SCREEN_FLAGS    0x00000120u     /* 5.30's &160, not doubly mapped */
#define FONTS_FLAGS     0x00000002u
#define RAMDISC_FLAGS   0x00100122u
#define CMOS_FONTSIZE   0x86u           /* FontCMOS: 4K pages */
#define VDU_WS          (ROS_ZEROPAGE + 0x1000u)    /* VduDriverWorkSpace */
#define XFONT_CHANGEAREA 0x600BFu
#define AREA_MAX_DEFAULT 0x01000000u    /* R5 = -1: ROSGD's own limit */
#define FLAG_SHRINKABLE (1u << 9)
#define FLAG_PMP        (1u << 20)
#define FREEPOOL_FLAGS  0x00100032u
#define BIG_PAGES       (1u << 19)      /* DynArea_PMP_BigPageCount */
#define BIG_BYTES       0x7FFFF000u     /* DynArea_PMP_BigByteCount */
#define ZP_APLWORK      0x368u          /* AplWorkSize */
#define ZP_CAO          0x7D4u          /* Curr_Active_Object */
#define TITLES          (ROS_ZEROPAGE + 0x6800u)    /* the system areas' titles */

#define SERVICE_MEMORY        0x11u
#define SERVICE_MEMORYMOVED   0x4Eu
#define SERVICE_DARENUMBER    0x92u
#define UPCALL_MOVINGMEMORY   257u

static os_error *err_unknown_area(void) { return ros_error(0x105u, "Unknown dynamic area"); }
static os_error *err_not_moved(void)    { return ros_error(0x1C1u, "Memory cannot be moved"); }
static os_error *err_bad_reason(void)   { return ros_error(0x180u, "Bad reason code"); }

struct area {
    struct area *next;
    uint32_t num, base, size, max, flags, handler, ws, title;
};
/* the system areas 0-5, then those OS_DynamicArea made, newest first */
static struct area *areas;
static struct area sysheap, rma, screen, sprites, fonts, ramdisc;
#define MADE (ramdisc.next)
static int da_space;                    /* the arena's range was had */
static uint32_t next_base = ROS_DA_BASE;
static uint32_t sig, prev_sig, sig_handle;          /* OS_DynamicArea 6 */
static uint32_t clamps[3] = { 0x08000000u, 0x08000000u, 0x08000000u };

static uint32_t pages(uint32_t n) { return (n + PAGE - 1) & ~(PAGE - 1); }

static struct area *find_area(uint32_t n)
{
    struct area *a = areas;
    while (a && a->num != n)
        a = a->next;
    return a;
}

/* ---- the free pool ---- */

uint64_t ros_mem_total(void)
{
    uint32_t size, n;
    xos_read_mem_map_info(&size, &n);
    return (uint64_t)size * n;
}

/* The RMA's heap as far as its top block, which is what a shrink must leave. */
static uint32_t rma_top(void)
{
    uint32_t hpd = ros_rma_heap();
    return pages(hpd - ROS_RMA_BASE + ros_ld32(hpd + 8));
}

static uint32_t screen_needs(void)
{
    return pages(ros_vdu_screen_size());
}

/* Refresh the system areas whose size follows what uses them. These are the
 * RMA's reach, the system heap's end and the screen's mode. */
static void refresh(void)
{
    if (!areas)
        return;
    rma.size = ros_rma_size();
    sysheap.size = ros_ld32(ROS_SYSHEAP_BASE + 12);
    screen.size = screen_needs();           /* only the mode's is mapped */
}

#if defined(__linux__) && !defined(ROS_ARENA_HOSTED)
/* Linux's MemAvailable, in bytes (0 if not known). */
static uint64_t mem_available(void)
{
    static int fd = -2;
    if (fd == -2)
        fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    char b[1024];
    ssize_t n = fd >= 0 ? pread(fd, b, sizeof b - 1, 0) : -1;
    if (n > 0) {
        b[n] = 0;
        const char *p = strstr(b, "MemAvailable:");
        if (p)
            return strtoull(p + 13, NULL, 10) << 10;
    }
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return ((uint64_t)si.freeram + si.bufferram) * si.mem_unit;
    return 0;
}
#endif

uint32_t ros_freepool_bytes(void)
{
    refresh();
    uint64_t total = ros_mem_total();
#if defined(__linux__) && !defined(ROS_ARENA_HOSTED)
    uint64_t free = mem_available(), pool = ros_slot_pool_free();
    if (pool < free)
        free = pool;
    if (free > total)
        free = total;
    return (uint32_t)free & ~(PAGE - 1);
#else
    uint64_t used = (uint64_t)ros_amb_bytes() + ros_task_slot_bytes();
    if (!areas)
        used += ros_rma_size();
    for (struct area *a = areas; a; a = a->next)
        used += a->size;
    return used >= total ? 0 : (uint32_t)(total - used);
#endif
}

/* A count of pages as bytes, clamped as the kernel clamps a physical pool's. */
static uint32_t pool_bytes(uint32_t bytes)
{
    return bytes / PAGE >= BIG_PAGES ? BIG_BYTES : bytes;
}

/* ---- titles ---- */

static void titles_init(void)
{
    static const char t[] = "System sprites\0Free pool\0Application space\0System heap\0"
                            "Module area\0Screen memory\0Font cache\0RAM disc";
    memcpy(ros_ptr(TITLES), t, sizeof t);
}
#define TITLE_SPRITES  (TITLES)
#define TITLE_FREEPOOL (TITLES + 15)
#define TITLE_SYSHEAP  (TITLES + 43)
#define TITLE_RMA      (TITLES + 55)
#define TITLE_SCREEN   (TITLES + 67)
#define TITLE_FONTS    (TITLES + 81)
#define TITLE_RAMDISC  (TITLES + 92)

static uint32_t copy_title(uint32_t from, uint32_t num)
{
    char buf[32];
    size_t n = 0;
    if (from) {
        /* Copy as the kernel does, up to a 0 byte and 31 characters at
         * most. So a CR that the caller ended the title with is kept
         * (ChangeDyn, DynArea_Create). */
        while (n < 31 && ros_ld8(from + n) != 0)
            buf[n] = (char)ros_ld8(from + n), n++;
    } else {
        static const char hex[] = "0123456789ABCDEF";
        for (int s = 28; s >= 0; s -= 4)            /* the number, in hex */
            buf[n++] = hex[(num >> s) & 15];
    }
    buf[n] = 0;
    char *p = ros_rma_alloc((uint32_t)n + 1);
    if (!p)
        return 0;
    memcpy(p, buf, n + 1);
    return ros_addr(p);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int title_cmp(uint32_t a, uint32_t b)
{
    for (;; a++, b++) {
        int x = lower((int)ros_ld8(a)), y = lower((int)ros_ld8(b));
        if (x != y || !x)
            return x - y;
    }
}

/* ---- the areas' list, in the kernel's order ---- */

/* Every area that enumeration sees, including the free pool, is in the
 * kernel's sorted list. Each area, as it was made, is put before the first
 * whose title is not less than its own (case ignored). */
struct entry { uint32_t num, title; };

static unsigned sorted(struct entry *out, unsigned most)
{
    struct area *made[QUICK_HANDLES + 8];
    unsigned m = 0, n = 0;
    for (struct area *a = areas ? MADE : NULL; a && m < QUICK_HANDLES + 8; a = a->next)
        made[m++] = a;                          /* newest first */
    struct entry order[QUICK_HANDLES + 16];
    unsigned k = 0;
    order[k++] = (struct entry){ AREA_FREEPOOL, TITLE_FREEPOOL };
    if (areas) {                                /* as 5.30 makes them */
        static struct area *const made_first[] = { &sysheap, &rma, &screen, &sprites, &ramdisc, &fonts };
        for (unsigned i = 0; i < sizeof made_first / sizeof made_first[0]; i++)
            order[k++] = (struct entry){ made_first[i]->num, made_first[i]->title };
    }
    for (; m; m--)                              /* oldest first, and an untitled one sorts first */
        order[k++] = (struct entry){ made[m - 1]->num, made[m - 1]->title ? made[m - 1]->title : TITLES + 14 };
    for (unsigned i = 0; i < k && n < most; i++) {
        unsigned at = 0;
        while (at < n && title_cmp(order[i].title, out[at].title) > 0)
            at++;
        memmove(&out[at + 1], &out[at], (n - at) * sizeof *out);
        out[at] = order[i];
        n++;
    }
    return n;
}

/* The area after `from` in title order. A `from` of -1 gives the first
 * area. System areas are skipped if asked. The result is -1 at the end, and
 * -2 if `from` is not an area. */
static uint32_t enumerate_next(uint32_t from, int skip_system)
{
    struct entry list[QUICK_HANDLES + 24];
    unsigned n = sorted(list, sizeof list / sizeof list[0]), i = 0;
    if (from != 0xFFFFFFFFu) {
        while (i < n && list[i].num != from)
            i++;
        if (i == n)
            return 0xFFFFFFFEu;
        i++;
    }
    for (; i < n; i++)
        if (!skip_system || list[i].num > MAX_SYSTEM)
            return list[i].num;
    return 0xFFFFFFFFu;
}

/* ---- the signature (OS_DynamicArea 6) ---- */

static void note(uint32_t bit, uint32_t num)
{
    prev_sig = sig;
    sig |= bit;
    sig_handle = num;
}

/* ---- calling out ---- */

static int service(uint32_t reason, uint32_t r0, uint32_t r2)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = r0, c.r[1] = reason, c.r[2] = r2;
    ros_swi(&c, XOS_ServiceCall);
    return c.r[1] == 0;
}

void ros_dynarea_pmp_moved(int32_t bytes)
{
    if (!bytes)
        return;
    note(4, 0xFFFFFFFFu);
    service(SERVICE_MEMORYMOVED, (uint32_t)bytes, 0xFFFFFFFFu);
}

/* CheckAppSpace: may application space change by `by` bytes (signed)? */
static os_error *check_app_space(int32_t by)
{
    uint32_t apl = ros_ld32(ROS_ZEROPAGE + ZP_APLWORK), cao = ros_ld32(ROS_ZEROPAGE + ZP_CAO);
    if (cao <= apl) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = UPCALL_MOVINGMEMORY;
        c.r[1] = by < 0 ? (uint32_t)-by : (uint32_t)by;
        ros_swi(&c, XOS_UpCall);
        return c.r[0] == 0 ? NULL
                           : ros_error(0x1C0u, "Can't change memory area (application running)");
    }
    if (service(SERVICE_MEMORY, (uint32_t)by, cao))
        return ros_error(0x1C2u, "Application memory area in use");
    return NULL;
}

/* CallTestShrink: how much a shrinkable area could give up. With no handler
 * the kernel adds whatever R3 held, which is the caller's value. */
static uint32_t test_shrink(const struct area *a, uint32_t r3)
{
    if (!a->handler)
        return r3;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 4;                             /* DAHandler_TestShrink */
    c.r[3] = r3;
    c.r[4] = a->size;
    c.r[5] = PAGE;
    c.r[12] = a->ws;
    c.r[14] = ROS_RETURN_TO_NATIVE;
    ros_call(&c, a->handler);
    if (c.r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(&c, ROS_RETURN_TO_NATIVE);
    return c.v ? 0 : c.r[3] & ~(PAGE - 1);
}

/* ---- the system sprite area ---- */

static void set_sprite_size(uint32_t size)
{
    uint32_t a = ROS_DA_BASE;
    if (size && !sprites.size) {                /* post-grow from nothing needs a fresh header */
        ros_st32(a + 4, 0);
        ros_st32(a + 8, 16);
        ros_st32(a + 12, 16);
    }
    sprites.size = size;
    if (size)
        ros_st32(a, size);
    ros_vdu_sprite_area(size ? a : 0);
}

/* The sprite area's pre-shrink. It goes down to the sprites in use, or to
 * nothing. */
static uint32_t sprites_can_shrink(void)
{
    if (!sprites.size)
        return 0;
    uint32_t used = ros_ld32(ROS_DA_BASE + 12);
    return sprites.size - (used == 16 ? 0 : pages(used));
}

/* Address space for a system area after the last, as 5.30 places them. */
static uint32_t place(const char *name, uint32_t size)
{
    uint32_t base = next_base;
    if (ros_arena_map_region(name, base, size))
        da_space = 0;
    next_base += size;
    return base;
}

void ros_dynarea_init(void)
{
    titles_init();
    da_space = 1;
    uint32_t sp = place("sprites", SPRITES_MAX);
    uint32_t rd = place("ramdisc", RAMDISC_SPACE);
    uint32_t fc = place("fontcache", FONTS_MAX);
    if (!da_space)
        return;
    if (ros_ld32(ROS_SYSHEAP_BASE) != 0x70616548u)     /* "Heap": one page */
        ros_heap_init(ROS_SYSHEAP_BASE, PAGE);
    sysheap = (struct area){ &rma, AREA_SYSHEAP, ROS_SYSHEAP_BASE, 0, ROS_SYSHEAP_SIZE, 0, 0,
                             ROS_SYSHEAP_BASE, TITLE_SYSHEAP };
    rma = (struct area){ &screen, AREA_RMA, ROS_RMA_BASE, 0, ROS_RMA_SIZE, 0, 0,
                         ROS_RMA_BASE, TITLE_RMA };
    screen = (struct area){ &sprites, AREA_SCREEN, ROS_SCREEN_BASE, 0, ROS_SCREEN_SIZE, SCREEN_FLAGS,
                            0, VDU_WS, TITLE_SCREEN };
    sprites = (struct area){ &fonts, AREA_SPRITES, sp, 0, SPRITES_MAX, 0, 0, sp, TITLE_SPRITES };
    fonts = (struct area){ &ramdisc, AREA_FONTS, fc, 0, FONTS_MAX, FONTS_FLAGS, 0, fc, TITLE_FONTS };
    ramdisc = (struct area){ NULL, AREA_RAMDISC, rd, 0, RAMDISC_SPACE, RAMDISC_FLAGS, 0, rd,
                             TITLE_RAMDISC };
    areas = &sysheap;
    fonts.size = ros_cmos_read(CMOS_FONTSIZE) * PAGE;
    refresh();
    const char *env = getenv("ROSGD_SPRITESIZE");
    uint32_t size = env ? (uint32_t)strtoul(env, NULL, 0) : 256 * 1024;
    if (size > SPRITES_MAX)
        size = SPRITES_MAX;
    set_sprite_size(pages(size));
}

/* ---- windows: address space for maps of slots (dynarea.h) ----
 *
 * Windows are taken from the same range as the areas, from next_base, so the
 * two never meet. Each has one more page than asked, left unmapped, after it.
 * A window given back is reused (first fit, split). The Worker module stops
 * every job that writes through a window before it gives the window back, so
 * nothing late can land in the next one there. Windows are not areas.
 * OS_DynamicArea does not list them, and the free pool does not count them,
 * because their pages belong to a slot. */
struct window {
    struct window *next;
    uint32_t base, size, used;          /* size is the space, its guard page included */
};
static struct window *windows;

uint32_t ros_dynarea_window_reserve(uint32_t bytes)
{
    if (!areas || !da_space || !bytes || bytes > ROS_SCREEN_BASE - ROS_DA_BASE)
        return 0;
    uint32_t want = pages(bytes) + PAGE;
    struct window *best = NULL;
    for (struct window *w = windows; w; w = w->next)
        if (!w->used && w->size >= want && (!best || w->size < best->size))
            best = w;
    if (best) {
        if (best->size - want >= 2 * PAGE) {    /* the rest becomes a free window of its own */
            struct window *rest = calloc(1, sizeof *rest);
            if (rest) {
                rest->base = best->base + want, rest->size = best->size - want;
                rest->next = best->next, best->next = rest;
                best->size = want;
            }
        }
        best->used = pages(bytes);
        return best->base;
    }
    if (next_base + want > ROS_SCREEN_BASE || next_base + want < next_base)
        return 0;
    struct window *w = calloc(1, sizeof *w);
    if (!w)
        return 0;
    w->base = next_base, w->size = want, w->used = pages(bytes);
    next_base += want;
    w->next = windows, windows = w;
    return w->base;
}

static void merge_free(void);

void ros_dynarea_window_release(uint32_t base)
{
    for (struct window *w = windows; w; w = w->next)
        if (w->base == base && w->used) {
            w->used = 0;
            break;
        }
    merge_free();
}

/* An area's address space, given back when it is removed, is reused too. It
 * becomes a free window of its size. The next area or window that fits takes
 * it (best fit) before any space after next_base is used. Without this,
 * every area ever made kept its space. A box that made and removed areas
 * ran the range out. Examples are a compression area each time (CompressPNG)
 * and a virtual display area each time (VDisplay). */
static void space_give(uint32_t base, uint32_t size)
{
    struct window *w = calloc(1, sizeof *w);
    if (!w)
        return;                         /* the space is lost */
    w->base = base, w->size = size;
    w->next = windows, windows = w;
    merge_free();
}

static uint32_t space_take(uint32_t want)
{
    struct window *best = NULL;
    for (struct window *w = windows; w; w = w->next)
        if (!w->used && w->size >= want && (!best || w->size < best->size))
            best = w;
    if (!best)
        return 0;
    uint32_t base = best->base;
    if (best->size > want) {            /* the rest stays free */
        best->base += want, best->size -= want;
        return base;
    }
    for (struct window **p = &windows; *p; p = &(*p)->next)
        if (*p == best) {
            *p = best->next;
            break;
        }
    free(best);
    return base;
}

/* Neighbours that are both free are made into one. */
static void merge_free(void)
{
    for (struct window *w = windows; w; w = w->next)
        for (struct window **p = &windows; *p;) {
            struct window *o = *p;
            if (o != w && !w->used && !o->used && w->base + w->size == o->base) {
                w->size += o->size;
                *p = o->next;
                free(o);
            } else {
                p = &o->next;
            }
        }
}

int ros_dynarea_contains(uint32_t start, uint32_t end)
{
    if (end < start)
        return 0;
    refresh();
    for (struct area *a = areas; a; a = a->next)
        if (a != &screen && start >= a->base && end <= a->base + a->size)
            return 1;
    for (struct window *w = windows; w; w = w->next)
        if (w->used && start >= w->base && end <= w->base + w->used)
            return 1;
    return 0;
}

/* ---- application space ---- */

static uint32_t app_size(void)
{
    return ros_ld32(ROS_ZEROPAGE + ZP_APLWORK) - ROS_APP_BASE;
}

/* Make application space `bytes` long. This is either the mapped node's
 * (AMB's growp and shrinkp set both AplWorkSize and MemLimit), or the task's
 * own. For the task's own, the kernel's DoTheGrow sets MemLimit and
 * AreaShrink leaves it (ChangeDyn 6266). */
static int app_resize(uint32_t bytes)
{
    int pmp = ros_amb_mapped();
    int grow = bytes > app_size();
    int e = pmp ? ros_amb_resize_mapped(bytes) : ros_task_resize_own(bytes);
    if (e == 0) {
        if (pmp || grow)
            ros_env_set_memory_limit(ROS_APP_BASE + bytes);     /* AplWorkSize, MemLimit */
        else
            ros_st32(ROS_ZEROPAGE + ZP_APLWORK, ROS_APP_BASE + bytes);
    }
    return e;
}

/* The free pool grows by `want` bytes, so application space shrinks. The
 * bytes moved are returned in *moved (R1). There is an error if fewer than
 * asked were moved, or if none may be. */
static os_error *freepool_grow(uint32_t want, uint32_t *moved)
{
    *moved = 0;
    uint32_t have = app_size();
    int pmp = ros_amb_mapped();
    os_error *e = NULL;
    if (!pmp && want > have) {                  /* AreaShrink of the static space */
        want = have;
        e = err_not_moved();
    }
    if (pmp || want) {
        os_error *no = check_app_space(-(int32_t)want);
        if (no)
            return no;
        if (pmp && want > have)                 /* the node's PMP resize goes to nothing, */
            want = have;                        /* silently (AMBDAHandler) */
        if (app_resize(have - want) != 0)
            return err_not_moved();
        *moved = want;
    }
    if (!pmp)
        service(SERVICE_MEMORYMOVED, (uint32_t)-(int32_t)*moved, AREA_APPSPACE);
    return e;
}

/* The free pool shrinks by `want` bytes, so application space grows. */
static os_error *freepool_shrink(uint32_t want, uint32_t *moved)
{
    *moved = 0;
    /* Application space is lazy (arena.h). It may grow to the map's limit,
     * whatever is free, because its pages are taken as they are touched. */
    uint32_t have = app_size(), room = ROS_APP_LIMIT - ROS_APP_BASE - have;
    os_error *e = NULL;
    if (want > room) {
        want = room;
        e = err_not_moved();
    }
    int pmp = ros_amb_mapped();
    if (want) {
        os_error *no = check_app_space((int32_t)want);
        if (no)
            return no;
        if (app_resize(have + want) != 0)
            return err_not_moved();
        *moved = want;
    }
    if (!pmp)
        service(SERVICE_MEMORYMOVED, *moved, AREA_APPSPACE);
    return e;
}

/* ---- OS_ChangeDynamicArea ---- */

/* Font_ChangeArea. R1 is the font cache's new size, or -1 to ask for its
 * minimum. The minimum comes back in R2, which is 0 with no FontManager. */
static uint32_t font_change_area(uint32_t r1)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = r1, c.r[2] = 0;
    ros_swi(&c, XFONT_CHANGEAREA);
    return c.v ? 0 : c.r[2];
}

/* Set a system area's size, as its handler's post-grow and post-shrink do. */
static void set_size(struct area *a, uint32_t size)
{
    switch (a->num) {
    case AREA_SYSHEAP:
        ros_st32(ROS_SYSHEAP_BASE + 12, size);      /* hpdEnd */
        break;
    case AREA_RMA:
        ros_st32(ros_rma_heap() + 12, size - 12);   /* the heap's end, hpdEnd */
        break;
    case AREA_SPRITES:
        set_sprite_size(size);
        return;
    }
    a->size = size;
    if (a == &fonts)
        font_change_area(size);
}

/* What a shrink may give up, as the handlers' pre-shrink allow. */
static uint32_t can_shrink(struct area *a)
{
    uint32_t keep;
    if (a->num > MAX_SYSTEM)
        return a->size;
    switch (a->num) {
    case AREA_SYSHEAP: keep = pages(ros_ld32(ROS_SYSHEAP_BASE + 8)); break;     /* hpdBase */
    case AREA_RMA:     keep = rma_top(); break;
    case AREA_SCREEN:  keep = screen_needs(); break;
    case AREA_SPRITES: return sprites_can_shrink();
    case AREA_FONTS:   keep = pages(font_change_area(0xFFFFFFFFu)); break;
    default:           keep = a->size; break;
    }
    return a->size > keep ? a->size - keep : 0;
}

/* Grow an area from the free pool. It is all or nothing. */
static os_error *area_grow(struct area *a, uint32_t want, uint32_t *moved)
{
    *moved = 0;
    /* There is no RAMFS, so there is no RAM disc. The screen is the
     * display's buffer. Only the mode's size of it is mapped, and the screen
     * block follows it. So the screen cannot be bigger than the mode. */
    if (a == &ramdisc || a == &screen || want > a->max - a->size || want > ros_freepool_bytes())
        return err_not_moved();
    set_size(a, a->size + want);
    *moved = want;
    return NULL;
}

/* Shrink an area into the free pool by as much as it can. */
static os_error *area_shrink(struct area *a, uint32_t want, uint32_t *moved)
{
    os_error *e = NULL;
    uint32_t can = can_shrink(a);
    if (want > can) {
        want = can;
        e = err_not_moved();
    }
    set_size(a, a->size - want);
    *moved = want;
    return e;
}

static os_error *change(uint32_t n, uint32_t by, uint32_t *moved)
{
    *moved = 0;
    refresh();
    struct area *a = n == AREA_FREEPOOL ? NULL : find_area(n);
    if (!a && n != AREA_FREEPOOL)
        return err_not_moved();
    if (n > MAX_SYSTEM && n <= 0xFFFFFFF0u && !(sig & 0x80000000u))
        note(4, n);
    sig &= ~0x80000000u;

    uint32_t change = (by + PAGE - 1) & ~(PAGE - 1);
    if (!change) {
        service(SERVICE_MEMORYMOVED, 0, n);
        return NULL;
    }
    if (n == AREA_FREEPOOL)
        return (int32_t)change > 0 ? freepool_grow(change, moved)
                                   : freepool_shrink(-change, moved);
    os_error *e;
    int32_t signed_moved;
    if ((int32_t)change > 0) {
        e = area_grow(a, change, moved);
        signed_moved = (int32_t)*moved;
    } else {
        e = area_shrink(a, -change, moved);
        signed_moved = -(int32_t)*moved;
    }
    service(SERVICE_MEMORYMOVED, (uint32_t)signed_moved, n);
    return e;
}

void ros_thunk_OS_ChangeDynamicArea(struct ros_cpu *s)
{
    uint32_t outer = ros_svc_sp_enter(s);
    uint32_t moved;
    os_error *e = change(s->r[0], s->r[1], &moved);
    ros_svc_sp = outer;
    s->v = 0;
    s->r[1] = moved;
    if (e)
        ros_swi_fail(s, e);
}

/* ---- OS_ReadDynamicArea ---- */

/* The most an area can be, in bytes. For a physical memory pool the maximum
 * is counted in pages and the logical size is 0 (the RAM disc has none).
 * Otherwise it is the area's logical size. */
static uint32_t max_bytes(const struct area *a)
{
    return a->flags & FLAG_PMP ? 0 : a->max;
}

void ros_thunk_OS_ReadDynamicArea(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0];
    s->v = 0;
    if (r0 == AREA_APPSPACE) {
        s->r[0] = ROS_APP_BASE;
        s->r[1] = app_size();
        s->r[2] = ROS_APP_LIMIT - ROS_APP_BASE;
        return;
    }
    uint32_t n = r0 >= NEW_AREAS ? r0 : r0 & ~RETURN_MAX;
    int max = r0 >= RETURN_MAX;
    refresh();
    if (n == AREA_FREEPOOL) {
        s->r[0] = 0;
        s->r[1] = pool_bytes(ros_freepool_bytes());
        if (max)
            s->r[2] = pool_bytes((uint32_t)ros_mem_total());
        return;
    }
    struct area *a = find_area(n);
    if (!a) {
        ros_swi_fail(s, err_unknown_area());
        return;
    }
    s->r[0] = a->base, s->r[1] = a->size;
    if (max)
        s->r[2] = max_bytes(a);
}

/* ---- OS_DynamicArea ---- */

static os_error *create(struct ros_cpu *s)
{
    uint32_t n = s->r[1];
    if (!areas || !da_space)                    /* the arena's range could not be had */
        return ros_error(0x1C7u, "Unable to allocate logical address space");
    if (n == 0xFFFFFFFFu) {
        for (n = NEW_AREAS; n < NEW_AREAS + QUICK_HANDLES && find_area(n); n++)
            ;
        if (n == NEW_AREAS + QUICK_HANDLES)
            for (n = NEW_AREAS + QUICK_HANDLES; find_area(n); n++)
                ;
    } else if (n <= MAX_SYSTEM || (n >= NEW_AREAS && n < NEW_AREAS + QUICK_HANDLES) ||
               find_area(n)) {
        return ros_error(0x1C4u, "Dynamic area already exists");
    }
    /* The maximum is clamped as OS_DynamicArea 8 set it. R5 = -1 is clamped
     * by the first clamp and by ROSGD's own limit, because its address space
     * is the arena's. A given maximum is clamped by the second clamp. A
     * sparse area's maximum (R4 bit 10) is clamped by the third. It is never
     * below the initial size, and at most the machine's memory
     * (ChangeDyn 883-897). */
    uint32_t req = s->r[5];
    if (s->r[4] & (1u << 10))
        req = req > clamps[2] ? clamps[2] : req;
    else if (req == 0xFFFFFFFFu)
        req = clamps[0] < AREA_MAX_DEFAULT ? clamps[0] : AREA_MAX_DEFAULT;
    else if (req > clamps[1])
        req = clamps[1];
    if (req < s->r[2])
        req = s->r[2];
    uint64_t total = ros_mem_total();
    if (req > total)
        req = (uint32_t)total;
    uint32_t max = pages(req);
    uint32_t size = pages(s->r[2]);
    if (size > max)
        max = size;
    uint32_t space = max ? max : PAGE;
    if (s->r[3] != 0xFFFFFFFFu)
        return ros_error(0x1C7u, "Unable to allocate logical address space");
    uint32_t base = space_take(space);
    int fresh = !base;
    if (fresh) {
        if (next_base + space > ROS_SCREEN_BASE || next_base + space < next_base)
            return ros_error(0x1C7u, "Unable to allocate logical address space");
        base = next_base;
    }
    if (ros_arena_map_region("dynamicarea", base, space)) {
        if (!fresh)
            space_give(base, space);
        return ros_error(0x1C7u, "Unable to allocate logical address space");
    }
    if (fresh)
        next_base += space;
    struct area *a = calloc(1, sizeof *a);
    if (!a) {
        ros_arena_unmap(base, space);
        space_give(base, space);
        return ros_error(0x1C7u, "Unable to allocate logical address space");
    }
    a->num = n, a->base = base, a->max = max;
    a->flags = s->r[4], a->handler = s->r[6];
    a->ws = s->r[7] == 0xFFFFFFFFu ? a->base : s->r[7];
    a->title = copy_title(s->r[8], n);
    a->next = MADE, MADE = a;                   /* after the system areas, newest first */

    sig |= 0x80000000u;                         /* no resize signature for the first grow */
    if (size) {
        uint32_t moved;
        os_error *e = change(n, size, &moved);
        if (e) {
            MADE = a->next;
            ros_arena_unmap(a->base, space);
            space_give(a->base, space);
            if (a->title)
                ros_rma_free(ros_ptr(a->title));
            free(a);
            return e;
        }
    }
    note(1, n);
    s->r[1] = n, s->r[3] = a->base, s->r[5] = max;
    return NULL;
}

static os_error *remove_area(uint32_t n)
{
    struct area **pp = &areas;
    while (*pp && (*pp)->num != n)
        pp = &(*pp)->next;
    struct area *a = *pp;
    if (!a || a->num <= MAX_SYSTEM)
        return err_unknown_area();
    sig |= 0x80000000u;                         /* and none for the last shrink */
    if (a->size) {
        uint32_t moved;
        change(n, (uint32_t)-(int32_t)a->size, &moved);
    }
    *pp = a->next;
    ros_arena_unmap(a->base, a->max ? a->max : PAGE);
    space_give(a->base, a->max ? a->max : PAGE);
    if (a->title)
        ros_rma_free(ros_ptr(a->title));
    free(a);
    note(2, n);
    return NULL;
}

static os_error *renumber(struct ros_cpu *s)
{
    uint32_t from = s->r[1], to = s->r[2];
    if (from <= MAX_SYSTEM || (from >= NEW_AREAS && from < NEW_AREAS + QUICK_HANDLES) ||
        (to >= NEW_AREAS && to < NEW_AREAS + QUICK_HANDLES))
        return ros_error(0, "illegal DA renumber");
    struct area *a = find_area(from);
    if (!a)
        return err_unknown_area();
    if (find_area(to) || to <= MAX_SYSTEM)
        return ros_error(0x1C4u, "Dynamic area already exists");
    a->num = to;
    struct ros_cpu c;                           /* Service_DynamicAreaRenumber */
    ros_cpu_enter(&c);
    c.r[1] = SERVICE_DARENUMBER, c.r[2] = from, c.r[3] = to;
    ros_swi(&c, XOS_ServiceCall);
    note(8, from);
    return NULL;
}

/* OS_DynamicArea 5 and 27: the free pool and what shrinkable areas could
 * give up, in pages, leaving out area R1 (-1 for none). */
static os_error *free_pages(struct ros_cpu *s, uint32_t *out)
{
    struct area *skip = NULL;
    if (s->r[1] != 0xFFFFFFFFu && s->r[1] != AREA_FREEPOOL && !(skip = find_area(s->r[1])))
        return err_unknown_area();
    uint32_t n = ros_freepool_bytes() / PAGE;
    for (struct area *a = areas; a; a = a->next)
        if ((a->flags & FLAG_SHRINKABLE) && a != skip)
            n += test_shrink(a, s->r[3]) >> 12;
    *out = n;
    return NULL;
}

/* OS_DynamicArea 7 and 28: the next non-system area, and what it is. */
static os_error *enumerate_info(struct ros_cpu *s, int in_pages)
{
    uint32_t n = enumerate_next(s->r[1], 1);
    if (n == 0xFFFFFFFEu)
        return err_unknown_area();
    s->r[1] = n;
    if (n == 0xFFFFFFFFu)
        return NULL;
    struct area *a = find_area(n);
    s->r[2] = in_pages ? a->size / PAGE : a->size;
    s->r[3] = a->base, s->r[4] = a->flags;
    s->r[5] = in_pages ? max_bytes(a) / PAGE : max_bytes(a);
    s->r[6] = a->title;
    return NULL;
}

void ros_thunk_OS_DynamicArea(struct ros_cpu *s)
{
    uint32_t outer = ros_svc_sp_enter(s);
    os_error *e = NULL;
    struct area *a;
    s->v = 0;
    refresh();
    switch (s->r[0]) {
    case 0:                                             /* create */
        e = create(s);
        break;
    case 1:                                             /* remove */
        e = remove_area(s->r[1]);
        break;
    case 2:                                             /* read */
        if (s->r[1] == AREA_FREEPOOL) {
            uint32_t total = (uint32_t)ros_mem_total();
            s->r[2] = pool_bytes(ros_freepool_bytes()), s->r[3] = 0, s->r[4] = FREEPOOL_FLAGS;
            s->r[5] = pool_bytes(total), s->r[6] = 0, s->r[7] = 0, s->r[8] = TITLE_FREEPOOL;
        } else if ((a = find_area(s->r[1]))) {
            s->r[2] = a->size, s->r[3] = a->base, s->r[4] = a->flags, s->r[5] = max_bytes(a);
            s->r[6] = a->handler, s->r[7] = a->ws, s->r[8] = a->title;
        } else {
            e = err_unknown_area();
        }
        break;
    case 3: {                                           /* enumerate */
        uint32_t n = enumerate_next(s->r[1], 0);
        if (n == 0xFFFFFFFEu)
            e = err_unknown_area();
        else
            s->r[1] = n;
        break;
    }
    case 4:                                             /* renumber */
        e = renumber(s);
        break;
    case 5:                                             /* free, in bytes */
    case 27: {                                          /* ... in pages */
        uint32_t n;
        if (!(e = free_pages(s, &n)))
            s->r[2] = s->r[0] == 27 ? n : n >= BIG_PAGES ? BIG_BYTES : n * PAGE;
        break;
    }
    case 24:                                            /* PMP_GetInfo */
        /* The Task Manager tries this first. R2 to R5 are the logical size,
         * base, flags and maximum. R6 and R7 are the size and maximum in
         * pages, which for a physical memory pool are its pages (the free
         * pool's, and none for the RAM disc). R8 is the title
         * (ChangeDyn 3648-3657). */
        if (s->r[1] == AREA_FREEPOOL) {
            s->r[2] = 0, s->r[3] = 0, s->r[4] = FREEPOOL_FLAGS, s->r[5] = 0;
            s->r[6] = ros_freepool_bytes() / PAGE;
            s->r[7] = (uint32_t)(ros_mem_total() / PAGE);
            s->r[8] = TITLE_FREEPOOL;
        } else if ((a = find_area(s->r[1]))) {
            int pmp = (a->flags & FLAG_PMP) != 0;
            s->r[2] = pmp ? 0 : a->size, s->r[3] = a->base, s->r[4] = a->flags, s->r[5] = a->max;
            s->r[6] = a->size / PAGE, s->r[7] = max_bytes(a) / PAGE, s->r[8] = a->title;
        } else {
            e = err_unknown_area();
        }
        break;
    case 6:                                             /* what changed */
        s->r[1] = prev_sig || !sig ? 0xFFFFFFFFu : sig_handle;
        s->r[2] = sig;
        sig = prev_sig = 0;
        break;
    case 7:
        e = enumerate_info(s, 0);
        break;
    case 28:
        e = s->r[2] ? ros_error(0x1EAu, "Parameters not recognised") : enumerate_info(s, 1);
        break;
    case 8:                                             /* clamps */
        for (unsigned i = 0; i < 3; i++) {
            uint32_t was = clamps[i];
            if (s->r[1 + i] >= 0x100000u)     /* values under 1M are ignored */
                clamps[i] = s->r[1 + i];
            s->r[1 + i] = was;
        }
        break;
    case 9: case 10: case 20: case 21: case 22: case 23: case 25: case 26:
        e = ros_error(ROS_ERR_UNIMPLEMENTED, "OS_DynamicArea %u is not implemented", s->r[0]);
        break;
    default:
        e = err_bad_reason();
        break;
    }
    ros_svc_sp = outer;
    if (e)
        ros_swi_fail(s, e);
}

/* ---- OS_Memory ----
 *
 * Reason 8 gives the amounts of each kind of memory (the Task Manager uses
 * it). The kind is R0 shifted right by 8, so it is in bits 8 upward. Kind 1
 * is DRAM, and all of it is reported. VRAM, ROM, I/O space and soft-loaded
 * ROM are reported as none here. On exit R1 is the number of pages and R2 the
 * page size. Kinds past 5 give "Parameters not recognised", as
 * MemoryAmounts does.
 *
 * Reason 16 is MemoryAreaInfo (the Task Manager's "System memory
 * allocation"). Bits 8-15 hold the area, 1 to 17. On exit R1 is its base, R2
 * the address space it has and R3 the memory it uses, as in Kernel/s/MemInfo.
 * ROSGD keeps RISC OS 5's map (arena.h). It uses its own addresses for zero
 * page, ScratchSpace and the SVC stack. Zero page is 32K here, because the
 * runtime's error buffers and titles are in it. For the kernel's regions
 * that ROSGD never maps, it reports 5.30's addresses and extents with no
 * memory used in them. These regions are the cursor chunk, the other stacks,
 * the soft CAM (16 bytes a page, of ROSGD's memory), the page tables, HAL
 * workspace, the kernel's buffers and its RW data. The box does map
 * DebuggerSpace (arena.h), so it reports that page as used, as 5.30 does.
 * The page-zero compatibility page stays disabled. 5.30 reports it the same
 * way when it is off. It stays off so that a null pointer still faults here.
 * Area 0, or an area past 17, gives "Parameters not recognised"
 * (MemoryBadParameters). Bits 16-31 are not looked at.
 *
 * The box answers reason 20, Compatibility, because it has a definite
 * answer. The page is off, and the box will not put it there.
 *
 * Other reasons that 5.30's kernel has are 0, 6, 7, 9, 12-15, 17-19, 21-24,
 * 64 and 65. They give "OS_Memory <n> is not implemented" (&1E7). The
 * reasons that 5.30 reserves are 1-5, 10, 11, 25-63 and 66 up. They give the
 * kernel's "Bad reason code". */
static const struct { uint32_t base, space, used; } memory_areas[18] = {
    [1]  = { 0xFAFF0000u, 0x8000u, 0 },                 /* cursor/system/sound */
    [2]  = { 0xFA100000u, 0x2000u, 0 },                 /* IRQ stack */
    [3]  = { ROS_SVCSTACK_BASE, ROS_SVCSTACK_SIZE, ROS_SVCSTACK_SIZE },
    [4]  = { 0xFA300000u, 0x2000u, 0 },                 /* ABT stack */
    [5]  = { 0xFA400000u, 0x2000u, 0 },                 /* UND stack */
    [6]  = { 0, 0, 0 },                                 /* the soft CAM, sized below */
    [7]  = { 0xFAC00000u, 0x4000u, 0 },                 /* L1PT */
    [8]  = { 0xFA800000u, 0x400000u, 0 },               /* L2PT */
    [9]  = { 0xFA000000u, 0x100000u, 0 },               /* HAL workspace */
    [10] = { 0xFA450000u, 0x10000u, 0 },                /* kernel buffers */
    [11] = { 0xFA460000u, 0x8000u, 0 },                 /* HAL uncacheable workspace */
    [12] = { ROS_ZEROPAGE, ROS_ZEROPAGE_SIZE, ROS_ZEROPAGE_SIZE },
    [13] = { 0, 0, 0 },                                 /* processor vectors: in zero page */
    [14] = { ROS_DEBUGGER_BASE, ROS_DEBUGGER_SIZE,                      /* DebuggerSpace */
             ROS_DEBUGGER_MAPPED ? ROS_DEBUGGER_SIZE : 0 },
    [15] = { ROS_SCRATCH_BASE, ROS_SCRATCH_SIZE, ROS_SCRATCH_SIZE },
    [16] = { 0, 0x1000u, 0 },                           /* compatibility page, disabled */
    [17] = { 0xFF000000u, 0x1000u, 0 },                 /* the kernel's RW data */
};

void ros_thunk_OS_Memory(struct ros_cpu *s)
{
    s->v = 0;
    uint32_t reason = s->r[0] & 0xFF;
    if (reason == 16) {
        uint32_t area = (s->r[0] >> 8) & 0xFF;
        if (area == 0 || area >= sizeof memory_areas / sizeof memory_areas[0]) {
            ros_swi_fail(s, ros_error(0x1EAu, "Parameters not recognised"));
            return;
        }
        s->r[1] = memory_areas[area].base;
        s->r[2] = memory_areas[area].space;
        s->r[3] = memory_areas[area].used;
        if (area == 6) {                    /* the soft CAM: below HAL workspace */
            s->r[2] = pages((uint32_t)(ros_mem_total() / PAGE * 16));
            s->r[1] = 0xFA000000u - s->r[2];
        }
        return;
    }
    /* Reason 20, Compatibility (Kernel/s/MemInfo, ChangeCompatibility).
     * R1 = 1 puts there the read-only page that RISC OS can keep at &0.
     * R1 = 0 takes it away and R1 = -1 asks. On exit R1 is the state before
     * the call. The box has no such page and will not take one. A null
     * pointer that is followed in its own compiled code must keep faulting
     * where it is made (arena.h, boot/main.c, area 16 above). So the box
     * answers 0, which means the page is off. To a request to put the page
     * there it answers -1, as 5.30 does when it cannot. That is not an
     * error. */
    if (reason == 20) {
        uint32_t want = s->r[1];
        if (want != 0 && want != 1 && want != 0xFFFFFFFFu) {
            ros_swi_fail(s, ros_error(0x1EAu, "Parameters not recognised"));
            return;
        }
        s->r[1] = want == 1 ? 0xFFFFFFFFu : 0;
        return;
    }
    if (reason == 0 || (reason >= 6 && reason <= 24 && reason != 8 && reason != 10 && reason != 11) ||
        reason == 64 || reason == 65) {
        ros_swi_fail(s, ros_error(ROS_ERR_UNIMPLEMENTED, "OS_Memory %u is not implemented", reason));
        return;
    }
    if (reason != 8) {
        ros_swi_fail(s, err_bad_reason());
        return;
    }
    uint32_t kind = s->r[0] >> 8;
    if (kind == 0 || kind > 5) {
        ros_swi_fail(s, ros_error(0x1EAu, "Parameters not recognised"));
        return;
    }
    s->r[1] = kind == 1 ? (uint32_t)(ros_mem_total() / PAGE) : 0;
    s->r[2] = PAGE;
}
