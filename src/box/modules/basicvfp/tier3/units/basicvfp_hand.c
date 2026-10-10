/* Copyright 2001 Pace Micro Technology plc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a translation into C of RISC OS Open's BBC BASIC source
 * (Sources/Programmer/BASIC: s.ErrorMsgs, s.Factor, s.fp).
 */

/* basicvfp_hand.c -- BBC BASIC's built-in functions, translated by hand
 * from Factor.s (RISC OS 5.31's BASIC, the VFP build).
 *
 * How the original was shaped, and what changed in the translation:
 *
 *   Every function there begins `STR R14,[SP,#-4]!`, evaluates its
 *   argument with BL FACTOR or BL EXPR, and ends `LDR PC,[SP],#4`. The
 *   push and the pop into pc cancel out. So here the return address is
 *   simply handed in and put back in R15. The string functions save
 *   their source to the interpreter's own stack with SPUSH and recover
 *   it with SPULL. These are two copy loops that move a word at a time,
 *   with the length rounded up. Here the source is an ordinary C local,
 *   which is the whole point of the exercise. The type checks read the
 *   accumulator's TYPE after the argument. The original tests the
 *   condition codes that FACTOR left (Z: string, MI: float, PL:
 *   integer). Here TYPE is tested directly.
 *
 *   The quirks are kept on purpose, and each says which line of the
 *   source it came from:
 *     - ASC("") is -1 (Factor.s:686, "null string gives -1");
 *     - LEFT$(A$) is A$ less its last character (:1529);
 *     - LEFT$(A$,N) with N<0 takes the whole string (:1516, the length
 *       compare is unsigned);
 *     - MID$(A$,0,...) reads from the first character, because only a
 *       nonzero start is decremented (:1560);
 *     - INSTR's optional start is clamped to 0 if it is negative or
 *       over 255 (:897). An empty pattern finds the start position;
 *     - STRING$ over 255 characters is an error, checked at each append
 *       (:1636), as in the original.
 */

#include <math.h>
#include <string.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "basicvfp_hand.h"

/* The generated interpreter's evaluators, which the comparison patch
 * exports from its translation unit. */
void basicvfp_EXPR(struct ros_cpu *s);
void basicvfp_FACTOR(struct ros_cpu *s);

/* The types, as Definitions.hdr spells them. */
#define T_STRING 0u
#define T_INTEGER 0x40000000u
#define T_FLOAT 0x80000000u

/* BASIC's error numbers, as the MSG pairs (= number, token) in
 * ErrorMsgs.s give them. ERR reports a plain number. The farm's golden
 * output prints "String too long 19", and not &4113. */
#define ERR_NUMBER_NEEDED 6u    /* ERTYPEINT/ERTYPESTR: "Type mismatch" */
#define ERR_MISSING_COMMA 5u    /* ERCOMM: "Missing ," */
#define ERR_MISSING_BRACKET 27u /* ERBRA: "Missing )" */
#define ERR_STRING_TOO_LONG 19u /* ERLONG: "String too long" */

/* The lift's FPSCR exception path (ErrorMsgs.s:882). DZC gives
 * "Division by zero" (18), OFC gives "Number too big" (20), and
 * anything else gives "Invalid arithmetic operation" (55). The SQRT
 * variant (:879) first turns IOC into "Negative root" (21).
 * VFPException is the lift's own function, exported. The lifter merged
 * the SQRT variant into its callers as a goto, so that one is written
 * out here. Neither returns. */
void basicvfp_VFPException(struct ros_cpu *s);

#define FPSCR_IOC 1u
static void raise_basic(uint32_t num, const char *what);

