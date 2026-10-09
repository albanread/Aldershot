/* thread.c: the box's threads. This file handles clone, futexes, sleeping
 * and the timer tick.
 *
 * The HAL represents each of the box's threads by its saved state. This is
 * the register frame that the exception vectors save, its floating-point
 * and SIMD registers, its thread pointer (TPIDR_EL0, which musl uses for
 * thread-local storage), and its signal mask and alternate signal stack.
 * The HAL does not keep a stack of its own for each thread. A system call
 * that must wait marks its thread as blocked and then switches threads: it
 * saves the frame it entered with and puts another thread's frame in its
 * place, and the restore code in vectors.S then returns to that thread.
 * When the HAL wakes the waiting thread, its saved x0 holds the result of
 * its call.
 *
 * The HAL switches threads when one waits, yields or ends, and also at each
 * timer tick (HZ times a second). This means a task that is computing
 * cannot stop the ticker and the background threads from running. When
 * there is nothing to run, a core waits for an interrupt.
 *
 * When there is more than one core (see smp.c), each core runs its own
 * current thread. Only one core at a time may pick a given thread, and
 * oncpu records which core has it. This code runs with the HAL's lock held,
 * and a core releases the lock while it waits. */
#include "hal.h"

#define THREADS 64
static struct thread threads[THREADS];
#define cur (this_cpu()->cur)                   /* this core's current thread */
static int next_tid = 1;
static uint64_t ticks;

void fpsimd_save(void *buf);
void fpsimd_load(const void *buf);

struct thread *thread_current(void)
{
    return cur;
}

void thread_init(void)
{
    memset(threads, 0, sizeof threads);
    for (int i = 0; i < THREADS; i++)
        threads[i].oncpu = -1;
    cur = &threads[0];
    cur->oncpu = 0;
    cur->state = T_RUN;
    cur->tid = next_tid++;
    cur->alt_flags = 2;                         /* SS_DISABLE */
    proc_init();                                /* /init's process, which owns this thread */
}

static uint64_t tpidr_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

static void tpidr_write(uint64_t v)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(v));
}

/* Copy the running thread's state, from f and the registers, into its record */
static void save_current(const struct frame *f)
{
    if (cur->state == T_FREE)
        return;
    cur->fr = *f;
    fpsimd_save(cur->fp);
    cur->tpidr = tpidr_read();
}

/* Decide whether the thread makes a call again after a signal handler has
 * interrupted it, following Linux's rules. The HAL never restarts ppoll,
 * pselect6 or epoll_pwait. It always restarts nanosleep and clock_nanosleep
 * (Linux uses their restart block). It restarts any other call only if the
 * handler asked for this with SA_RESTART. */
static int recalled(const struct thread *t, uint64_t nr, int sig)
{
    if (nr == 73 || nr == 72 || nr == 22)
        return 0;
    if (nr == 101 || nr == 115)
        return 1;
    return t->proc && (t->proc->act[sig].flags & 0x10000000) != 0;
}

/* Load thread t into f, the frame that the trap returns through. The frame
 * always comes from t's record, even when t is the thread that was already
 * running. This is because a thread that waited may have been woken in the
 * meantime, with its result written into its record. */
static void load(struct frame *f, struct thread *t)
{
    cur = t;
    t->oncpu = this_cpu()->id;
    if (t->proc && t->proc->mm)
        vm_enter(t->proc->mm);                  /* switch to its process's address space */
    *f = t->fr;
    fpsimd_load(t->fp);
    tpidr_write(t->tpidr);
    /* Deliver a signal that was sent to the thread while it was not running.
     * If the thread was in ppoll, the HAL tests the signal against the mask
     * that ppoll set. In that case the handler's frame keeps the mask from before
     * the call, and the handler's return restores it. */
    uint64_t ready = t->pending & ~t->sigmask;
    if (ready) {
        int sig = __builtin_ctzl(ready) + 1;
        t->pending &= ~(1UL << (sig - 1));
        /* The thread was woken to make its call again. If the call is not
         * to be restarted, it ends with EINTR instead, as it does on Linux
         * when a handler runs. */
        if (t->recall && !recalled(t, f->x[8], sig)) {
            f->elr += 4;                        /* step past the svc */
            f->x[0] = (uint64_t)-4;             /* EINTR */
            t->poll_deadline = 0;
            t->sock_deadline = 0;
        }
        t->recall = 0;
        if (t->mask_restore && f->x[0] == (uint64_t)-4) {      /* ppoll ended with EINTR */
            t->sigmask = t->saved_mask;
            t->mask_restore = 0;
        }
        signal_deliver(f, sig, -6, 0);          /* SI_TKILL */
    }
}

/* ---- waking ----------------------------------------------------------- */

