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
 * (Sources/Programmer/BASIC: s.Stmt, s.Stmt2, hdr.Definitions, hdr.Tokens).
 */

/* dispat_io.c: the I/O and graphics statements of the statement
 * executor, translated by hand from the RISC OS 5.31 BASIC VFP ObjAsm
 * source.  The unit covers:
 *   - PRINT (Stmt.s:1134-1201), the print-item loop, with the PRCOMM
 *     field-padding remainder division, ENDPRI/PRFUNY/CONTPR and
 *     PRINTA.  PRINT# (Stmt2.s:1891-1965).
 *   - VDU with VDUL/VDUP (Stmt2.s:1645-1666).
 *   - INPUT with the INPLP/INPLO/INPHP prompt loop and
 *     INGET/INGOT/INGETB/INTERM/INGETC (Stmt.s:722-802).  INPUT#
 *     (Stmt2.s:1783-1889), with ENDOFFILE/INPHNO/INPHNF/INPHSS, the
 *     5-byte and 8-byte float reads and FCONVERT2.
 *   - LINEST (Stmt2.s:966-989): LINE INPUT, and the four-point LINE
 *     draw.
 *   - READ (Stmt.s:1273-1295) and RESTORE (Stmt.s:1327-1352), with
 *     RESTOREERROR/RESTOREDATA reading the ON statement's stacked
 *     frame.
 *   - The SOUND family (Stmt.s:1485-1559): BEATS, VOICES, VOICE,
 *     TEMPO, STEREO and SOUND, with SOUNDFOUR/SOUNDOFF/SOUNDON.
 *   - MODES (Stmt2.s:990-1121): all five forms, through the
 *     ModeSelector block, MODESD, the bpp table walk MODESDFINDBPP,
 *     MODESTRING and the NoSuchSWI fallback.
 *   - COLOUR (Stmt2.s:426-597): TINT, PALETTE, the 4- and 5-argument
 *     palette forms PALETTE4/PALETTE5/PROGPAL, the 3-argument COLOUR3
 *     path and the OF/ON loops COLOUROFON*.
 *   - GCOL (Stmt2.s:832-965), which has the same shape, with
 *     GCOLTINT/GCOL2/GCOL3/GCOLOFON*.
 *   - The shared TINTEND (Stmt2.s:1637-1643).
 *   - CLS and CLG (Stmt2.s:387-395), CLEAR (Stmt.s:154-158) and WAIT
 *     (Stmt2.s:1668-1675).
 *   - The PLOT family: MOVE (Stmt2.s:1516), DRAW (:598), FILL (:830),
 *     PLOT (:1536-1546) and the PLOTER/PLOTER1/PLOTER2/PLOTACT web
 *     (:1517-1546).  PSET (:1548-1563, with the TO form's
 *     OsWord_DefinePointerAndMouse).  CIRCLE (:394-411).  ELLIPSE
 *     (:600-829, with the shear-angle sin/cos web).  RECT (:1565-1630,
 *     with RECTSIMPLE/RECTMOVE).  DOTINT (TINT, :1631-1636).
 *
 * In the lift, as in the original, these are labels inside the one
 * DISPAT.  The rig's case substitution calls each function here
 * through the one-argument ROS_TAIL_CALL contract.  The live return
 * address is in R[14], and the case stores every live local.  None of
 * the functions returns.  Every exit tails into NXT, DONEXT, DONXTS or
 * an error, as a plain call whose result is discarded.
 *
 * The conventions are the ones the landed units use:
 *   - The state is held in R[].
 *   - A BL is a marker call, with R14 set to 0xFFFFFFF0u.  The lift's
 *     store sets come before the call and its reload sets after it.
 *   - A B is a plain call whose result is discarded.
 *   - setjmp bodies (EXPR, PRSPEC, FCONFP, CHANNL, CRAELV and so on)
 *     are plain calls with no resume points.  None of this unit's
 *     callees transfers control through R14 non-locally, so the unit
 *     pushes no resume points of its own.
 *
 * The quirks kept, with their lines:
 *   - PRINT's ',' padding computes @%'s field width modulo the print
 *     zone with a shift-and-subtract loop (PRCOML1/PRCOML2,
 *     Stmt.s:1146-1158).  Its "SUBCSS/BEQ" pair exits when the
 *     remainder reaches exactly zero at either of two points.  The
 *     first SUBS leaves the dividend and the second subtracts again.
 *     If either result is zero, the tally already fills the zone, and
 *     R0 prints as 0 spaces.
 *   - PRINT's item loop evaluates the item with the lookahead already
 *     consumed.  AELINE is set to LINE-1 before EXPR, and LINE to
 *     AELINE-1 after it (Stmt.s:1185-1189).  The loop applies @%
 *     (FCONFP) only when the value is not a string.  When @% asked for
 *     more digits than the number produced, it pads the formatted
 *     width with SPCSWC (Stmt.s:1190-1198).
 *   - PRINT# writes numbers high byte first but writes STRINGS low
 *     byte first (PRTHSL walks CLEN downwards, Stmt2.s:1925-1929).  A
 *     float goes out as &88 followed by S1 then S0, which is the
 *     register pair in memory order (PRTHF, Stmt2.s:1936-1963).
 *   - INPUT's prompt machinery runs PRSPEL twice per item.  INPLP
 *     handles a leading string and INPLO handles the ones after it.
 *     The '?' prompt is printed only when the &80 bit survived
 *     (Stmt.s:756-769).  The typed line's CLEN is overwritten with ','
 *     unless the line ended at CR (Stmt.s:783-787).  A numeric l-value
 *     re-reads the item through VALSTR, from the R5 pushed on the
 *     stack (INGETB, Stmt.s:793-800).
 *   - INPUT#'s EOF is a retry loop (ENDOFFILE, BCS back,
 *     Stmt2.s:1802-1805).  The 5-byte float's middle byte carries the
 *     D-format exponent, rebased by +894 (&400-&82,
 *     Stmt2.s:1873-1883).  The top-word TFP tag is forced when any
 *     byte is nonzero.
 *   - SOUND's channel and amplitude are packed by two LSL#16/ORR
 *     pairs, which read the stacked halves in the wrong rotation
 *     order.  The second INTEXC's IACC keeps its low half and takes
 *     the first's high half (Stmt.s:1524-1537).  The 4-argument form
 *     goes out as an OS_Word 7 block in STRACC.  The 5-argument form
 *     uses Sound_QSchedule (SOUNDFOUR, Stmt.s:1547-1552).
 *   - MODES' <bpp> variant walks a byte table of permitted depths
 *     (MODESDFINDBPP, Stmt2.s:1070-1083).  It maps depth index 4
 *     (8bpp) down by one and appends the FullPalette variables
 *     (Stmt2.s:1086-1096).  When XOS_ScreenMode fails with NoSuchSWI,
 *     the error is rewritten as "Bad MODE" (MODESELECTORBLOCK,
 *     Stmt2.s:1097-1114).
 *   - The OF/ON forms of COLOUR and GCOL evaluate their colour lists
 *     onto the stack with a zero terminator.  The emit loops POP in a
 *     different order from the one they pushed in.  DOCOLOUROFON and
 *     DOGCOLOFON unwind one pair per OS_SetColour (Stmt2.s:544-551 and
 *     903-917).  The 3-argument forms share the ON loops' LPN entries.
 *     A magic 'O' (79) or TOF marker decides the exit action
 *     (GCOLOFON3LPN, Stmt2.s:934-945).
 *   - GCOL's 4-bit action keeps only the low nibble for OS_SetColour
 *     (AND R0,R2,#&F, Stmt2.s:911), but the ON form adds &10.
 *   - MOVE, DRAW and FILL share PLOTER's 'B'/'BY' suffix.  'BY'
 *     subtracts 4 from the plot code and consumes both letters.  A
 *     lone 'B' only backs LINE up by one (PLOTER1, Stmt2.s:1518-1525).
 *   - ELLIPSE's shear form computes sin and cos through the
 *     VFPTRANSCENDENTALS table by BLX (Stmt2.s:634-670).  It keeps
 *     every intermediate on the stack in the original's order.  It
 *     applies SFIX twice before the one VFPException check
 *     (Stmt2.s:790-803).  slicew goes through FTOSIZD into S4, and
 *     shearx goes through SFIX's rounding.
 *   - RECT's filled form re-enters PLOTACT, the PLOT web's exit, with
 *     the action word popped from under the three coordinates
 *     (RECTSIMPLE, Stmt2.s:1580-1606).  RECT TO's third point draws
 *     &BD or &BE according to the FILL flag (RECTMOVE,
 *     Stmt2.s:1608-1630).
 *   - RESTORE +n walks the line headers BACKWARDS from LINE-2, looking
 *     for CR, before it counts (RESTOREREL1, Stmt.s:1344-1352).  The
 *     blind 3-byte skip reads the length byte before it checks it.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include <math.h>

#include "dispat_io.h"

/* FMACD and the other fused-looking operations are modelled as two
 * roundings, as the lift expands them.  This stops the compiler from
 * contracting them into one. */
#pragma STDC FP_CONTRACT OFF

/* ---- the lift's functions this unit calls --------------------------
 * The manifest's EXPORTS list makes these non-static in the twin.  The
 * ones not already in EXPORTS are listed in the unit's report. */
