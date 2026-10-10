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

/* dispat_control.c: the control-flow statements of the executor,
 * translated by hand from Stmt.s and Stmt2.s. These are from RISC OS
 * 5.31's BASIC, the VFP build. The statements are IF (with THENLN,
 * ELSE and ELSEBLK), ELSE2, FOR, NEXT, WHILE, ENDWH, REPEAT, UNTIL,
 * CASE, ON (with ONERR and ONERRF), GOTO, GOSUB, RETURN, END (with
 * END=CHANGE), ENDPR, STOP, TRACE (with TRACETO and TRACECLOSE),
 * QUIT, CHAIN and RUN. In the lift, as in the original, these are
 * labels inside the one DISPAT, and the token switch enters them.
 * This file has the shape they will take: one function per
 * statement, called from the switch in the hand-written dispatcher.
 *
 * The conventions are the ones the landed units use:
 *   - State travels in R[].
 *   - A BL becomes a marker call. R14 is set to 0xFFFFFFF0u, the
 *     changed locals are stored, and what the callee wrote is
 *     reloaded afterwards.
 *   - A B becomes a plain call whose result is discarded. The
 *     compiler turns it back into a branch.
 *   - Routines that use setjmp (EXPR, AEEXPR, DOEXCEPTION, LOADER and
 *     so on) are plain calls with no resume points.
 *
 * The quirks of the original that are kept, with their lines:
 *   - THENLN's line-number cache stores FNDLNO's leftover R2 in the
 *     delta slot (Stmt.s:640-644). R2 holds the low byte of the found
 *     line's number. This does no harm, because GOFACT is the only
 *     reader of the delta and it never probes at a THENLN key. A
 *     token position is after THEN or after GOTO, but never both.
 *   - IF's scan for ELSE reads two bytes per turn in the odd places.
 *     ELSELP and ELSELP1 each read one byte in turn (Stmt.s:655-665),
 *     so a colon before ELSE is found only on even alignments. This
 *     is kept as the original has it, and so is the dead ELSELP label
 *     at its head.
 *   - The block-IF scanners count a THEN at the end of a line as an
 *     opener ([R2,#-1], Stmt.s:676/703). For ELSEBLK the count must
 *     reach exactly 1 (SUBEQS, Stmt.s:687). For ELSE2 it need only go
 *     negative (SUBS/BPL, Stmt.s:714-716).
 *   - NEXT's variable cache holds an array element as delta|TFP, with
 *     TFP = &80000000 (Stmt.s:884). The path for a misaligned
 *     variable, SLONXT (Stmt.s:2561-2572, the NoARMv6 build), joins
 *     the two words that cover it by shifting them by hand.
 *   - WHILE's skip scan counts a WHILE only when the byte before the
 *     token is TESCSTMT (Stmt.s:3977-3980, "to disassociate ACS").
 *     DATA and REM set the in-string state to 4. A quote can toggle
 *     that state but cannot close it (Stmt.s:3969-3972).
 *   - ON's empty form, with ELSE, ":" or CR after the token, turns on
 *     the cursor (Stmt.s:1041-1044). It leaves through Group C's
 *     CURSON, which is declared extern here.
 *   - GOSUB's stack check passes when FSA+1024 >= SP (BCC STMT,
 *     Stmt.s:566-568). So exactly equal is allowed.
 *   - RETURN reads the byte before the stacked restart point as the
 *     statement separator (Stmt.s:1361-1364) and hands it to NXT.
 *   - TRACE's flag word TRCNUM keeps the step bit in TINTEGER and the
 *     line number in the low 16 bits (Stmt.s:1651-1656). TRACE TO
 *     clears the stored handle before it tries to close the old file,
 *     so an error from the close leaves no handle behind. It then
 *     opens the new file (Stmt.s:1671-1689).
 *   - QUIT stores the "ABEX" magic word and the return code in the
 *     workspace before OS_Exit (Stmt2.s:2466-2471).
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "dispat_control.h"

/* ---- the lift's functions this unit calls --------------------------
 * (The manifest's EXPORTS list makes them non-static in the twin.) */
