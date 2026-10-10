/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_basicvfp.c: BASICVFP, the ROM's first real compiled module,
 * interpreting BASIC.  A program is written to a HostFS disc of the test's
 * own, run by *BASIC -quit through OS_CLI, and its output is read back.
 * The program uses integer and floating expressions, VAL and STR$.  VAL and
 * STR$ use the packed decimal pair ros_fpa_ldp and ros_fpa_stp, and this is
 * their first run through the whole interpreter: tokeniser, parser,
 * evaluator and filing system.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/platform.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

static char root[512];
static uint32_t line, outbuf;

/* The test disc taken off the host share when done, as the files test
 * takes its own: a run left one rosgd-basic-XXXXXX each time (#43) */
static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            remove_tree(p);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static uint32_t cli(const char *cmd)
{
    strcpy((char *)ros_ptr(line), cmd);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line;
    ros_swi(&s, XOS_CLI);
    if (s.v)
        return s.r[0];
    return 0;
}

/* Both encoders, and x86-64 the default: an x86-64 build (asmlib) */
#if BASICASM_HAVE_X64 && BASICASM_HAVE_A64 && defined(__x86_64__)
#define SELFTEST_ASM 1
#else
#define SELFTEST_ASM 0
#endif

static const char *prog =
    "10 A% = 6 * 7\n"
    "20 B = 2.5 + 2\n"
    "30 V = VAL(\"3.75\")\n"
    "40 S$ = STR$(B) + \" \" + STR$(V)\n"
    "50 PRINT \"Hello from BASIC on ROSGD\"\n"
    "60 F% = OPENOUT(\"HostFS::BASICDisc.$.Out\")\n"
    "70 BPUT#F%, A%\n"
    "80 FOR I% = 1 TO LEN(S$)\n"
    "90 BPUT#F%, ASC(MID$(S$, I%, 1))\n"
    "100 NEXT I%\n"
#if SELFTEST_ASM
    /* '[' assembles x86-64 by default in an x86-64 build (asmlib), and
     * AArch64 after *BasicAsmCPU A64.  The code is word-aligned first, as ARM code is */
    "102 DIM CODE% 63\n"
    "103 P% = CODE%\n"
    "104 [ OPT 0\n"
    "105 mov al, 61\n"
    "106 add al, 1\n"
    "107 ret\n"
    "108 ]\n"
    "109 *BasicAsmCPU A64\n"
    "110 [ OPT 0 : movz w0, #61 : add w0, w0, #1 : ret : ]\n"
    "111 *BasicAsmCPU X64\n"
    "112 FOR I% = 0 TO P%-CODE%-1 : BPUT#F%, CODE%?I% : NEXT I%\n"
#endif
    "120 CLOSE#F%\n"
    "130 QUIT\n";

void ros_selftest_basicvfp(void)
{
    ros_console_printf("rosgd: self-test -- BASICVFP, the interpreter interpreted\n");

    /* A disc of the test's own, as the files test has: on the host share
     * in the box, TMPDIR hosted. */
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-basic-XXXXXX", tmp);
    mkdir(tmp, 0777);
    if (!mkdtemp(root)) {
        check(0, "BASIC -- no directory for the test disc", "no directory for the test disc");
        return;
    }
    ros_hostfs_mount("BASICDisc", root);

    line = ros_addr(ros_rma_alloc(1024));
    outbuf = ros_addr(ros_rma_alloc(1024));

    char p[96];
    snprintf(p, sizeof p, "%s/Prog", root);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(prog, f);
        fclose(f);
    }
    /* An Obey file gives the language its room: *WimpSlot sizes the slot
     * the task has not got, then the interpreter runs in it. */
    snprintf(p, sizeof p, "%s/Run,feb", root);
    f = fopen(p, "w");
    if (f) {
        fputs("| Room for a BASIC program, then the interpreter\n"
              "WimpSlot 16M\n"
              "BASICVFP -quit HostFS::BASICDisc.$.Prog\n", f);
        fclose(f);
    }
    /* OPENOUT makes a Data file: HostFS keeps its type as ",ffd", as RISC
     * OS's HostFS does */
    snprintf(p, sizeof p, "%s/Out,ffd", root);
    unlink(p);

    /* *BASIC -quit: load, run, leave.  The whole interpreter goes through
     * this one command. */
    uint32_t e = cli("HostFS::BASICDisc.$.Run");
    check(e == 0, "BASIC: the Obey file ran *BASIC -quit", "*Obey: &%X %s", e,
          e ? (char *)ros_ptr(e + 4) : "");

    f = fopen(p, "rb");
    uint8_t got[64] = { 0 };
    size_t n = f ? fread(got, 1, sizeof got - 1, f) : 0;
    if (f)
        fclose(f);
    check(n > 0, "BASIC: the program wrote its output file", "output: %zu bytes", n);
    check(n > 0 && got[0] == 42, "BASIC: 6*7 is 42, by BPUT#", "byte 0: %u", got[0]);
    check(n > 1 && !strncmp((char *)got + 1, "4.5 3.75", 8), "BASIC: 2.5+2 and VAL(\"3.75\") by STR$",
          "floats: \"%s\"", n > 1 ? (char *)got + 1 : "");
