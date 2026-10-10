/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* task.c -- tasks, and the baton (task.h).
 *
 * The baton is a flag for each task and one mutex. A task waits for its own
 * flag, and whoever switches to it sets the flag and wakes it. The switch
 * itself runs on the task giving the baton away, holding the personality
 * lock. It saves that task's current program and puts the next one's in
 * place: mappings, pointers and zero page. So when the next task's thread
 * wakes, everything is already as it left it. Then the giving task
 * releases the lock and waits for its own flag.
 *
 * Every task's thread runs on one CPU, the one /init started on. Only the
 * task holding the baton runs, so a second CPU would add nothing but the
 * cost of waking a thread there. Under HVF a switch costs 58 us there
 * against 6 us on the same CPU. RISC OS is one processor, and its tasks
 * stay on one.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#ifdef __linux__
#include <sched.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/dynarea.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/task.h"
#include "rosgd/vdu.h"

struct ros_task {
    uint32_t id;                    /* the runtime's number for it: 0 is /init */
    uint32_t domain;                /* DomainId while it runs. It is id, unless a native
                                       Wimp gives its own (ros_task_set_domain) */
    struct ros_slot slot;           /* application space. Size 0 means none */
    uint32_t apps;                  /* applications running in it, nested */
    int sized;                      /* its own slot was made a size on purpose (Wimp_SlotSize's
                                       ros_task_resize_space) since its last application */
    struct ros_slot svc;            /* its SVC stack */
    /* the current program, while another task runs */
    uint32_t svc_sp;
    uint32_t call_depth;
    struct ros_handler *handlers;
    struct ros_fp fp;
    struct ros_environment env;     /* its handlers, from zero page */
    /* the baton */
    pthread_t thread;
    pthread_cond_t cv;
    int go;
    int ended;
    struct ros_task *creator;
    void (*entry)(void *);
    void *arg;
    /* the user context it waits to go back to: the block its registers
     * were dumped into when it last left (callback.c) */
    uint32_t user_block;
    /* The task whose end handed it the baton last, if one did. It is set by
     * the end and cleared by a switch (ros_task_take_ended_space). */
    struct ros_task *ended_by;
    int program_ended;                  /* ended by the default exit/error handler */
    uint32_t took_space;                /* the ended task's space, taken at the desktop's
                                           end. It holds the size until the application's end asks */
    struct ros_task *next_task;         /* every task there is */
    /* Its x32 application, whichever thread asks (capp.c). install() moves
     * none of it. */
    struct ros_capp_task capp;
    /* Its virtual display and real depth. install() makes the VDU context
     * they give live. */
    struct vdisplay *vdisplay;
    unsigned vdu_real;
};

static pthread_mutex_t baton_mu = PTHREAD_MUTEX_INITIALIZER;

#ifdef __linux__
static cpu_set_t task_cpu;

static void pin(pthread_t t)
{
    pthread_setaffinity_np(t, sizeof task_cpu, &task_cpu);
}
#else
static void pin(pthread_t t)
{
    (void)t;                        /* the hosted build: the host schedules */
}
#endif

static struct ros_task task0;
static struct ros_task *current;
static struct ros_task *all_tasks = &task0;
static uint32_t next_id = 1;

/* The next application slot's size (*WimpSlot -next). It is also the slot
 * that the current task gets if it has none when a language is entered.
 * RISC OS 5's Wimp starts it at 640K (Options/s/Ursula's DefaultNextSlot),
 * as the memory a task takes from the free pool. A slot here is lazy
 * (arena.h) and costs only the pages its program touches. So every task is
 * given all of application space, 1.5 GB, unless it asks for less. A
 * !Run's *WimpSlot -max or a flex program's Wimp_SlotSize can ask. */
uint32_t ros_slot_next_size = ROS_APP_LIMIT - ROS_APP_BASE;

void ros_task_slot_next(uint32_t size)
{
    if (size)
        ros_slot_next_size = size;
}

