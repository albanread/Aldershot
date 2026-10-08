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
 * (Sources/Programmer/BASIC: s.Array, s.Factor).
 */

/* factor_resid.c: FACTOR's remaining one-off built-ins, translated by
 * hand from Factor.s and Array.s (RISC OS 5.31's BASIC, the VFP
 * build).  This is the FINAL writer unit of the row-5 FACTOR swap.
 * Every case that the hand dispatcher still sent to the renamed lift
 * trunk is now hand code.  So the dispatcher's default arm becomes
 * FACERR, which is exactly the lift's own arm for every error
 * character.  The swap no longer reaches basicvfp_FACTOR_lift from any
 * path, and row 8's scaffold drop deletes the whole lift trunk.
 *
 * The bodies, with their source lines:
 *   TWOFUNC  Factor.s:293-301  the escape trunk for the function form
 *            (case 198).  A token below &8E, or at or above
 *            TTWOFUNCLIMIT (&90), is FACERR.  The two-word table at
 *            &FC1033CC gives SUM (Array.s:1543) and BEAT (Factor.s:392).
 *   TWOFUNCA Factor.s:303-321  the escape trunk for the statement form
 *            (case 200).  The table at &FC1033F0 covers
 *            TQUIT..TTWOSTMTLIMIT-1 and gives RQUIT, RSYS, RTINT,
 *            RBEATS and RTEMPO.  INSTALL, LIBRARY, ELLIPSE, VOICES,
 *            VOICE, STEREO and OVERLAY have no function form, and take
 *            the table's FACERR word.
 *   DIMFN    Factor.s:324-357  DIM(array) and DIM(array,n): the number
 *            of dimensions, or the limit of the n'th dimension minus
 *            one.
 *   REPFN    Factor.s:364-370  REP$: the text of the last error, copied
 *            from [ARGP,#ERRORS] to STRACC and NUL-terminated there.
 *            The CR is not included, since CLEN steps back over the
 *            NUL.
 *   WIDTHFN/ Factor.s:371-377  the state readers.  WIDTH gives the
 *   TRACEFN/                 stored value plus 1, since the stored
 *   MODEFN                   value is the maximum column minus one.
 *                            TRACE gives its file handle.  MODE gives
 *                            OS_Byte 135's R2.
 *   VDUFN    Factor.s:379-390  VDU(n): OS_ReadVduVariables with a
 *            three-word {n,-1,0} request block on the stack.
 *   BEAT/RBEATS/RTEMPO        the Sound_Q SWI family as functions.
 *            These are OS_SWI calls and not native thunks.  They need
 *            no OS support, and they can return errors.
 *   ADC      Factor.s:966-976  the analogue port: OS_Byte &80 with
 *            R1=channel.  The result is (R1 AND 255) OR R2<<8.  R2 is
 *            NOT masked ("removed since GStark claims its OK", :974).
 *   BBGET    Factor.s:994-998  BGET#chan, by OS_BGet after CHAN.
 *   COUNT/GIVEEND/ERL/ERR     state words in [ARGP].
 *   EXT      Factor.s:1166-    EXT#chan: OS_Args 2, through the SWI
 *            tail that it shares with RPTR.
 *   RSYS     Factor.s:756-837  SYS as a function, "(expr)".  It has
 *            three forms:
 *            - A string converts a SWI name to a number.  This uses
 *              OS_SWINumberFromString on the STRACC operand, which is
 *              zero-terminated in place at CLEN.
 *            - A positive number converts a SWI number to a string.
 *              This uses OS_SWINumberToString into STRACC.  The NUL is
 *              replaced by CR, and the string returns through LEFTX.
 *            - A negative number indexes the reason-code table
 *              SYS(-1)..SYS(-13): BASICID, module version, language
 *              version, flags, CPU, interrupt badge, FP registers,
 *              STRACC, FP format, ARGP and CALL2.  Past the end of the
 *              table it is ERRSUB.
 *   RQUIT    QUIT as a function.  It is TRUE when CALLEDNAME is set (a
 *            -quit program may not quit) and FALSE otherwise.
 *   GET      Factor.s:1059-    OS_ReadC.  No link is pushed.
 *   INKEY    Factor.s:1070-1085 OS_Byte &81 with R1=key and
 *            R2=key>>8.  If R2 comes back nonzero, the key was not
 *            pressed, and the result is TRUE (-1).  Otherwise the
 *            result is R1 AND 255.
 *   NOT      Factor.s:1178-1183 MVN IACC,IACC after INTEGZ.
 *   OPENU/   Factor.s:1195-1209 OS_Find &40/&C0/&80 on the filename
 *   OPENI/                   from OSSTRI.
 *   OPENO
 *   POINTB   Factor.s:1232-1251 POINT(x,y): OS_ReadPoint, returning R2
 *            (the colour).
 *   RTINT    Factor.s:1081-1100 the table's TINT twin: OS_ReadPoint,
 *            returning R3 (the tint).
 *   POS/VPOS Factor.s:1317-1327 OS_Byte &86's R1 or R2, AND 255.
 *   RND      Factor.s:1101-1153 the 33-bit generator.  SEED is at
 *            [ARGP,#SEED] and its 33rd bit is at [ARGP,#SEED+4]
 *            (DORANDOM's RRX chain).  The forms are:
 *            - RND(negative) stores the seed and sets the byte of the
 *              33rd bit to &40 (RNDSET).
 *            - Bare RND returns the new SEED as an INTEGER (SIMPLE ->
 *              PSINSTK).
 *            - RND(0) re-reads SEED and makes a float.
 *            - RND(1) makes a float through FRND1.
 *            - RND(n>=2) scales FRNDAA's mantissa float by n
 *              (FSITOD D7), truncates it (FTOSIZD) and adds one.
 *   TO       Factor.s:1424-1427 TOP.  'P' is consumed, or it is FACERR.
 *            Then it returns [ARGP,#TOP].
 *   USR      Factor.s:1429-1439 CALLARMROUT with TYPE=IACC and R5=0.
 *            The code's return value is in IACC.
 *   VPOS     (see POS)
 *   GETD     Factor.s:1471-1492 GET$[#chan].  With '#' it reads
 *            OS_BGet bytes into STRACC.  It stops at the 255th byte, at
 *            LF or at CR.  The terminator is NOT stored, and C set from
 *            OS_BGet is mapped to 10.  The bare form is OS_ReadC.  Both
 *            end in SINSTR, with one byte at STRACC and CLEN one past
 *            it.
 *   INKED    Factor.s:1494-1502 like INKEY$, using OS_Byte &81.  If R2
 *            is zero it makes the one-byte string of R1 AND 255.
 *            Otherwise it is RNUL.
 *   STRD     Factor.s:1602-1624 STR$[~].  Spaces are skipped.  '~'
 *            selects the hex form in R5, and AELINE is rewound when
 *            there is no '~'.  The operand is read by FACTOR.  The
 *            digit count in INTVAR (@% AND &FF00) is clamped to zero
 *            below &1000000.  FCONFP converts, and its length is
 *            carried in TYPE into CLEN.
 *   EOF      Factor.s:1646-1653 OS_Byte &7F's R1 AND 255, with nonzero
 *            made -1.
 *   SUM      Array.s:1543-1612  SUM(array): READARRAYFACTOR1, then by
 *            type:
 *            - Integers are summed with the NEON Q-register ladder when
 *              the flag is on and the count is >= 8.  Otherwise they
 *              use the word loop.
 *            - Floats use the D4-D15 ladder (SUMFP), with FPSCR's LEN
 *              bits set for the vector loads.
 *            - Strings are joined into STRACC.  Each element's length
 *              byte is at [TYPE,#4], the descriptor's pointer is loaded
 *              unaligned, and the total is checked against 256 for
 *              ERLONG.
 *            SUM LEN(array) is SUMLEN, which sums the length bytes
 *            without copying.
 *   MODULUS  Array.s:1615-1670 MOD(array): the sum of squares, typed
 *            in the same three ways, with the square root taken at the
 *            end (FSITOD/FSQRTD).  IOC from a negative square goes to
 *            FSQRTN ("Negative root") and not to VFPException.
 *
 * The setjmp ledger follows.  It is the clrstk enumeration.  Every BL
 * whose lift site carries a resume point keeps one at the lift's own
 * address, in a noinline helper (the endpr_gtargs rule).  These are the
 * 25 sites that row 8's residual note counts in the live bodies.  The
 * frames on the error paths never resume, so none is pushed there, as
 * in factor_core.
 *   DIMFN's BL LVBLNK             &FC103434
 *   DIMFN1's BL EXPR              &FC103480
 *   VDUFN's BL FACTOR             &FC103504
 *   ADC's BL FACTOR               &FC1038D8
 *   BBGET's BL CHAN               &FC103958
 *   EXT's BL CHAN                 &FC103A00
 *   RSYS's BL EXPR                &FC103A1C
 *   RTINT's BL EXPR / BL BRA      &FC103DD0 / &FC103DE4
 *   INKEY's BL FACTOR             &FC103B40
 *   NOT's BL FACTOR               &FC103D50
 *   OPENU's BL FACTOR             &FC103D78
 *   POINTB's BL EXPR / BL BRA     &FC103D90 / &FC103DA4
 *   RND's BL BRA                  &FC103E24
 *   USR's BL FACTOR / BL CALLARMROUT  &FC103FC4 / &FC103FD8
 *   GETDH's BL CHAN               &FC10405C
 *   INKED's BL FACTOR             &FC1040B8
 *   STRD's BL FACTOR / BL FCONFP  &FC104284 / &FC104298
 *   EOF's BL CHAN                 &FC104318
 *   SUM's BL READARRAYFACTOR1     &FC109340
 *   SUMLEN's BL READARRAYFACTOR   &FC1094A8
 *   MODULUS's BL READARRAYFACTOR  &FC1094D8
 * Plain checked calls (INTEGY, INTEGZ, AESPAC, OSSTRI, GETARRAYSIZE,
 * DORANDOM, FRNDAA) carry no frame in the lift and none here.  FCONFP
 * stays the LIFT's own body.  factor_const's hand FCONFP is held,
 * because adapting its FREAD to the (s) wrapper form of the box-build
 * lift is a separate landing.
 *
 * These quirks are kept.  Each is given with its line:
 *   - RND has three forms (Factor.s:1101-1153).  Bare RND is the raw
 *     SEED word as an INTEGER (SIMPLE).  RND(0) re-reads SEED for a
 *     fresh float (the LDREQ at :1112).  The BEQ FRND at :1113 tests
 *     the flags that TEQ IACC,#0 set BEFORE the load.  So it tests v16,
 *     the original argument, and not the seed.
 *   - RNDSET writes the seed, and writes &40 into the byte of the 33rd
 *     bit (Factor.s:1149-1152).  The state is two words.
 *   - ADC does not mask R2 before the ORR.  Factor.s:974's own comment
 *     says "removed since GStark claims its OK".  So the high bits of
 *     the R2 that OS_Byte returns go into bits 8 and up.
 *   - INKEY maps "key not pressed" to TRUE (-1) through the ANDS
 *     R2,R2,#255 (Factor.s:1083-1084).  INKED maps it to the empty
 *     string (RNUL) in the same way (Factor.s:1500-1502).
 *   - RSYS's name form zero-terminates the operand IN PLACE at CLEN
 *     (STRB R0,[CLEN], Factor.s:781).  The number form replaces the NUL
 *     that OS_SWINumberToString left with CR (STRB R14,[CLEN,#-1]! at
 *     :777), so the string is then terminated the BASIC way.
 *   - The reason-code table's dispatch is conditional on the SAME carry
 *     as its LDRLO (Factor.s:784-789).  A code past the table falls
 *     straight to ERRSUB.  The table word -369086110 is ERRSUB's own
 *     entry.
 *   - DIMFN's zero-dimension form counts the dimensions by walking the
 *     limit list from -1 (Factor.s:341-343).  DIMFN2's limit read takes
 *     the word at the OLD R2 (LDR R1,[R2],#4, :352).
 *   - WIDTH returns the stored byte PLUS ONE (Factor.s:372).
 *   - TO's 'P' is consumed and not pushed back (Factor.s:1424-1425).
 *     So a TO not followed by P gives an error with the P already
 *     used up.
 *   - GETDH's exit at the 255th byte and its exit at a terminator both
 *     go through LEFTX, with the terminator not stored
 *     (Factor.s:1474-1484).  The carry from OS_BGet is mapped to 10
 *     first (:1478).
 *   - STRD rewinds AELINE only when '~' was NOT seen (SUBNE,
 *     Factor.s:1606).  It clamps INTVAR's digit count to zero below
 *     &1000000 (MOVCC R4,#0, :1612).
 *   - SUM's integer ladder only runs with the NEON flag set and a count
 *     of at least 8 (Array.s:1549-1556).  In the string form, the
 *     ERLONG check is on the RUNNING total plus this element
 *     (Array.s:1589).
 *   - MODULUS's exception path tells IOC apart from the other flags by
 *     TST R14,#FPSCR_IOC (the VFPException_SQRT join, from the tail of
 *     SQR in Factor.s).  IOC goes to FSQRTN, the "Negative root" of a
 *     square sum that has wrapped.  The rest go to VFPException.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include <setjmp.h>

#include "factor_resid.h"

/* Landed hand callees. */
#include "basic_conv.h"           /* INTEGY INTEGZ */
#include "lexical_scan.h"         /* AESPAC OSSTRI */
#include "expr_factor_rest.h"     /* BRA CHAN FACTOR RNUL RNULX */
#include "basic_organs.h"         /* MSG */
#include "funct_call.h"           /* FN (case 164's target) */

