/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_environment.c: the environment handlers, against
 * environment.h and the kernel's OS_ChangeEnvironment (Kernel/s/Middle).
 *
 * The handlers are tested through the SWIs, as programs use them, and in
 * zero page, where compiled code reads them.  Then they are tested per
 * task.  Each task's handlers move with the baton.  An error that nothing
 * catches reaches the task's own error handler, with the error in its
 * buffer.  OS_Exit reaches the task's exit handler.
 */
#include <setjmp.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define ZP_ERRHAN (ROS_ZEROPAGE + 0x130)
#define ZP_MEMLIMIT (ROS_ZEROPAGE + 0x11C)
#define ZP_CAO (ROS_ZEROPAGE + 0x7D4)

/* OS_ChangeEnvironment through the SWI; V and R0-R3 back. */
static int change(uint32_t r[4])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 4 * sizeof r[0]);
    ros_swi(&s, XOS_ChangeEnvironment);
    memcpy(r, s.r, 4 * sizeof r[0]);
    return s.v;
}

/* Handlers a task installs, and what they saw. */
static struct {
    unsigned error_calls, exit_calls;
    uint32_t error_wp, exit_r12, errnum, error_mode, exit_mode, exit_r[3], exit_r10, exit_r11;
    char errmess[64];
    uint32_t buf;
    uint32_t a_memlimit, a_errhan_kept;
    int a_resumed_after_raise;
} seen;

static void error_handler(struct ros_cpu *s)
{
    seen.error_calls++;
    seen.error_wp = s->r[0];            /* R0 its workspace; R10-R12 the program's (ErrHandler) */
    seen.error_mode = s->mode;
    seen.errnum = ros_ld32(seen.buf + 4);
    strncpy(seen.errmess, ros_ptr(seen.buf + 8), sizeof seen.errmess - 1);
    s->r[15] = s->r[14];                /* returns: the program ends */
}

static void exit_handler(struct ros_cpu *s)
{
    seen.exit_calls++;
    seen.exit_r12 = s->r[12];
    seen.exit_mode = s->mode;
    memcpy(seen.exit_r, s->r, sizeof seen.exit_r);
    seen.exit_r10 = s->r[10], seen.exit_r11 = s->r[11];
    s->r[15] = s->r[14];
}

static unsigned vector_calls;
static int changev(struct ros_cpu *s, uint32_t r12)
{
    (void)s, (void)r12;
    vector_calls++;
    return ROS_VECTOR_PASS;
}

static struct ros_task *zero;

/* Task A: its own error handler; switches away and back; then raises with
 * nothing to catch it. */
static void task_a(void *arg)
{
    uint32_t code = *(uint32_t *)arg;
    seen.a_memlimit = ros_ld32(ZP_MEMLIMIT);
    uint32_t r[4] = { ROS_ENV_ERROR, code, 0xA11CE, seen.buf };
    change(r);
    ros_task_switch(zero);
    seen.a_errhan_kept = ros_ld32(ZP_ERRHAN) == code;
    ros_raise(ros_error(0x123, "A's unhandled error"));
    seen.a_resumed_after_raise = 1;     /* never */
}

/* Task B: its own exit handler, then OS_Exit. */
static void task_b(void *arg)
{
    uint32_t r[4] = { ROS_ENV_EXIT, *(uint32_t *)arg, 0xB0B, 0 };
    change(r);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0x9000, s.r[1] = 0x58454241u, s.r[2] = 7;     /* "ABEX", return code 7 */
    s.r[10] = 0x10AA, s.r[11] = 0x11BB;
    ros_swi(&s, XOS_Exit);
}

/* Task C: the defaults: OS_Exit ends it quietly. */
static void task_c(void *arg)
{
    (void)arg;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, XOS_Exit);
}

/* An error handler that fails: each entry raises again, from inside
 * itself, until the 200th, which returns.  Every entry should be at the
 * same depth of C stack, flattened as the kernel resets its stacks. */
static struct {
    unsigned entries;
    uintptr_t first_depth, max_depth;
} again;

static void again_handler(struct ros_cpu *s)
{
    volatile char here;
    uintptr_t depth = (uintptr_t)&here;
    if (again.entries++ == 0)
        again.first_depth = depth;
    uintptr_t d = again.first_depth > depth ? again.first_depth - depth : depth - again.first_depth;
    if (d > again.max_depth)
        again.max_depth = d;
    if (again.entries < 200)
        ros_raise(ros_error(0x124, "Again"));
    s->r[15] = s->r[14];
}

static void task_d(void *arg)
{
    uint32_t r[4] = { ROS_ENV_ERROR, *(uint32_t *)arg, 0, seen.buf };
    change(r);
    ros_raise(ros_error(0x124, "First"));
}

