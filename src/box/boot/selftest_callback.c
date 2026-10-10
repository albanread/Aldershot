/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_callback.c: the kernel's old-style CallBack and the user return
 * the Wimp switches tasks with (runtime/callback.c), through CtxTest, which
 * arms the CallBack as the Wimp's ExitPoll does and ends its handler as
 * callbackpoll does, compiled (modules/ctxtest/ctxtest.s).
 *
 * Two contexts, A (this, task 0) and B (a block no thread has run yet),
 * pass control back and forth, each through a block of seventeen words as
 * Wimp tasks do: whoever switches writes the other's R0 into its block, as
 * the Wimp writes a reason code, and the other comes back from its own
 * switch with it.
 *
 * Then the desktop's end (desktop_end below): the context that entered an
 * application is gone when the last task ends the default way. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/dynarea.h"
#include "rosgd/environment.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "selftest.h"

#define check ros_check
#define XCTXTEST_SWITCH 0xE0040u

static uint32_t blk_a, blk_b;
static uint32_t b_first, b_second, b_task, b_same_sp;

/* Go back to the context in `to`, leaving this one's registers in `from`;
 * R0 when something comes back to `from`. */
static uint32_t go(uint32_t to, uint32_t from, uint32_t *r9)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = to, s.r[1] = from, s.r[9] = 0x1234;
    s.mode = ROS_MODE_USR;
    ros_swi(&s, XCTXTEST_SWITCH);
    if (r9)
        *r9 = s.r[9];
    return s.r[0];
}

/* B: user code, entered at its block's PC with its block's registers. */
static void context_b(struct ros_cpu *s)
{
    b_first = s->r[0];
    b_task = ros_task_id(ros_task_current());
    ros_st32(blk_a, 99);                    /* A's R0 for when it runs again */
    uint32_t r9;
    b_second = go(blk_a, blk_b, &r9);
    b_same_sp = r9 == 0x1234;
    ros_st32(blk_a, b_second + 1);
    s->r[15] = s->r[14];                    /* its code ends: back to A */
}

/* ---- the desktop's end -------------------------------------------------------
 *
 * *Desktop from the command line: the Desktop is the application, entered on
 * task 0's thread, and each task it starts runs on a thread of its own.  The
 * Desktop dies first, its registers dumped into the Wimp's one buffer for
 * dead tasks; the next context dumped there takes the buffer, so the context
 * that ran *Desktop has nothing to go back to.  At Exit desktop the last
 * task's OS_Exit, the Wimp closed down, reaches the handler the Wimp put
 * back, the default.  That task's thread ends and the baton goes to task 0.
 * Task 0 returns from *Desktop to the command that entered it, as RISC OS
 * goes back to its * prompt.  An earlier fault made task 0 go back to a
 * block at 0.
 *
 *   D, the application (task 0): to T, a new context, D's registers in DEAD
 *   T (its own thread): to F, a new context, its registers in DEAD too.
 *     The buffer is T's now, and D's context is gone.
 *   F: its code ends and goes back to T, whose block DEAD is.
 *   T: takes application space from the free pool, as the Wimp's last
 *     closedown puts it back (restorepages: OS_ChangeDynamicArea 6 by a
 *     negative amount), and marks it.  Then OS_Exit goes to the default
 *     handler.  T's thread ends and task 0 wakes with no context.  The
 *     application ends and D's code never resumes.  Task 0 is left with
 *     that space and the marks in it, as the command line has RISC OS's
 *     one application space after the desktop. */
static uint32_t blk_t, blk_f, blk_dead;
static int d_resumed, f_ran, t_resumed, t_exiting, t_space;

#define SPACE_MARK0 0xDE5C0001u
#define SPACE_MARK1 0xDE5C0002u

static void desktop_f(struct ros_cpu *s)
{
    f_ran = 1;
    s->r[15] = s->r[14];                    /* its code ends: back to T */
}

static void desktop_t(struct ros_cpu *s)
{
    go(blk_f, blk_dead, NULL);
    t_resumed = 1;
    struct ros_cpu x;
    ros_cpu_enter(&x);
    x.r[0] = 6, x.r[1] = (uint32_t)-8192;   /* the free pool shrinks by 8K */
    ros_swi(&x, XOS_ChangeDynamicArea);
    t_space = !x.v && x.r[1] == 8192 && ros_ld32(ROS_ZEROPAGE + 0x368u) == 0xA000;
    if (t_space) {
        ros_st32(ROS_APP_BASE, SPACE_MARK0);
        ros_st32(0x9FFC, SPACE_MARK1);
    }
    t_exiting = 1;
    ros_cpu_enter(&x);
    x.mode = ROS_MODE_USR;
    ros_swi(&x, OS_Exit);                   /* does not return */
    t_exiting = 0;
    s->r[15] = s->r[14];
}

