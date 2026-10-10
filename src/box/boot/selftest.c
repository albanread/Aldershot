/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest.c: the box's acceptance test.
 *
 * T0Demo is ObjAsm compiled to C (modules/t0demo).  This drives it through
 * every path the runtime provides.  The paths are typed calls into compiled
 * code, compiled code calling native SWIs and itself, errors returned and
 * raised, and indirect calls through the dispatcher and its refusals.  It
 * checks each result against what the ObjAsm source says it must be.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rom_t0demo.h"
#include "selftest.h"

static struct ros_selftest *t;

#define check ros_check

void ros_check(int ok, const char *what, const char *fmt, ...)
{
    if (t->count < sizeof t->ok)
        t->ok[t->count] = (unsigned char)ok;
    t->count++;
    if (ok) {
        t->passed++;
        ros_console_printf("  ok    %s\n", what);
        return;
    }
    t->failed++;
    char detail[256] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
    }
    ros_console_printf("  FAIL  %s%s%s\n", what, fmt ? ": " : "", detail);
}

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char *msg(const os_error *e)
{
    return e ? e->errmess : "no error";
}

int ros_selftest(struct ros_selftest *result)
{
    memset(result, 0, sizeof *result);
    t = result;
    uint32_t calls = 0;         /* T0Demo's SWIs so far: it counts them too */
    os_error *e;
    char what[128];

    ros_console_printf("rosgd: self-test -- T0Demo, ObjAsm compiled to C\n");

    /* ---- the arena ---- */
    check(ros_addr(ros_ptr(ROS_APP_BASE)) == ROS_APP_BASE &&
              ros_addr(ros_ptr(ROS_ROM_BASE)) == ROS_ROM_BASE,
          "arena: RISC OS addresses round-trip", NULL);
#ifndef ROS_ARENA_HOSTED
    check(ros_ptr(ROS_RMA_BASE) == (void *)(uintptr_t)ROS_RMA_BASE,
          "arena: identity-mapped, so ros_ptr() is free", NULL);
#endif

    /* ---- the kernel command line: split as Linux splits it, so a quoted
     * value keeps its spaces (rosgd.run="Obey ...", #20) ---- */
    {
        static const char *line = "console=hvc0 rosgd.run=\"Obey HostFS::Host.$.Apps.!Go\" panic=-1 "
                                  "ip=dhcp \"rosgd.x=a b\" rosgd.y=\"rosgd.poweroff\" rosgd.z";
        const char *v, *x, *y;
        size_t n = 0, xn = 0, yn = 0;
        int run = ros_cmdline_find(line, "rosgd.run", &v, &n);
        int ok = run && v && n == strlen("Obey HostFS::Host.$.Apps.!Go") &&
                 !memcmp(v, "Obey HostFS::Host.$.Apps.!Go", n) &&
                 ros_cmdline_find(line, "rosgd.x", &x, &xn) && x && xn == 3 && !memcmp(x, "a b", 3) &&
                 ros_cmdline_find(line, "rosgd.y", &y, &yn) && yn == 14 &&
                 !ros_cmdline_find(line, "rosgd.poweroff", NULL, NULL) &&
                 ros_cmdline_find(line, "ip=dhcp", NULL, NULL) && ros_cmdline_find(line, "rosgd.z", NULL, NULL) &&
                 !ros_cmdline_find(line, "Obey", NULL, NULL) && !ros_cmdline_find(line, "rosgd", NULL, NULL);
        check(ok, "kernel command line: a quoted value keeps its spaces, a word in one is not a parameter",
              "rosgd.run: %.*s", run && v ? (int)n : 6, run && v ? v : "absent");
    }

    /* ---- the module, as its header and Init made it ---- */
    struct ros_module *m = ros_module_for_swi(T0Demo_Sum);
    check(m && strcmp(m->title, "T0Demo") == 0 && m->swi_chunk == 0xC0000 && m->swi_count == 5,
          "module: T0Demo started from its ROM header", "%s",
          m ? m->title : "no module owns &C0000");
    if (!m)
        goto done;
    const char *name = ros_swi_name(XT0Demo_Classify);
    check(name && strcmp(name, "T0Demo_Classify") == 0,
          "module: SWI names from its decoding table", "%s", name ? name : "none");
    uint32_t ws = ros_ld32(m->private_word);
    check(ws >= ROS_RMA_BASE && ws < ROS_RMA_BASE + ROS_RMA_SIZE && ros_ld32(ws) == 0,
          "module: compiled Init claimed its workspace by OS_Module 6",
          "private word &%08X holds &%08X", m->private_word, ws);

    /* ---- T0Demo_Sum: a loop, ADDS and ADC ---- */
    uint32_t *words = NULL;
    e = xos_module_claim(16, (void **)&words);
    check(!e && ros_in_arena(words), "OS_Module 6 from native code: an RMA block", "%s", msg(e));
    if (e)
        goto done;
    words[0] = 0xFFFFFFFFu;
    words[1] = 1;
    words[2] = 0x80000000u;
    words[3] = 0x80000000u;
    uint32_t lo = 1, hi = 1;
    e = xt0demo_sum(words, 4, &lo, &hi);
    calls++;
    check(!e && lo == 0 && hi == 2, "T0Demo_Sum: a 64-bit total, carried by ADDS and ADC",
          "%s: &%08X:%08X", msg(e), hi, lo);
    e = xt0demo_sum(words, 0, &lo, &hi);
    calls++;
    check(!e && lo == 0 && hi == 0, "T0Demo_Sum of nothing: MOVS, then MOVEQ pc, lr",
          "%s: &%08X:%08X", msg(e), hi, lo);

    /* ---- T0Demo_Classify: conditions, and Z as a result ---- */
    static const struct { int32_t value; uint32_t kind; int z; } cases[] = {
        { 0, 0, 1 }, { -5, 1, 0 }, { 42, 2, 0 }, { 1000, 3, 0 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t kind = 99;
        int z = -1;
        e = xt0demo_classify(cases[i].value, &kind, &z);
        calls++;
        snprintf(what, sizeof what, "T0Demo_Classify %d: %u, Z %s", cases[i].value,
                 cases[i].kind, cases[i].z ? "set" : "clear");
        check(!e && kind == cases[i].kind && z == cases[i].z, what, "%s: %u, Z %d", msg(e), kind, z);
    }

    /* ---- T0Demo_Fail: an error returned with V set ---- */
    e = xt0demo_fail();
    calls++;
    check(e && e->errnum == 0xC0000 && strcmp(e->errmess, "T0Demo_Fail fails, as it should") == 0 &&
              ros_addr(e) == T0DEMO_ErrorDemo,
          "T0Demo_Fail: V set, R0 -> its error block in ROM", "%s at &%08X", msg(e),
          e ? ros_addr(e) : 0);

    /* ---- T0Demo_Greet: native SWIs, a SWI to itself, an indirect call ---- */
    uint32_t count = 0, indirect = 0;
    ros_console_printf("  ..    T0Demo_Greet, by OS_Write0: ");
    e = xt0demo_greet(&count, &indirect);
    calls += 2;                 /* Greet, and the Classify it calls */
    check(!e && indirect == 42, "T0Demo_Greet: an indirect call through a DCD table",
          "%s: %u", msg(e), indirect);
    check(!e && count == calls, "T0Demo_Greet: its nested SWI to itself was counted",
          "count %u, expected %u", count, calls);

    /* ---- T0Demo_LongJump: out from two calls deep, sp put back ---- */
    uint32_t next = 0;
    e = xt0demo_long_jump(41, &next);
    calls++;
    check(!e && next == 42,
          "T0Demo_LongJump: a return from two calls deep, sp put back as it was on entry (the Wimp's menu selection)",
          "%s: %u", msg(e), next);
    uint32_t kind = 99;
    int z = -1;
    e = xt0demo_classify(7, &kind, &z);
    calls++;
    check(!e && kind == 2 && !z, "T0Demo_Classify after the long jump: the module is called as before",
          "%s: %u", msg(e), kind);

    /* ---- the SWI dispatcher's edges ---- */
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, XT0Demo_Sum + 5);
    calls++;
    e = s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
    check(e && e->errnum == ROS_ERR_NO_SUCH_SWI && ros_addr(e) == T0DEMO_ErrorBadSWI,
          "T0Demo SWI 5: the module's own BadSWI error", "%s", msg(e));

    ros_cpu_enter(&s);
    ros_swi(&s, ROS_X_BIT | 0x4C000);
    e = s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
    check(e && e->errnum == ROS_ERR_NO_SUCH_SWI && strcmp(e->errmess, "SWI &4C000 not known") == 0,
          "SWI &4C000: not known, error &1E6", "%s", msg(e));

    /* OS_CallASWI, OS_CallASWIR12: the SWI in R10 or R12 as if called.
     * The called SWI's own X bit decides, and R10 and R12 are kept. */
    ros_cpu_enter(&s);
    s.r[0] = 150, s.r[10] = XT0Demo_Classify;
    ros_swi(&s, ROS_X_BIT | OS_CALLASWI);
    calls++;
    int via10 = !s.v && s.r[0] == 3 && s.r[10] == XT0Demo_Classify;
    ros_cpu_enter(&s);
    s.r[12] = ROS_X_BIT | 0x4C000;
    ros_swi(&s, OS_CALLASWIR12);
    e = s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
    int via12 = e && e->errnum == ROS_ERR_NO_SUCH_SWI && s.r[12] == (ROS_X_BIT | 0x4C000);
    check(via10 && via12, "OS_CallASWI, OS_CallASWIR12: the SWI in R10 or R12, its own X bit "
          "deciding", "%s: R0 %u", msg(e), s.r[0]);

    /* ---- raising: non-X SWIs and the typed raising form ---- */
    struct ros_handler h;
    uint32_t sp = ros_svc_sp;
    if (ROS_TRY(&h)) {
        ros_cpu_enter(&s);
        ros_swi(&s, T0Demo_Fail);
        ros_handler_pop(&h);
        check(0, "T0Demo_Fail without X: raised", "it returned");
    } else {
        check(h.error->errnum == 0xC0000, "T0Demo_Fail without X: raised to the handler", "%s",
              msg(h.error));
    }
    calls++;
    if (ROS_TRY(&h)) {
        t0demo_fail();
        ros_handler_pop(&h);
        check(0, "t0demo_fail(): raised", "it returned");
    } else {
        check(h.error->errnum == 0xC0000, "t0demo_fail(): the typed call raises", "%s",
              msg(h.error));
    }
    calls++;

    /* ---- the code dispatcher refuses what nothing registered ---- */
    if (ROS_TRY(&h)) {
        ros_cpu_enter(&s);
        ros_call(&s, T0DEMO_SumLoop);
        ros_handler_pop(&h);
        check(0, "dispatcher: SumLoop is not an entry", "it was called");
    } else {
        check(h.error->errnum == ROS_ERR_BAD_ADDRESS,
              "dispatcher: a label nothing takes the address of is no entry", "%s",
              msg(h.error));
    }
    check(ros_svc_sp == sp && sp == ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE,
          "SVC stack: flat again after every raise", "&%08X", ros_svc_sp);

    /* ---- the RMA ---- */
    e = xos_module_free(words);
    check(!e, "OS_Module 7: the block freed", "%s", msg(e));
    e = xos_module_free(words);
    check(e && e->errnum == ROS_ERR_NOT_A_HEAP_BLOCK, "OS_Module 7 again: \"Not a heap block\"",
          "%s", msg(e));

    /* ---- speed: SWI round trips, and a compiled loop over 4 MB ---- */
    enum { ROUNDS = 1000000, WORDS = 1 << 20 };
    double t0 = seconds();
    uint32_t sink = 0;
    for (int i = 0; i < ROUNDS; i++) {
        uint32_t kind;
        xt0demo_classify(i & 0xFF, &kind, NULL);
        sink += kind;
    }
    double t1 = seconds();
    calls += ROUNDS;

    uint32_t *big = NULL;
    uint64_t want = 0;
    e = xos_module_claim(WORDS * 4, (void **)&big);
    if (!e) {
        for (uint32_t i = 0; i < WORDS; i++) {
            big[i] = i * 2654435761u;
            want += big[i];
        }
    }
    double t2 = seconds();
    if (!e)
        e = xt0demo_sum(big, WORDS, &lo, &hi);
    double t3 = seconds();
    calls++;
    check(!e && ((uint64_t)hi << 32 | lo) == want, "T0Demo_Sum over 4 MB of RMA: the exact total",
          "%s: &%08X:%08X, expected &%016llX", msg(e), hi, lo, (unsigned long long)want);
    if (big)
        xos_module_free(big);
    ros_console_printf("  ..    T0Demo_Classify round trip: %.1f ns (native -> compiled -> native)\n",
                       (t1 - t0) * 1e9 / ROUNDS);
    ros_console_printf("  ..    T0Demo_Sum compiled loop: %.2f ns per word\n",
                       (t3 - t2) * 1e9 / WORDS);
    (void)sink;

    check(ros_ld32(ws) == calls, "T0Demo counted every SWI it was given",
          "count %u, expected %u", ros_ld32(ws), calls);

    ros_selftest_buffers();
    ros_selftest_fp();
    ros_selftest_int();
    ros_selftest_graphicsv();
    ros_selftest_vdu();
    ros_selftest_internet();
    ros_selftest_resolver();
    ros_selftest_netcmds();
    ros_selftest_acornssl();
    ros_selftest_fetch();
    ros_selftest_pty();
    ros_selftest_sshd();
    ros_selftest_smb();
    ros_selftest_boxtools();
    ros_selftest_background();
    ros_selftest_ticker();
    ros_selftest_tasks();
    ros_selftest_vdisplay();
    ros_selftest_amb();
    ros_selftest_dynarea();
    ros_selftest_wimpswis();
    ros_selftest_callback();
    ros_selftest_pinboard();
    ros_selftest_drag();
    ros_selftest_environment();
    ros_selftest_input();
    ros_selftest_osbyte();
    ros_selftest_cmos();
    ros_selftest_kernelswis();
    ros_selftest_vm();
    ros_selftest_convert();
    ros_selftest_sysvars();
    ros_selftest_args();
    ros_selftest_oscli();
    ros_selftest_messagetrans();
    ros_selftest_territory();
    ros_selftest_international();
    ros_selftest_files();
    ros_selftest_imagefs();
    ros_selftest_program();
    ros_selftest_sclib();
    ros_selftest_capps();
    ros_selftest_filterhourglass();
    ros_selftest_basicvfp();
    ros_selftest_modules();
    ros_selftest_taskmanager();
    ros_selftest_filer();
    ros_selftest_hostfsfiler();
    ros_selftest_display();
    ros_selftest_deskclock();
    ros_selftest_deskmeter();
    ros_selftest_speakers();
    ros_selftest_machine();
    ros_selftest_desktop();
    ros_selftest_resfiler();
    ros_selftest_sprutils();
    ros_selftest_gdraw();
    ros_selftest_smooth();
    ros_selftest_fontfiles();
    ros_selftest_squash();
    ros_selftest_zlib();
    ros_selftest_compresspng();
    ros_selftest_compressjpeg();
    ros_selftest_worker();
    ros_selftest_pdriver();
    ros_selftest_armrun();
    ros_selftest_shadow();
    ros_selftest_jpegplot();
    ros_selftest_mimemap();
    ros_selftest_portable();
    ros_selftest_sharedsnd();
    ros_selftest_sound();
    ros_selftest_taskwindow();
    ros_selftest_shellcli();
    ros_selftest_bootcmds();

done:
    if (t->failed)
        ros_console_printf("ROSGD-SELFTEST-FAIL %u of %u checks failed\n", t->failed, t->count);
    else
        ros_console_printf("ROSGD-SELFTEST-PASS %u/%u\n", t->passed, t->count);
    return t->failed == 0;
}