/* A language is entered in a task. This gives the memory it runs in.
 * Application space that already has memory, such as a mapped Wimp node,
 * is used as it is. In the desktop the slot is the Wimp's to give
 * (Wimp_SlotSize, as a !Run file's *WimpSlot). It is a node that the Wimp
 * maps in and out as the task comes and goes. Only outside the desktop,
 * where Wimp_SlotSize finds no task, is the task given memory of its own.
 * The Wimp adopts that memory if the program becomes a task
 * (ros_task_wimp_initialised). */
static int has_memory(void)
{
    return ros_ld32(ROS_ZEROPAGE + 0x368u) > ROS_APP_BASE;      /* AplWorkSize */
}

os_error *ros_task_give_slot(void)
{
    struct ros_task *t = ros_task_current();
    if (!t)
        return NULL;
    /* Space of its own that no application is running in is the next
     * application's. That is the case when the last application from the
     * command line has ended. It gets the size that *WimpSlot -next set,
     * not the size the first one was given (#45). The pages are the ended
     * program's and are not kept. This does not apply to a slot that has
     * been sized since then (a desktop task's !Run, or *WimpSlot under a
     * native Wimp). That is the size the program is to have. */
    if (t->slot.size && !t->apps && !t->sized && ros_slot_current == &t->slot &&
        t->slot.size != ros_slot_next_size) {
        if (ros_task_resize_own(ros_slot_next_size) != 0)
            return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "Not enough memory for an application slot");
        ros_env_set_memory_limit(ROS_APP_BASE + t->slot.size);
        ros_env_save(&t->env);
        return NULL;
    }
    if (t->slot.size || has_memory())
        return NULL;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_slot_next_size;
    c.r[1] = 0xFFFFFFFFu;
    ros_swi(&c, XWimp_SlotSize);
    if (has_memory())
        return NULL;
    if (ros_slot_create(&t->slot, ros_slot_next_size) != 0 ||
        ros_slot_map(&t->slot) != 0) {
        t->slot.size = 0;
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "Not enough memory for an application slot");
    }
    ros_env_set_memory_limit(ROS_APP_BASE + t->slot.size);
    ros_env_save(&t->env);
    return NULL;
}

void ros_task_app_enter(void)
{
    if (current)
        current->apps++;
}

/* The last application ended, so its slot is the next one's again (#45).
 * The exception is when an Obey file of the task has lines still to run. A
 * !Run's *WimpSlot is for the program that its last line runs. It outlives
 * the programs that the other lines run (ResFind, say). If those were given
 * the next slot, 1.5 GB, a flex program would find no room to grow
 * (SparkFSApp's "Not enough memory, or not within *desktop world"). */
void ros_task_app_leave(void)
{
    if (current && current->apps && !--current->apps && !ros_obey_lines_left())
        current->sized = 0;
}

/* Makes the application space of a program that has its own size bytes.
 * Such a program was given the space outside the desktop, with no Wimp
 * node mapped. The kernel moves pages between the free pool and
 * application space in the same way. It returns 0, or -1 if there is no
 * space of its own or it cannot be that size. */
int ros_task_resize_space(uint32_t size)
{
    struct ros_task *t = current;
    size = (size + 4095u) & ~4095u;
    if (!t || !t->slot.size || ros_slot_current != &t->slot || size == 0 ||
        size > ROS_APP_LIMIT - ROS_APP_BASE)
        return -1;
    if (ros_slot_resize(&t->slot, size) != 0)
        return -1;
    t->sized = 1;                       /* a !Run's *WimpSlot: the program it runs keeps it */
    ros_env_set_memory_limit(ROS_APP_BASE + t->slot.size);
    ros_env_save(&t->env);
    return 0;
}

uint32_t ros_task_slot_bytes(void)
{
    uint32_t n = 0;
    for (struct ros_task *t = all_tasks; t; t = t->next_task)
        n += ros_slot_used(&t->slot);
    return n;
}

/* Application space changed by OS_ChangeDynamicArea with no Wimp node
 * mapped. The running task's own slot is remapped at its new size. The
 * caller sets AplWorkSize and MemLimit. */
