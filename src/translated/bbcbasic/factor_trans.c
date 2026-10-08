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
 * (Sources/Programmer/BASIC: s.ErrorMsgs, s.Factor, s.fp, s.fp2).
 */

/* factor_trans.c: BBC BASIC's transcendental functions, translated by
 * hand from Factor.s, fp.s and fp2.s (RISC OS 5.31's BASIC, the VFP
 * build).  This is unit 1 of the tier 3 translation.  Unit 0
 * (units/basicvfp_hand.c) states the conventions.
 *
 * In the VFP build every one of these functions takes the same six
 * steps, and the hand code keeps them visible:
 *
 *   ENTRY  STR R14,[SP,#-4]!        save the return address handed in
 *          BL FACTOR                evaluate the argument
 *          BLPL FLOATQ              integer -> double, string rejected
 *          VFPElementary <op>       D0 = op(D0), through the VFPSupport
 *                                  table at [ARGP-8].  The entries are
 *                                  the runtime's own libm wrappers
 *                                  (elementary.c)
 *          FPSCRCheck R14           IOC, DZC or OFC set -> the error
 *          MOVS TYPE,#TFP           the result is a float: N set, Z clear
 *          LDR PC,[SP],#4           return
 *
 * DEG and RAD multiply by a constant instead of calling the table
 * (Factor.s:1030-1046, FMULFSINSTK).  The constants are the assembler's
 * DCFD decimal strings, copied exactly.  The multiply is a plain C
 * double multiply, which is how the lift renders FMULD.  The FPSCR check
 * after it reads what the runtime holds there, as the lift's check does.
 *
 * These quirks are kept on purpose.  Each is given with its line:
 *   - A string argument is rejected by FLOATQ's BEQ ERTYPEINT
 *     (fp.s:23).  The error is "Type mismatch: number needed" and not
 *     the maths error.
 *   - An integer argument is converted and a float is left alone
 *     (fp.s:22, FLOATZ's MOVMI PC,R14).  So SIN(1) is evaluated in
 *     doubles either way, and the result is always a float.
 *   - Domain and overflow errors come from the FPSCR that the table
 *     call left (Definitions.hdr:164 FPSCRCheck).  The lift's
 *     VFPException gives "Division by zero" for DZC, "Overflow" for OFC
 *     and otherwise the MSG error.  It is called here through the
 *     lift's own function, so the wording and the error numbers are
 *     the same as the lift's by construction.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "factor_trans.h"

/* The generated interpreter's evaluator, exported by the substitution. */
void basicvfp_FACTOR(struct ros_cpu *s);
/* The lift's error path for a raised FPSCR flag (ErrorMsgs: "Division
 * by zero", "Overflow", "Invalid...").  Never returns. */
void basicvfp_VFPException(struct ros_cpu *s);

#define T_STRING 0u
#define T_INTEGER 0x40000000u
#define T_FLOAT 0x80000000u

/* ERTYPEINT's number (ErrorMsgs.s:258, = 6,28): what ERR reports. */
#define ERR_NUMBER_NEEDED 6u

/* VFPTRANSCENDENTALS, at ARGP-8 (WorkSpace).  The lift reads r8 - 8. */
#define VFPTRANSCENDENTALS_OFF 8u

/* The elementary table's entries, four bytes apart (vfpsupport.c).
 * These are the lift's own offsets, taken from its BLX sites. */
#define FN_SIN 0u
#define FN_COS 4u
#define FN_TAN 8u
#define FN_ASIN 12u
#define FN_ACOS 16u
#define FN_ATAN 20u
#define FN_LOG 28u
#define FN_LOG10 32u
#define FN_EXP 36u

/* A return address nobody checks, as unit 0's eval() uses. */
#define EVAL_RETURN_MARKER 0xFFFFFFF0u

/* The table call, as the lift renders `VFPElementary $op` (its BLX ip).
 * The entry is a native function that the runtime dispatches.  It
 * returns by writing R15 = R14, which is the marker set here.  LINE
 * (R12) is not live across the call in this code.  The macro's Push and
 * Pull of it only move the stack down and back, with no effect that can
 * be seen, so they are left out. */
static void elementary(struct ros_cpu *s, uint32_t fn)
{
    uint32_t t = ros_ld32(s->r[8] - VFPTRANSCENDENTALS_OFF) + fn;
    s->r[14] = EVAL_RETURN_MARKER;
    ros_call(s, t);
}

