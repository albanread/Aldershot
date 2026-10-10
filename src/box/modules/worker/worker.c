/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* worker.c: Worker runs computation jobs on a pool of Linux threads, on the
 * cores that the tasks do not use (README.md here lists the SWIs).
 *
 * Every Wimp task is a thread pinned to one core, and only the task holding
 * the baton runs (task.h). The pool's threads are not pinned to that core.
 * They never take the personality lock. A pool thread runs a job's kernel
 * over memory that stays mapped whichever task runs, which is the RMA or a
 * dynamic area. The first kernels are native C built into the module. The
 * thread writes the kernel's result words into the job's record in the RMA
 * and then bumps the job's pollword. The Wimp sees the pollword at its next
 * search and gives the task event 13. The task then collects the result.
 *
 * The task side (the SWIs and the service call) runs under the lock, as
 * all module code does. The pool's state is the queue and each job's
 * state. It is under the pool's own mutex. The task side takes the mutex
 * briefly and the workers take it between jobs. A worker never holds it
 * while it computes. A worker touches the arena only at addresses that the
 * task side checked at Submit: the job's record, its buffers and its
 * pollword.
 *
 * Worker_ShareMemory maps part of the caller's slot a second time, at a
 * window in the dynamic-area range (arena.h, ros_slot_window_map, and
 * dynarea.h for the address space). A worker can then write a task's own
 * memory with no copy, whichever task runs. A window's jobs are stopped
 * before the window goes. While a window exists, the slot cannot shrink
 * below it.
 *
 * Worker_LoadKernel loads an application's own kernel. This is A64 or
 * x86-64 code, compiled by roscc (link --kernel) or assembled by BASIC. It
 * is loaded into whole pages of the RMA, which are then made read-only and
 * executable. First kcheck.c checks that the code makes no system calls
 * and does not use system registers, x18, indirect branches or references
 * outside the kernel. A worker calls the kernel as a C function on an arena
 * stack of its own (kernel_call, the machine's code below).
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/dynarea.h"
#include "rosgd/error.h"
#include "rosgd/fault.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vdu.h"
#include "kcheck.h"
#include "worker.h"

#define MAX_THREADS 64
#define MAX_JOBS    65536u

/* Application kernels run only in the box, whose RMA is executable (arena.c,
 * PROT_CODE). A hosted build checks them but refuses to load them. */
#if defined(__linux__) && !defined(ROS_ARENA_HOSTED) && (defined(__aarch64__) || defined(__x86_64__))
#define KERNELS_RUN 1
#else
#define KERNELS_RUN 0
#endif
#if defined(__aarch64__)
#define BOX_MACHINE WK_EM_AARCH64
#else
#define BOX_MACHINE WK_EM_X86_64
#endif
#define KSTACK      (64u << 10)        /* an application kernel's stack, per thread */

/* A job, host side. The handle is its record in the RMA. */
struct job {
    uint32_t handle;
    uint32_t kernel, task, pollword;
    struct ros_task *rt;                /* the task's thread, for its end */
    uint32_t args[WORKER_MAX_ARGS];
    uint32_t entry;                     /* an application kernel's code, else 0 */
    uint32_t buf[5][2];                 /* what it touches: its buffers, its pollword */
    unsigned nbuf;
    int thread;                         /* the pool thread running it (under mu) */
    int state;                          /* WJOB_*, under mu */
    int cancel;                         /* asked to stop: read by the kernel, atomically */
    int collected;                      /* returned by Worker_Completed (task side) */
    uint32_t errnum, erraddr;           /* a failure's, set by its worker */
    int errsig;                         /* a fault's signal */
    struct job *next;                   /* every job, in submission order (task side) */
    struct job *qnext;                  /* the queue (under mu) */
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;   /* a job queued, or stop */
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;   /* a job finished */
static struct job *queue_head, **queue_tail = &queue_head;
static unsigned queued, running, limit, stopping;
static pthread_t threads[MAX_THREADS];
static unsigned nthreads, planned;
static int started;

/* Task side only (under the lock) */
static struct job *jobs, **jobs_tail = &jobs;
static unsigned njobs;

/* A shared window (Worker_ShareMemory). [base, base + len) is the caller's
 * slot, from start, mapped again. Task side only. */
struct window {
    uint32_t base, len, start;
    uint32_t task;
    struct ros_task *rt;
    struct window *next;
};
static struct window *wins;

/* An application kernel (Worker_LoadKernel). Its image is in whole pages of
 * the RMA, read-only and executable. Its number is the image's address.
 * Task side only. */
struct ukernel {
    uint32_t base, size;                /* the pages */
    void *block;                        /* the RMA block that holds them */
    uint32_t entry;
    uint16_t desc[4];                   /* the buffers its arguments name */
    uint32_t task;
    struct ros_task *rt;
    struct ukernel *next;
};
static struct ukernel *ukernels;

/* Each pool thread's arena stack for application kernels, made at the
 * first load. These hold its top and the RMA block. */
static uint32_t kstack_top[MAX_THREADS];
static void *kstack_block[MAX_THREADS];

/* ---- errors ------------------------------------------------------------------------ */

static os_error *err(int which, uint32_t arg)
{
    switch (which) {
    case WE_BADJOB:      return ros_error(WORKER_ERRBASE + WE_BADJOB, "Not a Worker job handle");
    case WE_BADKERNEL:   return ros_error(WORKER_ERRBASE + WE_BADKERNEL, "Unknown Worker kernel %u", arg);
    case WE_BADARGS:     return ros_error(WORKER_ERRBASE + WE_BADARGS, "Bad arguments for the Worker kernel");
    case WE_BADBUFFER:
        if (arg >= ROS_APP_BASE && arg < ROS_APP_LIMIT)
            return ros_error(WORKER_ERRBASE + WE_BADBUFFER,
                             "Worker buffer &%X is application memory: share it with "
                             "Worker_ShareMemory and pass the window's address", arg);
        return ros_error(WORKER_ERRBASE + WE_BADBUFFER,
                         "Worker buffers must be in the RMA, a dynamic area or a "
                         "Worker_ShareMemory window (&%X)", arg);
    case WE_NOTDONE:     return ros_error(WORKER_ERRBASE + WE_NOTDONE, "Worker job not finished");
    case WE_CANCELLED:   return ros_error(WORKER_ERRBASE + WE_CANCELLED, "Worker job cancelled");
    case WE_FAULT:       return ros_error(WORKER_ERRBASE + WE_FAULT,
                                          "Worker job failed: abort on data transfer at &%08X", arg);
    case WE_TOOMANY:     return ros_error(WORKER_ERRBASE + WE_TOOMANY, "Too many Worker jobs");
    case WE_BADRANGE:    return ros_error(WORKER_ERRBASE + WE_BADRANGE,
                                          "Worker_ShareMemory needs a range of the caller's "
                                          "application memory");
    case WE_BADPOLLWORD: return ros_error(WORKER_ERRBASE + WE_BADPOLLWORD,
                                          "A Worker pollword must be a word in the RMA, a dynamic "
                                          "area or a Worker_ShareMemory window");
    case WE_NOTWINDOW:   return ros_error(WORKER_ERRBASE + WE_NOTWINDOW,
                                          "Not a Worker_ShareMemory window");
    case WE_NOSPACE:     return ros_error(WORKER_ERRBASE + WE_NOSPACE,
                                          "No room for a Worker_ShareMemory window");
    case WE_NOTHREADS:   return ros_error(WORKER_ERRBASE + WE_NOTHREADS, "Worker threads cannot be started");
    case WE_NOKERNELS:   return ros_error(WORKER_ERRBASE + WE_NOKERNELS,
                                          "Application kernels run only in the box (this build has no "
                                          "executable arena)");
    default:             return ros_error(WORKER_ERRBASE + WE_BADREASON, "Bad Worker_Info reason");
    }
}

/* Memory a worker may touch: [a, a + len) wholly in the RMA, in one dynamic
 * area or in one shared window. These stay mapped whichever task holds the
 * baton. Application space belongs to the running task alone, so it is
 * refused. A window onto it, from Worker_ShareMemory, is the way to use it. */
static int stays_mapped(uint32_t a, uint32_t len)
{
    if (len == 0 || a < ROS_RMA_BASE || a + len < a)
        return 0;
    return ros_dynarea_contains(a, a + len);
}

/* ---- the kernels ------------------------------------------------------------------- */

/* A built-in kernel. Its arguments are checked at Submit (task side, under
 * the lock). It is run by a worker with no lock held. The run puts the
 * results into r and returns 0, or returns an error number. */
struct kernel {
    uint32_t number;
    const char *name;
    os_error *(*check)(const uint32_t *args, uint32_t nargs);
    uint32_t (*run)(const uint32_t *args, uint32_t *r, const int *cancel);
    /* The buffers it touches, found from checked arguments. It fills in up
     * to four [address, length) pairs and returns how many. */
    unsigned (*buffers)(const uint32_t *args, uint32_t b[4][2]);
};

static double coord(const uint32_t *a, int doubles)
{
    if (doubles) {
        uint64_t bits = (uint64_t)a[0] | (uint64_t)a[1] << 32;
        double d;
        memcpy(&d, &bits, sizeof d);
        return d;
    }
    return (double)(int32_t)a[0] + (double)a[1] * (1.0 / 4294967296.0);
}

static os_error *mandel_check(const uint32_t *a, uint32_t n)
{
    if (n < MR_NARGS || a[MR_WIDTH] == 0 || a[MR_WIDTH] > 16384 || a[MR_MAXITER] == 0 ||
        a[MR_MAXITER] > (1u << 20) || (a[MR_FLAGS] & 3u) == 3 || (a[MR_FLAGS] & ~7u))
        return err(WE_BADARGS, 0);
    if (!stays_mapped(a[MR_BUFFER], a[MR_WIDTH] * 4) || (a[MR_BUFFER] & 3u))
        return err(WE_BADBUFFER, a[MR_BUFFER]);
    if ((a[MR_FLAGS] & 3u) == MR_OUT_PALETTE &&
        (!stays_mapped(a[MR_PALETTE], (a[MR_MAXITER] + 1) * 4) || (a[MR_PALETTE] & 3u)))
        return err(WE_BADBUFFER, a[MR_PALETTE]);
    int doubles = (a[MR_FLAGS] & MR_DOUBLES) != 0;
    double x0 = coord(a + MR_X0, doubles), dx = coord(a + MR_DX, doubles),
           y = coord(a + MR_Y, doubles);
    if (!isfinite(x0) || !isfinite(dx) || !isfinite(y))
        return err(WE_BADARGS, 0);
    return NULL;
}

/* A smooth colour for a smooth iteration count, from a cosine ramp
 * (&00BBGGRR) */
static uint32_t ramp(double nu)
{
    double t = 0.6 + nu * 0.025;
    double r = 0.5 + 0.5 * cos(6.283185307179586 * (t + 0.00));
    double g = 0.5 + 0.5 * cos(6.283185307179586 * (t + 0.10));
    double b = 0.5 + 0.5 * cos(6.283185307179586 * (t + 0.20));
    return (uint32_t)(r * 255.0 + 0.5) | (uint32_t)(g * 255.0 + 0.5) << 8 |
           (uint32_t)(b * 255.0 + 0.5) << 16;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* One row of the Mandelbrot set. The results are R1 and R2, the total
 * iterations (low word, high word), R3, the points inside the set, and R4,
 * the microseconds it took. */
static uint32_t mandel_run(const uint32_t *a, uint32_t *r, const int *cancel)
{
    uint64_t t0 = now_us();
    uint32_t width = a[MR_WIDTH], maxit = a[MR_MAXITER], out = a[MR_FLAGS] & 3u;
    int doubles = (a[MR_FLAGS] & MR_DOUBLES) != 0;
    double x0 = coord(a + MR_X0, doubles), dx = coord(a + MR_DX, doubles),
           ci = coord(a + MR_Y, doubles);
    uint32_t *row = ros_ptr(a[MR_BUFFER]);
    const uint32_t *pal = out == MR_OUT_PALETTE ? ros_ptr(a[MR_PALETTE]) : NULL;
    /* The classic escape radius, |z| > 2, for counts. The smooth ramp goes
     * further, to |z| > 16. */
    double bail = out == MR_OUT_COLOUR ? 256.0 : 4.0;
    uint64_t total = 0;
    uint32_t inside = 0;
    for (uint32_t x = 0; x < width; x++) {
        if ((x & 63u) == 0 && __atomic_load_n(cancel, __ATOMIC_RELAXED))
            return WORKER_ERRBASE + WE_CANCELLED;
        double cr = x0 + dx * x, zr = 0, zi = 0, zr2 = 0, zi2 = 0;
        uint32_t n = 0;
        while (n < maxit && zr2 + zi2 <= bail) {
            zi = 2 * zr * zi + ci;
            zr = zr2 - zi2 + cr;
            zr2 = zr * zr;
            zi2 = zi * zi;
            n++;
        }
        total += n;
        uint32_t v;
        if (n >= maxit) {
            inside++;
            v = out == MR_OUT_COUNTS ? maxit : out == MR_OUT_PALETTE ? pal[maxit] : 0;
        } else if (out == MR_OUT_COUNTS) {
            v = n;
        } else if (out == MR_OUT_PALETTE) {
            v = pal[n];
        } else {
            double nu = n + 1 - log2(log2(sqrt(zr2 + zi2)));
            v = ramp(nu);
        }
        row[x] = v;
    }
    r[0] = (uint32_t)total;
    r[1] = (uint32_t)(total >> 32);
    r[2] = inside;
    r[3] = (uint32_t)(now_us() - t0);
    return 0;
}

static unsigned mandel_buffers(const uint32_t *a, uint32_t b[4][2])
{
    b[0][0] = a[MR_BUFFER], b[0][1] = a[MR_WIDTH] * 4;
    if ((a[MR_FLAGS] & 3u) != MR_OUT_PALETTE)
        return 1;
    b[1][0] = a[MR_PALETTE], b[1][1] = (a[MR_MAXITER] + 1) * 4;
    return 2;
}

static const struct kernel kernels[] = {
    { WORKER_KERNEL_MANDEL_ROW, "mandel_row", mandel_check, mandel_run, mandel_buffers },
};

static const struct kernel *kernel_of(uint32_t number)
{
    for (size_t i = 0; i < sizeof kernels / sizeof kernels[0]; i++)
        if (kernels[i].number == number)
            return &kernels[i];
    return NULL;
}

/* ---- the pool ---------------------------------------------------------------------- */

/* The wake. The Wimp idles in ros_idle when no task has anything to do, and
 * posted work ends the wait. A pollword that a worker bumps is therefore
 * seen at once and not at the next tick. The work itself does nothing. */
static void wake(void *arg, uint32_t info)
{
    (void)arg, (void)info;
}

static void record_status(const struct job *j, uint32_t status)
{
    __atomic_store_n((uint32_t *)ros_ptr(j->handle + WJOB_STATUS), status, __ATOMIC_RELEASE);
}

/* ---- application kernels: the call ---------------------------------------------------- */

/* kernel_call(fn, args, results, cancel, sp) calls fn as a C function
 * (AAPCS64, or SysV, which x32 shares). It passes the three arena addresses
 * args, results and cancel, runs on the arena stack whose top is sp, and
 * returns fn's 32-bit result. The kernel's own conventions are not trusted.
 * Every callee-saved register and the floating point control are saved
 * here and put back. The native stack comes back from a thread-local
 * variable that the kernel cannot reach, because the checker refuses the
 * thread pointer. While the kernel runs, the floating point control has
 * its default value: round to nearest, no traps. */
__attribute__((used)) _Thread_local uint64_t ros_worker_native_sp;
/* Set while a kernel is running on this thread. The stop signal may then
 * take the kernel away. */
static _Thread_local volatile sig_atomic_t in_kernel;

#if KERNELS_RUN && defined(__aarch64__)
uint32_t ros_worker_kernel_call(uint64_t fn, uint64_t args, uint64_t results, uint64_t cancel, uint64_t sp);
__asm__(".text\n"
        ".p2align 2\n"
        ".globl ros_worker_kernel_call\n"
        ".type ros_worker_kernel_call,%function\n"
        "ros_worker_kernel_call:\n"
        "   stp x29, x30, [sp, #-176]!\n"
        "   mov x29, sp\n"
        "   stp x19, x20, [sp, #16]\n"
        "   stp x21, x22, [sp, #32]\n"
        "   stp x23, x24, [sp, #48]\n"
        "   stp x25, x26, [sp, #64]\n"
        "   stp x27, x28, [sp, #80]\n"
        "   stp d8, d9, [sp, #96]\n"
        "   stp d10, d11, [sp, #112]\n"
        "   stp d12, d13, [sp, #128]\n"
        "   stp d14, d15, [sp, #144]\n"
        "   mrs x9, fpcr\n"
        "   str x9, [sp, #160]\n"
        "   mrs x10, tpidr_el0\n"
        "   add x10, x10, #:tprel_hi12:ros_worker_native_sp, lsl #12\n"
        "   add x10, x10, #:tprel_lo12_nc:ros_worker_native_sp\n"
        "   mov x11, sp\n"
        "   str x11, [x10]\n"
        "   msr fpcr, xzr\n"
        "   mov x9, x0\n"
        "   mov x0, x1\n"
        "   mov x1, x2\n"
        "   mov x2, x3\n"
        "   mov sp, x4\n"
        "   isb\n"                     /* code loaded on another core */
        "   blr x9\n"
        "   mrs x10, tpidr_el0\n"
        "   add x10, x10, #:tprel_hi12:ros_worker_native_sp, lsl #12\n"
        "   add x10, x10, #:tprel_lo12_nc:ros_worker_native_sp\n"
        "   ldr x11, [x10]\n"
        "   mov sp, x11\n"
        "   ldr x9, [sp, #160]\n"
        "   msr fpcr, x9\n"
        "   ldp d14, d15, [sp, #144]\n"
        "   ldp d12, d13, [sp, #128]\n"
        "   ldp d10, d11, [sp, #112]\n"
        "   ldp d8, d9, [sp, #96]\n"
        "   ldp x27, x28, [sp, #80]\n"
        "   ldp x25, x26, [sp, #64]\n"
        "   ldp x23, x24, [sp, #48]\n"
        "   ldp x21, x22, [sp, #32]\n"
        "   ldp x19, x20, [sp, #16]\n"
        "   ldp x29, x30, [sp], #176\n"
        "   ret\n"
        ".size ros_worker_kernel_call, .-ros_worker_kernel_call\n");
#define kernel_call ros_worker_kernel_call
#elif KERNELS_RUN && defined(__x86_64__)
uint32_t ros_worker_kernel_call(uint64_t fn, uint64_t args, uint64_t results, uint64_t cancel, uint64_t sp);
__asm__(".text\n"
        ".globl ros_worker_kernel_call\n"
        ".type ros_worker_kernel_call,@function\n"
        "ros_worker_kernel_call:\n"
        "   pushq %rbp\n"
        "   movq %rsp, %rbp\n"
        "   pushq %rbx\n"
        "   pushq %r12\n"
        "   pushq %r13\n"
        "   pushq %r14\n"
        "   pushq %r15\n"
        "   subq $24, %rsp\n"
        "   stmxcsr (%rsp)\n"
        "   fnstcw 4(%rsp)\n"
        "   movq %rsp, %fs:ros_worker_native_sp@tpoff\n"
        "   movl $0x1F80, 8(%rsp)\n"   /* the defaults: round to nearest, all masked */
        "   movw $0x037F, 12(%rsp)\n"
        "   ldmxcsr 8(%rsp)\n"
        "   fldcw 12(%rsp)\n"
        "   movq %rdi, %rax\n"
        "   movq %rsi, %rdi\n"
        "   movq %rdx, %rsi\n"
        "   movq %rcx, %rdx\n"
        "   movq %r8, %rsp\n"
        "   cld\n"
        "   callq *%rax\n"
        "   movq %fs:ros_worker_native_sp@tpoff, %rsp\n"
        "   cld\n"
        "   ldmxcsr (%rsp)\n"
        "   fldcw 4(%rsp)\n"
        "   addq $24, %rsp\n"
        "   popq %r15\n"
        "   popq %r14\n"
        "   popq %r13\n"
        "   popq %r12\n"
        "   popq %rbx\n"
        "   popq %rbp\n"
        "   retq\n"
        ".size ros_worker_kernel_call, .-ros_worker_kernel_call\n");
#define kernel_call ros_worker_kernel_call
#endif

/* The stop signal (SIGUSR2). Cancel, a task's end or an unload may find an
 * application kernel still running STOP_MS after its cancel word was set.
 * It then sends this signal, which takes the kernel away. A kernel that
 * never looks at the word therefore cannot hold up the desktop. On any
 * other thread, or once the kernel has returned, the signal does
 * nothing. */
#define STOP_SIG SIGUSR2
#define STOP_MS  250
__attribute__((unused)) static void stop_signal(int sig, siginfo_t *si, void *context)
{
    (void)si, (void)context;
    if (in_kernel && ros_fault_escape) {
        ros_fault_escape->sig = sig;
        ros_fault_escape->addr = ros_fault_escape->pc = 0;
        siglongjmp(ros_fault_escape->jb, 1);
    }
}

static void run_job(struct job *j, int self)
{
    const struct kernel *k = j->entry ? NULL : kernel_of(j->kernel);
    uint32_t r[WORKER_MAX_RESULTS] = { 0 }, e;
    struct ros_fault_escape esc;
    if (sigsetjmp(esc.jb, 1) == 0) {
        ros_fault_escape = &esc;
        if (k) {
            e = k->run(j->args, r, &j->cancel);
        } else {
#ifdef kernel_call
            in_kernel = 1;
            e = kernel_call(j->entry, j->handle + WJOB_ARGS, j->handle + WJOB_RESULTS,
                            j->handle + WJOB_CANCEL, kstack_top[self]);
            in_kernel = 0;
#else
            (void)self;
            e = WORKER_ERRBASE + WE_NOKERNELS;
#endif
        }
        ros_fault_escape = NULL;
    } else {
        in_kernel = 0;
        ros_fault_escape = NULL;
        if (esc.sig == STOP_SIG) {
            e = WORKER_ERRBASE + WE_CANCELLED;
        } else {
            e = WORKER_ERRBASE + WE_FAULT;
            j->errsig = esc.sig;
            j->erraddr = (uint32_t)(esc.sig == SIGSEGV || esc.sig == SIGBUS ? esc.addr : esc.pc);
        }
    }
    if (!k && e == WORKER_ERRBASE + WE_FAULT)
        j->errsig = j->errsig ? j->errsig : SIGSEGV;
    /* The results go first, then the status (release), then the pollword.
     * A task that sees the pollword or the status therefore sees the
     * results. An application kernel wrote its own into the record. */
    uint32_t *rec = ros_ptr(j->handle);
    if (k)
        memcpy(rec + WJOB_RESULTS / 4, r, sizeof r);
    rec[WJOB_ERRNUM / 4] = e;
    rec[WJOB_ERRADDR / 4] = j->erraddr;
    j->errnum = e;
    uint32_t was = 1;
    pthread_mutex_lock(&mu);
    j->state = e == WORKER_ERRBASE + WE_CANCELLED ? WJOB_CANCELLED : e ? WJOB_FAILED : WJOB_DONE;
    record_status(j, (uint32_t)j->state);
    if (j->pollword) {
        /* This is after the status, and before the job stops being RUNNING
         * to the task side. Worker_Cancel and a task's end wait for that,
         * and may then free the word. The task resets the word and then
         * collects. With a full fence on each side, either it sees this job
         * finished or the word ends up non-zero (README, "Completion"). */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        was = __atomic_fetch_add((uint32_t *)ros_ptr(j->pollword), 1, __ATOMIC_SEQ_CST);
    }
    running--;
    pthread_cond_broadcast(&done_cv);
    pthread_cond_signal(&work_cv);      /* under a limit, the next job may go */
    pthread_mutex_unlock(&mu);
    if (was == 0)
        ros_post(wake, NULL, 0);
}

#ifdef __linux__
static cpu_set_t pool_cpus;
static int pool_pinned;
#endif

static void *worker_main(void *arg)
{
    ros_thread_name("worker");      /* named for /proc: what uses the time */
    int self = (int)(intptr_t)arg;
    ros_thread_signal_stack();
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, STOP_SIG);
    pthread_sigmask(SIG_UNBLOCK, &stop, NULL);
#ifdef __linux__
    if (pool_pinned)
        sched_setaffinity(0, sizeof pool_cpus, &pool_cpus);
#endif
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!stopping && (!queue_head || (limit && running >= limit)))
            pthread_cond_wait(&work_cv, &mu);
        if (stopping)
            break;
        struct job *j = queue_head;
        queue_head = j->qnext;
        if (!queue_head)
            queue_tail = &queue_head;
        queued--;
        running++;
        j->state = WJOB_RUNNING;
        j->thread = self;
        record_status(j, WJOB_RUNNING);
        pthread_mutex_unlock(&mu);
        run_job(j, self);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* The box's online cores. This does not use sysconf first, because musl
 * answers _SC_NPROCESSORS_ONLN from the caller's affinity and a task thread
 * is pinned to one core. It reads Linux's own list (sysfs, "0-7"). If that
 * fails it counts the cpuN lines of /proc/stat. */
static unsigned cores_online(void)
{
    unsigned n = 0;
    FILE *f = fopen("/sys/devices/system/cpu/online", "r");
    if (f) {
        unsigned a, b;
        int c;
        while (fscanf(f, "%u", &a) == 1) {
            b = a;
            if ((c = fgetc(f)) == '-') {
                if (fscanf(f, "%u", &b) != 1)
                    break;
                c = fgetc(f);
            }
            n += b >= a ? b - a + 1 : 0;
            if (c != ',')
                break;
        }
        fclose(f);
    }
    if (!n && (f = fopen("/proc/stat", "r"))) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "cpu", 3) && line[3] >= '0' && line[3] <= '9')
                n++;
        fclose(f);
    }
    if (!n) {
        long l = sysconf(_SC_NPROCESSORS_ONLN);
        n = l > 0 ? (unsigned)l : 1;
    }
    return n;
}

