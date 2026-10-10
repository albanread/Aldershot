/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* capp.c.inc -- the A64X32 gate (runtime/capp.c includes it where it
 * stands, in a box on AArch64). It is the twin of x86_64/capp.c.inc. It
 * holds the per-thread state the assembler reads and writes, the clean
 * state an entry loads, the gate and the ways in and out of an A64X32
 * program in A64 assembler, the static base in x18, the floating point
 * control (FPCR) that the shared code saves and loads, and the gate page's
 * trampolines. It also holds the support for the code BASIC's assembler
 * writes for AArch64, which is entered as any nested entry is and has the
 * gate as its SWI entry (below).
 *
 * The contract is capp.h's EM_AARCH64 form. In short, the gates are
 * AAPCS64 functions, and entries are AAPCS64 calls with sp 16-aligned.
 * x18 is the static base. A64X32 code never writes it (roscc refuses to),
 * and the runtime may use it as a temporary (AArch64 Linux does not
 * reserve it). So every way into A64X32 code, whether an entry or a return
 * from the gate, loads it from ros_capp_x18. That is the base in force on
 * this thread, which is what set_gs last set, as %gs is on x86-64. The
 * runtime's own thread pointer is TPIDR_EL0. A64X32 code never reads it
 * (roscc turns its mrs into a mov from x18), so the two never meet. */

/* Per thread, read and written by the assembler below (local-exec TLS: the
 * runtime is a static PIE):
 *   ros_capp_user       the gate state of the A64X32 code running on this
 *                       thread now (capp.h). It holds the native sp, where
 *                       a gate call moves the thread. It holds the saved
 *                       sp, which is the A64X32 sp at its latest gate
 *                       call. It also holds fpc, the caller's FPCR at that
 *                       time.
 *   ros_capp_ret_sp     the native frame the innermost nested entry
 *                       returns to
 *   ros_capp_x18        the static base the thread's A64X32 code runs with
 *   ros_capp_entry_fp   whether ros_capp_enter loads ros_capp_entry_fpcr
 *                       after the clean state. A delivered handler gets the
 *                       program's FPCR. */
__attribute__((used)) _Thread_local struct ros_capp_gate_state *ros_capp_user;
__attribute__((used)) _Thread_local uint64_t ros_capp_ret_sp;
__attribute__((used)) _Thread_local uint64_t ros_capp_x18;
__attribute__((used)) _Thread_local uint32_t ros_capp_entry_fp;
__attribute__((used)) _Thread_local uint32_t ros_capp_entry_fpcr;
static _Thread_local uint16_t ros_capp_entry_fpc2;     /* x87's, so unused here */
/* Whether the gate is copying a register block, and its caller's sp and
 * return address. A fault during the copy is delivered as the caller's. */
static _Thread_local int gate_copying;
static _Thread_local uint32_t gate_sp, gate_ret;

_Static_assert(offsetof(struct ros_capp_gate_state, native_sp) == 0, "the gate reads it at +0");
_Static_assert(offsetof(struct ros_capp_gate_state, saved_sp) == 8, "the gate writes it at +8");
_Static_assert(offsetof(struct ros_capp_gate_state, fpc) == 16, "the gate writes it at +16");
_Static_assert(offsetof(struct ros_capp_gate_state, fpc2) == 20, "the gate writes it at +20");

/* The floating point control is FPCR. It holds the rounding mode,
 * flush-to-zero, default NaN and the trap enables. Apple's cores do not
 * implement the trap enables, and they read back 0 (ros_capp_fp_traps).
 * FPSR holds the cumulative flags and is the program's too. The gate keeps
 * it across a SWI on its own frame, and a delivered handler starts with it
 * clear, as x86's MXCSR flags are. */
static inline uint32_t fpcr_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (uint32_t)v;
}
static inline void fpcr_set(uint32_t v)
{
    __asm__ volatile("msr fpcr, %0" : : "r"((uint64_t)v));
}
#define FP_CTL_NOW()      fpcr_now()
#define FP_CTL2_STORE(v)  ((v) = 0)
#define FP_LOAD(c, c2)    ((void)(c2), fpcr_set(c))
#define FP_CLEAN_CTL      0u
#define FP_CLEAN_CTL2     0
#define FP_NO_FLAGS(c)    (c)           /* FPCR holds no flags: FPSR does */
#define ENTRY_FPC         ros_capp_entry_fpcr
#define ENTRY_FPC2        ros_capp_entry_fpc2

