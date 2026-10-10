/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* capp_jit.c.inc -- BASIC's AArch64 code on an Apple Silicon Mac (runtime/
 * capp.c includes it where it stands, in a hosted build that defines
 * ROS_CAPP_A64_JIT: BBC BASIC V for Mac's, capp.h).  No box compiles it.
 *
 * The code BASIC's assembler writes for AArch64 (*BasicAsmCPU A64) runs
 * here as it does in the box on AArch64 (capp.c.inc, call_assembled). The
 * ']' that closes a block registers its range. CALL and USR find no
 * compiled code there and come to ros_capp_call, which calls the block as
 * an AAPCS64 function of the same contract:
 *
 *     void block(uint32_t regs[17],
 *                uint64_t (*swi)(uint32_t number, uint32_t *in, uint32_t *out));
 *
 * with one addition, x2 (below). What differs is where the code runs.
 * On the Mac, unsigned code pages are writable or executable but never
 * both, and the arena is neither. So each assembled block is COPIED into a
 * MAP_JIT arena as ']' closes it, and CALL runs the copy. The copy is of
 * the block as assembled. A program that patches its own code through the
 * arena runs the bytes it wrote only in the box. No such program is
 * known. The write protection is per thread. The interpreter's thread
 * writes with the protection off (pthread_jit_write_protect_np(0)) and
 * calls with it on.
 *
 * x2 is the arena's base as a 64-bit host pointer. In the box the arena
 * is at 0, so an arena address in a 32-bit register is a pointer. On the
 * Mac it is not, and a 32-bit register cannot hold the base. So a kernel
 * that wants a pointer takes the register's 32-bit value as an offset and
 * adds the base, which it keeps from x2 (mov x20, x2). The box enters with
 * x2 zero, so the same program runs in both.
 *
 * A fault in the block (SIGSEGV, SIGBUS or SIGILL while its own
 * instructions run) is the program's error, as the box's fault.c makes
 * it. It does not end the process. capp_trap.c catches it, and it is
 * raised here as "Internal error: abort on data transfer at &XXXXXXXX".
 * The faulting instruction is named by its address in the arena, where
 * BASIC assembled it, and not by the address in its copy. */

#include <pthread.h>
#include <signal.h>

#include "rosgd/fault.h"

/* Both are guarded out under strict ISO (-std=c11). The declarations
 * have the system's own shapes, which are stable. */
extern void pthread_jit_write_protect_np(int enabled);
#ifndef MAP_JIT
#define MAP_JIT 0x8000
#endif

#define RANGES_A64 64

static struct {
    uint32_t lo, hi;
    unsigned char *jit;             /* the block's executable copy */
} a64code[RANGES_A64];
static unsigned a64n;

static void *jit_arena;
static size_t jit_used, jit_cap = 4u << 20;