static void sqrt_exception(struct ros_cpu *s)
{
    uint32_t f = s->fp->fpscr;
    if (f & FPSCR_IOC)
        raise_basic(21, "Negative root"); /* FSQRTN: ErrorMsgs.s:519 */
    if (f & 2u)
        raise_basic(18, "Division by zero"); /* ZDIVOR: :492 */
    if (f & 4u)
        raise_basic(20, "Number too big"); /* FOVR: :504 */
    raise_basic(55, "Invalid arithmetic operation"); /* EINVOP: :888 */
}

/* The workspace: STRACC is the 256-byte string accumulator at
 * ARGP-&600. (In WorkSpace, ERRORS is at -&700 and STRACC follows it.)
 * A string value is its bytes, with CLEN (R2) pointing one past the
 * last. The original computes lengths as CLEN-STRACC throughout. */
#define STRACC_OFF 0x600u

static char *stracc(struct ros_cpu *s)
{
    return (char *)ros_ptr(s->r[8] - STRACC_OFF);
}

static unsigned strlen_(struct ros_cpu *s)
{
    return (unsigned)(s->r[2] - (s->r[8] - STRACC_OFF));
}

/* A string as C sees it: the accumulator's bytes. It is valid until the
 * next evaluation, which is exactly how the original treats STRACC. */
static char *cur_string(struct ros_cpu *s)
{
    return stracc(s);
}

static void set_string_result(struct ros_cpu *s, const char *p, unsigned n)
{
    char *acc = stracc(s);
    if (n > 255)
        n = 255;
    memcpy(acc, p, n);
    s->r[2] = s->r[8] - STRACC_OFF + n; /* CLEN: one past the end */
    s->r[9] = T_STRING;
    /* The condition codes on return, as every factor promises them:
     * Z for a string, N for a float (Definitions.hdr: TYPE's top bits). */
    s->z = 1, s->n = 0;
}

static void set_int_result(struct ros_cpu *s, int32_t v, uint32_t ret_to)
{
    s->r[0] = (uint32_t)v;
    s->r[9] = T_INTEGER;
    s->z = 0, s->n = 0;
    s->r[15] = ret_to;
}

static void set_float_result(struct ros_cpu *s, double v, uint32_t ret_to)
{
    s->fp->vfp.d[0] = v;
    s->r[9] = T_FLOAT;
    s->z = 0, s->n = 1;
    s->r[15] = ret_to;
}

/* Raise an error the way the original's MSG path does. The error block
 * holds BASIC's number and text, and is handed to the environment. */
static void raise_basic(uint32_t num, const char *what)
{
    ros_raise(ros_error(num, "%s", what));
}

/* ---- the argument evaluators ---------------------------------------
 *
 * The generated interpreter exports its two evaluators (the patcher
 * removes their `static`). Calling one evaluates the expression that
 * starts at AELINE. It leaves the value in the accumulator, the next
 * character in the lookahead R10, and AELINE past it. Every original
 * caller relies on this protocol. R14 carries a marker return address.
 * The evaluator's own return convention writes it to R15, and nothing
 * here checks it. */
#define EVAL_RETURN_MARKER 0xFFFFFFF0u

static void eval(struct ros_cpu *s, void (*fn)(struct ros_cpu *))
{
    s->r[14] = EVAL_RETURN_MARKER;
    fn(s);
}

/* One numeric argument, as ABS and the like read it: FACTOR, then the
 * type check. It returns the value as a double for the numeric
 * functions. The original also keeps it as a float (FACC) until it has
 * to choose. */
static double eval_number(struct ros_cpu *s)
{
    eval(s, basicvfp_FACTOR);
    if (s->r[9] == T_STRING)
        raise_basic(ERR_NUMBER_NEEDED, "Type mismatch: number needed");
    if (s->r[9] == T_INTEGER)
        return (double)(int32_t)s->r[0];
    return s->fp->vfp.d[0];
}

