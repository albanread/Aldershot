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
 * (Sources/Programmer/BASIC: s.ErrorMsgs, s.Factor, s.fp2, hdr.Definitions).
 */

/* factor_core.c: FACTOR's constant readers, translated by hand from
 * Factor.s (RISC OS 5.31's BASIC, the VFP build).  They are:
 *   - TSTN/TSTNNOTCACHE, the decimal reader with its line cache
 *     (Factor.s:526-570).  These are the eleven dispatch cases '.' and
 *     '0'-'9' of FACTOR's jump table (Factor.s:31-92).
 *   - HEXIN, the '&' reader (Factor.s:577-602, case 38).
 *   - BININ, the '%' reader (Factor.s:604-615, case 37).
 *   - UNMINS, unary '-' (Factor.s:401-407 with VALCMP at :405-412,
 *     case 45).
 *
 * These are dispatch cases of FACTOR's switch, so each hand function
 * has the rig's one-argument shape.  The substituted case stores every
 * live local into R[] and then tail-calls.  R[14] is the live return
 * address, exactly what the original's BL prologue would have pushed.
 * This is substitute.py's CASE_SUBS contract.  The call is musttail, so
 * the C return carries the finished evaluator straight back to
 * FACTOR's caller.  Each body keeps the ARM border protocol:
 *   - the state is in R[];
 *   - the arena stack is kept word for word (STR/LDR R14);
 *   - the flags are returned as the factors promise (Z set means a
 *     string, N means a float);
 *   - FACTOR's own exit contract holds: the terminating lookahead
 *     character is left unread, with AELINE stepped back onto it.
 *
 * These quirks are kept.  Each is given with its line:
 *   - TSTN's cache probe reads the top bit of the delta as the float
 *     tag (CMN R1,#1, Factor.s:532-534).  A hit unpacks the two cached
 *     words as the double by FMDRR (IACC low, TYPE high, :541).  Then
 *     MOVS TYPE,#TFP leaves N set, as the exit contract needs.  It also
 *     leaves C set, which is the carry of the rotated immediate
 *     &80000000 (Factor.s:545).
 *   - The integer cache hit returns with the flags that CMN left
 *     (MOVPL PC,R14, :536).  Z would need a delta of exactly -1.  A real
 *     constant has a length of at least 1 and cannot produce that, so
 *     Z stays clear.
 *   - TSTNNOTCACHE fills the cache slot as {IACC, end-start (+TFP if a
 *     float), start, TYPE} (Factor.s:559-563).  For a float the TYPE
 *     slot holds the HIGH word of the double (FMRRDMI, :555).  The
 *     entry pushed is the AELINE AFTER the dispatch has consumed the
 *     first digit.  The delta counts from there, so it is one digit
 *     short on both the fill and the hit, and the two cancel.
 *   - FREAD's carry out is the "did read a number" flag (BCC FACERR,
 *     Factor.s:551).  So "&" followed by nothing is an error from
 *     FACERR and not from HEXIN.
 *   - HEXIN's overflow test runs BEFORE the shift (TST IACC,
 *     #&F0000000, Factor.s:595-596).  Exactly eight digits fit.  The
 *     ninth gives ERHEX2 "Hex number too large".  The offending digit
 *     has been consumed and AELINE is not stepped back.  HEXEND's SUB
 *     AELINE,AELINE,#1 at :600 is the push-back of the valid exit only.
 *   - HEXIN's exit hands R10 back in whatever half-reduced form the
 *     rejecting compare left it (Factor.s:579-593).  It is the raw
 *     character below '0', char-"A"+10 in the middle ranges, and
 *     char-"a"+10 for lower case.  The caller re-reads the lookahead
 *     from AELINE, so nothing reads it, but this translation keeps the
 *     value exactly.
 *   - BININ's digit value rides the CARRY out of CMP R10,#"1" into
 *     ADCEQ IACC,IACC,IACC (Factor.s:607-609), giving IACC*2+C.  Unlike
 *     HEXIN there is NO overflow test, so a 33rd binary digit wraps
 *     silently.
 *   - BININ's TYPE doubles as the validity flag (MOVEQ TYPE,#TINTEGER
 *     "thus making sure of final cc state", Factor.s:608).  The exit's
 *     TEQ TYPE,#0 (:613) rejects '%' when no 0 or 1 follows, and its Z
 *     is the exit's own Z.
 *   - UNMINS tests the operand's type through the Z that FACTOR's exit
 *     left (BEQ ERTYPEINT, Factor.s:404).  This is the factors' flag
 *     contract.  So -"string" gives "Type mismatch: number needed".
 *     The negation is done by VALCMP (Factor.s:405-412), the join that
 *     the VAL family shares.  An integer is negated in IACC, a float is
 *     negated by FNEGD, and the return address is popped from the one
 *     word that UNMINS pushed.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "factor_core.h"

/* ---- the lift's functions this unit calls --------------------------
 * The manifest's EXPORTS make them non-static in the twin.  The ones
 * new with this unit are VALCMP and FREAD (see the report).  FREAD is
 * the box-build lift's plain (struct ros_cpu *) wrapper around the
 * merged reader.  The harness lift of 26 Sep still has the
 * 12-parameter form, and this call does not typecheck against that at
 * the border. */
