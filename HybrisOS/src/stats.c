/* stats.c: statistics on what the HAL has done, for the box to display
 * (HAL_SYS_STATS).
 *
 * The HAL divides each core's time into four parts: running the box (its
 * programs and /init); idle (in WFI, with nothing to run); in the HAL with
 * its lock held (handling a call, an interrupt or a fault, and also the
 * share's file I/O and the screen's flushes, because the HAL waits for a
 * virtio request there); and waiting for the lock.  The code that takes and
 * releases the lock (smp.c) and the wait in pick() (thread.c) accumulate the
 * last three.  The time spent running the box is whatever remains.  Alongside
 * these are the counters the HAL keeps as it runs (struct hal_counts).
 *
 * The box reads the block twice and computes rates from the differences.
 * DeskMeter's HAL window does this once a second.  Linux has no such call
 * (it returns -ENOSYS), so the box shows the window only when the HAL
 * answers the call. */
#include "hal.h"

struct hal_counts counts;

int proc_count(void);                           /* process.c */
int thread_count(void);                         /* thread.c */

static uint64_t ns(uint64_t ticks, uint64_t freq)
{
    return ticks / freq * 1000000000UL + ticks % freq * 1000000000UL / freq;
}

long stats_read(uint64_t buf, uint64_t size)
{
    static struct hal_stats s;
    uint64_t freq = counter_freq();
    memset(&s, 0, sizeof s);
    s.version = 1;
    s.ncpu = smp_count();
    s.now_ns = ns(counter_read(), freq);
    s.hz = HZ;
    s.mem_pages = pages_total();
    s.mem_free_pages = pages_free();
    s.processes = (uint64_t)proc_count();
    s.threads = (uint64_t)thread_count();
    s.syscalls = counts.syscalls, s.irqs = counts.irqs, s.faults = counts.faults, s.signals = counts.signals;
    s.net_rx_packets = counts.net_rx_packets, s.net_rx_bytes = counts.net_rx_bytes;
    s.net_tx_packets = counts.net_tx_packets, s.net_tx_bytes = counts.net_tx_bytes;
    s.share_calls = counts.share_calls, s.share_bytes = counts.share_bytes;
    s.screen_flushes = counts.screen_flushes, s.screen_bytes = counts.screen_bytes;
    for (unsigned i = 0; i < s.ncpu && i < HAL_STATS_CPUS; i++) {
        struct cpu *c = &cpus[i];
        uint64_t hal = c->t_hal;
        if (c == this_cpu())
            hal += counter_read() - c->since;   /* include the time of this call so far */
        uint64_t idle = c->t_idle, slept = c->slept;
        if (c != this_cpu() && slept)
            idle += counter_read() - slept;     /* idle now, perhaps for seconds (no tick) */
        s.cpu[i].idle_ns = ns(idle, freq);
        s.cpu[i].hal_ns = ns(hal, freq);
        s.cpu[i].spin_ns = ns(c->t_spin, freq);
        s.cpu[i].wakes = c->wakes, s.cpu[i].switches = c->switches, s.cpu[i].ticks = c->ticks;
    }
    uint64_t n = size < sizeof s ? size : sizeof s;
    if (copy_to_box(buf, &s, n))
        return -14;                             /* EFAULT */
    return (long)sizeof s;
}
