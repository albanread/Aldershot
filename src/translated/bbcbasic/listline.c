/* Copyright 2001 Pace Micro Technology plc
 * Copyright 2009 Castle Technology Ltd
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
 * (Sources/Programmer/BASIC: s.Command, s.Lexical, hdr.Tokens).
 */

/* listline.c: LISTLINE, which prints a program line for LIST or the
 * printer.  Translated by hand from Command.s:608-659 (RISC OS 5.31's
 * BASIC, the VFP build).  It is the formatter behind LIST, LVARLINE
 * and the spooling TWIN.  It takes one program line from its tokenised
 * form to the output stream, under the LISTO bits in R7:
 *   - bit 0: spaces after the line number
 *   - bit 1: the indent
 *   - bit 2: statement splits at ':'
 *   - bit 3: the mode without line numbers, which LVARLINE enters
 *     with.
 * The lift had 236 lines for it.  Here it is one C function, and the
 * labels are the original's, as gotos.
 *
 * The conventions are those of the landed units.  State is in R[].  A
 * BL becomes a marker call, with R14 := 0xFFFFFFF0u.  The lift's store
 * sets and reload sets are copied one for one, so the ARM register
 * file at every boundary is the lift's.  A B becomes a plain call
 * whose result is discarded, or a goto when the target is a label of
 * this function.
 *
 * THE SETJMP QUESTION, settled for this unit.  The lifted body holds
 * 7 setjmp resume points (NPRN &FC10EE40, CHOUT &FC10EE5C,
 * CHOUTNOCOUNT &FC10EE64 and &FC10EE70, POSITE &FC10EED8, TOKOUT
 * &FC10EEFC, MSG &FC10FE94).  It also holds five plain bad-return
 * sites (SPCOUT &FC10EE4C/&FC10EE94/&FC10EE88, SPGETN &FC10EED4, CHOUT
 * &FC10EE9C).  A resume into this function needs a deep callee to
 * execute ros_resume with t set to one of this function's call-site
 * addresses.  In the twin no such path exists:
 *   - The only performers in the twin are these:
 *       - DISPAT's three (MOV PC,R7 / MOV PC,R11).  The target is a
 *         program address resolved by GOFACT, a continuation in the
 *         AJ7 ladder, or the user code address of the CALL statement.
 *       - The three jump-table fragments of the EXPR operators (ADD
 *         PC,R4,R14,LSL #2).  Their targets are FC102 ladder
 *         addresses.
 *       - MSGATLINE, the message system's return.
 *       - The start chain of Basic_Code.
 *       - The hand GTARGS.  Its stored entry link is its own caller's
 *         site, which is always one its hand callers pushed: ENDPR's
 *         &FC105270 or FNRET's &FC104A60.
 *       - The hand DOSTAR/CALL transfers in dispat_sys, to user code
 *         addresses.
 *   - This function's dynamic callee tree is NPRN/PRN, SPCOUT, CHOUT
 *     (with CHOUTS1/2), CHOUTNOCOUNT, TOKOUT, SPGETN, POSITE and MSG.
 *     None of these executes a statement or evaluates an expression.
 *     So DISPAT, the EXPR ladder and GTARGS are never entered from it.
 *     The static closure of CHOUT does reach the whole interpreter.
 *     But it does so only through its exit for a full spool buffer, B
 *     FSASET -> B CLRSTK.  That restarts the immediate-mode loop and
 *     never returns into the listing.  The abandoned frames are the
 *     same as in the lift.
 *   - Errors (MSG, and CHOUT's message for a full buffer) unwind to
 *     the environment's ORDERR frame.  That frame was pushed before
 *     this function was entered, so they never unwind back into it.
 *   - So no resume point is pushed, and the function holds no setjmp.
 *     Every one of the 12 sites is a plain call.  The one callee that
 *     READS its call site is MSG, whose error number and token are the
 *     two bytes after the BL.  That call carries the real site address
 *     (the ll_msg_at pattern).  The guard after it, for a call that
 *     must never continue, is the lift's own.
 *
 * The quirks kept, with their lines:
 *   - The entry link is pushed, and the CR exit pops it straight to PC
 *     (Command.s:609, 636-637).  The function returns THROUGH R14.  So
 *     the hand body sets R15 to the popped link, and the bad-return
 *     check in the lift's caller sees its own site.
 *   - Unless LISTO bit 3 is set, the line number is printed from R0
 *     exactly as the caller left it (Command.s:610-611, 929-937).
 *     CHKLST in CLRSTK passes the combined number.  LVARLINE never
 *     prints it, because it enters with R7 = 8.
 *   - The indent (bit 1) is printed only when the count in R3 is
 *     nonzero (MOVS R0,R3; TSTNE R7,#2; BLNE, Command.s:630-632).
 *   - A '"' toggles the expansion flag with EOR #1 (Command.s:638-639).
 *     A REM sets the flag to 4.  After that, quotes move it between 4
 *     and 5, and the line stays raw to its end (Command.s:656-657).
 *   - The ':' split (bit 2).  The colon is printed, then an uncounted
 *     LF.  A CR follows only when the stream is like the screen
 *     (R7 < 128).  TALLY is reset.  The next statement starts
 *     (R7&1)+5 spaces in, or just R7&1 if bit 3 is set
 *     (Command.s:616-629).
 *   - Characters below &7F are printed raw (Command.s:644-645).  A
 *     TCONST line number is decoded by SPGETN, and printed by POSITE
 *     with the expansion flag cleared (Command.s:646-650).  If bit 3
 *     is set (the LVARLINE mode), the listing stops.  It sends a
 *     newline to screen streams and gives message 0,12, "LIST found
 *     line number reference" (Command.s:651-655; ErrorMsgs:154-157).
 *   - Token bytes go to TOKOUT, which puts them in lower case if LISTO
 *     bit 4 is set (Command.s:658; Lexical.s:379).
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "listline.h"

/* ---- the lift's functions this unit calls --------------------------
 * (EXPORTS in the manifest makes them non-static in the twin). */