/* One thread per core that the tasks do not use. The tasks' core is the one
 * that the caller (a task thread) is pinned to. The pool may run on all the
 * other cores, and on that one too if there is no other. */
static unsigned cores_spare(void)
{
    unsigned n = cores_online();
    if (n < 2)
        return 1;
    return n - 1 > MAX_THREADS ? MAX_THREADS : n - 1;
}

static os_error *start_pool(void)
{
    if (started)
        return NULL;
    unsigned want = planned;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 10);
#ifdef __linux__
    /* Threads inherit the creator's pinning. The pool's affinity is every
     * online core but the tasks' core, and each thread sets it as it
     * starts. */
    cpu_set_t mine;
    CPU_ZERO(&pool_cpus);
    long n = (long)cores_online();
    pool_pinned = 0;
    if (sched_getaffinity(0, sizeof mine, &mine) == 0 && n > 1) {
        for (unsigned c = 0; c < (unsigned)n && c < CPU_SETSIZE; c++)
            if (!(CPU_COUNT(&mine) == 1 && CPU_ISSET(c, &mine)))
                CPU_SET(c, &pool_cpus);
        pool_pinned = 1;
    }
#endif
    stopping = 0;
    for (nthreads = 0; nthreads < want; nthreads++)
        if (pthread_create(&threads[nthreads], &attr, worker_main, (void *)(intptr_t)nthreads) != 0)
            break;
    pthread_attr_destroy(&attr);
    if (!nthreads)
        return err(WE_NOTHREADS, 0);
    started = 1;
    return NULL;
}

