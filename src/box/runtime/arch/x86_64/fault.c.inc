/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fault.c.inc -- the x86-64 half of the fault handler's signal side
 * (runtime/fault.c includes it where it stands). It holds the instruction
 * decoder that gives a #DE ARM's division result, the classifier of an x32
 * program's traps (int3, #UD, #AC, #XM...), the landing, the signal's way
 * in, and the handler itself. AArch64's is ../aarch64/fault.c.inc. That
 * one classifies by the ESR, and A64's divisions do not trap. */

/* Maps x86's register numbers to the context's. */
static const int greg_of[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                 REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                 REG_R12, REG_R13, REG_R14, REG_R15 };

static uint64_t gs_base(void)
{
    return ros_capp_gs();
}

/* What lies in the prefixes of the instruction at p. This is how many
 * there are, the REX byte (0 if none), the operand-size and address-size
 * overrides, and the segment (0x64, 0x65, else 0). */
struct prefixes {
    unsigned n;
    uint8_t rex, seg;
    int opsize16, addr32;
};

static int is_prefix(uint8_t b)
{
    return b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x26 ||
           b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65;
}

static void prefixes(const uint8_t *p, struct prefixes *x)
{
    *x = (struct prefixes){ 0 };
    while (x->n < 14 && is_prefix(p[x->n])) {
        uint8_t b = p[x->n++];
        if (b == 0x66)
            x->opsize16 = 1;
        else if (b == 0x67)
            x->addr32 = 1;
        else if (b == 0x64 || b == 0x65)
            x->seg = b;
    }
    if ((p[x->n] & 0xF0) == 0x40)
        x->rex = p[x->n++];
}

/* Whether the instruction at p is one that user mode may not execute. A
 * #GP from one is ARM's undefined instruction, as an MRC of a system
 * register is from user mode on 5.30 (the farm's record). */
