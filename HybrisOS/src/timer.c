/* timer.c: time and power. This file covers the Arm generic timer's
 * counter, the PL031 real-time clock on QEMU's virt machine, and PSCI.
 * PSCI calls are made through HVC, which QEMU answers itself, both under
 * HVF and in its own emulator. */
#include "hal.h"

#define RTC_PA 0x09010000                       /* the PL031's physical address */

uint64_t counter_read(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

uint64_t counter_freq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

uint64_t rtc_seconds(void)
{
    return *(volatile uint32_t *)pa_to_va(RTC_PA);      /* RTCDR, the data register */
}

static void psci(uint64_t fn)
{
    register uint64_t x0 __asm__("x0") = fn;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "x1", "x2", "x3", "memory");
}

void psci_off(void)
{
    psci(0x84000008);                           /* SYSTEM_OFF */
    for (;;)
        __asm__ volatile("wfi");
}

void psci_reset(void)
{
    psci(0x84000009);                           /* SYSTEM_RESET */
    for (;;)
        __asm__ volatile("wfi");
}

/* ---- the GICv3 and the tick ---------------------------------------------
 *
 * QEMU's virt machine has a GICv3 interrupt controller. Its distributor is
 * at &08000000, the first core's redistributor is at &080A0000 (with that
 * core's SGI and PPI frame &10000 above it), and the CPU interface is in
 * system registers. The HAL uses two interrupts of its own. The virtual
 * timer's interrupt, PPI 27, gives a tick of HZ times a second, which wakes
 * sleeping threads and lets the scheduler preempt a thread that is
 * computing. Software-generated interrupt 0 (SGI 0) is the one a core sends
 * to another to wake it when there is a thread for it to run (smp.c). */

#define GICD_PA  0x08000000UL
#define GICR_PA  0x080A0000UL
#define TIMER_INTID 27

void gic_init(void)
{
    /* Set up the distributor with affinity routing and non-secure group 1 */
    mmio_w32(GICD_PA + 0x0, (1u << 4) | (1u << 1));            /* GICD_CTLR: ARE_NS, EnableGrp1NS */
    gic_init_cpu(0);
}

/* Set up the parts of the GIC that belong to one core: its redistributor
 * (each core's is &20000 from the next) and its CPU interface */
void gic_init_cpu(unsigned id)
{
    uint64_t gicr = GICR_PA + (uint64_t)id * 0x20000;
    /* Wake the redistributor */
    uint64_t waker = gicr + 0x14;
    mmio_w32(waker, mmio_r32(waker) & ~(1u << 1));              /* clear ProcessorSleep */
    while (mmio_r32(waker) & (1u << 2))                         /* until ChildrenAsleep clears */
        ;
    uint64_t sgi = gicr + 0x10000;
    mmio_w32(sgi + 0x80, mmio_r32(sgi + 0x80) | 1u);                    /* put SGI 0 in group 1 */
    mmio_w32(sgi + 0x100, 1u);                                          /* and enable it */
    mmio_w32(sgi + 0x80, mmio_r32(sgi + 0x80) | 1u << TIMER_INTID);      /* IGROUPR0: group 1 */
    uint64_t pri = sgi + 0x400 + (TIMER_INTID & ~3u);
    mmio_w32(pri, mmio_r32(pri) & ~(0xFFu << (8 * (TIMER_INTID % 4))));  /* timer at priority 0 */
    mmio_w32(sgi + 0x100, 1u << TIMER_INTID);                   /* ISENABLER0: enable the timer */
    /* Let EL0 read the virtual counter (CNTKCTL_EL1.EL0VCTEN), which the
     * vDSO's clock uses */
    uint64_t k;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(k));
    __asm__ volatile("msr cntkctl_el1, %0\n isb" :: "r"(k | 2));
    /* Set up the CPU interface */
    __asm__ volatile(
        "msr S3_0_C12_C12_5, %0\n isb\n"                        /* ICC_SRE_EL1: enable SRE */
        "msr S3_0_C4_C6_0, %1\n"                                /* ICC_PMR_EL1: all priorities */
        "msr S3_0_C12_C12_3, xzr\n"                             /* ICC_BPR1_EL1 = 0 */
        "msr S3_0_C12_C12_7, %2\n isb"                          /* ICC_IGRPEN1_EL1: group 1 on */
        :: "r"(7UL), "r"(0xFFUL), "r"(1UL));
}

/* Stop the tick on a core that is waiting with nothing to run. This is
 * never used on the boot core. */
void tick_stop(void)
{
    __asm__ volatile("msr cntv_ctl_el0, xzr\n isb");
}

void tick_arm(void)
{
    uint64_t tval = counter_freq() / HZ;
    __asm__ volatile("msr cntv_tval_el0, %0\n msr cntv_ctl_el0, %1\n isb" :: "r"(tval), "r"(1UL));
}

/* Acknowledge the pending interrupt and return its INTID, or 1023 if none
 * is pending */
unsigned irq_ack(void)
{
    uint64_t id;
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(id));      /* ICC_IAR1_EL1 */
    return (unsigned)(id & 0xFFFFFF);
}

void irq_end(unsigned id)
{
    __asm__ volatile("msr S3_0_C12_C12_1, %0\n isb" :: "r"((uint64_t)id));   /* ICC_EOIR1_EL1 */
}
