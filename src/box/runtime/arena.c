/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* arena.c: reserve the low 4 GB and lay RISC OS's map out in it.
 *
 * The whole arena is reserved first, as one PROT_NONE mapping, so nothing
 * of Linux's can ever land in it. Then each region is mapped over the
 * reservation with MAP_FIXED. Regions are memfds rather than anonymous
 * memory so that a later stage, with a process per task, can map the same
 * regions at the same addresses in every process.
 *
 * The runtime is linked as a static PIE, which the kernel loads far above
 * 4 GB. A non-PIE static binary would sit at 16 MB, inside the arena, and
 * the reservation would fail. The first boot found this on 24 Sep 2026.
 *
 * Application slots and the RMA are executable in the box, as RISC OS's
 * are. A C application's code runs from its slot and a C module's from the
 * RMA (capp.h). Their memfds are made with MFD_EXEC, so vm.memfd_noexec
 * never refuses them. The hosted build runs no such code and maps them
 * read/write only.
 *
 * Application slots are lazy. A slot is a file of its size whose pages
 * Linux allocates when they are first touched, so a 1.5 GB slot costs what
 * its program uses (ros_slot_used: the file's blocks). In the box they
 * are files in the slot pool. This is a tmpfs that /init mounts at
 * ROS_SLOT_POOL with a size short of the machine's memory (boot/main.c).
 * When the pool is used up, a page touched faults as a bus error, which
 * runtime/fault.c gives the program as a data abort. A memfd's page would
 * instead have woken the OOM killer, and the only process it can kill is
 * /init, the box. Without the pool (hosted, or a kernel without
 * O_TMPFILE) slots are memfds.
 * Nothing here touches a slot's pages. Creating, growing and mapping one
 * set sizes and page tables only. The hosted copy (macOS, where shared
 * memory has one size) copies only the pages that are there.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) && !defined(ROS_ARENA_HOSTED)
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

#include "rosgd/arena.h"
#include "rosgd/platform.h"

#ifdef ROS_ARENA_HOSTED
uintptr_t ros_arena_base;
#endif

const struct ros_slot *ros_slot_current;
struct ros_slot ros_svcstack_initial = { -1, 0 };

uint32_t ros_addr(const void *p)
{
    if (!p)
        return 0;
    uintptr_t u = (uintptr_t)p;
    if (u < ros_arena_base || u - ros_arena_base >= 0x100000000ull) {
        ros_console_printf("rosgd: host pointer %p reached compiled code: "
                           "it is not arena memory\n", p);
        abort();
    }
    return (uint32_t)(u - ros_arena_base);
}

#ifdef __linux__
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010u
#endif
#endif
/* Slots and the RMA are executable in the box, where x32 code runs from
 * them (capp.h); a hosted build, even on Linux, runs none there. */
#if defined(__linux__) && !defined(ROS_ARENA_HOSTED)
#define PROT_CODE (PROT_READ | PROT_WRITE | PROT_EXEC)
#else
#define PROT_CODE (PROT_READ | PROT_WRITE)
#endif

/* An anonymous region, backed by a memfd where the platform has them. For a
 * slot (pool set), it is a nameless file in the slot pool if there is one. */