/* FPSCRCheck (Definitions.hdr:164): IOC, DZC or OFC set -> the error. */
static void check_fpscr(struct ros_cpu *s)
{
    if (s->fp->fpscr & (1u | 2u | 4u)) {
        s->r[14] = EVAL_RETURN_MARKER;
        basicvfp_VFPException(s); /* does not return */
    }
}

/* BL FACTOR then BLPL FLOATQ (fp.s:20-27): the argument as a double in
 * D0.  A string raises ERTYPEINT here and not at the table call,
 * because FLOATQ tests for it first. */
static double eval_argument(struct ros_cpu *s)
{
    s->r[14] = EVAL_RETURN_MARKER;
    basicvfp_FACTOR(s);
    if (s->r[9] == T_STRING)
        ros_raise(ros_error(ERR_NUMBER_NEEDED,
                            "Type mismatch: number needed"));
    if (s->r[9] == T_INTEGER) {
        s->fp->vfp.d[0] = (double)(int32_t)s->r[0];
        s->r[9] = T_FLOAT;
    }
    return s->fp->vfp.d[0];
}

/* FSINSTK (Factor.s:747, the shared tail).  The result is the float in
 * D0 and TYPE is float.  It returns with N set and Z clear, as
 * MOVS TYPE,#TFP leaves them. */
static void return_float(struct ros_cpu *s, uint32_t ret_to)
{
    s->r[9] = T_FLOAT;
    s->z = 0, s->n = 1;
    s->r[15] = ret_to;
}

/* One transcendental: evaluate, convert, call, check, return. */
#define TRANSCENDENTAL(name, fn, line)                                  \
    void basicvfp_hand_##name(struct ros_cpu *s)                        \
    {                                                                   \
        uint32_t ret_to = s->r[14];                                     \
        (void)eval_argument(s);                                         \
        elementary(s, fn);                                              \
        check_fpscr(s);                                                 \
        return_float(s, ret_to);                                        \
    }                                                                   \
    struct basicvfp_hand_trailing_semicolon_##name

/* SIN (fp2.s:59; the FPOINT=2 block at :183). */
TRANSCENDENTAL(SIN, FN_SIN, fp2);
/* COS (fp2.s:21; :48). */
TRANSCENDENTAL(COS, FN_COS, fp2);
/* TAN (Factor.s:1243; the FPOINT=2 branch at :1398, B FSINSTK). */
TRANSCENDENTAL(TAN, FN_TAN, factor);
/* ASN (fp.s; the asin expansion at :952). */
TRANSCENDENTAL(ASN, FN_ASIN, fp);
/* ACS (Factor.s:689; :700). */
TRANSCENDENTAL(ACS, FN_ACOS, factor);
/* ATN (fp2.s; the atan expansion at :281, B FSINSTK). */
TRANSCENDENTAL(ATN, FN_ATAN, fp2);
/* LN (Factor.s:979; :987, B FSINSTK). */
TRANSCENDENTAL(LN, FN_LOG, factor);
/* LOG (Factor.s:993; :1005).  This is log10.  LN is the natural log. */
TRANSCENDENTAL(LOG, FN_LOG10, factor);
/* EXP (Factor.s:736; :744, FSINSTK at :747). */
TRANSCENDENTAL(EXP, FN_EXP, factor);

/* DEG (Factor.s:1012): radians -> degrees, x * 180/pi.  The constant
 * is F180DP's DCFD string (Factor.s:1037), copied exactly.  The
 * assembler and the C compiler round it to the same double. */
void basicvfp_hand_DEG(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    double v = eval_argument(s);
    s->fp->vfp.d[0] = v * 57.2957795130823208767981548141;
    check_fpscr(s);
    return_float(s, ret_to);
}

/* RAD (Factor.s:1017): degrees -> radians, x * pi/180.  The constant
 * is FPID180's DCFD string (Factor.s:1039). */
void basicvfp_hand_RAD(struct ros_cpu *s)
{
    uint32_t ret_to = s->r[14];
    double v = eval_argument(s);
    s->fp->vfp.d[0] = v * 0.0174532925199432957692369076849;
    check_fpscr(s);
    return_float(s, ret_to);
}
