/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* swi.c -- where a SWI number goes.
 *
 *   &00-&FF      the kernel's SWIs: native, through their generated thunks
 *   &100-&1FF    OS_WriteI: one SWI per character
 *   the rest     a module's chunk of 64: a native module's thunk, or a
 *                compiled module's SWI handler. The handler is entered as
 *                RISC OS's dispatcher enters it, with R11 the offset in
 *                the chunk and R12 the private word. R10-R12 are preserved
 *                for the caller.
 *
 * The X bit decides what an error does. With X set, the error is returned
 * in R0 with V set. With X clear, it is raised to the task's error
 * handler.
 *
 * Module code runs holding the personality lock (background.h). The
 * dispatcher counts SWIs in and out. The way out of the outermost SWI is a
 * safe point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/callback.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/meter.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/vdu.h"
#include "rosgd/wimprec.h"
#include "rosgd/task.h"

uint32_t ros_svc_sp = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;

/* The first task's floating point state. The task switcher swaps this. */
static struct ros_fp first_task_fp = { .fpsr = ROS_FPSR_INITIAL };
struct ros_fp *ros_fp_current = &first_task_fp;
void (*ros_fp_user_return)(void);

static void (*kernel_swis[256])(struct ros_cpu *s);
static const char *kernel_names[256];

/* The number of a SWI whose thunk compiled code called directly. It is
 * looked up once for each thunk and then remembered, for the SWI record. */
#define THUNKS 1024u
static struct { void (*thunk)(struct ros_cpu *); uint32_t number; } thunk_numbers[THUNKS];

static uint32_t thunk_number(void (*thunk)(struct ros_cpu *))
{
    uint32_t i = (uint32_t)(((uintptr_t)thunk >> 4) * 2654435761u) & (THUNKS - 1);
    unsigned probes = 0;
    for (; thunk_numbers[i].thunk && probes < THUNKS; i = (i + 1) & (THUNKS - 1), probes++)
        if (thunk_numbers[i].thunk == thunk)
            return thunk_numbers[i].number;
    uint32_t n = 0xFFFFFF;
    for (unsigned k = 0; k < ros_native_swi_count && n == 0xFFFFFF; k++)
        if (ros_native_swis[k].thunk == thunk)
            n = ros_native_swis[k].number;
    for (struct ros_module *m = ros_module_first(); m && n == 0xFFFFFF; m = m->next)
        for (uint32_t k = 0; m->swi_thunks && k < m->swi_count; k++)
            if (m->swi_thunks[k] == thunk)
                n = m->swi_chunk + k;
    if (n != 0xFFFFFF && probes < THUNKS) {     /* (a module not yet registered: ask again) */
        thunk_numbers[i].thunk = thunk;
        thunk_numbers[i].number = n;
    }
    return n;
}

void ros_swi_init(void)
{
    ros_meter_init();
    ros_switrace_init();
    ros_wimprec_init();
    for (unsigned i = 0; i < ros_native_swi_count; i++) {
        uint32_t n = ros_native_swis[i].number;
        if (n < 256) {
            kernel_swis[n] = ros_native_swis[i].thunk;
            kernel_names[n] = ros_native_swis[i].name;
        }
    }
}

const char *ros_swi_name(uint32_t number)
{
    static char name[64];
    number &= ~(ROS_X_BIT | 0xFF000000u);
    if (number < 256)
        return kernel_names[number];
    struct ros_module *m = ros_module_for_swi(number);
    uint32_t off = m ? number - m->swi_chunk : 0;
    if (!m || !m->swi_names || off >= m->swi_count || !m->swi_names[off][0])
        return NULL;                    /* (an unnamed SWI in a native module's chunk) */
    snprintf(name, sizeof name, "%s_%s", m->swi_prefix, m->swi_names[off]);
    return name;
}

void ros_swi_fail(struct ros_cpu *s, const os_error *e)
{
    s->v = 1;
    s->r[0] = ros_addr(e);
}

void ros_cpu_enter(struct ros_cpu *s)
{
    memset(s, 0, sizeof *s);
    s->fp = ros_fp_current;
    s->r[13] = ros_svc_sp;
    s->r[14] = ROS_RETURN_TO_NATIVE;
    s->mode = ROS_MODE_SVC;
}

/* A module's SWI handler can leave from deep inside, over the frames of
 * the calls it made. For example, the Wimp, when choosing a menu item,
 * puts back the sp it was entered with (longjumpSP) and goes out through
 * ExitPoll. So the entry is a resume point for ROS_RETURN_FROM_SWI at that
 * sp. A checked return that finds it does a longjmp to here. This is what
 * the ARM code's single stack does with one load of sp. What the frames
 * left behind held is put back: the SVC sp, the call depth, the error
 * handlers and the trace. */
