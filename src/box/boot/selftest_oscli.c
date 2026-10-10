/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_oscli.c: OS_CLI, OS_SubstituteArgs and the kernel's *commands,
 * against the kernel's own (Kernel/s/Oscli, Utility, MoreComms, SysComms).
 * They are used through the SWIs, with the output caught on WrchV.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/switrace.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define BYTEV 0x06u

static uint32_t line;               /* arena: the command */
static const os_error *last;        /* the last command's error */
static char out[4096];                  /* room for *Modules as the ROM grows */
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

static uint32_t fx[3];
static int bytev(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] != 250)
        return ROS_VECTOR_PASS;
    memcpy(fx, s->r, sizeof fx);
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI: the error number or 0, the output in out. */
static uint32_t cli(const char *cmd)
{
    strcpy(ros_ptr(line), cmd);
    outn = 0, out[0] = 0;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line;
    ros_swi(&s, XOS_CLI);
    last = s.v ? ros_ptr(s.r[0]) : NULL;
    return last ? last->errnum : 0;
}

static const char *last_error(void)
{
    return last ? last->errmess : "";
}

static int says(const char *cmd, const char *want)
{
    uint32_t e = cli(cmd);
    int ok = e == 0 && strcmp(out, want) == 0;
    if (!ok)
        ros_console_printf("        %s: &%X \"%s\"\n", cmd, e, out);
    return ok;
}

static int fails(const char *cmd, uint32_t errnum)
{
    uint32_t e = cli(cmd);
    if (e != errnum)
        ros_console_printf("        %s: &%X, want &%X\n", cmd, e, errnum);
    return e == errnum;
}

static int var_is(const char *name, const char *want)
{
    char *b = ros_rma_alloc(256);
    strcpy(b + 128, name);
    uint32_t r[5] = { ros_addr(b + 128), ros_addr(b), 127, 0, 0 };
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, sizeof r);
    ros_swi(&s, XOS_ReadVarVal);
    int ok = !s.v && s.r[2] == strlen(want) && memcmp(b, want, s.r[2]) == 0;
    ros_rma_free(b);
    return ok;
}

/* A module laid out as a compiled one: a command table in its image, its
 * code at native entries standing for compiled routines. */
static char seen_tail[128];
static uint32_t seen_argc, seen_r12;
static void test_cmd(struct ros_cpu *s)
{
    snprintf(seen_tail, sizeof seen_tail, "%s", (const char *)ros_ptr(s->r[0]));
    char *cr = strchr(seen_tail, 13);
    if (cr)
        *cr = 0;
    seen_argc = s->r[1];
    seen_r12 = s->r[12];
    s->v = 0;
    s->r[15] = s->r[14];
}

static struct ros_module test_module;

/* *NeverBack: a command that never comes back to the one that ran it, as
 * an application started in a Wimp task never does, nor one whose exit
 * handler goes back past it (ShellCLI's): out to never_back's frame, as
 * the runtime's own longjmps go (ros_resume_unwind first) */
static jmp_buf gone;
static const void *gone_frame;

static os_error *cmd_never_back(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    ros_resume_unwind(gone_frame);
    longjmp(gone, 1);
}

/* *ExitApp: an application, started as *Run starts one
 * (ros_module_run_as_application), that ends at once with OS_Exit, as a
 * BASIC program's END does (ResFind's, in an !Run) */
static void exit_app_run(void *arg)
{
    (void)arg;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.mode = ROS_MODE_USR;
    ros_swi(&c, OS_Exit);
}

static os_error *cmd_exit_app(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    ros_module_run_as_application(exit_app_run, NULL);
    return NULL;
}

/* An exit handler that ends the task, as the Wimp's does: out to
 * never_back's frame */
static void task_exit_handler(struct ros_cpu *s)
{
    (void)s;
    ros_resume_unwind(gone_frame);
    longjmp(gone, 1);
}

static const struct ros_command never_commands[] = {
    { "NeverBack", ROS_CMD_INFO(0, 255, 0, 0), "Syntax: *NeverBack", "", cmd_never_back },
    { "ExitApp", ROS_CMD_INFO(0, 255, 0, 0), "Syntax: *ExitApp", "", cmd_exit_app },
    { 0 },
};

static struct ros_module never_module = {
    .title = "NeverBackTest",
    .help = "",
    .commands = never_commands,
};

/* OS_CLI of cmd, which is to reach *NeverBack: 1 if it went and did not
 * come back; the SVC stack, the depth, the handlers and the SWI trace put
 * back as ShellCLI's command loop puts them */
