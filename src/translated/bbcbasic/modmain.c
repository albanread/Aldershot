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
 * (Sources/Programmer/BASIC: s.Basic, s.ModHead, hdr.WorkSpace).
 */

/* modmain.c -- MODULEMAIN, the module's start entry, translated by
 * hand from Basic.s:23-612 (RISC OS 5.31's BASIC, the VFP build). It
 * covers:
 *   - the mode switch and OS_GetEnv;
 *   - the VFPSupport context check (32 registers, falling back to 16),
 *     with the NoStore error when the application space cannot hold
 *     VARS+VFPCONTEXT+256;
 *   - MAIN. That is the SWI 0 trampoline in SWICODE, WFS, the tables
 *     from the VFPData resource file, the creation of the VFP context
 *     and the feature flags, and the four OS_ChangeEnvironment
 *     handlers saved to OLDERR. Then FREEPTR, PAGE and HIMEM, the
 *     zeroed workspace, the SEED/ARW! quirk, the REPSTR banner copied
 *     into ERRORS, FROMAT, SETFSA and ORDERR, and the junk stack push;
 *   - the CALLEDNAME machinery. That is ENTRE1's copy loop, ENTRE2's
 *     space skip, and the -help, -load, -quit and -chain keywords,
 *     spelled out with RDCOMCHER's flag checks. Then ENTRYCHAIN and
 *     ENTRYCHAIN1, the in-core @hex,hex form (RDHEX twice,
 *     LOADFILEINCORE), the plain filename (ENTRYF's copy to STRACC and
 *     LOADFILEFINAL), and ENTRYFINAL's CRUNCHCHK/CRUNCHROUTINE, which
 *     hands on to RUNNER.
 * The transfers out (CLRSTK, ENTRYUNK, ENTRYHELP, BADIPHEX, FSASET and
 * RUNNER) are the lift's own guaranteed tail calls. So this frame
 * never sits under the interpreter it starts. The lift has 715 lines,
 * here made into one C function. The labels are the original's, used
 * as gotos.
 *
 * The conventions are the ones already landed. State is kept in R[]. A
 * BL becomes a marker call (R14 := 0xFFFFFFF0u). The lift's register
 * stores and reloads around it are copied one for one, so the ARM
 * register file at every boundary is the lift's. A B becomes a plain
 * call whose result is discarded, or a goto when the target is a label
 * in this function.
 *
 * THE SETJMP QUESTION, settled for this unit (the clrstk method).
 * The lifted body holds 21 setjmp resume points:
 *   - four around BL TITLE (&FC10087C, &FC1007E8, &FC10072C,
 *     &FC1005E8);
 *   - fourteen around BL RDCOMCHER (&FC100700-&FC100720,
 *     &FC100778-&FC1007B8 and &FC1007D4-&FC1007E4);
 *   - one around BL MSGPRNXXX (&FC1007F0);
 *   - one around BL LOADFILEFINAL (&FC100768);
 *   - one around BL LOADFILEINCORE (&FC10062C).
 * A resume into this function would need a deep callee to execute
 * ros_resume with t set to one of these addresses. In the twin no such
 * path exists:
 *   - The performers are the functions that call ros_resume. They are:
 *     . the lift's DISPAT. Its MOV PC,R7 goes to a program line address
 *       resolved by GOFACT, or to the AJ7 continuation built from R10's
 *       low bits. Both are arena or ladder values. Its two MOV PC,R11
 *       go to the SYS statement's AELINE, which is user code;
 *     . the three operator jump-table fragments FC1029D4/A28/A48
 *       (ADD PC,R4,R14,LSL #2, the FC102 ladder);
 *     . GTARGS, lift and hand. Its saved entry link is its own
 *       caller's site;
 *     . MSGATLINE. Its entry LR is the message system's ERRXLATE
 *       continuation;
 *     . the hand Basic_Code. Its entry R14 is the module-enter glue;
 *     . the hand FNRET (R7, a program address);
 *     . the hand SYS pair (R11, AELINE).
 *     None of these sources of t can hold a MODULEMAIN site. The
 *     arena's user-code addresses are guest memory, and the ladder is
 *     the FC102 band. The performers that carry a link are called only
 *     from DISPAT's region or from the OS. MODULEMAIN is not one of
 *     their callers. It enters the interpreter only by tail call
 *     (RUNNER, FSASET or CLRSTK). By then its frame has gone, and so
 *     has any resume point it had pushed.
 *   - Each callee that the lifter wrapped to return through R14
 *     STORES the link and returns through it as a plain lift return.
 *     So the round trip cannot skip a C frame:
 *     . TITLE's STMFD {R0-R12,R14} is popped by FC1006C0's
 *       LDMFD {R0-R12,PC}. The hand TITLE keeps the whole round trip
 *       inside its own frame before it returns;
 *     . MSGPRNXXX's STMFD {R1-R7,LR} is popped by MSGPRNTOK3's
 *       LDMFD {R1-R7,PC};
 *     . LOADFILEFINAL tail-calls LOADFILEINCORE. Its STR R14,[SP,#-4]!
 *       is popped by its LDR PC,[SP],#4, on both exits;
 *     . RDCOMCH ends in a plain MOV PC,R14.
 *     RDCOMCHER's other exit is the BNE ENTRYUNK on the caller's
 *     flags. It never returns at all. It runs on through ENTRYHELP and
 *     FSASET to the prompt loop.
 *   - So no resume point is pushed, and the function holds no setjmp.
 *     All 21 sites are plain calls, each made with the marker in R14.
 *     No callee reads its call site. MODULEMAIN has no BL MSG of its
 *     own. The one MSG in this area is BADIPHEX's, at BADIPHEX's own
 *     site. Because there is no setjmp, the fourteen transfer sites
 *     also stay legal musttails, and the endpr rule cannot apply.
 *
 * The quirks kept, with their lines:
 *   - before anything else, the entry drops to USR mode with IRQs and
 *     FIQs enabled (WritePSRc, Basic.s:24);
 *   - the VFP context is asked for 32 registers first. It is retried
 *     with 16 only when the X form returns an error (Basic.s:28-33).
 *     The space test is HIMEM-context-VARS-(VFPCONTEXT+256) >= 0
 *     (Basic.s:36-38). If that fails, the ErrorBase_BASIC+&FF
 *     "NoStore" block goes through XMessageTrans_ErrorLookup,
 *     OS_GenerateError and OS_Exit (Basic.s:44-49), and never comes
 *     back;
 *   - the SWICODE area at ARGP-44 is written as a live SWI 0 followed
 *     by OSESCRT's MOV PC,R14 (Basic.s:246-254). This is the CALL
 *     statement's trampoline. The code cache is synchronised over
 *     exactly those eight bytes;
 *   - WFS &70000 runs even in the VFP build (Basic.s:256-257: "we
 *     still rely on FPA for some ops");
 *   - the VFPData resource file is opened WITHOUT an X SWI
 *     ("Deliberately not using X SWIs; the file should exist!",
 *     Basic.s:261-262). The address of its tables is taken with
 *     OS_FSControl 21;
 *   - the features probe: VFPFLAG_Vectors comes from bit 0 of R0 even
 *     when the X form returned an error pointer. (The ASSERT at
 *     Basic.s:284-285 notes that the flag value is below 4, so an
 *     error pointer reads as no vectors.) The SystemRegs probe has no
 *     X bit on purpose ("this reason code has been around forever",
 *     Basic.s:288). NEON is the &F000 nybble of R2 (Basic.s:289-290);
 *   - the four old handlers (error, with STRACC as the buffer, escape,
 *     exit and upcall) are saved at OLDERR in exactly the order 6, 9,
 *     11, 16 (Basic.s:298-319). OSQUITR and PUTBACKHAND later replay
 *     that walk;
 *   - FREEPTR and PAGE both start at VFPCONTEXT plus the context SIZE
 *     that CheckContext returned in R6 (Basic.s:320-327). So the
 *     program begins directly after the VFP state;
 *   - HIMEM = MEMLIMIT = OS_GetEnv's R1 (Basic.s:328-331);
 *   - the initial workspace: ten zero words and bytes, WIDTHLOC = -1
 *     and BYTESM = &FF (Basic.s:332-345), and @% = 10 OR &900
 *     (Basic.s:346-348);
 *   - the SEED quirk: the ORRS takes in the bottom bit of the fifth
 *     byte (Basic.s:349-351), and an all-zero seed is replaced by
 *     MYNAME, "ARW!" (Basic.s:352-353). This gives a fixed seed for a
 *     fresh workspace;
 *   - the banner string REPSTR has two uses. It is TITLE's OS_WriteS
 *     text, and it is also copied byte by byte into ERRORS as the
 *     initial error buffer (Basic.s:354-359);
 *   - the junk STMFD {R0-R9} under HIMEM is there "to stop pops
 *     getting carried away" (Basic.s:366-368). R0 = 0, and R1-R9 are
 *     whatever MAIN left, exactly as the original leaves them;
 *   - the terminator that ends CALLEDNAME is overwritten with 0 at
 *     [R3,#-1] (Basic.s:378-379). The name is skipped over, and not
 *     parsed. ENTRE2 discards the spaces after it (Basic.s:380-382);
 *   - the keyword spellings: RDCOMCH converts to upper case only the
 *     characters at or above "a" (BICCS, Basic.s:609-611). RDCOMCHER
 *     is ENTERED WITH ITS CALLER'S FLAGS. The BNE at Basic.s:608 tests
 *     the CMP made before the BL. So "CHAIN" is spelled out as CMP/BL
 *     five times, and the exit on a mismatch is one function away
 *     (Basic.s:484-494);
 *   - -quit jumps to ENTRYCHAIN1, SKIPPING ENTRYCHAIN's BL TITLE
 *     (Basic.s:528). This is the quiet start: -chain and -load print
 *     the banner, and -quit does not;
 *   - ENTRYCHAIN1's LDREQB post-increment happens only when the load
 *     happens (Basic.s:499-501);
 *   - the @hex,hex form: RDHEX takes exactly eight digits (R4 counts
 *     down from 32-4, Basic.s:589-604). The comma and the terminator
 *     are checked. The START must be strictly ABOVE the end (CMP/BLS
 *     BADIPHEX, Basic.s:400-401);
 *   - a plain filename is copied to STRACC with a CR appended, and is
 *     handed to LOADFILEFINAL, the "internals of TEXTLOAD"
 *     (Basic.s:506-515);
 *   - ENTRYFINAL: a load that is not a chain goes straight to FSASET
 *     (TST R9,#2, Basic.s:405-407). A run started by name goes
 *     straight to RUNNER (Basic.s:408-410). Otherwise BASIC$Crunch is
 *     checked (CRUNCHCHK, Basic.s:411-413). Only if the variable is
 *     set does the program get crunched, with SAFECRUNCH = 15 ("15 is
 *     a nice safe looking number", ModHead.s:52), before RUNNER.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "modmain.h"

/* ---- the lift's functions this unit calls --------------------------
 * (the manifest's EXPORTS removes their `static` in the twin). */
