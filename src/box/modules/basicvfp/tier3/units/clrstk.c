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
 * (Sources/Programmer/BASIC: s.Basic, s.Command, hdr.Definitions, hdr.Tokens).
 */

/* clrstk.c -- CLRSTK, the immediate-mode loop, translated by hand
 * from Basic.s:619-707 and the whole of Command.s (RISC OS 5.31's
 * BASIC, the VFP build). It covers:
 *   - the > prompt cycle. That is the reset of the error stack,
 *     POPLOCALAR, ORDERR, the OS_Exit tested on CALLEDNAME,
 *     FLUSHCACHE, the ">" write, INLINE, SETVAR, MATCH, and the
 *     line-number test with INSRT and the three WARN messages;
 *   - the DC table of two-byte star commands;
 *   - the seventeen command bodies: NEW, APPEND, AUTO, CRUNCH, DELETE,
 *     EDIT, HELP, INSTALL, LIST (with the LIST O option and the TWIN
 *     spool exit), LOAD, TEXTLOAD, TEXTSAVE, LVAR, OLD, RENUM, and
 *     SAVE (with the SAVEIC scan for an immediate command);
 *   - the three error stubs ERLISTO, BADIC and ERINSTALL.
 * The lift has 1,267 lines, here made into one C function. The labels
 * are the original's, used as gotos.
 *
 * The conventions are the ones already landed. State is kept in R[]. A
 * BL becomes a marker call (R14 := 0xFFFFFFF0u). The lift's register
 * stores and reloads around it are copied one for one, so the ARM
 * register file at every boundary is the lift's. A B becomes a plain
 * call whose result is discarded, or a goto when the target is a label
 * in this function.
 *
 * THE SETJMP QUESTION, settled for this unit. The lifted body holds 40
 * setjmp resume points, one around nearly every call. A resume into
 * this function would need a deep callee to execute ros_resume with t
 * set to one of this function's call-site addresses. In the twin no
 * such path exists:
 *   - In the whole lift, only these functions call ros_resume:
 *     . DISPAT. Its MOV PC,R7 goes to a program address resolved by
 *       GOFACT, or to an AJ7 continuation. Both are built from arena
 *       or ladder values. Its MOV PC,AELINE goes to the CALL
 *       statement's user code address;
 *     . the three EXPR operator jump-table fragments FC1029D4/A28/A48
 *       (ADD PC,R4,R14,LSL #2, with addresses in the FC102 ladder);
 *     . the hand GTARGS. Its stored link is its own caller's site;
 *     . the hand Basic_Code start chain;
 *     . MSGATLINE (ERRXLATE). It is entered through the message
 *       system and returns to it.
 *     None of them can hold a CLRSTK site address. This function's
 *     sites are in the FC1008xx, FC10Exxx, FC10Fxxx and FC110xxx
 *     bands. Every performer takes its target from a different source:
 *     a resolved line address, a table base plus an index, or the
 *     performer's own entry link.
 *   - Some callees are still the lift's and RETURN through R14. They
 *     are GETTWO's LDRNE PC,[SP],#4, RENUM2's LDMFD{R0,TYPE,PC},
 *     INSRT's LDREQ PC,[SP],#4, and the MOV PC,R14 of ENDER,
 *     POPLOCALAR and TWINBG. The lift models these as plain returns
 *     that set R15. The marker passes to R15 and is discarded here,
 *     exactly as the landed statements treat GOFACT, LOADER and POPA.
 *   - So no resume point is pushed, and the function holds no setjmp.
 *     Every one of the 40 sites is a plain call. The one callee that
 *     READS its call site is MSG. Its error number and token are in
 *     the two words after the BL. So those three calls carry the real
 *     site addresses (the dsys_msg_at pattern). The guard after each,
 *     for a return that never happens, is the lift's own.
 *
 * The quirks kept, with their lines:
 *   - the error stack is reset to HIMEM-40, with R0-R9 pushed as junk
 *     "to stop pops getting carried away" (Basic.s:623-625). R0 = 0,
 *     and R1-R9 are whatever the last command left, exactly as the
 *     original leaves them;
 *   - when CALLEDNAME is zero (the -quit flag), OS_Exit is called
 *     before the prompt, so the prompt never appears
 *     (Basic.s:631-633);
 *   - the three cruncher warnings are MSGPRNXXX tokens 0, 1 and 2
 *     (unmatched (), line number too big, unmatched "). They are
 *     tested as SMODE bits &1000 and &100, and as the remainder masked
 *     with &FF not being 1 (Basic.s:646-677);
 *   - the immediate-mode line is ended by an &FF written at MATCH's
 *     R2 (Basic.s:678-679);
 *   - the last two slots of the two-byte table (tokens TWIN and TWINO)
 *     go to MISTAK. They are marked "was TWIN" and "was TWINO"
 *     (Basic.s:705-706);
 *   - APPEND numbers the appended lines from the LAST line's number
 *     plus 10. This is computed with the SMULL by (1<<32)/100, a
 *     multiply by the reciprocal. An empty program starts at 10
 *     (Command.s:23-60);
 *   - AUTO stops at 65280 and not at 65535 (CMP R4,#65280,
 *     Command.s:79). LIST's default end is 65279 (Command.s:492-493);
 *   - GETTWO (Command.s:82-96) is entered with R0 = 10 and returns
 *     through its stacked link. Its ERSILL error path is the lift's
 *     own, and is called plainly, like GOFACT in the landed
 *     statements;
 *   - HELP prints the memory figures as messages 26 and 25
 *     (Command.s:130-163). It computes TOP-PAGE, FSA-LOMEM and
 *     HIMEM-FSA, with FSA taken as GIVEEND takes it (Command.s:138);
 *   - INSTALL refuses unless HIMEM = MEMLIMIT (Command.s:178-180). It
 *     then chains the library at the top of the wimp stack, and moves
 *     it down a word at a time while R4 counts down with BHI
 *     (Command.s:182-193);
 *   - LIST's indent prescan:
 *     . THEN at the end of a line opens a multi-line block (+2,
 *       Command.s:597-599);
 *     . a negative indent count is reset to 0 (MOVMI,
 *       Command.s:553-554);
 *     . the indent used is the MINIMUM of the pushed old count and the
 *       new one (Command.s:600-602);
 *     . the only exception, ESCAPE, ends the listing
 *       (Command.s:543-545);
 *     . the compare against the search string skips 3 bytes over a
 *       TCONST constant (Command.s:590);
 *   - the TWIN spool exit reuses SAVEFILECLRSTK with the load/exec
 *     address MVN R2,#0 - (&F00-&B00) = &FFFFFBFF (Command.s:533-538
 *     and 1145-1146);
 *   - SAVEIC accepts only "REM ... >". That is spaces, a REM token,
 *     more spaces, and then ">". Anything else gives BADIC
 *     (Command.s:1162-1178);
 *   - a LIST spooled to a stream other than the screen ends by saving
 *     the program text (R7 >= 128, Command.s:531-538);
 *   - the EDIT command clears the top bit of TOP if the editor set it
 *     to mark that the program has changed (Command.s:453-458);
 *   - LVAR's head, for the static integers, prints the @% flags. It
 *     prints "+", and "g", "e" or "f" chosen by the &F0000 nybble and
 *     the &1000000 sign (Command.s:670-690). It then hands on to the
 *     tail of the lift's LVAR machinery (the FC10EF60 carve) with
 *     R5 = 3 and R6 = "A".
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "clrstk.h"

/* ---- the lift's functions this unit calls --------------------------
 * (the manifest's EXPORTS removes their `static` in the twin). */
