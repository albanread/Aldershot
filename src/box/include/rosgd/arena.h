/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* arena.h -- RISC OS's 32-bit logical map, kept, inside a 64-bit process.
 *
 * Compiled ObjAsm keeps 32-bit values in 32-bit registers, and decides
 * identity by address all over the corpus. An error block is "in ROM" if it
 * lies above &FC000000, and a window handle is an address plus one. So
 * ROSGD does more than put RISC OS memory somewhere below 4 GB. It
 * reproduces RISC OS 5's map there exactly, and every address assumption in
 * compiled code stays true without anyone having to find it.
 *
 * In the guest the arena sits at address 0. A RISC OS address, zero-extended,
 * *is* the host pointer, and ros_ptr() compiles to nothing. Host unit tests
 * run on macOS, which keeps the whole low 4 GB for itself, so there the arena
 * is a reservation elsewhere and ros_ptr() adds its base (ROS_ARENA_HOSTED).
 */
#ifndef ROSGD_ARENA_H
#define ROSGD_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- the map ------------------------------------------------------------
 *
 * RISC OS 5's, from Kernel/hdr/KernelWS. The hardware regions (IO space,
 * page tables, the CAM, the physical access window and HAL workspace) are
 * never mapped, because their users are the components that Linux
 * replaces.
 */
/* DebuggerSpace: the page the Debugger module is given below application
 * space (Kernel/hdr/KernelWS, with high processor vectors: "Debugger gets
 * a page all to itself!"). The box maps it because RISC OS has it there.
 * A program that hands the OS a pointer into it then reads zeroes, as
 * on 5.30, instead of taking the task down. That was #180, where an ARM
 * module's icon carried a validation pointer of &2370. &1000-&2000 and
 * &3000-&4000 stay unmapped, as 5.30 leaves them. */
#define ROS_DEBUGGER_BASE 0x00002000u   /* DebuggerSpace: the Debugger module's page */
#define ROS_DEBUGGER_SIZE 0x00001000u   /* ... one page (Kernel/hdr/KernelWS) */
/* The hosted build goes without it, as it goes without the SVC stack's
 * guard page below. macOS on Apple Silicon has 16K pages, so &2000 is not
 * a boundary mmap will take, and the 16K it would have to map instead
 * reaches address 0, where a null pointer must keep faulting. */
#ifdef ROS_ARENA_HOSTED
#define ROS_DEBUGGER_MAPPED 0
#else
#define ROS_DEBUGGER_MAPPED 1
#endif
#define ROS_SCRATCH_BASE  0x00004000u   /* ScratchSpace: the kernel's public scratch area */
#define ROS_SCRATCH_SIZE  0x00004000u   /* ... 16K, to application space (Kernel/hdr/PublicWS) */
/* Application space: the current task's slot, up to 1.5 GB. It is kept
 * below 2 GB, so that a slot's top and every size in it stay positive in a
 * signed 32-bit word (BASIC's HIMEM, programs' own arithmetic). A slot is
 * lazy (arena.c): its pages are memory only once touched, so every task is
 * given all of it unless it asks for less (task.h, ros_slot_next_size).
 * What 1.5 GB risks is 26-bit APIs that keep flags in an address's top
 * bits (OS_ReadLine's R0 bits 31 and 30, OS_HeapSort's R1 bits 31 to 29).
 * They misread a buffer above &40000000 (or an array above &20000000) in
 * the slot, as a program's stack at its top is. Their 32-bit forms do not.
 * BASIC (INPUT reads into its own workspace, low down), the task window's
 * command line and the C library are not affected. A program that is can
 * be given less with *WimpSlot -max. */
#define ROS_APP_BASE      0x00008000u   /* application space: the current task's slot */
#define ROS_APP_LIMIT     0x60000000u   /* ... up to 1.5 GB */
#define ROS_RMA_BASE      0x60000000u   /* the relocatable module area */
#define ROS_RMA_SIZE      0x10000000u   /* ... up to 256 MB */
#define ROS_SYSHEAP_BASE  0x70000000u   /* the system heap */
#define ROS_SYSHEAP_SIZE  0x02000000u   /* ... up to 32 MB */
#define ROS_SVCSTACK_BASE 0x72000000u   /* the SVC stack compiled code runs on */
#define ROS_SVCSTACK_SIZE 0x00100000u   /* 1 MB, aligned as TaskWindow assumes */
/* Its second page, inaccessible in the box. Code running away down the SVC
 * stack, such as a nested entry's x32 code or a compiled module's
 * recursion, faults there (runtime/fault.c) rather than running into the
 * system heap below. (Hosted, there is none.) It is not the lowest page,
 * because as in RISC OS, SharedCLibrary keeps its client modules' words
 * there, at the 1 MB-aligned base of the stack a module's code runs on
 * (sl = the base + 560; the emulated library of the ARM container). */