static void wake_sleepers(void)
{
    uint64_t now = counter_read();
    for (int i = 0; i < THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state == T_WAIT && t->wake_at && (int64_t)(now - t->wake_at) >= 0)
            thread_wake(t, t->timeout_result);
    }
}

int console_wakeup(void);                       /* in syscall.c: wakes a waiting console reader */

static void tick(void)
{
    tick_arm();
    this_cpu()->ticks++;
    /* Every core's tick preempts its thread, but only core 0 keeps the time */
    if (this_cpu()->id)
        return;
    ticks++;
    proc_alarms();
    net_poll();
    wake_sleepers();
    console_wakeup();
}

/* Handle an interrupt that has been acknowledged: the timer tick, or one
 * from a virtio device. Returns 1 if a thread may have been woken. */
static int irq_dispatch(unsigned id)
{
    int woke = 0;
    if (id < 1020)
        counts.irqs++;
    if (id == TIMER_IRQ) {
        tick();
        woke = 1;
    } else if (id < 16) {                       /* an SGI: another core made a thread runnable */
        woke = 1;
    } else if (id >= VIRTIO_IRQ0 && id < VIRTIO_IRQ0 + 32 &&
               (net_irq(id - VIRTIO_IRQ0) || sound_irq(id - VIRTIO_IRQ0) || virtio_irq(id - VIRTIO_IRQ0))) {
        futex_wake(KEY_POLL, 1L << 30);         /* wake any thread polling the device */
        woke = 1;
    }
    if (id < 1020)
        irq_end(id);
    return woke;
}

/* Whether t may run on this core, according to its affinity */
static int runs_here(const struct thread *t)
{
    return !t->cpus || t->cpus >> this_cpu()->id & 1;
}

/* Find the next thread to run after cur. If there is none, wait until one
 * becomes runnable. */
static struct thread *pick(void)
{
    for (;;) {
        int start = cur ? (int)(cur - threads) : 0;
        for (int n = 1; n <= THREADS; n++) {
            struct thread *t = &threads[(start + n) % THREADS];
            if (t->state == T_RUN && t->oncpu < 0 && !runs_here(t)) {
                smp_wake_idle(t->cpus);         /* it cannot run here: wake a core it may run on */
                continue;
            }
            if (t->state == T_RUN && t->oncpu < 0) {
                /* If another thread is also ready to run, wake an idle core for it */
                for (int k = n + 1; k <= THREADS; k++) {
                    struct thread *u = &threads[(start + k) % THREADS];
                    if (u->state == T_RUN && u->oncpu < 0) {
                        smp_wake_idle(u->cpus);
                        break;
                    }
                }
                return t;
            }
        }
        /* Nothing is runnable, so wait for an interrupt with the lock
         * released. Interrupts stay masked here, but WFI still wakes when
         * one is pending. A core other than the boot core stops its tick
         * while it waits, and is woken by the SGI that gives it a thread
         * (see smp.c). */
        struct cpu *me = this_cpu();
        if (me->id)
            tick_stop();
        me->idle = 1;
        hal_unlock();
        uint64_t slept = me->slept = counter_read();
        __asm__ volatile("wfi");
        me->t_idle += counter_read() - slept;
        me->slept = 0;
        me->wakes++;
        hal_lock();
        if (me->id)
            tick_arm();                         /* so that it can preempt what it runs */
        me->idle = 3;                           /* this core runs whatever the interrupt wakes */
        irq_dispatch(irq_ack());
        me->idle = 0;
    }
}

void schedule(struct frame *f)
{
    save_current(f);
    if (cur)
        cur->oncpu = -1;                        /* any core may now take this thread */
    struct thread *next = pick();
    if (next != cur)
        this_cpu()->switches++;
    load(f, next);
}

/* Load a core's first thread into f (called from smp.c) */
void thread_first(struct frame *f)
{
    cur = 0;
    load(f, pick());
}

/* The number of threads in use */
int thread_count(void)
{
    int n = 0;
    for (int i = 0; i < THREADS; i++)
        n += threads[i].state != T_FREE;
    return n;
}

/* Whether another core has ended the running thread (by a kill) */
int thread_dead(void)
{
    return cur && cur->state == T_FREE;
}

/* The calling thread waits. If the time runs out before anything wakes it,
 * timeout_result becomes its x0. */
void thread_block(struct frame *f, uint64_t futex, uint64_t wake_at, long timeout_result)
{
    cur->state = T_WAIT;
    cur->futex = futex;
    cur->wake_at = wake_at;
    cur->timeout_result = timeout_result;
    schedule(f);
}

void thread_wake(struct thread *t, long result)
{
    if (t->state != T_WAIT)
        return;
    t->state = T_RUN;
    t->futex = 0;
    t->wake_at = 0;
    t->fr.x[0] = t->restart ? t->restart_x0 : (uint64_t)result;
    t->recall = t->restart;
    t->restart = 0;
    smp_wake_idle(t->cpus);
}