void basicvfp_FACTOR(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_FACERR(struct ros_cpu *s);
void basicvfp_VALCMP(struct ros_cpu *s);
void basicvfp_FREAD(struct ros_cpu *s);

/* MSG is hand code (basic_organs.c). */
void basicvfp_hand_MSG(struct ros_cpu *s);

/* Type bits and the cache geometry (hdr/Definitions, as the lift
 * spells them). */
#define TFP     0x80000000u
#define TINTEGER 0x40000000u
#define CACHEMASK  255u
#define CACHESHIFT 4

/* The real addresses of the MSG error sites.  MSG reads its error
 * number and token from the words after these.  They are the lift's own
 * BL sites at the ERHEX, ERHEX2 and ERBIN labels (ErrorMsgs.s:594-610):
 * 28,79 "Bad Hex", 28,80 "Hex number too large" and 28,81 "Bad
 * Binary". */
#define SITE_ERHEX  0xFC110094u
#define SITE_ERHEX2 0xFC11009Cu
#define SITE_ERBIN  0xFC1100A4u

/* BL FREAD's own site in TSTNNOTCACHE.  This is the lift's marker
 * address, kept so that a trace shows the real call site. */
#define SITE_FREAD  0xFC103754u

/* BL FACTOR's site inside the lift's own UNMINS label.  The original
 * falls from that BL into VALCMP with R14 still holding this address.
 * VALCMP's code, and anything it calls that transfers through R14,
 * reads the LIVE R14.  So the hand tail must present exactly this
 * value and not a marker that nobody checks.  Otherwise Parsing_RP's
 * unary minus fails with "Call to &FFFFFFF0". */
#define SITE_UNMINS_FACTOR 0xFC10355Cu

/* A return address that nobody checks, like the BL markers of the
 * landed units. */
#define FKC_RETURN_MARKER 0xFFFFFFF0u

/* The error exits.  Hand the block to MSG, which never returns.  The
 * fault catches a MSG that does return. */
static void fkc_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* TSTNNOTCACHE, reached only from the TSTN probe's miss (below). */
static void fkc_tstnnotcache(struct ros_cpu *s);

/* TSTN (Factor.s:526-546): the eleven cases '.' and '0'-'9'.  Probe
 * the line cache entry {IACC, delta, LINE, TYPE} at
 * ARGP+(AELINE&CACHEMASK)<<4.  A hit returns without touching the
 * arena stack.  A miss runs TSTNNOTCACHE below, which reads the number
 * and fills the entry. */
void basicvfp_hand_TSTN(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r4 = s->r[4], r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r14 = s->r[14];

    r1 = r8 + ((r11 & CACHEMASK) << CACHESHIFT); /* AND R1,AELINE,#CACHEMASK; ADD */
    r0 = ros_ld32(r1);                          /* LDMIA R1,{IACC,R1,R4,TYPE} */
    r4 = ros_ld32(r1 + 8);
    r9 = ros_ld32(r1 + 12);
    r1 = ros_ld32(r1 + 4);
    if (r4 != r11) {                            /* CMP R4,AELINE ; BNE TSTNNOTCACHE */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[9] = r9;
        s->r[10] = r10; s->r[11] = r11;
        fkc_tstnnotcache(s);
        return;
    }
    ros_adds(s, r1, 1);                         /* CMN R1,#1 */
    r11 += r1;                                  /* ADD AELINE,AELINE,R1 */
    if (!s->n) {                                /* MOVPL PC,R14 */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[9] = r9;
        s->r[10] = r10; s->r[11] = r11;
        s->r[15] = r14;
        return;
    }
    r11 -= TFP;                                 /* SUB AELINE,AELINE,#TFP */
    s->fp->vfp.dw[0] = ((uint64_t)r9 << 32) | r0; /* FMDRR FACC,IACC,TYPE */
    r9 = ros_logic(s, TFP, 1);                  /* MOVS TYPE,#TFP ; C=1: the rotated immediate's carry */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[9] = r9;
    s->r[10] = r10; s->r[11] = r11;             /* MOV PC,R14 */
    s->r[15] = r14;
    return;
}

/* TSTNNOTCACHE (Factor.s:548-570): there is no usable cache entry.
 * Read the number with the lift's FREAD (fp2.s:766-1055).
 * units/factor_const.c holds a hand copy, which is HELD and not wired
 * in.  Carry set means a number was read.  Then fill the slot for the
 * next pass.  It is entered with TSTN's probe results already in R[],
 * because the branch carries every live local. */
static void fkc_tstnnotcache(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r3 = s->r[3], r4 = s->r[4],
             r5 = s->r[5], r6, r7 = s->r[7], r8 = s->r[8], r9 = s->r[9],
             r10 = s->r[10], r11 = s->r[11], r13 = s->r[13],
             r14 = s->r[14];
    int mi;

    r13 -= 8;                                   /* STMFD SP!,{AELINE,R14} */
    ros_st32(r13, r11);
    ros_st32(r13 + 4, r14);
    s->r[0] = r0; s->r[1] = r1; s->r[3] = r3;   /* BL FREAD */
    s->r[5] = r5; s->r[7] = r7; s->r[9] = r9;
    s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    s->r[14] = SITE_FREAD;
    basicvfp_FREAD(s);
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r5 = s->r[5]; r7 = s->r[7];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r14 = s->r[14];
    if (!s->c) {                                /* BCC FACERR */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[7] = r7; s->r[9] = r9; s->r[10] = r10;
        s->r[11] = r11; s->r[13] = r13; s->r[14] = r14;
        s->r[15] = r14;
        basicvfp_FACERR(s);
        return;
    }
    r6 = ros_ld32(r13);                         /* LDMFD SP!,{R6,R14} */
    r14 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    mi = (int32_t)r9 < 0;                       /* MI: a float (TFP) */
    if (mi)
        r0 = (uint32_t)s->fp->vfp.dw[0];        /* FMRRDMI IACC,TYPE,FACC */
    r5 = r8 + ((r6 & CACHEMASK) << CACHESHIFT); /* AND R5,R6,#CACHEMASK; ADD */
    r4 = r11 - r6;                              /* SUB R4,AELINE,R6 */
    if (mi)
        r4 -= TFP;                              /* ADDMI R4,R4,#TFP */
    ros_st32(r5, r0);                           /* STMIA R5,{IACC,R4,R6,TYPE} */
    ros_st32(r5 + 4, r4);
    ros_st32(r5 + 8, r6);
    ros_st32(r5 + 12, mi ? (uint32_t)(s->fp->vfp.dw[0] >> 32) : r9);
    if (mi)
        r9 = TFP;                               /* MOVMI TYPE,#TFP */
    s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[10] = r10;
    s->r[11] = r11; s->r[13] = r13; s->r[14] = r14;
    s->r[15] = r14;                             /* MOV PC,R14 */
    return;
}

/* HEXIN (Factor.s:577-602): '&' numbers.  IACC accumulates one nibble
 * per digit (ORR R10,IACC,LSL #4).  The overflow test comes before the
 * shift, so the ninth digit gives ERHEX2.  TYPE doubles as the validity
 * flag and as the register that sets the exit's flags. */
void basicvfp_hand_HEXIN(struct ros_cpu *s)
{
    uint32_t r0, r4 = s->r[4], r9, r10, r11 = s->r[11], r14 = s->r[14];

    r0 = 0;                                     /* MOV IACC,#0 */
    r9 = 0;                                     /* MOV TYPE,#0 ; invalid hex */
HEXIP:
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, 48);                       /* CMP R10,#"0" */
    if (r10 < 48)
        goto HEXEND;                            /* BCC HEXEND */
    ros_subs(s, r10, 57);                       /* CMP R10,#"9" */
    if (r10 <= 57)
        goto HEXOK;                             /* BLS HEXOK */
    r10 -= 55;                                  /* SUB R10,R10,#"A"-10 */
    ros_subs(s, r10, 10);                       /* CMP R10,#10 */
    if (r10 < 10)
        goto HEXEND;                            /* BCC HEXEND */
    ros_subs(s, r10, 16);                       /* CMP R10,#16 */
    if (r10 < 16)
        goto HEXOK;                             /* BCC HEXOK */
    r10 -= 32;                                  /* SUB R10,R10,#"a"-"A" */
    ros_subs(s, r10, 10);                       /* CMP R10,#10 */
    if (r10 < 10)
        goto HEXEND;                            /* BCC HEXEND */
    ros_subs(s, r10, 16);                       /* CMP R10,#16 */
    if (r10 >= 16)
        goto HEXEND;                            /* BCS HEXEND */
HEXOK:
    r10 &= 0xFu;                                /* AND R10,R10,#&F */
    ros_logic(s, r0 & 0xF0000000u, 1);          /* TST IACC,#&F0000000 */
    if (!s->z) {                                /* BNE ERHEX2 */
        s->r[0] = r0; s->r[4] = r4; s->r[9] = r9; s->r[10] = r10;
        s->r[11] = r11;
        fkc_msg_at(s, SITE_ERHEX2);
    }
    r0 = r10 | (r0 << 4);                       /* ORR IACC,R10,IACC,LSL #4 */
    r9 = TINTEGER;                              /* MOV TYPE,#TINTEGER ; final cc state */
    goto HEXIP;                                 /* B HEXIP */
HEXEND:
    r11 -= 1;                                   /* SUB AELINE,AELINE,#1 */
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 ; test TYPE for validity */
    if (r9 != 0) {                              /* MOVNE PC,R14 */
        s->r[0] = r0; s->r[4] = r4; s->r[9] = r9; s->r[10] = r10;
        s->r[11] = r11;
        s->r[15] = r14;
        return;
    }
    s->r[0] = r0; s->r[4] = r4; s->r[9] = r9;   /* B ERHEX */
    s->r[10] = r10; s->r[11] = r11;
    fkc_msg_at(s, SITE_ERHEX);
}

