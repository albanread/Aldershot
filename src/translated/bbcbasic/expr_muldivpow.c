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

/* expr_muldivpow.c: BASIC's *, / and ^, translated by hand from
 * Expr.s:1025-1130 and 1215-1290 (RISC OS 5.31's BASIC, the VFP
 * build).  They use the same precedence-climbing protocol as + and -
 * (units/expr_addsub.c).  The operator's continuation runs with its
 * priority in R7 and the next operator's word in R10.  It finishes by
 * returning up or by chaining through the jump table.
 *
 * * keeps small integer products as integers.  If both sides lie
 * within +-&B500 (a little less than the square root of &7FFFFFFF),
 * one MUL multiplies them and cannot overflow.  Anything wider goes to
 * the float path, with each side converted where it stands
 * (Expr.s:1025-1050).  The exact exception record of FMULD carries the
 * overflow.
 *
 * / is always done in floating point.  Both sides go through FLOATQ.
 * The divisor is compared with zero (FCMPZD) before the divide, to
 * give "Division by zero".  The FPSCR check after the divide catches
 * the exceptions of FDIVD (another division by zero, or overflow).
 *
 * ^ takes its right operand with FACTOR and not EXPRRECUR.  It takes a
 * factor only, so a^b*c is (a^b)*c.  FACTOR leaves the lookahead
 * unread, so POWEREND skips the spaces itself.  It then hands on to
 * EXPRNEXT for the table lookup and the compare (Expr.s:1284).
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "expr_muldivpow.h"

void basicvfp_EXPRRECUR(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_VFPException(struct ros_cpu *s);
void basicvfp_ZDIVOR(struct ros_cpu *s);
void basicvfp_EXPRNEXT(struct ros_cpu *s);
void basicvfp_FACTOR(struct ros_cpu *s);
void basicvfp_FC102830(struct ros_cpu *s);
void basicvfp_hand_FLOATY(struct ros_cpu *s);
void basicvfp_hand_FLOATQ(struct ros_cpu *s);
void basicvfp_hand_FPUSH(struct ros_cpu *s);

#define T_FLOAT 0x80000000u
#define FPOW 40u                    /* the elementary table's pow entry */

/* The protocol, as units/expr_addsub.c spells it. */
static void mdp_exprcall(struct ros_cpu *s, uint32_t r7, uint32_t r13)
{
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[4] = 0xFC1026A0u;          /* AJ7 */
    s->r[14] = s->r[10] & 0xFFFFFFu;
    basicvfp_FC102830(s);
}

static void mdp_pull_or_defer(struct ros_cpu *s, uint32_t r7, uint32_t r13)
{
    ros_subs(s, r7, s->r[10] >> 28);        /* CMP R7,R10,LSR #28 */
    if (s->c) {                             /* LDRCS PC,[SP],#4 */
        s->r[7] = r7;
        s->r[15] = ros_ld32(r13);
        s->r[13] = r13 + 4;
        return;
    }
    mdp_exprcall(s, r7, r13);
}

static void mdp_check_fpscr(struct ros_cpu *s)
{
    if (s->fp->fpscr & 7u) {
        s->r[14] = s->fp->fpscr;
        basicvfp_VFPException(s);
    }
}

/* ---- * --------------------------------------------------------------- */

void basicvfp_hand_EXPRMUL(struct ros_cpu *s)
{
    uint32_t r0, r4, r7, r9, r13 = s->r[13];
    int left_int, right_int;

    ros_logic(s, s->r[9], s->c);            /* TEQ TYPE,#0 */
    if (s->z) {
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }
    left_int = !s->n;
    if (left_int && !((int32_t)s->r[0] <= 0xB500
                      && (int32_t)s->r[0] >= -(int32_t)0xB500)) {
        /* FTIMF: too wide to multiply in 32 bits, so float it */
        s->fp->vfp.sw[0] = s->r[0];
        s->fp->vfp.d[0] = (double)(int32_t)s->r[0];
        left_int = 0;
    }

    if (left_int) {
        r13 -= 8;                           /* STMFD SP!,{IACC,R7} */
        ros_st32(r13, s->r[0]);
        ros_st32(r13 + 4, s->r[7]);
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_EXPRRECUR(s);
        r0 = s->r[0];
        r13 = s->r[13];
        r9 = s->r[9];
        ros_logic(s, r9, s->c);
        if (r9 == 0) {
            s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_ERTYPEINT(s);
        }
        right_int = (int32_t)r9 >= 0;
        if (right_int
            && !((int32_t)r0 <= 0xB500 && (int32_t)r0 >= -(int32_t)0xB500)) {
            /* FTIMEL: the right goes to the float side as well */
            s->fp->vfp.sw[0] = r0;
            s->fp->vfp.d[0] = (double)(int32_t)r0;
            right_int = 0;
        }
        if (!right_int) {
            /* FTIMET: the left, popped through S2, into D1 */
            s->fp->vfp.sw[2] = ros_ld32(r13);
            r13 += 4;
            s->fp->vfp.d[1] = (double)(int32_t)s->fp->vfp.sw[2];
            goto ftimes;
        }
        r4 = ros_ld32(r13);                 /* LDMFD SP!,{R4,R7} */
        r7 = ros_ld32(r13 + 4);
        r13 += 8;
        r0 = r4 * r0;                       /* MUL IACC,R4,IACC */
        s->r[0] = r0;
        s->r[4] = r4;
        s->r[13] = r13;
        mdp_pull_or_defer(s, r7, r13);
        return;
    }

    /* FTIME: the left is (or became) a float */
    r13 -= 4;                               /* STR R7,[SP,#-4]! */
    ros_st32(r13, s->r[7]);
    r13 -= 8;                               /* FPUSH */
    ros_std(r13, s->fp->vfp.d[0]);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r9 = s->r[9];
    ros_logic(s, r9, s->c);
    if (r9 == 0) {
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }
    if ((int32_t)r9 >= 0) {                 /* the right was an int */
        s->fp->vfp.sw[0] = r0;
        s->fp->vfp.d[0] = (double)(int32_t)r0;
    }
    s->fp->vfp.d[1] = ros_ldd(r13);         /* the left */
    r13 += 8;

ftimes: /* FTIMES: FMULD FACC,D1,FACC */
    {
        double a = s->fp->vfp.d[1], b = s->fp->vfp.d[0];
        s->fp->fpscr |= ros_vfp_ex2(a * b, a, b);
        s->fp->vfp.d[0] = a * b;
        s->r[9] = T_FLOAT;
        mdp_check_fpscr(s);
    }
    r7 = ros_ld32(r13);                     /* LDR R7,[SP],#4 */
    r13 += 4;
    mdp_pull_or_defer(s, r7, r13);
}

