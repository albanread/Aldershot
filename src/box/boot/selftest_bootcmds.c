/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_bootcmds.c: BootCommands, native (modules/bootcmds).  It tests
 * what must always hold of the *commands that !Boot sequences are written
 * in.  The rest, held to RISC OS 5.30's module, is tests/desktop/bootcmds.
 * The four commands that 5.30's module has not got are *AppPath, *PrepPath,
 * *RemPath and *Canonical.  They are from BootCommands 1.54 and are held
 * here to its source.
 *
 * The files are on a HostFS disc of the test's own, "BootTest", in a
 * directory made for it: in the box on the host share, hosted in TMPDIR.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bootcmds.h"
#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u

static uint32_t line;                       /* arena: a command, or a name */
static const os_error *last;
static char out[4096];
static unsigned outn;
static char root[512];

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1 && s->r[0] != 13)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI: the error number or 0; what it printed in out, LF CR as LF */
static uint32_t cli(const char *cmd)
{
    strcpy(ros_ptr(line), cmd);
    outn = 0, out[0] = 0;
    ros_vector_claim_native(WRCHV, wrch, 0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line;
    ros_swi(&s, XOS_CLI);
    ros_vector_release_native(WRCHV, wrch, 0);
    last = s.v ? ros_ptr(s.r[0]) : NULL;
    return last ? last->errnum : 0;
}

static const char *last_error(void)
{
    return last ? last->errmess : "";
}

/* A variable's value, expanded; "unset" when there is none */
static const char *var(const char *name)
{
    static char v[512];
    strcpy(ros_ptr(line), name);
    uint32_t b = line + 256;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line, s.r[1] = b, s.r[2] = 255, s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, XOS_ReadVarVal);
    if (s.v)
        return "unset";
    memcpy(v, ros_ptr(b), s.r[2]);
    v[s.r[2]] = 0;
    return v;
}

/* A file's bytes and length through OS_File 16, into buf */
static int load(const char *name, char *buf, uint32_t max, uint32_t *len)
{
    strcpy(ros_ptr(line), name);
    uint32_t b = ros_addr(ros_rma_alloc(max));
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 16, s.r[1] = line, s.r[2] = b, s.r[3] = 0;
    ros_swi(&s, XOS_File);
    int ok = !s.v && s.r[4] <= max;
    if (ok) {
        memcpy(buf, ros_ptr(b), s.r[4]);
        *len = s.r[4];
    }
    ros_rma_free(ros_ptr(b));
    return ok;
}

static void host_file(const char *leaf, const char *text)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, leaf);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void host_dir(const char *leaf)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, leaf);
    mkdir(p, 0777);
}

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

static void module_and_syntax(void)
{
    struct ros_module *m = ros_module_first();
    while (m && strcmp(m->title, "BootCommands") != 0)
        m = m->next;
    unsigned n = 0;
    for (const struct ros_command *c = m ? m->commands : NULL; c && c->name; c++)
        n++;
    check(m && n == 17 && ros_module_version(m) == 0x15400,
          "BootCommands -- native, 1.54, its seventeen commands", "%u commands", n);

    int s1 = cli("AddApp") == 0xDC && !strcmp(last_error(), "Syntax:\t*AddApp <application>");
    int s2 = cli("FreePool x") == 0xDC && !strcmp(last_error(), "Syntax:\t*FreePool");
    int s3 = cli("AppPath a") == 0xDC &&
             !strcmp(last_error(), "Syntax:\t*AppPath <variable> <path element>");
    int h = cli("Help X") == 0 &&
            strstr(out, "==> Help on keyword X\n*X passes its argument to the command") &&
            strstr(out, "Syntax: *X <command>\n");
    check(s1 && s2 && s3 && h,
          "BootCommands -- a wrong count answers with the syntax, its TAB kept; *Help pretty "
          "printed", "%s | %s", last_error(), out);
}

