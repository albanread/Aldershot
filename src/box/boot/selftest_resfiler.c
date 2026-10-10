/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_resfiler.c: the Resource Filer and Free, compiled from ObjAsm
 * into the ROM (FileSys/ResourceFS/ResFiler, Desktop/Free): what holds
 * before a desktop starts them as tasks. Their tasks are the Apps icon, its
 * help and clicks, and Free started as the Desktop starts it. They are
 * tests/desktop/resfiler's, against RISC OS 5.30 on the farm. Also checked
 * is what the Apps icon opens, Resources:$.Apps: the ROM's applications
 * (tools/mkromapps.py), whose running is tests/desktop/romapps's.
 *
 * Free's workspace: +0 its task (0 none, -1 asked to start), +4 its
 * windows, +8 its poll word, +16 its filing systems, which are blocks of
 * next, previous, number, entry, R12. A window block: +8 its window, +12 its
 * filing system, +40 -> its device, +44 its update flags. The Resource
 * Filer's: +0 its task, +4 its Filer's, +8 -> its private word, +12 its
 * messages, +72 its menu.
 */
#include <stdio.h>
#include <string.h>

#include "fileswitch.h"
#include "modwrap_draw.h"
#include "modwrap_fileract.h"
#include "modwrap_paint.h"
#include "resourcefs.h"
#include "romapps_stamps.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define XFREE_REGISTER 0x644C0u
#define XFREE_DEREGISTER 0x644C1u
#define TEST_FS 0xC8u                       /* a filing system number nothing has */

static uint32_t scratch;                    /* arena: strings in, results out */
static const os_error *last;

static struct ros_module *module(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (m->title && strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

static uint32_t workspace(const char *title)
{
    struct ros_module *m = module(title);
    return m && m->private_word ? ros_ld32(m->private_word) : 0;
}

/* The probes' checksum (tests/desktop/drag/modules.bas): each word, the
 * total rotated left one bit first */
static uint32_t checksum(uint32_t base, uint32_t size)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 4 <= size; i += 4)
        sum = (sum << 1 | sum >> 31) ^ ros_ld32(base + i);
    return sum;
}

static int swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 10 * sizeof r[0]);
    last = s.v ? ros_ptr(s.r[0]) : NULL;
    return s.v;
}

