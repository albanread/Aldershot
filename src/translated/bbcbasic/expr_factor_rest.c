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
 * (Sources/Programmer/BASIC: s.Expr, s.Factor, s.Lexical).
 */

/* expr_factor_rest.c: the expression evaluator's entry and trunk
 * machinery, translated by hand from Expr.s and Factor.s (RISC OS
 * 5.31's BASIC, the VFP build).  This is the rest of board row 5.
 *
 * EXPR is the evaluator that the right-hand side of every statement
 * goes through.  It saves its link, reads the first FACTOR, skips
 * spaces, and looks the lookahead character up in PRIORTABLE.  If the
 * word there is below 256, the character is not an operator and the
 * value returns up (LDRCC PC,[SP],#4).  Otherwise the word is
 * priority<<24 | a jump-table index into the operator ladder.  R7, the
 * stacked priority, is zeroed and the index dispatches straight to the
 * operator.  The operators themselves are landed (units expr_addsub,
 * muldivpow, bool, divop and compr).  What was still lift was the entry
 * and the deferral protocol around them:
 *   - EXPRRECUR, the read of the right operand, with the operator's
 *     priority stacked under the link;
 *   - EXPRNEXT, the compare CMP R7,R10,LSR #28 that decides whether to
 *     defer or chain.  It returns up when the stacked priority is the
 *     higher.  It also exits when the character is not an operator at
 *     all;
 *   - EXPRHARD, the shared computed jump that every operator's
 *     continuation re-enters.
 * The ladder's chain (EXPRCALL) and the join-point fragments (AJ7,
 * &FC1026A4...) stay lift.  The fragments are the lifter's own
 * rendering of the table's filler words.  They can only be reached
 * with an operator's index, and this unit's dispatches call the
 * operator thunks by the same indices that the lift's EXPR uses.
 *
 * FACTOR is a SWAP, in the shape of the DISPAT swap.  The hand
 * dispatcher owns the character switch for every landed case:
 *   - the constants (TSTN/HEXIN/BININ/UNMINS);
 *   - the twelve built-ins;
 *   - the transcendentals;
 *   - this unit's trunk bodies: the TSTVB chain that reads variables,
 *     QSTR, BRA, the file-pointer family RPTR/RPAGE/RTIME/RLOMEM/
 *     RHIMEM, EVAL and VAL.
 * unit-factor-resid translated every remaining one-off built-in (RND
 * GET INKEY OPENIN/OUT/UP POINT USR EOF STR$ GET$ INKEY$ SUM BEAT, the
 * SYS-function tree, DIM MOD VDU TRACE WIDTH REP$, the TIME$-argument
 * forms and so on).  So the default arm is FACERR, which is exactly the
 * lift's own arm for an error character.  Row 8's scaffold drop then
 * deleted the whole lift trunk, since nothing referred to it.
 *
 * The setjmp ledger for this unit follows.  It is the clrstk
 * enumeration.  Every BL whose lift site carries a resume point keeps
 * one at the lift's own address.  Where the containing function must
 * tail-call, the BL is in a noinline helper, because a function that
 * contains setjmp cannot use musttail (the endpr_gtargs rule).
 *   EXPR's BL FACTOR            &FC10263C   (Expr.s:786)
 *   EXPRRECUR's BL FACTOR       &FC102674   (Expr.s:801)
 *   CHANNL's BL FACTOR          &FC102F98   (Expr.s:1343)
 *   INTEXC's BL EXPR            &FC10DBE4   (Lexical.s:46)
 *   EXPRDN's BL EXPR            &FC10DC1C   (Lexical.s:60)
 *   ARLOOKCACHE's BL EXPR x2    &FC102394, &FC1023D8  (Expr.s:513,530)
 *   BRA's BL EXPR               &FC103788   (Factor.s:572)
 *   RPTR's BL CHAN              &FC10383C   (Factor.s:617)
 *   EVAL's BL FACTOR/EXPR x2    &FC103988, &FC1039B0  (Factor.s:720,730)
 *   EVAL's BL EVMATCH           &FC1039A8   (Factor.s:728)
 *   VAL's BL FACTOR             &FC103FEC   (Factor.s:1442)
 *   TSTVBNOTCACHE's BL LVNOTCACHE  &FC1035E8   (Factor.s:449)
 *   TSTVBCACHEARRAY's BL ARLOOKCACHE &FC103610  (Factor.s:460)
 * Plain checked calls (INTEGZ, INTEGB, SFIX, OSSTRI, FREAD, and MSG
 * on the error paths) carry no frame in the lift and none here.
 *
 * These quirks are kept.  Each is given with its line:
 *   - EXPRNEXT's compare doubles as the exit for a character that is
 *     not an operator.  A PRIORTABLE word below 256 has zero priority
 *     bits.  R7>=0 always holds, so LDRCS PC,[SP],#4 returns
 *     (Expr.s:810-811).
 *   - The relations' special bit (&10000000) is handled by
 *     EXPRNONRIGHT, in the landed operator bodies.  EXPRHARD only ever
 *     sees the thirteen real operator indices.
 *   - ARLOOKCACHE's word count steps EIGHT bytes past the end of the
 *     limit list (LDR R3,[R4],#8, Expr.s:544).  It re-reads the word at
 *     the OLD R4 for its zero test.  The word that holds the number of
 *     entries is the last but one of the block.
 *   - The element size depends on the type: 4 bytes by default, 8 for
 *     a float (TYPE=8), and 5 for everything above 8, which are the
 *     string types (CMP TYPE,#8; ADDHI; MOVEQ, Expr.s:552-556).
 *   - A ! or ? suffix after the closing bracket re-enters BIPLIN or
 *     BIQUER for the indirection (Expr.s:558-563).
 *   - QSTR accepts a quote inside a string only when it is doubled.
 *     A CR before the closing quote gives ERMISQ "Missing quote"
 *     (Factor.s:435-445).
 *   - VARIND's word read copes with an unaligned pointer.  It uses the
 *     LDW macro's sequence of ANDS/LDM/LSR/LSL/ORR (Factor.s:494).
 *   - VARNOTNUM's read of a $-string stops at CR or at 256 bytes.  On
 *     the full-buffer stop it subtracts the 256 back off before the -1
 *     (TEQ CLEN,R3; SUBEQ, Factor.s:504-511).
 *   - RTIME$ reads the 5-byte real-time clock into STRACC with a
 *     leading zero word (long time).  It measures the length by
 *     scanning to the first byte below 32 (Factor.s:636-646).
 *   - EVAL evaluates on a 256-byte staging block below SP, with the
 *     FSA+1024 check for stack room (ERDEEPNEST).  Afterwards it
 *     PURGECACHEs the line cache, because the evaluated text may have
 *     defined variables (Factor.s:719-735).
 *   - VAL leaves the lookahead for OSSTRI's stop mark and hands on to
 *     VAL0.  VAL0's '-' and '+' paths skip the sign and then read the
 *     number with FREAD.  VALMIN reads the character after the '-' and
 *     VALPLU the one after the '+' (Factor.s:1446-1465).
 *   - The top bit of the TSTVB cache probe's delta is the array tag
 *     (CMN R1,#1; BMI, Factor.s:469-471).  An array hit re-enters
 *     ARLOOKCACHE with the tag subtracted back off AELINE.
 *   - TSTVB1 (not an l-value at all) is the OPT_errors check.  If the
 *     error flag is set it goes to FACERR.  If it is clear, it reads
 *     [ARGP,#ASSPC] as the value (Factor.s:452-456).  So @% with errors
 *     off evaluates as the assembler's current address.
 *   - INTEXC's exit is the comma check.  R10=',' returns, and anything
 *     else gives ERCOMM (Lexical.s:50-51).
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"
#include "basicasm.h"               /* basicasm_unknown: the TSTVB1 hook */

#include <setjmp.h>

#include "expr_factor_rest.h"

/* Landed hand callees, by their units. */
#include "basicvfp_hand.h"          /* ABS SGN INT SQR LEN ASC INSTR
                                       CHRD LEFTD MIDD RIGHTD STRND */
#include "factor_trans.h"           /* SIN COS TAN ASN ACS ATN LN LOG
                                       EXP DEG RAD */
#include "factor_core.h"            /* TSTN HEXIN BININ UNMINS */
#include "basic_conv.h"             /* INTEGY INTEGZ INTEGB SFIX */
#include "expr_lv.h"                /* LVNOTCACHE BIPLIN BIQUER */
#include "lexical_scan.h"           /* OSSTRI AEDONE */
#include "lexical_match.h"          /* EVMATCH */
#include "basic_organs.h"           /* MSG PURGECACHE */
#include "funct_call.h"            /* FN (case 164's target) */
#include "factor_resid.h"          /* the residual built-ins: every
                                      case the lift trunk still owned */

