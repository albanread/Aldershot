/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_sprutils.c: the Screen Blanker and SpriteUtils, native modules
 * written to their contracts (modules/screenblanker, modules/spriteutils):
 * what must always hold. Their behaviour against RISC OS 5.30's is
 * tests/desktop/sprutils (compare.py), whose probes were recorded on the
 * farm. The blanker's timings there are in seconds, and here they are in a
 * flash.
 *
 * The runtime does some things for them, and those are checked too:
 * PaletteV's BlankScreen, R10-R12 kept over OS_GenerateEvent, files to and
 * from the system sprite area, OS_File 19 naming a file as it was given, and
 * the sprite area shrinking as far as its sprites allow.
 *
 * The output of *commands is caught on WrchV. The service calls are made by
 * a module of the test's own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/platform.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define PALETTEV 0x23u
#define SERVICE_BLANKED 0x7Au
#define SERVICE_RESTORED 0x7Bu
#define SERVICE_BLANKING 0xA9u

static char out[2048];
static unsigned outn;
static uint32_t line;                   /* arena scratch: a command, a name */

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* A SWI: its registers in and out; 0, or the error number */
static uint32_t swi(uint32_t number, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static const char *errmess(const uint32_t r[10])
{
    return ((os_error *)ros_ptr(r[0]))->errmess;
}

/* A command, its output in out: 0 or the error number, its text in error */
static uint32_t cli(const char *cmd, char *error, size_t size)
{
    strcpy(ros_ptr(line), cmd);
    outn = 0, out[0] = 0;
    uint32_t r[10] = { line };
    uint32_t e = swi(OS_CLI, r);
    snprintf(error, size, "%s", e ? errmess(r) : "");
    return e;
}

static const struct ros_module *module(const char *title)
{
    for (const struct ros_module *m = ros_module_first(); m; m = m->next)
        if (strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

/* PaletteV BlankScreen, reading: 1 blanked, 0 on; -1 if nobody answers */
static int screen(void)
{
    uint32_t r[10] = { 0xFFFFFFFFu, 0, 0, 0, 6, 0, 0, 0, 0, PALETTEV };
    swi(OS_CallAVector, r);
    return r[4] ? -1 : (int)r[0];
}

static uint32_t control(uint32_t reason, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4)
{
    uint32_t r[10] = { reason, r1, r2, r3, r4 };
    return swi(ScreenBlanker_Control, r);
}

/* The way out of an outermost SWI runs the callbacks */
static void run_callbacks(void)
{
    uint32_t r[10] = { 0 };
    swi(OS_ReadMonotonicTime, r);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- the service calls, as a module sees them ---- */

static uint32_t seen[16][2], nseen;
static int take_blanking;

static void watch_service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    uint32_t n = s->r[1];
    if (n != SERVICE_BLANKING && n != SERVICE_BLANKED && n != SERVICE_RESTORED)
        return;
    if (nseen < 16)
        seen[nseen][0] = n, seen[nseen][1] = s->r[0], nseen++;
    if (n == SERVICE_BLANKING && take_blanking)
        s->r[1] = 0;
}

static struct ros_module watcher = {
    .title = "SUWatch",
    .help = "SUWatch\t1.00 (26 Sep 2026)",
    .service = watch_service,
};

static int saw(unsigned i, uint32_t service, uint32_t r0)
{
    return i < nseen && seen[i][0] == service && seen[i][1] == r0;
}

/* Wait up to ms for the screen to be as wanted, ticks running meanwhile */
static int wait_screen(int want, unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 10) {
        if (screen() == want)
            return 1;
        ROS_BLOCKING(sleep_ms(10));
    }
    return screen() == want;
}

static void blanker(void)
{
    char error[256];
    uint32_t s1 = 0, c1 = 0, s2 = 0, c2 = 0, c3 = 0, c4 = 1;
    xscreenblanker_set_timeout(1234);
    xscreenblanker_read_timeout(&s1);
    xscreenblanker_read_timeout2(&c1);
    xscreenblanker_set_timeout(1);
    xscreenblanker_read_timeout(&s2);
    xscreenblanker_read_timeout2(&c2);
    xscreenblanker_set_timeout(26214400);
    xscreenblanker_read_timeout2(&c3);
    xscreenblanker_set_timeout(0);
    xscreenblanker_read_timeout2(&c4);
    check(s1 == 12 && c1 == 1220 && s2 == 5 && c2 == 500 && c3 == 26214400 && c4 == 0,
          "ScreenBlanker_Control 3, 4, 5: the time in ticks of 20 cs -- 1234 cs reads back as "
          "12 s and 1220 cs; 5 s at least; 0 never",
          "%u s %u cs; %u s %u cs; %u cs; %u cs", s1, c1, s2, c2, c3, c4);

    uint32_t r6[10] = { 6 }, bad[10] = { 0 };
    uint32_t e6 = swi(ScreenBlanker_Control, r6);
    int t6 = e6 && strcmp(errmess(r6), "SWI value out of range for module ScreenBlanker") == 0;
    uint32_t eb = swi(ScreenBlanker_Control + 1, bad);
    int tb = eb && strcmp(errmess(bad), "SWI value out of range for module ScreenBlanker") == 0;
    check(e6 == 0x110 && t6 && eb == 0x110 && tb,
          "ScreenBlanker_Control 6 (dimming, not built) and SWI &43101: &110, the global "
          "BadSWI with the module's title", "&%X %d, &%X %d", e6, t6, eb, tb);

    /* ---- blanking through PaletteV; standby ---- */
    int before = screen();
    control(0, 0, 0, 0, 0);
    int b = screen();
    control(1, 0, 0, 0, 0);
    int u = screen();
    control(8, 0, 0, 0, 0);
    int sb = screen();
    control(1, 0, 0, 0, 0);
    int su = screen();
    control(9, 0, 0, 0, 0);
    int on = screen();
    check(before == 0 && b == 1 && u == 0 && sb == 1 && su == 1 && on == 0,
          "Blank and Unblank through PaletteV 6 (read back with R0 -1); in standby (8) Unblank "
          "is ignored, StrictUnblank (9) ends it", "%d, %d %d, %d %d %d", before, b, u, sb, su, on);

    /* ---- the service calls ---- */
    ros_module_add(&watcher, "");
    run_callbacks();
    nseen = 0;
    xscreenblanker_blank();                 /* typed: no SWI's way out between */
    unsigned now = nseen;
    run_callbacks();
    xscreenblanker_unblank();
    run_callbacks();
    xscreenblanker_flash(100, 100, 0, 0x55);
    run_callbacks();
    xscreenblanker_unblank();
    run_callbacks();
    check(now == 1 && saw(0, SERVICE_BLANKING, 1) && saw(1, SERVICE_BLANKED, 0) &&
              saw(2, SERVICE_RESTORED, 0) && saw(3, SERVICE_RESTORED, 0x55) &&
              saw(4, SERVICE_RESTORED, 0) && nseen == 5,
          "Service_ScreenBlanking before blanking; Service_ScreenBlanked and _Restored after, "
          "from a callback, R0 the flags Flash was given",
          "%u then %u: &%X/%X &%X/%X &%X/%X &%X/%X", now, nseen, seen[0][0], seen[0][1],
          seen[1][0], seen[1][1], seen[2][0], seen[2][1], seen[3][0], seen[3][1]);

    take_blanking = 1, nseen = 0;
    xscreenblanker_blank();
    int ext = screen();
    run_callbacks();
    xscreenblanker_unblank();
    run_callbacks();
    take_blanking = 0;
    check(ext == 0 && saw(0, SERVICE_BLANKING, 1) && saw(1, SERVICE_BLANKED, 0) &&
              saw(2, SERVICE_RESTORED, 0) && screen() == 0,
          "a claimant of Service_ScreenBlanking blanks instead: PaletteV is left alone, the "
          "other calls still made", "screen %d, %u calls", ext, nseen);

    /* ---- flashing, on TickerV: 40 cs each way. A tick first sees the
     * mouse where the tests before left it, and moving it would stop the
     * flashing ---- */
    ROS_BLOCKING(sleep_ms(300));
    control(2, 40, 40, 0, 0);
    int off = wait_screen(1, 1500), back = off && wait_screen(0, 1500);
    control(1, 0, 0, 0, 0);
    check(off && back, "Flash, 40 cs each way: the ticks blank the screen and unblank it again",
          "off %d, back %d", off, back);

    uint32_t kill[10] = { 4, ros_addr(strcpy(ros_ptr(line), "SUWatch")) };
    swi(OS_Module, kill);

    /* ---- *BlankTime ---- */
    uint32_t k1 = cli("BlankTime 10", error, sizeof error);
    cli("BlankTime", error, sizeof error);
    int ten = strcmp(out, "BlankingTime : 10 seconds\n\r") == 0;
    cli("BlankTime W", error, sizeof error);
    cli("BlankTime", error, sizeof error);
    int w = strstr(out, "BlankingTime : 10 seconds WriteCV Claimed") != NULL;
    uint32_t k2 = cli("BlankTime 30x", error, sizeof error);
    int syn = strcmp(error, "Syntax: *BlankTime [W|O] [Time]") == 0;
    uint32_t t30 = 0;
    xscreenblanker_read_timeout(&t30);
    cli("BlankTime o 0", error, sizeof error);
    cli("BlankTime", error, sizeof error);
    int offs = strcmp(out, "BlankingTime : Off\n\r") == 0;
    check(!k1 && ten && w && k2 == 0xDC && syn && t30 == 30 && offs,
          "*BlankTime: 10 seconds, W claiming WriteC, O releasing it, Off -- from the Messages "
          "file; 30x sets 30 s, then the file's syntax error",
          "&%X %d %d, &%X \"%s\" %u, %d", k1, ten, w, k2, error, t30, offs);

    /* ---- OS_GenerateEvent keeps R10-R12, with the blanker on EventV ---- */
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 11, c.r[1] = 0, c.r[2] = 0x7E, c.r[10] = 0x1010, c.r[11] = 0x1111, c.r[12] = 0x1212;
    ros_swi(&c, XOS_GenerateEvent);
    check(c.r[10] == 0x1010 && c.r[11] == 0x1111 && c.r[12] == 0x1212,
          "OS_GenerateEvent: R10-R12 come back as they went, though a claimant ran with its "
          "own R12", "&%X &%X &%X", c.r[10], c.r[11], c.r[12]);
}

/* ---- SpriteUtils, and the runtime under it ---- */

static uint32_t area_size(void)
{
    uint32_t r[10] = { 3 };
    swi(OS_ReadDynamicArea, r);
    return r[1];
}

static void set_size(uint32_t size)
{
    uint32_t r[10] = { 3, size - area_size() };
    swi(OS_ChangeDynamicArea, r);
}

static void sprite_utils(void)
{
    char error[256];
    uint32_t start = area_size();
    cli("SNew", error, sizeof error);
    set_size(0);
    uint32_t n0 = cli("SInfo", error, sizeof error);
    int none = strcmp(out, "No system sprites memory\r\n") == 0;
    set_size(65536);
    cli("SNew", error, sizeof error);
    cli("SInfo", error, sizeof error);
    int info = strcmp(out, "System sprites status:\r\n  64 Kbytes system sprites workspace\r\n"
                           "  65520 byte(s) free\r\n  0 system sprite(s) defined\r\n") == 0;
    cli("SList", error, sizeof error);
    int nodef = strcmp(out, "No system sprites defined\r\n") == 0;
    uint32_t mk[10] = { 15, 0, ros_addr(strcpy(ros_ptr(line + 256), "one")), 0, 4, 4, 28 };
    swi(OS_SpriteOp, mk);
    cli("SCopy one  two", error, sizeof error);
    cli("SRename one Three", error, sizeof error);
    cli("SList", error, sizeof error);
    int list = strcmp(out, "three\n\rtwo\n\r") == 0 || strcmp(out, "two\n\rthree\n\r") == 0;
    check(!n0 && none && info && nodef && list,
          "*SInfo, *SList: no memory, the area's size and use from the Messages file, no "
          "sprites, the names after *SCopy and *SRename", "%d %d %d %d \"%s\"", none, info, nodef,
          list, out);

    /* ---- files to and from the area, through FileSwitch ---- */
    char root[512];
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-sprutils-XXXXXX", tmp);
    int dir = mkdtemp(root) != NULL;
    if (dir)
        ros_hostfs_mount("SUTest", root);
    uint32_t sv = cli("SSave HostFS::SUTest.$.Spr", error, sizeof error);
    cli("SNew", error, sizeof error);
    uint32_t ld = cli("SLoad HostFS::SUTest.$.Spr", error, sizeof error);
    cli("SList", error, sizeof error);
    int back = strstr(out, "two") && strstr(out, "three");
    uint32_t v[10] = { ros_addr(strcpy(ros_ptr(line + 256), "SUTest$Dir")),
                       ros_addr(strcpy(ros_ptr(line + 320), "HostFS::SUTest.$")), 16, 0, 0 };
    swi(OS_SetVarVal, v);
    uint32_t nf = cli("SLoad <SUTest$Dir>.Nothing", error, sizeof error);
    int given = strcmp(error, "File '<SUTest$Dir>.Nothing' not found") == 0;
    uint32_t sl = cli("ScreenLoad <SUTest$Dir>.Nothing", error, sizeof error);
    int full = strcmp(error, "File 'HostFS::SUTest.$.Nothing' not found") == 0;
    check(dir && !sv && !ld && back && ros_arena_valid(ROS_DA_BASE, ROS_DA_BASE + 16) &&
              nf == 0xD6 && given && sl == 0xD6 && full,
          "*SSave and *SLoad through FileSwitch: the area in use is memory a file comes from "
          "and goes to; a file not there named as given (OS_File 19), or in full (ScreenLoad's "
          "OS_Find)", "save &%X, load &%X, back %d; &%X %d, &%X \"%s\"", sv, ld, back, nf, given,
          sl, error);
    uint32_t del[10] = { 6, ros_addr(strcpy(ros_ptr(line + 256), "HostFS::SUTest.$.Spr")) };
    swi(OS_File, del);
    uint32_t unset[10] = { ros_addr(strcpy(ros_ptr(line + 256), "SUTest$Dir")), 0, (uint32_t)-1 };
    swi(OS_SetVarVal, unset);
    if (dir) {
        ros_hostfs_unmount("SUTest");
        rmdir(root);
    }

    /* ---- the area shrinks as far as its sprites allow ---- */
    uint32_t sh[10] = { 3, (uint32_t)-65536 };
    uint32_t e = swi(OS_ChangeDynamicArea, sh);
    int moved = (int32_t)sh[1];
    uint32_t left = area_size();
    check(e == 0x1C1 && left == 4096 && moved == 61440,
          "OS_ChangeDynamicArea 3 past the sprites in use: shrunk as far as they allow, then "
          "&1C1 Memory cannot be moved", "&%X, %d moved, %u left", e, moved, left);

    cli("SNew", error, sizeof error);
    set_size(start);
}

void ros_selftest_sprutils(void)
{
    const struct ros_module *sb = module("ScreenBlanker"), *su = module("SpriteUtils");
    check(sb && su && sb->swi_chunk == 0x43100 && sb->swi_count == 1 && su->commands &&
              ros_module_version(sb) == 0x23400 && ros_module_version(su) == 0x11300 &&
              sb->private_word && ros_ld32(sb->private_word),
          "ScreenBlanker 2.34 and SpriteUtils 1.13 in the ROM, native: the blanker started, "
          "its SWI chunk &43100", "ScreenBlanker %s, SpriteUtils %s", sb ? "found" : "missing",
          su ? "found" : "missing");
    if (!sb || !su)
        return;
    line = ros_addr(ros_rma_alloc(512));
    ros_vector_claim_native(WRCHV, wrch, 0);
    blanker();
    sprite_utils();
    ros_vector_release_native(WRCHV, wrch, 0);
    ros_rma_free(ros_ptr(line));
}
