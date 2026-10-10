/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* box.c: the ARM container in the box. It does *Run of ARM code.
 *
 * An &FF8 file that is not a C application's image (capp.c) is ARM code: an
 * AIF image or an Absolute. It is loaded and started as FileSwitch starts
 * one. The memory is asked for (ros_capp_memory_for), then StartApplication
 * (OS_FSControl 2) is called and the file is loaded at &8000. It is run as
 * the task's application (ros_module_run_as_application), on the task's own
 * thread, by the bridge (bridge.h) over the engine (armrun.h). The native
 * Wimp made that thread (Wimp_StartTask, child_main) and is not changed.
 *
 * The bridge's services are the runtime's here. Its SWIs are ros_swi's. The
 * environment's handlers are what OS_ChangeEnvironment gives. Those in the
 * application's memory are ARM code that the bridge enters. Any other is one
 * of the kernel's defaults, which are native, and is left to the runtime.
 * When the bridge ends the task for want of an ARM handler, the engine is
 * gone before the runtime's OS_Exit or OS_GenerateError runs. The runtime's
 * unwinding (a longjmp back to the command that ran the program) therefore
 * never crosses the engine's frames.
 *
 * The runtime makes calls into the task's code: a transient callback, a
 * vector claimant, a handler (ros_call). They reach ros_armrun_call, which
 * runs them on a nested engine while the task is inside a SWI. The other way
 * round (#146), ARM code can branch to native code, such as a callback that
 * a native program handed it. That leaves the engine, and box_gate calls the
 * code through ros_call. Either may call the other again, to the bridge's
 * depth.
 *
 * The starting state is RISC OS 5.30's for an Absolute (capp.h,
 * tests/capps/farm). It is USR mode, R12 = R13 = &80000000, R14 the return
 * that exits, and the entry &8000. An AIF's header code is its own. Its
 * decompression, relocation and zero-initialisation run as ARM code like the
 * rest.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/fault.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "armrun.h"
#include "rosgd/armbox.h"
#include "bridge.h"
#include "armrun_modules.h"

/* The ARM task this thread is running, if any (one a thread: an ARM
 * application that *Runs another runs it nested, the outer one restored) */
static _Thread_local struct arm_task *running;

/* ARM code outside any application: modules loaded from files, and
 * utilities, in the RMA. The runtime calls into them from any task's thread,
 * ARM or not. The calls are a module's entries, its SWIs, and a vector that
 * it claimed. A thread with no ARM application runs them on a task of its
 * own for calls only (system_task). There an error ends the call and comes
 * back to the native caller as an error. */
#define CODE_RANGES 64
static struct { uint32_t lo, hi; } ranges[CODE_RANGES];
static pthread_mutex_t ranges_mu = PTHREAD_MUTEX_INITIALIZER;

static int in_ranges(uint32_t addr)
{
    for (unsigned i = 0; i < CODE_RANGES; i++)
        if (addr - ranges[i].lo < ranges[i].hi - ranges[i].lo)
            return 1;
    return 0;
}

/* SharedCLibrary for ARM clients. This is RISC OS 5.30's own module,
 * SharedCLibrary 6.23 out of the 5.30 ROM (runtime/armrun/arm/README.md),
 * run under the engine. ARM clients call the library in APCS-32, with
 * doubles in integer registers, printf's variadics, and function pointers
 * into their own code. The native library is a C library of the native ABI.
 * It cannot take those calls without a marshalling layer for every entry.
 * 5.30's library takes them as they are. There is one instance in the RMA,
 * shared by every ARM task as RISC OS shares it. It is the native
 * SharedCLibrary's built-in ARM shadow (#147). It is made at the first ARM
 * reference, which is an ARM SWI in its chunk or an ARM OS_Module lookup.
 * Like any shadow, only ARM callers reach it (runtime/module.c,
 * ros_module_route). An x32 client's calls stay with the native module.
 * Native fast paths for simple entries may come later, through the
 * gateways. */
#define SHAREDCLIB_ROM 0xFC182320u  /* where 5.30's ROM holds it: its link address */

struct run {
    struct arm_task *task;
    uint32_t limit;                 /* the memory limit when it started: its code is below */
    uint32_t rc, errnum;
    char text[252];
    int ended_with_error;
};
static _Thread_local struct run *current;

