/* Copyright RISC OS Open Ltd and others
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
 * This file is derived from RISC OS Open's source and from other code.
 * The Apache licence of RISC OS Open's source applies to it.
 */

/* vfpsupport.c: VFPSupport, reimplemented over the runtime's floating
 * point.
 *
 * RISC OS's VFPSupport (HWSupport/VFPSupport) manages VFP register
 * contexts. A context says which program's registers are in the
 * coprocessor. The module also provides the elementary functions.
 * In ROSGD the VFP registers are C state belonging to the running task
 * (ros_fp_current). A context is what it is in RISC OS: a block of flags,
 * the register count, FPSCR, FPEXC and the registers. Activating one saves
 * the registers into the context that was active and loads the new one's.
 * Lazy activation is the same as activation now, because nothing is
 * saved by waiting.
 *
 * Which context is active is per task. It is kept with the task's
 * registers (struct ros_fp). On RISC OS the one register file is shared
 * and the Wimp swaps contexts as it switches tasks. Here each task's
 * registers are its own. A task's ChangeContext therefore saves and loads
 * only its own, and switching tasks needs nothing from VFPSupport.
 *
 * The Wimp still calls ChangeContext as it switches, as on RISC OS. It
 * passes 0 on entry to Wimp_Poll and Wimp_StartTask, on the polling
 * task's own thread. On exit from Wimp_Poll it passes the saved context,
 * lazily, to switch to the next task. That call is still on the thread
 * being switched from, before ros_user_return moves to the next task's
 * thread (runtime/callback.c). Taken there, the context landed in the
 * wrong task's registers. The next poll then saved them into another
 * task's paged-out application space, and the desktop stopped. This
 * happened with two BASIC desktop programs running at once.
 *
 * So a lazy activation of a context in application space waits. Only
 * its own task may touch that memory. The activation is taken by the
 * thread that returns to user mode next, which is the task the Wimp
 * switched to (ros_fp_user_return). A task is first run on its parent's
 * thread (Wimp_StartTask) and then on one of its own. It gets its
 * registers the same way: they are saved at its first poll and loaded on
 * its thread. Other lazy activations, such as a driver's RMA context,
 * are taken at once.
 *
 * Kept from the original, because callers depend on it:
 *   - The context block's layout: Context_Flags, NumRegs, FPSCR, FPEXC,
 *     FPINST and FPINST2, then the registers at +24. CheckContext's size
 *     is 24 + 8 per register, up to 32 registers.
 *   - A context that VFPSupport allocates is flagged (bit 31) and is freed
 *     when destroyed. A context the caller supplied is not.
 *   - DestroyContext of the active context activates R1, or none.
 *   - ElementaryFunctions' tables have twelve entries, four bytes apart.
 *     The single precision table comes first, then the double precision
 *     one. Each function takes s0 or d0 (and s1/d1, s2/d2) and answers in
 *     s0 or d0. Here the entries are native. The double precision
 *     functions are the original's, transliterated (elementary.c), since
 *     BASIC prints seventeen digits of them. The single precision ones
 *     still use libm.
 *   - The errors, &81F102 to &81F107.
 *
 * The features reported are a VFPv4 unit with 32 double registers and no
 * NEON. That is what the compiler lifts.
 */
#include <math.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "vfpmath.h"
#include "vfpsupport.h"

#define CTX_FLAGS 0
#define CTX_NUMREGS 4
#define CTX_FPSCR 8
#define CTX_FPEXC 12
#define CTX_REGDUMP 24

#define F_USERMODE 0x1u
#define F_APPSPACE 0x2u
#define F_LAZY_ACTIVATE 0x40000000u
#define F_ACTIVATE 0x80000000u
#define F_VFPMEMORY 0x80000000u
#define F_STATUS_ACTIVE 0x40000000u
#define CC_LAZY 0x1u                    /* ChangeContext's R1: lazily */

#define NUM_REGS 32u
#define VERSION 18u                     /* 0.18, as the module it replaces */

#define E_FEATURE_UNAVAILABLE 0x81F102u
#define E_BAD_CONTEXT 0x81F103u
#define E_BAD_FEATURE 0x81F104u
#define E_BAD_FLAGS 0x81F107u

#define FPSID 0x41034083u               /* ARM, VFPv3+ subarchitecture */
#define MVFR0 0x10110222u               /* 32 registers, single, double, divide, sqrt */
#define MVFR1 0x00000011u               /* flush to zero, default NaN; no NEON */

