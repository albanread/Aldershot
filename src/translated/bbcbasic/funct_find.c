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
 * (Sources/Programmer/BASIC: s.Funct).
 */

/* funct_find.c -- BASIC's DEF search, translated by hand from
 * Funct.s:109-150 (RISC OS 5.31's BASIC, the VFP build). It holds two
 * routines:
 *   - FNFIND scans one program section for the DEF PROC or DEF FN
 *     being called;
 *   - FNDEFLIST walks down the LIBRARY/INSTALL chain and calls FNFIND
 *     for each library.
 * The callers (FNBODY's cache-miss path, in funct_call.c) hand FNFIND
 * the section base in R5, the FN or PROC token in R10, the name's first
 * character in R3, the name's start in R4 and its end (AELINE) in R11.
 * FNFIND hands back R0/R2 one character past the name of a matching
 * DEF, ready for FNINSTANT's parameter-list reader.
 *
 * The layout: a program section is a run of lines. Each line has a
 * 4-byte header followed by the tokenised text. The header holds the
 * line number, with its high byte at +1, and the length byte at +3.
 * The section ends where the line number's high byte is &FF. The first
 * word of a library block is the link to the next library, and its
 * code area starts at +4.
 *
 * The quirks kept, with their lines:
 *   - "not found" is the EQ code, which comes straight from the test
 *     for the terminating line number (Funct.s:110-111). R5 has stopped
 *     on the &FF terminator line, and R0 still holds the &FF.
 *   - R5 steps onto the NEXT line, by the single length byte at +3,
 *     before this line is scanned (Funct.s:113-114). So on a find R5
 *     already addresses the line after the DEF's line.
 *   - The found/not-found flag is the Z of MOVS R14,R6 at Funct.s:137.
 *     The MOVS keeps the carry that WORDCQ has just left. WORDCQ tests
 *     whether the character AFTER the name can be part of a word, and
 *     BCS (Funct.s:138) uses that to reject a definition with a longer
 *     name. Z says "found" only because the saved return address (in
 *     R6, Funct.s:135) is never zero.
 *   - The definition's next character is read before the caller's name
 *     end is tested (Funct.s:128-130). So on a find R0/R2 sit one
 *     character past the matched name. FNINSTANT's CMP R0,#" " and
 *     CMP R0,#"(" expect exactly that on entry.
 *   - The second name character is loaded between the two compares
 *     (LDREQB, Funct.s:124). The conditionally executed pair
 *     TEQ R0,R10 / TEQEQ R0,R3 (Funct.s:123-125) tests the FN or PROC
 *     token and the first character together.
 *   - FNDEFLIST pushes its return address only to hand it on
 *     (Funct.s:145-147). If it finds the DEF, it branches into
 *     FNINSTANT with the caller's own address. This is the "naughty
 *     exit" (Funct.s:150). The definition is created and the call
 *     entered without FNDEFLIST ever returning. If it does not find
 *     the DEF, it steps to the next library by the link word at the
 *     start of the block (Funct.s:148-149) and loops, testing again for
 *     the end of the chain.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "funct_find.h"

/* WORDCQ is translated by hand in lexical_scan.c (unit 2a). FNINSTANT
 * is still the lift's, because its definition machinery is the next
 * unit of this area. */
void basicvfp_hand_WORDCQ(struct ros_cpu *s);
void basicvfp_FNINSTANT(struct ros_cpu *s);

/* The DEF token, as the lift defines it. */
#define TDEF 221u

/* FNFIND (Funct.s:109-139) scans the section at R5 for the DEF whose
 * name is R4..AELINE. On a find it returns NE (Z clear) with R0/R2 one
 * character past the name. Otherwise it returns EQ (Z set) with R5 on
 * the &FF terminator line. The lifted signature is kept. The thunk
 * passes as pointers the registers that the lifter merged. */
