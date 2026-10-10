/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_shadow.c: ARM shadows (#147). An ARM module of a native module's
 * title does not replace it. ARM callers reach the ARM one, and native
 * callers reach the native one.
 *
 * ShadowTest is built both ways. One build is tests/armrun/shadowtest.s, an
 * ARM module image that rosasm assembles, staged as a file on the disc
 * ShadowT and loaded by OS_Module 1. The other is a native descriptor here,
 * with the same title and SWI chunk, added as a ROM module is. Its SWI Who
 * answers "A" or "N" and the workspace its initialisation claimed. Extra is
 * the ARM build's only. An ARM caller is ARM code under the engine. It is a
 * three-word routine in the RMA (SWI n between a push and a pop of R14),
 * called from here through ros_call on the thread's task for calls only. So
 * its SVC goes through the bridge (box_swi) as any ARM program's does.
 *
 * First group of cases:
 *   case 6   the ARM module first: ARM-only, native Who gives A. The native
 *            one joins and the ARM one is demoted to its shadow, with its
 *            workspace kept. Native Who gives N, ARM Who A.
 *   case 4   an ARM RMKill (OS_Module 4) removes the shadow only. A second
 *            leaves the native module untouched.
 *   case 1   ARM code loads the ARM module over the native one, making a
 *            shadow. *NModules shows it after its twin, and *Modules does
 *            not.
 *   case 2   ARM Who A, native Who N. OS_CallASWI is the same. OS_Module 18
 *            and 12 give each caller its own. The shadow's own SWI is not a
 *            native caller's. SWI names depend on the kind (#149): ARM code
 *            converts ShadowTest_Extra and CHUNK+1 by the shadow's names, and
 *            native code by the native module's.
 *            A native RMKill of the twin promotes the shadow: native Who A.
 *   case 11  an ARM load of a never-shadowed title (FPEmulator) is
 *            absorbed: no error, nothing loaded.
 *   case 13  SharedCLibrary's built-in shadow is made by an ARM OS_Module
 *            lookup on a task for calls only (there is no ARM application,
 *            which is the null-task case). It is shown by *NModules, and ARM
 *            code reaches its SWIs.
 *
 * Second group, *commands by the OS_CLI's kind (the native ShadowTest is now
 * a ROM module, with its own *ShadowWho, which prints N):
 *   case 2   *ShadowWho, "ShadowTest:ShadowWho", an alias's line, an Obey
 *            file's line (by *Obey and run as a file) and *Help ShadowTest
 *            each reach the caller's own build.
 *   case 3   RMEnsure from each kind finds its own. ARM code's is
 *            satisfied by the shadow's 1.00. A native 1.00 is satisfied by
 *            the shadow too, but 2.00 is not. Its command runs as the
 *            caller's kind. With no shadow, the native 1.00 fails.
 *   case 4   *RMKill from ARM code (OS_Module 4 inherited) takes the shadow
 *            only. A second is quiet.
 *   case 5   a native *RMKill promotes the shadow (native Who A). A native
 *            *RMReInit starts the ROM module, which demotes it again: native
 *            Who N, and the shadow's workspace the same.
 *   RMClear  from ARM code kills the shadow and not the native module.
 *
 * Third group, services, vectors and shared state. Both builds count what
 * reaches them: the native one here, the ARM one in its workspace
 * (shadowtest.s). Each sets ShadowTest$Var as it initialises, and registers
 * Resources.ShadowTest.Messages ("Who:N" or "Who:A"):
 *   case 7   a ModeChange (notify) reaches both. A UKByte for a native
 *            OS_Byte &60 does not reach the shadow. One for an ARM OS_Byte
 *            &60 does, and the shadow's claim answers it (provide). The
 *            shadow's own ModulePostInit reaches shadows and not the native
 *            observer.
 *   case 8   a native OS_WriteC steps over the shadow's WrchV claim, and an
 *            ARM one reaches it. Its GraphicsV claim was refused, whereas
 *            an ARM-only module's, in case 6, was not.
 *   case 9   ShadowTest$Var keeps the native module's value after the
 *            shadow initialises, ShadowTest$New (new) is made, and after
 *            its initialisation ARM code's writes are made. The shadow's
 *            Messages do not hide the native one's, and its own new file is
 *            there.
 *   case 12  TickerV, EventV and UpCallV (notify vectors): the shadow's
 *            claims run for native and ARM callers alike, after the native
 *            claimants. The native claimants were on the vector first, so
 *            they would come after it in the chain. R8, set by the native
 *            claimant, is what the shadow sees. The shadow's claims do not
 *            run at all if a native claimant claimed. The ticker reaches
 *            it in the background.
 *
 * Fourth group, the escape hatch and the policy's homes:
 *   case 10  *ARMPrefer ShadowTest on is remembered with no shadow loaded
 *            (native Who still N). Then the shadow is loaded and preferred:
 *            native Who, *ShadowWho and OS_Module 18 reach it, *NModules
 *            says "(shadow, preferred)", and a native OS_Byte &60's UKByte
 *            and OS_WriteC reach it. *ARMPrefer lists it. With it off,
 *            native Who is N again. It is refused for SharedCLibrary.
 *   column   boot/rom_contents.c's column: MimeMap, Squash, ZLib,
 *            BootCommands and BASICVFP are shadowable, SharedCLibrary is
 *            built in, and FPEmulator is never shadowable.
 *   variable a native ShadowTest not in the ROM, with no policy of its
 *            own: an ARM load is absorbed until ROSGD$Shadowable lists it.
 *   case 12  RISC OS 5.30's MimeMap (System:Modules.Shadows.MimeMap,
 *            ARM code) is loaded as the native MimeMap's shadow. ARM callers'
 *            MimeMap_Translate reaches it and native callers' reaches the
 *            native one, with the same answers. Inet$MimeMappings is
 *            unchanged. *ARMPrefer MimeMap sends native callers to 5.30's
 *            and back.
 *   case 14  RISC OS 5.30's ZLib, squeezed (modsqz, #162), at
 *            System:Modules.Shadows.ZLib, is unsqueezed as 5.30's kernel
 *            does, and RMLoaded by ARM code as the native ZLib's shadow.
 *            ZLib_Version and *Help are checked. RMClear leaves it (bit 31
 *            of its finalise offset). OS_Module 11 from memory does the
 *            same.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/armbox.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "armrun_tests.h"
#include "fileswitch.h"
#include "resourcefs.h"
#include "selftest.h"

/* The scratch directory and its files gone, unmounted first (a run left one
 * in TMPDIR each time) */
static void scratch_gone(const char *name, const char *dir)
{
    ros_hostfs_unmount(name);
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[700];
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    if (d)
        closedir(d);
    rmdir(dir);
}

#define check ros_check

#define CHUNK      0x5BEC0u
#define WHO        (CHUNK + 0)
#define EXTRA      (CHUNK + 1)
#define X          0x20000u
#define WRCHV      0x03u
#define WS_ARM     0x5AD0A001u          /* the marks each build puts in its workspace */
#define WS_NATIVE  0x5AD0B001u

/* ---- the native build ------------------------------------------------------------- */

static struct ros_module native;

/* What reached the native build's service handler (case 7) */
static unsigned n_modechange, n_ukbyte, n_postinit_other;
static uint32_t native_files;           /* its ResourceFS block (case 9) */

static void st_service(struct ros_module *m, struct ros_cpu *s)
{
    switch (s->r[1]) {
    case 0x46:
        n_modechange++;
        break;
    case 0x07:
        n_ukbyte++;
        break;
    case 0xDA:                          /* another ShadowTest's PostInit */
        if (s->r[0] != m->base && s->r[2] && !strcmp(ros_ptr(s->r[2]), "ShadowTest"))
            n_postinit_other++;
        break;
    }
}

static void native_call(uint32_t n, struct ros_cpu *c)
{
    ros_swi(c, n | 0x20000u);
}