/* ---- / --------------------------------------------------------------- */

void basicvfp_hand_EXPRDIV(struct ros_cpu *s)
{
    uint32_t r0, r7, r9, r13 = s->r[13];

    ros_logic(s, s->r[9], s->c);            /* TEQ TYPE,#0; BLPL FLOATQ */
    if (!s->n)
        basicvfp_hand_FLOATQ(s);
    r13 -= 4;                               /* STR R7,[SP,#-4]! */
    ros_st32(r13, s->r[7]);
    r13 -= 8;                               /* FPUSH */
    ros_std(r13, s->fp->vfp.d[0]);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);
    r13 = s->r[13];
    r9 = s->r[9];
    ros_logic(s, r9, s->c);                 /* TEQ TYPE,#0; BLPL FLOATQ */
    if (!s->n)
        basicvfp_hand_FLOATQ(s);
    r13 = s->r[13];

    /* FCMPZD FACC; FMRX PC,FPSCR; BEQ ZDIVOR */
    s->fp->fpscr |= ros_vfp_cmp_ex(s->fp->vfp.d[0], 0.0, 0);
    ros_vfp_cmp(s, s->fp->vfp.d[0], 0.0);
    ros_vmrs_flags(s);
    if (s->z) {
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ZDIVOR(s);
    }

    s->fp->vfp.d[1] = ros_ldd(r13);         /* FLDD D1,[SP],#8: the left */
    r13 += 8;
    {   /* FDIVD FACC,D1,FACC */
        double a = s->fp->vfp.d[1], b = s->fp->vfp.d[0];
        s->fp->fpscr |= ros_vfp_ex2(a / b, a, b);
        s->fp->vfp.d[0] = a / b;
        s->r[9] = T_FLOAT;
        mdp_check_fpscr(s);
    }
    r7 = ros_ld32(r13);                     /* LDR R7,[SP],#4 */
    r13 += 4;
    mdp_pull_or_defer(s, r7, r13);
}

/* ---- ^ --------------------------------------------------------------- */

void basicvfp_hand_EXPRPOW(struct ros_cpu *s)
{
    uint32_t r0, r7, r10, r11, r13 = s->r[13];

    r13 -= 4;                               /* STR R7,[SP,#-4]! */
    ros_st32(r13, s->r[7]);
    s->r[13] = r13;                         /* the push is real stack */
    basicvfp_hand_FLOATY(s);                /* the left, floated */

    r13 = s->r[13];                         /* FPUSH */
    r13 -= 8;
    ros_std(r13, s->fp->vfp.d[0]);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_FACTOR(s);                     /* the right: a FACTOR only */
    r0 = s->r[0];
    r11 = s->r[11];
    r13 = s->r[13];
    if (s->z) {                             /* BEQ ERTYPEINT */
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
    }
    if (!s->n) {                            /* an integer exponent */
        s->fp->vfp.sw[2] = r0;              /* FMSRPL S2,IACC */
        s->fp->vfp.d[1] = (double)(int32_t)r0; /* FSITODPL D1,S2 */
    } else {
        s->fp->vfp.d[1] = s->fp->vfp.d[0];  /* FCPYDMI D1,D0 */
    }
    s->fp->vfp.d[0] = ros_ldd(r13);         /* the left back */
    r13 += 8;

    {   /* VFPElementary pow: D0 = pow(D0, D1) through the table */
        uint32_t t = ros_ld32(s->r[8] - 8u) + FPOW;
        s->r[14] = 0xFFFFFFF0u;
        ros_call(s, t);
    }
    mdp_check_fpscr(s);

    /* POWEREND: FACTOR left the lookahead unread, so skip the spaces */
    do {
        r10 = ros_ld8(r11);
        r11 += 1;
    } while (r10 == 32);
    s->r[9] = T_FLOAT;
    r7 = ros_ld32(r13);                     /* LDR R7,[SP],#4 */
    r13 += 4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[7] = r7;
    s->r[15] = s->r[14];
    basicvfp_EXPRNEXT(s);                   /* the lookup, compare, chain */
}