void basicvfp_ERSYNT(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_ERTYPESTR(struct ros_cpu *s);
void basicvfp_EXPR(struct ros_cpu *s);
void basicvfp_AEEXDN(struct ros_cpu *s);
void basicvfp_EXPRDN(struct ros_cpu *s);
void basicvfp_CRAELV(struct ros_cpu *s);
void basicvfp_GOFACT(struct ros_cpu *s);
void basicvfp_NOLINE(struct ros_cpu *s);
void basicvfp_CTALLY(struct ros_cpu *s);
void basicvfp_VFPException(struct ros_cpu *s);
void basicvfp_STOREA(struct ros_cpu *s);
void basicvfp_CHANNL(struct ros_cpu *s);
void basicvfp_FCONVERT2(struct ros_cpu *s);
void basicvfp_VALSTR(struct ros_cpu *s);
void basicvfp_DATAIT(struct ros_cpu *s);
void basicvfp_DATAST(struct ros_cpu *s);
void basicvfp_INTEXA(struct ros_cpu *s);
void basicvfp_INTEXC(struct ros_cpu *s);
void basicvfp_CHECKFILL(struct ros_cpu *s, uint32_t *p0, uint32_t r1,
                        uint32_t *p10, uint32_t *p12, uint32_t r13,
                        uint32_t r14);
void basicvfp_DOPLOT(struct ros_cpu *s);
void basicvfp_WRITEG(struct ros_cpu *s);
void basicvfp_ZEROX(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                    uint32_t r10, uint32_t r11, uint32_t r12,
                    uint32_t r13, uint32_t r14);
void basicvfp_NLINE(struct ros_cpu *s);
void basicvfp_FCONFP(struct ros_cpu *s);
void basicvfp_PRSPEC(struct ros_cpu *s);
void basicvfp_PRSPEL(struct ros_cpu *s);
void basicvfp_PRINTS(struct ros_cpu *s);
void basicvfp_SPCSWC(struct ros_cpu *s);

/* The hand units' functions (landed). */
void basicvfp_hand_NXT(struct ros_cpu *s);
void basicvfp_hand_DONEXT(struct ros_cpu *s);
void basicvfp_hand_DONXTS(struct ros_cpu *s);
void basicvfp_hand_DONES(struct ros_cpu *s);
void basicvfp_hand_DONE(struct ros_cpu *s);
void basicvfp_hand_AEDONE(struct ros_cpu *s);
void basicvfp_hand_AESPAC(struct ros_cpu *s);
void basicvfp_hand_SPACES(struct ros_cpu *s, uint32_t *p10, uint32_t *p12,
                          uint32_t r14);
void basicvfp_hand_INTEGY(struct ros_cpu *s);
void basicvfp_hand_INTEGB(struct ros_cpu *s);
void basicvfp_hand_IFLT(struct ros_cpu *s);
void basicvfp_hand_FLOATY(struct ros_cpu *s);
void basicvfp_hand_SFIX(struct ros_cpu *s);
void basicvfp_hand_FPUSH(struct ros_cpu *s);
void basicvfp_hand_FPULL(struct ros_cpu *s);
void basicvfp_hand_STORE(struct ros_cpu *s);
void basicvfp_hand_SETFSA(struct ros_cpu *s);
void basicvfp_hand_INLINE(struct ros_cpu *s);
void basicvfp_hand_MSG(struct ros_cpu *s);

/* The OS thunks.  The harness's api_gen declares them, and each unit
 * declares them again, as the landed units do. */
void ros_thunk_OS_WriteC(struct ros_cpu *s);
void ros_thunk_OS_Word(struct ros_cpu *s);
void ros_thunk_OS_BPut(struct ros_cpu *s);
void ros_thunk_OS_BGet(struct ros_cpu *s);
void ros_thunk_OS_Byte(struct ros_cpu *s);
void ros_thunk_OS_GenerateError(struct ros_cpu *s);
void ros_thunk_OS_CallAVector(struct ros_cpu *s);

/* Tokens, as the lift defines them from hdr/Tokens. */
#define TELSE    139u
#define TERROR   133u
#define TDATA    220u
#define TINPUT   232u
#define TLINE    134u
#define TOF      202u
#define TON      238u
#define TTO      184u
#define TESCSTMT 200u
#define TTINT    156u

/* The type bits and the two ROM tables this unit reads.  These are
 * the lift's own constants, from hdr/Definitions. */
#define TFP        0x80000000u
#define MODEBPPTAB 0xFC107224u

/* The real addresses of the MSG error sites.  MSG reads its error
 * number and token from the words after each one.  They are the lift's
 * own BL sites inside DISPAT's error labels, one for each label this
 * unit reaches. */
#define SITE_ONERRX        0xFC10FE84u
#define SITE_ERRDATASTACK  0xFC11012Cu
#define SITE_ERBADMODE     0xFC110054u
#define SITE_ERBPP         0xFC1101BCu

/* The error exits.  They hand the block to MSG, which never returns.
 * The fault catches a MSG that does return. */
static void dci_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* dci_store_all: the rig's own case shape.  It stores every live
 * local across an internal label boundary, so that the continuation
 * reads exactly the state the original's branch carried in
 * registers. */
static void dci_store_all(struct ros_cpu *s, uint32_t r0, uint32_t r1,
                          uint32_t r2, uint32_t r3, uint32_t r4,
                          uint32_t r5, uint32_t r6, uint32_t r7,
                          uint32_t r8, uint32_t r9, uint32_t r10,
                          uint32_t r11, uint32_t r12, uint32_t r13,
                          uint32_t r14)
{
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[8] = r8; s->r[9] = r9;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = r14;
}

/* The B NXT, B DONEXT and B DONXTS exits, in the plain-call shape the
 * landed units use.  Store the set the lift lists, set R15 from the
 * live link, and return once the callee has taken over. */
#define DCI_TO_NXT(s) do { \
    s->r[15] = s->r[14]; \
    basicvfp_hand_NXT(s); \
    return; \
} while (0)

/* Forward declarations of the shared continuations.  They are defined
 * below, in the order in which the original's labels flow. */
static void dci_printh(struct ros_cpu *s);
static void dci_inplp(struct ros_cpu *s);
static void dci_inputh(struct ros_cpu *s);
static void dci_tintend(struct ros_cpu *s);
static void dci_ploter(struct ros_cpu *s);
static void dci_ploter1(struct ros_cpu *s);
static void dci_ploter2(struct ros_cpu *s);

/* =====================================================================
 * PRINT (Stmt.s:1134-1201): the print-item loop.
 * ===================================================================== */
void basicvfp_hand_PRINT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 35);                       /* CMP R10,#"#" */
    if (r10 == 35) {                            /* BEQ PRINTH */
        dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                      r11, r12, r13, r14);
        dci_printh(s);
        return;
    }
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    goto STRTPR;

PRCOMM:
    r0 = ros_ld8(r8 - 0x100u);                  /* LDRB R0,[ARGP,#INTVAR] */
    if (r0 == 0) goto STRTPR;                   /* BEQ STRTPR */
    r1 = ros_ld32(r8 - 264);                    /* LDR R1,[ARGP,#TALLY] */
    r2 = r1;                                    /* MOVS R2,R1 */
    if (r1 == 0) goto STRTPR;                   /* BEQ STRTPR */
    r3 = r0;                                    /* MOV R3,R0 */
PRCOML1:
    v = r3;                                     /* MOVS R3,R3,LSL #1 */
    r3 <<= 1;
    if ((v & 0x80000000u) == 0 && r3 < r1)      /* CMPCC R3,R1 */
        goto PRCOML1;
    r3 = ros_ror(r3, 1);                        /* MOV R3,R3,ROR #1 */
PRCOML2:
    {   /* SUBS R1,R1,R3, SUBCSS R1,R1,R3, BEQ, ADDCC.  The carry of
         * each subtraction is the unsigned compare of the value BEFORE
         * it.  The two temporaries are exactly the lift's. */
        uint32_t v63 = r1, v64 = r1 - r3;
        r1 = v64;
        if (r1 == 0) goto STRTPR;               /* BEQ STRTPR */
        if (v63 >= r3) r1 -= r3;                /* SUBCSS R1,R1,R3 */
        if (v63 >= r3 ? v64 == r3 : v64 == 0)   /* BEQ STRTPR */
            goto STRTPR;
        if (v63 < r3 || v64 < r3) r1 += r3;     /* ADDCC R1,R1,R3 */
    }
    r3 >>= 1;                                   /* MOV R3,R3,LSR #1 */
    ros_subs(s, r3, r0);                        /* CMP R3,R0 */
    if (r3 >= r0) goto PRCOML2;                 /* BCS PRCOML2 */
    r0 -= r1;                                   /* RSB R0,R1,R0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPCSWC */
    basicvfp_SPCSWC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];

STRTPR:
    r4 = ros_ld8(r8 - 0x100u);                  /* LDRB R4,[ARGP,#INTVAR] */
    r5 = 0;                                     /* MOV R5,#0 */
ENDPRI:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 != 58) ros_subs(s, r10, 13);        /* CMP R10,#":" */
    if (r10 != 58 && r10 != 13)
        ros_subs(s, r10, TELSE);                /* CMPNE R10,#TELSE */
    if (r10 == 58 || r10 == 13 || r10 == TELSE) {
        s->r[0] = r0; s->r[2] = r2; s->r[10] = r10; s->r[11] = r11;
        s->r[12] = r12;
        s->r[14] = 0xFFFFFFF0u;                 /* BL NLINE */
        basicvfp_NLINE(s);
        r0 = s->r[0]; r2 = s->r[2]; r14 = s->r[14];
        s->r[1] = r1; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
        DCI_TO_NXT(s);                          /* B NXT */
    }
    goto CONTPR;

PRFUNY:
    r4 = 0;                                     /* MOV R4,#0 */
    r5 = 0;                                     /* MOV R5,#0 */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == 58 || r10 == 13 || r10 == TELSE) {
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[10] = r10; s->r[11] = r11;
        s->r[12] = r12; s->r[14] = r14;
        DCI_TO_NXT(s);                          /* BEQ NXT */
    }
    /* fall CONTPR */

CONTPR:
    if (r10 == 126) {                           /* CMP R10,#"~" */
        r5 = 1;                                 /* MOVEQ R5,#1 */
        goto ENDPRI;
    }
    if (r10 == 44) goto PRCOMM;                 /* CMP R10,#"," */
    if (r10 == 59) goto PRFUNY;                 /* CMP R10,#";" */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL PRSPEC */
    basicvfp_PRSPEC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) goto ENDPRI;                      /* BEQ ENDPRI */
    r13 -= 8;                                   /* STMFD SP!,{R4,R5} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    r11 = r12 - 1;                              /* SUB AELINE,LINE,#1 */
    s->r[11] = r11; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                         /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    r12 = r11 - 1;                              /* SUB LINE,AELINE,#1 */
    if (r9 == 0) goto PRINTA;                   /* TEQ TYPE,#0 */
    r13 -= 8;                                   /* STMFD SP!,{R4,R5} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    r4 = ros_ld32(r8 - 0x100u);                 /* LDR R4,[ARGP,#INTVAR] */
    s->r[4] = r4; s->r[5] = r5; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL FCONFP */
    basicvfp_FCONFP(s);
    r1 = s->r[1]; r3 = s->r[3]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r2 = r9;                                    /* MOV CLEN,TYPE */
    r4 = ros_ld32(r13);                         /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    r9 -= r8 - 1536;                            /* SUB TYPE,TYPE,R0 */
    r0 = ros_subs(s, r4, r9);                   /* SUBS R0,R4,TYPE */
    if (r4 > r9) {                              /* BLHI SPCSWC */
        s->r[0] = r0; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5;
        s->r[9] = r9; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_SPCSWC(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    }
PRINTA:
    s->r[0] = r0; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[9] = r9;
    s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL PRINTS */
    basicvfp_PRINTS(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    goto ENDPRI;                                /* B ENDPRI */
}

/* PRINT# (Stmt2.s:1891-1965).  CHANNL reads the handle.  Then the
 * items go out high byte first.  Strings go out low byte first, and
 * floats as &88 followed by S1,S0. */
static void dci_printh(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r11 = r12;                                  /* MOV AELINE,LINE */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CHANNL */
    basicvfp_CHANNL(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[14] = 0xFFFFFFF0u;                     /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10]; r11 = s->r[11]; r14 = s->r[14];
PRTHLP:
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44) goto PRTHEX;                 /* BNE PRTHEX */
    s->r[2] = r2; s->r[3] = r3; s->r[9] = r9; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = r0;                                    /* MOV R4,IACC */
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if ((int32_t)r9 < 0) goto PRTHF;            /* BMI PRTHF */
    r0 = r9 >> 24;                              /* MOV R0,TYPE,LSR #24 */
    r1 = ros_ld32(r13);                         /* LDR R1,[SP] */
    s->r[0] = r0; s->r[1] = r1;                 /* SWI OS_BPut */
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    if (r9 == 0) goto PRTHS;                    /* BEQ PRTHS */
    r0 = r4 >> 24;                              /* MOV R0,R4,LSR #24 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = r4 >> 16;                              /* MOV R0,R4,LSR #16 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = r4 >> 8;                               /* MOV R0,R4,LSR #8 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = r4;                                    /* MOV R0,R4 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    goto PRTHLP;                                /* B PRTHLP */

PRTHS:
    r3 = r8 - 1536;                             /* ADD R3,ARGP,#STRACC */
    r0 = r2 - r3;                               /* SUB R0,CLEN,R3 */
    s->r[0] = r0;                               /* SWI OS_BPut */
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_logic(s, r0, s->c);                     /* TEQ R0,#0 */
    if (r0 == 0) goto PRTHLP;                   /* BEQ PRTHLP */
PRTHSL:
    r2 -= 1;                                    /* LDRB R0,[CLEN,#-1]! */
    r0 = ros_ld8(r2);
    s->r[0] = r0;                               /* SWI OS_BPut */
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_logic(s, r2 ^ r3, s->c);                /* TEQ CLEN,R3 */
    if (!s->z) goto PRTHSL;                     /* BNE PRTHSL */
    goto PRTHLP;                                /* B PRTHLP */

PRTHF:
    r0 = 136;                                   /* MOV R0,#&88 */
    r1 = ros_ld32(r13);                         /* LDR R1,[SP] */
    s->r[0] = r0; s->r[1] = r1;                 /* SWI OS_BPut */
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r9 = r8 - 1536;                             /* ADD TYPE,ARGP,#STRACC */
    ros_sts(r9, s->fp->vfp.s[1]);               /* FSTS S1,[TYPE] */
    ros_sts(r9 + 4, s->fp->vfp.s[0]);           /* FSTS S0,[TYPE,#4] */
    r2 = r9 + 8;                                /* ADD CLEN,TYPE,#8 */
    r1 = ros_ld32(r13);                         /* LDR R1,[SP] */
PRTHFL:
    r0 = ros_ld8(r9);                           /* LDRB R0,[TYPE],#1 */
    r9 += 1;
    s->r[0] = r0; s->r[1] = r1;                 /* SWI OS_BPut */
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_logic(s, r9 ^ r2, s->c);                /* TEQ TYPE,CLEN */
    if (!s->z) goto PRTHFL;                     /* BNE PRTHFL */
    goto PRTHLP;                                /* B PRTHLP */

PRTHEX:
    r12 = r11;                                  /* PRTHEX: MOV LINE,AELINE */
    r13 += 4;                                   /* INPHEX: ADD SP,SP,#4 */
    s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[9] = r9;
    s->r[10] = r10; s->r[12] = r12; s->r[13] = r13; s->r[14] = r14;
    s->r[15] = s->r[14];                        /* B DONEXT */
    basicvfp_hand_DONEXT(s);
    return;
}

