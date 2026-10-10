/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* capp.c.inc -- the x32 gate (runtime/capp.c includes it where it stands):
 * the per-thread state the assembler reads and writes, the clean state an
 * entry loads, the gate and the ways in and out of an x32 program in
 * x86-64 assembler, the static base in %gs, the floating point control the
 * shared code saves and loads, and the gate page's trampolines. AArch64's
 * A64X32 gate is ../aarch64/capp.c.inc. */

/* The floating point control (capp.h's fpc and fpc2) is MXCSR and the x87
 * control word. FP_CTL_NOW() is MXCSR now. FP_CTL2_STORE(v) stores the
 * control word in v. FP_LOAD(c, c2) loads both. FP_CLEAN_* are what an
 * entry starts with (capp.h). FP_NO_FLAGS(c) is c with MXCSR's exception
 * flags clear, as a delivered handler gets it. */
#define FP_CTL_NOW()      __builtin_ia32_stmxcsr()
#define FP_CTL2_STORE(v)  __asm__ volatile("fnstcw %0" : "=m"(v))
#define FP_LOAD(c, c2)                                                                      \
    __asm__ volatile("fldcw %0" : : "m"(c2));                                               \
    __builtin_ia32_ldmxcsr(c)
#define FP_CLEAN_CTL      0x1F80u
#define FP_CLEAN_CTL2     0x037F
#define FP_NO_FLAGS(c)    ((c) & ~0x3Fu)
/* The per-thread words that ros_capp_enter loads for a delivered handler. */
#define ENTRY_FPC         ros_capp_entry_mxcsr
#define ENTRY_FPC2        ros_capp_entry_fcw

/* Per thread, read and written by the assembler below (local-exec):
 *   ros_capp_user    the gate state of the x32 code running on this thread
 *                    now (capp.h). It holds the native sp, where a gate
 *                    call moves the thread. That is below the frame that
 *                    entered that code. It also holds the saved sp, which
 *                    is the x32 sp at its latest gate call. While the
 *                    program's code runs, this is the task's own state
 *                    (struct ros_task), so the task's saved sp is where its
 *                    program last called the OS. While a nested entry runs,
 *                    this is that entry's own state.
 *   ros_capp_ret_sp  the native frame the innermost nested entry returns
 *                    to.
 * The native frames belong to the thread. The state they serve belongs to
 * the task. */
__attribute__((used)) _Thread_local struct ros_capp_gate_state *ros_capp_user;
__attribute__((used)) _Thread_local uint64_t ros_capp_ret_sp;
/* What ros_capp_enter loads after the clean state, if ros_capp_entry_fp is
 * set. These are the program's MXCSR and x87 control word, for a delivered
 * handler. The gate records them in the caller's gate state (capp.h). */
__attribute__((used)) _Thread_local uint32_t ros_capp_entry_fp;
__attribute__((used)) _Thread_local uint32_t ros_capp_entry_mxcsr;
__attribute__((used)) _Thread_local uint16_t ros_capp_entry_fcw;
/* Whether the gate is copying a register block, and its caller's sp and
 * return address. A fault during the copy is delivered as the caller's. */
static _Thread_local int gate_copying;
static _Thread_local uint32_t gate_sp, gate_ret;

_Static_assert(offsetof(struct ros_capp_gate_state, native_sp) == 0, "the gate reads it at +0");
_Static_assert(offsetof(struct ros_capp_gate_state, saved_sp) == 8, "the gate writes it at +8");
_Static_assert(offsetof(struct ros_capp_gate_state, fpc) == 16, "the gate writes it at +16");
_Static_assert(offsetof(struct ros_capp_gate_state, fpc2) == 20, "the gate writes it at +20");

/* Whether the CPU and the kernel give AVX (CPUID.1:ECX OSXSAVE and AVX,
 * XCR0 SSE and AVX state). If so, an entry clears ymm0-15 whole (vzeroall).
 * It is set by ros_capp_gate_init and read by the entries' code. */
