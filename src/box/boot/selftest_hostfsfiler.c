/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_hostfsfiler.c: HostFSFiler, native (modules/hostfsfiler),
 * before there is a desktop to start it: the module there and dormant,
 * its command and help, the service calls that start and stop it, and
 * FileSwitch's list of HostFS discs it takes its icons from.  It also
 * tests the runtime's native start entry (runtime/module.c), which the
 * module's task runs from, using a module of its own.  The desktop's side, checked
 * against RISC OS 5.30, is tests/desktop/hostfsfiler.
 */
#include <string.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define SERVICE_STARTFILER 0x4Bu
#define SERVICE_STARTEDFILER 0x4Cu
#define SERVICE_FILERDYING 0x4Fu

static uint32_t line;                       /* arena: a command, or a string */
static const os_error *last;
static char out[2048];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI: gives the error number, or 0 (also 0, with last set, for an
 * error 0).  What it printed is left in out */
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

static struct ros_module *module(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (m->title && strcmp(m->title, title) == 0)
            return m;
    return NULL;
}

/* A service call to HostFSFiler alone, through its service entry, as
 * OS_ServiceCall makes it: gives whether it was claimed, and R0.  The Resource
 * Filer, in the ROM too, claims Service_StartFiler as well. */
static int service(uint32_t number, uint32_t *r0)
{
    struct ros_module *m = module("HostFSFiler");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = *r0;
    s.r[1] = number;
    uint32_t depth = ros_call_depth;
    ros_call_depth = depth + 1;
    m->service(m, &s);
    ros_call_depth = depth;
    *r0 = s.r[0];
    return s.r[1] == 0;
}

/* ---- a native start entry, entered as the application ---------------------- */

static struct {
    int ran;
    uint32_t depth, tail_ok, private_word, swi_ok;
} started;
static int exit_by_os_exit;

static void test_start(struct ros_module *m, uint32_t tail)
{
    started.ran++;
    started.depth = ros_call_depth;
    started.tail_ok = tail && strcmp(ros_ptr(tail), "one two") == 0;
    started.private_word = m->private_word;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, XOS_ReadMonotonicTime);
    started.swi_ok = !s.v && ros_call_depth == 0;
    if (exit_by_os_exit) {
        ros_cpu_enter(&s);
        ros_swi(&s, OS_Exit);               /* back to the command that entered it */
        started.ran = 99;                   /* not reached */
    }
}

static struct ros_module start_test = {
    .title = "StartTest",
    .help = "StartTest\t1.00 (26 Sep 2026)",
    .start = test_start,
};

static int enter(const char *title, const char *tail)
{
    strcpy(ros_ptr(line), title);
    strcpy(ros_ptr(line + 64), tail);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 2, s.r[1] = line, s.r[2] = line + 64;
    ros_swi(&s, XOS_Module);
    return !s.v;
}

static void native_start(void)
{
    ros_module_add(&start_test, "");
    uint32_t depth = ros_call_depth;
    exit_by_os_exit = 1;
    int e1 = enter("StartTest", "one two");
    int ok1 = e1 && started.ran == 1 && started.depth == 0 && started.tail_ok && started.swi_ok &&
              started.private_word == start_test.private_word && ros_call_depth == depth;
    exit_by_os_exit = 0;
    int e2 = enter("StartTest", "one two");
    int ok2 = e2 && started.ran == 2 && ros_call_depth == depth;
    check(ok1 && ok2,
          "OS_Module 2 -- a native start entry runs as the application: its SWIs the "
          "outermost, R0's tail, its private word; OS_Exit, or returning, comes back to "
          "the command that entered it",
          "ran %d, depth %u, tail %u, SWI %u, returned %d %d", started.ran, started.depth,
          started.tail_ok, started.swi_ok, e1, e2);
    cli("RMKill StartTest");
}

/* ---- HostFSFiler --------------------------------------------------------- */