/* The lift's functions that this unit calls.  The manifest's EXPORTS
 * make them non-static in the twin. */
void basicvfp_FACERR(struct ros_cpu *s);
void basicvfp_ERARRZ(struct ros_cpu *s);
void basicvfp_ERRSUB(struct ros_cpu *s);
void basicvfp_ERRSB2(struct ros_cpu *s);
void basicvfp_ERVARAR(struct ros_cpu *s);
void basicvfp_ERBRA(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_ERCOMM(struct ros_cpu *s);
void basicvfp_MISSEQ(struct ros_cpu *s);
void basicvfp_ERDEEPNEST(struct ros_cpu *s);
void basicvfp_VALCMP(struct ros_cpu *s);
void basicvfp_FREAD(struct ros_cpu *s);
/* The thunks of the operator ladder (FUNCTION_SUBS), for the thirteen
 * real operator indices of the ladder's jump table. */
void basicvfp_EXPRADD(struct ros_cpu *s);
void basicvfp_EXPRSUB(struct ros_cpu *s);
void basicvfp_EXPRMUL(struct ros_cpu *s);
void basicvfp_EXPRDIV(struct ros_cpu *s);
void basicvfp_EXPRPOW(struct ros_cpu *s);
void basicvfp_EXPROR(struct ros_cpu *s);
void basicvfp_EXPREOR(struct ros_cpu *s);
void basicvfp_EXPRAND(struct ros_cpu *s);
void basicvfp_EXPRLT(struct ros_cpu *s);
void basicvfp_EXPREQ(struct ros_cpu *s);
void basicvfp_EXPRGT(struct ros_cpu *s);
void basicvfp_EXPRINTDIV(struct ros_cpu *s);
void basicvfp_EXPRMOD(struct ros_cpu *s);

/* The native SWIs that the bodies of RPTR and RTIME call. */
void ros_thunk_OS_Args(struct ros_cpu *s);
void ros_thunk_OS_Word(struct ros_cpu *s);

/* The lifter's tail-call and poll, as the tree's cpu.h spells them.
 * The poll is a no-op where cpu.h does not define it, because the
 * harness lift carries no polls. */
#if defined(__clang__)
# define EFR_TAIL_CALL(f) __attribute__((musttail)) return f(s);
#elif defined(__GNUC__) && __GNUC__ >= 15
# define EFR_TAIL_CALL(f) return __attribute__((musttail)) f(s);
# else
# define EFR_TAIL_CALL(f) return f(s);
#endif
#ifndef ROS_POLL
# define ROS_POLL(s, sp) do { } while (0)
#endif

/* Type bits, the cache geometry and the evaluator's addresses.  These
 * are the lift's own spellings, and they are identical redefinitions
 * in the twin. */
#define TFP         0x80000000u
#define TINTEGER    0x40000000u
#define CACHEMASK   255u
#define CACHESHIFT  4
#define STRACC_OFF 1536u
#define OPT_errors  2u
#define PRIORTABLE  0xFC102A4Cu     /* BASICVFP_PRIORTABLE */
#define AJ7         0xFC1026A0u     /* BASICVFP_AJ7 */
#define FACTORFTAB  0xFC102FB0u     /* FACTOR's own jump table */
#define OsWord_ReadSystemClock 1u
#define OsWord_ReadRealTimeClock 14u

/* ---- the BL call sites, at the lift's own addresses ------------- */

#define SITE_EXPR_FACTOR      0xFC10263Cu
#define SITE_EXPRRECUR_FACTOR 0xFC102674u
#define SITE_CHANNL_FACTOR    0xFC102F98u
#define SITE_INTEXC_EXPR      0xFC10DBE4u
#define SITE_EXPRDN_EXPR      0xFC10DC1Cu
#define SITE_ARL_EXPR1        0xFC102394u
#define SITE_ARL_EXPR2        0xFC1023D8u
#define SITE_BRA_EXPR         0xFC103788u
#define SITE_RPTR_CHAN        0xFC10383Cu
#define SITE_EVAL_FACTOR      0xFC103988u
#define SITE_EVAL_EVMATCH     0xFC1039A8u
#define SITE_EVAL_EXPR        0xFC1039B0u
#define SITE_VAL_FACTOR       0xFC103FECu
#define SITE_TSTVB_LV         0xFC1035E8u
#define SITE_TSTVBA_ARL       0xFC103610u

/* The real addresses of the MSG error sites.  MSG reads its error
 * number and token from the words after these. */
#define SITE_ERMISQ  0xFC10FF7Cu       /* QSTR's unterminated string */
#define SITE_CHANNE  0xFC110144u       /* CHAN's missing '#' */

/* A return address that nobody checks, like the BL markers of the
 * landed units. */
#define EFR_MARKER   0xFFFFFFF0u

/* The error exit.  Hand the block to MSG, which never returns.  The
 * fault catches a MSG that does return.  The lift's frames around
 * these BLs never resume, because the error path does not come back.
 * So no frame is pushed, as in factor_core. */
static void efr_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* The evaluator's BL, with one noinline helper per site.
 * PERFORMANCE PHASE (P1): the setjmp'd resume frame that this used to
 * push at every BL is GONE.  The only resumer that ever targeted these
 * sites was the FN return.  FNRET's ros_resume(link) longjmp'd back
 * into the BL site with the FN's value in the registers.  That return
 * now rides the C stack instead (see FNRET and GTARGS).  The whole FN
 * call is a tail-call chain from this helper's call: dispatcher case
 * 164, FNBODY, FNGOA/FNGOACACHE, DOFN's hand_STMT, and the body's
 * statement cycle.  So the body's FNRET returns straight back here.
 * R15 is already the site, and ros_check_return passes exactly as it
 * did after the resume.  Errors longjmp to the environment's frames,
 * which are a different chain.  GOTO-class transfers resume to the
 * statement loop's frames, which are the lift's own pushes.  Neither
 * ever targeted these BLs.  It was measured before the change: the
 * setjmp at each BL took about 90% of the twin's remaining statement
 * time (sieve went from 23cs to 1cs when it was removed). */
#define EFR_RESUME(name, site, call)                                   \
    __attribute__((noinline)) static void name(struct ros_cpu *s)      \
    {                                                                  \
        s->r[14] = (site);                                             \
        call;                                                          \
        ros_check_return(s, (site));                                   \
    }

EFR_RESUME(efr_expr_factor, SITE_EXPR_FACTOR, basicvfp_hand_FACTOR(s))
EFR_RESUME(efr_exprrecur_factor, SITE_EXPRRECUR_FACTOR, basicvfp_hand_FACTOR(s))
EFR_RESUME(efr_channl_factor, SITE_CHANNL_FACTOR, basicvfp_hand_FACTOR(s))
EFR_RESUME(efr_intexc_expr, SITE_INTEXC_EXPR, basicvfp_hand_EXPR(s))
EFR_RESUME(efr_exprdn_expr, SITE_EXPRDN_EXPR, basicvfp_hand_EXPR(s))
EFR_RESUME(efr_arl_expr1, SITE_ARL_EXPR1, basicvfp_hand_EXPR(s))
EFR_RESUME(efr_arl_expr2, SITE_ARL_EXPR2, basicvfp_hand_EXPR(s))
EFR_RESUME(efr_bra_expr, SITE_BRA_EXPR, basicvfp_hand_EXPR(s))
EFR_RESUME(efr_rptr_chan, SITE_RPTR_CHAN, basicvfp_hand_CHAN(s))
EFR_RESUME(efr_eval_factor, SITE_EVAL_FACTOR, basicvfp_hand_FACTOR(s))
EFR_RESUME(efr_eval_evmatch, SITE_EVAL_EVMATCH, basicvfp_hand_EVMATCH(s))
EFR_RESUME(efr_eval_expr, SITE_EVAL_EXPR, basicvfp_hand_EXPR(s))
EFR_RESUME(efr_val_factor, SITE_VAL_FACTOR, basicvfp_hand_FACTOR(s))
EFR_RESUME(efr_tstvb_lv, SITE_TSTVB_LV, basicvfp_hand_LVNOTCACHE(s))
EFR_RESUME(efr_tstvba_arl, SITE_TSTVBA_ARL, basicvfp_hand_ARLOOKCACHE(s))

/* ---- the exit joins (Factor.s:860-861, 977-978) ----------------------
 * These take one argument, like everything the trunk tail-calls,
 * because musttail needs matching signatures.  The value goes through
 * R[0].  The path has already made the rest of the exit's stores. */

/* SINSTK: return an integer in IACC with TYPE=TINTEGER to the
 * caller's link.  PSINSTK does the same, but pops the link from the one
 * arena word that the body pushed. */
static void efr_sinstk(struct ros_cpu *s)
{
    uint32_t r9 = ros_logic(s, TINTEGER, 0);   /* MOVS TYPE,#TINTEGER */
    s->r[9] = r9;
    s->r[15] = s->r[14];                        /* MOV PC,R14 */
}

static void efr_psinstk(struct ros_cpu *s)
{
    uint32_t r9 = ros_logic(s, TINTEGER, 0);   /* MOVS TYPE,#TINTEGER */
    uint32_t r14 = ros_ld32(s->r[13]);         /* LDR PC,[SP],#4 */
    s->r[9] = r9;
    s->r[14] = r14;
    s->r[15] = r14;
    s->r[13] += 4;
}

/* ---- EXPR (Expr.s:785-795) and the deferral protocol ---------------- */

void basicvfp_hand_EXPR(struct ros_cpu *s)
{
    uint32_t r4, r7 = s->r[7], r10, r11 = s->r[11], r13 = s->r[13],
             r14 = s->r[14];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    efr_expr_factor(s);                         /* BL FACTOR */
    r7 = s->r[7]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    do {                                        /* EXPRBLNK */
        ROS_POLL(s, r13);
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
    } while (r10 == 32);                        /* BEQ EXPRBLNK */
    r4 = PRIORTABLE;                            /* ADR R4,PRIORTABLE */
    r10 = ros_ld32(PRIORTABLE + (r10 << 2));    /* LDR R10,[R4,R10,LSL #2] */
    ros_subs(s, r10, 0x100u);                   /* CMP R10,#256 */
    if (r10 < 0x100u) {                         /* LDRCC PC,[SP],#4 */
        s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
        s->r[15] = ros_ld32(r13);
        s->r[13] = r13 + 4;
        return;
    }
    r7 = 0;             /* MOV R7,#0 ; expression a tough one after all! */
    r4 = AJ7;                                  /* ADR R4,AJ7 */
    r14 = r10 & 0xFFFFFFu;                      /* BIC R14,R10,#&FF000000 */
    s->r[4] = r4; s->r[7] = r7; s->r[10] = r10; s->r[11] = r11;
    s->r[14] = r14;
    switch (r14) {                              /* ADD PC,R4,R14,LSL #2 */
    case 159: EFR_TAIL_CALL(basicvfp_EXPRMUL)
    case 86:  EFR_TAIL_CALL(basicvfp_EXPRADD)
    case 144: EFR_TAIL_CALL(basicvfp_EXPRSUB)
    case 206: EFR_TAIL_CALL(basicvfp_EXPRDIV)
    case 35:  EFR_TAIL_CALL(basicvfp_EXPRLT)
    case 55:  EFR_TAIL_CALL(basicvfp_EXPREQ)
    case 58:  EFR_TAIL_CALL(basicvfp_EXPRGT)
    case 505: EFR_TAIL_CALL(basicvfp_EXPRPOW)
    case 24:  EFR_TAIL_CALL(basicvfp_EXPRAND)
    case 227: EFR_TAIL_CALL(basicvfp_EXPRINTDIV)
    case 13:  EFR_TAIL_CALL(basicvfp_EXPREOR)
    case 491: EFR_TAIL_CALL(basicvfp_EXPRMOD)
    case 2:   EFR_TAIL_CALL(basicvfp_EXPROR)
    default: ros_fault(s, 0xFC102664u, "a jump table index out of range");
    }
}

/* ---- EXPRRECUR (Expr.s:799-806): the right-operand read -------------
 * R10 must be the PRIORTABLE word of the pending operator. */
void basicvfp_hand_EXPRRECUR(struct ros_cpu *s)
{
    uint32_t r7, r10 = s->r[10], r11 = s->r[11], r13 = s->r[13];

    r7 = r10 >> 28;                             /* MOV R7,R10,LSR #28 */
    r13 -= 8;                                   /* STMFD SP!,{R7,R14} */
    ros_st32(r13, r7);
    ros_st32(r13 + 4, s->r[14]);
    s->r[7] = r7; s->r[13] = r13;
    efr_exprrecur_factor(s);                    /* BL FACTOR */
    r11 = s->r[11]; r13 = s->r[13];
    r7 = ros_ld32(r13);                         /* LDR R7,[SP],#4 */
    r13 += 4;
    do {                                        /* EXPRBLNK1 */
        ROS_POLL(s, r13);
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
    } while (r10 == 32);
    s->r[7] = r7; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    EFR_TAIL_CALL(basicvfp_hand_EXPRNEXT)
}

/* ---- EXPRNEXT (Expr.s:808-811): defer or chain ---------------------- */

void basicvfp_hand_EXPRNEXT(struct ros_cpu *s)
{
    uint32_t r4, r7 = s->r[7], r10 = s->r[10], r13 = s->r[13];
    uint32_t v1;

    r4 = PRIORTABLE;                            /* ADR R4,PRIORTABLE */
    r10 = ros_ld32(PRIORTABLE + (r10 << 2));    /* LDR R10,[R4,R10,LSL #2] */
    v1 = r10 >> 28;                             /* CMP R7,R10,LSR #28 */
    ros_subs(s, r7, v1);
    if (r7 >= v1) {   /* LDRCS PC,[SP],#4 ; also exits if R10<256 */
        s->r[4] = r4; s->r[10] = r10;
        s->r[15] = ros_ld32(r13);
        s->r[13] = r13 + 4;
        return;
    }
    s->r[4] = r4; s->r[10] = r10;
    EFR_TAIL_CALL(basicvfp_hand_EXPRHARD)
}

/* ---- EXPRHARD (Expr.s:813-814): the operators' shared re-entry -------
 * The index is the low 24 bits of the PRIORTABLE word, so only the
 * thirteen real operators can arrive here.  The lift's full switch
 * also covers the table's filler words (the &FC1026xx join points).
 * Those cannot be reached with an operator's index, and they stay lift
 * in the fragments. */
void basicvfp_hand_EXPRHARD(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10], r14;

    r14 = r10 & 0xFFFFFFu;                      /* BIC R14,R10,#&FF000000 */
    s->r[14] = r14;
    switch (r14) {                              /* ADD PC,PC,R14,LSL #2 */
    case 159: EFR_TAIL_CALL(basicvfp_EXPRMUL)
    case 86:  EFR_TAIL_CALL(basicvfp_EXPRADD)
    case 144: EFR_TAIL_CALL(basicvfp_EXPRSUB)
    case 206: EFR_TAIL_CALL(basicvfp_EXPRDIV)
    case 35:  EFR_TAIL_CALL(basicvfp_EXPRLT)
    case 55:  EFR_TAIL_CALL(basicvfp_EXPREQ)
    case 58:  EFR_TAIL_CALL(basicvfp_EXPRGT)
    case 505: EFR_TAIL_CALL(basicvfp_EXPRPOW)
    case 24:  EFR_TAIL_CALL(basicvfp_EXPRAND)
    case 227: EFR_TAIL_CALL(basicvfp_EXPRINTDIV)
    case 13:  EFR_TAIL_CALL(basicvfp_EXPREOR)
    case 491: EFR_TAIL_CALL(basicvfp_EXPRMOD)
    case 2:   EFR_TAIL_CALL(basicvfp_EXPROR)
    default: ros_fault(s, 0xFC102698u, "a jump table index out of range");
    }
}

