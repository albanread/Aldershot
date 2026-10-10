/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_worker.c: the Worker module (modules/worker). A mandel_row job
 * is run on a pool thread, and its row and results are checked against the
 * same sums done here. Its pollword is bumped and its completion collected
 * (Submit, Completed, Status, Result). A buffer in application space is
 * refused. A long job is cancelled while it runs.
 *
 * Windows onto a slot are checked: a window is shared, a job writing
 * through it is seen at the slot's own address, the slot is not shrunk
 * below it, it can be unshared, and it is gone with its slot.
 *
 * An application kernel is checked: tests/worker/mandelk.c, linked by
 * roscc link --kernel, is loaded and run, and its row is the built-in
 * kernel's. Code with a system call is refused. A kernel that faults fails
 * its job and the box goes on. One that never stops is taken away by
 * Cancel. The kernels of a task that closes down are gone.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/vdu.h"
#include "capp_images.h"
#include "kcheck.h"
#include "selftest.h"
#include "worker.h"

#if defined(__linux__) && !defined(ROS_ARENA_HOSTED) && (defined(__aarch64__) || defined(__x86_64__))
#define KERNELS_RUN 1
#else
#define KERNELS_RUN 0
#endif

#define check ros_check
#define W 32

static int swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[10])
{
    return ((const os_error *)ros_ptr(r[0]))->errnum;
}