/* The lift's functions that this unit calls.  The manifest's EXPORTS
 * make them non-static in the twin. */
void basicvfp_FACERR(struct ros_cpu *s);
void basicvfp_ERBRA(struct ros_cpu *s);
void basicvfp_ERCOMM(struct ros_cpu *s);
void basicvfp_ERRSUB(struct ros_cpu *s);
void basicvfp_ERARRY(struct ros_cpu *s);
void basicvfp_ERARRZ(struct ros_cpu *s);
void basicvfp_ERRSB2(struct ros_cpu *s);
void basicvfp_ERLONG(struct ros_cpu *s);
void basicvfp_ERTYPESTRINGARRAY(struct ros_cpu *s);
void basicvfp_VFPException(struct ros_cpu *s);
void basicvfp_LVBLNK(struct ros_cpu *s);
void basicvfp_FCONFP(struct ros_cpu *s);
void basicvfp_CALLARMROUT(struct ros_cpu *s);
void basicvfp_READARRAYFACTOR(struct ros_cpu *s);
void basicvfp_READARRAYFACTOR1(struct ros_cpu *s);
void basicvfp_GETARRAYSIZE(struct ros_cpu *s);
void basicvfp_DORANDOM(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                       uint32_t *p2, uint32_t *p3, uint32_t r8,
                       uint32_t r14);
void basicvfp_FRNDAA(struct ros_cpu *s);

/* The native SWIs that the bodies call. */
void ros_thunk_OS_Byte(struct ros_cpu *s);
void ros_thunk_OS_ReadC(struct ros_cpu *s);
void ros_thunk_OS_BGet(struct ros_cpu *s);
void ros_thunk_OS_Find(struct ros_cpu *s);
void ros_thunk_OS_ReadPoint(struct ros_cpu *s);
void ros_thunk_OS_ReadVduVariables(struct ros_cpu *s);
void ros_thunk_OS_Args(struct ros_cpu *s);
void ros_thunk_OS_SWINumberToString(struct ros_cpu *s);
void ros_thunk_OS_SWINumberFromString(struct ros_cpu *s);

/* The lifter's tail-call and poll, as the tree's cpu.h spells them.
 * The poll is a no-op where cpu.h does not define it, because the
 * harness lift carries no polls. */
#if defined(__clang__)
# define FRD_TAIL_CALL(f) __attribute__((musttail)) return f(s);
#elif defined(__GNUC__) && __GNUC__ >= 15
# define FRD_TAIL_CALL(f) return __attribute__((musttail)) f(s);
# else
# define FRD_TAIL_CALL(f) return f(s);
#endif
#ifndef ROS_POLL
# define ROS_POLL(s, sp) do { } while (0)
#endif

/* Type bits, tokens and addresses.  These are the lift's own
 * spellings, and they are identical redefinitions in the twin. */
#define TFP         0x80000000u
#define TINTEGER    0x40000000u
#define STRACC_OFF  1536u
#define TFPLV       8u
#define FPOINT      2u
#define TLEN        169u
#define TQUIT       152u
#define TTWOFUNCLIMIT 144u
#define TTWOSTMTLIMIT 164u
#define VFPFLAG_Vectors 1u
#define VFPFLAG_NEON    2u
#define FPSCR_IOC   1u
#define BASICVFP_SR0   0xFC103A80u
#define BASICVFP_CALL2 0xFC107EB8u
#define TWOFUNC_TAB  0xFC1033CCu
#define TWOFUNCA_TAB 0xFC1033F0u

/* ---- the BL call sites, at the lift's own addresses ------------- */

#define SITE_DIMFN_LVBLNK    0xFC103434u
#define SITE_DIMFN1_EXPR     0xFC103480u
#define SITE_VDUFN_FACTOR    0xFC103504u
#define SITE_ADC_FACTOR      0xFC1038D8u
#define SITE_BBGET_CHAN      0xFC103958u
#define SITE_EXT_CHAN        0xFC103A00u
#define SITE_RSYS_EXPR       0xFC103A1Cu
#define SITE_RTINT_EXPR      0xFC103DD0u
#define SITE_RTINT_BRA       0xFC103DE4u
#define SITE_INKEY_FACTOR    0xFC103B40u
#define SITE_NOT_FACTOR      0xFC103D50u
#define SITE_OPENU_FACTOR    0xFC103D78u
#define SITE_POINTB_EXPR     0xFC103D90u
#define SITE_POINTB_BRA      0xFC103DA4u
#define SITE_RND_BRA         0xFC103E24u
#define SITE_USR_FACTOR      0xFC103FC4u
#define SITE_USR_CALLARMROUT 0xFC103FD8u
#define SITE_GETDH_CHAN      0xFC10405Cu
#define SITE_INKED_FACTOR    0xFC1040B8u
#define SITE_STRD_FACTOR     0xFC104284u
#define SITE_STRD_FCONFP     0xFC104298u
#define SITE_EOF_CHAN        0xFC104318u
#define SITE_SUM_RAF1        0xFC109340u
#define SITE_SUMLEN_RAF      0xFC1094A8u
#define SITE_MODULUS_RAF     0xFC1094D8u

/* The real addresses of the MSG error sites.  MSG reads its error
 * number and token from the words after these. */
#define SITE_ERTYPENUMARRAY 0xFC10FF24u
#define SITE_ERDIMFN        0xFC10FF84u
#define SITE_ERARRW         0xFC10FFF4u
#define SITE_ERARRYDIM      0xFC110004u
#define SITE_FSQRTN         0xFC11004Cu
#define SITE_ERBRA1         0xFC110074u

/* The error exit.  Hand the block to MSG, which never returns.  The
 * fault catches a MSG that does return.  The lift's frames around
 * these BLs never resume, because the error path does not come back.
 * So no frame is pushed, as in factor_core. */