uint64_t ros_capp_swi(uint32_t number, uint32_t in, uint32_t out, uint64_t sp, uint32_t psr,
                      uint64_t lr);
/* The caller's return address, for a gate call's R14 and R15. It is the
 * caller's x30, which the gate passes as the sixth argument. */
#define GATE_RET(sp, lr)  ((void)(sp), (uint32_t)(lr))

__attribute__((noreturn)) void ros_capp_returned_c(void);
__attribute__((noreturn)) void ros_capp_ended_c(void);
void ros_capp_gate(void);
void ros_capp_gate_psr(void);
void ros_capp_returned(void);
void ros_capp_nested_return(void);
void ros_capp_ended(void);
__attribute__((noreturn)) void ros_capp_enter(uint64_t entry, uint64_t sp, uint64_t a0, uint64_t a1,
                                              uint64_t ret);
void ros_capp_call_x32(uint64_t code, uint64_t block, uint64_t sp);

/* Loads a thread-local's address into reg (local-exec). */
#define TLS_ADDR(reg, sym)                                                                  \
    "   mrs " reg ", tpidr_el0\n"                                                           \
    "   add " reg ", " reg ", #:tprel_hi12:" sym ", lsl #12\n"                              \
    "   add " reg ", " reg ", #:tprel_lo12_nc:" sym "\n"

/* How A64X32 code is entered, so that nothing of the runtime's shows
 * through. Every general register that the entry does not name is zeroed.
 * Those are x2-x15, x17 and x19-x29. The entry sets x0, x1, x16 (the
 * target), x18 (the base), x30 (the return) and sp. The vector registers
 * v0-v31 are zeroed and NZCV is cleared. FPCR and FPSR are left for the
 * entry to set. */
#define A64_CLEAN_STATE                                                                     \
    "   mov x2, xzr\n   mov x3, xzr\n   mov x4, xzr\n   mov x5, xzr\n"                      \
    "   mov x6, xzr\n   mov x7, xzr\n   mov x8, xzr\n   mov x9, xzr\n"                      \
    "   mov x10, xzr\n  mov x11, xzr\n  mov x12, xzr\n  mov x13, xzr\n"                     \
    "   mov x14, xzr\n  mov x15, xzr\n  mov x17, xzr\n  mov x19, xzr\n"                     \
    "   mov x20, xzr\n  mov x21, xzr\n  mov x22, xzr\n  mov x23, xzr\n"                     \
    "   mov x24, xzr\n  mov x25, xzr\n  mov x26, xzr\n  mov x27, xzr\n"                     \
    "   mov x28, xzr\n  mov x29, xzr\n"                                                     \
    "   movi v0.16b, #0\n  movi v1.16b, #0\n  movi v2.16b, #0\n  movi v3.16b, #0\n"         \
    "   movi v4.16b, #0\n  movi v5.16b, #0\n  movi v6.16b, #0\n  movi v7.16b, #0\n"         \
    "   movi v8.16b, #0\n  movi v9.16b, #0\n  movi v10.16b, #0\n movi v11.16b, #0\n"        \
    "   movi v12.16b, #0\n movi v13.16b, #0\n movi v14.16b, #0\n movi v15.16b, #0\n"        \
    "   movi v16.16b, #0\n movi v17.16b, #0\n movi v18.16b, #0\n movi v19.16b, #0\n"        \
    "   movi v20.16b, #0\n movi v21.16b, #0\n movi v22.16b, #0\n movi v23.16b, #0\n"        \
    "   movi v24.16b, #0\n movi v25.16b, #0\n movi v26.16b, #0\n movi v27.16b, #0\n"        \
    "   movi v28.16b, #0\n movi v29.16b, #0\n movi v30.16b, #0\n movi v31.16b, #0\n"        \
    "   msr nzcv, xzr\n"

