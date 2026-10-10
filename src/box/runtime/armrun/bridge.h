/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* bridge.h: the ARM container's bridge. It puts an ARM task between the
 * engine (armrun.h) and the runtime (see also ABI.md).
 *
 * An ARM task is one application's ARM code, plus the state that RISC OS
 * keeps for it and the engine does not. That state is the modelled mode (the
 * engine is user mode only), the deliveries queued for it, and the frames of
 * handlers entered on the runtime's behalf. It also includes a small stack of
 * engines, one per nesting depth. A SWI can call back into the task's code (a
 * qsort comparator, a handler) while the engine below is still inside it.
 *
 * The bridge asks three things of the runtime, in the ops table. The first is
 * the SWI dispatcher, which is always called in the X form. The bridge turns
 * a non-X error into the error handler's delivery itself, so nothing ever
 * longjmps through the engine. The second is the environment's handlers. The
 * third is which guest addresses are memory. The box gives ros_swi and the
 * runtime's environment (box.c). The hosted test gives stubs
 * (tests/armrun/hello.c).
 *
 * The errors are RISC OS 5.30's (ABI.md §3a). Every undefined, unpredictable
 * or untranslated instruction gives "undefined instruction" (&80000000). A
 * fetch from no memory gives "abort on instruction fetch" (&80000001). An
 * access to no memory gives "abort on data transfer" (&80000002). Each is
 * delivered to the task's error handler as OS_GenerateError would.
 */
#ifndef ROSGD_ARMRUN_BRIDGE_H
#define ROSGD_ARMRUN_BRIDGE_H

#include <stdint.h>

#include "rosgd/cpu.h"

struct arm_task;

/* The environment handlers the bridge enters (OS_ChangeEnvironment's numbers) */
#define ARM_HANDLER_ERROR 6
#define ARM_HANDLER_EXIT  11

struct arm_ops {
    void *ctx;
    /* A SWI, its number with the X bit always set: the registers in s (the
     * guest's r0-r15 and flags), out the same way; an error is s->v set and
     * s->r[0] the error block's guest address */
    void (*swi)(void *ctx, struct arm_task *t, struct ros_cpu *s, uint32_t number);
    /* An environment handler: 1 and its code, R12 value and buffer, or 0
     * for none (the default: the task ends) */
    int (*handler)(void *ctx, unsigned which, uint32_t *code, uint32_t *r12, uint32_t *buffer);
    /* Whether [addr, addr + size) is memory the task may read (write: and write) */
    int (*valid)(void *ctx, uint32_t addr, uint32_t size, int write);
    /* The task has ended: OS_Exit with no exit handler (rc its code; errnum
     * 0), or an error with no error handler (rc 1; the error) */
    void (*ended)(void *ctx, struct arm_task *t, uint32_t rc, uint32_t errnum, const char *text);
    /* The top of the SVC stack, for deliveries entered in SVC mode */
    uint32_t svc_stack;
    /* The task's floating point state. Its FPA instructions run natively on
     * it (fpa.c), and the native library shares it. If NULL, FPA instructions
     * are undefined instructions */
    struct ros_fp *fp;
    /* A call through a gateway address, or to native code. A gateway address
     * is in [ARM_GATE_LO, ARM_GATE_HI) and the bridge never fetches it as
     * code. It is an entry of the native library that an ARM client's stub
     * vectors point at (box.c). The native op below says which other
     * addresses are native code. s holds the registers at the call (R14 is
     * the return address). Returns 1 when it was a gateway or native code,
     * with s then holding the registers it returns with (for a native call,
     * R15 is where the ARM code goes on, normally its R14). Returns
     * ARM_GATE_RAISED when the native code raised an error, with s->r[0] the
     * error block's guest address. That error is delivered as a non-X SWI's
     * error is. Returns 0 when there is nothing there (an abort on
     * instruction fetch). */
    int (*gate)(void *ctx, struct arm_task *t, uint32_t addr, struct ros_cpu *s);
    /* A safe point, about every BUDGET instructions of a task that makes no
     * SWI, for the runtime's background work (ROS_POLL's, cpu.h). That work
     * is a key, an Escape, or a TaskWindow's time slice */
    void (*poll)(void *ctx, struct arm_task *t);
    /* Whether addr is native code, and not the task's ARM code. Native code
     * is compiled code, a C program's, native code that BASIC assembled, or a
     * native entry. ARM code that branches there leaves the engine, and the
     * gate op calls it with the ARM registers (#146). This is asked when the
     * address is translated and again at each call, so an address that stops
     * being native is fetched as ARM from then on. If NULL, no address is
     * native. */
    int (*native)(void *ctx, uint32_t addr);
    /* A fault in the task's ARM code, called before it is raised as the
     * task's error, so that it can be reported. what is the error's words
     * ("abort on data transfer"). r holds the registers at the faulting
     * instruction (r[15] is its address). cpsr, mode and depth give the
     * modelled state and the nesting depth. addr, size and write describe the
     * access that was refused (for a data abort; otherwise addr is the PC and
     * size is 0). If NULL, there is no report. */
    void (*fault)(void *ctx, struct arm_task *t, const char *what, const uint32_t r[16],
                  uint32_t cpsr, uint32_t mode, unsigned depth, uint32_t addr, unsigned size,
                  int write);
};

