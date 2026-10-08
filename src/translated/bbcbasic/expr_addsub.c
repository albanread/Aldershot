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

/* expr_addsub.c: BASIC's + and -, translated by hand from
 * Expr.s:902-1024 (RISC OS 5.31's BASIC, the VFP build).
 *
 * The expression evaluator is a precedence climber.  EXPRRECUR reads
 * the right operand with the operator's priority stacked under it.
 * Each operator's continuation (the functions in this file) then runs
 * with R7 holding that priority.  R10 holds the next operator's word
 * from PRIORTABLE, which is priority<<24 | a jump-table index.
 *
 * After the arithmetic the continuation does what every precedence
 * climber does, but with returns in place of recursion.  It does
 * CMP R7,R10,LSR #28.  If the stacked priority is the higher, it
 * returns up (LDRCS PC,[SP],#4) and leaves the result for the caller.
 * Otherwise it chains straight into the next operator through the
 * jump table (EXPRCALL: R4 := AJ7, R14 := the index).  The common exit
 * EXPRPULLNEXT makes the same compare after popping the operator's
 * own saved R7.
 *
 * Both operators handle three kinds of operand.  + concatenates
 * strings.  The pushed copy is moved to the top of the accumulator,
 * with its byte tail, and 256 is the limit.  Both + and - mix integers
 * and floats by converting the integer side where it stands.
 * FPLUST/FMINUT convert the left operand (popped through S2).
 * FPLUS/FMINUS convert the right operand (in the accumulator).  The
 * float result goes out through the exact exception record of
 * FADDD/FSUBD (ros_vfp_ex2) and the FPSCR check.
 *
 * The quirks kept, with their lines:
 *   - A string + string whose total length is over 255 gives "String
 *     too long" (BCS ERLONG, Expr.s:928).  This is checked before any
 *     bytes move.
 *   - An empty first string skips the copy entirely (BEQ
 *     EXPRPULLNEXT, :924).
 *   - The concatenation first moves the second string to the top, then
 *     copies words down from the stack.  So the accumulator's tail
 *     bytes ride along exactly as they do in the original.
 *   - A string where a number is wanted gives ERTYPEINT.  A number
 *     where a string is wanted gives ERTYPESTR.  The error comes from
 *     the operator and not from the operand.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "expr_addsub.h"

void basicvfp_EXPRRECUR(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_ERTYPESTR(struct ros_cpu *s);
void basicvfp_ERLONG(struct ros_cpu *s);
void basicvfp_VFPException(struct ros_cpu *s);
/* The operator jump table (ADD PC,PC,R14,LSL #2 in EXPRHARD).  It
 * chains into the next operator. */
void basicvfp_FC102830(struct ros_cpu *s);
void basicvfp_hand_SPUSH(struct ros_cpu *s);

#define T_FLOAT 0x80000000u
#define STRACC_OFF 1536u
#define AJ7 0xFC1026A0u

/* The evaluator's call, with R4 and R14 staged as EXPRCALL does. */
static void exprcall(struct ros_cpu *s)
{
    s->r[4] = AJ7;
    s->r[14] = s->r[10] & 0xFFFFFFu;
    basicvfp_FC102830(s);
}

/* The defer-or-chain exit.  It compares the stacked priority (R7)
 * with the next operator's.  If the stacked one wins it returns up.
 * Otherwise it chains into the next operator. */
static void pull_or_defer(struct ros_cpu *s, uint32_t r7, uint32_t r13)
{
    ros_subs(s, r7, s->r[10] >> 28);        /* CMP R7,R10,LSR #28 */
    if (s->c) {                             /* LDRCS PC,[SP],#4 */
        s->r[7] = r7;
        s->r[13] = r13;
        s->r[15] = ros_ld32(r13);
        s->r[13] = r13 + 4;
        return;
    }
    s->r[7] = r7;
    s->r[13] = r13;
    exprcall(s);
}

/* FPLUSS (Expr.s:946) and the FPSCR check that follows it. */
static void fadd_and_check(struct ros_cpu *s, uint32_t r13)
{
    double a = s->fp->vfp.d[0], b = s->fp->vfp.d[1];
    s->fp->fpscr |= ros_vfp_ex2(a + b, a, b);   /* FADDD FACC,FACC,D1 */
    s->fp->vfp.d[0] = a + b;
    s->r[9] = T_FLOAT;                      /* MOV TYPE,#TFP */
    if (s->fp->fpscr & 7u) {                /* FPSCRCheck */
        s->r[14] = s->fp->fpscr;
        s->r[13] = r13;
        basicvfp_VFPException(s);
    }
    /* EXPRPULLNEXT */
    {
        uint32_t r7 = ros_ld32(r13);        /* LDR R7,[SP],#4 */
        r13 += 4;
        pull_or_defer(s, r7, r13);
    }
}

