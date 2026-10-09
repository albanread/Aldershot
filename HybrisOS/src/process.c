/* process.c: the box's processes, which are /init and the programs it
 * starts.
 *
 * /init is process 1. The box starts a program in the ways that Linux
 * programs do. One way is musl's posix_spawn, which is a clone with CLONE_VM
 * and CLONE_VFORK: the child runs in the parent's memory, and the parent
 * waits, until the child calls execve. The other way is fork and execve
 * (pty.c). Each process here has its own address space (vm.c's struct mm,
 * with its own page tables). It needs one because clang is built to load
 * at a fixed address, which lies inside the arena in /init's address
 * space. Each process also has its own descriptor table, its own signal
 * actions, and its threads. execve loads a static ELF program, from the
 * RAM file system or the share, into a new address space. When a process
 * ends, it leaves its status for wait4.
 *
 * The HAL answers Linux's process calls for a program: getpid, getppid,
 * wait4 and kill. It does not yet do fork's copy of memory (H6c), and it
 * does not support sessions and process groups beyond answering yes to
 * requests for them. */
#include "hal.h"

#define ECHILD 10
#define ESRCH  3
#define ENOEXEC 8
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define E2BIG  7
#define EAGAIN 11

#define PROCS 64
static struct process procs[PROCS];

int proc_count(void)                            /* used by stats.c */
{
    int n = 0;
    for (int i = 0; i < PROCS; i++)
        n += procs[i].used;
    return n;
}

struct process *proc_by_pid(int pid)
{
    for (int i = 0; i < PROCS; i++)
        if (procs[i].used && procs[i].pid == pid)
            return &procs[i];
    return 0;
}

void proc_init(void)
{
    struct process *p = &procs[0];
    memset(p, 0, sizeof *p);
    p->used = 1, p->pid = 1, p->ppid = 0, p->threads = 1;
    p->umask = 022;
    memcpy(p->cwd, "/", 2);
    p->mm = vm_current();
    vm_hold(p->mm);                             /* the process's reference; the core has one too */
    p->fdt = fdt_new(0);
    if (!p->fdt)
        panic("no memory for /init's descriptors");
    memcpy(p->exe, "/init", 6);
    thread_current()->proc = p;
}

/* Makes a child of parent. The child either shares the parent's address
 * space (for a vfork) or has none yet. It gets copies of the parent's
 * descriptors and signal actions. The HAL gives the child its pid when it
 * makes the child's first thread; until then the pid is 0, meaning "not
 * yet". */
struct process *proc_new(struct process *parent, int share_mm)
{
    for (int i = 0; i < PROCS; i++) {
        struct process *p = &procs[i];
        if (p->used)
            continue;
        memset(p, 0, sizeof *p);
        p->fdt = fdt_new(parent->fdt);
        if (!p->fdt)
            return 0;
        p->used = 1;
        p->ppid = parent->pid;
        if (share_mm) {
            p->mm = parent->mm;
            vm_hold(p->mm);
        }
        memcpy(p->act, parent->act, sizeof p->act);
        p->umask = parent->umask;
        memcpy(p->uid, parent->uid, sizeof p->uid);
        memcpy(p->gid, parent->gid, sizeof p->gid);
        memcpy(p->cwd, parent->cwd, sizeof p->cwd);
        memcpy(p->exe, parent->exe, sizeof p->exe);
        return p;
    }
    return 0;
}

/* ---- the end ------------------------------------------------------------- */

/* Ends the process with status, in the form wait4 returns. It ends the
 * process's threads, closes its descriptors, frees its memory and tells
 * its parent. */
void proc_exit(struct process *p, int status)
{
    if (p->pid == 1) {
        kprintf("HAL: /init ended, status %d\n", status >> 8);
        psci_off();
    }
    thread_kill_process(p);                     /* every thread of the process except the caller */
    fdt_close_all(p->fdt);
    fdt_free(p->fdt);
    p->fdt = 0;
    struct mm *m = p->mm;
    p->mm = 0;
    if (m) {
        /* The caller is running in it, so move to /init's address space meanwhile. */
        if (vm_current() == m)
            vm_enter(procs[0].mm);
        vm_drop(m);
    }
    p->zombie = 1;
    p->status = status;
    if (p->vfork_tid) {                         /* resume a vfork's parent with the child's pid */
        struct thread *t = thread_by_tid(p->vfork_tid);
        if (t)
            thread_wake(t, p->pid);
        p->vfork_tid = 0;
    }
    struct process *parent = proc_by_pid(p->ppid);
    if (parent)
        futex_wake((uint64_t)parent, 1L << 30); /* wakes a waiting wait4 */
    /* The process's children now belong to /init, as Linux gives them to
     * process 1. */
    for (int i = 0; i < PROCS; i++)
        if (procs[i].used && procs[i].ppid == p->pid)
            procs[i].ppid = 1;
}

/* Called when one of p's threads has ended (exit). When the last thread
 * ends, p ends with status 0. */
