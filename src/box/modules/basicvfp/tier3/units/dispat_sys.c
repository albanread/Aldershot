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
 * (Sources/Programmer/BASIC: s.Basic, s.Funct, s.Stmt, s.Stmt2, hdr.Definitions, hdr.Tokens).
 */

/* dispat_sys.c: the system statements of the statement executor,
 * translated by hand from the ObjAsm source of RISC OS 5.31 BASIC VFP.
 * The statements are:
 *   - DOSTAR (Basic.s:708-716), the *command statement
 *   - FNRET (Funct.s:525-543), the '=' statement that returns from an
 *     FN, with FNRETA and FNRETP
 *   - OTHER and WHEN (Stmt.s:127-147), the skip scan of the CASE
 *     OTHERWISE arm
 *   - ENVEL (Stmt.s:461-479)
 *   - REPORT (Stmt.s:1317-1327)
 *   - WIDTH (Stmt.s:1713-1716)
 *   - LERROR and LERROREXT (Stmt2.s:18-48)
 *   - DOMOUSE and its seven forms (Stmt2.s:1348-1523): the plain
 *     three- or four-variable form, RECT, STEP, TO, OFF, ON and COLOUR
 *   - ORGIN (Stmt2.s:1526-1538)
 *   - OVERLAY (Stmt2.s:1688-1737)
 *   - BBPUT and BBPUT1 (Stmt2.s:1752-1782)
 *   - LIBRARY and NOLIBCRUNCH (Stmt2.s:1965-1992)
 *   - OSCL (Stmt2.s:2019-2033)
 *   - SYS and its machinery (Stmt2.s:2034-2218): SYSNAME, SYS0STRING,
 *     SYSGOTSWINUMB, SYS0SPACES, SYS0PUSH, SYS0END, SYSCALL,
 *     SYSCALLTO, SYS1 with its STRING, END, ENDA, SPACES and COMMA
 *     exits, and SYSEXIT
 *   - CALL with CALLARM, CALLPARM and CALLGO (Stmt2.s:2222-2252)
 *   - INSTALLBAD (the MSG site at &FC10FEAC).
 *
 * The conventions are the ones the landed units use:
 *   - State is in R[].
 *   - A BL becomes a marker call, with R14 set to 0xFFFFFFF0u. The
 *     sets of registers stored and reloaded are copied one for one
 *     from the lift, so the ARM register file at every boundary is
 *     the same as the lift's.
 *   - A B becomes a plain call whose result is discarded.
 *   - Routines that use setjmp (EXPR, AEEXPR, EXPRDN, AEEXDN, INTEXA,
 *     INTEXC, CRAELV, AELV, AECHAN) are plain calls with no resume
 *     points. The one exception is GTARGS. It returns non-locally
 *     through its stored link and needs the same setjmp helper as
 *     ENDPR (fnret_gtargs below, at the lift's own site &FC104A60).
 *
 * The quirks of the original that are kept, with their lines:
 *   - DOSTAR and OSCL run OS_CLI with MEMM = 3. That sets both the
 *     memory-may-move bit and the r12-relocation bit. LINE is saved
 *     in R12STORE. So the CLI can move the variables and fix ARGP
 *     through the r12 the environment was told about. MEMM is cleared
 *     on return (Basic.s:708-716, Stmt2.s:2019-2033). DOSTAR exits to
 *     REM (which is DATA) with R14 = 0. OSCL exits to NXT.
 *   - SYS builds `SWI n ; MOV pc,lr` in the SWICODE area, with the
 *     SWI word as (number & &00FFFFFF) | &EF000000. It runs the code
 *     by MOV PC,AELINE with R14 set to PC. This is the runtime's
 *     protocol for a SWI in the arena (dispatch.c ram_swi). The SWI's
 *     MOV pc,lr returns to the marker at &FC107CD0 or &FC107CF4
 *     (Stmt2.s:2125-2143).
 *   - After the SWI, ARGP is reloaded with the constant VARS (&8700)
 *     before MEMM is cleared. This is because the SWI may have moved
 *     the variables away from the old ARGP (Stmt2.s:2136-2139, the
 *     lift's folded STRB at &86E7).
 *   - SYSRELSTK: the parameter stack is released by a delta word, not
 *     by an absolute SP. The initial marker is 8, with the old SP kept
 *     in the R9/TYPE slot. A string parameter adds its stack usage to
 *     the marker while the 13 words are parked at the FSA
 *     (SYS0STRING, Stmt2.s:2040-2066, the STMIA/LDMDB shuffle).
 *   - SYS's string result is copied until CR, NUL, LF or the 256-byte
 *     limit of STRACC (SYS1STRING, Stmt2.s:2172-2181). At the limit
 *     the length is reduced by 255, which is the whole limit block
 *     less one byte. The unconditional SUB CLEN,CLEN,#1 always drops
 *     the terminator. So a result string is at most 255 bytes and
 *     never includes its terminator.
 *   - SYS TO stores the SWI's PSR >> 28 (nzcv) as a TINTEGER
 *     (Stmt2.s:2199-2206). An empty TO slot gives ERSYNT through
 *     SYS1COMMA (Stmt2.s:2215-2218).
 *   - CALL and USR share one body. The FPOINT=0 EMUMOS branch is not
 *     in the VFP build. CALL stacks {IACC,TYPE} l-value pairs, and R5
 *     counts them from 0. R5 is incremented before each push
 *     (Stmt2.s:2237-2248). CALLARMROUT stays as lift code. It builds
 *     the A%-Z% block and jumps through the CALL2 protocol.
 *   - FNRET saves R5 in R14 around ENDTRC, because ENDTRC corrupts R5
 *     (Funct.s:532-536). It unwinds through GTARGS with TYPE as AEEXPR
 *     and AEDONE left it. The FN's value is carried on the value
 *     stack, not in the registers. FNRET sets the flags from TYPE
 *     (string or not) for the code it resumes, and leaves by
 *     MOV PC,R7. R7 is the return address that GTARGS popped from the
 *     frame (Funct.s:537-543).
 *   - DOMOUSE's three-variable form stores the OS_Mouse results in the
 *     reverse of the push order. R2 (the buttons or switches) goes
 *     into the last l-value pushed, then R1 (y), then R0 (x). This is
 *     because STORE takes the stacked pairs from the top down
 *     (DOMOUSESTORE, Stmt2.s:1394-1403).
 *   - MOUSE ON and MOUSE OFF both end in OS_Byte 106, with IACC 1 or
 *     0. ON first reads an optional expression. A ':', CR or TELSE
 *     after ON means there is none (Stmt2.s:1488-1504).
 *   - MOUSE RECT truncates the coordinates to 16 bits. It uses AND
 *     &FF00 masks built by MOV R0,#&FF ; ORR R0,R0,#&FF00. It packs
 *     the words xlo|ylo<<16 and xhi|yhi<<16 after a reason byte of 1
 *     at STRACC+3 (Stmt2.s:1422-1451).
 *   - MOUSE STEP packs the x step as IACC<<8 (masked with &FF00) and
 *     the y step in bits 16-23, around reason 2 (Stmt2.s:1463-1473).
 *   - MOUSE TO writes its two packed words at SP and passes R1 = SP+1.
 *     So the OS_Word block is not word-aligned (Stmt2.s:1474-1487).
 *   - MOUSE COLOUR shares PROGPAL with COLOUR. PROGPAL sends the
 *     palette sequence VDU 19,r3,25,r2,r1,iacc (Stmt2.s:1506-1523,
 *     which enters Stmt2.s:486-502). It is translated here as the
 *     private static dsys_progoal. This is the only Group D body that
 *     reaches into Group C's code. The lift's PROGPAL stays for COLOUR
 *     until Group C lands.
 *   - ENVEL pushes R1, the byte index, onto the stack before every
 *     INTEXC and pops it straight back. The push and pop pair is the
 *     original's and is kept (Stmt.s:461-470). The 14th byte comes
 *     from EXPRDN. All the bytes are written into a 16-byte block on
 *     the stack, which is passed to OS_Word 8 (Stmt.s:471-479).
 *   - REPORT prints the ERRORS text one byte at a time through CHOUT
 *     until the NUL (Stmt.s:1317-1327).
 *   - WIDTH stores the value less 1 into WIDTHLOC. The printing code
 *     adds the 1 back (Stmt.s:1713-1716).
 *   - LERROR builds its error block from the string pushed by SPUSH.
 *     The error number is written over the length word at [SP], and
 *     MSGERR is given the block in R14 = SP (Stmt2.s:18-33). ERROR EXT
 *     restores OLDERR through PUTBACKHAND, sets CALLEDNAME to 42 (the
 *     '*'), and raises the error with OS_GenerateError. It then calls
 *     OS_Exit, which never returns (Stmt2.s:35-48).
 *   - OVERLAY finds the size of every element by adding a CR to it and
 *     calling OSFILEINFOSTRACC. So an empty file still costs 14 bytes
 *     (13+1). It claims FSA + 12 + aligned(max) + 1024 below SP, or
 *     raises BADDIMSIZE. It forgets PROCPTR and FNPTR only when
 *     overlays already existed. It writes the three-word header
 *     {array base, -1 (no current overlay), size}
 *     (Stmt2.s:1700-1737).
 *   - BBPUT with a string value writes the whole of STRACC through
 *     OS_GBPB 2, without the terminator (CLEN-1). It writes nothing at
 *     all when the string is empty. The byte form adds LF (10) to
 *     STRACC unless the next token is ';'. A ';' means AESPAC, so
 *     BPUT#ch ;"..." writes the string as it is (BBPUT1,
 *     Stmt2.s:1765-1782).
 *   - LIBRARY links the loaded block onto LIBRARYLIST. Only when
 *     CRUNCHCHK says the library was not safely crunched does it
 *     crunch it again with SAFECRUNCH (15) and round the FSA again.
 *     NOLIBCRUNCH adds the raw length instead (Stmt2.s:1965-1992).
 *   - OTHER (alias WHEN, tokens 127 and 201) scans the line headers.
 *     Starting from a nesting count of 1, it counts an OF at the end
 *     of a line up and an ENDCASE at the start of a line down. It
 *     carries on after the closing ENDCASE through DONXTS
 *     (Stmt.s:127-147). NOENDC at &FC110154 is also the error CASE
 *     uses.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "dispat_sys.h"

/* The FNRET fix in f93918e jumps to the frame that the pair (site, sp)
 * names. It uses ros_resume_point_at, part of the resume machinery in
 * the newer rosgd. The older trees, such as the Mac Pro's box-build,
 * do not have it. There the weak reference stays null and the jump is
 * skipped. The value then returns through the C stack, which is
 * exactly the behaviour those trees were verified with. */
