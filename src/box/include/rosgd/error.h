/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* error.h -- RISC OS errors, and the two ways a call reports one.
 *
 * Every typed call has two forms, RISC OS's X bit made visible:
 * the x form returns an error pointer, NULL on success. The plain form
 * raises it, as a non-X SWI does, by a non-local exit to the current error
 * handler, which in 32-bit RISC OS also flattens every privileged stack.
 */
#ifndef ROSGD_ERROR_H
#define ROSGD_ERROR_H

#include <setjmp.h>
#include <stdint.h>

typedef struct os_error {
    uint32_t errnum;
    char errmess[252];
} os_error;

/* Error numbers the runtime itself reports: RISC OS's own, from
 * hdr/NewErrors, so that code testing for them keeps working. */
#define ROS_ERR_BAD_ADDRESS      0x0FCu   /* "Bad address" */
#define ROS_ERR_NO_ROOM_IN_RMA   0x101u   /* "No room in RMA" */
#define ROS_ERR_WIMP_CANT_KILL   0x104u   /* the Wimp's WimpCantKill, which ShellCLI
                                             and TaskWindow refuse Wimp_Initialise
                                             with (the one after CantKill2, whose
                                             number is CantKill's, &103) */
#define ROS_ERR_RM_HEADER        0x10Du   /* "Illegal header field in module",
                                             and "not 32-bit compatible" */
#define ROS_ERR_NOT_A_HEAP_BLOCK 0x185u   /* "Not a heap block" */
#define ROS_ERR_NO_SUCH_SWI      0x1E6u   /* "SWI &%0 not known" */
#define ROS_ERR_UNIMPLEMENTED    0x1E7u   /* "This function or procedure
                                             unimplemented" */

/* An error block in arena memory. Compiled code receives it in R0, so it
 * must be an address compiled code can read. Blocks rotate through a small
 * pool, as MessageTrans's buffers do: valid until several more errors have
 * been made.  It is never null: the pool is a fixed place in zero page, and
 * the whole tree writes "return ros_error(...)" where an error is due and
 * tests the result for null to mean there was none. It is said here so that
 * the analyser knows it too. Without it, every "if (e) return e" looks like
 * a path that carries on with nothing set (clang --analyze on SpriteExtend
 * reported eleven such). */
os_error *ros_error(uint32_t errnum, const char *fmt, ...)
    __attribute__((format(printf, 2, 3))) __attribute__((returns_nonnull));

/* ---- raising ----------------------------------------------------------- */

struct ros_handler {
    jmp_buf jb;
    const os_error *error;      /* set when an error arrives */
    uint32_t svc_sp;            /* the SVC stack as it was: raising flattens
                                   it back to here, as RISC OS's does */
    uint32_t call_depth;        /* SWI nesting as it was (background.h) */
    struct ros_handler *prev;
};

void ros_handler_push(struct ros_handler *h);
void ros_handler_pop(struct ros_handler *h);

/* Each task has its own chain of handlers: the task switch puts one in
 * place and takes the other out (task.h).  Returns the chain it replaced. */
struct ros_handler *ros_handler_chain(struct ros_handler *chain);

/* Raise: to the innermost handler, as a non-X SWI's error reaches the
 * task's error handler.  With none, the runtime reports it and stops. */
__attribute__((noreturn)) void ros_raise(const os_error *e);

/* The same, the error's pc given: what the program's error handler finds
 * in its buffer's first word, as the kernel puts the aborting pc there
 * (fault.h).  ros_raise's errors give 0.  ros_error_pc_take is the error
 * handler's side: the pc, once, then 0 again. */
__attribute__((noreturn)) void ros_raise_at(const os_error *e, uint32_t pc);
uint32_t ros_error_pc_take(void);

/* Run a block with an error handler:
 *
 *     struct ros_handler h;
 *     if (ROS_TRY(&h)) { ...; ros_handler_pop(&h); }
 *     else             { ... h.error ... }
 */
#define ROS_TRY(h) (ros_handler_push(h), setjmp((h)->jb) == 0)

#endif