__attribute__((used, visibility("hidden"))) unsigned char ros_capp_avx;

static int have_avx(void)
{
    unsigned a, b, c, d;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if ((c & (1u << 27 | 1u << 28)) != (1u << 27 | 1u << 28))
        return 0;
    unsigned lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    (void)hi;
    return (lo & 6) == 6;
}

uint64_t ros_capp_swi(uint32_t number, uint32_t in, uint32_t out, uint64_t sp, uint32_t psr,
                      uint64_t lr);
/* The caller's return address, for a gate call's R14 and R15. It is at the
 * top of the caller's stack, because x32 code pushes it when it calls the
 * gate. */
#define GATE_RET(sp, lr)  ((void)(lr), ros_ld32((uint32_t)(sp)))

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

/* How x32 code is entered, so that nothing of the runtime's shows through.
 * MXCSR is &1F80, the x87 control word is &37F and its stack is empty
 * (fninit), and the direction flag is clear. Every register but %rdi,
 * %rsi, %rsp and %r11 is zero. That includes the vector registers, and all
 * of ymm0-15 where there is AVX. The macro's scratch word is at (%rsp),
 * below which nothing is live. */
#define X32_CLEAN_STATE                                                                     \
    "   subq $8, %rsp\n"                                                                    \
    "   fninit\n"                                                                           \
    "   movl $0x1F80, (%rsp)\n"                                                             \
    "   ldmxcsr (%rsp)\n"                                                                   \
    "   movw $0x037F, (%rsp)\n"                                                             \
    "   fldcw (%rsp)\n"                                                                     \
    "   addq $8, %rsp\n"                                                                    \
    "   cld\n"                                                                              \
    "   cmpb $0, ros_capp_avx(%rip)\n"                                                      \
    "   je 1f\n"                                                                            \
    "   vzeroall\n"                                                                         \
    "   jmp 2f\n"                                                                           \
    "1: pxor %xmm0, %xmm0\n   pxor %xmm1, %xmm1\n   pxor %xmm2, %xmm2\n"                    \
    "   pxor %xmm3, %xmm3\n   pxor %xmm4, %xmm4\n   pxor %xmm5, %xmm5\n"                    \
    "   pxor %xmm6, %xmm6\n   pxor %xmm7, %xmm7\n   pxor %xmm8, %xmm8\n"                    \
    "   pxor %xmm9, %xmm9\n   pxor %xmm10, %xmm10\n   pxor %xmm11, %xmm11\n"                \
    "   pxor %xmm12, %xmm12\n   pxor %xmm13, %xmm13\n   pxor %xmm14, %xmm14\n"              \
    "   pxor %xmm15, %xmm15\n"                                                              \
    "2:\n"                                                                                  \
    "   xorl %eax, %eax\n   xorl %ebx, %ebx\n   xorl %ecx, %ecx\n   xorl %edx, %edx\n"      \
    "   xorl %ebp, %ebp\n   xorl %r8d, %r8d\n   xorl %r9d, %r9d\n   xorl %r10d, %r10d\n"    \
    "   xorl %r12d, %r12d\n   xorl %r13d, %r13d\n   xorl %r14d, %r14d\n   xorl %r15d, %r15d\n"