/* ---- the emulated SharedCLibrary ---------------------------------------------------- */

/* Makes the built-in shadow. The module is copied into the RMA, its absolute
 * address constants are moved, and its code is registered as ARM code. It is
 * then initialised as any module is (module.c: R10 the environment, R11 0,
 * R12 the private word), through ros_call. That runs on the calling ARM
 * task's nested engine. When no ARM application runs on the thread, it runs
 * on the thread's task for calls only (this happens for an ARM C module's
 * library calls from a !Run's RMLoad) */
struct ros_module *ros_armrun_builtin_clib(struct ros_module *twin)
{
    static int making, failed;
    if (making || failed)
        return NULL;
    making = 1;
    struct ros_module *made = NULL;
    uint8_t *mem = ros_rma_alloc(ARMRUN_SHAREDCLIB_SIZE + 16);
    if (mem) {
        uint32_t base = (ros_addr(mem) + 15) & ~15u;
        memcpy(ros_ptr(base), armrun_sharedclib, ARMRUN_SHAREDCLIB_SIZE);
        /* The ROM build is linked where the ROM holds it, with its address
         * constants absolute (Lib$$Init$$Base and the rest). Each word that
         * points into [its ROM address, its end] is moved by as much as the
         * module has been moved. As an instruction, such a word would be an
         * unconditional coprocessor one, which this code never holds. The
         * module's other words above &FC000000 are floating point constants
         * and zero page. */
        uint32_t delta = base - SHAREDCLIB_ROM;
        for (uint32_t off = 0; off + 4 <= ARMRUN_SHAREDCLIB_SIZE; off += 4) {
            uint32_t w = ros_ld32(base + off);
            if (w - SHAREDCLIB_ROM <= ARMRUN_SHAREDCLIB_SIZE)
                ros_st32(base + off, w + delta);
        }
        if (ros_armrun_code_add(base, base + ARMRUN_SHAREDCLIB_SIZE)) {
            os_error *e = ros_module_make_shadow(twin, base, ARMRUN_SHAREDCLIB_SIZE, mem, &made);
            if (e) {
                ros_console_printf("rosgd: SharedCLibrary for ARM code: %s\n", e->errmess);
                ros_armrun_code_remove(base);
                made = NULL;
            }
        }
        if (!made)
            ros_rma_free(mem);
    }
    failed = !made;
    making = 0;
    return made;
}

static int is_arm_code(uint32_t addr);

/* Whether addr is native code (#146). Native code is compiled code (the
 * dispatcher's), a native entry, a C program's or module's code, or native
 * code that BASIC assembled (capp.h). It is not ARM code. An address in an
 * ARM range or in the running ARM application's memory is ARM code, and that
 * is tested first. */
static int box_native(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (is_arm_code(addr))
        return 0;
    if (addr >= ROS_NATIVE_BASE && addr < ROS_ZEROPAGE)
        return 1;
    return ros_code_lookup(addr) != NULL || ros_capp_code(addr) != 0;
}

/* ARM code's call to native code. It is made as the dispatcher makes calls
 * (ros_call), with the ARM registers. R14 is the ARM code's return and the
 * mode is the ARM code's. Privileged ARM code's frames on the SVC stack are
 * kept below, as for its SWIs. An error that the native code raises (a non-X
 * SWI's, a ros_raise) stops here, because the engine's frames are never
 * unwound past. The bridge delivers it as a non-X SWI's error, to the ARM
 * task's handler or (for calls only) back to the native caller of the ARM
 * code. */
static int box_gate(void *ctx, struct arm_task *t, uint32_t addr, struct ros_cpu *s)
{
    (void)ctx, (void)t;
    if (addr >= ARM_GATE_LO && addr < ARM_GATE_HI)
        return 0;                   /* native fast paths: none yet */
    uint32_t outer = ros_svc_sp;
    if (s->mode != ROS_MODE_USR && s->r[13] < outer && outer - s->r[13] < 0x100000u)
        ros_svc_sp = (s->r[13] - 16) & ~7u;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, s->r, sizeof c.r);
    c.n = s->n, c.z = s->z, c.c = s->c, c.v = s->v, c.q = s->q;
    c.irq_off = s->irq_off;
    c.mode = s->mode;
    struct ros_handler h;
    if (ROS_TRY(&h)) {
        ros_call(&c, addr);
        ros_handler_pop(&h);
    } else {
        ros_svc_sp = outer;
        s->r[0] = ros_addr(h.error);
        s->v = 1;
        return ARM_GATE_RAISED;
    }
    ros_svc_sp = outer;
    memcpy(s->r, c.r, sizeof s->r);
    s->n = c.n, s->z = c.z, s->c = c.c, s->v = c.v, s->q = c.q;
    return 1;
}