int ros_task_resize_own(uint32_t bytes)
{
    struct ros_task *t = current;
    if (!t || (t->slot.size && ros_slot_current != &t->slot))
        return -1;
    if (bytes == t->slot.size)
        return 0;
    if (t->slot.size && bytes < ros_slot_window_floor(&t->slot))
        return -1;                      /* shared below there (arena.h): kept */
    ros_slot_unmap();
    if (!bytes) {
        ros_slot_destroy(&t->slot);
        t->slot = (struct ros_slot){ -1, 0 };
        return 0;
    }
    if (!t->slot.size) {
        if (ros_slot_create(&t->slot, bytes) != 0) {
            t->slot = (struct ros_slot){ -1, 0 };
            return -1;
        }
    } else if (ros_slot_resize(&t->slot, bytes) != 0) {
        ros_slot_map(&t->slot);
        return -1;
    }
    return ros_slot_map(&t->slot) ? -1 : 0;
}

int ros_task_take_adopted(struct ros_slot *into)
{
    if (!task0.slot.size)
        return 0;
    *into = task0.slot;
    task0.slot = (struct ros_slot){ -1, 0 };
    return 1;
}

const struct ros_slot *ros_task_restore_adopted(const struct ros_slot *memory)
{
    task0.slot = *memory;
    return &task0.slot;
}

void ros_task_wimp_initialised(void)
{
    if (current != &task0 || !task0.slot.size)
        return;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = task0.slot.size;                   /* the current slot, as it is */
    c.r[1] = 0xFFFFFFFFu;                       /* the next: unchanged */
    ros_swi(&c, XWimp_SlotSize);
}

struct ros_task *ros_task_current(void)
{
    return current;
}

uint32_t ros_task_id(const struct ros_task *t)
{
    return t->id;
}

void ros_task_set_domain(struct ros_task *t, uint32_t id)
{
    t->domain = id;
    if (t == current)
        ros_st32(ROS_ZP_DOMAINID, id);
}

uint32_t ros_task_slot_size(const struct ros_task *t)
{
    return t->slot.size;
}

uint32_t ros_task_slot_used(const struct ros_task *t)
{
    return t ? ros_slot_used(&t->slot) : 0;
}

const struct ros_slot *ros_task_page_in(const struct ros_task *t)
{
    const struct ros_slot *was = ros_slot_current;
    if (t && t->slot.size && was != &t->slot)
        ros_slot_map(&t->slot);
    return was;
}

void ros_task_page_back(const struct ros_slot *was)
{
    if (was && ros_slot_current != was)
        ros_slot_map(was);
}

struct vdisplay *ros_task_vdisplay(const struct ros_task *t)
{
    return t ? t->vdisplay : NULL;
}

void ros_task_set_vdisplay(struct ros_task *t, struct vdisplay *d)
{
    if (t)
        t->vdisplay = d;
}

unsigned *ros_task_vdu_real(struct ros_task *t)
{
    return &t->vdu_real;
}

int ros_task_ended(const struct ros_task *t)
{
    return t->ended;
}

struct ros_task *ros_task_next(const struct ros_task *t)
{
    return t ? t->next_task : all_tasks;
}

struct ros_capp_task *ros_task_capp(struct ros_task *t)
{
    return &t->capp;
}

#ifdef __linux__
/* The core the tasks run on.
 *
 * Only the task holding the baton runs, so the whole of RISC OS is one
 * thread on one core. On a machine whose cores differ, it matters which
 * core. An i7-12700 has eight performance cores (sixteen threads) and four
 * efficiency ones, and the desktop on an efficiency core is a third slower
 * for nothing. Linux lists the cores. It uses /sys/devices/cpu_core/cpus
 * on an Intel hybrid, and cpu_capacity where an architecture scales them
 * (big.LITTLE). The first performance core is taken, and the workers have
 * the rest (modules/worker). A machine whose cores are all alike says
 * nothing, and the box stays where it started. */
