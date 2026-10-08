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
 * (Sources/Programmer/BASIC: s.Expr).
 */

/* expr_compr.c: BASIC's comparison, translated by hand from COMPR in
 * Expr.s (RISC OS 5.31's BASIC, the VFP build).  It is the typed
 * three-way compare behind every =, <> and ordering test.
 *
 * The answer is in the condition codes and is not a value.  The
 * statement and operator code that calls this reads C (and the
 * compare of the lengths) after the return.  The caller turns it into
 * TRUE or FALSE itself.  IACC is simply zeroed on the way out.  The
 * result is the last compare: bytes for strings, FCMPD for floats, or
 * CMP for integers.
 *
 * The three paths (Expr.s):
 *   - Strings.  The left string is pushed and the right evaluated.
 *     The right must be a string too.  The two are compared byte by
 *     byte up to the length of the shorter.  If they are equal that
 *     far, their lengths decide (CMPEQ R0,TYPE).  The comparison is
 *     unsigned, a byte at a time, from STRACC against the pushed copy.
 *   - Floats.  The left FACC is pushed and the right evaluated, and
 *     converted if it is an integer.  Then FCMPD D1,FACC is done
 *     through the runtime's exact helpers, IOC included
 *     (ros_vfp_cmp_ex).
 *   - Integers.  The left IACC is pushed.  If the right is also an
 *     integer, one CMP is done and the routine returns.  If the right
 *     is a float, the left is converted through S2 and joins the
 *     float compare.
 *
 * The recursive evaluator (EXPRRECUR) is the lift's own, exported.
 * Its non-local exits unwind this C frame just as they unwind the
 * lift's.  Nothing resumes into the middle of a comparison.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "expr_compr.h"

void basicvfp_EXPRRECUR(struct ros_cpu *s);
void basicvfp_ERTYPESTR(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_hand_SPUSH(struct ros_cpu *s);

#define T_INTEGER 0x40000000u
#define STRACC_OFF 1536u

/* The common exit, which pops {R7,PC} off the stack.  Each path
 * stores back different registers.  The string path stores the
 * registers of its walk, and the float paths store only IACC and
 * TYPE.  So each path passes what the lift's own exit block stored. */
static void compr_pop_ret(struct ros_cpu *s, uint32_t r13)
{
    s->r[7] = ros_ld32(r13);                /* LDMFD SP!,{R7,PC} */
    s->r[13] = r13 + 8;
    s->r[15] = ros_ld32(r13 + 4);
}

void basicvfp_hand_COMPR(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2 = s->r[2], r3, r5, r6, r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r13 = s->r[13], r14 = s->r[14];

    ros_logic(s, r9, s->c);                 /* TEQ TYPE,#0 */

    if (r9 == 0) {                          /* BEQ STNCMP: strings */
        r13 -= 8;                           /* STMFD SP!,{R7,R14} */
        ros_st32(r13, r7);
        ros_st32(r13 + 4, r14);
        s->r[13] = r13;
        basicvfp_hand_SPUSH(s);             /* BL SPUSH; preserves R5 */
        r13 = s->r[13];
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPRRECUR(s);              /* the right-hand side */
        r0 = s->r[0];
        r2 = s->r[2];
        r9 = s->r[9];
        r13 = s->r[13];
        r14 = s->r[14];
        ros_logic(s, r9, s->c);             /* TEQ TYPE,#0 */
        if (r9 != 0) {
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_ERTYPESTR(s);
        }
        r0 = ros_ld32(r13);                 /* the pushed CLEN */
        r13 += 4;
        r9 = r2;                            /* TYPE := the old CLEN */
        if (r2 >= r0)
            r2 = r0;                        /* the shorter length */
        r1 = r8 - STRACC_OFF;
        r3 = (r0 - r1 + 3) & ~3u;           /* the pushed words */
        r5 = r13;
        for (;;) {
            ros_subs(s, r2, r1);            /* CMP CLEN,R1 */
            if (r2 != r1) {
                r6 = ros_ld8(r1);
                r1 += 1;
                r7 = ros_ld8(r5);           /* the first string */
                r5 += 1;
                ros_subs(s, r7, r6);        /* CMP R7,R6 */
                if (r7 == r6)
                    continue;
            }
            if (s->z)
                ros_subs(s, r0, r9);        /* CMPEQ R0,TYPE */
            r13 += r3;
            s->r[0] = 0;                    /* IACC := 0, like FALSE */
            s->r[1] = r1;
            s->r[2] = r2;
            s->r[3] = r3;
            s->r[5] = r5;
            s->r[6] = r6;
            s->r[9] = T_INTEGER;
            compr_pop_ret(s, r13);
            return;
        }
    }

    if ((int32_t)r9 < 0) {                  /* BMI FCOMPR: left float */
        ros_st32(r13 - 8, r7);              /* STMFD SP!,{R14,R7} */
        ros_st32(r13 - 4, r14);
        r13 -= 16;                          /* FSTD FACC,[SP,#-8]! */
        ros_std(r13, s->fp->vfp.d[0]);
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPRRECUR(s);
        r0 = s->r[0];
        r9 = s->r[9];
        r13 = s->r[13];
        ros_logic(s, r9, s->c);             /* TEQ TYPE,#0 */
        if (r9 == 0) {
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_ERTYPEINT(s);
        }
        if ((int32_t)r9 >= 0) {             /* the right was an int */
            s->fp->vfp.sw[0] = r0;
            s->fp->vfp.d[0] = (double)(int32_t)r0;
        }
        s->fp->vfp.d[1] = ros_ldd(r13);     /* the first back */
        r13 += 8;
    } else {                                /* left integer */
        r13 -= 12;                          /* STMFD SP!,{R14,R7,IACC} */
        ros_st32(r13, r0);
        ros_st32(r13 + 4, r7);
        ros_st32(r13 + 8, r14);
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPRRECUR(s);
        r0 = s->r[0];
        r9 = s->r[9];
        r13 = s->r[13];
        r14 = s->r[14];
        ros_logic(s, r9, s->c);             /* TEQ TYPE,#0 */
        if (r9 == 0) {
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_ERTYPEINT(s);
        }
        if ((int32_t)r9 >= 0) {             /* both integers: one CMP */
            r1 = ros_ld32(r13);             /* LDMFD SP!,{R14,R7,R1} */
            r7 = ros_ld32(r13 + 4);
            r14 = ros_ld32(r13 + 8);        /* the caller's address */
            r13 += 12;
            ros_subs(s, r1, r0);            /* CMP R1,IACC */
            s->r[0] = 0;
            s->r[1] = r1;
            s->r[7] = r7;
            s->r[13] = r13;
            s->r[14] = r14;
            s->r[15] = r14;
            return;
        }
        s->fp->vfp.sw[2] = ros_ld32(r13);   /* FLDS S2,[SP],#4 */
        r13 += 4;
        s->fp->vfp.d[1] = (double)(int32_t)s->fp->vfp.sw[2];
    }

    /* the float compare */
    s->r[9] = T_INTEGER;
    s->fp->fpscr |= ros_vfp_cmp_ex(s->fp->vfp.d[1], s->fp->vfp.d[0], 0);
    ros_vfp_cmp(s, s->fp->vfp.d[1], s->fp->vfp.d[0]);
    ros_vmrs_flags(s);                      /* FMRX PC,FPSCR */
    s->r[0] = 0;
    s->r[9] = T_INTEGER;
    compr_pop_ret(s, r13);
}