void basicvfp_DISPAT(struct ros_cpu *s);
void basicvfp_ERSYNT(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_ERTYPESTR(struct ros_cpu *s);
void basicvfp_NOLINE(struct ros_cpu *s);
void basicvfp_ERDEEPNEST(struct ros_cpu *s);
void basicvfp_CLRSTK(struct ros_cpu *s);
void basicvfp_DOEXCEPTION(struct ros_cpu *s);
void basicvfp_AEEXPR(struct ros_cpu *s);
void basicvfp_AEEXDN(struct ros_cpu *s);
void basicvfp_EXPRDN(struct ros_cpu *s);
void basicvfp_CRAELV(struct ros_cpu *s);
void basicvfp_MUNGLE(struct ros_cpu *s);
void basicvfp_GOFACT(struct ros_cpu *s);
void basicvfp_FNDLNO(struct ros_cpu *s);
void basicvfp_ONSKIP(struct ros_cpu *s, uint32_t *p2, uint32_t r11,
                     uint32_t *p12, uint32_t r14);
void basicvfp_POP(struct ros_cpu *s);
void basicvfp_POPA(struct ros_cpu *s);
void basicvfp_ARLOOKCACHE(struct ros_cpu *s);
void basicvfp_RUNNER(struct ros_cpu *s);
void basicvfp_LOADER(struct ros_cpu *s);
void basicvfp_ENDER(struct ros_cpu *s);
void basicvfp_ENDTRC(struct ros_cpu *s);
void basicvfp_STORER0MISAL(struct ros_cpu *s);

/* The hand units' functions (landed). */
void basicvfp_EXPR(struct ros_cpu *s);
void basicvfp_hand_STMT(struct ros_cpu *s);
void basicvfp_hand_CRLINE(struct ros_cpu *s);
void basicvfp_hand_DATA(struct ros_cpu *s);
void basicvfp_hand_NXT(struct ros_cpu *s);
void basicvfp_hand_DONEXT(struct ros_cpu *s);
void basicvfp_hand_DONXTS(struct ros_cpu *s);
void basicvfp_hand_DONES(struct ros_cpu *s);
void basicvfp_hand_DONE(struct ros_cpu *s);
void basicvfp_hand_AEDONE(struct ros_cpu *s);
void basicvfp_hand_AESPAC(struct ros_cpu *s);
void basicvfp_hand_SPACES(struct ros_cpu *s, uint32_t *p10, uint32_t *p12,
                          uint32_t r14);
void basicvfp_hand_SPGETN(struct ros_cpu *s);
void basicvfp_hand_OSSTRI(struct ros_cpu *s);
void basicvfp_hand_STORE(struct ros_cpu *s);
void basicvfp_hand_LVNOTCACHE(struct ros_cpu *s);
void basicvfp_hand_INTEGY(struct ros_cpu *s);
void basicvfp_hand_FLOATY(struct ros_cpu *s);
void basicvfp_hand_FLOATZ(struct ros_cpu *s);
void basicvfp_hand_SFIX(struct ros_cpu *s);
void basicvfp_hand_PUSHTYPE(struct ros_cpu *s);
void basicvfp_hand_PULLTYPE(struct ros_cpu *s);
void basicvfp_hand_GTARGS(struct ros_cpu *s);
void basicvfp_hand_FNBODY(struct ros_cpu *s);
void basicvfp_hand_MSG(struct ros_cpu *s);
void basicvfp_hand_FLUSHCACHE(struct ros_cpu *s, uint32_t *p0,
                              uint32_t *p1, uint32_t *p2, uint32_t r8,
                              uint32_t r14);
void basicvfp_hand_ORDERR(struct ros_cpu *s, uint32_t *p0, uint32_t r8,
                          uint32_t r14);

/* Group C's CURSOFF unit owns CURSON (the cursor-on VDU string,
 * Stmt.s's Command fragment); ON's empty form leaves through it.
 * The integration wires it. */
void basicvfp_hand_CURSON(struct ros_cpu *s, uint32_t ret_to);

/* The OS thunks. The harness's api_gen declares them, and each unit
 * declares them again, as the landed units do. */
void ros_thunk_OS_Exit(struct ros_cpu *s);
void ros_thunk_OS_Find(struct ros_cpu *s);
void ros_thunk_OS_File(struct ros_cpu *s);

/* Tokens, as the lift defines them from hdr/Tokens. Other units
 * define the same values, and identical redefinitions do no harm. */
#define TELSE   139u
#define TELSE2  204u
#define TENDIF  205u
#define TCONST  141u
#define TTHEN   140u
#define TNEXT   237u
#define TFOR    227u
#define TTO     184u
#define TSTEP   136u
#define TRETURN 248u
#define TUNTIL  253u
#define TENDWH  206u
#define TERROR  133u
#define TOFF    135u
#define TLOCAL  234u
#define TDATA   220u
#define TPROC   242u
#define TGOTO   229u
#define TGOSUB  228u
#define TENDCA  203u
#define TWHEN   201u
#define TOTHER  127u
#define TOF     202u
#define TENDPR  225u
#define TFN     164u
#define TCLOSE  217u
#define TON     238u
#define TREM    244u
#define TDEF    221u
#define TESCSTMT 200u
#define TWHILE  149u

/* Type bits and the cache geometry (hdr/Definitions). */
#define TFP     0x80000000u   /* float flag; NEXT's cache delta marker */
#define TFPLV   8u            /* FP l-value type, FPOINT=2 */
#define TINTEGER 0x40000000u  /* integer bit; TRACE's step bit */
#define TEFP    0x20000000u
#define CACHEMASK  255u
#define CACHESHIFT 4

/* The real addresses of the MSG error sites. MSG reads its error
 * number and token from the words after each address. There is one
 * for each DISPAT error label this unit reaches. */
#define SITE_ERNEXT    0xFC1100D4u
#define SITE_NEXTER    0xFC1100DCu
#define SITE_FORCV     0xFC1100E4u
#define SITE_FORSTEP   0xFC1100ECu
#define SITE_FORTO     0xFC1100F4u
#define SITE_ERGOSB    0xFC110104u
#define SITE_ONER      0xFC11010Cu
#define SITE_ONRGER    0xFC110114u
#define SITE_ERREPT    0xFC110134u
#define SITE_ERWHIL    0xFC11014Cu
#define SITE_NOENDC    0xFC110154u
#define SITE_ERCASE1   0xFC11015Cu
#define SITE_ERCASE    0xFC110164u
#define SITE_NOENDI    0xFC11016Cu
#define SITE_ERSTOP    0xFC10FE64u
#define SITE_MISSEQFOR 0xFC10FEFCu
#define SITE_ERREND    0xFC10FFCCu
#define SITE_ERRENDARRAYREF 0xFC10FFD4u
#define SITE_ENDPRE    0xFC10FFECu
#define SITE_ERMVSTK   0xFC1101C4u

/* The error exits hand the block to MSG, which never returns. The
 * fault catches a MSG that does return. */
static void stmt_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* THENLN (Stmt.s:626-648) is shared by IF and ON. IF falls into it.
 * ON branches here when its index runs past the end of the list
 * (Stmt.s:2750-2752). LINE points just past a THEN or ELSE token.
 * THENLN skips spaces and looks at the next token:
 *   - If it is a line-number constant, check ESCWORD and probe the
 *     line cache. On a miss, read the number, find the line and fill
 *     the cache. Then enter the line.
 *   - Otherwise dispatch it as the statement after the ELSE.
 * State is in R[]. */
static void stmt_thenln(struct ros_cpu *s)
{
    uint32_t r1, r2, r3, r4, r8 = s->r[8], r10 = s->r[10],
             r11 = s->r[11], r12 = s->r[12], r13 = s->r[13];

THENLN:
    r10 = ros_ld8(r12);                       /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if (r10 == 32) goto THENLN;               /* CMP R10,#" " ; BEQ THENLN */
    ros_subs(s, r10, TCONST);                 /* CMP R10,#TCONST */
    if (r10 != TCONST) {                      /* BNE DISPAT */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_DISPAT(s);
        return;
    }
    r4 = ros_ld32(r8 - 112);                  /* LDR R4,[ARGP,#ESCWORD] */
    ros_subs(s, r4, 0);                       /* CMP R4,#0 */
    if (r4 != 0) {                            /* BLNE DOEXCEPTION */
        s->r[4] = r4;
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_DOEXCEPTION(s);
        r8 = s->r[8];
        r10 = s->r[10];
        r11 = s->r[11];
        r12 = s->r[12];
        r13 = s->r[13];
    }
    r1 = r8 + ((r12 & CACHEMASK) << CACHESHIFT); /* AND R1,LINE,#CACHEMASK */
    r2 = ros_ld32(r1 + 4);                    /* LDMIA R1,{R1,R2,R4} */
    r4 = ros_ld32(r1 + 8);
    r1 = ros_ld32(r1);
    ros_subs(s, r4, r12);                     /* CMP R4,LINE */
    if (r4 == r12) r12 = r1 + 4;              /* ADDEQ LINE,R1,#4 */
    if (s->z) {                               /* BEQ STMT */
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    s->r[10] = r10;                           /* BL SPGETN */
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPGETN(s);
    r10 = s->r[10];
    r12 = s->r[12];
    s->r[2] = r2;                             /* BL FNDLNO */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_FNDLNO(s);
    r1 = s->r[1];
    r2 = s->r[2];
    r3 = s->r[3];
    if (!s->c) {                              /* BCC NOLINE */
        s->r[4] = r4;
        s->r[11] = r11;
        s->r[15] = s->r[14];
        basicvfp_NOLINE(s);
        return;
    }
    r4 = r12 - 3;                             /* SUB R4,LINE,#3 */
    r3 = r8 + ((r4 & CACHEMASK) << CACHESHIFT);
    ros_st32(r3, r1);                         /* STMIA R3,{R1,R2,R4} */
    ros_st32(r3 + 4, r2);
    ros_st32(r3 + 8, r4);
    r12 = r1 + 4;                             /* ADD LINE,R1,#4 */
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);                    /* B STMT */
    return;
}

/* IF (Stmt.s:617-698): evaluate the condition. If it is true, run the
 * statement after THEN through THENLN, which has the shortcut for a
 * line-number constant. If it is false, skip to the ELSE (at ELSE).
 * A THEN at the end of the line starts a block scan (ELSEBLK). */
void basicvfp_hand_IF(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r8 = s->r[8], r9 = s->r[9], r10 = s->r[10],
             r11 = s->r[11], r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    s->r[0] = r0;                             /* BL AEEXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];

    ros_logic(s, r9, s->c);                   /* TEQ TYPE,#0 */
    if (r9 == 0) {                            /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    if ((int32_t)r9 < 0) {                    /* BLMI SFIX */
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_SFIX(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r13 = s->r[13];                       /* SFIX does not touch LINE */
    }
    r12 = r11;                                /* MOV LINE,AELINE */
    if (r0 == 0) goto ELSE;                   /* CMP IACC,#0 ; BEQ ELSE */
    ros_subs(s, r10, TTHEN);                  /* CMP R10,#TTHEN */
    if (r10 != TTHEN) {                       /* BNE DISPAT */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_DISPAT(s);
        return;
    }
    /* THENLN: the shared machinery, with IF's live state. */
    s->r[8] = r8;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = r14;
    stmt_thenln(s);
    return;

ELSE:
    if (r10 == 13) {                          /* TEQ R10,#13 ; BEQ CRLINE */
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_CRLINE(s);
        return;
    }
    if (r10 == TELSE)                         /* TEQ R10,#TELSE */
        goto thenln_shared;                   /* BEQ THENLN */
    if (r10 == TTHEN)                         /* TEQ R10,#TTHEN */
        goto ELSEBLK;                         /* BEQ ELSEBLK */
ELSELP:
    r10 = ros_ld8(r12);                       /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if (r10 == 13) {                          /* CMP R10,#13 ; BEQ CRLINE */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_CRLINE(s);
        return;
    }
ELSELP1:
    if (r10 == TELSE)                         /* CMP R10,#TELSE ; BEQ THENLN */
        goto thenln_shared;
    r10 = ros_ld8(r12);                       /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if (r10 == 13) {                          /* CMP R10,#13 ; BEQ CRLINE */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_CRLINE(s);
        return;
    }
    if (r10 != TELSE)                         /* CMP R10,#TELSE ; BNE ELSELP */
        goto ELSELP;
    goto thenln_shared;

thenln_shared:
    s->r[8] = r8;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = r14;
    stmt_thenln(s);
    return;

ELSEBLK:
    r10 = ros_ld8(r12);                       /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if (r10 != 13)                            /* TEQ R10,#13 */
        goto ELSELP1;                         /* BNE ELSELP1: not block form */
    r2 = r12 - 1;                             /* SUB R2,LINE,#1: the CR */
    r3 = 0;                                   /* MOV R3,#0: nesting count */
    goto elseblk_01;                          /* B %01 */
elseblk_00: ;
    r2 += ros_ld8(r2 + 3);                    /* LDRB R0,[R2,#3]; ADD R2,R2,R0 */
elseblk_01:
    r0 = ros_ld8(r2 + 1);                     /* LDRB R0,[R2,#1] */
    ros_subs(s, r0, 0xFFu);                   /* CMP R0,#&FF */
    if (r0 >= 0xFFu)                          /* BCS NOENDI */
        stmt_msg_at(s, SITE_NOENDI);
    if (ros_ld8(r2 - 1) == TTHEN)             /* LDRB R0,[R2,#-1]; CMP #TTHEN */
        r3 += 1;                              /* ADDEQ R3,R3,#1 */
    r1 = r2 + 4;                              /* ADD R1,R2,#4 */
elseblk_02:
    r0 = ros_ld8(r1);                         /* LDRB R0,[R1],#1 */
    r1 += 1;
    if (r0 == 32)                             /* TEQ R0,#" " ; BEQ %02 */
        goto elseblk_02;
    v = r3;                                   /* SUBEQS R3,R3,#1 */
    if (r0 == TENDIF)
        r3 -= 1;
    if (r0 == TENDIF && v == 1) {             /* MOVEQ LINE,R1; BEQ ENDIF */
        r12 = r1;                             /* MOVEQ LINE,R1: past the
                                               * ENDIF. Without this store
                                               * LINE was left at the IF's
                                               * own CR, and DONXTS read the
                                               * next line's header as a
                                               * token. */
        s->r[0] = r0;
        s->r[1] = r1;
        s->r[2] = r2;
        s->r[3] = r3;
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_DONXTS(s);
        return;
    }
    ros_subs(s, r0, TELSE2);                  /* CMP R0,#TELSE2 */
    if (r0 != TELSE2)                         /* BNE %00 */
        goto elseblk_00;
    ros_logic(s, r3 ^ 1, s->c);               /* TEQ R3,#1 */
    if (!s->z)                                /* BNE %00 */
        goto elseblk_00;
    r12 = r1;                                 /* MOV LINE,R1 */
    s->r[0] = r0;                             /* B STMT */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[10] = r10;
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
    return;
}

/* ELSE2 (Stmt.s:696-716) is the block ELSE run as a statement. Skip to
 * the ENDIF that closes the IF this ELSE belongs to, which is where
 * the count goes negative. Then carry on after it. */
void basicvfp_hand_ELSE2(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r10 = s->r[10], r12 = s->r[12];

    do {
        r10 = ros_ld8(r12);                   /* LDRB R10,[LINE],#1 */
        r12 += 1;
    } while (r10 != 13);                      /* TEQ R10,#13 ; BNE ELSE2 */
    r3 = 0;                                   /* MOV R3,#0 */
    r2 = r12 - 1;                             /* SUB R2,LINE,#1: the CR */
    goto else2_01;                            /* B %01 */
else2_00: ;
    r2 += ros_ld8(r2 + 3);                    /* LDRB R0,[R2,#3]; ADD R2,R2,R0 */
else2_01:
    r0 = ros_ld8(r2 + 1);                     /* LDRB R0,[R2,#1] */
    ros_subs(s, r0, 0xFFu);                   /* CMP R0,#&FF */
    if (r0 >= 0xFFu)                          /* BCS NOENDI */
        stmt_msg_at(s, SITE_NOENDI);
    if (ros_ld8(r2 - 1) == TTHEN)             /* nested block IF opener */
        r3 += 1;                              /* ADDEQ R3,R3,#1 */
    r1 = r2 + 4;                              /* ADD R1,R2,#4 */
else2_02:
    r0 = ros_ld8(r1);                         /* LDRB R0,[R1],#1 */
    r1 += 1;
    if (r0 == 32)                             /* TEQ R0,#" " ; BEQ %02 */
        goto else2_02;
    if (r0 != TENDIF)                         /* TEQ R0,#TENDIF ; BNE %00 */
        goto else2_00;
    r3 -= 1;                                  /* SUBS R3,R3,#1 */
    if ((int32_t)r3 >= 0)                     /* BPL %00 */
        goto else2_00;
    r12 = r1;                                 /* MOV LINE,R1 */
    s->r[0] = r0;                             /* B DONXTS */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[10] = r10;
    s->r[11] = s->r[11];
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_DONXTS(s);
    return;
}

/* FOR (Stmt.s:478-545): create the variable and evaluate the start
 * value (STORE), the limit and the step. Then stack the frame:
 *   - {TNEXT,VAR,ADDR,STEP,LIMIT} for an integer loop
 *   - {TFOR,VAR,ADDR,STEP(8),LIMIT(8)} for a float loop.
 * MUNGLE gives the restart position that goes in the frame. */
void basicvfp_hand_FOR(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3, r4 = s->r[4], r5, r6, r7,
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    s->r[0] = r0;                             /* BL CRAELV */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (s->z)                                 /* BEQ FORCV: no LV made */
        stmt_msg_at(s, SITE_FORCV);
    ros_subs(s, r9, TFPLV + 1);               /* CMP TYPE,#TFPLV+1 */
    if (r9 >= TFPLV + 1)                      /* BCS FORCV: not numeric */
        stmt_msg_at(s, SITE_FORCV);
    r13 -= 8;                                 /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[14] = 0xFFFFFFF0u;                   /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    ros_subs(s, r10, 61);                     /* CMP R10,#"=" */
    if (r10 != 61)                            /* BNE MISSEQFOR */
        stmt_msg_at(s, SITE_MISSEQFOR);
    s->r[13] = r13;                           /* BL EXPR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_subs(s, r10, TTO);                    /* CMP R10,#TTO */
    if (r10 != TTO)                           /* BNE FORTO */
        stmt_msg_at(s, SITE_FORTO);
    s->r[14] = 0xFFFFFFF0u;                   /* BL STORE: the start */
    basicvfp_hand_STORE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_subs(s, r5, TFPLV);                   /* CMP R5,#TFPLV */
    if (r5 == TFPLV)                          /* BEQ FFOR */
        goto FFOR;
    r0 = TNEXT;                               /* MOV R0,#TNEXT */
    r6 = 1;                                   /* MOV R6,#1 */
    r13 -= 20;                                /* STMFD SP!,{R0,R4,R5,R6,R7} */
    ros_st32(r13, TNEXT);
    ros_st32(r13 + 4, r4);
    ros_st32(r13 + 8, r5);
    ros_st32(r13 + 12, 1);
    ros_st32(r13 + 16, r7);
    s->r[0] = r0;                             /* BL EXPR */
    s->r[6] = r6;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_st32(r13 + 16, r0);                   /* STR IACC,[SP,#4*4]: limit */
    ros_subs(s, r10, TSTEP);                  /* CMP R10,#TSTEP */
    if (r10 != TSTEP)                         /* BNE FORSTW */
        goto FORSTW;
    s->r[13] = r13;                           /* BL EXPR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_logic(s, r0, s->c);                   /* TEQ IACC,#0 */
    if (r0 == 0)                              /* BEQ FORSTEP */
        stmt_msg_at(s, SITE_FORSTEP);
    ros_st32(r13 + 12, r0);                   /* STR IACC,[SP,#3*4]: step */
FORSTW:
    s->r[14] = 0xFFFFFFF0u;                   /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r12 -= 1;                                 /* SUB LINE,LINE,#1 */
    s->r[12] = r12;                           /* BL MUNGLE */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_MUNGLE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_st32(r13 + 8, r12);                   /* STR LINE,[SP,#2*4]: restart */
    s->r[0] = r0;                             /* B STMT */
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
    return;

FFOR:
    r0 = TFOR;                                /* MOV R0,#TFOR */
    r13 -= 28;                                /* STMFD SP!,{R0,R4,R5,R6,R7,R8,R9} */
    ros_st32(r13, TFOR);
    ros_st32(r13 + 4, r4);
    ros_st32(r13 + 8, r5);
    ros_st32(r13 + 12, r6);
    ros_st32(r13 + 16, r7);
    ros_st32(r13 + 20, r8);
    ros_st32(r13 + 24, r9);
    s->r[0] = r0;                             /* BL EXPR */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL FLOATY */
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_std(r13 + 20, s->fp->vfp.d[0]);       /* FSTD FACC,[SP,#20]: limit */
    s->fp->vfp.d[0] = 1.0;                    /* FLDD FACC,=1: step default */
    ros_subs(s, r10, TSTEP);                  /* CMP R10,#TSTEP */
    if (r10 != TSTEP)                         /* BNE FORSTA */
        goto FORSTA;
    s->r[13] = r13;                           /* BL EXPR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL FLOATY */
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->fp->fpscr |= ros_vfp_cmp_ex(s->fp->vfp.d[0], 0.0, 0); /* FCMPZD FACC */
    ros_vfp_cmp(s, s->fp->vfp.d[0], 0.0);     /* FMRX PC,FPSCR */
    ros_vmrs_flags(s);
    if (s->fp->vfp.d[0] == 0.0)               /* BEQ FORSTEP */
        stmt_msg_at(s, SITE_FORSTEP);
FORSTA:
    ros_std(r13 + 12, s->fp->vfp.d[0]);       /* FSTD FACC,[SP,#12]: step */
    goto FORSTW;                              /* B FORSTW */
}

/* NEXT (Stmt.s:865-2585): find the loop variable. A NEXT with no
 * variable takes the frame on top of the stack. Match the variable
 * against the frame and step it. Then either jump back, or pop the
 * frame and finish. */
void basicvfp_hand_NEXT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];
    uint32_t v, w;
    double d0, d1;

NEXT:
    r4 = ros_ld32(r8 - 112);                  /* LDR R4,[ARGP,#ESCWORD] */
    ros_subs(s, r4, 0);                       /* CMP R4,#0 */
    if (r4 != 0) {                            /* BLNE DOEXCEPTION */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[9] = r9; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
        s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_DOEXCEPTION(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r12 = s->r[12]; r13 = s->r[13];
    }
    r11 = r12;                                /* MOV AELINE,LINE */
NEXTBLNK:
    r10 = ros_ld8(r11);                       /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == 32)                            /* CMP R10,#" " ; BEQ NEXTBLNK */
        goto NEXTBLNK;
    ros_subs(s, r10, 33);                     /* CMP R10,#"!" */
    if (r10 != 33)                            /* CMPNE R10,#"?" */
        ros_subs(s, r10, 0x3Fu);
    if (!s->c)                                /* BCC NEXM1: ignore : and CR */
        goto NEXM1;
    r1 = r8 + ((r11 & CACHEMASK) << CACHESHIFT); /* AND R1,AELINE,#CACHEMASK */
    r0 = ros_ld32(r1);                        /* LDMIA R1,{IACC,R1,R4,TYPE} */
    r4 = ros_ld32(r1 + 8);
    r9 = ros_ld32(r1 + 12);
    r1 = ros_ld32(r1 + 4);
    if (r4 != r11)                            /* CMP R4,AELINE */
        goto NEXTVBNOTCACHE;
    r11 += r1;                                /* CMN R1,#1; ADD AELINE,AELINE,R1 */
    if ((int32_t)(r1 + 1) >= 0)               /* BPL STRIPA */
        goto STRIPA;
    r11 -= TFP;                               /* SUB AELINE,AELINE,#TFP */
    s->r[0] = r0;                             /* BL ARLOOKCACHE */
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
    s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_ARLOOKCACHE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (!s->z)                                /* BNE STRIPA */
        goto STRIPA;
    goto NEXM1;                               /* B NEXM1 */

NEXTVBNOTCACHE:
    s->r[0] = r0;                             /* BL LVNOTCACHE */
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
    s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LVNOTCACHE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (!s->z)                                /* BNE STRIPA */
        goto STRIPA;
    if (!s->c) {                              /* BCC ERSYNT */
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
NEXM1:
    r4 = ros_ld32(r13);                       /* LDMFD SP,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    if (r4 == TNEXT)                          /* TEQ R4,#TNEXT ; BEQ NOCHKI */
        goto NOCHKI;
    if (r4 == TFOR)                           /* TEQ R4,#TFOR ; BEQ FNEXT */
        goto FNEXT;
    s->r[0] = r0;                             /* BL POPA */
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
    s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFC105900u;      /* the lift's own site, because POPA */
    basicvfp_POPA(s);            /* can transfer through R14 */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (s->z)                                 /* BEQ NEXM1 */
        goto NEXM1;
    stmt_msg_at(s, SITE_ERNEXT);              /* B ERNEXT */

STRIPA:
    ros_subs(s, r9, 128);                     /* CMP TYPE,#128 */
    if (r9 >= 128) {                          /* BCS ERSYNT */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[9] = r9; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    r12 = r11;                                /* MOV LINE,AELINE */
    r4 = ros_ld32(r13);                       /* LDMFD SP,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    if (r4 == TNEXT)                          /* TEQ R4,#TNEXT ; BEQ NEXSI */
        goto NEXSI;
    ros_logic(s, r4 ^ TFOR, s->c);            /* TEQ R4,#TFOR */
    if (!s->z)                                /* BNE ERNEXT */
        stmt_msg_at(s, SITE_ERNEXT);
    if (r0 == r5)                             /* TEQ IACC,R5 ; BEQ FNEXT */
        goto FNEXT;
NEXP2:
    s->r[0] = r0;                             /* BL POP */
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
    s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFC105934u;      /* the lift's own site, because POP */
    basicvfp_POP(s);             /* can transfer through R14 */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (!s->z)                                /* BNE NEXTER */
        stmt_msg_at(s, SITE_NEXTER);
    r4 = ros_ld32(r13);                       /* LDMFD SP,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    if (r4 == TFOR)                           /* TEQ R4,#TFOR ; BEQ NEXSF */
        goto NEXSF;
    ros_logic(s, r4 ^ TNEXT, s->c);           /* TEQ R4,#TNEXT */
    if (!s->z)                                /* BNE NEXTER */
        stmt_msg_at(s, SITE_NEXTER);
NEXSI:
    if (r0 != r5)                             /* TEQ IACC,R5 ; BNE NEXP2 */
        goto NEXP2;
NOCHKI:
    r3 = ros_ld32(r13);                       /* LDMFD SP,{R3,R4,R5,R6,R7} */
    r4 = ros_ld32(r13 + 4);
    r5 = ros_ld32(r13 + 8);
    r6 = ros_ld32(r13 + 12);
    r7 = ros_ld32(r13 + 16);
    r2 = r4 & 3;                              /* ANDS R2,R4,#3 */
    if (r2 != 0)                              /* BNE SLONXT */
        goto SLONXT;
    v = ros_ld32(r4);                         /* LDR IACC,[R4] */
    r0 = v;
    r0 = ros_adds(s, r0, r6);                 /* ADDS IACC,IACC,R6 */
    if (ros_addv(v, r6))                      /* BVS INXTPL */
        goto INXTPL;
    ros_st32(r4, r0);                         /* STR IACC,[R4] */
INXTPR: ;
    if ((int32_t)r6 < 0)                      /* TEQ R6,#0 ; BMI INXTCH */
        goto INXTCH;
    ros_subs(s, r0, r7);                      /* CMP IACC,R7: ascending */
INXTMT:
    if (ros_cond(s, ROS_LE))                  /* MOVLE LINE,R5 */
        r12 = r5;
    if (ros_cond(s, ROS_LE)) {                /* BLE STMT */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[9] = r9; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
INXTPL:
    r13 += 20;                                /* ADD SP,SP,#4*5 */
NEXTEX:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == 44)                            /* CMP R10,#"," ; BEQ NEXT */
        goto NEXT;
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_DONEXT(s);                  /* B DONEXT */
    return;
INXTCH:
    ros_subs(s, r7, r0);                      /* CMP R7,IACC: descending */
    goto INXTMT;                              /* B INXTMT */
SLONXT:
    /* The misaligned variable (the NoARMv6 build). Read the two words
     * that cover it and join them using the byte offset
     * (Stmt.s:2561-2572). */
    v = r4 & 3;                               /* ANDS R1,R4,#3 */
    r1 = v;
    r2 = r4 & ~3u;                            /* BIC R2,R4,#3 */
    w = r0;                                   /* LDREQ IACC,[R4] */
    if (r1 == 0)
        r0 = ros_ld32(r4);
    if (r1 != 0) {                            /* LDMNEIA R2,{IACC,R2} */
        r0 = ros_ld32(r2);
        r2 = ros_ld32(r2 + 4);
    }
    if (r1 != 0) {                            /* MOVNE R1,R1,LSL #3 */
        uint32_t sh = v << 3;                 /* RSBNE R1,R1,#32 */
        r0 = ros_lsr(r0, sh) | ros_lsl(r2, 32 - sh);
    }
    r0 = ros_adds(s, r0, r6);                 /* ADDS IACC,IACC,R6 */
    if (!s->v) {                              /* BLVC STORER0MISAL */
        s->r[0] = r0;
        s->r[1] = r1;
        s->r[4] = r4;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_STORER0MISAL(s);
        r1 = s->r[1];
    }
    if (!s->v)                                /* BVC INXTPR */
        goto INXTPR;
    goto INXTPL;                              /* B INXTPL */
NEXSF:
    if (r0 != r5)                             /* TEQ IACC,R5 ; BNE NEXP2 */
        goto NEXP2;
FNEXT:
    d0 = ros_ldd(r5);                         /* FLDD FACC,[R5] */
    d1 = ros_ldd(r13 + 12);                   /* FLDD D1,[SP,#12]: step */
    s->fp->fpscr |= ros_vfp_cmp_ex(d1, 0.0, 0); /* FCMPZD D1 */
    ros_vfp_cmp(s, d1, 0.0);
    {
        double sum = d1 + d0;                 /* FADDD FACC,D1,FACC */
        s->fp->fpscr |= ros_vfp_ex2(sum, d1, d0);
        d0 = sum;
    }
    ros_std(r5, d0);                          /* FSTD FACC,[R5] */
    s->fp->vfp.d[0] = d0;
    ros_vmrs_flags(s);                        /* FMRX PC,FPSCR */
    d1 = ros_ldd(r13 + 20);                   /* FLDD D1,[SP,#20]: limit */
    s->fp->vfp.d[1] = d1;
    if (s->n)                                 /* BMI FNEXTA: step < 0 */
        goto FNEXTA;
    s->fp->fpscr |= ros_vfp_cmp_ex(d0, d1, 0); /* FCMPD FACC,D1 */
    ros_vfp_cmp(s, d0, d1);
    ros_vmrs_flags(s);
    if (!(d0 > d1))                           /* LDRLE LINE,[SP,#8] */
        r12 = ros_ld32(r13 + 8);
    if (!(d0 > d1)) {                         /* BLE STMT */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[9] = r9; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    r13 += 28;                                /* ADD SP,SP,#4*7 */
    goto NEXTEX;                              /* B NEXTEX */
FNEXTA:
    s->fp->fpscr |= ros_vfp_cmp_ex(d0, d1, 0); /* FCMPD FACC,D1 */
    ros_vfp_cmp(s, d0, d1);
    ros_vmrs_flags(s);
    if (d0 >= d1)                             /* LDRGE LINE,[SP,#8] */
        r12 = ros_ld32(r13 + 8);
    if (d0 >= d1) {                           /* BGE STMT */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[9] = r9; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    r13 += 28;                                /* ADD SP,SP,#4*7 */
    goto NEXTEX;                              /* B NEXTEX */
}

/* WHILE (Stmt.s:1717-1731, with the skip for a false condition at
 * :3956-3982). If the condition is true, stack {TENDWH, body start}
 * and run the body. If it is false, skip forward to the matching
 * ENDWH. The skip counts a nested WHILE only when the token really is
 * the statement, that is when the byte before it is TESCSTMT. It
 * ignores everything inside strings, DATA and REM. */
void basicvfp_hand_WHILE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    r11 = r12 - 1;                            /* SUB AELINE,LINE,#1 */
    r13 -= 4;                                 /* STR AELINE,[SP,#-4]! */
    ros_st32(r13, r11);
    s->r[0] = r0;                             /* BL EXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r12 = r11 - 1;                            /* SUB LINE,AELINE,#1 */
    if (r0 == 0)                              /* CMP IACC,#0 ; BEQ EWHILE */
        goto EWHILE;
    s->r[12] = r12;                           /* BL MUNGLE */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_MUNGLE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r0 = TENDWH;                              /* MOV R0,#TENDWH */
    r13 -= 8;                                 /* STMFD SP!,{R0,LINE} */
    ros_st32(r13, TENDWH);
    ros_st32(r13 + 4, r12);
    s->r[0] = r0;                             /* B STMT */
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
    return;

EWHILX:
    r10 = ros_ld8(r12);                       /* LDRB R10,[LINE],#1 */
    r12 += 1;
    ros_subs(s, r10, 0xFFu);                  /* CMP R10,#&FF */
    if (r10 >= 0xFFu) {                       /* BCS CLRSTK */
        s->r[0] = r0;
        s->r[1] = r1;
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_CLRSTK(s);
        return;
    }
    r12 += 2;                                 /* ADD LINE,LINE,#2 */
EWHILE:
    r1 = 0;                                   /* MOV R1,#0: inside strings */
EWHILP:
    r10 = ros_ld8(r12);                       /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if (r10 == 13)                            /* CMP R10,#13 ; BEQ EWHILX */
        goto EWHILX;
    if (r10 == 34)                            /* CMP R10,#""""; EOREQ R1,R1,#1 */
        r1 ^= 1;
    if (r1 != 0)                              /* TEQ R1,#0 ; BNE EWHILP */
        goto EWHILP;
    if (r10 == TDATA || r10 == TREM) {        /* CMP/CMPNE; MOVEQ R1,#4 */
        r1 = 4;
        goto EWHILP;                          /* BEQ EWHILP */
    }
    if (r10 != TWHILE)                        /* CMP R10,#TWHILE ; BNE NOTWHILE */
        goto NOTWHILE;
    r10 = TWHILE;                             /* MOV R10,#TWHILE */
    if (ros_ld8(r12 - 2) == TESCSTMT)         /* LDRB R10,[LINE,#-2] */
        r0 += 1;                              /* ADDEQ IACC,IACC,#1 */
NOTWHILE: ;
    if (r10 == TENDWH)                        /* CMP R10,#TENDWH */
        r0 -= 1;                              /* SUBEQ IACC,IACC,#1 */
    if (r10 != TENDWH)                        /* BNE EWHILP */
        goto EWHILP;
    if (r0 != 0xFFFFFFFFu)                    /* CMN IACC,#1 ; BNE EWHILP */
        goto EWHILP;
    r13 += 4;                                 /* ADD SP,SP,#4*1 */
    s->r[0] = r0;                             /* BL DONES */
    s->r[1] = r1;
    s->r[10] = r10;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r10 = s->r[10];
    r12 = s->r[12];
    r13 = s->r[13];
    s->r[15] = s->r[14];                      /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* ENDWH (Stmt.s:439-455): pop frames until the WHILE's frame is on
 * top. Evaluate the WHILE's expression again at the stacked pointer.
 * Then either jump back, or drop the frame and finish the loop. */
void basicvfp_hand_ENDWH(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r3, r4, r5, r6, r7,
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    r4 = ros_ld32(r8 - 112);                  /* LDR R4,[ARGP,#ESCWORD] */
    ros_subs(s, r4, 0);                       /* CMP R4,#0 */
    if (r4 != 0) {                            /* BLNE DOEXCEPTION */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_DOEXCEPTION(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13];
    }
    s->r[0] = r0;                             /* BL DONES */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    goto ENDWHM;                              /* B ENDWHM */
ENDWHP:
    s->r[4] = r4;                             /* BL POPA */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_POPA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (!s->z)                                /* BNE ERWHIL */
        stmt_msg_at(s, SITE_ERWHIL);
ENDWHM:
    r4 = ros_ld32(r13);                       /* LDR R4,[SP] */
    ros_subs(s, r4, TENDWH);                  /* CMP R4,#TENDWH */
    if (r4 != TENDWH)                         /* BNE ENDWHP */
        goto ENDWHP;
    r11 = ros_ld32(r13 + 8);                  /* LDR AELINE,[SP,#8]: expr */
    r13 -= 4;                                 /* STR R10,[SP,#-4]! */
    ros_st32(r13, r10);
    s->r[4] = r4;                             /* BL EXPR */
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_logic(s, r9, s->c);                   /* TEQ TYPE,#0 */
    if (r9 == 0) {                            /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    if ((int32_t)r9 < 0) {                    /* BLMI SFIX */
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_SFIX(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    }
    r10 = ros_ld32(r13);                      /* LDR R10,[SP],#4 */
    r13 += 4;
    ros_subs(s, r0, 0);                       /* CMP IACC,#0 */
    if (r0 != 0)                              /* LDRNE LINE,[SP,#4] */
        r12 = ros_ld32(r13 + 4);
    if (r0 != 0) {                            /* BNE STMT: restart */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    r13 += 12;                                /* ADD SP,SP,#4*3 */
    s->r[10] = r10;                           /* B NXT ; R10 ok */
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* REPEAT (Stmt.s:1313-1316): skip to the end of the statement
 * (MUNGLE), stack {TUNTIL, body start}, and run the body. */
void basicvfp_hand_REPEAT(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
             r12 = s->r[12], r13 = s->r[13];

    s->r[0] = s->r[0];                        /* BL MUNGLE */
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_MUNGLE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r0 = TUNTIL;                              /* MOV R0,#TUNTIL */
    r13 -= 8;                                 /* STMFD SP!,{R0,LINE} */
    ros_st32(r13, TUNTIL);
    ros_st32(r13 + 4, r12);
    s->r[0] = r0;                             /* B STMT */
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
    return;
}

/* UNTIL (Stmt.s:1698-1715): the condition has already been evaluated
 * (AEEXDN). Pop frames until the REPEAT's frame is on top. Then either
 * jump back to the start of the body, or drop the frame and finish. */
void basicvfp_hand_UNTIL(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r3, r4, r5, r6, r7, r9,
             r8 = s->r[8], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    s->r[0] = r0;                             /* BL AEEXDN */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r4 = ros_ld32(r8 - 112);                  /* LDR R4,[ARGP,#ESCWORD] */
    ros_subs(s, r4, 0);                       /* CMP R4,#0 */
    if (r4 != 0) {                            /* BLNE DOEXCEPTION */
        s->r[4] = r4;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_DOEXCEPTION(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    }
UNTILM:
    r4 = ros_ld32(r13);                       /* LDR R4,[SP] */
    ros_subs(s, r4, TUNTIL);                  /* CMP R4,#TUNTIL */
    if (r4 != TUNTIL)                         /* BNE UNTILP */
        goto UNTILP;
    ros_logic(s, r0, s->c);                   /* TEQ IACC,#0 */
    if (r0 == 0)                              /* LDREQ LINE,[SP,#4] */
        r12 = ros_ld32(r13 + 4);
    if (r0 == 0) {                            /* BEQ STMT: restart */
        s->r[4] = r4;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    r13 += 8;                                 /* ADD SP,SP,#4*2 */
    s->r[4] = r4;                             /* B NXT */
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
UNTILP:
    s->r[4] = r4;                             /* BL POPA */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_POPA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (s->z)                                 /* BEQ UNTILM */
        goto UNTILM;
    stmt_msg_at(s, SITE_ERREPT);              /* B ERREPT */
}

/* CASE (Stmt.s:17-151): evaluate the subject and stack it with
 * PUSHTYPE, followed by its TYPE word. Then scan the following lines
 * for a WHEN at nesting level 0 that matches. Each WHEN list is
 * evaluated against the stacked subject. OTHERWISE goes to CASEEL.
 * An ENDCASE at the current level goes to ENDCAS. Strings are
 * compared with the copy that PUSHTYPE left on the stack. */
void basicvfp_hand_CASE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];
    uint32_t v;

    s->r[0] = r0;                             /* BL AEEXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_logic(s, r10 ^ TOF, s->c);            /* TEQ R10,#TOF */
    if (!s->z)                                /* BNE ERCASE1 */
        stmt_msg_at(s, SITE_ERCASE1);
    r10 = ros_ld8(r11);                       /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_logic(s, r10 ^ 0xDu, s->c);           /* TEQ R10,#13 */
    if (!s->z)                                /* BNE ERCASE */
        stmt_msg_at(s, SITE_ERCASE);
    ros_logic(s, r9, s->c);                   /* TEQ TYPE,#0 */
    if (r9 != 0) {                            /* BLNE FLOATZ */
        s->r[10] = r10;
        s->r[11] = r11;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_FLOATZ(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    }
    s->r[14] = 0xFFFFFFF0u;                   /* BL PUSHTYPE */
    basicvfp_hand_PUSHTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r13 = s->r[13];
    r13 -= 4;                                 /* STR TYPE,[SP,#-4]! */
    ros_st32(r13, r9);
    r7 = r11 - 1;                             /* SUB R7,AELINE,#1 */
    r6 = 0xFFFFFFFFu;                         /* MVN R6,#0: nesting */
    goto case_02;                             /* B %02 */
case_06: ;
    if (r4 == TENDCA)                         /* CMP R4,#TENDCA */
        r6 -= 1;                              /* SUBEQ R6,R6,#1 */
case_02:
    r4 = ros_ld8(r7 + 1);                     /* LDRB R4,[R7,#1] */
    ros_subs(s, r4, 0xFFu);                   /* CMP R4,#&FF */
    if (r4 >= 0xFFu)                          /* BCS NOENDC */
        stmt_msg_at(s, SITE_NOENDC);
    if (ros_ld8(r7 - 1) == TOF)               /* LDRB R4,[R7,#-1]; CMP #TOF */
        r6 += 1;                              /* ADDEQ R6,R6,#1 */
    r5 = r7 + 4;                              /* ADD R5,R7,#4 */
    r10 = ros_ld8(r7 + 3);                    /* LDRB R10,[R7,#3] */
    r7 += r10;                                /* ADD R7,R7,R10 */
case_04:
    r4 = ros_ld8(r5);                         /* LDRB R4,[R5],#1 */
    r5 += 1;
    if (r4 == 32)                             /* TEQ R4,#" " ; BEQ %04 */
        goto case_04;
    if (r6 != 0)                              /* TEQ R6,#0 ; BNE %06 */
        goto case_06;
    ros_subs(s, r4, TENDCA);                  /* CMP R4,#TENDCA */
    if (r4 != TENDCA && r4 != TWHEN && r4 != TOTHER)
        goto case_02;                         /* BNE %02 */
    r12 = r5;                                 /* MOV LINE,R5 */
    if (r4 >= TENDCA)                         /* BCS ENDCAS */
        goto ENDCAS;
    ros_logic(s, r4 ^ TOTHER, s->c);          /* TEQ R4,#TOTHER */
    if (s->z)                                 /* BEQ CASEEL */
        goto CASEEL;
CASEWH:
    r13 -= 4;                                 /* STR R7,[SP,#-4]! */
    ros_st32(r13, r7);
    s->r[0] = r0;                             /* BL AEEXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    r7 = ros_ld32(r13);                       /* LDR R7,[SP],#4 */
    r13 += 4;
    r12 = r11;                                /* MOV LINE,AELINE */
    r4 = ros_ld32(r13);                       /* LDMFD SP,{R4,R5,R6} */
    r5 = ros_ld32(r13 + 4);
    r6 = ros_ld32(r13 + 8);
    if (r4 == 0)                              /* TEQ R4,#0 ; BEQ CASES */
        goto CASES;
    ros_logic(s, r9, s->c);                   /* TEQ TYPE,#0 */
    if (r9 == 0) {                            /* BEQ ERTYPEINT */
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    if ((int32_t)r9 >= 0) {                   /* BLPL IFLT: FSITODPL */
        s->fp->vfp.sw[0] = r0;                /* FMSRPL S0,IACC */
        s->fp->vfp.d[0] = (double)(int32_t)r0; /* FSITODPL FACC,S0 */
    }
    s->fp->vfp.d[1] = ros_ldd(r13 + 4);       /* FLDD D1,[SP,#4] */
    s->fp->fpscr |= ros_vfp_cmp_ex(s->fp->vfp.d[0], s->fp->vfp.d[1], 0);
    ros_vfp_cmp(s, s->fp->vfp.d[0], s->fp->vfp.d[1]); /* FCMPD FACC,D1 */
    ros_vmrs_flags(s);                        /* FMRX PC,FPSCR */
    if (s->fp->vfp.d[0] == s->fp->vfp.d[1])   /* BEQ CASEEQ */
        goto CASEEQ;
CASENE:
    ros_logic(s, r10 ^ 0x2Cu, s->c);          /* TEQ R10,#"," */
    if (s->z)                                 /* BEQ CASEWH */
        goto CASEWH;
    v = r10 ^ 0x3Au;                          /* TEQ R10,#":" */
    ros_logic(s, r10 ^ 0x3Au, s->c);
    if (v != 0)                               /* TEQNE R10,#13 */
        ros_logic(s, r10 ^ 0xDu, s->c);
    if (v != 0 && r10 != 0xDu) {              /* BNE ERSYNT */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5;
        s->r[6] = r6; s->r[7] = r7; s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    r6 = 0;                                   /* MOV R6,#0 */
    goto case_02;                             /* B %02 */
CASES:
    ros_logic(s, r9, s->c);                   /* TEQ TYPE,#0 */
    if (r9 != 0) {                            /* BNE ERTYPESTR */
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    v = r2 ^ r5;                              /* TEQ CLEN,R5 */
    if (v != 0)                               /* BNE CASENE */
        goto CASENE;
    r0 = r8 - 1536;                           /* ADD R0,ARGP,#STRACC */
    v = r2 ^ r0;                              /* TEQ CLEN,R0 */
    if (v == 0)                               /* BEQ CASEEQ: empty == empty */
        goto CASEEQ;
    r1 = r13 + 8;                             /* ADD R1,SP,#8 */
case_80:
    r4 = ros_ld8(r1);                         /* LDRB R4,[R1],#1 */
    r1 += 1;
    r5 = ros_ld8(r0);                         /* LDRB R5,[R0],#1 */
    r0 += 1;
    v = r4 ^ r5;                              /* TEQ R4,R5 */
    if (v != 0)                               /* BNE CASENE */
        goto CASENE;
    v = r0 ^ r2;                              /* TEQ R0,CLEN */
    if (v != 0)                               /* BNE %80 */
        goto case_80;
CASEEQ:
    s->r[0] = r0;                             /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r9 = s->r[9]; r13 = s->r[13];
    v = r10 ^ 0x3Au;                          /* TEQ R10,#":" */
    if (v == 0 || r10 == 0xDu) {              /* TEQNE R10,#13 ; BEQ DONEXT */
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_DONEXT(s);
        return;
    }
    ros_logic(s, r10 ^ 0x2Cu, s->c);          /* TEQ R10,#"," */
    if (!s->z) {                              /* BNE ERSYNT */
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    r0 = 0;                                   /* MOV R0,#0: quote state */
case_90:
    r10 = ros_ld8(r12);                       /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if ((r10 ^ 0x22u) == 0)                   /* TEQ R10,#""""; EOREQ R0,R0,#1 */
        r0 ^= 1;
    if ((r10 ^ 0xDu) == 0) {                  /* TEQ R10,#13 ; BEQ DONEXT */
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[7] = r7; s->r[10] = r10; s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_DONEXT(s);
        return;
    }
    if (r0 != 0)                              /* TEQ R0,#0 ; BNE %90 */
        goto case_90;
    v = r10 ^ 0x3Au;                          /* TEQ R10,#":" */
    if (v != 0)                               /* BNE %90 */
        goto case_90;
    s->r[0] = r0;                             /* B DONEXT */
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
    s->r[10] = r10; s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_DONEXT(s);
    return;
CASEEL:
    s->r[0] = r0;                             /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r9 = s->r[9]; r13 = s->r[13];
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);                    /* B STMT */
    return;
ENDCAS:
    s->r[0] = r0;                             /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r9 = s->r[9]; r13 = s->r[13];
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_DONXTS(s);                  /* B DONXTS */
    return;
}

/* ON (Stmt.s:1032-1160, with the error handlers ONERR and ONERRF at
 * :1150-1160). This handles:
 *   - ON ERROR, with LOCAL or OFF
 *   - the empty form, which turns the cursor on
 *   - ON expr GOTO, GOSUB or PROC.
 * The last form scans the list and counts the branches. It tracks
 * brackets so that commas inside them are not counted. When the index
 * runs past the end of the list, the exit goes through THENLN to the
 * code after the ELSE. ON PROC skips the rest of its own line and
 * enters FNBODY. */
void basicvfp_hand_ON(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3, r4 = s->r[4], r5, r6, r7,
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == TERROR)                        /* CMP R10,#TERROR ; BEQ ONERR */
        goto ONERR;
    ros_subs(s, r10, TELSE);                  /* CMP R10,#TELSE */
    if (r10 != TELSE)                         /* CMPNE R10,#":" */
        ros_subs(s, r10, 58);
    if (r10 != TELSE && r10 != 58)            /* CMPNE R10,#13 */
        ros_subs(s, r10, 13);
    if (r10 == TELSE || r10 == 58 || r10 == 13) { /* BEQ CURSON */
        s->r[10] = r10;
        s->r[12] = r12;
        basicvfp_hand_CURSON(s, r14);
        return;
    }
    r11 = r12 - 1;                            /* SUB AELINE,LINE,#1 */
    s->r[0] = r0;                             /* BL EXPR */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_subs(s, r10, TGOTO);                  /* CMP R10,#TGOTO */
    if (r10 != TGOTO)                         /* CMPNE R10,#TGOSUB */
        ros_subs(s, r10, TGOSUB);
    if (r10 != TGOTO && r10 != TGOSUB)        /* CMPNE R10,#TPROC */
        ros_subs(s, r10, TPROC);
    if (r10 != TGOTO && r10 != TGOSUB && r10 != TPROC) /* BNE ONER */
        stmt_msg_at(s, SITE_ONER);
    if (r10 == TPROC)                         /* CMP R10,#TPROC */
        r11 -= 1;                             /* SUBEQ AELINE,AELINE,#1 */
    v = r0;                                   /* SUBS IACC,IACC,#1 */
    r0 -= 1;
    if (v < 1)                                /* BCC ONRGA */
        goto ONRGA;
    if (r0 == 0)                              /* BEQ ONGOT */
        goto ONGOT;
    r4 = 0;                                   /* MOV R4,#0: paren depth */
ONSRCH:
    r1 = ros_ld8(r11);                        /* LDRB R1,[AELINE],#1 */
    r11 += 1;
    if (r1 == 13 || r1 == TELSE)              /* CMP/CMPNE ; BEQ ONRG */
        goto ONRG;
    if (r1 == 40)                             /* CMP R1,#"(" ; ADDEQ R4,R4,#1 */
        r4 += 1;
    if (r1 == 41)                             /* CMP R1,#")" ; SUBEQ R4,R4,#1 */
        r4 -= 1;
    if (r4 != 0)                              /* TEQ R4,#0 ; BNE ONSRCH */
        goto ONSRCH;
    if (r1 == 58)                             /* CMP R1,#":" ; BEQ ONRGA */
        goto ONRGA;
    if (r1 != 44)                             /* CMP R1,#"," ; BNE ONSRCH */
        goto ONSRCH;
    r0 -= 1;                                  /* SUBS IACC,IACC,#1 */
    if (r0 != 0)                              /* BNE ONSRCH */
        goto ONSRCH;
ONGOT: ;
    if (r10 == TGOSUB)                        /* CMP R10,#TGOSUB ; BEQ ONGOSB */
        goto ONGOSB;
    if (r10 == TPROC)                         /* CMP R10,#TPROC ; BEQ ONPROC */
        goto ONPROC;
    r12 = r11;                                /* MOV LINE,AELINE */
    s->r[0] = r0;                             /* BL GOFACT */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_GOFACT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r12 = r1 + 4;                             /* ADD LINE,R1,#4 */
    s->r[12] = r12;                           /* B STMT */
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
    return;
ONGOSB:
    r12 = r11;                                /* MOV LINE,AELINE */
    s->r[0] = r0;                             /* BL GOFACT */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_GOFACT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    basicvfp_ONSKIP(s, &r2, r11, &r12, 0xFFFFFFF0u); /* BL ONSKIP */
    r0 = TRETURN;                             /* MOV R0,#TRETURN */
    r13 -= 8;                                 /* STMFD SP!,{LINE,R0} */
    ros_st32(r13, TRETURN);
    ros_st32(r13 + 4, r12);
    r12 = r1 + 4;                             /* ADD LINE,R1,#4 */
    s->r[0] = r0;                             /* B STMT */
    s->r[2] = r2;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
    return;
ONPROC:
    r1 = ros_ld8(r11);                        /* LDRB R1,[AELINE],#1 */
    r11 += 1;
    if (r1 == 32)                             /* CMP R1,#" " ; BEQ ONPROC */
        goto ONPROC;
    ros_subs(s, r1, TPROC);                   /* CMP R1,#TPROC */
    if (r1 != TPROC)                          /* BNE ONER */
        stmt_msg_at(s, SITE_ONER);
    basicvfp_ONSKIP(s, &r2, r11, &r12, 0xFFFFFFF0u); /* BL ONSKIP */
    r12 -= 1;                                 /* SUB LINE,LINE,#1 */
    r1 = ros_ld32(r8 - 0x200u);               /* LDR R1,[ARGP,#PROCPTR] */
    s->r[0] = r0;                             /* B FNBODY */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[4] = r4;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_FNBODY(s);
    return;
ONRGA:
    r1 = ros_ld8(r11);                        /* LDRB R1,[AELINE],#1 */
    r11 += 1;
    if (r1 != 13 && r1 != TELSE)              /* CMP/CMPNE ; BNE ONRGA */
        goto ONRGA;
ONRG:
    ros_subs(s, r1, 13);                      /* CMP R1,#13 */
    if (r1 == 13)                             /* BEQ ONRGER */
        stmt_msg_at(s, SITE_ONRGER);
    r12 = r11;                                /* MOV LINE,AELINE */
    /* B THENLN: the shared IF machinery */
    s->r[8] = r8;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = r14;
    stmt_thenln(s);
    return;
ONERR:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == TOFF)                          /* CMP R10,#TOFF ; BEQ ONERRF */
        goto ONERRF;
    if (r10 != TLOCAL)                        /* CMP R10,#TLOCAL */
        r12 -= 1;                             /* SUBNE LINE,LINE,#1 */
    if (r10 == TLOCAL)                        /* STREQ SP,[ARGP,#ERRSTK] */
        ros_st32(r8 - 120, r13);
    ros_st32(r8 - 124, r12);                  /* STR LINE,[ARGP,#ERRORH] */
    s->r[0] = r0;                             /* B REM (= DATA) */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_DATA(s);
    return;
ONERRF:
    s->r[0] = r0;                             /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    basicvfp_hand_ORDERR(s, &r0, r8, 0xFFFFFFF0u); /* BL ORDERR */
    s->r[0] = r0;                             /* B NXT */
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* GOTO (Stmt.s:568-577): GOFACT finds the target line. It uses the
 * cache, or SPTSTN, EXPR and FNDLNO on a miss. DONES checks the end
 * of the statement, and execution enters the line. */
void basicvfp_hand_GOTO(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r3, r4, r5, r6, r7, r9,
             r8 = s->r[8], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    r4 = ros_ld32(r8 - 112);                  /* LDR R4,[ARGP,#ESCWORD] */
    ros_subs(s, r4, 0);                       /* CMP R4,#0 */
    if (r4 != 0) {                            /* BLNE DOEXCEPTION */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_DOEXCEPTION(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13];
    }
    s->r[0] = r0;                             /* BL GOFACT */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_GOFACT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL DONES */
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r12 = r1 + 4;                             /* ADD LINE,R1,#4 */
    s->r[12] = r12;                           /* B STMT */
    s->r[15] = s->r[14];
    basicvfp_hand_STMT(s);
    return;
}

/* GOSUB (Stmt.s:555-568): as GOTO, but first the return frame
 * {TRETURN, LINE} goes on the stack. The FSA depth check guards it. */
void basicvfp_hand_GOSUB(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r9,
             r8 = s->r[8], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    r4 = ros_ld32(r8 - 112);                  /* LDR R4,[ARGP,#ESCWORD] */
    ros_subs(s, r4, 0);                       /* CMP R4,#0 */
    if (r4 != 0) {                            /* BLNE DOEXCEPTION */
        s->r[0] = s->r[0]; s->r[1] = s->r[1]; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_DOEXCEPTION(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
        r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
        r12 = s->r[12]; r13 = s->r[13];
    }
    s->r[0] = s->r[0];                        /* BL GOFACT */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_GOFACT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL DONES */
    basicvfp_hand_DONES(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 8;                                 /* STMFD SP!,{LINE,R0} */
    ros_st32(r13, TRETURN);
    ros_st32(r13 + 4, r12);
    r12 = r1 + 4;                             /* ADD LINE,R1,#4 */
    r0 = ros_ld32(r8 - 140) + 0x400u;         /* LDR R0,[ARGP,#FSA]; ADD #1024 */
    ros_subs(s, r0, r13);                     /* CMP R0,SP */
    if (r0 < r13) {                           /* BCC STMT */
        s->r[0] = r0;
        s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    s->r[0] = r0;                             /* B ERDEEPNEST */
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_ERDEEPNEST(s);
    return;
}

/* RETURN (Stmt.s:1359-1370): pop frames (POPA) until a GOSUB's frame
 * {TRETURN, restart} is found. The separator character before the
 * restart point is put back in R10 for NXT. Any other frame gives
 * ERGOSB. */
void basicvfp_hand_RETURN(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r9,
             r8 = s->r[8], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    s->r[0] = s->r[0];                        /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
RETURNA:
    r4 = ros_ld32(r13);                       /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_subs(s, r4, TRETURN);                 /* CMP R4,#TRETURN */
    if (r4 == TRETURN) {
        r10 = ros_ld8(r5 - 1);                /* LDREQB R10,[R5,#-1] */
        r12 = r5;                             /* MOVEQ LINE,R5 */
        s->r[4] = r4;                         /* BEQ NXT */
        s->r[5] = r5;
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    r13 -= 8;                                 /* SUB SP,SP,#8 */
    s->r[4] = r4;                             /* BL POPA */
    s->r[5] = r5;
    s->r[10] = r10;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_POPA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (s->z)                                 /* BEQ RETURNA */
        goto RETURNA;
    stmt_msg_at(s, SITE_ERGOSB);              /* B ERGOSB */
}

/* END (Stmt.s:357-422): a plain END checks the end of the statement,
 * raises "At end" (ENDER) and unwinds (CLRSTK). END=<size> changes
 * the whole memory layout:
 *   - the stack is moved down to the FSA
 *   - the Wimp slot is resized
 *   - the stack is moved back up
 *   - every reference into the stack is moved by the same amount.
 *     These are ERRSTK, the LOCALARLIST chain and its owners. */
void basicvfp_hand_END(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3, r4, r5, r6, r7,
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 61);                     /* CMP R10,#"=" */
    if (r10 == 61)                            /* BEQ ENDCHANGE */
        goto ENDCHANGE;
    s->r[0] = r0;                             /* BL DONE */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[13] = r13;                           /* BL ENDER */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_ENDER(s);
    s->r[15] = s->r[14];                      /* B CLRSTK */
    basicvfp_CLRSTK(s);
    return;

ENDCHANGE:
    r0 = ros_ld32(r8 - 36);                   /* LDR R0,[ARGP,#DIMLOCAL] */
    ros_logic(s, r0, s->c);                   /* TEQ R0,#0 */
    if (r0 != 0)                              /* BNE ERMVSTK */
        stmt_msg_at(s, SITE_ERMVSTK);
    s->r[0] = r0;                             /* BL AEEXDN */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r7 = r0 - 0x8000u;                        /* SUB R7,R0,#&8000 */
    r4 = ros_ld32(r8 - 132);                  /* LDR R4,[ARGP,#HIMEM] */
    r5 = r4 - r13;                            /* SUB R5,R4,SP */
    r6 = ros_ld32(r8 - 140);                  /* LDR R6,[ARGP,#FSA] */
    r2 = r6 + 0x400u + r5;                    /* ADD R2,R6,#1024; ADD R2,R2,R5 */
    ros_subs(s, r2, r0);                      /* CMP R2,R0 */
    if (r2 >= r0)                             /* BCS ERREND */
        stmt_msg_at(s, SITE_ERREND);
ENDCHANGE1:                                   /* move the stack down */
    v = r13;                                  /* LDR R3,[SP],#4 */
    r13 += 4;
    ros_st32(r6, ros_ld32(v));                /* STR R3,[R6],#4 */
    r6 += 4;
    ros_subs(s, r13, r4);                     /* CMP SP,R4 */
    if (r13 < r4)                             /* BCC ENDCHANGE1 */
        goto ENDCHANGE1;
    r0 = r7;                                  /* MOV R0,R7 */
    r1 = 0xFFFFFFFFu;                         /* MOV R1,#-1 */
    s->r[0] = r0;                             /* SWI Wimp_SlotSize */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[13] = r13;
    ros_swi(s, 0x400ECu);
    r0 = s->r[0];
    r2 = s->r[2];
    r7 = r0 + 0x8000u - r13;                  /* SUB R7,R0+#&8000,SP */
    r13 = r0 + 0x8000u;                       /* MOV SP,R0+#&8000 */
    ros_st32(r8 - 84, r13);                   /* STR SP,[ARGP,#MEMLIMIT] */
    ros_st32(r8 - 132, r13);                  /* STR SP,[ARGP,#HIMEM] */
    r1 = ros_ld32(r8 - 140);                  /* LDR R1,[ARGP,#FSA] */
ENDCHANGE2:                                   /* move the stack back up */
    r6 -= 4;                                  /* LDR R3,[R6,#-4]! */
    r13 -= 4;                                 /* STR R3,[SP,#-4]! */
    ros_st32(r13, ros_ld32(r6));
    ros_subs(s, r6, r1);                      /* CMP R6,R1 */
    if (r6 > r1)                              /* BHI ENDCHANGE2 */
        goto ENDCHANGE2;
    r4 = r8 - 104;                            /* ADD R4,ARGP,#LOCALARLIST-4 */
    ros_st32(r8 - 120, ros_ld32(r8 - 120) + r7); /* ERRSTK rebased */
    r0 = 0;                                   /* MOV R0,#0 */
    ros_st32(r8 - 96, 0);                     /* STR R0,[ARGP,#INSTALLLIST] */
ENDCHANGELOCALAR:
    r3 = ros_ld32(r4 + 4);                    /* LDR R3,[R4,#4]: next */
    ros_logic(s, r3, s->c);                   /* TEQ R3,#0 */
    if (r3 == 0)                              /* BEQ ENDCHANGELOCALARDONE */
        goto ENDCHANGELOCALARDONE;
    r3 += r7;                                 /* ADD R3,R3,R7 */
    ros_st32(r4 + 4, r3);                     /* STR R3,[R4,#4] */
    r4 = r3;                                  /* MOV R4,R3 */
    r5 = ros_ld32(r3 + 8);                    /* LDR R5,[R4,#8]: owner */
    r6 = ros_ld32(r5) + r7;                   /* ADD R6,[R5],R7 */
    r1 = r3 + 16;                             /* ADD R1,R4,#16 */
    ros_subs(s, r6, r1);                      /* CMP R6,R1 */
    if (r6 == r1)                             /* STREQ R6,[R5] */
        ros_st32(r5, r6);
    if (r6 != r1)                             /* MOVNE R0,#1 */
        r0 = 1;
    goto ENDCHANGELOCALAR;                    /* B ENDCHANGELOCALAR */
ENDCHANGELOCALARDONE:
    ros_logic(s, r0, s->c);                   /* TEQ R0,#0 */
    if (r0 == 0) {                            /* BEQ NXT */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    stmt_msg_at(s, SITE_ERRENDARRAYREF);      /* B ERRENDARRAYREF */
}

/* GTARGS' BL (P1). GTARGS used to end its path for RETURN parameters
 * with ros_resume(link), a longjmp back into this helper's frame,
 * which called setjmp. GTARGS now returns through the C stack (see
 * funct_ret.c). So the setjmp frame is gone, and this is the plain BL
 * it always was underneath. R14 is set to the lift's own site, the
 * call is made, and ENDPR continues. The helper stays noinline so
 * that hand_ENDPR itself never carries any setjmp overhead. Its
 * B DONXTS continuation must stay a tail call. */
__attribute__((noinline)) static void endpr_gtargs(struct ros_cpu *s)
{
    s->r[14] = 0xFC105270u;
    basicvfp_hand_GTARGS(s);
}

/* ENDPR (Stmt.s:424-437): pop frames until the PROC's frame is on top.
 * Trace the exit, and unwind through GTARGS with TYPE set to TINTEGER,
 * which means no value. Then resume at whichever of LINE and AELINE is
 * further on. */
void basicvfp_hand_ENDPR(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r9,
             r8 = s->r[8], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];

    s->r[0] = s->r[0];                        /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
ENDPRA:
    r4 = ros_ld32(r13);                       /* LDR R4,[SP],#4 */
    r13 += 4;
    if (r4 != TPROC)                          /* TEQ R4,#TPROC ; BNE ENDPRP */
        goto ENDPRP;
    s->r[4] = r4;                             /* BL ENDTRC */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_ENDTRC(s);
    r0 = s->r[0]; r1 = s->r[1]; r4 = s->r[4]; r5 = s->r[5];
    r13 = s->r[13];
    r9 = TINTEGER;                            /* MOV TYPE,#TINTEGER */
    s->r[9] = r9;                             /* BL GTARGS */
    endpr_gtargs(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                 /* SUB SP,SP,#4 */
    if (r12 < r11)                            /* CMP LINE,AELINE */
        r12 = r11;                            /* MOVCC LINE,AELINE */
    s->r[12] = r12;                           /* B DONXTS */
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_DONXTS(s);
    return;
ENDPRP:
    r13 -= 4;                                 /* SUB SP,SP,#4 */
    s->r[4] = r4;                             /* BL POPA */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_POPA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (s->z)                                 /* BEQ ENDPRA */
        goto ENDPRA;
    stmt_msg_at(s, SITE_ENDPRE);              /* B ENDPRE */
}

/* STOP (Stmt.s:1562-1563): check the statement end and raise "STOP". */
void basicvfp_hand_STOP(struct ros_cpu *s)
{
    s->r[0] = s->r[0];                        /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = s->r[10];
    s->r[11] = s->r[11];
    s->r[12] = s->r[12];
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    stmt_msg_at(s, SITE_ERSTOP);              /* B ERSTOP */
}

/* TRACE (Stmt.s:1612-1696) sets the trace mode. TRACE TO opens a trace
 * file and TRACE CLOSE closes it. STEP, PROC, FN, ENDPROC, ON, OFF, a
 * line number or an expression set TRCNUM and TRCFLG. The bits of
 * TRCNUM are:
 *   - bit 31: trace PROC and FN
 *   - bit 30: step
 *   - bit 29: trace ENDPROC
 *   - bits 15-0: the line-number limit.
 * TRACE PROC and TRACE FN also flush the line cache, because a cache
 * hit would skip printing the name. */
void basicvfp_hand_TRACE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3, r4, r5, r6, r7, r9,
             r8 = s->r[8], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13];
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_logic(s, r10 ^ TTO, s->c);            /* TEQ R10,#TTO */
    if (s->z)                                 /* BEQ TRACETO */
        goto TRACETO;
    v = r10 ^ TCLOSE;                         /* TEQ R10,#TCLOSE */
    if (v == 0)                               /* BEQ TRACECLOSE */
        goto TRACECLOSE;
    r4 = ros_ld32(r8 - 108) & ~TINTEGER;      /* LDR/BIC R4,[ARGP,#TRCNUM] */
    r5 = 128;                                 /* MOV R5,#&80 */
    v = r10 ^ TSTEP;                          /* TEQ R10,#TSTEP */
    if (v == 0) {
        r4 |= TINTEGER;                       /* ORREQ R4,R4,#TINTEGER */
        basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BLEQ SPACES */
    }
    if (r10 == TPROC || r10 == TFN) {         /* CMP/CMPNE; ORREQ */
        r0 = r4 | TFP;                        /* turn on proc/fn trace */
        goto TRSET1;                          /* BEQ TRSET1 */
    }
    if (r10 == TENDPR) {                      /* CMP R10,#TENDPR */
        r0 = r4 | TEFP;                       /* ORREQ IACC,R4,#TEFP */
        goto TRSET1;
    }
    r4 = r4 & ~0xFFu & ~0xFF00u;              /* BIC R4,R4,#&FF,:&FF00 */
    ros_subs(s, r10, TCONST);                 /* CMP R10,#TCONST */
    if (r10 == TCONST)                        /* BEQ TRSET2 */
        goto TRSET2;
    if (r10 != TCONST)                        /* CMPNE R10,#TON */
        ros_subs(s, r10, TON);
    if (r10 == TON) {
        r0 = r4 | 0xFF00u;                    /* ORREQ IACC,R4,#&FF00 */
        goto TRSET1;
    }
    r0 = ros_logic(s, r10 ^ TOFF, s->c);      /* EORS IACC,R10,#TOFF */
    if (r0 != 0)                              /* BNE TRNUMB */
        goto TRNUMB;
    r5 = 0;                                   /* MOV R5,#0 */
TRSET1:
    s->r[0] = r0;                             /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_logic(s, r0 & TFP, 1);                /* TST IACC,#TFP */
    if (s->z)                                 /* BEQ TRSET */
        goto TRSET;
    r13 -= 12;                                /* STMFD SP!,{R0-R2} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r1);
    ros_st32(r13 + 8, r2);
    basicvfp_hand_FLUSHCACHE(s, &r0, &r1, &r2, r8, 0xFFFFFFF0u); /* BL */
    r0 = ros_ld32(r13);                       /* LDMFD SP!,{R0-R2} */
    r1 = ros_ld32(r13 + 4);
    r2 = ros_ld32(r13 + 8);
    r13 += 12;
TRSET:
    ros_st32(r8 - 108, r0);                   /* STR IACC,[ARGP,#TRCNUM] */
    ros_st8(r8 - 111, r5);                    /* STRB R5,[ARGP,#TRCFLG] */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5;
    s->r[13] = r13;
    s->r[15] = s->r[14];                      /* B NXT */
    basicvfp_hand_NXT(s);
    return;
TRSET2:
    s->r[0] = r0;                             /* BL SPGETN */
    s->r[1] = s->r[1];
    s->r[10] = r10;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPGETN(s);
    r0 = s->r[0]; r1 = s->r[1]; r10 = s->r[10]; r12 = s->r[12];
    r0 += r4;                                 /* ADD IACC,IACC,R4 */
    goto TRSET1;                              /* B TRSET1 */
TRNUMB:
    r13 -= 8;                                 /* STMFD SP!,{R4,R5} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    r12 -= 1;                                 /* SUB LINE,LINE,#1 */
    s->r[0] = r0;                             /* BL AEEXDN */
    s->r[1] = s->r[1];
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r4 = ros_ld32(r13);                       /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 += r4;                                 /* ADD IACC,IACC,R4 */
    goto TRSET;                               /* B TRSET */
TRACETO:
    s->r[0] = r0;                             /* BL AEEXPR */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL OSSTRI */
    basicvfp_hand_OSSTRI(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                   /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r1 = ros_ld32(r8 - 104);                  /* LDR R1,[ARGP,#TRACEFILE] */
    ros_logic(s, r1, s->c);                   /* TEQ R1,#0 */
    r0 = 0;                                   /* MOV R0,#0 */
    ros_st32(r8 - 104, 0);                    /* kill the handle */
    if (r1 != 0) {                            /* SWINE OS_Find */
        s->r[0] = r0;
        s->r[1] = r1;
        ros_native_swi(s, ros_thunk_OS_Find);
        if (s->v)
            ros_swi_raise(s);
    }
    r1 = r8 - 1536;                           /* ADD R1,ARGP,#STRACC */
    r0 = 128;                                 /* MOV R0,#&80 */
    s->r[0] = r0;                             /* SWI OS_Find */
    s->r[1] = r1;
    ros_native_swi(s, ros_thunk_OS_Find);
    if (s->v)
        ros_swi_raise(s);
    r0 = s->r[0];
    ros_st32(r8 - 104, r0);                   /* STR R0,[ARGP,#TRACEFILE] */
    ros_subs(s, r0, 0);                       /* CMP R0,#0 */
    r0 = 18;                                  /* MOV R0,#&12: set type */
    r2 = 0xFFFu;                              /* MOV R2,#&FF; ORR #&F00 */
    if (!s->z) {                              /* SWINE XOS_File */
        s->r[0] = r0;
        s->r[2] = r2;
        ros_native_swi(s, ros_thunk_OS_File);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    }
    if (s->z) {                               /* SWIEQ OS_File */
        s->r[0] = r0;
        s->r[2] = r2;
        ros_native_swi(s, ros_thunk_OS_File);
        if (s->v)
            ros_swi_raise(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    }
    s->r[0] = r0; s->r[2] = r2;               /* B NXT */
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
TRACECLOSE:
    s->r[0] = r0;                             /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r1 = ros_ld32(r8 - 104);                  /* LDR R1,[ARGP,#TRACEFILE] */
    ros_logic(s, r1, s->c);                   /* TEQ R1,#0 */
    r0 = 0;                                   /* MOV R0,#0 */
    ros_st32(r8 - 104, 0);                    /* kill the handle */
    if (r1 != 0) {                            /* SWINE OS_Find */
        s->r[0] = r0;
        s->r[1] = r1;
        ros_native_swi(s, ros_thunk_OS_Find);
        if (s->v)
            ros_swi_raise(s);
        r0 = s->r[0];
    }
    s->r[0] = r0; s->r[1] = r1;               /* B NXT */
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* QUIT (Stmt2.s:2454-2472): with no argument, exit at once. Otherwise
 * evaluate the expression, store the "ABEX" magic word and the return
 * code for whatever reads them after OS_Exit, and exit. */
void basicvfp_hand_QUIT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3, r4, r5, r6, r7, r8, r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13];

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 58);                     /* CMP R10,#":" */
    if (r10 != 58)                            /* CMPNE R10,#13 */
        ros_subs(s, r10, 13);
    if (r10 == 58 || r10 == 13) {             /* SWIEQ OS_Exit */
        s->r[0] = r0;
        s->r[1] = s->r[1];
        s->r[10] = r10;
        s->r[11] = r11;
        s->r[12] = r12;
        ros_native_swi(s, ros_thunk_OS_Exit);
        if (s->v)
            ros_swi_raise(s);
        r0 = s->r[0];
    }
    r11 = r12 - 1;                            /* SUB AELINE,LINE,#1 */
    s->r[0] = r0;                             /* BL EXPRDN */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r1 = 0x58454241u;                         /* LDR R1,ABEX */
    r2 = r0;                                  /* MOV R2,IACC */
    ros_st32(r8 - 44, 0x58454241u);           /* STR R1,[ARGP,#OS_Exit_ABEX] */
    ros_st32(r8 - 40, r0);                    /* STR R2,[ARGP,#OS_Exit_RetCode] */
    s->r[1] = r1;                             /* SWI OS_Exit */
    s->r[2] = r2;
    ros_native_swi(s, ros_thunk_OS_Exit);
    if (s->v)
        ros_swi_raise(s);
    r0 = s->r[0];
    ros_fault(s, 0xFC107F68u, "a transfer to an address that is not code");
}

/* CHAIN (Stmt.s:152-154): load the new program (LOADER), then enter
 * it through RUNNER. */
void basicvfp_hand_CHAIN(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3, r4, r5, r6, r7, r8, r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13];

    s->r[0] = r0;                             /* BL LOADER */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_LOADER(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[15] = s->r[14];                      /* B RUNNER */
    basicvfp_RUNNER(s);
    return;
}

/* RUN (Stmt.s:1475): check the statement end, then fall into RUNNER,
 * which resets the machine state and starts at PAGE. */
void basicvfp_hand_RUN(struct ros_cpu *s)
{
    s->r[0] = s->r[0];                        /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = s->r[10];
    s->r[11] = s->r[11];
    s->r[12] = s->r[12];
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    s->r[15] = s->r[14];                      /* B RUNNER */
    basicvfp_RUNNER(s);
    return;
}

/* CURSON and CURSOFF (Stmt2.s, and the lift's bodies at the CURSON:
 * and CURSOFF: labels of DISPAT). They write the cursor-on and
 * cursor-off VDU strings with OS_WriteS from their ROM addresses.
 * CURSOFF first checks the end of the statement (DONES). Neither
 * returns. After the write each one continues into the statement
 * loop's continuation fragment, exactly as the lift does. That is
 * ros_writes followed by the FC106CB4 or FC106CCC tail, which is the
 * fragment that runs NXT or DONES after the string. */
extern void basicvfp_FC106CB4(struct ros_cpu *s);
extern void basicvfp_FC106CCC(struct ros_cpu *s);

void basicvfp_hand_CURSON(struct ros_cpu *s, uint32_t ret_to)
{
    (void)ret_to;                            /* never returns */
    ros_writes(s, 0xFC106CB0u);              /* SWI OS_WriteS */
    s->r[15] = s->r[14];
    basicvfp_FC106CB4(s);
    ros_fault(s, 0xFC106CB0u, "a transfer to an address that is not code");
}

/* Group C landed CURSOFF's dispatch case (135). The CASE_SUBS contract
 * takes one argument (musttail), so the unused ret_to has gone. The
 * body never returns in either case. CURSON keeps the two-argument
 * shape from before that rule, because no case reaches it. The lift's
 * one reference to it is a branch. */
void basicvfp_hand_CURSOFF(struct ros_cpu *s)
{
    s->r[14] = 0xFC106CC4u;
    basicvfp_hand_DONES(s);                  /* BL DONES */
    ros_writes(s, 0xFC106CC8u);              /* SWI OS_WriteS */
    s->r[15] = s->r[14];
    basicvfp_FC106CCC(s);
    ros_fault(s, 0xFC106CC8u, "a transfer to an address that is not code");
}