static unsigned char *jit_copy(uint32_t lo, uint32_t hi)
{
    if (!jit_arena) {
        pthread_jit_write_protect_np(0);
        jit_arena = mmap(NULL, jit_cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
        if (jit_arena == MAP_FAILED) {
            jit_arena = NULL;
            return NULL;
        }
    }
    size_t n = hi - lo;
    if (n > jit_cap - jit_used)
        return NULL;
    pthread_jit_write_protect_np(0);
    unsigned char *p = (unsigned char *)jit_arena + jit_used;
    memcpy(p, ros_ptr(lo), n);
    __builtin___clear_cache((char *)p, (char *)p + n);
    pthread_jit_write_protect_np(1);
    jit_used += (n + 15u) & ~(size_t)15u;
    return p;
}

os_error *ros_capp_code_assembled(uint32_t lo, uint32_t hi)
{
    /* One range per block. It is grown over any range it overlaps or
     * touches, and its copy is made afresh of the whole. */
    for (unsigned i = 0; i < a64n;)
        if (lo <= a64code[i].hi && a64code[i].lo <= hi) {
            if (a64code[i].lo < lo)
                lo = a64code[i].lo;
            if (a64code[i].hi > hi)
                hi = a64code[i].hi;
            a64code[i] = a64code[--a64n];
        } else
            i++;
    if (a64n == RANGES_A64)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for another code range");
    unsigned char *p = jit_copy(lo, hi);
    if (!p)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for another code range");
    a64code[a64n].lo = lo;
    a64code[a64n].hi = hi;
    a64code[a64n].jit = p;
    a64n++;
    return NULL;
}

void ros_capp_code_forget_assembled(uint32_t lo, uint32_t hi)
{
    for (unsigned i = 0; i < a64n;)
        if (a64code[i].lo < hi && lo < a64code[i].hi)
            a64code[i] = a64code[--a64n];
        else
            i++;
}

/* capp_trap.c: the block's faults caught while its own instructions run */
int ros_capp_trap_call(void (*fn)(void *), void *arg, uint64_t *pc, uint64_t *addr);
void *ros_capp_trap_suspend(void);
void ros_capp_trap_resume(void *t);

/* The block's SWIs take a user-mode SWI's path, as the box's a64_swi does.
 * The runtime's own faults while it runs are not the block's. */
static uint64_t a64_swi(uint32_t number, uint32_t *in, uint32_t *out)
{
    void *trap = ros_capp_trap_suspend();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.mode = ROS_MODE_USR;
    for (unsigned i = 0; i < 10; i++)
        c.r[i] = in[i];
    ros_swi(&c, number);
    for (unsigned i = 0; i < 10; i++)
        out[i] = c.r[i];
    ros_capp_trap_resume(trap);
    return (uint64_t)ros_cpsr(&c) << 32 | (c.v ? c.r[0] : 0);
}

/* The call, for ros_capp_trap_call. x0 is the register block, x1 the SWI
 * entry and x2 the arena's base. They are set through asm so that x2 is
 * set at all. */
struct jit_call {
    uint32_t *regs;
    void *fn;
};

static void jit_run(void *arg)
{
    struct jit_call *c = arg;
    register uint32_t *r0 __asm__("x0") = c->regs;
    register uint64_t (*r1)(uint32_t, uint32_t *, uint32_t *) __asm__("x1") = a64_swi;
    register uint64_t r2 __asm__("x2") = (uint64_t)(uintptr_t)ros_ptr(0);
    void *fn = c->fn;
    /* blr writes x30. It is declared, so that the compiler saves the
     * return address it would otherwise keep there. In a function this
     * small it did keep it there, and the ret after the call came back
     * into the call. */
    __asm__ volatile("blr %3"
                     : "+r"(r0), "+r"(r1), "+r"(r2)
                     : "r"(fn)
                     : "x3", "x4", "x5", "x6", "x7", "x8", "x9",
                       "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17", "x18", "x30", "memory", "cc");
    (void)r0, (void)r1, (void)r2;
}

int ros_capp_call(struct ros_cpu *s, uint32_t addr)
{
    unsigned i = 0;
    while (i < a64n && !(addr >= a64code[i].lo && addr < a64code[i].hi))
        i++;
    if (i == a64n)
        return 0;
    uint32_t regs[17];
    for (unsigned r = 0; r < 15; r++)
        regs[r] = s->r[r];
    regs[15] = s->r[14];                /* where it returns to */
    regs[16] = ros_cpsr(s);
    unsigned char *jit = a64code[i].jit;
    uint32_t lo = a64code[i].lo, n = a64code[i].hi - a64code[i].lo;
    struct jit_call call = { regs, jit + (addr - lo) };
    uint64_t fpcr, fpc = 0, faddr = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    pthread_jit_write_protect_np(1);    /* executable, not writable */
    int sig = ros_capp_trap_call(jit_run, &call, &fpc, &faddr);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));     /* the caller's, as the box's */
    if (sig) {
        uint64_t base = (uint64_t)(uintptr_t)jit;
        uint32_t pc = fpc >= base && fpc < base + n ? lo + (uint32_t)(fpc - base) : (uint32_t)fpc;
        /* Raised at the instruction, as fault.c's landing raises it. */
        if (sig == SIGILL)
            ros_raise_at(ros_error(ROS_ERR_UNDEFINED_INSTRUCTION,
                                   "Internal error: undefined instruction at &%08X", pc), pc);
        if (faddr == fpc)
            ros_raise_at(ros_error(ROS_ERR_INSTRUCTION_ABORT,
                                   "Internal error: abort on instruction fetch at &%08X", pc), pc);
        ros_raise_at(ros_error(ROS_ERR_DATA_ABORT,
                               "Internal error: abort on data transfer at &%08X", pc), pc);
    }
    for (unsigned r = 0; r < 16; r++)
        s->r[r] = regs[r];
    ros_msr_f(s, regs[16]);
    return 1;
}