static int performance_core(void)
{
    unsigned first = 0;
    /* An Intel hybrid lists its performance cores as the cpu_core PMU's.
     * On an i7-12700 this is "0-15", the threads of its eight P-cores,
     * against cpu_atom's "16-19". The cpu/types directory that some kernels
     * have is not on 6.18. */
    static const char *const lists[] = {
        "/sys/devices/cpu_core/cpus",
        "/sys/devices/system/cpu/types/intel_core/cpulist",
    };
    for (unsigned i = 0; i < sizeof lists / sizeof lists[0]; i++) {
        FILE *f = fopen(lists[i], "r");
        if (!f)
            continue;
        int got = fscanf(f, "%u", &first);
        fclose(f);
        if (got == 1)
            return (int)first;
    }
    /* Otherwise use the highest capacity that Linux gives any core, if it
     * gives any. */
    unsigned long best = 0;
    int best_cpu = -1;
    for (unsigned c = 0; c < 256; c++) {
        char path[80];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/cpu_capacity", c);
        FILE *cf = fopen(path, "r");
        if (!cf)
            continue;
        unsigned long cap = 0;
        if (fscanf(cf, "%lu", &cap) == 1 && cap > best)
            best = cap, best_cpu = (int)c;
        fclose(cf);
    }
    return best_cpu;
}
#endif

int ros_tasks_init(void)
{
#ifdef __linux__
    int cpu = performance_core();
    if (cpu < 0)
        cpu = sched_getcpu();
    CPU_ZERO(&task_cpu);
    CPU_SET(cpu < 0 ? 0 : cpu, &task_cpu);
#endif
    pin(pthread_self());
    ros_thread_stack_record();
    task0.id = 0;
    task0.domain = 0;
    task0.slot = (struct ros_slot){ -1, 0 };
    task0.svc = ros_svcstack_initial;
    task0.thread = pthread_self();
    pthread_cond_init(&task0.cv, NULL);
    task0.fp = *ros_fp_current;
    ros_fp_current = &task0.fp;
    /* There is no application space, and the current object is the ROM,
     * not &8000. */
    ros_env_defaults(&task0.env, ROS_APP_BASE, ROS_ROM_BASE);
    ros_env_load(&task0.env);
    current = &task0;
    ros_st32(ROS_ZP_DOMAINID, 0);
    return 0;
}

/* Puts another task's current program in place of the running one's. The
 * lock is held, and the running task is `current`. This makes `to` the
 * current program. With `whole` it moves all of it. Without, it moves only
 * what the thread is. That is the Wimp's switch, where the slot, the
 * handlers and DomainId are the Wimp's to move and it has moved them
 * already. */
static void install(struct ros_task *to, int whole)
{
    struct ros_task *from = current;
    from->svc_sp = ros_svc_sp;
    from->call_depth = ros_call_depth;
    from->handlers = ros_handler_chain(to->handlers);
    if (whole) {
        ros_env_save(&from->env);
        ros_env_load(&to->env);
        if (to->slot.size) {
            int e = ros_slot_map(&to->slot);
            if (e != 0)
                ros_console_printf("rosgd: task %u: cannot map its slot (%u KB, fd %d): %s\n", to->id,
                                   to->slot.size >> 10, to->slot.fd, strerror(-e));
        } else {
            ros_slot_unmap();
        }
    }
    if (ros_slot_map_at(&to->svc, ROS_SVCSTACK_BASE) != 0)
        ros_console_printf("rosgd: task %u: cannot map its SVC stack\n", to->id);
    ros_svc_sp = to->svc_sp;
    ros_call_depth = to->call_depth;
    ros_fp_current = &to->fp;
    if (whole)
        ros_st32(ROS_ZP_DOMAINID, to->domain);
    current = to;
    if (ros_vdu_displays)
        ros_vdu_task_installed();
}

/* Gives the baton to `to`, and waits until it comes back to `self`. */
static void hand_over(struct ros_task *self, struct ros_task *to, int wait)
{
    pthread_mutex_lock(&baton_mu);
    to->go = 1;
    pthread_cond_signal(&to->cv);
    pthread_mutex_unlock(&baton_mu);
    unsigned depth = ros_blocking_begin();          /* the lock, for whoever runs next */
    if (!wait)
        return;
    pthread_mutex_lock(&baton_mu);
    while (!self->go)
        pthread_cond_wait(&self->cv, &baton_mu);
    self->go = 0;
    pthread_mutex_unlock(&baton_mu);
    ros_blocking_end(depth);                        /* the switcher put us back in place */
}