static int quiet(struct ros_cpu *s, uint32_t r12)
{
    (void)s, (void)r12;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI, printing nothing: 0 or the error number */
static uint32_t cli(const char *cmd)
{
    strcpy(ros_ptr(scratch), cmd);
    uint32_t r[10] = { scratch };
    ros_vector_claim_native(WRCHV, quiet, 0);
    int v = swi(XOS_CLI, r);
    ros_vector_release_native(WRCHV, quiet, 0);
    return v ? last->errnum : 0;
}

/* OS_CLI, its output kept (WrchV claimed): 0 or the error number */
static char captured[512];
static size_t ncaptured;

static int capture(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (ncaptured < sizeof captured - 1)
        captured[ncaptured++] = (char)s->r[0];
    return ROS_VECTOR_CLAIM;
}

static uint32_t cli_output(const char *cmd)
{
    strcpy(ros_ptr(scratch), cmd);
    uint32_t r[10] = { scratch };
    ncaptured = 0;
    ros_vector_claim_native(WRCHV, capture, 0);
    int v = swi(XOS_CLI, r);
    ros_vector_release_native(WRCHV, capture, 0);
    captured[ncaptured] = 0;
    return v ? last->errnum : 0;
}

/* Text's words, one space apart: what is left of OS_PrettyPrint's output
 * whatever the window's width (its line breaks, VDU 14 and 15, CR, LF) */
static void words(const char *text, char *out, size_t size)
{
    size_t n = 0;
    for (const char *p = text; *p && n < size - 1; p++)
        if ((uint8_t)*p > ' ')
            out[n++] = *p;
        else if (n && out[n - 1] != ' ')
            out[n++] = ' ';
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static const char *last_error(void)
{
    return last ? last->errmess : "";
}

/* A service call to one module alone, through its service entry: the Task
 * Manager and the Filer, before Free in the ROM, claim Service_StartWimp
 * too while they are not running as tasks, and HostFSFiler claims
 * Service_StartFiler as the Resource Filer does */
static void service(const char *title, struct ros_cpu *s)
{
    struct ros_module *m = module(title);
    uint32_t depth = ros_call_depth;
    ros_call_depth = depth + 1;
    s->r[12] = m->private_word;
    s->r[14] = ROS_RETURN_TO_NATIVE;
    ros_call(s, m->service_addr);
    ros_call_depth = depth;
}

/* A filing system's entry for Free, native. Free calls it with the
 * return address pushed (CallEntry: Push "PC", LDR PC, [r14, #fs_entry]),
 * and it returns by popping it, as Free's own do. ComparePath (3) is
 * noted, and Z is set because the file is on the device. */
static struct {
    unsigned calls;
    uint32_t r[4], r12, pushed;
} entered;

static void fs_entry(struct ros_cpu *s)
{
    entered.calls++;
    memcpy(entered.r, s->r, sizeof entered.r);
    entered.r12 = s->r[12];
    entered.pushed = ros_ld32(s->r[13] + 4);    /* CallEntry's R12, under the return */
    if (s->r[0] == 3)
        s->z = 1, s->v = 0;
    s->r[15] = ros_ld32(s->r[13]);
    s->r[13] += 4;
}

/* Free's list of filing systems: their numbers, in order */
static unsigned fs_list(uint32_t w, uint32_t *numbers, unsigned max)
{
    unsigned n = 0;
    for (uint32_t p = ros_ld32(w + 16); p && n < max; p = ros_ld32(p))
        numbers[n++] = ros_ld32(p + 8);
    return n;
}

static int path_is(const char *var, const char *want)
{
    strcpy(ros_ptr(scratch), var);
    uint32_t r[10] = { scratch, scratch + 64, 128, 0, 3 };
    return !swi(XOS_ReadVarVal, r) && r[2] == strlen(want) &&
           memcmp(ros_ptr(scratch + 64), want, r[2]) == 0;
}

static void test_free(void)
{
    struct ros_module *m = module("Free");
    uint32_t w = workspace("Free");
    check(m && m->base == 0xFCC80000u && ros_module_version(m) == 0x4200 && w &&
              m->swi_chunk == 0x444C0u && checksum(m->base, 6128) == 0x52940185u,
          "Free 0.42 in the ROM at &FCC80000: its workspace, SWIs at &444C0; its image "
          "RISC OS 5.30's, byte for byte", NULL);
    if (!m || !w)
        return;

    /* Free's own six, and HostFS's before them. A filing system registers
     * as it starts. HostFS, in FileSwitch, registers once every module in
     * the ROM has (fileswitch.c, Service_PostInit). That is where the team's
     * HostFS, after Free in RISC OS's ROM, puts itself. */
    static const uint32_t own[] = { 220, 89, 33, 5, 26, 23, 8 };
    uint32_t nums[16];
    unsigned n = fs_list(w, nums, 16);
    int code = 1;
    for (uint32_t p = ros_ld32(w + 16); p; p = ros_ld32(p))
        code &= ros_code_lookup(ros_ld32(p + 12)) != NULL &&
                (ros_ld32(p + 8) == 220 || ros_ld32(p + 16) == w);
    check(n == 7 && memcmp(nums, own, sizeof own) == 0 && code && ros_ld32(w) == 0 &&
              path_is("Free$Path", "Resources:$.Resources.Free."),
          "Free, initialised: no task; its own entries for PCCardFS, NFS, NetFS, SCSIFS, "
          "RAMFS and ADFS, each compiled code, and HostFS's first; Free$Path its resources",
          "%u, first %u", n, n ? nums[0] : 0);

    uint32_t entry = ros_native_entry(fs_entry, "selftest:free_fs_entry");
    uint32_t r[10] = { TEST_FS, entry, 0x1234 };
    int reg = !swi(XFREE_REGISTER, r) && r[0] == TEST_FS && r[1] == entry &&
              r[2] == 0x1234 && fs_list(w, nums, 16) == 8 && nums[0] == TEST_FS;
    uint32_t bad[10] = { 0 };
    swi(XFREE_REGISTER + 2, bad);
    int past = last && last->errnum == 0x110 &&
               strcmp(last_error(), "SWI value out of range for module Free") == 0;
    check(reg && past, "Free_Register: a filing system's entry first in the list, R0-R2 kept; "
          "a SWI past its two refused", "%s", last_error());

    /* UpCall_ModifyingFile: each window of that filing system asks its
     * entry whether the file is on its device, the return address pushed */
    uint32_t win = ros_addr(ros_rma_alloc(96));
    memset(ros_ptr(win), 0, 96);
    ros_st32(win + 8, 0xFFFFFFFFu);
    ros_st32(win + 12, TEST_FS);
    ros_st32(win + 40, win + 80);
    strcpy(ros_ptr(win + 80), "Disc");
    ros_st32(w + 4, win);
    strcpy(ros_ptr(scratch), "Disc.$.File");
    memset(&entered, 0, sizeof entered);
    uint32_t up[10] = { 3, scratch };
    up[8] = TEST_FS, up[9] = 0;                     /* upfsfile_Save */
    swi(XOS_UpCall, up);
    int called = entered.calls == 1 && entered.r[0] == 3 && entered.r[1] == TEST_FS &&
                 entered.r[2] == scratch && entered.r[3] == win + 80 && entered.r12 == 0x1234 &&
                 entered.pushed == w;
    int marked = ros_ld32(win + 44) == win && ros_ld32(w + 8) == win;
    ros_st32(w + 4, 0), ros_st32(w + 8, 0);
    ros_rma_free(ros_ptr(win));
    check(called && marked, "UpCall_ModifyingFile: Free calls a filing system's entry with the "
          "return address pushed (CallEntry), and it comes back; the window marked for update",
          "calls %u, R0 %u, R12 &%X", entered.calls, entered.r[0], entered.r12);

    uint32_t dr[10] = { TEST_FS, entry, 0x1234 };
    swi(XFREE_DEREGISTER, dr);
    check(fs_list(w, nums, 16) == 7 && nums[0] == 220, "Free_DeRegister: the entry gone", NULL);

    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x49;                                  /* Service_StartWimp */
    service("Free", &s);
    int s1 = s.r[1] == 0 && strcmp(ros_ptr(s.r[0]), "Desktop_Free") == 0 &&
             ros_ld32(w) == 0xFFFFFFFFu;
    ros_cpu_enter(&s);
    s.r[1] = 0x49;
    service("Free", &s);
    int s2 = s.r[1] == 0x49;
    int s3 = cli("ShowFree -FS HostFS Host") == 0 &&
             strcmp(last_error(), "Free task not running, use *Desktop") == 0;
    ros_cpu_enter(&s);
    s.r[1] = 0x4A;                                  /* Service_StartedWimp */
    service("Free", &s);
    int s4 = ros_ld32(w) == 0 && cli("Desktop_Free") == 0 &&
             strcmp(last_error(), "Use *Desktop to start Free") == 0;
    check(s1 && s2 && s3 && s4, "Service_StartWimp: Free claims it with *Desktop_Free and "
          "waits; asked again, passes; *ShowFree outside its task refused; "
          "Service_StartedWimp: dormant again, *Desktop_Free refused", "%s", last_error());

    ros_cpu_enter(&s);
    s.r[1] = 0x49;
    service("Free", &s);
    ros_cpu_enter(&s);
    s.r[1] = 0x27;                                  /* Service_Reset */
    service("Free", &s);
    check(s.r[1] == 0x27 && ros_ld32(w) == 0 && ros_ld32(w + 4) == 0 && fs_list(w, nums, 16) == 7,
          "Service_Reset: Free passes it on, its task forgotten, its filing systems kept",
          "task &%X", ros_ld32(w));

    /* *RMReInit Free: its own six again, and no one registers again, as
     * on RISC OS 5.30 (tests/desktop/resfiler, fr_register and fr_task),
     * where *ShowFree -FS HostFS is then "Unknown filing system" */
    uint32_t e = cli("RMReInit Free");
    w = workspace("Free");
    n = w ? fs_list(w, nums, 16) : 0;
    check(e == 0 && n == 6 && memcmp(nums, own + 1, sizeof own - sizeof own[0]) == 0,
          "*RMReInit Free: its own six filing systems, HostFS's forgotten, as on RISC OS 5.30",
          "&%X %s: %u, first %u", e, last_error(), n, n ? nums[0] : 0);
    /* ... and HostFS registered again, as the ROM had it, for what runs
     * after the self-test (rosgd.selftest's prompt, *Desktop: Host's Free) */
    ros_hostfs_free_register(0);
    n = w ? fs_list(w, nums, 16) : 0;
    check(n == 7 && memcmp(nums, own, sizeof own) == 0,
          "HostFS's Free entry registered again after the check: the seven, HostFS first",
          "%u, first %u", n, n ? nums[0] : 0);
}

static void test_filing_systems(void)
{
    /* OS_FSControl 13: a filing system by name or number */
    strcpy(ros_ptr(scratch), "HostFS");
    uint32_t a[10] = { 13, scratch, 0 };
    int f1 = !swi(XOS_FSControl, a) && a[1] == 220 && a[2] != 0;
    strcpy(ros_ptr(scratch), "resources:$");
    uint32_t b[10] = { 13, scratch, 0 };
    int f2 = !swi(XOS_FSControl, b) && b[1] == 46 && b[2] != 0;
    strcpy(ros_ptr(scratch), "NoSuchFS");
    uint32_t c[10] = { 13, scratch, 0 };
    int f3 = !swi(XOS_FSControl, c) && c[2] == 0;
    uint32_t d[10] = { 13, 46, 0 };
    int f4 = !swi(XOS_FSControl, d) && d[1] == 46 && d[2] == b[2];
    check(f1 && f2 && f3 && f4, "OS_FSControl 13: HostFS and Resources: by name, Resources "
          "by number; a name no filing system has, R2 0", NULL);

    /* OS_Byte 143, Service_StartUpFS: a filing system selected by number,
     * as the Resource Filer asks whether ResourceFS is there */
    uint32_t cur[10] = { 0, 0 };
    swi(XOS_Args, cur);
    uint32_t sel[10] = { 143, 0x12, 46 };
    swi(XOS_Byte, sel);
    uint32_t now[10] = { 0, 0 };
    swi(XOS_Args, now);
    uint32_t back[10] = { 143, 0x12, cur[0] };
    swi(XOS_Byte, back);
    uint32_t after[10] = { 0, 0 };
    swi(XOS_Args, after);
    check(sel[1] == 0 && now[0] == 46 && after[0] == cur[0],
          "OS_Byte 143, Service_StartUpFS 46: FileSwitch selects ResourceFS and claims the "
          "call; the filing system as it was after", "&%X %u %u", sel[1], now[0], after[0]);
}

static void test_resfiler(void)
{
    struct ros_module *m = module("ResourceFiler");
    check(m && m->base == 0xFCC00000u && ros_module_version(m) == 0x2000 &&
              workspace("ResourceFiler") == 0 && checksum(m->base, 1992) == 0x0BC4BBB9u &&
              path_is("ResFiler$Path", "Resources:$.Resources.ResFiler."),
          "ResourceFiler 0.20 in the ROM at &FCC00000, no workspace until the Filer starts "
          "it; its image RISC OS 5.30's, byte for byte; ResFiler$Path its resources", NULL);
    if (!m)
        return;

    /* The Filer starting it: its workspace, the command that starts it */
    struct ros_cpu r;
    ros_cpu_enter(&r);
    r.r[0] = 0x1234, r.r[1] = 0x4B;                 /* Service_StartFiler */
    service("ResourceFiler", &r);
    uint32_t w = workspace("ResourceFiler");
    int s1 = r.r[1] == 0 && strcmp(ros_ptr(r.r[0]), "Desktop_ResourceFiler") == 0 && w &&
             ros_ld32(w) == 0 && ros_ld32(w + 4) == 0x1234 &&
             ros_ld32(ros_ld32(w + 8)) == w;
    struct ros_cpu again;
    ros_cpu_enter(&again);
    again.r[0] = 0x5678, again.r[1] = 0x4B;
    service("ResourceFiler", &again);
    int s2 = again.r[1] == 0x4B && ros_ld32(w + 4) == 0x1234;
    check(s1 && s2, "Service_StartFiler: the Resource Filer claims it with "
          "*Desktop_ResourceFiler and notes its Filer; asked again with ResourceFS there "
          "(OS_Byte 143), passes", "R1 &%X, &%X", r.r[1], again.r[1]);

    uint32_t c[10] = { 0, 0x5E };                   /* Service_MessageFileClosed */
    swi(XOS_ServiceCall, c);
    uint32_t menu = w + 72;
    int made = ros_ld32(w + 12) != 0 && memcmp(ros_ptr(menu), "Resources", 9) == 0 &&
               ros_ld8(menu + 9) < ' ' && ros_ld32(menu + 28) == 0x80 &&
               (ros_ld32(menu + 36) & 0x100) &&
               memcmp(ros_ptr(ros_ld32(menu + 40)), "Open '$'", 8) == 0 &&
               ros_ld8(ros_ld32(menu + 40) + 8) < ' ';
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x4F;                                  /* Service_FilerDying */
    service("ResourceFiler", &s);
    int died = workspace("ResourceFiler") == 0;
    struct ros_cpu start;
    ros_cpu_enter(&start);
    start.r[0] = 0x1234, start.r[1] = 0x4B;
    service("ResourceFiler", &start);
    ros_cpu_enter(&s);
    s.r[1] = 0x27;                                  /* Service_Reset */
    service("ResourceFiler", &s);
    check(made && died && start.r[1] == 0 && workspace("ResourceFiler") == 0 &&
              cli("Desktop_ResourceFiler") == 0 &&
              strcmp(last_error(), "Use *Desktop to start ResourceFiler") == 0,
          "Service_MessageFileClosed: its menu made from ResFiler:Messages; "
          "Service_FilerDying, Service_Reset: its workspace freed; *Desktop_ResourceFiler "
          "refused", "menu %d, died %d", made, died);
}

/* OS_File 17: the object's type, 0 if none or an error; its load and exec */
static uint32_t object(const char *name, uint32_t *load, uint32_t *exec)
{
    strcpy(ros_ptr(scratch), name);
    uint32_t r[10] = { 17, scratch };
    if (swi(XOS_File, r))
        return 0;
    *load = r[2], *exec = r[3];
    return r[0];
}

static uint32_t file_type(uint32_t load)
{
    return (load & 0xFFF00000u) == 0xFFF00000u ? load >> 8 & 0xFFF : 0x1000;
}

/* A file in ResourceFS holding a line that starts with `want`: where the
 * rest of that line is, or NULL */
static const char *line_of(const char *name, const char *want, uint32_t *rest)
{
    uint32_t f = ros_resourcefs_find(name);
    if (!f)
        return NULL;
    const char *p = ros_ptr(f + 4), *end = p + ros_ld32(f) - 4;
    size_t n = strlen(want);
    for (const char *l = p; l < end;) {
        const char *nl = l;
        while (nl < end && *nl != '\n')
            nl++;
        if ((size_t)(nl - l) >= n && memcmp(l, want, n) == 0) {
            *rest = (uint32_t)(nl - l - n);
            return l + n;
        }
        l = nl + 1;
    }
    return NULL;
}

/* A tokenised program at a, n bytes: CR, its lines chained to CR &FF,
 * which ends it (BASIC's ENDER) */
static int program_at(uint32_t a, uint32_t n)
{
    uint32_t p = a;
    while (p + 4 <= a + n && ros_ld8(p) == 13 && ros_ld8(p + 1) != 0xFF && ros_ld8(p + 3) >= 4)
        p += ros_ld8(p + 3);
    return ros_ld8(p) == 13 && ros_ld8(p + 1) == 0xFF && p + 2 == a + n;
}

/* "2.07 (15-Mar-25)": VersionNum's Module_FullVersion and
 * Module_ApplicationDate, as Build:AwkVers writes them */
static int version_like(const char *v, uint32_t n)
{
    static const char shape[] = "9.99 (99-AAA-99)";   /* 9 a digit, A a letter */
    if (n != sizeof shape - 1)
        return 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = v[i], s = shape[i];
        if (s == '9' ? c < '0' || c > '9' : s == 'A' ? c < 'A' || c > 'z' : c != s)
            return 0;
    }
    return 1;
}

/* !Edit, a C application, as RISC OS's ROM build makes it (CApp; tools/
 * mkromapps.py). The files are Resources:$.Apps.!Edit with its ROM !Boot,
 * !Help and !Run. The !Run is RISC OS's, except that Export's line is
 * replaced by Edit$Tokenise and Edit$Detokenise -1. The files also include
 * Resources:$.Resources.Edit with its Export, Messages (_Version from
 * VersionNum) and Templates, and ROSGD's program beside them, which is
 * Edit's x32 image as !RunImage (type &FF8). ModuleWrap makes a module of
 * it, "!Edit" (modules/modulewrap). It has no workspace and its help string
 * is ModuleWrap's with Messages' version, and *Help Desktop_Edit works.
 * When it is killed, the ROM !Run's RMEnsure brings it back (RMReInit of a
 * ROM module). Its running is tests/deskprobe/edit.py's, in the box. Its
 * files against 5.30's are tests/desktop/romapps's. */
static void test_rom_edit(void)
{
    uint32_t l = 0, x = 0, bl = 0, hl = 0, rl = 0, il = 0, xl = 0, tl = 0;
    int dir = object("Resources:$.Apps.!Edit", &l, &x) == 2;
    int files = object("Resources:$.Apps.!Edit.!Boot", &bl, &x) == 1 && file_type(bl) == 0xFEB &&
                object("Resources:$.Apps.!Edit.!Help", &hl, &x) == 1 && file_type(hl) == 0xFFF &&
                object("Resources:$.Apps.!Edit.!Run", &rl, &x) == 1 && file_type(rl) == 0xFEB &&
                object("Resources:$.Resources.Edit.!RunImage", &il, &x) == 1 &&
                file_type(il) == 0xFF8 &&
                object("Resources:$.Resources.Edit.Export", &xl, &x) == 1 && file_type(xl) == 0xFFB &&
                object("Resources:$.Resources.Edit.Templates", &tl, &x) == 1 && file_type(tl) == 0xFEC;
    const char *run = "Resources:$.Apps.!Edit.!Run";
    uint32_t n = 0;
    int lines = line_of(run, "RMEnsure !Edit 0.00 RMReInit !Edit", &n) && n == 0 &&
                line_of(run, "SetEval Edit$Tokenise -1", &n) && n == 0 &&
                line_of(run, "SetEval Edit$Detokenise -1", &n) && n == 0 &&
                line_of(run, "Desktop_Edit %*0", &n) && n == 0 && !line_of(run, "Run Edit:Export", &n);
    uint32_t img = ros_resourcefs_find("Resources:$.Resources.Edit.!RunImage");
    int elf = img && ros_ld32(img) >= 24 && ros_ld8(img + 4) == 0x7F && ros_ld8(img + 5) == 'E' &&
              ros_ld8(img + 6) == 'L' && ros_ld8(img + 7) == 'F' && ros_ld8(img + 8) == 1 &&
              ros_ld8(img + 4 + 18) == ROS_CAPP_EM;         /* ELFCLASS32, the box's: x32, A64X32 */
    uint32_t vn = 0;
    const char *v = line_of("Resources:$.Resources.Edit.Messages", "_Version:", &vn);
    int msgs = v && version_like(v, vn);
    check(dir && files && lines && elf && msgs,
          "Resources:$.Apps.!Edit and Resources:$.Resources.Edit, as the ROM build makes a C "
          "application: !Boot, !Help, the ROM !Run with Export's line made Edit$Tokenise and "
          "Edit$Detokenise -1 (RMEnsure, Desktop_Edit %*0); Export, Templates, Messages' "
          "_Version from VersionNum; and the " ROS_CAPP_ABI " image, !RunImage",
          "directory %d files %d !Run's lines %d image %d messages %d: _Version:%.*s", dir, files,
          lines, elf, msgs, v ? (int)vn : 0, v ? v : "");

    /* The module: OS_Module 18 finds it, with no workspace. Its help
     * string is "!Edit", two tabs, the version (Messages') and the date. */
    strcpy(ros_ptr(scratch), "!Edit");
    uint32_t m[10] = { 18, scratch };
    int found = !swi(XOS_Module, m);
    char help[64] = "";
    if (found)
        snprintf(help, sizeof help, "%s", (const char *)ros_ptr(m[3] + ros_ld32(m[3] + 0x14)));
    const char *title = found ? ros_ptr(m[3] + ros_ld32(m[3] + 0x10)) : "";
    size_t vlen = 0;
    while (v && vlen < vn && v[vlen] != ' ')
        vlen++;
    int wrapped = found && m[4] == 0 && strcmp(title, "!Edit") == 0 &&
                  strncmp(help, "!Edit\t\t", 7) == 0 && vlen && strncmp(help + 7, v, vlen) == 0 &&
                  help[7 + vlen] == ' ' && help[8 + vlen] == '(';
    uint32_t helped = cli("Help Desktop_Edit");
    /* Killed, not found; the ROM !Run's RMEnsure line brings it back */
    uint32_t killed = cli("RMKill !Edit");
    strcpy(ros_ptr(scratch), "!Edit");
    uint32_t k[10] = { 18, scratch };
    int gone = swi(XOS_Module, k);
    uint32_t ensured = cli("RMEnsure !Edit 0.00 RMReInit !Edit");
    strcpy(ros_ptr(scratch), "!Edit");
    uint32_t b[10] = { 18, scratch };
    int back = !swi(XOS_Module, b) && b[4] == 0;
    check(wrapped && !helped && !killed && gone && !ensured && back,
          "!Edit, the module ModuleWrap makes, native: no workspace, its help string \"!Edit\", "
          "two tabs, Messages' version and its date; *Help Desktop_Edit; RMKill, then the ROM "
          "!Run's RMEnsure !Edit 0.00 RMReInit !Edit brings it back",
          "found %d workspace &%X title '%s' help '%s'; *Help &%X; RMKill &%X, gone %d; "
          "RMEnsure &%X, back %d", found, m[4], title, help, helped, killed, gone, ensured, back);

#if !ROS_CAPP_NATIVE
    /* *Desktop_Edit runs the image by its own name, as ModuleWrap enters its
     * own code, whatever Edit$Path says (its <Obey$Dir> is whichever Obey
     * file ran last). With Edit$Path unset it is still Resources:$.
     * Resources.Edit.!RunImage that runs. Here it is refused by name, the
     * hosted build having no 32-bit code. In the box it starts Edit
     * (tests/deskprobe/edit.py). The slot its WimpSlot line sets is put
     * back. */
    uint32_t slot[10] = { (uint32_t)-1, (uint32_t)-1 };
    swi(XWimp_SlotSize, slot);
    cli("Unset Edit$Path");
    uint32_t de = cli("Desktop_Edit");
    char why[160];
    snprintf(why, sizeof why, "%s", last_error());
    uint32_t put[10] = { slot[0], (uint32_t)-1 };
    swi(XWimp_SlotSize, put);
    check(de && strstr(why, "'Resources:$.Resources.Edit.!RunImage' is a C application"),
          "*Desktop_Edit runs Resources:$.Resources.Edit.!RunImage by its name, Edit$Path unset "
          "(hosted: refused as an x32 image)", "&%X %s", de, why);
#endif
}

/* !Draw, an older C application (StdTools; tools/mkromapps.py), as RISC
 * OS's ROM build makes it. The files are Resources:$.Apps.!Draw with its
 * ROM !Boot, !Help and ROM !Run. These are RISC OS's, unchanged: Draw$Path,
 * the ModuleWrap program's WIMPSlots, and Desktop_Draw %*0. The files also
 * include Resources:$.Resources.Draw with its Messages (_Version from
 * VersionNum) and Templates, the Sprites its ModuleWrap module registers,
 * and ROSGD's program, which is Draw's x32 image as !RunImage. The module,
 * "!Draw" (modules/modulewrap), has no workspace, and its help string is
 * ModuleWrap's with Messages' version. The words of *Help Desktop_Draw are
 * those that the kernel prints from Draw's Messages. They run on past
 * DrawHelp's line to the end of the file, as 5.30's do. RMKill and RMReInit
 * are checked as well. Draw's running is the box's. Its files against
 * 5.30's are tests/desktop/romapps's. */
static void test_rom_draw(void)
{
    uint32_t l = 0, x = 0, bl = 0, hl = 0, rl = 0, il = 0, ml = 0, tl = 0, sl = 0;
    int dir = object("Resources:$.Apps.!Draw", &l, &x) == 2 &&
              object("Resources:$.Resources.Draw", &l, &x) == 2;
    int files = object("Resources:$.Apps.!Draw.!Boot", &bl, &x) == 1 && file_type(bl) == 0xFEB &&
                object("Resources:$.Apps.!Draw.!Help", &hl, &x) == 1 && file_type(hl) == 0xFFF &&
                object("Resources:$.Apps.!Draw.!Run", &rl, &x) == 1 && file_type(rl) == 0xFEB &&
                object("Resources:$.Resources.Draw.!RunImage", &il, &x) == 1 &&
                file_type(il) == 0xFF8 &&
                object("Resources:$.Resources.Draw.Messages", &ml, &x) == 1 && file_type(ml) == 0xFFF &&
                object("Resources:$.Resources.Draw.Templates", &tl, &x) == 1 && file_type(tl) == 0xFEC &&
                object("Resources:$.Resources.Draw.Sprites", &sl, &x) == 1 && file_type(sl) == 0xFF9;
    const char *run = "Resources:$.Apps.!Draw.!Run";
    uint32_t n = 0;
    int lines = line_of(run, "Set Draw$Path <Obey$Dir>.,Resources:$.Resources.Draw.", &n) && n == 0 &&
                line_of(run, "WIMPSlot -min 96K", &n) && n == 0 &&
                line_of(run, "WIMPSlot -min 32K -max 32K", &n) && n == 0 &&
                line_of(run, "Desktop_Draw %*0", &n) && n == 0 &&
                line_of("Resources:$.Apps.!Draw.!Boot", "Set Alias$@RunType_AFF /<Obey$Dir>", &n);
    uint32_t img = ros_resourcefs_find("Resources:$.Resources.Draw.!RunImage");
    int elf = img && ros_ld32(img) >= 24 && ros_ld8(img + 4) == 0x7F && ros_ld8(img + 5) == 'E' &&
              ros_ld8(img + 6) == 'L' && ros_ld8(img + 7) == 'F' && ros_ld8(img + 8) == 1 &&
              ros_ld8(img + 4 + 18) == ROS_CAPP_EM;         /* ELFCLASS32, the box's: x32, A64X32 */
    uint32_t vn = 0;
    const char *v = line_of("Resources:$.Resources.Draw.Messages", "_Version:", &vn);
    int msgs = v && version_like(v, vn) && vn == strlen(MODWRAP_DRAW_VERSION) &&
               memcmp(v, MODWRAP_DRAW_VERSION, vn) == 0;
    check(dir && files && lines && elf && msgs,
          "Resources:$.Apps.!Draw and Resources:$.Resources.Draw, as the ROM build makes an older C "
          "application: !Boot, !Help, the ROM !Run RISC OS's (Draw$Path, WIMPSlot 96K, 32K, "
          "Desktop_Draw %*0); Messages' _Version " MODWRAP_DRAW_VERSION ", Templates, ModuleWrap's "
          "Sprites; and the " ROS_CAPP_ABI " image, !RunImage",
          "directories %d files %d !Run's lines %d image %d messages %d: _Version:%.*s", dir, files,
          lines, elf, msgs, v ? (int)vn : 0, v ? v : "");

    /* The module: OS_Module 18 finds it, with no workspace. Its help
     * string is "!Draw", two tabs, the version (Messages') and the date. */
    strcpy(ros_ptr(scratch), "!Draw");
    uint32_t m[10] = { 18, scratch };
    int found = !swi(XOS_Module, m);
    char help[64] = "";
    if (found)
        snprintf(help, sizeof help, "%s", (const char *)ros_ptr(m[3] + ros_ld32(m[3] + 0x14)));
    const char *title = found ? ros_ptr(m[3] + ros_ld32(m[3] + 0x10)) : "";
    size_t vlen = 0;
    while (v && vlen < vn && v[vlen] != ' ')
        vlen++;
    int wrapped = found && m[4] == 0 && strcmp(title, "!Draw") == 0 &&
                  strncmp(help, "!Draw\t\t", 7) == 0 && vlen && strncmp(help + 7, v, vlen) == 0 &&
                  help[7 + vlen] == ' ' && help[8 + vlen] == '(';
    /* *Help Desktop_Draw: the kernel's header, then DrawHelp's text on to
     * the end of Messages, then DrawSyntax's, as 5.30's prints them */
    uint32_t helped = cli_output("Help Desktop_Draw");
    char got[512], want[512];
    words(captured, got, sizeof got);
    snprintf(want, sizeof want, "==> Help on keyword Desktop_Draw %s %s", MODWRAP_DRAW_CMDHELP,
             MODWRAP_DRAW_CMDSYNTAX);
    words(want, want, sizeof want);
    int printed = !helped && strcmp(got, want) == 0 &&
                  strstr(got, "application DrawSyntax:Syntax: *Desktop_Draw _Version:") != NULL;
    uint32_t killed = cli("RMKill !Draw");
    strcpy(ros_ptr(scratch), "!Draw");
    uint32_t k[10] = { 18, scratch };
    int gone = swi(XOS_Module, k);
    uint32_t again = cli("RMReInit !Draw");
    strcpy(ros_ptr(scratch), "!Draw");
    uint32_t b[10] = { 18, scratch };
    int back = !swi(XOS_Module, b) && b[4] == 0;
    check(wrapped && printed && !killed && gone && !again && back,
          "!Draw, the module ModuleWrap makes, native: no workspace, its help string \"!Draw\", two "
          "tabs, Messages' version and its date; *Help Desktop_Draw prints DrawHelp and DrawSyntax as "
          "5.30's kernel does from Draw's Messages, on to its end; RMKill, then RMReInit brings it back",
          "found %d workspace &%X title '%s' help '%s'; *Help &%X: '%s'; RMKill &%X, gone %d; "
          "RMReInit &%X, back %d", found, m[4], title, help, helped, got, killed, gone, again, back);
}

/* Resources:$.Apps.!Paint and Resources:$.Resources.Paint: Paint, an older
 * C application as Draw is (tools/mkromapps.py: its resources rule; its
 * ResFiles registers nothing), and the native !Paint module */
static void test_rom_paint(void)
{
    uint32_t l = 0, x = 0, bl = 0, hl = 0, rl = 0, il = 0, ml = 0, tl = 0, sl = 0;
    int dir = object("Resources:$.Apps.!Paint", &l, &x) == 2 &&
              object("Resources:$.Resources.Paint", &l, &x) == 2;
    int files = object("Resources:$.Apps.!Paint.!Boot", &bl, &x) == 1 && file_type(bl) == 0xFEB &&
                object("Resources:$.Apps.!Paint.!Help", &hl, &x) == 1 && file_type(hl) == 0xFFF &&
                object("Resources:$.Apps.!Paint.!Run", &rl, &x) == 1 && file_type(rl) == 0xFEB &&
                object("Resources:$.Resources.Paint.!RunImage", &il, &x) == 1 &&
                file_type(il) == 0xFF8 &&
                object("Resources:$.Resources.Paint.Messages", &ml, &x) == 1 && file_type(ml) == 0xFFF &&
                object("Resources:$.Resources.Paint.Templates", &tl, &x) == 1 && file_type(tl) == 0xFEC &&
                object("Resources:$.Resources.Paint.Sprites", &sl, &x) == 1 && file_type(sl) == 0xFF9;
    const char *run = "Resources:$.Apps.!Paint.!Run";
    uint32_t n = 0;
    int lines = line_of(run, "Set Paint$Path <Obey$Dir>.,Resources:$.Resources.Paint.", &n) && n == 0 &&
                line_of(run, "WimpSlot -min 68K", &n) && n == 0 &&
                line_of(run, "WimpSlot -min 12K -max 12K", &n) && n == 0 &&
                line_of(run, "Desktop_Paint %*0", &n) && n == 0 &&
                line_of("Resources:$.Apps.!Paint.!Boot", "Set Alias$@RunType_FF9 /<Obey$Dir>", &n);
    uint32_t img = ros_resourcefs_find("Resources:$.Resources.Paint.!RunImage");
    int elf = img && ros_ld32(img) >= 24 && ros_ld8(img + 4) == 0x7F && ros_ld8(img + 5) == 'E' &&
              ros_ld8(img + 6) == 'L' && ros_ld8(img + 7) == 'F' && ros_ld8(img + 8) == 1 &&
              ros_ld8(img + 4 + 18) == ROS_CAPP_EM;         /* ELFCLASS32, the box's: x32, A64X32 */
    uint32_t vn = 0;
    const char *v = line_of("Resources:$.Resources.Paint.Messages", "_Version:", &vn);
    int msgs = v && version_like(v, vn) && vn == strlen(MODWRAP_PAINT_VERSION) &&
               memcmp(v, MODWRAP_PAINT_VERSION, vn) == 0;
    check(dir && files && lines && elf && msgs,
          "Resources:$.Apps.!Paint and Resources:$.Resources.Paint, as the ROM build makes an older C "
          "application: !Boot (Alias$@RunType_FF9), !Help, the ROM !Run RISC OS's (Paint$Path, "
          "WimpSlot 68K, 12K, Desktop_Paint %*0); Messages' _Version " MODWRAP_PAINT_VERSION ", Sprites, "
          "Templates; and the " ROS_CAPP_ABI " image, !RunImage",
          "directories %d files %d !Run's lines %d image %d messages %d: _Version:%.*s", dir, files,
          lines, elf, msgs, v ? (int)vn : 0, v ? v : "");

    /* The module: OS_Module 18 finds it, with no workspace. Its help
     * string is "!Paint", two tabs, the version (Messages') and the date. */
    strcpy(ros_ptr(scratch), "!Paint");
    uint32_t m[10] = { 18, scratch };
    int found = !swi(XOS_Module, m);
    char help[64] = "";
    if (found)
        snprintf(help, sizeof help, "%s", (const char *)ros_ptr(m[3] + ros_ld32(m[3] + 0x14)));
    const char *title = found ? ros_ptr(m[3] + ros_ld32(m[3] + 0x10)) : "";
    size_t vlen = 0;
    while (v && vlen < vn && v[vlen] != ' ')
        vlen++;
    int wrapped = found && m[4] == 0 && strcmp(title, "!Paint") == 0 &&
                  strncmp(help, "!Paint\t\t", 8) == 0 && vlen && strncmp(help + 8, v, vlen) == 0 &&
                  help[8 + vlen] == ' ' && help[9 + vlen] == '(';
    /* *Help Desktop_Paint: the kernel's header, then PaintHelp's text and
     * PaintSyntax's, each to its NUL, as 5.30's prints them */
    uint32_t helped = cli_output("Help Desktop_Paint");
    char got[512], want[512];
    words(captured, got, sizeof got);
    snprintf(want, sizeof want, "==> Help on keyword Desktop_Paint %s %s", MODWRAP_PAINT_CMDHELP,
             MODWRAP_PAINT_CMDSYNTAX);
    words(want, want, sizeof want);
    int printed = !helped && strcmp(got, want) == 0 &&
                  strcmp(got, "==> Help on keyword Desktop_Paint The !Paint module runs the Paint desktop "
                              "application Syntax: *Desktop_Paint") == 0;
    uint32_t killed = cli("RMKill !Paint");
    strcpy(ros_ptr(scratch), "!Paint");
    uint32_t k[10] = { 18, scratch };
    int gone = swi(XOS_Module, k);
    uint32_t again = cli("RMReInit !Paint");
    strcpy(ros_ptr(scratch), "!Paint");
    uint32_t b[10] = { 18, scratch };
    int back = !swi(XOS_Module, b) && b[4] == 0;
    check(wrapped && printed && !killed && gone && !again && back,
          "!Paint, the module ModuleWrap makes, native: no workspace, its help string \"!Paint\", two "
          "tabs, Messages' version and its date; *Help Desktop_Paint prints PaintHelp and PaintSyntax as "
          "5.30's kernel does from Paint's Messages; RMKill, then RMReInit brings it back",
          "found %d workspace &%X title '%s' help '%s'; *Help &%X: '%s'; RMKill &%X, gone %d; "
          "RMReInit &%X, back %d", found, m[4], title, help, helped, got, killed, gone, again, back);
}

/* Filer_Action: ModuleWrap's FilerAct shape (tools/mkromapps.py's CModule;
 * modules/modulewrap). The files are Resources:$.Resources.FilerAct with
 * Messages, CmdHelp appended (its help texts NUL-ended, as 5.30's ROM has
 * them), Templates, and the image as !RunImage. There is no
 * Resources:$.Apps directory. The module is Filer_Action, with the command
 * *Filer_Action and FilerAct$Path. */
static void test_rom_fileract(void)
{
    uint32_t l = 0, x = 0, ml = 0, tl = 0, il = 0;
    int dir = object("Resources:$.Resources.FilerAct", &l, &x) == 2 &&
              object("Resources:$.Apps.!FilerAct", &l, &x) == 0;
    int files = object("Resources:$.Resources.FilerAct.Messages", &ml, &x) == 1 && file_type(ml) == 0xFFF &&
                object("Resources:$.Resources.FilerAct.Templates", &tl, &x) == 1 && file_type(tl) == 0xFEC &&
                object("Resources:$.Resources.FilerAct.!RunImage", &il, &x) == 1 && file_type(il) == 0xFF8;
    uint32_t n = 0;
    const char *msgs = "Resources:$.Resources.FilerAct.Messages";
    const char *h = line_of(msgs, "HFACFAC:", &n);
    int helps = h && n == strlen(MODWRAP_FILERACT_CMDHELP) + 1 && h[n - 1] == 0 &&
                memcmp(h, MODWRAP_FILERACT_CMDHELP, n - 1) == 0 && line_of(msgs, "SFACFAC:Syntax:", &n) &&
                line_of(msgs, "89:", &n);           /* the task's name, wimpt_init's */
    uint32_t img = ros_resourcefs_find("Resources:$.Resources.FilerAct.!RunImage");
    int elf = img && ros_ld32(img) >= 24 && ros_ld8(img + 4) == 0x7F && ros_ld8(img + 5) == 'E' &&
              ros_ld8(img + 6) == 'L' && ros_ld8(img + 7) == 'F' && ros_ld8(img + 8) == 1 &&
              ros_ld8(img + 4 + 18) == ROS_CAPP_EM;
    check(dir && files && helps && elf,
          "Resources:$.Resources.FilerAct, as the ROM build's CModule rule makes it: Messages with CmdHelp's "
          "HFACFAC and SFACFAC (NUL-ended, as 5.30's ROM has them), Templates; the " ROS_CAPP_ABI
          " image, !RunImage; no Resources:$.Apps directory",
          "directory %d files %d help texts %d image %d", dir, files, helps, elf);

    /* The module has no workspace, its help string is "Filer_Action", a tab
     * and the version. FilerAct$Path is set at its initialisation.
     * *Help Filer_Action gives the command's help and syntax, then the
     * module's, as 5.30's kernel prints them. RMKill and RMReInit work. */
    strcpy(ros_ptr(scratch), "Filer_Action");
    uint32_t m[10] = { 18, scratch };
    int found = !swi(XOS_Module, m);
    char help[64] = "";
    if (found)
        snprintf(help, sizeof help, "%s", (const char *)ros_ptr(m[3] + ros_ld32(m[3] + 0x14)));
    const char *title = found ? ros_ptr(m[3] + ros_ld32(m[3] + 0x10)) : "";
    int wrapped = found && m[4] == 0 && strcmp(title, "Filer_Action") == 0 &&
                  strcmp(help, MODWRAP_FILERACT_HELP) == 0 && strncmp(help, "Filer_Action\t", 13) == 0;
    char got[512], want[512], path[128];
    cli_output("Show FilerAct$Path");
    words(captured, path, sizeof path);
    int pathed = strcmp(path, "FilerAct$Path : Resources:$.Resources.FilerAct.") == 0;
    uint32_t helped = cli_output("Help Filer_Action");
    words(captured, got, sizeof got);
    snprintf(want, sizeof want, "==> Help on keyword Filer_Action %s %s ==> Help on keyword Filer_Action "
             "Module is: %s Commands provided: Filer_Action", MODWRAP_FILERACT_CMDHELP,
             MODWRAP_FILERACT_CMDSYNTAX, MODWRAP_FILERACT_HELP);
    words(want, want, sizeof want);
    static const char head[] = "==> Help on keyword Filer_Action The Filer_Action module runs the background "
                               "Filer operations Syntax: *Filer_Action ==> Help on keyword Filer_Action Module "
                               "is: Filer_Action ";
    int printed = !helped && strcmp(got, want) == 0 && strncmp(got, head, sizeof head - 1) == 0;
    uint32_t killed = cli("RMKill Filer_Action");
    strcpy(ros_ptr(scratch), "Filer_Action");
    uint32_t k[10] = { 18, scratch };
    int gone = swi(XOS_Module, k);
    uint32_t again = cli("RMReInit Filer_Action");
    strcpy(ros_ptr(scratch), "Filer_Action");
    uint32_t b[10] = { 18, scratch };
    int back = !swi(XOS_Module, b) && b[4] == 0;
    check(wrapped && pathed && printed && !killed && gone && !again && back,
          "Filer_Action, the module ModuleWrap's FilerAct switch makes, native: no workspace, its help "
          "string \"Filer_Action\", a tab, the version and date; FilerAct$Path Resources:$.Resources.FilerAct.; "
          "*Help Filer_Action as 5.30's kernel prints it (HFACFAC, SFACFAC, the module); RMKill, RMReInit",
          "found %d workspace &%X title '%s' help '%s'; path '%s'; *Help &%X: '%s'; RMKill &%X, gone %d; "
          "RMReInit &%X, back %d", found, m[4], title, help, path, helped, got, killed, gone, again, back);
}

static void test_rom_apps(void)
{
    uint32_t load = 0, exec = 0, hl = 0, he = 0, rl = 0;
    int apps = object("Resources:$.Apps", &load, &exec) == 2 &&
               object("Resources:$.Apps.!Chars", &load, &exec) == 2;
    int help = object("Resources:$.Apps.!Chars.!Help", &hl, &he) == 1 && file_type(hl) == 0xFFF;
    int run = object("Resources:$.Apps.!Chars.!Run", &rl, &exec) == 1 && file_type(rl) == 0xFEB;
    uint32_t n = 0;
    int path = run && line_of("Resources:$.Apps.!Chars.!Run",
                              "Set Chars$Path <Obey$Dir>.,Resources:$.Resources.Chars.", &n) &&
               n == 0 && line_of("Resources:$.Apps.!Chars.!Run", "Run Chars:!RunLink", &n);
    /* Each file is dated as its source, as the ROM build keeps them. The dates
     * are what tools/mkromapps.py took from riscos-src (romapps_stamps.h).
     * Here !Help is 09:27:32 09-Jul-2016, the date the farm's Apps viewer
     * gives !Chars (ResourceFS dates a directory by its first file). */
    unsigned dated = 0, stamps = sizeof romapps_stamps / sizeof romapps_stamps[0];
    const char *undated = "";
    for (unsigned i = 0; i < stamps; i++) {
        uint32_t l = 0, x = 0;
        if (object(romapps_stamps[i].path, &l, &x) == 1 && (l & 0xFF) == romapps_stamps[i].load_lo &&
            x == romapps_stamps[i].exec && (l & 0xFF || x))
            dated++;
        else if (!*undated)
            undated = romapps_stamps[i].path;
    }
    check(apps && help && run && path && stamps >= 6 && dated == stamps,
          "Resources:$.Apps, the ROM's applications: !Chars a directory there, its !Help and "
          "its ROM !Run (Chars$Path it and Resources:$.Resources.Chars., then Chars:!RunLink); "
          "each file dated as its source",
          "apps %d help %d run %d path %d, %u of %u dated (%s), !Help &%08X &%08X", apps, help, run,
          path, dated, stamps, undated, hl, he);

    /* Directories, as RISC OS's ResourceFS has them (FindFileOrDirectory,
     * readdirentriesinfo). Read alone, a directory gives the load and exec
     * addresses of the first file in it, with length and attributes 0: $ 0
     * and 0, R, L and r. Listed, it gives that file's length and attributes
     * too. The farm's Apps viewer dates !Chars 09:27:32 09-Jul-2016, its
     * !Help's. */
    strcpy(ros_ptr(scratch), "Resources:$.Apps.!Chars");
    uint32_t dr[10] = { 17, scratch };
    int vdr = swi(XOS_File, dr);
    strcpy(ros_ptr(scratch), "Resources:$");
    uint32_t rr[10] = { 17, scratch };
    int vrr = swi(XOS_File, rr);
    uint32_t buf = scratch + 256;
    strcpy(ros_ptr(scratch), "Resources:$.Apps");
    strcpy(ros_ptr(scratch + 128), "!Chars");
    uint32_t gl[10] = { 10, scratch, buf, 1, 0, 128, scratch + 128 };
    int vgl = swi(XOS_GBPB, gl);
    uint32_t hattr = 0;
    strcpy(ros_ptr(scratch + 200), "Resources:$.Apps.!Chars.!Help");
    uint32_t hr[10] = { 17, scratch + 200 };
    if (!swi(XOS_File, hr))
        hattr = hr[5];
    int dirs = !vdr && dr[0] == 2 && dr[2] == hl && dr[3] == he && dr[4] == 0 && dr[5] == 0 &&
               !vrr && rr[0] == 2 && rr[2] == 0 && rr[3] == 0 && rr[5] == 0x19 && !vgl &&
               gl[3] == 1 && ros_ld32(buf) == hl && ros_ld32(buf + 4) == he &&
               ros_ld32(buf + 8) == hr[4] && ros_ld32(buf + 12) == hattr &&
               ros_ld32(buf + 16) == 2;
    check(dirs,
          "ResourceFS's directories as RISC OS's: !Chars read alone its first file's (!Help's) "
          "load and exec, length and attributes 0; $ 0 and 0, attributes R L r; !Chars listed, "
          "!Help's load, exec, length and attributes, a directory",
          "!Chars %u &%08X &%08X %u &%X (!Help &%08X &%08X); $ %u &%X &%X &%X; listed %u: &%08X "
          "&%08X %u &%X type %u", dr[0], dr[2], dr[3], dr[4], dr[5], hl, he, rr[0], rr[2], rr[3],
          rr[5], gl[3], ros_ld32(buf), ros_ld32(buf + 4), ros_ld32(buf + 8), ros_ld32(buf + 12),
          ros_ld32(buf + 16));

    /* The end of a directory as ResourceFS gives it. R4 is the next index
     * while a call reads anything, and -1 only from a call that reads
     * nothing. So the Desktop's PumpBootROMApps, reading Resources:$.Apps one
     * "!*" at a time and stopping as soon as R4 is -1, reaches every
     * entry. */
    strcpy(ros_ptr(scratch), "Resources:$.Apps");
    strcpy(ros_ptr(scratch + 128), "!*");
    uint32_t all[10] = { 9, scratch, buf, 64, 0, 256, scratch + 128 };
    int vall = swi(XOS_GBPB, all);
    uint32_t after[10] = { 9, scratch, buf, 64, all[4], 256, scratch + 128 };
    int vafter = swi(XOS_GBPB, after);
    uint32_t pos = 0, pumped = 0;
    for (int k = 0; k < 64; k++) {              /* the Desktop's loop */
        uint32_t one[10] = { 9, scratch, buf, 1, pos, 256, scratch + 128 };
        if (swi(XOS_GBPB, one) || (int32_t)one[4] < 0)
            break;
        pos = one[4];
        pumped += one[3] == 1;
    }
    check(!vall && all[3] >= 1 && all[4] == all[3] && !vafter && after[3] == 0 &&
              after[4] == 0xFFFFFFFFu && pumped == all[3],
          "ResourceFS -- OS_GBPB 9's R4 at the end: the next index while entries are read, -1 "
          "only when none is (s/ResourceFS); the Desktop's one-at-a-time loop reaches every "
          "Resources:$.Apps entry",
          "all: %u read, R4 &%X; after: %u read, R4 &%X; one at a time: %u", all[3], all[4],
          after[3], after[4], pumped);

    /* Its program, RUN in place: !RunLink opens it and sets PAGE to what
     * ReadFSHandle gives when the filing system is 46, ResourceFS */
    int prog = object("Resources:$.Resources.Chars.!RunImage", &load, &exec) == 1 &&
               file_type(load) == 0xFFB;
    strcpy(ros_ptr(scratch), "Resources:$.Resources.Chars.!RunImage");
    uint32_t o[10] = { 0x4F, scratch };
    int opened = !swi(XOS_Find, o) && o[0];
    uint32_t fh[10] = { 21, o[0] }, ext[10] = { 2, o[0] };
    int inplace = opened && !swi(XOS_FSControl, fh) && !swi(XOS_Args, ext) &&
                  (fh[2] & 0xFF) == 46 &&
                  fh[1] == ros_resourcefs_find("Resources:$.Resources.Chars.!RunImage") + 4 &&
                  program_at(fh[1], ext[2]);
    if (opened) {
        uint32_t c[10] = { 0, o[0] };
        swi(XOS_Find, c);
    }
    int link = object("Resources:$.Resources.Chars.!RunLink", &load, &exec) == 1 &&
               file_type(load) == 0xFFB &&
               object("Resources:$.Resources.Chars.Templates", &load, &exec) == 1 &&
               file_type(load) == 0xFEC;
    uint32_t vn = 0, tn = 0;
    const char *v = line_of("Resources:$.Resources.Chars.Messages", "_Version:", &vn);
    const char *t = line_of("Resources:$.Resources.Chars.Messages", "_TaskName:", &tn);
    int msgs = v && version_like(v, vn) && t && tn == 10 && memcmp(t, "Characters", 10) == 0;
    check(prog && inplace && link && msgs,
          "Resources:$.Resources.Chars: its !RunImage BASIC, ReadFSHandle its data (filing "
          "system 46) a tokenised program, CR to CR &FF, as !RunLink RUNs it in place; "
          "!RunLink, Templates; Messages' _TaskName Characters and _Version filled from "
          "VersionNum, as the ROM build fills it",
          "program %d in place %d link %d messages %d: _Version:%.*s", prog, inplace, link, msgs,
          v ? (int)vn : 0, v ? v : "");

    test_rom_edit();
    test_rom_draw();
    test_rom_paint();
    test_rom_fileract();
}

void ros_selftest_resfiler(void)
{
    scratch = ros_addr(ros_rma_alloc(512));
    test_free();
    test_filing_systems();
    test_resfiler();
    test_rom_apps();
    ros_rma_free(ros_ptr(scratch));
}