/* ---- AEEXPR (Expr.s:784): the statement-body entry ------------------ */

void basicvfp_hand_AEEXPR(struct ros_cpu *s)
{
    s->r[11] = s->r[12];                        /* MOV AELINE,LINE */
    EFR_TAIL_CALL(basicvfp_hand_EXPR)
}

/* ---- the Lexical.s evaluator entries --------------------------------- */

/* INTEXA/INTEXC (Lexical.s:45-50): set AELINE=LINE and call EXPR.
 * The result must be a number, and INTEGB converts a float.  Then a
 * comma must follow, or it is ERCOMM. */
void basicvfp_hand_INTEXA(struct ros_cpu *s)
{
    s->r[11] = s->r[12];                        /* MOV AELINE,LINE */
    EFR_TAIL_CALL(basicvfp_hand_INTEXC)
}

void basicvfp_hand_INTEXC(struct ros_cpu *s)
{
    uint32_t r9, r10, r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    efr_intexc_expr(s);                         /* BL EXPR */
    r9 = s->r[9]; r10 = s->r[10]; r13 = s->r[13];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 == 0) {                              /* BEQ ERTYPEINT */
        s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_ERTYPEINT)
    }
    if ((int32_t)r9 < 0) {                      /* BLMI INTEGB */
        s->r[14] = 0xFC10DBF0u;
        basicvfp_hand_INTEGB(s);
        ros_check_return(s, 0xFC10DBF0u);
        r10 = s->r[10]; r13 = s->r[13];
    }
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 == 44) {                            /* LDREQ PC,[SP],#4 */
        s->r[15] = ros_ld32(r13);
        s->r[13] = r13 + 4;
        return;
    }
    EFR_TAIL_CALL(basicvfp_ERCOMM)              /* B ERCOMM */
}