static int backing_in(const char *name, size_t size, int pool)
{
#ifdef __linux__
    int fd = -1;
#if !defined(ROS_ARENA_HOSTED) && defined(O_TMPFILE)
    if (pool) {
        fd = open(ROS_SLOT_POOL, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        if (fd >= 0 && ftruncate(fd, (off_t)size) < 0) {
            int e = -errno;
            close(fd);
            return e;
        }
        if (fd >= 0)
            return fd;
    }
#else
    (void)pool;
#endif
    fd = memfd_create(name, MFD_CLOEXEC | MFD_EXEC);
    if (fd < 0 && errno == EINVAL)          /* a kernel before 6.3: executable anyway */
        fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (ftruncate(fd, (off_t)size) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    return fd;
#else
    (void)pool;
    /* A hosted build on macOS: POSIX shared memory, unlinked at once, is
     * the nearest thing to a memfd. It gives a descriptor that can be mapped
     * and mapped again elsewhere. */
    static unsigned serial;
    char shm[64];
    snprintf(shm, sizeof shm, "/rosgd.%d.%s.%u", (int)getpid(), name, serial++);
    int fd = shm_open(shm, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -errno;
    shm_unlink(shm);
    if (ftruncate(fd, (off_t)size) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    return fd;
#endif
}

static int backing(const char *name, size_t size)
{
    return backing_in(name, size, 0);
}

static int map_region_prot(const char *name, uint32_t addr, uint32_t size, int prot)
{
    int fd = backing(name, size);
    void *want = ros_ptr(addr), *got;
    if (fd >= 0) {
        got = mmap(want, size, prot, MAP_SHARED | MAP_FIXED, fd, 0);
        close(fd);
    } else {
        /* No memfd (EINVAL, or EACCES under vm.memfd_noexec=2). Use private
         * memory with the protection asked for, keeping execute. */
        got = mmap(want, size, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    }
    if (got != want) {
        ros_console_printf("rosgd: cannot map %s at &%08X\n", name, addr);
        return -ENOMEM;
    }
    return 0;
}

static int map_region(const char *name, uint32_t addr, uint32_t size)
{
    return map_region_prot(name, addr, size, PROT_READ | PROT_WRITE);
}

int ros_arena_init(void)
{
    size_t span = 0x100000000ull - ROS_APP_BASE;
#ifdef ROS_ARENA_HOSTED
    /* macOS keeps the low 4 GB for itself. Reserve 4 GB anywhere, and
     * offset every arena address by where it landed. */
    void *p = mmap(NULL, 0x100000000ull, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -errno;
    ros_arena_base = (uintptr_t)p;
#else
    void *want = ros_ptr(ROS_APP_BASE);
    void *p = mmap(want, span, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p != want)
        return p == MAP_FAILED ? -errno : -EEXIST;
#endif
    (void)span;
    int e;
    if ((e = map_region_prot("rma", ROS_RMA_BASE, ROS_RMA_SIZE, PROT_CODE)) ||
        (e = map_region("sysheap", ROS_SYSHEAP_BASE, ROS_SYSHEAP_SIZE)) ||
        (e = ros_stack_create(&ros_svcstack_initial, ROS_SVCSTACK_SIZE)) ||
        (e = ros_slot_map_at(&ros_svcstack_initial, ROS_SVCSTACK_BASE)) ||
        (e = map_region("rom", ROS_ROM_BASE, ROS_ROM_SIZE)) ||
        (e = map_region("zeropage", ROS_ZEROPAGE, ROS_ZEROPAGE_SIZE)) ||
        (e = map_region("scratchspace", ROS_SCRATCH_BASE, ROS_SCRATCH_SIZE)) ||
        (ROS_DEBUGGER_MAPPED &&
         (e = map_region("debuggerspace", ROS_DEBUGGER_BASE, ROS_DEBUGGER_SIZE))))
        return e;
    return 0;
}

void ros_arena_unmap(uint32_t addr, uint32_t size)
{
    mmap(ros_ptr(addr), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

static int map_fd(uint32_t addr, size_t size, int fd, uint64_t offset, int prot)
{
    void *want = ros_ptr(addr);
    void *got = mmap(want, size, prot, MAP_SHARED | MAP_FIXED, fd, (off_t)offset);
    return got == want ? 0 : -errno;
}

int ros_arena_map_fd(uint32_t addr, size_t size, int fd, uint64_t offset)
{
    return map_fd(addr, size, fd, offset, PROT_READ | PROT_WRITE);
}

int ros_arena_map_region(const char *name, uint32_t addr, uint32_t size)
{
    return map_region(name, addr, size);
}

/* ---- application slots -------------------------------------------------- */

/* The slots' guards (ros_slot_guard). Each guard is known by its slot's
 * memory, which is the memfd's device and inode. These stay with the memory
 * when AMB grows it (ftruncate) and when the Wimp adopts it (the fd moved
 * by value). The guard is stored as offsets into that memory.
 * The guard for the slot at &8000 now is cur_lo..cur_hi, with (0, 0) meaning
 * none, so lookups (ros_arena_valid, a fault) cost nothing.
 * There are as many guards as there can be application spaces. That is
 * AMB's 255 nodes (runtime/amb.c, NODES) and the one outside the desktop.
 * Every C application may have a guard (its stack guard, capp.h), as
 * issue 2 required. A limit of 16 would refuse the guard of the 17th
 * task. */
#define GUARDS 256
static struct {
    uint64_t dev, ino;
    uint32_t lo, hi;                    /* hi > lo: in use */
} guards[GUARDS];
static unsigned n_guards;
static uint32_t cur_lo, cur_hi;

static int guard_find(int fd)
{
    struct stat st;
    if (!n_guards || fd < 0 || fstat(fd, &st) != 0)
        return -1;
    for (int i = 0; i < GUARDS; i++)
        if (guards[i].hi > guards[i].lo && guards[i].dev == (uint64_t)st.st_dev &&
            guards[i].ino == (uint64_t)st.st_ino)
            return i;
    return -1;
}

/* The slot just mapped at &8000. Its guard, if it has one, is made
 * inaccessible and current. */
static int guard_apply(const struct ros_slot *s)
{
    cur_lo = cur_hi = 0;
    int i = guard_find(s->fd);
    if (i < 0 || guards[i].hi > s->size)
        return 0;
    if (mprotect(ros_ptr(ROS_APP_BASE + guards[i].lo), guards[i].hi - guards[i].lo, PROT_NONE))
        return -errno;
    cur_lo = ROS_APP_BASE + guards[i].lo, cur_hi = ROS_APP_BASE + guards[i].hi;
    return 0;
}

static void guard_forget(int fd)
{
    int i = guard_find(fd);
    if (i >= 0) {
        guards[i].lo = guards[i].hi = 0;
        n_guards--;
    }
}

int ros_slot_create(struct ros_slot *s, uint32_t size)
{
    s->fd = backing_in("slot", size, 1);
    s->size = size;
    return s->fd < 0 ? s->fd : 0;
}

int ros_stack_create(struct ros_slot *s, uint32_t size)
{
    s->fd = backing("stack", size);
    s->size = size;
    return s->fd < 0 ? s->fd : 0;
}

uint32_t ros_slot_used(const struct ros_slot *s)
{
    struct stat st;
    if (!s || s->fd < 0 || !s->size || fstat(s->fd, &st) != 0)
        return 0;
    uint64_t used = (uint64_t)st.st_blocks * 512u;
    return used > s->size ? s->size : (uint32_t)used;
}

uint64_t ros_slot_pool_free(void)
{
#if defined(__linux__) && !defined(ROS_ARENA_HOSTED)
    struct statfs f;
    if (statfs(ROS_SLOT_POOL, &f) == 0 && f.f_type == TMPFS_MAGIC)
        return (uint64_t)f.f_bavail * (uint64_t)f.f_bsize;
#endif
    return UINT64_MAX;
}

/* ---- windows onto slots (ros_slot_window) ------------------------------
 *
 * A window is part of a slot mapped a second time, MAP_SHARED, at an address
 * outside application space. The Worker module's shared windows use this.
 * The same pages are at both addresses, whichever slot is at &8000.
 * Each window is known by the slot's memory, which is its memfd's device and
 * inode, as the guards are. So a resize of that memory sees the window,
 * whichever struct holds the fd (AMB's node, a task's own, the Wimp's
 * adoption). */
#define WINDOWS 256
static struct {
    uint64_t dev, ino;
    uint32_t addr, off, len;            /* len > 0: in use */
} windows[WINDOWS];
static unsigned n_windows;
void (*ros_slot_window_lost)(uint32_t addr);

static int same_memory(int fd, uint64_t *dev, uint64_t *ino)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
        return 0;
    *dev = (uint64_t)st.st_dev, *ino = (uint64_t)st.st_ino;
    return 1;
}

int ros_slot_window_map(const struct ros_slot *s, uint32_t off, uint32_t len, uint32_t addr)
{
    uint64_t dev, ino;
    if (!s || !len || ((off | len | addr) & 0xFFFu) || off > s->size || len > s->size - off)
        return -EINVAL;
    if (!same_memory(s->fd, &dev, &ino))
        return -EBADF;
    int i = 0;
    while (i < WINDOWS && windows[i].len)
        i++;
    if (i == WINDOWS)
        return -ENOSPC;
    int e = map_fd(addr, len, s->fd, off, PROT_READ | PROT_WRITE);
    if (e) {
        ros_arena_unmap(addr, len);
        return e;
    }
    windows[i].dev = dev, windows[i].ino = ino;
    windows[i].addr = addr, windows[i].off = off, windows[i].len = len;
    n_windows++;
    return 0;
}

void ros_slot_window_unmap(uint32_t addr)
{
    for (int i = 0; i < WINDOWS; i++)
        if (windows[i].len && windows[i].addr == addr) {
            ros_arena_unmap(addr, windows[i].len);
            windows[i].len = 0;
            n_windows--;
            return;
        }
}

uint32_t ros_slot_window_floor(const struct ros_slot *s)
{
    uint64_t dev, ino;
    uint32_t floor = 0;
    if (!n_windows || !s || !same_memory(s->fd, &dev, &ino))
        return 0;
    for (int i = 0; i < WINDOWS; i++)
        if (windows[i].len && windows[i].dev == dev && windows[i].ino == ino &&
            windows[i].off + windows[i].len > floor)
            floor = windows[i].off + windows[i].len;
    return floor;
}

/* The slot's memory is going. The windows' owner is told, so that it stops
 * what writes through them. Then each window is unmapped, so that an inode
 * Linux reuses never inherits a window. */
static void windows_lost(int fd)
{
    uint64_t dev, ino;
    if (!n_windows || !same_memory(fd, &dev, &ino))
        return;
    for (int i = 0; i < WINDOWS; i++)
        if (windows[i].len && windows[i].dev == dev && windows[i].ino == ino) {
            uint32_t addr = windows[i].addr;
            if (ros_slot_window_lost)
                ros_slot_window_lost(addr);
            ros_slot_window_unmap(addr);        /* (if the owner has not) */
        }
}

int ros_slot_resize(struct ros_slot *s, uint32_t size)
{
    /* A window's pages stay, so the slot cannot shrink below the highest
     * window. On Linux a resize is ftruncate on the same memory, so the
     * windows stay valid as it grows. On macOS the resize copies to new
     * memory, which the windows would not see. There a slot with windows
     * keeps its size. */
    uint32_t floor = ros_slot_window_floor(s);
#ifdef __linux__
    if (size < floor)
        return -EBUSY;
#else
    if (floor && size != s->size)
        return -EBUSY;
#endif
#ifdef __linux__
    if (ftruncate(s->fd, (off_t)size) < 0)
        return -errno;
#else
    /* macOS's shared memory takes one size, once. So make a new object and
     * copy the contents across. */
    int fd = backing_in("slot", size, 1);
    if (fd < 0)
        return fd;
    size_t keep = size < s->size ? size : s->size;
    if (keep) {
        void *from = mmap(NULL, keep, PROT_READ, MAP_SHARED, s->fd, 0);
        void *to = mmap(NULL, keep, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (from == MAP_FAILED || to == MAP_FAILED) {
            if (from != MAP_FAILED)
                munmap(from, keep);
            if (to != MAP_FAILED)
                munmap(to, keep);
            close(fd);
            return -ENOMEM;
        }
        /* Copy only the pages that are there. A page never touched reads as
         * zero in the new object too, and copying it would commit it. That
         * would commit all of a 1.5 GB slot. */
        static char vec[4096];
        size_t pg = 4096;
        for (size_t at = 0; at < keep; at += sizeof vec * pg) {
            size_t len = keep - at < sizeof vec * pg ? keep - at : sizeof vec * pg;
            size_t n = (len + pg - 1) / pg;
            int known = mincore((char *)from + at, len, (void *)vec) == 0;
            for (size_t i = 0; i < n; i++)
                if (!known || vec[i]) {
                    size_t o = at + i * pg, l = keep - o < pg ? keep - o : pg;
                    memcpy((char *)to + o, (char *)from + o, l);
                }
        }
        munmap(from, keep);
        munmap(to, keep);
    }
    close(s->fd);
    s->fd = fd;
#endif
    s->size = size;
    if (ros_slot_current == s)
        return ros_slot_map(s);
    return 0;
}

int ros_slot_map(const struct ros_slot *s)
{
    if (s->size > ROS_APP_LIMIT - ROS_APP_BASE)
        return -E2BIG;
    ros_slot_unmap();
    int e = map_fd(ROS_APP_BASE, s->size, s->fd, 0, PROT_CODE);
    if (e == 0) {
        ros_slot_current = s;
        guard_apply(s);                 /* if it cannot be applied, the memory is still there */
    }
    return e;
}

int ros_slot_map_at(const struct ros_slot *s, uint32_t addr)
{
    int e = ros_arena_map_fd(addr, s->size, s->fd, 0);
#if ROS_SVCSTACK_GUARD
    /* A task's SVC stack has a guard page (arena.h). */
    if (!e && addr == ROS_SVCSTACK_BASE &&
        mprotect(ros_ptr(ROS_SVCSTACK_GUARD_AT), ROS_SVCSTACK_GUARD, PROT_NONE) != 0)
        e = -errno;
#endif
    return e;
}

int ros_slot_guard(uint32_t lo, uint32_t hi)
{
    const struct ros_slot *s = ros_slot_current;
    if (!s)
        return -ENOENT;
    if (hi != lo && (((lo | hi) & 0xFFFu) || hi < lo || lo < ROS_APP_BASE ||
                     hi - ROS_APP_BASE > s->size))
        return -EINVAL;
    if (cur_hi > cur_lo)
        mprotect(ros_ptr(cur_lo), cur_hi - cur_lo, PROT_CODE);
    cur_lo = cur_hi = 0;
    guard_forget(s->fd);
    if (hi == lo)
        return 0;
    struct stat st;
    if (fstat(s->fd, &st) != 0)
        return -errno;
    int i = 0;
    while (i < GUARDS && guards[i].hi > guards[i].lo)
        i++;
    if (i == GUARDS)
        return -ENOSPC;
    guards[i].dev = (uint64_t)st.st_dev, guards[i].ino = (uint64_t)st.st_ino;
    guards[i].lo = lo - ROS_APP_BASE, guards[i].hi = hi - ROS_APP_BASE;
    n_guards++;
    int e = guard_apply(s);
    if (e)
        guard_forget(s->fd);
    return e;
}

int ros_slot_in_guard(uint32_t addr)
{
    return addr - cur_lo < cur_hi - cur_lo;
}

int ros_slot_guard_overlaps(uint32_t start, uint32_t end)
{
    return cur_hi > cur_lo && start < cur_hi && end > cur_lo;
}

void ros_slot_unmap(void)
{
    if (!ros_slot_current)
        return;
    /* Put the reservation back. Do not leave a hole for Linux. */
    mmap(ros_ptr(ROS_APP_BASE), ros_slot_current->size, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    ros_slot_current = NULL;
    cur_lo = cur_hi = 0;
}

void ros_slot_destroy(struct ros_slot *s)
{
    if (ros_slot_current == s)
        ros_slot_unmap();
    windows_lost(s->fd);
    guard_forget(s->fd);
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
}
