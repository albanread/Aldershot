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
 * (Sources/Programmer/BASIC: s.fp2).
 */

/* factor_const.c: BASIC's numeric constant codecs, translated by hand
 * from fp2.s (RISC OS 5.31's BASIC, the VFP build).  There are two:
 *   - FREAD, the reader that FACTOR's TSTN and the VAL family call to
 *     turn the text at AELINE into a number;
 *   - FCONFP, the converter that STR$ and PRINT's format machinery
 *     call to turn the float accumulator back into text.
 *
 * FREAD (fp2.s:766-1055, the FPOINT=2 build) is two machines.  The
 * fast path accumulates decimal digits in IACC and returns an integer.
 * It does so only while IACC stays below &0CC00000.  The first '.',
 * 'E' or overflow abandons it.  Reading then restarts from the first
 * digit, remembered in R3, in the packed-decimal machine.  That builds
 * the value as 18 BCD nibbles and a 3-digit decimal exponent in twelve
 * bytes on the stack.  Then it makes the one FP instruction that the
 * old code needed, LDFP F0.  This is the FPA packed-decimal load, here
 * the runtime's ros_fpa_ldp_at.  It moves the resulting double into
 * the VFP accumulator FACC (D0) by STFD and two FLDS.  The exit
 * contract is in the condition codes.  Carry set means "a number was
 * read".  SUBS TYPE,TYPE,#0 leaves N set for a float (TFP) and clear
 * for an integer.
 *
 * FCONFP (fp2.s:360-571) goes the other way.  FLOATY gives it the
 * accumulator as a double, and STFP packs that into decimal on the
 * stack.  The FPRT machinery then rounds the BCD mantissa to FDIGS
 * significant figures.  It prints the result into STRACC with the
 * decimal point taken from the format word.  It appends the exponent
 * unless the format has no use for one.  The R5 argument selects the
 * hex conversion FCONHX (fp2.s:744-761), which gives STR~'s eight
 * digits with leading zeros suppressed.
 *
 * These quirks are kept.  Each is given with its line:
 *   - A number is only an integer below &0CC00000 (fp2.s:897).  The
 *     compare runs after each digit is folded in.  So 213909504 is
 *     read as a float by the packed-decimal machine, but 213909503 is
 *     an integer.
 *   - The terminator that ends a short exponent is left in R10 with
 *     "0" already subtracted (the SUBLS chain, fp2.s:995-1007 and
 *     1013-1021).  So "1.5E2)" returns with AELINE at the ')' but the
 *     lookahead holding ')'-48.  At this depth the original's
 *     lookahead contract is on AELINE and not on R10.
 *   - A second '.' ends the number (FRDFPDOT's BNE, fp2.s:973-974).
 *   - Leading zeros after a leading '.' step the exponent down
 *     (SUBEQ R5,R5,IACC, fp2.s:923).  Every digit after the first that
 *     comes before the '.' steps it up (FRDFPEXPINC, fp2.s:941-945).
 *   - The mantissa stores at most 18 digits (CMP R3,#3*32,
 *     fp2.s:930-931).  Further digits still count in the exponent, but
 *     their values are lost.
 *   - The E exponent is added to the exponent implied by the decimal
 *     point.  A negative total is what flips the sign of the packed
 *     exponent (ADDS/RSBMI/EORMI, fp2.s:1027-1029).
 *   - "Nothing read" clears the carry (MOVS TYPE,#TINTEGER,
 *     fp2.s:763).  R10 holds the character less "0" and AELINE is not
 *     moved.  The lift leaves IACC undefined there.  This translation
 *     writes 0, and the error paths that follow never read it.
 *   - FCONFP clamps a digit count above 18 to 18.  A count of 0 means
 *     18, except in the fixed format 2 (fp2.s:370-378).  MAXDIGS is 18
 *     in every build but FPOINT=0 (fp2.s:348-353).
 *   - In the fixed format, the exponent is added to the digit count
 *     before that clamp.  The carry from CMP FMAT,#2 is folded into
 *     the ADCS (fp2.s:425-427).
 *   - Rounding that carries past the 18th digit appends a 1, steps the
 *     exponent, and restarts the whole rounding pass (fp2.s:456-461).
 *     So FPRTF is the head of a loop and not straight-line code.
 *   - Zero prints "0" and returns at once in the general format only
 *     (fp2.s:411-416).  In the other formats it goes round the digit
 *     loop like any other value (FPRTZR, fp2.s:479-484).
 *   - A negative exponent of -1 or -2 prints as "0.0" or "0.00".  This
 *     comes from the store at TYPE,#1 before the prefix, which the
 *     source remarks on with "interestingly!" (fp2.s:501-507).
 *   - Trailing zeros are stripped in the general format only (FPRTTZ,
 *     fp2.s:529-533).  The exponent's tens digit prints whenever the
 *     hundreds digit did (the ORRNE LR,LR,#256 flag, fp2.s:549-550).
 *     In the formats other than general, alignment spaces follow a
 *     short exponent (fp2.s:560-568).
 *   - R14 comes back as the last scratch value in LR and not as the
 *     caller's address.  The return address stays on the stack all
 *     along (LDMFD SP!,{FDIGS,PC}, fp2.s:571), exactly as the lift
 *     models it.
 *   - FCONHX suppresses leading zeros but always prints the last digit
 *     (fp2.s:752-758).  It calls INTEGY first, so STR~ of a float
 *     truncates (fp2.s:746).
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "factor_const.h"

/* FLOATY and INTEGY are hand code in basic_conv.c (unit 4a).  The
 * chains call each other directly, with R14 holding the marker return
 * address. */