/* One string argument: the accumulator's bytes are copied out and the
 * length is returned. The original then saves it with SPUSH. Here the
 * caller copies it to wherever it must survive the next evaluation.
 *
 * There are two protocols, and the original chooses one for each
 * function. FACTOR's callers (LEN, ASC) get their argument with
 * FACTOR, which leaves the lookahead UNREAD. The string builders
 * (LEFT$, MID$, RIGHT$, STRING$, INSTR) evaluate with EXPR, which has
 * already read the lookahead into R10. The wrong choice swallows or
 * duplicates the separator that follows. */
static unsigned eval_string_as(struct ros_cpu *s, void (*fn)(struct ros_cpu *),
                               char *out /* 256 bytes */)
{
    eval(s, fn);
    if (s->r[9] != T_STRING)
        raise_basic(ERR_NUMBER_NEEDED, "Type mismatch: string needed");
    unsigned n = strlen_(s);
    memcpy(out, cur_string(s), n);
    return n;
}

static unsigned eval_string(struct ros_cpu *s, char *out)
{
    return eval_string_as(s, basicvfp_FACTOR, out);
}

/* INTEGY/INTEGZ (fp.s:66, one routine with two entries): the value in
 * the accumulator truncated toward zero, by FTOSIZD as the VFP build
 * does it (fp.s:77-80). A value that FTOSIZD cannot represent sets
 * IOC. That is a value outside [-2^31, 2^31), or a NaN. The check
 * after it then raises "Invalid arithmetic operation" through
 * VFPException. The C cast would only saturate, silently. This does no
 * evaluation, because the argument has already been read. */
static int32_t acc_int(struct ros_cpu *s)
{
    if (s->r[9] == T_STRING)
        raise_basic(ERR_NUMBER_NEEDED, "Type mismatch: number needed");
    if (s->r[9] == T_FLOAT) {
        double t = trunc(s->fp->vfp.d[0]);
        if (!(t >= -2147483648.0 && t <= 2147483647.0)) {
            s->fp->fpscr |= FPSCR_IOC;
            s->r[14] = EVAL_RETURN_MARKER;
            basicvfp_VFPException(s); /* does not return */
        }
        s->r[0] = (uint32_t)(int32_t)t;
        s->r[9] = T_INTEGER;
    }
    return (int32_t)s->r[0];
}

/* The lookahead and the separators, as the original reads them. */
static int look(struct ros_cpu *s)
{
    return (int)(s->r[10] & 0xFF);
}

static void want(struct ros_cpu *s, int ch, const char *what)
{
    if (look(s) != ch)
        raise_basic(ERR_MISSING_COMMA, what); /* ERCOMM: "Missing ," */
}

/* BRA, as the string functions' second arguments go through it. It
 * evaluates the expression after the comma, which may itself be
 * bracketed. */
static void eval_rhs(struct ros_cpu *s)
{
    eval(s, basicvfp_EXPR);
}

/* ---- the numeric functions ----------------------------------------- */

/* ABS: a float's absolute value. An integer is negated only if it is
 * negative. The type is kept, so what came in as an integer goes out
 * as one (Factor.s:651). */
void basicvfp_hand_ABS(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    eval_number(s);
    if (s->r[9] == T_FLOAT)
        set_float_result(s, fabs(s->fp->vfp.d[0]), ret_to);
    else {
        int32_t v = (int32_t)s->r[0];
        if (v < 0)
            v = -v; /* -2^31 stays, as RSB leaves it */
        set_int_result(s, v, ret_to);
    }
}

/* SGN: -1, 0 or 1, floats and integers alike (Factor.s:1215). */
void basicvfp_hand_SGN(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    double v = eval_number(s);
    set_int_result(s, v < 0 ? -1 : v > 0 ? 1 : 0, ret_to);
}

/* INT: floor for floats. It rounds toward minus infinity, exactly as
 * Factor.s:957-966 does it:
 *   - the whole FPSCR is set to RMODE_DOWN. The FMXR writes the whole
 *     register, and so clears any flags too;
 *   - FTOSID converts, and sets IOC for a value it cannot represent;
 *   - the check then raises "Invalid arithmetic operation" (55);
 *   - on success the FPSCR is written back to zero, and not to what it
 *     held before.
 * Integers return unchanged (:934, the early return on the PL
 * condition). */
