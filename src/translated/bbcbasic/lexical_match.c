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

/* lexical_match.c: BASIC's tokeniser, translated by hand from
 * Lexical.s:143-412 (RISC OS 5.31's BASIC, the VFP build).  Unit 2b.
 *
 * The original is one state machine with four entries:
 *   - MATCH for a command line
 *   - EVMATCH for EVAL, with the destination on the stack
 *   - AUMATCH for AUTO
 *   - CLIENTMATCH for outside callers.  The box never dispatches this
 *     address.  It has no code-register entry, so nothing reaches it.
 * The lifter cut the machine into a web of tail-called functions, one
 * for each of the machine's %labels.  Here the three entries are
 * thunks (the lift's MATCH/EVMATCH/AUMATCH definitions).  The machine
 * is this one function, with the %labels as goto labels where the
 * states cycle.
 *
 * The registers, as Lexical.s:135-142 documents them:
 *   - SOURCE (R1)
 *   - DEST (R2)
 *   - MODE (R3): 0 for left mode, 1 for right mode
 *   - CONSTA (R4): encode line-number constants
 *   - SMODE (R5): bit 0 inside quotes, bit 2 impossible-string mode,
 *     bit 8 constant too large, bits 12 and up the bracket level.
 * R8 saves the start of the match.  R6 and R7 are scratch for the
 * table walker.  The arena stack holds the pushed return address.
 * While a keyword or constant is being tried it also holds
 * {SMODE,R6,R7,R8} or {SOURCE,SMODE,CONSTA,MODE}.
 *
 * The quirks kept on purpose, each with its line:
 *   - LF becomes CR in the copy, overwriting the byte just written
 *     (Lexical.s:160-163).
 *   - The bracket level is never counted inside strings.  An error in
 *     it (SMODE negative) stops the counting, and it stays stopped
 *     (:164-170).
 *   - '&' starts a hex run of digits and A-F/a-f only.  Anything else
 *     ends it.  A letter outside 'A'-'F' falls to the keyword matcher
 *     (:171-186).
 *   - ':' restarts the statement in left mode with constants off
 *     (:187).  ',' is copied with no change of state (:188).  '*' sets
 *     impossible-string mode in left mode, and switches to right mode
 *     in right mode (:189-195).
 *   - A digit run is encoded as a line-number constant only when
 *     CONSTA allows and the value stays under 65280.  If it is too
 *     large, the run is copied again as text and SMODE gains bit 8.
 *     The digit that overflowed is never copied, exactly as the
 *     original's fall into MATCHZ skips it (:211-246).
 *   - Keywords are matched from the table at INDEXTAB, with one chain
 *     for each first letter.  Each entry ends in a byte >= &7F (the
 *     token) and the job byte before it.  A trailing '.' abbreviates
 *     (:237-259, 270-278).
 *   - The VDUP collision (:283-298).  In right mode, PRINT abbreviated
 *     to "P." after an unabbreviated VDU is the VFP opcode VDUP and
 *     not the keyword.  The original implants "[TVDU]P." again.  It
 *     writes P, then overwrites it with '.' at the implant step, and
 *     switches back to left mode.
 *   - TRACE in right mode turns constants off for what follows
 *     (:280-282).
 *   - Two-byte tokens (job bit 3) plant TESCSTMT, TESCFN or TESCCOM
 *     first.  A polymorphic token in left mode has its value increased
 *     by TPTR2-&8F (:300-320).
 *   - FN and PROC copy their name over (:322-330).
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "lexical_match.h"

/* The scan helpers of unit 2a, called where the machine's BLs are. */
void basicvfp_hand_NUMBCP(struct ros_cpu *s);
void basicvfp_hand_NUMBCQ(struct ros_cpu *s);
void basicvfp_hand_WORDCQ(struct ros_cpu *s);
void basicvfp_hand_CONSTI(struct ros_cpu *s, uint32_t *p2, uint32_t *p4,
                          uint32_t *p5, uint32_t r14);

/* Tokens and table addresses, as the lift defines them.  These are its
 * own constants.  INDEXTAB and LEXICALADR are where the ROM image laid
 * the tables, and they change only if the source does. */