/* ---- the bridge's services ------------------------------------------------------- */

/* ARM code's SWI. It is an ARM caller's, so the module it reaches is a
 * shadow where the title has one. For SharedCLibrary's chunk that is 5.30's
 * module. OS_Module's lookups find shadows too. */
static void box_swi(void *ctx, struct arm_task *t, struct ros_cpu *s, uint32_t number)
{
    (void)ctx, (void)t;
    /* Privileged ARM code, such as a module's on the SVC stack, has its
     * frames below the runtime's SVC sp. What the SWI calls goes on below
     * them */
    uint32_t outer = ros_svc_sp;
    if (s->mode != ROS_MODE_USR && s->r[13] < outer && outer - s->r[13] < 0x100000u)
        ros_svc_sp = (s->r[13] - 16) & ~7u;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, s->r, sizeof c.r);
    c.n = s->n, c.z = s->z, c.c = s->c, c.v = 0, c.q = s->q;
    c.mode = ROS_MODE_USR;
    ros_swi_as(&c, number, ROS_KIND_ARM);
    ros_svc_sp = outer;
    memcpy(s->r, c.r, sizeof s->r);
    s->n = c.n, s->z = c.z, s->c = c.c, s->v = c.v, s->q = c.q;
}

/* ARM code is the application's (an address in its memory) or an emulated
 * module's (the built-in SharedCLibrary among them) */
static int is_arm_code(uint32_t addr)
{
    if (in_ranges(addr))
        return 1;
    return current && addr >= ROS_APP_BASE && addr < current->limit;
}

static int box_handler(void *ctx, unsigned which, uint32_t *code, uint32_t *r12, uint32_t *buf)
{
    (void)ctx;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = which, c.r[1] = c.r[2] = c.r[3] = 0;
    ros_swi(&c, XOS_ChangeEnvironment);
    if (c.v || !is_arm_code(c.r[1]))
        return 0;                   /* a native handler: the runtime's, after the engine */
    *code = c.r[1], *r12 = c.r[2], *buf = c.r[3];
    return 1;
}

static int box_valid(void *ctx, uint32_t addr, uint32_t size, int write)
{
    (void)ctx, (void)write;
    return addr + size >= addr && ros_arena_valid(addr, addr + size);
}

static void box_ended(void *ctx, struct arm_task *t, uint32_t rc, uint32_t errnum, const char *text)
{
    (void)ctx, (void)t;
    if (!current)
        return;
    current->rc = rc;
    current->errnum = errnum;
    current->ended_with_error = text != NULL;
    snprintf(current->text, sizeof current->text, "%s", text ? text : "");
}

/* A safe point in ARM code. The background work that is queued, such as a
 * key or a ticker event, runs here, as an interrupt would. ARM code in a
 * privileged mode has live frames on the SVC stack below ros_svc_sp when it
 * was entered there without a SWI to lower it. This happens for a module's
 * SWI handler called for an application's SWI from USR mode (SharedCLibrary's
 * LibInit, which starts at the very top), and for any other entry from native
 * code. The work then starts below the ARM code's R13, as an IRQ's handlers
 * do below the interrupted SVC code's frames. At ros_svc_sp it went over
 * them. That was #156: OvationPro's start now and then failed with an abort
 * in LibInit, because the work had written over the R1 that LibInit saves
 * there and then uses as a pointer. */
static void box_poll(void *ctx, struct arm_task *t)
{
    (void)ctx;
    if (ros_work_pending) {
        uint32_t mode, sp = arm_task_sp(t, &mode);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        ros_safe_point(&c, mode != ROS_MODE_USR ? sp : 0);
    }
}

/* ---- a fault's report -------------------------------------------------------------
 *
 * This goes to the serial console, before the bridge raises the fault as the
 * error. It says where the fault was (the module whose image holds the PC,
 * and its offset; the application; or other ARM code), the access that was
 * refused, and the registers. The error "Internal error: abort on data
 * transfer at &..." alone names only the PC (#156). */

