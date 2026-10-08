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
 * (Sources/Programmer/BASIC: s.Basic).
 */

/* basic_stack.c: BASIC's value stack, translated by hand from Basic.s
 * (RISC OS 5.31's BASIC, the VFP build).  It holds PUSHTYPE/PULLTYPE
 * and the four save and restore routines they dispatch to.
 *
 * The original keeps intermediate values, with their types, on its
 * own stack in the arena (R13):
 *   - an integer is one word
 *   - a float is the eight bytes of FACC
 *   - a string is its words, rounded up, with CLEN above them.
 * PUSHTYPE tests TYPE first, with TEQ.  The flags survive into the
 * dispatch: N is the test for a float and Z the test for a string.
 * The pulls run the same test in reverse, so each half mirrors the
 * other.
 *
 * The quirks kept, with their lines:
 *   - A string of zero length pushes (and pulls) nothing but CLEN
 *     itself (SPUSHX, the BEQ out of SPUSH).
 *   - The length is CLEN-STRACC, rounded UP to words ((n+3)&~3).  So
 *     the stack always moves a whole number of words (SPUSHLARGE,
 *     SPULL).
 *   - The copies are word moves.  The last word of a string carries
 *     up to three bytes of whatever followed it in the accumulator,
 *     and pulls restore them.  This can be seen only through the
 *     arena, which is exactly where it can be seen in the original
 *     too.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "basic_stack.h"

#define STRACC_OFF 1536u

/* ---- pushing --------------------------------------------------------- */

/* PUSHTYPE (Basic.s): push the accumulator, by type, onto the
 * stack. */
void basicvfp_hand_PUSHTYPE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r9 = s->r[9], r13 = s->r[13];
    ros_logic(s, r9, s->c);                 /* TEQ TYPE,#0 */
    if ((int32_t)r9 < 0) {                  /* BMI FPUSH */
        basicvfp_hand_FPUSH(s);
        return;
    }
    if (r9 != 0) {                          /* integer: one word */
        s->r[13] = r13 - 4;
        ros_st32(r13 - 4, r0);
        s->r[15] = s->r[14];
        return;
    }
    basicvfp_hand_SPUSH(s);                 /* string */
}

/* SPUSH: push the accumulator's string, by its own length. */
void basicvfp_hand_SPUSH(struct ros_cpu *s)
{
    uint32_t stracc = s->r[8] - STRACC_OFF;
    uint32_t len = ros_subs(s, s->r[2], stracc); /* SUBS R1,CLEN,R0 */
    if (len == 0) {
        s->r[0] = stracc;
        s->r[1] = 0;
        basicvfp_hand_SPUSHX(s);
        return;
    }
    s->r[0] = stracc;
    s->r[1] = len;
    basicvfp_hand_SPUSHLARGE(s);
}

/* SPUSHX: nothing to copy, so push just CLEN. */
void basicvfp_hand_SPUSHX(struct ros_cpu *s)
{
    s->r[13] -= 4;
    ros_st32(s->r[13], s->r[2]);
    s->r[15] = s->r[14];
}

/* SPUSHLARGE: push the words of the string, then CLEN. */
void basicvfp_hand_SPUSHLARGE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0];
    uint32_t n = (s->r[1] + 3) & ~3u;       /* rounded up to words */
    uint32_t r13 = s->r[13] - n;
    uint32_t to = r13;
    s->r[13] = r13;
    do {
        uint32_t w = ros_ld32(r0);          /* LDR R4,[R0],#4 */
        r0 += 4;
        ros_st32(to, w);
        to += 4;
        n = ros_subs(s, n, 4);              /* SUBS R1,R1,#4 */
    } while (n != 0);
    s->r[0] = r0;
    s->r[1] = n;
    s->r[3] = to;
    s->r[4] = ros_ld32(r0 - 4);
    s->r[15] = s->r[14];
    basicvfp_hand_SPUSHX(s);                /* STR CLEN,[SP,#-4]! */
}

/* FPUSH: push FACC, eight bytes. */
void basicvfp_hand_FPUSH(struct ros_cpu *s)
{
    s->r[13] -= 8;
    ros_std(s->r[13], s->fp->vfp.d[0]);
    s->r[15] = s->r[14];
}

/* ---- pulling --------------------------------------------------------- */

/* PULLTYPE: pull the typed value back off the stack. */
void basicvfp_hand_PULLTYPE(struct ros_cpu *s)
{
    uint32_t r9 = ros_ld32(s->r[13]);       /* LDR TYPE,[SP],#4 */
    s->r[13] += 4;
    ros_logic(s, r9, s->c);                 /* TEQ TYPE,#0 */
    if ((int32_t)r9 < 0) {
        s->r[9] = r9;
        basicvfp_hand_FPULL(s);
        return;
    }
    if (r9 != 0) {                          /* integer */
        s->r[0] = ros_ld32(s->r[13]);
        s->r[13] += 4;
        s->r[9] = r9;
        s->r[15] = s->r[14];
        return;
    }
    s->r[9] = r9;
    basicvfp_hand_SPULL(s);                 /* string */
}

/* SPULL: pull CLEN, then the string's words back into the
 * accumulator. */
void basicvfp_hand_SPULL(struct ros_cpu *s)
{
    uint32_t r13 = s->r[13];
    uint32_t clen = ros_ld32(r13);          /* LDR CLEN,[SP],#4 */
    r13 += 4;
    uint32_t to = s->r[8] - STRACC_OFF;
    uint32_t n = ros_subs(s, clen, to);     /* SUBS R1,CLEN,STRACC */
    s->r[2] = clen;
    if (n == 0) {                           /* empty: nothing copied */
        s->r[0] = to;
        s->r[1] = 0;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        return;
    }
    n = (n + 3) & ~3u;
    do {
        uint32_t w = ros_ld32(r13);         /* LDR R3,[SP],#4 */
        r13 += 4;
        ros_st32(to, w);
        to += 4;
        n = ros_subs(s, n, 4);
    } while (n != 0);
    s->r[0] = to;
    s->r[1] = n;
    s->r[3] = ros_ld32(to - 4);
    s->r[13] = r13;
    s->r[15] = s->r[14];
}

/* FPULL: pull FACC back. */
void basicvfp_hand_FPULL(struct ros_cpu *s)
{
    s->fp->vfp.d[0] = ros_ldd(s->r[13]);    /* FLDD FACC,[SP],#8 */
    s->r[13] += 8;
    s->r[15] = s->r[14];
}