static int never_back(const char *cmd)
{
    uint32_t svc = ros_svc_sp, depth = ros_call_depth;
    struct ros_swi_frame *trace = ros_switrace_top();
    struct ros_handler *handlers = ros_handler_chain(NULL);
    ros_handler_chain(handlers);
    strcpy(ros_ptr(line), cmd);
    if (setjmp(gone) == 0) {
        gone_frame = __builtin_frame_address(0);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = line;
        ros_swi(&s, XOS_CLI);
        ros_console_printf("        %s: came back\n", cmd);
        return 0;
    }
    ros_switrace_unwind(trace);
    ros_handler_chain(handlers);
    ros_call_depth = depth;
    ros_svc_sp = svc;
    return 1;
}

/* Alias$D1 to Alias$D<n>, each the next, the last *Set Test$Deep <n>: the
 * chain run, and Test$Deep set if its innermost line ran */
static uint32_t alias_chain(unsigned n, int *ran)
{
    char c[64];
    for (unsigned i = 1; i < n; i++) {
        snprintf(c, sizeof c, "Set Alias$D%u D%u", i, i + 1);
        cli(c);
    }
    snprintf(c, sizeof c, "Set Alias$D%u Set Test$Deep %u", n, n);
    cli(c);
    cli("Unset Test$Deep");
    uint32_t e = cli("D1");
    snprintf(c, sizeof c, "%u", n);
    *ran = var_is("Test$Deep", c);
    cli("Unset Alias$D*"), cli("Unset Test$Deep");
    return e;
}

static void add_test_module(void)
{
    uint8_t *image = ros_rma_alloc(256);
    memset(image, 0, 256);
    uint32_t base = ros_addr(image), code = ros_native_entry(test_cmd, "test:TestArgs");
    /* "TestArgs": 1-3 parameters, the second GSTransed */
    uint32_t t = 64;
    memcpy(image + t, "TestArgs", 9);
    uint32_t q = (t + 9 + 3) & ~3u;
    uint32_t words[4] = { code - base, ROS_CMD_INFO(1, 3, 2, 0), 0, 0 };
    memcpy(image + q, words, sizeof words);
    image[q + 16] = 0;
    memcpy(image + 32, "Syntax: *TestArgs <a> <b>", 26);
    memcpy(image + 100, "TestCmds", 9);         /* the header's title */
    ((uint32_t *)image)[4] = 100;
    ((uint32_t *)image)[5] = 109;               /* its help string, "" */
    ((uint32_t *)(image + q))[2] = 32;
    test_module.title = "TestCmds";
    test_module.help = "";
    test_module.base = base;
    test_module.command_table = base + t;
    ros_module_add(&test_module, "");
}