static void where_arm(uint32_t addr, char *out, size_t n)
{
    struct ros_module *m = ros_module_arm_at(addr);
    if (m)
        snprintf(out, n, "%s+&%X", m->title ? m->title : "?", addr - m->base);
    else if (addr >= ROS_APP_BASE && addr < ROS_APP_LIMIT)
        snprintf(out, n, "the application");
    else if (in_ranges(addr))
        snprintf(out, n, "ARM code in the RMA");
    else
        snprintf(out, n, "no ARM code");
}

static const char *mode_word(uint32_t mode)
{
    switch (mode & 0x1F) {
    case 0x10: return "USR";
    case 0x11: return "FIQ";
    case 0x12: return "IRQ";
    case 0x13: return "SVC";
    case 0x17: return "ABT";
    case 0x1B: return "UND";
    case 0x1F: return "SYS";
    default: return "?";
    }
}

static void box_fault(void *ctx, struct arm_task *t, const char *what, const uint32_t r[16],
                      uint32_t cpsr, uint32_t mode, unsigned depth, uint32_t addr, unsigned size,
                      int write)
{
    (void)ctx;
    char pc_in[96], r14_in[96], access[96] = "";
    where_arm(r[15], pc_in, sizeof pc_in);
    where_arm(r[14] & ~3u, r14_in, sizeof r14_in);
    if (size)
        snprintf(access, sizeof access, "; %s %u byte%s at &%08X", write ? "writing" : "reading",
                 size, size == 1 ? "" : "s", addr);
    ros_console_printf("armrun: %s at &%08X (%s)%s\n", what, r[15], pc_in, access);
    ros_console_printf("armrun:   %s, %s mode, depth %u, task &%X; R14 in %s\n",
                       arm_task_calls_only(t) ? "calls only (module code)" : "an application",
                       mode_word(mode), depth, ros_ld32(ROS_ZP_DOMAINID), r14_in);
    for (unsigned row = 0; row < 4; row++)
        ros_console_printf("armrun:   R%-2u &%08X  R%-2u &%08X  R%-2u &%08X  R%-2u &%08X\n",
                           row * 4, r[row * 4], row * 4 + 1, r[row * 4 + 1], row * 4 + 2,
                           r[row * 4 + 2], row * 4 + 3, r[row * 4 + 3]);
    ros_console_printf("armrun:   CPSR &%08X\n", cpsr);
    /* the SVC stack above R13, where the frames of the code that faulted
     * are: what overwrote them, if anything did (#156) */
    if (r[13] - ROS_SVCSTACK_BASE < ROS_SVCSTACK_SIZE) {
        uint32_t top = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;
        ros_console_printf("armrun:   the SVC stack (the runtime's top &%08X):\n", ros_svc_sp);
        for (uint32_t a = r[13] & ~15u; a < r[13] + 0x40 && a < top; a += 16)
            ros_console_printf("armrun:   %08X: %08X %08X %08X %08X\n", a, ros_ld32(a),
                               a + 4 < top ? ros_ld32(a + 4) : 0, a + 8 < top ? ros_ld32(a + 8) : 0,
                               a + 12 < top ? ros_ld32(a + 12) : 0);
    }
}

/* ---- faults in translated code ------------------------------------------------- */

static int jit_fault(int sig, siginfo_t *si, void *context)
{
    return armrun_handle_fault(sig, si, context);
}

/* ---- running it ------------------------------------------------------------------ */

#ifdef __linux__
#include <sched.h>
#endif

/* rosgd.armcpu=N is an experiment. It puts an ARM application's thread on
 * CPU N while it runs, rather than on the core that every task shares
 * (task.c). The thread goes back to that core afterwards. The baton is
 * unchanged, so this affects only the caches. */
static int arm_cpu(void)
{
    static int cpu = -2;
    if (cpu == -2) {
        const char *v = ros_cmdline_value("rosgd.armcpu");
        cpu = v ? atoi(v) : -1;
    }
    return cpu;
}

static void halt_option(void)
{
    if (ros_cmdline_has("rosgd.armnohalt"))         /* for measuring: inexact aborts */
        armrun_halt_on_access = 0;
}