extern int ros_resume_point_at(uint32_t at, uint32_t sp) __attribute__((weak));

/* ---- the lift's functions this unit calls --------------------------
 * (The manifest's EXPORTS list makes them non-static in the twin. The
 * new ones this unit needs are listed in the report.) */
void basicvfp_ERSYNT(struct ros_cpu *s);
void basicvfp_ERTYPESTR(struct ros_cpu *s);
void basicvfp_ERCOMM(struct ros_cpu *s);
void basicvfp_ERARRY(struct ros_cpu *s);
void basicvfp_ERARRZ(struct ros_cpu *s);
void basicvfp_ERTYPESTRINGARRAY(struct ros_cpu *s);
void basicvfp_EXPR(struct ros_cpu *s);
void basicvfp_AEEXPR(struct ros_cpu *s);
void basicvfp_AEEXDN(struct ros_cpu *s);
void basicvfp_EXPRDN(struct ros_cpu *s);
void basicvfp_INTEXA(struct ros_cpu *s);
void basicvfp_INTEXC(struct ros_cpu *s);
void basicvfp_CRAELV(struct ros_cpu *s);
void basicvfp_AECHAN(struct ros_cpu *s);
void basicvfp_ENDTRC(struct ros_cpu *s);
void basicvfp_POPA(struct ros_cpu *s);
void basicvfp_MSGERR(struct ros_cpu *s);
void basicvfp_NLINE(struct ros_cpu *s);
void basicvfp_CHOUT(struct ros_cpu *s);
void basicvfp_WRITEG(struct ros_cpu *s);
void basicvfp_STOREA(struct ros_cpu *s);
void basicvfp_VARSTR(struct ros_cpu *s);
void basicvfp_GETARRAYSIZE1(struct ros_cpu *s);
void basicvfp_OSFILEINFOSTRACC(struct ros_cpu *s);
void basicvfp_LIBSUB(struct ros_cpu *s);
void basicvfp_PUTBACKHAND(struct ros_cpu *s);
void basicvfp_CALLARMROUT(struct ros_cpu *s);
/* The merged-register signatures the lifter gave three of them. */
void basicvfp_OSCLIREGS(struct ros_cpu *s, uint32_t *p1, uint32_t *p2,
                        uint32_t *p3, uint32_t *p4, uint32_t *p5,
                        uint32_t r8, uint32_t r12, uint32_t r13,
                        uint32_t r14);
void basicvfp_CRUNCHCHK(struct ros_cpu *s, uint32_t r0, uint32_t r1,
                        uint32_t r2, uint32_t r3, uint32_t r4,
                        uint32_t r13, uint32_t r14);
void basicvfp_CRUNCHROUTINE(struct ros_cpu *s, uint32_t r0, uint32_t *p1,
                            uint32_t *p2, uint32_t *p3, uint32_t *p4,
                            uint32_t *p5, uint32_t *p6, uint32_t *p7,
                            uint32_t *p10, uint32_t r14);

/* The hand units' functions (landed). */
void basicvfp_hand_STMT(struct ros_cpu *s);
void basicvfp_hand_DATA(struct ros_cpu *s);
void basicvfp_hand_NXT(struct ros_cpu *s);
void basicvfp_hand_DONEXT(struct ros_cpu *s);
void basicvfp_hand_DONXTS(struct ros_cpu *s);
void basicvfp_hand_DONES(struct ros_cpu *s);
void basicvfp_hand_DONE(struct ros_cpu *s);
void basicvfp_hand_AEDONE(struct ros_cpu *s);
void basicvfp_hand_AEDONES(struct ros_cpu *s);
void basicvfp_hand_AESPAC(struct ros_cpu *s);
void basicvfp_hand_SPACES(struct ros_cpu *s, uint32_t *p10, uint32_t *p12,
                          uint32_t r14);
void basicvfp_hand_OSSTRI(struct ros_cpu *s);
void basicvfp_hand_INTEGY(struct ros_cpu *s);
void basicvfp_hand_INTEGB(struct ros_cpu *s);
void basicvfp_hand_SFIX(struct ros_cpu *s);
void basicvfp_hand_SPUSH(struct ros_cpu *s);
void basicvfp_hand_STORE(struct ros_cpu *s);
void basicvfp_hand_AELV(struct ros_cpu *s);
void basicvfp_hand_GTARGS(struct ros_cpu *s);
void basicvfp_hand_MSG(struct ros_cpu *s);

/* The OS thunks. The harness's api_gen declares them, and each unit
 * declares them again, as the landed units do. */
void ros_thunk_OS_CLI(struct ros_cpu *s);
void ros_thunk_OS_Word(struct ros_cpu *s);
void ros_thunk_OS_Byte(struct ros_cpu *s);
void ros_thunk_OS_BPut(struct ros_cpu *s);
void ros_thunk_OS_GBPB(struct ros_cpu *s);
void ros_thunk_OS_WriteC(struct ros_cpu *s);
void ros_thunk_OS_GenerateError(struct ros_cpu *s);
void ros_thunk_OS_Exit(struct ros_cpu *s);

/* Tokens and constants, as the lift defines them from hdr/Tokens and
 * hdr/Definitions. Identical redefinitions across units do no harm. */
#define TELSE   139u
#define TERROR  133u
#define TON     238u
#define TOFF    135u
#define TTO     184u
#define TSTEP   136u
#define TTEXT   251u
#define TEXT    162u
#define TRECT   147u
#define TFN     164u
#define TENDCA  203u
#define TOF     202u
#define TESCSTMT 200u
#define TFPLV   8u
#define TINTEGER 0x40000000u
#define SAFECRUNCH 15u
#define VARS    0x8700u
#define OsWord_DefineSoundEnvelope 8u
#define OsWord_DefinePointerAndMouse 21u

/* The real addresses of the MSG error sites. MSG reads its error
 * number and token from the words after each address. These are the
 * lift's own BL sites inside DISPAT's error labels. There is one for
 * each label this unit reaches. */
#define SITE_INSTALLBAD  0xFC10FEACu
#define SITE_ERRFN       0xFC10FF6Cu
#define SITE_BADDIMSIZE  0xFC10FFBCu
#define SITE_NOENDC      0xFC110154u
#define SITE_ERMOUS      0xFC110174u
#define SITE_ERSYSINPUTS 0xFC11017Cu
#define SITE_ERSYSOUTPUTS 0xFC110184u

/* The error exits hand the block to MSG, which never returns. The
 * fault catches a MSG that does return. */
static void dsys_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* PROGPAL (Stmt2.s:486-502). MOUSE COLOUR reaches it: Stmt2.s:
 * 1506-1523 sets R6 = 25 and branches here. Write VDU 19, then the
 * five parameter bytes r3, r6, r2, r1 and the EXPRDN value in IACC,
 * each through its own OS_WriteC. The lift's PROGPAL stays for
 * COLOUR's palette forms until Group C lands it. This private copy
 * serves only the MOUSE COLOUR entry, where SP holds {R1,R2,R3}. */