void proc_thread_gone(struct frame *f, struct thread *t)
{
    (void)f;
    struct process *p = t->proc;
    if (--p->threads == 0 && !p->zombie)
        proc_exit(p, 0);
}

long proc_wait(struct frame *f, long pid, uint64_t status, long options)
{
    struct process *me = thread_current()->proc;
    int any = 0;
    for (int i = 0; i < PROCS; i++) {
        struct process *c = &procs[i];
        if (!c->used || c->ppid != me->pid || c == me || !c->pid)
            continue;
        if (pid > 0 && c->pid != pid)
            continue;
        any = 1;
        if (!c->zombie)
            continue;
        int32_t st = c->status;
        if (status && copy_to_box(status, &st, 4))
            return -EFAULT;
        long r = c->pid;
        c->used = 0;
        return r;
    }
    if (!any)
        return -ECHILD;
    if (options & 1)                            /* WNOHANG */
        return 0;
    thread_block_restart(f, (uint64_t)me, 0);
    return SWITCHED;
}

/* Called at each tick. It sends SIGALRM to each process whose alarm
 * (setitimer's ITIMER_REAL) is due. */
void proc_alarms(void)
{
    uint64_t now = counter_read();
    for (int i = 0; i < PROCS; i++) {
        struct process *p = &procs[i];
        if (!p->used || p->zombie || !p->alarm_at || (int64_t)(now - p->alarm_at) < 0)
            continue;
        p->alarm_at = p->alarm_every ? now + p->alarm_every : 0;
        proc_kill(p->pid, 14);
    }
}

/* kill: a signal whose effect is to end a process ends it at once, for any
 * process other than /init. The HAL makes other signals pending on the
 * process. */
long proc_kill(long pid, long sig)
{
    struct process *p = proc_by_pid((int)pid);
    if (!p || p->zombie)
        return -ESRCH;
    if (sig == 0)
        return 0;
    if (sig < 1 || sig > 64)
        return -EINVAL;
    uint64_t h = p->act[sig].handler;
    int ends = sig == 9 || (h == 0 && sig != 17 && sig != 23 && sig != 28 && sig != 18 && sig != 20);
    if (h == 1 && sig != 9)                     /* SIG_IGN */
        return 0;
    if (ends && p->pid != 1) {
        proc_exit(p, sig);                      /* the status says the signal sig killed it */
        return 0;
    }
    thread_signal_process(p, (int)sig);
    return 0;
}

/* ---- execve -------------------------------------------------------------- */

struct ehdr {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PIE_BASE    0x1000000000UL              /* where a PIE is loaded, as for /init */
#define STACK_TOP   0x2000000000UL
#define STACK_SIZE  (8UL << 20)

/* Copies the strings of argv or envp from the old address space into strs. */
static char strs[256 * 1024];
static long take_vec(uint64_t vec, char **out, int max, size_t *used)
{
    int n = 0;
    for (; vec; n++) {
        uint64_t p;
        if (copy_from_box(&p, vec + (uint64_t)n * 8, 8))
            return -EFAULT;
        if (!p)
            break;
        if (n == max)
            return -E2BIG;
        long l = strlen_box(p, sizeof strs - *used - 1);
        if (l < 0)
            return -EFAULT;
        if (*used + (size_t)l + 1 >= sizeof strs)
            return -E2BIG;
        copy_from_box(strs + *used, p, (size_t)l + 1);
        out[n] = strs + *used;
        *used += (size_t)l + 1;
    }
    return n;
}

static char *argv_s[2048], *envp_s[2048];

long proc_execve(struct frame *f, uint64_t path_va, uint64_t argv_va, uint64_t envp_va)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    char path[512], canon[1024];
    long n = strlen_box(path_va, sizeof path - 1);
    if (n < 0)
        return -EFAULT;
    copy_from_box(path, path_va, (size_t)n + 1);
    long e = vfs_resolve(path, canon, 1);
    if (e)
        return e;
    long h = vfs_open(canon, 0, 0);
    if (h < 0)
        return h;
    struct ehdr eh;
    static struct phdr ph[32];
    if (vfs_pread_hal(h, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.ident, "\177ELF", 4) ||
        eh.ident[4] != 2 || eh.machine != 183 || (eh.type != 2 && eh.type != 3) || eh.phnum > 32 ||
        vfs_pread_hal(h, ph, (uint64_t)eh.phnum * sizeof ph[0], eh.phoff) != (long)(eh.phnum * sizeof ph[0])) {
        vfs_close(h);
        return -ENOEXEC;
    }
    for (int i = 0; i < eh.phnum; i++)
        if (ph[i].type == 3) {                  /* PT_INTERP: dynamically linked */
            vfs_close(h);
            return -ENOEXEC;
        }
    size_t used = 0;
    long argc = take_vec(argv_va, argv_s, 2047, &used);
    long envc = argc < 0 ? 0 : take_vec(envp_va, envp_s, 2047, &used);
    if (argc < 0 || envc < 0) {
        vfs_close(h);
        return argc < 0 ? argc : envc;
    }

