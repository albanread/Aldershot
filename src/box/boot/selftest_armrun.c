/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_armrun.c: the ARM container's engine inside /init
 * (runtime/armrun/SPRINTS.md).  It tests dynarmic behind armrun.h, with the
 * runtime around it.
 *
 *   - A block of A32 code in the RMA runs, and its SVC reaches the real SWI
 *     dispatcher (ros_swi) with the registers the code left.  OS_ConvertHex8
 *     writes its digits into guest memory, which the code then reads.
 *   - In the box, a guest load from the SVC stack's guard page faults in
 *     translated code.  The runtime's handler offers the fault to the engine
 *     first (ros_fault_jit, fault.h), and the engine takes it to the read
 *     hook.  A fault of the runtime's own, with the engine there, still
 *     reaches the runtime's handler (a Worker-style escape catches it).
 *   - Four chained blocks are rewritten and one invalidation covers them.
 *     All four are retranslated (runtime/armrun/patches/0002).
 *
 * tests/armrun/smoke.c is the same engine on the Mac, without the runtime.
 *
 * It also tests *Run of ARM code.  tests/armrun/boxhello.s is an ARM
 * Absolute assembled by rosasm, staged as the disc ArmTest and run through
 * FileSwitch, the loader (runtime/armrun/box.c), the bridge and the engine,
 * with the box's own SWIs and environment.  The checks cover its command
 * line, OS_Exit's return code, an error to the default handler and to its
 * own ARM handler, and a transient callback the runtime calls into its code.
 *
 * And it tests ARM code calling native code (#146).  tests/armrun/nest.s is
 * ARM code in the RMA.  It calls a native callback (a native entry) that
 * calls it again through ros_call, so the calls go native -> ARM -> native ->
 * ARM and so on, four deep each way.  The checks are that the values are
 * right at each level, that the registers are kept across each call, and
 * that an error raised at the deepest native level comes back to the top as
 * an error.
 */
#include <dirent.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/fault.h"
#include "rosgd/rma.h"
#include "rosgd/api.h"
#include "rosgd/armbox.h"
#include "rosgd/error.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "armrun.h"
#include "armrun_tests.h"
#include "fileswitch.h"
#include "selftest.h"

/* The scratch directory and its files gone, unmounted first (a run left one
 * in TMPDIR each time) */
static void scratch_gone(const char *name, const char *dir)
{
    ros_hostfs_unmount(name);
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[700];
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    if (d)
        closedir(d);
    rmdir(dir);
}

#define check ros_check
#define WRCHV 0x03u                     /* the write-character vector */

#if defined(__linux__) && !defined(ROS_ARENA_HOSTED)
#define BOX 1
#else
#define BOX 0
#endif

#define EXIT_SWI 0x11u                  /* OS_Exit: the code's end */
#define MARKER   0x5AFE5AFEu            /* what the read hook gives for the guard page */

static uint32_t mem_lo, mem_hi;         /* the RMA block the code and data are in */

static struct {
    unsigned svcs, guard_reads;
    uint32_t last_v;
    int exception;
} seen;

static int ours(uint32_t a, unsigned size)
{
    return a >= mem_lo && a + size <= mem_hi;
}

/* An SVC: the guest's registers into a ros_cpu, the real dispatcher, and
 * back.  This is what the bridge (runtime/armrun/bridge.c) does, in miniature */
static void on_svc(void *ctx, struct armrun *a, uint32_t n)
{
    (void)ctx;
    seen.svcs++;
    if (n == EXIT_SWI) {
        armrun_halt(a);
        return;
    }
    uint32_t *r = armrun_regs(a);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 15 * sizeof r[0]);
    ros_msr_f(&s, armrun_cpsr(a));
    ros_swi(&s, n);
    memcpy(r, s.r, 15 * sizeof r[0]);
    armrun_set_cpsr(a, (armrun_cpsr(a) & 0x0FFFFFFFu) | (ros_cpsr(&s) & 0xF0000000u));
    seen.last_v = s.v;
}

static void on_exception(void *ctx, struct armrun *a, uint32_t pc, int kind)
{
    (void)ctx, (void)pc;
    seen.exception = kind;
    armrun_halt(a);
}

