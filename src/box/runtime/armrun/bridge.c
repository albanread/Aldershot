/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* bridge.c: the ARM container's bridge (bridge.h; see also ABI.md).
 *
 * The engine runs the task's ARM code in user mode. It calls back here for
 * every SVC, every exception and every access that fastmem could not make.
 * This file is RISC OS's side of those. It does the following:
 *
 *   - forwards the SWI in its X form.
 *   - turns OS_Exit and a non-X error into the exit and error handlers'
 *     deliveries (ABI.md §2.1, §2.2).
 *   - turns the engine's exceptions into 5.30's errors (§3a).
 *   - enters transient callbacks in SVC mode at safe points, and returns
 *     from them (§2.7).
 *   - provides the way back to user mode through a register block (§2.8).
 *   - makes calls into the task's code from inside its own SWIs, on a nested
 *     engine.
 *   - makes calls from its code to native code (#146).
 *
 * For a call to native code, a branch to an address that the runtime says is
 * native is fetched as the gateways' SVC. The gate op then calls the native
 * code with the ARM registers. That code may call ARM code again on the next
 * engine, and so on to DEPTH.
 *
 * Nothing changes the engine's registers while it runs a block. A hook that
 * decides the task goes somewhere else records the registers it is to have
 * ("install") and halts the engine. The loop installs them when Run returns.
 * The engine is user mode only, so the mode that RISC OS would be in is kept
 * here (t->mode). A delivery entered in SVC mode saves the context that it
 * interrupted in a frame. Its return goes to a marker address that halts the
 * engine, and restores the saved context.
 */
#include "bridge.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "armrun.h"
#include "fpa.h"

#define DEPTH        ARM_DEPTH      /* engines: the task, and calls nested inside its SWIs
                                       and the native code it calls (bridge.h) */
#define FRAMES       16             /* SVC-mode deliveries nested */
#define CALLBACKS    32
#define BUDGET       500000         /* instructions between safe points: about a millisecond */
#define MARK_SWI     0xFFFFFFu      /* SVC &FFFFFF: the marker's instruction */
#define GATE_SWI     0xFFFFFEu      /* SVC &FFFFFE: a gateway's (bridge.h) */
#define MARK_RETURN  0xFFFFFFF0u    /* R14 for the code the bridge enters: fetching it halts */
#define CPSR_USR     0x110u         /* USR, the A bit set: 5.30's user mode (ABI.md §3a) */
#define CPSR_FLAGS   0xF8000000u    /* NZCVQ */
#define ERR_UNDEF    0x80000000u
#define ERR_PREFETCH 0x80000001u
#define ERR_DATA     0x80000002u

struct regs {                       /* a context: r0-r15, the flags, the modelled mode */
    uint32_t r[16], cpsr, mode;
};

struct frame {                      /* an SVC-mode delivery in progress */
    struct regs saved;
    unsigned depth;
};

/* The banked registers of the modes RISC OS code uses (ABI.md §3): R13,
 * R14 and the SPSR of each; R0-R12 are shared (no FIQ code runs here) */
enum { BANK_USR, BANK_SVC, BANK_UND, BANK_IRQ, BANKS };

static unsigned bank_of(uint32_t mode)
{
    switch (mode & 0x1F) {
    case 0x13: return BANK_SVC;
    case 0x1B: return BANK_UND;
    case 0x12: case 0x11: return BANK_IRQ;
    default: return BANK_USR;
    }
}

struct arm_task {
    uintptr_t base;
    struct arm_ops ops;
    struct armrun *jit[DEPTH];
    unsigned depth;                 /* the engine running now */
    uint32_t mode;                  /* the mode RISC OS would be in */

    int ended;
    uint32_t rc;

    int installing;                 /* install these registers when Run returns */
    struct regs install;

    struct frame frames[FRAMES];
    unsigned nframes;

    int returned[DEPTH];            /* a nested call's code has returned */
    uint32_t call_error[DEPTH];

    int aborted;                    /* a data abort: delivered when Run returns, R15 exact */
    uint32_t abort_addr;            /* its access, for the report (ops.fault) */
    unsigned abort_size;
    int abort_write;
    uint32_t fp_error;              /* an FPA trap: delivered at the end of the block */

    int resume;                     /* after this SWI: go on from a register block */
    uint32_t resume_block;

    pthread_mutex_t mu;             /* the callback queue, posted from any thread */
    struct { uint32_t code, r12; } cb[CALLBACKS];
    unsigned ncb;

    uint32_t bank13[BANKS], bank14[BANKS], spsr[BANKS];

    int calls_only;                 /* no application: an error ends the calls (arm_task_create_calls) */
    uint32_t last_error;
    char last_text[252];

    struct arm_task *next;          /* every task, for code that changes (arm_task_synchronise_all) */

    struct arm_task_counts counts;
};

static pthread_mutex_t tasks_mu = PTHREAD_MUTEX_INITIALIZER;
static struct arm_task *tasks;

/* ---- what the container costs --------------------------------------------------
 *
 * Each thread's time is in one place at once. The places are the engine (ARM
 * code, translated or being translated), native code that the ARM code
 * called (a SWI, a gateway), and the native FPA. Switching place adds the
 * time since the last switch to the place left. A SWI that calls back into
 * ARM code is therefore counted once, with each part where it ran. The totals
 * are for every task and thread there has been. At each safe point (about
 * every millisecond of ARM code) the PC is also sampled, to show where the
 * time goes. */

enum { AT_NONE, AT_ENGINE, AT_NATIVE, AT_FPA, AT_PLACES };

static _Thread_local int at_place;
static _Thread_local uint64_t at_since;
static _Atomic uint64_t at_ns[AT_PLACES], at_ticks, at_swis, at_calls, at_reset;

#define SAMPLES 4096
static struct { uint32_t key, n; } samples[SAMPLES];
static _Atomic uint64_t sampled;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int at_go(int place)
{
    uint64_t t = now_ns();
    int was = at_place;
    if (was != AT_NONE)
        atomic_fetch_add_explicit(&at_ns[was], t - at_since, memory_order_relaxed);
    at_place = place, at_since = t;
    return was;
}

static void sample_pc(uint32_t pc)
{
    uint32_t key = (pc >> 4) | 1, h = (key * 2654435761u) % SAMPLES;
    for (unsigned k = 0; k < 16; k++, h = (h + 1) % SAMPLES)
        if (samples[h].key == key || !samples[h].key) {
            samples[h].key = key;
            samples[h].n++;
            atomic_fetch_add_explicit(&sampled, 1, memory_order_relaxed);
            return;
        }
}

void arm_stats_read(struct arm_stats *st)
{
    at_go(at_place);                                /* this thread's time so far */
    st->ns_engine = atomic_load(&at_ns[AT_ENGINE]);
    st->ns_native = atomic_load(&at_ns[AT_NATIVE]);
    st->ns_fpa = atomic_load(&at_ns[AT_FPA]);
    st->ticks = atomic_load(&at_ticks);
    st->swis = atomic_load(&at_swis);
    st->calls = atomic_load(&at_calls);
    st->samples = atomic_load(&sampled);
    uint64_t r = atomic_load(&at_reset);
    st->ns_wall = r ? now_ns() - r : 0;
}

void arm_tasks_count(unsigned *apps, unsigned *calls)
{
    unsigned a = 0, c = 0;
    pthread_mutex_lock(&tasks_mu);
    for (struct arm_task *t = tasks; t; t = t->next)
        t->calls_only ? c++ : a++;
    pthread_mutex_unlock(&tasks_mu);
    *apps = a, *calls = c;
}

void arm_stats_reset(void)
{
    for (int k = 0; k < AT_PLACES; k++)
        atomic_store(&at_ns[k], 0);
    atomic_store(&at_ticks, 0), atomic_store(&at_swis, 0), atomic_store(&at_calls, 0);
    atomic_store(&sampled, 0);
    memset(samples, 0, sizeof samples);
    at_since = now_ns();
    atomic_store(&at_reset, at_since);
}

unsigned arm_stats_samples(uint32_t *addr, uint32_t *count, unsigned max)
{
    unsigned n = 0;
    for (unsigned h = 0; h < SAMPLES; h++)
        if (samples[h].key) {
            /* keep the max largest, by insertion */
            uint32_t a = (samples[h].key & ~1u) << 4, c = samples[h].n;
            unsigned j = n < max ? n++ : max;
            if (j == max && (!max || count[max - 1] >= c))
                continue;
            if (j == max)
                j = max - 1;
            while (j > 0 && count[j - 1] < c) {
                addr[j] = addr[j - 1], count[j] = count[j - 1];
                j--;
            }
            addr[j] = a, count[j] = c;
        }
    return n;
}

/* ---- guest memory ---------------------------------------------------------- */

static uint32_t ld32(struct arm_task *t, uint32_t a)
{
    uint32_t v;
    memcpy(&v, (void *)(t->base + a), 4);
    return v;
}

static void st32(struct arm_task *t, uint32_t a, uint32_t v)
{
    memcpy((void *)(t->base + a), &v, 4);
}

/* ---- contexts ---------------------------------------------------------------- */

static void get_regs(struct arm_task *t, unsigned d, struct regs *x)
{
    memcpy(x->r, armrun_regs(t->jit[d]), sizeof x->r);
    x->cpsr = armrun_cpsr(t->jit[d]);
    x->mode = t->mode;
}

static void put_regs(struct arm_task *t, unsigned d, const struct regs *x)
{
    memcpy(armrun_regs(t->jit[d]), x->r, sizeof x->r);
    /* the engine is user mode whatever the bits say; RISC OS's mode is kept
     * here, and in the CPSR's mode bits, which MRS reads */
    armrun_set_cpsr(t->jit[d], (x->cpsr & (CPSR_FLAGS | 0xC0u)) | 0x100u | (x->mode & 0x1F));
    t->mode = x->mode;
}

static void to_cpu(const struct regs *x, struct ros_cpu *s)
{
    memcpy(s->r, x->r, sizeof s->r);
    s->n = x->cpsr >> 31, s->z = (x->cpsr >> 30) & 1, s->c = (x->cpsr >> 29) & 1;
    s->v = (x->cpsr >> 28) & 1, s->q = (x->cpsr >> 27) & 1;
    s->irq_off = (x->cpsr >> 7) & 1;
    s->mode = x->mode;
}

/* A mode change in x: R13 and R14 to the old mode's bank, the new mode's in */
static void switch_mode(struct arm_task *t, struct regs *x, uint32_t mode)
{
    unsigned from = bank_of(x->mode), to = bank_of(mode);
    if (from != to) {
        t->bank13[from] = x->r[13], t->bank14[from] = x->r[14];
        x->r[13] = t->bank13[to], x->r[14] = t->bank14[to];
    }
    x->mode = mode & 0x1F;
    x->cpsr = (x->cpsr & ~0x1Fu) | x->mode;
}

static void from_cpu(const struct ros_cpu *s, struct regs *x)
{
    memcpy(x->r, s->r, sizeof x->r);
    x->cpsr = (s->n << 31) | (s->z << 30) | (s->c << 29) | (s->v << 28) | (s->q << 27) |
              (s->irq_off << 7) | CPSR_USR;
    x->mode = s->mode;
}

/* The engine at depth d is to go on from x: when Run returns */
static void install(struct arm_task *t, unsigned d, const struct regs *x)
{
    t->install = *x;
    t->installing = 1;
    armrun_halt(t->jit[d]);
}

/* ---- deliveries -------------------------------------------------------------- */

/* The task ends: no handler to take it */
static void end(struct arm_task *t, unsigned d, uint32_t rc, uint32_t errnum, const char *text)
{
    t->ended = 1;
    t->rc = rc;
    if (t->ops.ended)
        t->ops.ended(t->ops.ctx, t, rc, errnum, text);
    for (unsigned i = 0; i <= d; i++)
        armrun_halt(t->jit[i]);
}

/* An error, as OS_GenerateError delivers it (ABI.md §2.1). The error
 * handler's buffer is filled with the raise's return PC, the number and the
 * text. The handler is entered in USR mode with R0 its private word, and the
 * caller's other registers as they were. It does not return. Inside a nested
 * call the call ends with the error, and the delivery is made to the task as
 * the engines unwind (the depth-0 engine takes it). */
static void error(struct arm_task *t, unsigned d, uint32_t pc, uint32_t errnum, const char *text)
{
    t->counts.deliveries++;
    if (t->calls_only) {                           /* the calls end; the native caller has it */
        t->last_error = errnum;
        snprintf(t->last_text, sizeof t->last_text, "%s", text);
        for (unsigned i = d; i > 0; i--) {
            t->returned[i] = 1;
            t->call_error[i] = errnum ? errnum : ERR_UNDEF;
            armrun_halt(t->jit[i]);
        }
        t->nframes = 0;
        return;
    }
    uint32_t code, r12, buf;
    if (!t->ops.handler || !t->ops.handler(t->ops.ctx, ARM_HANDLER_ERROR, &code, &r12, &buf)) {
        end(t, d, 1, errnum, text);
        return;
    }
    st32(t, buf, pc);
    st32(t, buf + 4, errnum);
    size_t n = strnlen(text, 251);
    memcpy((void *)(t->base + buf + 8), text, n);
    *(char *)(t->base + buf + 8 + n) = 0;
    for (unsigned i = d; i > 0; i--) {             /* nested calls end with it */
        t->returned[i] = 1;
        t->call_error[i] = errnum;
        armrun_halt(t->jit[i]);
    }
    t->nframes = 0;                                /* the stacks are flattened */
    struct regs x;
    get_regs(t, 0, &x);
    if (d == 0)
        get_regs(t, d, &x);
    x.r[0] = r12;
    x.r[15] = code;
    x.cpsr = (x.cpsr & CPSR_FLAGS) | CPSR_USR;     /* IRQs enabled */
    x.mode = ROS_MODE_USR;
    t->install = x;
    t->installing = 1;
    armrun_halt(t->jit[0]);
}

/* An error whose block is in guest memory */
static void error_block(struct arm_task *t, unsigned d, uint32_t pc, uint32_t block)
{
    char text[252];
    size_t i = 0;
    for (; i < sizeof text - 1; i++) {
        char c = *(const char *)(t->base + block + 4 + i);
        if (!c)
            break;
        text[i] = c;
    }
    text[i] = 0;
    error(t, d, pc, ld32(t, block), text);
}

static void error_at(struct arm_task *t, unsigned d, uint32_t errnum, const char *what, uint32_t pc)
{
    if (t->ops.fault && t->jit[d]) {
        uint32_t r[16];
        memcpy(r, armrun_regs(t->jit[d]), sizeof r);
        r[15] = pc;
        int data = errnum == ERR_DATA;
        t->ops.fault(t->ops.ctx, t, what, r, armrun_cpsr(t->jit[d]), t->mode, d,
                     data ? t->abort_addr : pc, data ? t->abort_size : 0, data && t->abort_write);
    }
    char text[96];
    snprintf(text, sizeof text, "Internal error: %s at &%08X", what, pc);
    error(t, d, pc, errnum, text);
}

/* OS_Exit (ABI.md §2.2): enters the exit handler in USR mode, with R0 0, R1
 * the caller's, R2 the return code and R12 its private word. With no
 * handler, it ends the task with the return code ("ABEX" in R1 gives R2's) */
static void exit_task(struct arm_task *t, unsigned d, uint32_t r1, uint32_t r2)
{
    t->counts.deliveries++;
    if (t->calls_only) {                           /* not an application's to end */
        error(t, d, 0, ERR_UNDEF, "OS_Exit from module code");
        return;
    }
    uint32_t code, r12, buf;
    if (!t->ops.handler || !t->ops.handler(t->ops.ctx, ARM_HANDLER_EXIT, &code, &r12, &buf)) {
        end(t, d, r1 == 0x58454241u ? r2 : 0, 0, NULL);
        return;
    }
    struct regs x;
    get_regs(t, d, &x);
    x.r[0] = 0, x.r[1] = r1, x.r[2] = r2, x.r[12] = r12, x.r[15] = code;
    x.cpsr = (x.cpsr & CPSR_FLAGS) | CPSR_USR;
    x.mode = ROS_MODE_USR;
    t->nframes = 0;
    install(t, d, &x);
}

/* A transient callback (ABI.md §2.7): SVC mode, R12 its workspace, and every
 * other register the interrupted context's. Its return goes back there */
static void enter_callback(struct arm_task *t, uint32_t code, uint32_t r12)
{
    t->counts.callbacks++;
    t->counts.deliveries++;
    struct frame *f = &t->frames[t->nframes++];
    get_regs(t, 0, &f->saved);
    f->depth = 0;
    struct regs x = f->saved;
    x.r[12] = r12;
    x.r[13] = t->ops.svc_stack;
    x.r[14] = MARK_RETURN;
    x.r[15] = code;
    x.mode = ROS_MODE_SVC;
    put_regs(t, 0, &x);
}

/* ---- the privileged instructions --------------------------------------------- */

static uint32_t ror32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static int guest_ok(struct arm_task *t, uint32_t addr, unsigned size, int write);

/* An instruction's condition against the flags. NV is the unconditional
 * space, which CPS uses, and it passes. */
static int cond_passed(uint32_t cond, uint32_t cpsr)
{
    int n = cpsr >> 31 & 1, z = cpsr >> 30 & 1, c = cpsr >> 29 & 1, v = cpsr >> 28 & 1;
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    default: return 1;
    }
}