/* One ResourceFS file in a block of its own, in the RMA */
static uint32_t files_block(const char *name, const char *data)
{
    uint32_t nl = (uint32_t)strlen(name) + 1, dl = (uint32_t)strlen(data);
    uint32_t head = (20 + nl + 3) & ~3u, size = head + 4 + ((dl + 3) & ~3u);
    uint8_t *b = ros_rma_alloc(size + 4);
    if (!b)
        return 0;
    memset(b, 0, size + 4);
    uint32_t a = ros_addr(b);
    ros_st32(a, size);
    ros_st32(a + 4, 0xFFFFFF00u);
    ros_st32(a + 12, dl);
    ros_st32(a + 16, 3);
    memcpy(b + 20, name, nl);
    ros_st32(a + head, dl + 4);
    memcpy(b + head + 4, data, dl);
    return a;
}

/* The native claimants of UpCallV and EventV (case 12) act for the test's
 * codes only, and count them. R8 is set to "N", so the claimant after sees
 * that it ran. The CLAIM code is claimed. */
#define UPC_PASS   0x5AD0u
#define UPC_CLAIM  0x5AD1u
#define EV_USER    9u
static unsigned n_upcall, n_event;

static int st_upcallv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] != UPC_PASS && s->r[0] != UPC_CLAIM)
        return ROS_VECTOR_PASS;
    n_upcall++;
    s->r[8] = 'N';
    return s->r[0] == UPC_CLAIM ? ROS_VECTOR_CLAIM : ROS_VECTOR_PASS;
}

static int st_eventv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] != EV_USER || (s->r[1] != UPC_PASS && s->r[1] != UPC_CLAIM))
        return ROS_VECTOR_PASS;
    n_event++;
    s->r[8] = 'N';
    return s->r[1] == UPC_CLAIM ? ROS_VECTOR_CLAIM : ROS_VECTOR_PASS;
}

static void st_who(struct ros_cpu *s)
{
    s->r[0] = 'N';
    s->r[1] = ros_ld32(native.private_word);
    s->v = 0;
}

static os_error *st_init(struct ros_module *m, const char *tail)
{
    (void)tail;
    uint32_t *ws = ros_rma_alloc(16);
    if (!ws)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    ws[0] = WS_NATIVE;
    ros_st32(m->private_word, ros_addr(ws));
    /* ShadowTest$Var "N", and its Messages (case 9) */
    char *nv = ros_rma_alloc(32);
    struct ros_cpu c;
    if (nv) {
        strcpy(nv, "ShadowTest$Var");
        strcpy(nv + 16, "N");
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(nv), c.r[1] = ros_addr(nv + 16), c.r[2] = 1, c.r[4] = 4;
        native_call(0x24u, &c);
        ros_rma_free(nv);
    }
    if (!native_files)
        native_files = files_block("Resources.ShadowTest.Messages", "Who:N\n");
    ros_cpu_enter(&c);
    c.r[0] = native_files;
    native_call(0x41B40u, &c);
    return NULL;
}

static os_error *st_final(struct ros_module *m, int fatal)
{
    (void)fatal;
    uint32_t ws = ros_ld32(m->private_word);
    if (ws)
        ros_rma_free(ros_ptr(ws));
    ros_st32(m->private_word, 0);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = native_files;
    native_call(0x41B41u, &c);
    return NULL;
}