static uint64_t on_read(void *ctx, struct armrun *a, uint32_t addr, unsigned size)
{
    (void)ctx, (void)a;
    uint64_t v = 0;
    if (ours(addr, size)) {
        memcpy(&v, ros_ptr(addr), size);
        return v;
    }
    seen.guard_reads++;
    return MARKER;
}

static void on_write(void *ctx, struct armrun *a, uint32_t addr, unsigned size, uint64_t value)
{
    (void)ctx, (void)a;
    if (ours(addr, size))
        memcpy(ros_ptr(addr), &value, size);
}

static int on_fetch(void *ctx, struct armrun *a, uint32_t addr, uint32_t *word)
{
    (void)ctx, (void)a;
    if (!ours(addr, 4))
        return 0;
    *word = ros_ld32(addr);
    return 1;
}

static const struct armrun_hooks hooks = { NULL, on_svc, on_exception, on_read, on_write, on_fetch };

static void put(uint32_t at, const uint32_t *w, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        ros_st32(at + 4 * i, w[i]);
}

static uint32_t run_from(struct armrun *a, uint32_t pc)
{
    memset(&seen, 0, sizeof seen);
    seen.exception = -1;
    armrun_regs(a)[15] = pc;
    armrun_set_cpsr(a, 0x10);
    return armrun_run(a, 1000000);
}

/* Four blocks, 16 bytes apart, each `MOV ri, #v+i; B next` (the last exits) */
static void put_chain(uint32_t at, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++, at += 16) {
        ros_st32(at, 0xE3A00000u | (i << 12) | (v + i));
        ros_st32(at + 4, i < 3 ? 0xEA000001u : 0xEF000000u | EXIT_SWI);
        ros_st32(at + 8, 0xE1A00000u);
        ros_st32(at + 12, 0xE1A00000u);
    }
}

static void svc_block(struct armrun *a, uint32_t code, uint32_t buf)
{
    static const uint32_t w[] = {
        0xE59F0018, /* LDR   r0, [pc, #24]   ; the value, &DEADBEEF  */
        0xE1A01004, /* MOV   r1, r4          ; the buffer             */
        0xE3A02010, /* MOV   r2, #16                                   */
        0xEF0200D4, /* SVC   XOS_ConvertHex8                           */
        0x23A06001, /* MOVCS r6, #1          ; (never: C clear)        */
        0xE5947000, /* LDR   r7, [r4]        ; the first four digits   */
        0xEF000011, /* SVC   OS_Exit                                   */
        0x00000000,
        0xDEADBEEF, /* the literal, at pc + 8 + 24 from the LDR        */
    };
    put(code, w, sizeof w / 4);
    memset(ros_ptr(buf), 0, 16);
    uint32_t *r = armrun_regs(a);
    r[4] = buf, r[6] = 0, r[7] = 0;
    uint32_t why = run_from(a, code);
    const char *digits = ros_ptr(buf);
    check((why & ARMRUN_HALTED) && seen.svcs == 2 && seen.exception < 0 && !seen.last_v &&
              memcmp(digits, "DEADBEEF", 9) == 0 && r[0] == buf && r[7] == 0x44414544u && r[6] == 0,
          "armrun: A32 code in the RMA runs, and its SVC reaches the SWI dispatcher -- "
          "XOS_ConvertHex8's digits in guest memory, read back by the code",
          "halt &%X svcs %u exc %d V %u digits '%.8s' r0 &%X r7 &%08X", why, seen.svcs,
          seen.exception, seen.last_v, digits, r[0], r[7]);
}

#if BOX
/* ros_fault_jit's shape over armrun.h's (which names no siginfo_t) */
static int jit_fault(int sig, siginfo_t *si, void *context)
{
    return armrun_handle_fault(sig, si, context);
}

