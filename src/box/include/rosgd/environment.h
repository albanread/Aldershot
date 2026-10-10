/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* environment.h -- the environment handlers: the program's own part of the
 * machine.
 *
 * RISC OS keeps, in zero page, what the current program has said about
 * itself: where its memory ends, the code to call on an error, an exit, an
 * escape, an event, an UpCall, a callback, an exception, and the buffers
 * those use (Kernel/hdr/EnvNumbers; the words from Kernel/hdr/KernelWS).
 * OS_ChangeEnvironment reads and sets them, through ChangeEnvironmentV,
 * and the Wimp swaps them at every task switch.
 *
 * ROSGD keeps them where RISC OS does, in zero page at the kernel's own
 * offsets, because compiled code reads them there. The task switch saves
 * one task's and puts in the next's (task.h), as the Wimp did.
 */
#ifndef ROSGD_ENVIRONMENT_H
#define ROSGD_ENVIRONMENT_H

#include <setjmp.h>
#include <stdint.h>

#include "rosgd/error.h"

struct ros_cpu;

enum {
    ROS_ENV_MEMORY_LIMIT, ROS_ENV_UNDEFINED, ROS_ENV_PREFETCH_ABORT, ROS_ENV_DATA_ABORT,
    ROS_ENV_ADDRESS_EXCEPTION, ROS_ENV_OTHER_EXCEPTION, ROS_ENV_ERROR, ROS_ENV_CALLBACK,
    ROS_ENV_BREAKPOINT, ROS_ENV_ESCAPE, ROS_ENV_EVENT, ROS_ENV_EXIT, ROS_ENV_UNUSED_SWI,
    ROS_ENV_EXCEPTION_REGISTERS, ROS_ENV_APPLICATION_SPACE, ROS_ENV_CAO, ROS_ENV_UPCALL,
    ROS_ENV_HANDLERS
};

/* Every zero-page word the handlers occupy, as one task's copy. */
#define ROS_ENV_WORDS 27
struct ros_environment {
    uint32_t w[ROS_ENV_WORDS];
};

/* A program's defaults: RISC OS's default handlers, with its application
 * space ending at app_end and its current active object at cao. */
void ros_env_defaults(struct ros_environment *e, uint32_t app_end, uint32_t cao);
/* Whether code is the default for handler n (OS_ReadDefaultHandler's). */
int ros_env_is_default(uint32_t n, uint32_t code);

/* Zero page to and from a task's copy. */
void ros_env_save(struct ros_environment *e);
void ros_env_set_memory_limit(uint32_t end);
void ros_env_load(const struct ros_environment *e);

/* What OS_ChangeEnvironment's default does (the kernel's AdjustOurSet):
 * each of R1-R3 that is non-zero and names a field the handler has is
 * set; each field it has comes back, as it was, in R1-R3. */
os_error *ros_env_change(uint32_t n, uint32_t *r1, uint32_t *r2, uint32_t *r3);

/* The escape condition set (1) or cleared (0), and the program's escape
 * handler told, as the kernel's OS_Byte 125 and 124 do: every Escape the
 * OS raises or clears goes through here. */
void ros_env_escape(int set);

/* The current program's handler n: its code, R12 and buffer. */
void ros_env_read(uint32_t n, uint32_t *code, uint32_t *r12, uint32_t *buffer);

/* An error no C handler catches: to the program's error handler, with the
 * error in its buffer, as RISC OS raises to it.  Does not return. */
__attribute__((noreturn)) void ros_env_raise(const os_error *e);

/* End the current program as the default handlers do: its task, or the
 * application a command entered (back to the command), or /init and with
 * it the box.  status: 0 an exit, 1 an error.  A C application's delivered
 * handler that returns ends it so (capp.c). */
__attribute__((noreturn)) void ros_env_end_program(int status);

/* Where ros_env_raise unwinds to: the first error of a program leaves its
 * frame as a base, and each later error unwinds to it before entering the
 * handler again, so the handler's reruns do not nest.  An application has
 * a base of its own, in its own frames: entering one swaps in a fresh one,
 * and its end swaps the caller's back (module.c). */
struct ros_env_base {
    jmp_buf jb;
    int active;
    uint32_t svc_sp, call_depth;
    const void *frame;
};
void ros_env_base_swap(struct ros_env_base *other);
/* The frames below `to` are being left (dispatch.c): a base in them is
 * dropped, so an error never unwinds to a frame that has returned */
void ros_env_base_unwind(const void *to);

/* The program's R10-R12 where it last called the OS (its outermost SWI)
 * or where compiled code failed outside one. The kernel enters the error
 * handler with these, recovered from the top of the SVC stack
 * (Kernel/s/Kernel, ErrHandler). BASIC finds the line in error by its
 * R11 and R12. */
void ros_env_foreground(const struct ros_cpu *s);
/* All the program's registers as it made its outermost SWI (R14 is where
 * the gate's call returns, for x32 code), into r. The result says whether
 * there are any (an exception in a SWI, fault.h) */
int ros_env_foreground_regs(uint32_t r[16]);

#endif