/* BININ (Factor.s:604-615): '%' numbers.  The digit's value rides the
 * carry out of CMP R10,#"1" into ADCEQ IACC,IACC,IACC.  There is no
 * overflow test, so a 33rd digit wraps silently.  TYPE doubles as the
 * validity flag and as the register that sets the exit's flags. */
void basicvfp_hand_BININ(struct ros_cpu *s)
{
    uint32_t r0, r4 = s->r[4], r9, r10, r11 = s->r[11], r14 = s->r[14];

    r0 = 0;                                     /* MOV IACC,#0 */
    r9 = 0;                                     /* MOV TYPE,#0 ; invalid BIN */
BINIP:
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, 49);                       /* CMP R10,#"1" ; C = the digit */
    if (r10 == 49 || (r10 ^ 0x30u) == 0) {      /* TEQNE R10,#"0" ; EQ either way */
        r9 = TINTEGER;                          /* MOVEQ TYPE,#TINTEGER ; final cc state */
        r0 += r0 + s->c;                        /* ADCEQ IACC,IACC,IACC */
        goto BINIP;                             /* BEQ BINIP */
    }
    r11 -= 1;                                   /* SUB AELINE,AELINE,#1 */
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 ; test TYPE for validity */
    if (r9 != 0) {                              /* MOVNE PC,R14 */
        s->r[0] = r0; s->r[4] = r4; s->r[9] = r9; s->r[10] = r10;
        s->r[11] = r11;
        s->r[15] = r14;
        return;
    }
    s->r[0] = r0; s->r[4] = r4; s->r[9] = r9;   /* B ERBIN */
    s->r[10] = r10; s->r[11] = r11;
    fkc_msg_at(s, SITE_ERBIN);
}