void basicvfp_FROMAT(struct ros_cpu *s, uint32_t *p0, uint32_t *p1,
                     uint32_t r8, uint32_t r14);
void basicvfp_RDCOMCHER(struct ros_cpu *s);
void basicvfp_RDCOMCH(struct ros_cpu *s);
void basicvfp_RDHEX(struct ros_cpu *s);
void basicvfp_ENTRYUNK(struct ros_cpu *s);
void basicvfp_ENTRYHELP(struct ros_cpu *s);
void basicvfp_BADIPHEX(struct ros_cpu *s);
void basicvfp_FSASET(struct ros_cpu *s);
void basicvfp_CLRSTK(struct ros_cpu *s);
void basicvfp_RUNNER(struct ros_cpu *s);
void basicvfp_CRUNCHCHK(struct ros_cpu *s, uint32_t r0, uint32_t r1,
                        uint32_t r2, uint32_t r3, uint32_t r4,
                        uint32_t r13, uint32_t r14);
void basicvfp_CRUNCHROUTINE(struct ros_cpu *s, uint32_t r0, uint32_t *p1,
                            uint32_t *p2, uint32_t *p3, uint32_t *p4,
                            uint32_t *p5, uint32_t *p6, uint32_t *p7,
                            uint32_t *p10, uint32_t r14);