static void stop_pool(void)
{
    if (!started)
        return;
    pthread_mutex_lock(&mu);
    stopping = 1;
    pthread_cond_broadcast(&work_cv);
    pthread_mutex_unlock(&mu);
    for (unsigned i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);
    nthreads = 0;
    started = 0;
}

/* ---- jobs, task side --------------------------------------------------------------- */

static struct job *job_of(uint32_t handle)
{
    if (handle < ROS_RMA_BASE || (handle & 3u))
        return NULL;
    for (struct job *j = jobs; j; j = j->next)
        if (j->handle == handle)
            return j;
    return NULL;
}

static uint32_t current_task(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 5;
    ros_swi(&c, XWimp_ReadSysInfo);
    return c.v ? 0 : c.r[0];
}

/* Forget a finished (or never started) job. It comes off the list and its
 * record is freed. */
static void forget(struct job *j)
{
    struct job **p = &jobs;
    while (*p && *p != j)
        p = &(*p)->next;
    if (*p) {
        *p = j->next;
        if (!*p)
            jobs_tail = p;
    }
    ros_st32(j->handle + WJOB_MAGIC_OFF, 0);
    ros_rma_free(ros_ptr(j->handle));
    free(j);
    njobs--;
}

/* Stop a job. If it is waiting, it comes off the queue. If it is running,
 * it is asked to stop and this waits for it. A worker never waits on the
 * lock, so this cannot deadlock. */