/* The active context is the running task's. Each task has its own VFP
 * registers, so the record of which context they hold belongs with them. */
#define active (ros_fp_current->vfp_context)
static uint32_t descriptor;             /* ExamineContext's dump format block */
static uint32_t single_table, double_table;
static uint32_t fast[4];                /* FastAPI's entries */

/* ---- contexts ------------------------------------------------------------------- */

static void save(uint32_t ctx)
{
    struct ros_fp *fp = ros_fp_current;
    uint32_t n = ros_ld32(ctx + CTX_NUMREGS);
    ros_st32(ctx + CTX_FPSCR, fp->fpscr);
    for (uint32_t i = 0; i < n && i < NUM_REGS; i++) {
        uint64_t v = fp->vfp.dw[i];
        ros_st32(ctx + CTX_REGDUMP + 8 * i, (uint32_t)v);
        ros_st32(ctx + CTX_REGDUMP + 8 * i + 4, (uint32_t)(v >> 32));
    }
}

static void load(uint32_t ctx)
{
    struct ros_fp *fp = ros_fp_current;
    uint32_t n = ros_ld32(ctx + CTX_NUMREGS);
    fp->fpscr = ros_ld32(ctx + CTX_FPSCR);
    for (uint32_t i = 0; i < n && i < NUM_REGS; i++)
        fp->vfp.dw[i] = (uint64_t)ros_ld32(ctx + CTX_REGDUMP + 8 * i) |
                        (uint64_t)ros_ld32(ctx + CTX_REGDUMP + 8 * i + 4) << 32;
}

/* Activate ctx (0 for none). Returns the context that was active, for R0. */
static uint32_t change(uint32_t ctx)
{
    uint32_t was = active;
    if (ctx == was)
        return was;
    if (was)
        save(was);
    if (ctx)
        load(ctx);
    active = ctx;
    return was;
}

static os_error *check_context(uint32_t flags, uint32_t regs, uint32_t *size)
{
    if ((flags & ~(F_USERMODE | F_APPSPACE | F_LAZY_ACTIVATE | F_ACTIVATE)) || regs == 0 ||
        regs > NUM_REGS)
        return ros_error(E_FEATURE_UNAVAILABLE, "A requested VFP/NEON feature is unavailable");
    *size = CTX_REGDUMP + 8 * regs;
    return NULL;
}

#define FAIL(e)                                                                    \
    do {                                                                           \
        ros_swi_fail(s, (e));                                                      \
        return;                                                                    \
    } while (0)

void ros_thunk_VFPSupport_CheckContext(struct ros_cpu *s)
{
    uint32_t size;
    os_error *e = check_context(s->r[0], s->r[1], &size);
    if (e)
        FAIL(e);
    s->r[0] = size;
    s->v = 0;
}