void basicvfp_MSGPRNXXX(struct ros_cpu *s);
void basicvfp_LOADFILEFINAL(struct ros_cpu *s);
void basicvfp_LOADFILEINCORE(struct ros_cpu *s);

/* The hand units' functions (landed). */
void basicvfp_hand_TITLE(struct ros_cpu *s);
void basicvfp_hand_SETFSA(struct ros_cpu *s);
void basicvfp_hand_ORDERR(struct ros_cpu *s, uint32_t *p0, uint32_t r8,
                          uint32_t r14);

/* The OS thunks. The harness's api_gen declares them, and each unit
 * declares them again, as the landed units do. */
void ros_thunk_OS_GetEnv(struct ros_cpu *s);
void ros_thunk_OS_Find(struct ros_cpu *s);
void ros_thunk_OS_FSControl(struct ros_cpu *s);
void ros_thunk_OS_ChangeEnvironment(struct ros_cpu *s);
void ros_thunk_OS_GenerateError(struct ros_cpu *s);
void ros_thunk_OS_Exit(struct ros_cpu *s);

/* Workspace layout (hdr/WorkSpace, as the lift defines it). Identical
 * redefinitions across units are harmless. */
#define VARS             0x8700u      /* ARGP, Basic.s:245 */
#define VFPCONTEXT       0x1000u      /* the VFP state at ARGP+&1000 */

/* ModHead's constant. */
#define SAFECRUNCH       15u          /* ModHead.s:52 */

/* VFPSupport reason codes and flags (Hdr:VFPSupport), with the values
 * of the lift's immediates. */
#define VFPSupport_Features_Misc        2u
#define VFPSupport_Features_SystemRegs  0u
#define VFPFLAG_Vectors                 1u
#define VFPFLAG_NEON                    2u

/* The ROM data this unit addresses (rom_basicvfp.h's values). */
#define BASICVFP_VFPDataResourceFile 0xFC100114u
#define BASICVFP_OSESCR              0xFC100198u
#define BASICVFP_OSERRR              0xFC1001ACu
#define BASICVFP_OSUPCR              0xFC1001CCu
#define BASICVFP_OSQUITR             0xFC100364u
#define BASICVFP_SEVEREERROR         0xFC10018Cu
#define BASICVFP_REPSTR              0xFC100698u
#define BASICVFP_MSGATLINE           0xFC10F650u

/* A transfer to another region is a guaranteed tail call. The chain of
 * them (the prompt loop, and the statement loop it enters) must not
 * grow the C stack. This is the lift's own ROS_TAIL_CALL contract,
 * written here as the lift defines it. */
#if defined(__clang__)
# define MODMAIN_TAIL_CALL(f) __attribute__((musttail)) return f(s);
#elif defined(__GNUC__) && __GNUC__ >= 15
# define MODMAIN_TAIL_CALL(f) return __attribute__((musttail)) f(s);
#else
# define MODMAIN_TAIL_CALL(f) return f(s);
#endif

/* =====================================================================
 * MODULEMAIN (Basic.s:23-612): the start entry. It is reached once
 * each time the language starts, through the lift's address table at
 * &FC100138 (the module head's first word, ModHead.s:88). Everything
 * before MAIN negotiates the VFP context. MAIN sets up the workspace.
 * The rest is the CALLEDNAME machinery. The original does not use R10
 * or R11 anywhere in this region, so they have no locals here.
 * ===================================================================== */
