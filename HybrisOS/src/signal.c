/* signal.c: Linux signals, implemented as the box's musl library expects
 * them.  This covers signal actions, the signal mask, the alternate signal
 * stack, and delivery.  Delivery builds the same stack frame as AArch64
 * Linux (arch/arm64/kernel/signal.c): a siginfo, followed by a ucontext
 * whose mcontext holds the general registers and, in its reserved space,
 * the floating-point and SIMD registers.  The handler returns through the
 * action's restorer, which calls rt_sigreturn, and the HAL then loads the
 * saved frame back.
 *
 * Three things raise signals.  An undefined instruction raises SIGILL;
 * OpenSSL probes the processor's features this way.  A fault that the box's
 * memory cannot satisfy raises SIGSEGV; runtime/fault.c turns such a fault
 * in a program into a RISC OS error.  Finally, the box may send a signal to
 * itself with kill or tkill. */
#include "hal.h"

#define SA_ONSTACK   0x08000000UL
#define SA_RESTORER  0x04000000UL
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL
#define SS_ONSTACK   1
#define SS_DISABLE   2
#define SIG_IGN      1

void fpsimd_save(void *buf);
void fpsimd_load(const void *buf);

#define actions (thread_current()->proc->act)  /* actions belong to the process */

/* The signal mask and the alternate stack belong to each thread. */
struct altstack {
    uint64_t sp;
    int32_t flags, pad;
    uint64_t size;
};
#define MASK (thread_current()->sigmask)
#define alt (*(struct altstack *)&thread_current()->alt_sp)

/* ---- the signal frame ------------------------------------------------- */

struct sigcontext {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp, pc, pstate;
    unsigned char reserved[4096] __attribute__((aligned(16)));
};
struct ucontext {
    uint64_t flags, link;
    uint64_t ss_sp;
    int32_t ss_flags, ss_pad;
    uint64_t ss_size;
    uint64_t sigmask;
    unsigned char unused[120];
    struct sigcontext mc __attribute__((aligned(16)));
};
struct rt_sigframe {
    unsigned char info[128];
    struct ucontext uc;
    uint64_t fp, lr;                            /* the frame record */
};
#define FPSIMD_MAGIC 0x46508001u
struct fpsimd_context {
    uint32_t magic, size;
    uint32_t fpsr, fpcr;
    unsigned char vregs[512] __attribute__((aligned(16)));
};

static struct rt_sigframe sf;                   /* built here, then copied to the stack */
static unsigned char fpbuf[520] __attribute__((aligned(16)));