/* An instruction that the engine raised (undefined, unpredictable or not
 * translated) and that RISC OS code runs in a privileged mode. These are the
 * mode changes and banked transfers, which the engine does not model because
 * it is user mode only (ABI.md §3). Returns 1 when it was done (the
 * registers installed, R15 past it). Returns 0 when it is an undefined
 * instruction, as it is in user mode. RISC OS 5.30 gives that error for each
 * of them there. */
static int privileged(struct arm_task *t, unsigned d, uint32_t pc)
{
    if (!guest_ok(t, pc, 4, 0))
        return 0;
    uint32_t w = ld32(t, pc);
    struct regs x;
    get_regs(t, d, &x);
    const int priv = x.mode != ROS_MODE_USR;
    const unsigned b = bank_of(x.mode);
    x.r[15] = pc + 4;
    /* the engine hands some over before their condition is tested (LDM/STM ^) */
    if (!cond_passed(w >> 28, x.cpsr)) {
        install(t, d, &x);
        return 1;
    }
    /* CPS: the interrupt masks, and the mode (a no-op in user mode) */
    if ((w & 0xFFF1FE20u) == 0xF1000000u) {
        if (priv) {
            unsigned imod = (w >> 18) & 3;
            uint32_t bits = w & 0x1C0u;
            if (imod == 2)
                x.cpsr &= ~bits;
            else if (imod == 3)
                x.cpsr |= bits;
            if ((w >> 17) & 1)
                switch_mode(t, &x, w & 0x1F);
        }
        install(t, d, &x);
        return 1;
    }

    /* MSR, CPSR or SPSR, immediate or register */
    if ((w & 0x0FB00000u) == 0x03200000u || (w & 0x0FB0FFF0u) == 0x0120F000u) {
        uint32_t v = (w & 0x02000000u) ? ror32(w & 0xFF, 2 * ((w >> 8) & 0xF)) : x.r[w & 0xF];
        uint32_t mask = (w >> 16) & 0xF, bytes = 0;
        for (unsigned i = 0; i < 4; i++)
            if (mask & (1u << i))
                bytes |= 0xFFu << (8 * i);
        if ((w >> 22) & 1) {                        /* SPSR */
            if (!priv)
                return 0;
            t->spsr[b] = (t->spsr[b] & ~bytes) | (v & bytes);
        } else {
            if (!priv)
                bytes &= 0xFF000000u;               /* user mode writes the flags only */
            uint32_t cpsr = (x.cpsr & ~bytes) | (v & bytes);
            x.cpsr = (x.cpsr & 0x1Fu) | (cpsr & ~0x1Fu);
            if ((bytes & 0x1F) && (cpsr & 0x1F) != x.mode)
                switch_mode(t, &x, cpsr & 0x1F);
        }
        install(t, d, &x);
        return 1;
    }
    /* MRS Rd, SPSR */
    if ((w & 0x0FFF0FFFu) == 0x014F0000u) {
        if (!priv)
            return 0;
        x.r[(w >> 12) & 15] = t->spsr[b];
        install(t, d, &x);
        return 1;
    }
    /* LDM/STM with ^: the user bank, or (LDM with pc) an exception return */
    if ((w & 0x0E400000u) == 0x08400000u) {
        if (!priv)
            return 0;
        unsigned rn = (w >> 16) & 15, n = (unsigned)__builtin_popcount(w & 0xFFFF);
        int load = (w >> 20) & 1, up = (w >> 23) & 1, pre = (w >> 24) & 1, wb = (w >> 21) & 1;
        int ret = load && (w & 0x8000);
        uint32_t base = x.r[rn], addr = up ? base + (pre ? 4 : 0) : base - 4 * n + (pre ? 0 : 4);
        if (!guest_ok(t, addr, 4 * n, !load))
            return 0;
        for (unsigned r = 0; r < 16; r++) {
            if (!(w & (1u << r)))
                continue;
            int user = !ret && (r == 13 || r == 14) && b != BANK_USR;   /* the user bank's */
            if (load) {
                uint32_t v = ld32(t, addr);
                if (user)
                    *(r == 13 ? &t->bank13[BANK_USR] : &t->bank14[BANK_USR]) = v;
                else
                    x.r[r] = v;
            } else {
                uint32_t v = user ? (r == 13 ? t->bank13[BANK_USR] : t->bank14[BANK_USR])
                                  : r == 15 ? pc + 8 : x.r[r];
                st32(t, addr, v);
            }
            addr += 4;
        }
        if (wb && !(load && (w & (1u << rn))))
            x.r[rn] = up ? base + 4 * n : base - 4 * n;
        if (ret) {                                  /* CPSR from the SPSR */
            uint32_t spsr = t->spsr[b];
            x.cpsr = (spsr & ~0x1Fu) | x.mode;
            switch_mode(t, &x, spsr & 0x1F);
        }
        install(t, d, &x);
        return 1;
    }
    /* Data processing with S into pc: MOVS pc, lr; SUBS pc, lr, #4. CPSR comes from the SPSR */
    if ((w & 0x0C10F000u) == 0x0010F000u && priv) {
        uint32_t op2;
        if (w & 0x02000000u)
            op2 = ror32(w & 0xFF, 2 * ((w >> 8) & 0xF));
        else if ((w & 0xFF0u) == 0)
            op2 = x.r[w & 0xF] + ((w & 0xF) == 15 ? 4 : 0);
        else
            return 0;                               /* a shifted operand: none seen */
        uint32_t rn = x.r[(w >> 16) & 15], res;
        switch ((w >> 21) & 0xF) {
        case 0xD: res = op2; break;                 /* MOVS */
        case 0x2: res = rn - op2; break;            /* SUBS */
        case 0x4: res = rn + op2; break;            /* ADDS */
        case 0xF: res = ~op2; break;                /* MVNS */
        default: return 0;
        }
        uint32_t spsr = t->spsr[b];
        x.cpsr = (spsr & ~0x1Fu) | x.mode;
        switch_mode(t, &x, spsr & 0x1F);
        x.r[15] = res;
        install(t, d, &x);
        return 1;
    }
    return 0;
}

