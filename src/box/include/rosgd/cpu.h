/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* cpu.h -- the contract between the runtime and compiled ObjAsm.
 *
 * The ObjAsm compiler does not decompile. It treats each A32 instruction
 * as IR with exact semantics and emits C over an explicit machine state.
 * This header is everything that C may use: the state block, the flag
 * arithmetic, the condition codes, and the three ways out of compiled
 * code, which are a call through the dispatcher, a SWI and a return.
 *
 * RISC OS assembler has no calling convention. Each routine documents which
 * registers it takes and corrupts, and they all differ, and 31% of internal
 * call sites test the flags afterwards. So every compiled entry
 * takes the whole state block, and the flags live in it across calls.
 */
#ifndef ROSGD_CPU_H
#define ROSGD_CPU_H

#include <math.h>
#include <setjmp.h>
#include <stdint.h>

#include "rosgd/arena.h"

/* VFP's registers: thirty-two doubles, the first sixteen of which are also
 * thirty-two singles, with s(2n) the low half of d(n) as on the hardware.
 * The word views copy bits exactly, NaN payloads and all. */
union ros_vfp {
    double d[32];
    float s[64];
    uint64_t dw[32];
    uint32_t sw[64];
};

/* A task's floating point, as standard C floating point. FPA's eight
 * registers are doubles (its extended precision is computed in double),
 * and there is no emulator. The registers belong to the task, as in
 * RISC OS: they persist across calls and SWIs, and are switched with the
 * task. */
struct ros_fp {
    double f[8];
    uint32_t fpsr;      /* FPA status: system ID, AC, enables, flags */
    uint32_t fpscr;     /* VFP status: NZCV from compares, the rounding mode,
                           the cumulative exceptions (ROS_FPSCR_IOC...) */
    union ros_vfp vfp;
    uint32_t vfp_context; /* VFPSupport's active context for these registers */
};

#define ROS_FPSR_INITIAL 0x01000000u    /* system ID 1, no traps, AC clear */
#define ROS_FPSR_OFE 0x00040000u        /* the overflow trap enabled */

/* The running task's floating point. */
extern struct ros_fp *ros_fp_current;

/* Called as a thread goes back to user mode (runtime/callback.c), when set.
 * It is VFPSupport's (modules/vfpsupport), and takes a context that the
 * Wimp activated lazily while switching to this task, into this thread's
 * registers and not those of the thread the Wimp's switch ran on. */
extern void (*ros_fp_user_return)(void);

struct ros_cpu {
    uint32_t r[16];     /* r13 sp, r14 lr, r15 pc: all arena addresses */
    uint32_t n, z, c, v;/* each 0 or 1: clang keeps them in registers */
    uint32_t q;         /* sticky saturation, for the few that set it */
    uint32_t mode;      /* the modelled CPSR mode field, for MRS reads */
    uint32_t irq_off;   /* the modelled I bit, likewise */
    struct ros_fp *fp;  /* the task's floating point */
};

#define ROS_MODE_USR 0x10u
#define ROS_MODE_SVC 0x13u
#define ROS_V_BIT    (1u << 28)

/* An entry into compiled code: a routine, or a region's entry wrapper. */
typedef void ros_code(struct ros_cpu *cpu);

/* ---- the PSR ---------------------------------------------------------- */

static inline uint32_t ros_cpsr(const struct ros_cpu *s)
{
    return (s->n << 31) | (s->z << 30) | (s->c << 29) | (s->v << 28) |
           (s->q << 27) | (s->irq_off << 7) | s->mode;
}

/* MSR CPSR_f: the flags only, which is all user code may write. */
static inline void ros_msr_f(struct ros_cpu *s, uint32_t psr)
{
    s->n = psr >> 31;
    s->z = (psr >> 30) & 1;
    s->c = (psr >> 29) & 1;
    s->v = (psr >> 28) & 1;
    s->q = (psr >> 27) & 1;
}

/* ---- the barrel shifter -------------------------------------------------- */