static void dsys_progoal(struct ros_cpu *s, uint32_t r0, uint32_t r1,
                         uint32_t r2, uint32_t r3, uint32_t r4,
                         uint32_t r5, uint32_t r6, uint32_t r8,
                         uint32_t r10, uint32_t r11, uint32_t r12,
                         uint32_t r13, uint32_t r14)
{
    r5 = r0;                                    /* MOV R5,IACC */
    ros_swi(s, 0x113u);                         /* SWI OS_WriteI+19 */
    r1 = ros_ld32(r13);                         /* LDMFD SP!,{R1,R2,R3} */
    r2 = ros_ld32(r13 + 4);
    r3 = ros_ld32(r13 + 8);
    r13 += 12;
    r0 = r3;                                    /* MOV IACC,R3 */
    s->r[0] = r0;                               /* SWI OS_WriteC */
    s->r[13] = r13;
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
    s->r[1] = r1;                               /* B NXT */
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[8] = r8;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* =====================================================================
 * DOSTAR, OSCL: the two *command statements.
 * ===================================================================== */

/* DOSTAR (Basic.s:708-716): the bare '*' statement. The rest of the
 * line is a CLI command. It is run with the registers that OS_CLIREGS
 * sets up and with the memory-move protocol armed. Then execution
 * goes on to REM. */
void basicvfp_hand_DOSTAR(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r8 = s->r[8], r10 = s->r[10],
             r11 = s->r[11], r12 = s->r[12], r13 = s->r[13], r14;

    r0 = r12;                                   /* MOV R0,LINE ; do oscli */
    basicvfp_OSCLIREGS(s, &r1, &r2, &r3, &r4, &r5, r8, r12, r13,
                       0xFFFFFFF0u);            /* BL OSCLIREGS */
    r14 = 3;                                    /* MOV R14,#3 ; MEMM with
                                                 * move and r12 flags */
    ros_st32(r8 - 20, r12);                     /* STR LINE,[ARGP,#R12STORE] */
    ros_st8(r8 - 25, r14);                      /* STRB R14,[ARGP,#MEMM] */
    s->r[0] = r0;                               /* SWI OS_CLI */
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    ros_native_swi(s, ros_thunk_OS_CLI);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r14 = 0;                                    /* MOV R14,#0 */
    ros_st8(r8 - 25, r14);                      /* STRB R14,[ARGP,#MEMM] */
    s->r[1] = r1;                               /* B REM (= DATA) */
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_hand_DATA(s);
    return;
}

/* OSCL (Stmt2.s:2019-2033): the OSCL statement. Evaluate the *command
 * string into STRACC, then use the same OS_CLI protocol as DOSTAR. */
void basicvfp_hand_OSCL(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    s->r[0] = s->r[0];                          /* BL AEEXPR */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    basicvfp_hand_OSSTRI(s);                    /* BL OSSTRI */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 = r8 - 1536;                             /* ADD R0,ARGP,#STRACC */
    basicvfp_OSCLIREGS(s, &r1, &r2, &r3, &r4, &r5, r8, r12, r13,
                       0xFFFFFFF0u);            /* BL OSCLIREGS */
    r14 = 3;                                    /* MOV R14,#3 */
    ros_st32(r8 - 20, r12);                     /* STR LINE,[ARGP,#R12STORE] */
    ros_st8(r8 - 25, r14);                      /* STRB R14,[ARGP,#MEMM] */
    s->r[0] = r0;                               /* SWI OS_CLI */
    ros_native_swi(s, ros_thunk_OS_CLI);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r14 = 0;                                    /* MOV R14,#0 */
    ros_st8(r8 - 25, r14);                      /* STRB R14,[ARGP,#MEMM] */
    s->r[1] = r1;                               /* B NXT */
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* =====================================================================
 * FNRET: the '=' return statement.
 * ===================================================================== */

/* GTARGS' BL (P1). GTARGS used to end its path for RETURN parameters
 * with ros_resume(link), a longjmp back into this helper's frame,
 * which called setjmp. GTARGS now returns through the C stack (see
 * funct_ret.c). So the setjmp frame is gone, and this is the plain BL
 * it always was underneath. R14 is set to the lift's own site, the
 * call is made, and the caller continues. The helper stays noinline
 * so that hand_FNRET itself never carries any setjmp overhead, and
 * keeps its tail calls. */
__attribute__((noinline)) static void fnret_gtargs(struct ros_cpu *s)
{
    s->r[14] = 0xFC104A60u;
    basicvfp_hand_GTARGS(s);
}

/* FNRET (Funct.s:525-543): the '=' statement inside an FN body.
 * Evaluate the value and trace the exit. R5 is saved in R14 around
 * ENDTRC. Then, if the top frame is a TFN frame, unwind through
 * GTARGS. The value is carried on the stack, and the caller is
 * resumed at R7. Otherwise keep discarding frames through POPA until
 * the TFN frame is found. The stack at entry, from the original's
 * comment:
 *   rv, type of parameters    repeated!      : may not exist
 *   parameter list end + &c0000000
 *   OR lv of RETURN destinations + &80000000 form,
 *   rv, type, lv of LOCALs    repeated!
 *   LINE, AELINE, FN/PROC. */
void basicvfp_hand_FNRET(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12,
             r13 = s->r[13], r14;

    s->r[0] = s->r[0];                          /* BL AEEXPR */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = s->r[10];
    s->r[11] = s->r[11];
    s->r[12] = s->r[12];
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    /* Output trace information for end of proc/fn...
     * MOV R14,R5 ; BL ENDTRC (corrupts R5) ; MOV R5,R14. The BL
     * overwrites the R5 saved in R14, so what comes back in R14 is
     * ENDTRC's own leftover link. On the trace-off path that is the
     * call marker. On the trace-on path it is ENDTRC's internal BL
     * site. The value is dead in GTARGS, which loads R5 afresh before
     * using it. It is kept as the original has it. */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_ENDTRC(s);
    r0 = s->r[0]; r1 = s->r[1]; r13 = s->r[13]; r14 = s->r[14];
    r5 = r14;                                   /* MOV R5,R14 */
FNRETA:
    r4 = ros_ld32(r13);                         /* LDR R4,[SP],#4 */
    r13 += 4;
    ros_subs(s, r4, TFN);                       /* CMP R4,#TFN */
    if (r4 != TFN)                              /* BNE FNRETP */
        goto FNRETP;
    s->r[4] = r4;                               /* BL GTARGS */
    s->r[5] = r5;
    s->r[13] = r13;
    fnret_gtargs(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    s->r[15] = r7;                              /* MOV PC,R7 */
    /* P1 let this return through the C stack. The whole call is one
     * chain of tail calls from the caller's BL: the dispatcher's case,
     * FNBODY, FNGOACACHE and the body's statements. So a return lands
     * at the helper for that BL. This does not hold when BASIC's error
     * machinery has run in between. An error inside the FN, or inside
     * one it called, enters the handler in place, deeper than the
     * call. MSGERR's B STMT then runs the handler's statements on top
     * of those frames. A plain return would land inside the error
     * machinery, which carries on and runs the handler's line again.
     * When the frames run out the result is "Not in a function". The
     * lift had this right, because its MOV PC,R7 is a jump.
     * So this jumps to the frame that the pair (site, sp) names. That
     * is the caller's own level, and it is what the lift's ros_resume
     * finds. The plain return is used only when no frame claims the
     * pair. Where the runtime has no such frame search, the plain
     * return is all there is. */
    if (ros_resume_point_at && ros_resume_point_at(r7, s->r[13]))
        ros_resume(s, r7);                      /* the jump the ARM makes */
    /* PERFORMANCE PHASE (P1). The FN's value used to go back by
     * ros_resume(r7). That was a longjmp into the frame at the FN-call
     * site that had called setjmp. Those frames were the efr, FRD, lv
     * and fkc helpers that the callers put around their BL FACTOR or
     * BL EXPR. The value now returns through the C stack. The whole
     * call chain below this statement is one frame, because every
     * part of it is a tail call. The chain is the dispatcher's
     * case 164 (musttail), FNBODY, FNGOA or FNGOACACHE, DOFN's
     * hand_STMT, the body's statement cycle, and every control-flow
     * statement the body ran. So this return unwinds straight to the
     * caller of that chain. That is the helper that called FACTOR or
     * EXPR with BL. Its ros_check_return sees R15 == r7 == its site,
     * and it continues with the value in the registers, exactly as
     * the resume did. Callers on the lift side work the same way.
     * Their wrappers check R15 against their BL site, which is what
     * r7 holds, and the check passes. */
    return;

FNRETP:
    r13 -= 4;                                   /* SUB SP,SP,#4 */
    s->r[4] = r4;                               /* BL POPA */
    s->r[5] = r5;                               /* (POPA reads R5, so the */
    s->r[13] = r13;                             /* lift stores it here) */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_POPA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (s->z)                                   /* BEQ FNRETA */
        goto FNRETA;
    dsys_msg_at(s, SITE_ERRFN);                 /* B ERRFN */
}

/* =====================================================================
 * OTHER (= WHEN), ENVEL, REPORT, WIDTH, LERROR.
 * ===================================================================== */

/* OTHER (Stmt.s:127-147). WHEN is its alias: the ObjAsm source has
 * the entry `OTHER` and then `WHEN ROUT`. This is the CASE arm that
 * skips to the ENDCASE closing the CASE it belongs to. The scan walks
 * the line headers from a nesting count of 1. It counts an OF at the
 * end of a line and an ENDCASE at the start of one. */
void basicvfp_hand_OTHER(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12];

    do {
        r10 = ros_ld8(r12);                     /* LDRB R10,[LINE],#1 */
        r12 += 1;
    } while (r10 != 13);                        /* TEQ R10,#13 ; BNE WHEN */
    r2 = r12 - 1;                               /* SUB R2,LINE,#1 */
    r3 = 1;                                     /* MOV R3,#1 */
other_01:                                       /* %00: the loop head.
                                                 *  The original does not
                                                 *  advance R2 here. The
                                                 *  one advance per pass
                                                 *  is in the body below.
                                                 *  (The unit's first
                                                 *  version had a second
                                                 *  advance at the loop
                                                 *  head, so every pass
                                                 *  skipped a line.) */
    r0 = ros_ld8(r2 + 1);                       /* LDRB R0,[R2,#1] */
    ros_subs(s, r0, 0xFFu);                     /* CMP R0,#&FF */
    if (r0 >= 0xFFu)                            /* BCS NOENDC */
        dsys_msg_at(s, SITE_NOENDC);
    if (ros_ld8(r2 - 1) == TOF)                 /* LDRB R0,[R2,#-1]; CMP */
        r3 += 1;                                /* ADDEQ R3,R3,#1 ; OF at
                                                 * the end of a line */
    r1 = r2 + 4;                                /* ADD R1,R2,#4 */
    r2 += ros_ld8(r2 + 3);                      /* LDRB R0,[R2,#3]; ADD */
other_02:
    r0 = ros_ld8(r1);                           /* LDRB R0,[R1],#1 */
    r1 += 1;
    if (r0 == 32)                               /* TEQ R0,#" " ; BEQ %02 */
        goto other_02;
    if (r0 != TENDCA)                           /* TEQ R0,#TENDCA ; BNE %00 */
        goto other_01;
    r3 -= 1;                                    /* SUBS R3,R3,#1 */
    if (r3 != 0)                                /* BNE %00 */
        goto other_01;
    r12 = r1;                                   /* MOV LINE,R1 */
    s->r[0] = r0;                               /* B DONXTS */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[15] = s->r[14];
    basicvfp_hand_DONXTS(s);
    return;
}

/* ENVEL (Stmt.s:461-479): the sound envelope statement. It reads 13
 * bytes with INTEXC and a 14th with EXPRDN. They are written into a
 * 16-byte block on the stack, which is passed to OS_Word 8. */
void basicvfp_hand_ENVEL(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    r13 -= 16;                                  /* SUB SP,SP,#16 */
    r1 = 0;                                     /* MOV R1,#0 */
    r11 = r12;                                  /* MOV AELINE,LINE */
ENVELP:
    r13 -= 4;                                   /* STR R1,[SP,#-4]! */
    ros_st32(r13, r1);
    s->r[0] = s->r[0];                          /* BL INTEXC */
    s->r[1] = r1;
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    r13 += 4;
    ros_st8(r13 + r1, r0);                      /* STRB IACC,[SP,R1] */
    r1 += 1;                                    /* ADD R1,R1,#1 */
    ros_subs(s, r1, 13);                        /* CMP R1,#13 */
    if (r1 != 13)                               /* BNE ENVELP */
        goto ENVELP;
    s->r[1] = r1;                               /* BL EXPRDN */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    ros_st8(r13 + 13, r0);                      /* STRB IACC,[SP,#13] */
    r1 = r13;                                   /* MOV R1,SP */
    r0 = OsWord_DefineSoundEnvelope;            /* MOV R0,#OsWord_... */
    s->r[0] = r0;                               /* SWI OS_Word */
    s->r[1] = r1;
    ros_native_swi(s, ros_thunk_OS_Word);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r13 += 16;                                  /* ADD SP,SP,#16 */
    s->r[13] = r13;                             /* B NXT */
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* REPORT (Stmt.s:1317-1327): print the last error's text. */
void basicvfp_hand_REPORT(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    s->r[0] = s->r[0];                          /* BL DONES */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL NLINE */
    basicvfp_NLINE(s);
    r2 = s->r[2]; r14 = s->r[14];
    r4 = r8 - 1792;                             /* ADD R4,ARGP,#ERRORS */
    r7 = 0;                                     /* MOV R7,#0 */
REPORTLOP:
    r0 = ros_ld8(r4);                           /* LDRB R0,[R4],#1 */
    r4 += 1;
    ros_subs(s, r0, 0);                         /* CMP R0,#0 */
    if (r0 == 0) {                              /* BEQ NXT */
        s->r[0] = r0;
        s->r[4] = r4;
        s->r[7] = r7;
        s->r[15] = s->r[14];
        basicvfp_hand_NXT(s);
        return;
    }
    s->r[0] = r0;                               /* BL CHOUT */
    s->r[4] = r4;
    s->r[7] = r7;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CHOUT(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    goto REPORTLOP;                             /* B REPORTLOP */
}

/* WIDTH (Stmt.s:1713-1716): set the printer width. It is stored as
 * the value less 1, and the printing code adds the 1 back. */
void basicvfp_hand_WIDTH(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    s->r[0] = s->r[0];                          /* BL AEEXDN */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r0 -= 1;                                    /* SUB IACC,IACC,#1 */
    ros_st32(r8 - 260, r0);                     /* STR IACC,[ARGP,#WIDTHLOC] */
    s->r[0] = r0;                               /* B NXT */
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* LERROR (Stmt2.s:18-48): the ERROR statement and ERROR EXT. The
 * plain form builds its error block from the pushed string, with the
 * number written over the length word, and passes it to MSGERR. The
 * EXT form restores OLDERR and raises the error through
 * OS_GenerateError. It then calls OS_Exit, which never returns. */
void basicvfp_hand_LERROR(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, TEXT);                      /* CMP R10,#TEXT */
    if (r10 == TEXT)                            /* BEQ LERROREXT */
        goto LERROREXT;
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    s->r[0] = s->r[0];                          /* BL INTEXA */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL EXPR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 != 0) {                              /* BNE ERTYPESTR */
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r7 = ros_ld32(r13);                         /* LDR R7,[SP],#4 */
    r13 += 4;
    r0 = 0;                                     /* MOV R0,#0 */
    ros_st8(r2, r0);                            /* STRB R0,[CLEN],#1 ; 0 at
                                                 * end of string */
    r2 += 1;
    s->r[0] = r0;                               /* BL SPUSH */
    s->r[2] = r2;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPUSH(s);
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4];
    r13 = s->r[13];
    ros_st32(r13, r7);                          /* STR R7,[SP] */
    r14 = r13;                                  /* MOV R14,SP */
    s->r[7] = r7;                               /* B MSGERR */
    s->r[14] = r14;
    basicvfp_MSGERR(s);
    return;

LERROREXT:
    s->r[0] = s->r[0];                          /* BL INTEXA */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL EXPR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 != 0) {                              /* BNE ERTYPESTR */
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTR(s);
        return;
    }
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r7 = ros_ld32(r13);                         /* LDR R7,[SP],#4 */
    r13 += 4;
    r0 = 0;                                     /* MOV R0,#0 */
    ros_st8(r2, r0);                            /* STRB R0,[CLEN],#1 */
    r2 += 1;
    s->r[0] = r0;                               /* BL SPUSH */
    s->r[2] = r2;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPUSH(s);
    r0 = s->r[0]; r1 = s->r[1]; r3 = s->r[3]; r4 = s->r[4];
    r13 = s->r[13];
    ros_st32(r13, r7);                          /* STR R7,[SP] */
    r12 = r8 - 80;                              /* ADD R12,ARGP,#OLDERR */
    s->r[12] = r12;                             /* BL PUTBACKHAND */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_PUTBACKHAND(s);
    r12 = s->r[12];
    r2 = 42;                                    /* MOV R2,#42 */
    ros_st8(r8 - 3, r2);                        /* STRB R2,[ARGP,#CALLEDNAME] */
    r0 = r13;                                   /* MOV R0,SP */
    s->r[0] = r0;                               /* SWI OS_GenerateError */
    ros_native_swi(s, ros_thunk_OS_GenerateError);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[2] = r2;                               /* SWI OS_Exit */
    ros_native_swi(s, ros_thunk_OS_Exit);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    return;
}

/* =====================================================================
 * ORGIN, DOMOUSE (with the PROGPAL entry for MOUSE COLOUR).
 * ===================================================================== */

/* ORGIN (Stmt2.s:1526-1538): the graphics origin statement. It sends
 * VDU 29, then X (from the stack) and Y (kept in R3) through WRITEG. */
void basicvfp_hand_ORGIN(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    s->r[0] = s->r[0];                          /* BL INTEXA */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL EXPRDN */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_swi(s, 0x11Du);                         /* SWI OS_WriteI+29 */
    r0 = s->r[0];
    r3 = r0;                                    /* MOV R3,IACC */
    r0 = ros_ld32(r13);                         /* LDR R0,[SP],#4 ; X */
    r13 += 4;
    s->r[0] = r0;                               /* BL WRITEG */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_WRITEG(s);
    r0 = r3;                                    /* MOV R0,R3 */
    s->r[0] = r0;                               /* BL WRITEG */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_WRITEG(s);
    r0 = s->r[0]; r14 = s->r[14];
    s->r[3] = r3;                               /* B NXT */
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* DOMOUSE (Stmt2.s:1348-1523): the seven forms of the MOUSE
 * statement. They are COLOUR, ON, OFF, TO, STEP, RECT (after TESCSTMT)
 * and the plain read of the position into three or four variables. */
void basicvfp_hand_DOMOUSE(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14;
    uint32_t v;

    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == TTEXT)                           /* CMP R10,#TTEXT ; BEQ */
        goto DOMOUSECOLOUR;
    if (r10 == TON)                             /* CMP R10,#TON */
        goto DOMOUSEON;
    if (r10 == TOFF)                            /* CMP R10,#TOFF */
        goto DOMOUSEOFF;
    ros_subs(s, r10, TTO);                      /* CMP R10,#TTO */
    if (r10 == TTO)
        goto DOMOUSETO;
    ros_subs(s, r10, TSTEP);                    /* CMP R10,#TSTEP */
    if (r10 == TSTEP)
        goto DOMOUSESTEP;
    if (r10 == TESCSTMT)                        /* CMP R10,#TESCSTMT */
        goto DOMOUSERECT;
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    s->r[0] = r0;                               /* BL CRAELV */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z)                                   /* BEQ ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    ros_subs(s, r9, 128);                       /* CMP TYPE,#128 */
    if (r9 >= 128)                              /* BCS ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    r12 = r11;                                  /* MOV LINE,AELINE */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44) {                            /* BNE ERCOMM */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[13] = r13;
        s->r[14] = r14;
        s->r[15] = s->r[14];
        basicvfp_ERCOMM(s);
        return;
    }
    s->r[10] = r10;                             /* BL CRAELV */
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z)                                   /* BEQ ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    ros_subs(s, r9, 128);                       /* CMP TYPE,#128 */
    if (r9 >= 128)                              /* BCS ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    r12 = r11;                                  /* MOV LINE,AELINE */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44) {                            /* BNE ERCOMM */
        s->r[10] = r10;
        s->r[12] = r12;
        s->r[13] = r13;
        s->r[14] = r14;
        s->r[15] = s->r[14];
        basicvfp_ERCOMM(s);
        return;
    }
    s->r[10] = r10;                             /* BL CRAELV */
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z)                                   /* BEQ ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    ros_subs(s, r9, 128);                       /* CMP TYPE,#128 */
    if (r9 >= 128)                              /* BCS ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    r12 = r11;                                  /* MOV LINE,AELINE */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 == 44)                              /* CMP R10,#"," */
        goto MOUSEFOUR;
    s->r[10] = r10;                             /* BL DONE */
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[13] = r13;                             /* SWI OS_Mouse */
    ros_swi(s, 0x1Cu);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r6 = r0;                                    /* MOV R6,R0 */
    r7 = r1;                                    /* MOV R7,R1 */
    r0 = r2;                                    /* MOV IACC,R2 */
    goto DOMOUSESTORE;                          /* (fall through) */

