/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* unixbridge.h -- the Unix bridge's interface: what a POSIX
 * application's C library and the UnixBridge module (modules/unixbridge)
 * agree on.  Plain C, for both sides: the runtime's LP64 C and a program's
 * x32 musl (rosgd/posix).
 *
 * ---- the gate ------------------------------------------------------------------
 *
 * SWI Unix_Syscall (&C0100):
 *     R0  the call's number: x86-64's, with UNIX_X32_BIT set when its
 *         structures are x32's (a POSIX application's always are; the 36
 *         x32-only numbers, 512 to 547, have it set by definition);
 *         UNIX_ROSGD_BASE and up are ROSGD's own (below)
 *     R1  the arena address of the six arguments, each 64-bit
 * On exit
 *     R0  the result's low word, R1 its high word: the kernel's result,
 *         -errno (-4095 to -1) on failure
 * The SWI never returns an error of its own for a call it knows: a call it
 * does not is -ENOSYS, and the first such of each number is reported on the
 * serial console.  A pointer argument is an arena address; zero-extended, it
 * is the host address (arena.h: the arena is at 0 in the box).
 *
 * BASIC and other languages reach Unix the same way, with or without the
 * x32 bit: without it a call's structures are x86-64's, which BASIC can lay
 * out as well as x32's.
 */
#ifndef ROSGD_UNIXBRIDGE_H
#define ROSGD_UNIXBRIDGE_H

#include <stdint.h>

#define UNIX_SWI_CHUNK   0xC0100u
#define UNIX_SWI_SYSCALL 0xC0100u              /* Unix_Syscall */
#define UNIX_X32_BIT     0x40000000u

/* ROSGD's own calls, above anything Linux numbers */
#define UNIX_ROSGD_BASE  1024u
#define UNIX_ROSGD_INIT  (UNIX_ROSGD_BASE + 0)  /* a POSIX program starting: below */

/* UNIX_ROSGD_INIT's one argument: the arena address of this, in the
 * program's memory.  The bridge makes the program's run a process (its
 * descriptor table with 0, 1 and 2 on the console, its current directory
 * the CSD's Linux directory, its umask 022) and gives it a heap. The heap
 * is a dynamic area of its own, named and sized as UnixLib's programs name
 * theirs (__dynamic_da_name and __dynamic_da_max_size, if the program
 * defines them), holding its stack at the bottom, then brk's reach, then
 * what mmap gives. It is never application space, which each task switch
 * remaps whole.
 * Process ids are above Linux's pid_max, so a program's own never collides
 * with a Linux child's. */
struct unix_init {
    uint32_t version;           /* in: 1 */
    uint32_t image_end;         /* in: the image's end (_end), bss included */
    uint32_t stack_size;        /* in: the stack wanted */
    uint32_t name;              /* in: the program's name (argv[0]), or 0 */
    uint32_t heap_name;         /* in: its heap area's name (__dynamic_da_name), or 0 */
    uint32_t heap_size;         /* in: its most (__dynamic_da_max_size), or 0 */
    uint32_t stack_top;         /* out: where the stack starts, 16-byte aligned */
    uint32_t heap_base;         /* out: the heap's dynamic area: its base */
    uint32_t heap_max;          /* out: and how far it may grow */
    uint32_t pid;               /* out: the task's process id */
};
#define UNIX_INIT_VERSION 1u
#define UNIX_PID_BASE 0x800000u          /* above Linux's largest pid_max */

#endif