static void stop_job(struct job *j)
{
    pthread_mutex_lock(&mu);
    if (j->state == WJOB_QUEUED) {
        struct job **p = &queue_head;
        while (*p && *p != j)
            p = &(*p)->qnext;
        if (*p) {
            *p = j->qnext;
            if (!*p)
                queue_tail = p;
            queued--;
        }
        j->state = WJOB_CANCELLED;
        record_status(j, WJOB_CANCELLED);
    } else if (j->state == WJOB_RUNNING) {
        __atomic_store_n(&j->cancel, 1, __ATOMIC_RELAXED);
        __atomic_store_n((uint32_t *)ros_ptr(j->handle + WJOB_CANCEL), 1, __ATOMIC_RELAXED);
        /* An application kernel that does not look at its cancel word in
         * time is taken away (stop_signal). A built-in kernel always
         * looks. */
        while (j->state == WJOB_RUNNING) {
            if (!j->entry) {
                pthread_cond_wait(&done_cv, &mu);
                continue;
            }
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += STOP_MS * 1000000L;
            if (until.tv_nsec >= 1000000000L)
                until.tv_sec++, until.tv_nsec -= 1000000000L;
            if (pthread_cond_timedwait(&done_cv, &mu, &until) == ETIMEDOUT && j->state == WJOB_RUNNING)
                pthread_kill(threads[j->thread], STOP_SIG);
        }
    }
    pthread_mutex_unlock(&mu);
}