__asm__(
    ".text\n"
    /* The gate, called from x32 code on its own stack. %edi is the SWI
     * number, %esi is the address of the R0-R9 input block and %edx is the
     * address of the R0-R9 output block. The return address is at (%rsp).
     * The gate saves the code's sp and takes the native sp from the
     * calling code's gate state (ros_capp_user). The frame on the native
     * stack is: (%rsp) the runtime's MXCSR and x87 control word, 4 the
     * caller's MXCSR, 8 its control word, 12 the low word of its RFLAGS,
     * 16 its sp. The caller's MXCSR and control word are also recorded in
     * its gate state, for its delivered handlers. EFLAGS.AC, which user
     * code may set (x86's alignment check), is off in the runtime, whose
     * code does unaligned accesses. It is turned back on, on the way out,
     * if the caller had it. The SWI is entered with N Z C V clear. Through
     * ros_capp_gate_psr it is entered with the N Z C V of %ecx (its top
     * four bits). */
    ".globl ros_capp_gate_psr\n"
    ".type ros_capp_gate_psr,@function\n"
    "ros_capp_gate_psr:\n"
    "   movl %ecx, %r8d\n"
    "   jmp 1f\n"
    ".size ros_capp_gate_psr, .-ros_capp_gate_psr\n"
    ".globl ros_capp_gate\n"
    ".type ros_capp_gate,@function\n"
    "ros_capp_gate:\n"
    "   xorl %r8d, %r8d\n"
    "1: movq %rsp, %r11\n"
    "   movq %fs:ros_capp_user@tpoff, %r10\n"
    "   movq %r11, 8(%r10)\n"                   /* saved_sp */
    "   movq (%r10), %rsp\n"                    /* native_sp */
    "   subq $32, %rsp\n"
    "   movq %r11, 16(%rsp)\n"
    "   pushfq\n"
    "   popq %rax\n"
    "   movl %eax, 12(%rsp)\n"
    "   btrl $18, %eax\n"                      /* AC */
    "   jnc 4f\n"
    "   pushq %rax\n"
    "   popfq\n"
    "4: stmxcsr 4(%rsp)\n"
    "   fnstcw 8(%rsp)\n"
    "   movl 4(%rsp), %eax\n"
    "   movl %eax, 16(%r10)\n"                  /* mxcsr */
    "   movw 8(%rsp), %ax\n"
    "   movw %ax, 20(%r10)\n"                   /* fcw */
    "   movl $0x1F80, (%rsp)\n"
    "   ldmxcsr (%rsp)\n"
    "   movw $0x037F, (%rsp)\n"
    "   fldcw (%rsp)\n"
    "   cld\n"
    "   movq %r11, %rcx\n"
    "   call ros_capp_swi\n"
    "   ldmxcsr 4(%rsp)\n"
    "   fldcw 8(%rsp)\n"
    "   testl $0x40000, 12(%rsp)\n"
    "   jz 5f\n"
    "   pushfq\n"
    "   orl $0x40000, (%rsp)\n"
    "   popfq\n"
    "5: movq 16(%rsp), %rsp\n"
    "   ret\n"
    ".size ros_capp_gate, .-ros_capp_gate\n"

    /* A return from the program's entry, as FileSwitch's
     * ReturnFromAbsoluteCode is. */
    ".globl ros_capp_returned\n"
    ".type ros_capp_returned,@function\n"
    "ros_capp_returned:\n"
    "   movq %fs:ros_capp_user@tpoff, %rax\n"
    "   movq (%rax), %rsp\n"
    "   subq $16, %rsp\n"
    "   pushfq\n"                              /* AC off */
    "   andl $~0x40000, (%rsp)\n"
    "   popfq\n"
    "   movl $0x1F80, (%rsp)\n"
    "   ldmxcsr (%rsp)\n"
    "   movw $0x037F, (%rsp)\n"
    "   fldcw (%rsp)\n"
    "   cld\n"
    "   call ros_capp_returned_c\n"
    "   ud2\n"
    ".size ros_capp_returned, .-ros_capp_returned\n"

    /* A return from a delivered handler. The program is over. */
    ".globl ros_capp_ended\n"
    ".type ros_capp_ended,@function\n"
    "ros_capp_ended:\n"
    "   movq %fs:ros_capp_user@tpoff, %rax\n"
    "   movq (%rax), %rsp\n"
    "   subq $16, %rsp\n"
    "   pushfq\n"                              /* AC off */
    "   andl $~0x40000, (%rsp)\n"
    "   popfq\n"
    "   movl $0x1F80, (%rsp)\n"
    "   ldmxcsr (%rsp)\n"
    "   movw $0x037F, (%rsp)\n"
    "   fldcw (%rsp)\n"
    "   cld\n"
    "   call ros_capp_ended_c\n"
    "   ud2\n"
    ".size ros_capp_ended, .-ros_capp_ended\n"

    /* Into x32 code that does not come back here. %rdi is its entry, %rsi
     * is its sp, %edx and %ecx are its first two arguments and %r8d is its
     * return address (&FEEFF010 for the program's entry, &FEEFF030 for a
     * delivered handler). The gate's calls from it go below this frame, at
     * the native sp of the gate state in use (ros_capp_user, the task's).
     * After the clean state, a delivered handler gets the program's MXCSR
     * and x87 control word (when ros_capp_entry_fp is non-zero). */
    ".globl ros_capp_enter\n"
    ".type ros_capp_enter,@function\n"
    "ros_capp_enter:\n"
    "   leaq -64(%rsp), %rax\n"
    "   andq $-16, %rax\n"
    "   movq %fs:ros_capp_user@tpoff, %r10\n"
    "   movq %rax, (%r10)\n"
    "   movq %rdi, %r11\n"
    "   movq %rsi, %rsp\n"
    "   movl %r8d, %eax\n"                      /* zero-extended */
    "   pushq %rax\n"
    "   movl %edx, %edi\n"
    "   movl %ecx, %esi\n"
    X32_CLEAN_STATE
    "   cmpl $0, %fs:ros_capp_entry_fp@tpoff\n"     /* a delivered handler gets the program's */
    "   je 3f\n"
    "   ldmxcsr %fs:ros_capp_entry_mxcsr@tpoff\n"
    "   fldcw %fs:ros_capp_entry_fcw@tpoff\n"
    "3: pushq %r11\n"                           /* the entry, as the return address */
    "   xorl %r11d, %r11d\n"
    "   ret\n"                                  /* leaves (%rsp) the return address it is given */
    ".size ros_capp_enter, .-ros_capp_enter\n"

    /* A nested entry. %rdi is the code, %esi is the register block and
     * %rdx is the x32 sp. The code is called with the block and returns to
     * &FEEFF020, which comes back to ros_capp_nested_return and so here.
     * The native frame holds the callee-saved registers and the outer
     * entry's ret_sp. ros_capp_ret_sp points at it while the code runs.
     * The entry's own gate state (ros_capp_user, set by ros_capp_call) gets
     * the native sp. */
    ".globl ros_capp_call_x32\n"
    ".type ros_capp_call_x32,@function\n"
    "ros_capp_call_x32:\n"
    "   pushq %rbp\n"
    "   movq %rsp, %rbp\n"
    "   pushq %rbx\n"
    "   pushq %r12\n"
    "   pushq %r13\n"
    "   pushq %r14\n"
    "   pushq %r15\n"
    "   pushq %fs:ros_capp_ret_sp@tpoff\n"
    "   movq %rsp, %fs:ros_capp_ret_sp@tpoff\n"
    "   leaq -64(%rsp), %rax\n"
    "   andq $-16, %rax\n"
    "   movq %fs:ros_capp_user@tpoff, %r10\n"
    "   movq %rax, (%r10)\n"
    "   movq %rdi, %r11\n"
    "   movq %rdx, %rsp\n"
    "   movl $0xFEEFF020, %eax\n"               /* ROS_CAPP_NESTED, zero-extended */
    "   pushq %rax\n"
    "   movl %esi, %edi\n"
    "   xorl %esi, %esi\n"
    X32_CLEAN_STATE
    "   pushq %r11\n"
    "   xorl %r11d, %r11d\n"
    "   ret\n"
    ".size ros_capp_call_x32, .-ros_capp_call_x32\n"

    ".globl ros_capp_nested_return\n"
    ".type ros_capp_nested_return,@function\n"
    "ros_capp_nested_return:\n"
    "   movq %fs:ros_capp_ret_sp@tpoff, %rsp\n"
    "   pushfq\n"                              /* turn AC off, if the code left it on */
    "   testl $0x40000, (%rsp)\n"
    "   jz 6f\n"
    "   andl $~0x40000, (%rsp)\n"
    "   popfq\n"
    "   jmp 7f\n"
    "6: addq $8, %rsp\n"
    "7:\n"
    "   popq %rax\n"
    "   movq %rax, %fs:ros_capp_ret_sp@tpoff\n"
    "   popq %r15\n"
    "   popq %r14\n"
    "   popq %r13\n"
    "   popq %r12\n"
    "   popq %rbx\n"
    "   popq %rbp\n"
    "   ret\n"
    ".size ros_capp_nested_return, .-ros_capp_nested_return\n");

