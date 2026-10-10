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
 * This file is a reimplementation of parts of RISC OS Open's Kernel
 * (Sources/Kernel). The Apache licence of RISC OS Open's source applies to it.
 */

/* callback.c: the kernel's old-style CallBack, and the user return it
 * exists for. Together they let the Wimp switch tasks. This is the first
 * stage of the Wimp's task switching, which gives each task a thread.
 *
 * The Wimp switches tasks through the kernel. Inside Wimp_Poll it pages the
 * next task in, installs its own CallBack handler with the outgoing task's
 * register block as the buffer, calls OS_SetCallBack and returns. On the
 * way back to user mode, with the SVC stack empty, the kernel dumps the
 * outgoing task's user registers (R0-R14, the PC at +60, the PSR at +64)
 * into that block and enters the handler. The handler loads the next task's
 * block and drops into user mode with MSR SPSR, LDM {R0-R14}^, MOVS PC.
 * The compiler turns that last idiom into ros_user_return(block).
 *
 * Here each task is a thread, parked where it left. So "go back to the user
 * context in this block" means one of three things:
 *   - The block is the one this thread was dumped into. The thread goes
 *     back to its own caller, with the registers as the handler left them
 *     in the block (R0 the reason code, R2 the sender, V).
 *   - Another thread was dumped into the block. That thread wakes in its
 *     own ros_user_return and goes back to its own caller. This is the
 *     thread switch, ros_task_switch_thread. The current thread waits until
 *     something goes back to its block.
 *   - No thread owns the block. It is a context not yet run (Wimp_StartTask
 *     points a new task's PC at runthetask). A new thread enters it there.
 * A thread whose block has gone, because the Wimp dumped a later dead task
 * into it, can only be woken by the end of the program, which is the end of
 * the desktop (ros_user_return). That ends the thread in turn.
 *
 * The CallBack runs where the kernel runs it. That is on the way out of the
 * outermost SWI, after background work and transient callbacks. If the
 * handler is the default, which only returns, nothing is run.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/callback.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/platform.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/task.h"

#define ZP_CALLAD  (ROS_ZEROPAGE + 0x140u)  /* KernelWS CallAd */
#define ZP_CALLWS  (ROS_ZEROPAGE + 0x13Cu)  /* CallAd_ws */
#define ZP_CALLBF  (ROS_ZEROPAGE + 0x144u)  /* CallBf */
#define WORDS      17u

static int callback_flag;                   /* OS_SetCallBack's, as the kernel's */

/* What the handler went back to user mode with, for this thread's SWI. */
static __thread int returned;
static __thread uint32_t user_regs[WORDS];

static uint32_t psr_of(const struct ros_cpu *s)
{
    return s->n << 31 | s->z << 30 | s->c << 29 | s->v << 28 | ROS_MODE_USR;
}

static void load_block(uint32_t block, uint32_t r[WORDS])
{
    for (unsigned i = 0; i < WORDS; i++)
        r[i] = ros_ld32(block + 4 * i);
}

void ros_thunk_OS_SetCallBack(struct ros_cpu *s)
{
    callback_flag = 1;
    s->v = 0;
}

void ros_callback_run(struct ros_cpu *s)
{
    if (!callback_flag)
        return;
    callback_flag = 0;
    uint32_t code = ros_ld32(ZP_CALLAD), ws = ros_ld32(ZP_CALLWS), buf = ros_ld32(ZP_CALLBF);
    if (ros_env_is_default(ROS_ENV_CALLBACK, code) || !buf)
        return;
    for (unsigned i = 0; i < 16; i++)
        ros_st32(buf + 4 * i, s->r[i]);
    ros_st32(buf + 64, psr_of(s));
    ros_task_set_user_block(buf);
    if (ros_switrace_live()) {
        ros_console_printf("rosgd: callback: task &%X dumped into &%08X, pc &%08X\n",
                           ros_ld32(ROS_ZP_DOMAINID), buf, s->r[15]);
        ros_console_printf("rosgd:   regs");
        for (unsigned i = 0; i < 17; i++)
            ros_console_printf(" %08X", ros_ld32(buf + 4 * i));
        ros_console_printf("\n");
    }

    /* The handler runs as the kernel runs it, in SVC mode with the stack
     * empty: its own SWIs are not the way out to user mode. */
    struct ros_cpu h;
    ros_cpu_enter(&h);
    h.r[12] = ws;
    h.irq_off = 1;
    /* A handler may make a SWI whose way out runs a CallBack of its own.
     * A task window's handler polls the Wimp, and the Wimp's task switch is
     * such a CallBack. What that inner one went back to user mode with
     * belongs to it and not to this handler. So the outer outcome is kept
     * round the call. */
    int outer_returned = returned;
    uint32_t outer_regs[WORDS], regs[WORDS];
    memcpy(outer_regs, user_regs, sizeof user_regs);
    returned = 0;
    ros_call_depth++;
    ros_call(&h, code);
    ros_call_depth--;
    int back = returned;
    memcpy(regs, user_regs, sizeof regs);
    returned = outer_returned;
    memcpy(user_regs, outer_regs, sizeof user_regs);
    if (!back)
        return;                             /* it returned: the registers as they were */
    for (unsigned i = 0; i < 15; i++)
        s->r[i] = regs[i];
    uint32_t psr = regs[16];
    s->n = psr >> 31 & 1, s->z = psr >> 30 & 1, s->c = psr >> 29 & 1, s->v = psr >> 28 & 1;
}