DOMOUSESTORE:
    r9 = TINTEGER;                              /* MOV TYPE,#TINTEGER */
    s->r[0] = r0;                               /* BL STORE ; the switches
                                                 * into the LAST pushed */
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r0 = r7;                                    /* MOV IACC,R7 */
    r9 = TINTEGER;                              /* MOV TYPE,#TINTEGER */
    s->r[0] = r0;                               /* BL STORE ; store y */
    s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r0 = r6;                                    /* MOV IACC,R6 */
    r9 = TINTEGER;                              /* MOV TYPE,#TINTEGER */
    s->r[0] = r0;                               /* BL STORE ; store x */
    s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[15] = s->r[14];                        /* B NXT */
    basicvfp_hand_NXT(s);
    return;

MOUSEFOUR:
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[10] = r10;                             /* BL CRAELV */
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z)                                   /* BEQ ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    ros_subs(s, r9, 128);                       /* CMP TYPE,#128 */
    if (r9 >= 128)                              /* BCS ERMOUS */
        dsys_msg_at(s, SITE_ERMOUS);
    r12 = r11;                                  /* MOV LINE,AELINE */
    s->r[12] = r12;                             /* BL DONES */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r13 -= 8;                                   /* STMFD SP!,{IACC,TYPE} */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[13] = r13;                             /* SWI OS_Mouse */
    ros_swi(s, 0x1Cu);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r6 = r0;                                    /* MOV R6,R0 */
    r7 = r1;                                    /* MOV R7,R1 */
    r11 = r2;                                   /* MOV AELINE,R2 */
    r0 = r3;                                    /* MOV R0,R3 */
    r9 = TINTEGER;                              /* MOV TYPE,#TINTEGER */
    s->r[0] = r0;                               /* BL STORE */
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[9] = r9;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_STORE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r0 = r11;                                   /* MOV IACC,AELINE */
    goto DOMOUSESTORE;                          /* B DOMOUSESTORE */