static void desktop_d(void *arg)
{
    (void)arg;
    go(blk_t, blk_dead, NULL);
    d_resumed = 1;                          /* nothing may come back here */
}

/* MemLimit (OS_ChangeEnvironment 0, read) */
static uint32_t memory_limit(void)
{
    uint32_t code, ws, buf;
    ros_env_read(ROS_ENV_MEMORY_LIMIT, &code, &ws, &buf);
    return code;
}

/* Within a margin: the free pool is the box's real free memory, which
 * moves with whatever else the box does (dynarea.c) */
static int near(uint32_t a, uint32_t b)
{
    return (a > b ? a - b : b - a) < (4u << 20);
}

/* The free pool's size (OS_ReadDynamicArea 6) */
static uint32_t freepool(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 6;
    ros_swi(&c, XOS_ReadDynamicArea);
    return c.v ? 0 : c.r[1];
}

static uint32_t context(ros_code *fn, const char *name)
{
    uint32_t b = ros_addr(ros_rma_alloc(17 * 4));
    memset(ros_ptr(b), 0, 17 * 4);
    ros_st32(b + 60, ros_native_entry(fn, name));
    ros_st32(b + 64, ROS_MODE_USR);
    return b;
}

static void desktop_end(void)
{
    uint32_t exit_code, exit_ws, exit_buf;
    ros_env_read(ROS_ENV_EXIT, &exit_code, &exit_ws, &exit_buf);
    int deflt = ros_env_is_default(ROS_ENV_EXIT, exit_code);
    blk_t = context(desktop_t, "selftest:desktop_t");
    blk_f = context(desktop_f, "selftest:desktop_f");
    blk_dead = ros_addr(ros_rma_alloc(17 * 4));
    memset(ros_ptr(blk_dead), 0, 17 * 4);
    uint32_t depth = ros_call_depth, pool = freepool();
    int had_space = ros_slot_current != NULL;
    struct ros_environment env;
    ros_env_save(&env);
    ros_module_run_as_application(desktop_d, NULL);
    uint32_t apl = ros_ld32(ROS_ZEROPAGE + 0x368u), limit = memory_limit();
    check(deflt && f_ran && t_resumed && t_exiting && !d_resumed &&
              ros_task_id(ros_task_current()) == 0 && ros_ld32(ROS_ZP_DOMAINID) == 0 &&
              ros_call_depth == depth,
          "CallBack: the last task's OS_Exit to the default handler, the context that entered "
          "the application gone, ends the application: *Desktop returns (Exit desktop)",
          "default exit handler %d; F ran %d, T back %d and exiting %d, D resumed %d; "
          "task %u, DomainId &%X, depth %u (was %u)", deflt, f_ran, t_resumed, t_exiting,
          d_resumed, ros_task_id(ros_task_current()), ros_ld32(ROS_ZP_DOMAINID), ros_call_depth,
          depth);
    int mapped = ros_slot_current != NULL;
    uint32_t m0 = mapped ? ros_ld32(ROS_APP_BASE) : 0, m1 = mapped ? ros_ld32(0x9FFC) : 0;
    /* ... and given back, as task 0 had none */
    int freed = ros_task_resize_own(0) == 0 && ros_slot_current == NULL;
    ros_env_load(&env);
    check(!had_space && t_space && mapped && m0 == SPACE_MARK0 && m1 == SPACE_MARK1 && freed &&
              near(freepool(), pool) && apl == 0xA000 && limit == 0xA000,
          "CallBack: after the desktop's end task 0 has the application space the last task "
          "had, mapped, what it left there still there, AplWorkSize and MemLimit its end "
          "(CLIEXIT leaves them as restorepages set them)",
          "none before %d; T's 8K %d; mapped %d, &%08X &%08X; freed %d, free pool %u (was %u); "
          "AplWorkSize &%X MemLimit &%X", !had_space, t_space, mapped, m0, m1, freed,
          freepool(), pool, apl, limit);
    ros_rma_free(ros_ptr(blk_t));
    ros_rma_free(ros_ptr(blk_f));
    ros_rma_free(ros_ptr(blk_dead));
}


/* ---- the desktop's end, the last task's creator dead just before it ---------
 *
 *   D, the application (task 0): to T2, a new context, D's registers in DEAD
 *   T2 (its own thread): to L2, a new context, its registers in DEAD.  T2
 *     is the last task to die before L2, so it still holds the buffer.
 *   L2: takes application space from the free pool, marks it, then calls
 *     OS_Exit to the default handler.  Its thread ends into T2, which must
 *     not resume its dead context but end too, handing the space on to
 *     task 0. */
static uint32_t blk_t2, blk_l2;
static int d2_resumed, t2_resumed, l2_space;