static void fault(struct armrun *a, uint32_t code)
{
    static const uint32_t w[] = {
        0xE5945000, /* LDR r5, [r4]          ; the guard page */
        0xEF000011, /* SVC OS_Exit                            */
    };
    put(code, w, 2);
    int (*was)(int, siginfo_t *, void *) = ros_fault_jit;
    ros_fault_jit = jit_fault;
    uint32_t *r = armrun_regs(a);
    r[4] = ROS_SVCSTACK_GUARD_AT, r[5] = 0;
    run_from(a, code);
    check(seen.guard_reads == 1 && r[5] == MARKER && seen.svcs == 1,
          "armrun: a guest load faulting in translated code is offered to the engine by the "
          "runtime's handler, and answered by the read hook",
          "guard reads %u r5 &%08X svcs %u", seen.guard_reads, r[5], seen.svcs);

    /* The runtime's own fault, the engine present: still the runtime's */
    struct ros_fault_escape esc;
    volatile int escaped = 0;
    ros_fault_escape = &esc;
    if (sigsetjmp(esc.jb, 1) == 0)
        (void)*(volatile uint32_t *)ros_ptr(ROS_SVCSTACK_GUARD_AT);
    else
        escaped = 1;
    ros_fault_escape = NULL;
    ros_fault_jit = was;
    check(escaped && esc.sig == SIGSEGV && esc.addr == (uint64_t)(uintptr_t)ros_ptr(ROS_SVCSTACK_GUARD_AT),
          "armrun: a fault outside translated code goes on to the runtime's handler",
          "escaped %d sig %d addr &%llX", escaped, esc.sig, (unsigned long long)esc.addr);
}
#endif

static void inval(struct armrun *a, uint32_t at)
{
    uint32_t *r = armrun_regs(a);
    put_chain(at, 10);
    run_from(a, at);
    int first = r[0] == 10 && r[1] == 11 && r[2] == 12 && r[3] == 13;
    put_chain(at, 20);
    armrun_invalidate(a, at, 0x40);
    run_from(a, at);
    check(first && r[0] == 20 && r[1] == 21 && r[2] == 22 && r[3] == 23,
          "armrun: four translated blocks rewritten, one invalidation over them: all four "
          "retranslated", "first %d, then r0-r3 %u %u %u %u", first, r[0], r[1], r[2], r[3]);
}

/* ---- *Run of ARM code -------------------------------------------------------- */