DOMOUSERECT:
    r10 = ros_ld8(r12);                         /* LDRB R10,[LINE],#1 */
    r12 += 1;
    ros_subs(s, r10, TRECT);                    /* CMP R10,#TRECT */
    if (r10 != TRECT) {                         /* BNE ERSYNT */
        s->r[0] = r0; s->r[1] = r1; s->r[4] = r4; s->r[10] = r10;
        s->r[11] = r11; s->r[12] = r12; s->r[14] = s->r[14];
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    s->r[0] = r0;                               /* BL INTEXA */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL INTEXC */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL INTEXC */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL EXPRDN */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r1 = ros_ld32(r13);                         /* LDMFD SP!,{R1,R2,R3} */
    r2 = ros_ld32(r13 + 4);
    r3 = ros_ld32(r13 + 8);
    r13 += 12;
    r4 = r2 + r0;                               /* ADD R4,R2,R0 */
    /* R3 XLO, R2 YLO, R5 XHI, R4 YHI */
    r2 = (r3 & 0xFFFFu) | (r2 << 16);           /* AND/ORR pair */
    r5 = (r1 + r3) & 0xFFFFu;                   /* ADD R5,R1,R3 ; AND */
    r3 = r5 | (r4 << 16);                       /* ORR R3,R5,R4,LSL #16 */
    r1 = r8 - 1533;                             /* ADD R1,ARGP,#STRACC+3 */
    r0 = 1;                                     /* MOV R0,#1 */
    ros_st8(r1, r0);                            /* STRB R0,[R1] */
    ros_st32(r1 + 1, r2);                       /* STR R2,[R1,#1] */
    ros_st32(r1 + 5, r3);                       /* STR R3,[R1,#5] */
    r0 = OsWord_DefinePointerAndMouse;          /* MOV R0,#OsWord_... */
    s->r[0] = r0;                               /* SWI OS_Word */
    s->r[1] = r1;
    s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_Word);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[2] = r2;                               /* B NXT */
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;

DOMOUSESTEP:
    s->r[0] = r0;                               /* BL AEEXPR */
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
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r1 = r0 & 0xFFu;                            /* AND R1,IACC,#&FF */
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44)                              /* BNE DOMOUSESTEP1 */
        goto DOMOUSESTEP1;
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[1] = r1;                               /* BL EXPR */
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r1 = r0 & 0xFFu;                            /* AND R1,IACC,#&FF */
    r0 = ros_ld32(r13);                         /* LDR IACC,[SP],#4 */
    r13 += 4;
DOMOUSESTEP1:
    s->r[0] = r0;                               /* BL AEDONE */
    s->r[1] = r1;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    v = r1;                                     /* ORR IACC,R1,LSL #16 */
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC */
    ros_st32(r1, ((r0 << 8) & 0xFF00u) | 2 | (v << 16)); /* STR IACC,[R1] */
    r0 = OsWord_DefinePointerAndMouse;          /* MOV R0,#OsWord_... */
    s->r[0] = r0;                               /* SWI OS_Word */
    s->r[1] = r1;
    ros_native_swi(s, ros_thunk_OS_Word);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[15] = s->r[14];                        /* B NXT */
    basicvfp_hand_NXT(s);
    return;

DOMOUSETO:
    s->r[0] = r0;                               /* BL INTEXA */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL EXPRDN */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    ros_st32(r13, r0);                          /* STR IACC,[SP,#-4]! */
    r13 -= 4;                                   /* STR R1,[SP,#-4]! */
    ros_st32(r13, (r1 << 16) | 0x300u);
    r1 = r13 + 1;                               /* ADD R1,SP,#1 */
    r0 = OsWord_DefinePointerAndMouse;          /* MOV R0,#OsWord_... */
    s->r[0] = r0;                               /* SWI OS_Word */
    s->r[1] = r1;
    s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_Word);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    r13 += 8;                                   /* ADD SP,SP,#8 */
    s->r[13] = r13;                             /* B NXT */
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;

DOMOUSEOFF:
    r0 = 0;                                     /* MOV IACC,#0 */
    s->r[0] = r0;                               /* BL DONES */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    goto DOMOUSEON1;                            /* B DOMOUSEON1 */

DOMOUSEON:
    r0 = 1;                                     /* MOV IACC,#1 */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    if (r10 != 58)                              /* CMP R10,#":" */
        ros_subs(s, r10, 13);                   /* CMPNE R10,#13 */
    if (r10 != 58 && r10 != 13)
        ros_subs(s, r10, TELSE);                /* CMPNE R10,#TELSE */
    if (r10 == 58 || r10 == 13 || r10 == TELSE) /* BEQ DOMOUSEON1 */
        goto DOMOUSEON1;
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    s->r[0] = r0;                               /* BL AEEXDN */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
DOMOUSEON1:
    r1 = r0;                                    /* MOV R1,IACC */
    r0 = 106;                                   /* MOV R0,#106 */
    s->r[0] = r0;                               /* SWI OS_Byte */
    s->r[1] = r1;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    ros_native_swi(s, ros_thunk_OS_Byte);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2];
    s->r[4] = s->r[4];                          /* B NXT */
    s->r[14] = s->r[14];
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;

DOMOUSECOLOUR:
    s->r[0] = r0;                               /* BL INTEXA */
    s->r[1] = r1;
    s->r[4] = r4;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL INTEXC */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL INTEXC */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r13 -= 4;                                   /* STR IACC,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[13] = r13;                             /* BL EXPRDN */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPRDN(s);
    r0 = s->r[0]; r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r6 = 25;                                    /* MOV R6,#25 */
    /* B PROGPAL: the shared palette tail, as the private copy */
    dsys_progoal(s, r0, r1, r2, r3, r4, r5, r6, r8, r10, r11, r12,
                 r13, r14);
    return;
}

/* =====================================================================
 * OVERLAY, BBPUT, LIBRARY, INSTALLBAD.
 * ===================================================================== */

/* OVERLAY (Stmt2.s:1688-1737): declare a string array as the overlay
 * area. Find the size of each element from its file, claim the FSA,
 * and write the three-word overlay header. */