void basicvfp_NPRN(struct ros_cpu *s);
void basicvfp_SPCOUT(struct ros_cpu *s);
void basicvfp_CHOUT(struct ros_cpu *s);
void basicvfp_CHOUTNOCOUNT(struct ros_cpu *s);
void basicvfp_POSITE(struct ros_cpu *s);

/* The hand units' functions (landed). */
void basicvfp_hand_TOKOUT(struct ros_cpu *s);
void basicvfp_hand_SPGETN(struct ros_cpu *s);
void basicvfp_hand_MSG(struct ros_cpu *s);

/* The OS thunk.  The harness's api_gen declares it.  Each unit declares
 * it again, as the landed units do. */
void ros_thunk_OS_NewLine(struct ros_cpu *s);

/* Tokens, as the lift defines them from hdr/Tokens.  Identical
 * definitions in other units do no harm. */
#define TCONST           141u
#define TREM             244u

/* The MSG call site.  The error number and token are the two bytes
 * after the BL, read through R14 (USESLINENUMBERS, message 0,12). */
#define SITE_USESLINENUMBERS  0xFC10FE94u

static void ll_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* =====================================================================
 * LISTLINE (Command.s:608-659): print one tokenised program line.
 * The thunk enters it from two places:
 *   - CHKLST in CLRSTK, the LIST loop.  R0 = the line number, R3 =
 *     the indent count, R7 = LISTOP.
 *   - LVARLINE, which lists the first line for LVAR.  R7 = 8.
 * It returns by popping the entry link to PC at the line's CR.
 * ===================================================================== */
void basicvfp_hand_LISTLINE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r3 = s->r[3], r4, r7 = s->r[7], r8 = s->r[8],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v1, v2, v3, v4, v5;

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    v1 = r7 & 8;                                /* TST R7,#8 */
    if (v1 == 0) {                              /* BLEQ NPRN */
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;                 /* BL NPRN */
        basicvfp_NPRN(s);
        r3 = s->r[3]; r7 = s->r[7]; r8 = s->r[8]; r12 = s->r[12];
        r13 = s->r[13];
    }
    r4 = 0;                                     /* MOV R4,#0 */
    r0 = r7 & 1;                                /* ANDS R0,R7,#1 */
    if (r0 != 0) {                              /* BLNE SPCOUT */
        s->r[0] = r0; s->r[4] = r4; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;                 /* BL SPCOUT */
        basicvfp_SPCOUT(s);
        r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
        r12 = s->r[12]; r13 = s->r[13];
    }
    goto LPSD1;                                 /* B LPSD1 */

LPSD1:
    r0 = r3;                                    /* MOVS R0,R3 */
    v3 = r7 & 2;                                /* TSTNE R7,#2 */
    if (r0 != 0 && v3 != 0) {                   /* BLNE SPCOUT */
        s->r[0] = r0; s->r[4] = r4; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;                 /* BL SPCOUT */
        basicvfp_SPCOUT(s);
        r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
        r12 = s->r[12]; r13 = s->r[13];
    }
    goto LP;                                    /* B LP */