static void run_arm(void *arg)
{
    struct run *r = arg, *outer_run = current;
    struct arm_task *outer = running;
#ifdef __linux__
    cpu_set_t kept;
    int moved = 0;
    if (arm_cpu() >= 0 && pthread_getaffinity_np(pthread_self(), sizeof kept, &kept) == 0) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(arm_cpu(), &one);
        moved = pthread_setaffinity_np(pthread_self(), sizeof one, &one) == 0;
    }
#endif
    const struct arm_ops ops = { NULL, box_swi, box_handler, box_valid, box_ended,
                                 ros_svc_sp, ros_fp_current, box_gate, box_poll, box_native,
                                 box_fault };
    ros_fault_jit = jit_fault;
    halt_option();
    current = r;
    r->task = arm_task_create(ros_arena_base, &ops);
    running = r->task;
    struct ros_cpu s;
    memset(&s, 0, sizeof s);
    s.r[12] = s.r[13] = 0x80000000u;
    s.r[15] = ROS_APP_BASE;
    s.mode = ROS_MODE_USR;
    arm_task_run(r->task, &s);
    arm_task_destroy(r->task);
#ifdef __linux__
    if (moved)
        pthread_setaffinity_np(pthread_self(), sizeof kept, &kept);
#endif
    running = outer;
    current = outer_run;
    /* Its end, as the runtime makes it. The engine is gone, so the runtime
     * may unwind. OS_Exit's and an error's default handlers end the
     * application by returning to the command that ran it */
    struct ros_cpu c;
    ros_cpu_enter(&c);
    if (r->ended_with_error) {
        uint32_t *block = ros_rma_alloc(4 + (uint32_t)strlen(r->text) + 1);
        if (block) {
            block[0] = r->errnum;
            strcpy((char *)(block + 1), r->text);
            c.r[0] = ros_addr(block);
            ros_swi(&c, OS_GenerateError);
        }
        return;
    }
    c.r[0] = 0, c.r[1] = 0x58454241u, c.r[2] = r->rc;       /* "ABEX" */
    ros_swi(&c, OS_Exit);
}

/* The file's first words, read without loading it */
static os_error *read_head(const char *path, uint32_t *head, uint32_t n, uint32_t *len)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 5, c.r[1] = ros_addr(path);
    ros_swi(&c, XOS_File);
    if (c.v)
        return ros_ptr(c.r[0]);
    *len = c.r[4];
    ros_cpu_enter(&c);
    c.r[0] = 0x4F, c.r[1] = ros_addr(path);                 /* OS_Find: open to read, no path */
    ros_swi(&c, XOS_Find);
    if (c.v)
        return ros_ptr(c.r[0]);
    uint32_t h = c.r[0];
    uint32_t *buf = ros_rma_alloc(4 * n);
    if (!buf) {                                             /* the RMA full: the file closed again */
        ros_cpu_enter(&c);
        c.r[0] = 0, c.r[1] = h;
        ros_swi(&c, XOS_Find);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    memset(buf, 0, 4 * n);
    ros_cpu_enter(&c);
    c.r[0] = 3, c.r[1] = h, c.r[2] = ros_addr(buf), c.r[3] = 4 * n, c.r[4] = 0;
    ros_swi(&c, XOS_GBPB);
    memcpy(head, buf, 4 * n);
    ros_rma_free(buf);
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = h;
    ros_swi(&c, XOS_Find);
    return NULL;
}