static void switch_to(struct ros_task *to, int whole)
{
    struct ros_task *self = current;
    if (!to || to == self || to->ended)
        return;
    to->ended_by = NULL;
    install(to, whole);
    hand_over(self, to, 1);
}

void ros_task_switch(struct ros_task *to)
{
    switch_to(to, 1);
}

void ros_task_switch_thread(struct ros_task *to)
{
    switch_to(to, 0);
}

/* What ends with a task's thread (ros_task_on_end) */
static void (*end_hooks[4])(struct ros_task *);

void ros_task_on_end(void (*fn)(struct ros_task *))
{
    for (unsigned i = 0; i < sizeof end_hooks / sizeof end_hooks[0]; i++)
        if (!end_hooks[i] || end_hooks[i] == fn) {
            end_hooks[i] = fn;
            return;
        }
}

static void ending(struct ros_task *self)
{
    for (unsigned i = 0; i < sizeof end_hooks / sizeof end_hooks[0] && end_hooks[i]; i++)
        end_hooks[i](self);
}

void ros_task_exit(struct ros_task *next)
{
    struct ros_task *self = current;
    ending(self);
    if (!next || next == self || next->ended)
        next = self->creator ? self->creator : &task0;
    install(next, 1);
    self->ended = 1;
    next->ended_by = self;
    hand_over(self, next, 0);
    pthread_exit(NULL);
}

void ros_task_end_program(void)
{
    current->program_ended = 1;
    ros_task_exit(NULL);
}

int ros_task_woken_by_program_end(void)
{
    return current->ended_by && current->ended_by->program_ended;
}

void ros_task_exit_thread(struct ros_task *next)
{
    struct ros_task *self = current;
    ending(self);
    if (!next || next == self || next->ended)
        next = self->creator ? self->creator : &task0;
    install(next, 0);
    self->ended = 1;
    self->user_block = 0;
    next->ended_by = self;
    hand_over(self, next, 0);
    pthread_exit(NULL);
}

/* The desktop's end (callback.c). The task whose end woke this one had the
 * application space as its own. The Wimp's last Wimp_CloseDown put the
 * free pool back as the space of whichever task's thread ran it (Wimp08s,
 * restorepages). The space goes on as this task's, mapped, as RISC OS's
 * one application space is the command line's when the desktop has gone.
 * A task with a space of its own keeps it, and the ended task's is
 * freed. */
void ros_task_take_ended_space(void)
{
    struct ros_task *t = current, *from = t->ended_by;
    t->ended_by = NULL;
    if (!from || !from->ended || !from->slot.size)
        return;
    if (ros_slot_current == &from->slot)
        ros_slot_unmap();
    if (t->slot.size) {
        ros_slot_destroy(&from->slot);
    } else {
        t->slot = from->slot;
        t->took_space = t->slot.size;
    }
    from->slot = (struct ros_slot){ -1, 0 };
    int e = !ros_slot_current && t->slot.size ? ros_slot_map(&t->slot) : 0;
    if (e != 0)
        ros_console_printf("rosgd: task %u: cannot map its slot (%u KB, fd %d): %s\n", t->id,
                           t->slot.size >> 10, t->slot.fd, strerror(-e));
}

uint32_t ros_task_took_ended_space(void)
{
    uint32_t n = current ? current->took_space : 0;
    if (current)
        current->took_space = 0;
    return n;
}

void ros_task_set_user_block(uint32_t block)
{
    /* A block has one context. A task still waiting on it can never be
     * returned to now, because the Wimp dumps every dead task in one
     * buffer. */
    for (struct ros_task *t = all_tasks; t; t = t->next_task)
        if (t->user_block == block && t != current)
            t->user_block = 0;
    current->user_block = block;
}

uint32_t ros_task_user_block(const struct ros_task *t)
{
    return t->user_block;
}

/* The task whose thread waits to go back to user mode through the block.
 *
 * This is an open problem. It has been reasoned out but not reproduced.
 * A task closed down by another leaves its thread parked here, waiting on
 * its register block, and not ended. This happens with Wimp_CloseDown with
 * its handle, from Service_FilerDying, *RMKill or a reset. Suppose the
 * Wimp then gives a new task a block at the same address. ros_user_return
 * would wake that thread, on the dead task's native stack, instead of
 * starting the new context on a thread of its own. An experiment with
 * FilerDying and a restart did not hit it. A probe that closes a task down
 * from outside and then starts another would. */