    /* Make the new address space and load the program into it. */
    struct mm *old = p->mm, *m = vm_new();
    if (!m) {
        vfs_close(h);
        return -ENOMEM;
    }
    vm_switch(m);
    vm_set_user();                              /* a program runs at EL0, so its pages are EL0's */
    uint64_t base = eh.type == 3 ? PIE_BASE : 0, top = 0, phdr_va = 0;
    for (int i = 0; i < eh.phnum && !e; i++) {
        if (ph[i].type != 1)                    /* PT_LOAD */
            continue;
        uint64_t s = (base + ph[i].vaddr) & ~PAGE_MASK;
        uint64_t end = (base + ph[i].vaddr + ph[i].memsz + PAGE_MASK) & ~PAGE_MASK;
        /* MAP_FIXED | MAP_ANON, readable and writable while the HAL fills it */
        if (vm_mmap(s, end - s, 3, 0x10 | 0x20, -1, 0) < 0 ||
            vfs_read(h, base + ph[i].vaddr, ph[i].filesz, (int64_t)ph[i].offset) != (long)ph[i].filesz) {
            e = -ENOEXEC;
            break;
        }
        int prot = (ph[i].flags & 4 ? 1 : 0) | (ph[i].flags & 2 ? 2 : 0) | (ph[i].flags & 1 ? 4 : 0);
        vm_mprotect(s, end - s, prot);
        if (ph[i].offset == 0)
            phdr_va = base + ph[i].vaddr + eh.phoff;
        if (end > top)
            top = end;
    }
    vfs_close(h);
    if (!e && vm_mmap(STACK_TOP - STACK_SIZE, STACK_SIZE, 3, 0x10 | 0x20, -1, 0) < 0)
        e = -ENOMEM;
    if (e) {
        vm_switch(old);
        vm_drop(m);
        return e;
    }
    vm_switch(old);
    vm_enter(m);                                /* this core's reference moves to the new space */
    vm_set_brk_base(top + (1UL << 20));

    /* Build the stack: the strings, then argc, argv, envp and the auxiliary
     * vector. */
    uint64_t sp = STACK_TOP;
    static uint64_t av[2048], ev[2048];
    for (long i = 0; i < argc; i++) {
        size_t l = strlen(argv_s[i]) + 1;
        sp -= l;
        copy_to_box(sp, argv_s[i], l);
        av[i] = sp;
    }
    for (long i = 0; i < envc; i++) {
        size_t l = strlen(envp_s[i]) + 1;
        sp -= l;
        copy_to_box(sp, envp_s[i], l);
        ev[i] = sp;
    }
    sp -= 8, copy_to_box(sp, "aarch64", 8);
    uint64_t a_plat = sp;
    uint64_t rnd[2] = { counter_read() * 0x9E3779B97F4A7C15UL, hal_random() };
    sp -= 16, copy_to_box(sp, rnd, 16);
    uint64_t a_rand = sp;
    sp &= ~15UL;
    uint64_t aux[] = { 3, phdr_va, 4, sizeof(struct phdr), 5, eh.phnum, 6, PAGE_SIZE, 7, 0, 8, 0,
                       9, base + eh.entry, 11, 0, 12, 0, 13, 0, 14, 0, 15, a_plat, 16, 3, 17, 100,
                       23, 0, 25, a_rand, 31, argc ? av[0] : 0, 33, vm_vdso_map(), 0, 0 };
    uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + sizeof aux / 8;
    sp -= words * 8;
    sp &= ~15UL;
    uint64_t at = sp, w = (uint64_t)argc;
    copy_to_box(at, &w, 8), at += 8;
    copy_to_box(at, av, (uint64_t)argc * 8), at += (uint64_t)argc * 8;
    w = 0, copy_to_box(at, &w, 8), at += 8;
    copy_to_box(at, ev, (uint64_t)envc * 8), at += (uint64_t)envc * 8;
    copy_to_box(at, &w, 8), at += 8;
    copy_to_box(at, aux, sizeof aux);

    /* From here on the process is the new program. */
    p->mm = m;
    vm_drop(old);                               /* a vfork's parent still holds the old space */
    fdt_exec(p->fdt);
    /* Handlers revert to the default; ignored signals stay ignored. */
    for (int i = 1; i < 65; i++)
        if (p->act[i].handler > 1)
            memset(&p->act[i], 0, sizeof p->act[i]);
    size_t pl = strlen(canon);
    memcpy(p->exe, canon, pl < sizeof p->exe ? pl + 1 : sizeof p->exe);
    p->exe[sizeof p->exe - 1] = 0;
    if (p->vfork_tid) {
        struct thread *pt = thread_by_tid(p->vfork_tid);
        if (pt)
            thread_wake(pt, p->pid);
        p->vfork_tid = 0;
    }
    memset(f, 0, sizeof *f);
    f->elr = base + eh.entry;
    f->sp = sp;
    f->spsr = 0x340;                            /* EL0t, interrupts on: cannot touch the HAL */
    thread_exec_reset(t);
    return 0;
}