/* Whether a job touches [base, base + len), by a buffer or by its pollword.
 * Submit noted these. */
static int job_touches(const struct job *j, uint32_t base, uint32_t len)
{
    for (unsigned i = 0; i < j->nbuf; i++)
        if (j->buf[i][0] < base + len && j->buf[i][0] + j->buf[i][1] > base)
            return 1;
    return 0;
}

/* ---- windows (Worker_ShareMemory) ---------------------------------------------------- */

static struct window *window_of(uint32_t addr)
{
    for (struct window *w = wins; w; w = w->next)
        if (addr - w->base < w->len)
            return w;
    return NULL;
}

/* Remove a window. Every job that writes through it is stopped first. The
 * jobs stay listed, as cancelled, for their owners to collect. Then the
 * window is unmapped and its address space is given back. */
static void window_drop(struct window *w)
{
    for (struct job *j = jobs; j; j = j->next)
        if (job_touches(j, w->base, w->len))
            stop_job(j);
    for (struct window **p = &wins; *p; p = &(*p)->next)
        if (*p == w) {
            *p = w->next;
            break;
        }
    ros_slot_window_unmap(w->base);
    ros_dynarea_window_release(w->base);
    free(w);
}

/* The slot under a window is being destroyed (arena.c). */
static void window_lost(uint32_t addr)
{
    struct window *w = window_of(addr);
    if (w && w->base == addr)
        window_drop(w);
}

/* ---- application kernels: loading -------------------------------------------------- */

static struct ukernel *ukernel_of(uint32_t number)
{
    for (struct ukernel *u = ukernels; u; u = u->next)
        if (u->base == number)
            return u;
    return NULL;
}

/* The buffers that an application kernel's arguments name, found from its
 * descriptors. It fills in up to four [address, length) pairs and returns
 * how many. A buffer of no length is not counted. A buffer that is too
 * long is not counted either, and *bad is set for it. */