static void frd_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* The built-ins' BL, with one noinline helper per site.  PERFORMANCE
 * PHASE (P1): the setjmp'd resume frame that this used to push at each
 * BL is GONE.  The FN return was the only resumer that ever targeted
 * these sites, and it now rides the C stack (FNRET/GTARGS, and see
 * expr_factor_rest.c's EFR_RESUME for the full argument).  These are
 * the paths that read the built-ins' arguments.  They run far less
 * often than the evaluator's own BLs.  But the protocol is one
 * contract, so they changed along with it. */
#define FRD_RESUME(name, site, call)                                   \
    __attribute__((noinline)) static void name(struct ros_cpu *s)      \
    {                                                                  \
        s->r[14] = (site);                                             \
        call;                                                          \
        ros_check_return(s, (site));                                   \
    }

FRD_RESUME(frd_dimfn_lvblnk, SITE_DIMFN_LVBLNK, basicvfp_LVBLNK(s))
FRD_RESUME(frd_dimfn1_expr, SITE_DIMFN1_EXPR, basicvfp_hand_EXPR(s))
FRD_RESUME(frd_vdufn_factor, SITE_VDUFN_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_adc_factor, SITE_ADC_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_bbget_chan, SITE_BBGET_CHAN, basicvfp_hand_CHAN(s))
FRD_RESUME(frd_ext_chan, SITE_EXT_CHAN, basicvfp_hand_CHAN(s))
FRD_RESUME(frd_rsys_expr, SITE_RSYS_EXPR, basicvfp_hand_EXPR(s))
FRD_RESUME(frd_rtint_expr, SITE_RTINT_EXPR, basicvfp_hand_EXPR(s))
FRD_RESUME(frd_rtint_bra, SITE_RTINT_BRA, basicvfp_hand_BRA(s))
FRD_RESUME(frd_inkey_factor, SITE_INKEY_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_not_factor, SITE_NOT_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_openu_factor, SITE_OPENU_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_pointb_expr, SITE_POINTB_EXPR, basicvfp_hand_EXPR(s))
FRD_RESUME(frd_pointb_bra, SITE_POINTB_BRA, basicvfp_hand_BRA(s))
FRD_RESUME(frd_rnd_bra, SITE_RND_BRA, basicvfp_hand_BRA(s))
FRD_RESUME(frd_usr_factor, SITE_USR_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_usr_callarmrout, SITE_USR_CALLARMROUT,
           basicvfp_CALLARMROUT(s))
FRD_RESUME(frd_getdh_chan, SITE_GETDH_CHAN, basicvfp_hand_CHAN(s))
FRD_RESUME(frd_inked_factor, SITE_INKED_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_strd_factor, SITE_STRD_FACTOR, basicvfp_hand_FACTOR(s))
FRD_RESUME(frd_strd_fconfp, SITE_STRD_FCONFP, basicvfp_FCONFP(s))
FRD_RESUME(frd_eof_chan, SITE_EOF_CHAN, basicvfp_hand_CHAN(s))
FRD_RESUME(frd_sum_raf1, SITE_SUM_RAF1, basicvfp_READARRAYFACTOR1(s))
FRD_RESUME(frd_sumlen_raf, SITE_SUMLEN_RAF, basicvfp_READARRAYFACTOR(s))
FRD_RESUME(frd_modulus_raf, SITE_MODULUS_RAF, basicvfp_READARRAYFACTOR(s))

/* ---- the exit joins (Factor.s:860-861, 977-978, 1186, 1490, 1524) --
 * These take one argument, like everything the trunk tail-calls.  The
 * callers store their locals into R[] first, and the join does the
 * rest. */

/* SINSTK: MOVS TYPE,#TINTEGER; MOV PC,R14. */
static void frd_sinstk(struct ros_cpu *s)
{
    uint32_t r9 = ros_logic(s, TINTEGER, 0);
    s->r[9] = r9;
    s->r[15] = s->r[14];
}

/* PSINSTK: the same, but with the link popped from the one arena word
 * that the body pushed. */
static void frd_psinstk(struct ros_cpu *s)
{
    uint32_t r9 = ros_logic(s, TINTEGER, 0);
    uint32_t r14 = ros_ld32(s->r[13]);
    s->r[9] = r9;
    s->r[14] = r14;
    s->r[15] = r14;
    s->r[13] += 4;
}

/* FSINSTK: MOVS TYPE,#TFP; LDR PC,[SP],#4. */
static void frd_fsinstk(struct ros_cpu *s)
{
    uint32_t r9 = ros_logic(s, TFP, 1);
    uint32_t r14 = ros_ld32(s->r[13]);
    s->r[9] = r9;
    s->r[14] = r14;
    s->r[15] = r14;
    s->r[13] += 4;
}

/* LEFTX (Factor.s:1524-1527): the exit for a string in STRACC.  The
 * link is popped from the one word that the body pushed.  CLEN is in
 * R[2]. */
static void frd_leftx(struct ros_cpu *s)
{
    uint32_t r9 = ros_logic(s, 0, s->c);
    uint32_t r14 = ros_ld32(s->r[13]);
    s->r[9] = r9;
    s->r[14] = r14;
    s->r[15] = r14;
    s->r[13] += 4;
}

/* SINSTR (Factor.s:1490-1492): store one character at STRACC, set CLEN
 * one past it and TYPE=0, and return to R14.  No arena word is
 * touched. */
static void frd_sinstr(struct ros_cpu *s, uint32_t r0, uint32_t r8)
{
    uint32_t r9;
    ros_st8(r8 - STRACC_OFF, r0);                 /* STRB IACC,[CLEN],#1 */
    s->r[2] = r8 - STRACC_OFF + 1;
    r9 = ros_logic(s, 0, s->c);                   /* MOVS TYPE,#0 */
    s->r[0] = r0;
    s->r[9] = r9;
    s->r[15] = s->r[14];
}

/* ---- the two escape trunks (Factor.s:293-321) ---------------------- */

void basicvfp_hand_TWOFUNC(struct ros_cpu *s)
{
    uint32_t r4, r10, r11 = s->r[11];

    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, TTWOFUNCLIMIT);              /* CMP R10,#TTWOFUNCLIMIT */
    if (r10 >= TTWOFUNCLIMIT) {
        s->r[10] = r10; s->r[11] = r11;
        FRD_TAIL_CALL(basicvfp_FACERR)            /* BCS FACERR */
    }
    r4 = ros_subs(s, r10, 142);                   /* SUBS R4,R10,#&8E */
    if (r10 < 142) {
        s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
        FRD_TAIL_CALL(basicvfp_FACERR)            /* BCC FACERR */
    }
    r4 = ros_ld32(TWOFUNC_TAB + (r4 << 2));       /* LDR R4,[PC,R4,LSL #2] */
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
    switch (r4) {                                 /* ADD PC,PC,R4 */
    case 24412: FRD_TAIL_CALL(basicvfp_hand_SUM)
    case 352:   FRD_TAIL_CALL(basicvfp_hand_BEAT)
    default: ros_fault(s, 0xFC1033C8u, "a jump table index out of range");
    }
}

void basicvfp_hand_TWOFUNCA(struct ros_cpu *s)
{
    uint32_t r4, r10, r11 = s->r[11];

    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, TTWOSTMTLIMIT);              /* CMP R10,#TTWOSTMTLIMIT */
    if (r10 >= TTWOSTMTLIMIT) {
        s->r[10] = r10; s->r[11] = r11;
        FRD_TAIL_CALL(basicvfp_FACERR)            /* BCS FACERR */
    }
    r4 = ros_subs(s, r10, TQUIT);                 /* SUBS R4,R10,#TQUIT */
    if (r10 < TQUIT) {
        s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
        FRD_TAIL_CALL(basicvfp_FACERR)            /* BCC FACERR */
    }
    r4 = ros_ld32(TWOFUNCA_TAB + (r4 << 2));      /* LDR R4,[PC,R4,LSL #2] */
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
    switch (r4) {                                 /* ADD PC,PC,R4 */
    case 1828:  FRD_TAIL_CALL(basicvfp_hand_RQUIT)
    case 1556:  FRD_TAIL_CALL(basicvfp_hand_RSYS)
    case 52324: FRD_TAIL_CALL(basicvfp_FACERR)
    case 2504:  FRD_TAIL_CALL(basicvfp_hand_RTINT)
    case 328:   FRD_TAIL_CALL(basicvfp_hand_RBEATS)
    case 340:   FRD_TAIL_CALL(basicvfp_hand_RTEMPO)
    default: ros_fault(s, 0xFC1033ECu, "a jump table index out of range");
    }
}

/* ---- DIMFN (Factor.s:324-357) -------------------------------------- */

void basicvfp_hand_DIMFN(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r9 = s->r[9], r10, r11 = s->r[11],
             r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, 40);                         /* CMP R10,#"(" */
    if (r10 != 40) {                              /* BNE ERARRW */
        s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
        frd_msg_at(s, SITE_ERARRW);
    }
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_dimfn_lvblnk(s);                          /* BL LVBLNK */
    r0 = s->r[0]; r2 = s->r[2]; r4 = s->r[4]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    if (s->z) {                                   /* BEQ ERARRYDIM */
        s->r[13] = r13;
        frd_msg_at(s, SITE_ERARRYDIM);
    }
    ros_subs(s, r9, 0x100u);                      /* CMP TYPE,#256 */
    if (r9 < 0x100u) {                            /* BCC ERDIMFN */
        s->r[13] = r13;
        frd_msg_at(s, SITE_ERDIMFN);
    }
    r2 = ros_ld32(r0);                            /* LDR R2,[IACC] */
    ros_subs(s, r2, 16);                          /* CMP R2,#16 */
    if (r2 < 16) {
        s->r[2] = r2; s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERARRZ)            /* BCC ERARRZ */
    }
    s->r[14] = 0xFC103450u;                       /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    ros_check_return(s, 0xFC103450u);
    r10 = s->r[10]; r11 = s->r[11]; r14 = s->r[14];
    ros_subs(s, r10, 44);                         /* CMP R10,#"," */
    if (r10 == 44)
        goto DIMFN1;                              /* BEQ DIMFN1 */
    ros_subs(s, r10, 41);                         /* CMP R10,#")" */
    if (r10 != 41) {
        s->r[2] = r2; s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERBRA)             /* BNE ERBRA */
    }
    r0 = 0xFFFFFFFFu;                             /* MVN IACC,#0 */
    do {                                          /* DIMFN0 */
        ROS_POLL(s, r13);
        r0 += 1;                                  /* ADD IACC,IACC,#1 */
        r1 = ros_ld32(r2);                        /* LDR R1,[R2],#4 */
        r2 += 4;
    } while (r1 != 0);                            /* BNE DIMFN0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */

DIMFN1:
    r13 -= 4;                                     /* STR R2,[SP,#-4]! */
    ros_st32(r13, r2);
    s->r[2] = r2; s->r[13] = r13;
    frd_dimfn1_expr(s);                           /* BL EXPR */
    r0 = s->r[0]; r2 = s->r[2]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13];
    ros_subs(s, r10, 41);                         /* CMP R10,#")" */
    if (r10 != 41) {
        s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERBRA)             /* BNE ERBRA */
    }
    s->r[14] = 0xFC10348Cu;                       /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    ros_check_return(s, 0xFC10348Cu);
    r0 = s->r[0]; r13 = s->r[13];
    r2 = ros_ld32(r13);                           /* LDR R2,[SP],#4 */
    r13 += 4;
    s->r[13] = r13;
    do {                                          /* DIMFN2 */
        ROS_POLL(s, r13);
        r1 = ros_ld32(r2);                        /* LDR R1,[R2],#4 */
        r2 += 4;
        if (r1 == 0) {                            /* TEQ R1,#0 ; BEQ ERRSB2 */
            s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
            FRD_TAIL_CALL(basicvfp_ERRSB2)
        }
        r0 = ros_subs(s, r0, 1);                  /* SUBS IACC,IACC,#1 */
    } while (r0 != 0);                            /* BNE DIMFN2 */
    r0 = r1 - 1;                                  /* SUB IACC,R1,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- REPFN (Factor.s:364-370) --------------------------------------- */

void basicvfp_hand_REPFN(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r10, r11 = s->r[11], r8 = s->r[8], r13 = s->r[13];

    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_logic(s, r10 ^ 0x24u, s->c);              /* TEQ R10,#"$" */
    if (!s->z) {
        s->r[10] = r10; s->r[11] = r11;
        FRD_TAIL_CALL(basicvfp_FACERR)            /* BNE FACERR */
    }
    r2 = r8 - STRACC_OFF;                         /* ADD CLEN,ARGP,#STRACC */
    r1 = r8 - 1792;                               /* ADD R1,ARGP,#ERRORS */
    do {                                          /* REPTFN */
        ROS_POLL(s, r13);
        r0 = ros_ld8(r1);                         /* LDRB R0,[R1],#1 */
        r1 += 1;
        ros_st8(r2, r0);                          /* STRB R0,[CLEN],#1 */
        r2 += 1;
    } while (r0 != 0);                            /* BNE REPTFN */
    r2 -= 1;                                      /* SUB CLEN,CLEN,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    s->r[10] = r10; s->r[11] = r11;
    FRD_TAIL_CALL(basicvfp_hand_RNULX)            /* B RNULX */
}

/* ---- the state readers (Factor.s:371-390) --------------------------- */