/* The gate page (ros_capp_gate_init) is filled with int3. A trampoline
 * sits every 16 bytes. It is `movabs $target, %r11; jmp *%r11`, and the
 * target's 8 bytes are at GATE_TRAMP_TARGET. GATE_INIT_MACHINE() finds out
 * what the entries' code needs to know of the CPU. */
#define GATE_INIT_MACHINE() (ros_capp_avx = (unsigned char)have_avx())
#define GATE_FILL 0xCC                                              /* int3 */
#define GATE_TRAMP_TARGET 2
static const unsigned char gate_tramp[] = { 0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,  /* movabs $x, %r11 */
                                            0x41, 0xFF, 0xE3 };                  /* jmp *%r11 */
/* Code that BASIC's assembler has just written (ros_capp_code_assembled)
 * needs no synchronising, because x86 keeps its instruction cache coherent
 * itself. */
#define SYNC_ASSEMBLED(lo, hi) ((void)(lo), (void)(hi))
/* The program's sp at entry is below the RAM limit (16-aligned). Its
 * return address is pushed there (capp.h). */
#define ENTRY_SP_BIAS 8

/* ---- the static base ---------------------------------------------------------------- */

/* %gs's base is set with wrgsbase where the kernel allows it (FSGSBASE,
 * AT_HWCAP2 bit 1: 11 ns), and otherwise with arch_prctl (157 ns). The
 * runtime keeps what it last set, per thread, so an entry into code with
 * the base already in force costs nothing. */