#define TCONST 141u
#define TWIDTH 254u
#define TESC_FN 198u
#define TESC_COM 199u
#define TESC_STMT 200u
#define TFN 164u
#define TPROC 242u
#define TPRINT 241u
#define TTRACE 252u
#define TVDU 239u
#define INDEXTAB 0xFC10DF00u          /* the A..W chains, a word each */
#define LEXICALADR 0xFC10DEACu        /* chain addresses are from here */
#define OUTPUT_OFF 1280u              /* DEST for command lines: ARGP-&500 */

/* The condition-code helpers.  Each returns C from the class test, as
 * the BLs read it.  The hand helpers work in R[], so the character
 * goes there first.  The marker return address is never examined. */
static int numbcp_c(struct ros_cpu *s, uint32_t c)
{
    s->r[0] = c;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_NUMBCP(s);
    return s->c;
}

static int numbcq_c(struct ros_cpu *s, uint32_t c)
{
    s->r[0] = c;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_NUMBCQ(s);
    return s->c;
}

static int wordcq_c(struct ros_cpu *s, uint32_t c)
{
    s->r[0] = c;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_WORDCQ(s);
    return s->c;
}

/* The machine.  It is entered with the state in R[].  It returns
 * through the address its entry pushed, as `LDR PC,[SP],#4` did. */