/* EQAEEX (Lexical.s:53-57): skip spaces at LINE and expect '=', then
 * go on to AEEXDN.  Anything else gives MISSEQ. */
void basicvfp_hand_EQAEEX(struct ros_cpu *s)
{
    uint32_t r10, r12 = s->r[12];

    do {
        ROS_POLL(s, s->r[13]);
        r10 = ros_ld8(r12);                     /* LDRB R10,[LINE],#1 */
        r12 += 1;
    } while (r10 == 32);                        /* BEQ EQAEEX */
    ros_subs(s, r10, 61);                       /* CMP R10,#"=" */
    if (r10 != 61) {
        s->r[10] = r10; s->r[12] = r12;
        EFR_TAIL_CALL(basicvfp_MISSEQ)          /* BNE MISSEQ */
    }
    s->r[10] = r10; s->r[12] = r12;
    EFR_TAIL_CALL(basicvfp_hand_AEEXDN)
}

/* AEEXDN (Lexical.s:58): set AELINE=LINE, then EXPRDN. */
void basicvfp_hand_AEEXDN(struct ros_cpu *s)
{
    s->r[11] = s->r[12];                        /* MOV AELINE,LINE */
    EFR_TAIL_CALL(basicvfp_hand_EXPRDN)
}

/* EXPRDN (Lexical.s:59-63): call EXPR.  The result must be a number.
 * Then go on to AEDONE. */
void basicvfp_hand_EXPRDN(struct ros_cpu *s)
{
    uint32_t r9, r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    efr_exprdn_expr(s);                         /* BL EXPR */
    r9 = s->r[9]; r13 = s->r[13]; r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 == 0) {                              /* BEQ ERTYPEINT */
        s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_ERTYPEINT)
    }
    if ((int32_t)r9 < 0) {                      /* BLMI INTEGB */
        s->r[14] = 0xFC10DC28u;
        basicvfp_hand_INTEGB(s);
        ros_check_return(s, 0xFC10DC28u);
        r13 = s->r[13];
    }
    r14 = ros_ld32(r13);                        /* LDR R14,[SP],#4 */
    r13 += 4;
    s->r[13] = r13; s->r[14] = r14;
    EFR_TAIL_CALL(basicvfp_hand_AEDONE)         /* B AEDONE */
}

/* ---- the channel family (Expr.s:1336-1346) -------------------------- */

/* AECHAN: the #channel expression at LINE. */
void basicvfp_hand_AECHAN(struct ros_cpu *s)
{
    s->r[11] = s->r[12];                        /* MOV AELINE,LINE */
    EFR_TAIL_CALL(basicvfp_hand_CHAN)
}

/* CHAN: skip the spaces and expect '#'.  Anything else gives the
 * CHANNE error. */
void basicvfp_hand_CHAN(struct ros_cpu *s)
{
    uint32_t r10, r11 = s->r[11];

    do {
        ROS_POLL(s, s->r[13]);
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
    } while (r10 == 32);                        /* BEQ CHAN */
    ros_subs(s, r10, 35);                       /* CMP R10,#"#" */
    if (r10 != 35) {                            /* BNE CHANNE */
        s->r[10] = r10; s->r[11] = r11;
        efr_msg_at(s, SITE_CHANNE);
    }
    s->r[10] = r10; s->r[11] = r11;
    EFR_TAIL_CALL(basicvfp_hand_CHANNL)
}

/* CHANNL: FACTOR then INTEGZ.  The channel number is left in R1 for
 * the OS. */
void basicvfp_hand_CHANNL(struct ros_cpu *s)
{
    uint32_t r0, r1, r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    efr_channl_factor(s);                       /* BL FACTOR */
    r0 = s->r[0]; r13 = s->r[13];
    s->r[14] = 0xFC102F9Cu;                     /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC102F9Cu);
    r0 = s->r[0]; r13 = s->r[13];
    r1 = r0;                                    /* MOV R1,IACC */
    s->r[0] = r0; s->r[1] = r1;
    s->r[15] = ros_ld32(r13);                   /* LDR PC,[SP],#4 */
    s->r[13] = r13 + 4;
}

/* ---- ARLOOKCACHE (Expr.s:505-567): the array element evaluator ------- */

void basicvfp_hand_ARLOOKCACHE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r3 = s->r[3], r4 = s->r[4], r5 = s->r[5],
             r6 = s->r[6], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13], r14 = s->r[14];
    uint32_t v1;

    r1 = ros_ld8(r11);                          /* LDRB R1,[AELINE] */
    ros_subs(s, r1, 41);                        /* CMP R1,#")" */
    if (r1 == 41) {                             /* BEQ ARRAYREF */
        r9 = ros_logic(s, r9 | 0x100u, s->c);   /* ORRS TYPE,TYPE,#256 */
        r11 += 1;                               /* ADD AELINE,AELINE,#1 */
        s->r[1] = r1; s->r[9] = r9; s->r[11] = r11;
        s->r[15] = r14;                         /* MOV PC,R14 */
        return;
    }
    r0 = ros_ld32(r0);                          /* LDR IACC,[IACC] */
    ros_subs(s, r0, 16);                        /* CMP IACC,#16 */
    if (r0 < 16) {                              /* BCC ERARRZ */
        s->r[0] = r0; s->r[1] = r1;
        EFR_TAIL_CALL(basicvfp_ERARRZ)
    }
    r13 -= 12;                                  /* STMFD SP!,{IACC,TYPE,R14} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    ros_st32(r13 + 8, r14);
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    efr_arl_expr1(s);                           /* BL EXPR */
    r0 = s->r[0]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 == 0) {                              /* BEQ ERTYPEINT */
        s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_ERTYPEINT)
    }
    if ((int32_t)r9 < 0) {                      /* BLMI SFIX */
        s->r[14] = 0xFC1023A0u;
        basicvfp_hand_SFIX(s);
        ros_check_return(s, 0xFC1023A0u);
        r0 = s->r[0]; r5 = s->r[5]; r6 = s->r[6]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    }
    r4 = ros_ld32(r13);              /* LDR R4,[SP],#4 ; the limit list */
    r13 += 4;
    r3 = ros_ld32(r4);                          /* LDR R3,[R4],#4 */
    r4 += 4;
    ros_subs(s, r0, r3);                        /* CMP IACC,R3 */
    if (r0 >= r3) {                             /* BCS ERRSUB */
        s->r[3] = r3; s->r[4] = r4; s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_ERRSUB)
    }
    ros_subs(s, r10, 41);                       /* CMP R10,#")" */
    if (r10 != 41) {                            /* BEQ ARREND */
        ros_subs(s, r10, 44);                   /* CMP R10,#"," */
        if (r10 != 44) {                        /* BNE ERBRA */
            s->r[3] = r3; s->r[4] = r4; s->r[13] = r13;
            EFR_TAIL_CALL(basicvfp_ERBRA)
        }
        r5 = 0;                                 /* MOV R5,#0 */
        do {                                    /* ARLOP */
            ROS_POLL(s, r13);
            r0 = r5 + r0;                       /* ADD IACC,R5,IACC */
            r6 = ros_ld32(r4);                  /* LDR R6,[R4] */
            r5 = r6 * r0;                       /* MUL R5,R6,IACC */
            r13 -= 8;                           /* STMFD SP!,{R4,R5} */
            ros_st32(r13, r4);
            ros_st32(r13 + 4, r5);
            s->r[0] = r0; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
            s->r[6] = r6; s->r[13] = r13;
            efr_arl_expr2(s);                   /* BL EXPR */
            r0 = s->r[0]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
            r6 = s->r[6]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
            r13 = s->r[13]; r14 = s->r[14];
            ros_logic(s, r9, s->c);             /* TEQ TYPE,#0 */
            if (r9 == 0) {                      /* BEQ ERTYPEINT */
                s->r[13] = r13;
                EFR_TAIL_CALL(basicvfp_ERTYPEINT)
            }
            if ((int32_t)r9 < 0) {              /* BLMI SFIX */
                s->r[14] = 0xFC1023E4u;
                basicvfp_hand_SFIX(s);
                ros_check_return(s, 0xFC1023E4u);
                r0 = s->r[0]; r6 = s->r[6]; r9 = s->r[9]; r10 = s->r[10];
                r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
            }
            r4 = ros_ld32(r13);                 /* LDMFD SP!,{R4,R5} */
            r5 = ros_ld32(r13 + 4);
            r13 += 8;
            r3 = ros_ld32(r4);                  /* LDR R3,[R4],#4 */
            r4 += 4;
            ros_subs(s, r0, r3);                /* CMP IACC,R3 */
            if (r0 >= r3) {                     /* BCS ERRSUB */
                s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
                EFR_TAIL_CALL(basicvfp_ERRSUB)
            }
            ros_subs(s, r10, 44);               /* CMP R10,#"," */
        } while (r10 == 44);                    /* BEQ ARLOP */
        r5 += r0;                               /* ADD R5,R5,IACC */
        ros_subs(s, r10, 41);                   /* CMP R10,#")" */
        if (r10 != 41) {                        /* BNE ERBRA */
            s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
            EFR_TAIL_CALL(basicvfp_ERBRA)
        }
        r0 = r5;                                /* MOV IACC,R5 */
    }
    v1 = r4;                                    /* LDR R3,[R4],#8 */
    r3 = ros_ld32(r4);
    r4 += 8;
    ros_logic(s, ros_ld32(v1), s->c);           /* TEQ R3,#0 */
    if (r3 != 0) {                              /* BNE ERRSB2 */
        s->r[0] = r0; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
        s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_ERRSB2)
    }
    r1 = r0 << 2;                               /* MOV R1,IACC,LSL #2 */
    r9 = ros_ld32(r13);                         /* LDMFD SP!,{TYPE,R14} */
    r14 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_subs(s, r9, 8);                         /* CMP TYPE,#8 */
    /* ADDHI R1,R1,IACC (type>8: five bytes); MOVEQ R1,IACC,LSL #3 */
    r1 = r9 == 8 ? r0 << 3 : r9 > 8 ? r1 + r0 : r1;
    r0 = r1 + r4;                               /* ADD IACC,R1,R4 */
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE] */
    if ((r10 ^ 0x21u) == 0) {                   /* TEQ R10,#"!" */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[9] = r9; s->r[10] = r10; s->r[13] = r13;
        s->r[14] = r14;
        EFR_TAIL_CALL(basicvfp_hand_BIPLIN)     /* BEQ BIPLIN */
    }
    r1 = ros_logic(s, r10 ^ 0x3Fu, s->c);       /* EORS R1,R10,#"?" */
    if (r1 != 0) {                              /* MOVNE PC,R14 */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[9] = r9; s->r[10] = r10; s->r[13] = r13;
        s->r[14] = r14;
        s->r[15] = r14;
        return;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[9] = r9; s->r[10] = r10; s->r[13] = r13;
    s->r[14] = r14;
    EFR_TAIL_CALL(basicvfp_hand_BIQUER)         /* B BIQUER */
}