struct ros_task *ros_task_waiting_on(uint32_t block)
{
    for (struct ros_task *t = all_tasks; t; t = t->next_task)
        if (t->user_block == block && !t->ended)
            return t;
    return NULL;
}

/* ---- a signal stack per thread (task.h) ------------------------------------ */

/* Each thread has 64K of its own. It is mapped when the thread asks and
 * freed when the thread ends. A thread that has one already keeps it. That
 * applies to /init's main thread (boot/main.c) and to a worker's fallback
 * run on its caller's thread. */
#define SIGNAL_STACK (64u * 1024u)

static pthread_key_t signal_stack_key;
static pthread_once_t signal_stack_once = PTHREAD_ONCE_INIT;

static void signal_stack_free(void *p)
{
    stack_t off = { .ss_flags = SS_DISABLE };
    sigaltstack(&off, NULL);
    munmap(p, SIGNAL_STACK);
}

static void signal_stack_key_init(void)
{
    pthread_key_create(&signal_stack_key, signal_stack_free);
}

void ros_thread_signal_stack(void)
{
    ros_thread_stack_record();
    stack_t now;
    if (sigaltstack(NULL, &now) == 0 && !(now.ss_flags & SS_DISABLE))
        return;
    pthread_once(&signal_stack_once, signal_stack_key_init);
    void *p = mmap(NULL, SIGNAL_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return;
    stack_t ss = { .ss_sp = p, .ss_size = SIGNAL_STACK };
    if (sigaltstack(&ss, NULL) != 0) {
        munmap(p, SIGNAL_STACK);
        return;
    }
    pthread_setspecific(signal_stack_key, p);
}

/* ---- each thread's native stack (task.h) ----------------------------------- */

/* The calling thread's stack: its lowest usable address and its top, both
 * 0 until recorded. Also its last refusal. */
static _Thread_local uintptr_t stack_lo, stack_hi;
static _Thread_local struct ros_stack_refusal refusal;

/* The main thread's stack size when RLIMIT_STACK gives no limit or a larger
 * one. */
#define MAIN_STACK_MOST (8u << 20)

void ros_thread_stack_record(void)
{
    uintptr_t lo = 0, hi = 0;
#if defined(__APPLE__)
    /* The hosted build. macOS knows every thread's stack, the main one's
     * too. */
    pthread_t t = pthread_self();
    hi = (uintptr_t)pthread_get_stackaddr_np(t);
    lo = hi - pthread_get_stacksize_np(t);
#elif defined(__linux__)
    if (getpid() == (pid_t)syscall(SYS_gettid)) {
        /* The main thread's stack is the one that exec made. It grows down
         * from its top, where the program's name is (AT_EXECFN), as far as
         * RLIMIT_STACK lets it. The attributes would say only how far it
         * has grown so far. */
        size_t size = MAIN_STACK_MOST;
        struct rlimit rl;
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
            rl.rlim_cur < size)
            size = (size_t)rl.rlim_cur;
        const char *fn = (const char *)getauxval(AT_EXECFN);
        if (fn) {
            hi = ((uintptr_t)fn + strlen(fn) + 1 + 4095) & ~(uintptr_t)4095;
            lo = hi - size;
        }
    } else {
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            void *addr;
            size_t size;
            if (pthread_attr_getstack(&a, &addr, &size) == 0)
                lo = (uintptr_t)addr, hi = lo + size;
            pthread_attr_destroy(&a);
        }
    }
#endif
    stack_lo = lo, stack_hi = hi;
}

size_t ros_native_stack_left(void)
{
    if (!stack_hi)
        ros_thread_stack_record();
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (!stack_hi || sp <= stack_lo || sp > stack_hi)
        return SIZE_MAX;
    return sp - stack_lo;
}