/* A signal interrupts a waiting thread, and its call ends with EINTR. If the
 * call was to be made again, this moves its return address forward past the
 * svc once more; otherwise the thread would make the call again with EINTR as its
 * first argument. */
void thread_interrupt(struct thread *t)
{
    if (t->state != T_WAIT)
        return;
    if (t->restart) {
        t->fr.elr += 4;
        t->restart = 0;
    }
    /* The wait is over, so the next call sets its own deadline */
    t->poll_deadline = 0, t->sock_deadline = 0;
    thread_wake(t, -4);                         /* EINTR */
}

/* The calling thread waits for key (or until the time runs out), and then
 * makes its call again. Its svc runs once more, with its arguments as they
 * were. */
void thread_block_restart(struct frame *f, uint64_t key, uint64_t wake_at)
{
    cur->restart = 1;
    cur->restart_x0 = f->x[0];
    f->elr -= 4;                                /* return to the svc */
    thread_block(f, key, wake_at, 0);
}

/* The interrupt vector, entered from the box (kind 1) */
static uint64_t irq_pc, irq_lr, irq_fp;         /* where the tick interrupted the box */

void thread_irq(struct frame *f)
{
    irq_pc = f->elr, irq_lr = f->x[30], irq_fp = f->x[29];
    if (irq_dispatch(irq_ack()))
        schedule(f);                            /* preempt, or run the thread it woke */
}

/* ---- the calls -------------------------------------------------------- */

#define CLONE_VM             0x00000100
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

long thread_clone(struct frame *f)
{
    uint64_t flags = f->x[0], stack = f->x[1], ptid = f->x[2], tls = f->x[3], ctid = f->x[4];
    struct process *np = 0;
    if (!(flags & CLONE_VM)) {                  /* fork: a new process with a copy of this memory */
        np = proc_new(cur->proc, 0);
        if (!np)
            return -11;
        np->mm = vm_fork();
        if (!np->mm) {
            proc_exit(np, 0), np->used = 0;
            return -12;
        }
    } else if (!(flags & CLONE_THREAD)) {       /* a process sharing this memory: vfork, posix_spawn */
        np = proc_new(cur->proc, 1);
        if (!np)
            return -11;
    }
    struct thread *t = 0;
    /* A free slot must also be off every core: a thread that was killed may
     * still be running on another core. */
    for (int i = 0; i < THREADS; i++)
        if (threads[i].state == T_FREE && threads[i].oncpu < 0) {
            t = &threads[i];
            break;
        }
    if (!t) {
        if (np)
            proc_exit(np, 0), np->used = 0;
        return -11;                             /* EAGAIN */
    }
    memset(t, 0, sizeof *t);
    t->oncpu = -1;
    t->tid = next_tid++;
    t->proc = np ? np : cur->proc;
    t->proc->threads++;
    if (np)
        np->pid = t->tid;
    t->fr = *f;
    t->fr.x[0] = 0;                             /* the child's clone returns 0 */
    if (stack)
        t->fr.sp = stack;
    fpsimd_save(t->fp);
    t->tpidr = (flags & CLONE_SETTLS) ? tls : tpidr_read();
    t->sigmask = cur->sigmask;
    t->cpus = cur->cpus;                        /* clone keeps the affinity, as on Linux */
    t->alt_flags = 2;                           /* SS_DISABLE: a new thread has no signal stack */
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_tid = ctid;
    if ((flags & CLONE_PARENT_SETTID) && ptid)
        copy_to_box(ptid, &t->tid, 4);
    if ((flags & CLONE_CHILD_SETTID) && ctid)
        copy_to_box(ctid, &t->tid, 4);
    t->state = T_RUN;
    smp_wake_idle(t->cpus);                     /* wake an idle core for it now */
    if (np && (flags & 0x4000)) {               /* CLONE_VFORK: the parent waits for exec or exit */
        np->vfork_tid = cur->tid;
        thread_block(f, (uint64_t)np, 0, t->tid);
        return SWITCHED;
    }
    return t->tid;
}

/* A key below the HAL's half of the address space is a box address. It
 * belongs to one process, so futex_wake wakes only the waiting threads in
 * that process's address space. */
static int same_space(const struct thread *t, uint64_t key)
{
    return key >= HAL_KOFF || key < 16 || !cur || !t->proc || !cur->proc || t->proc->mm == cur->proc->mm;
}

long futex_wake(uint64_t addr, long n)
{
    long woken = 0;
    for (int i = 0; i < THREADS && woken < n; i++)
        if (threads[i].state == T_WAIT && threads[i].futex == addr && same_space(&threads[i], addr)) {
            thread_wake(&threads[i], 0);
            woken++;
        }
    return woken;
}

