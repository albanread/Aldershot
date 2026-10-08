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
 * (Sources/Programmer/BASIC: s.Lexical, hdr.Tokens).
 */

/* lexical_scan.c: BASIC's lexical scan helpers, translated by hand
 * from Lexical.s (RISC OS 5.31's BASIC, the VFP build).  This is unit
 * 2 of the tier 3 translation.  It was the first unit substituted as
 * whole functions in place of dispatch cases.  The twin's basicvfp_X
 * becomes a thunk that calls the basicvfp_hand_X here, with the lift's
 * own signature.  Registers the lifter could not keep within one
 * function arrive as pointer parameters, and must be written back
 * through them.
 *
 * These routines join the rest of the interpreter together.  They are:
 *   - every statement's scan for its end (SPACES, DONE)
 *   - the decoder for line-number constants (SPTSTN/SPGETN)
 *   - the character classes that the tokeniser and the evaluators
 *     test with (WORDCQ, NUMBCP, NUMBCQ).
 * The ARM idiom of returning answers in the condition codes is used
 * most heavily here.  Each hand function leaves the flags exactly as
 * the original's last flag-setting instruction left them.  Where the
 * original cleared only C (CLC in the CQ family), this code clears the
 * flags the way the lift models it.  That is MSR CPSR_f,#0, which
 * clears all of them.
 *
 * The quirks kept on purpose, each with its line:
 *   - SPTSTN skips spaces before anything else.  So a line-number
 *     constant after spaces is still found (Lexical.s:19-23).
 *   - SPGETN decodes the three-byte line-number constant with shifts
 *     and exclusive-ors.  The middle byte lands in R1, as well as the
 *     whole number in R0 (:24-33).  The evaluator reads both.
 *   - DONE accepts ' ', ':', 13 and TELSE.  Anything else is a syntax
 *     error and does not return (:68-76).  Every statement ends with
 *     this check.
 *   - OSSTRI terminates the accumulator's string with a bare CR and
 *     hands back a pointer to it (:78-85).
 *   - The word characters of the CQ family are _, ` and the letters
 *     and digits (:86-104).  Nothing between "Z" and "_" qualifies.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "lexical_scan.h"

/* The lift's error paths, called as the original's branches do. */
void basicvfp_ERSYNT(struct ros_cpu *s);
void basicvfp_ERTYPESTR(struct ros_cpu *s);

/* Tokens, as the lift defines them from hdr/Tokens. */
#define TCONST 141u
#define TELSE 139u

#define T_STRING 0u

/* Every original here ends with `MOV PC,R14`, or pops into pc.  The
 * lift models this as R[15] = R[14], and its callers may check it. */
static void ret(struct ros_cpu *s)
{
    s->r[15] = s->r[14];
}

/* ---- skipping spaces ------------------------------------------------ */

/* SPACES (Lexical.s:34): move LINE over its spaces.  R10 is left
 * holding the first other character, and LINE points one past it.
 * The flags are those of the loop's last compare, which is the one
 * that failed. */
void basicvfp_hand_SPACES(struct ros_cpu *s, uint32_t *p10, uint32_t *p12,
                          uint32_t r14)
{
    uint32_t line = *p12, ch;
    do {
        ch = ros_ld8(line);
        line++;
    } while (ch == 32);
    *p10 = ch;
    *p12 = line;
    ros_subs(s, ch, 32);                    /* CMP R10,#" " */
    s->r[15] = r14;
}

/* AESPAC (Lexical.s:39): SPACES over AELINE. */
void basicvfp_hand_AESPAC(struct ros_cpu *s)
{
    uint32_t line = s->r[11], ch;
    do {
        ch = ros_ld8(line);
        line++;
    } while (ch == 32);
    s->r[10] = ch;
    s->r[11] = line;
    ros_subs(s, ch, 32);
    ret(s);
}

/* ---- statement ends -------------------------------------------------- */

/* DONE (Lexical.s:69): R10 must be a statement end, which is ':', 13
 * or TELSE.  Any ' ' is first consumed by SPACES.  Anything else is a
 * syntax error. */
void basicvfp_hand_DONE(struct ros_cpu *s)
{
    uint32_t ch = s->r[10];
    if (ch == 32)
        return basicvfp_hand_DONES(s);      /* BEQ DONES */
    ros_subs(s, ch, 58);                    /* CMP R10,#":" */
    if (ch != 58)
        ros_subs(s, ch, 13);                /* CMPNE R10,#13 */
    if (ch == 58 || ch == 13) {
        ret(s);
        return;
    }
    ros_subs(s, ch, TELSE);                 /* CMP R10,#TELSE */
    if (ch == TELSE) {
        ret(s);
        return;
    }
    s->r[15] = s->r[14];                    /* the tail call's shape */
    basicvfp_ERSYNT(s);                     /* B ERSYNT: no return */
}

/* DONES (Lexical.s:68): the next character, then DONE. */
void basicvfp_hand_DONES(struct ros_cpu *s)
{
    s->r[10] = ros_ld8(s->r[12]);
    s->r[12] += 1;
    basicvfp_hand_DONE(s);
}

/* AEDONE (Lexical.s:65): LINE := AELINE, then DONE.  AEDONES (:67)
 * sets LINE the same way but falls into DONES, so it consumes the next
 * character first. */
void basicvfp_hand_AEDONE(struct ros_cpu *s)
{
    s->r[12] = s->r[11];
    basicvfp_hand_DONE(s);
}