/* A context no thread has run: its registers, and its PC, from the block. */
static void context_main(void *arg)
{
    uint32_t block = (uint32_t)(uintptr_t)arg, r[WORDS];
    ros_task_set_user_block(block);
    if (ros_fp_user_return)
        ros_fp_user_return();               /* its VFP context, on its thread */
    load_block(block, r);
    struct ros_cpu u;
    ros_cpu_enter(&u);
    memcpy(u.r, r, 15 * sizeof r[0]);
    u.n = r[16] >> 31 & 1, u.z = r[16] >> 30 & 1, u.c = r[16] >> 29 & 1, u.v = r[16] >> 28 & 1;
    u.mode = ROS_MODE_USR;
    u.irq_off = 0;
    ros_call(&u, r[15]);
    /* Its code came to an end: back to whoever started it. */
    ros_task_exit_thread(NULL);
}

/* Wimp_ReadSysInfo 0: the tasks the Wimp has (none, or no Wimp: 0) */
static uint32_t wimp_tasks(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0;
    ros_swi(&c, XWimp_ReadSysInfo);
    return c.v ? 0 : c.r[0];
}

void ros_user_return(struct ros_cpu *s, uint32_t block)
{
    struct ros_task *self = ros_task_current();
    if (block != ros_task_user_block(self)) {
        struct ros_task *to = ros_task_waiting_on(block);
        if (ros_switrace_live())
            ros_console_printf("rosgd: user return: from block &%08X to &%08X (pc &%08X): %s\n",
                               ros_task_user_block(self), block, ros_ld32(block + 60),
                               to ? "its thread" : "a new thread");
        if (!to)
            to = ros_task_create(0, context_main, (void *)(uintptr_t)block);
        if (!to) {
            ros_console_printf("rosgd: no thread for the context at &%08X\n", block);
            s->r[15] = ROS_RETURN_TO_NATIVE;
            return;
        }
        ros_task_switch_thread(to);
        block = ros_task_user_block(self);  /* something went back to ours */
        if (!block || (ros_task_woken_by_program_end() && !wimp_tasks())) {
            /* Nothing went back to this block, and nothing ever will. There
             * are two ways to get here. In the first, the context this
             * thread left is gone, because a task dumped there after it
             * took its block (the Wimp has one buffer for dead tasks). The
             * baton came back because a task this thread started ended its
             * program the default way. In the second, this thread's task
             * was the last to die before that one, so it still holds the
             * buffer (its own dead context, Do_ExitHandler's OS_Exit after
             * Wimp_Poll). A task ended by the default handlers, with the
             * Wimp left with no tasks, is the same end.
             * That end is the desktop's end. The last task's OS_Exit has
             * run, the Wimp has closed down, and the exit handler reached
             * is the one the Wimp put back, which was in place when it
             * started. On RISC OS that goes back to the command line
             * (Kernel/s/Super1, CLIEXIT). The end is this context's too.
             * Task 0's application (*Desktop) returns to the command that
             * entered it, and so to the prompt of /init. A thread of the
             * Wimp's hands the end on to the one that started it. The
             * application space goes with it. That is the space the Wimp
             * put back as it closed down, on the thread that ran its last
             * task. */
            ros_task_take_ended_space();
            ros_env_end_program(0);
        }
    }
    if (ros_switrace_live()) {
        ros_console_printf("rosgd: user return: to own block &%08X (pc &%08X)\n", block, ros_ld32(block + 60));
        ros_console_printf("rosgd:   regs");
        for (unsigned i = 0; i < 17; i++)
            ros_console_printf(" %08X", ros_ld32(block + 4 * i));
        ros_console_printf("\n");
    }
    if (ros_fp_user_return)
        ros_fp_user_return();               /* this task's VFP context, here */
    load_block(block, user_regs);
    returned = 1;
    s->r[15] = ROS_RETURN_TO_NATIVE;        /* the handler is done */
}