#ifdef ROS_ARENA_HOSTED
#define ROS_SVCSTACK_GUARD 0u
#else
#define ROS_SVCSTACK_GUARD 0x1000u
#endif
#define ROS_SVCSTACK_GUARD_AT (ROS_SVCSTACK_BASE + 0x1000u)
/* &72200000-&78000000: the C ROM, the ROM C library's image and the
 * self-test's stand-in libraries (capp.h, ROS_CROM_BASE) */
#define ROS_DA_BASE       0x78000000u   /* dynamic areas, allocated upward to the screen */
#define ROS_SCREEN_BASE   0xB4000000u   /* screen memory: the DRM dumb buffer */
#define ROS_SCREEN_SIZE   0x04000000u   /* ... up to 64 MB */
#define ROS_DA_LIMIT      0xB9000000u   /* where IO space began; never mapped */
#define ROS_ROM_BASE      0xFC000000u   /* the ROM image: compiled modules' areas */
#define ROS_ROM_SIZE      0x03000000u
#define ROS_NATIVE_BASE   0xFEF00000u   /* native entries: ROM addresses that stand
                                           for C functions (ros_native_entry) */
#define ROS_ZEROPAGE      0xFFFF0000u   /* public kernel workspace at its offsets */
#define ROS_ZEROPAGE_SIZE 0x00008000u

/* Zero-page words that compiled code reads at fixed offsets (Kernel/hdr/
 * PublicWS), which the runtime keeps current. */
#define ROS_ZP_IRQSEMA    (ROS_ZEROPAGE + 0x108)  /* non-zero in background work */
#define ROS_ZP_METROGNOME (ROS_ZEROPAGE + 0x10C)  /* centisecond counter */
#define ROS_ZP_RETURN_CODE (ROS_ZEROPAGE + 0xAC4) /* Sys$ReturnCode's value (KernelWS ReturnCode) */
#define ROS_ZP_RC_LIMIT   (ROS_ZEROPAGE + 0xAC8)  /* Sys$RCLimit's (RCLimit) */
/* SharedCLibrary's (Kernel/hdr/KernelWS: "their positions are assumed by
 * other modules"): the tmpnam counter, a byte; RISC_OSLib's word and the C
 * library's, the latter the CLib messages' descriptor, shared by the
 * module and every client's library code (k_body's open_messagefile) */
#define ROS_ZP_CLIBCOUNTER   (ROS_ZEROPAGE + 0xFE8)
#define ROS_ZP_RISCOSLIBWORD (ROS_ZEROPAGE + 0xFEC)
#define ROS_ZP_CLIBWORD      (ROS_ZEROPAGE + 0xFF0)
#define ROS_ZP_DOMAINID   (ROS_ZEROPAGE + 0xFF8)  /* the current Wimp task */
/* runtime-owned: error blocks handed to compiled code must be arena memory */
#define ROS_ZP_ERRBUFS    (ROS_ZEROPAGE + 0x6000)
#define ROS_ZP_ERRBUF_N   8

#ifdef ROS_ARENA_HOSTED
extern uintptr_t ros_arena_base;
#else
#define ros_arena_base ((uintptr_t)0)
#endif

/* An arena address as a host pointer. */
static inline void *ros_ptr(uint32_t a)
{
    return (void *)(ros_arena_base + (uintptr_t)a);
}

/* A host pointer as an arena address. A pointer outside the arena reaching
 * compiled code is a bug in whoever passed it, so this traps rather than
 * truncating. */
uint32_t ros_addr(const void *p);

/* Whether a host pointer lies in the arena at all. */
static inline int ros_in_arena(const void *p)
{
    uintptr_t u = (uintptr_t)p;
    return u >= ros_arena_base + ROS_APP_BASE &&
           u < ros_arena_base + 0x100000000ull;
}

/* Loads and stores as compiled code performs them.  memcpy, so an unaligned
 * access behaves as ARMv7's does with alignment checking off; clang turns
 * each into a single move. */
static inline uint32_t ros_ld32(uint32_t a) { uint32_t v; memcpy(&v, ros_ptr(a), 4); return v; }
static inline uint32_t ros_ld16(uint32_t a) { uint16_t v; memcpy(&v, ros_ptr(a), 2); return v; }
static inline uint32_t ros_ld8(uint32_t a)  { return *(const uint8_t *)ros_ptr(a); }
static inline void ros_st32(uint32_t a, uint32_t v) { memcpy(ros_ptr(a), &v, 4); }
static inline void ros_st16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(ros_ptr(a), &h, 2); }
static inline void ros_st8(uint32_t a, uint32_t v)  { *(uint8_t *)ros_ptr(a) = (uint8_t)v; }
static inline uint64_t ros_ld64(uint32_t a) { uint64_t v; memcpy(&v, ros_ptr(a), 8); return v; }
static inline void ros_st64(uint32_t a, uint64_t v) { memcpy(ros_ptr(a), &v, 8); }