void basicvfp_hand_WIDTHFN(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld32(s->r[8] - 260) + 1;    /* LDR IACC,[ARGP,#WIDTHLOC] */
    s->r[0] = r0;                                 /* ADD IACC,IACC,#1 */
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_TRACEFN(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld32(s->r[8] - 104);        /* LDR IACC,[ARGP,#TRACEFILE] */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_MODEFN(struct ros_cpu *s)
{
    uint32_t r0, r1, r2;

    r0 = 135;                                     /* MOV R0,#&87 */
    s->r[0] = r0; s->r[10] = s->r[10]; s->r[11] = s->r[11];
    ros_native_swi(s, ros_thunk_OS_Byte);         /* SWI OS_Byte */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    r0 = r2;                                      /* MOV IACC,R2 */
    s->r[0] = r0; s->r[2] = r2;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_VDUFN(struct ros_cpu *s)
{
    uint32_t r0, r1, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13], r14;

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_vdufn_factor(s);                          /* BL FACTOR */
    r0 = s->r[0]; r4 = s->r[4]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    s->r[14] = 0xFC103508u;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC103508u);
    r0 = s->r[0]; r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r14 = ros_ld32(r13);                          /* LDR R14,[SP],#4 */
    /* The lift models the pop without the increment, and the 3-word
     * push as -8 with three stores.  The two missing 4s cancel, so the
     * request block and the final SP end up the same. */
    r13 -= 8;                                     /* STMFD SP!,{R0-R2} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, 0xFFFFFFFFu);               /* MOV R1,#-1 */
    ros_st32(r13 + 8, 0);                         /* MOV R2,#0 */
    r0 = r13;                                     /* MOV R0,SP */
    r1 = r13 + 8;                                 /* ADD R1,SP,#8 */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_ReadVduVariables); /* SWI OS_ReadVduVariables */
    if (s->v) ros_swi_raise(s);
    r0 = ros_ld32(r1);                            /* LDR IACC,[R1] */
    r13 += 12;                                    /* ADD SP,SP,#12 */
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13; s->r[14] = r14;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

/* ---- the Sound_Q family as functions (Factor.s:392-402) ------------ */

void basicvfp_hand_BEAT(struct ros_cpu *s)
{
    uint32_t r0 = 0;                              /* MOV R0,#0 */
    s->r[0] = r0;
    ros_swi(s, 0x401C6u);                         /* SWI Sound_QBeat */
    r0 = s->r[0];
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_RBEATS(struct ros_cpu *s)
{
    uint32_t r0 = 0xFFFFFFFFu;                    /* MVN R0,#0 */
    s->r[0] = r0;
    ros_swi(s, 0x401C6u);                         /* SWI Sound_QBeat */
    r0 = s->r[0];
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_RTEMPO(struct ros_cpu *s)
{
    uint32_t r0 = 0;                              /* MOV R0,#0 */
    s->r[0] = r0;
    ros_swi(s, 0x401C5u);                         /* SWI Sound_QTempo */
    r0 = s->r[0];
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

/* ---- ADC (Factor.s:966-976) ----------------------------------------- */

void basicvfp_hand_ADC(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_adc_factor(s);                            /* BL FACTOR */
    r0 = s->r[0]; r4 = s->r[4]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    s->r[14] = 0xFC1038DCu;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC1038DCu);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4]; r13 = s->r[13];
    r1 = r0;                                      /* MOV R1,R0 */
    r2 = r0 >> 8;                                 /* MOV R2,R0,LSR #8 */
    r0 = 128;                                     /* MOV R0,#&80 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    ros_native_swi(s, ros_thunk_OS_Byte);         /* SWI OS_Byte */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    r0 = (r1 & 0xFFu) | (r2 << 8);                /* AND IACC,R1,#255 ; ORR */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- TRUE/FALSE, GET, INKEY (Factor.s:1051-1085) -------------------- */

void basicvfp_hand_TRUE(struct ros_cpu *s)
{
    uint32_t r0 = 0xFFFFFFFFu;                    /* MVN IACC,#0 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_FALSE(struct ros_cpu *s)
{
    uint32_t r0 = 0;                              /* MOV IACC,#0 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_GET(struct ros_cpu *s)
{
    uint32_t r0;
    ros_native_swi(s, ros_thunk_OS_ReadC);        /* SWI OS_ReadC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[0] = r0;
    ROS_POLL(s, s->r[13]);
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_INKEY(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_inkey_factor(s);                          /* BL FACTOR */
    r0 = s->r[0]; r4 = s->r[4]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    s->r[14] = 0xFC103B44u;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC103B44u);
    r0 = s->r[0]; r4 = s->r[4]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    r1 = r0;                                      /* MOV R1,IACC */
    r2 = r0 >> 8;                                 /* MOV R2,IACC,LSR #8 */
    r0 = 129;                                     /* MOV R0,#&81 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    ros_native_swi(s, ros_thunk_OS_Byte);         /* SWI OS_Byte */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    {
        uint32_t r14 = ros_ld32(r13);             /* LDR R14,[SP],#4 */
        r13 += 4;
        s->r[13] = r13; s->r[14] = r14;
    }
    r2 &= 0xFFu;                                  /* ANDS R2,R2,#255 */
    if (r2 != 0) {                                /* BNE TRUE */
        s->r[2] = r2;
        ROS_POLL(s, r13);
        FRD_TAIL_CALL(basicvfp_hand_TRUE)
    }
    r0 = r1 & 0xFFu;                              /* AND IACC,R1,#255 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

/* ---- NOT (Factor.s:1178-1183) --------------------------------------- */

void basicvfp_hand_NOT(struct ros_cpu *s)
{
    uint32_t r0, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_not_factor(s);                            /* BL FACTOR */
    r0 = s->r[0]; r4 = s->r[4]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    s->r[14] = 0xFC103D54u;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC103D54u);
    r0 = s->r[0]; r4 = s->r[4]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    r0 = ~r0;                                     /* MVN IACC,IACC */
    s->r[0] = r0;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- OPENIN/OPENOUT/OPENUP (Factor.s:1195-1209) --------------------- */

/* The shared body: R0 carries the OS_Find reason. */
static void frd_open(struct ros_cpu *s, uint32_t reason)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r8 = s->r[8], r10 = s->r[10],
             r11 = s->r[11], r13 = s->r[13];

    r0 = reason;
    r13 -= 8;                                     /* STMFD SP!,{R0,R14} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, s->r[14]);
    s->r[0] = r0; s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    frd_openu_factor(s);                          /* BL FACTOR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4]; r8 = s->r[8];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    s->r[14] = 0xFC103D7Cu;                       /* BL OSSTRI */
    basicvfp_hand_OSSTRI(s);
    ros_check_return(s, 0xFC103D7Cu);
    r1 = s->r[1]; r2 = s->r[2]; r13 = s->r[13];
    r0 = ros_ld32(r13);                           /* LDMFD SP!,{R0,R14} */
    {
        uint32_t r14 = ros_ld32(r13 + 4);
        r13 += 8;
        s->r[14] = r14;
    }
    s->r[0] = r0; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_Find);         /* SWI OS_Find */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[0] = r0;
    ROS_POLL(s, r13);
    frd_sinstk(s);                                /* B SINSTK */
}

void basicvfp_hand_OPENU(struct ros_cpu *s)
{
    frd_open(s, 64);                              /* MOV R0,#&40 */
}

void basicvfp_hand_OPENI(struct ros_cpu *s)
{
    frd_open(s, 192);                             /* MOV R0,#&C0 */
}

void basicvfp_hand_OPENO(struct ros_cpu *s)
{
    frd_open(s, 128);                             /* MOV R0,#&80 */
}

/* ---- POINT(x,y) and TINT's twin (Factor.s:1232-1251, 1081-1100) ----- */

void basicvfp_hand_POINTB(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_pointb_expr(s);                           /* BL EXPR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    s->r[14] = 0xFC103D94u;                       /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    ros_check_return(s, 0xFC103D94u);
    r0 = s->r[0]; r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    ros_subs(s, r10, 44);                         /* CMP R10,#"," */
    if (r10 != 44) {
        s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERCOMM)            /* BNE ERCOMM */
    }
    r13 -= 4;                                     /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    frd_pointb_bra(s);                            /* BL BRA */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    s->r[14] = 0xFC103DA8u;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC103DA8u);
    r0 = s->r[0]; r13 = s->r[13];
    r1 = r0;                                      /* MOV R1,R0 */
    r0 = ros_ld32(r13);                           /* LDMFD SP!,{R0,R14} */
    {
        uint32_t r14 = ros_ld32(r13 + 4);
        r13 += 8;
        s->r[14] = r14;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_ReadPoint);    /* SWI OS_ReadPoint */
    if (s->v) ros_swi_raise(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r0 = r2;                                      /* MOV R0,R2 */
    s->r[0] = r0; s->r[2] = r2; s->r[3] = r3;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_RTINT(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r10, r11 = s->r[11],
             r13 = s->r[13];

    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, 40);                         /* CMP R10,#"(" */
    if (r10 != 40) {                              /* BNE ERBRA1 */
        s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
        frd_msg_at(s, SITE_ERBRA1);
    }
    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_rtint_expr(s);                            /* BL EXPR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    s->r[14] = 0xFC103DD4u;                       /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    ros_check_return(s, 0xFC103DD4u);
    r0 = s->r[0]; r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    ros_subs(s, r10, 44);                         /* CMP R10,#"," */
    if (r10 != 44) {
        s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERCOMM)            /* BNE ERCOMM */
    }
    r13 -= 4;                                     /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;
    frd_rtint_bra(s);                             /* BL BRA */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    s->r[14] = 0xFC103DE8u;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC103DE8u);
    r0 = s->r[0]; r13 = s->r[13];
    r1 = r0;                                      /* MOV R1,R0 */
    r0 = ros_ld32(r13);                           /* LDMFD SP!,{R0,R14} */
    {
        uint32_t r14 = ros_ld32(r13 + 4);
        r13 += 8;
        s->r[14] = r14;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_ReadPoint);    /* SWI OS_ReadPoint */
    if (s->v) ros_swi_raise(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r0 = r3;                                      /* MOV R0,R3 */
    s->r[0] = r0; s->r[2] = r2; s->r[3] = r3;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

/* ---- POS/VPOS (Factor.s:1317-1327) ---------------------------------- */

void basicvfp_hand_POS(struct ros_cpu *s)
{
    uint32_t r0, r1, r2;

    r0 = 134;                                     /* MOV R0,#&86 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_Byte);         /* SWI OS_Byte */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    r0 = r1 & 0xFFu;                              /* AND IACC,R1,#255 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    ROS_POLL(s, s->r[13]);
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_VPOS(struct ros_cpu *s)
{
    uint32_t r0, r1, r2;

    r0 = 134;                                     /* MOV R0,#&86 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_Byte);         /* SWI OS_Byte */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    r0 = r2 & 0xFFu;                              /* AND IACC,R2,#255 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    ROS_POLL(s, s->r[13]);
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

/* ---- RND (Factor.s:1101-1153) ---------------------------------------- */

void basicvfp_hand_RND(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r8 = s->r[8],
             r13 = s->r[13], r14 = s->r[14];
    uint32_t v16, v17;

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    {
        uint32_t r10 = ros_ld8(s->r[11]);         /* LDRB R10,[AELINE] */
        ros_subs(s, r10, 40);                     /* CMP R10,#"(" */
        if (r10 != 40)
            goto SIMPLE;                          /* BNE SIMPLE */
        s->r[10] = r10;
        s->r[11] = s->r[11] + 1;                  /* ADD AELINE,AELINE,#1 */
    }
    s->r[4] = r4; s->r[13] = r13;
    frd_rnd_bra(s);                               /* BL BRA */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r8 = s->r[8]; r13 = s->r[13]; r14 = s->r[14];
    s->r[14] = 0xFC103E28u;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC103E28u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r8 = s->r[8]; r13 = s->r[13]; r14 = s->r[14];
    v16 = r0;                                     /* TEQ IACC,#0 */
    if ((int32_t)r0 < 0)
        goto RNDSET;                              /* BMI RNDSET */
    if (r0 == 0)
        r0 = ros_ld32(r8 - 32);                   /* LDREQ IACC,[ARGP,#SEED] */
    if (v16 == 0)
        goto FRND;                                /* BEQ FRND */
    v17 = r0 ^ 1;                                 /* TEQ IACC,#1 */
    if (v17 == 0)
        goto FRND1;                               /* BEQ FRND1 */
    s->fp->vfp.sw[14] = r0;                       /* FMSR S14,IACC */
    s->fp->vfp.d[7] = (double)(int32_t)s->fp->vfp.sw[14]; /* FSITOD D7,S14 */
    r14 = 0xFC103E4Cu;                            /* BL DORANDOM */
    basicvfp_DORANDOM(s, &r0, &r1, &r2, &r3, r8, r14);
    ros_check_return(s, 0xFC103E4Cu);
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; /* BL FRNDAA */
    s->r[14] = 0xFC103E50u;
    basicvfp_FRNDAA(s);
    ros_check_return(s, 0xFC103E50u);
    {
        double v18 = s->fp->vfp.d[0] * s->fp->vfp.d[7]; /* FMULD FACC,FACC,D7 */
        s->fp->fpscr |= ros_vfp_ex2(v18, s->fp->vfp.d[0], s->fp->vfp.d[7]);
        s->fp->vfp.d[0] = v18;
    }
    s->fp->fpscr |= ros_vfp_int_ex(s->fp->vfp.d[0], ROS_ROUND_ZERO, 1); /* FTOSIZD */
    s->fp->vfp.sw[0] = (uint32_t)ros_to_int(s->fp->vfp.d[0], ROS_ROUND_ZERO);
    r0 = s->fp->fpscr;                            /* FMRX IACC,FPSCR */
    if ((r0 & 7) != 0) {                          /* TST; BNE VFPException */
        s->r[0] = r0;
        FRD_TAIL_CALL(basicvfp_VFPException)
    }
    r0 = s->fp->vfp.sw[0] + 1;                    /* ADD IACC,IACC,#1 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */

FRND1:
    r14 = 0xFC103E74u;                            /* BL DORANDOM */
    basicvfp_DORANDOM(s, &r0, &r1, &r2, &r3, r8, r14);
    ros_check_return(s, 0xFC103E74u);
    /* fall into FRND */
FRND:
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; /* BL FRNDAA */
    s->r[14] = 0xFC103E78u;
    basicvfp_FRNDAA(s);
    ros_check_return(s, 0xFC103E78u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r13 = s->r[13];
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_fsinstk)                    /* B FSINSTK */

RNDSET:
    ros_st32(r8 - 32, r0);                        /* STR IACC,[ARGP,#SEED] */
    ros_st8(r8 - 28, 64);                         /* STRB R4,[ARGP,#SEED+4] */
    s->r[0] = r0; s->r[4] = 64;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */

SIMPLE:
    s->r[13] = r13;                               /* the link pushed above */
    r14 = 0xFC103E90u;                            /* BL DORANDOM */
    basicvfp_DORANDOM(s, &r0, &r1, &r2, &r3, r8, r14);
    ros_check_return(s, 0xFC103E90u);
    s->r[0] = r0;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- TO (Factor.s:1424-1427) ----------------------------------------- */

void basicvfp_hand_TO(struct ros_cpu *s)
{
    uint32_t r0, r4 = s->r[4], r8 = s->r[8], r10, r11 = s->r[11],
             r13 = s->r[13];

    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, 80);                         /* CMP R10,#"P" */
    if (r10 != 80) {
        s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
        FRD_TAIL_CALL(basicvfp_FACERR)            /* BNE FACERR */
    }
    r0 = ros_ld32(r8 - 144);                      /* LDR IACC,[ARGP,#TOP] */
    s->r[0] = r0; s->r[10] = r10; s->r[11] = r11; /* the 'P' consumed */
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

/* ---- USR (Factor.s:1429-1439) ---------------------------------------- */

void basicvfp_hand_USR(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r5, r6, r7, r10 = s->r[10],
             r11 = s->r[11], r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_usr_factor(s);                            /* BL FACTOR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r6 = s->r[6]; r7 = s->r[7]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    s->r[14] = 0xFC103FC8u;                       /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    ros_check_return(s, 0xFC103FC8u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r6 = s->r[6]; r7 = s->r[7]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    r4 = r0;                                      /* MOV TYPE,IACC */
    r5 = 0;                                       /* MOV R5,#0 */
    s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    frd_usr_callarmrout(s);                       /* BL CALLARMROUT */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- GET$ and INKEY$ (Factor.s:1471-1502) ---------------------------- */

void basicvfp_hand_GETD(struct ros_cpu *s)
{
    uint32_t r0, r2, r4 = s->r[4], r8 = s->r[8], r10 = s->r[10],
             r11 = s->r[11], r13 = s->r[13];

    r0 = ros_ld8(r11);                            /* LDRB R0,[AELINE] */
    ros_logic(s, r0 ^ 0x23u, s->c);               /* TEQ R0,#"#" */
    if (s->z) {
        ROS_POLL(s, r13);
        goto GETDH;                               /* BEQ GETDH */
    }
    s->r[0] = r0; s->r[10] = r10; s->r[11] = r11;
    ros_native_swi(s, ros_thunk_OS_ReadC);        /* SWI OS_ReadC */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    frd_sinstr(s, r0, r8);                        /* B SINSTR */
    return;

GETDH:
    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[0] = r0; s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    frd_getdh_chan(s);                            /* BL CHAN */
    r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r2 = r8 - STRACC_OFF;                         /* ADD CLEN,ARGP,#STRACC */
    for (;;) {                                    /* GETDH1 */
        uint32_t len = r2 - (r8 - STRACC_OFF);    /* SUB R0,CLEN,R0 */
        ros_subs(s, len, 0xFFu);                  /* CMP R0,#255 */
        if (len >= 0xFFu)
            goto LEFTX;                           /* BCS LEFTX */
        s->r[0] = len;
        ros_native_swi(s, ros_thunk_OS_BGet);     /* SWI OS_BGet */
        if (s->v) ros_swi_raise(s);
        r0 = s->r[0];
        if (s->c) r0 = 10;                        /* MOVCS R0,#10 */
        if (r0 == 10 || r0 == 13)                 /* TEQ R0,#10 ; TEQNE 13 */
            goto LEFTX;
        ros_st8(r2, r0);                          /* STRNEB R0,[CLEN],#1 */
        r2 += 1;
        ROS_POLL(s, r13);
    }

LEFTX:
    s->r[0] = r0; s->r[2] = r2;
    FRD_TAIL_CALL(frd_leftx)                      /* B LEFTX */
}

void basicvfp_hand_INKED(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r8 = s->r[8], r10 = s->r[10],
             r11 = s->r[11], r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_inked_factor(s);                          /* BL FACTOR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4]; r8 = s->r[8];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    s->r[14] = 0xFC1040BCu;                       /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    ros_check_return(s, 0xFC1040BCu);
    r0 = s->r[0]; r4 = s->r[4]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    r1 = r0;                                      /* MOV R1,IACC */
    r2 = r0 >> 8;                                 /* MOV R2,IACC,LSR #8 */
    r0 = 129;                                     /* MOV R0,#&81 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    ros_native_swi(s, ros_thunk_OS_Byte);         /* SWI OS_Byte */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    {
        uint32_t r14 = ros_ld32(r13);             /* LDR R14,[SP],#4 */
        r13 += 4;
        s->r[13] = r13; s->r[14] = r14;
    }
    r0 = r1 & 0xFFu;                              /* AND IACC,R1,#255 */
    r2 &= 0xFFu;                                  /* ANDS R2,R2,#255 */
    if (r2 == 0) {                                /* BEQ SINSTR */
        s->r[0] = r0; s->r[2] = r2;
        ROS_POLL(s, r13);
        frd_sinstr(s, r0, r8);
        return;
    }
    s->r[0] = r0; s->r[2] = r2;
    FRD_TAIL_CALL(basicvfp_hand_RNUL)             /* BNE RNUL */
}

/* ---- STR$ (Factor.s:1602-1624) --------------------------------------- */

void basicvfp_hand_STRD(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9 = s->r[9],
             r10, r11 = s->r[11], r13 = s->r[13], r14 = s->r[14];

    do {                                          /* STRD */
        r10 = ros_ld8(r11);                       /* LDRB R10,[AELINE],#1 */
        r11 += 1;
    } while (r10 == 32);                          /* BEQ STRD */
    ros_subs(s, r10, 126);                        /* CMP R10,#"~" */
    r5 = r10 == 126;                              /* MOVEQ R5,#1 ; MOVNE #0 */
    if (r10 != 126)
        r11 -= 1;                                 /* SUBNE AELINE,AELINE,#1 */
    r13 -= 8;                                     /* STMFD SP!,{R5,R14} */
    ros_st32(r13, r5);
    ros_st32(r13 + 4, r14);
    s->r[5] = r5; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_strd_factor(s);                           /* BL FACTOR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13];
    r4 = ros_ld32(r8 - 0x100u);                   /* LDR R4,[ARGP,#INTVAR] */
    ros_subs(s, r4, 0x1000000u);                  /* CMP R4,#&1000000 */
    if (r4 < 0x1000000u)
        r4 = 0;                                   /* MOVCC R4,#0 */
    r5 = ros_ld32(r13);                           /* LDR R5,[SP],#4 */
    r13 += 4;
    s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    frd_strd_fconfp(s);                           /* BL FCONFP */
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r2 = r9;                                      /* MOV CLEN,TYPE */
    r9 = ros_logic(s, 0, s->c);                   /* MOVS TYPE,#0 */
    s->r[2] = r2; s->r[9] = r9;
    s->r[15] = ros_ld32(r13);                     /* LDR PC,[SP],#4 */
    s->r[13] = r13 + 4;
}

/* ---- EOF (Factor.s:1646-1653) ---------------------------------------- */

void basicvfp_hand_EOF(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_eof_chan(s);                              /* BL CHAN */
    r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13];
    r0 = 0x7Fu;                                   /* MOV R0,#&7F */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_Byte);         /* SWI OS_Byte */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    r0 = r1 & 0xFFu;                              /* ANDS IACC,R1,#255 */
    if (r0 != 0)
        r0 = 0xFFFFFFFFu;                         /* MVNNE IACC,#0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- RSYS: SYS as a function (Factor.s:756-837) ---------------------- */

void basicvfp_hand_RSYS(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r8 = s->r[8], r10, r11 = s->r[11],
             r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_subs(s, r10, 40);                         /* CMP R10,#"(" */
    if (r10 != 40) {                              /* BNE ERBRA1 */
        s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
        frd_msg_at(s, SITE_ERBRA1);
    }
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_rsys_expr(s);                             /* BL EXPR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4]; r8 = s->r[8];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    ros_subs(s, r10, 41);                         /* CMP R10,#")" */
    if (r10 != 41) {
        s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERBRA)             /* BNE ERBRA */
    }
    ros_logic(s, s->r[9], s->c);                  /* TEQ TYPE,#0 */
    if (s->r[9] == 0)
        goto RSYSSTR;                             /* BEQ RSYSSTR */
    s->r[14] = 0xFC103A30u;                       /* BL INTEGZ */
    basicvfp_hand_INTEGZ(s);
    ros_check_return(s, 0xFC103A30u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r8 = s->r[8];
    r13 = s->r[13];
    ros_subs(s, r0, 0);                           /* CMP IACC,#0 */
    if ((int32_t)r0 < 0)
        goto RSYSINF;                             /* BLT RSYSINF */
    r1 = r8 - STRACC_OFF;                         /* ADD R1,ARGP,#STRACC */
    r2 = 0x100u;                                  /* MOV R2,#256 */
    s->r[1] = r1; s->r[2] = r2;
    ros_native_swi(s, ros_thunk_OS_SWINumberToString); /* SWI OS_SWINumberToString */
    if (s->v) ros_swi_raise(s);
    r2 = s->r[2];
    r2 = r1 + r2 - 1;                             /* STRB R14,[CLEN,#-1]! */
    ros_st8(r2, 13);                              /* MOV R14,#13 */
    r0 = r1;                                      /* MOV IACC,R1 */
    s->r[0] = r0; s->r[2] = r2;
    FRD_TAIL_CALL(frd_leftx)                      /* B LEFTX */

RSYSSTR:
    r1 = r8 - STRACC_OFF;                         /* ADD R1,ARGP,#STRACC */
    r0 = 0;                                       /* MOV R0,#0 */
    ros_st8(r2, 0);                               /* STRB R0,[CLEN] */
    s->r[0] = r0; s->r[1] = r1;
    ros_native_swi(s, ros_thunk_OS_SWINumberFromString); /* SWI OS_SWINumberFromString */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */

RSYSINF:
    r14 = ros_ld32(r13);                          /* LDR R14,[SP],#4 */
    r13 += 4;
    s->r[13] = r13; s->r[14] = r14;               /* the S_* exits' SINSTK */
    r0 = -r0;                                     /* RSB R0,IACC,#0 */
    ros_subs(s, r0, 13);                          /* CMP R0,#(S_BASID-SR0):SHR:2 */
    if (r0 < 13)
        r0 = ros_ld32(BASICVFP_SR0 + (r0 << 2));  /* LDRLO R0,[PC,R0,LSL #2] */
    if (!s->c) {                                  /* ADDLO PC,PC,R0 */
        switch (r0) {
        case -369086110:
            s->r[0] = r0; s->r[13] = r13; s->r[14] = r14;
            FRD_TAIL_CALL(basicvfp_ERRSUB)
        case 48: goto S_BASID;
        case 64: goto S_MODV;
        case 72: goto S_LANGV;
        case 80: goto S_FLAGS;
        case 88: goto S_CPU;
        case 96: goto S_INTB;
        case 104: goto S_FPB;
        case 112: goto S_STR;
        case 120: goto S_FPFMT;
        case 128: goto S_ARGP;
        case 136: goto S_CALL2;
        default: ros_fault(s, 0xFC103A7Cu, "a jump table index out of range");
        }
    }
    s->r[0] = r0; s->r[13] = r13; s->r[14] = r14;
    FRD_TAIL_CALL(basicvfp_ERRSUB)                /* B ERRSUB */

S_BASID:
    r0 = 0xBA51C005u;                             /* ADRL IACC,BASICID */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
S_MODV:
    r0 = 187;                                     /* MOV IACC,#Module_Version */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_LANGV:
    r0 = 6;                                       /* MOV IACC,#6 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_FLAGS:
    r0 = 0;                                       /* MOV IACC,#0 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_CPU:
    r0 = 0;                                       /* MOV IACC,#0 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_INTB:
    r0 = 32;                                      /* MOV IACC,#32 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_FPB:
    r0 = 64;                                      /* MOV IACC,#(TFPLV*8) */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_STR:
    r0 = 8;                                       /* MOV IACC,#8 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_FPFMT:
    r0 = FPOINT;                                  /* MOV IACC,#FPOINT */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_ARGP:
    r0 = s->r[8];                                 /* MOV IACC,ARGP */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
S_CALL2:
    r0 = BASICVFP_CALL2;                          /* ADRL IACC,CALL2 */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)
}

/* ---- RQUIT: QUIT as a function --------------------------------------- */

void basicvfp_hand_RQUIT(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld8(s->r[8] - 3);           /* LDRB R0,[ARGP,#CALLEDNAME] */
    ros_subs(s, r0, 0);                           /* CMP R0,#0 */
    if (r0 == 0) {                                /* BEQ TRUE */
        ROS_POLL(s, s->r[13]);
        FRD_TAIL_CALL(basicvfp_hand_TRUE)
    }
    FRD_TAIL_CALL(basicvfp_hand_FALSE)            /* else FALSE */
}

/* ---- the channel readers (Factor.s:994-998, 1166-, 1289-) ------------ */

void basicvfp_hand_BBGET(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_bbget_chan(s);                            /* BL CHAN */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    ros_native_swi(s, ros_thunk_OS_BGet);         /* SWI OS_BGet */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

void basicvfp_hand_EXT(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r4 = s->r[4], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, s->r[14]);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_ext_chan(s);                              /* BL CHAN */
    r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r0 = 2;                                       /* MOV R0,#2 */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_Args);         /* SWI OS_Args (RPTRA) */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2];
    r0 = r2;                                      /* MOV IACC,R2 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- the plain state words -------------------------------------------- */

void basicvfp_hand_COUNT(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld32(s->r[8] - 264);        /* LDR IACC,[ARGP,#TALLY] */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_GIVEEND(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld32(s->r[8] - 140);        /* LDR IACC,[ARGP,#FSA] */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_ERL(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld32(s->r[8] - 116);        /* LDR IACC,[ARGP,#ERRLIN] */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

void basicvfp_hand_ERR(struct ros_cpu *s)
{
    uint32_t r0 = ros_ld32(s->r[8] - 128);        /* LDR IACC,[ARGP,#ERRNUM] */
    s->r[0] = r0;
    FRD_TAIL_CALL(frd_sinstk)                     /* B SINSTK */
}

/* ---- SUM (Array.s:1543-1612) ------------------------------------------ */

void basicvfp_hand_SUM(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r5, r6, r7, r8 = s->r[8],
             r9 = s->r[9], r10, r11 = s->r[11], r13 = s->r[13],
             r14 = s->r[14];
    uint32_t v28;
    double v31;

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    r10 = ros_ld8(r11);                           /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == TLEN)                              /* CMP R10,#TLEN */
        goto SUMLEN;                              /* BEQ SUMLEN */
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_sum_raf1(s);                              /* BL READARRAYFACTOR1 */
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_subs(s, r9, 0x100u);                 /* SUBS R1,TYPE,#256 */
    if (r9 < 0x100u) {
        s->r[1] = r1; s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERARRY)            /* BCC ERARRY */
    }
    s->r[1] = r1; s->r[13] = r13;
    s->r[14] = 0xFC10934Cu;                       /* BL GETARRAYSIZE */
    basicvfp_GETARRAYSIZE(s);
    ros_check_return(s, 0xFC10934Cu);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    if (r1 == TFPLV)
        goto SUMFP;                               /* BEQ SUMFP */
    if (r1 > TFPLV)
        goto SUMSTRING;                           /* BHI SUMSTRING */
    r0 = 0;                                       /* MOV IACC,#0 */
    if (r10 < 8)
        goto L_FC1093AC;                          /* BLO %FT60 */
    if ((ros_ld8(r8 - 4) & VFPFLAG_NEON) == 0)    /* TST R14,#VFPFLAG_NEON */
        goto L_FC1093AC;                          /* BEQ %FT60 */
    r10 -= 8;                                     /* SUB R10,R10,#8 */
    s->fp->vfp.d[0] = ros_ldd(r9);                /* VLDMIA TYPE!,{Q0} */
    s->fp->vfp.d[1] = ros_ldd(r9 + 8);
    r9 += 16;
L_FC109378:
    s->fp->vfp.d[2] = ros_ldd(r9);                /* VLDMIA TYPE!,{Q1} */
    s->fp->vfp.d[3] = ros_ldd(r9 + 8);
    v28 = r10;                                    /* SUBS R10,R10,#4 */
    r10 -= 4;
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[4]);
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[1] + (int32_t)s->fp->vfp.sw[5]);
    s->fp->vfp.sw[2] = (uint32_t)((int32_t)s->fp->vfp.sw[2] + (int32_t)s->fp->vfp.sw[6]);
    s->fp->vfp.sw[3] = (uint32_t)((int32_t)s->fp->vfp.sw[3] + (int32_t)s->fp->vfp.sw[7]);
    if (v28 >= 4) {                               /* VLDMIAHS TYPE!,{Q2} */
        s->fp->vfp.d[4] = ros_ldd(r9 + 16);
        s->fp->vfp.d[5] = ros_ldd(r9 + 24);
    }
    r9 = v28 >= 4 ? r9 + 32 : r9 + 16;
    if (v28 < 4)
        goto L_FC109398;                          /* BLO %FT50 */
    r10 -= 4;                                     /* SUBS R10,R10,#4 */
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[8]);
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[1] + (int32_t)s->fp->vfp.sw[9]);
    s->fp->vfp.sw[2] = (uint32_t)((int32_t)s->fp->vfp.sw[2] + (int32_t)s->fp->vfp.sw[10]);
    s->fp->vfp.sw[3] = (uint32_t)((int32_t)s->fp->vfp.sw[3] + (int32_t)s->fp->vfp.sw[11]);
    if (v28 - 4 >= 4) { ROS_POLL(s, r13); goto L_FC109378; } /* BHS %BT10 */