os_error *ros_armrun_run(const char *path, uint32_t line)
{
    if (strlen(path) > 1000)
        return ros_error(ROS_ERR_UNIMPLEMENTED, "'%s': the name is too long", path);
    /* the path where SWIs can read it */
    uint32_t pn = (uint32_t)strlen(path) + 1;
    char *p = ros_rma_alloc(pn);
    if (!p)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(p, path, pn);
    uint32_t head[32], len = 0;
    os_error *e = read_head(p, head, 32, &len);
    if (e) {
        ros_rma_free(p);
        return e;
    }
    /* The memory it needs: an AIF says (RO + RW + ZI, and a squeezed
     * image's expansion is inside its RO and RW); an Absolute its length */
    uint32_t need = len;
    int aif = head[4] == 0xEF000011u;
    if (aif) {
        uint32_t mode = head[12] & 0xFF;
        if (mode == 26 || mode == 0) {
            ros_rma_free(p);
            return ros_error(ROS_ERR_UNIMPLEMENTED, "'%s' is 26-bit ARM code, which ROSGD does not run",
                             path);
        }
        uint32_t img = head[5] + head[6] + head[8];
        if (img > need)
            need = img;
    }
    need = (need + 0x10000u + 0xFFFu) & ~0xFFFu;            /* and room above it, as the AIF header code wants */
    if ((e = ros_capp_memory_for(ROS_APP_BASE + need))) {
        ros_rma_free(p);
        return e;
    }
    /* StartApplication: the command line as *Run had it, the CAO &8000 */
    uint32_t n = 0;
    while (ros_ld8(line + n) >= ' ' && n < 1000)
        n++;
    char *cmd = ros_rma_alloc(n + 1), *none = ros_rma_alloc(1);
    if (!cmd || !none) {
        if (cmd)
            ros_rma_free(cmd);
        if (none)
            ros_rma_free(none);
        ros_rma_free(p);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    memcpy(cmd, ros_ptr(line), n);
    cmd[n] = 0, none[0] = 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 2, c.r[1] = ros_addr(none), c.r[2] = ROS_APP_BASE, c.r[3] = ros_addr(cmd);
    ros_swi(&c, XOS_FSControl);
    ros_rma_free(cmd);
    ros_rma_free(none);
    if (c.v) {
        ros_rma_free(p);
        return ros_ptr(c.r[0]);
    }
    /* The file at &8000 */
    ros_cpu_enter(&c);
    c.r[0] = 255, c.r[1] = ros_addr(p), c.r[2] = ROS_APP_BASE, c.r[3] = 0;
    ros_swi(&c, XOS_File);
    ros_rma_free(p);
    if (c.v)
        return ros_ptr(c.r[0]);
    /* Its limit: what OS_GetEnv now gives, the top of its memory */
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_GetEnv);
    struct run r;
    memset(&r, 0, sizeof r);
    r.limit = c.r[1];
    ros_module_run_as_application(run_arm, &r);
    return NULL;
}

/* The thread's task for calls only: made at its first call, gone with
 * the thread */
static pthread_key_t system_key;
static pthread_once_t system_once = PTHREAD_ONCE_INIT;
static _Thread_local struct arm_task *calls_task;
static _Thread_local uint32_t *system_error;    /* the block a failed call returns */

static void system_gone(void *t)
{
    arm_task_destroy(t);
}

static void system_key_make(void)
{
    pthread_key_create(&system_key, system_gone);
}

static struct arm_task *system_task(void)
{
    if (calls_task)
        return calls_task;
    pthread_once(&system_once, system_key_make);
    const struct arm_ops ops = { NULL, box_swi, NULL, box_valid, NULL,
                                 ros_svc_sp, ros_fp_current, box_gate, NULL, box_native,
                                 box_fault };
    ros_fault_jit = jit_fault;
    calls_task = arm_task_create_calls(ros_arena_base, &ops);
    system_error = ros_rma_alloc(256);
    if (calls_task)
        pthread_setspecific(system_key, calls_task);
    return calls_task;
}

int ros_armrun_call(struct ros_cpu *s, uint32_t addr)
{
    if (!is_arm_code(addr))
        return 0;
    struct arm_task *t = running ? running : system_task();
    if (!t)
        return 0;
    uint32_t back = s->r[14];
    struct ros_cpu x = *s;
    /* an application's error went to its handler; for calls only, it comes back */
    uint32_t err = arm_task_call(t, addr, &x);
    memcpy(s->r, x.r, 14 * sizeof s->r[0]);
    s->n = x.n, s->z = x.z, s->c = x.c, s->v = x.v, s->q = x.q;
    if (err && t == calls_task && system_error) {
        const char *text;
        system_error[0] = arm_task_last_error(t, &text);
        snprintf((char *)(system_error + 1), 252, "%s", text);
        s->r[0] = ros_addr(system_error);
        s->v = 1;
    }
    s->r[14] = back;
    s->r[15] = back;                        /* as a call returns: the caller goes on */
    return 1;
}

/* ---- ARM code in the RMA: modules and utilities ------------------------------------ */

int ros_armrun_is_code(uint32_t addr)
{
    return is_arm_code(addr);
}