/* Task F: the handler jumps out of the error, to a resume point the task
 * set before it, as BASIC's does when an FN with an ON ERROR LOCAL
 * returns to EXPR's call of FACTOR.  This leaves ros_env_raise's frame, the
 * error base, behind.  A second error, raised shallow, must enter the
 * handler from a live frame (#65): with the stale base it longjmped down
 * into the first error's dead frames, as deep as the first entry. */
#define JUMP_OUT_AT 0xFFFFFFE0u         /* no code: only the chain knows it */
static struct {
    unsigned entries;
    uintptr_t depth[2];
    uint32_t sp;
    int resumed;
} out;

static void out_handler(struct ros_cpu *s)
{
    volatile char here;
    unsigned n = out.entries++;
    if (n < 2)
        out.depth[n] = (uintptr_t)&here;
    if (n == 0) {                       /* jump out, to the task's point */
        s->r[13] = out.sp;
        ros_resume(s, JUMP_OUT_AT);
    }
    s->r[15] = s->r[14];                /* the second: returns, the task ends */
}

__attribute__((noinline)) static void raise_deep(unsigned levels)
{
    volatile char keep = (char)levels;  /* a frame a level, 16 bytes at least */
    if (levels)
        raise_deep(levels - 1);
    else
        ros_raise(ros_error(0x126, "Deep"));
    (void)keep;
}

static void task_f(void *arg)
{
    uint32_t r[4] = { ROS_ENV_ERROR, *(uint32_t *)arg, 0, seen.buf };
    change(r);
    struct ros_resume rs;
    rs.at = JUMP_OUT_AT;
    rs.sp = out.sp = 0x7F00;
    rs.prev = ros_resume_top;
    if (setjmp(rs.jb) == 0) {
        ros_resume_top = &rs;
        raise_deep(512);                /* 8K of frames and more under the base */
    } else {
        ros_resume_top = rs.prev;
        out.resumed = 1;
        ros_raise(ros_error(0x127, "Shallow"));
    }
}

/* Task E: a handler address no code could be at, so the default's is used instead */
static void task_e(void *arg)
{
    (void)arg;
    uint32_t r[4] = { ROS_ENV_ERROR, 0x8003, 0, seen.buf };
    change(r);
    ros_raise(ros_error(0x125, "To a duff handler"));
}