LP:
    r0 = ros_ld8(r12);                          /* LDRB R0,[LINE],#1 */
    r12 += 1;
    ros_subs(s, r0, 13);                        /* CMP R0,#13 */
    if (r0 == 13) {                             /* LDREQ PC,[SP],#4 */
        s->r[0] = r0; s->r[4] = r4; s->r[12] = r12;
        {
            uint32_t t = ros_ld32(r13);
            s->r[13] = r13 + 4;
            s->r[15] = t;
            return;
        }
    }
    if (r0 == 34) r4 ^= 1;          /* EOREQ R4,R4,#1 ; flip expansion */
    if (r4 != 0) goto LPQUOT;                   /* BNE LPQUOT */
    v4 = r0 ^ 0x3Au;                            /* TEQ R0,#":" */
    if (v4 == 0) goto LPSD;                     /* BEQ LPSD */
    if (r0 < 0x7Fu) goto LPQUOT;                /* BCC LPQUOT */
    ros_subs(s, r0, TCONST);                    /* CMP R0,#TCONST */
    if (r0 != TCONST) goto LPSIMP;              /* BNE LPSIMP */
    /* TCONST: the line-number constant.  It is decoded and printed as
     * a number, and never expanded (Command.s:646-655). */
    s->r[0] = r0; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPGETN */
    basicvfp_hand_SPGETN(s);
    r0 = s->r[0]; r12 = s->r[12];
    s->r[13] = r13;                             /* BL POSITE */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_POSITE(s);
    r0 = s->r[0]; r3 = s->r[3]; r7 = s->r[7]; r8 = s->r[8];
    r12 = s->r[12]; r13 = s->r[13];
    r4 = 0;                                     /* MOV R4,#0 */
    v5 = r7 & 8;                                /* TST R7,#8 */
    if (v5 == 0) goto LP;                       /* BEQ LP */
    ros_subs(s, r7, 128);                       /* CMP R7,#128 */
    if (r7 < 128) {                             /* SWICC OS_NewLine */
        ros_native_swi(s, ros_thunk_OS_NewLine);
        if (s->v) ros_swi_raise(s);
        r0 = s->r[0];
    }
    s->r[4] = r4;                               /* B USESLINENUMBERS */
    ll_msg_at(s, SITE_USESLINENUMBERS);         /* BL MSG ; = 0,12 */

LPSIMP:
    if (r0 == TREM) r4 = 4;         /* MOVEQ R4,#4 ; no more expansion */
    s->r[0] = r0; s->r[4] = r4; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL TOKOUT */
    basicvfp_hand_TOKOUT(s);
    r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
    r12 = s->r[12]; r13 = s->r[13];
    goto LP;                                    /* B LP */

LPSD:
    /* the ':' statement split (Command.s:616-629). */
    v2 = r7 & 4;                                /* TST R7,#4 */
    if (v2 == 0) goto LPQUOT;                   /* BEQ LPQUOT */
    s->r[0] = r0; s->r[4] = r4; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CHOUT ; the ':' */
    basicvfp_CHOUT(s);
    r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
    r12 = s->r[12]; r13 = s->r[13];
    r0 = 10;                                    /* MOV R0,#10 */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CHOUTNOCOUNT */
    basicvfp_CHOUTNOCOUNT(s);
    r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
    r12 = s->r[12]; r13 = s->r[13];
    ros_subs(s, r7, 128);                       /* CMP R7,#128 */
    r0 = 13;                                    /* MOV R0,#13 */
    if (r7 < 128) {                             /* BLCC CHOUTNOCOUNT */
        s->r[0] = r0;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_CHOUTNOCOUNT(s);
        r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
        r12 = s->r[12]; r13 = s->r[13];
    }
    ros_st32(r8 - 264, 0);                      /* STR R0,[ARGP,#TALLY] */
    r0 = r7 & 1;                                /* AND R0,R7,#1 */
    if ((r7 & 8) == 0) r0 += 5;                 /* ADDEQ R0,R0,#5 */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPCOUT */
    basicvfp_SPCOUT(s);
    r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
    r12 = s->r[12]; r13 = s->r[13];
    goto LPSD1;                                 /* B LPSD1 */

LPQUOT:
    s->r[0] = r0; s->r[4] = r4; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CHOUT */
    basicvfp_CHOUT(s);
    r3 = s->r[3]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8];
    r12 = s->r[12]; r13 = s->r[13];
    goto LP;                                    /* B LP */
}