/* ---- QSTR (Factor.s:434-446): the quoted string ---------------------- */

void basicvfp_hand_QSTR(struct ros_cpu *s)
{
    uint32_t r2, r8 = s->r[8], r10, r11 = s->r[11];

    r2 = r8 - STRACC_OFF;                       /* ADD CLEN,ARGP,#STRACC */
    for (;;) {                                  /* QSTRLOP */
        ROS_POLL(s, s->r[13]);
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
        if (r10 == 13) {                        /* CMP R10,#13 ; BEQ ERMISQ */
            s->r[2] = r2; s->r[10] = r10; s->r[11] = r11;
            efr_msg_at(s, SITE_ERMISQ);
        }
        if (r10 != 34) {                        /* STRNEB R10,[CLEN],#1 */
            ros_st8(r2, r10);
            r2 += 1;
        } else {                                /* a quote: doubled only */
            r10 = ros_ld8(r11);                 /* LDRB R10,[AELINE],#1 */
            r11 += 1;
            if (r10 == 34) {                    /* STREQB R10,[CLEN],#1 */
                ros_st8(r2, r10);
                r2 += 1;
            }
            if (r10 != 34) {                    /* BEQ QSTRLOP */
                r11 -= 1;                       /* SUB AELINE,AELINE,#1 */
                s->r[2] = r2; s->r[10] = r10; s->r[11] = r11;
                EFR_TAIL_CALL(basicvfp_hand_RNULX)
            }
        }
    }
}

/* ---- DATAST (Factor.s:419-433): the DATA item reader ----------------- */

void basicvfp_hand_DATAST(struct ros_cpu *s)
{
    uint32_t r2 = s->r[2], r8 = s->r[8], r10, r11 = s->r[11];

    do {
        ROS_POLL(s, s->r[13]);
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
    } while (r10 == 32);                        /* BEQ DATAST */
    if (r10 == 34) {                            /* CMP R10,#"""" ; BEQ QSTR */
        s->r[10] = r10; s->r[11] = r11;
        EFR_TAIL_CALL(basicvfp_hand_QSTR)
    }
    r2 = r8 - STRACC_OFF;                       /* ADD CLEN,ARGP,#STRACC */
    r11 -= 1;                                   /* SUB AELINE,AELINE,#1 */
    do {                                        /* DATASL */
        ROS_POLL(s, s->r[13]);
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
        ros_st8(r2, r10);                       /* STRB R10,[CLEN],#1 */
        r2 += 1;
        ros_subs(s, r10, 44);                   /* CMP R10,#"," */
        if (r10 != 44)
            ros_subs(s, r10, 13);               /* CMPNE R10,#13 */
    } while (r10 != 44 && r10 != 13);           /* BNE DATASL */
    r2 -= 1;                                    /* SUB CLEN,CLEN,#1 */
    r11 -= 1;                                   /* SUB AELINE,AELINE,#1 */
    s->r[2] = r2; s->r[10] = r10; s->r[11] = r11;
    EFR_TAIL_CALL(basicvfp_hand_RNULX)          /* B RNULX */
}

/* ---- RNUL/RNULX (Factor.s:1505-1506): the empty-string exits --------- */

void basicvfp_hand_RNUL(struct ros_cpu *s)
{
    uint32_t r2, r8 = s->r[8];
    r2 = r8 - STRACC_OFF;                       /* ADD CLEN,ARGP,#STRACC */
    s->r[2] = r2;
    EFR_TAIL_CALL(basicvfp_hand_RNULX)
}

void basicvfp_hand_RNULX(struct ros_cpu *s)
{
    uint32_t r9 = ros_logic(s, 0, s->c);        /* MOVS TYPE,#0 */
    s->r[9] = r9;
    s->r[15] = s->r[14];                        /* MOV PC,R14 */
}

/* ---- the typed variable read (Factor.s:472-525) ---------------------- */

/* VARIND: IACC points at the data, TYPE says what it is. */
void basicvfp_hand_VARIND(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r9 = s->r[9],
             r14 = s->r[14];

    ros_subs(s, r9, 4);                         /* CMP TYPE,#4 */
    if (r9 < 4) {                              /* BCC VARBYT */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[9] = r9;
        EFR_TAIL_CALL(basicvfp_hand_VARBYT)
    }
    if (r9 == 4) {                              /* BEQ VARINT */
        uint32_t v1 = r0, v2 = r0 & 3;          /* ANDS R1,IACC,#3 */
        r1 = v2;
        r2 = r0 & ~3u;                          /* BIC R2,IACC,#3 */
        if (v2 == 0) {
            r0 = ros_ld32(v1);                  /* LDREQ IACC,[IACC] */
        } else {                                /* LDMNEIA R2,{IACC,R2} */
            uint32_t lo = ros_ld32(r2), hi = ros_ld32(r2 + 4);
            /* IACC>>R1*8 | R2<<(32-R1*8): the LDW macro's tail */
            r0 = ros_lsr(lo, v2 << 3) | ros_lsl(hi, 32 - (v2 << 3));
            r2 = hi;
            r1 = 32 - (v2 << 3);                /* RSBNE R1,R1,#32 */
        }
        r9 = ros_logic(s, TINTEGER, 0);         /* MOVS TYPE,#TINTEGER */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[9] = r9;
        s->r[15] = r14;                         /* MOV PC,R14 */
        return;
    }
    ros_subs(s, r9, 128);                       /* CMP TYPE,#128 */
    if (r9 >= 128) {                            /* BCS VARNOTNUM */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[9] = r9;
        EFR_TAIL_CALL(basicvfp_hand_VARNOTNUM)
    }
    s->fp->vfp.d[0] = ros_ldd(r0);              /* FLDD FACC,[IACC] */
    r9 = ros_logic(s, TFP, 1);                  /* MOVS TYPE,#TFP */
    s->r[9] = r9;
    s->r[15] = r14;                             /* MOV PC,R14 */
}