void basicvfp_FROMAT(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                     uint32_t r8, uint32_t r14);
void basicvfp_POPLOCALAR(struct ros_cpu *s);
void basicvfp_DISPAT(struct ros_cpu *s);
void basicvfp_ERSYNT(struct ros_cpu *s);
void basicvfp_MISTAK(struct ros_cpu *s);
void basicvfp_ESCAPE(struct ros_cpu *s);
void basicvfp_ERCOMM(struct ros_cpu *s);
void basicvfp_INSRT(struct ros_cpu *s);
void basicvfp_AEEXPR(struct ros_cpu *s);
void basicvfp_AEEXDN(struct ros_cpu *s);
void basicvfp_EXPR(struct ros_cpu *s);
void basicvfp_ENDER(struct ros_cpu *s);
void basicvfp_GETTWO(struct ros_cpu *s);
void basicvfp_RENUM1(struct ros_cpu *s);
void basicvfp_RENUM2(struct ros_cpu *s);
void basicvfp_NPRN(struct ros_cpu *s);
void basicvfp_POSITE(struct ros_cpu *s);
void basicvfp_HELPPRN(struct ros_cpu *s);
void basicvfp_MSGPRNSSX(struct ros_cpu *s);
void basicvfp_MSGPRNCCC(struct ros_cpu *s);
void basicvfp_MSGPRNXXX(struct ros_cpu *s);
void basicvfp_LIBSUB(struct ros_cpu *s);
void basicvfp_LOADER(struct ros_cpu *s);
void basicvfp_LOADFILEFINAL(struct ros_cpu *s);
void basicvfp_OSFILELOAD(struct ros_cpu *s);
void basicvfp_INTEXC(struct ros_cpu *s);
void basicvfp_LISTLINE(struct ros_cpu *s);
void basicvfp_CHOUTNOCOUNT(struct ros_cpu *s);
void basicvfp_TWINBG(struct ros_cpu *s, uint32_t r8, uint32_t *p9,
                     uint32_t r14);
void basicvfp_FNDLNO(struct ros_cpu *s);
void basicvfp_FC10EF60(struct ros_cpu *s);    /* LVAR's machine tail */
void basicvfp_CRUNCHROUTINE(struct ros_cpu *s, uint32_t r0, uint32_t *p1,
                            uint32_t *p2, uint32_t *p3, uint32_t *p4,
                            uint32_t *p5, uint32_t *p6, uint32_t *p7,
                            uint32_t *p10, uint32_t r14);
void basicvfp_REMOVE(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                     uint32_t *p2, uint32_t *p3, uint32_t r4, uint32_t r5,
                     uint32_t *p6, uint32_t r8, uint32_t r13, uint32_t *p14);
void basicvfp_OSCLIREGS(struct ros_cpu *s, uint32_t *p1, uint32_t *p2,
                        uint32_t *p3, uint32_t *p4, uint32_t *p5,
                        uint32_t r8, uint32_t r12, uint32_t r13,
                        uint32_t r14);

/* The hand units' functions (landed). */
void basicvfp_hand_DONES(struct ros_cpu *s);
void basicvfp_hand_DONE(struct ros_cpu *s);
void basicvfp_hand_SPTSTN(struct ros_cpu *s);
void basicvfp_hand_SPACES(struct ros_cpu *s, uint32_t *p10, uint32_t *p12,
                          uint32_t r14);
void basicvfp_hand_OSSTRI(struct ros_cpu *s);
void basicvfp_hand_AEDONE(struct ros_cpu *s);
void basicvfp_hand_MATCH(struct ros_cpu *s);
void basicvfp_hand_AUMATCH(struct ros_cpu *s);
void basicvfp_hand_LEXTABADR(struct ros_cpu *s, uint32_t *p2, uint32_t r14);
void basicvfp_hand_SETVAR(struct ros_cpu *s, uint32_t *p0, uint32_t r8,
                          uint32_t r14);
void basicvfp_hand_SETFSA(struct ros_cpu *s);
void basicvfp_hand_INLINE(struct ros_cpu *s);
void basicvfp_hand_ORDERR(struct ros_cpu *s, uint32_t *p0, uint32_t r8,
                          uint32_t r14);
void basicvfp_hand_FLUSHCACHE(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                              uint32_t *p2, uint32_t r8, uint32_t r14);
void basicvfp_hand_MSG(struct ros_cpu *s);

/* The OS thunks. The harness's api_gen declares them, and each unit
 * declares them again, as the landed units do. */
void ros_thunk_OS_Exit(struct ros_cpu *s);
void ros_thunk_OS_Word(struct ros_cpu *s);
void ros_thunk_OS_File(struct ros_cpu *s);
void ros_thunk_OS_CLI(struct ros_cpu *s);

/* Tokens and constants, as the lift defines them from hdr/Tokens and
 * hdr/Definitions. Identical redefinitions across units are harmless. */
#define VARS             0x8700u      /* ARGP, Basic.s:619 */
#define TESCCOM          199u
#define TTWOCOMMLIMIT    160u
#define TESCSTMT         200u
#define TCONST           141u
#define TREM             244u
#define TNEXT            237u
#define TUNTIL           253u
#define TENDWH           206u
#define TENDCA           203u
#define TENDIF           205u
#define TFOR             227u
#define TREPEAT          245u
#define TWHILE           149u
#define TCASE            142u
#define TIF              231u
#define TTHEN            140u

/* The ROM data this unit addresses. */
#define BASICVFP_Basic_Title  0xFC100089u
#define BASICVFP_HELPTTL     0xFC10E7F0u
#define BASICVFP_HELPDATE    0xFC10E80Bu
#define BASICVFP_EDITENS     0xFC10EBBCu
#define BASICVFP_EDITTXT     0xFC10EBF6u
#define OsWord_ReadRealTimeClock 14u

/* MSG call sites. The error number and token are the two words after
 * the BL, and are read through R14. */
#define SITE_ERLISTO     0xFC10FE6Cu
#define SITE_BADIC       0xFC10FE8Cu
#define SITE_ERINSTALL   0xFC11018Cu