/* ---- setting it up ------------------------------------------------------- */

/* Reserve the whole arena and map its fixed regions.  0, or -errno. */
int ros_arena_init(void);

/* Map a host file descriptor (a DRM dumb buffer, say) at a fixed arena
 * address.  0, or -errno. */
int ros_arena_map_fd(uint32_t addr, size_t size, int fd, uint64_t offset);

/* Map zeroed memory of the arena's own at a fixed address (a dynamic area,
 * say).  0, or -errno. */
int ros_arena_map_region(const char *name, uint32_t addr, uint32_t size);

/* Put the reservation back over part of the arena: what was mapped there
 * is gone (OS_AMBControl's mapsome, mapped out). */
void ros_arena_unmap(uint32_t addr, uint32_t size);

/* ---- application slots ----------------------------------------------------
 *
 * A slot is a memfd. Mapping one in is a single mmap at &8000, exactly
 * OS_AMBControl 3, "mapslot", with Linux doing the demand paging that AMB's
 * lazy map-in did by hand.
 */
/* Where the box's slot pool is mounted (boot/main.c) */
#define ROS_SLOT_POOL "/run/slots"

struct ros_slot {
    int fd;
    uint32_t size;
};

/* An application slot: lazy, of any size up to the map's, with its pages
 * from the slot pool where the box has one (arena.c). A page that Linux
 * cannot supply when it is touched is a bus error there, which fault.c
 * makes a data abort of the program's and not the death of the box. */
int ros_slot_create(struct ros_slot *s, uint32_t size);
int ros_slot_resize(struct ros_slot *s, uint32_t size);
/* A region of the same kind outside the pool: a task's SVC stack */
int ros_stack_create(struct ros_slot *s, uint32_t size);
/* The bytes of it in use: the pages ever touched (st_blocks), at most its
 * size. It is what the Task Manager shows for a task */
uint32_t ros_slot_used(const struct ros_slot *s);
/* The memory the slot pool can still give, in bytes (UINT64_MAX: no pool,
 * Linux's own free memory is the limit) */
uint64_t ros_slot_pool_free(void);
int ros_slot_map(const struct ros_slot *s);   /* at ROS_APP_BASE */
/* Any memfd region at a fixed address: a task's SVC stack (task.h). */
int ros_slot_map_at(const struct ros_slot *s, uint32_t addr);
void ros_slot_unmap(void);
void ros_slot_destroy(struct ros_slot *s);
extern const struct ros_slot *ros_slot_current;
/* A guard in the slot mapped at &8000, which is an application's stack guard
 * (capp.h, ros_capp_stack): [lo, hi), arena addresses, page aligned and
 * inside it, made inaccessible now and whenever that memory is mapped at
 * &8000 again (a task switch, AMB growing it, the Wimp adopting it: the
 * guard is the memory's, known by its memfd's inode); lo == hi takes it
 * away, as destroying the slot does. The result is 0, or -errno (-EINVAL: not page
 * aligned or not inside the slot; -ENOENT: no slot mapped; -ENOSPC: too
 * many slots have one). */
int ros_slot_guard(uint32_t lo, uint32_t hi);
/* Whether addr is in the guard of the slot mapped at &8000 now; whether
 * [start, end) meets it */
int ros_slot_in_guard(uint32_t addr);
int ros_slot_guard_overlaps(uint32_t start, uint32_t end);
/* Windows onto a slot: [off, off + len) of its memory mapped again at
 * addr, read/write and shared. They are the same pages as at &8000 + off,
 * whoever runs (the Worker module's ShareMemory). All are page aligned and
 * inside the slot, and addr is address space that the caller reserved
 * (dynarea.c, ros_dynarea_window_reserve). The result is 0, or -errno.
 * Unmapping puts the reservation back. The floor is the most a slot's
 * windows reach, in bytes from its start (0: none). ros_slot_resize
 * refuses (-EBUSY) to shrink a slot below it. Hosted on macOS, where a
 * resize copies to new memory, it refuses to resize a slot with windows at
 * all. Destroying a slot calls ros_slot_window_lost for each window on it,
 * then unmaps it. */
int ros_slot_window_map(const struct ros_slot *s, uint32_t off, uint32_t len, uint32_t addr);
void ros_slot_window_unmap(uint32_t addr);
uint32_t ros_slot_window_floor(const struct ros_slot *s);
extern void (*ros_slot_window_lost)(uint32_t addr);
/* The SVC stack /init starts on: task 0's (task.h). */
extern struct ros_slot ros_svcstack_initial;

#endif