void basicvfp_hand_AEDONES(struct ros_cpu *s)
{
    s->r[12] = s->r[11];
    basicvfp_hand_DONES(s);
}

/* ---- line constants --------------------------------------------------- */

/* SPTSTN (Lexical.s:19): skip spaces, then look at the character.  If
 * it is not TCONST, return it with the flags of its compare against
 * TCONST.  If it is TCONST, decode the line number that follows into
 * R0 (and R1). */
void basicvfp_hand_SPTSTN(struct ros_cpu *s)
{
    uint32_t line = s->r[12], ch;
    do {
        ch = ros_ld8(line);
        line++;
    } while (ch == 32);
    s->r[10] = ch;
    s->r[12] = line;
    ros_subs(s, ch, TCONST);                /* CMP R10,#TCONST */
    if (ch != TCONST) {
        ret(s);
        return;
    }
    basicvfp_hand_SPGETN(s);                /* the fall into SPGETN */
}

/* SPGETN (Lexical.s:24): the three bytes after TCONST hold the line
 * number, coded roughly in base 64.  The original shifts and
 * exclusive-ors them into place, and so does this.  No flags are
 * set. */
void basicvfp_hand_SPGETN(struct ros_cpu *s)
{
    uint32_t line = s->r[12];
    uint32_t x = ros_ld8(line) << 2;
    uint32_t r1 = (x & 0xC0u) ^ ros_ld8(line + 1);
    uint32_t r10 = ros_ld8(line + 2);
    uint32_t r0 = r1 | (((r10 ^ (x << 2)) & 0xFFu) << 8);
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[10] = r10;
    s->r[12] = line + 3;
    ret(s);
}

/* ---- strings for the OS ---------------------------------------------- */

/* OSSTRI (Lexical.s:78): return the accumulator's string in R1 as an
 * OS string, CR-terminated and counted.  A non-string is a type
 * error. */
void basicvfp_hand_OSSTRI(struct ros_cpu *s)
{
    ros_logic(s, s->r[9], s->c);            /* TEQ TYPE,#0 */
    if (s->r[9] != T_STRING) {
        ret(s);
        basicvfp_ERTYPESTR(s);              /* B ERTYPESTR: no return */
    }
    ros_st8(s->r[2], 13);                   /* STRB #13,[CLEN],#1 */
    s->r[2] += 1;
    s->r[1] = s->r[8] - 0x600u;             /* ADD R1,ARGP,#STRACC */
    s->r[0] = 13;
    ret(s);
}

/* ---- the character classes -------------------------------------------- */

/* WORDCQ (Lexical.s:86): C set if R0 may be part of a name.  That is
 * '_', '`', the letters and the digits.  The digits are tested by
 * falling into NUMBCP.  Each exit returns the flags of its own last
 * compare. */
void basicvfp_hand_WORDCQ(struct ros_cpu *s)
{
    uint32_t c = s->r[0];
    if (c > 122) {                          /* CMP R0,#"z" ; BHI clear */
        ros_msr_f(s, 0);
        ret(s);
        return;
    }
    ros_subs(s, c, 95);                     /* CMP R0,#"_" */
    if (c >= 95) {
        ret(s);
        return;
    }
    if (c > 90) {                           /* CMP R0,#"Z" ; BHI clear */
        ros_msr_f(s, 0);
        ret(s);
        return;
    }
    ros_subs(s, c, 65);                     /* CMP R0,#"A" */
    if (c >= 65) {
        ret(s);
        return;
    }
    basicvfp_hand_NUMBCP(s);                /* the fall into NUMBCP */
}

/* NUMBCP (Lexical.s:94): C set if R0 is a digit. */
void basicvfp_hand_NUMBCP(struct ros_cpu *s)
{
    uint32_t c = s->r[0];
    if (c > 57) {                           /* CMP R0,#"9" ; BHI clear */
        ros_msr_f(s, 0);
        ret(s);
        return;
    }
    ros_subs(s, c, 48);                     /* CMP R0,#"0" */
    ret(s);
}

/* NUMBCQ (Lexical.s:98): C set if R0 is a digit or a '.'. */
void basicvfp_hand_NUMBCQ(struct ros_cpu *s)
{
    uint32_t c = s->r[0];
    ros_subs(s, c, 46);                     /* CMP R0,#"." */
    if (c != 46)
        basicvfp_hand_NUMBCP(s);            /* BNE NUMBCP */
    else
        ret(s);
}

/* ---- the line-constant encoder's tail --------------------------------- */

/* CONSTI (Lexical.s:105): write the constant in SMODE into DEST as
 * three bytes, and move DEST past them.  CONSTA returns the token
 * byte.  Two of the bytes each hold six bits of the number ORed with
 * &40.  The other mixes the top two bits of each half and is tagged
 * with &54.  No flags are set. */
void basicvfp_hand_CONSTI(struct ros_cpu *s, uint32_t *p2, uint32_t *p4,
                          uint32_t *p5, uint32_t r14)
{
    uint32_t dest = *p2, smode = *p5, consta;
    ros_st8(dest + 2, ((smode >> 8) & 0x3Fu) | 0x40u);
    ros_st8(dest + 1, (smode & 0x3Fu) | 0x40u);
    smode &= 0xC0u;
    consta = (((( *p5 >> 12) & 0xCu) | (smode >> 2)) ^ 0x54u);
    ros_st8(dest, consta);
    *p2 = dest + 3;
    *p4 = consta;
    *p5 = smode;
    s->r[15] = r14;
}