/* VARBYT: a byte. */
void basicvfp_hand_VARBYT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r9, r14 = s->r[14];
    r0 = ros_ld8(r0);                           /* LDRB IACC,[IACC] */
    r9 = ros_logic(s, TINTEGER, 0);             /* MOVS TYPE,#TINTEGER */
    s->r[0] = r0; s->r[9] = r9;
    s->r[15] = r14;                             /* MOV PC,R14 */
}

/* VARNOTNUM: a $-string's bytes, or a string control block's copy. */
void basicvfp_hand_VARNOTNUM(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r3 = s->r[3], r8 = s->r[8],
             r9 = s->r[9], r14 = s->r[14];

    if (s->z) {                                 /* BEQ VARSTR */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[9] = r9;
        EFR_TAIL_CALL(basicvfp_hand_VARSTR)
    }
    ros_subs(s, r9, 0x100u);                    /* CMP TYPE,#256 */
    if (r9 >= 0x100u) {                         /* BCS ERVARAR */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = s->r[2]; s->r[3] = r3;
        s->r[9] = r9;
        EFR_TAIL_CALL(basicvfp_ERVARAR)
    }
    r2 = r8 - STRACC_OFF;                       /* ADD CLEN,ARGP,#STRACC */
    r3 = r2 + 257;                              /* ADD R3,CLEN,#256+#1 */
    do {                                        /* VARRPA */
        ROS_POLL(s, s->r[13]);
        r1 = ros_ld8(r0);                       /* LDRB R1,[IACC],#1 */
        r0 += 1;
        ros_st8(r2, r1);                        /* STRB R1,[CLEN],#1 */
        r2 += 1;
    } while ((r2 ^ r3) != 0 && (r1 ^ 0xDu) != 0); /* TEQ CLEN,R3 ; TEQNE R1,#13 */
    /* SUBEQ CLEN,CLEN,#256 ; SUB CLEN,CLEN,#1 */
    r2 = ((r2 ^ r3) == 0 ? r2 - 0x100u : r2) - 1;
    r9 = ros_logic(s, 0, s->c);                 /* MOVS TYPE,#0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[9] = r9;
    s->r[15] = r14;                             /* MOV PC,R14 */
}

/* VARSTR: a string variable's descriptor {pointer, length}. */
void basicvfp_hand_VARSTR(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r3 = s->r[3], r8 = s->r[8],
             r9 = s->r[9], r14 = s->r[14];

    r2 = ros_ld8(r0 + 4);                       /* LDRB CLEN,[IACC,#4] */
    if (r2 == 0) {                              /* TEQ CLEN,#0 ; BEQ RNUL */
        s->r[2] = r2;
        EFR_TAIL_CALL(basicvfp_hand_RNUL)
    }
    {   /* the LDW macro's unaligned pointer load */
        uint32_t v1 = r0, v3 = r0 & 3;          /* ANDS R3,IACC,#3 */
        r1 = r0 & ~3u;                          /* BIC R1,IACC,#3 */
        if (v3 == 0) {
            r0 = ros_ld32(v1);                  /* LDREQ IACC,[IACC] */
        } else {
            uint32_t lo = ros_ld32(r1), hi = ros_ld32(r1 + 4);
            r0 = ros_lsr(lo, v3 << 3) | ros_lsl(hi, 32 - (v3 << 3));
            r1 = hi;
        }
    }
    r3 = r8 - STRACC_OFF;                       /* ADD R3,ARGP,#STRACC */
    r2 += r3;                                   /* ADD CLEN,CLEN,R3 */
    do {                                        /* VARST2 */
        ROS_POLL(s, s->r[13]);
        r1 = ros_ld32(r0);                      /* LDR R1,[IACC],#4 */
        r0 += 4;
        ros_st32(r3, r1);                       /* STR R1,[R3],#4 */
        r3 += 4;
        ros_subs(s, r3, r2);                    /* CMP R3,CLEN */
    } while (r3 < r2);                          /* BCC VARST2 */
    r9 = ros_logic(s, 0, s->c);                 /* MOVS TYPE,#0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[9] = r9;
    s->r[15] = r14;                             /* MOV PC,R14 */
}

/* ---- BRA (Factor.s:571-576): the parenthesised expression ------------ */

void basicvfp_hand_BRA(struct ros_cpu *s)
{
    uint32_t r9 = s->r[9], r10 = s->r[10], r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    efr_bra_expr(s);                            /* BL EXPR */
    r9 = s->r[9]; r10 = s->r[10]; r13 = s->r[13];
    ros_subs(s, r10, 41);                       /* CMP R10,#")" */
    if (r10 != 41) {                            /* BNE ERBRA */
        s->r[9] = r9; s->r[10] = r10; s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_ERBRA)
    }
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    s->r[15] = ros_ld32(r13);                   /* LDR PC,[SP],#4 */
    s->r[13] = r13 + 4;
}

/* ---- RPTR and its family (Factor.s:616-650) -------------------------- */

/* RPTR: PTR#channel.  Call CHAN, then read the pointer with OS_Args
 * 0. */
static void efr_rptr(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r5, r6, r7, r10 = s->r[10],
             r11 = s->r[11], r12, r13 = s->r[13];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    efr_rptr_chan(s);                           /* BL CHAN */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r0 = 0;                                     /* MOV R0,#0 */
    s->r[0] = r0;                               /* SWI OS_Args */
    ros_native_swi(s, ros_thunk_OS_Args);
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    r0 = r2;                                    /* MOV IACC,R2 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
    EFR_TAIL_CALL(efr_psinstk)                  /* B PSINSTK */
}

/* ---- EVAL (Factor.s:719-735) and VAL (Factor.s:1441-1465) ------------ */

/* EVAL: the operand is a string.  It is tokenised and evaluated on a
 * staging block below SP. */
static void efr_eval(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r5, r6, r7, r8 = s->r[8],
             r9, r10 = s->r[10], r11 = s->r[11], r12, r13 = s->r[13],
             r14 = s->r[14];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    efr_eval_factor(s);                         /* BL FACTOR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFC10398Cu;                     /* BL OSSTRI */
    basicvfp_hand_OSSTRI(s);
    ros_check_return(s, 0xFC10398Cu);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    ros_st32(r13 - 4, r11);                     /* STR AELINE,[SP,#-4]! */
    r13 -= 260;                                 /* SUB SP,SP,#256 */
    r4 = ros_ld32(r8 - 140) + 0x400u;           /* LDR R4,[ARGP,#FSA] ; +1024 */
    ros_subs(s, r4, r13);                       /* CMP R4,SP */
    if (r4 >= r13) {                            /* BCS ERDEEPNEST */
        s->r[4] = r4; s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_ERDEEPNEST)
    }
    s->r[13] = r13;
    efr_eval_evmatch(s);                        /* BL EVMATCH */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r13 = s->r[13]; r14 = s->r[14];
    r11 = r13;                                  /* MOV AELINE,SP */
    s->r[11] = r11;
    efr_eval_expr(s);                           /* BL EXPR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = r13;                                   /* MOV R4,SP */
    r14 = 0xFC1039B8u;                          /* BL PURGECACHE */
    basicvfp_hand_PURGECACHE(s, r4, &r5, &r6, &r7, r8, &r10, r11, r13,
                             &r14);
    ros_check_return(s, 0xFC1039B8u);
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    r13 += 0x100u;                              /* ADD SP,SP,#256 */
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
    s->r[10] = r10; s->r[14] = r14;             /* LDMFD SP!,{AELINE,PC} */
    s->r[11] = ros_ld32(r13);
    s->r[15] = ros_ld32(r13 + 4);
    s->r[13] = r13 + 8;
}