int ros_armrun_code_add(uint32_t lo, uint32_t hi)
{
    int ok = 0;
    pthread_mutex_lock(&ranges_mu);
    for (unsigned i = 0; i < CODE_RANGES && !ok; i++)     /* the same again: BASIC's passes */
        if (ranges[i].lo == lo && ranges[i].hi == hi)
            ok = 1;
    for (unsigned i = 0; i < CODE_RANGES && !ok; i++)
        if (ranges[i].lo == ranges[i].hi) {
            ranges[i].lo = lo, ranges[i].hi = hi;
            ok = 1;
        }
    pthread_mutex_unlock(&ranges_mu);
    arm_task_synchronise_all(lo, hi - lo);  /* what was here before, translated, goes */
    return ok;
}

void ros_armrun_code_remove(uint32_t lo)
{
    uint32_t hi = 0;
    pthread_mutex_lock(&ranges_mu);
    for (unsigned i = 0; i < CODE_RANGES; i++)
        if (ranges[i].lo == lo && ranges[i].hi != lo) {
            hi = ranges[i].hi;
            ranges[i].lo = ranges[i].hi = 0;
            break;
        }
    pthread_mutex_unlock(&ranges_mu);
    if (hi)
        arm_task_synchronise_all(lo, hi - lo);
}

void ros_armrun_code_forget(uint32_t lo, uint32_t hi)
{
    int any = 0;
    pthread_mutex_lock(&ranges_mu);
    for (unsigned i = 0; i < CODE_RANGES; i++)
        if (ranges[i].lo != ranges[i].hi && ranges[i].lo < hi && lo < ranges[i].hi) {
            ranges[i].lo = ranges[i].hi = 0;
            any = 1;
        }
    pthread_mutex_unlock(&ranges_mu);
    if (any)
        arm_task_synchronise_all(lo, hi - lo);
}

void ros_armrun_code_native(uint32_t lo, uint32_t hi)
{
    if (hi > lo)
        arm_task_synchronise_all(lo, hi - lo);
}

/* The built-in SharedCLibrary's address, made if it is not yet: what a
 * 5.30 ROM C module's calls into the library are moved to (runtime/
 * module.c, tools/romwrap.py).  0 if it cannot be made. */
uint32_t ros_armrun_clib(void)
{
    struct ros_module *m = ros_module_route(ros_module_for_swi(SharedCLibrary_LibInitAPCS_A), ROS_KIND_ARM);
    return m && m->arm ? m->base : 0;
}

/* ---- what it costs: *ARMStats ------------------------------------------------ */

/* Says where an address is. That is an ARM module, the emulated
 * SharedCLibrary or another shadow among them (by its title), the
 * application, or else the address itself. */
static void where(uint32_t a, char *buf, size_t n)
{
    uint32_t lo = 0;
    for (unsigned i = 0; i < CODE_RANGES; i++)          /* the range it is in, then whose */
        if (a - ranges[i].lo < ranges[i].hi - ranges[i].lo)
            lo = ranges[i].lo;
    for (struct ros_module *c = ros_module_first(); lo && c; c = c->next)
        for (struct ros_module *m = c; m; m = m == c ? c->shadow : NULL)
            if (m->loaded && m->base == lo) {
                snprintf(buf, n, "%s+&%X", m->title, a - lo);
                return;
            }
    if (a >= ROS_APP_BASE && a < 0x60000000u) {
        snprintf(buf, n, "application &%X", a);
        return;
    }
    snprintf(buf, n, "&%08X", a);
}

void ros_armrun_view(struct ros_arm_view *v)
{
    struct arm_stats st;
    arm_stats_read(&st);
    memset(v, 0, sizeof *v);
    v->ns_engine = st.ns_engine, v->ns_native = st.ns_native, v->ns_fpa = st.ns_fpa;
    v->instructions = st.ticks, v->swis = st.swis, v->calls = st.calls, v->samples = st.samples;
    arm_tasks_count(&v->tasks, &v->call_tasks);
    for (struct ros_module *c = ros_module_first(); c; c = c->next)
        for (struct ros_module *m = c; m; m = m == c ? c->shadow : NULL)
            v->modules += m->loaded && m->arm;
    uint32_t addr[3], count[3];
    v->hot = st.samples ? arm_stats_samples(addr, count, 3) : 0;
    for (unsigned i = 0; i < v->hot; i++) {
        v->hot_count[i] = count[i];
        where(addr[i], v->hot_name[i], sizeof v->hot_name[i]);
    }
}

