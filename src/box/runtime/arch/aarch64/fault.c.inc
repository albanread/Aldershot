/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fault.c.inc -- the AArch64 half of the fault handler's signal side
 * (runtime/fault.c includes it where it stands, in a box on AArch64). It is
 * the twin of x86_64/fault.c.inc. It holds the classifier of a fault in
 * code below 4 GB, which is A64X32 code or BASIC's AArch64 code. The
 * classifier uses the ESR that the kernel records and the instruction at
 * the pc. The file also holds the landing and the handler.
 *
 * What each is (fault.h's table, AArch64's column):
 *
 *   SIGSEGV, ESR EC &20 (an instruction   prefetch abort
 *     abort), or no ESR and the address
 *     the pc
 *   SIGSEGV, a data abort (EC &24)        data abort; in the stack's guard,
 *                                         the stack overflow; in the SVC
 *                                         stack's guard page, a data abort
 *                                         (SVC stack overflow)
 *   SIGBUS                                data abort, for an sp not 16-aligned
 *                                         (EC &26) or a bus error. A pc not
 *                                         4-aligned (EC &22) is a prefetch
 *                                         abort. Other unaligned accesses
 *                                         do not fault on AArch64.
 *   SIGILL                                undefined instruction; udf #0 is
 *                                         an entry the library does not
 *                                         define (roscc's traps)
 *   SIGTRAP, brk #&5503                   "Divide by zero": C's division by
 *                                         zero (the flag table's trap, and
 *                                         the library's _kernel_udiv...).
 *                                         A64's sdiv and udiv never trap.
 *                                         INT_MIN / -1 is INT_MIN, as on ARM.
 *                                         So nothing is ever finished here.
 *   SIGTRAP, another brk                  prefetch abort (a breakpoint), at
 *                                         the brk, as 5.30's BKPT
 *   SIGFPE                                FPEmulator's error for the trap.
 *                                         It never arises on Apple's cores,
 *                                         whose FPCR trap enables are not
 *                                         implemented (ros_capp_fp_traps)
 *
 * There is no pipeline offset. The pc in a text is the instruction's. */

static const char *const fp_kind[5] = { "invalid operation", "overflow", "division by zero",
                                        "underflow", "inexact operation" };

/* The records after the general registers in the signal's context. They
 * follow arch/arm64's sigcontext: _aarch64_ctx headers, ended by a zero
 * magic. The two wanted are FPSIMD's (FPSR, FPCR) and ESR's. */
#define CTX_FPSIMD_MAGIC 0x46508001u
#define CTX_ESR_MAGIC    0x45535201u

struct ctx_head {
    uint32_t magic, size;
};

static unsigned char *ctx_record(mcontext_t *m, uint32_t magic)
{
    unsigned char *p = (unsigned char *)m->__reserved, *end = p + sizeof m->__reserved;
    while (p + sizeof(struct ctx_head) <= end) {
        struct ctx_head h;
        memcpy(&h, p, sizeof h);
        if (!h.magic || h.size < sizeof h || h.size > (size_t)(end - p))
            break;
        if (h.magic == magic)
            return p;
        p += h.size;
    }
    return NULL;
}

/* An instruction in the arena, which has just been fetched. */
static uint32_t insn_at(uint32_t pc)
{
    uint32_t v;
    memcpy(&v, ros_ptr(pc), 4);
    return v;
}

/* What a fault in code below 4 GB is. It always returns 1, because nothing
 * is finished here. x86-64 finishes an overflowing division, but A64's
 * never trap. */
static int classify_a64(struct fault *f)
{
    uint64_t pc = f->g.pc, esr = f->g.esr;
    unsigned ec = (unsigned)(esr >> 26) & 0x3F;
    f->pc = (uint32_t)pc;
    switch (f->sig) {
    case SIGSEGV:
        if (ec == 0x20 || ec == 0x21 || (!esr && f->addr == pc)) {
            f->errnum = ROS_ERR_INSTRUCTION_ABORT, f->kind = "prefetch abort";
        } else if (f->addr < ARENA_TOP && ros_capp_stack_overflow((uint32_t)f->addr)) {
            f->errnum = ROS_ERR_STACK_OVERFLOW, f->kind = "stack overflow", f->overflow = 1;
        } else if (f->addr - ROS_SVCSTACK_GUARD_AT < ROS_SVCSTACK_GUARD) {
            f->errnum = ROS_ERR_DATA_ABORT, f->kind = "data abort (SVC stack overflow)";
        } else {
            f->errnum = ROS_ERR_DATA_ABORT, f->kind = "data abort";
        }
        return 1;
    case SIGBUS:
        if (ec == 0x22) {
            f->errnum = ROS_ERR_INSTRUCTION_ABORT, f->kind = "prefetch abort (pc alignment)";
        } else {
            f->errnum = ROS_ERR_DATA_ABORT;
            f->kind = ec == 0x26 ? "data abort (sp alignment)"
                      : f->code == BUS_ADRALN ? "data abort (alignment)"
                      : out_of_memory(f) ? OUT_OF_MEMORY : "data abort (bus error)";
        }
        return 1;
    case SIGILL:
        f->errnum = ROS_ERR_UNDEFINED_INSTRUCTION;
        f->kind = insn_at(f->pc) == 0 ? "undefined instruction (udf #0: an entry the library does "
                                        "not define)"
                                      : "undefined instruction";
        return 1;
    case SIGTRAP: {
        uint32_t i = insn_at(f->pc);
        if ((i & 0xFFE0001Fu) == 0xD4200000u && ((i >> 5) & 0xFFFF) == 0x5503) {
            f->errnum = ROS_ERR_DIVIDE_BY_ZERO, f->kind = "integer division by zero (brk #&5503)";
        } else {
            f->errnum = ROS_ERR_INSTRUCTION_ABORT;
            f->kind = (i & 0xFFE0001Fu) == 0xD4200000u ? "breakpoint (brk)" : "single step";
        }
        return 1;
    }
    case SIGFPE: {
        unsigned n = f->code == FPE_FLTOVF ? 1 : f->code == FPE_FLTDIV ? 2 : f->code == FPE_FLTUND ? 3
                     : f->code == FPE_FLTRES ? 4 : 0;
        f->errnum = ROS_ERR_FP_BASE + n, f->kind = fp_kind[n];
        return 1;
    }
    }
    return 1;
}

void ros_fault_landing(void);
__attribute__((noreturn, used)) void ros_fault_land_c(void);

/* Runs with the runtime's floating point (FPCR 0), on the stack the handler
 * chose, which is 16-aligned. There is no frame record below it. */
__asm__(".text\n"
        ".globl ros_fault_landing\n"
        ".type ros_fault_landing,%function\n"
        "ros_fault_landing:\n"
        "   msr fpcr, xzr\n"
        "   mov x29, xzr\n"
        "   bl ros_fault_land_c\n"
        "   brk #1\n"
        ".size ros_fault_landing, .-ros_fault_landing\n");

/* Whether code below 4 GB faulting on the native stack is the program's.
 * That is BASIC's AArch64 code: the pc is in a block, or the block has
 * branched away from it (x30). The code is now entered on an arena stack,
 * as a nested entry, so its faults come by the A64X32 path above. This
 * test stays for a block that has moved sp itself. Anything else there is
 * the runtime jumping below 4 GB, which is its own failure. */
static int assembled(uint64_t pc, uint64_t lr)
{
    return (pc < ARENA_TOP && ros_capp_code((uint32_t)pc) == ROS_CAPP_ASSEMBLED) ||
           (lr < ARENA_TOP && ros_capp_code((uint32_t)lr) == ROS_CAPP_ASSEMBLED);
}

/* The handler. It runs with SA_ONSTACK, on the thread's signal stack
 * (task.h). */
void ros_fault_signal(int sig, siginfo_t *si, void *context);
void ros_fault_signal(int sig, siginfo_t *si, void *context)
{
    ucontext_t *uc = context;
    mcontext_t *m = &uc->uc_mcontext;
    uint64_t pc = m->pc, sp = m->sp, addr = (uint64_t)(uintptr_t)si->si_addr;
    if (ros_fault_jit && (sig == SIGSEGV || sig == SIGBUS) && ros_fault_jit(sig, si, context))
        return;                         /* the ARM container's guest access (fault.h) */
    if (ros_fault_escape && ROS_FAULT_ESCAPES(sig)) {
        /* A Worker thread's job (fault.h). That job fails. */
        ros_fault_escape->sig = sig;
        ros_fault_escape->addr = (uint64_t)(uintptr_t)si->si_addr;
        ros_fault_escape->pc = pc;
        siglongjmp(ros_fault_escape->jb, 1);
    }
    struct fault *f = &fault;
    if (delivering || !ros_lock_held() || ros_in_background())
        goto fatal;
    f->sig = sig, f->code = si->si_code, f->addr = addr;
    memcpy(f->g.x, m->regs, sizeof f->g.x);
    f->g.sp = sp, f->g.pc = pc, f->g.pstate = m->pstate;
    unsigned char *esr = ctx_record(m, CTX_ESR_MAGIC), *fp = ctx_record(m, CTX_FPSIMD_MAGIC);
    /* The kernel records the ESR of the last fault it took for the thread
     * (thread.fault_code). A brk or a trapped FP exception does not
     * replace it. So only a memory fault's ESR belongs to this fault. */
    f->g.esr = 0;
    if (esr && (sig == SIGSEGV || sig == SIGBUS))
        memcpy(&f->g.esr, esr + 8, 8);
    uint32_t fpsr = 0, fpcr = 0;
    if (fp) {
        memcpy(&fpsr, fp + 8, 4);
        memcpy(&fpcr, fp + 12, 4);
    }
    f->g.fpsr = fpsr;
    f->fpc = fpcr, f->fpc2 = 0;
    f->overflow = f->in_gate = 0;
    uint64_t stack;
    if (pc < ARENA_TOP && sp < ARENA_TOP) {
        /* A64X32 code is below 4 GB, on an arena stack. */
        f->where = X32;
        if (!classify_a64(f))
            return;
        stack = ros_capp_native_stack();
        if (!stack)
            goto fatal;
        stack -= 64;
    } else if (sp >= ARENA_TOP && assembled(pc, m->regs[30])) {
        /* BASIC's AArch64 code, on this thread's native stack. Its error is
         * raised below where it stands. */
        f->where = X32;
        classify_a64(f);
        stack = sp - 512;
    } else if (pc >= ARENA_TOP && ((sig == SIGSEGV && si->si_code != SI_KERNEL) ||
                (sig == SIGBUS && si->si_code != BUS_ADRALN)) && addr < ARENA_TOP) {
        /* The runtime on an arena address, which is a SWI's copy or the
         * gate's. */
        f->in_gate = ros_capp_gate_fault(&f->gate_sp, &f->gate_ret);
        if (!f->in_gate && ros_call_depth == 0 && !program_takes_errors())
            goto fatal;                 /* compiled code outside a SWI, and no program's handler */
        f->where = RUNTIME;
        f->errnum = ROS_ERR_DATA_ABORT;
        f->kind = addr - ROS_SVCSTACK_GUARD_AT < ROS_SVCSTACK_GUARD ? "data abort (SVC stack overflow)"
                  : sig == SIGBUS && out_of_memory(f)            ? OUT_OF_MEMORY
                                                                : "data abort";
        stack = sp - 512;               /* below the frame that faulted, with room to spare */
    } else {
        goto fatal;
    }
    m->sp = stack & ~15ull;
    m->pc = (uint64_t)(uintptr_t)ros_fault_landing;
    if (fp) {                           /* the runtime's floating point, flags clear */
        uint32_t zero = 0;
        memcpy(fp + 8, &zero, 4);
        memcpy(fp + 12, &zero, 4);
    }
    delivering = 1;
    return;
fatal:
    if (fatal)
        fatal(sig, si, context);
    signal(sig, SIG_DFL);               /* there was no fatal handler, or it returned, as Linux would */
}