static char out[4096];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    char c = (char)s->r[0];
    if (c != '\r' && outn < sizeof out - 1)
        out[outn++] = c;
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* A command, its VDU output kept in out; the error, or NULL */
static const os_error *run(const char *line)
{
    static char err[260];
    size_t n = strlen(line);
    char *c = ros_rma_alloc((uint32_t)n + 1);
    memcpy(c, line, n);
    c[n] = '\r';
    outn = 0, out[0] = 0;
    ros_vector_claim_native(WRCHV, wrch, 0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(c);
    ros_swi(&s, XOS_CLI);
    ros_vector_release_native(WRCHV, wrch, 0);
    ros_rma_free(c);
    if (!s.v)
        return NULL;
    memcpy(err, ros_ptr(s.r[0]), sizeof err);
    return (const os_error *)err;
}

static void var(const char *name, char *v, size_t room)
{
    char *b = ros_rma_alloc(300);
    v[0] = 0;
    if (!b)
        return;
    strcpy(b, name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b) + 64, c.r[2] = 200, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    if (!c.v) {
        size_t n = c.r[2] < room ? c.r[2] : room - 1;
        memcpy(v, b + 64, n);
        v[n] = 0;
    }
    ros_rma_free(b);
}

static void star_run(void)
{
    char dir[256];
    const char *t = getpid() == 1 ? "/tmp" : getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rosgd-armrun-%d", t ? t : "/tmp", (int)getpid());
    mkdir(dir, 0755);
    char path[300];
    snprintf(path, sizeof path, "%s/boxhello,ff8", dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(armrun_boxhello, 1, ARMRUN_BOXHELLO_SIZE, f);
        fclose(f);
    }
    ros_hostfs_mount("ArmTest", dir);

    char rc[32];
    const os_error *e = run("Set Sys$ReturnCode 99");
    e = run("Run HostFS::ArmTest.$.boxhello one two");
    var("Sys$ReturnCode", rc, sizeof rc);
    check(!e && strstr(out, "ARM: HostFS::ArmTest.$.boxhello one two") && !strcmp(rc, "0"),
          "armrun: *Run of an ARM Absolute -- loaded at &8000, run by the engine, its SWIs the box's "
          "(OS_GetEnv's command line written by OS_Write0), its return an exit with code 0",
          "%s [%s] rc %s", e ? e->errmess : "ran", out, rc);

    e = run("HostFS::ArmTest.$.boxhello -x");
    var("Sys$ReturnCode", rc, sizeof rc);
    check(!e && !strcmp(rc, "17"), "armrun: OS_Exit from ARM code, \"ABEX\" and 17: Sys$ReturnCode 17",
          "%s rc %s", e ? e->errmess : "ran", rc);

    e = run("HostFS::ArmTest.$.boxhello -e");
    check(!e && strstr(out, "Error: ARM error (Error number &1A2B)"),
          "armrun: a non-X error from ARM code with no handler of its own: the default handler "
          "reports it and *Run comes back", "%s [%s]", e ? e->errmess : "ran", out);

    e = run("HostFS::ArmTest.$.boxhello -h");
    var("Sys$ReturnCode", rc, sizeof rc);
    check(!e && strstr(out, "caught &1A2B") && !strcmp(rc, "3"),
          "armrun: the same error with an ARM error handler of its own (OS_ChangeEnvironment 6): "
          "the handler runs, reads the buffer, exits 3", "%s [%s] rc %s", e ? e->errmess : "ran", out,
          rc);

    e = run("HostFS::ArmTest.$.boxhello -c");
    check(!e && strstr(out, "callback ran"),
          "armrun: OS_AddCallBack into ARM code -- the runtime calls it (ros_call's fourth kind, a "
          "nested engine)", "%s [%s]", e ? e->errmess : "ran", out);
    scratch_gone("ArmTest", dir);
}

/* ---- ARM code calling native code, nested (#146) ------------------------------------ */

#define NEST_LEVELS 4                   /* native callbacks: ARM calls as many */
#define NEST_ERR    0x1F146u

static struct {
    uint32_t arm, cb;                   /* nest.s in the RMA; the callback's native entry */
    unsigned level, deepest;            /* the callback's depth now; the most it went */
    int raise, vset;                    /* the deepest: raise an error, or return one */
    uint32_t seen[NEST_LEVELS], r4[NEST_LEVELS];
    int kept;                           /* the callback's own r4-r11 kept across its ARM call */
    os_error *block;                    /* the error the deepest returns (vset) */
} nest;

/* The native callback: r0 a value.  Above the deepest it calls the ARM
 * routine with r0 + 1 and gives back twice its result; the deepest gives
 * r0 + 1, or raises, or returns an error */
static void nest_cb(struct ros_cpu *s)
{
    unsigned lv = nest.level++;
    if (nest.level > nest.deepest)
        nest.deepest = nest.level;
    nest.seen[lv] = s->r[0];
    nest.r4[lv] = s->r[4];
    if (lv + 1 == NEST_LEVELS) {
        if (nest.raise)
            ros_raise(ros_error(NEST_ERR, "Raised at the deepest native level"));
        if (nest.vset) {
            s->v = 1;
            s->r[0] = ros_addr(nest.block);
        } else {
            s->r[0] += 1;
        }
    } else {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        for (unsigned i = 4; i < 12; i++)
            c.r[i] = 0xC0DE0000u + i;
        c.r[0] = s->r[0] + 1;
        c.r[1] = nest.cb;
        ros_call(&c, nest.arm);
        for (unsigned i = 4; i < 12; i++)
            if (c.r[i] != 0xC0DE0000u + i)
                nest.kept = 0;
        if (c.v) {
            s->v = 1;
            s->r[0] = c.r[0];
        } else {
            s->r[0] = c.r[0] * 2;
        }
    }
    nest.level--;
    s->r[15] = s->r[14];
}

/* What the chain gives for x at the callback's level lv */
static uint32_t nest_model(uint32_t x, unsigned lv)
{
    uint32_t cb = lv + 1 == NEST_LEVELS ? x + 1 : nest_model(x + 1, lv + 1) * 2;
    return cb + 100;                    /* the ARM routine's */
}

/* The top: native code calling the ARM routine with 1 and the callback;
 * the result, V and R0 in c */
static void nest_top(struct ros_cpu *c)
{
    nest.level = nest.deepest = 0;
    nest.kept = 1;
    memset(nest.seen, 0, sizeof nest.seen);
    memset(nest.r4, 0, sizeof nest.r4);
    ros_cpu_enter(c);
    for (unsigned i = 4; i < 12; i++)
        c->r[i] = 0x70700000u + i;
    c->r[0] = 1;
    c->r[1] = nest.cb;
    ros_call(c, nest.arm);
}

static void nesting(void)
{
    void *block = ros_rma_alloc(ARMRUN_NEST_SIZE + 16);
    nest.block = ros_rma_alloc(64);
    if (!block || !nest.block) {
        check(0, "armrun: an RMA block for nest.s", "none");
        return;
    }
    nest.block->errnum = NEST_ERR + 1;
    strcpy(nest.block->errmess, "Returned at the deepest native level");
    nest.arm = (ros_addr(block) + 15) & ~15u;
    memcpy(ros_ptr(nest.arm), armrun_nest, ARMRUN_NEST_SIZE);
    ros_armrun_code_add(nest.arm, nest.arm + ARMRUN_NEST_SIZE);
    if (!nest.cb)
        nest.cb = ros_native_entry(nest_cb, "selftest_armrun nest_cb");
    const uint32_t svc_sp = ros_svc_sp;

    struct ros_cpu c;
    nest_top(&c);
    int kept = 1, r4 = 1;
    for (unsigned i = 4; i < 12; i++)
        kept &= c.r[i] == 0x70700000u + i;
    for (unsigned lv = 0; lv < NEST_LEVELS; lv++)
        r4 &= nest.seen[lv] == lv + 1 && nest.r4[lv] == 0xA4A4A4A4u;
    uint32_t want = nest_model(1, 0);
    check(!c.v && c.r[0] == want && nest.deepest == NEST_LEVELS && r4,
          "armrun: ARM code calls a native callback, which calls ARM code again: native -> ARM -> "
          "native ... four deep each way, the value right at each level (#146)",
          "v %d r0 %u (want %u) deepest %u seen %u %u %u %u r4 %08X", c.v, c.r[0], want,
          nest.deepest, nest.seen[0], nest.seen[1], nest.seen[2], nest.seen[3], nest.r4[0]);
    check(kept && nest.kept && !(c.r[0] & 0xFFF00000u) && ros_svc_sp == svc_sp,
          "armrun: registers kept across each call -- the ARM code's r4-r11 across the native "
          "callback, the native code's across the ARM code -- and the SVC stack back where it was",
          "top kept %d, callbacks kept %d, r0 &%X, svc sp &%X was &%X", kept, nest.kept, c.r[0],
          ros_svc_sp, svc_sp);

    nest.raise = 1;
    nest_top(&c);
    nest.raise = 0;
    const os_error *e = c.v ? ros_ptr(c.r[0]) : NULL;
    check(e && e->errnum == NEST_ERR && strstr(e->errmess, "Raised at the deepest") &&
              nest.deepest == NEST_LEVELS && ros_svc_sp == svc_sp,
          "armrun: an error raised by the deepest native callback ends the nested calls and "
          "comes back to the top native caller as an error", "v %d %s deepest %u",
          c.v, e ? e->errmess : "-", nest.deepest);

    nest.vset = 1;
    nest_top(&c);
    nest.vset = 0;
    e = c.v ? ros_ptr(c.r[0]) : NULL;
    check(e && e->errnum == NEST_ERR + 1,
          "armrun: an error the deepest native callback returns (V set) passes up through each "
          "ARM and native level", "v %d %s", c.v, e ? e->errmess : "-");

    nest_top(&c);
    check(!c.v && c.r[0] == want, "armrun: after the errors, the same nesting again: the engines "
          "unharmed", "v %d r0 %u", c.v, c.r[0]);

    ros_armrun_code_remove(nest.arm);
    ros_rma_free(nest.block);
    ros_rma_free(block);
}

void ros_selftest_armrun(void)
{
    star_run();
    nesting();

    const uint32_t size = 0x1000;
    void *block = ros_rma_alloc(size + 16);
    if (!block) {
        check(0, "armrun: an RMA block for the code", "none");
        return;
    }
    mem_lo = (ros_addr(block) + 15) & ~15u;
    mem_hi = mem_lo + size;
    struct armrun *a = armrun_create(ros_arena_base, 0, &hooks);
    svc_block(a, mem_lo, mem_lo + 0x800);
#if BOX
    fault(a, mem_lo + 0x100);
#endif
    inval(a, mem_lo + 0x200);
    armrun_destroy(a);
    ros_rma_free(block);
}