/* ---- + --------------------------------------------------------------- */

void basicvfp_hand_EXPRADD(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r6, r7, r8, r9, r13 = s->r[13];

    ros_logic(s, s->r[9], s->c);            /* TEQ TYPE,#0 */

    if (s->z) {                             /* BEQ STNCON: strings */
        r13 -= 4;                           /* STR R7,[SP,#-4]! */
        ros_st32(r13, s->r[7]);
        s->r[13] = r13;
        basicvfp_hand_SPUSH(s);             /* the first string */
        r13 = s->r[13];
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPRRECUR(s);              /* the second */
        r0 = s->r[0];
        r2 = s->r[2];
        r8 = s->r[8];
        r13 = s->r[13];
        r9 = s->r[9];
        ros_logic(s, r9, s->c);
        if (r9 != 0) {                      /* BNE ERTYPESTR */
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_ERTYPESTR(s);
        }
        /* concatenate: the pushed string, then the accumulator's */
        {
            uint32_t clen = ros_ld32(r13);  /* the pushed CLEN */
            r13 += 4;
            r3 = r8 - STRACC_OFF;
            uint32_t r7 = clen - r3;        /* SUBS R7,R7,R3 */
            s->r[13] = r13;
            if (r7 == 0) {                  /* first empty: done */
                s->r[3] = r3;
                s->r[7] = r7;
                goto pullnext_add;
            }
            r6 = r7 + r2;                   /* total length */
            r4 = r3 + 0x100u;
            ros_subs(s, r6, r4);            /* CMP R6,R4 */
            if (r6 >= r4) {                 /* BCS ERLONG */
                s->r[3] = r3;
                s->r[4] = r4;
                s->r[6] = r6;
                s->r[7] = r7;
                s->r[15] = s->r[14];
                basicvfp_ERLONG(s);
            }
            r4 = r6;
            while (r2 != r3) {              /* second string to the end */
                r2 -= 1;
                r0 = ros_ld8(r2);
                r4 -= 1;
                ros_st8(r4, r0);
            }
            for (;;) {                      /* the pushed words down */
                uint32_t before = r7;
                r7 -= 4;
                if (before < 4) {           /* the fractional tail */
                    r2 = r6;
                    /* CMN R7,#4 / #3 / #2: -4, -3, -2 give the counts */
                    if (r7 == 0xFFFFFFFCu) {
                        s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
                        s->r[6] = r6; s->r[7] = r7;
                        goto pullnext_add;
                    }
                    r0 = ros_ld32(r13);     /* the last word of all */
                    r13 += 4;
                    ros_st8(r3, r0);
                    r3 += 1;
                    if (r7 == 0xFFFFFFFDu) {
                        s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
                        s->r[6] = r6; s->r[7] = r7;
                        goto pullnext_add;
                    }
                    r0 >>= 8;
                    ros_st8(r3, r0);
                    r3 += 1;
                    if (r7 == 0xFFFFFFFEu) {
                        s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
                        s->r[6] = r6; s->r[7] = r7;
                        goto pullnext_add;
                    }
                    r0 >>= 8;
                    ros_st8(r3, r0);
                    r3 += 1;
                    s->r[0] = r0;
                    s->r[2] = r2;
                    s->r[3] = r3;
                    s->r[4] = r4;
                    s->r[6] = r6;
                    s->r[7] = r7;
                    goto pullnext_add;
                }
                r0 = ros_ld32(r13);
                r13 += 4;
                ros_st32(r3, r0);
                r3 += 4;
            }
        }
    }

    if (s->n) {                             /* BMI FPLUS: left float */
        r13 -= 4;                           /* STR R7,[SP,#-4]! */
        ros_st32(r13, s->r[7]);
        r13 -= 8;                           /* FSTD FACC,[SP,#-8]! */
        ros_std(r13, s->fp->vfp.d[0]);
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPRRECUR(s);
        r0 = s->r[0];
        r13 = s->r[13];
        r9 = s->r[9];
        ros_logic(s, r9, s->c);
        if (r9 == 0) {                      /* BEQ ERTYPEINT */
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_ERTYPEINT(s);
        }
        if ((int32_t)r9 >= 0) {             /* the right was an int */
            s->fp->vfp.sw[0] = r0;          /* FMSRPL S0,IACC */
            s->fp->vfp.d[0] = (double)(int32_t)r0; /* FSITODPL */
        }
        s->fp->vfp.d[1] = ros_ldd(r13);     /* FLDD D1,[SP],#8 */
        r13 += 8;
        s->r[13] = r13;
        fadd_and_check(s, r13);
        return;
    }

    /* left integer */
    r13 -= 8;                               /* STMFD SP!,{IACC,R7} */
    ros_st32(r13, s->r[0]);
    ros_st32(r13 + 4, s->r[7]);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r9 = s->r[9];
    ros_logic(s, r9, s->c);
    if (r9 == 0) {                          /* BEQ ERTYPEINT */
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }
    if ((int32_t)r9 < 0) {                  /* BMI FPLUST */
        s->fp->vfp.sw[2] = ros_ld32(r13);   /* FLDS S2,[SP],#4 */
        r13 += 4;
        s->fp->vfp.d[1] = (double)(int32_t)s->fp->vfp.sw[2];
        s->r[13] = r13;
        fadd_and_check(s, r13);
        return;
    }
    r1 = ros_ld32(r13);                     /* LDMFD SP!,{R1,R7} */
    r7 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 += r1;                               /* ADD IACC,IACC,R1 */
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[13] = r13;
    pull_or_defer(s, r7, r13);
    return;

pullnext_add:                               /* EXPRPULLNEXT for strings */
    {
        uint32_t r7p = ros_ld32(r13);       /* the pushed R7 */
        r13 += 4;
        pull_or_defer(s, r7p, r13);
    }
}

