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
 * (Sources/Programmer/BASIC: s.Array, s.Basic, s.Stmt, s.Stmt2, hdr.Definitions, hdr.Tokens).
 */

/* dispat_assign.c: the assignment family of the statement executor,
 * translated by hand from the RISC OS 5.31 BASIC VFP ObjAsm.  It
 * covers:
 *   - the LETST web (Basic.s:717-900: GOTLTEND1/2, MINUSBC,
 *     GOTLT2/ATGOTLT2, the PLUSBC family, LETSTNOTCACHE, GOTLT/1,
 *     LETSTCACHEARRAY, LETST, EXPRSTORESTMT);
 *   - LET and LOCAL (Stmt.s:785-870);
 *   - DIM, with its LOCAL form (Stmt.s:157-341);
 *   - SWAP (Stmt.s:1564-1612);
 *   - the LVALUE keyword assignments: LPAGE, LTIME, LLOMEM and LHIMEM
 *     (Stmt2.s:54-201), LLEFTD, LMIDD and LRIGHTD (:202-300),
 *     ASSIGNAT (:301-380), and LEXT and LPTR (:1739-1750);
 *   - the whole-array operations (Array.s:417-1664), which are reached
 *     only through LETST's LETARRAY.  These are the array=array copy,
 *     the elementwise + - * / in their integer, FP and string forms,
 *     array op constant in both orders, negation, constant fill, the
 *     misuse of the DIM LOCAL list for initialisers, and matrix and
 *     vector multiply.
 *
 * These are the hottest statements in the interpreter, since every
 * assignment and every array element store passes through them.  The
 * conventions are the ones the landed units use:
 *   - State is kept in R[].
 *   - A BL becomes a marker call.  R14 is set to 0xFFFFFFF0u, the
 *     lift's store sets come before it and its reload sets after it.
 *     This is the ARM APCS border protocol.
 *   - A B becomes a plain call whose result is discarded.  The
 *     compiler flattens it back to the original's branch.
 *   - Setjmp bodies (EXPR, EQAEEX, FACTOR, LVCONT, LVBLNK, CRAELV,
 *     AELV, DOEXCEPTION) are plain calls with no resume points.
 *   - Callees that never return (the error raisers) are reached by a
 *     final call followed by a return.
 *
 * The quirks of the original that are kept, with their lines:
 *   - LETST's letter cases all share one body.  The cache probe is at
 *     ARGP+(LINE&CACHEMASK)<<4.  Only a TFP-tagged delta (top bit set,
 *     CMN R1,#1, Basic.s:876-877) is an array-element cache entry, and
 *     ARLOOKCACHE validates it.  A stale hit gives MISTAK ("Mistake"),
 *     Basic.s:870.
 *   - += and -= are lexed as '-' then '=', and the carry decides which
 *     it is (GOTLT2's TEQCC R10,#"+" / BCS MINUSBC, Basic.s:770-778).
 *     The -= path negates IACC only for integer type, but always
 *     negates FACC (RSBPL / FNEGD, Basic.s:754-767).  The negated
 *     garbage in FACC on the integer path is never read.
 *   - GOTLTEND1 (the CR end of an assignment) does the line break
 *     itself instead of calling CRLINE (Basic.s:730-741).  It skips
 *     the 3-byte line header blindly (LDRB R10,[LINE],#3) and tests
 *     for the &FF end marker.
 *   - The byte-type SWAP exchanges nothing at all.  SWAP1's byte loop
 *     runs R5 (TYPE) times with BHI, and type 0 (byte) fails BHI on
 *     the first SUBS (Stmt.s:1596-1601).  The original's comment
 *     ("swap 1,4,5/8 bytes for types 0,4,5/8") claims more than it
 *     does.
 *   - SWAP of two numerics of the same type below 128 uses the raw
 *     byte loop.  Strings and mixed types make the round trip through
 *     VARIND, PUSHTYPE and STOREA (SWAP2).  Arrays swap only their
 *     pointers (SWAPAR1).
 *   - DIM's subscript product loop RDLOOM multiplies by shifting the
 *     subscript and adding partial products.  Its overflow test is
 *     the carry from either the accumulating ADD or the doubling
 *     (Stmt.s:306-318).
 *   - DIM's LOCAL array claim pushes a TDIM (222) marker frame onto
 *     the stack and links it at LOCALARLIST.  It reuses the PROC frame
 *     words below the claimed block (Stmt.s:199-227).
 *   - Out-of-range assignments to LPAGE, LLOMEM and LHIMEM do NOT
 *     raise an error.  They print message 4, 5 or 6 through MSGPRNXXX
 *     and carry on (Stmt2.s:71, 165, 193).  LPAGE also accepts the
 *     whole ROM range above &1800000 without checking MEMLIMIT
 *     (LPAGEROM falls into LLOMEMROM, Stmt2.s:80-86).
 *   - LTIME= uses the length of the value to guess the OS_Word reason
 *     code.  A length of 8 or less means the time.  Otherwise the code
 *     is 15 if the length fits the date format length from
 *     Territory_ReadCalendarInformation, and 24 if it is longer.  The
 *     code is written into the byte BELOW
 *     STRACC (STRB R0,[R1,#-1]!, Stmt2.s:105-143).
 *   - ASSIGNAT (@%) accepts "+=" and "-=" on the integer word.  A
 *     string value is parsed as a format spec: a letter G, E or F, an
 *     optional '+' prefix (the STR$ flag), and two numbers read by
 *     READNUM (Stmt2.s:301-380).
 *   - The whole-array machinery's NEON/VFP vector loops are only a
 *     faster version of the element loops.  The vector paths write
 *     the same element results and set the same cumulative IOC, DZC
 *     and OFC flags.  (The lift models VADD..S32 as wrapping adds and
 *     FMACD as a multiply followed by an add, Array.s:26-139.)  So
 *     this unit implements only the scalar %FT60 loops.  The FPSCR
 *     bookkeeping is kept exactly: ros_vfp_ex2 or ros_vfp_div_ex for
 *     each element, then the flags&7 -> VFPException check after.
 *   - Matrix multiply copies the destination to the FSA when it
 *     aliases a source, "as a kindness".  It fails with ERMATMULSPACE
 *     if the copy would collide with SP (Array.s:1333-1355).  The FP
 *     inner loop is FMACD, a multiply then an add, so there are two
 *     roundings per term (Array.s:1487-1495).
 *   - The array += path (A() += B) re-enters the array op constant
 *     machinery, with the destination standing in as both operands
 *     (ARRAYPLUSBC, Array.s:720-724).  0-A() is built as a TINTEGER 0
 *     constant minus the array (ARRAYZEROMINUS, Array.s:1025-1038).
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "dispat_assign.h"

/* FMACD and the ops that look fused are modelled with two roundings,
 * as the lift expands them.  This stops the compiler contracting them
 * into one. */
#pragma STDC FP_CONTRACT OFF

/* ---- the lift's functions this unit calls --------------------------
 * (The manifest's EXPORTS list makes them non-static in the twin.  The
 * new ones this unit needs are listed in the report.) */
void basicvfp_DISPAT(struct ros_cpu *s);
void basicvfp_ERSYNT(struct ros_cpu *s);
void basicvfp_ERTYPEINT(struct ros_cpu *s);
void basicvfp_ERTYPESTR(struct ros_cpu *s);
void basicvfp_CLRSTK(struct ros_cpu *s);
void basicvfp_DOEXCEPTION(struct ros_cpu *s);
void basicvfp_EXPR(struct ros_cpu *s);
void basicvfp_AEEXPR(struct ros_cpu *s);
void basicvfp_FACTOR(struct ros_cpu *s);
void basicvfp_ARLOOKCACHE(struct ros_cpu *s);
void basicvfp_MISTAK(struct ros_cpu *s);
void basicvfp_MISSEQ(struct ros_cpu *s);
void basicvfp_ERCOMM(struct ros_cpu *s);
void basicvfp_ERBRA(struct ros_cpu *s);
void basicvfp_FACERR(struct ros_cpu *s);
void basicvfp_ERTYPENUM(struct ros_cpu *s);
void basicvfp_ERARRZ(struct ros_cpu *s);
void basicvfp_ERRSUB(struct ros_cpu *s);
void basicvfp_ERLONG(struct ros_cpu *s);
void basicvfp_VFPException(struct ros_cpu *s);
void basicvfp_ZDIVOR(struct ros_cpu *s);
void basicvfp_VARIND(struct ros_cpu *s);
void basicvfp_VARSTR(struct ros_cpu *s);
void basicvfp_VARNOTNUM(struct ros_cpu *s);
void basicvfp_STOREA(struct ros_cpu *s);
void basicvfp_STSTOR(struct ros_cpu *s);
void basicvfp_STSTORE(struct ros_cpu *s);
void basicvfp_STOREANINT(struct ros_cpu *s);
void basicvfp_ARRAYINTDIV(struct ros_cpu *s);
void basicvfp_INITIALISERAM(struct ros_cpu *s, uint32_t r0, uint32_t *p1,
                            uint32_t *p2, uint32_t *p3, uint32_t *p4,
                            uint32_t *p5, uint32_t r14);
void basicvfp_READNUM(struct ros_cpu *s, uint32_t *p0, uint32_t *p4,
                      uint32_t *p5, uint32_t *p6, uint32_t r14);
void basicvfp_EQAEEX(struct ros_cpu *s);
void basicvfp_AECHAN(struct ros_cpu *s);
void basicvfp_MSGPRNXXX(struct ros_cpu *s);
void basicvfp_POPLOCALAR(struct ros_cpu *s);
void basicvfp_CRAELV(struct ros_cpu *s);

/* The hand units' functions (landed). */
void basicvfp_hand_STMT(struct ros_cpu *s);
void basicvfp_hand_DATA(struct ros_cpu *s);
void basicvfp_hand_NXT(struct ros_cpu *s);
void basicvfp_hand_DONEXT(struct ros_cpu *s);
void basicvfp_hand_DONXTS(struct ros_cpu *s);
void basicvfp_hand_DONES(struct ros_cpu *s);
void basicvfp_hand_AEDONE(struct ros_cpu *s);
void basicvfp_hand_AEDONES(struct ros_cpu *s);
void basicvfp_hand_AESPAC(struct ros_cpu *s);
void basicvfp_hand_SPACES(struct ros_cpu *s, uint32_t *p10, uint32_t *p12,
                          uint32_t r14);
void basicvfp_hand_WORDCQ(struct ros_cpu *s);
void basicvfp_hand_INTEGY(struct ros_cpu *s);
void basicvfp_hand_INTEGB(struct ros_cpu *s);
void basicvfp_hand_FLOATY(struct ros_cpu *s);
void basicvfp_hand_PUSHTYPE(struct ros_cpu *s);
void basicvfp_hand_PULLTYPE(struct ros_cpu *s);
void basicvfp_hand_SPUSH(struct ros_cpu *s);
void basicvfp_hand_SPULL(struct ros_cpu *s);
void basicvfp_hand_SPUSHLARGE(struct ros_cpu *s);
void basicvfp_hand_LOOKUP(struct ros_cpu *s);
void basicvfp_hand_CREATE(struct ros_cpu *s);
void basicvfp_hand_GOTLTCREATE(struct ros_cpu *s);
void basicvfp_hand_AELV(struct ros_cpu *s);
void basicvfp_hand_LVCONT(struct ros_cpu *s);
void basicvfp_hand_LVNOTCACHE(struct ros_cpu *s);
void basicvfp_hand_LVBLNK(struct ros_cpu *s);
void basicvfp_hand_STORE(struct ros_cpu *s);
void basicvfp_hand_SETVAL(struct ros_cpu *s);
void basicvfp_hand_MSG(struct ros_cpu *s);

/* The OS thunks.  The harness's api_gen declares them, and each unit
 * declares them again, as the landed units do. */
void ros_thunk_OS_Word(struct ros_cpu *s);
void ros_thunk_OS_Args(struct ros_cpu *s);

/* Tokens, as the lift defines them from hdr/Tokens. */
#define TELSE   139u
#define TERROR  133u
#define TDATA   220u
#define TDIM    222u
#define TPROC   242u
#define TFN     164u
#define TLOCAL  234u

/* Type bits and the cache geometry (hdr/Definitions). */
#define TFP     0x80000000u
#define TFPLV   8u
#define TINTEGER 0x40000000u
#define CACHEMASK  255u
#define CACHESHIFT 4

/* OS_Word reasons used by LTIME (hdr/OS from the lift's constants). */
#define OsWord_WriteSystemClock   2u
#define OsWord_WriteRealTimeClock 15u

/* The real addresses of the MSG error sites.  MSG reads its error
 * number and token from the words after these.  They are the lift's
 * own BL sites inside DISPAT's error labels, one for each label this
 * unit reaches. */
#define SITE_ERTYPESTRING  0xFC10FF34u
#define SITE_ERTYPEARRAY   0xFC10FF44u
#define SITE_ERTYPEARRAYB  0xFC10FF4Cu
#define SITE_ERTYPEARRAYC  0xFC10FF54u
#define SITE_ERTYPESWAP    0xFC10FF64u
#define SITE_ERMATMULSPACE 0xFC10FF8Cu
#define SITE_BADDIMSUB     0xFC10FF94u
#define SITE_BADDIMLIST    0xFC10FF9Cu
#define SITE_BADDIM        0xFC10FFA4u
#define SITE_BADDIMSIGN    0xFC10FFACu
#define SITE_ERNDIM        0xFC10FFB4u
#define SITE_BADDIMSIZE    0xFC10FFBCu
#define SITE_DIMRAM        0xFC10FFC4u
#define SITE_ERRNLC        0xFC10FFE4u

/* The error exits.  These hand the block to MSG, which never returns.
 * The fault catches a MSG that does return. */
static void da_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* Forward declarations of the web's continuations.  They are defined
 * below, in the order the original's labels flow. */
static void da_arrayplusbc_entry(struct ros_cpu *s);
static void da_letarray(struct ros_cpu *s);
static void da_arraynegate(struct ros_cpu *s);
static void da_arrayfact(struct ros_cpu *s);
static void da_arrayfactlv(struct ros_cpu *s);
static void da_arrayfactrv(struct ros_cpu *s);
static void da_arraybinary(struct ros_cpu *s);
static void da_arraybinaryconst(struct ros_cpu *s);
static void da_arraybinaryconstlv(struct ros_cpu *s);
static void da_arraybinaryconst1(struct ros_cpu *s);
static void da_arraybinaryconst1_run(struct ros_cpu *s, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3,
                                     uint32_t r4, uint32_t r5, uint32_t r6,
                                     uint32_t r7, uint32_t r8, uint32_t r9,
                                     uint32_t r10, uint32_t r11,
                                     uint32_t r12, uint32_t r13,
                                     uint32_t r14);
static void da_arrayzerominus(struct ros_cpu *s);
static void da_arrayconstbinary(struct ros_cpu *s);
static void da_matrixmultiply(struct ros_cpu *s);

/* da_store_all/da_load_all follow the rig's own case shape.  Every
 * live local is stored across an internal label boundary, so the
 * continuation reads exactly the state that the original's branch
 * carried in registers. */
static void da_store_all(struct ros_cpu *s, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3, uint32_t r4, uint32_t r5,
                         uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r9,
                         uint32_t r10, uint32_t r11, uint32_t r12,
                         uint32_t r13, uint32_t r14)
{
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[8] = r8; s->r[9] = r9;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = r14;
}

/* =====================================================================
 * The GOTLT web: EXPRSTORESTMT, the +=/-= machinery, and the entries
 * that LET, LETST and LETSTNOTCACHE lead into (Basic.s:855-900).
 * ===================================================================== */

/* EXPRSTORESTMT (Basic.s:888-896), with the statement ends GOTLTEND1
 * (Basic.s:730-741) and GOTLTEND2 (Basic.s:717-729).  It evaluates the
 * right side, stores it through the stacked l-value, and continues the
 * statement loop.  The CR end does the line break itself, inline. */