/* VAL: the operand is a string, read as a number by FREAD. */
static void efr_val(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r5, r6, r7, r9,
             r10 = s->r[10], r11 = s->r[11], r12, r13 = s->r[13];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    efr_val_factor(s);                          /* BL FACTOR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFC103FF0u;                     /* BL OSSTRI ; stop mark */
    basicvfp_hand_OSSTRI(s);
    ros_check_return(s, 0xFC103FF0u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    EFR_TAIL_CALL(basicvfp_hand_VAL0)           /* B VAL0 */
}

/* ---- VALSTR/VAL0 (Factor.s:1441-1465) -------------------------------- */

/* VALSTR: the string is in the accumulator.  Push the link, then let
 * VAL0 read the string. */
void basicvfp_hand_VALSTR(struct ros_cpu *s)
{
    uint32_t r13 = s->r[13], r14 = s->r[14];
    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[13] = r13;
    EFR_TAIL_CALL(basicvfp_hand_VAL0)           /* B VAL0 */
}

/* VAL0: read the number at AELINE (the operand's string, or STRACC
 * from VALSTR), with an optional sign consumed first. */
void basicvfp_hand_VAL0(struct ros_cpu *s)
{
    uint32_t r8 = s->r[8], r10, r11 = s->r[11], r13 = s->r[13];

    r13 -= 4;                                   /* STR AELINE,[SP,#-4]! */
    ros_st32(r13, r11);
    r11 = r8 - STRACC_OFF;                      /* ADD AELINE,ARGP,#STRACC */
    do {                                        /* VALA */
        ROS_POLL(s, r13);
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
    } while (r10 == 32);
    if (r10 == 45) {                            /* CMP R10,#"-" ; VALMIN */
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
        s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
        s->r[14] = 0xFC104028u;                 /* BL FREAD */
        basicvfp_FREAD(s);
        ros_check_return(s, 0xFC104028u);
        r13 = s->r[13];
        r11 = ros_ld32(r13);                    /* LDR AELINE,[SP],#4 */
        r13 += 4;
        s->r[11] = r11; s->r[13] = r13;
        EFR_TAIL_CALL(basicvfp_VALCMP)          /* B VALCMP */
    }
    if (r10 == 43) {                            /* CMP R10,#"+" ; VALPLU */
        r10 = ros_ld8(r11);                     /* LDRB R10,[AELINE],#1 */
        r11 += 1;
    }
    s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    s->r[14] = 0xFC10401Cu;                     /* BL FREAD */
    basicvfp_FREAD(s);
    ros_check_return(s, 0xFC10401Cu);
    r13 = s->r[13];                             /* LDMFD SP!,{AELINE,PC} */
    s->r[11] = ros_ld32(r13);
    s->r[15] = ros_ld32(r13 + 4);
    s->r[13] = r13 + 8;
}

/* ---- the TSTVB chain (Factor.s:447-471) ------------------------------ */

/* TSTVB1: not an l-value at all.  If the OPT_errors bit is set it is
 * FACERR.  If it is clear, the value is [ARGP,#ASSPC].  R13 and R14
 * come through R[], because the callers store them on the way in. */
static void efr_tstvb1(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld8(s->r[8] - 26);        /* LDRB R0,[ARGP,#BYTESM] */
    ros_logic(s, r0 & OPT_errors, s->c);        /* TST R0,#OPT_errors */
    if (!s->z) {                                /* BNE FACERR */
        s->r[0] = r0;
        EFR_TAIL_CALL(basicvfp_FACERR)
    }
    r0 = ros_ld32(s->r[8] - 192);               /* LDR R0,[ARGP,#ASSPC] */
    /* A forward reference, counted for the cross assemblers.  The
     * value is not yet the name's own.  This is patch-basicasm.py's
     * TSTVB1 hook.  It patched the lift's FACTOR, which the scaffold
     * drop deleted, so the hand body carries the hook now. */
    basicasm_unknown++;
    s->r[0] = r0;
    efr_sinstk(s);                              /* B SINSTK */
}

/* TSTVBCACHEARRAY: the cache held an array slot.  Re-enter
 * ARLOOKCACHE with the tag removed, then go to VARIND or TSTVB1 as
 * before. */
static void efr_tstvbcachearray(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r9, r10, r11, r12, r14,
             r13 = s->r[13];

    r11 = s->r[11] - TFP;                       /* SUB AELINE,AELINE,#TFP */
    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[11] = r11; s->r[13] = r13;
    efr_tstvba_arl(s);                          /* BL ARLOOKCACHE */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = ros_ld32(r13);                        /* LDR R14,[SP],#4 */
    r13 += 4;
    if (!s->z) {                                /* BNE VARIND */
        s->r[13] = r13; s->r[14] = r14;
        EFR_TAIL_CALL(basicvfp_hand_VARIND)
    }
    s->r[13] = r13; s->r[14] = r14;
    efr_tstvb1(s);                              /* B TSTVB1 */
}

/* TSTVBNOTCACHE: the entry for !, $ and |.  Walk the l-value, then do
 * the typed read, or go to TSTVB1. */
static void efr_tstvbnotcache(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r9, r10, r11, r12, r14,
             r13 = s->r[13];

    r13 -= 4;                                   /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[13] = r13;
    efr_tstvb_lv(s);                            /* BL LVNOTCACHE */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = ros_ld32(r13);                        /* LDR R14,[SP],#4 */
    r13 += 4;
    if (!s->z) {                                /* BNE VARIND */
        s->r[13] = r13; s->r[14] = r14;
        EFR_TAIL_CALL(basicvfp_hand_VARIND)
    }
    s->r[13] = r13; s->r[14] = r14;
    efr_tstvb1(s);
}

/* TSTVB: the entry for the letters.  Probe the cache, and on a miss
 * do the full walk. */
static void efr_tstvb(struct ros_cpu *s)
{
    uint32_t r0, r1, r4, r8 = s->r[8], r9, r11 = s->r[11];

    r1 = r8 + ((r11 & CACHEMASK) << CACHESHIFT); /* AND R1,AELINE,#CACHEMASK */
    r0 = ros_ld32(r1);                          /* LDMIA R1,{IACC,R1,R4,TYPE} */
    r4 = ros_ld32(r1 + 8);
    r9 = ros_ld32(r1 + 12);
    r1 = ros_ld32(r1 + 4);
    if (r4 != r11) {                            /* CMP R4,AELINE */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[9] = r9;
        s->r[10] = s->r[10]; s->r[11] = r11;
        ROS_POLL(s, s->r[13]);
        efr_tstvbnotcache(s);                   /* BNE TSTVBNOTCACHE */
        return;
    }
    r11 += r1;                                  /* ADD AELINE,AELINE,R1 */
    if ((int32_t)(r1 + 1) < 0) {                /* CMN R1,#1 ; BMI */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[9] = r9;
        s->r[11] = r11;
        ROS_POLL(s, s->r[13]);
        efr_tstvbcachearray(s);                 /* TSTVBCACHEARRAY */
        return;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[9] = r9;
    s->r[10] = s->r[10]; s->r[11] = r11;
    EFR_TAIL_CALL(basicvfp_hand_VARIND)
}

/* ---- FACTOR's dispatch trunk (Factor.s:25-31 and the table) ---------- */

/* The hand dispatcher.  Every character case that this row has landed
 * goes to its hand body.  unit-factor-resid translated the remaining
 * cases.  The default arm is FACERR, which is exactly the lift's own
 * arm for an error character.  The lift trunk itself is GONE.  Row 8's
 * scaffold drop deleted the whole of it, since nothing referred to it
 * any more. */
void basicvfp_hand_FACTOR(struct ros_cpu *s)
{
    uint32_t r4, r10, r11 = s->r[11], r13 = s->r[13];

FACTOR:
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    r4 = ros_ld32(FACTORFTAB + (r10 << 2));     /* LDR R4,[PC,R10,LSL #2] */
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;

    switch (r10) {                              /* ADD PC,PC,R4 */
    case 32: case 43:                           /* space, '+' */
        ROS_POLL(s, r13);
        goto FACTOR;
    case 33: case 36: case 124:                 /* ! $ | */
        efr_tstvbnotcache(s);
        return;
    case 34:                                    /* '"' */
        EFR_TAIL_CALL(basicvfp_hand_QSTR)
    case 37:                                    /* '%' */
        EFR_TAIL_CALL(basicvfp_hand_BININ)
    case 38:                                    /* '&' */
        EFR_TAIL_CALL(basicvfp_hand_HEXIN)
    case 40:                                    /* '(' */
        EFR_TAIL_CALL(basicvfp_hand_BRA)
    case 45:                                    /* '-' */
        EFR_TAIL_CALL(basicvfp_hand_UNMINS)
    case 46: case 48: case 49: case 50: case 51: case 52: case 53:
    case 54: case 55: case 56: case 57:         /* '.' '0'-'9' */
        EFR_TAIL_CALL(basicvfp_hand_TSTN)
    case 63: case 64: case 65: case 66: case 67: case 68: case 69:
    case 70: case 71: case 72: case 73: case 74: case 75: case 76:
    case 77: case 78: case 79: case 80: case 81: case 82: case 83:
    case 84: case 85: case 86: case 87: case 88: case 89: case 90:
    case 95: case 96: case 97: case 98: case 99: case 100: case 101:
    case 102: case 103: case 104: case 105: case 106: case 107:
    case 108: case 109: case 110: case 111: case 112: case 113:
    case 114: case 115: case 116: case 117: case 118: case 119:
    case 120: case 121: case 122:               /* ? @ A-Z _ ` a-z */
        efr_tstvb(s);
        return;
    case 143:                                   /* RPTR */
        efr_rptr(s);
        return;
    case 144: {                                 /* RPAGE */
        s->r[0] = ros_ld32(s->r[8] - 148);      /* LDR IACC,[ARGP,#PAGE] */
        efr_sinstk(s);
        return;
    }
    case 145: {                                 /* RTIME */
        uint32_t r1 = s->r[8] - STRACC_OFF;     /* ADD R1,ARGP,#STRACC */
        uint32_t rt = ros_ld8(r11);             /* LDRB R10,[AELINE] */
        ros_subs(s, rt, 36);                    /* CMP R10,#"$" */
        if (rt == 36)
            goto RTIMED;
        s->r[0] = OsWord_ReadSystemClock;
        s->r[1] = r1; s->r[10] = rt; s->r[11] = r11;
        ros_native_swi(s, ros_thunk_OS_Word);   /* SWI OS_Word */
        if (s->v) ros_swi_raise(s);
        s->r[0] = ros_ld32(r1);                 /* LDR IACC,[R1] */
        efr_sinstk(s);
        return;
    RTIMED:
        r11 += 1;                               /* ADD AELINE,AELINE,#1 */
        ros_st32(r1, 0);                        /* STR R0,[R1] ; long time */
        s->r[0] = OsWord_ReadRealTimeClock;
        s->r[1] = r1; s->r[11] = r11;
        ros_native_swi(s, ros_thunk_OS_Word);   /* SWI OS_Word */
        if (s->v) ros_swi_raise(s);
        {
            uint32_t r2 = r1 - 1;               /* SUB CLEN,R1,#1 */
            uint32_t r0;
            do {                                /* RTIMED1 */
                ROS_POLL(s, r13);
                r2 += 1;                        /* LDRB R0,[CLEN,#1]! */
                r0 = ros_ld8(r2);
                ros_subs(s, r0, 32);            /* CMP R0,#32 */
            } while (r0 >= 32);
            s->r[0] = r0; s->r[2] = r2;
            EFR_TAIL_CALL(basicvfp_hand_RNULX)
        }
    }
    case 146: {                                 /* RLOMEM */
        s->r[0] = ros_ld32(s->r[8] - 136);      /* LDR IACC,[ARGP,#LOMEM] */
        efr_sinstk(s);
        return;
    }
    case 147: {                                 /* RHIMEM */
        s->r[0] = ros_ld32(s->r[8] - 132);      /* LDR IACC,[ARGP,#HIMEM] */
        efr_sinstk(s);
        return;
    }
    case 148: EFR_TAIL_CALL(basicvfp_hand_ABS)   /* the landed built-ins */
    case 149: EFR_TAIL_CALL(basicvfp_hand_ACS)
    case 151: EFR_TAIL_CALL(basicvfp_hand_ASC)
    case 152: EFR_TAIL_CALL(basicvfp_hand_ASN)
    case 153: EFR_TAIL_CALL(basicvfp_hand_ATN)
    case 155: EFR_TAIL_CALL(basicvfp_hand_COS)
    case 157: EFR_TAIL_CALL(basicvfp_hand_DEG)
    case 161: EFR_TAIL_CALL(basicvfp_hand_EXP)
    case 167: EFR_TAIL_CALL(basicvfp_hand_INSTR)
    case 168: EFR_TAIL_CALL(basicvfp_hand_INT)
    case 169: EFR_TAIL_CALL(basicvfp_hand_LEN)
    case 170: EFR_TAIL_CALL(basicvfp_hand_LN)
    case 171: EFR_TAIL_CALL(basicvfp_hand_LOG)
    case 178: EFR_TAIL_CALL(basicvfp_hand_RAD)
    case 180: EFR_TAIL_CALL(basicvfp_hand_SGN)
    case 181: EFR_TAIL_CALL(basicvfp_hand_SIN)
    case 182: EFR_TAIL_CALL(basicvfp_hand_SQR)
    case 183: EFR_TAIL_CALL(basicvfp_hand_TAN)
    case 189: EFR_TAIL_CALL(basicvfp_hand_CHRD)
    case 192: EFR_TAIL_CALL(basicvfp_hand_LEFTD)
    case 193: EFR_TAIL_CALL(basicvfp_hand_MIDD)
    case 194: EFR_TAIL_CALL(basicvfp_hand_RIGHTD)
    case 196: EFR_TAIL_CALL(basicvfp_hand_STRND)
    case 160:                                   /* EVAL */
        efr_eval(s);
        return;
    case 187:                                   /* VAL */
        efr_val(s);
        return;
    case 131: EFR_TAIL_CALL(basicvfp_hand_MODULUS)  /* the residuals */
    case 142: EFR_TAIL_CALL(basicvfp_hand_OPENU)
    case 150: EFR_TAIL_CALL(basicvfp_hand_ADC)
    case 154: EFR_TAIL_CALL(basicvfp_hand_BBGET)
    case 156: EFR_TAIL_CALL(basicvfp_hand_COUNT)
    case 158: EFR_TAIL_CALL(basicvfp_hand_ERL)
    case 159: EFR_TAIL_CALL(basicvfp_hand_ERR)
    case 162: EFR_TAIL_CALL(basicvfp_hand_EXT)
    case 163: EFR_TAIL_CALL(basicvfp_hand_FALSE)
    case 164: EFR_TAIL_CALL(basicvfp_hand_FN)      /* FN (landed unit) */
    case 165: EFR_TAIL_CALL(basicvfp_hand_GET)
    case 166: EFR_TAIL_CALL(basicvfp_hand_INKEY)
    case 172: EFR_TAIL_CALL(basicvfp_hand_NOT)
    case 173: EFR_TAIL_CALL(basicvfp_hand_OPENI)
    case 174: EFR_TAIL_CALL(basicvfp_hand_OPENO)
    case 175: {                                 /* PI */
        uint32_t r9;
        s->fp->vfp.d[0] = 3.141592653589793;    /* FLDD FACC,FULLPI */
        r9 = ros_logic(s, TFP, 1);              /* MOVS TYPE,#TFP */
        s->r[9] = r9;
        s->r[15] = s->r[14];                    /* MOV PC,R14 */
        return;
    }
    case 176: EFR_TAIL_CALL(basicvfp_hand_POINTB)
    case 177: EFR_TAIL_CALL(basicvfp_hand_POS)
    case 179: EFR_TAIL_CALL(basicvfp_hand_RND)
    case 184: EFR_TAIL_CALL(basicvfp_hand_TO)
    case 185: EFR_TAIL_CALL(basicvfp_hand_TRUE)
    case 186: EFR_TAIL_CALL(basicvfp_hand_USR)
    case 188: EFR_TAIL_CALL(basicvfp_hand_VPOS)
    case 190: EFR_TAIL_CALL(basicvfp_hand_GETD)
    case 191: EFR_TAIL_CALL(basicvfp_hand_INKED)
    case 195: EFR_TAIL_CALL(basicvfp_hand_STRD)
    case 197: EFR_TAIL_CALL(basicvfp_hand_EOF)
    case 198: EFR_TAIL_CALL(basicvfp_hand_TWOFUNC)
    case 200: EFR_TAIL_CALL(basicvfp_hand_TWOFUNCA)
    case 222: EFR_TAIL_CALL(basicvfp_hand_DIMFN)
    case 224: EFR_TAIL_CALL(basicvfp_hand_GIVEEND)
    case 235: EFR_TAIL_CALL(basicvfp_hand_MODEFN)
    case 239: EFR_TAIL_CALL(basicvfp_hand_VDUFN)
    case 246: EFR_TAIL_CALL(basicvfp_hand_REPFN)
    case 252: EFR_TAIL_CALL(basicvfp_hand_TRACEFN)
    case 254: EFR_TAIL_CALL(basicvfp_hand_WIDTHFN)
    default:                                    /* the error characters */
        EFR_TAIL_CALL(basicvfp_FACERR)
    }
}