void ros_armrun_stats(int reset, int (*emit)(const char *line, void *ctx), void *ctx)
{
    if (reset) {
        arm_stats_reset();
        return;
    }
    struct arm_stats st;
    arm_stats_read(&st);
    char line[160];
    double eng = st.ns_engine / 1e6, nat = st.ns_native / 1e6, fpa = st.ns_fpa / 1e6;
    double all = eng + nat + fpa;
    if (st.ns_wall) {
        snprintf(line, sizeof line, "Since *ARMStats -reset: %.1f ms", st.ns_wall / 1e6);
        if (emit(line, ctx))
            return;
    }
    snprintf(line, sizeof line, "ARM code: %.1f ms in the engine, %.1f ms in native code it called, "
             "%.1f ms in the FPA", eng, nat, fpa);
    if (emit(line, ctx))
        return;
    snprintf(line, sizeof line, "  %.0f%% emulated; %llu instructions, %.1f million a second in the engine",
             all > 0 ? 100.0 * eng / all : 0.0, (unsigned long long)st.ticks,
             eng > 0 ? st.ticks / (eng * 1000.0) : 0.0);
    if (emit(line, ctx))
        return;
    snprintf(line, sizeof line, "  %llu SWIs, %llu calls into ARM code; %llu PC samples",
             (unsigned long long)st.swis, (unsigned long long)st.calls,
             (unsigned long long)st.samples);
    if (emit(line, ctx) || !st.samples)
        return;
    uint32_t addr[24], count[24];
    unsigned n = arm_stats_samples(addr, count, 24);
    if (emit("  where (16-byte ranges, the most sampled first):", ctx))
        return;
    for (unsigned k = 0; k < n; k++) {
        char w[80];
        where(addr[k], w, sizeof w);
        snprintf(line, sizeof line, "  %5.1f%%  %s", 100.0 * count[k] / (double)st.samples, w);
        if (emit(line, ctx))
            return;
    }
}

/* A utility (&FFC), as FileSwitch runs one (Run_TransientFile). It is loaded
 * into an RMA block after a save area, with 1K of workspace above it. It is
 * entered in USR mode with R0 the command line, R1 the tail, R12 the
 * workspace and R13 its top. It returns through R14. An error is returned
 * with V set and R0 the block. */
os_error *ros_armrun_utility(const char *path, uint32_t line, uint32_t tail)
{
    uint32_t pn = (uint32_t)strlen(path) + 1;
    char *p = ros_rma_alloc(pn);
    if (!p)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(p, path, pn);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 5, c.r[1] = ros_addr(p);
    ros_swi(&c, XOS_File);
    if (c.v) {
        ros_rma_free(p);
        return ros_ptr(c.r[0]);
    }
    uint32_t len = (c.r[4] + 3) & ~3u, ws = 0x400;
    uint8_t *block = ros_rma_alloc(24 + len + ws);
    if (!block) {
        ros_rma_free(p);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    uint32_t code = ros_addr(block) + 24;
    ros_cpu_enter(&c);
    c.r[0] = 255, c.r[1] = ros_addr(p), c.r[2] = code, c.r[3] = 0;
    ros_swi(&c, XOS_File);
    ros_rma_free(p);
    if (c.v) {
        ros_rma_free(block);
        return ros_ptr(c.r[0]);
    }
    if (!ros_armrun_code_add(code, code + len)) {
        ros_rma_free(block);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for more ARM code");
    }
    ros_cpu_enter(&c);
    c.r[0] = line, c.r[1] = tail;
    c.r[12] = code + len;
    c.r[13] = code + len + ws;
    c.mode = ROS_MODE_USR;
    ros_call(&c, code);
    ros_armrun_code_remove(code);
    ros_rma_free(block);
    if (c.v) {
        /* the block may be the utility's own, which is gone: copied */
        static _Thread_local uint32_t copy[64];
        memcpy(copy, ros_ptr(c.r[0]), sizeof copy);
        copy[63] = 0;
        return ros_error(copy[0], "%s", (const char *)(copy + 1));
    }
    return NULL;
}

int ros_armrun_active(void)
{
    return running != NULL;
}