__asm__(
    ".text\n"
    /* The gate, called from A64X32 code on its own stack. w0 is the SWI
     * number, w1 is the address of the R0-R9 input block, w2 is the
     * address of the R0-R9 output block (both arena addresses) and x30 is
     * the return address. The gate saves the code's sp and takes the
     * native sp from the calling code's gate state (ros_capp_user). The
     * frame on the native stack is: +0 x29, x30; +16 the caller's sp; +24
     * its FPCR; +32 its FPSR. The caller's FPCR is also recorded in its
     * gate state, for its delivered handlers. The runtime runs with FPCR 0
     * (round to nearest, no flush-to-zero) and its own FPSR. The caller's
     * FPCR and FPSR both come back afterwards. x18 comes back as the base
     * in force (ros_capp_x18), which is the caller's unless the SWI
     * registered a new one (SharedCLibrary's LibInitAPCS_32). The SWI is
     * entered with N Z C V clear. Through ros_capp_gate_psr it is entered
     * with the N Z C V of w3 (its top four bits). The C function called is
     * ros_capp_swi(number, in, out, sp, psr, lr). */
    ".globl ros_capp_gate_psr\n"
    ".type ros_capp_gate_psr,%function\n"
    "ros_capp_gate_psr:\n"
    "   mov w4, w3\n"
    "   b 1f\n"
    ".size ros_capp_gate_psr, .-ros_capp_gate_psr\n"
    ".globl ros_capp_gate\n"
    ".type ros_capp_gate,%function\n"
    "ros_capp_gate:\n"
    "   mov w4, wzr\n"
    "1: mov x9, sp\n"
    TLS_ADDR("x10", "ros_capp_user")
    "   ldr x10, [x10]\n"
    "   str x9, [x10, #8]\n"                    /* saved_sp */
    "   ldr x11, [x10]\n"                       /* native_sp */
    "   mov sp, x11\n"
    "   sub sp, sp, #48\n"
    "   stp x29, x30, [sp, #0]\n"
    "   mrs x12, fpcr\n"
    "   mrs x13, fpsr\n"
    "   str x9, [sp, #16]\n"
    "   stp x12, x13, [sp, #24]\n"
    "   str w12, [x10, #16]\n"                  /* fpc: the caller's FPCR */
    "   strh wzr, [x10, #20]\n"
    "   msr fpcr, xzr\n"
    "   mov x29, sp\n"
    "   mov w1, w1\n"                           /* the blocks' addresses, zero-extended */
    "   mov w2, w2\n"
    "   mov x3, x9\n"
    "   mov x5, x30\n"                          /* its return, for R14 and R15 */
    "   bl ros_capp_swi\n"
    "   ldp x12, x13, [sp, #24]\n"
    "   msr fpcr, x12\n"
    "   msr fpsr, x13\n"
    TLS_ADDR("x9", "ros_capp_x18")
    "   ldr x18, [x9]\n"
    "   ldr x9, [sp, #16]\n"
    "   ldp x29, x30, [sp, #0]\n"
    "   mov sp, x9\n"
    "   ret\n"
    ".size ros_capp_gate, .-ros_capp_gate\n"

    /* A return from the program's entry, as FileSwitch's
     * ReturnFromAbsoluteCode is. */
    ".globl ros_capp_returned\n"
    ".type ros_capp_returned,%function\n"
    "ros_capp_returned:\n"
    TLS_ADDR("x10", "ros_capp_user")
    "   ldr x10, [x10]\n"
    "   ldr x11, [x10]\n"
    "   mov sp, x11\n"
    "   msr fpcr, xzr\n"
    "   mov x29, xzr\n"
    "   bl ros_capp_returned_c\n"
    "   brk #1\n"
    ".size ros_capp_returned, .-ros_capp_returned\n"

    /* A return from a delivered handler. The program is over. */
    ".globl ros_capp_ended\n"
    ".type ros_capp_ended,%function\n"
    "ros_capp_ended:\n"
    TLS_ADDR("x10", "ros_capp_user")
    "   ldr x10, [x10]\n"
    "   ldr x11, [x10]\n"
    "   mov sp, x11\n"
    "   msr fpcr, xzr\n"
    "   mov x29, xzr\n"
    "   bl ros_capp_ended_c\n"
    "   brk #1\n"
    ".size ros_capp_ended, .-ros_capp_ended\n"

    /* Into A64X32 code that does not come back here. x0 is its entry, x1
     * is its sp (16-aligned), w2 and w3 are its first two arguments and w4
     * is its return address (&FEEFF010 for the program's entry, &FEEFF030
     * for a delivered handler). The gate's calls from it go below this
     * frame, at the native sp of the gate state in use (ros_capp_user, the
     * task's). The state is the clean state with FPCR 0 and FPSR 0. For a
     * delivered handler (ros_capp_entry_fp non-zero) FPCR is the program's
     * instead. x18 is the base in force. */
    ".globl ros_capp_enter\n"
    ".type ros_capp_enter,%function\n"
    "ros_capp_enter:\n"
    "   mov x9, sp\n"
    "   sub x9, x9, #64\n"
    "   and x9, x9, #0xFFFFFFFFFFFFFFF0\n"
    TLS_ADDR("x10", "ros_capp_user")
    "   ldr x10, [x10]\n"
    "   str x9, [x10]\n"
    "   mov x16, x0\n"
    "   mov sp, x1\n"
    "   mov w0, w2\n"
    "   mov w1, w3\n"
    "   mov w30, w4\n"                          /* zero-extended */
    TLS_ADDR("x9", "ros_capp_entry_fp")
    "   ldr w9, [x9]\n"
    TLS_ADDR("x10", "ros_capp_entry_fpcr")
    "   ldr w10, [x10]\n"
    "   cmp w9, #0\n"
    "   csel w10, w10, wzr, ne\n"
    "   msr fpcr, x10\n"
    "   msr fpsr, xzr\n"
    TLS_ADDR("x9", "ros_capp_x18")
    "   ldr x18, [x9]\n"
    A64_CLEAN_STATE
    "   br x16\n"
    ".size ros_capp_enter, .-ros_capp_enter\n"

    /* A nested entry. x0 is the code, w1 is the register block and x2 is
     * the A64X32 sp (16-aligned). The code is called with x0 holding the
     * block and x1 holding 0, and it returns to &FEEFF020, which comes
     * back to ros_capp_nested_return and so here. The native frame holds
     * x29, x30, the callee-saved x19-x28 and d8-d15, and the outer entry's
     * ret_sp at +160. ros_capp_ret_sp points at it while the code runs.
     * The entry's own gate state (ros_capp_user, set by ros_capp_call) gets
     * the native sp. FPCR and FPSR are 0. The runtime's FPCR comes back in
     * ros_capp_call. */
    ".globl ros_capp_call_x32\n"
    ".type ros_capp_call_x32,%function\n"
    "ros_capp_call_x32:\n"
    "   sub sp, sp, #176\n"
    "   stp x29, x30, [sp, #0]\n"
    "   stp x19, x20, [sp, #16]\n"
    "   stp x21, x22, [sp, #32]\n"
    "   stp x23, x24, [sp, #48]\n"
    "   stp x25, x26, [sp, #64]\n"
    "   stp x27, x28, [sp, #80]\n"
    "   stp d8, d9, [sp, #96]\n"
    "   stp d10, d11, [sp, #112]\n"
    "   stp d12, d13, [sp, #128]\n"
    "   stp d14, d15, [sp, #144]\n"
    "   mov x29, sp\n"
    TLS_ADDR("x9", "ros_capp_ret_sp")
    "   ldr x10, [x9]\n"
    "   str x10, [sp, #160]\n"
    "   mov x10, sp\n"
    "   str x10, [x9]\n"
    "   sub x10, x10, #64\n"
    "   and x10, x10, #0xFFFFFFFFFFFFFFF0\n"
    TLS_ADDR("x9", "ros_capp_user")
    "   ldr x9, [x9]\n"
    "   str x10, [x9]\n"
    "   mov x16, x0\n"
    "   mov sp, x2\n"
    "   mov w0, w1\n"
    TLS_ADDR("x9", "ros_capp_entry_x1")
    "   ldr x1, [x9]\n"                         /* 0, or the SWI entry (the gate) for BASIC's code */
    "   movz w30, #0xF020\n"                    /* ROS_CAPP_NESTED */
    "   movk w30, #0xFEEF, lsl #16\n"
    "   msr fpcr, xzr\n"
    "   msr fpsr, xzr\n"
    TLS_ADDR("x9", "ros_capp_x18")
    "   ldr x18, [x9]\n"
    A64_CLEAN_STATE
    "   br x16\n"
    ".size ros_capp_call_x32, .-ros_capp_call_x32\n"

    ".globl ros_capp_nested_return\n"
    ".type ros_capp_nested_return,%function\n"
    "ros_capp_nested_return:\n"
    TLS_ADDR("x9", "ros_capp_ret_sp")
    "   ldr x10, [x9]\n"
    "   mov sp, x10\n"
    "   ldr x10, [sp, #160]\n"
    "   str x10, [x9]\n"
    "   ldp d14, d15, [sp, #144]\n"
    "   ldp d12, d13, [sp, #128]\n"
    "   ldp d10, d11, [sp, #112]\n"
    "   ldp d8, d9, [sp, #96]\n"
    "   ldp x27, x28, [sp, #80]\n"
    "   ldp x25, x26, [sp, #64]\n"
    "   ldp x23, x24, [sp, #48]\n"
    "   ldp x21, x22, [sp, #32]\n"
    "   ldp x19, x20, [sp, #16]\n"
    "   ldp x29, x30, [sp, #0]\n"
    "   add sp, sp, #176\n"
    "   ret\n"
    ".size ros_capp_nested_return, .-ros_capp_nested_return\n");

