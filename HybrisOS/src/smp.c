/* smp.c: support for the other cores.
 *
 * The box is a program of many threads: its tasks, its background threads
 * and its Workers. As under Linux, it runs on every core that QEMU gives it
 * (the -smp option). The HAL starts the other cores through PSCI (the
 * CPU_ON call, which QEMU answers). Each core starts with the MMU off at
 * secondary_start in boot.S, which turns the MMU on with the boot core's
 * page tables and calls secondary_main in this file.
 *
 * The HAL's own code is not written to run on two cores at once, and it
 * does not need to be. A core takes a single lock on every entry from the
 * box (in trap.c), and releases it when it resumes the box and while it
 * waits for work. The box's own code, which takes nearly all the time,
 * runs on every core at once.
 *
 * A core with nothing to run waits for an interrupt. When one core makes a
 * thread runnable, it wakes one waiting core with a software interrupt
 * (SGI 0), so that a woken Worker does not wait for the next tick. The boot
 * core's tick keeps the time and wakes sleeping threads. The tick on any
 * other core only preempts, so the core stops its tick while it waits (see
 * thread.c). An idle core in the box therefore costs the Mac nothing, just
 * as an idle core under Linux does. */
#include "hal.h"

struct cpu cpus[NCPU];
static unsigned ncpu = 1;

/* ---- the lock: a ticket lock ------------------------------------------- */

static volatile uint32_t next_ticket, serving;

void hal_lock(void)
{
    uint64_t asked = counter_read();
    uint32_t me = __atomic_fetch_add(&next_ticket, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&serving, __ATOMIC_ACQUIRE) != me)
        __asm__ volatile("yield");
    /* Record the time this core waited for the lock. From now on its time
     * counts as the HAL's (for stats.c). */
    struct cpu *c = this_cpu();
    c->since = counter_read();
    c->t_spin += c->since - asked;
}

void hal_unlock(void)
{
    struct cpu *c = this_cpu();
    c->t_hal += counter_read() - c->since;
    __atomic_store_n(&serving, serving + 1, __ATOMIC_RELEASE);
}

unsigned smp_count(void)
{
    return ncpu;
}

/* Send SGI 0 to one other core that is waiting, and mark that core as woken
 * (idle 2) so that the next thread made runnable wakes a different core.
 * The HAL does not wake every waiting core: each one would leave the
 * hypervisor and queue for the lock, and all but one would find nothing to
 * do. Nor does it send an SGI while this core is handling the interrupt
 * that woke it from waiting (idle 3), because this core looks for a thread
 * next. The HAL wakes only a core that the thread may run on (its
 * affinity). A core's Aff0 value is its number (see psci_cpu_on). */
void smp_wake_idle(uint32_t allowed)
{
    if (!allowed)
        allowed = ~0u;
    /* This core was waiting and is about to look for a thread itself */
    if (this_cpu()->idle == 3 && allowed >> this_cpu()->id & 1)
        return;
    for (unsigned i = 0; i < ncpu; i++)
        if (cpus[i].idle == 1 && &cpus[i] != this_cpu() && allowed >> i & 1) {
            cpus[i].idle = 2;
            /* ICC_SGI1R_EL1: send SGI 0 to the core whose Aff0 is i */
            __asm__ volatile("msr S3_0_C12_C11_5, %0\n isb" :: "r"(1UL << i));
            return;
        }
}

/* ---- starting the cores ------------------------------------------------ */

#define STACK 32768
unsigned char stacks[NCPU][STACK] __attribute__((aligned(16)));    /* boot.S finds each core's own */

void secondary_start(void);                     /* in boot.S */
void resume_frame(struct frame *f, void *top);  /* in vectors.S: copies f to top, then restores it */

static long psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx)
{
    register uint64_t x0 __asm__("x0") = 0xC4000003, x1 __asm__("x1") = mpidr, x2 __asm__("x2") = entry,
                      x3 __asm__("x3") = ctx;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (long)x0;
}

void smp_start(void)
{
    unsigned want = boot.ncpus ? boot.ncpus : 1;
    if (want > NCPU)
        want = NCPU;
    for (unsigned i = 1; i < want; i++) {
        cpus[i].id = (int)i;
        cpus[i].mm = cpus[0].mm;
        cpus[i].root = cpus[0].root;
        long r = psci_cpu_on(i, va_to_pa(secondary_start), i);
        if (r) {
            kprintf("HAL: core %u would not start (%ld)\n", i, r);
            break;
        }
        ncpu = i + 1;
    }
    kprintf("HAL: %u core%s\n", ncpu, ncpu == 1 ? "" : "s");
}

/* A core has started. Set up the HAL's vectors, the core's interrupts and
 * its tick, and then run the box's threads as any core does. */
void secondary_main(unsigned id)
{
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&cpus[id]));
    trap_init();
    mm_activate();                              /* use the boot core's TTBR0 tables */
    gic_init_cpu(id);
    hal_lock();
    vm_hold(cpus[id].mm);                       /* hold the address space the core is in */
    tick_arm();
    static struct frame first[NCPU];
    struct frame *f = &first[id];
    thread_first(f);                            /* load the next thread to run into f */
    hal_unlock();
    resume_frame(f, stacks[id] + STACK);
}