/* ---- the engine's hooks ------------------------------------------------------ */

static void on_svc(void *ctx, struct armrun *a, uint32_t number)
{
    struct arm_task *t = ctx;
    unsigned d = t->depth;
    (void)a;
    uint32_t n = number & 0xFFFFFF, x_bit = n & 0x20000, swi = n & ~0x20000u;
    if (t->fp_error || t->aborted)
        return;                     /* an error is waiting for the block's end: nothing after it acts */
    if (n == GATE_SWI) {
        /* a gateway: the native library's entry, for an ARM client; or a
         * branch to native code (#146), called as the gateways are */
        struct regs r;
        get_regs(t, d, &r);
        struct ros_cpu s;
        to_cpu(&r, &s);
        uint32_t gate = r.r[15] - 4, back = r.r[14];
        int gateway = gate >= ARM_GATE_LO && gate < ARM_GATE_HI;
        if (!gateway && !(t->ops.native && t->ops.native(t->ops.ctx, gate))) {
            /* native when translated, not now (its range has gone): the
             * translation goes, and the code there is fetched as ARM */
            armrun_invalidate(t->jit[d], gate, 4);
            r.r[15] = gate;
            put_regs(t, d, &r);
            return;
        }
        if (gateway)
            t->counts.swis++;
        else
            t->counts.natives++;
        atomic_fetch_add_explicit(&at_swis, 1, memory_order_relaxed);
        int was = at_go(AT_NATIVE);
        int gated = t->ops.gate ? t->ops.gate(t->ops.ctx, t, gate, &s) : 0;
        at_go(was);
        t->depth = d;                               /* (calls it made into ARM code are over) */
        if (!gated) {
            error_at(t, d, ERR_PREFETCH, "abort on instruction fetch", gate);
            return;
        }
        if (t->ended || t->installing)
            return;                                 /* a call inside it erred: delivered */
        if (d > 0 && t->returned[d])
            return;                                 /* ... or ended the calls (calls only) */
        if (gated == ARM_GATE_RAISED) {
            /* As a non-X SWI's error: to the error handler, with the
             * raise's PC as the return */
            r.r[15] = back;
            put_regs(t, d, &r);
            error_block(t, d, back, s.r[0]);
            return;
        }
        from_cpu(&s, &r);
        r.mode = t->mode;
        if (gateway) {
            r.r[14] = back;
            r.r[15] = back;                         /* the entry returns to its caller */
        }                                           /* native code: where it left R15 */
        put_regs(t, d, &r);
        if (t->ncb && d == 0)
            armrun_halt(t->jit[0]);
        return;
    }
    if (n == MARK_SWI) {
        /* code the bridge entered has returned to its R14 */
        if (t->nframes && t->frames[t->nframes - 1].depth == d) {
            struct regs back = t->frames[--t->nframes].saved;
            install(t, d, &back);
        } else if (d > 0) {
            t->returned[d] = 1;
            armrun_halt(t->jit[d]);
        } else {
            /* the task's own entry has returned to the R14 it was given: as
             * FileSwitch's ReturnFromAbsoluteCode, OS_Exit with R0-R2 zero */
            exit_task(t, d, 0, 0);
        }
        return;
    }
    t->counts.swis++;
    struct regs r;
    get_regs(t, d, &r);
    switch (swi) {
    case 0x11:                                      /* OS_Exit */
        exit_task(t, d, r.r[1], r.r[2]);
        return;
    case 0x2B:                                      /* OS_GenerateError */
        if (!x_bit) {
            error_block(t, d, r.r[15], r.r[0]);
            return;
        }
        r.cpsr |= 1u << 28;                         /* X: back with V set, R0 the error */
        put_regs(t, d, &r);
        return;
    case 0x16:                                      /* OS_EnterOS: SVC mode, the SVC stack */
        if (r.mode == ROS_MODE_USR) {
            t->spsr[BANK_SVC] = (r.cpsr & ~0x1Fu) | ROS_MODE_USR;
            switch_mode(t, &r, ROS_MODE_SVC);
        }
        put_regs(t, d, &r);
        return;
    case 0x7C:                                      /* OS_LeaveOS: back to USR */
        if (r.mode != ROS_MODE_USR)
            switch_mode(t, &r, ROS_MODE_USR);
        put_regs(t, d, &r);
        return;
    case 0x6E:                                      /* OS_SynchroniseCodeAreas */
        arm_task_synchronise_all((r.r[0] & 1) ? r.r[1] : 0, (r.r[0] & 1) ? r.r[2] - r.r[1] + 4 : 0);
        return;
    }
    struct ros_cpu s;
    to_cpu(&r, &s);
    s.v = 0;                                        /* a SWI that succeeds returns V clear */
    atomic_fetch_add_explicit(&at_swis, 1, memory_order_relaxed);
    int was = at_go(AT_NATIVE);
    t->ops.swi(t->ops.ctx, t, &s, swi | 0x20000u);
    at_go(was);
    if (t->ended || t->installing)
        return;                                     /* the SWI ended the task, or a call inside it erred */
    uint32_t pc = r.r[15];
    from_cpu(&s, &r);
    r.r[15] = pc;                                   /* the SWI returns after itself */
    r.mode = t->mode;
    if (s.v && !x_bit) {
        put_regs(t, d, &r);
        error_block(t, d, pc, s.r[0]);
        return;
    }
    if (t->resume) {                                /* back to user mode through a block */
        t->resume = 0;
        struct regs b;
        for (unsigned i = 0; i < 16; i++)
            b.r[i] = ld32(t, t->resume_block + 4 * i);
        b.cpsr = (ld32(t, t->resume_block + 64) & CPSR_FLAGS) | CPSR_USR;
        b.mode = ROS_MODE_USR;
        install(t, d, &b);
        return;
    }
    put_regs(t, d, &r);
    if (t->ncb && d == 0)
        armrun_halt(t->jit[0]);                     /* a safe point: the callbacks are waiting */
}