/* A transfer to another region is a guaranteed tail call. The chain of
 * them (the prompt loop, and the statement loop it enters) must not
 * grow the C stack. This is the lift's own ROS_TAIL_CALL contract,
 * written here as the lift defines it. */
#if defined(__clang__)
# define CLRSTK_TAIL_CALL(f) __attribute__((musttail)) return f(s);
#elif defined(__GNUC__) && __GNUC__ >= 15
# define CLRSTK_TAIL_CALL(f) return __attribute__((musttail)) f(s);
#else
# define CLRSTK_TAIL_CALL(f) return f(s);
#endif

static void clrstk_msg_at(struct ros_cpu *s, uint32_t site)
{
    s->r[14] = site;
    basicvfp_hand_MSG(s);
    ros_fault(s, site, "a transfer to an address that is not code");
}

/* =====================================================================
 * CLRSTK: the immediate-mode loop (Basic.s:619-707) and the seventeen
 * star-command bodies (Command.s). It is entered through the thunk from
 * every B CLRSTK in the interpreter. These include the program's end,
 * a HELP keyword match, INSTALL, the saved file, and BADPRO's B CLRSTK
 * inside ENDER's lift body, which now lands here through the thunk. It
 * is re-entered by its own goto wherever the original branched to
 * FSASET or CLRSTK.
 * ===================================================================== */
void basicvfp_hand_CLRSTK(struct ros_cpu *s)
{
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12,
             r13, r14;
    uint32_t v1, v2, v3, v4, v5, v6;
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];

    /* The entry is the CLRSTK label, exactly as in the lift's own head,
     * `goto CLRSTK`. NEW is a label INSIDE the function (the branch
     * target of the *NEW command). Entering there runs *NEW. That used
     * to wipe the program on every call, and broke 159 Rosetta programs
     * at load ("Syntax error" on the emptied prompt line). */
    goto CLRSTK;

NEW:
    /* NEW (Basic.s:613): DONES, FROMAT, then FSASET. */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                    /* BL DONES */
    basicvfp_hand_DONES(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    basicvfp_FROMAT(s, &r0, &r1, r8, 0xFFFFFFF0u);   /* BL FROMAT */
    s->r[0] = r0; s->r[1] = r1;                /* B FSASET */
    goto FSASET;

CLRSTK:
    /* Every B CLRSTK restarts the loop with the register file as the
     * branch left it. In the lift that is a fresh function entry, so
     * the locals are read again from R[], exactly as its head does. */
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];

    r8 = VARS;                                  /* MOV ARGP,#VARS */
    r12 = 0x8100u;                              /* ADD LINE,ARGP,#STRACC */
    r0 = ros_ld32(r8 - 132);                    /* LDR R0,[ARGP,#HIMEM] */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[8] = r8;
    s->r[9] = r9; s->r[10] = r10; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL POPLOCALAR */
    basicvfp_POPLOCALAR(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r14 = s->r[14];

    r13 = ros_ld32(r8 - 132);                   /* LDR SP,[ARGP,#HIMEM] */
    r13 -= 40;                                  /* STMFD SP!,{R0-R9} */
    ros_st32(r13, 0);                           /* R0 = 0 */
    ros_st32(r13 + 4, r1);
    ros_st32(r13 + 8, r2);
    ros_st32(r13 + 12, r3);
    ros_st32(r13 + 16, r4);
    ros_st32(r13 + 20, r5);
    ros_st32(r13 + 24, r6);
    ros_st32(r13 + 28, r7);
    ros_st32(r13 + 32, r8);
    ros_st32(r13 + 36, r9);
    ros_st8(r8 - 25, 0);                        /* STRB R0,[ARGP,#MEMM] */
    ros_st8(r8 - 26, 0xFFu);                    /* STRB MVN0,[ARGP,#BYTESM] */
    ros_st32(r8 - 120, r13);                    /* STR SP,[ARGP,#ERRSTK] */
    basicvfp_hand_ORDERR(s, &r0, r8, 0xFFFFFFF0u);   /* BL ORDERR */

    r0 = ros_ld8(r8 - 3);                       /* LDRB R0,[ARGP,#CALLEDNAME] */
    ros_subs(s, r0, 0);                         /* CMP R0,#0 */
    if (r0 == 0) {                              /* SWIEQ OS_Exit */
        s->r[0] = r0;
        s->r[13] = r13;
        ros_native_swi(s, ros_thunk_OS_Exit);
        if (s->v) ros_swi_raise(s);
        r0 = s->r[0];
    }
    basicvfp_hand_FLUSHCACHE(s, &r0, &r1, &r2, r8, 0xFFFFFFF0u);
    s->r[0] = r0; s->r[13] = r13;               /* SWI OS_WriteI+">" */
    ros_swi(s, 0x13Eu);
    r0 = s->r[0];
    s->r[1] = r1; s->r[2] = r2; s->r[14] = 0xFFFFFFF0u;
    basicvfp_hand_INLINE(s);                    /* BL INLINE ; R1=STRACC */
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    basicvfp_hand_SETVAR(s, &r0, r8, 0xFFFFFFF0u);   /* BL SETVAR */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL MATCH */
    basicvfp_hand_MATCH(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r13 = s->r[13]; r14 = s->r[14];

    r12 = r8 - 1280;                            /* ADD LINE,ARGP,#OUTPUT */
    s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPTSTN */
    basicvfp_hand_SPTSTN(s);
    r0 = s->r[0]; r1 = s->r[1]; r10 = s->r[10]; r12 = s->r[12];
    r14 = s->r[14];
    if (!s->z) goto DC;                         /* BNE DC */

    r13 -= 4;                                   /* STR SMODE,[SP,#-4]! */
    ros_st32(r13, r5);
    r4 = r0;                                    /* MOV R4,R0 */
    s->r[4] = r4; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INSRT */
    basicvfp_INSRT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];

    r5 = ros_ld32(r13);                         /* LDR SMODE,[SP],#4 */
    r13 += 4;
    ros_subs(s, r5, 0x1000u);                   /* CMP SMODE,#&1000 */
    if (r5 < 0x1000u) goto WARNC;               /* BCC WARNC */
    r0 = 0;                                     /* MOV R0,#0 */
    s->r[0] = r0; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL MSGPRNXXX */
    basicvfp_MSGPRNXXX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r13 = s->r[13]; r14 = s->r[14];

WARNC:
    ros_logic(s, r5 & 0x100u, 0);               /* TST SMODE,#256 */
    if (s->z) goto WARNQ;                       /* BEQ WARNQ */
    r0 = 1;                                     /* MOV R0,#1 */
    s->r[0] = r0; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL MSGPRNXXX */
    basicvfp_MSGPRNXXX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r13 = s->r[13]; r14 = s->r[14];

WARNQ:
    r5 &= 0xFFu;                                /* AND SMODE,SMODE,#255 */
    ros_logic(s, r5 ^ 1, s->c);                 /* TEQ SMODE,#1 */
    if (!s->z) {                                /* BNE FSASET */
        s->r[5] = r5;
        s->r[13] = r13;
        goto FSASET;
    }
    r0 = 2;                                     /* MOV R0,#2 */
    s->r[0] = r0; s->r[5] = r5; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL MSGPRNXXX */
    basicvfp_MSGPRNXXX(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r13 = s->r[13]; r14 = s->r[14];
    /* fall into FSASET */

FSASET:
    /* B FSASET is SETFSA then B CLRSTK (Basic.s:615-616). It is inlined
     * so that the > prompt loop stays in this one frame. */
    s->r[8] = r8;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SETFSA */
    basicvfp_hand_SETFSA(s);
    goto CLRSTK;

DC:
    r3 = 0xFFu;                                 /* MOV R3,#255 */
    ros_st8(r2, 0xFFu);                         /* STRB R3,[R2] */
    ros_subs(s, r10, TESCCOM);                  /* CMP R10,#TESCCOM */
    if (r10 != TESCCOM) {                       /* BNE DISPAT */
        s->r[3] = r3;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_DISPAT);      /* B DISPAT */
    }
    r10 = ros_ld8(r12);                         /* LDRB R10,[LINE],#1 */
    r12 += 1;
    ros_subs(s, r10, TTWOCOMMLIMIT);            /* CMP R10,#TTWOCOMMLIMIT */
    if (r10 >= TTWOCOMMLIMIT) {                 /* BCS ERSYNT */
        s->r[3] = r3; s->r[10] = r10; s->r[12] = r12;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_ERSYNT);
    }
    r4 = ros_subs(s, r10, 142);                 /* SUBS R4,R10,#&8E */
    if (r10 < 142) {                            /* BCC ERSYNT */
        s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_ERSYNT);
    }
    r4 = ros_ld32(0xFC10094Cu + (r4 << 2));     /* LDR R4,[PC,R4,LSL #2] */
    switch (r4) {                               /* ADD PC,PC,R4 (AJ2) */
    case 56588: goto APPEND;
    case 56708: goto AUTO;
    case 56836: goto CRUNCH;
    case 56856: goto DELETE;
    case 57920: goto EDIT;
    case 56900: goto HELP;
    case 58028: goto LIST;
    case 57100: goto LOAD;
    case 58800: goto LVAR;
    case -232:  goto NEW;
    case 60088: goto OLD;
    case 60112: goto RENUM;
    case 60464: goto SAVE;
    case 57108: goto TEXTLOAD;
    case 58068: goto TEXTSAVE;
    case 62896:                                /* TWIN, TWINO: was MISTAK */
        s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_MISTAK);
    case 57032: goto INSTALL;
    default:
        ros_fault(s, 0xFC100948u, "a jump table index out of range");
    }