void basicvfp_hand_OVERLAY(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    s->r[0] = s->r[0];                          /* BL AELV */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    if (s->z) {                                 /* BEQ ERARRY */
        s->r[15] = s->r[14];
        basicvfp_ERARRY(s);
        return;
    }
    ros_subs(s, r9, 384);                       /* CMP TYPE,#256+128 */
    if (r9 != 384) {                            /* BNE ERTYPESTRINGARRAY */
        s->r[15] = s->r[14];
        basicvfp_ERTYPESTRINGARRAY(s);
        return;
    }
    r9 = ros_ld32(r0);                          /* LDR TYPE,[IACC] */
    ros_subs(s, r9, 16);                        /* CMP TYPE,#16 */
    if (r9 < 16) {                              /* BCC ERARRZ */
        s->r[9] = r9;
        s->r[15] = s->r[14];
        basicvfp_ERARRZ(s);
        return;
    }
    s->r[9] = r9;                               /* BL AEDONES */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL GETARRAYSIZE1 */
    basicvfp_GETARRAYSIZE1(s);
    r9 = s->r[9]; r10 = s->r[10];
    r11 = r9;                                   /* MOV AELINE,TYPE */
    r6 = r9;                                    /* MOV R6,TYPE */
    r7 = 0;                                     /* MOV R7,#0 ; max size */
OVERLAYSIZES:
    r0 = r6;                                    /* MOV IACC,R6 */
    s->r[0] = r0;                               /* BL VARSTR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_VARSTR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r9 = s->r[9];
    r14 = s->r[14];
    r4 = r2 - r8;                               /* SUB R4,CLEN,ARGP */
    r4 = ros_adds(s, r4, 1536);                 /* SUBS R4,R4,#STRACC */
    if (r4 == 0)                               /* BEQ OVERLAYSIZES1 */
        goto OVERLAYSIZES1;
    r0 = 13;                                    /* MOV R0,#13 */
    ros_st8(r2, r0);                            /* STRB R0,[CLEN] */
    s->r[0] = r0;                               /* BL OSFILEINFOSTRACC */
    s->r[4] = r4;
    s->r[6] = r6;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_OSFILEINFOSTRACC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r14 = s->r[14];
    ros_subs(s, r0, 1);                         /* CMP R0,#1 */
    if (r0 != 1)                               /* BNE OVERLAYSIZES1 */
        goto OVERLAYSIZES1;
    ros_subs(s, r4, r7);                        /* CMP R4,R7 */
    if (r4 >= r7)                              /* MOVCS R7,R4 */
        r7 = r4;
OVERLAYSIZES1:
    r6 += 5;                                    /* ADD R6,R6,#5 */
    r10 = ros_subs(s, r10, 1);                  /* SUBS R10,R10,#1 */
    if (r10 != 0)                               /* BNE OVERLAYSIZES */
        goto OVERLAYSIZES;
    r7 = (r7 + 3) & ~3u;                        /* ADD R7,R7,#3 ; BIC */
    r4 = ros_ld32(r8 - 140);                    /* LDR R4,[ARGP,#FSA] */
    r6 = r4 + 12 + r7;                          /* ADD R6,R4,#12 ; 3 words
                                                 * of data: array base,
                                                 * current overlay, size */
    r5 = r6 + 0x400u;                           /* ADD R5,R6,#1024 */
    ros_subs(s, r5, r13);                       /* CMP R5,SP */
    if (r5 >= r13)                              /* BCS BADDIMSIZE */
        dsys_msg_at(s, SITE_BADDIMSIZE);
    r0 = ros_ld32(r8 - 88);                     /* LDR R0,[ARGP,#OVERPTR] */
    ros_logic(s, r0, s->c);                     /* TEQ R0,#0 */
    r0 = 0;                                     /* MOV R0,#0 */
    if (ros_ld32(r8 - 88) != 0) {
        ros_st32(r8 - 0x200u, 0);               /* STRNE R0,[ARGP,#PROCPTR] ;
                                                 * forget all procedures or
                                                 * functions in case they
                                                 * are overlays already */
        ros_st32(r8 - 272, 0);                  /* STRNE R0,[ARGP,#FNPTR] */
    }
    ros_st32(r8 - 140, r6);                     /* STR R6,[ARGP,#FSA] */
    ros_st32(r8 - 88, r4);                      /* STR R4,[ARGP,#OVERPTR] ;
                                                 * overlay pointer */
    ros_st32(r4, r11);                          /* STR AELINE,[R4] ; base of
                                                 * array: [,#-4] has size */
    r0 = 0xFFFFFFFFu;                           /* MVN R0,#0 */
    ros_st32(r4 + 4, 0xFFFFFFFFu);              /* STR R0,[R4,#4] ; current
                                                 * overlay (-1=none) */
    ros_st32(r4 + 8, r7);                       /* STR R7,[R4,#8] ; total
                                                 * size allowed */
    r10 = ros_ld8(r12 - 1);                     /* LDRB R10,[LINE,#-1] */
    s->r[0] = r0;                               /* B NXT */
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* BBPUT (Stmt2.s:1752-1782): BPUT#channel,value. The value may be a
 * string. Then the whole of STRACC is written through OS_GBPB,
 * without the terminator. */
void basicvfp_hand_BBPUT(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;
    uint32_t v;

    s->r[0] = s->r[0];                          /* BL AECHAN */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_AECHAN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10]; r11 = s->r[11]; r14 = s->r[14];
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44) {                            /* BNE ERCOMM */
        s->r[15] = s->r[14];
        basicvfp_ERCOMM(s);
        return;
    }
    r13 -= 4;                                   /* STR R1,[SP,#-4]! */
    ros_st32(r13, r1);
    s->r[13] = r13;                             /* BL EXPR */
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 == 0)                                /* BEQ BBPUT1 */
        goto BBPUT1;
    if ((int32_t)r9 < 0) {                      /* BLMI INTEGB */
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_INTEGB(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    }
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    r13 += 4;
    s->r[1] = r1;                               /* SWI OS_BPut */
    s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_BPut);
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    s->r[15] = s->r[14];                        /* B NXT */
    basicvfp_hand_NXT(s);
    return;