#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004
#define HWCAP2_FSGSBASE 2u

static int fsgsbase = -1;
/* What this thread's %gs is, as the runtime last set or read it. It is ~0
 * until the first use, which reads it. A new thread starts with its
 * creator's base, because Linux copies it on clone, and does not start
 * with 0. So a task's first entry, with its base 0 before it registers,
 * must write it. */
static _Thread_local uint64_t gs_now = ~0ull;

uint64_t ros_capp_gs(void)
{
    if (fsgsbase < 0)
        fsgsbase = (getauxval(AT_HWCAP2) & HWCAP2_FSGSBASE) != 0;
    unsigned long v = 0;
    if (fsgsbase)
        __asm__ volatile("rdgsbase %0" : "=r"(v));
    else
        syscall(SYS_arch_prctl, ARCH_GET_GS, &v);
    return v;
}

/* The base in force on this thread, for putting back later. */
static uint32_t gs_in_force(void)
{
    if (gs_now == ~0ull)
        gs_now = ros_capp_gs();
    return (uint32_t)gs_now;
}

static void set_gs(uint32_t base)
{
    if (base == gs_in_force() && !(gs_now >> 32))
        return;
    if (fsgsbase)
        __asm__ volatile("wrgsbase %0" : : "r"((uint64_t)base) : "memory");
    else if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)base) != 0)
        return;                         /* it was not set, so it is not recorded as set */
    gs_now = base;
}