/* =====================================================================
 * VDU (Stmt2.s:1645-1666) with the VDUL loop and VDUP's high byte.
 * ===================================================================== */
void basicvfp_hand_VDU(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

VDU:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
VDUL:
    if (r10 == 58 || r10 == 13 || r10 == TELSE) { /* CMP/CR/TELSE */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12; s->r[14] = r14;
        DCI_TO_NXT(s);                          /* BEQ NXT */
    }
    r11 = r12 - 1;                              /* SUB AELINE,LINE,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r12 = r11;                                  /* MOV LINE,AELINE */
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    if (r10 == 44) goto VDU;                    /* CMP R10,#"," */
    ros_subs(s, r10, 59);                       /* CMP R10,#";" */
    if (r10 == 59) goto VDUP;                   /* BEQ VDUP */
    ros_subs(s, r10, 124);                      /* CMP R10,#"|" */
    if (r10 != 124) goto VDUL;                  /* BNE VDUL */
    r1 = 9;                                     /* MOV R1,#9 */
    basicvfp_ZEROX(s, &r0, &r1, r10, r11, r12, r13, r14); /* BL ZEROX */
    goto VDU;                                   /* B VDU */

VDUP:
    r0 >>= 8;                                   /* MOV IACC,IACC,LSR #8 */
    s->r[0] = r0;                               /* SWI OS_WriteC */
    ros_native_swi(s, ros_thunk_OS_WriteC);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    goto VDU;                                   /* fall into VDU */
}

/* =====================================================================
 * INPUT (Stmt.s:722-802), INPUT# (Stmt2.s:1783-1889), and the shared
 * INPLP loop.  LINEST's LINE INPUT enters INPLP too.
 * ===================================================================== */
void basicvfp_hand_INPUT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 35);                       /* CMP R10,#"#" */
    if (r10 == 35) {                            /* BEQ INPUTH */
        dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                      r11, r12, r13, r14);
        dci_inputh(s);
        return;
    }
    r5 = 0;                                     /* MOV R5,#0 */
    r4 = r10 != TLINE ? 0 : 64;                 /* TLINE: MOV R4,#&40 */
    if (r10 != TLINE) r12 -= 1;                 /* SUBNE LINE,LINE,#1 */
    dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                  r11, r12, r13, r14);
    dci_inplp(s);                               /* fall into INPLP */
    return;
}

static void dci_inplp(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

INPLP:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL PRSPEL */
    basicvfp_PRSPEL(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (!s->z) {                                /* ORRNE R4,R4,#&80 */
        r4 |= 128;
        goto INPHP;
    }
INPLO:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL PRSPEL */
    basicvfp_PRSPEL(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) goto INPLO;                       /* BEQ INPLO */
    r5 = 0;                                     /* MOV R5,#0 */
    r4 &= ~0x80u;                               /* BIC R4,R4,#&80 */
INPHP:
    if (r10 == 44 || r10 == 59) goto INPLP;     /* CMP ","/";" */
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    r13 -= 8;                                   /* STMFD SP!,{R4,R5} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    s->r[4] = r4; s->r[5] = r5; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CRAELV */
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                         /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    if (s->z) {                                 /* BEQ DONXTS */
        s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_DONXTS(s);
        return;
    }
    r12 = r11;                                  /* MOV LINE,AELINE */
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    v = r4 & 0x40u;                             /* TST R4,#&40 */
    if (v != 0) goto INGET;
    if (r5 != 0) goto INGOT;                    /* TEQ R5,#0 */

INGET:
    ros_logic(s, r4 & 0x80u, s->c);             /* TST R4,#&80 */
    if (!s->z) {                                /* SWINE OS_WriteI+"?" */
        s->r[12] = r12;
        s->r[13] = r13;
        ros_swi(s, 0x13Fu);
    }
    s->r[4] = r4; s->r[5] = r5; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INLINE */
    basicvfp_hand_INLINE(s);
    r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r2 = r1 - 1;                                /* SUB CLEN,R1,#1 */
INLEN:
    r2 += 1;                                    /* LDRB R0,[CLEN,#1]! */
    r0 = ros_ld8(r2);
    if (r0 != 13) goto INLEN;                   /* CMP R0,#13 */
    r4 &= ~0x80u;                               /* BIC R4,R4,#&80 */
    v = r4 & 0x40u;                             /* TST R4,#&40 */
    if (v != 0) goto INGETB;                    /* BNE INGETB */
    r5 = r8 - 1536;                             /* ADD R5,ARGP,#STRACC */
INGOT:
    r11 = r5;                                   /* MOV AELINE,R5 */
    s->r[0] = r0; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DATAST */
    basicvfp_DATAST(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    if (ros_ld8(r2) != 13) ros_st8(r2, 44);     /* STRNEB ",",[CLEN] */
INTERM:
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == 44) goto INGETC;                 /* CMP R10,#"," */
    if (r10 != 13) goto INTERM;                 /* CMP R10,#13 */
    r11 = 0;                                    /* MOV AELINE,#0 */
INGETC:
    r5 = r11;                                   /* MOV R5,AELINE */
INGETB:
    v = ros_ld32(r13 + 4);                      /* LDMFD SP!,{IACC,TYPE} */
    r0 = ros_ld32(r13);
    r9 = v;
    ros_st32(r13, r4);                          /* STMFD SP!,{R4,R5} */
    ros_st32(r13 + 4, r5);
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    if (r9 >= 128) r9 = 0;                      /* MOVCS TYPE,#0 */
    if (v < 128) {                              /* BLCC VALSTR */
        s->r[0] = r0; s->r[5] = r5; s->r[9] = r9; s->r[10] = r10;
        s->r[11] = r11; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_VALSTR(s);
        r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r7 = s->r[7];
        r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
        r14 = s->r[14];
    }
    s->r[0] = r0; s->r[2] = r2; s->r[9] = r9; s->r[10] = r10;
    s->r[11] = r11; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL STORE */
    basicvfp_hand_STORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                         /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    goto INPLP;                                 /* B INPLP */
}

/* INPUT# (Stmt2.s:1783-1889). */
static void dci_inputh(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    r11 = r12;                                  /* MOV AELINE,LINE */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CHANNL */
    basicvfp_CHANNL(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
INPHLP:
    r12 = r11;                                  /* MOV LINE,AELINE */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 != 44) goto INPHEX;                 /* CMP R10,#"," */
    s->r[10] = r10; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CRAELV */
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                 /* BEQ ERSYNT */
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    r4 = r0;                                    /* MOV R4,IACC */
    r5 = r9;                                    /* MOV R5,TYPE */
    r1 = ros_ld32(r13);                         /* LDR R1,[SP] */
ENDOFFILE:
    s->r[1] = r1;                               /* SWI OS_BGet */
    ros_native_swi(s, ros_thunk_OS_BGet);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    if (s->c) goto ENDOFFILE;                   /* BCS ENDOFFILE */
    ros_subs(s, r5, 128);                       /* CMP R5,#128 */
    if (r5 < 128) goto INPHNO;                  /* BCC INPHNO */
    r9 = ros_logic(s, r0 << 24, (r0 >> 8) & 1); /* MOVS TYPE,R0,LSL #24 */
    if (r9 != 0) {                              /* BNE ERTYPESTR */
        s->r[4] = r4; s->r[5] = r5; s->r[9] = r9;
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    ros_native_swi(s, ros_thunk_OS_BGet);       /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_logic(s, r0, s->c);                     /* TEQ R0,#0 */
    r2 = r8 - 1536 + r0;                        /* ADD CLEN,CLEN,R0 */
    if (r0 == 0) goto INPHSS;                   /* BEQ INPHSS */
    r3 = r0;                                    /* MOV R3,R0 */
    r6 = r2;                                    /* MOV R6,CLEN */
INPHSL:
    ros_native_swi(s, ros_thunk_OS_BGet);       /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r6 -= 1;                                    /* STRB R0,[R6,#-1]! */
    ros_st8(r6, r0);
    r3 = ros_subs(s, r3, 1);                    /* SUBS R3,R3,#1 */
    if (r3 != 0) goto INPHSL;                   /* BNE INPHSL */
    goto INPHSS;                                /* B INPHSS */

INPHNO:
    r9 = ros_logic(s, r0 << 24, (r0 >> 8) & 1); /* MOVS TYPE,R0,LSL #24 */
    if (r9 == 0) {                              /* BEQ ERTYPEINT */
        s->r[4] = r4; s->r[5] = r5; s->r[9] = r9;
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    if ((int32_t)r9 < 0) goto INPHNF;           /* BMI INPHNF */
    ros_native_swi(s, ros_thunk_OS_BGet);       /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r2 = r0 << 24;                              /* MOV R2,R0,LSL #24 */
    ros_native_swi(s, ros_thunk_OS_BGet);       /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r2 |= r0 << 16;                             /* ORR R2,R2,R0,LSL #16 */
    ros_native_swi(s, ros_thunk_OS_BGet);       /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r2 |= r0 << 8;                              /* ORR R2,R2,R0,LSL #8 */
    ros_native_swi(s, ros_thunk_OS_BGet);       /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r0 = r2 | r0;                               /* ORR R0,R2,R0 */
    /* fall INPHSS */

INPHSS:
    s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[6] = r6; s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;                     /* BL STOREA */
    basicvfp_STOREA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    goto INPHLP;                                /* B INPHLP */

INPHNF:
    ros_subs(s, r0, 136);                       /* CMP R0,#&88 */
    r3 = r0 == 136 ? 8 : 5;                     /* new FP? */
    r9 = r8 - 1536;                             /* ADD TYPE,ARGP,#STRACC */
    r2 = 0;                                     /* MOV R2,#0 */
INPHFP:
    ros_native_swi(s, ros_thunk_OS_BGet);       /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_st8(r9 + r2, r0);                       /* STRB R0,[TYPE,R2] */
    r2 += 1;                                    /* ADD R2,R2,#1 */
    ros_subs(s, r2, r3);                        /* CMP R2,R3 */
    if (r2 != r3) goto INPHFP;                  /* BNE INPHFP */
    if (r3 == 8) goto INPHFP8;                  /* CMP R3,#8 */
    v = ros_ld32(r9);                           /* LDMIA TYPE,{IACC,R1} */
    r0 = v;
    r1 = ros_ld32(r9 + 4) & 0xFFu;              /* ANDS R1,R1,#255 */
    if (r1 != 0 || r0 != 0) r0 |= TFP;          /* ORRNE IACC,IACC,#TFP */
    r2 = (v & TFP) | ((r1 + 894) << 20);        /* rebase the exponent */
    r1 = 0;                                     /* MOV R1,#0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[14] = 0xFFFFFFF0u;                     /* BL FCONVERT2 */
    basicvfp_FCONVERT2(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r14 = s->r[14];
    r9 = TFP;                                   /* MOV TYPE,#TFP */
    goto INPHSS;                                /* B INPHSS */

INPHFP8:
    s->fp->vfp.s[1] = ros_lds(r9);              /* FLDS S1,[TYPE] */
    s->fp->vfp.s[0] = ros_lds(r9 + 4);          /* FLDS S0,[TYPE,#4] */
    r9 = TFP;                                   /* MOV TYPE,#TFP */
    goto INPHSS;                                /* B INPHSS */

INPHEX:
    r13 += 4;                                   /* ADD SP,SP,#4 */
    s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[9] = r9;
    s->r[10] = r10; s->r[12] = r12; s->r[13] = r13; s->r[14] = r14;
    s->r[15] = s->r[14];                        /* B DONEXT */
    basicvfp_hand_DONEXT(s);
    return;
}

/* =====================================================================
 * LINEST (Stmt2.s:966-989).  LINE INPUT joins INPUT's INPLP.  Plain
 * LINE draws four points through two DOPLOTs.
 * ===================================================================== */
void basicvfp_hand_LINEST(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_logic(s, r10 ^ TINPUT, s->c);           /* TEQ R10,#TINPUT */
    if (s->z) {                                 /* BEQ INPLP */
        r5 = 0;                                 /* MOVEQ R5,#0 */
        r4 = 64;                                /* MOVEQ R4,#&40 */
        dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                      r11, r12, r13, r14);
        dci_inplp(s);
        return;
    }
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r4 = ros_ld32(r13);                         /* LDMFD SP!,{R4,R5,R6} */
    r5 = ros_ld32(r13 + 4);
    r6 = ros_ld32(r13 + 8);
    r13 += 12;
    r3 = r0;                                    /* MOV R3,IACC */
    r0 = 4;                                     /* MOV R0,#4 */
    r1 = r6;                                    /* MOV R1,R6 */
    r2 = r5;                                    /* MOV R2,R5 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; move x,y */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = 5;                                     /* MOV R0,#5 */
    r1 = r4;                                    /* MOV R1,R4 */
    r2 = r3;                                    /* MOV R2,R3 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; draw x2,y2 */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    DCI_TO_NXT(s);                              /* B NXT */
}

/* =====================================================================
 * READ (Stmt.s:1273-1295) and RESTORE (Stmt.s:1327-1352).
 * ===================================================================== */
void basicvfp_hand_READ(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

READ:
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CRAELV */
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) goto READS;                       /* BEQ READS */
    r12 = r11;                                  /* MOV LINE,AELINE */
    ros_subs(s, r9, 128);                       /* CMP TYPE,#128 */
    if (r9 >= 128) goto READST;                 /* BCS READST */
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DATAIT */
    basicvfp_DATAIT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r11 -= 1;                                   /* SUB AELINE,AELINE,#1 */
    goto READEN;                                /* B READEN */

READST:
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DATAIT */
    basicvfp_DATAIT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DATAST */
    basicvfp_DATAST(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    /* fall READEN */

READEN:
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;                     /* BL STORE */
    basicvfp_hand_STORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_st32(r8 - 268, r11);                    /* STR AELINE,[ARGP,#DATAP] */
READS:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 != 44) {                            /* CMP R10,#"," */
        s->r[10] = r10; s->r[12] = r12; s->r[14] = r14;
        s->r[15] = s->r[14];                    /* BNE DONEXT */
        basicvfp_hand_DONEXT(s);
        return;
    }
    goto READ;                                  /* B READ */
}

void basicvfp_hand_RESTORE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == TERROR) goto RESTOREERROR;       /* CMP R10,#TERROR */
    if (r10 == TDATA) goto RESTOREDATA;         /* CMP R10,#TDATA */
    r1 = ros_ld32(r8 - 148);                    /* LDR R1,[ARGP,#PAGE] */
    ros_subs(s, r10, 43);                       /* CMP R10,#"+" */
    if (r10 == 43) goto RESTOREREL;             /* BEQ RESTOREREL */
    if (r10 == 58 || r10 == 13 || r10 == TELSE) /* :/CR/ELSE */
        goto RESDON;
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL GOFACT */
    basicvfp_GOFACT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
RESDON:
    ros_st32(r8 - 268, r1);                     /* STR R1,[ARGP,#DATAP] */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[14] = r14;
    DCI_TO_NXT(s);                              /* B NXT */

RESTOREREL:
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXDN */
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = r12 - 2;                               /* SUB R1,LINE,#2 */
RESTOREREL1:
    r2 = ros_ld8(r1);                           /* LDRB R2,[R1],#1 */
    r1 += 1;
    if (r2 != 13) goto RESTOREREL1;             /* CMP R2,#13 */
    r2 = ros_ld8(r1);                           /* LDRB R2,[R1],#1 */
    r1 += 3;                                    /* ADD R1,R1,#2 */
    if (r2 == 0xFFu) {                          /* CMP R2,#&FF */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
        s->r[15] = s->r[14];                    /* BEQ NOLINE */
        basicvfp_NOLINE(s);
        return;
    }
    v = r0;                                     /* SUBS R0,R0,#1 */
    r0 -= 1;
    if ((int32_t)v > 1) goto RESTOREREL1;       /* BGT RESTOREREL1 */
    r1 -= 4;                                    /* SUB R1,R1,#4 */
    goto RESDON;                                /* B RESDON */

RESTOREERROR:
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 = ros_ld32(r13);                         /* LDR R0,[SP],#4 */
    r13 += 4;
    if (r0 != TERROR) goto ONERRX;              /* CMP R0,#TERROR */
    r1 = ros_ld32(r13);                         /* LDMFD SP!,{R1,R2} */
    r2 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_st32(r8 - 120, r1);                     /* STR R1,[ARGP,#ERRSTK] */
    ros_st32(r8 - 124, r2);                     /* STR R2,[ARGP,#ERRORH] */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    DCI_TO_NXT(s);                              /* B NXT */
ONERRX:
    s->r[0] = r0; s->r[13] = r13;               /* BL MSG (ONERRX) */
    dci_msg_at(s, SITE_ONERRX);

RESTOREDATA:
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 = ros_ld32(r13);                         /* LDR R0,[SP],#4 */
    r13 += 4;
    if (r0 != TDATA) goto ERRDATASTACK;         /* CMP R0,#TDATA */
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    r13 += 4;
    ros_st32(r8 - 268, r1);                     /* STR R1,[ARGP,#DATAP] */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    DCI_TO_NXT(s);                              /* B NXT */
ERRDATASTACK:
    s->r[0] = r0; s->r[13] = r13;               /* BL MSG */
    dci_msg_at(s, SITE_ERRDATASTACK);
}