static void enter_compiled(struct ros_cpu *s, struct ros_module *m, uint32_t number)
{
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12], lr = s->r[14];
    s->r[11] = number - m->swi_chunk;
    s->r[12] = m->private_word;
    s->r[14] = ROS_RETURN_FROM_SWI;
    s->v = 0;               /* the handler starts with V clear */
    struct ros_resume rs;
    rs.at = ROS_RETURN_FROM_SWI;
    rs.sp = s->r[13];
    rs.prev = ros_resume_top;
    uint32_t svc_sp = ros_svc_sp;
    uint32_t depth = ros_call_depth;
    struct ros_handler *handlers = ros_handler_chain(NULL);
    ros_handler_chain(handlers);
    struct ros_swi_frame *trace = ros_switrace_top();
    if (setjmp(rs.jb) == 0) {
        ros_resume_top = &rs;
        ros_call(s, m->swi_addr);
    } else {
        ros_resume_take(s);
        ros_svc_sp = svc_sp;
        ros_call_depth = depth;
        ros_handler_chain(handlers);
        ros_switrace_unwind(trace);
    }
    ros_resume_top = rs.prev;
    if (s->r[15] != ROS_RETURN_FROM_SWI)
        ros_bad_return(s, ROS_RETURN_FROM_SWI);
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = lr;
}

/* The kernel's check on the way out of every SWI (Kernel/s/Kernel's
 * VSetReturn, with CheckErrorBlocks off as in RISC OS 5). An error pointer
 * below &4000 or not word-aligned is replaced by an error naming the SWI.
 * OS_GenerateError's R0 is excepted. */
static void check_error_pointer(struct ros_cpu *s, uint32_t n)
{
    if (n == 0x2B || (s->r[0] >= 0x4000 && !(s->r[0] & 3)))
        return;
    char hex[8] = "";
    if (n)
        snprintf(hex, sizeof hex, "%X", n);                /* (OS_ConvertHex6, 0s stripped) */
    ros_swi_fail(s, ros_error(0x116, "SWI &%s returned a bad error pointer", hex));
}

void ros_swi_raise(struct ros_cpu *s)
{
    if (ros_call_depth == 0 && !ros_in_background())
        ros_env_foreground(s);
    ros_raise(ros_ptr(s->r[0]));
}

/* A SWI from user mode runs as the kernel runs it. It runs in SVC mode on
 * the task's SVC stack, with the caller's R13 banked away until it
 * returns. What a SWI keeps on its stack must outlive the caller's memory.
 * For example, Wimp_StartTask maps its caller's slot out with its own
 * frame pushed, and if that frame were on the caller's stack it would go
 * with the slot. OS_EnterOS leaves the caller in SVC mode, on that stack.
 * Code already on the SVC stack stays there, whether in user mode or not.
 * The Wimp drops to user mode inside Wimp_Poll with its frames on the SVC
 * stack. The model banks no registers in that case: its R13 is those
 * frames' (ros_svc_sp_enter builds below them). */
struct user_bank {
    uint32_t sp;
    int user;
};

static void bank_in(struct ros_cpu *s, struct user_bank *b)
{
    b->user = s->mode == ROS_MODE_USR && s->r[13] - ROS_SVCSTACK_BASE > ROS_SVCSTACK_SIZE;
    if (b->user) {
        b->sp = s->r[13];
        s->r[13] = ros_svc_sp;
        s->mode = ROS_MODE_SVC;
    }
}

static void bank_out(struct ros_cpu *s, const struct user_bank *b, uint32_t n)
{
    if (b->user && n != 0x16) {                 /* OS_EnterOS */
        s->r[13] = b->sp;
        s->mode = ROS_MODE_USR;
    }
}

void ros_native_swi(struct ros_cpu *s, void (*thunk)(struct ros_cpu *))
{
    int user = ros_call_depth == 0 && !ros_in_background();    /* not an interrupt's */
    if (user)
        ros_env_foreground(s);
    struct user_bank bank;
    bank_in(s, &bank);
    uint32_t outer = ros_svc_sp_enter(s);
    struct ros_swi_frame f;
    uint32_t n = thunk_number(thunk);
    f.kind = ROS_KIND_NATIVE;
    ros_switrace_in(&f, n, s);
    uint64_t m0 = ros_meter_now_ns();
    int vreal = ros_vdu_displays ? ros_vdu_swi_enter(n, user) : 0;
    ros_call_depth++;
    thunk(s);
    ros_svc_sp = outer;
    bank_out(s, &bank, n);
    ros_call_depth--;
    if (vreal)
        ros_vdu_real_leave(1);
    ros_meter_add_swi(ros_meter_now_ns() - m0);
    ros_switrace_out(&f, s);
    ros_call_depth++;
    if (--ros_call_depth == 0 && user) {
        struct ros_cpu keep = *s;
        int vr = ros_vdu_displays ? ros_vdu_real_enter() : 0;
        ros_swi_exit_outermost();
        *s = keep;
        ros_callback_run(s);
        ros_vdu_real_leave(vr);
    }
}