void ros_selftest_oscli(void)
{
    line = ros_addr(ros_rma_alloc(1100));
    ros_vector_claim_native(WRCHV, wrch, 0);

    /* ---- what is nothing ---- */
    int n1 = says("", "") && says("  **  ", "") && says("| a comment", "") &&
             fails("Nosuchcommand", 0xD6);
    char *longline = ros_ptr(line);
    memset(longline, 'a', 1030);
    longline[1030] = 0;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line;
    ros_swi(&s, XOS_CLI);
    int n2 = s.v && ((os_error *)ros_ptr(s.r[0]))->errnum == 0x1E0;
    check(n1 && n2,
          "OS_CLI -- blank, *s and | comments are nothing; unknown, run as a file: &D6; 1024 characters &1E0",
          NULL);

    /* ---- the kernel's commands: found, abbreviated, any case ---- */
    int e1 = says("Echo Hello <Sys$RCLimit>|G", "Hello 256\x07\n\r") &&
             says("*ECHO hi", "hi\n\r") && says("ec. abbreviated", "abbreviated\n\r") &&
             says("UtilityModule:Echo prefixed", "prefixed\n\r") &&
             fails("UtilityModule:Nope", 0xFE);
    check(e1, "OS_CLI -- *Echo through GSTrans; any case, abbreviated, or Module:Command",
          NULL);

    /* *Show lists with paged mode on, VDU 14 and 15 around it, as the
     * kernel's (RISC OS 5.30 spools them) */
    int v1 = says("Set Test$A hello world", "") && var_is("Test$A", "hello world") &&
             says("Se. Test$B by abbreviation", "") && var_is("Test$B", "by abbreviation") &&
             says("SetEval Test$N 6*7", "") &&
             says("Show Test$N", "\x0eTest$N(Number) : 42\n\r\x0f") &&
             says("SetMacro Test$M <Test$A>!", "") &&
             says("Show Test$M", "\x0eTest$M(Macro) : <Test$A>!\n\r\x0f") &&
             says("Set Test$Q a<b", "") && says("Show Test$Q", "\x0eTest$Q : a|<b\n\r\x0f");
    int v2 = says("Unset Test$*", "") && !var_is("Test$A", "hello world") &&
             says("Unset Sys$Time", "") && fails("Unset", 0xDC);
    cli("Set x");
    int v3 = strcmp(last_error(), "Syntax: *Set <varname> <value>") == 0;
    check(v1 && v2 && v3,
          "*Set, *SetEval, *SetMacro, *Show, *Unset -- and the syntax when the count is wrong",
          "%s", last_error());

    /* ---- aliases ---- */
    int a1 = says("Set Alias$Greet Echo Hi %0", "") && says("Greet there", "Hi there\n\r") &&
             says("Gr. abbreviated", "Hi abbreviated\n\r") &&
             says("Set Alias$Two Echo A%0|MEcho B", "") && says("Two x", "Ax\n\rB\n\r") &&
             says("Set Alias$Pre Echo pre-", "") && says("Pre a b", "pre-a b\n\r") &&
             fails("%Greet x", 0xD6) && says("Set Alias$Loop Loop", "") &&
             fails("Loop", 0x1E1);
    cli("Unset Alias$Greet"), cli("Unset Alias$Two"), cli("Unset Alias$Pre"),
        cli("Unset Alias$Loop");      /* only its own: FileSwitch's run actions stay */
    check(a1, "Aliases -- %0 substituted, the rest appended, lines split, % skips them, "
              "recursion &1E1", NULL);

    /* ---- the kernel's circular buffers: RISC OS 5.30 runs fifteen aliases
     * deep and back whole. Sixteen runs the innermost line, then the
     * outermost finds its buffer gone, &1E1 (bigmacfarm) ---- */
    int ran15, ran16, ran20;
    uint32_t e15 = alias_chain(15, &ran15), e16 = alias_chain(16, &ran16),
             e20 = alias_chain(20, &ran20);
    check(e15 == 0 && ran15 && e16 == 0x1E1 && ran16 && e20 == 0x1E1 && ran20,
          "Aliases -- the kernel's circular buffers: 15 deep and back; 16 and 20 run the innermost "
          "line, then Expansion too complex, as RISC OS 5.30", "15: &%X %d, 16: &%X %d, 20: &%X %d",
          e15, ran15, e16, ran16, e20, ran20);

    /* ---- commands that never come back: an alias, an Obey file's last line,
     * a file's run action, and *IF's THEN. Each, in the Wimp where an
     * application's start has a task that ends, leaves neither an
     * expansion in progress nor memory ---- */
    ros_module_add(&never_module, "");
    char *obey = ros_rma_alloc(64), *saved = ros_rma_alloc(256);
    strcpy(obey, "Set Test$Obeyed yes\nNeverBack %0\n");
    char om[48];
    snprintf(om, sizeof om, "Obey -m %X one", ros_addr(obey));
    uint32_t rv[5] = { ros_addr(saved + 128), ros_addr(saved), 127, 0, 0 };
    strcpy(saved + 128, "Alias$@RunType_FEB");
    struct ros_cpu rs;
    ros_cpu_enter(&rs);
    memcpy(rs.r, rv, sizeof rv);
    ros_swi(&rs, XOS_ReadVarVal);
    uint32_t feb_len = rs.v ? 0 : rs.r[2];
    cli("Set Alias$Gone NeverBack %*0");
    cli("Set Alias$@RunType_FEB NeverBack %*0");
    const char *ways[4] = { "Gone a b", om, "Run Resources:$.Apps.!Chars.!Run",
                            "IF 1 THEN NeverBack" };
    int g1 = 1;
    for (unsigned w = 0; w < 4; w++)
        g1 = g1 && never_back(ways[w]);             /* anything made once, made */
    uint32_t used0, used1, free_bytes;
    ros_rma_stats(&used0, &free_bytes);
    unsigned went = 0;
    for (unsigned k = 0; k < 40; k++)
        for (unsigned w = 0; w < 4; w++)
            went += (unsigned)never_back(ways[w]);
    ros_rma_stats(&used1, &free_bytes);
    int obeyed = var_is("Test$Obeyed", "yes");
    if (feb_len) {                                  /* FileSwitch's run action back */
        ros_cpu_enter(&rs);
        rs.r[0] = ros_addr(saved + 128), rs.r[1] = ros_addr(saved), rs.r[2] = feb_len;
        rs.r[3] = 0, rs.r[4] = 4;                    /* as it was: a literal */
        ros_swi(&rs, XOS_SetVarVal);
    }
    cli("Unset Alias$Gone"), cli("Unset Test$Obeyed");

    /* An application started by an Obey file's line that has lines after
     * it comes back to the file when it exits, whatever the exit handler
     * underneath. RISC OS's Obey module puts its own in front (s.Obey,
     * Service_NewApplication; MyExitHandler goes on with the next line).
     * An example is an !Run's "Run ...!ResFind" (BASIC, ending with OS_Exit)
     * in a Wimp task, whose handler is the Wimp's and ends the task. Here
     * that handler is task_exit_handler. The application on the last line
     * exits to it. The file is closed first, as RISC OS's Obey drops an
     * exhausted file. */
    uint32_t xh = ros_native_entry(task_exit_handler, "test:TaskExit");
    uint32_t xr1 = xh, xr2 = 0, xr3 = 0;
    ros_env_change(ROS_ENV_EXIT, &xr1, &xr2, &xr3);        /* the old one back in xr1-xr3 */
    strcpy(obey, "Set Test$Before yes\nExitApp\nSet Test$After yes\n");
    snprintf(om, sizeof om, "Obey -m %X", ros_addr(obey));
    int x1 = !never_back(om) && var_is("Test$Before", "yes") && var_is("Test$After", "yes");
    cli("Unset Test$Before"), cli("Unset Test$After");
    strcpy(obey, "Set Test$Before yes\nExitApp\n");
    int x2 = never_back(om) && var_is("Test$Before", "yes");
    cli("Unset Test$Before");
    ros_env_change(ROS_ENV_EXIT, &xr1, &xr2, &xr3);
    check(x1 && x2, "Obey -- an application started on a line before the last comes back to the "
          "file when it exits, past the exit handler underneath (a Wimp task's); on the last line, "
          "to that handler", "middle %d, last %d", x1, x2);

    int after15;
    uint32_t e15b = alias_chain(15, &after15);
    int g2 = says("Echo still here", "still here\n\r");
    cli("RMKill NeverBackTest");
    ros_rma_free(obey), ros_rma_free(saved);
    /* (each of them left 1K to 5K of RMA behind before, 1024 is less than one) */
    check(g1 && went == 160 && obeyed && used1 < used0 + 1024 && e15b == 0 && after15 && g2 && feb_len,
          "Commands that never come back -- 40 each through an alias, an Obey file's last line, "
          "FileSwitch's run action and *IF's THEN: none left in progress (15 deep still whole), "
          "no RMA left behind",
          "%u of 160 gone, RMA used %u then %u, then 15 deep &%X %d", went, used0, used1, e15b, after15);

    /* ---- OS_SubstituteArgs ---- */
    char *b = ros_rma_alloc(256);
    strcpy(b, "a \"b c\" d");
    strcpy(b + 64, "x%1y%*2z%%");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b + 128), c.r[2] = 64;
    c.r[3] = ros_addr(b + 64), c.r[4] = 10;
    ros_swi(&c, XOS_SubstituteArgs);
    int u1 = !c.v && c.r[2] == 11 && memcmp(b + 128, "x\"b c\"ydz%", 10) == 0 &&
             b[128 + 10] == 0;
    strcpy(b + 64, "[%0]");
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b + 128), c.r[2] = 64;
    c.r[3] = ros_addr(b + 64), c.r[4] = 4;
    ros_swi(&c, XOS_SubstituteArgs);
    int u2 = !c.v && memcmp(b + 128, "[a]\"b c\" d", 10) == 0;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b + 128), c.r[2] = 64;
    c.r[3] = ros_addr(b + 64), c.r[4] = 4, c.r[5] = 0x80000000u;
    ros_swi(&c, XOS_SubstituteArgs32);
    int u3 = !c.v && c.r[2] == 4 && memcmp(b + 128, "[a]", 3) == 0;
    ros_rma_free(b);
    check(u1 && u2 && u3,
          "OS_SubstituteArgs -- %n, %*n, %%, quoted arguments; the unused appended unless "
          "R5 says not", "%u \"%s\"", c.r[2], (char *)ros_ptr(c.r[1]));

    /* ---- *FX, *Key, *IF, *Eval, *Error, *Time ---- */
    ros_vector_claim_native(BYTEV, bytev, 0);
    int f1 = says("FX 250,1,2", "") && fx[0] == 250 && fx[1] == 1 && fx[2] == 2;
    int f2 = says("fx250 7", "") && fx[1] == 7 && fx[2] == 0;
    int f3 = fails("FX 250 1 2 3", 0xDC);
    ros_vector_release_native(BYTEV, bytev, 0);
    int k1 = says("Key 1 |!hello", "") && var_is("Key$1", "\xE8" "ello") &&
             fails("Key 16 x", 0xFB) && says("Unset Key$1", "");
    check(f1 && f2 && f3 && k1,
          "*FX -- commas optional, a number straight after; too many &DC; *Key sets Key$n",
          NULL);

    int i1 = says("IF 1=1 THEN Echo yes ELSE Echo no", "yes\n\r") &&
             says("IF 1=2 THEN Echo yes ELSE Echo no", "no\n\r") &&
             says("IF 0 THEN Echo yes", "") && fails("IF 1 Echo", 0xDC) &&
             fails("IF \"a\" THEN Echo x", 0xDC);
    /* The kernel's IF_Code takes " Else" as ELSE only with a space after it
     * (its check of the next byte against CR, LF and 0 tests the pointer,
     * not the byte): an Else ending the line is the THEN part's (#59) */
    int i5 = says("IF 1=1 THEN Echo B Else", "B Else\n\r") &&
             says("IF 1=0 THEN Echo A Else", "") &&
             says("IF 1=0 THEN Echo x Elsewhere Echo y", "") &&
             says("If 1=0 Then Echo A else  Echo two", "two\n\r");
    int i2 = says("Eval 6*7", "Result is an integer, value : 42\n\r") &&
             says("Eval \"a\"+\"<\"", "Result is a string, value : a|<\n\r");
    int i3 = cli("Error 123 Oops <Sys$RCLimit>") == 123 && strcmp(last_error(), "Oops 256") == 0;
    cli("Time");
    int i4 = outn == 26 && out[3] == ',' && out[15] == '.' && out[18] == ':';
    check(i1 && i2 && i3 && i4,
          "*IF THEN ELSE, *Eval, *Error with its number and GSTransed text, *Time", "\"%s\"",
          out);
    check(i5, "*IF -- an Else ending the line is not ELSE, one with a space after it is (#59)",
          "\"%s\"", out);

    /* ---- a module's own commands, from its table ---- */
    add_test_module();
    int t1 = says("TestArgs one <Sys$RCLimit> \"three four\"", "") && seen_argc == 3 &&
             strcmp(seen_tail, "one 256 \"three four\" ") == 0 &&
             seen_r12 == test_module.private_word;
    cli("TestArgs");
    int t2 = strcmp(last_error(), "Syntax: *TestArgs <a> <b>") == 0;
    int t3 = fails("TestArgs a <Nope$X>", 0xFD) && says("TestCmds:TestA. x", "") &&
             seen_argc == 1;
    int t4 = cli("Modules") == 0 && strstr(out, "UtilityModule") && strstr(out, "TestCmds");
    int t5 = cli("Help Set") == 0 && strstr(out, "==> Help on keyword Set") &&
             strstr(out, "Syntax: *Set <varname> <value>");
    check(t1 && t2 && t3 && t4 && t5,
          "Module commands -- counted, GSTransed as the table asks, R12 the private word; "
          "*Modules, *Help", "\"%s\" %u; %s", seen_tail, seen_argc, last_error());

    /* The kernel's bytes: paged mode on and off around it, CR before each
     * header, "Module is:" and the help string, a blank line before each
     * kind of keyword. */
    int h1 = says("Help TestC.", "\x0e\r==> Help on keyword TestCmds\n\rModule is: \n\r\n\r"
                                 "Commands provided:\n\rTestArgs\n\r\x0f");
    int h2 = says("Help NoSuch", "\x0eNo help found.\n\r\x0f");
    check(h1 && h2, "*Help on a module's title -- \"Module is:\" and its help string, its "
          "commands; paged, as the kernel's", NULL);

    int r1 = fails("Echo { > Nofs:file }", 0xF8) && says("Echo \"{ > file }\"", "\"{ > file }\"\n\r");
    check(r1, "OS_CLI -- redirection opens its files (a bad name fails); not in quotes", NULL);

    ros_vector_release_native(WRCHV, wrch, 0);
    ros_rma_free(ros_ptr(line));
}