/* =====================================================================
 * The SOUND family (Stmt.s:1485-1559).
 * ===================================================================== */
void basicvfp_hand_BEATS(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXDN */
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_swi(s, 0x401C6u);                       /* SWI Sound_QBeat */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_VOICES(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXDN */
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r1 = 0;                                     /* MOV R1,#0 */
    r2 = 0;                                     /* MOV R2,#0 */
    r3 = 0;                                     /* MOV R3,#0 */
    r4 = 0;                                     /* MOV R4,#0 */
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    ros_swi(s, 0x40140u);                       /* SWI Sound_Configure */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_VOICE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 != 0) {                              /* BNE ERTYPESTR */
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 = ros_ld32(r13);                         /* LDR R0,[SP],#4 */
    r13 += 4;
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC */
    r4 = 0;                                     /* MOV R4,#0 */
    ros_st8(r2, 0);                             /* STRB R4,[CLEN] */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[13] = r13;
    ros_swi(s, 0x4018Au);                       /* SWI Sound_AttachNamedVoice */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_TEMPO(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXDN */
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_swi(s, 0x401C5u);                       /* SWI Sound_QTempo */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_STEREO(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = ros_ld32(r13);                         /* LDR R0,[SP],#4 */
    r13 += 4;
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_swi(s, 0x40142u);                       /* SWI Sound_Stereo */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_SOUND(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, TOF);                      /* CMP R10,#TOF */
    if (r10 == TOF) goto SOUNDOFF;              /* BEQ SOUNDOFF */
    r10 = ros_logic(s, r10 ^ TON, s->c);        /* EORS R10,R10,#TON */
    if (r10 == 0) goto SOUNDON;                 /* BEQ SOUNDON */
    r11 = r12 - 1;                              /* SUB AELINE,LINE,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    v = ros_ld32(r13);                          /* LDR R1,[SP],#4 */
    r1 = v;
    r1 <<= 16;                                  /* MOV R1,R1,LSL #16 */
    r0 = (r0 << 16) | (v & 0xFFFFu);            /* ORR IACC,IACC,R1,LSR #16 */
    ros_st32(r13, r0);                          /* STR IACC,[SP,#-4]! */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    v = r13;                                    /* LDMFD SP!,{R1,R2} */
    r1 = ros_ld32(r13);
    r2 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 <<= 16;                                  /* MOV IACC,IACC,LSL #16 */
    r1 <<= 16;                                  /* MOV R1,R1,LSL #16 */
    r3 = r0 | (ros_ld32(v) & 0xFFFFu);          /* ORR R3,IACC,R1,LSR #16 */
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44) goto SOUNDFOUR;              /* BNE SOUNDFOUR */
    r13 -= 8;                                   /* STMFD SP!,{R2,R3} */
    ros_st32(r13, r2);
    ros_st32(r13 + 4, r3);
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN ; time in R0 */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = 0;                                     /* MOV R1,#0 */
    r2 = ros_ld32(r13);                         /* LDMFD SP!,{R2,R3} */
    r3 = ros_ld32(r13 + 4);
    r13 += 8;
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[13] = r13;
    ros_swi(s, 0x401C1u);                       /* SWI Sound_QSchedule */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */

SOUNDFOUR:
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC */
    ros_st32(r1, r2);                           /* STMIA R1,{R2,R3} */
    ros_st32(r1 + 4, r3);
    r0 = 7;                                     /* MOV R0,#OsWord_GenerateSound */
    s->r[0] = r0; s->r[1] = r1;                 /* SWI OS_Word */
    ros_native_swi(s, ros_thunk_OS_Word);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    DCI_TO_NXT(s);                              /* B NXT */

SOUNDOFF:
    r0 = 1;                                     /* MOV R0,#1 */
    goto SOUNDENABLEON;                         /* B SOUNDENABLEON */
SOUNDON:
    r0 = 2;                                     /* MOV R0,#2 */
SOUNDENABLEON:
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_swi(s, 0x40141u);                       /* SWI Sound_Enable */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

/* =====================================================================
 * CLEAR (Stmt.s:154-158), CLS/CLG (Stmt2.s:387-395), WAIT
 * (Stmt2.s:1668-1675).
 * ===================================================================== */
void basicvfp_hand_CLEAR(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL SETFSA */
    basicvfp_hand_SETFSA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r6 = s->r[6];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

static void dci_clsa(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[14] = 0xFFFFFFF0u;                     /* BL CTALLY */
    basicvfp_CTALLY(s);
    r2 = s->r[2]; r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_CLG(struct ros_cpu *s)
{
    s->r[0] = 16;                               /* MOV R0,#16 */
    dci_clsa(s);
}

void basicvfp_hand_CLS(struct ros_cpu *s)
{
    s->r[0] = 12;                               /* MOV R0,#12 */
    dci_clsa(s);
}

void basicvfp_hand_WAIT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 = 19;                                    /* MOV R0,#19 */
    s->r[0] = r0;                               /* SWI OS_Byte */
    ros_native_swi(s, ros_thunk_OS_Byte);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2];
    DCI_TO_NXT(s);                              /* B NXT */
}

/* =====================================================================
 * MODES (Stmt2.s:990-1121): all five forms.
 * ===================================================================== */
void basicvfp_hand_MODES(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    r11 = r12;                                  /* MOV AELINE,LINE */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r12 = r11;                                  /* MOV LINE,AELINE */
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 == 0) goto MODESTRING;               /* BEQ MODESTRING */
    if ((int32_t)r9 < 0) {                      /* BLMI INTEGB */
        s->r[12] = r12;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_INTEGB(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    }
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (s->z) goto MODESD;                      /* BEQ MODESD */
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONE */
    basicvfp_hand_DONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_subs(s, r0, 0x100u);                    /* CMP R0,#256 */
    if (r0 >= 0x100u) r1 = r0;                  /* MOVCS R1,R0 */
    if (r0 >= 0x100u) goto MODESELECTORBLOCK;   /* BCS MODESELECTORBLOCK */
    ros_st8(r8 - 25, 1);                        /* STRB R14,[ARGP,#MEMM] */
    ros_swi(s, 0x116u);                         /* SWI OS_WriteI+22 */
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_st8(r8 - 25, 0);                        /* STRB R14,[ARGP,#MEMM] */
MODEGOOD:
    s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CTALLY */
    basicvfp_CTALLY(s);
    r2 = s->r[2]; r14 = s->r[14];
    s->r[1] = r1; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    s->r[7] = r7;
    DCI_TO_NXT(s);                              /* B NXT */

MODESD:
    r1 = 1;                                     /* MOV R1,#1 */
    r13 -= 28;          /* STR R1,[SP,#-(ModeSelector_ModeVars+8)]! */
    ros_st32(r13, 1);
    ros_st32(r13 + 4, r0);                      /* STR IACC,[SP,#XRes] */
    s->r[1] = r1; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_st32(r13 + 8, r0);                      /* STR IACC,[SP,#YRes] */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_st32(r13 + 20, r0);         /* [SP,#ModeVars] ; depth or flags */
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (!s->z) {
        r0 = 0xFFFFFFFFu;                       /* MOVNE IACC,#-1 */
        goto MODESD_BPP;
    }
    s->r[0] = r0;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (!s->z) goto MODESD_BPP;                 /* BNE MODESD_BPP */
    ros_st32(r13 + 24, r0);         /* [ModeVars+4] ; NColour */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_st32(r13 + 12, r0);         /* [PixelDepth] ; Log2BPP */
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (!s->z) {
        r0 = 0xFFFFFFFFu;                       /* MOVNE IACC,#-1 */
        goto MODESD_FL_NC_L2;
    }
    s->r[0] = r0;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
MODESD_FL_NC_L2:
    ros_st32(r13 + 16, r0);         /* [FrameRate] */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r4 = ros_ld32(r13 + 4);                     /* LDMIA SP!,{R3-R7} */
    r5 = ros_ld32(r13 + 8);
    r6 = ros_ld32(r13 + 12);
    r7 = ros_ld32(r13 + 16);
    ros_st32(r8 - 1536, ros_ld32(r13));         /* STMIA R1!,{R3-R7} */
    ros_st32(r8 - 1532, r4);
    ros_st32(r8 - 1528, r5);
    ros_st32(r8 - 1524, r6);
    ros_st32(r8 - 1520, r7);
    r2 = 0;                                     /* MOV R2,#VduExt_ModeFlags */
    r3 = ros_ld32(r13 + 20);                    /* LDMIA SP!,{R3,R5} */
    r5 = ros_ld32(r13 + 24);
    r13 += 28;
    r4 = 3;                                     /* MOV R4,#VduExt_NColour */
    r6 = 0xFFFFFFFFu;                           /* MOV R6,#-1 */
    ros_st32(r8 - 1516, r2);                    /* STMIA R1,{R2-R6} */
    ros_st32(r8 - 1512, r3);
    ros_st32(r8 - 1508, r4);
    ros_st32(r8 - 1504, r5);
    ros_st32(r8 - 1500, r6);
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC */
    goto MODESELECTORBLOCK;                     /* B MODESELECTORBLOCK */

MODESD_BPP:
    ros_st32(r13 + 16, r0);         /* [FrameRate] */
    r0 = ros_ld32(r13 + 20);        /* LDR IACC,[SP,#ModeVars] */
    r1 = 0;                                     /* MOV R1,#0 */
    r2 = MODEBPPTAB;                            /* ADR R2,MODEBPPTAB */
MODESDFINDBPP:
    v = r2 + r1;
    r3 = ros_ld8(v);                            /* LDRB R3,[R2,R1] */
    ros_logic(s, ros_ld8(v), s->c);             /* TEQ R3,#0 */
    if (r3 == 0) goto ERBPP;                    /* BEQ ERBPP */
    if ((r0 ^ r3) != 0) {                       /* TEQ IACC,R3 */
        r1 += 1;
        goto MODESDFINDBPP;
    }
    ros_st32(r13 + 12, r1);         /* [PixelDepth] ; depth index */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r1 = r8 - 1536;                /* STRACC; clear stack for reclaim */
    v = r13;                       /* LDMIA SP!,{R3-R7} ; flg,X,Y,D,F */
    r4 = ros_ld32(r13 + 4);
    r5 = ros_ld32(r13 + 8);
    r6 = ros_ld32(r13 + 12);
    r7 = ros_ld32(r13 + 16);
    r13 += 28;      /* ADD SP,SP,#8 ; junk the 2nd variant's words */
    ros_st32(r1, ros_ld32(v));
    ros_st32(r1 + 4, r4);
    ros_st32(r1 + 8, r5);
    ros_st32(r1 + 12, r6);
    ros_st32(r1 + 16, r7);
    r3 = 3;                                     /* MOV R3,#VduExt_NColour */
    r4 = 0xFFu;                                 /* MOV R4,#255 */
    r5 = 0xFFFFFFFFu;                           /* MOV R5,#-1 */
    ros_st32(r1 + 24, 0x80u);       /* ModeFlag_FullPalette list for */
    ros_st32(r1 + 28, 3);           /* depth index 4 (8bpp) */
    ros_st32(r1 + 32, 0xFFu);
    ros_st32(r1 + 36, 0xFFFFFFFFu);
    ros_subs(s, r6, 4);                         /* CMP R6,#4 */
    r2 = r6 == 4 ? 0u : 0xFFFFFFFFu;            /* MOVEQ VduExt_ModeFlags */
    ros_st32(r1 + 12, r6 < 4 ? r6 : r6 - 1);    /* [PixelDepth] */
    ros_st32(r1 + 20, r2);                      /* [ModeVars] */

MODESELECTORBLOCK:
    ros_st8(r8 - 25, 1);                        /* STRB R14,[ARGP,#MEMM] */
    r0 = 0;                                     /* SelectMode */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_swi(s, 0x20065u);                       /* SWI XOS_ScreenMode */
    r0 = s->r[0]; r1 = s->r[1];
    ros_st8(r8 - 25, 0);                        /* STRB R14,[ARGP,#MEMM] */
    if (!s->v) goto MODEGOOD;                   /* BVC MODEGOOD */
    r1 = ros_ld32(r0) - 0x100u;                 /* LDR R1,[R0]; SUB #256 */
    ros_subs(s, r1, 230);                       /* CMP #NoSuchSWI-256 */
    if (r1 != 230) {                            /* SWINE OS_GenerateError */
        ros_native_swi(s, ros_thunk_OS_GenerateError);
        if (s->v) ros_swi_raise(s);
        r0 = s->r[0];
    }
    goto ERBADMODE;                             /* B ERBADMODE */

MODESTRING:
    ros_st8(r2, 0);                             /* STRB R0,[CLEN] */
    ros_st8(r8 - 25, 1);                        /* STRB R14,[ARGP,#MEMM] */
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC */
    r0 = 15;                                    /* SelectModeByString */
    s->r[0] = r0; s->r[1] = r1; s->r[12] = r12;
    ros_swi(s, 0x65u);                          /* SWI OS_ScreenMode */
    r0 = s->r[0]; r1 = s->r[1];
    ros_st8(r8 - 25, 0);                        /* STRB R14,[ARGP,#MEMM] */
    goto MODEGOOD;                              /* B MODEGOOD */

ERBADMODE:
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[6] = r6; s->r[7] = r7;
    dci_msg_at(s, SITE_ERBADMODE);
ERBPP:
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    dci_msg_at(s, SITE_ERBPP);
}

/* =====================================================================
 * TINTEND (Stmt2.s:1637-1643): VDU 23,17,action,tint, then ZEROX.
 * COLOUR, GCOL and DOTINT share it.
 * ===================================================================== */
static void dci_tintend(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r10 = s->r[10],
             r11 = s->r[11], r12 = s->r[12], r13 = s->r[13],
             r14 = s->r[14];

    s->r[0] = r0; s->r[13] = r13;
    ros_swi(s, 0x117u);                         /* SWI OS_WriteI+23 */
    r0 = s->r[0];
    ros_swi(s, 0x111u);                         /* SWI OS_WriteI+17 */
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = r1;                                    /* MOV IACC,R1 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r1 = 6;                                     /* MOV R1,#6 */
    basicvfp_ZEROX(s, &r0, &r1, r10, r11, r12, r13, r14); /* BL ZEROX */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[14] = r14;
    DCI_TO_NXT(s);                              /* B NXT */
}

/* =====================================================================
 * COLOUR (Stmt2.s:426-597).
 * ===================================================================== */
void basicvfp_hand_COLOUR(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_logic(s, r10 ^ TOF, s->c);              /* TEQ R10,#TOF */
    v = r10 ^ TON;                              /* TEQNE R10,#TON */
    if (v != 0) ros_logic(s, r10 ^ TON, s->c);
    if (r10 == TOF || r10 == TON) goto COLOUROFON;
    r11 = r12 - 1;                              /* SUB AELINE,LINE,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (r10 == TESCSTMT) goto COLOURTINT;       /* CMP R10,#TESCSTMT */
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (s->z) goto PALETTE;                     /* BEQ PALETTE */
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_swi(s, 0x111u);                         /* SWI OS_WriteI+17 */
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    DCI_TO_NXT(s);                              /* B NXT */

COLOURTINT:
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 != TTINT) {                         /* CMP R10,#TTINT */
        s->r[10] = r10; s->r[11] = r11;
        s->r[15] = s->r[14];                    /* BNE ERSYNT */
        basicvfp_ERSYNT(s);
        return;
    }
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = ros_ld32(r13);                         /* LDR IACC,[SP],#4 */
    r13 += 4;
    s->r[0] = r0; s->r[13] = r13;               /* SWI OS_WriteI+17 */
    ros_swi(s, 0x111u);
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r0 = (r0 >> 7) & 1;                         /* MOV IACC,IACC,LSR #7 */
    dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                  r11, r12, r13, r14);
    dci_tintend(s);                             /* B TINTEND */
    return;

PALETTE:
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (s->z) goto PALETTE4;                    /* BEQ PALETTE4 */
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = ros_ld32(r13);                         /* LDR IACC,[SP],#4 */
    r13 += 4;
    s->r[0] = r0; s->r[13] = r13;               /* SWI OS_WriteI+19 */
    ros_swi(s, 0x113u);
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = r1;                                    /* MOV IACC,R1 */
    s->r[0] = r0;                               /* BL WRITEG */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_WRITEG(s);
    r0 = r1 >> 16;                              /* MOV IACC,R1,LSR #16 */
    s->r[0] = r0;                               /* BL WRITEG */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_WRITEG(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[1] = r1;
    DCI_TO_NXT(s);                              /* B NXT */

PALETTE4:
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (!s->z) goto COLOUR3;                    /* BNE COLOUR3 */
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (s->z) goto PALETTE5;                    /* BEQ PALETTE5 */
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r6 = 16;                                    /* MOV R6,#16 */
PROGPAL:
    r5 = r0;                                    /* MOV R5,IACC */
    ros_swi(s, 0x113u);                         /* SWI OS_WriteI+19 */
    r1 = ros_ld32(r13);                         /* LDMFD SP!,{R1,R2,R3} */
    r2 = ros_ld32(r13 + 4);
    r3 = ros_ld32(r13 + 8);
    r13 += 12;
    r0 = r3;                                    /* MOV IACC,R3 */
    s->r[0] = r0; s->r[13] = r13;               /* SWI OS_WriteC */
    ros_native_swi(s, ros_thunk_OS_WriteC);
    if (s->v) ros_swi_raise(s);
    r0 = r6;                                    /* MOV IACC,R6 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_WriteC);
    if (s->v) ros_swi_raise(s);
    r0 = r2;                                    /* MOV IACC,R2 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_WriteC);
    if (s->v) ros_swi_raise(s);
    r0 = r1;                                    /* MOV IACC,R1 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_WriteC);
    if (s->v) ros_swi_raise(s);
    r0 = r5;                                    /* MOV IACC,R5 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_WriteC);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[5] = r5; s->r[6] = r6;
    DCI_TO_NXT(s);                              /* B NXT */

PALETTE5:
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    v = r13;                                    /* LDMFD SP!,{R1-R4} */
    r13 += 16;
    r3 = ros_ld32(v + 8) & 0xFFu;               /* AND R3,R3,#&FF */
    r2 = ((ros_ld32(v + 4) & 0xFFu) << 16) | (r0 & 0xFFu)
        | ((ros_ld32(v) & 0xFFu) << 24) | (r3 << 8);
    r0 = ros_ld32(v + 12) & 0xFFu;              /* AND R0,R4,#&FF */
    r1 = 16;                                    /* MOV R1,#16 */
    r4 = 2;                                     /* MOV R4,#paletteV_Set */
    r9 = 35;                                    /* MOV R9,#PaletteV */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[9] = r9; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_CallAVector); /* SWI OS_CallAVector */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */

COLOUR3:
    r1 = ros_ld32(r13);                         /* LDMFD SP!,{R1,R2} */
    r2 = ros_ld32(r13 + 4);
    ros_st32(r13 + 4, 0);                       /* STR R14,[SP,#-4]! */
    r4 = TOF;                                   /* MOV R4,#TOF */
    r13 -= 24;                                  /* STMFD SP!,{R0,R1,R2,R4} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r1);
    ros_st32(r13 + 8, r2);
    ros_st32(r13 + 12, TOF);
    goto COLOUROFON3LPN;                        /* B COLOUROFON3LPN */

COLOUROFON:
    r13 -= 8;                    /* STMFD SP!,{R11,R14} ; terminator */
    ros_st32(r13, r11);
    ros_st32(r13 + 4, 0);
    r11 = r12;                                  /* MOV AELINE,LINE */
COLOUROFONLP:
    r13 -= 4;                                   /* STR R10,[SP,#-4]! */
    ros_st32(r13, r10);
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r5 = ros_ld32(r13 + 8);                     /* LDR R5,[SP,#8] */
    r4 = ros_ld32(r13);                         /* LDR R4,[SP,#0] */
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    v = r10 ^ 0x2Cu;
    if (v == 0) ros_logic(s, ros_ld32(r13 + 8), s->c); /* TEQEQ R5,#0 */
    if (v == 0 && r5 == 0) goto COLOUROFON3;    /* BEQ COLOUROFON3 */
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    ros_logic(s, r4 ^ TOF, s->c);               /* TEQ R4,#TOF */
    v = r4 ^ TOF;
    if (v == 0) ros_logic(s, r10 ^ TON, s->c);  /* TEQEQ R10,#TON */
    if (v == 0 && r10 == TON) goto COLOUROFONLP; /* BEQ COLOUROFONLP */
    s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
DOCOLOUROFON:
    r1 = ros_ld32(r13);                         /* LDMFD SP!,{R1,R4} */
    r4 = ros_ld32(r13 + 4);
    r13 += 8;
    if (r4 == 0) {                              /* TEQ R4,#0 */
        s->r[1] = r1; s->r[4] = r4; s->r[13] = r13;
        DCI_TO_NXT(s);                          /* BEQ NXT */
    }
    ros_logic(s, r4 ^ TON, s->c);               /* TEQ R4,#TON */
    r0 = s->z ? 80 : 64;                        /* MOVEQ &50 / MOVNE &40 */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_swi(s, 0x61u);                          /* SWI OS_SetColour */
    r0 = s->r[0];
    goto DOCOLOUROFON;                          /* B DOCOLOUROFON */

COLOUROFON3:
    r13 -= 8;                    /* SUB SP,#4 ; STR R4,[SP,#-4]! */
    ros_st32(r13, r4);
COLOUROFON3LP:
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[1] = r1; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    r4 = ros_ld32(r13 + 12);                    /* LDR R4,[SP,#12] */
COLOUROFON3LPN:
    ros_logic(s, r10 ^ TON, s->c);              /* TEQ R10,#TON */
    v = r10 ^ TON;
    if (v == 0) ros_logic(s, r4 ^ TOF, s->c);   /* TEQEQ R4,#TOF */
    if (v != 0 || (r4 ^ TOF) != 0) goto DOCOLOUROFON3DN;
    r13 -= 4;                                   /* STR R10,[SP,#-4]! */
    ros_st32(r13, r10);
    s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    goto COLOUROFON3LP;                         /* B COLOUROFON3LP */

DOCOLOUROFON3DN:
    s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r3 = s->r[3]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
DOCOLOUROFON3:
    r0 = ros_ld32(r13);                         /* LDMFD SP!,{R0,R1,R2,R4} */
    r1 = ros_ld32(r13 + 4);
    r2 = ros_ld32(r13 + 8);
    r4 = ros_ld32(r13 + 12);
    r13 += 16;
    if (r4 == 0) {                              /* TEQ R4,#0 */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[4] = r4;
        s->r[13] = r13;
        DCI_TO_NXT(s);                          /* BEQ NXT */
    }
    r2 &= 0xFFu;                                /* AND R2,R2,#255 ; red */
    r1 = r2 | ((r1 & 0xFFu) << 8);              /* ORR R1,R2,R1,LSL #8 */
    r0 = (r1 | ((r0 & 0xFFu) << 16)) << 8;      /* MOV R0,R0,LSL #8 */
    ros_logic(s, r4 ^ TON, s->c);               /* TEQ R4,#TON */
    r3 = s->z ? 128 : 0;                        /* MOVEQ #&80 / MOVNE #0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[13] = r13;
    ros_swi(s, 0x40761u);                       /* SWI ColourTrans_SetTextColour */
    r3 = s->r[3]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    goto DOCOLOUROFON3;                         /* B DOCOLOUROFON3 */
}

/* =====================================================================
 * GCOL (Stmt2.s:832-965).
 * ===================================================================== */
void basicvfp_hand_GCOL(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_logic(s, r10 ^ TOF, s->c);              /* TEQ R10,#TOF */
    v = r10 ^ TON;
    if (v != 0) ros_logic(s, r10 ^ TON, s->c);  /* TEQNE R10,#TON */
    if (r10 == TOF || r10 == TON) goto GCOLOFON;
    r11 = r12 - 1;                              /* SUB AELINE,LINE,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 == 44) goto GCOL2;                  /* BEQ GCOL2 */
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = 0;                                     /* MOV R0,#0 */
    if (r10 != TESCSTMT) goto DOGCOL;           /* CMP R10,#TESCSTMT */

GCOLTINT:
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 != TTINT) {                         /* CMP R10,#TTINT */
        s->r[0] = r0; s->r[1] = r1; s->r[10] = r10; s->r[11] = r11;
        s->r[13] = r13;
        s->r[15] = s->r[14];                    /* BNE ERSYNT */
        basicvfp_ERSYNT(s);
        return;
    }
    r13 -= 8;                                   /* STMFD SP!,{R0,R1} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r1);
    s->r[0] = r0; s->r[1] = r1; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = ros_ld32(r13);                         /* LDMFD SP!,{R0,R2} */
    r2 = ros_ld32(r13 + 4);
    r13 += 8;
    s->r[0] = r0; s->r[13] = r13;               /* SWI OS_WriteI+18 */
    ros_swi(s, 0x112u);
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = r2;                                    /* MOV R0,R2 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_logic(s, r0 & 0x80u, s->c);             /* TST R0,#128 */
    r0 = s->z ? 2 : 3;                          /* MOVEQ #2 / MOVNE #3 */
    dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                  r11, r12, r13, r14);
    dci_tintend(s);                             /* B TINTEND */
    return;