void basicvfp_hand_FNFIND(struct ros_cpu *s, uint32_t *p0, uint32_t *p2,
                          uint32_t r3, uint32_t r4, uint32_t *p5,
                          uint32_t *p6, uint32_t *p7, uint32_t r10,
                          uint32_t r11, uint32_t *p14)
{
    uint32_t r0, r2 = *p2, r5 = *p5, r6 = *p6, r7 = *p7, r14 = *p14;
    uint32_t v1, v2, v3, v4;

    for (;;) {
FNFIND:  /* one line of the section per turn */
        r0 = ros_ld8(r5 + 1);               /* LDRB R0,[R5,#1] */
        ros_subs(s, r0, 0xFFu);             /* CMP R0,#&FF */
        if (r0 == 0xFFu) {                  /* MOVEQ PC,R14 ; not found, EQ */
            *p0 = r0; *p2 = r2; *p5 = r5; *p6 = r6; *p7 = r7; *p14 = r14;
            s->r[15] = r14;
            return;
        }
        r2 = r5 + 4;                        /* ADD R2,R5,#4 ; first token */
        r5 += ros_ld8(r5 + 3);              /* ADD R5,R5,R0 ; the next line */
        do {                                /* FNFINS: the leading spaces */
            r0 = ros_ld8(r2);
            r2 += 1;
        } while (r0 == 32);
        if (r0 == TDEF) {                   /* CMP R0,#TDEF ; BNE FNFIND */
            do {                            /* FNDEFA: spaces after DEF */
                r0 = ros_ld8(r2);
                r2 += 1;
            } while (r0 == 32);
            v1 = r0 ^ r10;                  /* TEQ R0,R10 ; the FN/PROC? */
            if (v1 == 0) {                  /* LDREQB R0,[R2],#1 */
                r0 = ros_ld8(r2);
                r2 += 1;
            }
            v2 = r0 ^ r3;                   /* TEQEQ R0,R3 ; first char? */
            if (v1 == 0 && v2 == 0) {       /* BNE FNFIND ; neither */
                r7 = r4;                    /* MOV R7,R4 */
                for (;;) {                  /* FNDFLP: the name compare */
                    r0 = ros_ld8(r2);
                    r2 += 1;
                    v3 = r7 ^ r11;          /* TEQ R7,AELINE */
                    if (v3 == 0) {          /* BEQ FNDFEN ; the list end */
                        r6 = r14;           /* MOV R6,R14 */
                        s->r[0] = r0;       /* BL WORDCQ */
                        s->r[14] = 0xFFFFFFF0u;
                        basicvfp_hand_WORDCQ(s);
                        /* the leaf returns to the marker. Only its
                         * carry is read. */
                        r14 = ros_logic(s, r6, s->c); /* MOVS R14,R6 */
                        if (s->c)
                            goto FNFIND;    /* BCS FNFIND ; a longer name */
                        *p0 = r0; *p2 = r2; *p5 = r5; *p6 = r6;
                        *p7 = r7; *p14 = r14;
                        s->r[15] = r14;     /* MOV PC,R14 ; found, NE */
                        return;
                    }
                    r6 = ros_ld8(r7);       /* LDRB R6,[R7],#1 */
                    r7 += 1;
                    v4 = r6 ^ r0;           /* TEQ R6,R0 */
                    if (v4 != 0)
                        goto FNFIND;        /* B FNFIND ; a char differs */
                }
            }
        }
    }
}

/* FNDEFLIST (Funct.s:141-150) walks the library chain in R1. It
 * returns only at the end of the chain (EQ, nothing found). A find
 * leaves through FNINSTANT, which never comes back here. */
void basicvfp_hand_FNDEFLIST(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1], r5 = s->r[5], r13 = s->r[13], r14 = s->r[14];

    do {
        ros_subs(s, r1, 0);                 /* CMP R1,#0 */
        if (r1 == 0) {                      /* MOVEQ PC,R14 ; chain end */
            s->r[1] = r1; s->r[5] = r5; s->r[13] = r13; s->r[14] = r14;
            s->r[15] = r14;
            return;
        }
        r5 = r1 + 4;                        /* ADD R5,R1,#4 ; the code area */
        r13 -= 4;                           /* STR R14,[SP,#-4]! */
        ros_st32(r13, r14);
        r14 = 0xFFFFFFF0u;                  /* BL FNFIND */
        basicvfp_hand_FNFIND(s, &s->r[0], &s->r[2], s->r[3], s->r[4],
                             &r5, &s->r[6], &s->r[7], s->r[10], s->r[11],
                             &r14);
        /* FNFIND holds no resume points. It returns only to here. */
        r14 = ros_ld32(r13);                /* LDR R14,[SP],#4 */
        r13 += 4;
        if (s->z)
            r1 = ros_ld32(r1);              /* LDREQ R1,[R1] ; next library */
    } while (s->z);                         /* BEQ FNDEFLIST */
    s->r[1] = r1; s->r[5] = r5; s->r[13] = r13; s->r[14] = r14;
    s->r[15] = r14;
    basicvfp_FNINSTANT(s);                  /* B FNINSTANT ; naughty exit */
    return;
}