BBPUT1:
    v = r10 ^ 0x3Bu;                            /* TEQ R10,#";" */
    if (v != 0) {                               /* MOVNE R0,#10 */
        r0 = 10;                                /* STRNEB R0,[CLEN],#1 */
        ros_st8(r2, r0);
        r2 += 1;
    } else {
        s->r[14] = 0xFFFFFFF0u;                 /* BLEQ AESPAC */
        basicvfp_hand_AESPAC(s);
        r10 = s->r[10]; r11 = s->r[11];
    }
    s->r[0] = r0;                               /* BL AEDONE */
    s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONE(s);
    r2 = s->r[2]; r4 = s->r[4]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r5 = r2;                                    /* MOV R5,CLEN */
    r0 = 2;                                     /* MOV R0,#2 */
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    r13 += 4;
    r2 = r8 - 1536;                             /* ADD R2,ARGP,#STRACC */
    r3 = ros_subs(s, r5, r2);                   /* SUBS R3,R5,R2 */
    if (r3 != 0) {                              /* SWINE OS_GBPB */
        s->r[0] = r0;
        s->r[1] = r1;
        s->r[2] = r2;
        s->r[3] = r3;
        s->r[5] = r5;
        s->r[13] = r13;
        ros_native_swi(s, ros_thunk_OS_GBPB);
        if (s->v) ros_swi_raise(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    }
    s->r[0] = r0;                               /* B NXT */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[5] = r5;
    s->r[13] = r13;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* LIBRARY (Stmt2.s:1965-1992): load a library at the FSA, link it at
 * LIBRARYLIST, and crunch it when CRUNCHCHK says it is needed. */
void basicvfp_hand_LIBRARY(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    s->r[0] = s->r[0];                          /* BL LIBSUB */
    s->r[1] = s->r[1];
    s->r[4] = s->r[4];
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_LIBSUB(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r3 = ros_ld32(r8 - 92);                     /* LDR R3,[ARGP,#LIBRARYLIST] */
    ros_st32(r8 - 92, r2);                      /* STR R2,...; link in at
                                                 * list head */
    ros_st32(r2, r3);                           /* STR R3,[R2],#4 */
    r2 += 4;
    basicvfp_CRUNCHCHK(s, r0, r1, r2, r3, r4, r13, 0xFFFFFFF0u); /* BL */
    if (s->z)                                   /* BEQ NOLIBCRUNCH */
        goto NOLIBCRUNCH;
    r0 = SAFECRUNCH;                            /* MOV R0,#SAFECRUNCH */
    r1 = r2;                                    /* MOV R1,R2 */
    r13 -= 4;                                   /* STR R10,[SP,#-4]! */
    ros_st32(r13, r10);
    basicvfp_CRUNCHROUTINE(s, r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7,
                           &r10, 0xFFFFFFF0u);  /* BL CRUNCHROUTINE */
    r2 = (r2 + 3) & ~3u;                        /* ADD R2,R2,#3 ; BIC */
    ros_st32(r8 - 140, r2);                     /* STR R2,[ARGP,#FSA] */
    r10 = ros_ld32(r13);                        /* LDR R10,[SP],#4 */
    r13 += 4;
    s->r[0] = r0;                               /* B NXT */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[10] = r10;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;

NOLIBCRUNCH:
    r2 += r4;                                   /* ADD R2,R2,R4 */
    ros_st32(r8 - 140, r2);                     /* STR R2,[ARGP,#FSA] */
    s->r[2] = r2;                               /* B NXT */
    s->r[3] = r3;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;
}

/* INSTALLBAD: the whole statement is its error. This is INSTALL's exit
 * for bad syntax, at MsgSite &FC10FEAC. */
void basicvfp_hand_INSTALLBAD(struct ros_cpu *s)
{
    dsys_msg_at(s, SITE_INSTALLBAD);
}

/* =====================================================================
 * SYS: the whole SYS machine (Stmt2.s:2034-2218).
 * ===================================================================== */

void basicvfp_hand_SYS(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3],
             r4 = s->r[4], r5 = s->r[5], r6 = s->r[6], r7 = s->r[7],
             r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v121, v122, z1;

    s->r[0] = r0;                               /* BL AEEXPR ; handle SYS */
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
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 == 0)                                /* BEQ SYSNAME */
        goto SYSNAME;
    if ((int32_t)r9 < 0) {                      /* BLMI SFIX */
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_SFIX(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r8 = s->r[8];
        r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
        r14 = s->r[14];
    }
    goto SYSGOTSWINUMB;

SYSNAME:
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC ;
                                                 * convert the SWI name
                                                 * to a number */
    r0 = 0;                                     /* MOV R0,#0 */
    ros_st8(r2, r0);                            /* STRB R0,[CLEN] */
    s->r[0] = r0;                               /* SWI OS_SWINumberFrom.. */
    s->r[1] = r1;
    ros_swi(s, 0x39u);
    r0 = s->r[0];

SYSGOTSWINUMB:
    r9 = 8;                                     /* MOV TYPE,#8 ; initial
                                                 * SP in R9/TYPE (SYSRELSTK) */
    ros_st32(r13 - 4, r0);                      /* STR IACC,[SP,#-4]! ;
                                                 * save action */
    r4 = 0;                                     /* MOV R4,#0 */
    r5 = 0;                                     /* MOV R5,#0 */
    r6 = 0;                                     /* MOV R6,#0 */
    r7 = 0;                                     /* MOV R7,#0 */
    ros_st32(r13 - 24, 0);                      /* STMFD SP!,{R4-R7,R9} */
    ros_st32(r13 - 20, 0);
    ros_st32(r13 - 16, 0);
    ros_st32(r13 - 12, 0);
    ros_st32(r13 - 8, 8);
    ros_st32(r13 - 40, 0);                      /* STMFD SP!,{R4-R7} */
    ros_st32(r13 - 36, 0);
    ros_st32(r13 - 32, 0);
    ros_st32(r13 - 28, 0);
    r13 -= 48;                                  /* STMFD SP!,{R4,R5} ; 10
                                                 * register slots and the
                                                 * old sp */
    ros_st32(r13, 0);
    ros_st32(r13 + 4, 0);
    if (r10 != 44)                              /* CMP R10,#"," */
        goto SYSCALL;
    /* Note that R4, the SYS register index, is already 0. */
SYS0:
    ros_subs(s, r4, 10);                        /* CMP R4,#10 */
    if (r4 >= 10)                               /* BCS ERSYSINPUTS */
        dsys_msg_at(s, SITE_ERSYSINPUTS);
SYS0SPACES:
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE],#1 */
    r11 += 1;
    if (r10 == 32)                              /* CMP R10,#" " */
        goto SYS0SPACES;
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 == 44)                              /* BEQ SYS0END */
        goto SYS0END;
    r13 -= 4;                                   /* STR R4,[SP,#-4]! */
    ros_st32(r13, r4);
    r11 -= 1;                                   /* SUB AELINE,AELINE,#1 */
    s->r[0] = r0;                               /* BL EXPR */
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_logic(s, r9, s->c);                     /* TEQ TYPE,#0 */
    if (r9 == 0)                                /* BEQ SYS0STRING */
        goto SYS0STRING;
    if ((int32_t)r9 < 0) {                      /* BLMI SFIX */
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_SFIX(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r5 = s->r[5];
        r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
        r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    }
    goto SYS0PUSH;

SYS0STRING: ;
    r0 = 0;                                     /* MOV R0,#0 */
    ros_st8(r2, r0);                            /* STRB R0,[CLEN],#1 */
    r2 += 1;
    r7 = ros_ld32(r8 - 140);                    /* LDR R7,[ARGP,#FSA] ;
                                                 * the stack holds R4, the
                                                 * R0-R9 parameters, the
                                                 * stack pointer or size,
                                                 * and the SWI number */
    v121 = r13;
    /* LDMFD/STMIA R7!,{R0,R4,R5,R6}: copy 13 words from the stack to
     * the FSA. (The unit's first version split this comment across the
     * code lines, and so silently commented out two of the loads.) */
    r4 = ros_ld32(r13 + 4);
    r5 = ros_ld32(r13 + 8);
    r6 = ros_ld32(r13 + 12);
    ros_st32(r7, ros_ld32(r13));
    ros_st32(r7 + 4, r4);
    ros_st32(r7 + 8, r5);
    ros_st32(r7 + 12, r6);
    r4 = ros_ld32(r13 + 20);                    /* the second four */
    r5 = ros_ld32(r13 + 24);
    r6 = ros_ld32(r13 + 28);
    ros_st32(r7 + 16, ros_ld32(r13 + 16));
    ros_st32(r7 + 20, r4);
    ros_st32(r7 + 24, r5);
    ros_st32(r7 + 28, r6);
    r3 = ros_ld32(r13 + 36);                    /* the final five */
    r4 = ros_ld32(r13 + 40);
    r5 = ros_ld32(r13 + 44);
    r6 = ros_ld32(r13 + 48);
    r13 += 52;
    ros_st32(r7 + 32, ros_ld32(v121 + 32));
    ros_st32(r7 + 36, r3);
    ros_st32(r7 + 40, r4);
    ros_st32(r7 + 44, r5);
    ros_st32(r7 + 48, r6);
    r7 += 52;
    r6 = r13;                                   /* MOV R6,SP */
    s->r[2] = r2;                               /* BL SPUSH */
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_SPUSH(s);
    r1 = s->r[1];
    r13 = s->r[13];
    r14 = r6 - r13;                             /* SUB R14,R6,SP */
    /* LDMDB R7!,{R0,R3,R4,R5,R6}: copy the 13 words back. */
    r3 = ros_ld32(r7 - 16);
    r4 = ros_ld32(r7 - 12);
    r5 = ros_ld32(r7 - 8);
    r6 = ros_ld32(r7 - 4);
    ros_st32(r13 - 20, ros_ld32(r7 - 20));      /* STMFD SP!,{R0,R3,R4,R5,R6} */
    ros_st32(r13 - 16, r3);
    ros_st32(r13 - 12, r4);
    ros_st32(r13 - 8, r5 + r14);                /* ADD R5,R5,R14 ; the
                                                 * stack-usage count */
    ros_st32(r13 - 4, r6);
    r4 = ros_ld32(r7 - 32);                     /* LDMDB R7!,{R0,R4,R5,R6} */
    r5 = ros_ld32(r7 - 28);
    r6 = ros_ld32(r7 - 24);
    ros_st32(r13 - 36, ros_ld32(r7 - 36));      /* STMFD SP!,{R0,R4,R5,R6} */
    ros_st32(r13 - 32, r4);
    ros_st32(r13 - 28, r5);
    ros_st32(r13 - 24, r6);
    r7 -= 52;                                   /* LDMDB R7!,{R0,R4,R5,R6} */
    r4 = ros_ld32(r7 + 4);
    r5 = ros_ld32(r7 + 8);
    r6 = ros_ld32(r7 + 12);
    r13 -= 52;                                  /* STMFD SP!,{R0,R4,R5,R6} */
    ros_st32(r13, ros_ld32(r7));
    ros_st32(r13 + 4, r4);
    ros_st32(r13 + 8, r5);
    ros_st32(r13 + 12, r6);
    r0 = r13 + 56;                              /* ADD IACC,SP,#12*4+4+4 ;
                                                 * 12 words plus string
                                                 * length plus R4 */
    goto SYS0PUSH;

SYS0PUSH:
    r4 = ros_ld32(r13);                         /* LDR R4,[SP],#4 */
    r13 += 4;
    ros_st32(r13 + (r4 << 2), r0);              /* STR IACC,[SP,R4,LSL #2] */
SYS0END:
    r4 += 1;                                    /* ADD R4,R4,#1 */
    if (r10 == 44)                              /* CMP R10,#"," ; BEQ SYS0 */
        goto SYS0;

SYSCALL:
    r12 = r11;                                  /* MOV LINE,AELINE */
    r11 = r8 - 44;                              /* ADD AELINE,ARGP,#SWICODE */
    r5 = ros_ld32(r13 + 44);                    /* LDR R5,[SP,#11*4] */
    r4 = ros_ld32(r13 + 40);                    /* LDR R4,[SP,#10*4] */
    /* TEQ R4,#8: MEMM is 3, with the move bit, only when the stack
     * never moved, that is when no string parameter extended it.
     * Otherwise MEMM is 2. */
    ros_st8(r8 - 25, (r4 ^ 8) == 0 ? 3 : 2);    /* STRB R14,[ARGP,#MEMM] */
    ros_st32(r8 - 20, r12);                     /* STR LINE,[ARGP,#R12STORE] */
    ros_subs(s, r10, TTO);                      /* CMP R10,#TTO */
    r5 = (r5 & 0xFFFFFFu) | 0xEF000000u;        /* BIC/ORR: make a SWI
                                                 * from the number that
                                                 * is already there */
    ros_st32(r8 - 44, r5);                      /* STR R5,[ARGP,#SWICODE] */
    if (r10 == TTO)                             /* BEQ SYSCALLTO */
        goto SYSCALLTO;
    s->r[0] = r0;                               /* BL DONE */
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
    basicvfp_hand_DONE(s);
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    /* LDMFD SP!,{R0-R9}: load the parameters from the stack. (The
     * unit's first version had this comment as a trailing /* that was
     * opened on the r0 line and closed only after the r2 line. The two
     * loads in between were silently commented out, and the SWI ran
     * with the live IACC and CLEN in R0-R2 instead of the values in
     * the stacked slots.) */
    r0 = ros_ld32(r13);
    r1 = ros_ld32(r13 + 4);
    r2 = ros_ld32(r13 + 8);
    r3 = ros_ld32(r13 + 12);
    r4 = ros_ld32(r13 + 16);
    r5 = ros_ld32(r13 + 20);
    r6 = ros_ld32(r13 + 24);
    r7 = ros_ld32(r13 + 28);
    r8 = ros_ld32(r13 + 32);
    r9 = ros_ld32(r13 + 36);
    r13 += 40;
    r14 = 0xFC107CD0u;                          /* MOV R14,PC */
    /* MOV PC,AELINE: go execute the SWI... */
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[8] = r8;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = r11;
    ros_resume(s, r11);
    if (s->r[15] != 0xFC107CD0u)                /* ...and return here */
        ros_bad_return(s, 0xFC107CD0u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r8 = VARS;                                  /* MOV ARGP,#VARS */
    r13 += ros_ld32(r13);                       /* LDR R14,[SP] ; ADD SP,SP,
                                                 *  R14 (SYSRELSTK) */
    r14 = 0;                                    /* MOV R14,#0 */
    ros_st8(r8 - 25, 0);                        /* STRB R14,[ARGP,#MEMM] */
    s->r[8] = r8;                               /* B NXT */
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_hand_NXT(s);
    return;

SYSCALLTO:
    r0 = ros_ld32(r13);                         /* LDMFD SP!,{R0-R9} */
    r1 = ros_ld32(r13 + 4);
    r2 = ros_ld32(r13 + 8);
    r3 = ros_ld32(r13 + 12);
    r4 = ros_ld32(r13 + 16);
    r5 = ros_ld32(r13 + 20);
    r6 = ros_ld32(r13 + 24);
    r7 = ros_ld32(r13 + 28);
    r8 = ros_ld32(r13 + 32);
    r9 = ros_ld32(r13 + 36);
    r13 += 40;
    r14 = 0xFC107CF4u;                          /* MOV R14,PC */
    /* MOV PC,AELINE: go execute the SWI... */
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[6] = r6;
    s->r[7] = r7;
    s->r[8] = r8;
    s->r[9] = r9;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = r11;
    ros_resume(s, r11);
    if (s->r[15] != 0xFC107CF4u)                /* ...and return here */
        ros_bad_return(s, 0xFC107CF4u);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = ros_cpsr(s);                          /* SavePSR R14 */
    r13 -= 44;                                  /* STMFD SP!,{R0-R9,R14} ;
                                                 *  write the parameters
                                                 *  back */
    ros_st32(r13, r0);
    ros_st32(r13 + 4, r1);
    ros_st32(r13 + 8, r2);
    ros_st32(r13 + 12, r3);
    ros_st32(r13 + 16, r4);
    ros_st32(r13 + 20, r5);
    ros_st32(r13 + 24, r6);
    ros_st32(r13 + 28, r7);
    ros_st32(r13 + 32, r8);
    ros_st32(r13 + 36, r9);
    ros_st32(r13 + 40, r14);
    r8 = VARS;                                  /* MOV ARGP,#VARS */
    r14 = 0;                                    /* MOV R14,#0 */
    ros_st8(r8 - 25, 0);                        /* STRB R14,[ARGP,#MEMM] */
    r7 = 0;                                     /* MOV R7,#0 */
SYS1:
    ros_subs(s, r7, 10);                        /* CMP R7,#10 */
    if (r7 >= 10)                               /* BCS ERSYSOUTPUTS */
        dsys_msg_at(s, SITE_ERSYSOUTPUTS);
    r13 -= 4;                                   /* STR R7,[SP,#-4]! */
    ros_st32(r13, r7);
    s->r[7] = r7;                               /* BL CRAELV */
    s->r[8] = r8;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CRAELV(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r7 = ros_ld32(r13);                         /* LDR R7,[SP],#4 */
    r13 += 4;
    if (s->z)                                   /* BEQ SYS1COMMA */
        goto SYS1COMMA;
    r4 = r0;                                    /* MOV R4,IACC */
    r5 = r9;                                    /* MOV R5,TYPE */
    v122 = r13 + (r7 << 2);                     /* LDR IACC,[SP,R7,LSL #2] */
    r0 = ros_ld32(v122);
    r9 = TINTEGER;                              /* MOV TYPE,#TINTEGER */
    ros_subs(s, r5, 128);                       /* CMP R5,#128 */
    if (r5 < 128)                               /* BCC SYS1END */
        goto SYS1END;
    r9 = 0;                                     /* MOV TYPE,#0 */
    r2 = r8 - 1536;                             /* ADD CLEN,ARGP,#STRACC */
    r3 = r2 + 0x100u;                           /* ADD R3,CLEN,#256 */
    z1 = (ros_ld32(v122) == 0);                 /* MOVS R1,IACC */
    ros_logic(s, ros_ld32(v122), s->c);
    do {                                        /* SYS1STRING: the chain of
                                                 * TEQNEs carries on while
                                                 * none has matched */
        if (!z1) {                              /* LDRNEB R1,[IACC],#1 */
            r1 = ros_ld8(r0);
            r0 += 1;
        }
        ros_st8(r2, r1);                        /* STRB R1,[CLEN],#1 */
        r2 += 1;
        z1 = (r2 == r3) || r1 == 13 || r1 == 0 || r1 == 10;
    } while (!z1);                              /* BNE SYS1STRING */
    r2 = ((r2 ^ r3) == 0 ? r2 - 0xFFu : r2);    /* TEQ CLEN,R3 ; SUBEQ
                                                 *  CLEN,CLEN,#255 */
    r2 -= 1;                                    /* SUB CLEN,CLEN,#1 ; drop
                                                 *  the terminator */
SYS1END:
    r13 -= 4;                                   /* STR R7,[SP,#-4]! */
    ros_st32(r13, r7);
    s->r[0] = r0;                               /* BL STOREA */
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[7] = r7;
    s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r8 = s->r[8]; r9 = s->r[9]; r11 = s->r[11];
    r13 = s->r[13]; r14 = s->r[14];
    r7 = ros_ld32(r13);                         /* LDR R7,[SP],#4 */
    r13 += 4;
SYS1ENDA:
    r7 += 1;                                    /* ADD R7,R7,#1 */
    r12 = r11;                                  /* MOV LINE,AELINE */
SYS1SPACES:
    r10 = ros_ld8(r12);                         /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if (r10 == 32)                              /* CMP R10,#" " */
        goto SYS1SPACES;
    if (r10 == 44)                              /* CMP R10,#"," */
        goto SYS1;
    if (r10 != 59)                              /* CMP R10,#";" ; BNE SYSEXIT */
        goto SYSEXIT;
    s->r[7] = r7;                               /* BL CRAELV */
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
    if (s->z) {                                 /* BEQ ERSYNT */
        s->r[15] = s->r[14];
        basicvfp_ERSYNT(s);
        return;
    }
    r4 = r0;                                    /* MOV R4,IACC */
    r5 = r9;                                    /* MOV R5,TYPE */
    r9 = TINTEGER;                              /* MOV TYPE,#TINTEGER */
    r0 = ros_ld32(r13 + 40) >> 28;              /* LDR IACC,[SP,#10*4] ;
                                                 *  psr for flags ; nzcv */
    s->r[0] = r0;                               /* BL STOREA */
    s->r[4] = r4;
    s->r[5] = r5;
    s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_STOREA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r12 = r11;                                  /* MOV LINE,AELINE */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */

SYSEXIT:
    r14 = ros_ld32(r13 + 44);                   /* LDR R14,[SP,#11*4]! */
    r13 = r13 + 44 + r14;                       /* ADD SP,SP,R14 */
    s->r[7] = r7;                               /* B DONEXT */
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_hand_DONEXT(s);
    return;

SYS1COMMA:
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u); /* BL SPACES */
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44)                              /* CMPNE R10,#";" */
        ros_subs(s, r10, 59);
    if (r10 == 44 || r10 == 59) {
        r11 = r12 - 1;                          /* SUBEQ AELINE,LINE,#1 */
        goto SYS1ENDA;                          /* BEQ SYS1ENDA */
    }
    s->r[7] = r7;                               /* B ERSYNT */
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = r14;
    s->r[15] = s->r[14];
    basicvfp_ERSYNT(s);
    return;
}