void ros_swi(struct ros_cpu *s, uint32_t number)
{
    ros_swi_as(s, number, ROS_KIND_NATIVE);
}

void ros_swi_as(struct ros_cpu *s, uint32_t number, int kind)
{
    uint32_t x = number & ROS_X_BIT;
    uint32_t n = number & ~(ROS_X_BIT | 0xFF000000u);   /* 24 bits, X clear */

    /* OS_CallASWI and OS_CallASWIR12 call the SWI whose number is in R10
     * or R12. It is dispatched as if called itself, so its own X bit
     * decides, with R10 or R12 as they were (Kernel/s/Kernel, CallASWI). */
    if (n == OS_CALLASWI || n == OS_CALLASWIR12) {
        number = s->r[n == OS_CALLASWI ? 10 : 12] & 0x00FFFFFFu;
        x = number & ROS_X_BIT;
        n = number & ~ROS_X_BIT;
    }

    int user = ros_call_depth == 0 && !ros_in_background();    /* not an interrupt's */
    if (user)
        ros_env_foreground(s);          /* for the error handler, if this fails */
    uint64_t m0 = ros_meter_now_ns();
    struct user_bank bank;
    bank_in(s, &bank);
    uint32_t outer_svc_sp = ros_svc_sp_enter(s);
    struct ros_swi_frame f;
    f.kind = (uint8_t)kind;
    ros_switrace_in(&f, n, s);
    /* A task's own Wimp call, for the recorder (wimprec.c). Only the
     * outermost is recorded, never the SWIs a Wimp makes of itself. */
    int wimprec = ros_wimprec_on && user && n >= 0x400C0 && n < 0x40100;
    uint32_t wimprec_in[4];
    if (wimprec) {
        memcpy(wimprec_in, s->r, sizeof wimprec_in);
        ros_wimprec_call(n, wimprec_in);
    }
    /* A virtual display: the task's own SWI heals its real depth. A Wimp
     * SWI is a real section, on the real display. */
    int vreal = ros_vdu_displays ? ros_vdu_swi_enter(n, user) : 0;
    ros_call_depth++;
    if (n < 256 && kernel_swis[n]) {
        kernel_swis[n](s);
    } else if (n >= 0x100 && n < 0x200) {
        uint32_t outer_sp = ros_svc_sp_enter(s);
        os_error *e = xos_write_c((uint8_t)n);
        ros_svc_sp = outer_sp;
        if (e)
            ros_swi_fail(s, e);
        else
            s->v = 0;
    } else {
        /* The module a caller of this kind reaches. An ARM caller reaches
         * a shadow, and a native caller reaches the chain's module. This
         * is strict: a shadow's own SWIs are never a native caller's. */
        struct ros_module *m = ros_module_for_swi_kind(n, kind);
        uint32_t off = m ? n - m->swi_chunk : 0;
        if (m && !m->swi_thunks)
            enter_compiled(s, m, n);
        else if (m && off < m->swi_count && m->swi_thunks[off])
            m->swi_thunks[off](s);
        else if (m && m->bad_swi)
            ros_swi_fail(s, m->bad_swi(m, off));
        else
            ros_swi_fail(s, ros_error(ROS_ERR_NO_SUCH_SWI, "SWI &%X not known", n));
    }
    if (n == 0x400C0 && !s->v)                  /* Wimp_Initialise: its memory adopted (task.h) */
        ros_task_wimp_initialised();
    if (vreal)
        ros_vdu_real_leave(1);
    ros_svc_sp = outer_svc_sp;
    bank_out(s, &bank, n);
    if (s->v)
        check_error_pointer(s, n);
    ros_call_depth--;
    ros_meter_add_swi(ros_meter_now_ns() - m0);
    ros_switrace_out(&f, s);
    ros_call_depth++;
    /* The way back out of the outermost SWI is a safe point. Background
     * work runs, then callbacks, as RISC OS runs them on the way to user
     * mode. Registers go back to the caller as the SWI left them. This does
     * not apply to a SWI that background work makes. That is an interrupt's,
     * which goes back to the interrupted code and never to user mode. */
    if (--ros_call_depth == 0 && user) {
        struct ros_cpu keep = *s;
        int vr = ros_vdu_displays ? ros_vdu_real_enter() : 0;
        ros_swi_exit_outermost();
        *s = keep;
        ros_callback_run(s);
        ros_vdu_real_leave(vr);
    }
    /* After the callbacks. A Wimp_Poll's task switch is the Wimp's
     * CallBack, so only here is this thread's task back, with its own slot
     * paged in and its registers the ones it is given. */
    if (wimprec)
        ros_wimprec_return(n, wimprec_in, s);
    if (s->v && !x)
        ros_raise(ros_ptr(s->r[0]));
}