static os_error *st_cmd_who(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 'N';
    ros_swi(&c, 0x20000u | 0x00u);      /* XOS_WriteC */
    ros_cpu_enter(&c);
    ros_swi(&c, 0x20000u | 0x03u);      /* XOS_NewLine */
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static const struct ros_command st_cmds[] = {
    { "ShadowWho", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *ShadowWho", "Prints N.", st_cmd_who },
    { NULL, 0, NULL, NULL, NULL },
};

static ros_swi_thunk *const st_thunks[] = { st_who };
static const char *const st_names[] = { "Who" };

static void native_init(void)
{
    memset(&native, 0, sizeof native);
    native.title = "ShadowTest";
    native.help = "ShadowTest\t0.50 (04 Oct 2026) native";
    native.init = st_init;
    native.final = st_final;
    native.service = st_service;
    native.swi_chunk = CHUNK;
    native.swi_count = 1;
    native.swi_thunks = st_thunks;
    native.swi_names = st_names;
    native.swi_prefix = "ShadowTest";
    native.shadow_policy = ROS_SHADOW_ALLOW;
}

/* ---- callers of each kind ------------------------------------------------------------ */

static uint32_t tramp;                  /* the ARM routine: push, SWI n, pop */

/* A SWI from native code: r[0..9] in and out; the error, or NULL */
static const os_error *native_swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | X);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* The same SWI made by ARM code */
static const os_error *arm_swi(uint32_t n, uint32_t r[10])
{
    ros_st32(tramp, 0xE52DE004u);                       /* STR   lr, [sp, #-4]! */
    ros_st32(tramp + 4, 0xEF000000u | ((n | X) & 0x00FFFFFFu));
    ros_st32(tramp + 8, 0xE49DF004u);                   /* LDR   pc, [sp], #4: R0-R9 the SWI's */
    ros_armrun_code_add(tramp, tramp + 12);             /* (again: retranslated) */
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_call(&c, tramp);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

typedef const os_error *caller(uint32_t n, uint32_t r[10]);

/* Who, from a caller: its letter (0 on an error), *ws the workspace */
static uint32_t who(caller *c, uint32_t *ws)
{
    uint32_t r[10] = { 0 };
    if (c(WHO, r))
        return 0;
    if (ws)
        *ws = r[1];
    return r[0];
}

/* Who through OS_CallASWI (R10 the SWI's number) */
static uint32_t who_callaswi(caller *c)
{
    struct ros_cpu cc;
    ros_cpu_enter(&cc);
    if (c == native_swi) {
        cc.r[10] = WHO | X;
        ros_swi(&cc, X | 0x6Fu);
        return cc.v ? 0 : cc.r[0];
    }
    ros_st32(tramp, 0xE92D4FF0u);                       /* STMFD sp!, {r4-r11, lr} */
    ros_st32(tramp + 4, 0xE59FA008u);                   /* LDR   r10, [pc, #8]: the word at +20 */
    ros_st32(tramp + 8, 0xEF000000u | X | 0x6Fu);       /* SWI   XOS_CallASWI */
    ros_st32(tramp + 12, 0xE8BD8FF0u);                  /* LDMFD sp!, {r4-r11, pc} */
    ros_st32(tramp + 16, 0);
    ros_st32(tramp + 20, WHO | X);
    ros_armrun_code_add(tramp, tramp + 24);
    ros_call(&cc, tramp);
    return cc.v ? 0 : cc.r[0];
}

/* OS_SWINumberFromString of name from a caller: the number, or ~0 */
static uint32_t swi_from(caller *c, const char *name)
{
    static char *buf;
    if (!buf)
        buf = ros_rma_alloc(64);
    snprintf(buf, 64, "%s", name);
    uint32_t r[10] = { 0 };
    r[1] = ros_addr(buf);
    return c(0x39, r) ? 0xFFFFFFFFu : r[0];
}

/* OS_SWINumberToString of n from a caller, into out */
static void swi_to(caller *c, uint32_t n, char *out, size_t size)
{
    static char *buf;
    if (!buf)
        buf = ros_rma_alloc(64);
    uint32_t r[10] = { n, ros_addr(buf), 64 };
    snprintf(out, size, "%s", c(0x38, r) ? "(error)" : buf);
}

/* OS_Module reason, R1 a name (in the RMA), from a caller: r out */
static const os_error *module(caller *c, uint32_t reason, const char *name, uint32_t r[10])
{
    static char *buf;
    if (!buf)
        buf = ros_rma_alloc(256);
    memset(r, 0, 10 * sizeof r[0]);
    r[0] = reason;
    if (name) {
        snprintf(buf, 256, "%s", name);
        r[1] = ros_addr(buf);
    }
    return c(0x1Eu, r);
}

/* OS_Module 12 from a caller: the base (R3) of the module numbered like
 * the chain's module n (0 the first) */
static uint32_t enumerated(caller *c, uint32_t n)
{
    uint32_t r[10] = { 12, n, 0 };
    return c(0x1Eu, r) ? 0 : r[3];
}

/* ---- *commands, their output kept -------------------------------------------------- */

static char out[16384];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    char ch = (char)s->r[0];
    if (ch != '\r' && outn < sizeof out - 1)
        out[outn++] = ch;
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI from a caller, the output kept in out */
static const os_error *cli_as(caller *by, const char *line)
{
    static char err[260];
    size_t n = strlen(line);
    char *c = ros_rma_alloc((uint32_t)n + 1);
    memcpy(c, line, n);
    c[n] = '\r';
    outn = 0, out[0] = 0;
    ros_vector_claim_native(WRCHV, wrch, 0);
    uint32_t r[10] = { ros_addr(c) };
    const os_error *e = by(0x05u, r);
    ros_vector_release_native(WRCHV, wrch, 0);
    ros_rma_free(c);
    if (!e)
        return NULL;
    memcpy(err, e, sizeof err);
    return (const os_error *)err;
}

static const os_error *cli(const char *line)
{
    return cli_as(native_swi, line);
}

/* *ShadowWho's answer to a caller of a line: its letter, or '?' */
static char line_who(caller *by, const char *line)
{
    if (cli_as(by, line))
        return 'E';
    for (const char *p = out; *p; p++)
        if ((*p == 'A' || *p == 'N') && (p[1] == '\n' || p[1] == 0) && (p == out || p[-1] == '\n'))
            return *p;
    return '?';
}

/* The lines of out naming title, each copied into lines (separated by |) */
static unsigned lines_naming(const char *title, char *lines, size_t room)
{
    unsigned n = 0;
    size_t used = 0;
    lines[0] = 0;
    for (const char *p = out; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[200];
        snprintf(line, sizeof line, "%.*s", (int)len, p);
        const char *t = strstr(line, title);
        if (t && (t[strlen(title)] == 0 || t[strlen(title)] == ' ' || t[strlen(title)] == '%')) {
            n++;
            used += (size_t)snprintf(lines + used, used < room ? room - used : 0, "%s|", line);
        }
        p += len + (e ? 1 : 0);
    }
    return n;
}

/* ---- the cases -------------------------------------------------------------------------- */

static char dir[256];

static void stage(const char *leaf, const char *title)
{
    char path[320];
    snprintf(path, sizeof path, "%s/%s,ffa", dir, leaf);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    unsigned char *img = malloc(ARMRUN_SHADOWTEST_SIZE);
    memcpy(img, armrun_shadowtest, ARMRUN_SHADOWTEST_SIZE);
    /* another title, as long as "ShadowTest": the title and the help's */
    for (size_t i = 0; title && i + 10 <= ARMRUN_SHADOWTEST_SIZE; i++)
        if (!memcmp(img + i, "ShadowTest", 10) && (img[i + 10] == 0 || img[i + 10] == 9) &&
            (i == 0 || img[i - 1] == 0))
            memcpy(img + i, title, 10);
    fwrite(img, 1, ARMRUN_SHADOWTEST_SIZE, f);
    fclose(f);
    free(img);
}

static unsigned number_of(const char *title)
{
    uint32_t r[10];
    return module(native_swi, 18, title, r) ? 0xFFFFFFFFu : r[1];
}

/* ---- the escape hatch, the column, the variable, 5.30's MimeMap ---------------- */

static struct ros_module *chain_module(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (!m->arm && !strcmp(m->title, title))
            return m;
    return NULL;
}

/* case 10: *ARMPrefer, with the twins of the services case (the shadow's workspace ws3) */
static void s4_prefer(const char *file, uint32_t ws3)
{
    uint32_t r[10];
    /* remembered with no shadow: no effect until one is loaded */
    cli_as(arm_swi, "RMKill ShadowTest");
    const os_error *e = cli("ARMPrefer ShadowTest on");
    uint32_t n0 = who(native_swi, NULL);
    const os_error *e2 = module(arm_swi, 1, file, r);
    uint32_t n1 = who(native_swi, NULL), a1 = who(arm_swi, NULL);
    char l1 = line_who(native_swi, "ShadowWho");
    uint32_t rn[10];
    const os_error *e18 = module(native_swi, 18, "ShadowTest", rn);
    check(!e && n0 == 'N' && !e2 && native.shadow && native.prefer_arm && n1 == 'A' && a1 == 'A' &&
              l1 == 'A' && !e18 && rn[3] == native.shadow->base,
          "shadow: *ARMPrefer ShadowTest on is remembered with no shadow (native Who N); the shadow "
          "loaded then is preferred -- native Who, *ShadowWho and OS_Module 18 reach it (case 10)",
          "%s; before the shadow %c; %s; native %c, ARM %c, *ShadowWho %c, 18 &%X (shadow &%X)",
          e ? e->errmess : "ok", n0, e2 ? e2->errmess : "loaded", n1, a1, l1, rn[3],
          native.shadow ? native.shadow->base : 0);

    cli("NModules");
    char nm[1024];
    lines_naming("ShadowTest", nm, sizeof nm);
    cli("ARMPrefer");
    int listed = strstr(out, "ShadowTest") != NULL;
    uint32_t ws = 0;
    who(arm_swi, &ws);
    unsigned b0 = ws ? ros_ld32(ws + 8) : 0, w0 = ws ? ros_ld32(ws + 16) : 0;
    uint32_t bn[10] = { 0x60, 0, 0 }, wn[10] = { 0 };
    const os_error *eb = native_swi(0x06u, bn);
    native_swi(0x00u, wn);
    unsigned b1 = ws ? ros_ld32(ws + 8) : 0, w1 = ws ? ros_ld32(ws + 16) : 0;
    check(strstr(nm, "ShadowTest (shadow, preferred)|") && listed && !eb && bn[1] == 'A' &&
              b1 == b0 + 1 && w1 == w0 + 1,
          "shadow: a preferred shadow -- *NModules says (shadow, preferred), *ARMPrefer lists it, a "
          "native OS_Byte &60's UKByte and a native OS_WriteC reach it (case 10)",
          "NModules [%s], listed %d, OS_Byte %s R1 %c (%u -> %u), WrchV %u -> %u (ws &%X, S3's &%X)",
          nm, listed, eb ? eb->errmess : "ok", bn[1], b0, b1, w0, w1, ws, ws3);

    e = cli("ARMPrefer ShadowTest off");
    uint32_t n2 = who(native_swi, NULL), a2 = who(arm_swi, NULL);
    cli("NModules");
    lines_naming("ShadowTest", nm, sizeof nm);
    e2 = cli("ARMPrefer SharedCLibrary on");
    char e2_text[96];
    snprintf(e2_text, sizeof e2_text, "%s", e2 ? e2->errmess : "accepted");
    cli("ARMPrefer");
    check(!e && n2 == 'N' && a2 == 'A' && !native.prefer_arm && strstr(nm, "ShadowTest (shadow)|") &&
              e2 && strstr(out, "No title"),
          "shadow: *ARMPrefer ShadowTest off -- native Who N again, ARM Who A, *NModules (shadow); "
          "*ARMPrefer SharedCLibrary refused (case 10)",
          "%s; native %c, ARM %c; NModules [%s]; SharedCLibrary: %s; list [%s]",
          e ? e->errmess : "ok", n2, a2, nm, e2_text, out);
}

/* boot/rom_contents.c's column, and ROSGD$Shadowable for a title not in
 * the ROM */
static void s4_policy(const char *file)
{
    static const struct { const char *title; int policy; } want[] = {
        { "MimeMap", ROS_SHADOW_ALLOW },  { "Squash", ROS_SHADOW_ALLOW },
        { "ZLib", ROS_SHADOW_ALLOW },     { "BootCommands", ROS_SHADOW_ALLOW },
        { "BASICVFP", ROS_SHADOW_ALLOW }, { "SharedCLibrary", ROS_SHADOW_BUILTIN },
        { "FPEmulator", ROS_SHADOW_NEVER }, { "FileSwitch", ROS_SHADOW_NEVER },
    };
    unsigned bad = 0;
    char first[96] = "";
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        struct ros_module *m = chain_module(want[i].title);
        if (!m || m->shadow_policy != want[i].policy) {
            if (!bad++)
                snprintf(first, sizeof first, "%s: %d, not %d", want[i].title,
                         m ? m->shadow_policy : -1, want[i].policy);
        }
    }
    check(!bad, "shadow: the ROM table's column -- MimeMap, Squash, ZLib, BootCommands, BASICVFP "
          "shadowable; SharedCLibrary built in; FPEmulator, FileSwitch never (S4)",
          "%u wrong; first %s", bad, first);

    /* the native ShadowTest out of the ROM, with no policy of its own */
    uint32_t r[10];
    cli("RMKill ShadowTest");                   /* the twin: its shadow promoted */
    cli("RMKill ShadowTest");                   /* and the promoted shadow */
    ros_module_unregister_rom(&native);
    native_init();
    native.shadow_policy = 0;
    cli("Unset ROSGD$Shadowable");
    const os_error *e = ros_module_add(&native, "");
    const os_error *e1 = module(arm_swi, 1, file, r);
    int absorbed = !e && !e1 && !native.shadow && who(arm_swi, NULL) == 'N';
    cli("Set ROSGD$Shadowable MimeMap, ShadowTest");
    const os_error *e2 = module(arm_swi, 1, file, r);
    int shadowed = !e2 && native.shadow && who(arm_swi, NULL) == 'A' && who(native_swi, NULL) == 'N';
    check(absorbed && shadowed,
          "shadow: a native module not in the ROM, with no policy of its own: an ARM load of its "
          "title is absorbed, until ROSGD$Shadowable lists it -- then it is a shadow (S4)",
          "%s / %s: absorbed %d; %s: shadowed %d", e ? e->errmess : "added",
          e1 ? e1->errmess : "ok", absorbed, e2 ? e2->errmess : "ok", shadowed);
    cli("Unset ROSGD$Shadowable");
}

/* MimeMap_Translate from a caller: 0 and the output (a type in *type,
 * else text), or the error's number */
static uint32_t mm_translate(caller *c, uint32_t from, const char *in, uint32_t to, uint32_t *type,
                             char *text, size_t max)
{
    static char *b;
    if (!b)
        b = ros_rma_alloc(512);
    strcpy(b, in);
    memset(b + 256, 0, 64);
    uint32_t r[10] = { from, ros_addr(b), to, ros_addr(b + 256) };
    const os_error *e = c(0x50B00u, r);
    if (e)
        return e->errnum;
    if (type)
        *type = r[3];
    if (text)
        snprintf(text, max, "%s", b + 256);
    return 0;
}

/* Three translations: the answers in one line, for comparing */
static void mm_answers(caller *c, char *line, size_t max)
{
    uint32_t png = 0, cal = 0;
    char html[64] = "", c_ext[64] = "";
    uint32_t e1 = mm_translate(c, 3, ".png", 0, &png, NULL, 0);
    uint32_t e2 = mm_translate(c, 3, ".html", 2, NULL, html, sizeof html);
    uint32_t e3 = mm_translate(c, 2, "text/calendar", 0, &cal, NULL, 0);
    uint32_t e4 = mm_translate(c, 3, ".c", 2, NULL, c_ext, sizeof c_ext);
    snprintf(line, max, "png &%X/%X html %s/%X calendar &%X/%X .c %s/%X", png, e1, html, e2, cal, e3,
             c_ext, e4);
}

static void var_value(const char *name, char *v, size_t max)
{
    static char *b;
    if (!b)
        b = ros_rma_alloc(320);
    snprintf(b, 64, "%s", name);
    uint32_t r[10] = { ros_addr(b), ros_addr(b + 64), 255, 0, 3 };
    v[0] = 0;
    if (!native_swi(0x23u, r))
        snprintf(v, max, "%.*s", (int)r[2], b + 64);
}

/* case 12: RISC OS 5.30's MimeMap as the native MimeMap's shadow */
static void s4_mimemap(void)
{
    struct ros_module *mm = chain_module("MimeMap");
    if (!mm) {
        check(0, "shadow: 5.30's MimeMap (case 12)", "no native MimeMap");
        return;
    }
    char vars0[256], vars1[256];
    var_value("Inet$MimeMappings", vars0, sizeof vars0);
    const os_error *e = cli("RMLoad Resources:$.Resources.!System.Modules.Shadows.MimeMap");
    char e_text[96];
    snprintf(e_text, sizeof e_text, "%s", e ? e->errmess : "loaded");
    struct ros_module *sh = mm->shadow;
    int routed = sh && sh->arm && ros_module_for_swi_kind(0x50B00u, 1) == sh &&
                 ros_module_for_swi_kind(0x50B00u, 0) == mm;
    char an[160], aa[160];
    mm_answers(native_swi, an, sizeof an);
    mm_answers(arm_swi, aa, sizeof aa);
    var_value("Inet$MimeMappings", vars1, sizeof vars1);
    check(!e && routed && !strcmp(an, aa) && strstr(an, "png &B60/0 html text/html/0") &&
              !strcmp(vars0, vars1),
          "shadow: RISC OS 5.30's MimeMap, ARM code, loaded as the native MimeMap's shadow -- ARM "
          "callers' MimeMap_Translate reaches 5.30's, native callers' the box's, with the same "
          "answers; Inet$MimeMappings unchanged (case 12)",
          "%s; routed %d; native [%s]; ARM [%s]; Inet$MimeMappings '%s' -> '%s'", e_text, routed, an,
          aa, vars0, vars1);

    e = cli("ARMPrefer MimeMap on");
    int pref = sh && mm->shadow == sh && ros_module_for_swi_kind(0x50B00u, 0) == sh;
    char ap[160];
    mm_answers(native_swi, ap, sizeof ap);
    uint32_t rn[10];
    const os_error *e18 = module(native_swi, 18, "MimeMap", rn);
    int found_530 = !e18 && sh && rn[3] == sh->base;
    const os_error *e2 = cli("ARMPrefer MimeMap off");
    int back = ros_module_for_swi_kind(0x50B00u, 0) == mm;
    uint32_t r18[10];
    module(native_swi, 18, "MimeMap", r18);
    check(!e && pref && !strcmp(ap, an) && found_530 && !e2 && back && r18[3] == mm->base,
          "shadow: *ARMPrefer MimeMap on sends native callers to 5.30's MimeMap (MimeMap_Translate, "
          "the same answers; OS_Module 18 its header); off sends them back to the box's (case 12)",
          "%s; preferred %d [%s]; 18 &%X (5.30's &%X); off %s, back %d, 18 &%X (box's &%X)",
          e ? e->errmess : "ok", pref, ap, rn[3], sh ? sh->base : 0, e2 ? e2->errmess : "ok", back,
          r18[3], mm->base);

    /* gone again: an ARM RMKill takes the shadow only */
    e = cli_as(arm_swi, "RMKill MimeMap");
    char after[160];
    mm_answers(native_swi, after, sizeof after);
    check(!e && !mm->shadow && chain_module("MimeMap") == mm && !strcmp(after, an),
          "shadow: an ARM RMKill MimeMap takes 5.30's shadow; the box's MimeMap answers as before "
          "(case 12)", "%s, shadow %p, [%s]", e ? e->errmess : "ok", (void *)mm->shadow, after);
}

/* case 14 (#162): RISC OS 5.30's ZLib from its disc, squeezed (modsqz),
 * at System:Modules.Shadows.ZLib.  Unsqueezed as 5.30's kernel does it
 * (75352 bytes; FNV-1a of ROOL's unmodsqz output with the finalise
 * offset's bit 31 kept, as the kernel keeps it); RMLoaded by ARM code, it
 * initialises and becomes the native ZLib's shadow: ARM code's
 * ZLib_Version answers from 5.30's image, *Help ZLib shows 0.05; then
 * OS_Module 11 (insert from memory) of the same file does the same */
static void s4_zlib(void)
{
    static const char path[] = "Resources:$.Resources.!System.Modules.Shadows.ZLib";
    struct ros_module *z = chain_module("ZLib");
    if (!z) {
        check(0, "shadow: 5.30's ZLib, squeezed (case 14)", "no native ZLib");
        return;
    }
    char *name = ros_rma_alloc(sizeof path);
    memcpy(name, path, sizeof path);
    uint32_t r[10] = { 5, ros_addr(name) };            /* OS_File 5: its size */
    const os_error *e = native_swi(0x08u, r);
    uint32_t fsize = e ? 0 : r[4];
    uint8_t *img = fsize ? ros_rma_alloc(fsize) : NULL;
    uint32_t l[10] = { 255, ros_addr(name), img ? ros_addr(img) : 0, 0 };
    int loaded = img && !native_swi(0x08u, l);
    uint32_t init0 = loaded ? ros_ld32(ros_addr(img) + 4) : 0, size = fsize, hash = 0x811C9DC5u;
    uint8_t *u = img;
    e = loaded ? ros_module_unsqueeze(&u, &size) : NULL;
    for (uint32_t i = 0; u && !e && i < size; i++)
        hash = (hash ^ u[i]) * 0x01000193u;
    check(loaded && (init0 & 0x80000000u) && !e && size == 75352 && hash == 0x0DC0A993u &&
              ros_ld32(ros_addr(u) + 4) == 0x2A4,
          "shadow: 5.30's squeezed ZLib unsqueezes as 5.30's kernel does it: 75352 bytes, the "
          "same as ROOL's unmodsqz gives, init &2A4 (case 14, #162)",
          "%s; file %u bytes, init &%X; %u bytes, FNV &%08X", e ? e->errmess : "ok", fsize, init0,
          size, hash);

    /* RMLoad by ARM code: the shadow */
    char cmd[96];
    snprintf(cmd, sizeof cmd, "RMLoad %s", path);
    e = cli_as(arm_swi, cmd);
    char e_text[96];
    snprintf(e_text, sizeof e_text, "%s", e ? e->errmess : "loaded");
    struct ros_module *sh = z->shadow;
    uint32_t va[10] = { 0 }, vn[10] = { 0 };
    const os_error *ea = arm_swi(0x53AC4u, va), *en = native_swi(0x53AC4u, vn);   /* ZLib_Version */
    int in530 = sh && va[0] >= sh->base && va[0] < sh->image_end;
    char vtext[24] = "";
    if (!ea && va[0])
        snprintf(vtext, sizeof vtext, "%s", (const char *)ros_ptr(va[0]));
    cli_as(arm_swi, "Help ZLib");
    int help = strstr(out, "0.05 (28 May 2022)") != NULL;
    check(!e && sh && sh->arm && ros_ld32(sh->base + 4) == 0x2A4 && !ea && in530 && vtext[0] == '1' &&
              !en && !(sh && vn[0] >= sh->base && vn[0] < sh->image_end) && help,
          "shadow: 5.30's squeezed ZLib, RMLoaded by ARM code, unsqueezes, initialises and becomes "
          "the native ZLib's shadow: ARM code's ZLib_Version answers from it (\"1...\"), native "
          "code's from the box's; *Help ZLib shows 0.05 (case 14, #162)",
          "%s; shadow %p; ARM version %s at &%X (in 5.30's: %d); native %s; help [%.80s]", e_text,
          (void *)sh, ea ? ea->errmess : vtext, va[0], in530, en ? en->errmess : "ok", out);

    /* RMClear leaves it: modsqz set its finalise offset's bit 31 */
    e = cli_as(arm_swi, "RMClear");
    int kept = z->shadow == sh && sh;
    e = cli_as(arm_swi, "RMKill ZLib");
    check(kept && !e && !z->shadow,
          "shadow: RMClear leaves 5.30's ZLib (its finalise offset's bit 31, which modsqz sets); "
          "RMKill takes it (case 14)", "kept %d; RMKill %s, shadow %p", kept, e ? e->errmess : "ok",
          (void *)z->shadow);

    /* OS_Module 11, the squeezed image from memory: the same */
    uint32_t r11[10] = { 11, loaded ? ros_addr(img) : 0, fsize };
    if (loaded && u != img) {                   /* (unsqueeze freed the file's block) */
        img = ros_rma_alloc(fsize);
        native_swi(0x08u, (uint32_t[10]){ 255, ros_addr(name), ros_addr(img), 0 });
        r11[1] = ros_addr(img);
    }
    e = arm_swi(0x1Eu, r11);
    sh = z->shadow;
    check(!e && sh && ros_ld32(sh->base + 4) == 0x2A4 && sh->base != r11[1],
          "shadow: OS_Module 11 of the squeezed ZLib from memory unsqueezes it into the RMA too: "
          "the native ZLib's shadow (case 14)", "%s; shadow %p", e ? e->errmess : "ok", (void *)sh);
    cli_as(arm_swi, "RMKill ZLib");
    if (u && u != img)
        ros_rma_free(u);
    if (img)
        ros_rma_free(img);
    ros_rma_free(name);
}

void ros_selftest_shadow(void)
{
    const char *t = getpid() == 1 ? "/tmp" : getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rosgd-shadow-%d", t ? t : "/tmp", (int)getpid());
    mkdir(dir, 0755);
    stage("ShadowTest", NULL);
    stage("FPEm", "FPEmulator");
    ros_hostfs_mount("ShadowT", dir);
    void *tb = ros_rma_alloc(64);
    tramp = (ros_addr(tb) + 15) & ~15u;
    native_init();
    const char *file = "HostFS::ShadowT.$.ShadowTest";
    uint32_t r[10], ws0 = 0, ws1 = 0, ws2 = 0;
    const os_error *e;

    check(!ros_module_for_swi(CHUNK), "shadow: ShadowTest's chunk is free", "&%X taken", CHUNK);

    /* ---- case 6: ARM first, then the native one joins ---- */
    e = module(native_swi, 1, file, r);
    uint32_t n_a = who(native_swi, &ws0), a_a = who(arm_swi, NULL);
    uint32_t gv_armonly = ws0 ? ros_ld32(ws0 + 20) : 0;  /* its GraphicsV claim (case 8) */
    check(!e && n_a == 'A' && a_a == 'A' && ws0 && ros_ld32(ws0) == WS_ARM,
          "shadow: an ARM ShadowTest with no native one is ARM-only, in the chain: native and ARM "
          "callers both reach it (case 6)", "%s, native %c, ARM %c, ws &%X", e ? e->errmess : "loaded",
          n_a, a_a, ws0);
    e = ros_module_add(&native, "");
    uint32_t n_b = who(native_swi, NULL), a_b = who(arm_swi, &ws1);
    check(!e && n_b == 'N' && a_b == 'A' && ws1 == ws0 && ros_ld32(ws1) == WS_ARM && native.shadow &&
              native.shadow->twin == &native && native.shadow->arm,
          "shadow: the native ShadowTest joining demotes the ARM one to its shadow, its workspace "
          "kept: native Who gives N, ARM Who A (case 6)",
          "%s, native %c, ARM %c, ws &%X was &%X, shadow %p", e ? e->errmess : "added", n_b, a_b,
          ws1, ws0, (void *)native.shadow);

    /* ---- case 4: ARM RMKill takes the shadow only ---- */
    e = module(arm_swi, 4, "ShadowTest", r);
    uint32_t a_c = who(arm_swi, NULL), n_c = who(native_swi, NULL);
    const os_error *e2 = module(arm_swi, 4, "ShadowTest", r);
    uint32_t n_d = who(native_swi, NULL);
    check(!e && !native.shadow && a_c == 'N' && n_c == 'N' && !e2 && n_d == 'N' &&
              number_of("ShadowTest") != 0xFFFFFFFFu,
          "shadow: an ARM RMKill (OS_Module 4) removes the shadow only; ARM Who then reaches the "
          "native module; a second ARM RMKill is quiet and leaves it (case 4, SH-f)",
          "%s / %s, shadow %p, ARM %c, native %c then %c", e ? e->errmess : "ok",
          e2 ? e2->errmess : "ok", (void *)native.shadow, a_c, n_c, n_d);

    /* ---- case 1: ARM code loads the ARM one over the native one ---- */
    cli("Modules");
    char before[1024];
    unsigned mods_before = lines_naming("ShadowTest", before, sizeof before);
    e = module(arm_swi, 1, file, r);
    cli("NModules");
    char nm[1024];
    unsigned nlines = lines_naming("ShadowTest", nm, sizeof nm);
    /* the twin's line, numbered, N; then the shadow's, unnumbered, A */
    char *bar = strchr(nm, '|');
    int twin_first = bar && nm[2] != ' ' && strstr(nm, "  N  ") && strstr(nm, "  N  ") < bar &&
                     !strncmp(bar + 1, "     A  ", 8) && !strcmp(strchr(bar + 1, '|') ?
                     strchr(bar + 1, '|') - 19 : "", "ShadowTest (shadow)|");
    cli("Modules");
    char after[1024];
    unsigned mods_after = lines_naming("ShadowTest", after, sizeof after);
    check(!e && native.shadow && nlines == 2 && twin_first && mods_before == 1 && mods_after == 1 &&
              !strcmp(before, after),
          "shadow: ARM code's RMLoad over the native ShadowTest makes a shadow: *NModules shows it "
          "unnumbered after its twin, A and (shadow); *Modules is unchanged (case 1)",
          "%s; NModules %u [%s]; Modules [%s] -> [%s]", e ? e->errmess : "loaded", nlines, nm,
          before, after);

    /* ---- case 2: each kind reaches its own ---- */
    uint32_t a_w = who(arm_swi, &ws2), n_w = who(native_swi, NULL);
    uint32_t a_cw = who_callaswi(arm_swi), n_cw = who_callaswi(native_swi);
    check(a_w == 'A' && n_w == 'N' && a_cw == 'A' && n_cw == 'N' && ws2 && ros_ld32(ws2) == WS_ARM,
          "shadow: with both loaded, ARM code's ShadowTest_Who gives A and native code's N; the same "
          "through OS_CallASWI (case 2)", "Who ARM %c native %c; CallASWI ARM %c native %c", a_w, n_w,
          a_cw, n_cw);
    uint32_t ra[10], rn[10];
    const os_error *ea = module(arm_swi, 18, "ShadowTest", ra), *en = module(native_swi, 18, "ShadowTest", rn);
    unsigned num = rn[1];
    uint32_t a_en = enumerated(arm_swi, num), n_en = enumerated(native_swi, num);
    uint32_t sh_base = native.shadow ? native.shadow->base : 0;
    uint32_t ws18 = !ea && ra[4] ? ros_ld32(ra[4]) : 0;
    check(!ea && !en && sh_base && ra[3] == sh_base && rn[3] == native.base && ra[1] == rn[1] &&
              ws18 == WS_ARM && a_en == sh_base && n_en == native.base,
          "shadow: OS_Module 18 and 12 by caller kind -- ARM code gets the shadow's header (and its "
          "twin's module number), native code the native module's (case 2)",
          "18: %s ARM &%X #%u ws &%X (&%X), native &%X #%u (shadow &%X, native &%X); 12: ARM &%X native &%X",
          ea ? ea->errmess : "ok", ra[3], ra[1], ra[4], ws18, rn[3], rn[1], sh_base, native.base, a_en, n_en);
    uint32_t rx[10] = { 0 }, ry[10] = { 0 };
    const os_error *xa = arm_swi(EXTRA, rx), *xn = native_swi(EXTRA, ry);
    check(!xa && rx[0] == 'X' && xn && strstr(xn->errmess, "not known"),
          "shadow: the shadow's own SWI (Extra) reaches ARM callers only; native code gets the "
          "native module's \"not known\" (SH-d, strict)", "ARM %s %c; native %s",
          xa ? xa->errmess : "ok", rx[0], xn ? xn->errmess : "ok");

    /* #149: SWI names by kind */
    uint32_t fa = swi_from(arm_swi, "ShadowTest_Extra"), fn = swi_from(native_swi, "ShadowTest_Extra");
    uint32_t wa1 = swi_from(arm_swi, "XShadowTest_Who"), wn1 = swi_from(native_swi, "XShadowTest_Who");
    char ta[64], tn[64];
    swi_to(arm_swi, EXTRA, ta, sizeof ta);
    swi_to(native_swi, EXTRA, tn, sizeof tn);
    check(fa == EXTRA && fn == 0xFFFFFFFFu && wa1 == (WHO | X) && wn1 == (WHO | X) &&
              !strcmp(ta, "ShadowTest_Extra") && !strcmp(tn, "ShadowTest_1"),
          "shadow: SWI names by caller kind -- ARM code converts ShadowTest_Extra, the shadow's "
          "own, both ways; native code does not know it (#149)",
          "from: ARM &%X native &%X, Who &%X &%X; to: ARM %s, native %s", fa, fn, wa1, wn1, ta, tn);

    /* ---- the native twin killed, the shadow promoted ---- */
    unsigned pos = number_of("ShadowTest");
    e = module(native_swi, 4, "ShadowTest", r);
    uint32_t n_p = who(native_swi, NULL);
    unsigned pos2 = number_of("ShadowTest");
    e2 = module(native_swi, 4, "ShadowTest", r);
    uint32_t gone = who(native_swi, NULL);
    check(!e && n_p == 'A' && pos2 == pos && !e2 && !gone && !ros_module_for_swi(CHUNK),
          "shadow: a native RMKill of the twin promotes the shadow into its place -- native Who gives "
          "A -- and a second kills it (SH-c)", "%s, native %c, #%u then #%u; %s, then %c",
          e ? e->errmess : "ok", n_p, pos, pos2, e2 ? e2->errmess : "ok", gone);

    /* ---- case 11: a never-shadowed title is absorbed ---- */
    module(native_swi, 18, "FPEmulator", rn);
    uint32_t fp_base = rn[3];
    cli("NModules");
    char nm1[sizeof out];
    strcpy(nm1, out);
    e = module(arm_swi, 1, "HostFS::ShadowT.$.FPEm", r);
    cli("NModules");
    module(native_swi, 18, "FPEmulator", rn);
    check(!e && !strcmp(nm1, out) && rn[3] == fp_base && !ros_module_for_swi(CHUNK),
          "shadow: an ARM RMLoad of FPEmulator, never shadowed, is absorbed: no error, *NModules "
          "unchanged, the native FPEmulator still the one (case 11)", "%s; FPEmulator &%X was &%X",
          e ? e->errmess : "absorbed", rn[3], fp_base);

    /* ---- case 13: SharedCLibrary's built-in shadow ---- */
    struct ros_module *clib = NULL;
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (!strcmp(m->title, "SharedCLibrary"))
            clib = m;
    int fresh = clib && !clib->shadow;
    ea = module(arm_swi, 18, "SharedCLibrary", ra);
    en = module(native_swi, 18, "SharedCLibrary", rn);
    cli("NModules");
    char cl[1024];
    unsigned cn = lines_naming("SharedCLibrary", cl, sizeof cl);
    check(clib && !ea && !en && clib->shadow && ra[3] == clib->shadow->base && rn[3] == clib->base &&
              cn == 2 && strstr(cl, "SharedCLibrary (shadow)|"),
          "shadow: SharedCLibrary's built-in shadow (5.30's) made by an ARM OS_Module lookup, on a "
          "task for calls only; *NModules shows it after the native one (case 13)",
          "fresh %d, %s / %s, ARM &%X native &%X, NModules [%s]", fresh, ea ? ea->errmess : "ok",
          en ? en->errmess : "ok", ra[3], rn[3], cl);
    /* its SWIs: the chunk routed by kind, and an ARM SWI in it (offset &3F,
     * which neither has) answered by 5.30's handler under the engine */
    uint32_t rs[10] = { 0 }, rt[10] = { 0 };
    int routed = clib && clib->shadow && ros_module_for_swi_kind(0x806BFu, 1) == clib->shadow &&
                 ros_module_for_swi_kind(0x806BFu, 0) == clib;
    const os_error *sa = arm_swi(0x806BFu, rs), *sn = native_swi(0x806BFu, rt);
    char sa_text[128];
    snprintf(sa_text, sizeof sa_text, "%s", sa ? sa->errmess : "no error");
    check(routed && sa && sn,
          "shadow: SharedCLibrary's SWI chunk routed by kind -- ARM code's SWIs to the shadow's "
          "handler (5.30's, under the engine), native code's to the native module (case 13)",
          "routed %d; ARM: %s; native: %s", routed, sa_text, sn ? sn->errmess : "no error");

    /* ---- *commands and the RM* commands by the OS_CLI's kind ---- */
    native_init();
    native.commands = st_cmds;
    ros_module_register_rom(&native);
    e = ros_module_add(&native, "");
    e2 = module(arm_swi, 1, file, r);
    check(!e && !e2 && native.shadow, "shadow: S2's twins -- the native ShadowTest a ROM module, "
          "the ARM one its shadow", "%s / %s", e ? e->errmess : "ok", e2 ? e2->errmess : "ok");
    {
        char obey[320];
        snprintf(obey, sizeof obey, "%s/WhoObey,feb", dir);
        FILE *f = fopen(obey, "w");
        if (f) {
            fputs("ShadowWho\n", f);
            fclose(f);
        }
    }
    const char *obeyfile = "HostFS::ShadowT.$.WhoObey";
    char obeyline[96];
    snprintf(obeyline, sizeof obeyline, "Obey %s", obeyfile);
    cli("Set Alias$SWho ShadowWho");

    /* case 2: *ShadowWho, and the ways a line reaches it */
    char wa = line_who(arm_swi, "ShadowWho"), wn = line_who(native_swi, "ShadowWho");
    char pa = line_who(arm_swi, "ShadowTest:ShadowWho"), pn = line_who(native_swi, "ShadowTest:ShadowWho");
    char aa = line_who(arm_swi, "SWho"), an = line_who(native_swi, "SWho");
    char oa = line_who(arm_swi, obeyline), on = line_who(native_swi, obeyline);
    char ra2 = line_who(arm_swi, obeyfile), rn2 = line_who(native_swi, obeyfile);
    check(wa == 'A' && wn == 'N' && pa == 'A' && pn == 'N' && aa == 'A' && an == 'N' && oa == 'A' &&
              on == 'N' && ra2 == 'A' && rn2 == 'N',
          "shadow: *ShadowWho by the OS_CLI's kind -- ARM code's line reaches the shadow's command, "
          "native code's the native one's; the same through Title:, an alias's line, *Obey's line "
          "and an Obey file run as a command (case 2, S2)",
          "ShadowWho %c/%c, Title: %c/%c, alias %c/%c, Obey %c/%c, run %c/%c", wa, wn, pa, pn, aa,
          an, oa, on, ra2, rn2);
    e = cli_as(arm_swi, "Help ShadowTest");
    int ha = !e && strstr(out, "ARM") && !strstr(out, "native");
    e2 = cli("Help ShadowTest");
    int hn = !e2 && strstr(out, "native") && !strstr(out, "ARM");
    check(ha && hn, "shadow: *Help ShadowTest by kind -- ARM code's describes the shadow, native "
          "code's the native module (S2)", "ARM %d, native %d [%s]", ha, hn, out);

    /* case 3: RMEnsure */
    const os_error *ea1 = cli_as(arm_swi, "RMEnsure ShadowTest 1.00");
    const os_error *en05 = cli("RMEnsure ShadowTest 0.50");
    const os_error *en1 = cli("RMEnsure ShadowTest 1.00");
    const os_error *en2 = cli("RMEnsure ShadowTest 2.00");
    int en2_err = en2 != NULL;
    const os_error *ea2 = cli_as(arm_swi, "RMEnsure ShadowTest 2.00");
    int ea2_err = ea2 != NULL;
    char rca = line_who(arm_swi, "RMEnsure ShadowTest 2.00 ShadowWho");
    char rcn = line_who(native_swi, "RMEnsure ShadowTest 2.00 ShadowWho");
    check(!ea1 && !en05 && !en1 && en2_err && ea2_err && rca == 'A' && rcn == 'N',
          "shadow: RMEnsure by kind -- ARM code's 1.00 met by the shadow; native 0.50 by its own, "
          "1.00 by the newer shadow (SH-e), 2.00 by neither; the command runs as the caller's "
          "kind (case 3)", "ARM 1.00 %d 2.00 %d; native 0.50 %d 1.00 %d 2.00 %d; command %c/%c",
          !ea1, !ea2_err, !en05, !en1, !en2_err, rca, rcn);

    /* case 4: *RMKill from ARM code */
    e = cli_as(arm_swi, "RMKill ShadowTest");
    int k1 = !e && !native.shadow && who(native_swi, NULL) == 'N' && who(arm_swi, NULL) == 'N';
    e2 = cli_as(arm_swi, "RMKill ShadowTest");
    int k2 = !e2 && who(native_swi, NULL) == 'N' && number_of("ShadowTest") != 0xFFFFFFFFu;
    const os_error *en1b = cli("RMEnsure ShadowTest 1.00");
    check(k1 && k2 && en1b, "shadow: *RMKill from ARM code takes the shadow only, and a second is "
          "quiet (the OS_Module inherits the OS_CLI's kind, case 4); then a native RMEnsure 1.00 "
          "fails, the native module 0.50 (case 3)", "first %d %s, second %d %s, RMEnsure %s", k1,
          e ? e->errmess : "ok", k2, e2 ? e2->errmess : "ok", en1b ? en1b->errmess : "met");

    /* case 5: promotion, and the ROM restart that demotes */
    uint32_t wsa = 0, wsp = 0, wsd = 0;
    e = cli_as(arm_swi, "RMLoad HostFS::ShadowT.$.ShadowTest");
    who(arm_swi, &wsa);
    e2 = cli("RMKill ShadowTest");
    uint32_t np = who(native_swi, &wsp);
    int promoted = !e && !e2 && np == 'A' && wsp == wsa && !native.shadow;
    const os_error *e3 = cli("RMReInit ShadowTest");
    uint32_t nd = who(native_swi, NULL), ad = who(arm_swi, &wsd);
    check(promoted && !e3 && nd == 'N' && ad == 'A' && wsd == wsa && native.shadow &&
              ros_ld32(wsd) == WS_ARM,
          "shadow: a native *RMKill promotes the shadow (native Who A); a native *RMReInit starts "
          "the ROM module, which demotes the ARM one again: native Who N, ARM Who A, its "
          "workspace the same (case 5, SH-c)",
          "promoted %d (Who %c, ws &%X was &%X); %s; native %c, ARM %c, ws &%X", promoted, np, wsp,
          wsa, e3 ? e3->errmess : "ok", nd, ad, wsd);

    /* RMClear from ARM code: the ARM modules only */
    e = cli_as(arm_swi, "RMClear");
    check(!e && !native.shadow && who(native_swi, NULL) == 'N' && who(arm_swi, NULL) == 'N',
          "shadow: *RMClear from ARM code kills the shadow and leaves the native module (S2)",
          "%s, shadow %p", e ? e->errmess : "ok", (void *)native.shadow);

    /* ---- services, vectors, variables and ResourceFS ---- */
    cli("Set ShadowTest$Var N");
    cli("Unset ShadowTest$New");
    unsigned pi0 = n_postinit_other;
    /* case 12's native claimants, on before the shadow's */
    ros_vector_claim_native(ROS_UPCALLV, st_upcallv, 0);
    ros_vector_claim_native(ROS_EVENTV, st_eventv, 0);
    e = module(arm_swi, 1, file, r);
    uint32_t ws3 = 0;
    who(arm_swi, &ws3);
    check(!e && native.shadow && ws3 && ros_ld32(ws3) == WS_ARM, "shadow: S3's twins -- the ARM "
          "ShadowTest the native one's shadow again", "%s, shadow %p", e ? e->errmess : "ok",
          (void *)native.shadow);

    /* case 7: services */
    if (ws3) {
        unsigned pi_own = ros_ld32(ws3 + 12);
        check(n_postinit_other == pi0 && pi_own >= 1,
              "shadow: the shadow's own Service_ModulePostInit goes to shadows only -- it saw "
              "its own, the native observer did not (case 7)", "native saw %u (was %u), shadow %u",
              n_postinit_other, pi0, pi_own);

        uint32_t rm[10] = { 1 };                        /* OS_ScreenMode 1: the mode */
        native_swi(0x65u, rm);
        unsigned nm0 = n_modechange, am0 = ros_ld32(ws3 + 4);
        uint32_t sv[10] = { 0, 0x46u, rm[1], 0xFFFFFFFFu };
        native_swi(0x30u, sv);                          /* Service_ModeChange */
        unsigned nm1 = n_modechange, am1 = ros_ld32(ws3 + 4);
        check(nm1 == nm0 + 1 && am1 == am0 + 1 && sv[1] == 0x46u,
              "shadow: Service_ModeChange (notify) reaches the native module and the shadow "
              "(case 7)", "native %u -> %u, shadow %u -> %u, R1 &%X", nm0, nm1, am0, am1, sv[1]);

        unsigned nb0 = n_ukbyte, ab0 = ros_ld32(ws3 + 8);
        uint32_t bn[10] = { 0x60, 0, 0 }, ba[10] = { 0x60, 0, 0 };
        const os_error *ebn = native_swi(0x06u, bn);
        unsigned nb1 = n_ukbyte, ab1 = ros_ld32(ws3 + 8);
        const os_error *eba = arm_swi(0x06u, ba);
        unsigned nb2 = n_ukbyte, ab2 = ros_ld32(ws3 + 8);
        char ebn_text[64];
        snprintf(ebn_text, sizeof ebn_text, "%s", ebn ? ebn->errmess : "no error");
        check(ebn && nb1 == nb0 + 1 && ab1 == ab0 && !eba && ba[1] == 'A' && nb2 == nb1 + 1 &&
                  ab2 == ab1 + 1,
              "shadow: Service_UKByte (provide) -- a native OS_Byte &60 is not offered to the "
              "shadow (Bad command); an ARM one is, after the native module declines, and the "
              "shadow's claim answers it (case 7)",
              "native: %s, counts native %u->%u shadow %u->%u; ARM: %s R1 %c, native %u shadow %u",
              ebn_text, nb0, nb1, ab0, ab1, eba ? eba->errmess : "ok", ba[1], nb2, ab2);

        /* case 8: vectors */
        unsigned w0 = ros_ld32(ws3 + 16);
        uint32_t wn[10] = { 0 }, wa[10] = { 0 };       /* VDU 0: nothing */
        native_swi(0x00u, wn);
        unsigned w1 = ros_ld32(ws3 + 16);
        arm_swi(0x00u, wa);
        unsigned w2 = ros_ld32(ws3 + 16);
        check(w1 == w0 && w2 == w1 + 1,
              "shadow: the shadow's WrchV claim -- a native OS_WriteC steps over it, an ARM "
              "OS_WriteC reaches it (case 8)", "count %u, native -> %u, ARM -> %u", w0, w1, w2);
        unsigned gv = ros_ld32(ws3 + 20);
        check(gv == 1 && gv_armonly == 2,
              "shadow: a shadow's OS_Claim of GraphicsV is refused; an ARM-only module's was "
              "not (case 8)", "shadow %u, ARM-only %u (1 refused, 2 allowed)", gv, gv_armonly);

        /* case 12: the notify vectors, shadows after the natives */
        unsigned u[4], un[3];
        uint32_t ur[2] = { 0 };
        u[0] = ros_ld32(ws3 + 24), un[0] = n_upcall;
        caller *by[2] = { native_swi, arm_swi };
        for (int i = 0; i < 2; i++) {
            uint32_t ru[10] = { UPC_PASS };
            ros_st32(ws3 + 28, 0);
            by[i](0x33u, ru);                           /* OS_UpCall */
            u[i + 1] = ros_ld32(ws3 + 24);
            ur[i] = ros_ld32(ws3 + 28);
        }
        un[1] = n_upcall;
        uint32_t rc[10] = { UPC_CLAIM };
        native_swi(0x33u, rc);
        u[3] = ros_ld32(ws3 + 24), un[2] = n_upcall;
        check(u[1] == u[0] + 1 && u[2] == u[1] + 1 && ur[0] == 'N' && ur[1] == 'N' &&
                  un[1] == un[0] + 2 && un[2] == un[1] + 1 && u[3] == u[2],
              "shadow: the shadow's UpCallV claim runs for a native and an ARM OS_UpCall, after "
              "the native claimant (R8 \"N\") though claimed after it; not when the native "
              "claimant claims (case 12)", "shadow %u %u %u %u, R8 &%X &%X; native %u %u %u",
              u[0], u[1], u[2], u[3], ur[0], ur[1], un[0], un[1], un[2]);

        uint32_t eb[10] = { 14, EV_USER };
        native_swi(0x06u, eb);                          /* OS_Byte 14: enable the event */
        unsigned v0 = ros_ld32(ws3 + 32), vn0 = n_event;
        ros_st32(ws3 + 36, 0);
        uint32_t ge[10] = { EV_USER, UPC_PASS };
        arm_swi(0x22u, ge);                             /* OS_GenerateEvent */
        unsigned v1 = ros_ld32(ws3 + 32), vr = ros_ld32(ws3 + 36);
        uint32_t gc[10] = { EV_USER, UPC_CLAIM };
        native_swi(0x22u, gc);
        unsigned v2 = ros_ld32(ws3 + 32), vn2 = n_event;
        eb[0] = 13, eb[1] = EV_USER;
        native_swi(0x06u, eb);                          /* OS_Byte 13: disable it */
        check(v1 == v0 + 1 && vr == 'N' && v2 == v1 && vn2 == vn0 + 2,
              "shadow: the shadow's EventV claim runs after the native claimant for an event, and "
              "not when the native one claims (case 12)", "shadow %u %u %u, R8 &%X; native %u -> %u",
              v0, v1, v2, vr, vn0, vn2);

        unsigned t0 = ros_ld32(ws3 + 40);
        ROS_BLOCKING(usleep(100000));
        unsigned t1 = ros_ld32(ws3 + 40);
        check(t1 - t0 >= 5 && t1 - t0 <= 15,
              "shadow: the shadow's TickerV claim is called by the ticker in the background, "
              "about ten times in 100 ms (case 12)", "%u -> %u", t0, t1);
    }
    ros_vector_release_native(ROS_UPCALLV, st_upcallv, 0);
    ros_vector_release_native(ROS_EVENTV, st_eventv, 0);

    /* case 9: variables and ResourceFS */
    {
        static char *vb;
        if (!vb)
            vb = ros_rma_alloc(96);
        char got[3][16];
        const char *names[3] = { "ShadowTest$Var", "ShadowTest$New", "ShadowTest$Var" };
        for (int i = 0; i < 3; i++) {
            if (i == 2) {                               /* after init, ARM code's write is made */
                strcpy(vb, "ShadowTest$Var");
                strcpy(vb + 32, "B");
                uint32_t sv[10] = { ros_addr(vb), ros_addr(vb + 32), 1, 0, 4 };
                arm_swi(0x24u, sv);
            }
            strcpy(vb, names[i]);
            uint32_t rv[10] = { ros_addr(vb), ros_addr(vb + 32), 32, 0, 3 };
            const os_error *ev = native_swi(0x23u, rv);
            snprintf(got[i], sizeof got[i], "%.*s", ev ? 1 : (int)rv[2], ev ? "-" : vb + 32);
        }
        check(!strcmp(got[0], "N") && !strcmp(got[1], "A") && !strcmp(got[2], "B"),
              "shadow: while the shadow initialised, ShadowTest$Var kept the native module's "
              "value and ShadowTest$New was made; afterwards ARM code's write is made (case 9, "
              "SH-g)", "Var %s, New %s, after an ARM write %s", got[0], got[1], got[2]);
        cli("Set ShadowTest$Var N");
        uint32_t msg = ros_resourcefs_find("Resources:$.Resources.ShadowTest.Messages");
        uint32_t only = ros_resourcefs_find("Resources:$.Resources.ShadowTest.ARMOnly");
        char m1[16] = "", m2[16] = "";
        if (msg)
            snprintf(m1, sizeof m1, "%.*s", (int)(ros_ld32(msg) - 5), (char *)ros_ptr(msg + 4));
        if (only)
            snprintf(m2, sizeof m2, "%.*s", (int)(ros_ld32(only) - 5), (char *)ros_ptr(only + 4));
        check(!strcmp(m1, "Who:N") && !strcmp(m2, "Only:A"),
              "shadow: the shadow's ResourceFS files go under the native module's -- its "
              "Messages do not hide the native one's; its new file is there (case 9, SH-g)",
              "Messages \"%s\", ARMOnly \"%s\"", m1, m2);
    }

    s4_prefer(file, ws3);
    s4_policy(file);
    s4_mimemap();
    s4_zlib();

    cli("Unset ShadowTest$Var");
    cli("Unset ShadowTest$New");
    cli("Unset Alias$SWho");
    cli("Unset ROSGD$Shadowable");
    cli("RMKill ShadowTest");
    cli("RMKill ShadowTest");
    ros_module_unregister_rom(&native);
    ros_rma_free(tb);
    scratch_gone("ShadowT", dir);
}
