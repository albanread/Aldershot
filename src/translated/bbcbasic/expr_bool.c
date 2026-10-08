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

/* expr_bool.c: BASIC's boolean, comparison and shift operators,
 * translated by hand from Expr.s:816-905 and 1147-1215 (RISC OS
 * 5.31's BASIC, the VFP build).  These are the last operators to be
 * translated.  With them, every operator in the language is tier 3.
 *
 * The operators take three forms, as in the source:
 *   - OR, EOR, AND.  Both sides are made integers.  The bitwise
 *     operation is done, then the climber's compare and the chain
 *     (Expr.s:820-850).
 *   - The comparisons.  They peek at AELINE for the two-character
 *     forms (<=, <>, >=, <<, >>) and use COMPR for the rest.  The
 *     answer, -1 or 0, comes from the condition codes COMPR left
 *     (MVNcc).  They end in EXPRNONRIGHT, the tail that stops chained
 *     relations evaluating left to right.  If the next operator word
 *     is itself a relation (bit 28, the special bit in PRIORTABLE), it
 *     is shifted down (R10,LSR #25).  So the pending relation defers,
 *     and the chain evaluates the right first (:856-865).
 *   - The shifts.  Both sides are made integers, then ASR, LSR or LSL
 *     is done.  They also end in EXPRNONRIGHT, because << and >> share
 *     the priority band of the relations.
 *
 * DIV and MOD (Expr.s:1147, 1185) are DIVOP called as operators.  DIV
 * keeps the quotient in IACC.  MOD moves the remainder out of R4.
 * MOD's chain goes straight to EXPRHARD, which is the jump table
 * without the R4 := AJ7 of EXPRCALL.  This is exactly how the original
 * branches.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "expr_bool.h"

void basicvfp_EXPRRECUR(struct ros_cpu *s);
void basicvfp_FC102830(struct ros_cpu *s);
void basicvfp_EXPRHARD(struct ros_cpu *s);
void basicvfp_hand_COMPR(struct ros_cpu *s);
void basicvfp_hand_INTEGY(struct ros_cpu *s);
void basicvfp_hand_DIVOP(struct ros_cpu *s);

#define T_INTEGER 0x40000000u

/* The climber's chain, and the relations' variant of it. */
static void ebp_chain(struct ros_cpu *s, uint32_t r7, uint32_t r13)
{
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[4] = 0xFC1026A0u;                  /* AJ7 */
    s->r[14] = s->r[10] & 0xFFFFFFu;
    basicvfp_FC102830(s);
}

static void ebp_defer(struct ros_cpu *s, uint32_t r7, uint32_t r13)
{
    ros_subs(s, r7, s->r[10] >> 28);        /* CMP R7,R10,LSR #28 */
    if (s->c) {
        s->r[7] = r7;
        s->r[15] = ros_ld32(r13);
        s->r[13] = r13 + 4;
        return;
    }
    ebp_chain(s, r7, r13);
}

/* EXPRNONRIGHT: the tail of the relations.  A pending relation word
 * is rebuilt (R10,LSR #25) so that this relation defers to it. */
static void ebp_nonright(struct ros_cpu *s, uint32_t r7, uint32_t r13)
{
    uint32_t r10 = s->r[10];
    if (r10 & 0x10000000u)                  /* TST R10,#&10000000 */
        r10 >>= 25;                         /* MOVNE R10,R10,LSR #25 */
    s->r[10] = r10;
    ebp_defer(s, r7, r13);
}

/* ---- OR, EOR, AND ----------------------------------------------------- */

#define BITOP(name, expr)                                                \
    void basicvfp_hand_##name(struct ros_cpu *s)                         \
    {                                                                    \
        uint32_t r0, r1, r7, r13 = s->r[13];                             \
        s->r[14] = 0xFFFFFFF0u;                                          \
        basicvfp_hand_INTEGY(s);                                         \
        r13 = s->r[13];                                                  \
        r13 -= 8;                       /* STMFD SP!,{IACC,R7} */        \
        ros_st32(r13, s->r[0]);                                          \
        ros_st32(r13 + 4, s->r[7]);                                      \
        s->r[13] = r13;                                                  \
        s->r[14] = 0xFFFFFFF0u;                                          \
        basicvfp_EXPRRECUR(s);                                           \
        s->r[14] = 0xFFFFFFF0u;                                          \
        basicvfp_hand_INTEGY(s);                                         \
        r0 = s->r[0];                                                    \
        r13 = s->r[13];                                                  \
        r1 = ros_ld32(r13);                       /* LDMFD {R1,R7} */    \
        r7 = ros_ld32(r13 + 4);                                          \
        r13 += 8;                                                        \
        r0 = expr;                                /* the operation */   \
        s->r[0] = r0;                                                    \
        s->r[1] = r1;                                                    \
        ebp_defer(s, r7, r13);                                           \
    }

BITOP(EXPROR, r0 | r1)     /* Expr.s:820 */
BITOP(EXPREOR, r0 ^ r1)    /* :829 */
BITOP(EXPRAND, r0 & r1)    /* :838 */

/* ---- the comparisons -------------------------------------------------- */

/* COMPR, then -1 or 0 from its flags, then the relations' tail. */
#define RELOP(name, cond)                                                \
    void basicvfp_hand_##name(struct ros_cpu *s)                         \
    {                                                                    \
        uint32_t r7 = s->r[7], r13 = s->r[13];                           \
        s->r[13] = r13;                                                  \
        s->r[15] = s->r[14];                                             \
        basicvfp_hand_COMPR(s);                                          \
        if (cond)                                                        \
            s->r[0] = 0xFFFFFFFFu;      /* MVNcc IACC,#0 */              \
        ebp_nonright(s, r7, r13);                                        \
    }