static void match_from_99(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = 0, r7 = 0, r8 = s->r[8],
             r13 = s->r[13], r14 = s->r[14];
    uint32_t tab;

L99: /* copy the next byte, assuming it is not a token */
    r0 = ros_ld8(r1);
    r1 += 1;
    ros_st8(r2, r0);
    r2 += 1;
L00:
    if (r0 == 32)
        goto L99;                              /* %99: repeat if " " */
    if (r0 == 10) {                            /* TEQ R0,#10 */
        r0 = 13;                               /* MOVEQ R0,#13 */
        ros_st8(r2 - 1, r0);                   /* STREQB R0,[DEST,#-1] */
    }
    if (r0 == 13) {                            /* return: pop into pc */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[14] = r14;
        {
            uint32_t t = ros_ld32(r13);
            s->r[13] = r13 + 4;
            s->r[15] = t;
        }
        return;
    }
    if (r0 == 34)                              /* CMP R0,#"""" */
        r5 ^= 1;                               /* EOREQ SMODE,SMODE,#1 */
    if (r5 & 0xFFu)                            /* TST SMODE,#255 */
        goto L99;                              /* in a string (or REM) */
    if ((int32_t)r5 >= 0) {                    /* TEQ SMODE,#0 ; BMI %05 */
        if (r0 == 40)
            r5 += 0x1000u;                     /* "(" */
        if (r0 == 41)
            r5 -= 0x1000u;                     /* ")" */
    }
L05:
    if (r0 == 38) {                            /* '&': a hex run */
        for (;;) {
            r0 = ros_ld8(r1);
            r1 += 1;
            ros_st8(r2, r0);
            r2 += 1;
            if (!numbcp_c(s, r0)) {
                if (r0 < 65)
                    goto L00;                  /* <'A': done with it */
                if (r0 < 71)
                    continue;                  /* 'A'-'F' */
                if (r0 < 97)
                    break;                     /* 'G'-'@'..'Z': %10 */
                if (r0 < 103)
                    continue;                  /* 'a'-'f' */
                break;                         /* 'g' on: %10 */
            }
        }
    }
L10:
    if (r0 == 58) {                            /* ':': a statement starts */
        r4 = 0;                                /* %95 */
        r3 = 0;                                /* %97 */
        goto L99;
    }
    if (r0 == 44)                              /* ',': no state change */
        goto L99;
    if (r0 == 42) {                            /* '*' */
        if (r3 == 0) {                         /* left mode */
            r5 |= 4;                           /* impossible string mode */
            goto L99;
        }
        goto LYMATCH;                          /* else right mode */
    }
L20:
    if (r0 == 46)                              /* '.': MATCHZ */
        goto LMATCHZ;
    if (!numbcp_c(s, r0))
        goto L30;                              /* not a digit */
    if (r4 == 0)
        goto LMATCHZ;                          /* constants not allowed */
    { /* %24: encode a constant line number, if it fits */
        r13 -= 16;
        ros_st32(r13, r1);
        ros_st32(r13 + 4, r3);
        ros_st32(r13 + 8, r4);
        ros_st32(r13 + 12, r5);
        r5 = r0 & 0xFu;
        for (;;) {
            r0 = ros_ld8(r1);
            r1 += 1;
            if (!numbcp_c(s, r0)) {
                /* %26: it fits.  Overwrite the number with the token,
                 * encode it, and copy the byte that ended it */
                ros_st8(r2 - 1, r4);
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_hand_CONSTI(s, &r2, &r4, &r5, s->r[14]);
                ros_st8(r2, r0);
                r2 += 1;
                r3 = ros_ld32(r13 + 4);        /* LDMFD, dropping SOURCE */
                r4 = ros_ld32(r13 + 8);
                r5 = ros_ld32(r13 + 12);
                r13 += 16;
                goto L00;
            }
            r5 = (r0 & 0xFu) + ((r5 + (r5 << 2)) << 1);
            if (r5 >= 0xFF00u) {               /* too large for one */
                r1 = ros_ld32(r13);
                r3 = ros_ld32(r13 + 4);
                r4 = ros_ld32(r13 + 8);
                {
                    uint32_t sm = ros_ld32(r13 + 12);
                    r13 += 16;
                    r5 = sm | 0x100u;
                }
                goto LMATCHZ;                  /* the digit is lost, as
                                                 * the original's fall
                                                 * into MATCHZ loses it */
            }
        }
    }
L30:
    if (r0 < 65)
        goto LYMATCH;                          /* not a keyword start */
    if (r0 > 87) {                             /* past 'W': MATCHW */
        if (!wordcq_c(s, r0))
            goto LYMATCH;
        goto LMATCHH;
    }
    /* 'A'..'W': walk the chain */
    r13 -= 16;                                 /* STMFD {SMODE,R6,R7,R8} */
    ros_st32(r13, r5);
    ros_st32(r13 + 4, r6);
    ros_st32(r13 + 8, r7);
    ros_st32(r13 + 12, r8);
    tab = LEXICALADR + ros_ld32(INDEXTAB + (r0 << 2));
L50:
    for (;;) {
        r6 = ros_ld8(tab);
        tab += 1;
        if (r0 != r6)
            goto LMATCHG;                      /* not this chain */
        r8 = r1;                               /* save the start */
        for (;;) {
            r6 = ros_ld8(tab);
            tab += 1;
            if (r6 >= 0x7Fu)
                goto L56;                      /* the token: a match */
            r7 = ros_ld8(r1);
            r1 += 1;
            if (r6 != r7)
                break;                         /* mismatch */
        }
        if (r7 == 46) {                        /* '.': abbreviated */
            do {
                r6 = ros_ld8(tab);
                tab += 1;
            } while (r6 < 0x7Fu);
            goto L56;
        }
        r1 = r8;                               /* reset and find the
                                                 * next token in the chain */
        do {
            r6 = ros_ld8(tab);
            tab += 1;
        } while (r6 < 0x7Fu);
        if (r6 == TWIDTH)
            goto LMATCHG;                      /* the last: give up */
        tab += 1;                              /* step past the job */
    }
L56: /* a keyword matched; r6 is its token, tab points at its job */
    {
        uint32_t tok = r6;
        uint32_t job = ros_ld8(tab);
        int vdup = 0;
        if (r3 == 1 && tok == TPRINT && r7 == '.') {
            /* PRINT abbreviated "P." after an unabbreviated VDU is the
             * VFP opcode VDUP: re-implant "[TVDU]P." (Lexical.s:283) */
            r14 = ros_ld8(r1 - 2);
            if (r14 == 0x50u) {                /* 'P' */
                uint32_t d2 = ros_ld8(r2 - 2);
                if (d2 == TVDU) {
                    uint32_t s3 = ros_ld8(r1 - 3);
                    if (s3 == 0x55u) {         /* 'U' */
                        ros_st8(r2, r14);      /* the P ... */
                        r2 += 1;
                        r6 = 46;               /* ... which the implant
                                                 * below overwrites with
                                                 * '.', giving
                                                 * [TVDU][P][.] */
                        job = 4;               /* back to left mode */
                        vdup = 1;
                    }
                }
            }
        }
        if (!vdup) {
            if (r3 == 1 && tok == TTRACE)
                job &= ~0x10u;                 /* TRACE: no constants */
            if (job & 1) {                     /* next char not a wordc */
                uint32_t next = ros_ld8(r1);
                if (wordcq_c(s, next)) {
                    r1 = r8;
                    goto LMATCHG;              /* fail the match */
                }
            }
            if (job & 8) {                     /* two-byte token */
                r7 = (job & 4) ? TESC_FN
                    : (job & 0x40u) ? TESC_STMT : TESC_COM;
                if (job & 4)
                    job &= ~4u;
                ros_st8(r2 - 1, r7);
                r2 += 1;
            } else if (job & 0x40u) {          /* polymorphic */
                if (r3 == 0)
                    r6 += 64;                  /* TPTR2-&8F */
            }
        }
        ros_st8(r2 - 1, r6);                   /* implant the token */
        if (job & 4) {                         /* to left mode */
            r3 = 0;
            r4 = 0;
        } else if (job & 2) {                  /* to right mode */
            r3 = 1;
            r4 = 0;
        }
        if (r6 == TFN || r6 == TPROC) {        /* copy the name over */
            do {
                r0 = ros_ld8(r1);
                r1 += 1;
                ros_st8(r2, r0);
                r2 += 1;
            } while (wordcq_c(s, r0));
            r1 -= 1;
            r2 -= 1;
        }
        if (job & 0x10u)
            r4 = TCONST;                       /* constants may follow */
        r5 = ros_ld32(r13);                    /* LDMFD {SMODE,R6,R7,R8} */
        r6 = ros_ld32(r13 + 4);
        r7 = ros_ld32(r13 + 8);
        r8 = ros_ld32(r13 + 12);
        r13 += 16;
        if (job & 0x20u)
            r5 |= 4;                           /* give up completely */
        if (job & 0x80u)
            r5 += 0x1000u;                     /* owns its bracket */
        /* This is the only place the machine stores R6-R8.  It is the
         * lift's exit from the match, and callers reload them after
         * the call */
        s->r[6] = r6;
        s->r[7] = r7;
        s->r[8] = r8;
        goto L99;
    }
LMATCHZ: /* copy the number through, then right mode */
    do {
        r0 = ros_ld8(r1);
        r1 += 1;
        ros_st8(r2, r0);
        r2 += 1;
    } while (numbcq_c(s, r0));
    /* fall into MATCHY */
LMATCHY:
    r4 = 0;
    r3 = 1;
    goto L00;
LYMATCH:
    r4 = 0;
    r3 = 1;
    goto L99;
LMATCHG: /* give up on the chain: pop, then copy the word through */
    r5 = ros_ld32(r13);
    r6 = ros_ld32(r13 + 4);
    r7 = ros_ld32(r13 + 8);
    r8 = ros_ld32(r13 + 12);
    r13 += 16;
    /* fall into MATCHH */
LMATCHH:
    do {
        r0 = ros_ld8(r1);
        r1 += 1;
        ros_st8(r2, r0);
        r2 += 1;
    } while (wordcq_c(s, r0));
    goto LMATCHY;
}