/* The gate page (ros_capp_gate_init) is filled with zero words, which are
 * udf #0, an undefined instruction. A trampoline sits every 16 bytes. It
 * is `ldr x16, .+8; br x16`, and the target's 8 bytes are at
 * GATE_TRAMP_TARGET. x16 is IP0, which any call may lose (AAPCS64). */
#define GATE_INIT_MACHINE() ((void)0)
#define GATE_FILL 0
#define GATE_TRAMP_TARGET 8
static const unsigned char gate_tramp[] = { 0x50, 0x00, 0x00, 0x58,     /* ldr x16, .+8 */
                                            0x00, 0x02, 0x1F, 0xD6 };   /* br x16 */
/* The program's sp at entry is the RAM limit itself, 16-aligned. Its
 * return address is in x30 and is not on the stack (capp.h). */
#define ENTRY_SP_BIAS 0

/* ---- the static base ---------------------------------------------------------------- */

/* x18, as the runtime keeps it for this thread (ros_capp_x18). Every way
 * into A64X32 code loads it. Only the runtime sets it. A64X32 code cannot,
 * because roscc refuses a write to x18. These functions have the names of
 * x86-64's %gs functions, because the shared code calls them by those
 * names. */
uint64_t ros_capp_gs(void)
{
    return ros_capp_x18;
}