GCOL2:
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 == 44) goto GCOL3;                  /* BEQ GCOL3 */
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = ros_ld32(r13);                         /* LDR IACC,[SP],#4 */
    r13 += 4;
    if (r10 == TESCSTMT) goto GCOLTINT;         /* CMP R10,#TESCSTMT */

DOGCOL:
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_swi(s, 0x112u);                         /* SWI OS_WriteI+18 */
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = r1;                                    /* MOV IACC,R1 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_WriteC);     /* SWI OS_WriteC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    DCI_TO_NXT(s);                              /* B NXT */

GCOL3:
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    ros_st32(r13, 0);                           /* STR R14,[SP,#-4]! */
    r4 = 79;                                    /* MOV R4,#"O" ; magic */
    r13 -= 24;                                  /* STMFD SP!,{R1,R4} */
    ros_st32(r13, r1);
    ros_st32(r13 + 4, 79);
    goto GCOLOFON3LP;                           /* B GCOLOFON3LP */

GCOLOFON:
    r13 -= 12;                  /* STMFD SP!,{R11,R12,R14} ; terminator */
    ros_st32(r13, r11);
    ros_st32(r13 + 4, r12);
    ros_st32(r13 + 8, 0);
    r11 = r12;                                  /* MOV AELINE,LINE */