static unsigned ukernel_buffers(const struct ukernel *u, const uint32_t *a, uint32_t b[4][2], int *bad)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned d = u->desc[i];
        if (!(d & WKB_PRESENT))
            continue;
        uint64_t bytes = (uint64_t)a[d >> 4 & 15] << (d >> 8 & 3);
        if (!bytes)
            continue;
        if (bytes > 0xFFFFFFFFu) {
            if (bad)
                *bad = 1;
            continue;
        }
        b[n][0] = a[d & 15], b[n][1] = (uint32_t)bytes;
        n++;
    }
    return n;
}

static os_error *ukernel_check(const struct ukernel *u, const uint32_t *a)
{
    uint32_t b[4][2];
    int bad = 0;
    unsigned n = ukernel_buffers(u, a, b, &bad);
    if (bad)
        return err(WE_BADARGS, 0);
    for (unsigned i = 0; i < n; i++)
        if (!stays_mapped(b[i][0], b[i][1]))
            return err(WE_BADBUFFER, b[i][0]);
    return NULL;
}

/* Make each pool thread's arena stack for application kernels, with a guard
 * page below it. A kernel that runs off its stack faults, and its job
 * fails. */
static os_error *kernel_stacks(void)
{
#if KERNELS_RUN
    for (unsigned i = 0; i < planned; i++) {
        if (kstack_block[i])
            continue;
        void *b = ros_rma_alloc(KSTACK + 2 * 4096u);
        if (!b)
            return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
        uint32_t lo = (ros_addr(b) + 4095u) & ~4095u;
        mprotect(ros_ptr(lo), 4096, PROT_NONE);
        kstack_block[i] = b;
        kstack_top[i] = lo + 4096u + KSTACK;
    }
#endif
    return NULL;
}