L_FC109398:
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[1]); /* VPADD */
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[2] + (int32_t)s->fp->vfp.sw[3]);
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[1]);
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[1]);
    r10 = ros_adds(s, r10, 4);                    /* ADDS R10,R10,#4 */
    r0 = s->fp->vfp.sw[0];                        /* VMOV.32 IACC,D0[0] */
    if (r10 == 0)
        goto L_FC1093BC;                          /* BEQ %FT90 */
L_FC1093AC:
    do {                                          /* the word loop */
        uint32_t w = ros_ld32(r9);                /* LDR R14,[TYPE],#4 */
        r9 += 4;
        r10 = ros_subs(s, r10, 1);                /* SUBS R10,R10,#1 */
        r0 += w;                                  /* ADD IACC,IACC,R14 */
        ROS_POLL(s, r13);
    } while (r10 != 0);                           /* BNE %BT60 */
L_FC1093BC:
    s->r[0] = r0; s->r[9] = r9; s->r[10] = r10;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */

SUMFP:
    s->fp->vfp.d[0] = 0.0;                        /* FLDD FACC,=0 */
    if (r10 < 8)
        goto L_FC109420;                          /* BLO %FT60 */
    if ((ros_ld8(r8 - 4) & VFPFLAG_Vectors) == 0)
        goto L_FC109420;                          /* BEQ %FT60 */
    s->fp->fpscr = 0x30000u;                      /* FMXR FPSCR,R14 */
    r10 -= 8;                                     /* SUB R10,R10,#8 */
    s->fp->vfp.d[4] = ros_ldd(r9);                /* FLDMIAD TYPE!,{D4-D7} */
    s->fp->vfp.d[5] = ros_ldd(r9 + 8);
    s->fp->vfp.d[6] = ros_ldd(r9 + 16);
    s->fp->vfp.d[7] = ros_ldd(r9 + 24);
    r9 += 32;