#include "basic_conv.h"

/* The types, as Definitions.hdr spells them. */
#define T_INTEGER 0x40000000u
#define T_FLOAT 0x80000000u

/* MAXDIGS is 18 in every build but FPOINT=0 (fp2.s:348-353). */
#define FC_MAXDIGS 18u

/* A return address that nobody checks, as unit 0's eval() uses. */
#define FC_RETURN_MARKER 0xFFFFFFF0u

/* FREAD (fp2.s:766-1055): read the constant at AELINE.  The lifted
 * signature is kept exactly.  The registers that the lifter merged
 * into pointer parameters are written back at every return.  On exit
 * the carry says whether anything was read, and N is set for a float
 * (TFP) and clear for an integer.  AELINE points at the terminator and
 * R10 holds it.  When a short exponent ended the number, R10 has "0"
 * subtracted, as the quirk above describes. */
void basicvfp_hand_FREAD(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                         uint32_t *p3, uint32_t *p5, uint32_t *p7,
                         uint32_t *p9, uint32_t *p10, uint32_t *p11,
                         uint32_t r12, uint32_t *p13, uint32_t r14)
{
    uint32_t r0 = 0, r1 = *p1, r3, r5 = *p5, r7 = *p7, r9,
             r10 = *p10, r11 = *p11, r13 = *p13;

    /* The integer fast path (fp2.s:866-902). */
    r3 = r11 - 1;                           /* SUB R3,AELINE,#1 ; first digit */
    if (r10 == 46)                          /* CMP R10,#"." */
        goto FRDDOT;                        /* BEQ ; starts with a . */
    r10 -= 48;                              /* SUB R10,R10,#"0" */
    ros_subs(s, r10, 9);                    /* CMP R10,#9 */
    if (r10 > 9)
        goto FRDDXX;                        /* BHI ; if <0 or >9 */
    r0 = r10;                               /* MOV IACC,R10 */
    r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    r1 = r10 - 48;                          /* SUBS R1,R10,#"0" */
    if (r10 < 48)
        goto FRDDDP;                        /* BCC FRDDDP ; could be . */
    if (r1 > 9)
        goto FRDDD;                         /* BHI FRDDD */
    r0 = r1 + ((r0 + (r0 << 2)) << 1);      /* mult by 10 and add next char */
FRDDC:
    r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
    r11 += 1;
FRDDDP:
    if (r10 == 46)                          /* CMP R10,#"." */
        goto FRDDOT;                        /* BEQ FRDDOT */
FRDDD:
    if (r10 == 69)                          /* CMP R10,#"E" ; FRDINTEX, merged */
        goto FRDDOT;                        /* into FRDDOT by the lift */
    r10 -= 48;                              /* SUB R10,R10,#"0" */
    ros_subs(s, r10, 9);                    /* CMP R10,#9 */
    if (r10 > 9) {
FRDINTQ:                                    /* the integer exit */
        r11 -= 1;                           /* SUB AELINE,AELINE,#1 */
        r9 = ros_subs(s, T_INTEGER, 0);     /* MOV TYPE,#TINTEGER; SUBS ,#0 */
        *p0 = r0; *p1 = r1; *p3 = r3; *p5 = r5; *p7 = r7; *p9 = r9;
        *p10 = r10; *p11 = r11; *p13 = r13; /* MOV PC,R14 */
        s->r[15] = r14;
        return;
    }
    r0 = r10 + ((r0 + (r0 << 2)) << 1);     /* mult by 10 and add next char */
    if (r0 < 0xCC00000u)                    /* CMP IACC,#&0CC00000 */
        goto FRDDC;                         /* BCC */
    /* B FRDRANGE, which is folded into FRDDOT (fp2.s:904). */

    /* The packed-decimal machine (fp2.s:904-1055). */
FRDDOT:
    r11 = r3;                               /* MOV AELINE,R3 ; from the top */
    r0 = 0;                                 /* MOV IACC,#0 ; the dot-seen flag */
    r5 = 0;                                 /* MOV R5,#0 ; the decimal exponent */
    r13 -= 12;                              /* Push "IACC,R3,R5" ; the packed value */
    ros_st32(r13, 0);
    ros_st32(r13 + 4, 0);
    ros_st32(r13 + 8, 0);
    r3 = 8;                                 /* MOV R3,#8 ; the bit index */
FRDFP0:                                     /* skip leading zeros (fp2.s:921) */
    r10 = ros_ld8(r11) - 48;                /* LDRB R10,[AELINE],#1; SUBS */
    r11 += 1;
    if (r10 == 0) {                         /* BEQ FRDFP0 */
        r5 -= r0;                           /* SUBEQ R5,R5,IACC ; zeros after . */
        goto FRDFP0;
    }
    if (r10 == 0xFFFFFFFEu)                 /* CMP R10,#"."-"0" */
        goto FRDFPDOT;                      /* BEQ */
FRDFPDIG:                                   /* the digit store (fp2.s:928) */
    if (r10 > 9)
        goto FRDFPNOTDIG;                   /* BHI */
    if (r3 < 96) {                          /* CMP R3,#3*32; BHS FRDFPEXPINC */
        r7 = ros_ld8(r13 + (r3 >> 3))       /* LDRB R7,[SP,R3,LSR #3] */
                  | ros_lsl(r10, r3 & 4);   /* ORR R7,R7,R10,LSL TYPE */
        ros_st8(r13 + (r3 >> 3), r7);       /* STRB R7,[SP,R3,LSR #3] */
        r3 = (r3 & 0x1Fu) == 0 ? r3 + 60    /* TST R3,#31; SUB; ADDEQ ,#64 */
                               : r3 - 4;
    }
FRDFPEXPINC:                                /* (fp2.s:941) */
    if (r3 != 4 && r0 != 1)                 /* CMP R3,#4; CMPNE IACC,#1 */
        r5 += 1;                            /* ADDNE R5,R5,#1 */
FRDFPNEXTDIG:
    r10 = ros_ld8(r11) - 48;                /* LDRB R10,[AELINE],#1; SUB */
    r11 += 1;
    goto FRDFPDIG;                          /* B FRDFPDIG */
FRDFPNOTDIG:                                /* (fp2.s:950) */
    if (r10 == 0xFFFFFFFEu)                 /* '.' again */
        goto FRDFPDOT;
    if (r10 == 21)                          /* CMP R10,#"E"-"0" */
        goto FRDFPEXP;
    r7 = r5;                                /* MOVS R7,R5 */
    if (r5 != 0) {                          /* LDRNE R3,[SP] */
        r3 = ros_ld32(r13);
        if ((int32_t)r5 < 0) {              /* RSBMI/EORMI: a negative */
            r7 = -r5;                       /* implicit exponent flips */
            r3 ^= 0x40000000u;              /* the packed sign (fp2.s:957) */
        }
        goto FRDFPEXPBIN1;                  /* BNE ; set the exponent */
    }
    /* fall through: no exponent to fold in */
FRDFPDONE:                                  /* (fp2.s:960-971) */
    s->fp->f[0] = ros_fpa_ldp_at(r13, r10, r11, r12); /* LDFP F0,[SP],#12 */
    ros_fpa_std(r13 + 4, s->fp->f[0]);      /* STFD F0,[SP,#-8]! */
    s->fp->vfp.s[1] = ros_lds(r13 + 4);     /* FLDS S1,[SP],#4 */
    s->fp->vfp.s[0] = ros_lds(r13 + 8);     /* FLDS S0,[SP],#4 */
    r13 += 12;
    r11 -= 1;                               /* SUB AELINE,AELINE,#1 */
    r9 = ros_subs(s, T_FLOAT, 0);           /* MOV TYPE,#TFP; SUBS ,#0 */
    *p0 = r0; *p1 = r1; *p3 = r3; *p5 = r5; *p7 = r7; *p9 = r9;
    *p10 = r10; *p11 = r11; *p13 = r13;     /* MOV PC,R14 */
    s->r[15] = r14;
    return;
FRDFPDOT:                                   /* (fp2.s:972-978) */
    if (r0 != 0)                            /* CMP IACC,#0 */
        goto FRDFPDONE;                     /* BNE : a second . ends it */
    r0 = 1;                                 /* MOV IACC,#1 */
    if (r3 == 8) {                          /* CMP R3,#8 : nothing stored yet */
        r5 = 0xFFFFFFFFu;                   /* MOVEQ R5,#-1 */
        goto FRDFP0;                        /* BEQ ; leading zeros after . */
    }
    goto FRDFPNEXTDIG;                      /* B */
FRDFPEXP:                                   /* the E exponent (fp2.s:980-1007) */
    r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 ; the sign */
    r11 += 1;
    r3 = ros_ld32(r13);                     /* LDR R3,[SP] */
    if (r10 == 45) {                        /* CMP R10,#"-" */
        r5 = -r5;                           /* RSBEQ R5,R5,#0 */
        r3 ^= 0x40000000u;                  /* EOREQ : flip exponent sign */
    }
    if (r10 == 45 || r10 == 43) {           /* CMPNE R10,#"+" ; LDREQB */
        r10 = ros_ld8(r11);
        r11 += 1;
    }
    r7 = r10 - 48;                          /* SUB R7,R10,#"0" */
    if (r5 != 0)                            /* CMP R5,#0 */
        goto FRDFPEXPBIN;                   /* BNE */
    /* no correction to apply: read the exponent as BCD (fp2.s:991) */
    if (r7 > 9)                             /* CMP R7,#9 */
        goto FRDFPDONE;                     /* BHI */
    r10 = ros_ld8(r11);                     /* LDRLSB : the second char */
    r11 += 1;
    r10 -= 48;                              /* SUBLS */
    if (r10 <= 9) {                         /* CMPLS/ORRLS */
        r7 = r10 | (r7 << 4);
        r10 = ros_ld8(r11);                 /* LDRLSB : the third char */
        r11 += 1;
        r10 -= 48;                          /* SUBLS */
        if (r10 <= 9) {                     /* CMPLS/ORRLS/ADDLS */
            r7 = r10 | (r7 << 4);
            r11 += 1;                       /* pays for DONE's un-read */
        }
    }
    r3 |= r7 << 12;                         /* ORR R3,R3,R7,LSL #12 */
    ros_st32(r13, r3);                      /* STR R3,[SP] */
    goto FRDFPDONE;
FRDFPEXPBIN:                                /* (fp2.s:1008-1030) */
    if (r7 > 9)                             /* CMP R7,#9 */
        r7 = 0;                             /* MOVHI : not a digit */
    else {
        r10 = ros_ld8(r11);                 /* LDRLSB : the second char */
        r11 += 1;
        r10 -= 48;                          /* SUBLS */
        if (r10 <= 9) {                     /* CMPLS/ADDLS pair */
            r7 = r10 + ((r7 + (r7 << 2)) << 1);    /* *10 and add */
            r10 = ros_ld8(r11);             /* LDRLSB : the third char */
            r11 += 1;
            r10 -= 48;                      /* SUBLS */
            if (r10 <= 9) {
                r7 = r10 + ((r7 + (r7 << 2)) << 1);
                r11 += 1;                   /* ADDLS : pays for DONE */
            }
        }
    }
    {
        uint32_t total = r7 + r5;           /* ADDS R7,R7,R5 */
        r7 = (int32_t)total < 0 ? -total : total;  /* RSBMI */
        if ((int32_t)total < 0)             /* EORMI : flip the packed sign */
            r3 ^= 0x40000000u;
    }
    /* the exponent to BCD, the noddy way (fp2.s:1031-1042) */
    if (r7 >= 1000) {                       /* CMP R7,#1000 */
        r7 -= 1000;
        r3 += 0x1000000u;                   /* ADDHS R3,R3,#1:SHL:24 */
    }
FRDFPEXPBIN1:
    while (r7 >= 100) {                     /* CMP R7,#100; BHS loop */
        r7 -= 100;
        r3 += 0x100000u;                    /* ADDHS ,#1:SHL:20 */
    }
    while (r7 >= 10) {                      /* CMP R7,#10; BHS loop */
        r7 -= 10;
        r3 += 0x10000u;                     /* ADDHS ,#1:SHL:16 */
    }
    r3 += r7 << 12;                         /* ADD R3,R3,R7,LSL #12 */
    ros_st32(r13, r3);                      /* STR R3,[SP] */
    goto FRDFPDONE;
FRDDXX:                                     /* nothing read (fp2.s:763) */
    r9 = ros_logic(s, T_INTEGER, 0);        /* MOVS TYPE,#TINTEGER ; C=0 */
    *p0 = r0; *p1 = r1; *p3 = r3; *p5 = r5; *p7 = r7; *p9 = r9;
    *p10 = r10; *p11 = r11; *p13 = r13;     /* MOV PC,R14 */
    s->r[15] = r14;
    return;
}