static void on_exception(void *ctx, struct armrun *a, uint32_t pc, int kind)
{
    struct arm_task *t = ctx;
    (void)a;
    if (t->fp_error || t->aborted)
        return;
    switch (kind) {
    case ARMRUN_UNDEFINED:
    case ARMRUN_UNPREDICTABLE:
    case ARMRUN_DECODE_ERROR:
    case ARMRUN_INTERPRET:
        if (privileged(t, t->depth, pc))
            return;
        error_at(t, t->depth, ERR_UNDEF, "undefined instruction", pc);
        return;
    case ARMRUN_NO_EXECUTE:
    case ARMRUN_BREAKPOINT:
        error_at(t, t->depth, ERR_PREFETCH, "abort on instruction fetch", pc);
        return;
    default:                                        /* hints: nothing */
        return;
    }
}

static int guest_ok(struct arm_task *t, uint32_t addr, unsigned size, int write)
{
    return t->ops.valid ? t->ops.valid(t->ops.ctx, addr, size, write) : 1;
}

static uint64_t on_read(void *ctx, struct armrun *a, uint32_t addr, unsigned size)
{
    struct arm_task *t = ctx;
    uint64_t v = 0;
    if (guest_ok(t, addr, size, 0)) {
        memcpy(&v, (void *)(t->base + addr), size);
        return v;
    }
    t->aborted = 1;                                 /* the loop delivers it, R15 exact */
    t->abort_addr = addr, t->abort_size = size, t->abort_write = 0;
    armrun_abort(a);
    return 0;
}

