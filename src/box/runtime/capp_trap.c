/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* capp_trap.c: a fault in the A64 code that BASIC's CALL runs on the Mac
 * becomes the program's error, as in the box. It does not end the app.
 *
 * In the box a data abort in a program's code becomes "Internal error:
 * abort on data transfer at &XXXXXXXX" (runtime/fault.c), and the box
 * lives on. On the Mac the block runs from its copy in the MAP_JIT
 * arena (arch/aarch64/capp_jit.c.inc), where a fault would be a SIGSEGV for
 * the whole app. ros_capp_trap_call runs the block with SIGSEGV, SIGBUS
 * and SIGILL caught. A fault while the block runs jumps back here with the
 * faulting instruction's address, and capp_jit.c.inc raises the box's
 * error for it.
 *
 * Only the block's own instructions are trapped. A SWI that the block
 * calls runs the runtime, and ros_capp_trap_suspend and
 * ros_capp_trap_resume take the trap away for that time. A signal anywhere
 * else goes to whatever handled it before. The old action is put back, so
 * the instruction faults again and is handled as it always was.
 *
 * This has its own file because it needs Darwin's thread state (the pc),
 * which the runtime's strict C11 hides. Only a build that defines
 * ROS_CAPP_A64_JIT (BBC BASIC V for Mac's, capp.h) compiles anything here.
 * No box's build lists it.
 */
#if defined(ROS_CAPP_A64_JIT) && defined(__APPLE__) && defined(__aarch64__)
#define _DARWIN_C_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>

static __thread sigjmp_buf *trap_jb;
static __thread uint64_t trap_pc, trap_addr;
static __thread int trap_sig;

static const int sigs[3] = { SIGSEGV, SIGBUS, SIGILL };
static struct sigaction old_action[3];
static pthread_once_t installed = PTHREAD_ONCE_INIT;

static void on_fault(int sig, siginfo_t *si, void *context)
{
    if (trap_jb) {
        ucontext_t *uc = context;
        sigjmp_buf *jb = trap_jb;
        trap_jb = NULL;
        trap_sig = sig;
        trap_addr = (uint64_t)(uintptr_t)si->si_addr;
        trap_pc = uc->uc_mcontext->__ss.__pc;
        siglongjmp(*jb, 1);
    }
    /* Not the block's fault. Put the old action back, so that the
     * instruction faults again under it, as it did before this file. */
    for (int i = 0; i < 3; i++)
        if (sigs[i] == sig)
            sigaction(sig, &old_action[i], NULL);
}

static void install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 3; i++)
        sigaction(sigs[i], &sa, &old_action[i]);
}

int ros_capp_trap_call(void (*fn)(void *), void *arg, uint64_t *pc, uint64_t *addr)
{
    pthread_once(&installed, install);
    sigjmp_buf jb;
    sigjmp_buf *outer = trap_jb;
    if (sigsetjmp(jb, 1)) {
        trap_jb = outer;
        *pc = trap_pc;
        *addr = trap_addr;
        return trap_sig;
    }
    trap_jb = &jb;
    fn(arg);
    trap_jb = outer;
    return 0;
}

void *ros_capp_trap_suspend(void)
{
    void *t = trap_jb;
    trap_jb = NULL;
    return t;
}

void ros_capp_trap_resume(void *t)
{
    trap_jb = t;
}
#endif
