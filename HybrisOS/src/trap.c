/* trap.c: handles what arrives at the exception vectors. These are system
 * calls, interrupts and page faults, and the faults that move a RISC OS
 * application between EL0 and EL1 through the gate page. A fault that the
 * HAL cannot satisfy becomes a signal to the program. If the program has no
 * handler for it, the HAL reports it and ends the program; if the program is
 * /init, the box stops. */
#include "hal.h"

extern char vectors[];

void trap_init(void)
{
    __asm__ volatile("msr vbar_el1, %0\n isb" :: "r"(vectors));
    __asm__ volatile("msr cntkctl_el1, %0\n isb" :: "r"(3UL));     /* let EL0 read the counters */
}

#define EC(esr)      ((esr) >> 26 & 0x3F)
#define EC_SVC64     0x15
#define EC_IABT_SAME 0x21
#define EC_DABT_SAME 0x25
#define EC_IABT_LOW  0x20
#define EC_DABT_LOW  0x24
#define ISS_WNR      (1u << 6)

static const char *const kinds[] = {
    "EL1t sync", "EL1t IRQ", "EL1t FIQ", "EL1t SError",
    "EL1h sync", "EL1h IRQ", "EL1h FIQ", "EL1h SError",
    "EL0 sync", "EL0 IRQ", "EL0 FIQ", "EL0 SError",
    "AArch32", "AArch32", "AArch32", "AArch32",
};

static void report(struct frame *f, unsigned kind, const char *what)
{
    struct thread *t = this_cpu()->cur;
    kprintf("HAL: %s: %s, ESR %lx (EC %x), FAR %lx, ELR %lx, SP %lx; core %d, thread %d, frame %lx, LR %lx\n",
            kinds[kind & 15], what, f->esr, (unsigned)EC(f->esr), f->far, f->elr, f->sp, this_cpu()->id,
            t ? t->tid : 0, (uint64_t)f, f->x[30]);
}

static void trap_locked(struct frame *f, unsigned kind);

/* ---- RISC OS applications at EL0 -----------------------------------------
 *
 * The box's runtime and an A64X32 application share /init's address space.
 * The arena belongs to EL0, and the runtime's memory above 4 GB belongs to
 * EL1 (see vm.c). The runtime enters application code by jumping to it.
 * That jump faults, because EL1 may not execute memory that EL0 may write,
 * so the HAL lets the code carry on at EL0. The application reaches the
 * runtime only through the gate page (&FEEFF000). A trampoline there,
 * `ldr x16, .+8; br x16`, branches into the runtime. The branch faults at
 * EL0, and the HAL lets it carry on at EL1 if, and only if, the target is
 * one of the gate page's own targets (and x16 shows that the branch came
 * through a trampoline). Any other access the application makes to the
 * runtime's memory faults, and the runtime turns the resulting SIGSEGV into
 * an error for the application. The HAL does not take the lock for either
 * switch. */
static const unsigned char gate_tramp[8] = { 0x50, 0x00, 0x00, 0x58, 0x00, 0x02, 0x1F, 0xD6 };
#define GATE_PAGE 0xFEEFF000UL

static int gate_target(uint64_t target)
{
    if (!lookup_page(GATE_PAGE))
        return 0;
    const unsigned char *p = (const unsigned char *)GATE_PAGE;
    for (unsigned i = 0; i < 256; i++) {
        uint64_t t;
        if (memcmp(p + 16 * i, gate_tramp, 8))
            continue;
        memcpy(&t, p + 16 * i + 8, 8);
        if (t == target)
            return 1;
    }
    return 0;
}

static int el_switch(struct frame *f, unsigned kind)
{
    unsigned ec = EC(f->esr), fsc = (unsigned)f->esr & 0x3F;
    if (kind == 0 && ec == EC_IABT_SAME && f->elr < (1UL << 32) && (fsc & 0x3C) == 0x0C &&
        !vm_is_user()) {
        f->spsr &= ~0xFUL;                      /* enter the application at EL0t */
        return 1;
    }
    if (kind == 8 && ec == EC_IABT_LOW && f->elr >= (1UL << 32) && f->elr == f->x[16] &&
        !vm_is_user() && gate_target(f->elr)) {
        f->spsr = (f->spsr & ~0xFUL) | 4;       /* through the gate into the runtime at EL1t */
        return 1;
    }
    return 0;
}