static void do_x_ifthere(void)
{
    cli("Set BootTest$V Hello");
    int d1 = cli("Do Echo <BootTest$V> world") == 0 && !strcmp(out, "Hello world\n");
    int d2 = cli("Do Error 17 Failed <BootTest$V>") == 17 && !strcmp(last_error(), "Failed Hello");
    char big[400];
    memset(big, 'x', 300);
    big[300] = 0;
    char cmd[512];
    snprintf(cmd, sizeof cmd, "Set BootTest$Big %s", big);
    cli(cmd);
    int d3 = cli("Do Echo <BootTest$Big><BootTest$Big><BootTest$Big><BootTest$Big>") == 0x1E4 &&
             !strcmp(last_error(), "Buffer overflow");
    check(d1 && d2 && d3,
          "*Do -- its argument GSTransed, then run; its error returned; past 1024 characters, "
          "Buffer overflow", "%s", last_error());

    cli("Unset X$Error");
    int x1 = cli("X Error 5 First") == 0 && !strcmp(var("X$Error"), "First");
    int x2 = cli("X Error 6 Second") == 0 && !strcmp(var("X$Error"), "First");
    int x3 = cli("X Echo fine") == 0 && !strcmp(out, "fine\n");
    cli("Unset X$Error");
    check(x1 && x2 && x3, "*X -- the first error in X$Error, the next kept out, none returned",
          "%s", var("X$Error"));

    int i1 = cli("IfThere Resources:$.Resources.BootCmds.Messages then Echo yes else Echo no")
             == 0 && !strcmp(out, "yes\n");
    int i2 = cli("IfThere Resources:$.Resources.BootCmds then Echo yes else Echo no") == 0 &&
             !strcmp(out, "yes\n");
    int i3 = cli("IfThere Resources:$.Nothing then Echo yes else Echo no") == 0 &&
             !strcmp(out, "no\n");
    int i4 = cli("IfThere Resources:$.Nothing Echo yes") == 0xDC &&
             !strncmp(last_error(), "Syntax:\t*IfThere", 16);
    check(i1 && i2 && i3 && i4,
          "*IfThere -- a file or a directory there, or nothing; *If's syntax error its own",
          "%s | %s", last_error(), out);
}

static void paths(void)
{
    cli("Unset BootTest$Path");
    int a1 = cli("AppPath BootTest$Path a.") == 0 && !strcmp(var("BootTest$Path"), "a.");
    int a2 = cli("AppPath BootTest$Path b.") == 0 && cli("AppPath BootTest$Path A.") == 0 &&
             !strcmp(var("BootTest$Path"), "b.,A.");
    int p1 = cli("PrepPath BootTest$Path c.") == 0 && !strcmp(var("BootTest$Path"), "c.,b.,A.");
    int p2 = cli("PrepPath BootTest$Path B.") == 0 && !strcmp(var("BootTest$Path"), "B.,c.,A.");
    int r1 = cli("RemPath BootTest$Path a.") == 0 && !strcmp(var("BootTest$Path"), "B.,c.");
    int r2 = cli("RemPath BootTest$Path b.") == 0 && cli("RemPath BootTest$Path C.") == 0 &&
             !strcmp(var("BootTest$Path"), "");
    int r3 = cli("RemPath BootTest$None x.") == 0 && !strcmp(var("BootTest$None"), "unset");
    cli("SetMacro BootTest$Macro <BootTest$Path>");
    int w1 = cli("AppPath BootTest$Macro x.") == BOOTCMDS_ERR_WANT_STR &&
             !strcmp(last_error(), "System variable must contain a string");
    cli("SetEval BootTest$Num 7");
    int w2 = cli("Canonical BootTest$Num") == BOOTCMDS_ERR_WANT_STR;
    cli("Set BootTest$Dir Resources:$.Resources.BootCmds.^");
    int c1 = cli("Canonical BootTest$Dir") == 0 &&
             !strcmp(var("BootTest$Dir"), "Resources:$.Resources");
    check(a1 && a2 && p1 && p2 && r1 && r2 && r3 && w1 && w2 && c1,
          "*AppPath, *PrepPath, *RemPath, *Canonical (5.31's) -- no duplicates, without case; "
          "a variable not a string refused; a path canonicalised", "%s / %s",
          var("BootTest$Path"), last_error());
    cli("Unset BootTest$*");
}