/* The R13 of the code running now (its depth's engine) and the modelled
 * mode it is in */
uint32_t arm_task_sp(const struct arm_task *t, uint32_t *mode);

/* Whether the task runs calls only (arm_task_create_calls) */
int arm_task_calls_only(const struct arm_task *t);

#define ARM_GATE_RAISED 2           /* the gate op's: the native code raised an error */

#define ARM_GATE_LO 0xFFE00000u     /* gateways: never memory (arena.h's map) */
#define ARM_GATE_HI 0xFFE10000u

/* A task over guest memory at base (the arena's origin; 0 in the box) */
struct arm_task *arm_task_create(uintptr_t base, const struct arm_ops *ops);
void arm_task_destroy(struct arm_task *t);

/* A task for calls only. It runs ARM code that the runtime calls when no ARM
 * application is running: a module's entries, a vector claimant, a utility.
 * Only arm_task_call is given to it. An error in the code would go to an
 * application's error handler. Here it ends the calls instead, and
 * arm_task_last_error says what it was. */
struct arm_task *arm_task_create_calls(uintptr_t base, const struct arm_ops *ops);
uint32_t arm_task_last_error(const struct arm_task *t, const char **text);

/* Run the task's code from the registers in s until it ends, by OS_Exit or
 * by an error with no handler. r15 is the entry. s->mode is the mode, USR for
 * an application. R14 is the bridge's, and a return to it exits with R0-R2
 * zero, as from an Absolute. Returns the return code. */
uint32_t arm_task_run(struct arm_task *t, const struct ros_cpu *s);

/* Call the task's code at addr with s's registers, from inside a SWI the
 * task made or native code it called. A nested engine runs it, one for each
 * depth, up to ARM_DEPTH engines in all. It returns when the code returns to
 * the R14 that the bridge gives it, with s holding the registers then.
 * Returns 0, or the error that ended the call. The callee's error is
 * delivered to the task's handler, and that ends the call too. */
#define ARM_DEPTH 8
uint32_t arm_task_call(struct arm_task *t, uint32_t addr, struct ros_cpu *s);

/* A transient callback (OS_AddCallBack's). It is entered in SVC mode at the
 * task's next safe point with R12 = r12, and returns to where the task was.
 * It can be called from the task's own thread or from another. The engine is
 * halted to take it. */
void arm_task_add_callback(struct arm_task *t, uint32_t code, uint32_t r12);

/* From inside a SWI the task made: when the SWI returns, the task goes on
 * from the 17-word register block at the guest address block (R0-R15, then
 * the PSR) rather than after the SWI. This is the way back to user mode that
 * the runtime's tasks take (ros_task_set_user_block; ABI.md §2.8) */
void arm_task_resume_from_block(struct arm_task *t, uint32_t block);

/* OS_SynchroniseCodeAreas: the translations of [addr, addr + len) go, in
 * every engine of the task (len 0: all of them) */
void arm_task_synchronise(struct arm_task *t, uint32_t addr, uint32_t len);

/* The same in every task. Code that has changed (a module loaded where
 * another was, OS_SynchroniseCodeAreas) is shared by all of them */
void arm_task_synchronise_all(uint32_t addr, uint32_t len);

/* What the task has done: instructions run (about), SWIs, deliveries */
struct arm_task_counts {
    uint64_t ticks;
    uint32_t swis, deliveries, callbacks, calls, max_depth;
    uint32_t natives;               /* calls from ARM code to native code */
};
const struct arm_task_counts *arm_task_counts(const struct arm_task *t);

/* What the container has cost, for every task and thread together, since the
 * start or arm_stats_reset. It counts time in the engine (ARM code), in
 * native code that it called (SWIs, gateways), and in the native FPA. It also
 * counts instructions (about), SWIs, and calls into ARM code from native
 * code. The PC is sampled at safe points (about every millisecond of ARM
 * code). The samples are kept as 16-byte ranges with their counts, the
 * largest first. */
struct arm_stats {
    uint64_t ns_engine, ns_native, ns_fpa, ticks, swis, calls, samples;
    uint64_t ns_wall;                   /* since arm_stats_reset (0 if never) */
};
void arm_stats_read(struct arm_stats *st);
/* The ARM tasks there are: applications' and those for calls only */
void arm_tasks_count(unsigned *apps, unsigned *calls);
void arm_stats_reset(void);
unsigned arm_stats_samples(uint32_t *addr, uint32_t *count, unsigned max);

#endif