GCOLOFONLP:
    r13 -= 4;                                   /* STR R10,[SP,#-4]! */
    ros_st32(r13, r10);
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    if (!s->z) r1 = 0;                          /* MOVNE R1,#0 */
    if (!s->z) {                                /* STMNEFD SP!,{R0,R1} */
        ros_st32(r13 - 8, r0);
        ros_st32(r13 - 4, r1);
        r13 -= 8;
        goto GCOLOFONLPN;                       /* BNE GCOLOFONLPN */
    }
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[1] = r1; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r5 = ros_ld32(r13 + 16);                    /* LDR R5,[SP,#16] */
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    v = r10 ^ 0x2Cu;
    if (v == 0) ros_logic(s, ros_ld32(r13 + 16), s->c); /* TEQEQ R5,#0 */
    if (v == 0 && r5 == 0) goto GCOLOFON3;      /* BEQ GCOLOFON3 */
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
GCOLOFONLPN:
    r4 = ros_ld32(r13 + 8);                     /* LDR R4,[SP,#8] */
    ros_logic(s, r10 ^ TON, s->c);              /* TEQ R10,#TON */
    v = r10 ^ TON;
    if (v == 0) ros_logic(s, r4 ^ TOF, s->c);   /* TEQEQ R4,#TOF */
    if (v == 0 && r4 == TOF) goto GCOLOFONLP;   /* BEQ GCOLOFONLP */
    s->r[1] = r1; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r3 = s->r[3]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