L_FC1093E8:
    s->fp->vfp.d[8] = ros_ldd(r9);                /* FLDMIAD TYPE!,{D8-D11} */
    s->fp->vfp.d[9] = ros_ldd(r9 + 8);
    s->fp->vfp.d[10] = ros_ldd(r9 + 16);
    s->fp->vfp.d[11] = ros_ldd(r9 + 24);
    {
        uint32_t v30 = r10;                       /* SUBS R10,R10,#4.  The
                                                   * HS tests below read the
                                                   * count from before the
                                                   * SUBS, and the SUBHSS
                                                   * line takes both
                                                   * decrements.  This is
                                                   * the lift's shape
                                                   * (Review, 6 Oct). */
        v31 = s->fp->vfp.d[4] + s->fp->vfp.d[8];  /* FADDD D4,D4,D8 */
        s->fp->fpscr |= ros_vfp_ex2(v31, s->fp->vfp.d[4], s->fp->vfp.d[8]);
        s->fp->vfp.d[4] = v31;
        if (r10 >= 4) {                           /* FLDMIADHS TYPE!,{D12-D15} */
            s->fp->vfp.d[12] = ros_ldd(r9 + 32);
            s->fp->vfp.d[13] = ros_ldd(r9 + 40);
            s->fp->vfp.d[14] = ros_ldd(r9 + 48);
            s->fp->vfp.d[15] = ros_ldd(r9 + 56);
        }
        r9 = r10 >= 4 ? r9 + 64 : r9 + 32;
        {
            double v32 = s->fp->vfp.d[4] + s->fp->vfp.d[12]; /* FADDDHS */
            if (r10 >= 4) {
                s->fp->fpscr |= ros_vfp_ex2(v32, s->fp->vfp.d[4], s->fp->vfp.d[12]);
                s->fp->vfp.d[4] = v32;
            }
        }
        r10 = r10 >= 4 ? r10 - 8 : r10 - 4;       /* SUBHSS R10,R10,#4 */
        if (v30 >= 4 && v30 - 4 >= 4) { ROS_POLL(s, r13); goto L_FC1093E8; }
    }
    r10 = ros_adds(s, r10, 4);                    /* ADDS R10,R10,#4 */
    s->fp->fpscr = 0;                             /* FMXR FPSCR,R14 */
    {
        double v33 = s->fp->vfp.d[4] + s->fp->vfp.d[5]; /* FADDD D4,D4,D5 */
        s->fp->fpscr |= ros_vfp_ex2(v33, s->fp->vfp.d[4], s->fp->vfp.d[5]);
        s->fp->vfp.d[4] = v33;
        v31 = s->fp->vfp.d[6] + s->fp->vfp.d[7];  /* FADDD FACC,D6,D7 */
        s->fp->fpscr |= ros_vfp_ex2(v31, s->fp->vfp.d[6], s->fp->vfp.d[7]);
        s->fp->vfp.d[0] = v31;
        s->fp->fpscr |= ros_vfp_ex2(s->fp->vfp.d[0] + s->fp->vfp.d[4],
                                    s->fp->vfp.d[0], s->fp->vfp.d[4]);
        s->fp->vfp.d[0] += s->fp->vfp.d[4];
    }
    if (r10 == 0)
        goto L_FC109430;                          /* BEQ %FT90 */