/* ---- - --------------------------------------------------------------- */

void basicvfp_hand_EXPRSUB(struct ros_cpu *s)
{
    uint32_t r0, r1, r7, r9, r13 = s->r[13];

    ros_logic(s, s->r[9], s->c);            /* TEQ TYPE,#0 */
    if (s->z) {                             /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }

    if (s->n) {                             /* BMI FMINUS: left float */
        r13 -= 4;                           /* STR R7,[SP,#-4]! */
        ros_st32(r13, s->r[7]);
        r13 -= 8;                           /* FSTD FACC,[SP,#-8]! */
        ros_std(r13, s->fp->vfp.d[0]);
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPRRECUR(s);
        r0 = s->r[0];
        r13 = s->r[13];
        r9 = s->r[9];
        ros_logic(s, r9, s->c);
        if (r9 == 0) {                      /* BEQ ERTYPEINT */
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_ERTYPEINT(s);
        }
        if ((int32_t)r9 >= 0) {             /* the right was an int */
            s->fp->vfp.sw[0] = r0;
            s->fp->vfp.d[0] = (double)(int32_t)r0;
        }
        s->fp->vfp.d[1] = ros_ldd(r13);     /* FLDD D1,[SP],#8 */
        r13 += 8;
        s->r[13] = r13;
        goto fsub_and_check;
    }

    /* left integer */
    r13 -= 8;                               /* STMFD SP!,{IACC,R7} */
    ros_st32(r13, s->r[0]);
    ros_st32(r13 + 4, s->r[7]);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r9 = s->r[9];
    ros_logic(s, r9, s->c);
    if (r9 == 0) {                          /* BEQ ERTYPEINT */
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }
    if ((int32_t)r9 < 0) {                  /* BMI FMINUT */
        s->fp->vfp.sw[2] = ros_ld32(r13);   /* FLDS S2,[SP],#4 */
        r13 += 4;
        s->fp->vfp.d[1] = (double)(int32_t)s->fp->vfp.sw[2];
        s->r[13] = r13;
        goto fsub_and_check;
    }
    r1 = ros_ld32(r13);                     /* LDMFD SP!,{R1,R7} */
    r7 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 = r1 - r0;                           /* SUB IACC,R1,IACC */
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[13] = r13;
    pull_or_defer(s, r7, r13);
    return;

fsub_and_check:                             /* FSUBD FACC,D1,FACC */
    {
        double a = s->fp->vfp.d[1], b = s->fp->vfp.d[0];
        s->fp->fpscr |= ros_vfp_ex2(a - b, a, b);
        s->fp->vfp.d[0] = a - b;
        s->r[9] = T_FLOAT;                  /* MOV TYPE,#TFP */
        if (s->fp->fpscr & 7u) {
            s->r[14] = s->fp->fpscr;
            basicvfp_VFPException(s);
        }
        {                                   /* EXPRPULLNEXT */
            uint32_t r7 = ros_ld32(s->r[13]);
            uint32_t r13b = s->r[13] + 4;
            s->r[13] = r13b;
            pull_or_defer(s, r7, r13b);
        }
    }
}
