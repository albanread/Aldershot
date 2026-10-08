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
 * (Sources/Programmer/BASIC: s.Lexical).
 */

/* lexical_list.c: BASIC's token output for LIST, translated by hand
 * from Lexical.s:379-412 (RISC OS 5.31's BASIC, the VFP build).  This
 * is unit 2c.  With it, all of the executable code in Lexical.s is
 * translated by hand.
 *
 * TOKOUT prints one character.  For a token byte it prints the keyword
 * the token stands for.  R7 holds LISTO, whose bit 4 asks for keywords
 * in lower case.
 *
 * TOKENADDR maps a token byte to its text.  The two-byte escapes
 * (TESCFN/TESCCOM/TESCSTMT, Lexical.s:392-412) index the extra table
 * sections RTABLETF/TC/TS.  They use the byte that follows in the
 * line, less &8E.  Plain tokens subtract &7F and use RTABLE itself.
 * Each table word holds two offsets, and the token's bottom bit
 * selects one half.  The offset is from PLEXA, the keyword strings at
 * Lexical.s:608.
 *
 * The quirks kept, with their lines:
 *   - The lower-case option changes only "A"-"Z" (:385-390).
 *   - The token text ends at a byte >= &7F, which is not printed
 *     (:381-383).
 *   - TOKENADDR consumes the escape's second byte from LINE (R12)
 *     (:398-400).  It advances the line only on the escape path.  It
 *     hands the advanced LINE back through the pointer the lift
 *     passes.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "lexical_list.h"

/* The character output (CHOUT in Lexical.s, still the lift's). */
void basicvfp_CHOUT(struct ros_cpu *s);

/* Tokens and tables, as the lift defines them. */
#define TESC_FN 198u
#define TESC_COM 199u
#define TESC_STMT 200u
#define RTABLE 0xFC10E0ECu
#define RTABLE_TS 0xFC10E1F0u
#define RTABLE_TC 0xFC10E21Cu
#define RTABLE_TF 0xFC10E240u
#define PLEXA 0xFC10E244u

/* TOKOUT (Lexical.s:379): print R0, or the keyword of the token in R0.
 * R7 is LISTO.  Its bit 4 puts the keyword's letters in lower case. */
void basicvfp_hand_TOKOUT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r7 = s->r[7], r13 = s->r[13],
             r14 = s->r[14];

    if (r0 < 0x7Fu) {                          /* BCC CHOUT */
        s->r[15] = s->r[14];
        basicvfp_CHOUT(s);
        return;
    }
    r13 -= 4;                                  /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_TOKENADDR(s, r0, &r1, &s->r[12], s->r[14]);
    r13 = s->r[13];
    for (;;) {
        r0 = ros_ld8(r1);                      /* LDRB R0,[R1],#1 */
        r1 += 1;
        if (r0 >= 0x7Fu) {                     /* the end mark */
            s->r[1] = r1;
            s->r[14] = r14;
            {
                uint32_t t = ros_ld32(r13);    /* LDRCS PC,[SP],#4 */
                s->r[13] = r13 + 4;
                s->r[15] = t;
            }
            return;
        }
        if ((r7 & 0x10u) != 0                  /* LISTO bit 4 */
            && r0 >= 65 && r0 <= 90)
            r0 |= 32;                          /* A-Z to a-z */
        s->r[0] = r0;
        s->r[1] = r1;
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_CHOUT(s);
        r1 = s->r[1];
        r7 = s->r[7];
        r13 = s->r[13];
        r14 = s->r[14];
    }
}

/* TOKENADDR (Lexical.s:392): return the text of the token in R0, in
 * R1.  The escapes take their second byte from [LINE] and advance
 * LINE. */
void basicvfp_hand_TOKENADDR(struct ros_cpu *s, uint32_t r0, uint32_t *p1,
                             uint32_t *p12, uint32_t r14)
{
    uint32_t r1, r12 = *p12;
    int esc = (r0 == TESC_FN || r0 == TESC_COM || r0 == TESC_STMT);

    if (r0 == TESC_FN)
        r1 = RTABLE_TF;
    else if (esc)
        r1 = r0 == TESC_COM ? RTABLE_TC : RTABLE_TS;
    else
        r1 = RTABLE;
    if (esc) {
        r0 = ros_ld8(r12);                     /* LDREQB R0,[LINE],#1 */
        r12 += 1;
        r0 -= 142u;                            /* SUBEQ R0,R0,#&8E */
    } else {
        r0 -= 0x7Fu;                           /* SUBNE R0,R0,#&7F */
    }
    r1 = ros_ld32(r1 + ((r0 & ~1u) << 1));
    r1 = (((r0 & 1) == 0 ? r1 << 16 : r1) >> 16) + PLEXA;
    *p1 = r1;
    *p12 = r12;
    s->r[15] = r14;
}

/* LEXTABADR (Lexical.s:405): the keyword table's base, for the
 * *HELP and LIST machinery that walks it. */
void basicvfp_hand_LEXTABADR(struct ros_cpu *s, uint32_t *p2, uint32_t r14)
{
    *p2 = PLEXA;
    s->r[15] = r14;
}