/* FCONFP (fp2.s:360-571): the accumulator to text in STRACC.  R4 is
 * the format word from INTVAR, with the digit count in bits 8-15 and
 * the choice of point character in bit 23.  R5 nonzero selects FCONHX,
 * the hex form.  On return TYPE points one past the last byte
 * written. */
void basicvfp_hand_FCONFP(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r13 = s->r[13], r14 = s->r[14];
    uint32_t v1, v4, v6, v17;

    if (r5 != 0) {                          /* TEQ R5,#0 */
        /* FCONHX (fp2.s:744-761): the hex conversion, STR~'s. */
        r13 -= 4;                           /* STR R14,[SP,#-4]! */
        ros_st32(r13, r14);
        s->r[13] = r13;
        s->r[14] = FC_RETURN_MARKER;        /* BL INTEGY */
        basicvfp_hand_INTEGY(s);
        r0 = s->r[0]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
        r7 = s->r[7]; r8 = s->r[8]; r13 = s->r[13]; r14 = s->r[14];
        r9 = r8 - 1536;                     /* ADD TYPE,ARGP,#STRACC */
        r2 = 28;                            /* MOV R2,#32-4 */
        r3 = 0;                             /* MOV R3,#0 ; lzb */
        do {
            r1 = ros_lsr(r0, r2) & 0xFu;    /* MOV R1,IACC,LSR R2; AND ,#15 */
            ros_subs(s, r1, 9);             /* CMP R1,#9 */
            r1 = r1 <= 9 ? r1 | 0x30u       /* ORRLS ,#"0" */
                         : r1 + 55;         /* ADDHI ,#"A"-10 */
            v17 = r2 == 0 ? 1 : r3;         /* TEQ R2,#0 ; last time? */
            r3 = v17;                       /* MOVEQ R3,#1 */
            if (r3 != 0 || r1 != 48)        /* TEQ R3,#0; CMPEQ R1,#"0" */
                r3 = 1;                     /* MOVNE R3,#1 */
            if (v17 != 0 || r1 != 48) {     /* STRNEB R1,[TYPE],#1 */
                ros_st8(r9, r1);
                r9 += 1;
            }
            r2 = ros_subs(s, r2, 4);        /* SUBS R2,R2,#4 */
        } while ((int32_t)r2 >= 0);         /* BPL FCONH1 */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[9] = r9;
        s->r[15] = ros_ld32(r13);           /* LDR PC,[SP],#4 */
        s->r[13] = r13 + 4;
        return;
    }

    /* The format word (fp2.s:362-378). */
    v1 = r4;                                /* INTVAR, saved for the point */
    r5 = (r4 >> 16) & 0x7Fu;                /* MOV/AND FMAT,R4 */
    r4 = (r4 >> 8) & 0xFFu;                 /* MOV/AND FDIGS,R4 */
    r4 = (v1 & 0x800000u) == 0 ? r4 | 0x2E000000u   /* ORREQ ,#&2e000000 "." */
                               : r4 | 0x2C000000u;  /* ORRNE ,#&2c000000 "," */
    if (r5 >= 3)                            /* CMP FMAT,#3 */
        r5 = 0;                             /* MOVCS FMAT,#0 */
    r7 = r4 & 0xFFu;                        /* AND R7,FDIGS,#255 */
    ros_subs(s, r7, FC_MAXDIGS + 1);        /* CMP R7,#MAXDIGS+1 */
    if (r7 >= FC_MAXDIGS + 1)               /* BICCS/ORRCS */
        r4 = (r4 & ~0xFFu) | FC_MAXDIGS;
    if ((r4 & 0xFFu) == 0 && (r5 ^ 2) != 0) /* TST FDIGS,#255; TEQ FMAT,#2 */
        r4 |= FC_MAXDIGS;                   /* ORRNE : 0 digits means max */

    /* FCONA (fp2.s:379): the double out, packed decimal on the stack. */
    r13 -= 8;                               /* STMFD SP!,{FDIGS,R14} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r14);
    s->r[4] = r4; s->r[5] = r5; s->r[7] = r7; s->r[13] = r13;
    s->r[14] = FC_RETURN_MARKER;             /* BL FLOATY */
    basicvfp_hand_FLOATY(s);
    r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r8 = s->r[8]; r13 = s->r[13];
    ros_sts(r13 - 4, s->fp->vfp.s[0]);      /* FSTS S0,[SP,#-4]! */
    ros_sts(r13 - 8, s->fp->vfp.s[1]);      /* FSTS S1,[SP,#-4]! */
    s->fp->f[0] = ros_fpa_ldd(r13 - 8);     /* LDFD F0,[SP],#8 */
    ros_fpa_stp(r13 - 12, s->fp->f[0]);     /* STFP F0,[SP,#-12]! */
    r0 = ros_ld32(r13 - 12);                /* LDMFD SP!,{R0,R1,R2} */
    r1 = ros_ld32(r13 - 8);
    r2 = ros_ld32(r13 - 4);
    r9 = r8 - 1536;                         /* ADD TYPE,ARGP,#STRACC */
    if ((int32_t)r0 < 0) {                  /* TEQ R0,#0 ; MOVMI R6,#"-" */
        r6 = 45;
        ros_st8(r9, r6);                    /* STRMIB R6,[TYPE],#1 */
        r9 = r8 - 1535;
    }
    /* the packed exponent, BCD to binary (fp2.s:399-409) */
    r7 = r0 & 0xF00000u;                    /* AND FPRTDX,R0,#&F:SHL:20 */
    r7 = (r0 & 0xF0000u) + ((r7 + (r7 >> 2)) >> 1);
    r14 = r0 & 0xF000u;                     /* AND LR,R0,#&F:SHL:12 */
    r7 = (r14 + ((r7 + (r7 >> 2)) >> 1)) >> 12;
    if ((r0 & 0x40000000u) != 0)            /* TST R0,#1:SHL:30 */
        r7 = -r7;                           /* RSBNE FPRTDX,FPRTDX,#0 */
    r0 = ros_logic(s, r0 & 0xFFFu,          /* MOVS R0,R0,LSR #20 : the top */
                   (r0 << 20 >> 19) & 1);   /* BCD digit, flags kept */
    v4 = r2 | r1;                           /* ORREQS LR,R2,R1 */
    if (r0 == 0)
        r14 = v4;
    if (r0 != 0 || v4 != 0) {               /* BNE FPRTA */
        for (;;) {
FPRTF:                                      /* the rounding pass (fp2.s:421) */
            r4 = ros_ld32(r13);             /* LDR FDIGS,[SP] */
            ros_subs(s, r5, 2);             /* CMP FMAT,#2 */
            if (r5 == 2) {                  /* BNE FPRTFH */
                r6 = ros_adcs(s, r4 & 0xFFu, r7);  /* ADCS R6,R6,FPRTDX */
                if (s->n)
                    goto FPRTZR;            /* BMI */
                if (r6 >= FC_MAXDIGS + 1)   /* CMP R6,#MAXDIGS+1 */
                    r5 = 0;                 /* MOVCS FMAT,#0 */
                r4 = (r4 & ~0xFFu)          /* BIC/ORR FDIGS */
                      | (r6 >= FC_MAXDIGS + 1 ? FC_MAXDIGS : r6);
            }
            /* FPRTFH: round to FDIGS figures (fp2.s:434-465).  The
             * three registers are pushed below SP, which re-creates
             * the packed value there.  The loop rounds it in place. */
            ros_st32(r13 - 4, r0);          /* Push "r0" */
            ros_st32(r13 - 8, r1);          /* Push "r1" */
            r13 -= 12;                      /* Push "r2" */
            ros_st32(r13, r2);
            r1 = 72 - ((r4 & 0xFFu) << 2);  /* RSB R1,R1,#18*4 */
            r2 = 0;                         /* MOV R2,#0 */
            for (;;) {
                r0 = ros_ror(ros_ld8(r13 + (r2 >> 3)), 4);  /* LDRB/MOV ROR #4 */
                if ((r2 & 4) != 0)          /* TST R2,#4 */
                    r0 = ros_ror(r0, 4);
                ros_subs(s, r1, r2);        /* CMP R1,R2 */
                if (r1 < r2) {              /* BLO %FT15 */
                    ros_subs(s, r0, 0x90000000u);    /* CMP R0,#9:SHL:28 */
                    r0 = r0 >= 0x90000000u  /* BICCS/ADDCC */
                          ? r0 & 0xFFFFFFFu : r0 + 0x10000000u;
                } else {
                    if (r1 == r2)
                        ros_subs(s, r0, 0x50000000u); /* CMPEQ R0,#5:SHL:28 */
                    r0 &= 0xFFFFFFFu;       /* BIC R0,R0,#&F:SHL:28 */
                }
                r0 = ros_ror(r0, 28);       /* MOV R0,R0,ROR #28 */
                v6 = r2;                    /* TST R2,#4 */
                ros_st8(r13 + (r2 >> 3),    /* STRB R0,[SP,R2,LSR #3] */
                        (r2 & 4) != 0 ? ros_ror(r0, 28) : r0);
                if (!s->c) {                /* BCC %FT30 */
                    r2 = ros_ld32(r13);     /* Pull "r2"/"r1"/"r0" */
                    r1 = ros_ld32(r13 + 4);
                    r0 = ros_ld32(r13 + 8);
                    r13 += 12;
                    if ((r4 & 0xFFu) != 0)  /* TST FDIGS,#255 */
                        goto FPRTH;         /* BNE */
                    goto FPRTZR;            /* B */
                }
                if (r2 != 72)               /* CMP R2,#18*4; ADDNE */
                    r2 += 4;
                if (v6 == 72) {
                    /* ran out of digits (fp2.s:456-461) */
                    ros_st8(r13 + (r2 >> 3), 1);      /* MOV R0,#1; STRB */
                    r7 += 1;                /* ADD FPRTDX,FPRTDX,#1 */
                    r2 = ros_ld32(r13);     /* Pull and round again */
                    r1 = ros_ld32(r13 + 4);
                    r0 = ros_ld32(r13 + 8);
                    r13 += 12;
                    goto FPRTF;             /* B FPRTF */
                }
            }
        }
    } else {
        /* the number is zero (fp2.s:411-420) */
        ros_logic(s, r5, s->c);             /* TEQ FMAT,#0 */
        if (r5 == 0) {
            r6 = 48;                        /* MOVEQ R6,#"0" */
            ros_st8(r9, r6);                /* STREQB R6,[TYPE],#1 */
            r9 += 1;
            s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[7] = r7;
            s->r[9] = r9; s->r[14] = r14;
            s->r[6] = ros_ld32(r13);        /* LDMEQFD SP!,{R6,PC} */
            s->r[15] = ros_ld32(r13 + 4);
            s->r[13] = r13 + 8;
            return;
        }
        if (r5 == 1)                        /* TEQ FMAT,#1 */
            goto FPRTH;                     /* BEQ */
        goto FPRTZR;                        /* B */
    }

FPRTZR:                                     /* zero in the other formats */
    r4 = ros_ld32(r13) + 1;                 /* LDR FDIGS,[SP]; ADD ,#1 */
    r0 = 0;                                 /* MOV R0,#0 */
    r1 = 0;                                 /* MOV R1,#0 */
    r2 = 0;                                 /* MOV R2,#0 */
    r7 = 0;                                 /* MOV FPRTDX,#0 */
FPRTH:                                      /* the digit loop (fp2.s:485) */
    r6 = 1;                                 /* MOV FPRTWN,#1 */
    if (r5 != 1) {                          /* TEQ FMAT,#1; BEQ FPRTK */
        if ((int32_t)r7 < 0) {              /* BMI FPRTKK */
            if (r5 != 2                     /* TEQ FMAT,#2; BEQ FPRTKL */
                && r7 != 0xFFFFFFFFu        /* CMN FPRTDX,#1 */
                && r7 != 0xFFFFFFFEu)       /* CMNNE FPRTDX,#2 */
                goto FPRTK;                 /* BNE */
            /* FPRTKL: "0." and zeros to the left (fp2.s:501-507) */
            ros_st8(r9 + 1, r4 >> 24);      /* STRB FPRTWN,[TYPE,#1] */
            r6 = 48;                        /* MOV FPRTWN,#"0" */
            ros_st8(r9, 48);                /* STRB FPRTWN,[TYPE],#2 */
            r9 += 2;
            do {                            /* FPRTKM */
                r7 += 1;                    /* ADDS FPRTDX,FPRTDX,#1 */
                if (r7 != 0) {              /* STRNEB FPRTWN,[TYPE],#1 */
                    ros_st8(r9, r6);
                    r9 += 1;
                }
            } while (r7 != 0);              /* BNE FPRTKM */
            r6 = 128;                       /* MOV FPRTWN,#&80 */
        } else {
            r14 = r4 & 0xFFu;               /* AND LR,FDIGS,#255 */
            if (r7 < r14) {                 /* CMP FPRTDX,LR; BCS FPRTK */
                r6 = r7 + 1;                /* ADD FPRTWN,FPRTDX,#1 */
                r7 = 0;                     /* MOV FPRTDX,#0 */
            }
        }
    }
    do {
FPRTK:                                      /* one digit per turn (fp2.s:509) */
        r14 = (r0 >> 8) | 0x30u;            /* MOV LR,R0,LSR #8; ORR ,#"0" */
        ros_st8(r9, r14);                   /* STRB LR,[TYPE],#1 */
        r0 = ((r0 << 4) | (r1 >> 28))       /* the shift-ins, with the */
            & ~0xF000u;                     /* BIC R0,R0,#&F000 */
        r1 = (r1 << 4) | (r2 >> 28);        /* ORR R1,R1,R2,LSR #28 */
        r2 <<= 4;                           /* MOV R2,R2,LSL #4 */
        r6 = ros_subs(s, r6, 1);            /* SUBS FPRTWN,FPRTWN,#1 */
        if (r6 == 0) {                      /* MOVEQ LR,FDIGS,LSR #24 */
            r14 = r4 >> 24;
            ros_st8(r9 + 1, r14);           /* STREQB LR,[TYPE],#1 */
            r9 += 2;                        /* past the point */
        } else {
            r9 += 1;
        }
        r4 -= 1;                            /* SUB FDIGS,FDIGS,#1 */
    } while ((r4 & 0xFFu) != 0);            /* TST FDIGS,#255; BNE FPRTK */
    if (r5 != 1) {                          /* TEQ FMAT,#1; BEQ FPRTTX */
        if (r5 != 2) {                      /* TEQ FMAT,#2; BEQ FPRTTY */
            do {                            /* FPRTTZ: strip trailing zeros */
                r9 -= 1;                    /* LDRB LR,[TYPE,#-1]! */
                r14 = ros_ld8(r9);
            } while (r14 == 48);            /* CMP LR,#"0"; BEQ */
            ros_subs(s, r14, r4 >> 24);     /* CMP LR,FDIGS,LSR #24 */
            if (!s->z)                      /* ADDNE TYPE,TYPE,#1 */
                r9 += 1;
        }
FPRTTY:
        ros_logic(s, r7, s->c);             /* TEQ FPRTDX,#0 */
        if (r7 == 0)                        /* BEQ FPRTX */
            goto FPRTX;
    }
FPRTTX:                                     /* the exponent (fp2.s:536-559) */
    ros_st8(r9, 69);                        /* MOV LR,#"E"; STRB LR,[TYPE],#1 */
    if ((int32_t)r7 < 0) {                  /* ADDS FPRTDX,FPRTDX,#0 */
        ros_st8(r9 + 1, 45);                /* MOVMI LR,#"-"; STRMIB */
        r9 += 2;
    } else {
        r9 += 1;
    }
    r6 = (int32_t)r7 < 0 ? -r7 : r7;        /* RSBMI FPRTWN,FPRTWN,#0 */
    r14 = 48;                               /* MOV LR,#"0" */
    {
        uint32_t v13;
        do {                                /* IPRTB: the hundreds */
            v13 = r6;
            r6 = ros_subs(s, r6, 100);      /* SUBS FPRTWN,FPRTWN,#100 */
            if (v13 >= 100)                 /* ADDCS LR,LR,#1 */
                r14 += 1;
        } while (v13 >= 100);               /* BCS IPRTB */
    }
    if ((r14 ^ 0x30u) != 0) {               /* TEQ LR,#"0"; STRNEB */
        ros_st8(r9, r14);
        r9 += 1;
    }
    r6 += 100;                              /* ADD FPRTWN,FPRTWN,#100 */
    r14 = (r14 ^ 0x30u) != 0 ? 304 : 48;    /* MOV LR,#"0"; ORRNE ,#256 */
    do {                                    /* IPRTA: the tens */
        r6 = ros_subs(s, r6, 10);           /* SUBS FPRTWN,FPRTWN,#10 */
        if (s->c)                           /* ADDCS LR,LR,#1 */
            r14 += 1;
    } while (s->c);                         /* BCS IPRTA */
    if ((r14 ^ 0x30u) != 0) {               /* TEQ LR,#"0"; STRNEB */
        ros_st8(r9, r14);
        r9 += 1;
    }
    r6 += 58;                               /* ADD FPRTWN,FPRTWN,#"0"+10 */
    ros_st8(r9, r6);                        /* STRB FPRTWN,[TYPE],#1 */
    r9 += 1;
    ros_logic(s, r5, s->c);                 /* TEQ FMAT,#0 */
    if (r5 != 0) {                          /* BEQ FPRTX */
        r3 = 32;                            /* MOV R3,#" " */
        if ((int32_t)r7 >= 0) {             /* STRPLB R3,[TYPE],#1 */
            ros_st8(r9, 32);
            r9 += 1;
        }
        if ((r14 & 0x100u) == 0) {          /* TST LR,#256; STREQB */
            ros_st8(r9, 32);
            r9 += 1;
        }
        ros_logic(s, r14 ^ 0x30u, s->c);    /* TEQ LR,#"0" */
        if (s->z) {                         /* STREQB R3,[TYPE],#1 */
            ros_st8(r9, 32);
            r9 += 1;
        }
    }
FPRTX:                                      /* (fp2.s:571) */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[14] = r14;
    s->r[4] = ros_ld32(r13);                /* LDMFD SP!,{FDIGS,PC} */
    s->r[15] = ros_ld32(r13 + 4);
    s->r[13] = r13 + 8;
    return;
}