DOGCOLOFON:
    r1 = ros_ld32(r13);                         /* LDMFD SP!,{R1,R2,R4} */
    r2 = ros_ld32(r13 + 4);
    r4 = ros_ld32(r13 + 8);
    r13 += 12;
    r0 = r2 & 0xFu;                             /* AND R0,R2,#&F */
    if (r4 == 0) {                              /* TEQ R4,#0 */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[4] = r4;
        s->r[13] = r13;
        DCI_TO_NXT(s);                          /* BEQ NXT */
    }
    ros_logic(s, r4 ^ TON, s->c);               /* TEQ R4,#TON */
    if (s->z) r0 |= 16;                         /* ORREQ R0,R0,#&10 */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_swi(s, 0x61u);                          /* SWI OS_SetColour */
    goto DOGCOLOFON;                            /* B DOGCOLOFON */

GCOLOFON3:
    r1 = ros_ld32(r13);                         /* LDMFD SP,{R1,R4} */
    r4 = ros_ld32(r13 + 4);
    r13 -= 8;                                   /* STMFD SP!,{R1,R4} */
    ros_st32(r13, r1);
    ros_st32(r13 + 4, r4);
GCOLOFON3LP:
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[1] = r1; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r10 ^ 0x2Cu, s->c);            /* TEQ R10,#"," */
    v = r13;
    if (!s->z) {                                /* LDMNEFD SP!,{R1,R2} */
        r1 = ros_ld32(r13);
        r2 = ros_ld32(r13 + 4);
        r13 += 8;
        r4 = 0;                                 /* MOVNE R4,#0 */
        ros_st32(r13 - 16, r0);                 /* STMNEFD SP!,{R0,R1,R2,R4} */
        ros_st32(r13 - 12, r1);
        ros_st32(r13 - 8, r2);
        ros_st32(r13 - 4, r4);
        r13 = v - 8;
        goto GCOLOFON3LPN;                      /* BNE GCOLOFON3LPN */
    }
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
GCOLOFON3LPN:
    r5 = ros_ld32(r13 + 16);                    /* LDR R5,[SP,#16] */
    ros_logic(s, r5 ^ TOF, s->c);               /* TEQ R5,#TOF */
    v = r5 ^ 0x4Fu;
    if (v != 0) ros_logic(s, r5 ^ 0x4Fu, s->c); /* TEQNE R5,#"O" */
    if (r5 == TOF || r5 == 0x4Fu)
        ros_logic(s, r10 ^ TON, s->c);          /* TEQEQ R10,#TON */
    if ((r5 != TOF && r5 != 0x4Fu) || r10 != TON)
        goto DOGCOLOFON3DN;                     /* BNE DOGCOLOFON3DN */
    r13 -= 4;                                   /* STR R10,[SP,#-4]! */
    ros_st32(r13, r10);
    s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    goto GCOLOFON3LP;                           /* B GCOLOFON3LP */

DOGCOLOFON3DN:
    s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r3 = s->r[3]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
DOGCOLOFON3:
    r0 = ros_ld32(r13);                         /* LDMFD SP!,{R0-R2,R4,R5} */
    r1 = ros_ld32(r13 + 4);
    r2 = ros_ld32(r13 + 8);
    r4 = ros_ld32(r13 + 12);
    r5 = ros_ld32(r13 + 16);
    r13 += 20;
    if (r5 == 0) {                              /* TEQ R5,#0 */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[4] = r4;
        s->r[5] = r5; s->r[13] = r13;
        DCI_TO_NXT(s);                          /* BEQ NXT */
    }
    v = r5 ^ 0x4Fu;                             /* TEQ R5,#"O" */
    r3 = v != 0 ? 0x100u : r4 | 0x100u;         /* MOVNE #256 / ORREQ */
    r4 = v != 0 ? r4 & 0xFu : r4 & 0xFFu;
    ros_logic(s, r5 ^ TON, s->c);               /* TEQ R5,#TON */
    if (s->z) r3 |= 128;                        /* ORREQ R3,R3,#128 */
    r2 &= 0xFFu;                                /* AND R2,R2,#255 */
    r1 &= 0xFFu;                                /* AND R1,R1,#255 */
    r0 = (r2 | ((r0 & 0xFFu) << 16) | (r1 << 8)) << 8; /* MOV R0,LSL #8 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[13] = r13;
    ros_swi(s, 0x40743u);                       /* SWI ColourTrans_SetGCOL */
    r3 = s->r[3]; r6 = s->r[6]; r7 = s->r[7];
    goto DOGCOLOFON3;                           /* B DOGCOLOFON3 */
}

/* =====================================================================
 * The PLOT family (Stmt2.s:1516-1630): MOVE, DRAW, FILL, PLOT, PSET
 * and the PLOTER/PLOTER1/PLOTER2/PLOTACT web.
 * ===================================================================== */
void basicvfp_hand_MOVE(struct ros_cpu *s)
{
    s->r[0] = 4;                                /* MOV IACC,#4 */
    dci_ploter(s);
}

void basicvfp_hand_DRAW(struct ros_cpu *s)
{
    s->r[0] = 5;                                /* MOV IACC,#5 */
    dci_ploter(s);
}

void basicvfp_hand_FILL(struct ros_cpu *s)
{
    s->r[0] = 133;                              /* MOV IACC,#&85 */
    dci_ploter(s);
}

/* PLOTER (Stmt2.s:1517): SPACES, then the 'B'/'BY' suffix, then the
 * two-coordinate body. */
static void dci_ploter(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                  r11, r12, r13, r14);
    dci_ploter1(s);                             /* fall PLOTER1 */
    return;
}

/* PLOTER1 (Stmt2.s:1518-1525): the 'B'/'BY' relative-suffix check. */
static void dci_ploter1(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    ros_subs(s, r10, 66);                       /* CMP R10,#"B" */
    if (r10 != 66) {
        r12 -= 1;                               /* SUBNE LINE,LINE,#1 */
    } else {
        r10 = ros_ld8(r12);                     /* LDRB R10,[LINE],#1 */
        ros_subs(s, r10, 89);                   /* CMP R10,#"Y" */
        if (r10 == 89) r0 -= 4;                 /* SUBEQ IACC,IACC,#4 */
        r12 = r10 != 89 ? r12 - 1 : r12 + 1;    /* SUBNE LINE,LINE,#2 */
    }
    dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                  r11, r12, r13, r14);
    dci_ploter2(s);                             /* B PLOTER2 */
    return;
}

/* PLOTER2 + PLOTACT (Stmt2.s:1538-1546). */
static void dci_ploter2(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13];
    r3 = ros_ld32(r13);                         /* LDMFD SP!,{R3,R4} */
    r4 = ros_ld32(r13 + 4);
    r13 += 8;
    r2 = r0;                                    /* MOV R2,IACC */
    r0 = r4;                                    /* MOV R0,R4 */
PLOTACT:
    r1 = r3;                                    /* MOV R1,R3 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[3] = r3; s->r[4] = r4;
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_PLOT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r12 = r11;                                  /* MOV LINE,AELINE */
    dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                  r11, r12, r13, r14);
    dci_ploter2(s);                             /* B PLOTER2 */
    return;
}

void basicvfp_hand_PSET(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    r0 = 69;                                    /* MOV IACC,#&45 */
    ros_subs(s, r10, TTO);                      /* CMP R10,#TTO */
    if (r10 != TTO) {
        dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                      r11, r12, r13, r14);
        dci_ploter1(s);                         /* BNE PLOTER1 */
        return;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA ; point to */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    ros_st32(r13, r0);                          /* STR IACC,[SP,#-4]! */
    r13 -= 4;
    ros_st32(r13, (r1 << 16) | 0x500u);         /* STR R1,[SP,#-4]! */
    r1 = r13 + 1;                               /* ADD R1,SP,#1 */
    r0 = 21;                                    /* OsWord_DefinePointerAndMouse */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_Word);       /* SWI OS_Word */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r13 += 8;                                   /* ADD SP,SP,#8 */
    s->r[13] = r13;
    DCI_TO_NXT(s);                              /* B NXT */
}

/* =====================================================================
 * CIRCLE (Stmt2.s:394-411) and ELLIPSE (Stmt2.s:600-829).
 * ===================================================================== */
void basicvfp_hand_CIRCLE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r0 = 149;                                   /* MOV R0,#&95 */
    r1 = 157;                                   /* MOV R1,#&9D */
    basicvfp_CHECKFILL(s, &r0, r1, &r10, &r12, r13, r14); /* BL CHECKFILL */
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r3 = ros_ld32(r13);                         /* LDMFD SP!,{R3,R4,TYPE} */
    r4 = ros_ld32(r13 + 4);
    r9 = ros_ld32(r13 + 8);
    r13 += 12;
    r5 = r4 + r0;                               /* ADD R5,R4,IACC */
    r0 = 4;                                     /* MOV R0,#4 */
    r1 = r4;                                    /* MOV R1,R4 */
    r2 = r3;                                    /* MOV R2,R3 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; move x,y */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = r9;                                    /* MOV R0,TYPE */
    r1 = r5;                                    /* MOV R1,R5 */
    s->r[0] = r0; s->r[1] = r1;                 /* BL DOPLOT ; q,x+w,y */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[9] = r9;
    DCI_TO_NXT(s);                              /* B NXT */
}

void basicvfp_hand_ELLIPSE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    double d0, d1, d2, dv;

    r0 = 197;                                   /* MOV R0,#&C5 */
    r1 = 205;                                   /* MOV R1,#&CD */
    basicvfp_CHECKFILL(s, &r0, r1, &r10, &r12, r13, r14); /* BL CHECKFILL */
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 == 44) goto ELLIPSEANGLE;           /* BEQ ELLIPSEANGLE */
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r5 = ros_ld32(r13);                         /* LDMFD SP!,{R5,R6,R7,TYPE} */
    r6 = ros_ld32(r13 + 4);
    r7 = ros_ld32(r13 + 8);
    r9 = ros_ld32(r13 + 12);
    r13 += 16;
    r3 = r0;                                    /* MOV R3,R0 */
    r4 = 0;                                     /* MOV R4,#0 */
    goto ELLIPSEDO;                             /* B ELLIPSEDO */

