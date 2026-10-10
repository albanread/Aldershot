/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* fault.h -- a processor's exceptions as RISC OS's errors.
 *
 * RISC OS turns an exception into an error. The kernel passes an
 * undefined instruction, a prefetch abort and a data abort to the handler
 * the environment names for it (OS_ChangeEnvironment 1, 2, 3: Kernel/s/
 * Exceptions, DAbPreVeneer and PAbPreVeneer). The defaults (Kernel/s/
 * Middle, UNDEF, ABORTP, ABORTD) write the registers into the exception
 * register block (OS_ChangeEnvironment 13), flatten the stacks and call
 * OS_GenerateError, so the error reaches the program's error handler.
 * SharedCLibrary installs handlers of its own there (RISC_OSLib kernel/s/
 * k_body, IIHandler, PAHandler, DAHandler), which make signals of them.
 * FPEmulator raises a floating point trap as an error through
 * OS_GenerateError (HWSupport/FPASC/vensrc/riscos/end, handle_exception).
 * SharedCLibrary raises its own two, x / 0 and the stack overflow, the
 * same way (k_body, dividebyzero, StackOverflowFault).
 *
 * In ROSGD the exceptions are Linux's signals. The box's handler takes
 * SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP and makes RISC OS 5.30's
 * exception of each one that is the program's:
 *
 *   - x32 code faulting: an application's, the ROM C library's, or a C
 *     module's (capp.h). That is any code below 4 GB, which only x32 code
 *     is;
 *   - the runtime faulting on an arena address inside a SWI (or the SWI
 *     gate reading or writing a program's register block): a SWI given a
 *     bad pointer, as a kernel SWI aborts on one in RISC OS.
 *
 * Anything else is the runtime's own failure, and goes to the fatal report
 * (boot/main.c), which powers the box off. That is the runtime's own
 * pointers, a fault on a thread that does not hold the runtime's lock,
 * background work, compiled ObjAsm outside any SWI, and a fault while one
 * is being delivered.
 *
 * So a SWI's code may fault on its caller's memory, and the error unwinds
 * it (longjmp). It must not touch a caller's memory while it holds a lock
 * of its own, which would stay held, with the worker threads and every
 * later call waiting on it for ever. (modules/acornhttp's URL_ReadData
 * tries the caller's buffer before it takes its lock.) The runtime's own
 * lock is not one of these: the thread that faulted goes on holding it, as
 * it should, into the error's handler.
 *
 * What each becomes (5.30's numbers and texts, recorded on the farm by
 * tests/capps/faults with the same probe built for ARM), and where it goes:
 *
 *   SIGSEGV or SIGBUS, a data access  &80000002 "Internal error: abort on
 *                                     data transfer at &<pc>"  (data abort)
 *   SIGSEGV fetching the instruction  &80000001 "Internal error: abort on
 *     (a call to an unmapped or       instruction fetch at &<pc>"
 *     non-executable address, 0 too)  (prefetch abort)
 *   SIGTRAP, int3                     &80000001, at the int3 (prefetch
 *                                     abort: 5.30's BKPT is one)
 *   SIGILL; SIGSEGV of a privileged   &80000000 "Internal error: undefined
 *     instruction (#GP: hlt, cli ...) instruction at &<pc>" (undefined)
 *   SIGFPE, integer division by 0     &80000020 "Divide by zero" (the
 *                                     library's error: k_body, C06), to the
 *                                     error handler
 *   SIGFPE, integer division          no error: the quotient truncated and
 *     overflow (INT_MIN / -1)         the remainder 0, as ARM's division and
 *                                     the library's give them. The
 *                                     instruction is finished here and the
 *                                     code goes on
 *   SIGFPE, SSE or x87 trap           &80000200 + n, FPEmulator's: invalid
 *                                     operation, overflow, division by
 *                                     zero, underflow, inexact operation
 *                                     ("Floating point exception: ..."), to
 *                                     the error handler
 *   SIGSEGV in the stack's guard      &80000021 "Not enough memory, stack
 *     (ros_capp_stack, capp.h)        overflow" (the library's: k_body's
 *                                     StackOverflowFault, C45), to the
 *                                     error handler, on the emergency stack
 *   SIGSEGV in the SVC stack's guard  a data abort: the lowest page of the
 *     page (arena.h)                  SVC stack is inaccessible, so a
 *                                     nested entry's x32 code, or compiled
 *                                     code in a SWI, running away down it
 *                                     faults there rather than running into
 *                                     the system heap below
 *   the runtime, on a SWI's pointer   a data abort, at the return address of
 *                                     the program's call into the OS (the
 *                                     runtime has no 32-bit pc of its own;
 *                                     5.30 gives the kernel's, in the ROM)
 *
 * The pc in a text is the faulting instruction's. x86 has no pipeline
 * offset (ARM's dump holds pc+8 or pc+4, and its texts subtract it).
 *
 * Where it goes (the three aborts): to the environment's handler for it,
 * if that is not the default and is x32 code, either the application's or
 * the library's (as SharedCLibrary's are). It is delivered as capp.h says
 * (ros_capp_deliver_exception), with the register block, and the
 * exception register block is not written, as the kernel does not write it
 * for a handler of the program's own. Otherwise, the default applies. The
 * block is written where OS_ChangeEnvironment 13 names (the kernel's own,
 * DUMPER, if that is not memory the program may write, which then becomes
 * the one named), and the error is raised as a non-X SWI's is (error.h) to
 * a C handler of the runtime's, or the program's error handler
 * (environment.c). For a C application that handler is delivered with
 * everything since it was entered flattened (capp.h). The error buffer's
 * first word, the pc of the error, is the text's. The register block,
 * R0-R15 then the PSR, comes from x86-64's:
 *
 *   R0 rdi   R1 rsi   R2 rdx   R3 rcx   R4 r8    R5 r9    R6 rax   R7 rbx
 *   R8 r12   R9 r13   R10 r14  R11 rbp  R12 r10  R13 rsp  R14 r15  R15 rip
 *   PSR: the mode the code was entered in (USR, or SVC for a module's
 *        code) and N Z C V from SF ZF CF OF
 *
 * (r11, the scratch register of the gate's trampolines and PLTs, is left
 * out. The report has it.) For the runtime faulting in a SWI, the block
 * is the registers the program called the OS with, with R15 its R14 and
 * the mode SVC, as 5.30's abort in the kernel has it.
 *
 * Alignment: x86-64 does unaligned loads and stores, and ROSGD lets it
 * (capp.h, "Alignment"). x32 code that turns on EFLAGS.AC gets SIGBUS on
 * one, which is a data abort as above.
 *
 * Each one is reported on the serial console (stderr), every line
 * "rosgd: exception: ...". The line gives the kind, where it happened, the
 * address, the error, the registers, and where it was delivered. Where it
 * happened is symbolised: x32 code from its image's symbol table, the
 * runtime's from /init's. For the runtime, it also gives the SWI it was in
 * and the last SWIs. tests/lib/boxrun.py collects these, and they fail
 * nothing.
 */
#ifndef ROSGD_FAULT_H
#define ROSGD_FAULT_H

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>

#define ROS_ERR_UNDEFINED_INSTRUCTION 0x80000000u
#define ROS_ERR_INSTRUCTION_ABORT     0x80000001u
#define ROS_ERR_DATA_ABORT            0x80000002u
#define ROS_ERR_DIVIDE_BY_ZERO        0x80000020u
#define ROS_ERR_STACK_OVERFLOW        0x80000021u
#define ROS_ERR_FP_BASE               0x80000200u   /* + 0 invalid, 1 overflow, 2 division
                                                       by zero, 3 underflow, 4 inexact */

/* The fatal report, for what is not the program's */
typedef void ros_fault_fatal_fn(int sig, siginfo_t *si, void *context);

/* Take SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP: the program's become
 * exceptions, the rest go to fatal.  The box only (hosted, nothing is
 * done). */
void ros_fault_init(ros_fault_fatal_fn *fatal);

/* How many have been made exceptions, and the last one's report (for
 * checks) */
unsigned ros_fault_count(void);
const char *ros_fault_last_report(void);

/* A thread outside RISC OS can catch its own faults. Such a thread is a
 * Worker module thread (modules/worker), which runs a job's kernel on
 * memory a task may take away (a dynamic area removed, a page the slot
 * pool cannot supply), or an application's own kernel. With
 * ros_fault_escape set, a SIGSEGV, SIGBUS, SIGILL, SIGTRAP or SIGFPE on
 * the thread records the signal, the address and the pc, and siglongjmps
 * to jb, which the thread set with sigsetjmp(jb, 1). That job fails and
 * the box does not. Nothing else of the handler runs. NULL (the default)
 * for every other thread. */
struct ros_fault_escape {
    sigjmp_buf jb;
    int sig;
    uint64_t addr;
    uint64_t pc;
};
#define ROS_FAULT_ESCAPES(sig) \
    ((sig) == SIGSEGV || (sig) == SIGBUS || (sig) == SIGILL || (sig) == SIGTRAP || (sig) == SIGFPE)
extern _Thread_local struct ros_fault_escape *ros_fault_escape;

/* The ARM container's JIT (runtime/armrun): when set, the
 * box's handler offers it every SIGSEGV and SIGBUS first. A fault in its
 * translated code on an arena address is the guest's memory access. The
 * JIT takes it back into its slow path (the context is changed: the
 * handler returns into it) and answers 1. Anything else is the handler's,
 * and the answer is 0. */
extern int (*ros_fault_jit)(int sig, siginfo_t *si, void *context);

#endif