void ros_selftest_hostfsfiler(void)
{
    line = ros_addr(ros_rma_alloc(512));
    native_start();

    struct ros_module *m = module("HostFSFiler");
    check(m && m->start && m->service && !m->init && ros_ld32(m->private_word) == 0 &&
              ros_module_version(m) == 0x20000u,
          "HostFSFiler 2.00 -- native, in the ROM and started; dormant, no workspace until the "
          "Filer asks for filers", NULL);
    if (!m)
        return;

    int u1 = cli("Desktop_HostFSFiler") == 0 && last &&
             strcmp(last->errmess, "Use *Desktop to start HostFSFiler") == 0;
    int u2 = cli("Desktop_HostFSFiler now") == 0xDC &&
             strcmp(last->errmess, "Syntax: *Desktop_HostFSFiler") == 0;
    int u3 = cli("Help Desktop_HostFSFiler") == 0 &&
             strstr(out, "The HostFS filer puts the host computer's shared directory on the") &&
             strstr(out, "Do not use *Desktop_HostFSFiler, use *Desktop instead.");
    check(u1 && u2 && u3,
          "*Desktop_HostFSFiler outside the desktop: \"Use *Desktop to start HostFSFiler\" (&0); "
          "a parameter, its syntax (&DC); *Help, the team's text",
          "%s / \"%.120s\"", last ? last->errmess : "", out);

    /* Service_StartFiler: claimed with the command that starts it, and
     * the workspace is claimed.  Asked again, it waits.  StartedFiler leaves
     * it waiting.  FilerDying frees it */
    uint32_t r0 = 0x1234;
    int s1 = service(SERVICE_STARTFILER, &r0) && r0 >= ROS_RMA_BASE &&
             strcmp(ros_ptr(r0), "Desktop_HostFSFiler") == 0;
    uint32_t w = ros_ld32(m->private_word);
    uint32_t again = 0x1234;
    int s2 = w && !service(SERVICE_STARTFILER, &again) && again == 0x1234;
    uint32_t z = 0;
    service(SERVICE_STARTEDFILER, &z);
    int s3 = ros_ld32(m->private_word) == w;
    service(SERVICE_FILERDYING, &z);
    int s4 = ros_ld32(m->private_word) == 0;
    check(s1 && s2 && s3 && s4,
          "Service_StartFiler: claimed, R0 -> \"Desktop_HostFSFiler\" in the RMA, workspace "
          "claimed; again: not claimed; StartedFiler: kept; FilerDying: freed",
          "%d %d %d %d", s1, s2, s3, s4);

    /* A start that gave up (-1) is not started again until StartedFiler.
     * *RMReInit frees a workspace */
    ros_st32(m->private_word, 0xFFFFFFFFu);
    uint32_t q = 0;
    int g1 = !service(SERVICE_STARTFILER, &q) && ros_ld32(m->private_word) == 0xFFFFFFFFu;
    service(SERVICE_STARTEDFILER, &q);
    int g2 = ros_ld32(m->private_word) == 0;
    q = 0x1234;
    int g3 = service(SERVICE_STARTFILER, &q) && cli("RMReInit HostFSFiler") == 0 &&
             (m = module("HostFSFiler")) && ros_ld32(m->private_word) == 0;
    check(g1 && g2 && g3,
          "HostFSFiler told not to start (-1): Service_StartFiler not claimed until "
          "Service_StartedFiler; *RMReInit frees its workspace",
          "%d %d %d", g1, g2, g3);

    /* Its icons come from FileSwitch's discs, in the order they were
     * mounted */
    unsigned n = 0;
    while (ros_hostfs_disc(n))
        n++;
    ros_hostfs_mount("HFFTest", "/nonexistent");
    int d1 = ros_hostfs_disc(n) && strcmp(ros_hostfs_disc(n), "HFFTest") == 0 &&
             !ros_hostfs_disc(n + 1);
    ros_hostfs_unmount("HFFTest");
    int d2 = !ros_hostfs_disc(n);
    check(d1 && d2, "HostFS's discs, listed for HostFSFiler's icons: a disc mounted is last, "
                    "and gone when unmounted", "%u discs, %d %d", n, d1, d2);
    ros_rma_free(ros_ptr(line));
}