void ros_selftest_environment(void)
{
    zero = ros_task_current();
    uint32_t code, r12, buf;

    /* ---- the defaults, and zero page ---- */
    xos_read_default_handler(ROS_ENV_ERROR, &code, &r12, &buf);
    uint32_t r[4] = { ROS_ENV_ERROR, 0, 0, 0 };
    int v = change(r);
    check(code && buf && !v && r[1] == code && r[3] == buf && ros_ld32(ZP_ERRHAN) == code,
          "environment: task 0 has the default error handler, in zero page at ErrHan",
          "default &%08X, current &%08X", code, r[1]);
    check(ros_ld32(ZP_MEMLIMIT) == ROS_APP_BASE && ros_ld32(ZP_CAO) == ROS_ROM_BASE,
          "environment: task 0's MemoryLimit is &8000 -- no application space -- CAO the ROM",
          NULL);

    /* ---- OS_ChangeEnvironment ---- */
    struct ros_environment saved;
    ros_env_save(&saved);
    uint32_t mine = ros_native_entry(error_handler, "selftest:ErrorHandler");
    ros_vector_claim_native(0x1E, changev, 0);
    uint32_t w[4] = { ROS_ENV_ERROR, mine, 0x1234, 0 };
    v = change(w);
    ros_vector_release_native(0x1E, changev, 0);
    check(!v && w[1] == code && w[2] == 0 && w[3] == buf && ros_ld32(ZP_ERRHAN) == mine &&
              vector_calls == 1,
          "environment: ChangeEnvironment sets it, through ChangeEnvironmentV, old ones back",
          "R1 &%08X R2 &%X R3 &%08X, %u vector calls", w[1], w[2], w[3], vector_calls);
    ros_env_load(&saved);               /* a 0 cannot be set back through the SWI */
    uint32_t m[4] = { ROS_ENV_MEMORY_LIMIT, 0, 0x55, 0x66 };
    change(m);
    check(m[1] == ROS_APP_BASE && m[2] == 0x55 && m[3] == 0x66,
          "environment: a handler without an R12 or a buffer leaves those registers", NULL);
    uint32_t bad[4] = { 17, 0, 0, 0 };
    v = change(bad);
    check(v && ((os_error *)ros_ptr(bad[0]))->errnum == 0x1B0,
          "environment: handler 17 -- Bad environment number, &1B0", NULL);

    /* ---- per task ---- */
    seen.buf = ros_addr(ros_rma_alloc(256));
    struct ros_task *a = ros_task_create(0x20000, task_a, &mine);
    ros_task_switch(a);                 /* A installs its handler, switches back */
    check(ros_ld32(ZP_ERRHAN) == code,
          "environment: task 0's handlers come back with the baton, A's went with A", NULL);
    check(seen.a_memlimit == ROS_APP_BASE + 0x20000,
          "environment: a task's MemoryLimit is the end of its slot", "&%08X", seen.a_memlimit);
    ros_task_switch(a);                 /* A raises: its handler, then it ends */
    check(seen.a_errhan_kept && seen.error_calls == 1 && seen.error_wp == 0xA11CE &&
              seen.errnum == 0x123 && strcmp(seen.errmess, "A's unhandled error") == 0 &&
              ros_task_ended(a) && !seen.a_resumed_after_raise,
          "environment: an error nothing catches reaches the task's error handler, in its buffer",
          "%u calls, &%X \"%s\"", seen.error_calls, seen.errnum, seen.errmess);
    /* The kernel's ErrHandler enters it with MOVS pc: user mode.  In SVC
     * mode a BASIC program that had trapped an error went on in SVC, its
     * SWIs kept its R13 as their stack, and Wimp_Poll's frames went into
     * the application slot the task switch then paged out (#1) */
    check(seen.error_mode == ROS_MODE_USR,
          "environment: the error handler is entered in user mode, as the kernel's ErrHandler",
          "mode &%X", seen.error_mode);
    ros_task_destroy(a);

    uint32_t ag = ros_native_entry(again_handler, "selftest:AgainHandler");
    struct ros_task *d = ros_task_create(0x20000, task_d, &ag);
    ros_task_switch(d);
    check(again.entries == 200 && again.max_depth == 0 && ros_task_ended(d),
          "environment: errors raised inside the error handler re-enter it flattened, not nested",
          "%u entries, %lu bytes deeper", again.entries, (unsigned long)again.max_depth);
    ros_task_destroy(d);

    uint32_t oh = ros_native_entry(out_handler, "selftest:OutHandler");
    struct ros_task *f = ros_task_create(0x20000, task_f, &oh);
    ros_task_switch(f);
    check(out.entries == 2 && out.resumed && ros_task_ended(f) &&
              out.depth[1] > out.depth[0] + 8192,
          "environment: after the handler jumps out of an error, the next is raised from a live "
          "frame, not the first's (#65)",
          "%u entries, resumed %d, second entry %ld bytes above the first", out.entries,
          out.resumed, (long)(out.depth[1] - out.depth[0]));
    ros_task_destroy(f);

    struct ros_task *e = ros_task_create(0x20000, task_e, NULL);
    ros_task_switch(e);
    check(ros_task_ended(e) && ros_ld32(ZP_ERRHAN) == code,
          "environment: an unusable error handler is replaced by the default, as the kernel's",
          NULL);
    ros_task_destroy(e);

    uint32_t ex = ros_native_entry(exit_handler, "selftest:ExitHandler");
    struct ros_task *b = ros_task_create(0, task_b, &ex);
    ros_task_switch(b);
    check(seen.exit_calls == 1 && seen.exit_r12 == 0xB0B && ros_task_ended(b),
          "environment: OS_Exit calls the task's exit handler with its R12, and ends it", NULL);
    /* SEXIT enters it with MOVS pc, in user mode, as ErrHandler does the
     * error handler (#1): R0 = 0, R1 as OS_Exit had it, R2 the return code,
     * R10 and R11 the caller's (#66) */
    check(seen.exit_mode == ROS_MODE_USR && seen.exit_r[0] == 0 &&
              seen.exit_r[1] == 0x58454241u && seen.exit_r[2] == 7 &&
              seen.exit_r10 == 0x10AA && seen.exit_r11 == 0x11BB,
          "environment: the exit handler is entered in user mode, R0-R2 and R10-R11 as SEXIT's "
          "(#66)", "mode &%X R0-R2 &%X &%X %u R10 &%X R11 &%X", seen.exit_mode, seen.exit_r[0],
          seen.exit_r[1], seen.exit_r[2], seen.exit_r10, seen.exit_r11);
    ros_st32(ROS_ZP_RETURN_CODE, 0);
    ros_task_destroy(b);

    struct ros_task *c = ros_task_create(0, task_c, NULL);
    ros_task_switch(c);
    check(ros_task_ended(c) && ros_task_current() == zero && ros_ld32(ZP_ERRHAN) == code,
          "environment: OS_Exit with the default handler ends the task quietly", NULL);
    ros_task_destroy(c);
    ros_rma_free(ros_ptr(seen.buf));
}