RELOP(EXPRNEQUAL, !s->z)                       /* <> : Expr.s:816 */
RELOP(EXPRLTOREQ, s->z || (s->n != s->v))      /* <= : :864 */
RELOP(EXPREQ, s->z)                            /* =  : :868 */
RELOP(EXPRGTOREQ, s->n == s->v)                /* >= : :880 */

/* < and > first peek at AELINE for the two-character forms. */
void basicvfp_hand_EXPRLT(struct ros_cpu *s)
{
    uint32_t r4 = ros_ld8(s->r[11]);        /* LDRB R4,[AELINE],#1 */
    s->r[11] += 1;
    s->r[4] = r4;
    s->r[13] = s->r[13];
    if (r4 == 61)                           /* "=" */
        return basicvfp_hand_EXPRLTOREQ(s);
    if (r4 == 62)                           /* ">" */
        return basicvfp_hand_EXPRNEQUAL(s);
    if (r4 == 60)                           /* "<" */
        return basicvfp_hand_EXPRLSHIFT(s);
    s->r[11] -= 1;                          /* SUB AELINE,AELINE,#1 */
    {
        uint32_t r7 = s->r[7], r13 = s->r[13];
        s->r[15] = s->r[14];
        basicvfp_hand_COMPR(s);
        if (s->n != s->v)                   /* MVNLT */
            s->r[0] = 0xFFFFFFFFu;
        ebp_nonright(s, r7, r13);
    }
}

void basicvfp_hand_EXPRGT(struct ros_cpu *s)
{
    uint32_t r4 = ros_ld8(s->r[11]);
    s->r[11] += 1;
    s->r[4] = r4;
    if (r4 == 61)                           /* "=" */
        return basicvfp_hand_EXPRGTOREQ(s);
    if (r4 == 62)                           /* ">" */
        return basicvfp_hand_EXPRRSHIFT(s);
    s->r[11] -= 1;
    {
        uint32_t r7 = s->r[7], r13 = s->r[13];
        s->r[15] = s->r[14];
        basicvfp_hand_COMPR(s);
        if (!s->z && s->n == s->v)          /* MVNGT */
            s->r[0] = 0xFFFFFFFFu;
        ebp_nonright(s, r7, r13);
    }
}

/* ---- the shifts -------------------------------------------------------- */

void basicvfp_hand_EXPRRSHIFT(struct ros_cpu *s)
{
    uint32_t r0, r1, r7, r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r13 = s->r[13];
    r13 -= 8;                               /* STMFD SP!,{IACC,R7} */
    ros_st32(r13, s->r[0]);
    ros_st32(r13 + 4, s->r[7]);
    s->r[13] = r13;
    {
        uint32_t r4 = ros_ld8(s->r[11]);    /* LDRB R4,[AELINE],#1 */
        s->r[11] += 1;
        s->r[4] = r4;
        if (r4 == 62)                       /* ">>": logical */
            return basicvfp_hand_EXPRRSHIFTLOGICAL(s);
        s->r[11] -= 1;
    }
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r1 = ros_ld32(r13);                     /* LDMFD SP!,{R1,R7} */
    r7 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 = ros_asr(r1, r0);                   /* ASR, ARM's exact rules */
    s->r[0] = r0;
    s->r[1] = r1;
    ebp_nonright(s, r7, r13);
}

void basicvfp_hand_EXPRRSHIFTLOGICAL(struct ros_cpu *s)
{
    uint32_t r0, r1, r7, r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r1 = ros_ld32(r13);
    r7 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 = ros_lsr(r1, r0);                   /* LSR, ARM's exact rules */
    s->r[0] = r0;
    s->r[1] = r1;
    ebp_nonright(s, r7, r13);
}

void basicvfp_hand_EXPRLSHIFT(struct ros_cpu *s)
{
    uint32_t r0, r1, r7, r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r13 = s->r[13];
    r13 -= 8;
    ros_st32(r13, s->r[0]);
    ros_st32(r13 + 4, s->r[7]);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRRECUR(s);
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0];
    r13 = s->r[13];
    r1 = ros_ld32(r13);
    r7 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 = ros_lsl(r1, r0);                   /* LSL, ARM's exact rules */
    s->r[0] = r0;
    s->r[1] = r1;
    ebp_nonright(s, r7, r13);
}

/* ---- DIV and MOD ------------------------------------------------------- */

void basicvfp_hand_EXPRINTDIV(struct ros_cpu *s)
{
    uint32_t r7, r13 = s->r[13];
    r13 -= 4;                               /* STR R7,[SP,#-4]! */
    ros_st32(r13, s->r[7]);
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_DIVOP(s);                 /* the quotient stays in IACC */
    r7 = ros_ld32(r13);                     /* LDR R7,[SP],#4 */
    r13 += 4;
    s->r[13] = r13;
    ebp_defer(s, r7, r13);
}

void basicvfp_hand_EXPRMOD(struct ros_cpu *s)
{
    uint32_t r7, r13 = s->r[13];
    r13 -= 4;
    ros_st32(r13, s->r[7]);
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_DIVOP(s);
    s->r[0] = s->r[4];                      /* MOV IACC,R4: the remainder */
    r7 = ros_ld32(r13);
    r13 += 4;
    s->r[13] = r13;
    /* B EXPRHARD: the chain without EXPRCALL's R4 := AJ7 */
    ros_subs(s, r7, s->r[10] >> 28);
    if (s->c) {
        s->r[7] = r7;
        s->r[15] = ros_ld32(r13);
        s->r[13] = r13 + 4;
        return;
    }
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[14] = s->r[10] & 0xFFFFFFu;
    basicvfp_EXPRHARD(s);
}