ELLIPSEANGLE:
    s->r[14] = 0xFFFFFFF0u;                     /* BL IFLT */
    basicvfp_hand_IFLT(s);
    s->r[14] = 0xFFFFFFF0u;                     /* BL FPUSH ; min */
    basicvfp_hand_FPUSH(s);
    r13 = s->r[13];
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL FLOATY */
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL FPUSH ; ang */
    basicvfp_hand_FPUSH(s);
    r13 = s->r[13]; r14 = s->r[14];
    /* Convert to OS_Plot commands, by the algorithm in the original's
     * comment (Stmt2.s:626-829).  sin and cos come from the VFPSupport
     * table by BLX.  Every intermediate keeps the original's stack
     * slot. */
    r13 -= 4;                                   /* STR LINE,[SP,#-4]! */
    ros_st32(r13, r12);
    r12 = ros_ld32(r8 - 8);                     /* VFPTRANSCENDENTALS */
    s->r[12] = r12; s->r[13] = r13;             /* BLX ip ; sin */
    s->r[14] = 0xFC106D54u;
    ros_call(s, r12);
    if (s->r[15] != 0xFC106D54u) ros_bad_return(s, 0xFC106D54u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r12 = ros_ld32(r13);                        /* LDR LINE,[SP],#4 */
    ros_std(r13 - 4, s->fp->vfp.d[0]);          /* FSTD FACC,[SP,#-8]! */
    d0 = ros_ldd(r13 + 4);                      /* FLDD FACC,[SP,#8] */
    r13 -= 8;                                   /* STR LINE,[SP,#-4]! */
    ros_st32(r13, r12);
    r12 = ros_ld32(r8 - 8) + 4;                 /* VFPSupport_Fn_cos */
    s->r[12] = r12; s->r[13] = r13;             /* BLX ip ; cos */
    s->r[14] = 0xFC106D70u;
    ros_call(s, r12);
    if (s->r[15] != 0xFC106D70u) ros_bad_return(s, 0xFC106D70u);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13];
    r12 = ros_ld32(r13);                        /* LDR LINE,[SP],#4 */
    r13 += 4;
    ros_std(r13 + 8, s->fp->vfp.d[0]);          /* FSTD FACC,[SP,#8] */
    r0 = ros_ld32(r13 + 24);                    /* LDR IACC,[SP,#24] ; maj% */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL IFLT */
    basicvfp_hand_IFLT(s);
    ros_std(r13 - 8, s->fp->vfp.d[0]);          /* FSTD FACC,[SP,#-8]! */
    d1 = ros_ldd(r13 + 16);                     /* FLDD D1,[SP,#24] ; min */
    dv = d1 * s->fp->vfp.d[0];                  /* FMULD FACC,D1,FACC */
    s->fp->fpscr |= ros_vfp_ex2(dv, d1, s->fp->vfp.d[0]);
    d0 = dv;
    ros_std(r13 - 16, d0);                      /* FSTD FACC,[SP,#-8]! */
    d2 = ros_ldd(r13 + 8);                      /* FLDD D2,[SP,#24] ; cos */
    dv = d2 * d1;                               /* FMULD D2,D2,D1 ; min*cos */
    s->fp->fpscr |= ros_vfp_ex2(dv, d2, d1);
    d2 = dv;
    dv = d2 * d2;                               /* (min*cos)^2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d2, d2);
    d2 = dv;
    r13 -= 24;                                  /* FSTD D2,[SP,#-8]! */
    ros_std(r13, d2);
    d1 = ros_ldd(r13 + 16);                     /* FLDD D1,[SP,#16] ; maj */
    d0 = ros_ldd(r13 + 24);                     /* FLDD FACC,[SP,#24] ; sin */
    dv = d1 * d0;                               /* FMULD FACC,D1,FACC */
    s->fp->fpscr |= ros_vfp_ex2(dv, d1, d0);
    d0 = dv;
    dv = d0 * d0;                               /* (maj*sin)^2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d0, d0);
    d0 = dv;
    dv = d0 + d2;                               /* FADDD FACC,FACC,D2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d0, d2);
    d0 = dv;
    s->fp->fpscr |= ros_vfp_sqrt_ex(d0);        /* FSQRTD FACC,FACC */
    d0 = sqrt(d0);
    ros_std(r13, d0);                           /* FSTD FACC,[SP] ; maxy */
    d2 = ros_ldd(r13 + 8);                      /* FLDD D2,[SP,#8] ; slicet */
    dv = d2 / d0;                               /* FDIVD D2,D2,FACC */
    s->fp->fpscr |= ros_vfp_div_ex(dv, d2, d0);
    d2 = dv;
    s->fp->fpscr |= ros_vfp_int_ex(d2, ROS_ROUND_ZERO, 1); /* FTOSIZD S4 */
    s->fp->vfp.sw[4] = (uint32_t)ros_to_int(d2, ROS_ROUND_ZERO);
    ros_sts(r13 + 48, s->fp->vfp.s[4]);         /* FSTS S4,[SP,#48] ; slicew */
    dv = d1 * d1;                               /* FMULD D1,D1,D1 ; maj^2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d1, d1);
    d1 = dv;
    d2 = ros_ldd(r13 + 40);                     /* FLDD D2,[SP,#40] ; min */
    dv = d2 * d2;                               /* FMULD D2,D2,D2 ; min^2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d2, d2);
    d2 = dv;
    dv = d1 - d2;                               /* FSUBD D2,D1,D2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d1, d2);
    d2 = dv;
    d1 = ros_ldd(r13 + 24);                     /* FLDD D1,[SP,#24] ; sin */
    dv = d1 * d2;                               /* FMULD D2,D1,D2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d1, d2);
    d2 = dv;
    d1 = ros_ldd(r13 + 32);                     /* FLDD D1,[SP,#32] ; cos */
    dv = d1 * d2;                               /* FMULD D2,D1,D2 */
    s->fp->fpscr |= ros_vfp_ex2(dv, d1, d2);
    d2 = dv;
    ros_std(r13 + 8, d2);                       /* FSTD D2,[SP,#8] */
    s->fp->fpscr |= ros_vfp_div_ex(d2 / d0, d2, d0); /* FDIVD ; sheart/maxy */
    d0 = d2 / d0;
    s->r[12] = r12; s->r[13] = r13;             /* BL SFIX */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SFIX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_st32(r13 + 44, r0);                     /* STR IACC,[SP,#44] ; shearx */
    s->r[14] = 0xFFFFFFF0u;                     /* BL FPULL */
    basicvfp_hand_FPULL(s);
    r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL SFIX */
    basicvfp_hand_SFIX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r14 = s->fp->fpscr;                         /* FMRX R14,FPSCR */
    ros_logic(s, r14 & 7, s->c);                /* TST IOC+DZC+OFC */
    if (!s->z) {                                /* BNE VFPException */
        s->r[14] = r14;
        s->r[15] = s->r[14];
        basicvfp_VFPException(s);
        return;
    }
    ros_st32(r13 + 32, r0);                     /* STR IACC,[SP,#32]! */
    r3 = ros_ld32(r13 + 32);                    /* LDMFD SP!,{R3-R7,TYPE} */
    r4 = ros_ld32(r13 + 36);
    r5 = ros_ld32(r13 + 40);
    r6 = ros_ld32(r13 + 44);
    r7 = ros_ld32(r13 + 48);
    r9 = ros_ld32(r13 + 52);
    r13 += 56;

ELLIPSEDO:
    r0 = 4;                                     /* MOV R0,#4 */
    r1 = r7;                                    /* MOV R1,R7 */
    r2 = r6;                                    /* MOV R2,R6 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; move x,y */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = 4;                                     /* MOV R0,#4 */
    r1 = r5 + r7;                               /* ADD R1,R5,R7 */
    s->r[0] = r0; s->r[1] = r1;                 /* BL DOPLOT ; x+slicew,y */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = r9;                                    /* MOV R0,TYPE */
    r1 = r4 + r7;                               /* ADD R1,R4,R7 */
    r2 = r3 + r6;                               /* ADD R2,R3,R6 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;   /* BL DOPLOT ; q,x+shearx */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
    s->r[9] = r9;
    DCI_TO_NXT(s);                              /* B NXT */
}

/* =====================================================================
 * RECT (Stmt2.s:1565-1630) and DOTINT (Stmt2.s:1631-1636).
 * ===================================================================== */
void basicvfp_hand_RECT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    r0 = 0;                                     /* MOV R0,#0 */
    r1 = 101;                                   /* MOV R1,#&65 */
    basicvfp_CHECKFILL(s, &r0, r1, &r10, &r12, r13, r14); /* BL CHECKFILL */
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! ; X */
    ros_st32(r13, r0);
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! ; Y */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! ; W */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44) goto RECTSIMPLE;             /* BNE RECTSIMPLE */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    /* fall RECTSIMPLE */

RECTSIMPLE:
    v = r13;                                    /* LDMFD SP!,{R3,R4,R5} */
    r4 = ros_ld32(r13 + 4);
    r5 = ros_ld32(r13 + 8);
    r13 += 12;
    r3 = r5 + ros_ld32(v);                      /* ADD R3,R5,R3 ; X+W */
    r6 = r4 + r0;                               /* ADD R6,R4,IACC ; Y+H */
    ros_subs(s, r10, TTO);                      /* CMP R10,#TTO */
    if (r10 == TTO) goto RECTMOVE;              /* BEQ RECTMOVE */
    s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r0 = 4;                                     /* MOV R0,#4 */
    r1 = r5;                                    /* MOV R1,R5 */
    r2 = r4;                                    /* MOV R2,R4 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; move x,y */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    v = r13;                                    /* LDR R0,[SP],#4 */
    r0 = ros_ld32(r13);
    r13 += 4;
    ros_logic(s, ros_ld32(v), s->c);            /* TEQ R0,#0 */
    if (r0 != 0) {                              /* MOVNE R2,R6 */
        r2 = r6;
        goto PLOTACT;                           /* BNE PLOTACT */
    }
    r0 = 13;                                    /* MOV R0,#13 */
    r1 = r3;                                    /* MOV R1,R3 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; x+w,y */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = 13;                                    /* MOV R0,#13 */
    r2 = r6;                                    /* MOV R2,R6 */
    s->r[0] = r0; s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; x+w,y+h */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = 13;                                    /* MOV R0,#13 */
    r1 = r5;                                    /* MOV R1,R5 */
    s->r[0] = r0; s->r[1] = r1;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; x,y+h */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = 13;                                    /* MOV R0,#13 */
    r2 = r4;                                    /* MOV R2,R4 */
    s->r[0] = r0; s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; x,y */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    DCI_TO_NXT(s);                              /* B NXT */

PLOTACT:  /* RECT's filled form re-enters the PLOT web's exit
           * (Stmt2.s:1545) with the popped action word in R0. */
    r1 = r3;                                    /* MOV R1,R3 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[3] = r3; s->r[4] = r4;
    DCI_TO_NXT(s);                              /* B NXT */

RECTMOVE:
    r13 -= 16;                  /* STMFD SP!,{R3,R4,R5,R6} ; x2,y,x,y2 */
    ros_st32(r13, r3);
    ros_st32(r13 + 4, r4);
    ros_st32(r13 + 8, r5);
    ros_st32(r13 + 12, r6);
    s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r8 = s->r[8]; r10 = s->r[10]; r12 = s->r[12];
    r13 = s->r[13];
    r3 = ros_ld32(r13);                         /* LDMFD SP!,{R3-R7,TYPE} */
    r4 = ros_ld32(r13 + 4);
    r5 = ros_ld32(r13 + 8);
    r6 = ros_ld32(r13 + 12);
    r7 = ros_ld32(r13 + 16);
    r9 = ros_ld32(r13 + 20);
    r13 += 24;
    r11 = r0;                                   /* MOV AELINE,IACC */
    r0 = 4;                                     /* MOV R0,#4 */
    r1 = r6;                                    /* MOV R1,R6 */
    r2 = r5;                                    /* MOV R2,R5 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; move x,y */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    r0 = 4;                                     /* MOV R0,#4 */
    r1 = r4;                                    /* MOV R1,R4 */
    r2 = r7;                                    /* MOV R2,R7 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DOPLOT ; move x2,y2 */
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    r0 = r9 == 0 ? 190 : 189;                   /* MOVEQ &BE / MOVNE &BD */
    r1 = r3;                                    /* MOV R1,R3 */
    r2 = r11;                                   /* MOV R2,AELINE */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;   /* BL DOPLOT ; q,x3,y3 */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_DOPLOT(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
    s->r[9] = r9;
    DCI_TO_NXT(s);                              /* B NXT */
}

/* DOTINT: the TINT statement (Stmt2.s:1631-1636).  It falls into the
 * shared TINTEND. */
void basicvfp_hand_DOTINT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXA */
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPRDN */
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = ros_ld32(r13);                         /* LDR IACC,[SP],#4 */
    r13 += 4;
    dci_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10,
                  r11, r12, r13, r14);
    dci_tintend(s);                             /* B TINTEND */
    return;
}