static void on_write(void *ctx, struct armrun *a, uint32_t addr, unsigned size, uint64_t v)
{
    struct arm_task *t = ctx;
    if (guest_ok(t, addr, size, 1)) {
        memcpy((void *)(t->base + addr), &v, size);
        return;
    }
    t->aborted = 1;
    t->abort_addr = addr, t->abort_size = size, t->abort_write = 1;
    armrun_abort(a);
}

static int on_fetch(void *ctx, struct armrun *a, uint32_t addr, uint32_t *word)
{
    struct arm_task *t = ctx;
    (void)a;
    if (addr == MARK_RETURN) {
        *word = 0xEF000000u | MARK_SWI;
        return 1;
    }
    if ((addr >= ARM_GATE_LO && addr < ARM_GATE_HI) || (t->ops.native && t->ops.native(t->ops.ctx, addr))) {
        /* a gateway, or native code (#146): on_svc finds which from R15.
         * The translation is cached: native code that goes is found at the
         * call (on_svc), and ARM code put where native code was invalidates
         * the range (arm_task_synchronise_all) */
        *word = 0xEF000000u | GATE_SWI;
        return 1;
    }
    if (!guest_ok(t, addr, 4, 0))
        return 0;                                   /* the engine raises NoExecuteFault */
    *word = ld32(t, addr);
    return 1;
}