/* ---- the entries (Lexical.s:143-157) --------------------------------- */

/* MATCH: a command line, into the OUTPUT buffer, in left mode with
 * constants.  SOURCE arrives in R1. */
void basicvfp_hand_MATCH(struct ros_cpu *s)
{
    s->r[13] -= 4;                             /* STR R14,[SP,#-4]! */
    ros_st32(s->r[13], s->r[14]);
    s->r[5] = 0;                               /* not in a string */
    s->r[2] = s->r[8] - OUTPUT_OFF;            /* DEST = ARGP+OUTPUT */
    s->r[4] = TCONST;                          /* left mode, constants */
    s->r[3] = 0;
    match_from_99(s);
}

/* EVMATCH: EVAL's expression, in right mode without constants.  The
 * destination is on the stack, below the pushed return address. */
void basicvfp_hand_EVMATCH(struct ros_cpu *s)
{
    uint32_t dest = s->r[13];                  /* MOV DEST,SP */
    s->r[13] -= 4;
    ros_st32(s->r[13], s->r[14]);
    s->r[2] = dest;
    s->r[5] = 0;
    s->r[4] = 0;                               /* YMATCH: right, none */
    s->r[3] = 1;
    match_from_99(s);
}

/* AUMATCH: AUTO's line, into the OUTPUT buffer, in left mode without
 * constants. */
void basicvfp_hand_AUMATCH(struct ros_cpu *s)
{
    s->r[13] -= 4;
    ros_st32(s->r[13], s->r[14]);
    s->r[2] = s->r[8] - OUTPUT_OFF;
    s->r[5] = 0;
    s->r[4] = 0;                               /* %95: no constants */
    s->r[3] = 0;                               /* %97: left mode */
    match_from_99(s);
}