static void add_app(void)
{
    host_dir("Apps");
    host_dir("Apps/!BTOne");
    host_file("Apps/!BTOne/!Boot,feb", "Set BTOne$Booted yes\n");
    host_file("Apps/!BTOne/!Help", "help\n");
    host_file("Apps/!BTOne/!Run,feb", "Set BTOne$Ran yes\n");
    host_dir("Apps/!BTTwo");
    host_file("Apps/!BTTwo/!Sprites,ff9", "not sprites\n");
    int a1 = cli("AddApp HostFS::BootTest.$.Apps.!BT*") == 0;
    char b[512];
    uint32_t n = 0;
    int f1 = load("Resources:$.Apps.!BTOne.!Run", b, sizeof b, &n) && n == 36 &&
             !memcmp(b, "/HostFS::BootTest.$.Apps.!BTOne %*0\n", 36);
    int f2 = load("Resources:$.Apps.!BTOne.!Boot", b, sizeof b, &n) &&
             !memcmp(b, "/HostFS::BootTest.$.Apps.!BTOne.!Boot %*0\n", n);
    int f3 = load("Resources:$.Apps.!BTOne.!Help", b, sizeof b, &n) &&
             !memcmp(b, "Filer_Run HostFS::BootTest.$.Apps.!BTOne.!Help\n", n);
    int f4 = load("Resources:$.Apps.!BTTwo.!Boot", b, sizeof b, &n) &&
             !memcmp(b, "IconSprites HostFS::BootTest.$.Apps.!BTTwo.!Sprites\n", n);
    int e1 = cli("AddApp HostFS::BootTest.$.Apps.!BTNone") == 0xD6 &&
             !strcmp(last_error(), "Directory '!BTNone' not found");
    int e2 = cli("AddApp HostFS::BootTest.$.Apps.!BTNone*") == 0;
    int e3 = cli("AddApp Resources:$.Apps.!BTOne") == 0;
    check(a1 && f1 && f2 && f3 && f4 && e1 && e2 && e3,
          "*AddApp -- !Boot, !Help and !Run Obey files in ResourceFS's Apps, as RISC OS writes "
          "them; a name not there is an error, a wildcard not", "%s", last_error());
}

static void cmos(void)
{
    cli("Set Boot$OSVersion 530");
    int s1 = cli("SaveCMOS HostFS::BootTest.$.CMOS") == 0;
    char b[2100];
    uint32_t n = 0, word = 0;
    int s2 = load("HostFS::BootTest.$.CMOS", b, sizeof b, &n) && n == 2052;
    if (s2)
        memcpy(&word, b + 2048, 4);
    int l1 = cli("LoadCMOS HostFS::BootTest.$.CMOS") == 0 && !out[0];
    cli("Set Boot$OSVersion 531");
    int l2 = cli("LoadCMOS HostFS::BootTest.$.CMOS") == 0 &&
             !strcmp(out, "CMOS file is for a different OS version\n");
    int l3 = cli("LoadCMOS HostFS::BootTest.$.Nothing") == 0 &&
             !strcmp(out, "File 'HostFS::BootTest.$.Nothing' not found\n");
    host_file("Short,ff2", "short");
    int l4 = cli("LoadCMOS HostFS::BootTest.$.Short") == 0 && !strcmp(out, "Corrupt CMOS file\n");
    cli("Unset Boot$OSVersion");
    check(s1 && s2 && word == 530 && l1 && l2 && l3 && l4,
          "*SaveCMOS and *LoadCMOS -- 2048 bytes and the OS version; loaded back; what it "
          "refuses written, never an error", "%s | %s", last_error(), out);
}