void basicvfp_hand_INT(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    eval_number(s);
    if (s->r[9] != T_FLOAT) {
        set_int_result(s, (int32_t)s->r[0], ret_to);
        return;
    }
    s->fp->fpscr = 0x800000u; /* FPSCR_RMODE_DOWN, and nothing else */
    double t = floor(s->fp->vfp.d[0]);
    if (!(t >= -2147483648.0 && t < 2147483648.0)) {
        s->fp->fpscr |= FPSCR_IOC;
        s->r[14] = EVAL_RETURN_MARKER;
        basicvfp_VFPException(s); /* does not return */
    }
    s->fp->fpscr = 0; /* the FMXR that restores the default mode */
    set_int_result(s, (int32_t)t, ret_to);
}

/* SQR: always a float. Integers are converted first (FLOATQ, :1413).
 * A negative argument gives an error and does not return a NaN.
 * FSQRTD sets IOC, and SQR's own FPSCRCheck variant (:1416,
 * FPSCRCheck R14,_SQRT) passes it to VFPException_SQRT. Its IOC case
 * is "Negative root" (21, ErrorMsgs.s:519). */
void basicvfp_hand_SQR(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    double v = eval_number(s);
    if (v < 0) {
        s->fp->fpscr |= FPSCR_IOC;
        s->r[14] = EVAL_RETURN_MARKER;
        sqrt_exception(s); /* does not return */
    }
    set_float_result(s, sqrt(v), ret_to);
}

/* ---- the string interrogators -------------------------------------- */

/* LEN (Factor.s:972). */
void basicvfp_hand_LEN(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    char tmp[256];
    unsigned n = eval_string(s, tmp);
    set_int_result(s, (int32_t)n, ret_to);
}

/* ASC: the first character, or -1 for the empty string (Factor.s:679,
 * the TRUE label's MVN). */
void basicvfp_hand_ASC(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    char tmp[256];
    unsigned n = eval_string(s, tmp);
    set_int_result(s, n ? (int32_t)(uint8_t)tmp[0] : -1, ret_to);
}

/* INSTR(haystack, needle [, start]): the position counts from 1, and is
 * 0 when the needle is not there. The original (Factor.s:876) searches
 * the saved first string for the second string, which is in the
 * accumulator. The start counts from 1. It is decremented once, and
 * clamped to nothing if it is negative or over 255. An empty needle
 * finds the start position. A needle longer than the haystack, or one
 * that runs off its end, finds nothing. */
void basicvfp_hand_INSTR(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    char hay[256], needle[256];
    unsigned hlen = eval_string_as(s, basicvfp_EXPR, hay);
    want(s, ',', "Missing ,");
    unsigned nlen = eval_string_as(s, basicvfp_EXPR, needle);

    unsigned off = 0; /* the clamped 0-based start */
    if (look(s) == ',') {
        eval_rhs(s);
        int32_t start = acc_int(s);
        off = start > 0 && start <= 256 ? (unsigned)start - 1 : 0;
    }

    int32_t found = 0;
    if (nlen <= hlen && off + nlen <= hlen) {
        if (nlen == 0)
            found = (int32_t)off + 1;
        else
            for (;; off++) {
                if (memcmp(hay + off, needle, nlen) == 0) {
                    found = (int32_t)off + 1;
                    break;
                }
                if (off + nlen == hlen)
                    break;
            }
    }
    set_int_result(s, found, ret_to);
}

/* ---- the string builders ------------------------------------------- */

/* CHR$: one character, the argument's low byte (Factor.s:1481). Its
 * SINSTR tail stores it with STRB. */