static int privileged(const uint8_t *p)
{
    struct prefixes x;
    prefixes(p, &x);
    uint8_t op = p[x.n], op2 = p[x.n + 1];
    if (op == 0xF4 || op == 0xFA || op == 0xFB || op == 0xCF || (op >= 0xE4 && op <= 0xE7) ||
        (op >= 0xEC && op <= 0xEF) || (op >= 0x6C && op <= 0x6F) || op == 0xCD || op == 0xF1)
        return 1;
    return op == 0x0F && (op2 == 0x00 || op2 == 0x01 || op2 == 0x06 || op2 == 0x07 || op2 == 0x08 ||
                          op2 == 0x09 || (op2 >= 0x20 && op2 <= 0x23) || op2 == 0x30 ||
                          op2 == 0x32 || op2 == 0x35 || op2 == 0x78 || op2 == 0x79);
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* A #DE: the DIV or IDIV at pc. If its divisor is 0, this returns 0.
 * Otherwise it writes the quotient as ARM's division gives it, which is
 * truncated to the operand's size, with INT_MIN / -1 being INT_MIN. It
 * writes the remainder, steps over the instruction and returns 1. It
 * returns -1 if the instruction is not one that it can read. */
static int division(ucontext_t *uc, uint32_t pc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    const uint8_t *p = ros_ptr(pc);
    struct prefixes x;
    prefixes(p, &x);
    unsigned i = x.n;
    uint8_t op = p[i++];
    if (op != 0xF6 && op != 0xF7)
        return -1;
    uint8_t modrm = p[i++];
    unsigned mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (reg != 6 && reg != 7)
        return -1;
    int is_signed = reg == 7;
    unsigned size = op == 0xF6 ? 1 : (x.rex & 8) ? 8 : x.opsize16 ? 2 : 4;
    uint64_t divisor = 0;
    if (mod == 3) {
        unsigned r = rm | (x.rex & 1) << 3;
        if (size == 1 && !x.rex && rm >= 4)
            divisor = (uint64_t)g[greg_of[rm - 4]] >> 8;        /* AH, CH, DH, BH */
        else
            divisor = (uint64_t)g[greg_of[r]];
    } else {
        uint64_t ea = 0;
        int rip_relative = 0;
        if (rm == 4) {
            uint8_t sib = p[i++];
            unsigned scale = sib >> 6, index = ((sib >> 3) & 7) | (x.rex & 2) << 2;
            unsigned base = (sib & 7) | (x.rex & 1) << 3;
            if (index != 4)
                ea += (uint64_t)g[greg_of[index]] << scale;
            if ((sib & 7) == 5 && mod == 0) {
                ea += (uint64_t)(int64_t)(int32_t)rd32(p + i);
                i += 4;
            } else {
                ea += (uint64_t)g[greg_of[base]];
            }
        } else if (rm == 5 && mod == 0) {
            ea = (uint64_t)(int64_t)(int32_t)rd32(p + i);
            i += 4;
            rip_relative = 1;
        } else {
            ea = (uint64_t)g[greg_of[rm | (x.rex & 1) << 3]];
        }
        if (mod == 1) {
            ea += (uint64_t)(int64_t)(int8_t)p[i];
            i += 1;
        } else if (mod == 2) {
            ea += (uint64_t)(int64_t)(int32_t)rd32(p + i);
            i += 4;
        }
        if (rip_relative)
            ea += (uint64_t)pc + i;
        if (x.addr32)
            ea &= 0xFFFFFFFFull;
        if (x.seg == 0x65)
            ea += gs_base();
        else if (x.seg == 0x64)
            return -1;                  /* the runtime's thread pointer, not x32 code's */
        if (ea + size > ARENA_TOP)
            return -1;
        memcpy(&divisor, ros_ptr((uint32_t)ea), size);  /* it was just read */
    }
    unsigned bits = 8 * size;
    uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    divisor &= mask;
    if (divisor == 0)
        return 0;
    uint64_t rax = (uint64_t)g[REG_RAX], rdx = (uint64_t)g[REG_RDX];
    unsigned __int128 dividend;
    if (size == 1)
        dividend = rax & 0xFFFF;
    else
        dividend = (unsigned __int128)(rdx & mask) << bits | (rax & mask);
    unsigned __int128 q, r;
    if (is_signed) {
        /* Sign-extend the 2n-bit dividend and the n-bit divisor. */
        unsigned dbits = 2 * bits;
        __int128 n = dbits == 128 ? (__int128)dividend
                                  : (__int128)(dividend << (128 - dbits)) >> (128 - dbits);
        int64_t d = bits == 64 ? (int64_t)divisor : (int64_t)(divisor << (64 - bits)) >> (64 - bits);
        if (d == -1) {
            q = (unsigned __int128)0 - (unsigned __int128)n;        /* wraps: INT_MIN / -1 */
            r = 0;
        } else {
            q = (unsigned __int128)(n / d);
            r = (unsigned __int128)(n % d);
        }
    } else {
        q = dividend / divisor;
        r = dividend % divisor;
    }
    uint64_t qv = (uint64_t)q & mask, rv = (uint64_t)r & mask;
    if (size == 1) {
        g[REG_RAX] = (greg_t)((rax & ~0xFFFFull) | rv << 8 | qv);
    } else if (size == 2) {
        g[REG_RAX] = (greg_t)((rax & ~0xFFFFull) | qv);
        g[REG_RDX] = (greg_t)((rdx & ~0xFFFFull) | rv);
    } else {
        g[REG_RAX] = (greg_t)qv;        /* 32-bit results zero-extend, as on x86 */
        g[REG_RDX] = (greg_t)rv;
    }
    g[REG_RIP] = (greg_t)((uint64_t)pc + i);
    return 1;
}

static const char *const fp_kind[5] = { "invalid operation", "overflow", "division by zero",
                                        "underflow", "inexact operation" };

/* What a fault in x32 code is. This returns 0 if the fault is finished (a
 * division emulated), else 1. */
static int classify_x32(struct fault *f, ucontext_t *uc)
{
    uint64_t rip = (uint64_t)f->g[REG_RIP];
    unsigned trap = (unsigned)f->g[REG_TRAPNO], err = (unsigned)f->g[REG_ERR];
    f->pc = (uint32_t)rip;
    switch (f->sig) {
    case SIGSEGV:
        if (trap == TRAP_GP || f->code == SI_KERNEL) {     /* #GP: si_addr says nothing */
            if (privileged(ros_ptr(f->pc)))
                f->errnum = ROS_ERR_UNDEFINED_INSTRUCTION, f->kind = "privileged instruction";
            else
                f->errnum = ROS_ERR_DATA_ABORT, f->kind = "general protection fault";
        } else if (trap == TRAP_PF ? (err & PF_FETCH) != 0
                                   : f->addr == rip ||
                                         (f->addr > rip && f->addr - rip < 16 &&
                                          (f->addr & 0xFFF) == 0)) {
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
        f->errnum = ROS_ERR_DATA_ABORT;
        f->kind = trap == TRAP_AC || f->code == BUS_ADRALN ? "data abort (alignment check)"
                  : out_of_memory(f)                     ? OUT_OF_MEMORY
                                                         : "data abort (bus error)";
        return 1;
    case SIGILL:
        f->errnum = ROS_ERR_UNDEFINED_INSTRUCTION, f->kind = "undefined instruction";
        return 1;
    case SIGTRAP: {
        /* int3 reports the address of the instruction after it. Step back
         * to it, because 5.30's BKPT gives its own address, as a prefetch
         * abort. */
        const uint8_t *p = ros_ptr(f->pc);
        if (trap == TRAP_BP && rip >= 1 && p[-1] == 0xCC)
            f->pc -= 1;
        else if (trap == TRAP_BP && rip >= 2 && p[-2] == 0xCD && p[-1] == 0x03)
            f->pc -= 2;
        f->errnum = ROS_ERR_INSTRUCTION_ABORT;
        f->kind = trap == TRAP_DB ? "single step" : "breakpoint";
        return 1;
    }
    case SIGFPE:
        if (trap == TRAP_DE || f->code == FPE_INTDIV || f->code == FPE_INTOVF) {
            int d = division(uc, f->pc);
            if (d == 1)
                return 0;
            f->errnum = ROS_ERR_DIVIDE_BY_ZERO;
            f->kind = d == 0 ? "integer division by zero" : "integer division (not decoded)";
            return 1;
        }
        unsigned n = f->code == FPE_FLTOVF ? 1 : f->code == FPE_FLTDIV ? 2 : f->code == FPE_FLTUND ? 3
                     : f->code == FPE_FLTRES ? 4 : 0;
        f->errnum = ROS_ERR_FP_BASE + n, f->kind = fp_kind[n];
        return 1;
    }
    return 1;
}

void ros_fault_landing(void);
__attribute__((noreturn, used)) void ros_fault_land_c(void);

/* Runs with the runtime's floating point, as the gate's other ways in
 * have it. The stack is as the handler left it, 16-aligned. */
__asm__(".text\n"
        ".globl ros_fault_landing\n"
        ".type ros_fault_landing,@function\n"
        "ros_fault_landing:\n"
        "   fninit\n"
        "   subq $16, %rsp\n"
        "   movl $0x1F80, (%rsp)\n"
        "   ldmxcsr (%rsp)\n"
        "   movw $0x037F, (%rsp)\n"
        "   fldcw (%rsp)\n"
        "   addq $16, %rsp\n"
        "   cld\n"
        "   call ros_fault_land_c\n"
        "   ud2\n"
        ".size ros_fault_landing, .-ros_fault_landing\n");

/* The signal's way in. EFLAGS.AC is turned off before any C runs. x32
 * code may have set it, and the kernel keeps it for the handler, but the
 * runtime's code does unaligned accesses. */
void ros_fault_signal(int sig, siginfo_t *si, void *context);
__asm__(".text\n"
        ".globl ros_fault_signal\n"
        ".type ros_fault_signal,@function\n"
        "ros_fault_signal:\n"
        "   pushfq\n"
        "   andl $~0x40000, (%rsp)\n"
        "   popfq\n"
        "   jmp ros_fault_on_signal\n"
        ".size ros_fault_signal, .-ros_fault_signal\n");

__attribute__((used)) void ros_fault_on_signal(int sig, siginfo_t *si, void *context);
void ros_fault_on_signal(int sig, siginfo_t *si, void *context)
{
    ucontext_t *uc = context;
    greg_t *g = uc->uc_mcontext.gregs;
    uint64_t rip = (uint64_t)g[REG_RIP], addr = (uint64_t)(uintptr_t)si->si_addr;
    if (ros_fault_jit && (sig == SIGSEGV || sig == SIGBUS) && ros_fault_jit(sig, si, context))
        return;                         /* the ARM container's guest access (fault.h) */
    if (ros_fault_escape && ROS_FAULT_ESCAPES(sig)) {
        /* A Worker thread's job (fault.h). That job fails. */
        ros_fault_escape->sig = sig;
        ros_fault_escape->addr = (uint64_t)(uintptr_t)si->si_addr;
        ros_fault_escape->pc = rip;
        siglongjmp(ros_fault_escape->jb, 1);
    }
    struct fault *f = &fault;
    if (delivering || !ros_lock_held() || ros_in_background())
        goto fatal;
    f->sig = sig, f->code = si->si_code, f->addr = addr;
    memcpy(f->g, g, sizeof f->g);
    f->fpc = uc->uc_mcontext.fpregs ? uc->uc_mcontext.fpregs->mxcsr : 0x1F80u;
    f->fpc2 = uc->uc_mcontext.fpregs ? uc->uc_mcontext.fpregs->cwd : 0x037F;
    f->overflow = f->in_gate = 0;
    uint64_t stack;
    if (rip < ARENA_TOP && (uint64_t)g[REG_RSP] < ARENA_TOP) {
        /* x32 code is the only code below 4 GB, and it runs on an arena
         * stack. If the runtime jumps below 4 GB (a bad function pointer),
         * it is on its native stack, and the fault is fatal. */
        f->where = X32;
        if (!classify_x32(f, uc))
            return;                     /* finished, so carry on with the code */
        stack = ros_capp_native_stack();
        if (!stack)
            goto fatal;
        stack -= 64;
    } else if (rip >= ARENA_TOP && ((sig == SIGSEGV && si->si_code != SI_KERNEL) ||
                (sig == SIGBUS && si->si_code != BUS_ADRALN)) && addr < ARENA_TOP) {
        /* The runtime on an arena address, which is a SWI's copy or the
         * gate's. It is a page fault and not a #GP, whose si_addr would
         * be 0. */
        f->in_gate = ros_capp_gate_fault(&f->gate_sp, &f->gate_ret);
        if (!f->in_gate && ros_call_depth == 0 && !program_takes_errors())
            goto fatal;                 /* compiled code outside a SWI, and no program's handler */
        f->where = RUNTIME;
        f->errnum = ROS_ERR_DATA_ABORT;
        f->kind = addr - ROS_SVCSTACK_GUARD_AT < ROS_SVCSTACK_GUARD ? "data abort (SVC stack overflow)"
                  : sig == SIGBUS && out_of_memory(f)            ? OUT_OF_MEMORY
                                                                : "data abort";
        stack = (uint64_t)g[REG_RSP] - 512;     /* below the red zone, with room to spare */
    } else {
        goto fatal;
    }
    g[REG_RSP] = (greg_t)(stack & ~15ull);
    g[REG_RIP] = (greg_t)(uintptr_t)ros_fault_landing;
    g[REG_EFL] &= ~(greg_t)(1u << 8 | 1u << 10 | 1u << 18);    /* TF, DF, AC */
    if (uc->uc_mcontext.fpregs) {
        uc->uc_mcontext.fpregs->mxcsr = 0x1F80u;
        uc->uc_mcontext.fpregs->cwd = 0x037F;
        uc->uc_mcontext.fpregs->swd = 0;
        uc->uc_mcontext.fpregs->ftw = 0;
    }
    delivering = 1;
    return;
fatal:
    if (fatal)
        fatal(sig, si, context);
    signal(sig, SIG_DFL);               /* there was no fatal handler, or it returned, as Linux would */
}