/* An FPA instruction (CP1, CP2): native (fpa.c), on the task's floating point */
static uint64_t on_coproc(void *ctx, struct armrun *a, uint32_t word, uint32_t arg)
{
    struct arm_task *t = ctx;
    uint32_t err;
    int was = at_go(AT_FPA);
    uint64_t r = fpa_execute(t->ops.fp, t->base, word, arg, &err);
    at_go(was);
    if (err && !t->fp_error) {
        t->fp_error = err;          /* the call has no PC: the error comes at the block's end */
        armrun_halt(a);
    }
    return r;
}

/* ---- the loop ---------------------------------------------------------------- */

static struct armrun *engine(struct arm_task *t, unsigned d)
{
    if (!t->jit[d]) {
        const struct armrun_hooks hooks = { t, on_svc, on_exception, on_read, on_write, on_fetch,
                                            t->ops.fp ? on_coproc : NULL };
        t->jit[d] = armrun_create(t->base, d ? (8u << 20) : 0, &hooks);
    }
    return t->jit[d];
}

/* Run the engine at depth d until the task ends, or (d > 0) its call returns */
static void loop(struct arm_task *t, unsigned d)
{
    for (;;) {
        t->depth = d;
        int was = at_go(AT_ENGINE);
        armrun_run(t->jit[d], BUDGET);
        at_go(was);
        uint64_t ran = BUDGET - armrun_ticks_left(t->jit[d]);
        t->counts.ticks += ran;
        atomic_fetch_add_explicit(&at_ticks, ran, memory_order_relaxed);
        if (ran >= BUDGET)                          /* the budget ran out: where it was */
            sample_pc(armrun_regs(t->jit[d])[15]);
        if (t->ended)
            return;
        if (t->fp_error) {                          /* an FPA trap, as FPEmulator raises it */
            uint32_t err = t->fp_error;
            t->fp_error = 0;
            if (err == ERR_UNDEF)
                error_at(t, d, ERR_UNDEF, "undefined instruction", armrun_regs(t->jit[d])[15]);
            else
                error(t, d, armrun_regs(t->jit[d])[15], err, fpa_error_text(err));
            if (t->ended)
                return;
        }
        if (t->aborted) {                           /* stopped at the access: R15 is its instruction */
            t->aborted = 0;
            error_at(t, d, ERR_DATA, "abort on data transfer", armrun_regs(t->jit[d])[15]);
            if (t->ended)
                return;
        }
        if (t->installing) {
            if (d > 0 && t->returned[d])
                return;                             /* an error ended the call: depth 0 installs it */
            t->installing = 0;
            put_regs(t, d, &t->install);
            continue;
        }
        if (d > 0 && t->returned[d])
            return;
        /* a safe point: the runtime's background work, then transient
         * callbacks at depth 0 in user mode */
        if (t->ops.poll)
            t->ops.poll(t->ops.ctx, t);
        if (t->ended)
            return;
        if (d == 0 && t->mode == ROS_MODE_USR) {
            pthread_mutex_lock(&t->mu);
            int have = t->ncb > 0;
            uint32_t code = 0, r12 = 0;
            if (have) {
                code = t->cb[0].code, r12 = t->cb[0].r12;
                memmove(t->cb, t->cb + 1, --t->ncb * sizeof t->cb[0]);
            }
            pthread_mutex_unlock(&t->mu);
            if (have)
                enter_callback(t, code, r12);
        }
    }
}