L_FC109420:
    do {                                          /* FLDD D1,[TYPE],#8 */
        double d1 = ros_ldd(r9);
        r9 += 8;
        r10 = ros_subs(s, r10, 1);                /* SUBS R10,R10,#1 */
        s->fp->fpscr |= ros_vfp_ex2(s->fp->vfp.d[0] + d1,
                                    s->fp->vfp.d[0], d1); /* FADDD */
        s->fp->vfp.d[0] += d1;
        ROS_POLL(s, r13);
    } while (r10 != 0);                           /* BNE %BT60 */
L_FC109430:
    r14 = s->fp->fpscr;                           /* FMRX R14,FPSCR */
    if ((r14 & 7) != 0) {                         /* BNE VFPException */
        s->r[9] = r9; s->r[10] = r10; s->r[14] = r14;
        FRD_TAIL_CALL(basicvfp_VFPException)
    }
    s->r[9] = r9; s->r[10] = r10;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_fsinstk)                    /* B FSINSTK */

SUMSTRING:
    r2 = r8 - STRACC_OFF;                         /* ADD CLEN,ARGP,#STRACC */
    {
        uint32_t r7 = r2 + 0x100u;                /* ADD R7,CLEN,#256 */
SUMSTRING1:
        r0 = ros_ld8(r9 + 4);                     /* LDRB R0,[TYPE,#4] */
        if (r0 == 0)
            goto SUMSTRING3;                      /* BEQ SUMSTRING3 */
        {
            uint32_t r5 = r2 + r0;                /* ADD R5,CLEN,R0 */
            ros_subs(s, r5, r7);                  /* CMP R5,R7 */
            if (r5 >= r7) {                       /* BCS ERLONG */
                s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
                s->r[4] = r4; s->r[5] = r5; s->r[7] = r7; s->r[9] = r9;
                s->r[10] = r10; s->r[13] = r13;
                FRD_TAIL_CALL(basicvfp_ERLONG)
            }
        }
        {   /* the descriptor's pointer, the LDW macro's unaligned load */
            uint32_t v36 = r9 & 3;                /* ANDS R4,TYPE,#3 */
            if (v36 == 0) {
                r1 = ros_ld32(r9);                /* LDREQ R1,[TYPE] */
            } else {
                uint32_t lo = ros_ld32(r9 & ~3u);
                uint32_t hi = ros_ld32((r9 & ~3u) + 4);
                r1 = ros_lsr(lo, v36 << 3) | ros_lsl(hi, 32 - (v36 << 3));
            }
        }
SUMSTRING2:
        r3 = ros_ld8(r1);                         /* LDRB R3,[R1],#1 */
        r1 += 1;
        ros_st8(r2, r3);                          /* STRB R3,[CLEN],#1 */
        r2 += 1;
        r0 -= 1;                                  /* SUBS R0,R0,#1 */
        if (r0 != 0) { ROS_POLL(s, r13); goto SUMSTRING2; }
SUMSTRING3:
        r9 += 5;                                  /* ADD TYPE,TYPE,#5 */
        r10 = ros_subs(s, r10, 1);                /* SUBS R10,R10,#1 */
        if (r10 != 0) { ROS_POLL(s, r13); goto SUMSTRING1; }
    }
    r9 = ros_logic(s, 0, s->c);                   /* MOVS TYPE,#0 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[9] = r9; s->r[10] = r10;
    s->r[15] = ros_ld32(r13);                     /* LDR PC,[SP],#4 */
    s->r[13] = r13 + 4;
    return;

SUMLEN:
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_sumlen_raf(s);                            /* BL READARRAYFACTOR */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    ros_subs(s, r9, 384);                         /* CMP TYPE,#256+128 */
    if (r9 != 384) {
        s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERTYPESTRINGARRAY) /* BNE ERTYPESTRINGARRAY */
    }
    s->r[13] = r13;
    s->r[14] = 0xFC1094B4u;                       /* BL GETARRAYSIZE */
    basicvfp_GETARRAYSIZE(s);
    ros_check_return(s, 0xFC1094B4u);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13]; r14 = s->r[14];
    r0 = 0;                                       /* MOV IACC,#0 */
SUMLEN3:
    r1 = ros_ld8(r9 + 4);                         /* LDRB R1,[TYPE,#4] */
    r9 += 5;                                      /* ADD TYPE,TYPE,#5 */
    r0 += r1;                                     /* ADD IACC,IACC,R1 */
    r10 = ros_subs(s, r10, 1);                    /* SUBS R10,R10,#1 */
    if (r10 != 0) { ROS_POLL(s, r13); goto SUMLEN3; }
    s->r[0] = r0; s->r[1] = r1; s->r[9] = r9; s->r[10] = r10;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_psinstk)                    /* B PSINSTK */
}

/* ---- MODULUS (Array.s:1615-1670) -------------------------------------- */

void basicvfp_hand_MODULUS(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4 = s->r[4], r5, r6, r7, r8 = s->r[8],
             r9 = s->r[9], r10 = s->r[10], r11 = s->r[11], r13 = s->r[13],
             r14 = s->r[14];
    uint32_t v40;
    double v44, v45;

    r13 -= 4;                                     /* STR R14,[SP,#-4]! */
    ros_st32(r13, r14);
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    frd_modulus_raf(s);                           /* BL READARRAYFACTOR */
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_subs(s, r9, 0x100u);                 /* SUBS R1,TYPE,#256 */
    if (r9 < 0x100u) {
        s->r[1] = r1; s->r[13] = r13;
        FRD_TAIL_CALL(basicvfp_ERARRY)            /* BCC ERARRY */
    }
    s->r[1] = r1; s->r[13] = r13;
    s->r[14] = 0xFC1094E4u;                       /* BL GETARRAYSIZE */
    basicvfp_GETARRAYSIZE(s);
    ros_check_return(s, 0xFC1094E4u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r13 = s->r[13];
    ros_subs(s, r1, TFPLV);                       /* CMP R1,#TFPLV */
    if (r1 == TFPLV)
        goto MODULUSFP;                           /* BEQ MODULUSFP */
    if (r1 > TFPLV) {                             /* BHI ERTYPENUMARRAY */
        s->r[13] = r13;
        frd_msg_at(s, SITE_ERTYPENUMARRAY);
    }
    r0 = 0;                                       /* MOV IACC,#0 */
    if (r10 < 8)
        goto L_FC109548;                          /* BLO %FT60 */
    if ((ros_ld8(r8 - 4) & VFPFLAG_NEON) == 0)
        goto L_FC109548;                          /* BEQ %FT60 */
    r10 -= 8;                                     /* SUB R10,R10,#8 */
    r9 += 16;                                     /* VLDMIA TYPE!,{Q0} */
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] * (int32_t)s->fp->vfp.sw[0]);
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[1] * (int32_t)s->fp->vfp.sw[1]);
    s->fp->vfp.sw[2] = (uint32_t)((int32_t)s->fp->vfp.sw[2] * (int32_t)s->fp->vfp.sw[2]);
    s->fp->vfp.sw[3] = (uint32_t)((int32_t)s->fp->vfp.sw[3] * (int32_t)s->fp->vfp.sw[3]);
