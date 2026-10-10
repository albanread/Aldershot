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

/* expr_divop.c: BASIC's integer division and remainder, translated by
 * hand from DIVOP in Expr.s (RISC OS 5.31's BASIC, the VFP build).
 *
 * DIV is / between integers, and MOD gives its remainder.  This one
 * routine lies behind both.  EXPRDIV and EXPRMOD in Expr.s each leave
 * the other's result in the register they ignore.  Both operands are
 * forced to integers (INTEGY).  The left is pushed while the right is
 * evaluated.  Then a shift-and-subtract long division runs:
 *   - R6 is the divisor's current bit.  It is doubled while it fits.
 *     This is the normalisation loop, whose conditional MOVS/CMPLS
 *     read and leave the flags (LS both times).
 *   - It is then subtracted down, and the quotient is built one bit
 *     at a time by ADC.
 * The signs are noted beforehand (R3 for the quotient, R7 for the
 * remainder) and applied afterwards.
 *
 * The quirks kept, with their lines:
 *   - A zero divisor gives "Division by zero" (ZDIVOR, error 18).  It
 *     is raised after the operands' magnitudes have been taken,
 *     exactly where the original's BEQ leaves it.
 *   - Negating INT_MIN has no effect (RSBMI of &80000000).  So the
 *     magnitudes of extreme operands stay extreme, as in the original.
 *   - The loop's compares are unsigned (LS/CS).  The shift count and
 *     the exit come from carries and not from signed order.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "expr_divop.h"

void basicvfp_EXPRRECUR(struct ros_cpu *s);
void basicvfp_ZDIVOR(struct ros_cpu *s);
void basicvfp_hand_INTEGY(struct ros_cpu *s);

void basicvfp_hand_DIVOP(struct ros_cpu *s)
{
    uint32_t r0, r2, r3, r4, r6, r7, r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                               /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);                /* the left as an integer */
    r0 = s->r[0];
    r13 = s->r[13];
    r13 -= 4;                               /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);                  /* the right */
    r0 = s->r[0];
    r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r4 = ros_ld32(r13);                     /* LDMFD SP!,{R4,R14} */
    r14 = ros_ld32(r13 + 4);
    r13 += 8;

    r3 = r0 ^ r4;                           /* the quotient's sign */
    r7 = r4;                                /* the remainder's sign */
    if ((int32_t)r4 < 0)
        r4 = -r4;
    r2 = ros_logic(s, r0, s->c);            /* MOVS R2,IACC */
    if (r0 == 0) {                          /* BEQ ZDIVOR */
        s->r[2] = r2;
        s->r[3] = r3;
        s->r[4] = r4;
        s->r[7] = r7;
        s->r[13] = r13;
        s->r[14] = r14;
        s->r[15] = r14;
        basicvfp_ZDIVOR(s);
        return;
    }
    if ((int32_t)r0 < 0)
        r2 = -r0;
    r6 = r2;
    r0 = 0;                                 /* the quotient so far */

    /* normalise: double the bit while it still fits */
    ros_subs(s, r2, r4 >> 1);               /* CMP R6,R4,LSR #1 */
    for (;;) {
        int ls = ros_cond(s, ROS_LS);       /* MOVLS R6,R6,LSL #1 */
        uint32_t half = r4 >> 1;
        if (ls) {
            r6 <<= 1;
            ros_subs(s, r6, half);          /* CMPLS R6,R4,LSR #1 */
        }
        if (!(ls && r6 <= half))            /* BLS DIVJUS */
            break;
    }

    /* divide: subtract the bit down, collecting carries */
    do {
        uint32_t before = r4;
        if (r4 >= r6)                       /* CMP R4,R6 ; SUBCS */
            r4 -= r6;
        r0 += r0 + (before >= r6);          /* ADC IACC,IACC,IACC */
        r6 >>= 1;
    } while (r6 >= r2);                     /* CMP R6,R2 ; BCS DIVER */

    if ((int32_t)r3 < 0)
        r0 = -r0;
    if ((int32_t)r7 < 0)
        r4 = -r4;
    s->r[0] = r0;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = r14;
}
