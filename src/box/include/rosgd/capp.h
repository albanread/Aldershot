/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* capp.h -- C applications: the loader and the gate (runtime/capp.c).
 *
 * A C application is an x32 image: ILP32 code for x86-64, with every
 * pointer an arena address. It is linked by roscc as an ELF32 EM_X86_64
 * executable at &8000, with OS/ABI 255 and a "ROSGD" note of version 1,
 * flags 0 (roscc's src/x32.rs), and filed as an Absolute (&FF8). An ELF32
 * x86-64 image without them, such as a Linux x32 program, another version
 * or unknown flags, is refused before anything of it runs. *Run of one does
 * what FileSwitch's Run_Absolute8000File does for an ARM Absolute
 * (FileSys/FileSwitch/s/FSControl):
 *
 *   - the memory: the task's application space (ros_task_give_slot, the
 *     Wimp_SlotSize path), grown by Wimp_SlotSize to hold the image, its
 *     zero-initialised data and 8K more (CheckAIFMemoryLimit's margin), and
 *     OS_ValidateAddress over all of it. What does not fit is refused, &411
 *     "No writable memory at this address", before anything is started.
 *     Outside the desktop (Wimp_ReadSysInfo 11 = 0) nothing grows the
 *     slot, and *WimpSlot sets the limit, as it does for an Absolute;
 *   - StartApplication (OS_FSControl 2), with its strings in the RMA: the
 *     command line exactly as *Run was given it becomes OS_GetEnv's, and
 *     the CAO is &8000;
 *   - the PT_LOAD segments read from the file straight to their addresses
 *     (OS_GBPB; only the headers pass through the RMA), the rest zeroed,
 *     and the code synchronised (__builtin___clear_cache, as
 *     OS_SynchroniseCodeAreas does);
 *   - the entry point entered in user mode. When it ends, by OS_Exit, an
 *     error its handler does not survive, or a return from the entry,
 *     *Run returns, as for any application ROSGD enters from a command.
 *
 * The hosted build has no 32-bit-pointer code. It refuses such an image
 * with an error that says so. An &FF8 file that is not one is ARM code,
 * refused as before.
 *
 * ---- the contract an x32 program is built against (ABI v1) ----------------
 *
 * Entry, at the ELF entry point, as a SysV function called from the gate
 * page: %edi = OS_GetEnv's command line, %esi = its tail (after the name
 * and spaces), %rsp = the RAM limit (OS_GetEnv R1) less 8, and (%rsp) =
 * ROS_CAPP_EXIT, so a plain `ret` from the entry exits as FileSwitch's
 * ReturnFromAbsoluteCode does (OS_Exit, R0-R2 zero). MXCSR is &1F80, the
 * x87 control word &37F and its stack empty, the direction flag clear, and
 * every other register 0: the general registers, and xmm0-15 (ymm0-15
 * whole where the CPU has AVX).
 * (RISC OS 5.30 enters an Absolute with R0-R3 corrupt, R12 = R13 =
 * &80000000 and R14 its return: bigmac7's record, tests/capps/farm.)
 *
 * The SWI gate, at ROS_CAPP_GATE, is a SysV function:
 *
 *     uint64_t gate(uint32_t swi, const uint32_t in[10], uint32_t out[10]);
 *
 * R0-R9 come from in, the SWI is called in user mode with R13 the caller's
 * stack pointer (the kernel banks it: runtime/swi.c), and R0-R9 go to out
 * (which may be in). It returns the PSR the SWI left in the high word, with
 * N Z C V in its top four bits and USR mode. If V is set, the error
 * block's address (R0) is in the low word, else 0. The program's MXCSR and
 * x87 control word are saved across it and the runtime's used inside.
 * %rbx, %rbp and %r12-%r15 are preserved, as for any SysV call. A SWI
 * without the X bit whose error is not handled goes to the program's error
 * handler and does not return. The SWI is entered with N Z C V clear.
 *
 * ROS_CAPP_GATE_PSR is the same with a fourth argument,
 *
 *     uint64_t gate_psr(uint32_t swi, const uint32_t in[10], uint32_t out[10], uint32_t psr);
 *
 * whose top four bits are the N Z C V the SWI is entered with. A SWI that
 * leaves a flag alone returns it as it came, as an ARM's SWI does. (The
 * ROM C library's veneers enter SWIs with the flags RISC_OSLib's leave
 * there: _kernel_swi with C set, from its TST a1, #&80000000.)
 *
 * ---- the same contract on AArch64: A64X32 ---------------------------------
 *
 * roscc links the same C for the Apple Silicon box as A64X32, which is
 * AArch64 with 32-bit pointers (roscc's src/a64.rs), into the same file
 * format: an ELF32 executable at &8000, OS/ABI 255, the "ROSGD" note of
 * version 1 with the same flags, and one PT_LOAD (p_align 4096).
 * e_machine is EM_AARCH64. The note's version is the contract's, and the
 * contract has a form for each machine, which e_machine names. So an
 * A64X32 image needs no version or flag of its own, and a box refuses the
 * other machine's image by its ELF header, as ros_capp_is_image does (the
 * AArch64 box knows an x32 image and refuses it by name). This is the form
 * for EM_AARCH64, which the AArch64 runtime implements
 * (runtime/arch/aarch64):
 *
 * Entry, at the ELF entry point, as an AAPCS64 function: w0 = OS_GetEnv's
 * command line, w1 its tail, sp = the RAM limit (16-aligned), x30 =
 * ROS_CAPP_EXIT, so a `ret` exits. x16 (IP0) is the entry point, which the
 * branch into it uses, and x18 is the task's static base (0 before it
 * registers). FPCR is 0 (round to nearest, no trap enabled) and so is
 * FPSR, and every other register is 0, including v0-v31 and NZCV. A
 * delivered handler gets the program's FPCR instead, with its traps still
 * enabled (FPSR clear).
 *
 * The gates, ROS_CAPP_GATE and ROS_CAPP_GATE_PSR, are AAPCS64 functions
 * with the prototypes above and the same results: w0 the SWI, x1 and x2
 * the blocks (arena addresses, zero-extended), w3 the flags, and x0 the
 * PSR in its high word. x19-x29, sp and d8-d15 are preserved, as for any
 * AAPCS64 call, and so is x18.
 *
 * The static base is x18, where x32's is %gs: the thread pointer at the top
 * of the client's static block, which SharedCLibrary's registration sets
 * and every entry into A64X32 code sets as it sets %gs (a library handler
 * its R12, a module its range's base). All A64X32 code is compiled
 * -ffixed-x18, and roscc refuses code that writes it. The runtime's own
 * code may use it, so every entry, and the gate on its way back, loads
 * x18 with the base in force on the thread. That is the caller's, unless
 * the SWI registered a new one (LibInitAPCS_32). The stubs are x32's
 * tables, with 8-byte entries after the slots.
 *
 * The gate runs the SWI with FPCR 0 and puts the caller's FPCR and FPSR
 * back. Apple's cores implement FPCR's trap enables (ros_capp_fp_traps
 * probes for them), so a program's enabled FP exceptions trap in
 * hardware, as x32's do through MXCSR.
 *
 * A module (ET_DYN at 0) moves only by whole 4 KB pages (adrp is
 * PC-relative in pages). Its PT_LOAD asks p_align 4096, so it is placed at
 * a page boundary, and its fix-ups are R_AARCH64_P32_RELATIVE (&B7), base +
 * addend into a 32-bit word. The C library image is the same format at
 * ROS_CROM_BASE, e_machine EM_AARCH64.
 *
 * Faults: C's division by zero is `brk #0x5503` (the flag table's trap; the
 * library's _kernel_udiv and the rest raise the same), and an entry the
 * library does not define is `udf #0` (fault.h's table; the classifier is
 * runtime/arch/aarch64/fault.c.inc, by the ESR and the instruction).
 *
 * Nested entries and delivered handlers follow the x32 descriptions below,
 * with AArch64's registers: the block's address in w0 (x1 0), sp the
 * block's (16-aligned), and x30 &FEEFF020 or &FEEFF030.
 */
/*
 * ---- entries into x32 code ------------------------------------------------
 *
 * The OS calls user code through ros_call(addr), for handlers, vector
 * claimants, callbacks and a module's veneers. The dispatcher, finding no
 * compiled ObjAsm at addr, asks here (ros_capp_call). x32 code is:
 *
 *   - an application's image, [its lowest executable segment, its
 *     highest): the application of the task whose memory is at &8000 now
 *     (per-task state, below), whichever thread asks, as on RISC OS the
 *     code at an address in application space is the paged-in task's;
 *   - a range registered with ros_capp_code_add: the ROM C library (in the
 *     C ROM, ROS_CROM_BASE up) or a C module's code in the RMA;
 *   - a block BASIC's assembler wrote in x86-64 (*BasicAsmCPU X64), which
 *     CALL and USR enter nested with R0-R7 = A%-H%.
 *
 * In a box on AArch64 (ROS_CAPP_A64) the same code is A64X32 (the
 * contract's EM_AARCH64 form, above). A block BASIC wrote in AArch64
 * (*BasicAsmCPU A64) is called as an AAPCS64 function of its own contract:
 * x0 the register block, x1 the SWI entry, on the native stack
 * (runtime/arch/aarch64/capp.c.inc, call_assembled).
 *
 * Nested (a handler that returns: UpCall, Escape, Event, a vector claimant,
 * a module veneer): the code is called as a SysV function
 *
 *     void entry(uint32_t regs[17]);        R0-R15, then the PSR
 *
 * with %edi the block's arena address, on the SVC stack below where the
 * caller stands (the task's, as RISC OS runs such handlers: the kernel
 * enters the UpCall handler from UpCallV's default owner, CallUpcallHandler
 * in Kernel/s/Kernel), and its return address &FEEFF020. R15 in the block
 * is R14 on the way in. What the code leaves in R0-R15 and the PSR's
 * N Z C V comes back, so a claimant claims as a compiled one does, with R15
 * set to the word at [R13] and R13 up 4. The code's SWIs go through the
 * gate in the mode it was entered in, on a native stack below this entry.
 * Entries nest. MXCSR is &1F80 and the x87 control word &37F on entry, and
 * the runtime's afterwards.
 *
 * The CallBack handler is not entered this way. SharedCLibrary's
 * (RISC_OSLib kernel/s/k_body, CallBackHandler) never returns. It reloads
 * the user registers from its dump, or runs the program's event handlers
 * in user mode on the program's stack, and those may longjmp. A nested
 * entry's mode, stacks, entry chain and base go back only on a return
 * through &FEEFF020, so it would leave them as the entry set them. The
 * CallBack handler would need a "resume" way out: the entry chain unwound
 * as ros_capp_unwind does, then user mode on the dump's sp.
 *
 * Delivered (a handler that does not return: error, exit): the running
 * application's own, entered as the kernel enters them. Every native
 * frame since the program was entered is flattened first (the SWIs it was
 * in, the runtime, nested entries), with the SVC stack flat and the SWI
 * depth 0. The handler is entered in user mode, on its stack below where
 * it last called the OS itself, with the same block, R12 = the handler's
 * R12, and R0-R2 as the kernel gives them. For the error handler R0 = its
 * R12 (the kernel's ErrHandler gives R0 the handler's R12 and R10-R12 the
 * foreground's; x32 code has no foreground R10-R12, so R12 is the
 * handler's too). For the exit handler R0 = 0, R1 = OS_Exit's R1 and R2 is
 * the return code, which is OS_Exit's R2 if R1 is "ABEX" and else 0
 * (Kernel/s/Kernel, SEXIT); SharedCLibrary's ExitHandler passes that on to
 * the next OS_Exit. So the handler may longjmp into the program, as no
 * native frame is left to skip. A delivered handler that returns (to
 * &FEEFF030) ends the program, as a default one would.
 *
 * The static base (%gs, the library's statics' base) is set on every entry
 * and put back after it. A library handler gets its R12 (the library
 * registers its handlers with R12 = the client's base), a module's code
 * gets its range's base, and an application's own code gets its task's
 * base (ros_capp_task_base, set by registration). Delivery sets the
 * task's base whatever the error unwound past, and so does any unwinding
 * of the runtime's (ros_resume_unwind). Only the runtime writes %gs, and
 * the runtime itself never reads it.
 */
/*
 * ---- per-task state -------------------------------------------------------
 *
 * What an x32 application running in a task is to the runtime is kept in
 * the task, struct ros_task (runtime/task.c), and not in the thread. That
 * is its static base, where it stood when it last called the OS (the saved
 * sp), the native stack its SWIs run on (the native sp), its code and the
 * memory it was loaded into. So any thread holding the lock finds a task's
 * own. The Wimp, switching from task A to task B on A's thread, pages B in
 * and calls B's post-filter there (Desktop/Wimp s/Wimp07, ExitPoll), and
 * the background thread may run a task's handler while the task waits.
 * x32 code in application space is the task's whose memory is mapped at
 * &8000. That is told by the memory itself (the memfd, as AMB's node or the
 * task's own slot holds it, and adoption keeps it) and not by which thread
 * runs, and the code is entered with that task's base. A task switch moves
 * none of it. This is ROSGD's rule: what multiplexes shared state keeps it
 * per task, and task switches need nothing from it. (Each task is a thread,
 * and Linux keeps %gs per thread, so the base in force after Wimp_Poll is
 * the task's own without the switch touching it.)
 *
 * Its error and exit handlers are delivered only on the task's own thread,
 * whose native frames delivery flattens, and only while its memory is at
 * &8000. Another task's handler met in the Wimp's switch is not delivered
 * there (ros_capp_deliver returns). That handler may be in that task's
 * image, or the library's registered with that task's R12, which cannot
 * tell whose it is when two copies of one image run. RISC OS's mapslotin
 * makes the paged-in task's handlers the ones in force.
 * Nested entries, into the task's code or anyone's, keep their own saved
 * and native sp, on the thread that makes them, so the task's saved sp
 * stays where its program last called the OS itself, which is where
 * delivery puts the handler.
 */
#ifndef ROSGD_CAPP_H
#define ROSGD_CAPP_H

#include <stdint.h>

#include "rosgd/error.h"

struct ros_cpu;

/* Whether this build runs 32-bit-pointer code: a box's (x32 on Linux on
 * x86-64, A64X32 on Linux on AArch64) and never a hosted one, whatever it
 * is hosted on. (#if, not #ifdef: it is 0 there.) */
#if !defined(ROS_ARENA_HOSTED) && (defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)
#define ROS_CAPP_NATIVE 1
#else
#define ROS_CAPP_NATIVE 0
#endif

/* Whether this is the box on AArch64, the Apple Silicon box. Its C
 * applications, library and modules are A64X32 (the contract's EM_AARCH64
 * form, above), and it runs the AArch64 code BASIC's assembler writes
 * (*BasicAsmCPU A64) too. The machine code is runtime/arch/aarch64/'s. */
#if ROS_CAPP_NATIVE && defined(__aarch64__)
#define ROS_CAPP_A64 1
#else
#define ROS_CAPP_A64 0
#endif

/* ROS_CAPP_A64_JIT: a hosted build on an Apple Silicon Mac that runs the
 * AArch64 code BASIC's assembler writes, from copies of it in a MAP_JIT
 * arena (runtime/arch/aarch64/capp_jit.c.inc, runtime/capp_trap.c). It is
 * BBC BASIC V for Mac's. Its build defines it (-DROS_CAPP_A64_JIT).
 * Nothing here does, and no box's build or the hosted test has it. */
#if defined(ROS_CAPP_A64_JIT) && \
    !(defined(ROS_ARENA_HOSTED) && defined(__aarch64__) && defined(__APPLE__))
#error "ROS_CAPP_A64_JIT is for a hosted build on an Apple Silicon Mac"
#endif

/* The machine a box's C images are for (their e_machine), and its name in
 * errors.  Hosted builds, which run none, know x32's. */
#if ROS_CAPP_A64
#define ROS_CAPP_EM      183u           /* EM_AARCH64: A64X32 */
#define ROS_CAPP_MACHINE "A64X32"
#define ROS_CAPP_ABI     "A64X32"
#else
#define ROS_CAPP_EM      62u            /* EM_X86_64: x32 */
#define ROS_CAPP_MACHINE "x86-64"
#define ROS_CAPP_ABI     "x32"
#endif

#if ROS_CAPP_A64
/* The FPCR trap enables this CPU implements: those of IOE, DZE, OFE, UFE,
 * IXE and IDE (&9F00) that read back set (runtime/arch/aarch64) */
uint32_t ros_capp_fp_traps(void);
#endif

/* The gate page, in the ROM's range, below the native entries: ROM images
 * end below it (rom.c) */
#define ROS_CAPP_GATE_PAGE 0xFEEFF000u
#define ROS_CAPP_GATE      (ROS_CAPP_GATE_PAGE + 0x00u)     /* the SWI gate */
#define ROS_CAPP_EXIT      (ROS_CAPP_GATE_PAGE + 0x10u)     /* where a returning entry goes */
#define ROS_CAPP_NESTED    (ROS_CAPP_GATE_PAGE + 0x20u)     /* a nested entry's return */
#define ROS_CAPP_ENDED     (ROS_CAPP_GATE_PAGE + 0x30u)     /* a delivered handler's return */
#define ROS_CAPP_GATE_PSR  (ROS_CAPP_GATE_PAGE + 0x40u)     /* the SWI gate, with the flags in */

/* The C ROM: the ROM C library's image, executed in place and mapped
 * there by the SharedCLibrary module (modules/sharedclib) */
#define ROS_CROM_BASE  0x72200000u
#define ROS_CROM_LIMIT 0x78000000u
/* Where the self-test's stand-in libraries go, clear of the ROM library:
 * the top of the C ROM (boot/selftest_capps.c, tests/capps/lib.ld) */
#define ROS_CROM_TEST  0x77000000u

#define ROS_ERR_CODE_TOO_LOW      0x408u    /* "Code runs too low" */
#define ROS_ERR_EXEC_NOT_IN_CODE  0x407u    /* "Execution address not within code" */
#define ROS_ERR_CORE_NOT_WRITABLE 0x411u    /* "No writable memory at this address" */

/* Whether the bytes are an ELF32, little-endian, EM_X86_64 image: one for
 * the C applications' loader to run or refuse (its OS/ABI and note are the
 * loader's to check), and never ARM code. */
int ros_capp_is_image(const void *bytes, uint32_t size);

/* *Run of an &FF8 file: path its canonical name, line the arena address of
 * the command line as *Run had it (the name as given, then the rest).  If
 * the file is not an ELF32 x86-64 image, *is_capp is 0 and nothing was
 * done; otherwise it has run (NULL) or was refused (the error). */
os_error *ros_capp_run(const char *path, uint32_t line, int *is_capp);
/* The memory an application loaded at &8000 and ending at end needs, as
 * FileSwitch's StartApplication checks wants it: the slot grown through
 * the Wimp when it gives application space (the ARM container's loader,
 * runtime/armrun/box.c, asks it too) */
os_error *ros_capp_memory_for(uint32_t end);

/* Map the gate page and write it: at arena set-up, in the box. */
int ros_capp_gate_init(void);

/* ---- entries --------------------------------------------------------------- */

enum { ROS_CAPP_TASK = 1, ROS_CAPP_LIBRARY, ROS_CAPP_MODULE, ROS_CAPP_ASSEMBLED };

/* x32 code at [lo, hi): ROS_CAPP_LIBRARY (entered with %gs = its R12) or
 * ROS_CAPP_MODULE (with %gs = base).  0, or an error if the table is full. */
os_error *ros_capp_code_add(uint32_t lo, uint32_t hi, int kind, uint32_t base);
void ros_capp_code_remove(uint32_t lo);
/* Code BASIC's assembler has just put at [lo, hi) in this machine's
 * instruction set: x86-64 (*BasicAsmCPU X64), entered as a module's code
 * is, with %gs 0; or, in a box on AArch64, AArch64 (*BasicAsmCPU A64). A
 * block assembled again, in a second pass or a loop, replaces the range
 * it overlaps or touches, so the table holds a block once. The result is
 * 0, or an error if the table is full; hosted, it does nothing. */
os_error *ros_capp_code_assembled(uint32_t lo, uint32_t hi);
/* Forget the assembled ranges [lo, hi) overlaps: the application space
 * handed to a new application, or a block that BASIC's assembler wrote
 * there in another CPU's code, since those are bytes they no longer
 * describe. */
void ros_capp_code_forget_assembled(uint32_t lo, uint32_t hi);
/* What addr is: ROS_CAPP_TASK, _LIBRARY, _MODULE, or 0 if not x32 code. */
int ros_capp_code(uint32_t addr);

/* The dispatcher's fallback: if addr is x32 code, call it nested with the
 * register block and return 1; else 0 (dispatch.c). */
int ros_capp_call(struct ros_cpu *s, uint32_t addr);

/* Deliver the running application's error (status 1) or exit (0) handler
 * at code, with R0-R2 = r[0..2] and R12 = r12 (the error's: r12, 0, 0;
 * the exit's: 0, R1, the return code). It returns only if code is not x32
 * code for the application running on this thread. That is the case if
 * it is not x32 code, or is another task's, or the memory at &8000 is not
 * this application's task's (environment.c then calls it as it calls
 * compiled code). */
void ros_capp_deliver(uint32_t code, uint32_t r12, const uint32_t r[3], int status);

/* The task's static base: set for this thread and recorded in the task
 * (struct ros_task) as its running application's, as SharedCLibrary's
 * registration does. It is for a SWI the program makes itself, outside
 * any nested entry, whose way out would put the entry's base back. */
void ros_capp_task_base(uint32_t base);
/* A C module's static base, as SharedCLibrary's module registration sets
 * it (LibInitModuleAPCS_32): the ROS_CAPP_MODULE range that holds addr
 * (the module's stub descriptors) gets base for every later entry, and
 * this thread's base is set now. The result is 0 if no module's range
 * holds addr, and nothing is set. */
int ros_capp_module_base(uint32_t addr, uint32_t base);
/* This thread's %gs base as the hardware has it (for checks). */
uint64_t ros_capp_gs(void);

/* The runtime is longjmping out to a frame at `to`: nested entries in the
 * frames it leaves are over, and the base, mode and stacks go back to what
 * the outermost of them found (dispatch.c, ros_resume_unwind). */
void ros_capp_unwind(const void *to);

/* ---- per-task state (above): a task's, in struct ros_task (task.h,
 * ros_task_capp) ---------------------------------------------------------------- */

struct ros_capp_gate_state {    /* the gate's assembler uses these at +0, +8, +16, +20 */
    uint64_t native_sp;         /* where a gate call moves the thread */
    uint64_t saved_sp;          /* the x32 sp at the latest gate call */
    uint32_t fpc;               /* the caller's floating point control at that call, */
    uint16_t fpc2;              /* which its delivered handlers get back (R2b): MXCSR and
                                   the x87 control word; on AArch64, FPCR and 0 */
};

struct ros_capp_task {
    struct ros_capp_gate_state gate;   /* its program's own (not a nested entry's) */
    uint32_t base;              /* its static base, registration's; 0 before */
    uint32_t code_lo, code_hi;  /* its image's code; 0, 0: none running */
    int mem_fd;                 /* the memory it was loaded into (the slot at */
    uint64_t mem_dev, mem_ino;  /* &8000 then), which keeps these while it lives */
    char name[64];              /* its leaf name, and its file on Linux (HostFS), */
    char file[256];             /* for a fault's symbols (R2b); "" if none */
};
/* ---- faults (fault.h) ------------------------------------------------------
 *
 * A fault in x32 code becomes an exception (runtime/fault.c): an error,
 * delivered as any other error, with the handler entered as above but with
 * two differences. Its stack is below the sp the fault had, where the
 * program stood, and not where it last called the OS (for a fault in a
 * nested entry, still the program's own sp from before the entry). And if
 * that stack is not memory it may write, the handler runs on the
 * emergency stack (ros_capp_stack), or with none on the top of its
 * application space, where its outermost frames are, which a handler's
 * longjmp would find overwritten. A stack overflow, which is a fault in
 * the guard that ros_capp_stack gave, is delivered on the emergency stack,
 * below the guard, so the frames above it stay whole for a longjmp.
 *
 * An undefined instruction, a prefetch abort or a data abort may have a
 * handler (OS_ChangeEnvironment 1, 2, 3) that is x32 code, either the
 * application's or the library's, as SharedCLibrary's trap handlers are.
 * That handler is delivered the same way (flattened, user mode, the
 * program's stack as for an error, the task's base, the program's floating
 * point), and called as
 *
 *     void handler(uint32_t regs[17]);     R0-R15, then the PSR
 *
 * with the registers as the fault had them (fault.h's table). R15 is the
 * faulting instruction's address, where an ARM handler finds pc+8 (data)
 * or pc+4 in R14, because x86 has no pipeline offset. The PSR is the
 * faulting code's mode and flags. A handler that returns ends the program,
 * as a delivered handler's return does. RISC OS enters these handlers in
 * abort or undefined mode on that mode's stack. x32 code has no such
 * modes, and what the library's handlers do next, which is to make a
 * signal and run its handler in user mode on the program's stack, is where
 * this starts.
 *
 * Every delivered handler, whether error, exit or exception, and whether
 * a fault or not, is entered with the program's own MXCSR and x87 control
 * word, as the gate last saw them or as the fault had them, with the
 * exception flags clear. The traps it enabled stay enabled, as
 * FPEmulator's FPSR does across an error on RISC OS (the farm's record,
 * tests/capps/faults; SharedCLibrary's handlers longjmp and trap again).
 * Nested entries still get &1F80 and &37F.
 *
 * Alignment: x86-64 does unaligned loads and stores, and ROSGD lets it.
 * RISC OS 5.30 aborts on an unaligned word load or store (the farm's
 * record, tests/capps/faults; SharedCLibrary makes it SIGSEGV) but not on a
 * halfword one. Making x32 code fault on them (EFLAGS.AC) would fault on
 * every misaligned access, including halfwords, which 5.30 allows, and the
 * 8-byte doubles at 4-byte alignment that RISC OS's ABI and its malloc
 * give. There is no way to tell the one from the others short of
 * emulating the instruction. So an unaligned word access gives the bytes
 * there, as memcpy would. Code that sets EFLAGS.AC itself gets its SIGBUS
 * as a data abort. The gate turns AC off for the runtime and back on for
 * the code. */

/* What the x32 code at addr is (as ros_capp_code), the start of its range
 * (the application's lowest code address, or a range's lo), and the Linux
 * file of its image, for its symbols (NULL if none is known). The result
 * is 0 if addr is no x32 code's. */
int ros_capp_where(uint32_t addr, uint32_t *lo, const char **file, const char **name);
/* The mode the x32 code running now was entered in: USR, or SVC */
uint32_t ros_capp_mode(void);
/* The native stack below the frame that entered the x32 code running now */
uint64_t ros_capp_native_stack(void);

/* The running application's stack, as SharedCLibrary's registration lays
 * it out: [guard_lo, guard_hi), page aligned in its application space,
 * made inaccessible and kept so while the program runs, whenever its
 * memory is mapped (arena.h, ros_slot_guard). There is also an emergency
 * stack whose top is emergency_sp (below the guard, or anywhere the
 * program may write). A data fault in the guard is a stack overflow:
 * "Not enough memory, stack overflow", delivered on the emergency stack.
 * guard_hi = guard_lo takes it away, and so does the program's end. */
os_error *ros_capp_stack(uint32_t guard_lo, uint32_t guard_hi, uint32_t emergency_sp);
/* Whether addr is in the running application's guard (the fault handler) */
int ros_capp_stack_overflow(uint32_t addr);

/* A fault in x32 code, about to be delivered: the handler's stack (the
 * fault's sp, or the emergency stack if overflow) and the program's
 * floating point control (MXCSR and the x87 control word; on AArch64,
 * FPCR and 0). Nothing if the fault is in a nested entry. */
void ros_capp_fault(uint32_t sp, uint32_t fpc, uint16_t fpc2, int overflow);

/* Whether code can take an exception as the running application's
 * handler (x32 code of its own or the library's); deliver it with the
 * register block, as above.  Returns only if it cannot. */
int ros_capp_exception_handler(uint32_t code);
void ros_capp_deliver_exception(uint32_t code, const uint32_t block[17]);

/* Whether the gate is copying a program's register block (in or out) now;
 * if so, the caller's sp and return address, and it is no longer (a fault
 * there is the program's). */
int ros_capp_gate_fault(uint32_t *sp, uint32_t *ret);

#endif