int signal_deliver(struct frame *f, int sig, int code, uint64_t addr)
{
    if (sig < 1 || sig > 64)
        return -1;
    struct ksigaction *a = &actions[sig];
    if (a->handler == 0 || (a->handler == SIG_IGN && (sig == 4 || sig == 11 || sig == 7 || sig == 8)))
        return -1;                              /* default action: the caller stops the box */
    if (a->handler == SIG_IGN)
        return 0;
    counts.signals++;

    memset(&sf, 0, sizeof sf);
    int32_t *si = (int32_t *)sf.info;
    si[0] = sig, si[2] = code;
    *(uint64_t *)(sf.info + 16) = addr;         /* si_addr */
    sf.uc.ss_sp = alt.sp, sf.uc.ss_flags = alt.flags, sf.uc.ss_size = alt.size;
    sf.uc.sigmask = MASK;
    struct sigcontext *mc = &sf.uc.mc;
    mc->fault_address = addr;
    memcpy(mc->regs, f->x, sizeof mc->regs);
    mc->sp = f->sp, mc->pc = f->elr, mc->pstate = f->spsr;
    fpsimd_save(fpbuf);
    struct fpsimd_context *fp = (void *)mc->reserved;
    fp->magic = FPSIMD_MAGIC, fp->size = sizeof *fp;
    memcpy(fp->vregs, fpbuf, 512);
    fp->fpsr = ((uint32_t *)fpbuf)[128], fp->fpcr = ((uint32_t *)fpbuf)[129];
    /* The next record stays zero, which ends the list. */

    uint64_t sp = f->sp;
    int on_alt = alt.flags != SS_DISABLE && sp >= alt.sp && sp < alt.sp + alt.size;
    if ((a->flags & SA_ONSTACK) && alt.flags != SS_DISABLE && !on_alt)
        sp = alt.sp + alt.size;
    sp = (sp - sizeof sf) & ~15UL;
    sf.fp = f->x[29], sf.lr = f->elr;
    if (copy_to_box(sp, &sf, sizeof sf))
        return -1;

    f->x[0] = (uint64_t)sig;
    f->x[1] = sp + offsetof(struct rt_sigframe, info);
    f->x[2] = sp + offsetof(struct rt_sigframe, uc);
    f->x[29] = sp + offsetof(struct rt_sigframe, fp);
    f->x[30] = (a->flags & SA_RESTORER) ? a->restorer : 0;
    f->sp = sp;
    f->elr = a->handler;
    if (!vm_is_user())
        f->spsr = (f->spsr & ~0xFUL) | 4;       /* /init's handler runs at EL1t, even if an
                                                   application was running */
    if (!(a->flags & SA_NODEFER))
        MASK |= 1UL << (sig - 1);
    MASK |= a->mask;
    if (a->flags & SA_RESETHAND)
        a->handler = 0;
    return 0;
}

long signal_return(struct frame *f)
{
    uint64_t sp = f->sp;
    if (copy_from_box(&sf, sp, sizeof sf))
        return -14;
    struct sigcontext *mc = &sf.uc.mc;
    memcpy(f->x, mc->regs, sizeof mc->regs);
    f->sp = mc->sp, f->elr = mc->pc;
    f->spsr = (f->spsr & ~0xF0000000UL) | (mc->pstate & 0xF0000000UL);   /* the flags */
    if (!vm_is_user())                          /* choose /init's exception level by where it
                                                   returns to: EL0t for an application's code (in
                                                   the arena), EL1t for the runtime's code (its
                                                   fault landing code, for example) */
        f->spsr = (f->spsr & ~0xFUL) | (vm_apps_el0 && f->elr < (1UL << 32) ? 0 : 4);
    struct fpsimd_context *fp = (void *)mc->reserved;
    if (fp->magic == FPSIMD_MAGIC) {
        memcpy(fpbuf, fp->vregs, 512);
        ((uint32_t *)fpbuf)[128] = fp->fpsr, ((uint32_t *)fpbuf)[129] = fp->fpcr;
        fpsimd_load(fpbuf);
    }
    MASK = sf.uc.sigmask & ~((1UL << 8) | (1UL << 18));   /* SIGKILL and SIGSTOP are never masked */
    return (long)f->x[0];
}

/* ---- the system calls ------------------------------------------------ */

long signal_action(long sig, uint64_t act, uint64_t old)
{
    if (sig < 1 || sig > 64 || sig == 9 || sig == 19)
        return -22;
    if (old && copy_to_box(old, &actions[sig], sizeof actions[sig]))
        return -14;
    if (act && copy_from_box(&actions[sig], act, sizeof actions[sig]))
        return -14;
    return 0;
}

long signal_procmask(long how, uint64_t set, uint64_t old)
{
    uint64_t was = MASK, s;
    if (set) {
        if (copy_from_box(&s, set, 8))
            return -14;
        MASK = how == 0 ? MASK | s : how == 1 ? MASK & ~s : s;
    }
    return old && copy_to_box(old, &was, 8) ? -14 : 0;
}

long signal_altstack(uint64_t ss, uint64_t old)
{
    if (old && copy_to_box(old, &alt, sizeof alt))
        return -14;
    if (ss && copy_from_box(&alt, ss, sizeof alt))
        return -14;
    return 0;
}