/* ---- the interface ----------------------------------------------------------- */

struct arm_task *arm_task_create(uintptr_t base, const struct arm_ops *ops)
{
    struct arm_task *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->base = base;
    t->ops = *ops;
    t->mode = ROS_MODE_USR;
    t->bank13[BANK_SVC] = ops->svc_stack;
    pthread_mutex_init(&t->mu, NULL);
    engine(t, 0);
    pthread_mutex_lock(&tasks_mu);
    t->next = tasks;
    tasks = t;
    pthread_mutex_unlock(&tasks_mu);
    return t;
}

struct arm_task *arm_task_create_calls(uintptr_t base, const struct arm_ops *ops)
{
    struct arm_task *t = arm_task_create(base, ops);
    if (t)
        t->calls_only = 1;
    return t;
}

uint32_t arm_task_sp(const struct arm_task *t, uint32_t *mode)
{
    *mode = t->mode;
    return t->jit[t->depth] ? armrun_regs(t->jit[t->depth])[13] : 0;
}

int arm_task_calls_only(const struct arm_task *t)
{
    return t->calls_only;
}

uint32_t arm_task_last_error(const struct arm_task *t, const char **text)
{
    *text = t->last_text;
    return t->last_error;
}

void arm_task_destroy(struct arm_task *t)
{
    pthread_mutex_lock(&tasks_mu);
    for (struct arm_task **p = &tasks; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    pthread_mutex_unlock(&tasks_mu);
    for (unsigned i = 0; i < DEPTH; i++)
        if (t->jit[i])
            armrun_destroy(t->jit[i]);
    pthread_mutex_destroy(&t->mu);
    free(t);
}

uint32_t arm_task_run(struct arm_task *t, const struct ros_cpu *s)
{
    struct regs x;
    from_cpu(s, &x);
    x.mode = s->mode ? s->mode : ROS_MODE_USR;
    x.r[14] = MARK_RETURN;                          /* its return exits (ReturnFromAbsoluteCode) */
    t->ended = 0;
    put_regs(t, 0, &x);
    loop(t, 0);
    return t->rc;
}

uint32_t arm_task_call(struct arm_task *t, uint32_t addr, struct ros_cpu *s)
{
    unsigned outer = t->depth, d = outer + 1;
    if (d >= DEPTH) {
        t->last_error = ERR_UNDEF;
        snprintf(t->last_text, sizeof t->last_text, "ARM code called %u deep: too deep", d);
        return ERR_UNDEF;
    }
    engine(t, d);
    t->counts.calls++;
    atomic_fetch_add_explicit(&at_calls, 1, memory_order_relaxed);
    if (d > t->counts.max_depth)
        t->counts.max_depth = d;
    uint32_t mode = t->mode;
    struct regs x;
    from_cpu(s, &x);
    x.r[14] = MARK_RETURN;
    x.r[15] = addr;
    x.mode = s->mode ? s->mode : mode;            /* a module's entry: SVC mode */
    if (x.mode != ROS_MODE_USR && mode == ROS_MODE_USR && t->jit[outer]) {
        struct regs o;                              /* the user bank: the caller's R13, R14 */
        get_regs(t, outer, &o);
        t->bank13[BANK_USR] = o.r[13], t->bank14[BANK_USR] = o.r[14];
    }
    t->returned[d] = 0;
    t->call_error[d] = 0;
    if (outer == 0)
        t->last_error = 0, t->last_text[0] = 0;
    put_regs(t, d, &x);
    loop(t, d);
    struct regs back;
    get_regs(t, d, &back);
    to_cpu(&back, s);
    t->depth = outer;
    t->mode = mode;
    return t->call_error[d];
}

void arm_task_add_callback(struct arm_task *t, uint32_t code, uint32_t r12)
{
    pthread_mutex_lock(&t->mu);
    if (t->ncb < CALLBACKS) {
        t->cb[t->ncb].code = code;
        t->cb[t->ncb].r12 = r12;
        t->ncb++;
    }
    pthread_mutex_unlock(&t->mu);
    armrun_halt(t->jit[0]);
}

void arm_task_resume_from_block(struct arm_task *t, uint32_t block)
{
    t->resume = 1;
    t->resume_block = block;
}

void arm_task_synchronise(struct arm_task *t, uint32_t addr, uint32_t len)
{
    for (unsigned i = 0; i < DEPTH; i++) {
        if (!t->jit[i])
            continue;
        if (len)
            armrun_invalidate(t->jit[i], addr, len);
        else
            armrun_clear(t->jit[i]);
    }
}

void arm_task_synchronise_all(uint32_t addr, uint32_t len)
{
    pthread_mutex_lock(&tasks_mu);
    for (struct arm_task *t = tasks; t; t = t->next)
        arm_task_synchronise(t, addr, len);
    pthread_mutex_unlock(&tasks_mu);
}

const struct arm_task_counts *arm_task_counts(const struct arm_task *t)
{
    return &t->counts;
}