/* End every thread of process p except the running one (for exit_group, or
 * a kill) */
void thread_kill_process(struct process *p)
{
    for (int i = 0; i < THREADS; i++) {
        struct thread *t = &threads[i];
        if (t != cur && t->state != T_FREE && t->proc == p)
            t->state = T_FREE;
    }
    p->threads = cur && cur->proc == p ? 1 : 0;
}

/* Send sig to process p. This makes the signal pending on p's first thread,
 * and wakes that thread if it is waiting. */
void thread_signal_process(struct process *p, int sig)
{
    for (int i = 0; i < THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state == T_FREE || t->proc != p)
            continue;
        t->pending |= 1UL << (sig - 1);
        if (t->state == T_WAIT)
            thread_interrupt(t);
        return;
    }
}

/* After execve, make the thread a new program's thread. This clears its
 * registers, and leaves it no TLS, no signal stack, and no tid to clear
 * when it ends. */
void thread_exec_reset(struct thread *t)
{
    memset(t->fp, 0, sizeof t->fp);
    fpsimd_load(t->fp);
    tpidr_write(0);
    t->tpidr = 0;
    t->clear_tid = 0;
    t->alt_flags = 2, t->alt_sp = 0, t->alt_size = 0;
    t->pending = 0;
}

/* The running thread ends with its process (exit_group) */
void thread_end_quiet(struct frame *f)
{
    cur->state = T_FREE;
    schedule(f);
}

/* The calling thread ends (exit). The HAL clears its clear_child_tid word
 * and wakes any thread waiting on it, which is what pthread_join waits for. */
void thread_exit(struct frame *f)
{
    if (cur->clear_tid) {
        uint32_t zero = 0;
        if (!copy_to_box(cur->clear_tid, &zero, 4))
            futex_wake(cur->clear_tid, 1);
    }
    cur->state = T_FREE;
    proc_thread_gone(f, cur);
    int live = 0;
    for (int i = 0; i < THREADS; i++)
        live += threads[i].state != T_FREE;
    if (!live) {
        kprintf("HAL: the box's last thread ended\n");
        psci_off();
    }
    schedule(f);
}

struct thread *thread_by_tid(int tid)
{
    for (int i = 0; i < THREADS; i++)
        if (threads[i].state != T_FREE && threads[i].tid == tid)
            return &threads[i];
    return 0;
}

/* futex_requeue: wake n threads waiting on a, and move up to m more of them
 * to wait on b */
long futex_requeue(uint64_t a, long n, long m, uint64_t b)
{
    long woken = futex_wake(a, n), moved = 0;
    for (int i = 0; i < THREADS && moved < m; i++)
        if (threads[i].state == T_WAIT && threads[i].futex == a) {
            threads[i].futex = b;
            moved++;
        }
    return woken + moved;
}

/* Ctrl-\ on the console lists every thread, to help find a hang */
void thread_dump(void)
{
    static const char *const st[] = {"free", "run", "wait"};
    kprintf("\nHAL: tick %lu, thread %d running at %lx, its last call %lu -> %ld\n", ticks, cur->tid, irq_pc,
            cur->last_nr, (long)cur->last_ret);
    uint64_t fpv = irq_fp, ret;
    kprintf("  lr %lx; frames:", irq_lr);
    for (int i = 0; i < 6 && fpv && !copy_from_box(&ret, fpv + 8, 8); i++) {
        kprintf(" %lx", ret);
        if (copy_from_box(&fpv, fpv, 8))
            break;
    }
    kprintf("\n");
    for (int i = 0; i < THREADS; i++) {
        struct thread *t = &threads[i];
        if (t->state == T_FREE)
            continue;
        /* Only a thread that is not running has a saved frame */
        const struct frame *fr = t == cur && t->state == T_RUN ? 0 : &t->fr;
        kprintf("  tid %d %s key %lx wake %lx restart %d pc %lx x8 %lu pid %d signals %lx mask %lx\n", t->tid,
                st[t->state], t->futex, t->wake_at, t->restart, fr ? fr->elr : 0, fr ? fr->x[8] : 0,
                t->proc ? t->proc->pid : 0, t->pending, t->sigmask);
        if (fr) {
            uint64_t tfp = fr->x[29], tret;
            kprintf("    lr %lx; frames:", fr->x[30]);
            for (int k = 0; k < 20 && tfp && !copy_from_box(&tret, tfp + 8, 8); k++) {
                kprintf(" %lx", tret);
                if (copy_from_box(&tfp, tfp, 8))
                    break;
            }
            kprintf("\n");
        }
    }
}

uint64_t thread_ticks(void)
{
    return ticks;
}