int ros_stack_room(enum ros_stack_need need)
{
    size_t native = ros_native_stack_left();
    uint32_t svc = ros_svc_sp > ROS_SVCSTACK_BASE ? ros_svc_sp - ROS_SVCSTACK_BASE : 0;
    size_t want_native = need == ROS_STACK_FILE    ? ROS_STACK_FILE_NATIVE
                         : need == ROS_STACK_BASIC ? ROS_STACK_BASIC_NATIVE
                                                   : ROS_STACK_ALIAS_NATIVE;
    uint32_t want_svc = need == ROS_STACK_FILE    ? ROS_STACK_FILE_SVC
                        : need == ROS_STACK_BASIC ? 0     /* BASIC's calls stay off it */
                                                  : ROS_STACK_ALIAS_SVC;
    if (native >= want_native && svc >= want_svc)
        return 1;
    refusal.count++;
    refusal.need = need, refusal.native = native, refusal.svc = svc;
    return 0;
}

const struct ros_stack_refusal *ros_stack_last_refusal(void)
{
    return &refusal;
}

/* ---- a task's own thread -------------------------------------------------- */

static void *task_main(void *arg)
{
    ros_thread_name("task");      /* named for /proc, to show what uses the time */
    struct ros_task *t = arg;
    ros_thread_signal_stack();
    pthread_mutex_lock(&baton_mu);
    while (!t->go)
        pthread_cond_wait(&t->cv, &baton_mu);
    t->go = 0;
    pthread_mutex_unlock(&baton_mu);
    ros_lock();                                     /* the switcher installed us */

    /* An error that nothing in the task catches goes to the task's own
     * error handler. Its default reports the error and ends the task
     * (environment.h). */
    t->entry(t->arg);
    ros_task_exit(t->creator);
}

struct ros_task *ros_task_create(uint32_t slot_size, void (*entry)(void *), void *arg)
{
    struct ros_task *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->slot = (struct ros_slot){ -1, 0 };
    if (slot_size && ros_slot_create(&t->slot, slot_size) != 0) {
        free(t);
        return NULL;
    }
    if (ros_stack_create(&t->svc, ROS_SVCSTACK_SIZE) != 0) {
        ros_slot_destroy(&t->slot);
        free(t);
        return NULL;
    }
    t->id = next_id++;
    t->domain = t->id;
    t->svc_sp = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;
    t->fp = (struct ros_fp){ .fpsr = ROS_FPSR_INITIAL };
    ros_env_defaults(&t->env, ROS_APP_BASE + slot_size, ROS_APP_BASE);
    t->creator = current;
    t->entry = entry;
    t->arg = arg;
    pthread_cond_init(&t->cv, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* The stack holds the native frames of module code, such as the Wimp
     * calling the Filer calling FileSwitch, and anything that recurses. It
     * is 8 MB. It was 1 MB, which was too small. A 64-bit frame is two to
     * four times the ARM one that the code was written for. A call chain
     * that fitted RISC OS's stack can therefore be that much deeper here in
     * bytes. Overflowing a thread stack hits a guard page and aborts with
     * nothing printed. The size costs no memory, because a thread stack is
     * reserved address space that is paged in as it is touched. */
    pthread_attr_setstacksize(&attr, 8u << 20);
    int e = pthread_create(&t->thread, &attr, task_main, t);
    pthread_attr_destroy(&attr);
    if (e) {
        ros_slot_destroy(&t->svc);
        ros_slot_destroy(&t->slot);
        free(t);
        return NULL;
    }
    pin(t->thread);
    t->next_task = all_tasks;
    all_tasks = t;
    return t;
}

void ros_task_destroy(struct ros_task *t)
{
    if (!t || !t->ended)
        return;
    for (struct ros_task **p = &all_tasks; *p; p = &(*p)->next_task)
        if (*p == t) {
            *p = t->next_task;
            break;
        }
    /* The task that this one's end woke may not have run since. It must not
     * look at this task when it does. An example is an SSH session's task,
     * which the session frees as soon as it has ended (#164). */
    for (struct ros_task *o = all_tasks; o; o = o->next_task)
        if (o->ended_by == t)
            o->ended_by = NULL;
    pthread_join(t->thread, NULL);
    ros_slot_destroy(&t->slot);
    ros_slot_destroy(&t->svc);
    pthread_cond_destroy(&t->cv);
    free(t);
}