static void da_exprstore(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0;                              /* BL EXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r12 = r11;                                 /* MOV LINE,AELINE */
    if (r10 != 58)                             /* CMP R10,#":" */
        goto GOTLTEND1;
    s->r[12] = r12;                            /* BL STORE */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B STMT */
    basicvfp_hand_STMT(s);
    return;

GOTLTEND1:
    if (r10 != 13)                             /* CMP R10,#13 */
        goto GOTLTEND2;
    s->r[12] = r12;                            /* BL STORE */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r10 = ros_ld8(r12);                        /* LDRB R10,[LINE],#3 */
    r12 += 3;
    if (r10 == 0xFFu) {                        /* CMP R10,#&FF */
        s->r[10] = r10;                        /* BEQ CLRSTK */
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_CLRSTK(s);
        return;
    }
    r4 = ros_ld32(r8 - 112);                   /* LDR R4,[ARGP,#ESCWORD] */
    ros_subs(s, r4, 0);                        /* CMP R4,#0 */
    if (r4 == 0) {                            /* BEQ STMT */
        s->r[4] = r4;
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_hand_STMT(s);
        return;
    }
    s->r[4] = r4;                              /* BL DOEXCEPTION */
    s->r[10] = r10;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_DOEXCEPTION(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B STMT */
    basicvfp_hand_STMT(s);
    return;

GOTLTEND2:
    ros_subs(s, r10, TELSE);                   /* CMP R10,#TELSE */
    if (r10 != TELSE) {                        /* BNE ERSYNT */
        s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    s->r[12] = r12;                            /* BL STORE */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B REM (= DATA) */
    basicvfp_hand_DATA(s);
    return;
}

/* ATGOTLT2 (Basic.s:773-778) and the PLUSBC family (Basic.s:779-853).
 * The character after "+" or "-" must be "=".  The += path evaluates
 * the right side, then adds it into the l-value cell.  An integer is
 * added through VARIND and STOREANINT, an FP value in place, and a
 * string by appending through STRACC.  The -= path negates the right
 * side first (MINUSBC). */
static void da_atgotlt2(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    double d0;
    uint32_t v;

    r10 = ros_ld8(r11);                        /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    ros_logic(s, r10 ^ 0x3Du, s->c);           /* TEQ R10,#"=" */
    if (!s->z) {                               /* BNE MISTAK */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5; s->r[9] = r9;
        s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_MISTAK(s);
        return;
    }
    if (s->c)                                  /* BCS MINUSBC */
        goto MINUSBC;
    s->r[0] = r0;                              /* BL EXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    goto PLUSBC;                               /* B PLUSBC */

MINUSBC:
    s->r[0] = r0;                              /* BL EXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_logic(s, r9, s->c);                    /* TEQ TYPE,#0 */
    if (r9 == 0) {                             /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    if ((int32_t)r9 >= 0)                      /* RSBPL IACC,IACC,#0 */
        r0 = -r0;
    s->fp->vfp.d[0] = -s->fp->vfp.d[0];        /* FNEGD FACC,FACC */
    goto PLUSBC;                               /* B PLUSBC */

PLUSBC:
    s->r[0] = r0;                              /* IACC out: MINUSBC has */
    s->r[14] = 0xFFFFFFF0u;                    /* negated its local, and */
    basicvfp_hand_AEDONE(s);                   /* AEDONE/INTEGY read R0 */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_subs(s, r5, TFPLV);                    /* CMP R5,#TFPLV */
    if (r5 == TFPLV)                           /* BEQ PLUSBCFP */
        goto PLUSBCFP;
    if (r5 >= TFPLV)                           /* BCS PLUSBCSTRING */
        goto PLUSBCSTRING;
    s->r[4] = r4;                              /* the popped l-value pair */
    s->r[5] = r5;                              /* must survive INTEGY and */
    s->r[13] = r13;                            /* VARIND in R[]: STOREANINT */
    s->r[14] = 0xFFFFFFF0u;                    /* reads the address from R4 */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r7 = r0;                                   /* MOV R7,IACC */
    r0 = r4;                                   /* MOV IACC,R4 */
    r9 = r5;                                   /* MOV TYPE,R5 */
    s->r[0] = r0;                              /* BL VARIND */
    s->r[7] = r7;
    s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARIND(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r0 += r7;                                  /* ADD IACC,IACC,R7 */
    s->r[0] = r0;                              /* BL STOREANINT */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREANINT(s);
    r1 = s->r[1]; r14 = s->r[14];
    da_store_all(s, s->r[0], r1, s->r[2], s->r[3], s->r[4], s->r[5],
                 s->r[6], s->r[7], s->r[8], s->r[9], s->r[10], s->r[11],
                 s->r[12], s->r[13], r14);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

PLUSBCSTRING:
    ros_subs(s, r5, 0x100u);                   /* CMP R5,#256 */
    if (r5 >= 0x100u) {                        /* BCS ARRAYPLUSBC */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayplusbc_entry(s);
        return;
    }
    ros_logic(s, r9, s->c);                    /* TEQ TYPE,#0 */
    if (r9 != 0) {                             /* BNE ERTYPESTR */
        s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    r0 = r8 - 1536;                            /* ADD R0,ARGP,#STRACC */
    r1 = r2 - r0;                              /* SUBS R1,CLEN,R0 */
    if (r1 == 0) {                             /* BEQ NXT: nothing to add */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    r7 = r1;                                   /* MOV R7,R1 */
    r11 = r4;                                  /* MOV AELINE,R4 */
    s->r[0] = r0;                              /* BL SPUSHLARGE */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPUSHLARGE(s);
    r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13];
    r0 = r11;                                  /* MOV IACC,AELINE */
    ros_subs(s, r5, 128);                      /* CMP R5,#128 */
    s->r[0] = r0;                              /* BL VARNOTNUM */
    s->r[5] = r5;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARNOTNUM(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r13 += 4;                                  /* ADD SP,SP,#4 */
    r6 = r7 + r2 - (r8 - 1536);                /* new length */
    ros_subs(s, r6, 0x100u);                   /* CMP R6,#256 */
    if (r6 >= 0x100u) {                        /* BCS ERLONG */
        s->r[6] = r6; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERLONG(s);
        return;
    }
PLUSBCLP:
    r6 = ros_ld8(r13);                         /* LDRB R6,[SP],#1 */
    r13 += 1;
    ros_st8(r2, r6);                           /* STRB R6,[CLEN],#1 */
    r2 += 1;
    r7 -= 1;                                   /* SUBS R7,R7,#1 */
    if (r7 != 0) goto PLUSBCLP;                /* BNE PLUSBCLP */
    r13 = (r13 + 3) & ~3u;                     /* ADD SP,SP,#3 ; BIC */
    r4 = r11;                                  /* MOV R4,AELINE */
    s->r[2] = r2;                              /* BL STSTOR ; TYPE still 0 */
    s->r[4] = r4;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STSTOR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

PLUSBCFP:
    s->r[4] = r4;                              /* BL FLOATY */
    s->r[5] = r5;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    d0 = ros_ldd(r4);                          /* FLDD D1,[R4] */
    {
        double sum = d0 + s->fp->vfp.d[0];     /* FADDD FACC,D1,FACC */
        s->fp->fpscr |= ros_vfp_ex2(sum, d0, s->fp->vfp.d[0]);
        s->fp->vfp.d[0] = sum;
    }
    v = s->fp->fpscr;                          /* FMRX R14,FPSCR */
    if ((v & 7) != 0) {                        /* TST IOC/DZC/OFC */
        s->r[14] = v;
        basicvfp_VFPException(s);
        return;
    }
    ros_std(r4, s->fp->vfp.d[0]);              /* FSTD FACC,[R4] */
    s->r[14] = v;
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, v);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* GOTLT2 (Basic.s:770-772): the token after the l-value must be "-",
 * or "+" only when the '-' compare borrowed. */
static void da_gotlt2(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10], r13 = s->r[13];
    uint32_t v1;

    ros_subs(s, r10, 45);                      /* CMP R10,#"-" */
    v1 = r10 ^ 0x2Bu;                          /* TEQCC R10,#"+" */
    if (r10 < 45)
        ros_logic(s, r10 ^ 0x2Bu, s->c);
    if (r10 < 45 ? v1 != 0 : r10 != 45) {      /* BNE MISTAK */
        s->r[0] = s->r[0]; s->r[1] = s->r[1]; s->r[4] = s->r[4];
        s->r[9] = s->r[9]; s->r[10] = r10; s->r[11] = s->r[11];
        s->r[12] = s->r[12]; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_MISTAK(s);
        return;
    }
    da_atgotlt2(s);                            /* ATGOTLT2 */
    return;
}

/* GOTLT/GOTLT1 (Basic.s:859-866).  The l-value pair goes on the
 * stack.  After '=', the type decides between the scalar store and
 * the whole-array machinery.  Anything other than '=' is += or -=. */
static void da_gotlt(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r13 = s->r[13];

    r13 -= 8;                                  /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
GOTLT1:
    r10 = ros_ld8(r11);                        /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == 32)                             /* CMP R10,#" " */
        goto GOTLT1;
    if (r10 != 61) {                           /* CMP R10,#"=" ; BNE GOTLT2 */
        da_store_all(s, r0, s->r[1], s->r[2], s->r[3], s->r[4], s->r[5],
                     s->r[6], s->r[7], s->r[8], r9, r10, r11, s->r[12],
                     r13, s->r[14]);
        da_gotlt2(s);
        return;
    }
    ros_subs(s, r9, 0x100u);                   /* CMP TYPE,#256 */
    if (r9 < 0x100u) {                         /* BCC EXPRSTORESTMT */
        s->r[0] = r0; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
        s->r[13] = r13;
        da_exprstore(s);
        return;
    }
    da_store_all(s, r0, s->r[1], s->r[2], s->r[3], s->r[4], s->r[5],
                 s->r[6], s->r[7], s->r[8], r9, r10, r11, s->r[12],
                 r13, s->r[14]);               /* B LETARRAY */
    da_letarray(s);
    return;
}

/* =====================================================================
 * The whole-array machinery (Array.s:417 onwards).  It is entered from
 * GOTLT1's array branch and from += on an array.  Only the scalar
 * element loops are translated.  The NEON/VFP vector blocks in the
 * original (and in the lift) are faster versions of these loops.  They
 * give identical results and accumulate the same flags, so no
 * expression can see the difference.
 * ===================================================================== */

/* da_vfpscr_check: the tail that the FP loops share.  It is FMRX
 * R14,FPSCR, then TST R14,#IOC+DZC+OFC and BNE VFPException, and
 * otherwise B NXT. */
static void da_vfpscr_check_nxt(struct ros_cpu *s)
{
    uint32_t v = s->fp->fpscr;                 /* FMRX R14,FPSCR */
    s->r[14] = v;
    if ((v & 7) != 0) {                        /* TST ...; BNE VFPException */
        basicvfp_VFPException(s);
        return;
    }
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);                      /* B NXT */
    return;
}

/* LETARRAY (Array.s:417-445) and the array-to-array copy
 * (ARRAYARRAYASSIGNSIZE/COPY/STRING, Array.s:447-489). */
static void da_letarray(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

LETARRAY:
    r10 = ros_ld8(r11);                        /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == 32)                             /* CMP R10,#" " */
        goto LETARRAY;
    if (r10 == 45) {                           /* CMP R10,#"-" ; BEQ ARRAYNEGATE */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arraynegate(s);
        return;
    }
    r13 -= 4;                                  /* STR AELINE,[SP,#-4]! */
    ros_st32(r13, r11);
    r0 = ros_subs(s, 57, r10);                 /* RSBS R0,R10,#"9" */
    if (57 >= r10)                             /* CMPCS R10,#"." */
        ros_subs(s, r10, 46);
    if (57 >= r10 && r10 >= 46) {              /* BCS ARRAYFACT */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayfact(s);
        return;
    }
    s->r[0] = r0;                              /* BL LVCONT */
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LVCONT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ ARRAYFACT */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayfact(s);
        return;
    }
    r13 += 4;                                  /* ADD SP,SP,#4 */
    if (r9 < 0x100u) {                         /* CMP TYPE,#256 ; BCC ARRAYFACTLV */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayfactlv(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    if (r10 == 43 || r10 == 45 || r10 == 42 || r10 == 47 || r10 == 46) {
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);           /* BEQ ARRAYBINARY */
        da_arraybinary(s);
        return;
    }
    s->r[13] = r13;                            /* BL AEDONE */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_subs(s, r9, r5);                       /* CMP TYPE,R5 */
    if (r9 != r5)                              /* BNE ERTYPEARRAYB */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    r6 = ros_ld32(r0);                         /* LDR R6,[IACC] */
    r4 = ros_ld32(r4);                         /* LDR R4,[R4] */
    ros_subs(s, r6, 16);                       /* CMP R6,#16 */
    if (r6 >= 16)                              /* CMPCS R4,#16 */
        ros_subs(s, r4, 16);
    if (r6 < 16 || r4 < 16) {                  /* BCC ERARRZ */
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERARRZ(s);
        return;
    }
ARRAYARRAYASSIGNSIZE:
    r5 = ros_ld32(r4);                         /* LDR R5,[R4],#4 */
    r4 += 4;
    r7 = ros_ld32(r6);                         /* LDR R7,[R6],#4 */
    r6 += 4;
    ros_subs(s, r5, r7);                       /* CMP R5,R7 */
    if (r5 != r7)                              /* BNE ERTYPEARRAYC */
        da_msg_at(s, SITE_ERTYPEARRAYC);
    if (r5 != 0)                               /* CMP R5,#0 */
        goto ARRAYARRAYASSIGNSIZE;
    r11 = ros_ld32(r4);                        /* LDR AELINE,[R4],#4 */
    r4 += 4;
    r6 += 4;                                   /* ADD R6,R6,#4 */
    r9 -= 0x100u;                              /* SUB TYPE,TYPE,#256 */
    if (r9 > TFPLV)                            /* CMP TYPE,#TFPLV ; BHI STRING */
        goto ARRAYARRAYASSIGNSTRING;
    r11 = r9 == TFPLV ? r11 << 3 : r11 << 2;   /* bytes to copy */
ARRAYARRAYASSIGNCOPY:
    v = r11;                                   /* SUBS AELINE,AELINE,#16 */
    r11 -= 16;
    if (v >= 16) {                             /* LDMHSIA R6!,{R0-R3} */
        r0 = ros_ld32(r6);
        r1 = ros_ld32(r6 + 4);
        r2 = ros_ld32(r6 + 8);
        r3 = ros_ld32(r6 + 12);
        r6 += 16;
    }
    if (v >= 16) {                             /* STMHSIA R4!,{R0-R3} */
        ros_st32(r4, r0);
        ros_st32(r4 + 4, r1);
        ros_st32(r4 + 8, r2);
        ros_st32(r4 + 12, r3);
        r4 += 16;
    }
    if (v > 16)                                /* BHI ARRAYARRAYASSIGNCOPY */
        goto ARRAYARRAYASSIGNCOPY;
    if (r11 == 0) {                            /* BEQ NXT */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[9] = r9;
        s->r[11] = r11; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    if (r11 < 0xFFFFFFF8u) {                   /* LDRLO/STRLO one word */
        r0 = ros_ld32(r6);
        r6 += 4;
        ros_st32(r4, r0);
        r4 += 4;
    }
    if (r11 == 0xFFFFFFF8u) {                  /* LDMEQIA/STMEQIA two */
        r0 = ros_ld32(r6);
        r1 = ros_ld32(r6 + 4);
        r6 += 8;
        ros_st32(r4, r0);
        ros_st32(r4 + 4, r1);
        r4 += 8;
    }
    if (r11 > 0xFFFFFFF8u) {                   /* LDMHIIA/STMHIIA three */
        r0 = ros_ld32(r6);
        r2 = ros_ld32(r6 + 4);
        r3 = ros_ld32(r6 + 8);
        r6 += 12;
        ros_st32(r4, r0);
        ros_st32(r4 + 4, r2);
        ros_st32(r4 + 8, r3);
        r4 += 12;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[9] = r9;
    s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYARRAYASSIGNSTRING:
    r9 = r6;                                   /* MOV TYPE,R6 ; source */
ARRAYARRAYASSIGNSTRING1:
    r6 = r9 & 3;                               /* ANDS R6,TYPE,#3 */
    r7 = r9 & ~3u;                             /* BIC R7,TYPE,#3 */
    if (r6 == 0)
        r3 = ros_ld32(r9);                     /* LDREQ R3,[TYPE] */
    if (r6 != 0) {                             /* LDMNEIA R7,{R3,R7} */
        r3 = ros_ld32(r7);
        r7 = ros_ld32(r7 + 4);
    }
    if (r6 != 0)                               /* the misaligned word glue */
        r3 = ros_lsr(r3, r6 << 3) | ros_lsl(r7, 32 - (r6 << 3));
    r9 += 5;                                   /* ADD TYPE,TYPE,#5 */
    r2 = ros_ld8(r9 - 1) + r3;                 /* LDRB R2 ; ADD R2,R2,R3 */
    s->r[2] = r2;                              /* BL STSTORE */
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[9] = r9;
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STSTORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r4 += 5;                                   /* ADD R4,R4,#5 */
    r11 -= 1;                                  /* SUBS AELINE,AELINE,#1 */
    if (r11 != 0)
        goto ARRAYARRAYASSIGNSTRING1;
    s->r[4] = r4; s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* ARRAYBINARY (Array.s:491-527) and the elementwise array op array
 * loops (Array.s:529-719).  These are ADD, SUB, MUL and DIV in their
 * integer, FP and string forms.  R4 is the destination base, IACC and
 * R1 are the operands, R3 is the operator character and AELINE is the
 * element count. */
static void da_arraybinary(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    double a, b, v;

    r13 -= 16;                                 /* STMFD {IACC,TYPE,R10} and */
    ros_st32(r13 + 4, r0);                     /* STR AELINE,[SP,#-4]!: one */
    ros_st32(r13 + 8, r9);                     /* 16-byte reservation, the */
    ros_st32(r13 + 12, r10);                   /* AELINE word at the bottom */
    ros_st32(r13, r11);
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r0 = ros_subs(s, 57, r10);                 /* RSBS R0,R10,#"9" */
    if (57 >= r10)                             /* CMPCS R10,#"." */
        ros_subs(s, r10, 46);
    if (57 >= r10 && r10 >= 46) {              /* BCS ARRAYBINARYCONST */
        s->r[0] = r0; s->r[13] = r13;
        da_arraybinaryconst(s);
        return;
    }
    s->r[0] = r0;                              /* BL LVCONT */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LVCONT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ ARRAYBINARYCONST */
        s->r[0] = r0; s->r[13] = r13;
        da_arraybinaryconst(s);
        return;
    }
    r13 += 4;                                  /* ADD SP,SP,#4 */
    s->r[13] = r13;                            /* BL AEDONES */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (r9 < 0x100u) {                         /* CMP TYPE,#256 ; BCC CONSTLV */
        s->r[0] = r0; s->r[13] = r13;
        da_arraybinaryconstlv(s);
        return;
    }
    r1 = ros_ld32(r13);                        /* LDMFD SP!,{R1,R2,R3,R4,R5} */
    r2 = ros_ld32(r13 + 4);
    r3 = ros_ld32(r13 + 8);
    r4 = ros_ld32(r13 + 12);
    r5 = ros_ld32(r13 + 16);
    r13 += 20;
    ros_subs(s, r9, r2);                       /* CMP TYPE,R2 */
    if (r9 == r2)                              /* CMPEQ R2,R5 */
        ros_subs(s, r2, r5);
    if (r9 != r2 || r2 != r5)                  /* BNE ERTYPEARRAYB */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    r0 = ros_ld32(r0);                         /* LDR IACC,[IACC] */
    r1 = ros_ld32(r1);                         /* LDR R1,[R1] */
    r4 = ros_ld32(r4);                         /* LDR R4,[R4] */
    ros_subs(s, r0, 16);                       /* CMP IACC,#16 */
    if (r0 >= 16)                              /* CMPCS R1,#16 */
        ros_subs(s, r1, 16);
    if (r0 >= 16 && r1 >= 16)
        ros_subs(s, r4, 16);
    if (r0 < 16 || r1 < 16 || r4 < 16) {       /* BCC ERARRZ */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERARRZ(s);
        return;
    }
    if (r3 == 46) {                            /* CMP R3,#"." ; BEQ MATRIX */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_matrixmultiply(s);
        return;
    }
ARRAYBINARYSIZE:
    r2 = ros_ld32(r0);                         /* LDR R2,[IACC],#4 */
    r0 += 4;
    r5 = ros_ld32(r1);                         /* LDR R5,[R1],#4 */
    r1 += 4;
    r6 = ros_ld32(r4);                         /* LDR R6,[R4],#4 */
    r4 += 4;
    ros_subs(s, r2, r5);                       /* CMP R2,R5 */
    if (r2 == r5)                              /* CMPEQ R5,R6 */
        ros_subs(s, r5, r6);
    if (r2 != r5 || r5 != r6)                  /* BNE ERTYPEARRAYC */
        da_msg_at(s, SITE_ERTYPEARRAYC);
    if (r6 != 0)                               /* CMP R6,#0 */
        goto ARRAYBINARYSIZE;
    r11 = ros_ld32(r4);                        /* LDR AELINE,[R4],#4 */
    r4 += 4;
    r0 += 4;                                   /* ADD IACC,IACC,#4 */
    r1 += 4;                                   /* ADD R1,R1,#4 */
    r9 -= 0x100u;                              /* SUB TYPE,TYPE,#256 */
    if (r3 == 45)                              /* CMP R3,#"-" */
        goto ARRAYBINARYSUB;
    if (r3 == 42)                              /* CMP R3,#"*" */
        goto ARRAYBINARYMUL;
    if (r3 == 47)                              /* CMP R3,#"/" */
        goto ARRAYBINARYDIV;
    ros_subs(s, r9, TFPLV);                    /* CMP TYPE,#TFPLV */
    if (r9 == TFPLV)
        goto ARRAYBINARYADDFP;
    if (r9 >= TFPLV)
        goto ARRAYBINARYADDSTRING;
    /* IntVectorVectorOp ADD,R4,IACC,R1: the scalar %FT60 loop */
    for (; r11 != 0; r11--) {                  /* Array.s:66-71 */
        r3 = ros_ld32(r0);
        r0 += 4;
        r2 = r3 + ros_ld32(r1);
        r1 += 4;
        ros_st32(r4, r2);
        r4 += 4;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[9] = r9; s->r[11] = r11; s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYBINARYADDFP:                              /* VFPVectorVectorOp FADDD */
    for (; r11 != 0; r11--) {                  /* Array.s:549-555 */
        a = ros_ldd(r0);
        r0 += 8;
        b = ros_ldd(r1);
        r1 += 8;
        v = a + b;
        s->fp->fpscr |= ros_vfp_ex2(v, a, b);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;

ARRAYBINARYADDSTRING:                          /* Array.s:557-589 */
    r13 -= 8;                                  /* STMFD SP!,{R0,R10} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r10);
    r10 = r1;                                  /* MOV R10,R1 */
ARRAYBINARYADDSTRING1:
    r0 = r10;                                  /* MOV IACC,R10 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;                    /* BL VARSTR */
    basicvfp_VARSTR(s);
    r2 = s->r[2];
    r9 = s->r[9];
    r14 = s->r[14];
    r10 += 5;                                  /* ADD R10,R10,#5 */
    r0 = ros_ld32(r13);                        /* LDR IACC,[SP] */
    r1 = r0 + 5;                               /* ADD R1,IACC,#5 */
    ros_st32(r13, r1);                         /* STR R1,[SP] */
    r5 = ros_ld8(r0 + 4);                      /* LDRB R5,[IACC,#4] */
    if (r5 == 0)                               /* TEQ R5,#0 */
        goto ARRAYBINARYADDSTRING3;
    r1 = r0 & 3;                               /* ANDS R1,IACC,#3 */
    r3 = r0 & ~3u;                             /* BIC R3,IACC,#3 */
    if (r1 == 0)
        r0 = ros_ld32(r0);
    if (r1 != 0) {
        r0 = ros_ld32(r3);
        r3 = ros_ld32(r3 + 4);
    }
    if (r1 != 0)                               /* the misaligned glue */
        r0 = ros_lsr(r0, r1 << 3) | ros_lsl(r3, 32 - (r1 << 3));
    r6 = r2 + r5;                              /* ADD R6,CLEN,R5 */
    r1 = r8 - 1280;                            /* ADD R1,ARGP,#STRACC+256 */
    ros_subs(s, r6, r1);                       /* CMP R6,R1 */
    if (r6 >= r1) {                            /* BCS ERLONG */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
        s->r[6] = r6; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERLONG(s);
        return;
    }
ARRAYADDBINARYSTRING2:
    r1 = ros_ld8(r0);                          /* LDRB R1,[IACC],#1 */
    r0 += 1;
    ros_st8(r2, r1);                           /* STRB R1,[CLEN],#1 */
    r2 += 1;
    r5 -= 1;                                   /* SUBS R5,R5,#1 */
    if (r5 != 0)
        goto ARRAYADDBINARYSTRING2;
ARRAYBINARYADDSTRING3:
    r3 = r8 - 1536;                            /* ADD R3,ARGP,#STRACC */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                    /* BL STSTORE */
    basicvfp_STSTORE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 += 5;                                   /* ADD R4,R4,#5 */
    r11 -= 1;                                  /* SUBS AELINE,AELINE,#1 */
    if (r11 != 0)
        goto ARRAYBINARYADDSTRING1;
    r0 = ros_ld32(r13);                        /* LDMFD SP!,{R0,R10} */
    r10 = ros_ld32(r13 + 4);
    r13 += 8;
    s->r[0] = r0; s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYBINARYSUB:
    ros_subs(s, r9, TFPLV);                    /* CMP TYPE,#TFPLV */
    if (r9 == TFPLV)
        goto ARRAYBINARYSUBFP;
    if (r9 >= TFPLV)                           /* BCS ERTYPEARRAYB */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    /* IntVectorVectorOp SUB,R4,R1,IACC */
    for (; r11 != 0; r11--) {                  /* Array.s:598-603 */
        r3 = ros_ld32(r1);
        r1 += 4;
        r2 = r3 - ros_ld32(r0);
        r0 += 4;
        ros_st32(r4, r2);
        r4 += 4;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[9] = r9; s->r[11] = r11; s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYBINARYSUBFP:                              /* VFPVectorVectorOp FSUBD */
    for (; r11 != 0; r11--) {
        a = ros_ldd(r1);
        r1 += 8;
        b = ros_ldd(r0);
        r0 += 8;
        v = a - b;
        s->fp->fpscr |= ros_vfp_ex2(v, a, b);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;

ARRAYBINARYMUL:
    ros_subs(s, r9, TFPLV);                    /* CMP TYPE,#TFPLV */
    if (r9 == TFPLV)
        goto ARRAYBINARYMULFP;
    if (r9 >= TFPLV)
        da_msg_at(s, SITE_ERTYPEARRAYB);
    /* IntVectorVectorOp MUL,R4,R1,IACC */
    for (; r11 != 0; r11--) {
        r3 = ros_ld32(r1);
        r1 += 4;
        r2 = r3 * ros_ld32(r0);
        r0 += 4;
        ros_st32(r4, r2);
        r4 += 4;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[9] = r9; s->r[11] = r11; s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYBINARYMULFP:                              /* VFPVectorVectorOp FMULD */
    for (; r11 != 0; r11--) {
        a = ros_ldd(r0);
        r0 += 8;
        b = ros_ldd(r1);
        r1 += 8;
        v = a * b;
        s->fp->fpscr |= ros_vfp_ex2(v, a, b);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;

ARRAYBINARYDIV:
    ros_subs(s, r9, TFPLV);                    /* CMP TYPE,#TFPLV */
    if (r9 == TFPLV)
        goto ARRAYBINARYDIVFP;
    if (r9 >= TFPLV)
        da_msg_at(s, SITE_ERTYPEARRAYB);
ARRAYBINARYDIVINT:                             /* Array.s:668-675 */
    for (; r11 != 0; r11--) {
        r3 = ros_ld32(r0);
        r0 += 4;
        r2 = ros_ld32(r1);
        r1 += 4;
        s->r[2] = r2;                          /* BL ARRAYINTDIV */
        s->r[3] = r3;
        s->r[4] = r4;
        s->r[6] = r6;
        s->r[11] = r11;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_ARRAYINTDIV(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
        r14 = s->r[14];
        ros_st32(r4, r9);                      /* STR TYPE,[R4],#4 */
        r4 += 4;
    }
    s->r[4] = r4; s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYBINARYDIVFP:                              /* VFPVectorVectorOp FDIVD */
    for (; r11 != 0; r11--) {
        a = ros_ldd(r1);                       /* the dividend is the FIRST */
        r1 += 8;                               /* operand (R1). IACC holds */
        b = ros_ldd(r0);                       /* the second: FDIVD D,D0,D4 */
        r0 += 8;
        v = a / b;
        s->fp->fpscr |= ros_vfp_div_ex(v, a, b);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;
}

/* ARRAYBINARYCONST (Array.s:726-769): array op constant, with the
 * constant on the right (A() = B() op C%).  It includes the -=
 * negation quirk and the string array append (CONST2). */
static void da_arraybinaryconst(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r11 = ros_ld32(r13);                       /* LDR AELINE,[SP],#4 */
    r13 += 4;
    s->r[0] = r0;                              /* BL FACTOR */
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_FACTOR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[0] = r0;                              /* BL AEDONES */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
ARRAYBINARYCONSTRV:
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5,R6,R7,AELINE} */
    r5 = ros_ld32(r13 + 4);
    r6 = ros_ld32(r13 + 8);
    r7 = ros_ld32(r13 + 12);
    r11 = ros_ld32(r13 + 16);
    r13 += 20;
    ros_subs(s, r6, 45);                       /* CMP R6,#"-" */
    if (r6 != 45)
        goto ARRAYBINARYCONST1;
    ros_logic(s, r9, s->c);                    /* TEQ TYPE,#0 */
    if (r9 == 0)                               /* BEQ ERTYPEARRAYB */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    if ((int32_t)r9 >= 0)                      /* RSBPL IACC,IACC,#0 */
        r0 = -r0;
    s->fp->vfp.d[0] = -s->fp->vfp.d[0];        /* FNEGD FACC,FACC */
    r6 = 43;                                   /* MOV R6,#"+" */
    /* fall through */
ARRAYBINARYCONST1:
    da_arraybinaryconst1_run(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9,
                             r10, r11, r12, r13, r14);
    return;
}

/* The shared tail of the constant paths.  It is entered from CONSTRV
 * above, from ARRAYPLUSBC (the += on an array), and from
 * ARRAYBINARYCONSTLV.  State arrives in R[]. */
static void da_arraybinaryconst1(struct ros_cpu *s)
{
    da_arraybinaryconst1_run(s, s->r[0], s->r[1], s->r[2], s->r[3],
                             s->r[4], s->r[5], s->r[6], s->r[7], s->r[8],
                             s->r[9], s->r[10], s->r[11], s->r[12],
                             s->r[13], s->r[14]);
}

static void da_arraybinaryconst1_run(struct ros_cpu *s, uint32_t r0,
                                     uint32_t r1, uint32_t r2, uint32_t r3,
                                     uint32_t r4, uint32_t r5, uint32_t r6,
                                     uint32_t r7, uint32_t r8, uint32_t r9,
                                     uint32_t r10, uint32_t r11,
                                     uint32_t r12, uint32_t r13,
                                     uint32_t r14)
{
    double a, v;

    if (r6 == 43 && r5 == 384)                 /* CMP R6,#"+" ; CMPEQ R5,#384 */
        goto ARRAYBINARYCONST2;
    ros_subs(s, r9, 0);                        /* CMP TYPE,#0 */
    if (r9 == 0) {                             /* BEQ ERTYPEINT */
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
        s->r[11] = r11; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    s->r[0] = r0;                              /* BL PUSHTYPE */
    s->r[4] = r4;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PUSHTYPE(s);
    r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13]; r14 = s->r[14];
    r13 -= 4;                                  /* STR TYPE,[SP,#-4]! */
    ros_st32(r13, r9);
    r0 = r4;                                   /* MOV IACC,R4 */
    r9 = r5;                                   /* MOV TYPE,R5 */
    r4 = r7;                                   /* MOV R4,R7 */
    r5 = r11;                                  /* MOV R5,AELINE */
    r7 = r6;                                   /* MOV R7,R6 */
    r1 = r9 - 0x100u;                          /* SUB R1,TYPE,#256 */
    if (r6 != 47)                              /* CMP R7,#"/" */
        goto ARRAYCONSTBINARY1;
    ros_subs(s, r5, r9);                       /* CMP R5,TYPE */
    if (r5 != r9)                              /* BNE ERTYPEARRAYB */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    r6 = ros_ld32(r0);                         /* LDR R6,[IACC] */
    r4 = ros_ld32(r4);                         /* LDR R4,[R4] */
    ros_subs(s, r6, 16);                       /* CMP R6,#16 */
    if (r6 >= 16)
        ros_subs(s, r4, 16);
    if (r6 < 16 || r4 < 16) {                  /* BCC ERARRZ */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[7] = r7; s->r[9] = r9; s->r[11] = r11; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERARRZ(s);
        return;
    }
ARRAYBINARYCONSTDIV1:
    r5 = ros_ld32(r4);                         /* LDR R5,[R4],#4 */
    r4 += 4;
    r2 = ros_ld32(r6);                         /* LDR R2,[R6],#4 */
    r6 += 4;
    ros_subs(s, r5, r2);                       /* CMP R5,R2 */
    if (r5 != r2)
        da_msg_at(s, SITE_ERTYPEARRAYC);
    if (r5 != 0)
        goto ARRAYBINARYCONSTDIV1;
    r11 = ros_ld32(r4);                        /* LDR AELINE,[R4],#4 */
    r4 += 4;
    r6 += 4;
    ros_subs(s, r1, TFPLV);                    /* CMP R1,#TFPLV */
    if (r1 > TFPLV)                            /* BHI ERTYPEARRAY */
        da_msg_at(s, SITE_ERTYPEARRAY);
    if (r1 == TFPLV)
        goto ARRAYBINARYCONSTDIVFP;
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL INTEGY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
ARRAYBINARYCONSTDIVINT:                        /* Array.s:803-810 */
    for (; r11 != 0; r11--) {
        r2 = ros_ld32(r6);
        r6 += 4;
        r3 = r0;                               /* MOV R3,R0 */
        s->r[2] = r2;                          /* BL ARRAYINTDIV */
        s->r[3] = r3;
        s->r[4] = r4;
        s->r[6] = r6;
        s->r[11] = r11;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_ARRAYINTDIV(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
        r14 = s->r[14];
        ros_st32(r4, r9);                      /* STR TYPE,[R4],#4 */
        r4 += 4;
    }
    s->r[4] = r4; s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYBINARYCONSTDIVFP:                         /* Array.s:812-851 */
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL FLOATY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 8;                                  /* STMFD SP!,{R6,R10} */
    ros_st32(r13, r6);
    ros_st32(r13 + 4, r10);
    r10 = r4;                                  /* MOV R10,R4 */
    s->fp->fpscr |= ros_vfp_cmp_ex(s->fp->vfp.d[0], 0.0, 0); /* FCMPZD */
    ros_vfp_cmp(s, s->fp->vfp.d[0], 0.0);
    ros_vmrs_flags(s);
    if (s->fp->vfp.d[0] == 0.0) {              /* BEQ ZDIVOR */
        s->r[10] = r10; s->r[13] = r13;
        basicvfp_ZDIVOR(s);
        return;
    }
    for (; r11 != 0; r11--) {                  /* the scalar %FT60 loop */
        a = ros_ldd(r6);
        r6 += 8;
        v = a / s->fp->vfp.d[0];               /* FDIVD D4,D4,FACC */
        s->fp->fpscr |= ros_vfp_div_ex(v, a, s->fp->vfp.d[0]);
        ros_std(r10, v);
        r10 += 8;
    }
    r6 = ros_ld32(r13);                        /* LDMFD SP!,{R6,R10} */
    r10 = ros_ld32(r13 + 4);
    r13 += 8;
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;

ARRAYBINARYCONST2:                             /* Array.s:853-894 */
    if (r5 != r11)                             /* CMP R5,AELINE */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    ros_subs(s, r9, 0);                        /* CMP TYPE,#0 */
    if (r9 != 0) {                             /* BNE ERTYPESTR */
        s->r[4] = r4; s->r[11] = r11;
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    r6 = ros_ld32(r4);                         /* LDR R6,[R4] */
    s->r[0] = r0;                              /* BL SPUSH */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPUSH(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13];
    r14 = s->r[14];
    r4 = ros_ld32(r7);                         /* LDR R4,[R7] */
    ros_subs(s, r4, 16);                       /* CMP R4,#16 */
    if (r4 >= 16)
        ros_subs(s, r6, 16);
    if (r4 < 16 || r6 < 16) {                  /* BCC ERARRZ */
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERARRZ(s);
        return;
    }
ARRAYBINARYCONSTSIZE:
    r5 = ros_ld32(r4);
    r4 += 4;
    r2 = ros_ld32(r6);
    r6 += 4;
    ros_subs(s, r5, r2);
    if (r5 != r2)
        da_msg_at(s, SITE_ERTYPEARRAYC);
    if (r5 != 0)
        goto ARRAYBINARYCONSTSIZE;
    r11 = ros_ld32(r4);                        /* LDR AELINE,[R4],#4 */
    r4 += 4;
    r6 += 4;
    r13 -= 4;                                  /* STR R10,[SP,#-4]! */
    ros_st32(r13, r10);
    r10 = r6;                                  /* MOV R10,R6 */
ARRAYBINARYCONSTADDSTRING1:
    r0 = r10;                                  /* MOV IACC,R10 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;                    /* BL VARSTR */
    basicvfp_VARSTR(s);
    r2 = s->r[2]; r9 = s->r[9]; r14 = s->r[14];
    r3 = r8 - 1536;                            /* ADD R3,ARGP,#STRACC */
    r5 = ros_ld32(r13 + 4);                    /* LDR R5,[SP,#4] */
    r1 = r5 - r3;                              /* SUBS R1,R5,R3 */
    if (r1 == 0)
        goto ARRAYBINARYCONSTADDSTRING3;
    r6 = r1 + r2;                              /* ADD R6,R1,CLEN */
    r5 = r8 - 1280;                            /* ADD R5,ARGP,#STRACC+256 */
    ros_subs(s, r6, r5);                       /* CMP R6,R5 */
    if (r6 >= r5) {                            /* BCS ERLONG */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
        s->r[6] = r6; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERLONG(s);
        return;
    }
    r6 = r13 + 8;                              /* ADD R6,SP,#8 */
ARRAYBINARYCONSTADDSTRING2:
    r5 = ros_ld8(r6);                          /* LDRB R5,[R6],#1 */
    r6 += 1;
    ros_st8(r2, r5);                           /* STRB R5,[CLEN],#1 */
    r2 += 1;
    r1 -= 1;                                   /* SUBS R1,R1,#1 */
    if (r1 != 0)
        goto ARRAYBINARYCONSTADDSTRING2;
ARRAYBINARYCONSTADDSTRING3:
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                    /* BL STSTORE */
    basicvfp_STSTORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r4 += 5;                                   /* ADD R4,R4,#5 */
    r10 += 5;                                  /* ADD R10,R10,#5 */
    r11 -= 1;                                  /* SUBS AELINE,AELINE,#1 */
    if (r11 != 0)
        goto ARRAYBINARYCONSTADDSTRING1;
    r10 = ros_ld32(r13);                       /* LDR R10,[SP],#4 */
    r13 += 4;
    s->r[0] = r0; s->r[13] = r13;              /* BL SPULL */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPULL(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r13 = s->r[13]; r14 = s->r[14];
    s->r[4] = r4; s->r[10] = r10; s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTBINARY1:                            /* A() = B() op C% tail */
    if (r5 != r9)                              /* CMP R5,TYPE */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    r6 = ros_ld32(r0);                         /* LDR R6,[IACC] */
    r4 = ros_ld32(r4);                         /* LDR R4,[R4] */
    ros_subs(s, r6, 16);                       /* CMP R6,#16 */
    if (r6 >= 16)
        ros_subs(s, r4, 16);
    if (r6 < 16 || r4 < 16) {                  /* BCC ERARRZ */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[9] = r9; s->r[11] = r11;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERARRZ(s);
        return;
    }
ARRAYCONSTBINARYSIZE:
    r5 = ros_ld32(r4);
    r4 += 4;
    r2 = ros_ld32(r6);
    r6 += 4;
    ros_subs(s, r5, r2);
    if (r5 != r2)
        da_msg_at(s, SITE_ERTYPEARRAYC);
    if (r5 != 0)
        goto ARRAYCONSTBINARYSIZE;
    r11 = ros_ld32(r4);                        /* LDR AELINE,[R4],#4 */
    r4 += 4;
    r6 += 4;
    if (r7 == 45)                              /* CMP R7,#"-" */
        goto ARRAYCONSTBINARYMINUS;
    if (r7 == 42)                              /* CMP R7,#"*" */
        goto ARRAYCONSTBINARYMUL;
    if (r7 == 47)                              /* CMP R7,#"/" */
        goto ARRAYCONSTBINARYDIV;
    ros_subs(s, r7, 46);                       /* CMP R7,#"." */
    if (r7 == 46)                              /* BEQ ERTYPEARRAY */
        da_msg_at(s, SITE_ERTYPEARRAY);
    ros_subs(s, r1, TFPLV);                    /* CMP R1,#TFPLV */
    if (r1 > TFPLV)
        goto ARRAYCONSTBINARYADDSTRING;
    if (r1 == TFPLV)
        goto ARRAYCONSTBINARYADDFP;
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL INTEGY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    for (; r11 != 0; r11--) {                  /* IntVectorScalarOp ADD */
        r2 = ros_ld32(r6);
        r6 += 4;
        r2 = r2 + r0;
        ros_st32(r4, r2);
        r4 += 4;
    }
    s->r[2] = r2; s->r[4] = r4; s->r[6] = r6; s->r[11] = r11;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTBINARYADDFP:                         /* VFPVectorScalarOp FADDD */
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL FLOATY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    for (; r11 != 0; r11--) {
        a = ros_ldd(r6);
        r6 += 8;
        v = a + s->fp->vfp.d[0];
        s->fp->fpscr |= ros_vfp_ex2(v, a, s->fp->vfp.d[0]);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;

ARRAYCONSTBINARYADDSTRING:                     /* Array.s:1120-1148 */
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    r13 -= 8;                                  /* STMFD SP!,{CLEN,R10} */
    ros_st32(r13, r2);
    ros_st32(r13 + 4, r10);
    r3 = r8 - 1536;                            /* ADD R3,ARGP,#STRACC */
    r10 = r6;                                  /* MOV R10,R6 */
ARRAYCONSTBINARYADDSTRING1:
    r2 = ros_ld32(r13);                        /* LDR CLEN,[SP] */
    r5 = ros_ld8(r10 + 4);                     /* LDRB R5,[R10,#4] */
    if (r5 == 0)                               /* TEQ R5,#0 */
        goto ARRAYCONSTBINARYADDSTRING3;
    r6 = r10 & 3;                              /* ANDS R6,R10,#3 */
    r7 = r10 & ~3u;                            /* BIC R7,R10,#3 */
    if (r6 == 0)
        r0 = ros_ld32(r10);
    if (r6 != 0) {
        r0 = ros_ld32(r7);
        r7 = ros_ld32(r7 + 4);
    }
    if (r6 != 0)                               /* the misaligned glue */
        r0 = ros_lsr(r0, r6 << 3) | ros_lsl(r7, 32 - (r6 << 3));
    r6 = r2 + r5;                              /* ADD R6,CLEN,R5 */
    r1 = r8 - 1280;                            /* ADD R1,ARGP,#STRACC+256 */
    ros_subs(s, r6, r1);                       /* CMP R6,R1 */
    if (r6 >= r1) {                            /* BCS ERLONG */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
        s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[10] = r10;
        s->r[11] = r11; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERLONG(s);
        return;
    }
ARRAYCONSTBINARYADDSTRING2:
    r1 = ros_ld8(r0);                          /* LDRB R1,[R0],#1 */
    r0 += 1;
    ros_st8(r2, r1);                           /* STRB R1,[CLEN],#1 */
    r2 += 1;
    r5 -= 1;                                   /* SUBS R5,R5,#1 */
    if (r5 != 0)
        goto ARRAYCONSTBINARYADDSTRING2;
ARRAYCONSTBINARYADDSTRING3:
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                    /* BL STSTORE */
    basicvfp_STSTORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r4 += 5;                                   /* ADD R4,R4,#5 */
    r10 += 5;                                  /* ADD R10,R10,#5 */
    r11 -= 1;                                  /* SUBS AELINE,AELINE,#1 */
    if (r11 != 0)
        goto ARRAYCONSTBINARYADDSTRING1;
    r2 = ros_ld32(r13);                        /* LDMFD SP!,{CLEN,R10} */
    r10 = ros_ld32(r13 + 4);
    r13 += 8;
    s->r[2] = r2; s->r[4] = r4; s->r[10] = r10; s->r[11] = r11;
    s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTBINARYMINUS:
    ros_subs(s, r1, TFPLV);                    /* CMP R1,#TFPLV */
    if (r1 > TFPLV)                            /* BHI ERTYPEARRAY */
        da_msg_at(s, SITE_ERTYPEARRAY);
    if (r1 == TFPLV)
        goto ARRAYCONSTBINARYMINUSFP;
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL INTEGY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    for (; r11 != 0; r11--) {                  /* IntScalarVectorOp SUB */
        r2 = ros_ld32(r6);
        r6 += 4;
        r2 = r0 - r2;
        ros_st32(r4, r2);
        r4 += 4;
    }
    s->r[2] = r2; s->r[4] = r4; s->r[6] = r6; s->r[11] = r11;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTBINARYMINUSFP:                       /* VFPScalarVectorOp FSUBD */
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL FLOATY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    for (; r11 != 0; r11--) {
        a = ros_ldd(r6);
        r6 += 8;
        v = s->fp->vfp.d[0] - a;               /* FSUBD D4,FACC,D4 */
        s->fp->fpscr |= ros_vfp_ex2(v, s->fp->vfp.d[0], a);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;

ARRAYCONSTBINARYMUL:
    ros_subs(s, r1, TFPLV);                    /* CMP R1,#TFPLV */
    if (r1 > TFPLV)
        da_msg_at(s, SITE_ERTYPEARRAY);
    if (r1 == TFPLV)
        goto ARRAYCONSTBINARYMULFP;
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL INTEGY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    for (; r11 != 0; r11--) {                  /* IntVectorScalarOp MUL */
        r2 = ros_ld32(r6);
        r6 += 4;
        r2 = r2 * r0;
        ros_st32(r4, r2);
        r4 += 4;
    }
    s->r[2] = r2; s->r[4] = r4; s->r[6] = r6; s->r[11] = r11;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTBINARYMULFP:                         /* VFPVectorScalarOp FMULD */
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL FLOATY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    for (; r11 != 0; r11--) {
        a = ros_ldd(r6);
        r6 += 8;
        v = a * s->fp->vfp.d[0];
        s->fp->fpscr |= ros_vfp_ex2(v, a, s->fp->vfp.d[0]);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;

ARRAYCONSTBINARYDIV:
    ros_subs(s, r1, TFPLV);                    /* CMP R1,#TFPLV */
    if (r1 > TFPLV)
        da_msg_at(s, SITE_ERTYPEARRAY);
    if (r1 == TFPLV)
        goto ARRAYCONSTBINARYDIVFP;
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL INTEGY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
ARRAYCONSTBINARYDIVINT:                        /* Array.s:1256-1264 */
    for (; r11 != 0; r11--) {
        r3 = ros_ld32(r6);
        r6 += 4;
        r2 = r0;                               /* MOV R2,R0 */
        s->r[2] = r2;                          /* BL ARRAYINTDIV */
        s->r[3] = r3;
        s->r[4] = r4;
        s->r[6] = r6;
        s->r[11] = r11;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_ARRAYINTDIV(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
        r14 = s->r[14];
        ros_st32(r4, r9);                      /* STR TYPE,[R4],#4 */
        r4 += 4;
    }
    s->r[4] = r4; s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTBINARYDIVFP:                         /* VFPScalarVectorOp FDIVD */
    s->r[0] = r0;                              /* BL PULLTYPE */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = s->r[13];
    s->r[4] = r4;                              /* BL FLOATY */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLOATY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    for (; r11 != 0; r11--) {
        a = ros_ldd(r6);
        r6 += 8;
        v = s->fp->vfp.d[0] / a;               /* FDIVD D4,FACC,D4 */
        s->fp->fpscr |= ros_vfp_div_ex(v, s->fp->vfp.d[0], a);
        ros_std(r4, v);
        r4 += 8;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_vfpscr_check_nxt(s);
    return;
}

/* ARRAYBINARYCONSTLV (Array.s:720-722): the right side was a scalar
 * l-value.  VARIND reads its value, then the constant tail is taken. */
static void da_arraybinaryconstlv(struct ros_cpu *s)
{
    s->r[14] = 0xFFFFFFF0u;                    /* BL VARIND */
    basicvfp_VARIND(s);
    da_arraybinaryconst(s);                    /* B ARRAYBINARYCONSTRV path */
    return;
}

/* ARRAYPLUSBC (Array.s:718-724): A() += B re-enters the constant
 * machinery, with the destination as the source and "+" as the
 * operator. */
static void da_arrayplusbc_entry(struct ros_cpu *s)
{
    uint32_t r4 = s->r[4], r5 = s->r[5];

    s->r[6] = 43;                              /* MOV R6,#"+" */
    s->r[7] = r4;                              /* MOV R7,R4 */
    s->r[11] = r5;                             /* MOV AELINE,R5 */
    da_arraybinaryconst1(s);                   /* B ARRAYBINARYCONST1 */
    return;
}

/* ARRAYNEGATE (Array.s:896-931).  For an array, -A() is done as 0-A()
 * through the constant machinery.  A scalar l-value is simply
 * negated. */
static void da_arraynegate(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                  /* STR AELINE,[SP,#-4]! */
    ros_st32(r13, r11);
    s->r[10] = r10;                            /* BL AESPAC */
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    r0 = ros_subs(s, 57, r10);                 /* RSBS R0,R10,#"9" */
    if (57 >= r10)                             /* CMPCS R10,#"." */
        ros_subs(s, r10, 46);
    if (57 >= r10 && r10 >= 46) {              /* BCS ARRAYFACT */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayfact(s);
        return;
    }
    s->r[0] = r0;                              /* BL LVCONT */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LVCONT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ ARRAYFACT */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayfact(s);
        return;
    }
    r13 += 4;                                  /* ADD SP,SP,#4 */
    r1 = r9 - 0x100u;                          /* SUBS R1,TYPE,#256 */
    if (r9 >= 0x100u) {                        /* BCS ARRAYZEROMINUS */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayzerominus(s);
        return;
    }
    s->r[1] = r1;                              /* BL VARIND */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARIND(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                    /* TEQ TYPE,#0 */
    if (s->z) {                                /* BEQ ERTYPEINT */
        s->r[15] = s->r[14];
        basicvfp_ERTYPEINT(s);
        return;
    }
    if (!s->n)                                 /* RSBPL IACC,IACC,#0 */
        r0 = -r0;
    if (!s->n) {                               /* BPL ARRAYFACTRV */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_arrayfactrv(s);
        return;
    }
    s->fp->vfp.d[0] = -s->fp->vfp.d[0];        /* FNEGD FACC,FACC */
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_arrayfactrv(s);                         /* B ARRAYFACTRV */
    return;
}

/* ARRAYFACTLV (Array.s:933-935): a scalar l-value on the right of the
 * array assignment expression. */
static void da_arrayfactlv(struct ros_cpu *s)
{
    s->r[14] = 0xFFFFFFF0u;                    /* BL VARIND */
    basicvfp_VARIND(s);
    da_arrayfactrv(s);                         /* B ARRAYFACTRV */
    return;
}

/* ARRAYFACT (Array.s:937-943): a constant right side.  It works from
 * the saved AELINE minus one, because the FACTOR convention reads from
 * just after it. */
static void da_arrayfact(struct ros_cpu *s)
{
    uint32_t r11;

    r11 = ros_ld32(s->r[13]);                  /* LDR AELINE,[SP],#4 */
    s->r[13] += 4;
    r11 -= 1;                                  /* SUB AELINE,AELINE,#1 */
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;                    /* BL FACTOR */
    basicvfp_FACTOR(s);
    da_arrayfactrv(s);                         /* B ARRAYFACTRV */
    return;
}

/* ARRAYFACTRV (Array.s:945-1023), with the constant fill
 * (ARRAYCONSTASSIGN1/FP/STRING), the initialiser list (ARRAYCONSTS)
 * and 0-A() (ARRAYZEROMINUS). */
static void da_arrayfactrv(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    if (r10 == 43 || r10 == 45 || r10 == 42 || r10 == 47) {
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);           /* BEQ ARRAYCONSTBINARY */
        da_arrayconstbinary(s);
        return;
    }
    r5 = ros_ld32(r13 + 4);                    /* LDMFD SP!,{R4,R5} */
    r4 = ros_ld32(ros_ld32(r13));              /* LDR R4,[R4] */
    r13 += 8;
    ros_subs(s, r4, 16);                       /* CMP R4,#16 */
    if (r4 < 16) {                             /* BCC ERARRZ */
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERARRZ(s);
        return;
    }
ARRAYCONSTASSIGN1:
    r6 = ros_ld32(r4);                         /* LDR R6,[R4],#4 */
    r4 += 4;
    if (r6 != 0)                               /* CMP R6,#0 */
        goto ARRAYCONSTASSIGN1;
    r5 -= 0x100u;                              /* SUB R5,R5,#256 */
    if (r10 == 44)                             /* CMP R10,#"," */
        goto ARRAYCONSTS;
    s->r[0] = r0;                              /* BL AEDONE */
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r11 = ros_ld32(r4);                        /* LDR AELINE,[R4],#4 */
    r4 += 4;
    ros_subs(s, r5, TFPLV);                    /* CMP R5,#TFPLV */
    if (r5 > TFPLV)
        goto ARRAYCONSTASSIGNSTRING;
    if (r5 == TFPLV)
        goto ARRAYCONSTASSIGNFP;
    s->r[4] = r4;                              /* BL INTEGY */
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r5 = r11 << 2;                             /* MOV R5,AELINE,LSL #2 */
    basicvfp_INITIALISERAM(s, r0, &r1, &r2, &r3, &r4, &r5, 0xFFFFFFF0u);
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4; s->r[5] = r5;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTASSIGNFP:                            /* Array.s:973-996 */
    s->r[4] = r4;                              /* BL FLOATY */
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_FLOATY(s);
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    ros_std(r13 - 8, s->fp->vfp.d[0]);         /* FSTD FACC,[SP,#-8]! */
    r0 = ros_ld32(r13 - 8);                    /* LDMFD SP,{R0,R1} */
    r1 = ros_ld32(r13 - 4);
    r2 = ros_ld32(r13 - 8);                    /* LDMFD SP!,{R2,R3} */
    r3 = ros_ld32(r13 - 4);
ARRAYCONSTASSIGNFP1:
    {
        uint32_t v = r11;                      /* SUBS AELINE,AELINE,#2 */
        r11 -= 2;
        if (v >= 2) {                          /* STMHSIA R4!,{R0-R3} */
            ros_st32(r4, r0);
            ros_st32(r4 + 4, r1);
            ros_st32(r4 + 8, r2);
            ros_st32(r4 + 12, r3);
            r4 += 16;
        }
        if (v > 2)
            goto ARRAYCONSTASSIGNFP1;
    }
    if (r11 != 0) {                            /* STMNEIA R4!,{R0,R1} */
        ros_st32(r4, r0);
        ros_st32(r4 + 4, r1);
        r4 += 8;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTASSIGNSTRING:                        /* Array.s:998-1007 */
    ros_subs(s, r9, 0);                        /* CMP TYPE,#0 */
    if (r9 != 0) {                             /* BNE ERTYPESTR */
        s->r[4] = r4; s->r[11] = r11;
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    r3 = r8 - 1536;                            /* ADD R3,ARGP,#STRACC */
ARRAYCONSTASSIGNSTRING1:
    s->r[3] = r3;                              /* BL STSTORE */
    s->r[4] = r4;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STSTORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r4 += 5;                                   /* ADD R4,R4,#5 */
    r11 -= 1;                                  /* SUBS AELINE,AELINE,#1 */
    if (r11 != 0)
        goto ARRAYCONSTASSIGNSTRING1;
    s->r[4] = r4; s->r[11] = r11; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ARRAYCONSTS:                                   /* Array.s:1009-1023 */
    r6 = ros_ld32(r4);                         /* LDR R6,[R4],#4 */
    r4 += 4;
ARRAYCONSTS1:
    r6 = ros_subs(s, r6, 1);                   /* SUBS R6,R6,#1 */
    if ((int32_t)r6 < 0) {                     /* BMI ERRSUB */
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERRSUB(s);
        return;
    }
    r13 -= 12;                                 /* STMFD SP!,{R4,R5,R6} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    ros_st32(r13 + 8, r6);
    s->r[0] = r0;                              /* BL STOREA */
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5,R6} */
    r5 = ros_ld32(r13 + 4);
    r6 = ros_ld32(r13 + 8);
    r13 += 12;
    r4 = r5 < 128 ? r4 + r5 : r4 + 5;          /* ADDCC R4,R4,R5 */
    if (r10 == 44)                             /* CMP R10,#"," */
        goto ARRAYCONSTS1;
    s->r[4] = r4;                              /* BL AEDONE */
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r6 = ros_subs(s, r6, 1);                   /* SUBS R6,R6,#1 */
    if ((int32_t)r6 < 0) {                     /* BMI ERRSUB */
        s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERRSUB(s);
        return;
    }
    s->r[0] = r0;                              /* BL STOREA */
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* ARRAYCONSTBINARY (Array.s:1040-1073): A() = B% op C(), with the
 * constant on the LEFT. */
static void da_arrayconstbinary(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r6 = ros_ld32(r13);                        /* LDMFD SP!,{R6,R7} */
    r7 = ros_ld32(r13 + 4);
    r13 += 8;
    s->r[0] = r0;                              /* BL PUSHTYPE */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_PUSHTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13];
    r14 = s->r[14];
    ros_st32(r13 - 4, r9);                     /* STR TYPE,[SP,#-4]! */
    r13 -= 16;                                 /* STMFD SP!,{R6,R7,R10} */
    ros_st32(r13, r6);
    ros_st32(r13 + 4, r7);
    ros_st32(r13 + 8, r10);
    s->r[6] = r6;                              /* BL LVBLNK */
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LVBLNK(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z)                                  /* BEQ ERTYPEARRAY */
        da_msg_at(s, SITE_ERTYPEARRAY);
    r1 = ros_subs(s, r9, 0x100u);              /* SUBS R1,TYPE,#256 */
    if (r9 < 0x100u)                           /* BCC ERTYPEARRAY */
        da_msg_at(s, SITE_ERTYPEARRAY);
    s->r[1] = r1;                              /* BL AEDONES */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5,R7} */
    r5 = ros_ld32(r13 + 4);
    r7 = ros_ld32(r13 + 8);
    r13 += 12;
    da_arraybinaryconst1_run(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9,
                             r10, r11, r12, r13, r14);
    return;
}

/* ARRAYZEROMINUS (Array.s:1025-1038): 0-A() as the constant-left
 * machinery with a TINTEGER zero. */
static void da_arrayzerominus(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1], r13 = s->r[13];

    s->r[14] = 0xFFFFFFF0u;                    /* BL AEDONES */
    basicvfp_hand_AEDONES(s);
    s->r[2] = TINTEGER;                        /* MOV R2,#TINTEGER */
    s->r[3] = 0;                               /* MOV R3,#0 */
    s->r[4] = ros_ld32(r13);                   /* LDMFD SP!,{R4,R5} */
    s->r[5] = ros_ld32(r13 + 4);
    ros_st32(r13 - 8, TINTEGER);               /* STMFD SP!,{R2,R3} */
    ros_st32(r13 - 4, 0);
    s->r[13] = r13 - 8;
    s->r[7] = 45;                              /* MOV R7,#"-" */
    da_arraybinaryconst1(s);                   /* B ARRAYCONSTBINARY1 */
    return;
}

/* MATRIXMULTIPLY (Array.s:1272-1509), with the copy of the
 * destination done "as a kindness", and the vector special cases
 * (VECTORMULTIPLY, VECTORROWMULTIPLY, Array.s:1511-1547). */
static void da_matrixmultiply(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v, w;
    double d1, d2, acc;

    r9 -= 0x100u;                              /* SUB TYPE,TYPE,#256 */
    ros_subs(s, r9, 128);                      /* CMP TYPE,#128 */
    if (r9 >= 128)                             /* BCS ERTYPEARRAYB */
        da_msg_at(s, SITE_ERTYPEARRAYB);
    if (r4 != r1 && r4 != r0)                  /* CMP R4,R1 ; CMPNE R4,R0 */
        goto MATRIXMULTIPLYNOCOPY;
    r5 = r4;                                   /* MOV R5,R4 */
    r6 = r4;                                   /* MOV R6,R4 */
MATRIXMULTIPLYDESTSIZE:
    r7 = ros_ld32(r6);                         /* LDR R7,[R6],#4 */
    r6 += 4;
    if (r7 != 0)                               /* TEQ R7,#0 */
        goto MATRIXMULTIPLYDESTSIZE;
    r7 = ros_ld32(r6);                         /* LDR R7,[R6],#4 */
    r7 = (r9 == 4 ? r7 << 2 : r7 << 3) + r6 + 4 - r5;
    r6 = ros_ld32(r8 - 140);                   /* LDR R6,[ARGP,#FSA] */
    r2 = r6 + r7 + 0x400u;                     /* ADD R2,R6,R7,#1024 */
    ros_subs(s, r2, r13);                      /* CMP R2,SP */
    if (r2 >= r13)                             /* BCS ERMATMULSPACE */
        da_msg_at(s, SITE_ERMATMULSPACE);
MATRIXMULTIPLYCOPY:
    r5 += 4;
    ros_st32(r6, ros_ld32(r5 - 4));            /* LDR R2,[R5],#4 ; STR */
    r6 += 4;
    v = r7;                                    /* SUBS R7,R7,#4 */
    r7 -= 4;
    if (v >= 4)                                /* BCS MATRIXMULTIPLYCOPY */
        goto MATRIXMULTIPLYCOPY;
    if (r4 == r1)                              /* CMP R4,R1 */
        r1 = ros_ld32(r8 - 140);               /* LDREQ R1,[ARGP,#FSA] */
    if (r4 == r0)                              /* CMP R4,R0 */
        r0 = ros_ld32(r8 - 140);
MATRIXMULTIPLYNOCOPY:
    r13 -= 8;                                  /* STMFD SP!,{R8,R10} */
    ros_st32(r13, r8);
    ros_st32(r13 + 4, r10);
    r2 = ros_ld32(r4);                         /* LDR R2,[R4],#4 ; ALIMI */
    r3 = ros_ld32(r1);                         /* LDR R3,[R1],#4 ; BLIMI */
    r5 = ros_ld32(r0);                         /* LDR R5,[R0],#4 ; CLIMJ */
    r6 = ros_ld32(r4);                         /* LDR R6,[R4],#4 ; ALIMK */
    r4 += 8;
    r7 = ros_ld32(r1);                         /* LDR R7,[R1],#4 ; BLIMJ */
    r1 += 8;
    r10 = ros_ld32(r0);                        /* LDR R10,[R0],#4 ; CLIMK */
    r0 += 8;
    if (r6 == 0)                               /* CMP R6,#0 ; BEQ VECTOR */
        goto VECTORMULTIPLY;
    v = r3;                                    /* BLIMI, before R3 is reused */
    ros_subs(s, r2, r3);                       /* CMP R2,R3 ; test LIMI */
    if (r2 == v) ros_subs(s, r7, r5);          /* CMPEQ R7,R5 */
    if (r2 == v && r7 == r5) ros_subs(s, r6, r10);
    w = ros_ld32(r4);                          /* LDR R3,[R4],#8 */
    r4 += 8;
    r3 = ros_ld32(r1);                         /* LDR R3,[R1],#8 */
    r1 += 8;
    {
        uint32_t zero3 = ros_ld32(r0);         /* LDR R3,[R0],#8 */
        r0 += 8;
        if (r2 != v || r7 != r5 || r6 != r10 || w != 0 || r3 != 0
            || zero3 != 0)
            da_msg_at(s, SITE_ERTYPEARRAYC);   /* BNE ERTYPEARRAYC */
    }
    goto MATRIXMULTIPLYMAIN;

VECTORMULTIPLY:                                /* Array.s:1511-1528 */
    r4 += 4;                                   /* ADD R4,R4,#4 */
    if (r7 == 0)                               /* CMP R7,#0 */
        goto VECTORROWMULTIPLY;
    ros_subs(s, r2, r3);                       /* CMP R2,R3 */
    if (r2 == r3) ros_subs(s, r5, r7);         /* CMPEQ R5,R7 */
    r6 = ros_ld32(r1);                         /* LDR R6,[R1],#8 */
    r1 += 8;
    if (r2 != r3 || r5 != r7 || r6 != 0)       /* BNE ERTYPEARRAYC */
        da_msg_at(s, SITE_ERTYPEARRAYC);
    r0 += 4;                                   /* ADD R0,R0,#4 */
    r6 = 1;                                    /* MOV R6,#1 ; LIMK=1 */
    goto MATRIXMULTIPLYMAIN;
VECTORROWMULTIPLY:                             /* Array.s:1529-1547 */
    ros_subs(s, r2, r10);                      /* CMP R2,R10 */
    if (r2 == r10) ros_subs(s, r3, r5);        /* CMPEQ R3,R5 */
    r6 = ros_ld32(r0);                         /* LDR R6,[R0],#8 */
    r0 += 8;
    if (r2 != r10 || r3 != r5 || r6 != 0)
        da_msg_at(s, SITE_ERTYPEARRAYC);
    r1 += 4;                                   /* ADD R1,R1,#4 */
    r5 = r3;                                   /* MOV R5,R3 ; move LIMJ */
    r6 = r10;                                  /* MOV R6,R10 ; move LIMK */
    r2 = 1;                                    /* MOV R2,#1 ; LIMI=1 */

MATRIXMULTIPLYMAIN:                            /* Array.s:1375-1509 */
    if (r9 == TFPLV)                           /* CMP TYPE,#TFPLV */
        goto MATRIXMULTIPLYFP;
    r13 -= 4;                                  /* STR R0,[SP,#-4]! */
    ros_st32(r13, r0);
    r6 <<= 2;                                  /* MOV R6,R6,LSL #2 */
MATRIXMULTIPLYINT1:
    r0 = ros_ld32(r13);                        /* LDR R0,[SP] */
    r11 = r6;                                  /* MOV R11,R6 */
MATRIXMULTIPLYINT2:
    r3 = 0;                                    /* MOV R3,#0 ; result */
    r14 = r1;                                  /* MOV R14,R1 ; l source */
    r7 = r0;                                   /* MOV R7,R0 ; r source */
    r8 = r5;                                   /* MOV R8,R5 ; LIMJ count */
MATRIXMULTIPLYINT3:
    r9 = ros_ld32(r14);                        /* LDR R9,[R14],#4 */
    r14 += 4;
    w = r7;                                    /* LDR R10,[R7],R6 */
    r7 += r6;
    r3 = r9 * ros_ld32(w) + r3;                /* MLA R3,R9,R10,R3 */
    r8 -= 1;                                   /* SUBS R8,R8,#1 */
    if (r8 != 0)
        goto MATRIXMULTIPLYINT3;
    ros_st32(r4, r3);                          /* STR R3,[R4],#4 */
    r4 += 4;
    r0 += 4;                                   /* ADD R0,R0,#4 */
    r11 -= 4;                                  /* SUBS R11,R11,#4 */
    if (r11 != 0)
        goto MATRIXMULTIPLYINT2;
    r1 += r5 << 2;                             /* ADD R1,R1,R5,LSL #2 */
    r2 -= 1;                                   /* SUBS R2,R2,#1 */
    if (r2 != 0)
        goto MATRIXMULTIPLYINT1;
    r0 = ros_ld32(r13);                        /* LDMFD SP!,{R0,R8,R10} */
    r8 = ros_ld32(r13 + 4);
    r10 = ros_ld32(r13 + 8);
    r13 += 12;
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

MATRIXMULTIPLYFP:                              /* the FPOINT=2 loops */
    r6 <<= 3;                                  /* MOV R6,R6,LSL #3 */
MATRIXMULTIPLYFP1:
    r10 = r0;                                  /* MOV R10,R0 */
    r11 = r6;                                  /* MOV R11,R6 */
MATRIXMULTIPLYFP2:
    acc = 0.0;                                 /* FLDD FACC,=0 */
    r14 = r1;                                  /* MOV R14,R1 ; l source */
    r7 = r10;                                  /* MOV R7,R10 ; r source */
    r3 = r5;                                   /* MOV R3,R5 ; LIMJ count */
MATRIXMULTIPLYFP3:
    d1 = ros_ldd(r14);                         /* FLDD D1,[R14],#8 */
    r14 += 8;
    d2 = ros_ldd(r7);                          /* FLDD D2,[R7] */
    r7 += r6;
    r3 -= 1;                                   /* SUBS R3,R3,#1 */
    {                                          /* FMACD FACC,D1,D2 */
        double prod = d1 * d2;
        s->fp->fpscr |= ros_vfp_ex2(prod, d1, d2);
        double sum = acc + prod;
        s->fp->fpscr |= ros_vfp_ex2(sum, acc, prod);
        acc = sum;
    }
    if (r3 != 0)
        goto MATRIXMULTIPLYFP3;
    ros_std(r4, acc);                          /* FSTD FACC,[R4],#8 */
    r4 += 8;
    r10 += 8;                                  /* ADD R10,R10,#8 */
    r11 -= 8;                                  /* SUBS R11,R11,#8 */
    if (r11 != 0)
        goto MATRIXMULTIPLYFP2;
    r1 += r5 << 3;                             /* ADD R1,R1,R5,LSL #3 */
    r2 = ros_subs(s, r2, 1);                   /* SUBS R2,R2,#1 */
    if (r2 != 0)
        goto MATRIXMULTIPLYFP1;
    {                                          /* FPSCRCheck R14 */
        uint32_t f = s->fp->fpscr;             /* FMRX R14,FPSCR */
        s->r[14] = f;
        if ((f & 7) != 0) {                    /* TST IOC/DZC/OFC */
            basicvfp_VFPException(s);
            return;
        }
    }
    r8 = ros_ld32(r13);                        /* LDMFD SP!,{R8,R10} */
    r10 = ros_ld32(r13 + 4);
    r13 += 8;
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, s->r[14]);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* =====================================================================
 * The statement entries.
 * ===================================================================== */

/* LETSTNOTCACHE (Basic.s:855-858): the l-value is not in the cache.
 * LVNOTCACHE walks the variable and creates it if need be, through
 * GOTLTCREATE.  The Z flag on GOTLTCREATE's exit is left over from
 * LVNOTCACHE's DONEXT (the "tricky DONEXT call" note).  Then the
 * shared GOTLT follows. */
void basicvfp_hand_LETSTNOTCACHE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r4 = s->r[4], r9 = s->r[9],
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];

    r11 = r12;                                 /* MOV AELINE,LINE */
    s->r[0] = r0;                              /* BL LVNOTCACHE */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LVNOTCACHE(s);
    r0 = s->r[0]; r1 = s->r[1]; r4 = s->r[4]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12];
    if (s->z) {                                /* BLEQ GOTLTCREATE */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = s->r[2]; s->r[3] = s->r[3];
        s->r[4] = r4; s->r[5] = s->r[5]; s->r[6] = s->r[6]; s->r[7] = s->r[7];
        s->r[8] = s->r[8]; s->r[9] = r9; s->r[11] = r11; s->r[12] = r12;
        s->r[13] = s->r[13];
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_GOTLTCREATE(s);
        r0 = s->r[0]; r1 = s->r[1]; r4 = s->r[4]; r9 = s->r[9];
        r11 = s->r[11]; r12 = s->r[12];
    }
    (void)r0; (void)r1; (void)r4; (void)r9; (void)r11; (void)r12;
    da_gotlt(s);                               /* B GOTLT */
    return;
}

/* LETST (Basic.s:873-886): the hot entry.  It probes the line cache
 * for the l-value.  A TFP-tagged delta is an array-element cache
 * entry, which ARLOOKCACHE checks against the array.  Then it pushes
 * the pair and takes the shared '=' machinery. */
void basicvfp_hand_LETST(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r1 = r8 + ((r12 & CACHEMASK) << CACHESHIFT); /* AND R1,LINE,#CACHEMASK */
    r0 = ros_ld32(r1);                         /* LDMIA R1,{IACC,R1,R4,TYPE} */
    r4 = ros_ld32(r1 + 8);
    r9 = ros_ld32(r1 + 12);
    r1 = ros_ld32(r1 + 4);
    if (r4 != r12) {                           /* CMP R4,LINE */
        da_store_all(s, r0, r1, s->r[2], s->r[3], r4, s->r[5], s->r[6],
                     s->r[7], r8, r9, r10, s->r[11], r12, r13, r14);
        basicvfp_hand_LETSTNOTCACHE(s);        /* BNE LETSTNOTCACHE */
        return;
    }
    r11 = r12 + r1;                            /* ADD AELINE,LINE,R1 */
    if ((int32_t)(r1 + 1) < 0)                 /* CMN R1,#1 ; BMI */
        goto LETSTCACHEARRAY;
    r13 -= 8;                                  /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
LETSTSPACE:
    r10 = ros_ld8(r11);                        /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == 32)                             /* CMP R10,#" " */
        goto LETSTSPACE;
    if (r10 != 61) {                           /* CMP R10,#"=" ; BNE GOTLT2 */
        da_store_all(s, r0, r1, s->r[2], s->r[3], r4, s->r[5], s->r[6],
                     s->r[7], r8, r9, r10, r11, r12, r13, r14);
        da_gotlt2(s);
        return;
    }
    da_store_all(s, r0, r1, s->r[2], s->r[3], r4, s->r[5], s->r[6],
                 s->r[7], r8, r9, r10, r11, r12, r13, r14);
    da_exprstore(s);                           /* EXPRSTORESTMT */
    return;

LETSTCACHEARRAY:                               /* Basic.s:868-871 */
    r11 -= TFP;                                /* SUB AELINE,AELINE,#TFP */
    s->r[0] = r0;                              /* BL ARLOOKCACHE */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_ARLOOKCACHE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (!s->z) {                               /* BNE GOTLT */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_gotlt(s);
        return;
    }
    s->r[15] = s->r[14];                       /* B MISTAK */
    basicvfp_MISTAK(s);
    return;
}

/* LET (Stmt.s:785-795): an explicit LET.  AELV reads the l-value.  If
 * the variable does not exist, the code scans to the '=' and CREATEs
 * it.  Then the shared GOTLT follows. */
void basicvfp_hand_LET(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0;                              /* BL AELV */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (!s->z) {                               /* BNE GOTLT */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_gotlt(s);
        return;
    }
    if (s->c) {                                /* BCS ERSYNT */
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    r0 = r11;                                  /* MOV R0,AELINE */
LET1:
    r7 = ros_ld8(r0);                          /* LDRB R7,[R0],#1 */
    r0 += 1;
    if (r7 == 32)                              /* CMP R7,#" " */
        goto LET1;
    ros_subs(s, r7, 61);                       /* CMP R7,#"=" */
    if (r7 != 61) {                            /* BNE MISSEQ */
        s->r[0] = r0;
        s->r[7] = r7;
        s->r[15] = s->r[14];
        basicvfp_MISSEQ(s);
        return;
    }
    s->r[0] = r0;                              /* BL CREATE */
    s->r[7] = r7;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_CREATE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, s->r[10], r11,
                 r12, r13, r14);
    da_gotlt(s);                               /* B GOTLT */
    return;
}

/* DIM (Stmt.s:157-341): the array dimensioner and the DIM of a block
 * on the heap or stack.  The LOCAL form claims the block from the
 * stack and links it into the DIM LOCAL list. */
void basicvfp_hand_DIM(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v, w, vv;

DIM:                                           /* the loop-back for items */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    r0 = r10;                                  /* MOV R0,R10 */
    s->r[0] = r0;                              /* BL WORDCQ */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_WORDCQ(s);
    v = s->c;                                  /* CMPCS R10,#"A" */
    if (s->c) ros_subs(s, r10, 65);
    if (!v || r10 < 65)                        /* BCC BADDIM */
        da_msg_at(s, SITE_BADDIM);
    r9 = TFPLV;                                /* MOV TYPE,#TFPLV */
    r11 = r12;                                 /* MOV AELINE,LINE */
DIMNO:
    r0 = ros_ld8(r11);                         /* LDRB R0,[AELINE],#1 */
    r11 += 1;
    s->r[0] = r0;                              /* BL WORDCQ */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_WORDCQ(s);
    if (s->c) goto DIMNO;                      /* BCS DIMNO */
    if (r0 == 40)                              /* CMP R0,#"(" */
        goto DIMVAR;
    if (r0 == 37)                              /* CMP R0,#"%" */
        r9 = 4;
    if (r0 != 37 && r0 != 36)                  /* CMPNE R0,#"$" */
        goto DIMSPA;
    if (r9 == TFPLV)                           /* CMP TYPE,#TFPLV */
        r9 = 128;
    r0 = ros_ld8(r11);                         /* LDRB R0,[AELINE],#1 */
    r11 += 1;
    if (r0 != 40)                              /* CMP R0,#"(" */
        goto DIMSPA;
DIMVAR:
    r3 = ros_ld8(r12);                         /* LDRB R3,[R4],#1 */
    r4 = r12 + 1;
    r13 -= 4;                                  /* STR TYPE,[SP,#-4]! */
    ros_st32(r13, r9);
    s->r[0] = r0;                              /* BL LOOKUP */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LOOKUP(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r5 = s->r[5]; r6 = s->r[6];
    r14 = s->r[14];
    if (!s->z)                                 /* BNE DIMVA2 */
        goto DIMVA2;
    r0 = (r0 + 3) & ~3u;                       /* ADD/BIC IACC */
    r3 = ros_ld32(r0);                         /* LDR R3,[IACC] */
    ros_subs(s, r3, 16);                       /* CMP R3,#16 */
    if (r3 < 16)                               /* BCC DIMVA3 */
        goto DIMVA3;
    da_msg_at(s, SITE_ERNDIM);                 /* B ERNDIM */
DIMVA2:
    s->r[9] = 4;                               /* MOV TYPE,#4 */
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                    /* BL CREATE */
    basicvfp_hand_CREATE(s);
    r0 = s->r[0]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r3 = 0;                                    /* MOV R3,#0 */
DIMVA3:
    r2 = r8 - 1536;                            /* ADD CLEN,ARGP,#STRACC */
    r9 = ros_ld32(r13);                        /* LDR TYPE,[SP],#4 ; the */
    r1 = 1;                                    /* MOV R1,#1 ; writeback and */
    ros_st32(r13, r0);                         /* STR IACC,[SP,#-4]! ; the STR's pre-dec cancel */
RDLOOP:
    r13 -= 12;                                 /* STMFD SP!,{R1,R3,TYPE} */
    ros_st32(r13, r1);
    ros_st32(r13 + 4, r3);
    ros_st32(r13 + 8, r9);
    s->r[0] = r0;                              /* BL SPUSH */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPUSH(s);
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[7] = r7;                              /* BL EXPR */
    s->r[9] = r9;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_subs(s, r0, 0x1000000u);               /* CMP IACC,#&1000000 */
    if (r0 >= 0x1000000u)                      /* BCS BADDIMSUB */
        da_msg_at(s, SITE_BADDIMSUB);
    r7 = r0 + 1;                               /* ADD R7,IACC,#1 */
    s->r[14] = 0xFFFFFFF0u;                    /* BL SPULL */
    basicvfp_hand_SPULL(s);
    r0 = s->r[0]; r2 = s->r[2]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r13);                        /* LDMFD SP!,{R1,R3,TYPE} */
    r3 = ros_ld32(r13 + 4);
    r9 = ros_ld32(r13 + 8);
    r13 += 12;
    ros_st32(r2, r7);                          /* STR R7,[CLEN],#4 */
    r2 += 4;
    r4 = 0;                                    /* MOV R4,#0 */
RDLOOM:
    v = r7;                                    /* MOVS R7,R7,LSR #1 */
    r7 >>= 1;
    w = r1;
    if ((v & 1) != 0)                          /* ADDCS R4,R4,R1 */
        r4 += r1;
    ros_subs(s, r4, 0x1000000u);               /* CMP R4,#&1000000 */
    vv = r1 + r1;                              /* ADDCCS R1,R1,R1 */
    if (r4 < 0x1000000u)
        r1 = ros_adds(s, r1, r1);
    if (r4 >= 0x1000000u || vv < w)            /* BCS DIMRAM */
        da_msg_at(s, SITE_DIMRAM);
    if (r7 != 0)                               /* TEQ R7,#0 */
        goto RDLOOM;
    r1 = r4;                                   /* MOV R1,R4 */
    if (r10 == 44)                             /* CMP R10,#"," */
        goto RDLOOP;
    ros_subs(s, r10, 41);                      /* CMP R10,#")" */
    if (r10 != 41)                             /* BNE BADDIMLIST */
        da_msg_at(s, SITE_BADDIMLIST);
    r5 = (((r9 ^ 8) == 0 ? r4 << 3 : (r9 ^ 4) == 0 ? r4 << 2
                                                   : r4 + (r4 << 2)) + 3)
         & ~3u;                                /* bytes, rounded to words */
    r0 = r2 - (r8 - 1536) + 8 + r5;            /* ADD IACC,ARGP,#STRACC ... */
    r4 = ros_ld32(r8 - 140);                   /* LDR R4,[ARGP,#FSA] */
    r7 = r4 + r0;                              /* ADD R7,R4,IACC */
    r6 = r7 + 0x400u;                          /* ADD R6,R7,#1024 */
    v = r13;                                   /* CMP R6,SP */
    ros_subs(s, r6, r13);
    if (r6 >= r13)                             /* BCS DIMRAM */
        da_msg_at(s, SITE_DIMRAM);
    r6 = ros_ld32(r13);                        /* LDR R6,[SP],#4 ; address */
    if (r3 == 1) {                             /* CMP R3,#1 : LOCAL array */
        uint32_t sp0 = r13 + 4 - r0;           /* SUBEQ SP,SP,IACC */
        r4 = sp0;                              /* MOVEQ R4,SP */
        ros_st32(sp0 - 4, r0);                 /* STREQ IACC,[SP,#-4]! */
        ros_st32(sp0 - 8, r6);                 /* STREQ R6,[SP,#-4]! */
        r13 = sp0 - 8;
        r7 = ros_ld32(r8 - 100);               /* LDREQ R7,[ARGP,#LOCALARLIST] */
        ros_st32(r13 - 8, r9 + 0x100u);        /* STMEQFD SP!,{IACC,R7} */
        ros_st32(r13 - 4, r7);
        r13 -= 8;                              /* two words: SP is sp0-16 */
        ros_st32(r8 - 100, r13);               /* STREQ SP,[ARGP,#LOCALARLIST] */
    } else {
        r13 = v + 4;
        ros_st32(r8 - 140, r7);                /* STRNE R7,[ARGP,#FSA] */
    }
    ros_st32(r6, r4);                          /* STR R4,[R6] ; update ptr */
    r7 = r8 - 1536;                            /* ADD R7,ARGP,#STRACC */
DIMCOPY:
    w = r7;                                    /* LDR IACC,[R7],#4 */
    r7 += 4;
    ros_st32(r4, ros_ld32(w));                 /* STR IACC,[R4],#4 */
    r4 += 4;
    if (r7 < r2)                               /* CMP R7,CLEN */
        goto DIMCOPY;
    r0 = 0;                                    /* MOV IACC,#0 */
    ros_st32(r4, 0);                           /* terminate list */
    ros_st32(r4 + 4, r1);                      /* number of entries */
    r4 += 8;
    basicvfp_INITIALISERAM(s, r0, &r1, &r2, &r3, &r4, &r5, 0xFFFFFFF0u);
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    goto DIMNXT;                               /* B DIMNXT */

DIMSPA:                                        /* Stmt.s:157-227 */
    r12 -= 1;                                  /* SUB LINE,LINE,#1 */
    s->r[0] = r0;                              /* BL CRAELV */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z)                                  /* BEQ BADDIM */
        da_msg_at(s, SITE_BADDIM);
    ros_subs(s, r9, 9);                        /* CMP TYPE,#TFPLV+1 */
    if (r9 >= 9) {                             /* BHS ERTYPENUM */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[9] = r9;
        s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_ERTYPENUM(s);
        return;
    }
DIMSPA2:
    r10 = ros_ld8(r11);                        /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == 32)                             /* TEQ R10,#" " */
        goto DIMSPA2;
    ros_logic(s, r10 ^ TLOCAL, s->c);          /* TEQ R10,#TLOCAL */
    if (s->z)
        r4 = ros_ld32(r13);                    /* LDREQ R4,[SP] */
    if (!s->z) {
        r11 -= 1;                              /* SUBNE AELINE,AELINE,#1 */
        goto DIMHEAP;
    }
    v = r4 ^ TPROC;                            /* TEQ R4,#TPROC */
    ros_logic(s, r4 ^ TPROC, s->c);
    w = r4 ^ TFN;                              /* TEQNE R4,#TFN */
    if (v != 0)
        ros_logic(s, r4 ^ TFN, s->c);
    if (v != 0 && w != 0)                      /* BNE ERRNLC */
        da_msg_at(s, SITE_ERRNLC);
    r13 -= 8;                                  /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[4] = r4;                              /* BL EXPR */
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r7 = ros_adds(s, r0, 1);                   /* ADDS R7,IACC,#1 */
    if ((int32_t)r7 < 0)                       /* BMI BADDIMSIGN */
        {
            r13 += 8;                          /* ADDMI SP,SP,#8 */
            da_msg_at(s, SITE_BADDIMSIGN);
        }
    r7 = (r7 + 3) & ~3u;                       /* round to words */
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    r0 = r7 + ros_ld32(r8 - 140) + 0x200u;     /* FSA + claim + 512 */
    ros_subs(s, r0, r13);                      /* CMP IACC,SP */
    if (r0 > r13)                              /* BHI BADDIMSIZE */
        da_msg_at(s, SITE_BADDIMSIZE);
    r0 = r13 - r7 + 12;                        /* SUB IACC,SP,R7 ; +12 */
    r13 -= 4;                                  /* STR R7,[SP,#-4]! */
    ros_st32(r13, r7);
    s->r[0] = r0;                              /* BL STOREA */
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    v = r13;                                   /* LDR R6,[SP],#4 */
    r6 = ros_ld32(r13);
    r13 += 4;
    if (r6 == 0)                               /* TEQ R6,#0 */
        goto DIMNXT;
    r7 = ros_ld32(r8 - 36);                    /* LDR R7,[ARGP,#DIMLOCAL] */
    r3 = ros_ld32(r13 + 4);                    /* LDMFD SP!,{R0,R3,R4} */
    r4 = ros_ld32(r13 + 8);
    r5 = TDIM;                                 /* MOV R5,#TDIM */
    r13 = r13 + 12 - r6;                       /* SUB SP,SP,R6 */
    r13 -= 24;                                 /* STMFD SP!,{R0,R3,R4,R5,R6,R7} */
    ros_st32(r13, ros_ld32(v + 4));
    ros_st32(r13 + 4, r3);
    ros_st32(r13 + 8, r4);
    ros_st32(r13 + 12, TDIM);
    ros_st32(r13 + 16, r6);
    ros_st32(r13 + 20, r7);
    r0 = r13 + 12;                             /* ADD R0,SP,#12 */
    ros_st32(r8 - 36, r0);                     /* STR R0,[ARGP,#DIMLOCAL] */
    goto DIMNXT;

DIMHEAP:                                       /* Stmt.s:211-228 */
    r13 -= 8;                                  /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[4] = r4;                              /* BL EXPR */
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r7 = r0 + 1;                               /* ADD R7,IACC,#1 */
    r0 = ros_ld32(r8 - 140);                   /* LDR IACC,[ARGP,#FSA] */
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    /* The pop's writeback and the push's pre-decrement cancel out, as
     * they do at DIMVA3.  The two words go back where they came from
     * and SP does not move.  (An earlier version wrote this as a push
     * BELOW them.  That left SP eight bytes low for the rest of the
     * program.  A `DIM b% 1024` inside a PROC then put ENDPROC's frame
     * word out of reach and gave "Not in a procedure", and every DIM
     * at the top level leaked eight bytes of BASIC's stack.  No corpus
     * program DIMs a block inside a PROC, but rosgd's Wimp cases all
     * do, in PROCstart.) */
    ros_st32(r13, r0);                         /* STMFD SP!,{IACC,R7} */
    ros_st32(r13 + 4, r7);
    s->r[0] = r0;                              /* BL STOREA */
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[7] = r7;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREA(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    v = r13;                                   /* LDMFD SP!,{IACC,R7} */
    r0 = ros_ld32(r13);
    r7 = ros_ld32(r13 + 4);
    r13 += 8;
    ros_logic(s, ros_ld32(v + 4), s->c);       /* TEQ R7,#0 */
    if ((int32_t)r7 < 0)                       /* BMI BADDIMSIGN */
        da_msg_at(s, SITE_BADDIMSIGN);
    r7 = r0 + ((r7 + 3) & ~3u);                /* ADD R7,IACC,R7(rounded) */
    r6 = r7 + 0x200u;                          /* ADD R6,R7,#512 */
    ros_subs(s, r6, r13);                      /* CMP R6,SP */
    if (r6 >= r13)                             /* BCS BADDIMSIZE */
        da_msg_at(s, SITE_BADDIMSIZE);
    ros_st32(r8 - 140, r7);                    /* STR R7,[ARGP,#FSA] */

DIMNXT:                                       /* Stmt.s:229-231 */
    r12 = r11;                                 /* MOV LINE,AELINE */
    if (r10 != 44) {                           /* CMP R10,#"," */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[9] = r9;
        s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];                   /* B DONEXT */
        basicvfp_hand_DONEXT(s);
        return;
    }
    goto DIM;                                  /* B DIM : the next item */
}

/* LOCAL (Stmt.s:796-870): LOCAL ERROR, LOCAL DATA, and the saving of
 * variables.  The value and the l-value go under the PROC/FN frame
 * words.  The new local is then cleared to zero, or to an empty
 * string, a CR, or a null array. */
void basicvfp_hand_LOCAL(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v, w;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == TERROR)                         /* CMP R10,#TERROR */
        goto LOCALERROR;
    ros_subs(s, r10, TDATA);                   /* CMP R10,#TDATA */
    if (r10 == TDATA)                          /* BEQ LOCALDATA */
        goto LOCALDATA;
    r0 = ros_ld32(r13);                        /* LDR R0,[SP] */
    v = r0 ^ TPROC;                            /* TEQ R0,#TPROC */
    ros_logic(s, r0 ^ TPROC, s->c);
    w = r0 ^ TFN;                              /* TEQNE R0,#TFN */
    if (v != 0)
        ros_logic(s, r0 ^ TFN, s->c);
    if (v != 0 && w != 0)                      /* BNE ERRNLC */
        da_msg_at(s, SITE_ERRNLC);
    r12 -= 1;                                  /* SUB LINE,LINE,#1 */
LOCALP:
    s->r[0] = r0;                              /* BL CRAELV */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ DONXTS ; failed */
        s->r[15] = s->r[14];
        basicvfp_hand_DONXTS(s);
        return;
    }
    r12 = r11;                                 /* MOV LINE,AELINE */
    r5 = ros_ld32(r13);                        /* LDMFD SP!,{R5,R6,R7} */
    r6 = ros_ld32(r13 + 4);
    r7 = ros_ld32(r13 + 8);
    r13 += 12;
    r10 = r0;                                  /* MOV R10,IACC ; value */
    r11 = r9;                                  /* MOV AELINE,TYPE */
    if (r9 >= 0x100u)                          /* CMP TYPE,#256 */
        goto LOCALARRAY;
    s->r[5] = r5;                              /* BL VARIND */
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARIND(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL PUSHTYPE */
    basicvfp_hand_PUSHTYPE(s);
    r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13]; r14 = s->r[14];
    ros_st32(r13 - 8, r10);                    /* STMFD SP!,{R10,AELINE} */
    ros_st32(r13 - 4, r11);
    r13 -= 20;                                 /* STMFD SP!,{R5,R6,R7} */
    ros_st32(r13, r5);
    ros_st32(r13 + 4, r6);
    ros_st32(r13 + 8, r7);
    r0 = 0;                                    /* MOV R0,#0 */
    if (r11 == 128)                            /* CMP AELINE,#128 */
        goto LOCALZS;
    if (r11 >= 128)                            /* BCS LOCALZR */
        goto LOCALZR;
LOCALZN:
    ros_st8(r10, r0);                          /* STRB R0,[R10],#1 */
    r10 += 1;
    v = r11;                                   /* SUBS AELINE,AELINE,#1 */
    r11 -= 1;
    if (v > 1)                                 /* BHI LOCALZN */
        goto LOCALZN;
    goto LOCALZ;
LOCALARRAY:
    r2 = 4;                                    /* MOV R2,#4 */
    ros_st32(r13 - 4, ros_ld32(r0));           /* STR R1,[SP,#-4]! */
    ros_st32(r13 - 12, r0);                    /* STMFD SP!,{IACC,R2} */
    ros_st32(r13 - 8, 4);
    r13 -= 24;                                 /* STMFD SP!,{R5,R6,R7} */
    ros_st32(r13, r5);
    ros_st32(r13 + 4, r6);
    ros_st32(r13 + 8, r7);
    ros_st32(r0, 1);                           /* STR R1,[IACC] ; null it */
    goto LOCALZ;
LOCALZR:
    r0 = 13;                                   /* MOV R0,#13 */
    ros_st8(r10, 13);                          /* STRB R0,[R10] */
    goto LOCALZ;
LOCALZS:
    r2 = r3;                                   /* MOV CLEN,R3 */
    r4 = r10;                                  /* MOV R4,R10 */
    s->r[0] = r0;                              /* BL STSTORE */
    s->r[2] = r2;
    s->r[4] = r4;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STSTORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
LOCALZ:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == 44)                             /* CMP R10,#"," */
        goto LOCALP;
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[5] = r5; s->r[6] = r6;
    s->r[7] = r7; s->r[10] = r10; s->r[11] = r11; s->r[12] = r12;
    s->r[13] = r13; s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B DONEXT */
    basicvfp_hand_DONEXT(s);
    return;

LOCALERROR:                                    /* Stmt.s:796-802 */
    s->r[0] = r0;                              /* BL DONES */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r8 - 120);                   /* LDR R1,[ARGP,#ERRSTK] */
    r2 = ros_ld32(r8 - 124);                   /* LDR R2,[ARGP,#ERRORH] */
    r0 = TERROR;                               /* MOV R0,#TERROR */
    r13 -= 12;                                 /* STMFD SP!,{R0,R1,R2} */
    ros_st32(r13, TERROR);
    ros_st32(r13 + 4, r1);
    ros_st32(r13 + 8, r2);
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

LOCALDATA:                                     /* Stmt.s:803-809 */
    s->r[0] = r0;                              /* BL DONES */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r8 - 268);                   /* LDR R1,[ARGP,#DATAP] */
    r0 = TDATA;                                /* MOV R0,#TDATA */
    r13 -= 8;                                  /* STMFD SP!,{R0,R1} */
    ros_st32(r13, TDATA);
    ros_st32(r13 + 4, r1);
    s->r[0] = r0; s->r[1] = r1; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* SWAP (Stmt.s:1564-1612) takes two l-values.  Two numerics of the
 * same type use the raw byte loop.  Strings and mixed types go through
 * VARIND, PUSHTYPE, STOREA and STORE.  Arrays exchange their
 * pointers. */
void basicvfp_hand_SWAP(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    s->r[0] = r0;                              /* BL AELV */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ FACERR */
        s->r[15] = s->r[14];
        basicvfp_FACERR(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    ros_logic(s, r10 ^ 0x2Cu, s->c);           /* TEQ R10,#"," */
    if (!s->z) {                               /* BNE ERCOMM */
        s->r[15] = s->r[14];
        basicvfp_ERCOMM(s);
        return;
    }
    r13 -= 8;                                  /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[13] = r13;                            /* BL LVBLNK */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_LVBLNK(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ FACERR */
        s->r[15] = s->r[14];
        basicvfp_FACERR(s);
        return;
    }
    r12 = r11;                                 /* MOV LINE,AELINE */
    s->r[12] = r12;                            /* BL DONES */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    if (r9 >= 0x100u)                          /* CMP TYPE,#256 ; BCS SWAPAR1 */
        goto SWAPAR1;
    if (r9 < 128)                              /* CMP TYPE,#128 ; BCC SWAP1 */
        goto SWAP1;
SWAP2:                                         /* Stmt.s:1579-1593 */
    ros_st32(r13 - 8, r4);                     /* STMFD SP!,{R4,R5} ; lv1 */
    ros_st32(r13 - 4, r5);
    r13 -= 16;                                 /* STMFD SP!,{IACC,TYPE} ; lv2 */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    r11 = r13;                                 /* MOV AELINE,SP */
    s->r[4] = r4;                              /* BL VARIND ; of lv2 */
    s->r[5] = r5;
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARIND(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL PUSHTYPE ; value of lv2 */
    basicvfp_hand_PUSHTYPE(s);
    r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4]; r13 = s->r[13]; r14 = s->r[14];
    r13 -= 4;                                  /* STR TYPE,[SP,#-4]! */
    ros_st32(r13, r9);
    r0 = ros_ld32(r11 + 8);                    /* LDR IACC,[AELINE,#8] */
    r9 = ros_ld32(r11 + 12);                   /* LDR TYPE,[AELINE,#12] */
    s->r[0] = r0;                              /* BL VARIND ; value of lv1 */
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARIND(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r11);                        /* LDMFD AELINE!,{R4,R5} ; lv2 */
    r5 = ros_ld32(r11 + 4);
    r11 += 8;
    s->r[4] = r4;                              /* BL STOREA ; lv2=(lv1) */
    s->r[5] = r5;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL PULLTYPE ; (lv2) */
    basicvfp_hand_PULLTYPE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r13 = r11;                                 /* MOV SP,AELINE */
    s->r[13] = r13;                            /* BL STORE ; lv1=(lv2) */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

SWAP1:                                         /* Stmt.s:1594-1601 */
    v = r9 ^ r5;                               /* TEQ TYPE,R5 */
    if (v != 0)                                /* BNE SWAP2 */
        goto SWAP2;
SWAP1A:
    r1 = ros_ld8(r0);                          /* LDRB R1,[IACC] */
    r2 = ros_ld8(r4);                          /* LDRB R2,[R4] */
    ros_st8(r4, r1);                           /* STRB R1,[R4],#1 */
    r4 += 1;
    ros_st8(r0, r2);                           /* STRB R2,[IACC],#1 */
    r0 += 1;
    v = r5;                                    /* SUBS R5,R5,#1 */
    r5 -= 1;
    if (v > 1)                                 /* BHI SWAP1A */
        goto SWAP1A;
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5;
    s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

SWAPAR1:                                       /* Stmt.s:1603-1611 */
    if (r5 < 0x100u)                           /* CMP R5,#256 ; BCC SWAP2 */
        goto SWAP2;
    ros_subs(s, r9, r5);                       /* CMP TYPE,R5 */
    if (r9 != r5)                              /* BNE ERTYPESWAP */
        da_msg_at(s, SITE_ERTYPESWAP);
    r1 = ros_ld32(r0);                         /* LDR R1,[IACC] */
    r2 = ros_ld32(r4);                         /* LDR R2,[R4] */
    ros_st32(r4, r1);                          /* STR R1,[R4] */
    ros_st32(r0, r2);                          /* STR R2,[IACC] */
    s->r[1] = r1; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* The string-part store shared by LLEFTD, LMIDD and LRIGHTD
 * (LRIGHTD2/LMIDD2, Stmt2.s:247-267).  It overwrites at most R5 bytes
 * of the string whose 5-byte block is at R6, from the start position.
 * It stops at the end of either string. */
static void da_lrightd2(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r2 = s->r[2], r4 = s->r[4], r5 = s->r[5],
             r6 = s->r[6], r7 = s->r[7], r8 = s->r[8], r13 = s->r[13];
    uint32_t v;

    v = r6 & 3;                                /* ANDS R1,R6,#3 */
    r7 = r6 & ~3u;                             /* BIC R7,R6,#3 */
    if (v == 0)
        r0 = ros_ld32(r6);                     /* LDREQ IACC,[R6] */
    if (v != 0) {
        r0 = ros_ld32(r7);
        r7 = ros_ld32(r7 + 4);
    }
    if (v != 0)                                /* the misaligned glue */
        r0 = ros_lsr(r0, v << 3) | ros_lsl(r7, 32 - (v << 3));
    if (r5 - 1 >= 0xFFu)                       /* CMP R1,#255 ; MOVCS R5,#1 */
        r5 = 1;
    v = ros_ld8(r6 + 4);                       /* LDRB R1,[R6,#4] ; length */
    if (r5 > v) {                              /* CMP R5,R1 ; BHI NXT */
        s->r[0] = r0; s->r[1] = v; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[7] = r7; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    v = r0 + v;                                /* ADD R1,IACC,R1 */
    r7 = r0 + r5 - 1;                          /* ADD R7,IACC,R5,#-1 */
    r5 = r8 - 1536;                            /* ADD R3,ARGP,#STRACC */
LMIDD2:
    {
        uint32_t b = ros_ld8(r5);              /* LDRB R5,[R3],#1 */
        r5 += 1;
        ros_st8(r7, b);                        /* STRB R5,[R7],#1 */
        r7 += 1;
        if (r5 == r2) {                        /* TEQ R3,CLEN ; BEQ NXT */
            s->r[0] = r0; s->r[1] = v; s->r[3] = r5; s->r[4] = r4;
            s->r[5] = b; s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_hand_NXT(s);
            return;
        }
        if (r7 >= v) {                         /* CMP R7,R1 ; BCS NXT */
            s->r[0] = r0; s->r[1] = v; s->r[3] = r5; s->r[4] = r4;
            s->r[5] = b; s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
            s->r[15] = s->r[14];
            basicvfp_hand_NXT(s);
            return;
        }
        uint32_t old = r4;                     /* SUBS R4,R4,#1 */
        r4 -= 1;
        if (old > 1)                           /* BHI LMIDD2 */
            goto LMIDD2;
        s->r[0] = r0; s->r[1] = v; s->r[3] = r5; s->r[4] = r4;
        s->r[5] = b; s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
    }
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* LMIDD1 (Stmt2.s:232-246), shared by LLEFTD and LMIDD.  After the
 * arguments, the ')' and the '=', the right side must be a string.
 * Then the shared store follows. */
static void da_lmidd1(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    r13 -= 4;                                  /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    ros_subs(s, r10, 41);                      /* CMP R10,#")" */
    if (r10 != 41) {                           /* BNE ERBRA */
        s->r[0] = r0; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERBRA(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    ros_subs(s, r10, 61);                      /* CMP R10,#"=" */
    if (r10 != 61) {                           /* BNE MISSEQ */
        s->r[0] = r0; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_MISSEQ(s);
        return;
    }
    s->r[0] = r0;                              /* BL EXPR */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                    /* TEQ TYPE,#0 */
    if (r9 != 0) {                             /* BNE ERTYPESTR */
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                    /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R5,R6} */
    r5 = ros_ld32(r13 + 4);
    r6 = ros_ld32(r13 + 8);
    r13 += 12;
    r1 = r8 - 1536;                            /* ADD R1,ARGP,#STRACC */
    if (r2 == r1) {                            /* CMP CLEN,R1 ; BEQ NXT */
        s->r[1] = r1; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_lrightd2(s);                            /* LRIGHTD2 */
    return;
}

/* LLEFTD (Stmt2.s:202-215): LEFT$(A$[,N]) = S$.  The maximum length
 * defaults to 255, and the start position to 1. */
void basicvfp_hand_LLEFTD(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0;                              /* BL AELV */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ FACERR */
        s->r[15] = s->r[14];
        basicvfp_FACERR(s);
        return;
    }
    ros_subs(s, r9, 128);                      /* CMP TYPE,#128 */
    if (r9 != 128)                             /* BNE ERTYPESTRING */
        da_msg_at(s, SITE_ERTYPESTRING);
    ros_st32(r13 - 4, r0);                     /* STR IACC,[SP,#-4]! */
    r0 = 1;                                    /* MOV IACC,#1 */
    r13 -= 8;                                  /* STR IACC,[SP,#-4]! */
    ros_st32(r13, 1);
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    r0 = 0xFFu;                                /* MOV IACC,#255 */
    if (r10 == 44)                             /* CMP R10,#"," */
        goto lleftd_expr;
    goto lleftd_done;
lleftd_expr:
    s->r[0] = r0;                              /* BL EXPR */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
lleftd_done:                                   /* B LMIDD1 */
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_lmidd1(s);
    return;
}

/* LMIDD (Stmt2.s:216-266): MID$(A$,P[,N]) = S$. */
void basicvfp_hand_LMIDD(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0;                              /* BL AELV */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ FACERR */
        s->r[15] = s->r[14];
        basicvfp_FACERR(s);
        return;
    }
    ros_subs(s, r9, 128);                      /* CMP TYPE,#128 */
    if (r9 != 128)                             /* BNE ERTYPESTRING */
        da_msg_at(s, SITE_ERTYPESTRING);
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    ros_subs(s, r10, 44);                      /* CMP R10,#"," */
    if (r10 != 44) {                           /* BNE ERCOMM */
        s->r[15] = s->r[14];
        basicvfp_ERCOMM(s);
        return;
    }
    r13 -= 4;                                  /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                            /* BL EXPR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r13 -= 4;                                  /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    r0 = 0xFFu;                                /* MOV R0,#255 */
    if (r10 == 44)                             /* CMP R10,#"," */
        goto lmidd_expr2;
    goto lmidd_after;
lmidd_expr2:
    s->r[0] = r0;                              /* BL EXPR */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
lmidd_after:                                   /* LMIDD1 */
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    da_lmidd1(s);
    return;
}

/* LRIGHTD (Stmt2.s:268-299): RIGHT$(A$[,N]) = S$. */
void basicvfp_hand_LRIGHTD(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    s->r[0] = r0;                              /* BL AELV */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                /* BEQ FACERR */
        s->r[15] = s->r[14];
        basicvfp_FACERR(s);
        return;
    }
    ros_subs(s, r9, 128);                      /* CMP TYPE,#128 */
    if (r9 != 128)                             /* BNE ERTYPESTRING */
        da_msg_at(s, SITE_ERTYPESTRING);
    r13 -= 4;                                  /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    r0 = 0xFFu;                                /* MOV IACC,#255 */
    if (r10 == 44)                             /* CMP R10,#"," */
        goto lrightd_expr;
    goto LRIGHTD1;
lrightd_expr:
    s->r[0] = r0;                              /* BL EXPR */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
LRIGHTD1:
    r13 -= 4;                                  /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    ros_subs(s, r10, 41);                      /* CMP R10,#")" */
    if (r10 != 41) {                           /* BNE ERBRA */
        s->r[0] = r0; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_ERBRA(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                    /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10];
    r11 = s->r[11];
    r14 = s->r[14];
    ros_subs(s, r10, 61);                      /* CMP R10,#"=" */
    if (r10 != 61) {                           /* BNE MISSEQ */
        s->r[0] = r0; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_MISSEQ(s);
        return;
    }
    s->r[0] = r0;                              /* BL EXPR */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                    /* TEQ TYPE,#0 */
    if (r9 != 0) {                             /* BNE ERTYPESTR */
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                    /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r4 = ros_ld32(r13);                        /* LDMFD SP!,{R4,R6} */
    r6 = ros_ld32(r13 + 4);
    r13 += 8;
    r1 = r8 - 1536;                            /* ADD R1,ARGP,#STRACC */
    r0 = r2 - r1;                              /* SUBS R0,CLEN,R1 */
    if (r0 == 0) {                             /* BEQ NXT */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[6] = r6;
        s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    if (r4 >= r0)                              /* CMP R4,R0 ; MOVCS R4,R0 */
        r4 = r0;
    v = ros_ld8(r6 + 4);                       /* LDRB R5,[R6,#4] */
    r5 = v - r4 + 1;                           /* SUBS/ADD R5 */
    if (v >= r4) {                             /* BCS LRIGHTD2 */
        da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                     r12, r13, r14);
        da_lrightd2(s);
        return;
    }
    s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* da_llomem_rom: LLOMEMROM (Stmt2.s:146-173), the store that both
 * LPAGE's ROM range and LLOMEM fall into.  LOMEM and FSA move
 * together, the free list is discarded, and SETVAL resets the
 * variables above LOMEM. */
static void da_llomem_rom(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r8 = s->r[8], r13 = s->r[13],
             r14 = s->r[14];

    r0 = (r0 + 3) & ~3u;                       /* ADD/BIC IACC */
    r1 = ros_ld32(r8 - 12);                    /* LDR R1,[ARGP,#FREEPTR] */
    ros_subs(s, r0, r1);                       /* CMP IACC,R1 */
    if (r0 < r1)                               /* BCC LLOMEMOUT */
        goto out;
    r1 = ros_ld32(r8 - 84);                    /* LDR R1,[ARGP,#MEMLIMIT] */
    ros_subs(s, r0, r1);                       /* CMP IACC,R1 */
    if (r0 >= r1)                              /* BCS LLOMEMOUT */
        goto out;
    ros_st32(r8 - 136, r0);                    /* STR IACC,[ARGP,#LOMEM] */
    ros_st32(r8 - 140, r0);                    /* STR IACC,[ARGP,#FSA] */
    ros_st32(r8 - 768, 0);                     /* STR 0,[ARGP,#FREELIST] */
    s->r[0] = 0;                               /* MOV R0,#0 */
    s->r[1] = r1;                              /* BL SETVAL */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SETVAL(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r14 = s->r[14];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

out:                                           /* LLOMEMOUT */
    s->r[0] = 5;                               /* MOV R0,#5 */
    s->r[1] = r1;                              /* BL MSGPRNXXX */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_MSGPRNXXX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r13 = s->r[13]; r14 = s->r[14];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* LPAGE (Stmt2.s:54-86). */
void basicvfp_hand_LPAGE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r3, r4, r5, r6, r7,
             r8 = s->r[8], r9, r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];

    s->r[0] = r0;                              /* BL EQAEEX */
    s->r[1] = r1;
    s->r[4] = s->r[4];
    s->r[7] = s->r[7];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EQAEEX(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 = (r0 + 3) & ~3u;                       /* ADD/BIC IACC */
    r1 = ros_ld32(r8 - 12);                    /* LDR R1,[ARGP,#FREEPTR] */
    ros_subs(s, r0, r1);                       /* CMP IACC,R1 */
    if (r0 < r1)                               /* BCC LPAGEOUT */
        goto LPAGEOUT;
    if (r0 >= 0x1800000u) {                    /* CMP IACC,#&1800000 ; BCS ROM */
        ros_st32(r8 - 148, r0);                /* STR IACC,[ARGP,#PAGE] */
        s->r[0] = ros_ld32(r8 - 12);           /* LDR IACC,[ARGP,#FREEPTR] */
        da_llomem_rom(s);                      /* B LLOMEMROM */
        return;
    }
    r1 = ros_ld32(r8 - 84);                    /* LDR R1,[ARGP,#MEMLIMIT] */
    ros_subs(s, r0, r1);                       /* CMP IACC,R1 */
    if (r0 < r1)                               /* STRCC IACC,[ARGP,#PAGE] */
        ros_st32(r8 - 148, r0);
    if (r0 < r1) {                             /* BCC NXT */
        s->r[0] = r0; s->r[1] = r1;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
LPAGEOUT:                                      /* Stmt2.s:71-86 */
    s->r[0] = 4;                               /* MOV R0,#4 */
    s->r[1] = r1;                              /* BL MSGPRNXXX */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_MSGPRNXXX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r13 = s->r[13]; r14 = s->r[14];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* LLOMEM (Stmt2.s:145-173). */
void basicvfp_hand_LLOMEM(struct ros_cpu *s)
{
    s->r[14] = 0xFFFFFFF0u;                    /* BL EQAEEX */
    basicvfp_EQAEEX(s);
    da_llomem_rom(s);                          /* LLOMEMROM */
    return;
}

/* LTIME (Stmt2.s:88-143).  TIME = sets the time in centiseconds.
 * TIME$ = sets the string form, with the OS_Word reason code deduced
 * from it. */
void basicvfp_hand_LTIME(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    r10 = ros_ld8(r12);                        /* LDRB R10,[LINE] */
    if (r10 == 36)                             /* CMP R10,#"$" ; BEQ LTIMED */
        goto LTIMED;
    s->r[0] = r0;                              /* BL EQAEEX */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EQAEEX(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = r8 - 1536;                            /* ADD R1,ARGP,#STRACC */
    ros_st32(r1, r0);                          /* STR IACC,[R1] */
    ros_st32(r1 + 4, 0);                       /* STR R0,[R1,#4] */
    r0 = OsWord_WriteSystemClock;              /* MOV R0,#2 */
    s->r[0] = r0;                              /* SWI OS_Word */
    s->r[1] = r1;
    ros_native_swi(s, ros_thunk_OS_Word);
    if (s->v)
        ros_swi_raise(s);
    r0 = s->r[0];
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

LTIMED:                                        /* Stmt2.s:105-143 */
    r12 += 1;                                  /* ADD LINE,LINE,#1 */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 61);                      /* CMP R10,#"=" */
    if (r10 != 61) {                           /* BNE MISSEQ */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_MISSEQ(s);
        return;
    }
    s->r[0] = r0;                              /* BL AEEXPR */
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
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                    /* TEQ TYPE,#0 */
    if (r9 != 0) {                             /* BNE ERTYPESTR */
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                    /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    ros_st8(r2, r9);                           /* STRB TYPE,[CLEN] */
    r1 = r8 - 1536;                            /* ADD R1,ARGP,#STRACC */
    r0 = r2 - r1;                              /* SUB R0,CLEN,R1 */
    ros_subs(s, r0, 8);                        /* CMP R0,#8 */
    if (r0 <= 8)                               /* MOVLS R0,#8 */
        r0 = 8;
    if (ros_cond(s, ROS_LS))                   /* BLS LTIMED1 */
        goto LTIMED1;
    r13 -= 12;                                 /* STMFD SP!,{R0-R2} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r1);
    ros_st32(r13 + 8, r2);
    r2 = ros_ld32(r8 - 140);                   /* LDR R2,[ARGP,#FSA] */
    r1 = r2;                                   /* MOV R1,R2 ; dummy */
    r0 = 0xFFFFFFFFu;                          /* MOV R0,#-1 */
    s->r[0] = r0;                              /* SWI Territory_Read... */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[13] = r13;
    ros_swi(s, 0x4305Fu);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r7 = 7 + ros_ld32(r2 + 24) + ros_ld32(r2 + 28) + ros_ld32(r2 + 40);
    v = r13;                                   /* LDMFD SP!,{R0-R2} */
    r1 = ros_ld32(r13 + 4);
    r2 = ros_ld32(r13 + 8);
    r13 += 12;
    ros_subs(s, ros_ld32(v), r7);              /* CMP R0,R7 */
    r0 = ros_cond(s, ROS_LS) ? 0xFu : 24;      /* MOVLS #15 ; MOVHI #24 */
LTIMED1:
    r1 -= 1;                                  /* STRB R0,[R1,#-1]! */
    ros_st8(r1, r0);
    r0 = OsWord_WriteRealTimeClock;            /* MOV R0,#15 */
    s->r[0] = r0;                              /* SWI OS_Word */
    s->r[1] = r1;
    s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_Word);
    if (s->v)
        ros_swi_raise(s);
    r0 = s->r[0];
    s->r[2] = r2; s->r[7] = r7;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* LHIMEM (Stmt2.s:174-201): the stack is rebuilt at the new HIMEM
 * with the ten-word stop frame, and the DIM LOCAL list is unwound. */
void basicvfp_hand_LHIMEM(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2, r3, r4, r5, r6, r7,
             r8 = s->r[8], r9, r10, r11, r12, r13 = s->r[13],
             r14 = s->r[14];

    s->r[0] = r0;                              /* BL EQAEEX */
    s->r[1] = r1;
    s->r[4] = s->r[4];
    s->r[10] = s->r[10];
    s->r[11] = s->r[11];
    s->r[12] = s->r[12];
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EQAEEX(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 &= ~3u;                                 /* BIC IACC,IACC,#3 */
    r1 = ros_ld32(r8 - 12);                    /* LDR R1,[ARGP,#FREEPTR] */
    ros_subs(s, r0, r1);                       /* CMP IACC,R1 */
    if (r0 < r1)                               /* BCC LHIMEMOUT */
        goto LHIMEMOUT;
    r1 = ros_ld32(r8 - 84);                    /* LDR R1,[ARGP,#MEMLIMIT] */
    ros_subs(s, r0, r1);                       /* CMP IACC,R1 */
    if (r0 > r1)                               /* BHI LHIMEMOUT */
        goto LHIMEMOUT;
    ros_st32(r8 - 132, r0);                    /* STR IACC,[ARGP,#HIMEM] */
    s->r[0] = r0;                              /* BL POPLOCALAR */
    s->r[1] = r1;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_POPLOCALAR(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r14 = s->r[14];
    r13 = ros_ld32(r8 - 132);                  /* LDR SP,[ARGP,#HIMEM] */
    r0 = 0;                                    /* MOV R0,#0 */
    r13 -= 40;                                 /* STMFD SP!,{R0-R9} */
    ros_st32(r13, 0);
    ros_st32(r13 + 4, r1);
    ros_st32(r13 + 8, r2);
    ros_st32(r13 + 12, r3);
    ros_st32(r13 + 16, r4);
    ros_st32(r13 + 20, r5);
    ros_st32(r13 + 24, r6);
    ros_st32(r13 + 28, r7);
    ros_st32(r13 + 32, r8);
    ros_st32(r13 + 36, r9);
    ros_st32(r8 - 120, r13);                   /* STR SP,[ARGP,#ERRSTK] */
    s->r[0] = r0; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

LHIMEMOUT:                                     /* Stmt2.s:193-201 */
    s->r[0] = 6;                               /* MOV R0,#6 */
    s->r[1] = r1;                              /* BL MSGPRNXXX */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_MSGPRNXXX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r13 = s->r[13]; r14 = s->r[14];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[13] = r13;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* ASSIGNAT (Stmt2.s:301-380): @% = n, @% += n, or @% = format string
 * (G/E/F, '+', width, dot/comma, decimals). */
void basicvfp_hand_ASSIGNAT(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v;

    r10 = ros_ld8(r12);                        /* LDRB R10,[LINE],#1 */
    r12 += 1;
    ros_subs(s, r10, 37);                      /* CMP R10,#"%" */
    if (r10 != 37) {                           /* BNE ERSYNT */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    r10 = ros_ld8(r12);                        /* LDRB R10,[LINE],#1 */
    r12 += 1;
    ros_subs(s, r10, 40);                      /* CMP R10,#"(" */
    if (r10 == 40) {                           /* BEQ ERSYNT */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12;
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    if (r10 == 32)                             /* CMP R10,#" " */
        basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BLEQ SPACES */
    ros_subs(s, r10, 45);                      /* CMP R10,#"-" */
    v = r10 ^ 0x2Bu;                           /* TEQCC R10,#"+" */
    if (r10 < 45 ? v == 0 : r10 == 45) {
        r5 = 4;                                /* MOVEQ R5,#4 */
        r4 = r8 - 0x100u;                      /* ADDEQ R4,ARGP,#INTVAR */
        ros_st32(r13 - 8, r4);                 /* STMEQFD SP!,{R4,R5} */
        ros_st32(r13 - 4, r5);
        r13 -= 8;
        r11 = r12;                             /* MOVEQ AELINE,LINE */
        da_store_all(s, r0, r1, s->r[2], s->r[3], r4, r5, s->r[6],
                     s->r[7], r8, s->r[9], r10, r11, r12, r13, r14);
        da_atgotlt2(s);                        /* BEQ ATGOTLT2 */
        return;
    }
    ros_subs(s, r10, 61);                      /* CMP R10,#"=" */
    if (r10 != 61) {                           /* BNE MISSEQ */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[5] = r5;
        s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        basicvfp_MISSEQ(s);
        return;
    }
    s->r[0] = r0;                              /* BL AEEXPR */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                    /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (r9 == 0)                               /* TEQ TYPE,#0 */
        goto ASSIGNATSTRING;
    if ((int32_t)r9 < 0) {                     /* BLMI INTEGB */
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_INTEGB(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
        r14 = s->r[14];
    }
    ros_st32(r8 - 0x100u, r0);                 /* STR IACC,[ARGP,#INTVAR] */
    da_store_all(s, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11,
                 r12, r13, r14);
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;

ASSIGNATSTRING:                                /* Stmt2.s:326-380 */
    ros_st8(r2, r9);                           /* STRB TYPE,[CLEN],#1 */
    r2 += 1;
    v = ros_ld8(r8 - 1536);                    /* LDRB R5,[R4],#1 */
    r5 = v;
    ros_st8(r8 - 253, r5 == 43);               /* STRB,[ARGP,#INTVAR+3] */
    if (r5 == 43)
        r5 = ros_ld8(r8 - 1535);               /* LDREQB R5,[R4],#1 */
    r4 = v == 43 ? r8 - 1534 : r8 - 1535;
    r6 = r5 & ~0x20u;                          /* BIC R6,R5,#&20 */
    r0 = ros_ld8(r8 - 254) | 3;                /* LDRB/ORR R0,#3 */
    if (r6 == 71)                              /* CMP R6,#"G" */
        r0 -= 1;
    if (r6 == 71 || r6 == 69)                  /* CMPNE R6,#"E" */
        r0 -= 1;
    if (r6 == 71 || r6 == 69 || r6 == 70) {    /* CMPNE R6,#"F" */
        r0 -= 1;
        ros_st8(r8 - 254, r0);                 /* STREQB R0,[ARGP,#INTVAR+2] */
        r5 = ros_ld8(r4);                      /* LDREQB R5,[R4],#1 */
        r4 += 1;
    }
    if (r5 == 46 || r5 == 44)                  /* CMP R5,#"." ; CMPNE #"," */
        goto ASSIGNATDOT;
    basicvfp_READNUM(s, &r0, &r4, &r5, &r6, 0xFFFFFFF0u); /* BL READNUM */
    if (!s->c) {                               /* BCC NXT ; read garbage! */
        s->r[0] = r0; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[14] = r14;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    ros_st8(r8 - 0x100u, r0);                  /* STRB R0,[ARGP,#INTVAR] */
    if (r5 != 46 && r5 != 44) {                /* CMP/CMPNE */
        s->r[0] = r0; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
        s->r[14] = r14;
        s->r[15] = s->r[14];                   /* B NXT */
        basicvfp_hand_NXT(s);
        return;
    }
ASSIGNATDOT:                                   /* Stmt2.s:355-380 */
    r0 = ros_ld8(r8 - 254) & ~0x80u;           /* LDRB/BIC R0,#&80 */
    if (r5 == 44)                              /* CMP R5,#"," */
        r0 |= 128;                             /* ORREQ R0,R0,#&80 */
    ros_st8(r8 - 254, r0);                     /* STRB R0,[ARGP,#INTVAR+2] */
    r5 = ros_ld8(r4);                          /* LDRB R5,[R4],#1 */
    r4 += 1;
    basicvfp_READNUM(s, &r0, &r4, &r5, &r6, 0xFFFFFFF0u); /* BL READNUM */
    if (s->c)                                  /* STRCSB R0,[INTVAR+1] */
        ros_st8(r8 - 0xFFu, r0);
    s->r[0] = r0; s->r[2] = r2; s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    s->r[14] = r14;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

/* LEXT/LPTR (Stmt2.s:1739-1750): EXT#f = n (OS_Args 3) and PTR#f = n
 * (OS_Args 1). */
static void da_lptra(struct ros_cpu *s, uint32_t a0)
{
    uint32_t r0 = a0, r1 = s->r[1], r3 = s->r[3], r4 = s->r[4],
             r5 = s->r[5], r6 = s->r[6], r7 = s->r[7], r8 = s->r[8],
             r9 = s->r[9], r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14 = s->r[14];
    uint32_t r2;

    r13 -= 4;                                  /* STR R0,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[0] = r0;                              /* BL AECHAN */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AECHAN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13];
    r13 -= 4;                                  /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    r12 = r11;                                 /* MOV LINE,AELINE */
    s->r[12] = r12;                            /* BL EQAEEX */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EQAEEX(s);
    r0 = s->r[0]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r2 = r0;                                   /* MOV R2,IACC */
    r1 = ros_ld32(r13);                        /* LDR R1,[SP],#4 */
    r0 = ros_ld32(r13 + 4);                    /* LDR R0,[SP],#4 */
    r13 += 8;
    s->r[0] = r0;                              /* SWI OS_Args */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_Args);
    if (s->v)
        ros_swi_raise(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r5 = s->r[5];
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[5] = r5;
    s->r[15] = s->r[14];                       /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}

void basicvfp_hand_LPTR(struct ros_cpu *s)
{
    da_lptra(s, 1);                            /* MOV R0,#1 */
    return;
}

void basicvfp_hand_LEXT(struct ros_cpu *s)
{
    da_lptra(s, 3);                            /* MOV R0,#3 */
    return;
}