void basicvfp_hand_CHRD(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    eval(s, basicvfp_FACTOR); /* the argument, then INTEGY */
    int32_t c = acc_int(s);
    char one = (char)c;
    set_string_result(s, &one, 1);
    s->r[15] = ret_to;
}

/* LEFT$(A$ [, N]): the first N characters. With no N it gives all but
 * the last character (Factor.s:1529). N=0 gives the empty string. N<0
 * gives the whole string, because the original's one compare reads it
 * as unsigned (:1516). */
void basicvfp_hand_LEFTD(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    char src[256];
    unsigned n = eval_string_as(s, basicvfp_EXPR, src);
    if (look(s) == ',') {
        eval_rhs(s);
        int32_t want_ = acc_int(s);
        n = want_ >= 0 && (unsigned)want_ < n ? (unsigned)want_ : n;
    } else if (n > 0)
        n -= 1; /* the one-argument form drops the last character */
    set_string_result(s, src, n);
    s->r[15] = ret_to;
}

/* RIGHT$(A$ [, N]): the last N characters. With no N it gives just the
 * last one (Factor.s:1581). If N is at least the length, the result is
 * the whole string. The original notices this and copies nothing
 * (:1574). N=0 gives the empty string. */
void basicvfp_hand_RIGHTD(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    char src[256];
    unsigned n = eval_string_as(s, basicvfp_EXPR, src);
    unsigned take;
    if (look(s) == ',') {
        eval_rhs(s);
        int32_t want_ = acc_int(s);
        take = want_ >= 0 && (unsigned)want_ < n ? (unsigned)want_ : n;
    } else
        take = 1;
    if (n == 0)
        take = 0;
    set_string_result(s, src + (n - take), take);
    s->r[15] = ret_to;
}

/* MID$(A$, start [, length]): length characters, or up to the end,
 * from the character at start (Factor.s:1534). The start counts from
 * 1, but 0 is not decremented (:1560). So 0 reads from the first
 * character, as 1 does. A start past the end, or a length of 0, gives
 * the empty string. */
void basicvfp_hand_MIDD(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    char src[256];
    unsigned n = eval_string_as(s, basicvfp_EXPR, src);
    want(s, ',', "Missing ,");
    eval_rhs(s);
    unsigned start = (unsigned)acc_int(s);

    unsigned take = 255; /* the default: the rest of the string */
    if (look(s) == ',') {
        eval_rhs(s);
        take = (unsigned)acc_int(s);
    }
    if (look(s) != ')')
        raise_basic(ERR_MISSING_BRACKET, "Missing )"); /* ERBRA */

    if (start != 0)
        start -= 1; /* MID$(A$,0,...) reads from the first character */
    if (start > n)
        start = n, take = 0;
    if (take > n - start)
        take = n - start;
    set_string_result(s, src + start, take);
    s->r[15] = ret_to;
}

/* STRING$(count, A$): A$ repeated count times (Factor.s:1619). A count
 * below one gives the empty string. A count of one gives A$ itself.
 * Each further append is checked against the 255-byte accumulator.
 * Going over that is ERLONG (:1636), which is kept as the original's
 * error. */
void basicvfp_hand_STRND(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    eval_rhs(s); /* the count comes first */
    int32_t count = acc_int(s);
    want(s, ',', "Missing ,");
    char unit[256];
    unsigned unit_len = eval_string_as(s, basicvfp_EXPR, unit);

    if (count < 1) { /* RNUL: the empty string */
        set_string_result(s, unit, 0);
        s->r[15] = ret_to;
        return;
    }
    char out[256];
    unsigned n = unit_len;
    memcpy(out, unit, n);
    for (int32_t i = 1; i < count; i++) {
        if (n + unit_len > 255)
            raise_basic(ERR_STRING_TOO_LONG, "String too long"); /* ERLONG */
        memcpy(out + n, unit, unit_len);
        n += unit_len;
    }
    set_string_result(s, out, n);
    s->r[15] = ret_to;
}