static inline uint32_t ros_ror(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

enum { ROS_LSL, ROS_LSR, ROS_ASR, ROS_ROR };

/* A shift by a register (op2 as `Rm, <type> Rs`) by the bottom byte of
 * Rs, as the ARM ARM's Shift_C defines it. *c holds C on entry and the
 * shifter's carry out on return, which is C again when the amount is 0. */
static inline uint32_t ros_shift(uint32_t v, int type, uint32_t rs, uint32_t *c)
{
    uint32_t n = rs & 0xFF;
    if (n == 0)
        return v;
    switch (type) {
    case ROS_LSL:
        if (n < 32) {
            *c = (v >> (32 - n)) & 1;
            return v << n;
        }
        *c = n == 32 ? v & 1 : 0;
        return 0;
    case ROS_LSR:
        if (n < 32) {
            *c = (v >> (n - 1)) & 1;
            return v >> n;
        }
        *c = n == 32 ? v >> 31 : 0;
        return 0;
    case ROS_ASR:
        if (n < 32) {
            *c = (v >> (n - 1)) & 1;
            return (uint32_t)((int32_t)v >> n);
        }
        *c = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default:
        n &= 31;
        *c = n ? (v >> (n - 1)) & 1 : v >> 31;
        return ros_ror(v, n);
    }
}

/* The same shifts as values, for lifted code: by the bottom byte of rs,
 * as the ARM ARM defines it, and the carry out given the carry in. */
static inline uint32_t ros_lsl(uint32_t v, uint32_t rs)
{
    uint32_t n = rs & 0xFF;
    return n < 32 ? v << n : 0;
}

static inline uint32_t ros_lsr(uint32_t v, uint32_t rs)
{
    uint32_t n = rs & 0xFF;
    return n < 32 ? v >> n : 0;
}

static inline uint32_t ros_asr(uint32_t v, uint32_t rs)
{
    uint32_t n = rs & 0xFF;
    return (uint32_t)((int32_t)v >> (n < 32 ? n : 31));
}

static inline uint32_t ros_rorr(uint32_t v, uint32_t rs)
{
    return ros_ror(v, rs & 0xFF);
}

static inline uint32_t ros_shift_c(uint32_t v, int type, uint32_t rs, uint32_t c)
{
    ros_shift(v, type, rs, &c);
    return c;
}

static inline uint32_t ros_clz(uint32_t v)
{
    return v ? (uint32_t)__builtin_clz(v) : 32u;
}

/* ---- flag-setting arithmetic, exactly as the ARM ARM defines it --------- */

static inline void ros_nz(struct ros_cpu *s, uint32_t r)
{
    s->n = r >> 31;
    s->z = r == 0;
}

/* ADDS: C is the carry out, V signed overflow. */
static inline uint32_t ros_adds(struct ros_cpu *s, uint32_t a, uint32_t b)
{
    uint32_t r = a + b;
    ros_nz(s, r);
    s->c = r < a;
    s->v = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    return r;
}

/* ADCS */
static inline uint32_t ros_adcs(struct ros_cpu *s, uint32_t a, uint32_t b)
{
    uint64_t wide = (uint64_t)a + b + s->c;
    uint32_t r = (uint32_t)wide;
    ros_nz(s, r);
    s->c = (uint32_t)(wide >> 32);
    s->v = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    return r;
}

/* Signed overflow of a + b and of a - b: V, for lifted code that reads it
 * without setting the flags. */
static inline uint32_t ros_addv(uint32_t a, uint32_t b)
{
    uint32_t r = a + b;
    return ((~(a ^ b) & (a ^ r)) >> 31) & 1;
}

static inline uint32_t ros_subv(uint32_t a, uint32_t b)
{
    uint32_t r = a - b;
    return (((a ^ b) & (a ^ r)) >> 31) & 1;
}

/* SUBS and CMP: C is NOT borrow. It is set when a >= b unsigned. */
static inline uint32_t ros_subs(struct ros_cpu *s, uint32_t a, uint32_t b)
{
    uint32_t r = a - b;
    ros_nz(s, r);
    s->c = a >= b;
    s->v = (((a ^ b) & (a ^ r)) >> 31) & 1;
    return r;
}

/* SBCS */
static inline uint32_t ros_sbcs(struct ros_cpu *s, uint32_t a, uint32_t b)
{
    uint64_t wide = (uint64_t)a - b - (1u - s->c);
    uint32_t r = (uint32_t)wide;
    ros_nz(s, r);
    s->c = (wide >> 32) == 0;
    s->v = (((a ^ b) & (a ^ r)) >> 31) & 1;
    return r;
}

/* The logical ops set N and Z from the result and C from the shifter; they
 * leave V alone. */
static inline uint32_t ros_logic(struct ros_cpu *s, uint32_t r, uint32_t shifter_c)
{
    ros_nz(s, r);
    s->c = shifter_c;
    return r;
}

/* ---- condition codes --------------------------------------------------- */

enum ros_cond {
    ROS_EQ, ROS_NE, ROS_CS, ROS_CC, ROS_MI, ROS_PL, ROS_VS, ROS_VC,
    ROS_HI, ROS_LS, ROS_GE, ROS_LT, ROS_GT, ROS_LE, ROS_AL
};

static inline int ros_cond(const struct ros_cpu *s, enum ros_cond cc)
{
    switch (cc) {
    case ROS_EQ: return s->z;
    case ROS_NE: return !s->z;
    case ROS_CS: return s->c;
    case ROS_CC: return !s->c;
    case ROS_MI: return s->n;
    case ROS_PL: return !s->n;
    case ROS_VS: return s->v;
    case ROS_VC: return !s->v;
    case ROS_HI: return s->c && !s->z;
    case ROS_LS: return !s->c || s->z;
    case ROS_GE: return s->n == s->v;
    case ROS_LT: return s->n != s->v;
    case ROS_GT: return !s->z && s->n == s->v;
    case ROS_LE: return s->z || s->n != s->v;
    default:     return 1;
    }
}

/* ---- floating point ------------------------------------------------------ */

/* FPA's CMF: N less, Z equal, C greater or equal, V unordered. C is also
 * set for unordered when the FPSR's AC bit says so (fpadefs). */
static inline void ros_fpa_cmp(struct ros_cpu *s, double a, double b)
{
    int unordered = isunordered(a, b);
    s->n = a < b;
    s->z = a == b;
    s->c = a >= b || (unordered && (s->fp->fpsr >> 12 & 1));
    s->v = unordered;
}

/* VFP's VCMP sets the FPSCR's NZCV (equal 0110, less 1000, greater 0010,
 * unordered 0011); VMRS APSR_nzcv, FPSCR copies them to the flags. */
static inline void ros_vfp_cmp(struct ros_cpu *s, double a, double b)
{
    uint32_t nzcv = isunordered(a, b) ? 0x3 : a < b ? 0x8 : a == b ? 0x6 : 0x2;
    s->fp->fpscr = (s->fp->fpscr & 0x0FFFFFFFu) | nzcv << 28;
}

static inline void ros_vmrs_flags(struct ros_cpu *s)
{
    uint32_t f = s->fp->fpscr;
    s->n = f >> 31 & 1;
    s->z = f >> 30 & 1;
    s->c = f >> 29 & 1;
    s->v = f >> 28 & 1;
}

/* Rounding, as FPA encodes it and the FPSCR's RMode field does: to
 * nearest (ties to even), towards +infinity, -infinity, zero. */
enum { ROS_ROUND_NEAREST, ROS_ROUND_PLUS, ROS_ROUND_MINUS, ROS_ROUND_ZERO };

static inline double ros_round(double x, int mode)
{
    return mode == ROS_ROUND_NEAREST ? nearbyint(x)
         : mode == ROS_ROUND_PLUS    ? ceil(x)
         : mode == ROS_ROUND_MINUS   ? floor(x)
         : trunc(x);
}

/* To a 32-bit integer, as FIX and VCVT do it: rounded, and saturated at
 * the limits, with NaN giving 0. */
static inline int32_t ros_to_int(double x, int mode)
{
    if (isnan(x))
        return 0;
    double r = ros_round(x, mode);
    return r >= 2147483647.0 ? INT32_MAX : r <= -2147483648.0 ? INT32_MIN : (int32_t)r;
}

static inline uint32_t ros_to_uint(double x, int mode)
{
    if (isnan(x))
        return 0;
    double r = ros_round(x, mode);
    return r >= 4294967295.0 ? UINT32_MAX : r <= 0.0 ? 0 : (uint32_t)r;
}

/* An exact double rounded to single in a given direction: FLTS with P, M
 * or Z, whose integer argument is exact in double. */
static inline float ros_to_float(double x, int mode)
{
    float f = (float)x;
    if (mode == ROS_ROUND_PLUS && (double)f < x)
        f = nextafterf(f, INFINITY);
    else if (mode == ROS_ROUND_MINUS && (double)f > x)
        f = nextafterf(f, -INFINITY);
    else if (mode == ROS_ROUND_ZERO && fabs((double)f) > fabs(x))
        f = nextafterf(f, 0.0f);
    return f;
}

/* VFP's cumulative exceptions, the FPSCR's bits 0-2, as IEEE 754 raises
 * them with its traps disabled: invalid operation, division by zero and
 * overflow.  The lifted code ORs them in after each operation
 * (rosasm/src/lift.rs), working them out from the operands and the result
 * so that they do not depend on the host's floating point state.  BASIC
 * reads them after each operation ("Negative root", "Number too big").
 * Underflow, inexact and input denormal (bits 3, 4 and 7) are not kept. */
#define ROS_FPSCR_IOC 0x1u
#define ROS_FPSCR_DZC 0x2u
#define ROS_FPSCR_OFC 0x4u

static inline int ros_snan(double x)
{
    uint64_t b;
    memcpy(&b, &x, 8);
    return isnan(x) && !(b >> 51 & 1);
}

static inline int ros_snanf(float x)
{
    uint32_t b;
    memcpy(&b, &x, 4);
    return isnan(x) && !(b >> 22 & 1);
}

/* Add, subtract, multiply: NaN from numbers (or from a signalling NaN) is
 * invalid; infinity from finite operands is overflow. */
static inline uint32_t ros_vfp_ex2(double r, double a, double b)
{
    if (isnan(r))
        return (!isnan(a) && !isnan(b)) || ros_snan(a) || ros_snan(b) ? ROS_FPSCR_IOC : 0;
    return isinf(r) && isfinite(a) && isfinite(b) ? ROS_FPSCR_OFC : 0;
}

static inline uint32_t ros_vfp_ex2f(float r, float a, float b)
{
    if (isnan(r))
        return (!isnan(a) && !isnan(b)) || ros_snanf(a) || ros_snanf(b) ? ROS_FPSCR_IOC : 0;
    return isinf(r) && isfinite(a) && isfinite(b) ? ROS_FPSCR_OFC : 0;
}

/* Divide: a finite non-zero number over zero is division by zero */
static inline uint32_t ros_vfp_div_ex(double r, double a, double b)
{
    return b == 0 && isfinite(a) && a != 0 ? ROS_FPSCR_DZC : ros_vfp_ex2(r, a, b);
}

static inline uint32_t ros_vfp_div_exf(float r, float a, float b)
{
    return b == 0 && isfinite(a) && a != 0 ? ROS_FPSCR_DZC : ros_vfp_ex2f(r, a, b);
}

/* The fused multiply-adds */
static inline uint32_t ros_vfp_ex3(double r, double a, double b, double c)
{
    if (isnan(r))
        return (!isnan(a) && !isnan(b) && !isnan(c)) || ros_snan(a) || ros_snan(b) || ros_snan(c)
                   ? ROS_FPSCR_IOC : 0;
    return isinf(r) && isfinite(a) && isfinite(b) && isfinite(c) ? ROS_FPSCR_OFC : 0;
}

static inline uint32_t ros_vfp_ex3f(float r, float a, float b, float c)
{
    if (isnan(r))
        return (!isnan(a) && !isnan(b) && !isnan(c)) || ros_snanf(a) || ros_snanf(b) || ros_snanf(c)
                   ? ROS_FPSCR_IOC : 0;
    return isinf(r) && isfinite(a) && isfinite(b) && isfinite(c) ? ROS_FPSCR_OFC : 0;
}

/* Square root: of anything below zero (not -0) */
static inline uint32_t ros_vfp_sqrt_ex(double a)
{
    return a < 0 || ros_snan(a) ? ROS_FPSCR_IOC : 0;
}

static inline uint32_t ros_vfp_sqrt_exf(float a)
{
    return a < 0 || ros_snanf(a) ? ROS_FPSCR_IOC : 0;
}

/* VCMP: a signalling NaN; VCMPE: any NaN */
static inline uint32_t ros_vfp_cmp_ex(double a, double b, int e)
{
    return (e ? isunordered(a, b) : ros_snan(a) || ros_snan(b)) ? ROS_FPSCR_IOC : 0;
}

static inline uint32_t ros_vfp_cmp_exf(float a, float b, int e)
{
    return (e ? isunordered(a, b) : ros_snanf(a) || ros_snanf(b)) ? ROS_FPSCR_IOC : 0;
}

/* Between precisions: a signalling NaN; to single, overflow */
static inline uint32_t ros_vfp_widen_ex(float a)
{
    return ros_snanf(a) ? ROS_FPSCR_IOC : 0;
}

static inline uint32_t ros_vfp_narrow_ex(float r, double a)
{
    return ros_snan(a) ? ROS_FPSCR_IOC : isinf(r) && isfinite(a) ? ROS_FPSCR_OFC : 0;
}

/* To a 32-bit integer: NaN, or out of range once rounded, is invalid (the
 * result saturates, ros_to_int) */
static inline uint32_t ros_vfp_int_ex(double x, int mode, int is_signed)
{
    if (isnan(x))
        return ROS_FPSCR_IOC;
    double r = ros_round(x, mode);
    return (is_signed ? r > 2147483647.0 || r < -2147483648.0 : r > 4294967295.0 || r < 0.0)
               ? ROS_FPSCR_IOC : 0;
}

/* FPA's formats in memory. A double is stored high word first, which is
 * the reverse of a C double's order here. An extended value is stored as
 * three words: sign and exponent, then the mantissa's high and low words. */
static inline float ros_fpa_lds(uint32_t a)
{
    uint32_t w = ros_ld32(a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static inline void ros_fpa_sts(uint32_t a, double x)
{
    float f = (float)x;
    uint32_t w;
    memcpy(&w, &f, 4);
    ros_st32(a, w);
}

static inline double ros_fpa_ldd(uint32_t a)
{
    uint64_t b = (uint64_t)ros_ld32(a) << 32 | ros_ld32(a + 4);
    double x;
    memcpy(&x, &b, 8);
    return x;
}

static inline void ros_fpa_std(uint32_t a, double x)
{
    uint64_t b;
    memcpy(&b, &x, 8);
    ros_st32(a, (uint32_t)(b >> 32));
    ros_st32(a + 4, (uint32_t)b);
}

double ros_fpa_lde(uint32_t a);             /* runtime/fp.c */
double ros_fpa_ldp(uint32_t a);             /* FPA packed decimal, runtime/fp.c */
double ros_fpa_ldp_at(uint32_t a, uint32_t r10, uint32_t r11, uint32_t r12); /* may trap */
void ros_fpa_stp(uint32_t a, double x);

/* VFP's formats in memory are C's own: little-endian IEEE. */
static inline double ros_ldd(uint32_t a)
{
    double x;
    memcpy(&x, ros_ptr(a), 8);
    return x;
}

static inline void ros_std(uint32_t a, double x)
{
    memcpy(ros_ptr(a), &x, 8);
}

static inline float ros_lds(uint32_t a)
{
    float x;
    memcpy(&x, ros_ptr(a), 4);
    return x;
}

static inline void ros_sts(uint32_t a, float x)
{
    memcpy(ros_ptr(a), &x, 4);
}
void ros_fpa_ste(uint32_t a, double x);

/* ---- ways out of compiled code ------------------------------------------ */

/* An entry in the dispatcher: a code address that something takes the
 * address of, such as an ADR, a DCD table, an export or a module header
 * offset, and the compiled entry that stands for it. The source lists every
 * one, so the table is complete by construction. */
struct ros_code_entry {
    uint32_t addr;
    ros_code *fn;
    const char *name;
};

void ros_code_register(const struct ros_code_entry *entries, unsigned count);
ros_code *ros_code_lookup(uint32_t addr);

/* A code address for a C function, so compiled code can call it: a
 * native module's direct-call routine, or a wake-up routine a test hands
 * to a module. The function follows the register protocol: it takes the
 * state block, and returns by setting R15 to R14. Addresses are in ROM,
 * from ROS_NATIVE_BASE up. */
uint32_t ros_native_entry(ros_code *fn, const char *name);

/* An indirect call or jump: through the dispatcher to whatever compiled
 * entry stands at addr.  An address nothing registered is a fault in the
 * compiled code, reported with the address and the caller's pc. */
void ros_call(struct ros_cpu *s, uint32_t addr);

/* SWI n, from compiled code.  Constant SWIs compile to typed calls through
 * their thunks; this is the path for SWI instructions whose target the
 * compiler binds at run time, and the one every thunk shares. */
void ros_swi(struct ros_cpu *s, uint32_t number);
/* A SWI compiled code called directly, without X, failed (R0 -> the error):
 * to the error handler, with the caller's R10-R12 noted for it. */
__attribute__((noreturn)) void ros_swi_raise(struct ros_cpu *s);

/* A return that did not come back where its call expected. To the
 * processor it is a jump, so the dispatcher goes on at R15 until the code
 * returns where the call expected. It is an error if R15 is not compiled
 * code, or the chain does not come back. */
void ros_bad_return(struct ros_cpu *s, uint32_t expected);
/* The dispatcher's part of that: on at R15 until it is one of the
 * runtime's return addresses (&FFFFFFF0 up), or not compiled code. */
void ros_continue(struct ros_cpu *s);

/* Resume points (rosasm, for code that stores an lr and jumps back to it
 * later). A BL whose return address can be reached that way leaves one on
 * this chain, holding the address, sp as the BL left it, and the C frame.
 * A jump to the address longjmps to the frame whose BL made it, as the one
 * register file does with a single jump. Code that recurses leaves one
 * point per level at the same address, and sp tells which level a jump
 * means. The chain is the task's (its thread's). */
struct ros_resume {
    jmp_buf jb;
    uint32_t at, sp;
    struct ros_resume *prev;
};
extern _Thread_local struct ros_resume *ros_resume_top;

/* A jump to t: to the point t with the current sp, else the innermost at
 * t, else through the dispatcher */
void ros_resume(struct ros_cpu *s, uint32_t t);
/* Whether the chain holds a point at `at` with this sp. That is one live
 * frame's BL and no other level's: a transfer there is a jump into a
 * frame that is still below, which a caller may want to tell from a
 * return that left R15 lying about. */
int ros_resume_point_at(uint32_t at, uint32_t sp);

/* A checked call's return: the expected address, or the code unwinding
 * to a stored lr (see ros_resume), else a bad return. After a longjmp
 * to a resume point it is the first thing the frame does: it takes the
 * registers the jump carried (ros_resume_take).
 *
 * Inline, because every BL in every translated module ends in one and
 * nearly every one of them is the plain case: no registers held by a
 * jump, and the return where the call expected it. The rest is out of
 * line (dispatch.c). */
extern _Thread_local int ros_resume_regs_held;
void ros_check_return_slow(struct ros_cpu *s, uint32_t back);
static inline void ros_check_return(struct ros_cpu *s, uint32_t back)
{
    if (__builtin_expect(!ros_resume_regs_held && s->r[15] == back, 1))
        return;
    ros_check_return_slow(s, back);
}
/* The registers a jump to a resume point carried, into the struct of the
 * frame it landed in (the jumper's may be another's: the error
 * handler's); a no-op when none are held.  A resume point other than
 * rosasm's (swi.c's SWI entry) calls it when its setjmp returns 1. */
void ros_resume_take(struct ros_cpu *s);

/* The runtime longjmps out to a frame at `to` (an object in it): the
 * points in the frames it leaves are taken off the chain first. */
void ros_resume_unwind(const void *to);
/* ...and OS_CLI's alias expansions in progress there (oscli.c) */
void ros_oscli_unwind_below(const void *to);

/* Compiled code reaching what the compiler could not compile exactly: a
 * transfer into data, a jump table index out of its bound.  Raised as an
 * error naming the address, never guessed past. */
__attribute__((noreturn)) void ros_fault(struct ros_cpu *s, uint32_t addr, const char *why);

/* Compiled code's return to user mode from a block of seventeen words --
 * R0-R14, the PC, the PSR: `MSR SPSR` and `LDM rN, {R0-R14}^` then `MOVS
 * pc`, as the Wimp's CallBack handler ends (callback.c).  Ends the handler:
 * it sets R15 for the routine to return. */
void ros_user_return(struct ros_cpu *s, uint32_t block);

/* MSR CPSR_c: the mode and interrupt-disable bits.  Only SVC and USR exist
 * in the model; a switch to a mode with banked registers is a fault. */
static inline void ros_msr_c(struct ros_cpu *s, uint32_t psr, uint32_t addr)
{
    uint32_t mode = psr & 0x1F;
    if (mode != ROS_MODE_SVC && mode != ROS_MODE_USR)
        ros_fault(s, addr, "a switch to a processor mode with banked registers");
    s->mode = mode;
    s->irq_off = (psr >> 7) & 1;
}

/* Return addresses no compiled code can produce: the pc a native caller
 * gives compiled code, and the one the SWI dispatcher gives a module's SWI
 * handler, so that returning to them can be recognised. */
#define ROS_RETURN_TO_NATIVE 0xFFFFFFFCu
#define ROS_RETURN_FROM_SWI  0xFFFFFFF8u

/* A fresh state for native code calling into compiled code: the SVC stack,
 * SVC mode, interrupts on, flags clear, lr the native-return marker. */
void ros_cpu_enter(struct ros_cpu *s);

/* Where on the SVC stack native code's calls into compiled code begin.
 *
 * RISC OS has one SVC stack, and calls nest on it. Compiled code calls a
 * native SWI, which calls back into compiled code through a vector or a
 * service call, which calls another SWI. So every thunk from compiled to
 * native code records the caller's sp here for the duration of the call,
 * and ros_cpu_enter() starts below it, never over live frames. Raising
 * an error resets it to what the handler saw, flattening the stack as a
 * RISC OS error does. */
extern uint32_t ros_svc_sp;

/* The one rule of ros_svc_sp, for a SWI: while it runs, the caller's sp
 * is the top, if the caller is on the SVC stack below it, as compiled
 * module code is, with its frames there. A caller elsewhere (an
 * application on its own stack) leaves it as it is. */
static inline uint32_t ros_svc_sp_enter(const struct ros_cpu *s)
{
    uint32_t outer = ros_svc_sp;
    if (s->r[13] - ROS_SVCSTACK_BASE <= ROS_SVCSTACK_SIZE && s->r[13] < outer)
        ros_svc_sp = s->r[13];
    return outer;
}

/* A native SWI called from compiled code, directly (the compiler binds
 * native SWIs statically). ros_svc_sp and the call depth are as for any
 * SWI. The outermost one's way out is the same safe point, for background
 * work and callbacks, or a task computing in a loop of them (BASIC's
 * REPEAT MOUSE X,Y,B:UNTIL B) would never see its input. The caller
 * raises the error, if any (runtime/swi.c). */
void ros_native_swi(struct ros_cpu *s, void (*thunk)(struct ros_cpu *));

/* A loop's back-edge, in code compiled with --poll-loops (BASIC's). The
 * compiler puts one on every jump backwards and at the head of every loop
 * it lifts, with sp the code's R13 there. With background work queued it
 * is a safe point for it (a key, an Escape), so a loop that never calls
 * the OS can still take an interrupt (runtime/background.c). Otherwise it
 * costs one load and a branch. */
extern volatile int ros_work_pending;
void ros_safe_point(struct ros_cpu *s, uint32_t sp);
#define ROS_POLL(s, sp)                                                     \
    do {                                                                    \
        if (__builtin_expect(ros_work_pending, 0))                          \
            ros_safe_point(s, sp);                                          \
    } while (0)

/* OS_WriteS, which the compiler emits as a call: the string inline after
 * the SWI, in the image at addr, written through OS_WriteC; execution goes
 * on past it, which the compiled code does itself.  The compiler does not
 * pass the X bit, and OS_WriteS is almost always called without it, so an
 * error is raised (runtime/os_native.c). */
void ros_writes(struct ros_cpu *s, uint32_t addr);

#endif