/* Every exception arrives here. The HAL handles it with its lock held (see
 * smp.c), and releases the lock as it resumes the box. It does not resume a
 * thread that another core has ended (by a kill). */
void trap(struct frame *f, unsigned kind)
{
    /* An exception in the HAL itself, which already holds the lock: report it */
    if (kind >= 4 && kind < 8) {
        trap_locked(f, kind);
        return;
    }
    if (vm_apps_el0 && (kind == 0 || kind == 8) && el_switch(f, kind))
        return;
    hal_lock();
    if (thread_dead() && (kind == 0 || kind == 1)) {
        if (kind == 1)
            irq_end(irq_ack());
        schedule(f);
    } else {
        trap_locked(f, kind);
    }
    hal_unlock();
}

/* A signal that the program does not handle ends the program. As under
 * Linux, a program that the box started ends with that signal. If the
 * program is /init, the box itself ends. */
static void fatal(struct frame *f, unsigned kind, int sig, const char *what)
{
    struct process *p = this_cpu()->cur->proc;
    report(f, kind, what);
    if (p->pid == 1)
        panic("the box faulted, with no handler for signal %d", sig);
    kprintf("HAL: process %d (%s) ended by signal %d\n", p->pid, p->exe, sig);
    proc_exit(p, sig);
    thread_end_quiet(f);
}

static void trap_locked(struct frame *f, unsigned kind)
{
    unsigned ec = EC(f->esr);
    if (kind == 1 || kind == 9) {               /* an interrupt while the box was running */
        thread_irq(f);
        return;
    }
    if (kind == 0 || kind == 8) {               /* a synchronous exception from the box */
        if (ec == EC_SVC64) {
            syscall(f);
            return;
        }
        if (ec == EC_DABT_SAME || ec == EC_IABT_SAME || ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
            int write = (ec == EC_DABT_SAME || ec == EC_DABT_LOW) && (f->esr & ISS_WNR);
            int fetch = ec == EC_IABT_SAME || ec == EC_IABT_LOW;
            counts.faults++;
            if (!(f->far >> 63) && vm_fault(f->far, fetch ? 2 : write, kind == 8) == 0)
                return;                         /* the page is now mapped */
            int mapped = !(f->far >> 63) && lookup_page(f->far & ~PAGE_MASK);
            if (signal_deliver(f, 11, mapped ? 2 : 1, f->far) == 0)    /* SIGSEGV, ACCERR or MAPERR */
                return;
            fatal(f, kind, 11, write ? "a write to memory it may not write" : "a read of memory it may not read");
            return;
        }
        /* An undefined instruction, or an SVE or SME instruction while those
         * are turned off (the HAL does not save their state), raises SIGILL
         * with ILL_ILLOPC. Linux does the same when a process uses a feature
         * that it may not use. */
        if ((ec == 0x00 || ec == 0x19 || ec == 0x1D) && signal_deliver(f, 4, 1, f->elr) == 0)
            return;
        /* A BRK instruction raises SIGTRAP with TRAP_BRKPT, as Linux does for
         * a BRK immediate that it has no handler for. The box uses BRK
         * itself: brk #&5503 is a divide by zero. */
        if (ec == 0x3C && signal_deliver(f, 5, 1, f->elr) == 0)
            return;
        /* A trapped floating-point exception (one enabled in FPCR) raises
         * SIGFPE. As Linux does, the HAL reads the syndrome to find which
         * exception it was, and gives the matching code. */
        if (ec == 0x2C) {
            uint64_t iss = f->esr;
            int code = iss & 2 ? 3 : iss & 1 ? 7 : iss & 4 ? 4 : iss & 8 ? 5 : iss & 16 ? 6 : 14;
            if (signal_deliver(f, 8, code, f->elr) == 0)
                return;
        }
        /* A misaligned PC or SP raises SIGBUS with BUS_ADRALN */
        if ((ec == 0x22 || ec == 0x26) && signal_deliver(f, 7, 1, ec == 0x22 ? f->elr : f->sp) == 0)
            return;
        fatal(f, kind, ec == 0x22 || ec == 0x26 ? 7 : ec == 0x2C ? 8 : ec == 0x3C ? 5 : 4,
              "an exception it does not handle");
        return;
    }
    report(f, kind, "in the HAL");
    panic("stopped");
}