static uint32_t gs_in_force(void)
{
    return (uint32_t)ros_capp_x18;
}

static void set_gs(uint32_t base)
{
    ros_capp_x18 = base;
}

/* ---- BASIC's AArch64 ---------------------------------------------------------------- */

/* The code BASIC's assembler writes for AArch64 (*BasicAsmCPU A64). The
 * ']' that closes a block registers its range (ros_capp_code_assembled),
 * which is a ROS_CAPP_ASSEMBLED range, as on x86-64. CALL and USR find no
 * compiled code there and come to ros_capp_call. The block is called as an
 * AAPCS64 function (modules/basicvfp/asmlib/README.md)
 *
 *     void block(uint32_t regs[17],
 *                uint64_t (*swi)(uint32_t number, uint32_t *in, uint32_t *out));
 *
 *   x0  the register block: R0-R15, then the PSR, as 32-bit words. CALL
 *       puts A% to H% in R0 to R7, USR gives R0 back, and what the code
 *       leaves in them is what BASIC sees.
 *   x1  the SWI entry. It takes R0-R9 in from in[] and puts them out to
 *       out[] (the one block will do for both). It returns the PSR in its
 *       top 32 bits and, with V set, the error's address in its bottom 32.
 *       Set the X bit to get errors back.
 *
 * The code returns with ret. It must preserve x19-x29 and d8-d15
 * (AAPCS64) and leave x18 alone. Arena addresses are pointers, because the
 * arena is identity-mapped. A DIM'd block's address in A% is the pointer
 * that x0's block gives.
 *
 * The code is entered as any nested entry is (ros_capp_call). It runs on
 * the SVC stack in the arena and returns through the gate page (&FEEFF020).
 * Its SWI entry is the gate itself (&FEEFF000), and the same contract
 * applies: w0 is the SWI, w1 and w2 are the blocks, and the PSR and the
 * error come back in x0. So the code reaches the runtime only through the
 * gate page, as an application's does. On the box on QEMU it runs
 * unprivileged (EL0), and its stack, its blocks and the gate are all it
 * can touch. A fault in it is BASIC's error, as the runtime's fault on a
 * bad pointer is (fault.c). */