APPEND:
    /* APPEND (Command.s:17-64): load a file after the program, and
     * number the appended lines from the last line's number + 10. */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXPR */
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL OSSTRI */
    basicvfp_hand_OSSTRI(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r1 = s->r[1]; r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r13 = s->r[13];
    r0 = ros_ld32(r8 - 148);                    /* LDR R0,[ARGP,#PAGE] */
    r2 = 0;                                     /* MOV R2,#0 */
APPENDNO:
    v1 = r0;
    r3 = ros_ld8(r0 + 1);                       /* LDRB R3,[R0,#1] */
    ros_subs(s, r3, 0xFFu);                     /* CMP R3,#&FF */
    if (r3 != 0xFFu) r3 = ros_ld8(r0 + 3);      /* LDRNEB R3,[R0,#3] */
    v2 = r2;
    if (!s->z) r2 = r0;                         /* MOVNE R2,R0 */
    if (!s->z) r0 += r3;                        /* ADDNE R0,R0,R3 */
    if (!s->z) goto APPENDNO;                   /* BNE APPENDNO */
    r3 = ros_ld8(r2 + 1);                       /* LDRB R3,[R2,#1] */
    /* LDRB R0,[R2,#2] ; ORR R7,R0,R3,LSL #8 */
    ros_logic(s, !s->z ? v1 : v2, s->c);        /* TEQ R2,#0 */
    r7 = r2 == 0 ? 10 : ros_ld8(r2 + 2) | (r3 << 8);  /* MOVEQ R7,#10 */
    r12 = ros_ld32(r8 - 144);                   /* LDR LINE,[ARGP,#TOP] */
    r2 = r12 - 2;                               /* SUB R2,LINE,#2 */
    s->r[2] = r2; s->r[3] = r3; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL OSFILELOAD */
    basicvfp_OSFILELOAD(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6];
    s->r[7] = r7;
    s->r[14] = 0xFFFFFFF0u;                     /* BL ENDER */
    basicvfp_ENDER(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10];
    r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    /* the FPOINT=2 step-10: SMULL by (1<<32)/100 (Command.s:52-60) */
    r0 = (uint32_t)((uint64_t)((int64_t)(int32_t)r7 *
                               (int64_t)(int32_t)0x28F5C29u) >> 32) + 1;
    r4 = (r0 + (((r0 << 2) - r0) << 3)) << 2;
    r0 = r12 - 2;                               /* SUB R0,LINE,#2 */
    r5 = 10;                                    /* MOV R5,#10 */
    s->r[0] = r0; s->r[4] = r4; s->r[5] = r5;
    s->r[14] = 0xFFFFFFF0u;                     /* BL RENUM2 */
    basicvfp_RENUM2(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    goto FSASET;

AUTO:
    /* AUTO (Command.s:65-81): number lines from start in steps of
     * step, until 65280. */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL GETTWO */
    basicvfp_GETTWO(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL ENDER */
    basicvfp_ENDER(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
L_FC10E6DC:
    r13 -= 8;                                   /* STMFD SP!,{R4,R5} */
    ros_st32(r13, r4);
    ros_st32(r13 + 4, r5);
    r0 = r4;                                    /* MOV R0,R4 */
    r9 = 0;                                     /* MOV TYPE,#0 */
    s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[9] = r9;
    s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL NPRN */
    basicvfp_NPRN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL INLINE */
    basicvfp_hand_INLINE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AUMATCH */
    basicvfp_hand_AUMATCH(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r13 = s->r[13];
    r4 = ros_ld32(r13);                         /* LDR R4,[SP] */
    r12 = r8 - 1280;                            /* ADD LINE,ARGP,#OUTPUT */
    s->r[4] = r4; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INSRT */
    basicvfp_INSRT(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL SETFSA */
    basicvfp_hand_SETFSA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r6 = s->r[6];
    r14 = s->r[14];
    v3 = r13;                                   /* LDMFD SP!,{R4,R5} */
    r5 = ros_ld32(r13 + 4);
    r13 += 8;
    r4 = ros_ld32(v3) + r5;                     /* ADD R4,R4,R5 */
    if (r4 < 0xFF00u) goto L_FC10E6DC;          /* BCC %B00 ; 65280 */
    s->r[4] = r4; s->r[5] = r5; s->r[13] = r13;  /* B FSASET */
    goto FSASET;

CRUNCH:
    /* CRUNCH (Command.s:97-101). */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXDN */
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r8 - 148);                    /* LDR R1,[ARGP,#PAGE] */
    basicvfp_CRUNCHROUTINE(s, r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7,
                           &r10, 0xFFFFFFF0u);
    ros_st32(r8 - 144, r2);                     /* STR R2,[ARGP,#TOP] */
    s->r[1] = r1; s->r[2] = r2; s->r[3] = r3; s->r[4] = r4;
    s->r[5] = r5; s->r[6] = r6; s->r[7] = r7; s->r[10] = r10;
    goto FSASET;                                /* B FSASET */

DELETE:
    /* DELETE (Command.s:102-112). */
    s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPTSTN */
    basicvfp_hand_SPTSTN(s);
    r0 = s->r[0]; r1 = s->r[1]; r10 = s->r[10]; r12 = s->r[12];
    r14 = s->r[14];
    if (!s->z) {                                /* BNE ERSYNT */
        s->r[3] = r3; s->r[4] = r4;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_ERSYNT);
    }
    r4 = r0;                                    /* MOV R4,R0 */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u);  /* BL SPACES */
    ros_subs(s, r10, 44);                       /* CMP R10,#"," */
    if (r10 != 44) {                            /* BNE ERCOMM */
        s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_ERCOMM);
    }
    s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPTSTN */
    basicvfp_hand_SPTSTN(s);
    r0 = s->r[0]; r1 = s->r[1]; r10 = s->r[10]; r12 = s->r[12];
    r14 = s->r[14];
    if (!s->z) {                                /* BNE ERSYNT */
        s->r[3] = r3; s->r[4] = r4;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_ERSYNT);
    }
    r5 = r0;                                    /* MOV R5,R0 */
    basicvfp_REMOVE(s, &r0, &r1, &r2, &r3, r4, r5, &r6, r8, r13,
                    &r14);                      /* BL REMOVE */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6;
    s->r[8] = r8;
    goto FSASET;

HELP:
    /* HELP (Command.s:113-165). */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u);  /* BL SPACES */
    basicvfp_hand_LEXTABADR(s, &r2, 0xFFFFFFF0u);      /* BL LEXTABADR */
    r0 = r12 - 1;                               /* SUB R0,LINE,#1 */
    r1 = BASICVFP_Basic_Title;                  /* ADRL R1,Basic_Title */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL HELPPRN */
    basicvfp_HELPPRN(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r10 = s->r[10];
    r13 = s->r[13]; r14 = s->r[14];
    ros_subs(s, r1, 0);                         /* CMP R1,#0 */
    if (r1 == 0) goto CLRSTK;                   /* BEQ CLRSTK ; hit */
    r1 = BASICVFP_HELPTTL;                      /* ADR R1,HELPTTL */
    r2 = BASICVFP_HELPDATE;                     /* ADR R2,HELPDATE */
    r0 = 26;                                    /* MOV R0,#26 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    s->r[14] = 0xFFFFFFF0u;                     /* BL MSGPRNSSX */
    basicvfp_MSGPRNSSX(s);
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r13 = s->r[13];
    r1 = ros_ld32(r8 - 144) - ros_ld32(r8 - 148);  /* TOP-PAGE */
    r14 = ros_ld32(r8 - 140);                   /* LDR R14,[ARGP,#FSA] */
    r2 = r14 - ros_ld32(r8 - 136);              /* END-LOMEM */
    r3 = ros_ld32(r8 - 132) - r14;              /* HIMEM-END */
    r0 = 25;                                    /* MOV R0,#25 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[14] = 0xFFFFFFF0u;                     /* BL MSGPRNCCC */
    basicvfp_MSGPRNCCC(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r13 = s->r[13];
    r14 = s->r[14];
    goto CLRSTK;                                /* B CLRSTK */

INSTALL:
    /* INSTALL (Command.s:177-194). */
    r0 = ros_ld32(r8 - 132);                    /* LDR R0,[ARGP,#HIMEM] */
    r1 = ros_ld32(r8 - 84);                     /* LDR R1,[ARGP,#MEMLIMIT] */
    ros_subs(s, r0, r1);                        /* CMP R0,R1 */
    if (r0 != r1) goto ERINSTALL;               /* BNE ERINSTALL */
    s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[4] = r4;
    s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL LIBSUB */
    basicvfp_LIBSUB(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r13 = s->r[13]; r14 = s->r[14];
    r1 = ros_ld32(r8 - 84);                     /* LDR R1,[ARGP,#MEMLIMIT] */
    /* SUB R1,R1,R4 ; LDR R0,[ARGP,#INSTALLLIST] ; STR R0,[R1,#-4]! */
    r1 = r1 - r4 - 4;
    ros_st32(r1, ros_ld32(r8 - 96));
    ros_st32(r8 - 84, r1);                      /* STR R1,[ARGP,#MEMLIMIT] */
    ros_st32(r8 - 132, r1);                     /* STR R1,[ARGP,#HIMEM] */
    ros_st32(r8 - 96, r1);                      /* STR R1,[ARGP,#INSTALLLIST] */
INSTALLCOPY:
    r2 += 4;                                    /* LDR R0,[R2,#4]! */
    r1 += 4;                                    /* STR R0,[R1,#4]! */
    ros_st32(r1, ros_ld32(r2));
    r4 = ros_subs(s, r4, 4);                    /* SUBS R4,R4,#4 */
    if (ros_cond(s, ROS_HI)) goto INSTALLCOPY;  /* BHI INSTALLCOPY */
    goto CLRSTK;                                /* B CLRSTK */

LOAD:
    /* LOAD (Command.s:195-196). */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL LOADER */
    basicvfp_LOADER(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[8] = r8;
    goto FSASET;

TEXTLOAD:
    /* TEXTLOAD (Command.s:197-202). */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXPR */
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL OSSTRI */
    basicvfp_hand_OSSTRI(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL LOADFILEFINAL */
    basicvfp_LOADFILEFINAL(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[8] = r8;
    goto FSASET;

EDIT:
    /* EDIT (Command.s:448-458): make sure the editor is loaded and run
     * it. Then clear the top bit of TOP if it was set to mark a change. */
    r0 = BASICVFP_EDITENS;                      /* ADR R0,EDITENS */
    s->r[0] = r0; s->r[10] = r10; s->r[12] = r12;
    ros_native_swi(s, ros_thunk_OS_CLI);        /* SWI OS_CLI */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    basicvfp_OSCLIREGS(s, &r1, &r2, &r3, &r4, &r5, r8, r12, r13,
                       0xFFFFFFF0u);            /* BL OSCLIREGS */
    r0 = BASICVFP_EDITTXT;                      /* ADR R0,EDITTXT */
    s->r[0] = r0;
    ros_native_swi(s, ros_thunk_OS_CLI);        /* SWI OS_CLI */
    if (s->v) ros_swi_raise(s);
    r0 = ros_ld32(r8 - 144);                    /* LDR R0,[ARGP,#TOP] */
    ros_subs(s, r0, 0);                         /* CMP R0,#0 */
    if ((int32_t)r0 >= 0) goto CLRSTK;          /* BPL CLRSTK */
    r0 &= 0x7FFFFFFFu;                          /* BIC R0,R0,#&80000000 */
    ros_st32(r8 - 144, r0);                     /* STR R0,[ARGP,#TOP] */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5;
    s->r[8] = r8;
    goto FSASET;

LIST:
    /* LIST (Command.s:462-471): the LIST O option sets the format. */
    r7 = ros_ld8(r8 - 27);                      /* LDRB R7,[ARGP,#LISTOP] */
    r0 = ros_ld8(r12);                          /* LDRB R0,[LINE] */
    ros_subs(s, r0, 79);                        /* CMP R0,#"O" */
    if (r0 != 79) goto LISTGO;                  /* BNE LISTGO */
    r12 += 1;                                   /* ADDEQ LINE,LINE,#1 */
    s->r[0] = r0; s->r[3] = r3; s->r[4] = r4; s->r[7] = r7;
    s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXDN */
    basicvfp_AEEXDN(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    ros_subs(s, r0, 32);                        /* CMP IACC,#32 */
    if (r0 >= 32) goto ERLISTO;                 /* BCS ERLISTO */
    ros_st8(r8 - 27, r0);                       /* STRB IACC,[ARGP,#LISTOP] */
    goto CLRSTK;                                /* B CLRSTK */

TEXTSAVE:
    /* TEXTSAVE (Command.s:472-490). */
    r0 = 192;                                   /* MOV R0,#64+128 */
    r11 = r12;                                  /* MOV AELINE,LINE */
    r10 = ros_ld8(r11);                         /* LDRB R10,[AELINE] */
    ros_subs(s, r10, 79);                       /* CMP R10,#"O" */
    if (r10 != 79) goto TEXTSAVEGO;             /* BNE TEXTSAVEGO */
    r11 += 1;                                   /* ADDEQ AELINE,AELINE,#1 */
    s->r[0] = r0; s->r[3] = r3; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL INTEXC */
    basicvfp_INTEXC(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r0 |= 0xC0u;                                /* ORR R0,R0,#64+128 */
TEXTSAVEGO:
    r13 -= 4;                                   /* STR R0,[SP,#-4]! */
    ros_st32(r13, r0);
    s->r[0] = r0; s->r[3] = r3; s->r[4] = r4; s->r[10] = r10;
    s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL EXPR */
    basicvfp_EXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL OSSTRI */
    basicvfp_hand_OSSTRI(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    r7 = ros_ld32(r13);                         /* LDR R7,[SP],#4 */
    r13 += 4;
    s->r[8] = r8;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SETFSA ; init fsa to top */
    basicvfp_hand_SETFSA(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r6 = s->r[6];
    r14 = s->r[14];
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    ros_subs(s, r10, 13);                       /* CMP R10,#13 */
    if (r10 != 13) {                            /* BNE ERSYNT */
        s->r[7] = r7; s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_ERSYNT);
    }
    /* fall into LISTGO */

LISTGO:
    /* LISTGO (Command.s:491-517): parse [start][,end] and an optional
     * IF, then list. */
    r4 = 0;                                     /* MOV R4,#0 ; start 0 */
    r5 = 0xFEFFu;                               /* MOV R5,#&FF ; ORR R5,R5,#&FE00 */
    s->r[0] = r0; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPTSTN */
    basicvfp_hand_SPTSTN(s);
    r0 = s->r[0]; r1 = s->r[1]; r10 = s->r[10]; r12 = s->r[12];
    r14 = s->r[14];
    if (!s->z) goto NONUML;                     /* BNE NONUML */
    r4 = r0;                                    /* MOV R4,R0 ; start */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u);  /* BL SPACES */
    if (r10 == 44) goto GOTCX;                  /* BEQ GOTCX */
    r5 = r4;                                    /* MOV R5,R4 */
    goto GOTCFF;                                /* B GOTCFF */
NONUML:
    if (r10 != 44) goto GOTCFF;                 /* BNE GOTCFF */
GOTCX:
    s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL SPTSTN */
    basicvfp_hand_SPTSTN(s);
    r0 = s->r[0]; r1 = s->r[1]; r10 = s->r[10]; r12 = s->r[12];
    r14 = s->r[14];
    if (s->z) r5 = r0;                          /* MOVEQ R5,R0 ; end */
    if (s->z)                                   /* BLEQ SPACES */
        basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u);
GOTCFF:
    /* CMP R10,#TIF ; MOV AELINE,LINE ; SUBNE AELINE,LINE,#1 */
    r11 = r10 != TIF ? r12 - 1 : r12;
    if (r10 != TIF) {                           /* BLNE DONE */
        s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[7] = r7;
        s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_DONE(s);
        r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
        r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
        r13 = s->r[13]; r14 = s->r[14];
    }
    s->r[3] = r3; s->r[4] = r4; s->r[5] = r5; s->r[7] = r7;
    s->r[10] = r10; s->r[11] = r11; s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL ENDER */
    basicvfp_ENDER(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    basicvfp_TWINBG(s, r8, &r9, 0xFFFFFFF0u);   /* BL TWINBG */
    r0 = r4;                                    /* MOV IACC,R4 */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL FNDLNO ; find it */
    basicvfp_FNDLNO(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r14 = s->r[14];
    r12 = r1;                                   /* MOV LINE,R1 */
    r6 = 0;                                     /* MOV R6,#0 */
    goto GETNUM;                                /* B GETNUM */

ENDLN:
    r0 = 10;                                    /* MOV R0,#10 */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL CHOUTNOCOUNT */
    basicvfp_CHOUTNOCOUNT(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_subs(s, r7, 128);                       /* CMP R7,#128 */
    if (r7 < 128) ros_swi(s, 0x10Du);           /* SWICC OS_WriteI+13 */
    ros_st32(r8 - 264, 0);                      /* STR 0,[ARGP,#TALLY] */
GETNEW:
    r12 = r2 + ros_ld8(r2 + 3);                 /* ADD LINE,R2,R0 */
GETNUM:
    r1 = ros_ld8(r12 + 2);                      /* LDRB R1,[LINE,#2] */
    r0 = r1 | (ros_ld8(r12 + 1) << 8);          /* ORR R0,R1,R0,LSL #8 */
    if (r0 <= r5) goto LTTEST;                  /* BLS LTTEST */
    ros_subs(s, r7, 128);                       /* CMP R7,#128 */
    if (r7 < 128) goto CLRSTK;                  /* BCC CLRSTK ; end of LIST */
    r5 = r9;                                    /* MOV R5,TYPE ; end address */
    basicvfp_TWINBG(s, r8, &r9, 0xFFFFFFF0u);   /* BL TWINBG */
    r4 = r9;                                    /* MOV R4,TYPE ; start */
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC */
    r2 = 0xFFFFFFFFu;                           /* MVN R2,#0 */
    goto SAVEFILECLRSTK;                        /* B SAVEFILECLRSTK */

LTTEST:
    r3 = ros_ld8(r8 - 112);                     /* LDRB R3,[ARGP,#ESCFLG] */
    ros_subs(s, r3, 128);                       /* CMP R3,#&80 */
    if (r3 >= 128) {                            /* BCS ESCAPE */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        s->r[4] = r4; s->r[6] = r6; s->r[9] = r9; s->r[10] = r10;
        s->r[12] = r12; s->r[13] = r13;
        s->r[15] = s->r[14];
        CLRSTK_TAIL_CALL(basicvfp_ESCAPE);
    }
    /* MOV R4,#0 ; LDRB R14,[AELINE] ; CMP R14,#13 ; MOVEQ R4,#1 */
    r4 = ros_ld8(r11) == 13;
    r3 = 0;                                     /* MOV R3,#0 ; expansion */
    r2 = r12;                                   /* MOV R2,LINE ; save line */
    r12 += 4;                                   /* ADD LINE,LINE,#4 */
    if ((int32_t)r6 < 0) r6 = 0;                /* MOVMI R6,#0 */
    r13 -= 4;                                   /* STR R6,[SP,#-4]! */
    ros_st32(r13, r6);
LTLOOP:
    r10 = ros_ld8(r12);                         /* LDRB R10,[LINE],#1 */
    r12 += 1;
    if (r10 == 13) goto CHKLST;                 /* BEQ CHKLST */
    if (r10 == TREM) r3 = r10;                  /* MOVEQ R3,R10 */
    if (r10 == 34) r3 ^= r10;                   /* EOREQ R3,R3,R10 */
    if (r3 != 0) goto LPSIMT;                   /* BNE LPSIMT */
    if (r10 == TNEXT || r10 == TUNTIL || r10 == TENDWH ||
        r10 == TENDCA || r10 == TENDIF)
        r6 -= 2;                                /* SUBEQ R6,R6,#2 */
    if (r10 == TFOR || r10 == TREPEAT)
        r6 += 2;                                /* ADDEQ R6,R6,#2 */
    v4 = ros_ld8(r12 - 2) ^ TESCSTMT;           /* TEQ R14,#TESCSTMT */
    if (v4 != 0) goto LPSIMT;                   /* BNE LPSIMT */
    if (r10 == TWHILE || r10 == TCASE)
        r6 += 2;                                /* ADDEQ R6,R6,#2 */
    goto LTLOOP;                                /* B LTLOOP */
LPSIMT:
    v5 = r4 & 1;                                /* TST R4,#1 */
    if (v5 != 0) goto LTLOOP;                   /* BNE LTLOOP */
    r1 = 0;                                     /* MOV R1,#0 ; offset */
LPSIMS:
    r14 = ros_ld8(r11 + r1);                    /* LDRB R14,[AELINE,R1] */
    if (r14 == 13) r4 = 1;                      /* MOVEQ R4,#1 */
    if (r14 == 13) goto LTLOOP;                 /* BEQ LTLOOP */
    v6 = r10;                                   /* CMP R10,#TCONST */
    if (r10 == TCONST) r12 += 3;                /* ADDEQ LINE,LINE,#3 */
    r10 = ros_ld8(r12 + r1);                    /* LDRB R10,[LINE,R1] */
    r1 += 1;                                    /* ADD R1,R1,#1 */
    if (r14 == v6) goto LPSIMS;                 /* BEQ LPSIMS */
    goto LTLOOP;                                /* B LTLOOP */
CHKLST:
    r14 = ros_ld8(r12 - 2);                     /* LDRB R14,[LINE,#-2] */
    if (r14 == TTHEN) r6 += 2;                  /* ADDEQ R6,R6,#2 */
    r3 = ros_ld32(r13);                         /* LDR R3,[SP],#4 */
    r13 += 4;
    ros_subs(s, r3, r6);                        /* CMP R3,R6 */
    if (r3 >= r6) r3 = r6;                      /* MOVCS R3,R6 ; minimum */
    if (r4 == 0) goto GETNEW;                   /* BEQ GETNEW */
    r12 = r2 + 4;                               /* ADD LINE,R2,#4 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[6] = r6; s->r[9] = r9; s->r[10] = r10;
    s->r[12] = r12; s->r[13] = r13;
    s->r[14] = 0xFFFFFFF0u;                     /* BL LISTLINE */
    basicvfp_LISTLINE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13]; r14 = s->r[14];
    goto ENDLN;                                 /* B ENDLN */

LVAR:
    /* LVAR (Command.s:660-712): the head, for the static integers. The
     * walk over A..Z and everything after it is the lift's FC10EF60
     * machinery. */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13];
    r0 = 10;                                    /* MOV R0,#10 */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL MSGPRNXXX */
    basicvfp_MSGPRNXXX(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r6 = s->r[6];
    r13 = s->r[13];
    r5 = ros_ld32(r8 - 0x100u);                 /* LDR R5,[ARGP,#INTVAR] */
    r7 = r8 - 252;                              /* ADD R7,ARGP,#INTVAR+4 */
    ros_subs(s, r5, 0x1000000u);                /* CMP R5,#&1000000 */
    if (r5 >= 0x1000000u) ros_swi(s, 0x12Bu);   /* SWICS OS_WriteI+"+" */
    r4 = r5 & 0xF0000u;                         /* AND R4,R5,#&F0000 */
    ros_subs(s, r4, 0x10000u);                  /* CMP R4,#&10000 */
    if (r4 < 0x10000u) ros_swi(s, 0x167u);      /* SWICC OS_WriteI+"g" */
    if (s->z) ros_swi(s, 0x165u);               /* SWIEQ OS_WriteI+"e" */
    if (ros_cond(s, ROS_HI)) ros_swi(s, 0x166u);/* SWIHI OS_WriteI+"f" */
    r0 = r5 & 0xFFu;                            /* AND R0,R5,#&FF */
    r9 = 0;                                     /* MOV TYPE,#0 */
    s->r[0] = r0; s->r[4] = r4; s->r[5] = r5; s->r[7] = r7;
    s->r[9] = r9;
    s->r[14] = 0xFFFFFFF0u;                     /* BL POSITE */
    basicvfp_POSITE(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
    r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12];
    r13 = s->r[13];
    ros_logic(s, r5 & 0x800000u, 0);            /* TST R5,#&800000 */
    if (s->z) ros_swi(s, 0x12Eu);               /* SWIEQ OS_WriteI+"." */
    if (!s->z) ros_swi(s, 0x12Cu);              /* SWINE OS_WriteI+"," */
    r0 = (r5 & 0xFF00u) >> 8;                   /* AND R0,R5,#&FF00 ; MOV R0,LSR #8 */
    s->r[0] = r0;
    s->r[14] = 0xFFFFFFF0u;                     /* BL POSITE */
    basicvfp_POSITE(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    ros_swi(s, 0x122u);                         /* SWI OS_WriteI+""" */
    r0 = s->r[0];
    r6 = 65;                                    /* MOV R6,#"A" */
    r5 = 3;                                     /* MOV R5,#3 */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
    s->r[8] = r8; s->r[9] = r9; s->r[10] = r10; s->r[11] = r11;
    s->r[12] = r12; s->r[13] = r13;
    s->r[15] = s->r[14];
    CLRSTK_TAIL_CALL(basicvfp_FC10EF60);        /* B LVAR1 machine */

OLD:
    /* OLD (Command.s:1023-1028). */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL DONES */
    basicvfp_hand_DONES(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r0 = 0;                                     /* MOV R0,#0 */
    r1 = ros_ld32(r8 - 148);                    /* LDR R1,[ARGP,#PAGE] */
    ros_st8(r1 + 1, 0);                         /* STRB R0,[R1,#1] */
    s->r[0] = r0; s->r[1] = r1;
    s->r[14] = 0xFFFFFFF0u;                     /* BL ENDER */
    basicvfp_ENDER(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[8] = r8;
    goto FSASET;

RENUM:
    /* RENUM (Command.s:1029-1031). */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL GETTWO */
    basicvfp_GETTWO(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13];
    s->r[14] = 0xFFFFFFF0u;                     /* BL RENUM1 */
    basicvfp_RENUM1(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[8] = r8;
    goto FSASET;

SAVE:
    /* SAVE (Command.s:1134-1178). */
    basicvfp_hand_SPACES(s, &r10, &r12, 0xFFFFFFF0u);  /* BL SPACES */
    ros_logic(s, r10 ^ 0xDu, s->c);             /* TEQ R10,#13 */
    if (s->z) goto SAVEIC;                      /* BEQ SAVEIC */
    r12 -= 1;                                   /* SUB LINE,LINE,#1 */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEEXPR */
    basicvfp_AEEXPR(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL OSSTRI */
    basicvfp_hand_OSSTRI(s);
    r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
    r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
    r8 = s->r[8]; r9 = s->r[9]; r10 = s->r[10]; r11 = s->r[11];
    r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL AEDONE */
    basicvfp_hand_AEDONE(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    s->r[14] = 0xFFFFFFF0u;                     /* BL ENDER */
    basicvfp_ENDER(s);
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r1 = r8 - 1536;                             /* ADD R1,ARGP,#STRACC */
SAVEFIL:
    r4 = ros_ld32(r8 - 148);                    /* LDR R4,[ARGP,#PAGE] */
    r5 = ros_ld32(r8 - 144);                    /* LDR R5,[ARGP,#TOP] */
    r2 = 0xFFFFFBFFu;                           /* MVN R2,#0 ; SUB &F00-&B00 */
    /* fall into SAVEFILECLRSTK: save R4 to R5 as the file named at
     * [R1], with type R2 and a date stamp */

SAVEFILECLRSTK:
    r13 -= 4;                                   /* STR R1,[SP,#-4]! */
    ros_st32(r13, r1);
    r0 = OsWord_ReadRealTimeClock;              /* MOV R0,#OsWord... */
    r1 = r13 - 8;                               /* SUB R1,SP,#8 */
    ros_st32(r1, 3);                            /* MOV R3,#3 ; STR R3,[R1] */
    ros_st32(r1 + 4, r2);                       /* STR R2,[R1,#4] */
    s->r[0] = r0; s->r[1] = r1; s->r[10] = r10; s->r[12] = r12;
    s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_Word);       /* SWI OS_Word */
    if (s->v) ros_swi_raise(s);
    r0 = 0;                                     /* MOV R0,#0 */
    r2 = ros_ld32(r1 + 4);                      /* LDR R2,[R1,#4] */
    r3 = ros_ld32(r1);                          /* LDR R3,[R1] */
    r1 = ros_ld32(r13);                         /* LDR R1,[SP],#4 */
    r13 += 4;
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
    s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[13] = r13;
    ros_native_swi(s, ros_thunk_OS_File);       /* SWI OS_File */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
    r5 = s->r[5]; r6 = s->r[6];
    goto CLRSTK;                                /* B CLRSTK */

SAVEIC:
    /* SAVEIC (Command.s:1162-1178): save the immediate command. */
    s->r[3] = r3; s->r[4] = r4; s->r[10] = r10; s->r[12] = r12;
    s->r[14] = 0xFFFFFFF0u;                     /* BL ENDER */
    basicvfp_ENDER(s);
    r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4]; r5 = s->r[5];
    r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8]; r9 = s->r[9];
    r10 = s->r[10]; r11 = s->r[11]; r12 = s->r[12]; r13 = s->r[13];
    r14 = s->r[14];
    r1 = ros_ld32(r8 - 148);                    /* LDR R1,[ARGP,#PAGE] */
    r0 = ros_ld8(r1 + 1);                       /* LDRB R0,[R1,#1] */
    ros_subs(s, r0, 0xFFu);                     /* CMP R0,#&FF */
    if (r0 == 0xFFu) goto BADIC;                /* BEQ BADIC */
    r1 += 4;                                    /* ADD R1,R1,#4 */
SAVEI2:
    r0 = ros_ld8(r1);                           /* LDRB R0,[R1],#1 */
    r1 += 1;
    v4 = r0 ^ 0x20u;                            /* TEQ R0,#" " */
    if (v4 == 0) goto SAVEI2;                   /* BEQ SAVEI2 */
    ros_logic(s, r0 ^ TREM, s->c);              /* TEQ R0,#TREM */
    if (!s->z) goto BADIC;                      /* BNE BADIC */
SAVEI3:
    r0 = ros_ld8(r1);                           /* LDRB R0,[R1],#1 */
    r1 += 1;
    v5 = r0 ^ 0x20u;                            /* TEQ R0,#" " */
    if (v5 == 0) goto SAVEI3;                   /* BEQ SAVEI3 */
    ros_logic(s, r0 ^ 0x3Eu, s->c);             /* TEQ R0,#">" */
    if (s->z) goto SAVEFIL;                     /* BEQ SAVEFIL */
    goto BADIC;                                 /* B BADIC */

ERLISTO:
    clrstk_msg_at(s, SITE_ERLISTO);             /* BL MSG ; = 22,4 */
BADIC:
    clrstk_msg_at(s, SITE_BADIC);               /* BL MSG ; = 17,14 */
ERINSTALL:
    clrstk_msg_at(s, SITE_ERINSTALL);           /* BL MSG ; = 17,15 */
}