void basicvfp_hand_MODULEMAIN(struct ros_cpu *s)
{
    uint32_t r0, r1 = s->r[1], r2, r3 = s->r[3], r4 = s->r[4],
             r5 = s->r[5], r6, r7, r8 = s->r[8], r9 = s->r[9],
             r12 = s->r[12], r13 = s->r[13], r14 = s->r[14];
    uint32_t v2, v4, v5;

    s->irq_off = 0;                            /* WritePSRc USR_mode,R1 */
    ros_msr_c(s, (ros_cpsr(s) & ~0x1Fu) | 0x10u, 0xFC100138u);
    ros_native_swi(s, ros_thunk_OS_GetEnv);    /* SWI OS_GetEnv */
    if (s->v) ros_swi_raise(s);
    r1 = s->r[1];
    r2 = r1;                                    /* MOV R2,R1 */
    r0 = 3;                                     /* MOV R0,#UserMode+AppSpace */
    r1 = 32;                                    /* MOV R1,#32 */
    s->r[0] = r0; s->r[1] = r1;                 /* SWI XVFPSupport_CheckContext */
    ros_swi(s, 0x78EC0u);
    r0 = s->r[0];
    if (s->v) {                                 /* MOVVS/SWIVS: */
        r0 = 3;                                 /* 32 registers not available,
                                                 * try 16 */
        r1 = 16;
        s->r[0] = r0; s->r[1] = r1;             /* SWIVS VFPSupport_CheckContext */
        ros_swi(s, 0x58EC0u);
        r0 = s->r[0];
    }
    r6 = r0;                                    /* MOV R6,R0: the context size */
    r7 = r1;                                    /* MOV R7,R1: the old context */
    r1 = ros_subs(s, r2 - r0 - VARS, VFPCONTEXT + 256u); /* SUBS: room? */
    if ((int32_t)r1 >= 0) {                     /* BPL MAIN */
        r8 = VARS;                              /* MOV ARGP,#VARS */
        /* Create the SWI 0 / MOV PC,R14 pair in the SWICODE data area
         * and synchronise the code cache over it (Basic.s:246-254). */
        r1 = 0x86D4u;                           /* ADD R1,ARGP,#SWICODE */
        ros_st32(0x86D4u, 0xEF000000u);         /* STMIA R1,{R0,R2}: SWI 0 */
        ros_st32(0x86D8u, 0xE1A0F00Eu);         /* ... then MOV PC,R14 */
        r2 = 0x86DCu;                           /* ADD R2,R1,#8 */
        r0 = 1;                                 /* MOV R0,#1 */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; /* SWI XOS_SynchroniseCodeAreas */
        ros_swi(s, 0x2006Eu);
        s->fp->fpsr = 0x70000u;                 /* MOV R0,#&70000 ; WFS R0 */
        /* The VFP/NEON assembler data tables (Basic.s:259-273). */
        r0 = 79;                                /* MOV R0,#&4F */
        r1 = BASICVFP_VFPDataResourceFile;      /* ADR R1,VFPDataResourceFile */
        s->r[0] = r0; s->r[1] = r1;             /* SWI OS_Find (no X: the
                                                 * file should exist!) */
        ros_native_swi(s, ros_thunk_OS_Find);
        if (s->v) ros_swi_raise(s);
        r0 = s->r[0];
        r3 = r0;                                /* MOV R3,R0 */
        r1 = r0;                                /* MOV R1,R0 */
        r0 = 21;                                /* MOV R0,#21 */
        s->r[0] = r0; s->r[1] = r1; s->r[3] = r3; s->r[6] = r6;
        s->r[7] = r7; s->r[8] = r8;             /* SWI OS_FSControl */
        ros_native_swi(s, ros_thunk_OS_FSControl);
        if (s->v) ros_swi_raise(s);
        r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
        r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
        ros_st32(r8 - 24, r1);                  /* STR R1,[ARGP,#VFPTABLES] */
        r0 = 0;                                 /* MOV R0,#0 */
        r1 = r3;                                /* MOV R1,R3 */
        s->r[0] = r0; s->r[1] = r1;             /* SWI OS_Find: close */
        ros_native_swi(s, ros_thunk_OS_Find);
        if (s->v) ros_swi_raise(s);
        /* Set up the VFP context and read the features (Basic.s:274-294). */
        r0 = 0x80000003u;                       /* UserMode+AppSpace+Activate */
        r1 = r7;                                /* MOV R1,R7 */
        r2 = r8 + VFPCONTEXT;                   /* ADD R2,ARGP,#VFPCONTEXT */
        r3 = 0;                                 /* MOV R3,#0 */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        ros_swi(s, 0x58EC1u);                   /* SWI VFPSupport_CreateContext */
        r0 = VFPSupport_Features_Misc;          /* MOV R0,#Features_Misc */
        s->r[0] = r0;
        ros_swi(s, 0x78EC8u);                   /* SWI XVFPSupport_Features */
        r0 = s->r[0]; r2 = s->r[2];
        r7 = r0 & VFPFLAG_Vectors;              /* AND R7,R0,#VFPFLAG_Vectors */
        r0 = VFPSupport_Features_SystemRegs;    /* MOV R0,#Features_SystemRegs */
        s->r[0] = r0;
        ros_swi(s, 0x58EC8u);                   /* SWI VFPSupport_Features (no
                                                 * X: around forever) */
        r2 = s->r[2];
        ros_logic(s, r2 & 0xF000u, 0);          /* TST R2,#&F000 */
        if (!s->z) r7 |= VFPFLAG_NEON;          /* ORRNE R7,R7,#VFPFLAG_NEON */
        ros_st8(r8 - 4, r7);                    /* STRB R7,[ARGP,#VFPFLAGS] */
        r0 = 0;                                 /* MOV R0,#0 */
        s->r[0] = r0;
        ros_swi(s, 0x58ECAu);                   /* SWI VFPSupport_ElementaryFunctions */
        r2 = s->r[2];
        ros_st32(r8 - 8, r2);                   /* STR R2,[ARGP,#VFPTRANSCENDENTALS] */
        /* The four environment handlers. The old ones are saved in turn
         * into OLDERR (Basic.s:296-319). */
        ros_st32(r8 - 16, BASICVFP_MSGATLINE);  /* ADRL R0,MSGATLINE ; STR */
        r9 = r8 - 80;                           /* ADD R9,ARGP,#OLDERR */
        r0 = 6;                                 /* MOV R0,#6: the error handler */
        r1 = BASICVFP_OSERRR;                   /* ADR R1,OSERRR */
        r2 = 0;                                 /* MOV R2,#0 */
        r3 = r8 - 1536;                         /* ADD R3,ARGP,#STRACC */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
        ros_native_swi(s, ros_thunk_OS_ChangeEnvironment); /* SWI XOS_ChangeEnvironment */
        r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        ros_st32(r9, r1);                       /* STMIA R9!,{R1,R2,R3} */
        ros_st32(r9 + 4, r2);
        ros_st32(r9 + 8, r3);
        r9 += 12;
        r0 = 9;                                 /* MOV R0,#9: the escape handler */
        r1 = BASICVFP_OSESCR;                   /* ADR R1,OSESCR */
        r2 = 0;                                 /* MOV R2,#0 */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
        ros_native_swi(s, ros_thunk_OS_ChangeEnvironment);
        r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        ros_st32(r9, r1);                       /* STMIA R9!,{R1,R2} */
        ros_st32(r9 + 4, r2);
        r9 += 8;
        r0 = 11;                                /* MOV R0,#11: the exit handler */
        r1 = BASICVFP_OSQUITR;                  /* ADR R1,OSQUITR */
        r2 = r8 - 80;                           /* ADD R2,ARGP,#OLDERR */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
        ros_native_swi(s, ros_thunk_OS_ChangeEnvironment);
        r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        ros_st32(r9, r1);                       /* STMIA R9!,{R1,R2} */
        ros_st32(r9 + 4, r2);
        r9 += 8;
        r0 = 16;                                /* MOV R0,#16: the upcall handler */
        r1 = BASICVFP_OSUPCR;                   /* ADR R1,OSUPCR */
        r2 = r8 - 80;                           /* ADD R2,ARGP,#OLDERR */
        s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
        ros_native_swi(s, ros_thunk_OS_ChangeEnvironment);
        r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
        ros_st32(r9, r1);                       /* STMIA R9!,{R1,R2} */
        ros_st32(r9 + 4, r2);
        r9 += 8;
        /* FREEPTR = PAGE = VFPCONTEXT + the context's size
         * (Basic.s:320-327). */
        r0 = r8 + VFPCONTEXT + r6;              /* ADD R0,ARGP,#VFPCONTEXT ;
                                                 * ADD R0,R0,R6 */
        ros_st32(r8 - 12, r0);                  /* STR R0,[ARGP,#FREEPTR] */
        ros_st32(r8 - 148, r0);                 /* STR R0,[ARGP,#PAGE] */
        ros_native_swi(s, ros_thunk_OS_GetEnv); /* SWI OS_GetEnv */
        if (s->v) ros_swi_raise(s);
        r1 = s->r[1];
        r13 = r1;                               /* MOV SP,R1: the himem limit */
        ros_st32(r8 - 132, r1);                 /* STR SP,[ARGP,#HIMEM] */
        ros_st32(r8 - 84, r1);                  /* STR SP,[ARGP,#MEMLIMIT] */
        r0 = 0;                                 /* MOV R0,#0 */
        ros_st32(r8 - 116, 0);                  /* STR R0,[ARGP,#ERRLIN] */
        ros_st32(r8 - 128, 0);                  /* STR R0,[ARGP,#ERRNUM] */
        ros_st32(r8 - 112, 0);                  /* STR R0,[ARGP,#ESCWORD] */
        ros_st32(r8 - 100, 0);                  /* STR R0,[ARGP,#LOCALARLIST] */
        ros_st32(r8 - 96, 0);                   /* STR R0,[ARGP,#INSTALLLIST] */
        ros_st32(r8 - 104, 0);                  /* STR R0,[ARGP,#TRACEFILE] */
        ros_st32(r8 - 264, 0);                  /* STR R0,[ARGP,#TALLY] */
        ros_st32(r8 - 36, 0);                   /* STR R0,[ARGP,#DIMLOCAL] */
        ros_st8(r8 - 27, 0);                    /* STRB R0,[ARGP,#LISTOP] */
        ros_st8(r8 - 25, 0);                    /* STRB R0,[ARGP,#MEMM] */
        ros_st32(r8 - 260, 0xFFFFFFFFu);        /* MVN R0,#0 ; STR [WIDTHLOC] */
        ros_st8(r8 - 26, 0xFFu);                /* STRB MVN0,[ARGP,#BYTESM] */
        ros_st32(r8 - 0x100u, 2314);            /* MOV R0,#10 ; ORR &900 ; @% */
        r0 = ros_ld32(r8 - 32) | (ros_ld8(r8 - 28) << 31); /* ORRS with the
                                                 * SEED's fifth byte's bit */
        if (r0 == 0)                            /* LDREQ R0,MYNAME ; STREQ */
            ros_st32(r8 - 32, 0x21575241u);     /* "ARW!" */
        r0 = BASICVFP_REPSTR;                   /* ADR R0,REPSTR */
        r2 = r8 - 1792;                         /* ADD R2,ARGP,#ERRORS */
        do {
            r1 = ros_ld8(r0);                   /* ENTRYL: LDRB R1,[R0],#1 */
            r0 += 1;
            ros_st8(r2, r1);                    /* STRB R1,[R2],#1 */
            r2 += 1;
        } while (r1 != 0);                      /* TEQ R1,#0 ; BNE ENTRYL */
        basicvfp_FROMAT(s, &r0, &r1, r8, 0xFFFFFFF0u); /* BL FROMAT */
        s->r[1] = r1; s->r[2] = r2;             /* BL SETFSA */
        s->r[8] = r8;
        s->r[14] = 0xFFFFFFF0u;
        basicvfp_hand_SETFSA(s);
        r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r6 = s->r[6]; r14 = s->r[14];
        basicvfp_hand_ORDERR(s, &r0, r8, 0xFFFFFFF0u); /* BL ORDERR */
        r12 = r8 - 1536;                        /* ADD LINE,ARGP,#STRACC */
        r13 = ros_ld32(r8 - 132);               /* LDR SP,[ARGP,#HIMEM] */
        r0 = 0;                                 /* MOV R0,#0 */
        /* to stop pops getting carried away (Basic.s:366-368) */
        r13 -= 40;                              /* STMFD SP!,{R0-R9} */
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
        ros_st32(r8 - 120, r13);                /* STR SP,[ARGP,#ERRSTK] */
        /* see if there's a name waiting to be read in (Basic.s:370) */
        s->r[0] = r0; s->r[12] = r12; s->r[13] = r13; /* SWI OS_GetEnv */
        ros_native_swi(s, ros_thunk_OS_GetEnv);
        if (s->v) ros_swi_raise(s);
        r0 = s->r[0]; r1 = s->r[1];
        r3 = r8 - 3;                            /* ADD R3,ARGP,#CALLEDNAME */
        for (;;) {                              /* ENTRE1 (Basic.s:372-377) */
            r2 = ros_ld8(r0);                   /* LDRB R2,[R0],#1 */
            r0 += 1;
            ros_st8(r3, r2);                    /* STRB R2,[R3],#1 */
            r3 += 1;
            ros_logic(s, r2, s->c);             /* TEQ R2,#0 */
            if (r2 == 0) {                      /* BEQ CLRSTKTITLE */
                s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[7] = r7;
                s->r[9] = r9;                   /* BL TITLE */
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_hand_TITLE(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
                r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
                r13 = s->r[13]; r14 = s->r[14];
                { MODMAIN_TAIL_CALL(basicvfp_CLRSTK) }
            }
            ros_subs(s, r2, 32);                /* CMP R2,#" " */
            if (r2 > 32) continue;              /* BHI ENTRE1 */
            r2 = 0;                             /* MOV R2,#0 */
            ros_st8(r3 - 1, 0);                 /* STRB R2,[R3,#-1] */
            do {
                r2 = ros_ld8(r0);               /* ENTRE2: LDRB R2,[R0],#1 */
                r0 += 1;
                ros_subs(s, r2, 32);            /* CMP R2,#" " */
            } while (r2 == 32);                 /* BEQ ENTRE2 */
            r9 = 2;                             /* MOV R9,#2 ; set to chain */
            ros_subs(s, r2, 45);                /* CMP R2,#"-" */
            if (r2 == 45) {                     /* BEQ ENTRYKEYW */
                /* ENTRYKEYW (Basic.s:476-483): the keyword head. */
                s->r[0] = r0; s->r[2] = r2;     /* BL RDCOMCH */
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_RDCOMCH(s);
                r0 = s->r[0]; r2 = s->r[2]; r14 = s->r[14];
                ros_subs(s, r2, 72);            /* CMP R2,#"H" */
                if (r2 == 72) goto ENTRYKEYW2;  /* BEQ ENTRYKEYW2 (-help) */
                ros_subs(s, r2, 76);            /* CMP R2,#"L" */
                if (r2 == 76) goto ENTRYKEYW3;  /* BEQ ENTRYKEYW3 (-load) */
                ros_subs(s, r2, 81);            /* CMP R2,#"Q" */
                if (r2 == 81) goto ENTRYKEYW4;  /* BEQ ENTRYKEYW4 (-quit) */
                /* -chain: C,H,A,I,N. Each CMP is checked by the
                 * RDCOMCHER that follows it (Basic.s:484-494). */
                ros_subs(s, r2, 67);            /* CMP R2,#"C" */
                s->r[3] = r3; s->r[7] = r7; s->r[9] = r9; /* BL RDCOMCHER */
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_RDCOMCHER(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
                r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
                r13 = s->r[13]; r14 = s->r[14];
                ros_subs(s, r2, 72);            /* CMP R2,#"H" */
                s->r[14] = 0xFFFFFFF0u;         /* BL RDCOMCHER */
                basicvfp_RDCOMCHER(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
                r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
                r13 = s->r[13]; r14 = s->r[14];
                ros_subs(s, r2, 65);            /* CMP R2,#"A" */
                s->r[14] = 0xFFFFFFF0u;         /* BL RDCOMCHER */
                basicvfp_RDCOMCHER(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
                r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
                r13 = s->r[13]; r14 = s->r[14];
                ros_subs(s, r2, 73);            /* CMP R2,#"I" */
                s->r[14] = 0xFFFFFFF0u;         /* BL RDCOMCHER */
                basicvfp_RDCOMCHER(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
                r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
                r13 = s->r[13]; r14 = s->r[14];
                ros_subs(s, r2, 78);            /* CMP R2,#"N" */
                s->r[14] = 0xFFFFFFF0u;         /* BL RDCOMCHER */
                basicvfp_RDCOMCHER(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
                r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
                r13 = s->r[13]; r14 = s->r[14];
                ros_subs(s, r2, 32);            /* CMP R2,#" " */
                if (r2 != 32) { MODMAIN_TAIL_CALL(basicvfp_ENTRYUNK) } /* BNE */
                /* fall through to ENTRYCHAIN with r9 = 2 */
            } else {
                /* no '-': a plain filename, or @hex,hex, or nothing
                 * (Basic.s:386-391). */
                s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[7] = r7;
                s->r[9] = r9;                   /* BL TITLE */
                s->r[14] = 0xFFFFFFF0u;
                basicvfp_hand_TITLE(s);
                r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
                r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
                r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
                r13 = s->r[13]; r14 = s->r[14];
                ros_logic(s, r2, s->c);         /* TEQ R2,#0 */
                if (r2 == 0) { MODMAIN_TAIL_CALL(basicvfp_CLRSTK) } /* BEQ */
                v2 = r2 ^ 0x40u;                /* TEQ R2,#"@" */
                if (v2 == 0) {                  /* BNE ENTRYF */
                    r9 = 0;                     /* MOV R9,#0 ; no chain */
                    goto ENTRYCONT;
                }
                goto ENTRYF;                    /* BNE ENTRYF */
            }
        ENTRYCHAIN:                             /* Basic.s:496-497: the banner
                                                 * before the name. -chain
                                                 * falls through to here and
                                                 * -load branches here. -quit
                                                 * jumped past it. */
            s->r[9] = r9;                       /* BL TITLE */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_TITLE(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            do {
            ENTRYCHAIN1:                        /* Basic.s:498-501 */
                v4 = r2;                        /* CMP R2,#" " */
                ros_subs(s, r2, 32);
                if (v4 == 32) {                 /* LDREQB R2,[R0],#1. The
                                                 * writeback happens only on
                                                 * EQ. */
                    r2 = ros_ld8(r0);
                    r0 += 1;
                }
            } while (v4 == 32);                 /* BEQ ENTRYCHAIN1 */
            ros_logic(s, r2, s->c);             /* TEQ R2,#0 */
            if (r2 == 0) {                      /* BEQ CLRSTK */
                s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
                { MODMAIN_TAIL_CALL(basicvfp_CLRSTK) }
            }
            v5 = r2 ^ 0x40u;                    /* TEQ R2,#"@" */
            if (v5 == 0) goto ENTRYCONT;        /* BEQ ENTRYCONT */
            goto ENTRYF;                        /* and BNE ENTRYF */

        ENTRYKEYW2:                             /* -help (Basic.s:541-561) */
            s->r[0] = r0; s->r[2] = r2;         /* BL RDCOMCH */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDCOMCH(s);
            r0 = s->r[0]; r2 = s->r[2]; r14 = s->r[14];
            ros_subs(s, r2, 69);                /* CMP R2,#"E" */
            s->r[3] = r3; s->r[7] = r7; s->r[9] = r9; /* BL RDCOMCHER */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 76);                /* CMP R2,#"L" */
            s->r[14] = 0xFFFFFFF0u;             /* BL RDCOMCHER */
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 80);                /* CMP R2,#"P" */
            s->r[14] = 0xFFFFFFF0u;             /* BL RDCOMCHER */
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            s->r[0] = r0; s->r[2] = r2; s->r[3] = r3; s->r[7] = r7;
            s->r[9] = r9;                       /* BL TITLE */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_hand_TITLE(s);
            r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3]; r4 = s->r[4];
            r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7]; r8 = s->r[8];
            r9 = s->r[9]; r12 = s->r[12]; r13 = s->r[13];
            r0 = 27;                            /* MOV R0,#27: the -help text */
            s->r[0] = r0;                       /* BL MSGPRNXXX */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_MSGPRNXXX(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r13 = s->r[13]; r14 = s->r[14];
            { MODMAIN_TAIL_CALL(basicvfp_ENTRYHELP) } /* B ENTRYHELP */

        ENTRYKEYW3:                             /* -load (Basic.s:529-540) */
            s->r[0] = r0; s->r[2] = r2;         /* BL RDCOMCH */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDCOMCH(s);
            r0 = s->r[0]; r2 = s->r[2]; r14 = s->r[14];
            ros_subs(s, r2, 79);                /* CMP R2,#"O" */
            s->r[3] = r3; s->r[7] = r7; s->r[9] = r9; /* BL RDCOMCHER */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 65);                /* CMP R2,#"A" */
            s->r[14] = 0xFFFFFFF0u;             /* BL RDCOMCHER */
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 68);                /* CMP R2,#"D" */
            s->r[14] = 0xFFFFFFF0u;             /* BL RDCOMCHER */
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 32);                /* CMP R2,#" " */
            if (r2 != 32) { MODMAIN_TAIL_CALL(basicvfp_ENTRYUNK) } /* BNE */
            r9 = 0;                             /* MOV R9,#0 */
            goto ENTRYCHAIN;                    /* B ENTRYCHAIN */

        ENTRYKEYW4:                             /* -quit (Basic.s:516-528) */
            s->r[0] = r0; s->r[2] = r2;         /* BL RDCOMCH */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDCOMCH(s);
            r0 = s->r[0]; r2 = s->r[2]; r14 = s->r[14];
            ros_subs(s, r2, 85);                /* CMP R2,#"U" */
            s->r[3] = r3; s->r[7] = r7; s->r[9] = r9; /* BL RDCOMCHER */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 73);                /* CMP R2,#"I" */
            s->r[14] = 0xFFFFFFF0u;             /* BL RDCOMCHER */
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 84);                /* CMP R2,#"T" */
            s->r[14] = 0xFFFFFFF0u;             /* BL RDCOMCHER */
            basicvfp_RDCOMCHER(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12];
            r13 = s->r[13]; r14 = s->r[14];
            ros_subs(s, r2, 32);                /* CMP R2,#" " */
            if (r2 != 32) { MODMAIN_TAIL_CALL(basicvfp_ENTRYUNK) } /* BNE */
            r1 = 0;                             /* MOV R1,#0 ; set QUIT flag */
            ros_st8(r8 - 3, 0);                 /* STRB R1,[ARGP,#CALLEDNAME] */
            goto ENTRYCHAIN1;                   /* B ENTRYCHAIN1: skips TITLE */

        ENTRYF:                                 /* Basic.s:506-515: the
                                                 * filename into STRACC, CR,
                                                 * and the internals of
                                                 * TEXTLOAD. */
            r4 = r8 - 1536;                     /* ADD R4,ARGP,#STRACC */
            do {
                ros_st8(r4, r2);                /* ENTRF1: STRB R2,[R4],#1 */
                r4 += 1;
                r2 = ros_ld8(r0);               /* LDRB R2,[R0],#1 */
                r0 += 1;
                ros_subs(s, r2, 32);            /* CMP R2,#" " */
            } while (r2 > 32);                  /* BHI ENTRF1 */
            r5 = 13;                            /* MOV R5,#13 */
            ros_st8(r4, 13);                    /* STRB R5,[R4],#1 */
            r4 += 1;
            s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[4] = r4;
            s->r[5] = r5;                       /* BL LOADFILEFINAL */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_LOADFILEFINAL(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12]; r13 = s->r[13];
            r14 = s->r[14];
            goto ENTRYFINAL;                    /* B ENTRYFINAL */

        ENTRYCONT:                              /* Basic.s:392-404: the
                                                 * in-core @hex,hex form. */
            s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[9] = r9; /* RDHEX */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDHEX(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r7 = s->r[7]; r8 = s->r[8];
            r9 = s->r[9]; r12 = s->r[12]; r13 = s->r[13]; r14 = s->r[14];
            r6 = r5;                            /* MOV R6,R5 */
            ros_logic(s, r2 ^ 0x2Cu, s->c);     /* TEQ R2,#"," */
            if (!s->z) {                        /* BNE BADIPHEX */
                s->r[6] = r6;
                { MODMAIN_TAIL_CALL(basicvfp_BADIPHEX) }
            }
            s->r[6] = r6;                       /* BL RDHEX */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_RDHEX(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12]; r13 = s->r[13];
            r14 = s->r[14];
            ros_logic(s, r2, s->c);             /* TEQ R2,#0 */
            if (r2 != 0) { MODMAIN_TAIL_CALL(basicvfp_BADIPHEX) } /* BNE */
            ros_subs(s, r5, r6);                /* CMP R5,R6 */
            if (r5 <= r6) { MODMAIN_TAIL_CALL(basicvfp_BADIPHEX) } /* BLS */
            r1 = r6;                            /* MOV R1,R6 */
            r7 = r6;                            /* MOV R7,R6 */
            s->r[1] = r1; s->r[7] = r7;         /* BL LOADFILEINCORE */
            s->r[14] = 0xFFFFFFF0u;
            basicvfp_LOADFILEINCORE(s);
            r0 = s->r[0]; r1 = s->r[1]; r2 = s->r[2]; r3 = s->r[3];
            r4 = s->r[4]; r5 = s->r[5]; r6 = s->r[6]; r7 = s->r[7];
            r8 = s->r[8]; r9 = s->r[9]; r12 = s->r[12]; r13 = s->r[13];
            r14 = s->r[14];
            /* fall into ENTRYFINAL */

        ENTRYFINAL:                             /* Basic.s:405-419 */
            v2 = r9 & 2;                        /* TST R9,#2 */
            if (v2 == 0) { MODMAIN_TAIL_CALL(basicvfp_FSASET) } /* BEQ FSASET */
            r0 = ros_ld8(r8 - 3);               /* LDRB R0,[ARGP,#CALLEDNAME] */
            ros_logic(s, r0, s->c);             /* TEQ R0,#0 */
            if (r0 != 0) {                      /* BNE RUNNER: not QUIT, so
                                                 * just run */
                s->r[0] = r0;
                { MODMAIN_TAIL_CALL(basicvfp_RUNNER) }
            }
            basicvfp_CRUNCHCHK(s, r0, r1, r2, r3, r4, r13, 0xFFFFFFF0u); /* BL */
            if (s->z) {                         /* BEQ RUNNER */
                s->r[0] = r0; s->r[14] = r14;
                { MODMAIN_TAIL_CALL(basicvfp_RUNNER) }
            }
            r0 = SAFECRUNCH;                    /* MOV R0,#SAFECRUNCH */
            r1 = ros_ld32(r8 - 148);            /* LDR R1,[ARGP,#PAGE] */
            basicvfp_CRUNCHROUTINE(s, r0, &r1, &r2, &r3, &r4, &r5, &r6,
                                   &r7, &s->r[10], 0xFFFFFFF0u); /* BL */
            ros_st32(r8 - 144, r2);             /* STR R2,[ARGP,#TOP] */
            s->r[0] = r0; s->r[1] = r1; s->r[2] = r2; s->r[3] = r3;
            s->r[4] = r4; s->r[5] = r5; s->r[6] = r6; s->r[7] = r7;
            s->r[14] = r14;
            { MODMAIN_TAIL_CALL(basicvfp_RUNNER) } /* B RUNNER */
        }
    }
    /* The NoStore error (Basic.s:44-49): no room for the VFP context.
     * None of these SWIs returns. The fault catches the case that
     * cannot happen. */
    r0 = BASICVFP_SEVEREERROR;                  /* ADR R0,SEVEREERROR */
    r1 = 0;                                     /* MOV R1,#0 ; global messages */
    r2 = 0;                                     /* MOV R2,#0 ; internal buffer */
    s->r[0] = r0; s->r[1] = r1; s->r[2] = r2;
    s->r[6] = r6; s->r[7] = r7;
    ros_swi(s, 0x61506u);                       /* SWI XMessageTrans_ErrorLookup */
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_GenerateError); /* SWI OS_GenerateError */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_native_swi(s, ros_thunk_OS_Exit);       /* SWI OS_Exit */
    if (s->v) ros_swi_raise(s);
    r0 = s->r[0];
    ros_fault(s, 0xFC10018Cu, "a transfer to an address that is not code");
}