__attribute__((used)) _Thread_local uint64_t ros_capp_entry_x1;    /* a nested entry's x1 */
#define ENTRY_X1(v) (ros_capp_entry_x1 = (v))

/* ---- the instruction cache ------------------------------------------------------------ */

/* This is OS_SynchroniseCodeAreas' work on AArch64 (capp.c's sync_code). It
 * makes the instruction cache coherent with what was written over
 * [lo, hi). Apple's cores keep the data side coherent (CTR_EL0.IDC) but not
 * the instruction side (DIC clear). So __clear_cache's ic ivau runs over
 * the range. That faults on memory that is not mapped, and it would fault
 * in every page not yet touched, such as a whole RMA or a slot. That would
 * commit the memory and take 0.1 s. So only pages that are there are
 * synchronised. These are the pages that mincore() calls resident, which
 * any page ever written is, because the box has no swap. A page that is not
 * there holds no stale code. With both IDC and DIC set, nothing but the
 * barriers is needed. */
static void sync_resident(uint32_t lo, uint32_t hi)
{
    if (hi <= lo)
        return;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if ((ctr >> 28 & 3) == 3) {
        __asm__ volatile("dsb ish\n isb" : : : "memory");
        return;
    }
    unsigned char vec[2048];            /* a 1.5 GB slot: 192 calls */
    uint64_t end = hi, run = 0, at = lo & ~0xFFFull;    /* run is a resident run's start, or 0 */
    while (at < end) {
        uint64_t pages = (end - at + 0xFFF) >> 12;
        if (pages > sizeof vec)
            pages = sizeof vec;
        int whole = mincore(ros_ptr((uint32_t)at), pages << 12, vec) == 0;
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t page = at + (i << 12);
            int there = whole ? vec[i] & 1
                              : mincore(ros_ptr((uint32_t)page), 4096, vec + i) == 0 && (vec[i] & 1);
            if (there && !run)
                run = page > lo ? page : lo;
            else if (!there && run) {
                __builtin___clear_cache((char *)ros_ptr((uint32_t)run), (char *)ros_ptr((uint32_t)page));
                run = 0;
            }
        }
        at += pages << 12;
    }
    if (run)
        __builtin___clear_cache((char *)ros_ptr((uint32_t)run), (char *)ros_ptr(0) + end);
}
#define SYNC_CODE_RANGE(lo, hi) sync_resident(lo, hi)
/* Code that BASIC's assembler has just written (']' calls
 * ros_capp_code_assembled) is made coherent before CALL or USR runs it. */
#define SYNC_ASSEMBLED(lo, hi) sync_resident(lo, hi)

/* ---- floating point traps -------------------------------------------------------------- */

/* Tests whether this CPU implements FPCR's trap enables (IOE, DZE, OFE,
 * UFE, IXE, IDE: bits 8-12 and 15). They are written, read back and put
 * back. Apple's cores are expected not to implement them. Then the bits
 * read as 0, floating point exceptions never trap, and RISC OS's FP
 * exceptions would have to be raised in software. This returns the bits
 * that stuck. */
uint32_t ros_capp_fp_traps(void)
{
    uint32_t was = fpcr_now();
    fpcr_set(was | 0x9F00u);
    uint32_t got = fpcr_now() & 0x9F00u;
    fpcr_set(was);
    return got;
}