static void kernel_stacks_free(void)
{
    for (unsigned i = 0; i < MAX_THREADS; i++) {
        if (!kstack_block[i])
            continue;
#if KERNELS_RUN
        mprotect(ros_ptr(kstack_top[i] - KSTACK - 4096u), 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
        ros_rma_free(kstack_block[i]);
        kstack_block[i] = NULL;
        kstack_top[i] = 0;
    }
}

/* Remove an application kernel. Its jobs are stopped first. They stay
 * listed, as cancelled, for their owners to collect. Then its pages are
 * made writable again and freed. */
static void ukernel_drop(struct ukernel *u)
{
    for (struct job *j = jobs; j; j = j->next)
        if (j->entry && j->kernel == u->base)
            stop_job(j);
    for (struct ukernel **p = &ukernels; *p; p = &(*p)->next)
        if (*p == u) {
            *p = u->next;
            break;
        }
#if KERNELS_RUN
    mprotect(ros_ptr(u->base), u->size, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
    ros_rma_free(u->block);
    free(u);
}

static const char *machine_name(unsigned m)
{
    return m == WK_EM_AARCH64 ? "AArch64" : m == WK_EM_X86_64 ? "x86-64" : "another machine's";
}

static uint32_t rd32(const uint8_t *p, uint32_t at)
{
    return (uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24;
}

/* Worker_LoadKernel. R0 is the flags (WLK_FILE: R1 points to a kernel
 * file). R1 points to the code or the file, and R2 is its length. For raw
 * code, R3 is the entry's offset and R6 is the code's length (0 means all
 * of it, and any rest is read-only data). R4 and R5 give the buffers its
 * arguments name, two halfwords each. On exit R0 is the kernel's number,
 * for Worker_Submit. */
static os_error *load_kernel(uint32_t *r)
{
    uint32_t flags = r[0], src = r[1], len = r[2];
    if ((flags & ~WLK_FILE) || len == 0 || len > WORKER_MAX_KERNEL || src == 0)
        return err(WE_BADARGS, 0);
    uint16_t desc[4] = { (uint16_t)r[4], (uint16_t)(r[4] >> 16), (uint16_t)r[5], (uint16_t)(r[5] >> 16) };
    for (unsigned i = 0; i < 4; i++)
        if ((desc[i] & WKB_PRESENT) && (desc[i] & 0x7C00u))
            return err(WE_BADARGS, 0);
    /* Copy first. A bad pointer is then the caller's error, with nothing
     * claimed yet. */
    uint32_t size = (len + 4095u) & ~4095u;
    uint8_t *copy = malloc(len);
    if (!copy)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for the Worker kernel");
    memcpy(copy, ros_ptr(src), len);
    unsigned machine = BOX_MACHINE;
    uint32_t entry, code_off = 0, code_len;
    if (flags & WLK_FILE) {
        if (len < WKF_HEADER || rd32(copy, 0) != WKF_MAGIC || rd32(copy, WKF_VERSION) != 1 ||
            rd32(copy, WKF_LENGTH) != len || rd32(copy, WKF_CODE) > len - WKF_HEADER) {
            free(copy);
            return ros_error(WORKER_ERRBASE + WE_BADCODE, "Not a Worker kernel file");
        }
        machine = rd32(copy, WKF_MACHINE);
        entry = rd32(copy, WKF_ENTRY);
        code_off = WKF_HEADER;
        code_len = rd32(copy, WKF_CODE);
    } else {
        entry = r[3];
        code_len = r[6] ? r[6] : len;
    }
    if (code_len == 0 || code_len > len - code_off || entry < code_off || entry >= code_off + code_len ||
        (machine == WK_EM_AARCH64 && (entry & 3))) {
        free(copy);
        return ros_error(WORKER_ERRBASE + WE_BADARGS, "The Worker kernel's entry is not in its code");
    }
    void *block = ros_rma_alloc(size + 4096u);
    if (!block) {
        free(copy);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    uint32_t base = (ros_addr(block) + 4095u) & ~4095u;
    memset(ros_ptr(base), 0, size);
    memcpy(ros_ptr(base), copy, len);
    free(copy);
#if KERNELS_RUN
    /* Read-only from here, so the code that runs is the code checked. */
    mprotect(ros_ptr(base), size, PROT_READ | PROT_EXEC);
#endif
    char why[160];
    uint32_t at = wk_check(machine, ros_ptr(base + code_off), code_len, base + code_off, base, base + len,
                           why, sizeof why);
    os_error *e = NULL;
    if (at)
        e = ros_error(WORKER_ERRBASE + WE_BADCODE, "Worker kernel refused: %s, at +&%X", why,
                      code_off + at - 1);
    else if (machine != BOX_MACHINE)
        e = ros_error(WORKER_ERRBASE + WE_BADCODE, "Worker kernel refused: it is %s code, and this box is %s",
                      machine_name(machine), machine_name(BOX_MACHINE));
    else if (!KERNELS_RUN)
        e = err(WE_NOKERNELS, 0);
    else
        e = kernel_stacks();
    struct ukernel *u = e ? NULL : calloc(1, sizeof *u);
    if (!e && !u)
        e = ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for the Worker kernel");
    if (e) {
#if KERNELS_RUN
        mprotect(ros_ptr(base), size, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
        ros_rma_free(block);
        return e;
    }
#if KERNELS_RUN && defined(__aarch64__)
    /* This makes the instruction side coherent with the data side, which was
     * just written. Every page was just written, so every page is covered.
     * Each worker's isb before the call does the rest. */
    __builtin___clear_cache((char *)ros_ptr(base), (char *)ros_ptr(base) + size);
#endif
    u->base = base, u->size = size, u->block = block;
    u->entry = base + entry;
    memcpy(u->desc, desc, sizeof desc);
    u->task = current_task();
    u->rt = ros_task_current();
    u->next = ukernels, ukernels = u;
    r[0] = base;
    return NULL;
}

/* Worker_UnloadKernel. R0 is the kernel. Its jobs are cancelled first. */
static os_error *unload_kernel(uint32_t *r)
{
    struct ukernel *u = ukernel_of(r[0]);
    if (!u)
        return err(WE_BADKERNEL, r[0]);
    ukernel_drop(u);
    return NULL;
}

/* Stop and forget every job of a task, or every job if all is set. Every
 * window and kernel of the task goes too. */
static void cancel_task(uint32_t task, int all)
{
    struct job *j = jobs;
    while (j) {
        struct job *next = j->next;
        if (all || j->task == task) {
            stop_job(j);
            forget(j);
        }
        j = next;
    }
    struct window *w = wins;
    while (w) {
        struct window *next = w->next;
        if (all || w->task == task)
            window_drop(w);
        w = next;
    }
    struct ukernel *u = ukernels;
    while (u) {
        struct ukernel *next = u->next;
        if (all || u->task == task)
            ukernel_drop(u);
        u = next;
    }
}

/* A task's thread has ended (task.h, ros_task_on_end), whether it closed
 * down or not. Its jobs, windows and kernels go as at its CloseDown, before
 * its slot does. */
static void task_ended(struct ros_task *rt)
{
    struct job *j = jobs;
    while (j) {
        struct job *next = j->next;
        if (j->rt == rt) {
            stop_job(j);
            forget(j);
        }
        j = next;
    }
    struct window *w = wins;
    while (w) {
        struct window *next = w->next;
        if (w->rt == rt)
            window_drop(w);
        w = next;
    }
    struct ukernel *u = ukernels;
    while (u) {
        struct ukernel *next = u->next;
        if (u->rt == rt)
            ukernel_drop(u);
        u = next;
    }
}

static int state_of(struct job *j)
{
    pthread_mutex_lock(&mu);
    int s = j->state;
    pthread_mutex_unlock(&mu);
    return s;
}

/* ---- the SWIs ---------------------------------------------------------------------- */

static os_error *submit(uint32_t *r)
{
    const struct kernel *k = kernel_of(r[1]);
    const struct ukernel *u = k ? NULL : ukernel_of(r[1]);
    if (!k && !u)
        return err(WE_BADKERNEL, r[1]);
    uint32_t n = r[3];
    if (n > WORKER_MAX_ARGS || (n && r[2] == 0))
        return err(WE_BADARGS, 0);
    uint32_t args[WORKER_MAX_ARGS] = { 0 };
    for (uint32_t i = 0; i < n; i++)
        args[i] = ros_ld32(r[2] + 4 * i);       /* the caller's memory, so a fault is its error */
    os_error *e = k ? k->check(args, n) : ukernel_check(u, args);
    if (e)
        return e;
    uint32_t pw = r[4];
    if (pw && ((pw & 3u) || !stays_mapped(pw, 4)))
        return err(WE_BADPOLLWORD, pw);
    if (njobs >= MAX_JOBS)
        return err(WE_TOOMANY, 0);
    if ((e = start_pool()))
        return e;
    uint32_t *rec = ros_rma_alloc(WJOB_SIZE);
    struct job *j = calloc(1, sizeof *j);
    if (!rec || !j) {
        if (rec)
            ros_rma_free(rec);
        free(j);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    memset(rec, 0, WJOB_SIZE);
    j->handle = ros_addr(rec);
    j->kernel = k ? k->number : u->base;
    j->entry = u ? u->entry : 0;
    j->nbuf = k ? k->buffers(args, j->buf) : ukernel_buffers(u, args, j->buf, NULL);
    if (pw)
        j->buf[j->nbuf][0] = pw, j->buf[j->nbuf][1] = 4, j->nbuf++;
    j->task = current_task();
    j->rt = ros_task_current();
    j->pollword = pw;
    memcpy(j->args, args, sizeof args);
    rec[WJOB_MAGIC_OFF / 4] = WJOB_MAGIC;
    rec[WJOB_KERNEL / 4] = j->kernel;
    rec[WJOB_TAG / 4] = r[5];
    rec[WJOB_POLLWORD / 4] = pw;
    rec[WJOB_TASK / 4] = j->task;
    memcpy(rec + WJOB_ARGS / 4, args, sizeof args);
    *jobs_tail = j;
    jobs_tail = &j->next;
    njobs++;
    pthread_mutex_lock(&mu);
    j->state = WJOB_QUEUED;
    record_status(j, WJOB_QUEUED);
    *queue_tail = j;
    queue_tail = &j->qnext;
    queued++;
    pthread_cond_signal(&work_cv);
    pthread_mutex_unlock(&mu);
    r[0] = j->handle;
    return NULL;
}

static os_error *status(uint32_t *r)
{
    struct job *j = job_of(r[0]);
    if (!j)
        return err(WE_BADJOB, 0);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    r[0] = (uint32_t)state_of(j);
    r[1] = ros_ld32(j->handle + WJOB_TAG);
    r[2] = j->kernel;
    return NULL;
}

static os_error *result(uint32_t *r)
{
    struct job *j = job_of(r[0]);
    if (!j)
        return err(WE_BADJOB, 0);
    int s = state_of(j);
    if (s == WJOB_QUEUED || s == WJOB_RUNNING)
        return err(WE_NOTDONE, 0);
    int keep = r[1] & 1u;
    os_error *e = NULL;
    if (s == WJOB_CANCELLED)
        e = err(WE_CANCELLED, 0);
    else if (s == WJOB_FAILED && j->errnum == WORKER_ERRBASE + WE_FAULT)
        e = j->errsig == SIGILL || j->errsig == SIGTRAP
                ? ros_error(j->errnum, "Worker job failed: undefined instruction at &%08X", j->erraddr)
            : j->errsig == SIGFPE
                ? ros_error(j->errnum, "Worker job failed: arithmetic exception at &%08X", j->erraddr)
                : err(WE_FAULT, j->erraddr);
    else if (s == WJOB_FAILED)
        e = ros_error(j->errnum, "Worker job failed: its kernel returned &%X", j->errnum);
    if (!e) {
        r[0] = ros_ld32(j->handle + WJOB_TAG);
        for (unsigned i = 0; i < WORKER_MAX_RESULTS; i++)
            r[1 + i] = ros_ld32(j->handle + WJOB_RESULTS + 4 * i);
    }
    if (!keep)
        forget(j);
    return e;
}

static os_error *cancel(uint32_t *r)
{
    struct job *j = job_of(r[0]);
    if (!j)
        return err(WE_BADJOB, 0);
    stop_job(j);
    forget(j);
    return NULL;
}

static os_error *info(uint32_t *r)
{
    if (r[0] > 1)
        return err(WE_BADREASON, 0);
    pthread_mutex_lock(&mu);
    if (r[0] == 1) {
        limit = r[1];
        pthread_cond_broadcast(&work_cv);
    }
    r[0] = started ? nthreads : planned;
    r[1] = limit;
    r[2] = queued;
    r[3] = running;
    r[4] = WORKER_VERSION;
    r[5] = BOX_MACHINE;
    pthread_mutex_unlock(&mu);
    return NULL;
}

static os_error *completed(uint32_t *r)
{
    uint32_t pw = r[0], task = current_task();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (struct job *j = jobs; j; j = j->next) {
        if (j->collected || j->task != task || (pw && j->pollword != pw))
            continue;
        int s = state_of(j);
        if (s == WJOB_QUEUED || s == WJOB_RUNNING)
            continue;
        j->collected = 1;
        r[0] = j->handle;
        return NULL;
    }
    r[0] = 0;
    return NULL;
}

/* Worker_ShareMemory. R0 = 0 shares [R1, R1 + R2) of the caller's slot. On
 * exit R0 is the window's address of R1, R1 is the window's start and R2 is
 * its length (whole pages). R0 = 1 unshares the window that R1 is in.
 * R0 = 2 unshares all of the caller's windows. */
static os_error *share_memory(uint32_t *r)
{
    if (r[0] == WSM_UNSHARE) {
        struct window *w = window_of(r[1]);
        if (!w)
            return err(WE_NOTWINDOW, 0);
        window_drop(w);
        return NULL;
    }
    if (r[0] == WSM_UNSHARE_ALL) {
        uint32_t task = current_task();
        struct ros_task *rt = ros_task_current();
        struct window *w = wins;
        while (w) {
            struct window *next = w->next;
            if (w->rt == rt && w->task == task)
                window_drop(w);
            w = next;
        }
        return NULL;
    }
    if (r[0] != WSM_SHARE)
        return err(WE_BADREASON, 0);
    const struct ros_slot *slot = ros_slot_current;
    uint32_t start = r[1], len = r[2];
    if (!slot || !slot->size || len == 0 || start < ROS_APP_BASE ||
        start - ROS_APP_BASE > slot->size || len > slot->size - (start - ROS_APP_BASE))
        return err(WE_BADRANGE, 0);
    uint32_t lo = start & ~0xFFFu;
    uint32_t hi = (uint32_t)(((uint64_t)start + len + 0xFFFu) & ~(uint64_t)0xFFFu);
    if (hi - ROS_APP_BASE > slot->size || ros_slot_guard_overlaps(lo, hi))
        return err(WE_BADRANGE, 0);          /* a slot is whole pages, and the range must not touch its stack's guard */
    struct window *w = calloc(1, sizeof *w);
    uint32_t base = w ? ros_dynarea_window_reserve(hi - lo) : 0;
    if (!base) {
        free(w);
        return err(WE_NOSPACE, 0);
    }
    if (ros_slot_window_map(slot, lo - ROS_APP_BASE, hi - lo, base) != 0) {
        ros_dynarea_window_release(base);
        free(w);
        return err(WE_NOSPACE, 0);
    }
    w->base = base, w->len = hi - lo, w->start = lo;
    w->task = current_task();
    w->rt = ros_task_current();
    w->next = wins, wins = w;
    r[0] = base + (start - lo);
    r[1] = base;
    r[2] = hi - lo;
    return NULL;
}

static void dispatch(struct ros_cpu *s, unsigned which)
{
    static os_error *(*const fn[])(uint32_t *) = { submit, status, result, cancel, info, completed,
                                                   share_memory, load_kernel, unload_kernel };
    os_error *e = which < sizeof fn / sizeof fn[0] ? fn[which](s->r)
                                                   : ros_error(0x1E6u, "SWI value out of range for module Worker");
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

void ros_thunk_Worker_Submit(struct ros_cpu *s) { dispatch(s, 0); }
void ros_thunk_Worker_Status(struct ros_cpu *s) { dispatch(s, 1); }
void ros_thunk_Worker_Result(struct ros_cpu *s) { dispatch(s, 2); }
void ros_thunk_Worker_Cancel(struct ros_cpu *s) { dispatch(s, 3); }
void ros_thunk_Worker_Info(struct ros_cpu *s) { dispatch(s, 4); }
void ros_thunk_Worker_Completed(struct ros_cpu *s) { dispatch(s, 5); }
void ros_thunk_Worker_ShareMemory(struct ros_cpu *s) { dispatch(s, 6); }
void ros_thunk_Worker_LoadKernel(struct ros_cpu *s) { dispatch(s, 7); }
void ros_thunk_Worker_UnloadKernel(struct ros_cpu *s) { dispatch(s, 8); }

/* ---- the module -------------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    planned = cores_spare();
    limit = 0;
    ros_task_on_end(task_ended);
    ros_slot_window_lost = window_lost;
#if KERNELS_RUN
    struct sigaction sa = { .sa_sigaction = stop_signal, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigemptyset(&sa.sa_mask);
    sigaction(STOP_SIG, &sa, NULL);
#endif
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    cancel_task(0, 1);
    ros_slot_window_lost = NULL;
    stop_pool();
    kernel_stacks_free();
    return NULL;
}

/* Service_WimpCloseDown. A task's jobs go, stopped and with their records
 * freed, so nothing writes to its buffers or its pollword after it. Then
 * its windows and kernels go. */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] == 0x53 && s->r[0] == 0)
        cancel_task(s->r[2], 0);
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6u, "SWI value out of range for module Worker");
}

struct ros_module worker_module = {
    .title = "Worker",
    .help = "Worker\t3.00 (03 Oct 2026) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0xC0180,
    .swi_thunks = ros_swi_thunks_Worker,
    .swi_names = ros_swi_names_Worker,
    .swi_prefix = "Worker",
};

__attribute__((constructor)) static void count(void)
{
    worker_module.swi_count = ros_swi_count_Worker;
}