static void nap_ms(unsigned ms)
{
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

/* The status of a job, waiting up to ms for it to finish */
static uint32_t wait_done(uint32_t job, unsigned ms)
{
    uint32_t r[10];
    for (unsigned t = 0;; t++) {
        memset(r, 0, sizeof r);
        r[0] = job;
        if (swi(XWorker_Status, r))
            return 0xFFFFFFFFu;
        if (r[0] >= WJOB_DONE || t >= ms)
            return r[0];
        nap_ms(1);
    }
}

/* Raw code for the box's machine: a system call; a store into its own
 * code, which is read-only (a fault); a loop that never looks at its cancel
 * word */
#if defined(__aarch64__)
static const uint32_t k_svc[] = { 0xD4000001u, 0xD65F03C0u };                  /* svc #0; ret */
static const uint32_t k_fault[] = { 0x10000000u, 0xB9000000u, 0x52800000u, 0xD65F03C0u };
                                        /* adr x0, .; str w0, [x0]; mov w0, #0; ret */
static const uint32_t k_spin[] = { 0x14000000u };                              /* b . */
#else
static const uint8_t k_svc[] = { 0x0F, 0x05, 0xC3 };                            /* syscall; ret */
static const uint8_t k_fault[] = { 0x48, 0x8D, 0x05, 0xF9, 0xFF, 0xFF, 0xFF,     /* lea rax, [rip - 7] */
                                   0x89, 0x00, 0x31, 0xC0, 0xC3 };               /* mov [rax], eax; xor; ret */
static const uint8_t k_spin[] = { 0xEB, 0xFE };                                 /* jmp . */
#endif

/* Load raw code (Worker_LoadKernel, R0 = 0) from an RMA copy: 0 and the
 * kernel, or the error number */
static uint32_t load_raw(const void *code, uint32_t len, uint32_t *kernel, uint32_t *rerr)
{
    void *m = ros_rma_alloc(len);
    if (!m)
        return 1;
    memcpy(m, code, len);
    uint32_t r[10] = { 0 };
    r[0] = 0, r[1] = ros_addr(m), r[2] = len, r[3] = 0, r[4] = 0x8210u;
    int bad = swi(XWorker_LoadKernel, r);
    ros_rma_free(m);
    if (rerr)
        *rerr = r[0];
    if (bad)
        return errnum(r);
    *kernel = r[0];
    return 0;
}

static void selftest_kernels(uint32_t *row, uint32_t *args, uint32_t *pw, const uint32_t *want,
                             uint64_t total, uint32_t inside)
{
    uint32_t r[10] = { 0 };
    swi(XWorker_Info, r);
#if defined(__aarch64__)
    unsigned machine = WK_EM_AARCH64;
#else
    unsigned machine = WK_EM_X86_64;
#endif
    check(r[4] == 300 && r[5] == machine, "Worker_Info: version 3.00, and the machine kernels are for",
          "version %u, machine %u", r[4], r[5]);

    /* code with a system call is refused, whatever the build */
    uint32_t kernel = 0, e;
    uint32_t refused = load_raw(k_svc, sizeof k_svc, &kernel, &e);
    const char *msg = refused == WORKER_ERRBASE + WE_BADCODE ? ((const os_error *)ros_ptr(e))->errmess : "";
    check(refused == WORKER_ERRBASE + WE_BADCODE && strstr(msg, "system call"),
          "Worker_LoadKernel: code with a system call refused by the checker", "&%X: %s", refused, msg);

    /* the kernel file roscc made */
    void *file = ros_rma_alloc(CAPP_MANDELK_SIZE);
    memcpy(file, capp_mandelk, CAPP_MANDELK_SIZE);
    memset(r, 0, sizeof r);
    r[0] = WLK_FILE, r[1] = ros_addr(file), r[2] = CAPP_MANDELK_SIZE, r[4] = 0x8210u;
    int bad = swi(XWorker_LoadKernel, r);
    ros_rma_free(file);
    if (!KERNELS_RUN) {
        check(bad && errnum(r) == WORKER_ERRBASE + WE_NOKERNELS,
              "Worker_LoadKernel: checked, and refused where the arena is not executable", NULL);
        return;
    }
    kernel = bad ? 0 : r[0];
    check(!bad && kernel >= ROS_RMA_BASE && !(kernel & 0xFFFu),
          "Worker_LoadKernel: roscc's kernel file loaded at a page of the RMA", "&%X %s", kernel,
          bad ? ((const os_error *)ros_ptr(r[0]))->errmess : "");
    if (bad)
        return;

    /* run: its row and results the built-in kernel's */
    memset(row, 0xEE, W * 4);
    *pw = 0;
    memset(r, 0, sizeof r);
    r[1] = kernel, r[2] = ros_addr(args), r[3] = MR_NARGS, r[4] = ros_addr(pw), r[5] = 0x33;
    int ok = !swi(XWorker_Submit, r);
    uint32_t job = ok ? r[0] : 0;
    uint32_t st = job ? wait_done(job, 5000) : 0;
    uint32_t same = 1;
    for (unsigned x = 0; x < W; x++)
        same &= row[x] == want[x];
    memset(r, 0, sizeof r);
    r[0] = job;
    ok = ok && st == WJOB_DONE && !swi(XWorker_Result, r);
    check(ok && same && r[0] == 0x33 && r[1] == (uint32_t)total && r[3] == inside && *pw == 1,
          "Worker: an application kernel run, its row and results the built-in kernel's",
          "status %u, x=8: %u (%u), iterations %u (%u), inside %u (%u)", st, row[8], want[8], r[1],
          (uint32_t)total, r[3], inside);

    /* a buffer it names is checked as a built-in's are */
    uint32_t saved = args[MR_BUFFER];
    args[MR_BUFFER] = ROS_APP_BASE + 0x1000;
    memset(r, 0, sizeof r);
    r[1] = kernel, r[2] = ros_addr(args), r[3] = MR_NARGS;
    check(swi(XWorker_Submit, r) && errnum(r) == WORKER_ERRBASE + WE_BADBUFFER,
          "Worker_Submit: an application kernel's buffer in application space refused", NULL);
    args[MR_BUFFER] = saved;

    /* a kernel that faults fails its job; the box goes on */
    uint32_t fk = 0;
    refused = load_raw(k_fault, sizeof k_fault, &fk, &e);
    memset(r, 0, sizeof r);
    r[1] = fk, r[2] = ros_addr(args), r[3] = MR_NARGS;
    ok = !refused && !swi(XWorker_Submit, r);
    job = ok ? r[0] : 0;
    st = job ? wait_done(job, 5000) : 0;
    memset(r, 0, sizeof r);
    r[0] = job;
    int failed = job && swi(XWorker_Result, r) && errnum(r) == WORKER_ERRBASE + WE_FAULT;
    msg = failed ? ((const os_error *)ros_ptr(r[0]))->errmess : "";
    memset(r, 0, sizeof r);
    r[1] = kernel, r[2] = ros_addr(args), r[3] = MR_NARGS;
    ok = !swi(XWorker_Submit, r);
    job = ok ? r[0] : 0;
    uint32_t st2 = job ? wait_done(job, 5000) : 0;
    memset(r, 0, sizeof r);
    r[0] = job;
    swi(XWorker_Result, r);
    check(!refused && st == WJOB_FAILED && failed && st2 == WJOB_DONE,
          "Worker: a kernel that faults (a store into its own code, which is read-only) fails its job, "
          "and the next job runs", "load &%X, status %u, \"%s\", then %u", refused, st, msg, st2);

    /* a kernel that never looks at its cancel word: Cancel takes it away */
    uint32_t sk = 0;
    refused = load_raw(k_spin, sizeof k_spin, &sk, &e);
    memset(r, 0, sizeof r);
    r[1] = sk, r[2] = ros_addr(args), r[3] = MR_NARGS;
    ok = !refused && !swi(XWorker_Submit, r);
    job = ok ? r[0] : 0;
    nap_ms(20);
    st = job ? wait_done(job, 0) : 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    memset(r, 0, sizeof r);
    r[0] = job;
    ok = ok && !swi(XWorker_Cancel, r);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(ok && st == WJOB_RUNNING && ms < 2000, "Worker_Cancel: a kernel that never stops, taken away",
          "status %u, %ld ms", st, ms);

    /* a task's kernels go at its CloseDown */
    memset(r, 0, sizeof r);
    r[0] = 5;
    uint32_t task = swi(XWimp_ReadSysInfo, r) ? 0 : r[0];
    struct ros_cpu c;                   /* the Worker module's alone: the others' state stays */
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0x53, c.r[2] = task;
    worker_module.service(&worker_module, &c);
    int gone = 1;
    uint32_t ks[3] = { kernel, fk, sk };
    for (unsigned i = 0; i < 3; i++) {
        memset(r, 0, sizeof r);
        r[1] = ks[i], r[2] = ros_addr(args), r[3] = MR_NARGS;
        gone &= swi(XWorker_Submit, r) && errnum(r) == WORKER_ERRBASE + WE_BADKERNEL;
        memset(r, 0, sizeof r);
        r[0] = ks[i];
        gone &= swi(XWorker_UnloadKernel, r) && errnum(r) == WORKER_ERRBASE + WE_BADKERNEL;
    }
    check(gone, "Worker: a task's kernels unloaded at its CloseDown", "task &%X", task);
}

void ros_selftest_worker(void)
{
    uint32_t r[10] = { 0 };
    int ok = !swi(XWorker_Info, r);
    check(ok && r[0] >= 1 && r[4] == WORKER_VERSION, "Worker_Info: the pool's threads and version",
          "threads %u, version %u", r[0], r[4]);

    uint32_t *mem = ros_rma_alloc(W * 4 + 16 * 4 + 4);
    if (!mem) {
        check(0, "Worker: RMA for the tests", NULL);
        return;
    }
    uint32_t *row = mem, *args = mem + W, *pw = mem + W + 16;
    memset(mem, 0xEE, W * 4);
    *pw = 0;
    /* the real axis from -2, a quarter a pixel: x0 = -2 (32.32: -2, 0), dx =
     * 0.25 (0, &40000000), y = 0 */
    const uint32_t maxit = 200;
    uint32_t a[MR_NARGS] = { ros_addr(row), W, maxit, MR_OUT_COUNTS, 0,
                             (uint32_t)-2, 0, 0, 0x40000000u, 0, 0 };
    memcpy(args, a, sizeof a);
    memset(r, 0, sizeof r);
    r[1] = WORKER_KERNEL_MANDEL_ROW, r[2] = ros_addr(args), r[3] = MR_NARGS, r[4] = ros_addr(pw),
    r[5] = 0x7A67;
    ok = !swi(XWorker_Submit, r);
    uint32_t job = ok ? r[0] : 0;
    check(ok && job >= ROS_RMA_BASE && ros_ld32(job) == WJOB_MAGIC,
          "Worker_Submit: mandel_row queued, its record in the RMA", "handle &%X", job);
    uint32_t st = job ? wait_done(job, 5000) : 0;
    check(st == WJOB_DONE, "Worker: the job done on a pool thread", "status %u", st);

    /* the same sums here */
    uint32_t want[W], inside = 0, same = 1;
    uint64_t total = 0;
    for (unsigned x = 0; x < W; x++) {
        double cr = -2.0 + 0.25 * x, zr = 0, zi = 0, zr2 = 0, zi2 = 0;
        uint32_t n = 0;
        while (n < maxit && zr2 + zi2 <= 4.0) {
            zi = 2 * zr * zi;
            zr = zr2 - zi2 + cr;
            zr2 = zr * zr, zi2 = zi * zi;
            n++;
        }
        want[x] = n;
        total += n;
        inside += n >= maxit;
        same &= row[x] == n;
    }
    check(same, "Worker: mandel_row's counts, as computed here",
          "x=0: %u (%u), x=8: %u (%u), x=10: %u (%u)", row[0], want[0], row[8], want[8], row[10], want[10]);
    check(*pw == 1, "Worker: the pollword bumped once", "%u", *pw);

    *pw = 0;
    memset(r, 0, sizeof r);
    r[0] = ros_addr(pw);
    ok = !swi(XWorker_Completed, r);
    uint32_t got = r[0];
    memset(r, 0, sizeof r);
    r[0] = ros_addr(pw);
    ok = ok && !swi(XWorker_Completed, r);
    check(ok && got == job && r[0] == 0, "Worker_Completed: the job, once", "&%X then &%X", got, r[0]);

    memset(r, 0, sizeof r);
    r[0] = job;
    ok = !swi(XWorker_Result, r);
    check(ok && r[0] == 0x7A67 && r[1] == (uint32_t)total && r[2] == 0 && r[3] == inside,
          "Worker_Result: the tag, the iterations and the points inside",
          "tag &%X, iterations %u (%u), inside %u (%u)", r[0], r[1], (uint32_t)total, r[3], inside);
    memset(r, 0, sizeof r);
    r[0] = job;
    check(swi(XWorker_Status, r) && errnum(r) == WORKER_ERRBASE + WE_BADJOB,
          "Worker: a job collected is gone", NULL);

    /* application space is the running task's alone: refused */
    a[MR_BUFFER] = ROS_APP_BASE + 0x1000;
    memcpy(args, a, sizeof a);
    memset(r, 0, sizeof r);
    r[1] = WORKER_KERNEL_MANDEL_ROW, r[2] = ros_addr(args), r[3] = MR_NARGS;
    check(swi(XWorker_Submit, r) && errnum(r) == WORKER_ERRBASE + WE_BADBUFFER,
          "Worker_Submit: a buffer in application space refused", NULL);

    /* a long job, with every point inside and a million iterations each,
     * cancelled while it runs */
    uint32_t a2[MR_NARGS] = { ros_addr(row), W, 1u << 20, MR_OUT_COUNTS, 0,
                              (uint32_t)-1, 0xE0000000u, 0, 0, 0, 0 };   /* -0.125, dx 0 */
    memcpy(args, a2, sizeof a2);
    memset(r, 0, sizeof r);
    r[1] = WORKER_KERNEL_MANDEL_ROW, r[2] = ros_addr(args), r[3] = MR_NARGS;
    ok = !swi(XWorker_Submit, r);
    job = r[0];
    nap_ms(20);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    memset(r, 0, sizeof r);
    r[0] = job;
    ok = ok && !swi(XWorker_Cancel, r);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    memset(r, 0, sizeof r);
    r[0] = job;
    check(ok && ms < 1000 && swi(XWorker_Status, r), "Worker_Cancel: a running job stopped and gone",
          "%ld ms", ms);

    /* A window onto a slot of its own, mapped at &8000 for the
     * test as a task's is. The range is unaligned: 12 bytes into a page. */
    const struct ros_slot *was = ros_slot_current;
    struct ros_slot sl;
    if (ros_slot_create(&sl, 0x40000) != 0 || ros_slot_map(&sl) != 0) {
        check(0, "Worker_ShareMemory: a slot for the tests", NULL);
        if (was)
            ros_slot_map(was);
        ros_rma_free(mem);
        return;
    }
    const uint32_t start = ROS_APP_BASE + 0x10000u + 12;
    memset(r, 0, sizeof r);
    r[0] = 0, r[1] = start, r[2] = W * 4;
    ok = !swi(XWorker_ShareMemory, r);
    uint32_t win = r[0], wbase = r[1], wlen = r[2];
    check(ok && win >= ROS_DA_BASE && win < ROS_SCREEN_BASE && win == wbase + 12 && wlen == 0x1000 &&
              ros_dynarea_contains(win, win + W * 4),
          "Worker_ShareMemory: a range of the slot, unaligned, mapped again at a window in the "
          "dynamic-area range", "window &%X, start &%X, %u bytes", win, wbase, wlen);

    /* a job writes through the window; the result is at the slot's address */
    memset(ros_ptr(start), 0xEE, W * 4);
    a[MR_BUFFER] = win;
    memcpy(args, a, sizeof a);
    *pw = 0;
    memset(r, 0, sizeof r);
    r[1] = WORKER_KERNEL_MANDEL_ROW, r[2] = ros_addr(args), r[3] = MR_NARGS, r[4] = ros_addr(pw);
    ok = !swi(XWorker_Submit, r);
    job = ok ? r[0] : 0;
    st = job ? wait_done(job, 5000) : 0;
    same = 1;
    for (unsigned x = 0; x < W; x++)
        same &= ros_ld32(start + 4 * x) == want[x];
    memset(r, 0, sizeof r);
    r[0] = job;
    ok = ok && st == WJOB_DONE && !swi(XWorker_Result, r);
    check(ok && same && __atomic_load_n(pw, __ATOMIC_ACQUIRE) == 1,
          "Worker: a job writing through the window, its row seen at the slot's own address",
          "status %u, x=8: %u (%u)", st, ros_ld32(start + 32), want[8]);

    /* the slot cannot shrink below the window, as Wimp_SlotSize, AMB and
     * *WimpSlot all resize it (ros_slot_resize); it may grow, the window
     * still the same memory (Linux; macOS's copy keeps the size) */
    int shrunk = ros_slot_resize(&sl, 0x10000);
    int grown = ros_slot_resize(&sl, 0x80000);
#ifdef __linux__
    int grow_ok = grown == 0 && sl.size == 0x80000;
#else
    int grow_ok = grown == -EBUSY;
#endif
    ros_st32(start, 0x5A5A1234u);
    check(shrunk == -EBUSY && grow_ok && ros_ld32(win) == 0x5A5A1234u,
          "Worker_ShareMemory: the slot not shrunk below a window, the window the slot's memory still",
          "shrink %d, grow %d, size &%X", shrunk, grown, sl.size);

    /* unshared: gone from the map, refused as a buffer, the slot free to shrink */
    memset(r, 0, sizeof r);
    r[0] = 1, r[1] = win + 100;
    ok = !swi(XWorker_ShareMemory, r);
    memset(r, 0, sizeof r);
    r[1] = WORKER_KERNEL_MANDEL_ROW, r[2] = ros_addr(args), r[3] = MR_NARGS;
    int refused = swi(XWorker_Submit, r) && errnum(r) == WORKER_ERRBASE + WE_BADBUFFER;
    memset(r, 0, sizeof r);
    r[0] = 1, r[1] = win;
    int again = swi(XWorker_ShareMemory, r) && errnum(r) == WORKER_ERRBASE + WE_NOTWINDOW;
    check(ok && refused && again && !ros_dynarea_contains(win, win + 4) &&
              ros_slot_resize(&sl, 0x10000) == 0,
          "Worker_ShareMemory 1: unshared, the window gone and the slot free to shrink", NULL);

    /* a window goes with its slot */
    memset(r, 0, sizeof r);
    r[0] = 0, r[1] = ROS_APP_BASE, r[2] = 0x2000;
    ok = !swi(XWorker_ShareMemory, r);
    win = r[0];
    ros_slot_destroy(&sl);
    check(ok && !ros_dynarea_contains(win, win + 4), "Worker_ShareMemory: a window gone with its slot",
          "window &%X", win);
    if (was)
        ros_slot_map(was);

    /* Application kernels */
    *pw = 0;
    a[MR_BUFFER] = ros_addr(row);
    memcpy(args, a, sizeof a);
    selftest_kernels(row, args, pw, want, total, inside);
    ros_rma_free(mem);
}