static void desktop_l2(struct ros_cpu *s)
{
    struct ros_cpu x;
    ros_cpu_enter(&x);
    x.r[0] = 6, x.r[1] = (uint32_t)-8192;
    ros_swi(&x, XOS_ChangeDynamicArea);
    l2_space = !x.v && x.r[1] == 8192;
    if (l2_space) {
        ros_st32(ROS_APP_BASE, SPACE_MARK0);
        ros_st32(0x9FFC, SPACE_MARK1);
    }
    ros_cpu_enter(&x);
    x.mode = ROS_MODE_USR;
    ros_swi(&x, OS_Exit);                   /* does not return */
    s->r[15] = s->r[14];
}

static void desktop_t2(struct ros_cpu *s)
{
    go(blk_l2, blk_dead, NULL);
    t2_resumed = 1;                         /* its context is dead: never */
    s->r[15] = s->r[14];
}

static void desktop_d2(void *arg)
{
    (void)arg;
    go(blk_t2, blk_dead, NULL);
    d2_resumed = 1;
}

static void desktop_end_creator(void)
{
    blk_t2 = context(desktop_t2, "selftest:desktop_t2");
    blk_l2 = context(desktop_l2, "selftest:desktop_l2");
    blk_dead = ros_addr(ros_rma_alloc(17 * 4));
    memset(ros_ptr(blk_dead), 0, 17 * 4);
    uint32_t pool = freepool();
    struct ros_environment env;
    ros_env_save(&env);
    ros_module_run_as_application(desktop_d2, NULL);
    uint32_t apl = ros_ld32(ROS_ZEROPAGE + 0x368u), limit = memory_limit();
    int mapped = ros_slot_current != NULL;
    uint32_t m0 = mapped ? ros_ld32(ROS_APP_BASE) : 0, m1 = mapped ? ros_ld32(0x9FFC) : 0;
    int freed = ros_task_resize_own(0) == 0 && ros_slot_current == NULL;
    ros_env_load(&env);
    check(l2_space && !t2_resumed && !d2_resumed && mapped && m0 == SPACE_MARK0 &&
              m1 == SPACE_MARK1 && freed && near(freepool(), pool) && apl == 0xA000 &&
              limit == 0xA000,
          "CallBack: the desktop's end with the last task's creator the last to die before it "
          "(holding the dead buffer): no dead context resumes, task 0 has the space",
          "L2's 8K %d; T2 resumed %d, D resumed %d; mapped %d, &%08X &%08X; freed %d, "
          "free pool %u (was %u); AplWorkSize &%X MemLimit &%X", l2_space, t2_resumed,
          d2_resumed, mapped, m0, m1, freed, freepool(), pool, apl, limit);
    ros_rma_free(ros_ptr(blk_t2));
    ros_rma_free(ros_ptr(blk_l2));
    ros_rma_free(ros_ptr(blk_dead));
}

void ros_selftest_callback(void)
{
    uint32_t code, ws, buf;
    ros_env_read(ROS_ENV_CALLBACK, &code, &ws, &buf);
    blk_a = ros_addr(ros_rma_alloc(17 * 4));
    blk_b = ros_addr(ros_rma_alloc(17 * 4));
    memset(ros_ptr(blk_a), 0, 17 * 4);
    memset(ros_ptr(blk_b), 0, 17 * 4);
    ros_st32(blk_b, 42);
    ros_st32(blk_b + 60, ros_native_entry(context_b, "selftest:context_b"));
    ros_st32(blk_b + 64, ROS_MODE_USR);

    uint32_t sp_a, depth = ros_call_depth;
    uint32_t first = go(blk_b, blk_a, &sp_a);
    check(first == 99 && b_first == 42 && b_task != 0 && sp_a == 0x1234 && ros_task_id(ros_task_current()) == 0,
          "CallBack: a switch to a block no thread has run starts one there; it hands back with R0 in our block",
          "A got %u, B got %u on task %u, R9 &%X", first, b_first, b_task, sp_a);

    ros_st32(blk_b, 7);                     /* B's R0 for when it runs again */
    uint32_t second = go(blk_b, blk_a, NULL);
    check(second == 8 && b_second == 7 && b_same_sp && ros_call_depth == depth,
          "CallBack: B resumes inside its own switch with R0 7 and its registers; its end comes back to A",
          "A got %u, B got %u, B's R9 kept %u", second, b_second, b_same_sp);

    desktop_end();
    desktop_end_creator();

    /* CtxTest's handler out again */
    ros_env_change(ROS_ENV_CALLBACK, &code, &ws, &buf);
    uint32_t d_code, d_ws, d_buf;
    ros_env_read(ROS_ENV_CALLBACK, &d_code, &d_ws, &d_buf);
    check(ros_env_is_default(ROS_ENV_CALLBACK, d_code), "CallBack: the handler put back as it was", "&%X", d_code);

    ros_rma_free(ros_ptr(blk_a));
    ros_rma_free(ros_ptr(blk_b));
}