void ros_thunk_VFPSupport_CreateContext(struct ros_cpu *s)
{
    uint32_t flags = s->r[0], regs = s->r[1], area = s->r[2], size;
    os_error *e = check_context(flags, regs, &size);
    if (e)
        FAIL(e);
    uint32_t keep = flags & (F_USERMODE | F_APPSPACE);
    if (!area) {
        area = ros_addr(ros_rma_alloc(size));
        if (!area)
            FAIL(ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA"));
        keep |= F_VFPMEMORY;
    }
    memset(ros_ptr(area), 0, size);
    ros_st32(area + CTX_FLAGS, keep);
    ros_st32(area + CTX_NUMREGS, regs);
    ros_st32(area + CTX_FPSCR, s->r[3]);
    s->r[0] = area;
    if (flags & (F_ACTIVATE | F_LAZY_ACTIVATE))
        s->r[1] = change(area);
    s->v = 0;
}

void ros_thunk_VFPSupport_DestroyContext(struct ros_cpu *s)
{
    uint32_t ctx = s->r[0], next = s->r[1];
    if (next == ctx)
        next = 0;
    if (ctx == active) {
        ros_st32(ctx + CTX_NUMREGS, 0);     /* nothing to save of it */
        change(next);
    }
    if (ctx && (ros_ld32(ctx + CTX_FLAGS) & F_VFPMEMORY))
        ros_rma_free(ros_ptr(ctx));
    s->r[0] = active;
    s->v = 0;
}

/* An application-space context activated lazily, for the thread that
 * returns to user mode next (see the top of the file). */
static uint32_t pending, pending_domain;

/* Taken only while the task it was for is the one returning. That task is
 * identified by DomainId, as the Wimp set it when switching to the task.
 * Any other return drops the pending context. Nothing is lost by that,
 * because the running task's registers are its own. */
static void take_pending(void)
{
    uint32_t ctx = pending;
    pending = 0;
    if (ctx && ros_ld32(ROS_ZP_DOMAINID) == pending_domain)
        change(ctx);
}

void ros_thunk_VFPSupport_ChangeContext(struct ros_cpu *s)
{
    uint32_t ctx = s->r[0];
    if (ctx && (s->r[1] & CC_LAZY) && ctx >= ROS_APP_BASE && ctx < ROS_APP_LIMIT) {
        s->r[0] = active;               /* the Wimp's switch: see the top of the file */
        pending = ctx;
        pending_domain = ros_ld32(ROS_ZP_DOMAINID);
        s->v = 0;
        return;
    }
    pending = 0;
    s->r[0] = change(ctx);
    s->v = 0;
}

void ros_thunk_VFPSupport_ExamineContext(struct ros_cpu *s)
{
    uint32_t ctx = s->r[0];
    if (!ctx)
        FAIL(ros_error(E_BAD_CONTEXT, "Bad VFP context"));
    if (ctx == active)
        save(ctx);                      /* what it holds, now */
    uint32_t n = ros_ld32(ctx + CTX_NUMREGS);
    uint32_t flags = ros_ld32(ctx + CTX_FLAGS);
    s->r[0] = ctx == active ? flags | F_STATUS_ACTIVE : flags;
    s->r[1] = n;
    s->r[2] = ctx == active ? (n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1) : 0;
    s->r[3] = descriptor;
    s->r[4] = CTX_REGDUMP + 8 * n;
    s->v = 0;
}

void ros_thunk_VFPSupport_FastAPI(struct ros_cpu *s)
{
    s->r[0] = vfpsupport_module.private_word;
    s->r[1] = fast[0], s->r[2] = fast[1], s->r[3] = fast[2], s->r[4] = fast[3];
    s->v = 0;
}

void ros_thunk_VFPSupport_ActiveContext(struct ros_cpu *s)
{
    s->r[0] = active;
    s->v = 0;
}

void ros_thunk_VFPSupport_Version(struct ros_cpu *s)
{
    s->r[0] = VERSION;
    s->v = 0;
}

void ros_thunk_VFPSupport_Features(struct ros_cpu *s)
{
    switch (s->r[0]) {
    case 0:                             /* system registers */
        s->r[0] = FPSID, s->r[1] = MVFR0, s->r[2] = MVFR1;
        break;
    case 1:                             /* exceptions that can trap: none */
        s->r[0] = 0;
        break;
    case 2:                             /* no short vectors; the elementary functions */
        s->r[0] = 0x4;
        break;
    case 3:                             /* MVFR2 */
        s->r[0] = s->r[1] = s->r[2] = s->r[3] = 0;
        break;
    default:
        FAIL(ros_error(E_BAD_FEATURE, "Bad VFPSupport_Features reason code"));
    }
    s->v = 0;
}

void ros_thunk_VFPSupport_ExceptionDump(struct ros_cpu *s)
{
    if (s->r[0] > 0xF)
        FAIL(ros_error(E_BAD_FLAGS, "Invalid flags passed to VFPSupport SWI"));
    s->r[0] = 0, s->r[1] = 0;           /* no exception has been trapped */
    s->v = 0;
}

void ros_thunk_VFPSupport_ElementaryFunctions(struct ros_cpu *s)
{
    if (s->r[0] != 0)
        FAIL(ros_error(E_BAD_FLAGS, "Invalid flags passed to VFPSupport SWI"));
    s->r[0] = 12, s->r[1] = single_table, s->r[2] = double_table;
    s->v = 0;
}

/* ---- the elementary functions, as native code at table addresses -------------------- */

#define D(n) (s->fp->vfp.d[n])
#define S(n) (s->fp->vfp.s[n])
#define RET s->r[15] = s->r[14]

#define FN1(name, expr)                                                            \
    static void name(struct ros_cpu *s) { expr; RET; }

FN1(f32sin, S(0) = sinf(S(0)))
FN1(f32cos, S(0) = cosf(S(0)))
FN1(f32tan, S(0) = tanf(S(0)))
FN1(f32asin, S(0) = asinf(S(0)))
FN1(f32acos, S(0) = acosf(S(0)))
FN1(f32atan, S(0) = atanf(S(0)))
FN1(f32atan2, S(0) = atan2f(S(0), S(1)))
FN1(f32log, S(0) = logf(S(0)))
FN1(f32log10, S(0) = log10f(S(0)))
FN1(f32exp, S(0) = expf(S(0)))
FN1(f32pow, S(0) = powf(S(0), S(1)))
FN1(f32fma, S(0) = fmaf(S(0), S(1), S(2)))
/* Double precision, RISC OS's own (elementary.c). The fma rounds once, as
 * fused64_muladd does. */
FN1(f64sin, D(0) = ros_vfp_sin(s, D(0)))
FN1(f64cos, D(0) = ros_vfp_cos(s, D(0)))
FN1(f64tan, D(0) = ros_vfp_tan(s, D(0)))
FN1(f64asin, D(0) = ros_vfp_asin(s, D(0)))
FN1(f64acos, D(0) = ros_vfp_acos(s, D(0)))
FN1(f64atan, D(0) = ros_vfp_atan(s, D(0)))
FN1(f64atan2, D(0) = ros_vfp_atan2(s, D(0), D(1)))
FN1(f64log, D(0) = ros_vfp_log(s, D(0)))
FN1(f64log10, D(0) = ros_vfp_log10(s, D(0)))
FN1(f64exp, D(0) = ros_vfp_exp(s, D(0)))
FN1(f64pow, D(0) = ros_vfp_pow(s, D(0), D(1)))
FN1(f64fma, double r = fma(D(0), D(1), D(2)); s->fp->fpscr |= ros_vfp_ex3(r, D(0), D(1), D(2)); D(0) = r)

static ros_code *const singles[12] = { f32sin, f32cos, f32tan, f32asin, f32acos, f32atan,
                                       f32atan2, f32log, f32log10, f32exp, f32pow, f32fma };
static ros_code *const doubles[12] = { f64sin, f64cos, f64tan, f64asin, f64acos, f64atan,
                                       f64atan2, f64log, f64log10, f64exp, f64pow, f64fma };

/* FastAPI: the four context calls, which a program can call with BL. */
static void fast_call(struct ros_cpu *s, void (*thunk)(struct ros_cpu *))
{
    thunk(s);
    RET;
}
static void fast_check(struct ros_cpu *s) { fast_call(s, ros_thunk_VFPSupport_CheckContext); }
static void fast_create(struct ros_cpu *s) { fast_call(s, ros_thunk_VFPSupport_CreateContext); }
static void fast_destroy(struct ros_cpu *s) { fast_call(s, ros_thunk_VFPSupport_DestroyContext); }
static void fast_change(struct ros_cpu *s) { fast_call(s, ros_thunk_VFPSupport_ChangeContext); }

/* ---- the module ---------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    if (!single_table) {
        /* The table needs twelve entries in a row, four bytes apart.
         * Native entries are given out consecutively. */
        single_table = ros_native_entry(singles[0], "VFPSupport:fp32sin");
        for (int i = 1; i < 12; i++)
            ros_native_entry(singles[i], "VFPSupport:fp32");
        double_table = ros_native_entry(doubles[0], "VFPSupport:fp64sin");
        for (int i = 1; i < 12; i++)
            ros_native_entry(doubles[i], "VFPSupport:fp64");
        fast[0] = ros_native_entry(fast_check, "VFPSupport:CheckContext");
        fast[1] = ros_native_entry(fast_create, "VFPSupport:CreateContext");
        fast[2] = ros_native_entry(fast_destroy, "VFPSupport:DestroyContext");
        fast[3] = ros_native_entry(fast_change, "VFPSupport:ChangeContext");
        if (double_table != single_table + 48)
            return ros_error(ROS_ERR_UNIMPLEMENTED, "VFPSupport: its tables are not in a row");
        descriptor = ros_addr(ros_rma_alloc(4));
        ros_st32(descriptor, 0xFFFFFFFFu);      /* an empty dump format */
    }
    active = 0;
    pending = 0;
    ros_fp_user_return = take_pending;
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    ros_fp_user_return = NULL;
    pending = 0;
    change(0);
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module vfpsupport_module = {
    .title = "VFPSupport",
    .help = "VFPSupport\t0.18 (25 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x58EC0,
    .swi_thunks = ros_swi_thunks_VFPSupport,
    .swi_names = ros_swi_names_VFPSupport,
    .swi_prefix = "VFPSupport",
};

__attribute__((constructor)) static void count(void)
{
    vfpsupport_module.swi_count = ros_swi_count_VFPSupport;
}