static void repeat(void)
{
    host_dir("R");
    host_file("R/b", "b\n");
    host_file("R/A", "A\n");
    host_dir("R/!C");
    cli("Set Sys$RCLimit 256");
    int r1 = cli("Repeat Echo HostFS::BootTest.$.R -sort") == 0 &&
             !strcmp(out, "HostFS::BootTest.$.R.!C\nHostFS::BootTest.$.R.A\nHostFS::BootTest.$.R.b\n") &&
             !strcmp(var("Sys$ReturnCode"), "0");
    int r2 = cli("Repeat Echo HostFS::BootTest.$.R -files -sort tail") == 0 &&
             !strcmp(out, "HostFS::BootTest.$.R.A tail\nHostFS::BootTest.$.R.b tail\n");
    int r3 = cli("Repeat Error HostFS::BootTest.$.R -applications") == 0 &&
             !strcmp(out, "Repeat: HostFS::BootTest.$.R.!C\n") &&
             !strcmp(var("Sys$ReturnCode"), "1");
    cli("Unset X$Error");
    int r4 = cli("Repeat Error HostFS::BootTest.$.R -sort -continue") == 0 && !out[0] &&
             !strcmp(var("X$Error"), "HostFS::BootTest.$.R.!C") &&
             !strcmp(var("Sys$ReturnCode"), "0");
    cli("Unset X$Error");
    cli("Set Sys$RCLimit 0");
    int r5 = cli("Repeat Echo HostFS::BootTest.$.R -type NoSuchType") == 0x800E06 &&
             !strcmp(out, "Repeat: File type is unrecognised\n");
    cli("Set Sys$RCLimit 256");
    cli("Set Sys$ReturnCode 0");
    check(r1 && r2 && r3 && r4 && r5,
          "*Repeat -- sorted without case, a tail, files or applications; a failure written, "
          "return code 1; -continue's X$Error; past Sys$RCLimit, exit()'s error",
          "%s | %s", last_error(), out);
}

/* The RMA's size, dynamic area 1's */
static uint32_t rma_size(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 1;
    ros_swi(&c, XOS_ReadDynamicArea);
    return c.v ? 0 : c.r[1];
}

static void memory(void)
{
    int m1 = cli("AppSize 512M") == 0 && cli("ShrinkRMA") == 0;
    uint32_t shrunk = rma_size();
    int grew = cli("AddToRMA 16K") == 0;
    uint32_t added = rma_size();
    int m2 = cli("AddToRMA 1024M") == 0x1C1 && !strcmp(last_error(), "Memory cannot be moved");
    int m3 = cli("AddToRMA 4096M") == 0;                /* 2^32: nothing */
    int m4 = cli("AppSize x") == 0x16B;
    check(m1 && grew && m2 && m3 && m4,
          "*AppSize, *ShrinkRMA, *AddToRMA -- sizes in K and M, wrapping at 32 bits; more "
          "than the RMA has room for refused", "%s", last_error());
    int again = cli("ShrinkRMA") == 0;
    uint32_t back = rma_size();
    check(shrunk && added == shrunk + 16 * 1024 && again && back < added,
          "*AddToRMA and *ShrinkRMA move the RMA's size, dynamic area 1: 16K more, then back "
          "to its top block, as BootCommands' OS_ChangeDynamicArea calls",
          "shrunk %u, 16K added %u, shrunk again %u", shrunk, added, back);
}

void ros_selftest_bootcmds(void)
{
    line = ros_addr(ros_rma_alloc(1024));
    module_and_syntax();
    do_x_ifthere();
    paths();
    memory();

    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-bootcmds-XXXXXX", tmp);
    if (!mkdtemp(root)) {
        check(0, "BootCommands -- no directory for the test disc", "no directory");
        return;
    }
    ros_hostfs_mount("BootTest", root);
    add_app();
    cmos();
    repeat();
    ros_hostfs_unmount("BootTest");
    remove_tree(root);
}