/* UNMINS (Factor.s:401-407): unary '-'.  The operand is a full
 * recursive FACTOR.  The BEQ ERTYPEINT reads the Z that the exit left,
 * which is set for a string.  The negation is done by VALCMP
 * (Factor.s:405-412), the join that the VAL family shares.  An integer
 * is negated in IACC, a float is negated by FNEGD, and the return
 * address is popped from the word pushed here.
 *
 * The recursive BL FACTOR.  PERFORMANCE PHASE (P1): this used to push a
 * setjmp'd resume frame at the lift's site.  That was because the
 * operand can be an FN call, whose GTARGS unwinder resumed non-locally
 * through R14.  That return now rides the C stack (FNRET/GTARGS, and
 * see expr_factor_rest.c's EFR_RESUME for the full argument), so the
 * frame is gone.  The helper stays noinline.  The call must not be
 * inlined into UNMINS, whose VALCMP continuation is a tail call. */
__attribute__((noinline)) static void fkc_unmins_factor(struct ros_cpu *s)
{
    s->r[14] = SITE_UNMINS_FACTOR;
    basicvfp_FACTOR(s);
    ros_check_return(s, SITE_UNMINS_FACTOR);
}

void basicvfp_hand_UNMINS(struct ros_cpu *s)
{
    uint32_t r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[4] = r4; s->r[10] = r10;               /* BL FACTOR */
    s->r[11] = r11;
    s->r[13] = r13;
    fkc_unmins_factor(s);                       /* the recursive BL FACTOR */
    /* VALCMP reads the live R[] that the FACTOR call left.  Only R14
     * must be set, to the value the original's BL holds as it falls
     * in. */
    s->r[14] = SITE_UNMINS_FACTOR;              /* R14 as the BL leaves it */
    if (s->z) {                                 /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    basicvfp_VALCMP(s);                         /* fall into VALCMP */
    return;
}
