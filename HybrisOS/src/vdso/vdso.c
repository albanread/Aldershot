/* vdso.c: the HAL's vDSO. It provides clock_gettime without a system call,
 * as Linux's vDSO does, for every program that musl links. musl's
 * __vdsosym finds __kernel_clock_gettime, version LINUX_2.6.39, through
 * AT_SYSINFO_EHDR.
 *
 * The counter is the virtual timer's (CNTVCT_EL0), which EL0 may read
 * because the HAL sets CNTKCTL_EL1.EL0VCTEN on each core. The HAL's clocks
 * count the ticks since boot_counter, and REALTIME adds boot_seconds. This
 * is the same sum that syscall.c's now() makes. The HAL writes those two
 * values once, in the page after this image (vm.c, vdso_init); the image
 * occupies the one page before it. For clocks that the vDSO does not keep,
 * it makes the system call itself. */
#include <stdint.h>

struct vdso_data {
    uint64_t boot_counter, boot_seconds;
};

/* the first byte of this image (lld defines the symbol) */
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

struct ts {
    int64_t sec, nsec;
};

static long syscall2(long nr, long a, long b)
{
    register long x8 __asm__("x8") = nr, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

int __kernel_clock_gettime(int clock, struct ts *t)
{
    switch (clock) {
    /* REALTIME, MONOTONIC (and its RAW and COARSE forms), BOOTTIME */
    case 0: case 1: case 4: case 5: case 6: case 7:
        break;
    default:
        return (int)syscall2(113, clock, (long)t);
    }
    const struct vdso_data *d = (const struct vdso_data *)(__ehdr_start + 4096);
    uint64_t c, f;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(c));
    __asm__("mrs %0, cntfrq_el0" : "=r"(f));
    c -= d->boot_counter;
    uint64_t s = c / f, ns = (c % f) * 1000000000UL / f;
    if (clock == 0 || clock == 5)
        s += d->boot_seconds;
    t->sec = (int64_t)s, t->nsec = (int64_t)ns;
    return 0;
}