/* =====================================================================
 * CALL (and USR's statement half): CALLARM/PARM/GO.
 * ===================================================================== */

/* CALL (Stmt2.s:2222-2252): call ARM code at an address. Variables
 * may be passed as well. Their l-value pairs are stacked, with a count
 * in R5. USR shares the body, and its value is returned through
 * CALLARMROUT's TYPE. */
void basicvfp_hand_CALL(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8 = s->r[8], r9,
             r10 = s->r[10], r11 = s->r[11], r12 = s->r[12],
             r13 = s->r[13], r14;

    /* BL AEEXPR: handle CALL and USR keywords */
    s->r[0] = s->r[0];
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
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEGY */
    basicvfp_hand_INTEGY(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    if (r10 == 44)                              /* CMP R10,#"," */
        goto CALLARM;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    r9 = r0;                                    /* MOV TYPE,IACC */
    r4 = r0;                                    /* MOV R4,TYPE */
    r5 = 0;                                     /* MOV R5,#0 */
    goto CALLARMGO;                             /* B CALLARMGO */

CALLARM:
    r4 = r0;                                    /* MOV R4,IACC */
    r5 = 0;                                     /* MOV R5,#0 */
CALLARMPARM:
    r13 -= 8;                                   /* STMFD SP!,{R4,R5} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    r12 = r11;                                  /* MOV LINE,AELINE */
    s->r[4] = r4;                               /* BL CRAELV */
    s->r[5] = r5;
    s->r[12] = r12;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
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
    r4 = ros_ld32(r13);                         /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4) + 1;                 /* ADD R5,R5,#1 */
    /* STMFD SP!,{IACC,TYPE}: a pop and then a push, to the same slots */

    ros_st32(r13, r0);
    ros_st32(r13 + 4, r9);
    s->r[14] = 0xFFFFFFF0u;                     /* BL AESPAC */
    basicvfp_hand_AESPAC(s);
    r10 = s->r[10]; r11 = s->r[11];
    if (r10 == 44)                              /* CMP R10,#"," */
        goto CALLARMPARM;
    s->r[4] = r4;                               /* BL AEDONE */
    s->r[5] = r5;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    /* Go to ARM code */
CALLARMGO:
    s->r[4] = r4;                               /* BL CALLARMROUT */
    s->r[5] = r5;
    s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;
    basicvfp_CALLARMROUT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    s->r[15] = s->r[14];                        /* B NXT */
    basicvfp_hand_NXT(s);
    return;
}