#if SELFTEST_ASM
    /* mov al,61 / add al,1 / ret, then ALIGN's zeros and movz w0,#61 /
     * add w0,w0,#1 / ret: as LLVM MC encodes them */
    static const uint8_t mc[20] = { 0xB0, 0x3D, 0x04, 0x01, 0xC3, 0, 0, 0,
                                    0xA0, 0x07, 0x80, 0x52, 0x00, 0x04, 0x00, 0x11,
                                    0xC0, 0x03, 0x5F, 0xD6 };
    check(n == 29 && !memcmp(got + 9, mc, 5), "BASIC: the inline assembler assembled x86-64",
          "bytes: %02X %02X %02X %02X %02X", got[9], got[10], got[11], got[12], got[13]);
    check(n == 29 && !memcmp(got + 14, mc + 5, 15), "BASIC: *BasicAsmCPU A64 assembled AArch64",
          "bytes: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X", got[17], got[16], got[15],
          got[14], got[21], got[20], got[19], got[18], got[25], got[24], got[23], got[22]);
#endif

    /* ARM recognised from a block's start (#178): AWViewer's callback,
     * cut down.  Its first instruction is good x86-64 too (R11, and '#'
     * the encoder allows), and was assembled so in the first pass, seven
     * bytes, until the next statement turned the task to ARM: then every
     * label after it was a word on in that pass, and the second pass's
     * forward branch went a word past its label.  B f must be &EAFFFFFF
     * (f the next word); it was &EA000000.  *BasicAsmCPU with no CPU puts
     * recognition back, which the program above turned off. */
    snprintf(p, sizeof p, "%s/ArmProg", root);
    f = fopen(p, "w");
    if (f) {
        fputs("10 *BasicAsmCPU\n"
              "20 DIM C% 63\n"
              "30 FOR pass=0 TO 2 STEP 2\n"
              "40 P%=C%\n"
              "50 [OPT pass\n"
              "60 .cb cmp r11,#(e-s)DIV4\n"
              "70 addcc pc,pc,r11,lsl #2\n"
              "80 mov pc,r14\n"
              "90 .s b f\n"
              "100 .e\n"
              "110 .f mov r0,#1\n"
              "120 ]\n"
              "130 NEXT\n"
              "140 F%=OPENOUT \"HostFS::BASICDisc.$.ArmOut\"\n"
              "150 FOR I%=0 TO 19 : BPUT#F%,C%?I% : NEXT\n"
              "160 CLOSE#F%\n"
              "170 QUIT\n", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/RunArm,feb", root);
    f = fopen(p, "w");
    if (f) {
        fputs("WimpSlot 16M\nBASICVFP -quit HostFS::BASICDisc.$.ArmProg\n", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/ArmOut,ffd", root);
    unlink(p);
    e = cli("HostFS::BASICDisc.$.RunArm");
    {
        static const uint32_t want[5] = { 0xE35B0001, 0x308FF10B, 0xE1A0F00E, 0xEAFFFFFF,
                                          0xE3A00001 };
        uint32_t w[5] = { 0 };
        FILE *g = fopen(p, "rb");
        size_t m = g ? fread(w, 4, 5, g) : 0;
        if (g)
            fclose(g);
        check(e == 0 && m == 5 && !memcmp(w, want, sizeof want),
              "BASIC: ARM recognised from the block's start -- cmp r11,#n first, its forward branch right (#178)",
              "*Obey &%X, %zu words: %08X %08X %08X %08X %08X", e, m, w[0], w[1], w[2], w[3], w[4]);
    }


    /* MODE in a program: VDU 22 through WrchV, the mode change's service
     * calls under it, the Wimp's Service_ModeChange among them.  The SWI
     * thunks once pointed the SVC stack at the application's own, where
     * the next vector down wrote over the Wimp's frame ("Return to
     * &FFFFFFF0").  The mode the test found is put back after. */
    uint32_t *keep = ros_rma_alloc(64);
    struct ros_cpu sm;
    ros_cpu_enter(&sm);
    sm.r[0] = 1;
    ros_swi(&sm, XOS_ScreenMode);
    uint32_t was = sm.r[1];
    if (was >= 256)
        memcpy(keep, ros_ptr(was), 64);
    snprintf(p, sizeof p, "%s/Mode", root);
    f = fopen(p, "w");
    if (f) {
        fputs("10 MODE 28\n"
              "20 F%=OPENOUT \"HostFS::BASICDisc.$.ModeOut\"\n"
              "30 BPUT#F%,77\n"
              "40 CLOSE#F%\n"
              "50 QUIT\n", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/RunMode,feb", root);
    f = fopen(p, "w");
    if (f) {
        fputs("BASICVFP -quit HostFS::BASICDisc.$.Mode\n", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/ModeOut,ffd", root);
    unlink(p);
    e = cli("HostFS::BASICDisc.$.RunMode");
    f = fopen(p, "rb");
    int byte = f ? fgetc(f) : -1;
    if (f)
        fclose(f);
    check(e == 0 && byte == 77, "BASIC: MODE 28 in a program -- VDU 22 through WrchV, the service "
          "calls under it, and on", "&%X %s, byte %d", e, e ? (char *)ros_ptr(e + 4) : "", byte);
    ros_cpu_enter(&sm);
    sm.r[0] = 0, sm.r[1] = was >= 256 ? ros_addr(keep) : was;
    ros_swi(&sm, XOS_ScreenMode);
    ros_rma_free(keep);

    /* An error in a SYS inside a PROC inside an FN, trapped by the PROC's
     * LOCAL ERROR handler, which ENDPROCs.  This is !Maestro's start (PROCcc
     * looks up MIDI_SoundEnable).  The handler runs on the register file
     * the runtime enters it with; the FN's return is a jump to EXPR's
     * resume point, which read the struct the SYS had left, R15 still the
     * SYS's return: "Return to &FC107CF4 where &FC10263C was expected"
     * (#1).  Twenty times over, the error base reused; then an FN that
     * traps its own; then a plain Syntax error in an FN with no handler,
     * which BASIC's default handler reports (CALL !ERRXLATE, #25). */
    snprintf(p, sizeof p, "%s/Errs", root);
    f = fopen(p, "w");
    if (f) {
        fputs("10 qb%=TRUE : n%=0\n"
              "20 FOR i%=1 TO 20 : x%=FNi(i%) : IF qb% THEN n%-=1000\n"
              "30 qb%=TRUE : n%+=x% : NEXT\n"
              "40 y$=FNsafe\n"
              "50 F%=OPENOUT \"HostFS::BASICDisc.$.ErrsOut\"\n"
              "60 BPUT#F%,STR$(n%)+\" \"+y$;\n"
              "70 CLOSE#F%\n"
              "80 PRINT FNsum(1,10)\n"
              "90 END\n"
              "100 DEF FNi(k%) : PROCcc : =k%*2\n"
              "110 DEF PROCcc : LOCAL M% : LOCAL ERROR : ON ERROR LOCAL qb%=FALSE : ENDPROC\n"
              "120 SYS \"OS_SWINumberFromString\",0,\"MIDI_SoundEnable\" TO M% : ENDPROC\n"
              "130 DEF FNsafe : LOCAL ERROR : ON ERROR LOCAL =\"caught \"+REPORT$\n"
              "140 SYS \"MIDI_SoundEnable\",0 : =\"ran\"\n"
              "150 DEF FNsum(A,B) : FOR I% = A TO B : NEXT = T%\n", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/RunErrs,feb", root);
    f = fopen(p, "w");
    if (f) {
        fputs("WimpSlot 16M\n"
              "Spool HostFS::BASICDisc.$.ErrsLog\n"
              "BASICVFP -quit HostFS::BASICDisc.$.Errs\n"
              "Spool\n", f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/ErrsLog,ffd", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/ErrsOut,ffd", root);
    unlink(p);
    e = cli("HostFS::BASICDisc.$.RunErrs");
    f = fopen(p, "rb");
    memset(got, 0, sizeof got);
    n = f ? fread(got, 1, sizeof got - 1, f) : 0;
    if (f)
        fclose(f);
    check(!strcmp((char *)got, "420 caught SWI name not known"),
          "BASIC: a SYS's error in a PROC in an FN, trapped by the PROC's LOCAL ERROR, which "
          "ENDPROCs; the FN returns (#1)", "got \"%s\"", (char *)got);
    /* BASIC -quit hands the error on to the program's handler, the
     * runtime's default here, which writes the kernel's report and ends
     * the program: once, and the Obey file goes on to close the spool */
    snprintf(p, sizeof p, "%s/ErrsLog,ffd", root);
    f = fopen(p, "rb");
    char log[512] = "";
    n = f ? fread(log, 1, sizeof log - 1, f) : 0;
    if (f)
        fclose(f);
    log[n] = 0;
    const char *report = "Error: Syntax error (Error number &10)";
    char *at = strstr(log, report);
    check(e == 0 && at && !strstr(at + 1, report),
          "BASIC: a Syntax error in an FN, no handler: BASIC's own handler reports it once, "
          "and the program ends (#25)", "&%X %s; spooled \"%s\"", e,
          e ? (char *)ros_ptr(e + 4) : "", log);

    ros_hostfs_unmount("BASICDisc");
    remove_tree(root);
    struct stat gone;
    check(stat(root, &gone) != 0, "BASIC: its test disc taken off the share when done",
          "%s still there", root);
}