L_FC109514:
    s->fp->vfp.d[2] = ros_ldd(r9);                /* VLDMIA TYPE!,{Q1} */
    s->fp->vfp.d[3] = ros_ldd(r9 + 8);
    v40 = r10;                                    /* SUBS R10,R10,#4 */
    r10 -= 4;
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[4] * (int32_t)s->fp->vfp.sw[4]);
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[1] + (int32_t)s->fp->vfp.sw[5] * (int32_t)s->fp->vfp.sw[5]);
    s->fp->vfp.sw[2] = (uint32_t)((int32_t)s->fp->vfp.sw[2] + (int32_t)s->fp->vfp.sw[6] * (int32_t)s->fp->vfp.sw[6]);
    s->fp->vfp.sw[3] = (uint32_t)((int32_t)s->fp->vfp.sw[3] + (int32_t)s->fp->vfp.sw[7] * (int32_t)s->fp->vfp.sw[7]);
    if (v40 >= 4) {                               /* VLDMIAHS TYPE!,{Q2} */
        s->fp->vfp.d[4] = ros_ldd(r9 + 16);
        s->fp->vfp.d[5] = ros_ldd(r9 + 24);
    }
    r9 = v40 >= 4 ? r9 + 32 : r9 + 16;
    if (v40 < 4)
        goto L_FC109534;                          /* BLO %FT50 */
    r10 -= 4;                                     /* SUBS R10,R10,#4 */
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[8] * (int32_t)s->fp->vfp.sw[8]);
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[1] + (int32_t)s->fp->vfp.sw[9] * (int32_t)s->fp->vfp.sw[9]);
    s->fp->vfp.sw[2] = (uint32_t)((int32_t)s->fp->vfp.sw[2] + (int32_t)s->fp->vfp.sw[10] * (int32_t)s->fp->vfp.sw[10]);
    s->fp->vfp.sw[3] = (uint32_t)((int32_t)s->fp->vfp.sw[3] + (int32_t)s->fp->vfp.sw[11] * (int32_t)s->fp->vfp.sw[11]);
    if (v40 - 4 >= 4) { ROS_POLL(s, r13); goto L_FC109514; } /* BHS %BT10 */
L_FC109534:
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[1]); /* VPADD */
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[2] + (int32_t)s->fp->vfp.sw[3]);
    s->fp->vfp.sw[0] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[1]);
    s->fp->vfp.sw[1] = (uint32_t)((int32_t)s->fp->vfp.sw[0] + (int32_t)s->fp->vfp.sw[1]);
    r10 = ros_adds(s, r10, 4);                    /* ADDS R10,R10,#4 */
    r0 = s->fp->vfp.sw[0];                        /* VMOV.32 IACC,D0[0] */
    if (r10 == 0)
        goto L_FC109558;                          /* BEQ %FT90 */
L_FC109548:
    do {                                          /* the word loop */
        uint32_t w = ros_ld32(r9);                /* LDR R14,[TYPE],#4 */
        r9 += 4;
        r10 = ros_subs(s, r10, 1);                /* SUBS R10,R10,#1 */
        r0 = w * w + r0;                          /* MLA IACC,R14,R14,IACC */
        ROS_POLL(s, r13);
    } while (r10 != 0);                           /* BNE %BT60 */
L_FC109558:
    s->fp->vfp.sw[0] = r0;                        /* FMSR S0,IACC */
    s->fp->vfp.d[0] = (double)(int32_t)s->fp->vfp.sw[0]; /* FSITOD FACC,S0 */
    goto FSQRTJOIN;

MODULUSFP:
    s->fp->vfp.d[0] = 0.0;                        /* FLDD FACC,=0 */
    if (r10 < 8)
        goto L_FC1095D8;                          /* BLO %FT60 */
    if ((ros_ld8(r8 - 4) & VFPFLAG_Vectors) == 0)
        goto L_FC1095D8;                          /* BEQ %FT60 */
    s->fp->fpscr = 0x30000u;                      /* FMXR FPSCR,R14 */
    r10 -= 8;                                     /* SUB R10,R10,#8 */
    s->fp->vfp.d[4] = ros_ldd(r9);                /* FLDMIAD TYPE!,{D4-D7} */
    s->fp->vfp.d[5] = ros_ldd(r9 + 8);
    s->fp->vfp.d[6] = ros_ldd(r9 + 16);
    s->fp->vfp.d[7] = ros_ldd(r9 + 24);
    r9 += 32;
    s->fp->fpscr |= ros_vfp_ex2(s->fp->vfp.d[4] * s->fp->vfp.d[4],
                                s->fp->vfp.d[4], s->fp->vfp.d[4]); /* FMULD */
    s->fp->vfp.d[4] *= s->fp->vfp.d[4];
L_FC1095A0:
    s->fp->vfp.d[8] = ros_ldd(r9);                /* FLDMIAD TYPE!,{D8-D11} */
    s->fp->vfp.d[9] = ros_ldd(r9 + 8);
    s->fp->vfp.d[10] = ros_ldd(r9 + 16);
    s->fp->vfp.d[11] = ros_ldd(r9 + 24);
    {
        uint32_t v43 = r10;                       /* SUBS R10,R10,#4.  The
                                                   * HS tests below read the
                                                   * count from before the
                                                   * SUBS, and the SUBHSS
                                                   * line takes both
                                                   * decrements.  This is
                                                   * the lift's shape
                                                   * (Review, 6 Oct). */
        v44 = s->fp->vfp.d[8] * s->fp->vfp.d[8];  /* FMACD D4,D8,D8 */
        s->fp->fpscr |= ros_vfp_ex2(v44, s->fp->vfp.d[8], s->fp->vfp.d[8]);
        v45 = s->fp->vfp.d[4] + v44;
        s->fp->fpscr |= ros_vfp_ex2(v45, s->fp->vfp.d[4], v44);
        s->fp->vfp.d[4] = v45;
        if (r10 >= 4) {                           /* FLDMIADHS TYPE!,{D12-D15} */
            s->fp->vfp.d[12] = ros_ldd(r9 + 32);
            s->fp->vfp.d[13] = ros_ldd(r9 + 40);
            s->fp->vfp.d[14] = ros_ldd(r9 + 48);
            s->fp->vfp.d[15] = ros_ldd(r9 + 56);
        }
        r9 = r10 >= 4 ? r9 + 64 : r9 + 32;
        {
            double v46 = s->fp->vfp.d[12] * s->fp->vfp.d[12]; /* FMACDHS */
            if (r10 >= 4)
                s->fp->fpscr |= ros_vfp_ex2(v46, s->fp->vfp.d[12], s->fp->vfp.d[12]);
            v45 = s->fp->vfp.d[4] + v46;
            if (r10 >= 4) {
                s->fp->fpscr |= ros_vfp_ex2(v45, s->fp->vfp.d[4], v46);
                s->fp->vfp.d[4] = v45;
            }
        }
        r10 = r10 >= 4 ? r10 - 8 : r10 - 4;       /* SUBHSS R10,R10,#4 */
        if (v43 >= 4 && v43 - 4 >= 4) { ROS_POLL(s, r13); goto L_FC1095A0; }
    }
    r10 = ros_adds(s, r10, 4);                    /* ADDS R10,R10,#4 */
    s->fp->fpscr = 0;                             /* FMXR FPSCR,R14 */
    {
        double v48 = s->fp->vfp.d[4] + s->fp->vfp.d[5]; /* FADDD D4,D4,D5 */
        s->fp->fpscr |= ros_vfp_ex2(v48, s->fp->vfp.d[4], s->fp->vfp.d[5]);
        s->fp->vfp.d[4] = v48;
        v44 = s->fp->vfp.d[6] + s->fp->vfp.d[7];  /* FADDD FACC,D6,D7 */
        s->fp->fpscr |= ros_vfp_ex2(v44, s->fp->vfp.d[6], s->fp->vfp.d[7]);
        s->fp->vfp.d[0] = v44;
        s->fp->fpscr |= ros_vfp_ex2(s->fp->vfp.d[0] + s->fp->vfp.d[4],
                                    s->fp->vfp.d[0], s->fp->vfp.d[4]);
        s->fp->vfp.d[0] += s->fp->vfp.d[4];
    }
    if (r10 == 0)
        goto L_FC1095E8;                          /* BEQ %FT90 */
L_FC1095D8:
    do {                                          /* FLDD D1,[TYPE],#8 */
        double d1 = ros_ldd(r9);
        r9 += 8;
        r10 = ros_subs(s, r10, 1);                /* SUBS R10,R10,#1 */
        {
            double v50 = d1 * d1;                 /* FMACD FACC,D1,D1 */
            s->fp->fpscr |= ros_vfp_ex2(v50, d1, d1);
            s->fp->fpscr |= ros_vfp_ex2(s->fp->vfp.d[0] + v50,
                                        s->fp->vfp.d[0], v50);
            s->fp->vfp.d[0] += v50;
        }
        ROS_POLL(s, r13);
    } while (r10 != 0);                           /* BNE %BT60 */
L_FC1095E8:
    r14 = s->fp->fpscr;                           /* FMRX R14,FPSCR */
    if ((r14 & 7) != 0) {                         /* BNE VFPException */
        s->r[9] = r9; s->r[10] = r10; s->r[14] = r14;
        FRD_TAIL_CALL(basicvfp_VFPException)
    }
FSQRTJOIN:
    s->fp->fpscr |= ros_vfp_sqrt_ex(s->fp->vfp.d[0]); /* FSQRTD FACC,FACC */
    s->fp->vfp.d[0] = sqrt(s->fp->vfp.d[0]);
    r14 = s->fp->fpscr;                           /* FMRX R14,FPSCR */
    if ((r14 & 7) != 0) {                         /* BNE VFPException_SQRT */
        ros_logic(s, r14 & FPSCR_IOC, s->c);      /* TST R14,#FPSCR_IOC */
        if (!s->z) {                              /* BNE FSQRTN */
            s->r[0] = r0; s->r[9] = r9; s->r[10] = r10; s->r[13] = r13;
            frd_msg_at(s, SITE_FSQRTN);
        }
        s->r[0] = r0; s->r[9] = r9; s->r[10] = r10; s->r[14] = r14;
        FRD_TAIL_CALL(basicvfp_VFPException)
    }
    s->r[9] = r9; s->r[10] = r10;
    ROS_POLL(s, r13);
    FRD_TAIL_CALL(frd_fsinstk)                    /* B FSINSTK */
}
