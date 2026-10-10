/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_capps.c: C applications, the loader and the gate (capp.h).
 *
 * The probe (tests/capps/probe.c) is an x32 image linked at &8000, filed as
 * an Absolute.  In the box, *Run of it must start it as FileSwitch starts
 * an Absolute.  The command line is as typed, for OS_GetEnv.  The CAO is
 * &8000.  The memory limit is the top of the task's application space.
 * *Run must enter the image with its stack there and a return that exits.
 * It must give its SWIs the gate's contract: R0-R9, the flags and an X
 * SWI's error back, and its MXCSR kept.  The probe leaves by returning, by
 * OS_Exit, or by an error its handler ends it for.  Each brings *Run back.
 * An image bigger than application space is refused before anything
 * starts.  An &FF8 file that is ARM code is refused as before.  An x32
 * image that is not a ROSGD one of version 1 is refused, box and hosted,
 * before anything of it runs.  The cases are another OS/ABI (a Linux
 * program), no ROSGD note, another version and unknown flags.  The
 * start-up values are 5.30's, as bigmac7 recorded them for an ARM Absolute
 * (tests/capps/farm).  The registers at entry are the contract's
 * (capp.h).  OS_SynchroniseCodeAreas takes both its forms.
 *
 * Hosted, there is no 32-bit-pointer code.  The same *Run is refused, and
 * the refusal is printed.  The files live in a directory of the test's own,
 * as the disc CAppTest.
 *
 * Entries into x32 code: the entries probe (tests/capps/entries.c) is run
 * with a stand-in for the library's and a module's code
 * (tests/capps/xlib.c).  The stand-in is mapped at &77000000, the top of
 * the C ROM (clear of the ROM library's image at &72200000), as a library,
 * and copied into the RMA as a module.  The dispatcher calls each kind
 * with the register block.  The static base (%gs) is the task's for its
 * own code, R12 for the library's and the module's for the module's, and
 * it is back after each.  A nested UpCall makes SWIs and nests again.  An
 * error raised inside the library's handler is delivered to the program's
 * error handler, flattened, with the task's base back.  That handler
 * longjmps into the program.  OS_Exit is delivered to its exit handler.
 * Every thread the runtime starts has a signal stack (hosted too).
 *
 * The C library's image: a stand-in library is linked by roscc
 * --clib-image as the ROM library is, and mapped at &77000000.  Its
 * header, entry table and template are read (clibimage.h).  Its checks run
 * through the dispatcher, with two clients' blocks made from the template.
 *
 * Per-task state: the pertask probe (tests/capps/pertask.c).  The runtime
 * keeps this for its task (struct ros_task): the base it registered, its
 * code, the memory it runs in, the sp it last called the OS from, and the
 * native stack of its SWIs.  Its UpCall handler is found by the memory at
 * &8000, not by the thread.  The handler is called from the background
 * thread while the program waits in a SWI.  It is entered with the task's
 * base and makes a SWI of its own.  With no memory at &8000, or a copy of
 * the task's in other memory, the same address is no one's code.  Two
 * tasks, two images, the Wimp between them: tests/capps/baton.py (make
 * boxtest).  The registration the probes use is the test hooks' stand-in
 * for SharedCLibrary's (ros_selftest_capps_hooks, selftest.h).
 *
 * Faults (fault.h): the fault probe (tests/capps/faults, whose log
 * tests/capps/faults/faults.py compares with RISC OS 5.30's) gives each
 * case its 5.30 error, the register block and a report.  A stack overflow
 * into a guard (the self-test standing in for the library's registration)
 * is delivered on the emergency stack.  x86's division overflows in every
 * form are finished as ARM's.  The library's and a module's code fault
 * into a C handler of the runtime's, and the entry is unwound.  The
 * runtime faulting in a SWI on a bad pointer is an error too.  Its block
 * is in the kernel's own when the program's is unusable.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef ROS_ARENA_HOSTED
#include <sys/auxv.h>
#endif

#include "capp_images.h"
#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/capp.h"
#include "rosgd/clibimage.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/fault.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"
#include "selftest.h"
#include "sharedclib.h"

#define check ros_check

/* The probes this build carries (capp_images.h, the Makefile's).  The
 * x86-64 box has x32's whole set.  The Apple Silicon box has only the
 * start-up probe, built A64X32 so far.  The entries, library, registration
 * and fault probes are x86 code. */
#ifdef CAPP_ENTRIES_SIZE
#define X32_PROBES 1
#else
#define X32_PROBES 0
#endif

#define WRCHV 0x03u

static char out[16384];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    char c = (char)s->r[0];
    if (c == '\r')
        return ROS_VECTOR_CLAIM;
    if (outn < sizeof out - 1)
        out[outn++] = c;
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* A command, its VDU output kept in out; the error, or NULL */
static const os_error *run(const char *line)
{
    static char err[260];
    size_t n = strlen(line);
    char *c = ros_rma_alloc((uint32_t)n + 1);
    memcpy(c, line, n);
    c[n] = '\r';
    outn = 0, out[0] = 0;
    ros_vector_claim_native(WRCHV, wrch, 0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(c);
    ros_swi(&s, XOS_CLI);
    ros_vector_release_native(WRCHV, wrch, 0);
    ros_rma_free(c);
    if (!s.v)
        return NULL;
    memcpy(err, ros_ptr(s.r[0]), sizeof err);
    return (const os_error *)err;
}

/* ---- the test hooks: SharedCLibrary's stand-ins (selftest.h) --------------------------- */

#define R_SETBASE 0x52324142u       /* "R2AB" */
#define R_STATE   0x52335453u       /* "R3TS" */

/* R0 "R2AB": registration, R1 the calling task's base (ros_capp_task_base).
 * R0 "R3TS": what the runtime keeps for the task holding the baton.  R1 is
 * its base.  R2/R3 are the base in force on this thread (%gs, low and
 * high).  R4 is its saved sp.  R5/R6 are its native sp (low, high).  R7 is
 * its task id.  Each hook claims the call and sets R0 = 0. */
static int capps_hooks(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == R_SETBASE) {
        ros_capp_task_base(s->r[1]);
        s->r[0] = 0;
        return ROS_VECTOR_CLAIM;
    }
    if (s->r[0] != R_STATE)
        return ROS_VECTOR_PASS;
    struct ros_task *t = ros_task_current();
    const struct ros_capp_task *c = t ? ros_task_capp(t) : NULL;
    uint64_t gs = ros_capp_gs();
    s->r[1] = c ? c->base : 0;
    s->r[2] = (uint32_t)gs, s->r[3] = (uint32_t)(gs >> 32);
    s->r[4] = c ? (uint32_t)c->gate.saved_sp : 0;
    s->r[5] = c ? (uint32_t)c->gate.native_sp : 0;
    s->r[6] = c ? (uint32_t)(c->gate.native_sp >> 32) : 0;
    s->r[7] = t ? ros_task_id(t) : 0xFFFFFFFFu;
    s->r[0] = 0;
    return ROS_VECTOR_CLAIM;
}

void ros_selftest_capps_hooks(int on)
{
    if (on)
        ros_vector_claim_native(ROS_UPCALLV, capps_hooks, 0);
    else
        ros_vector_release_native(ROS_UPCALLV, capps_hooks, 0);
}

static void put(const char *dir, const char *name, const void *data, size_t n)
{
    char p[400];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

#if ROS_CAPP_NATIVE
/* The value after `label` in the output, as the probe prints it ("&XXXXXXXX") */
static uint32_t value(const char *label)
{
    const char *p = strstr(out, label);
    if (!p)
        return 0xDEADDEADu;
    p = strchr(p + strlen(label), '&');
    return p ? (uint32_t)strtoul(p + 1, NULL, 16) : 0xDEADDEADu;
}

/* The bracketed text after `label` */
static const char *text(const char *label)
{
    static char t[256];
    const char *p = strstr(out, label);
    const char *a = p ? strchr(p, '[') : NULL, *b = a ? strchr(a, ']') : NULL;
    size_t n = a && b ? (size_t)(b - a - 1) : 0;
    if (n >= sizeof t)
        n = sizeof t - 1;
    memcpy(t, a ? a + 1 : "", n);
    t[n] = 0;
    return t;
}

/* The permissions /proc/self/maps gives the mapping holding addr: "rwxs" */
static const char *perms(uint32_t addr)
{
    static char pm[8];
    strcpy(pm, "none");
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char p[8];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, p) == 3 && addr >= lo && addr < hi) {
            strcpy(pm, p);
            break;
        }
    }
    if (f)
        fclose(f);
    return pm;
}
#endif

static const unsigned char arm_absolute[] = { 0x0E, 0xF0, 0xA0, 0xE1 };  /* MOV pc, lr */

/* The probe with one byte or word changed: at `off` from the start, or
 * from the ROSGD note's descriptor (version +0, flags +4) if note. */
static void put_variant(const char *dir, const char *name, int note, uint32_t off, uint32_t value,
                        int word)
{
    unsigned char *b = malloc(CAPP_PROBE_SIZE);
    if (!b)
        return;
    memcpy(b, capp_probe, CAPP_PROBE_SIZE);
    if (note) {
        int found = 0;
        for (uint32_t i = 0; i + 6 < 512 && i + 6 < CAPP_PROBE_SIZE; i++)
            if (!memcmp(b + i, "ROSGD", 6)) {
                off += i + 8;                       /* the name, padded to 8 */
                found = 1;
                break;
            }
        if (!found) {                               /* no file: its refusal check fails */
            free(b);
            return;
        }
    }
    if (word)
        memcpy(b + off, &value, 4);
    else
        b[off] = (unsigned char)value;
    put(dir, name, b, CAPP_PROBE_SIZE);
    free(b);
}

static const struct {
    const char *file, *why;
} refused[] = {
    { "linux", "its OS/ABI is 0, not 255" },
    { "nonote", "it has no ROSGD note" },
    { "version2", "a ROSGD image of version 2; this loader runs version 1" },
    { "flags", "flags &1, which a version 1 loader does not know" },
};

/* ---- a signal stack on every thread ---------------------------------------------------- */

static int has_signal_stack(void)
{
    stack_t ss;
    return sigaltstack(NULL, &ss) == 0 && !(ss.ss_flags & SS_DISABLE) && ss.ss_size >= 65536;
}

static int task_alt = -1;
static void alt_task(void *arg)
{
    (void)arg;
    task_alt = has_signal_stack();
}

static volatile int bg_alt = -1;
static pthread_t bg_thread;
static void alt_work(void *arg, uint32_t info)
{
    (void)arg, (void)info;
    bg_thread = pthread_self();
    bg_alt = has_signal_stack();
}

static void signal_stacks(void)
{
    struct ros_task *t = ros_task_create(0, alt_task, NULL);
    if (t) {
        ros_task_switch(t);                     /* it runs, ends, and the baton comes back */
        ros_task_destroy(t);
    }
    /* Posted with the lock let go, so the background thread runs it, not
     * this thread's safe point */
    bg_alt = -1;
    unsigned d = ros_blocking_begin();
    ros_post(alt_work, NULL, 0);
    for (unsigned i = 0; i < 400 && bg_alt < 0; i++) {
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
    ros_blocking_end(d);
#ifdef ROS_ARENA_HOSTED
    int main_alt = 1;                           /* (hosttest's main thread is not the runtime's) */
#else
    int main_alt = has_signal_stack();
#endif
    check(main_alt && task_alt == 1 && bg_alt == 1 && !pthread_equal(bg_thread, pthread_self()),
          "C applications: every thread has a signal stack -- /init's, a task's, the "
          "background thread's", "main %d task %d background %d", main_alt, task_alt, bg_alt);
}

#if ROS_CAPP_NATIVE && !ROS_CAPP_A64
/* ---- entries into x32 code ---------------------------------------------------------- */

/* The n-th line (from 0) of the output that begins with prefix, or "",
 * copied into l (256 bytes) */
typedef char line_buf[256];
static const char *line_of(char *l, const char *prefix, unsigned n)
{
    size_t k = strlen(prefix);
    for (const char *p = out; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= k && !memcmp(p, prefix, k) && n-- == 0) {
            if (len >= sizeof(line_buf))
                len = sizeof(line_buf) - 1;
            memcpy(l, p, len);
            l[len] = 0;
            return l;
        }
        p += len + (e != NULL);
    }
    l[0] = 0;
    return l;
}

/* The value after key in a line ("key &XXXXXXXX") */
static uint32_t val(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    if (!p)
        return 0xDEADDEADu;
    p = strchr(p + strlen(key), '&');
    return p ? (uint32_t)strtoul(p + 1, NULL, 16) : 0xDEADDEADu;
}

#define R_NEST    0x5232414Eu
#define R_INNER   0x52324149u
#define R_LIB     0x5232414Cu
#define R_MODULE  0x5232414Du
#define R_FAIL    0x52324146u

/* The stand-in library's image, its PT_LOAD segments put at dest as they
 * are at &77000000; its extent, or 0 */
static uint32_t load_xlib(unsigned char *dest, uint32_t room)
{
    const unsigned char *img = capp_xlib;
    uint32_t phoff, extent = 0;
    uint16_t phnum;
    memcpy(&phoff, img + 28, 4);
    memcpy(&phnum, img + 44, 2);
    for (unsigned i = 0; i < phnum; i++) {
        uint32_t ph[8];
        memcpy(ph, img + phoff + 32 * i, sizeof ph);
        if (ph[0] != 1 || ph[5] == 0)
            continue;
        uint32_t at = ph[2] - ROS_CROM_TEST;
        if (at + ph[5] > room || ph[1] + ph[4] > CAPP_XLIB_SIZE)
            return 0;
        memcpy(dest + at, img + ph[1], ph[4]);
        memset(dest + at + ph[4], 0, ph[5] - ph[4]);
        if (at + ph[5] > extent)
            extent = at + ph[5];
    }
    return extent;
}

static void entries(void)
{
    /* The library, in the C ROM; the module, a copy in the RMA */
    const uint32_t lib = ROS_CROM_TEST, room = 0x10000;
    int mapped = ros_arena_map_region("capptest-xlib", lib, room) == 0;
    uint32_t extent = mapped ? load_xlib(ros_ptr(lib), room) : 0;
    if (mapped)
        mprotect(ros_ptr(lib), room, PROT_READ | PROT_EXEC);
    unsigned char *mblock = ros_rma_alloc(room + 16);
    uint32_t mod = mblock ? (ros_addr(mblock) + 15) & ~15u : 0;
    uint32_t *mbase = ros_rma_alloc(16), *lbase = ros_rma_alloc(16);
    if (mod)
        load_xlib(ros_ptr(mod), room);
    uint32_t off_regs = extent ? ros_ld32(lib + 12) : 0, off_claim = extent ? ros_ld32(lib + 8) : 0;
    int ready = extent && mod && mbase && lbase && ros_ld32(lib) == 0x42494C58u;   /* "XLIB" */
    check(ready, "C applications: the stand-in library at &77000000 and its copy in the RMA",
          "mapped %d extent &%X module &%X", mapped, extent, mod);
    if (!ready)
        goto out;
    mbase[0] = ros_addr(mbase);                 /* each base's self word */
    lbase[0] = ros_addr(lbase);
    ros_capp_code_add(lib, lib + extent, ROS_CAPP_LIBRARY, 0);
    ros_capp_code_add(mod, mod + extent, ROS_CAPP_MODULE, ros_addr(mbase));

    /* Called by the dispatcher with the register block: the library's with
     * its R12 as the base, the module's with the module's; R0-R15 and the
     * flags back; the base as it was after */
    uint64_t gs0 = ros_capp_gs();
    unsigned mx = __builtin_ia32_stmxcsr();
    struct ros_cpu s;
    ros_cpu_enter(&s);
    for (unsigned i = 0; i < 12; i++)
        s.r[i] = 0x1000u * i;
    s.r[12] = ros_addr(lbase);
    s.n = s.z = 1;
    ros_call(&s, lib + off_regs);
    int regs_ok = 1;
    for (unsigned i = 0; i < 12; i++)
        regs_ok &= s.r[i] == 0x1000u * i + 0x100u + i;
    int lib_ok = regs_ok && s.r[12] == ros_addr(lbase) && s.c && s.v && !s.n && !s.z &&
                 s.r[15] == ROS_RETURN_TO_NATIVE && s.r[14] == ROS_RETURN_TO_NATIVE &&
                 ros_capp_gs() == gs0;
    ros_cpu_enter(&s);
    s.r[12] = 0x77;
    ros_call(&s, mod + off_regs);
    int mod_ok = s.r[12] == ros_addr(mbase) && s.r[0] == 0x100 && ros_capp_gs() == gs0 &&
                 __builtin_ia32_stmxcsr() == mx;
    check(lib_ok && mod_ok && ros_capp_code(lib + off_regs) == ROS_CAPP_LIBRARY &&
              ros_capp_code(mod + off_regs) == ROS_CAPP_MODULE && !ros_capp_code(lib - 4),
          "C applications: the dispatcher enters x32 library and module code with the register "
          "block -- R0-R15 and the flags back, the base R12's or the module's, then as it was",
          "library %d (R12 &%X) module %d (base &%X)", lib_ok, s.r[12], mod_ok, ros_addr(mbase));
    ros_console_printf("        the base by %s\n",
                       getauxval(AT_HWCAP2) & 2 ? "wrgsbase (FSGSBASE)" : "arch_prctl");

    /* An error raised inside x32 code that a C handler of the runtime's
     * catches: the entry is unwound, the base put back, and the next entry
     * works */
    struct ros_handler h;
    const os_error *caught = NULL;
    uint64_t gs_in = 0;
    ros_cpu_enter(&s);
    s.r[0] = R_FAIL;
    s.r[12] = ros_addr(lbase);
    if (ROS_TRY(&h)) {
        ros_call(&s, lib + ros_ld32(lib + 4));
        ros_handler_pop(&h);
    } else {
        caught = h.error;
        gs_in = ros_capp_gs();
    }
    ros_cpu_enter(&s);
    s.r[12] = 0x77;
    ros_call(&s, mod + off_regs);
    check(caught && caught->errnum == 0x2A2A && gs_in == gs0 && s.r[12] == ros_addr(mbase) &&
              ros_capp_gs() == gs0,
          "C applications: an error out of x32 code to a C handler of the runtime's unwinds the "
          "entry -- the base put back -- and the next entry works", "%s, base &%llX",
          caught ? caught->errmess : "not caught", (unsigned long long)gs_in);

    /* Faults in the library's and a module's code: each is an error to
     * the runtime's C handler, the entry is unwound and the base is back.
     * The report says whose code it was.  An overflowing division is
     * finished as ARM's. */
    uint32_t off_fault = ros_ld32(lib + 16);
    unsigned faults0 = ros_fault_count();
    const os_error *lfe = NULL, *mfe = NULL, *lde = NULL;
    char lrep[600] = "", mrep[600] = "";
    uint32_t mode_dump = 0xFF;
    ros_cpu_enter(&s);
    s.r[0] = ROS_DA_LIMIT, s.r[1] = 0, s.r[12] = ros_addr(lbase);      /* never mapped */
    if (ROS_TRY(&h)) {
        ros_call(&s, lib + off_fault);
        ros_handler_pop(&h);
    } else {
        lfe = h.error;
        snprintf(lrep, sizeof lrep, "%s", ros_fault_last_report());
    }
    uint64_t gs_lib = ros_capp_gs();
    ros_cpu_enter(&s);
    s.r[1] = 2, s.r[12] = 0x77;                                         /* ud2 */
    if (ROS_TRY(&h)) {
        ros_call(&s, mod + off_fault);
        ros_handler_pop(&h);
    } else {
        mfe = h.error;
        snprintf(mrep, sizeof mrep, "%s", ros_fault_last_report());
        uint32_t at = ros_ld32(ROS_ZEROPAGE + 0x958);
        mode_dump = ros_ld32(at + 64) & 0x1F;
    }
    ros_cpu_enter(&s);
    s.r[0] = 7, s.r[1] = 1, s.r[2] = 0, s.r[12] = ros_addr(lbase);      /* 7 / 0 */
    if (ROS_TRY(&h)) {
        ros_call(&s, lib + off_fault);
        ros_handler_pop(&h);
    } else {
        lde = h.error;
    }
    ros_cpu_enter(&s);
    s.r[0] = 0x80000000u, s.r[1] = 1, s.r[2] = 0xFFFFFFFFu, s.r[12] = ros_addr(lbase);
    ros_call(&s, lib + off_fault);                                      /* INT_MIN / -1 */
    uint32_t quotient = s.r[0];
    /* A module's code running away down the SVC stack: its guard page */
    const os_error *sve = NULL;
    char srep[600] = "", gpk2[8], spk[8];
    uint32_t svc_before = ros_svc_sp;
    ros_cpu_enter(&s);
    s.r[1] = 3, s.r[12] = 0x77;
    if (ROS_TRY(&h)) {
        ros_call(&s, mod + off_fault);
        ros_handler_pop(&h);
    } else {
        sve = h.error;
        snprintf(srep, sizeof srep, "%s", ros_fault_last_report());
    }
    strcpy(gpk2, perms(ROS_SVCSTACK_GUARD_AT));
    strcpy(spk, perms(ROS_SVCSTACK_GUARD_AT + ROS_SVCSTACK_GUARD));
    ros_cpu_enter(&s);
    s.r[12] = 0x77;
    ros_call(&s, mod + off_regs);
    char want[80];
    snprintf(want, sizeof want, "Internal error: abort on data transfer at &%08X", lib + off_fault);
    int lf_ok = lfe && lfe->errnum == ROS_ERR_DATA_ABORT &&
                !strncmp(lfe->errmess, want, strlen(want) - 2) &&
                strstr(lrep, "data abort in the library at &") &&
                strstr(lrep, "(the library+&") && gs_lib == gs0;
    int mf_ok = mfe && mfe->errnum == ROS_ERR_UNDEFINED_INSTRUCTION &&
                strstr(mfe->errmess, "Internal error: undefined instruction at &") &&
                strstr(mrep, "undefined instruction in a module at &") && mode_dump == ROS_MODE_SVC;
    check(lf_ok && mf_ok && lde && lde->errnum == ROS_ERR_DIVIDE_BY_ZERO &&
              !strcmp(lde->errmess, "Divide by zero") && quotient == 0x80000000u &&
              s.r[12] == ros_addr(mbase) && ros_capp_gs() == gs0 &&
              ros_fault_count() - faults0 >= 3,
          "C applications: faults in the library's and a module's x32 code are 5.30's errors "
          "to the runtime's C handler -- a data abort, an undefined instruction (the block's "
          "mode SVC), x / 0 -- reported as whose; INT_MIN / -1 finished as ARM's; the entry "
          "unwound, the base back", "library %s | module %s (mode &%X) | %s | quotient &%X",
          lfe ? lfe->errmess : "none", mfe ? mfe->errmess : "none", mode_dump,
          lde ? lde->errmess : "none", quotient);
    check(sve && sve->errnum == ROS_ERR_DATA_ABORT &&
              strstr(srep, "data abort (SVC stack overflow) in a module at &") &&
              !strcmp(gpk2, "---s") && !strcmp(spk, "rw-s") && ros_svc_sp == svc_before &&
              ros_capp_gs() == gs0 && ros_fault_count() - faults0 == 4,
          "C applications: x32 code running away down the SVC stack faults in its guard page "
          "(the lowest, inaccessible) -- a data abort to the runtime's C handler, the SVC "
          "stack and base as they were", "%s; guard %s, above it %s",
          sve ? sve->errmess : "none", gpk2, spk);

    /* The probe: its own handlers, the library's, the module's claimant */
    ros_selftest_capps_hooks(1);
    ros_vector_claim(ROS_UPCALLV, mod + off_claim, 0xAB12, 0);
    const os_error *e = run("Run HostFS::CAppTest.$.entries");
    ros_console_printf("%s", out);
    line_buf l, u1, u2, ret, lh, lr, mc, mr, lf, eh, rc, ag, iu;
    line_of(l, "base set:", 0);
    uint32_t task = val(l, "task base");
    uint32_t client = val(line_of(lr, "library UpCall returned:", 0), "client");
    check(!e && val(l, "R0") == 0 && val(l, "in force") == task && task >= ROS_APP_BASE &&
              task < ROS_RMA_BASE,
          "C applications: the task's base set as registration sets it, and in force", "%s: %s",
          e ? e->errmess : "ran", l);

    line_of(u1, "upcall handler:", 0);
    line_of(u2, "upcall handler:", 1);
    line_of(ret, "UpCall returned:", 0);
    line_of(iu, "inner UpCall:", 0);
    uint32_t sp1 = val(u1, "sp"), sp2 = val(u2, "sp");
    check(val(u1, "R0") == R_NEST && val(u1, "R12") == 0x12121212u && val(u1, "base") == task &&
              val(u1, "depth") == 1 && val(u2, "R0") == R_INNER && val(u2, "depth") == 2 &&
              val(u2, "base") == task && sp1 - ROS_SVCSTACK_BASE < ROS_SVCSTACK_SIZE &&
              sp2 < sp1 && sp1 - sp2 < ROS_SVCSTACK_SIZE && val(iu, "R0") == 0 &&
              val(iu, "R1") == 0x100 && val(ret, "R0") == 0 && val(ret, "R1") == 0x101 &&
              val(ret, "base") == task,
          "C applications: a nested UpCall into the program's own code, on the SVC stack, makes "
          "SWIs and an UpCall that nests again; R0/R1 come back claimed", "%s | %s | %s", u1, u2,
          ret);

    line_of(lh, "library handler:", 0);
    check(val(lh, "R0") == R_LIB && val(lh, "R12") == client && val(lh, "base") == client &&
              client != task && client >= ROS_APP_BASE && val(lr, "R0") == 0 &&
              val(lr, " base") == task,
          "C applications: a library handler is entered with its R12 as the base; the task's "
          "base is back after", "%s | %s", lh, lr);

    line_of(mc, "module claimant:", 0);
    line_of(mr, "module UpCall returned:", 0);
    check(val(mc, "R12") == 0xAB12 && val(mc, "base") == ros_addr(mbase) && val(mr, "R0") == 0 &&
              val(mr, "R1") == 0x600D && val(mr, "base") == task,
          "C applications: a module's vector claimant runs with the module's base and claims "
          "through the stack; the task's base is back after", "%s | %s", mc, mr);

    line_of(lf, "library handler:", 1);
    line_of(eh, "error handler:", 0);
    line_of(rc, "recovered by longjmp:", 0);
    uint32_t limit = val(line_of(ag, "after: OS_GetEnv", 0), "R1");
    check(val(lf, "R0") == R_FAIL && val(lf, "base") == client && val(eh, "R0") == 0xE4404 &&
              val(eh, "R12") == 0xE4404 && val(eh, "base") == task &&
              val(eh, "psr") == ROS_MODE_USR && val(eh, "sp") > ROS_APP_BASE &&
              val(eh, "sp") < limit && val(rc, "base") == task && val(rc, "error") == 0x2A2A &&
              strstr(rc, "[Library handler failed]") && val(rc, "frame") == 1 &&
              limit > ROS_APP_BASE && !strstr(out, "the error came back") &&
              strstr(out, "returning from the entry"),
          "C applications: an error in a library handler is delivered to the program's error "
          "handler -- flattened, user mode, its stack, the task's base -- which longjmps back "
          "into the program; SWIs go on", "%s | %s | %s", lf, eh, rc);

    /* OS_Exit, delivered to the exit handler, which returns: the end */
    e = run("HostFS::CAppTest.$.entries exit");
    line_buf xh;
    line_of(xh, "exit handler:", 0);
    check(!e && val(xh, "R0") == 0 && val(xh, "R1") == 0x58454241u && val(xh, "R2") == 7 &&
              val(xh, "R12") == 0xE11 && val(xh, "base") == task &&
              val(xh, "psr") == ROS_MODE_USR && strstr(out, "leaving by OS_Exit") &&
              !strstr(out, "OS_Exit returned") && !strstr(out, "returning from the entry"),
          "C applications: OS_Exit is delivered to the program's exit handler as the kernel "
          "enters it -- R0 0, R1 \"ABEX\", R2 the return code, R12 its own -- with the task's "
          "base; its return ends the program and *Run comes back", "%s: %s",
          e ? e->errmess : "ran", xh);
    check(ros_capp_gs() == gs0 && __builtin_ia32_stmxcsr() == mx,
          "C applications: after the programs, the runtime's base and MXCSR as they were",
          "gs &%llX (was &%llX)", (unsigned long long)ros_capp_gs(), (unsigned long long)gs0);

    ros_vector_release(ROS_UPCALLV, mod + off_claim, 0xAB12);
    ros_selftest_capps_hooks(0);
    ros_capp_code_remove(lib);
    ros_capp_code_remove(mod);
out:
    if (mapped)
        ros_arena_unmap(lib, room);
    ros_rma_free(mblock);
    ros_rma_free(mbase);
    ros_rma_free(lbase);
}

/* ---- the C library's image ---------------------------------------------------------- */

/* The stand-in C library, linked by roscc as the ROM library is
 * (--clib-image).  It is mapped at &77000000 (the ROM library's own image
 * is at &72200000, SharedCLibrary's).  Its header, Lib$$Init and template
 * are read (clibimage.h).  Its checks run through the dispatcher, with two
 * clients' blocks made from the template.  Each entry is entered as
 * library code with R12 the client's base (clibcall.c's lib_call, which
 * calls the function in R0).  The TP offsets are ld.lld's for the
 * stand-in's objects: calls -&120, errno_x -&11C, iobuf -&110, last -&10.
 * roscc's tests hold them to ld.lld's link. */
#define STAND_ENTRIES 7
enum { SETERRNO, GETERRNO, CALLS, BUF, COPY, LAST, CALL };

static uint32_t stand_vec[STAND_ENTRIES];

/* fn(a) in the client whose base is tp, through the dispatcher */
static uint32_t stand_call(uint32_t tp, unsigned fn, uint32_t a)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = stand_vec[fn];
    s.r[1] = a;
    s.r[12] = tp;
    ros_call(&s, stand_vec[CALL]);
    return s.r[0];
}

static void clib_image(void)
{
    const unsigned char *img = capp_clibstand;
    unsigned char *ba = NULL, *bb = NULL;
    char *msg = NULL;
    uint32_t phoff, note = 0, note_size = 0, load_at = 0, load_off = 0, filesz = 0, memsz = 0, flags = 0;
    uint16_t phnum;
    memcpy(&phoff, img + 28, 4);
    memcpy(&phnum, img + 44, 2);
    for (unsigned i = 0; i < phnum; i++) {
        uint32_t ph[8];
        memcpy(ph, img + phoff + 32 * i, sizeof ph);
        if (ph[0] == 1)
            load_off = ph[1], load_at = ph[2], filesz = ph[4], memsz = ph[5], flags = ph[6];
        if (ph[0] == 4)
            note = ph[1], note_size = ph[4];
    }
    uint32_t note_flags = 0;
    if (note)
        memcpy(&note_flags, img + note + 24, 4);
    const uint32_t lib = ROS_CROM_TEST, room = (memsz + 0xFFF) & ~0xFFFu;
    int fits = load_at == lib && load_off == 0 && filesz == memsz && filesz <= CAPP_CLIBSTAND_SIZE && room;
    int mapped = fits && ros_arena_map_region("capptest-clib", lib, room) == 0;
    if (mapped) {
        memcpy(ros_ptr(lib), img, filesz);
        mprotect(ros_ptr(lib), room, PROT_READ | PROT_EXEC);
    }
    /* the header, after the note (PT_NOTE's offset and size, 4-aligned),
     * and Lib$$Init */
    struct ros_clib_header h;
    struct ros_clib_chunk c;
    memset(&h, 0, sizeof h);
    memset(&c, 0, sizeof c);
    uint32_t hat = lib + ((note + note_size + 3) & ~3u);
    if (mapped) {
        memcpy(&h, ros_ptr(hat), sizeof h);
        memcpy(&c, ros_ptr(hat + sizeof h), sizeof c);
    }
    int hdr = mapped && !memcmp(h.magic, ROS_CLIB_MAGIC, 8) && h.format == ROS_CLIB_FORMAT && h.base == lib &&
              h.size == filesz && h.chunks == 1 && c.id == 2 &&
              c.entries_end - c.entries == 4 * STAND_ENTRIES && c.data_end - c.data == h.tp_offset &&
              h.block_size == h.tp_offset + 16 && h.block_align == 16;
    int zero = hdr;
    for (uint32_t a = c.data; hdr && a < c.data_end; a++)
        zero &= ros_ld8(a) == 0;
    for (unsigned i = 0; hdr && i < STAND_ENTRIES; i++) {
        stand_vec[i] = ros_ld32(c.entries + 4 * i);
        hdr &= stand_vec[i] > hat && stand_vec[i] < lib + filesz;
    }
    check(hdr && zero && note_flags == ROS_CLIB_NOTE_FLAG && flags == 5,
          "C applications: the C library's image (A.1's stand-in, roscc --clib-image) mapped at &77000000, read "
          "only and executable -- its header, Lib$$Init (chunk 2, 7 entries) and its template (.tbss's zeros) "
          "read", "mapped %d header %d note flags %u, block &%X, TP at &%X", mapped, hdr, note_flags,
          h.block_size, h.tp_offset);
    if (!hdr)
        goto out;

    /* Two clients: each a block from the template, TP at its top, the self word */
    ba = ros_rma_alloc(h.block_size + 16), bb = ros_rma_alloc(h.block_size + 16);
    msg = ros_rma_alloc(64);
    if (!ba || !bb || !msg)
        goto out;
    uint32_t blk[2] = { (ros_addr(ba) + 15) & ~15u, (ros_addr(bb) + 15) & ~15u }, tp[2];
    for (unsigned k = 0; k < 2; k++) {
        memcpy(ros_ptr(blk[k]), ros_ptr(c.data), c.data_end - c.data);
        memset(ros_ptr(blk[k] + h.tp_offset), 0, 16);
        tp[k] = blk[k] + h.tp_offset;
        ros_st32(tp[k], tp[k]);
    }
    const uint32_t A = tp[0], B = tp[1];
    ros_capp_code_add(lib, lib + filesz, ROS_CAPP_LIBRARY, 0);
    uint64_t gs0 = ros_capp_gs();

    /* The call sequence of the earlier fsgs.c test */
    strcpy(msg, "client A");
    uint32_t copied_a = stand_call(A, COPY, ros_addr(msg));
    stand_call(A, SETERRNO, 11);
    strcpy(msg, "client B, longer");
    uint32_t copied_b = stand_call(B, COPY, ros_addr(msg));
    stand_call(B, SETERRNO, 22);
    stand_call(B, GETERRNO, 0);
    uint32_t errno_a = stand_call(A, GETERRNO, 0);
    uint32_t abuf = stand_call(A, BUF, 0), alast = stand_call(A, LAST, 0), acalls = stand_call(A, CALLS, 0);
    uint32_t bbuf = stand_call(B, BUF, 0), errno_b = stand_call(B, GETERRNO, 0),
             bcalls = stand_call(B, CALLS, 0);
    check(copied_a == 8 && copied_b == 16 && errno_a == 11 && errno_b == 22,
          "C applications: A.1 in the box, the library linked by roscc -- two clients, one image: each keeps its "
          "own errno (A 11 after B set 22)", "copied %u %u, errno A %u B %u", copied_a, copied_b, errno_a,
          errno_b);
    check(abuf == A - 0x110 && !strcmp(ros_ptr(abuf), "client A") && bbuf == B - 0x110 &&
              !strcmp(ros_ptr(bbuf), "client B, longer") && alast == abuf,
          "C applications: A.1 -- each client's buffer in its own block at TP-&110, ld.lld's offset (&static "
          "through the self word); a pointer-valued static points into its own", "A &%X [%s] last &%X, B &%X "
          "[%s]", abuf, (const char *)ros_ptr(abuf), alast, bbuf, (const char *)ros_ptr(bbuf));
    check(acalls == 4 && bcalls == 5 && ros_ld32(A - 0x120) == 4 && ros_ld32(B - 0x120) == 5 &&
              ros_ld32(A - 0x11C) == 11 && ros_capp_gs() == gs0,
          "C applications: A.1 -- each client's private call count its own (4 and 5, at TP-&120, errno at "
          "TP-&11C); the runtime's base back after each entry", "calls A %u B %u, gs &%llX (was &%llX)", acalls,
          bcalls, (unsigned long long)ros_capp_gs(), (unsigned long long)gs0);
    ros_capp_code_remove(lib);
out:
    ros_rma_free(ba);
    ros_rma_free(bb);
    ros_rma_free(msg);
    if (mapped)
        ros_arena_unmap(lib, room);
}

/* ---- SharedCLibrary's registration ------------------------------------------------ */

/* The next line of text from *p (to its end), NUL-terminated in line; 0 at the end */
static int next_line(const char **p, const char *end, char *line, size_t room)
{
    if (*p >= end)
        return 0;
    const char *nl = memchr(*p, '\n', (size_t)(end - *p));
    size_t n = (size_t)((nl ? nl : end) - *p);
    if (n >= room)
        n = room - 1;
    memcpy(line, *p, n);
    line[n] = 0;
    *p = nl ? nl + 1 : end;
    return 1;
}

/* The ROM library, mapped by SharedCLibrary, then the stub-only client
 * (tests/capps/sclib/clibreg.c).  Its log matches RISC OS 5.30's line for
 * line, except for its "x32:" lines.  5.30's log is its AArch32 build's,
 * tests/capps/farm/clibreg.txt.  The "x32:" lines are the facts of ROSGD's
 * registration. */
static void registration(void)
{
    uint32_t header = ros_sharedclib_image();
    struct ros_clib_header h;
    memset(&h, 0, sizeof h);
    if (header)
        memcpy(&h, ros_ptr(header), sizeof h);
    char pm[8];
    strcpy(pm, perms(ROS_CROM_BASE));
    check(header && !memcmp(h.magic, ROS_CLIB_MAGIC, 8) && h.base == ROS_CROM_BASE && h.chunks == 8 &&
              !strncmp(pm, "r-x", 3) && ros_capp_code(ROS_CROM_BASE + 0x1000) == ROS_CAPP_LIBRARY,
          "SharedCLibrary: the ROM C library's image mapped at &72200000, read-only and executable, its "
          "header checked, the range x32 library code", "header &%X, %u chunks, %s", header, h.chunks, pm);

    uint64_t gs0 = ros_capp_gs();
    const os_error *e = run("Run HostFS::CAppTest.$.clibreg");
    for (unsigned i = 0; i < outn; i += 512)        /* the console's lines are 1K at most */
        ros_console_printf("%.512s", out + i);
    const char *f = (const char *)capp_clibreg_farm, *fend = f + CAPP_CLIBREG_FARM_SIZE;
    const char *o = out, *oend = out + outn;
    char want[256], got[256], diff[600] = "";
    unsigned lines = 0, x32 = 0;
    for (;;) {
        int w;
        while ((w = next_line(&f, fend, want, sizeof want)) && !strncmp(want, "# ", 2))
            ;
        int g;
        while ((g = next_line(&o, oend, got, sizeof got)) && !strncmp(got, "x32:", 4))
            x32++;
        if (!w && !g)
            break;
        if (!w || !g || strcmp(want, got)) {
            snprintf(diff, sizeof diff, "line %u: 5.30 [%s], ROSGD [%s]", lines + 1, w ? want : "(end)",
                     g ? got : "(end)");
            break;
        }
        lines++;
    }
    check(!e && !diff[0] && lines == 22 && x32 == 2,
          "SharedCLibrary: a stub-only client's registrations, good and spoiled, and its calls through the "
          "slots, as RISC OS 5.30's -- APCS-A/R refused (C71, C72), unknown and ROM-only chunks (C02), wrong "
          "static sizes (C04), inconsistent offsets (C05), too many entries (C63), too little workspace "
          "(C01); version 6, the stack bounds, the template copied, strlen and strtok",
          "%s%s (%u lines the same)", e ? e->errmess : "", diff, lines);

    unsigned tp = 0, self = 1, gs = 2, limit = 3, r1 = 4, entry = 0, high = 1, slot = 0;
    const char *l1 = strstr(out, "x32: TP "), *l2 = strstr(out, "x32: chunk 2's first table entry ");
    int n1 = l1 ? sscanf(l1, "x32: TP &%x self &%x %%gs:0 &%x stack limit &%x R1 &%x", &tp, &self, &gs, &limit,
                         &r1) : 0;
    int n2 = l2 ? sscanf(l2, "x32: chunk 2's first table entry &%x &%x, its first slot &%x", &entry, &high,
                         &slot) : 0;
    check(n1 == 5 && n2 == 3 && tp >= ROS_APP_BASE && tp < ROS_RMA_BASE && self == tp && gs == tp && limit == r1 &&
              entry >= ROS_CROM_BASE && entry < ROS_CROM_BASE + h.size && high == 0 && (slot & 0xFFFF) == 0x25FF &&
              ros_capp_gs() == gs0,
          "SharedCLibrary: registration makes the block's top the static base -- its self word and %gs:0 the "
          "thread pointer, the stack limit the stack's base -- fills the table after the slots with the "
          "library's addresses, the slots untouched; the runtime's base back after",
          "TP &%X self &%X gs &%X limit &%X R1 &%X entry &%X:%X slot &%X", tp, self, gs, limit, r1, entry,
          high, slot);
}
#endif

#if ROS_CAPP_NATIVE
/* A variable's value as a string, "" if there is none */
static void var(const char *name, char *v, size_t room)
{
    char *b = ros_rma_alloc(300);
    v[0] = 0;
    if (!b)
        return;
    strcpy(b, name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b) + 64, c.r[2] = 200, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    if (!c.v) {
        size_t n = c.r[2] < room ? c.r[2] : room - 1;
        memcpy(v, b + 64, n);
        v[n] = 0;
    }
    ros_rma_free(b);
}

/* ---- a C program through the ROM library ------------------------------------------ */

/* tests/capps/clibapp.c, linked by roscc --clib.  crt0_x32 registers, the
 * library's kernel starts it and _main runs main.  The ways out are
 * RISC OS 5.30's, recorded in tests/capps/clib/expected/exitcode.log and
 * argv.log.  The return code is Sys$ReturnCode, exit(-1) gives 255, a code
 * at Sys$RCLimit gives "Return code too large", and the atexit functions
 * run after the redirection has gone. */
/* tests/capps/modmalloc.c: a C module's allocation through the ROM library
 * (#21).  It is registered as a module, the library's module
 * initialisation is entered (_kernel_moduleinit, _clib_initialise), the
 * heap mutex is free, the client is in SVC mode, and malloc's blocks are
 * the RMA's.  Without the initialisation the ColourPicker's first malloc
 * waited on the mutex for ever, so the probe reads it first rather than
 * hang.  Then comes the module's end.  _clib_finalisemodule was a trap
 * until the library's module finalisation was written, so any C module's
 * *RMKill gave "undefined instruction". */
static void mod_malloc(void)
{
    const os_error *e = run("Run HostFS::CAppTest.$.modmalloc");
    ros_console_printf("%s", out);
    check(!e && strstr(out, "modmalloc: registration ok\n"
                            "modmalloc: heap mutex &1\n"
                            "modmalloc: malloc 3 blocks in the RMA, written, freed\n"
                            "modmalloc: finalised: 0, the atexit function run\n"),
          "SharedCLibrary: a C module's initialisation (modclib: _kernel_moduleinit, "
          "_clib_initialise) -- the heap mutex free, SVC mode, malloc from the RMA (#21); "
          "and its finalisation, _clib_finalisemodule: the atexit functions, 0 (L5)",
          "%s: [%s]", e ? e->errmess : "ran", out);
}

static void clib_app(const char *dir)
{
    char rc[32];
    const os_error *e = run("Set Sys$ReturnCode 99");
    e = run("Run HostFS::CAppTest.$.clibapp one \"two  three\" 4");
    ros_console_printf("%s", out);
    var("Sys$ReturnCode", rc, sizeof rc);
    check(!e &&
              strstr(out, "clibapp: argc 4\nclibapp: argv[1] [one]\nclibapp: argv[2] [two  three]\n"
                          "clibapp: argv[3] [4]\nclibapp: printf -42 beef str c|    7|ab |%\n"
                          "clibapp: command [HostFS::CAppTest.$.clibapp one \"two  three\" 4]\n"
                          "clibapp: atexit\n") &&
              !strcmp(rc, "0"),
          "SharedCLibrary: a C program through the ROM library (roscc link --clib: crt0_x32 registers, the "
          "kernel starts it) -- main's argv from the command line, quotes and all, printf, "
          "_kernel_command_string, atexit, and a return of 0 for Sys$ReturnCode",
          "%s: Sys$ReturnCode [%s]", e ? e->errmess : "ran", rc);

    e = run("HostFS::CAppTest.$.clibapp swis");
    check(!e && strstr(out, "clibapp: swix 123 vswix 456 vswi 789\nclibapp: block [abcdefgh]\n"),
          "SharedCLibrary: _vswix and _vswi answer as _swix does (va_list veneers, which the farm's DDE "
          "stubs lack), and _swix's block (_BLOCK) is the arguments after the output pointers",
          "%s: [%s]", e ? e->errmess : "ran", out);

    /* setjmp and longjmp: cl_body's longjmp, 0 made 1.  Out of a
     * trap-raised signal handler (in_signal_handler set), the trap handler
     * left through _kernel_exittraphandler (k_body 1376: not in a trap
     * handler, CallBacks no longer postponed).  x32's callee-saved MXCSR
     * control bits and x87 control word are back, and the exception flags
     * are as they were.  The flags are the library's statics in the
     * client's block, which the program reads and writes itself. */
    e = run("HostFS::CAppTest.$.clibapp jmp");
#if ROS_CAPP_A64
    check(!e && strstr(out, "clibapp: longjmp 0 gives 1, 42 42, -1 -1\n"
                            "clibapp: FPCR back, IXC kept\n"
                            "clibapp: at the start: in signal handler 0, in trap handler 0, callbacks inactive 1\n"
                            "clibapp: longjmp in a signal handler: 0 0 1; not in one: 0 1 0; "
                            "_kernel_exittraphandler: 0 1\n"),
          "SharedCLibrary, A64X32: setjmp and longjmp in the ROM library -- longjmp(env, 0) makes setjmp "
          "return 1; FPCR back (a 44-word jmp_buf, D9), FPSR's flags kept; out of a signal handler a trap "
          "raised, the trap handler left (_kernel_exittraphandler), as cl_body's longjmp does",
          "%s: [%s]", e ? e->errmess : "ran", out);
#else
    check(!e && strstr(out, "clibapp: longjmp 0 gives 1, 42 42, -1 -1\n"
                            "clibapp: MXCSR control back, PE kept; x87 control word back\n"
                            "clibapp: at the start: in signal handler 0, in trap handler 0, callbacks inactive 1\n"
                            "clibapp: longjmp in a signal handler: 0 0 1; not in one: 0 1 0; "
                            "_kernel_exittraphandler: 0 1\n"),
          "SharedCLibrary: setjmp and longjmp in the ROM library -- longjmp(env, 0) makes setjmp return 1; "
          "MXCSR's control bits and the x87 control word back, its exception flags kept; out of a signal "
          "handler a trap raised, the trap handler left (_kernel_exittraphandler), as cl_body's longjmp does",
          "%s: [%s]", e ? e->errmess : "ran", out);
#endif

    char rcs[4][32];
    const os_error *ex[4];
    static const char *const how[4] = { "ret", "exit 9", "exit -1", "exit 255" };
    for (unsigned i = 0; i < 4; i++) {
        char cmd[80];
        snprintf(cmd, sizeof cmd, "HostFS::CAppTest.$.clibapp %s", how[i]);
        ex[i] = run(cmd);
        var("Sys$ReturnCode", rcs[i], sizeof rcs[i]);
    }
    /* 256, at the limit: the library finalises, then generates the error
     * to the caller's error handler, here the kernel's default, which
     * reports it */
    const os_error *big = run("HostFS::CAppTest.$.clibapp exit 256");
    int big_ok = !big && strstr(out, "clibapp: atexit\nError: Return code too large (Error number &800E06)\n");
    check(!ex[0] && !ex[1] && !ex[2] && !ex[3] && !strcmp(rcs[0], "7") && !strcmp(rcs[1], "9") &&
              !strcmp(rcs[2], "255") && !strcmp(rcs[3], "255") && big_ok,
          "SharedCLibrary: exit codes as RISC OS 5.30's -- main's return and exit()'s code are "
          "Sys$ReturnCode, exit(-1) 255 (the limit added), 256 at the limit \"Return code too large\" "
          "(C50, &800E06) after the atexit functions",
          "rc %s %s %s %s; 256: %s [%s]", rcs[0], rcs[1], rcs[2], rcs[3], big ? big->errmess : "no error", out);

    /* OS_Exit made by the program itself, "ABEX" and a code.  The kernel's
     * SEXIT (runtime/environment.c) sets Sys$ReturnCode.  Past
     * Sys$RCLimit (256) it raises an error to the error handler.  The
     * error is R0's if that is one, else its own: "Return code limit
     * exceeded", or for a negative code "Negative return code".  These are
     * the kernel's texts, which OS_SetVarVal of Sys$ReturnCode gives too
     * (selftest_sysvars.c).  No exit handler runs, so no atexit function
     * runs.  Within the limit, the exit goes to the library's exit
     * handler. */
    static const char *const osx[4] = { "osexit 300", "osexit -5", "osexit 300 err", "osexit 200" };
    static const char *const osx_want[4] = { "Error: Return code limit exceeded (Error number &1E2)\n",
                                             "Error: Negative return code (Error number &1E2)\n",
                                             "Error: Probe error (Error number &123)\n", "clibapp: atexit\n" };
    char osx_line[4][64], osx_rc[4][32];
    int osx_ok = 1;
    for (unsigned i = 0; i < 4; i++) {
        char cmd[80];
        snprintf(cmd, sizeof cmd, "HostFS::CAppTest.$.clibapp %s", osx[i]);
        const os_error *oe = run(cmd);
        const char *l = strstr(out, i < 3 ? "Error:" : "clibapp: atexit");
        snprintf(osx_line[i], sizeof osx_line[i], "%.*s", l ? (int)strcspn(l, "\n") : 0, l ? l : "");
        osx_ok &= !oe && strstr(out, osx_want[i]) && (i < 3 ? !strstr(out, "atexit") : !strstr(out, "Error"));
        var("Sys$ReturnCode", osx_rc[i], sizeof osx_rc[i]);
    }
    check(osx_ok && !strcmp(osx_rc[0], "300") && !strcmp(osx_rc[1], "-5") && !strcmp(osx_rc[2], "300") &&
              !strcmp(osx_rc[3], "200"),
          "SharedCLibrary: OS_Exit made directly with \"ABEX\" -- Sys$ReturnCode set, then past "
          "Sys$RCLimit the kernel's \"Return code limit exceeded\" or \"Negative return code\", or "
          "R0's error, to the error handler, no exit handler run; within it, the library's exit handler",
          "300 [%s] rc %s; -5 [%s] rc %s; 300 err [%s] rc %s; 200 [%s] rc %s", osx_line[0], osx_rc[0],
          osx_line[1], osx_rc[1], osx_line[2], osx_rc[2], osx_line[3], osx_rc[3]);

    /* No SharedCLibrary: crt0's registration meets "SWI not known", which
     * cl_stub makes "Shared C library is out of date".  That is cl_stub's
     * own text, because the module's death cleared the CLib messages' word. */
    const os_error *k = run("RMKill SharedCLibrary");
    const os_error *gone = k ? k : run("HostFS::CAppTest.$.clibapp");
    char killed[300];
    snprintf(killed, sizeof killed, "%s", out);
    const os_error *back = run("RMReInit SharedCLibrary");
    check(!k && !gone && !back &&
              !strcmp(killed, "Error: Shared C library is out of date (Error number &800E91)\n"),
          "SharedCLibrary: with the module killed, a client's start (crt0_x32) gives cl_stub's \"Shared C "
          "library is out of date\" (C63), as it does on RISC OS; *RMReInit brings the module back",
          "%s%s%s [%s]", k ? k->errmess : "", gone ? gone->errmess : "", back ? back->errmess : "", killed);

    /* atexit after the redirection: the exit handler runs them, after
     * OS_Exit has shut it */
    e = run("HostFS::CAppTest.$.clibapp redirected { > HostFS::CAppTest.$.out }");
    char p[480], file[600] = "";
    FILE *f = NULL;
    static const char *const leaf[] = { "out,ffd", "out,fff", "out" };      /* whatever type HostFS gave it */
    for (unsigned i = 0; i < 3 && !f; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, leaf[i]);
        f = fopen(p, "rb");
    }
    if (f) {
        size_t n = fread(file, 1, sizeof file - 1, f);
        file[n] = 0;
        fclose(f);
        unlink(p);
    }
    check(!e && strstr(file, "clibapp: argv[1] [redirected]") && !strstr(file, "atexit") &&
              !strcmp(out, "clibapp: atexit\n"),
          "SharedCLibrary: the atexit functions print after the redirection has gone (OS_Exit shuts it "
          "before the library's exit handler runs them), as on RISC OS 5.30",
          "%s: file [%.80s] screen [%s]", e ? e->errmess : "ran", file, out);
}
#endif

#if ROS_CAPP_NATIVE && !ROS_CAPP_A64
/* ---- per-task state ------------------------------------------------------------ */

#define R_OWNER   0x5233574Fu       /* "R3OW": pertask.c asks for the checks below */
#define R_HANDLER 0x52334848u       /* "R3HH": its UpCall handler's reason */

/* What the claimant saw, inside the probe's R3OW */
static struct {
    int ran;
    uint32_t handler;                       /* its UpCall handler */
    struct ros_capp_task before, after;     /* the task's record */
    int kind_mapped, kind_unmapped, kind_other, kind_samefd;
    volatile int bg_done;
    int bg_other_thread;
    uint32_t bg_v, bg_r[5];
    uint32_t unmapped_v, other_v, samefd_v;
    char unmapped_err[120], other_err[120], samefd_err[120];
    int lib_code, lib_own, lib_other, lib_samefd, lib_returned;   /* the ROM library's handlers */
} own;

/* An address in the ROM C library (SharedCLibrary's range).  Its handlers
 * are x32 library code.  They are delivered to whichever program is
 * running on the thread, if that program's task's memory is the memory at
 * &8000. */
#define LIB_HANDLER (ROS_CROM_BASE + 0x1000u)

static pthread_t owner_thread;         /* the probe's */

static void owner_bg(void *arg, uint32_t info)
{
    (void)arg, (void)info;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = R_HANDLER;
    ros_swi(&c, XOS_UpCall);
    own.bg_v = c.v;
    for (unsigned i = 0; i < 5; i++)
        own.bg_r[i] = c.r[i];
    own.bg_other_thread = !pthread_equal(pthread_self(), owner_thread);
    own.bg_done = 1;
}

/* The handler called on this thread.  It returns 1 and the error's text if
 * the call failed.  A call to what is not code is raised, X or not, as an
 * abort is.  It returns 0, "entered", otherwise. */
static uint32_t call_handler(char *err, size_t n)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = R_HANDLER;
    struct ros_handler h;
    if (ROS_TRY(&h)) {
        ros_swi(&c, XOS_UpCall);
        ros_handler_pop(&h);
        snprintf(err, n, "%s", c.v ? ((const os_error *)ros_ptr(c.r[0]))->errmess : "entered");
        return c.v;
    }
    snprintf(err, n, "%s", h.error->errmess);
    return 1;
}

/* The probe's R3OW runs on its thread with its memory, the task's slot, at
 * &8000.  First its handler is called from the background thread while it
 * waits.  Then it runs with no memory mapped, and with a copy of it in
 * other memory.  The copy is under another descriptor, and under the
 * task's own descriptor number (dup2: the same fd, another file), as the
 * Wimp's switch maps another task's memory on this thread.  A library
 * handler is then not this program's to take, so ros_capp_deliver
 * returns.  Were it delivered, the probe would never come back here. */
static int owner_claim(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] != R_OWNER)
        return ROS_VECTOR_PASS;
    own.ran = 1;
    struct ros_task *t = ros_task_current();
    own.before = *ros_task_capp(t);
    uint32_t h12, buf;
    ros_env_read(ROS_ENV_UPCALL, &own.handler, &h12, &buf);
    own.kind_mapped = ros_capp_code(own.handler);
    own.lib_code = ros_capp_code(LIB_HANDLER);
    own.lib_own = ros_capp_exception_handler(LIB_HANDLER);

    own.bg_done = 0;
    owner_thread = pthread_self();
    unsigned d = ros_blocking_begin();
    ros_post(owner_bg, NULL, 0);
    for (unsigned i = 0; i < 400 && !own.bg_done; i++) {
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
    ros_blocking_end(d);

    const struct ros_slot *m = ros_slot_current;
    if (m) {
        ros_slot_unmap();
        own.kind_unmapped = ros_capp_code(own.handler);
        own.unmapped_v = call_handler(own.unmapped_err, sizeof own.unmapped_err);
        struct ros_slot other;
        void *from = mmap(NULL, m->size, PROT_READ, MAP_SHARED, m->fd, 0);
        if (from != MAP_FAILED && ros_slot_create(&other, m->size) == 0) {
            void *to = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_SHARED, other.fd, 0);
            if (to != MAP_FAILED) {
                memcpy(to, from, m->size);
                munmap(to, m->size);
                ros_slot_map(&other);
                own.kind_other = ros_capp_code(own.handler);
                own.other_v = call_handler(own.other_err, sizeof own.other_err);
                own.lib_other = ros_capp_exception_handler(LIB_HANDLER);
                uint32_t regs[3] = { 0, 0, 0 };
                ros_capp_deliver(LIB_HANDLER, own.before.base, regs, 1);
                own.lib_returned = 1;
                ros_slot_unmap();
                int keep = dup(m->fd);
                if (keep >= 0 && dup2(other.fd, m->fd) == m->fd) {
                    ros_slot_map(m);
                    own.kind_samefd = ros_capp_code(own.handler);
                    own.samefd_v = call_handler(own.samefd_err, sizeof own.samefd_err);
                    own.lib_samefd = ros_capp_exception_handler(LIB_HANDLER);
                    ros_slot_unmap();
                    dup2(keep, m->fd);
                }
                if (keep >= 0)
                    close(keep);
            }
            ros_slot_destroy(&other);
        }
        if (from != MAP_FAILED)
            munmap(from, m->size);
        ros_slot_map(m);
    }
    own.after = *ros_task_capp(t);
    s->r[0] = 0;
    return ROS_VECTOR_CLAIM;
}

static void pertask(void)
{
    struct ros_task *t = ros_task_current();
    struct ros_capp_task idle = *ros_task_capp(t);
    uint64_t gs0 = ros_capp_gs();
    memset(&own, 0, sizeof own);
    ros_selftest_capps_hooks(1);
    ros_vector_claim_native(ROS_UPCALLV, owner_claim, 0);
    const os_error *e = run("Run HostFS::CAppTest.$.pertask");
    ros_vector_release_native(ROS_UPCALLV, owner_claim, 0);
    ros_selftest_capps_hooks(0);
    ros_console_printf("%s", out);
    line_buf bs, rec, ow, bk, af;
    line_of(bs, "base set:", 0);
    line_of(rec, "record:", 0);
    line_of(ow, "owner:", 0);
    line_of(bk, "back:", 0);
    line_of(af, "after:", 0);
    uint32_t base = val(bs, " base"), handler = val(bs, "handler"), frame = val(ow, "frame");
    const struct ros_capp_task *b = &own.before;

    /* What the task keeps: its base, its code, the memory it runs in, the
     * sp it last called the OS from, the native stack its SWIs run on */
    check(!e && own.ran && val(bs, "R0") == 0 && val(bs, "in force") == base &&
              base >= ROS_APP_BASE && b->base == base && val(rec, "base") == base &&
              val(rec, "in force") == base && val(rec, "task") == ros_task_id(t) &&
              b->code_lo <= handler && handler < b->code_hi && b->code_lo >= ROS_APP_BASE &&
              b->mem_fd >= 0 && b->mem_fd == (ros_slot_current ? ros_slot_current->fd : -2) &&
              b->gate.saved_sp < frame && frame - b->gate.saved_sp < 512 &&
              (b->gate.native_sp >> 32) != 0,
          "C applications (R3): the task keeps its application's state -- the base "
          "registration set, its code, the memory it runs in, the sp it last called the OS "
          "from, the native stack its SWIs run on", "%s: %s | %s | saved &%llX frame &%X",
          e ? e->errmess : "ran", bs, rec, (unsigned long long)b->gate.saved_sp, frame);

    /* Its handler, from the background thread while it waited in a SWI.
     * Its code is found by the memory at &8000 and entered with its base.
     * Its SWI goes through the gate there.  The task's saved sp is not
     * moved by it. */
    check(own.kind_mapped == ROS_CAPP_TASK && own.handler == handler && own.bg_done &&
              !own.bg_v && own.bg_r[0] == 0 && own.bg_r[1] == base &&
              own.bg_r[2] == 0x9E27A5C0u && own.bg_r[3] == 0x600D &&
              own.bg_r[4] - ROS_SVCSTACK_BASE < ROS_SVCSTACK_SIZE && own.bg_other_thread == 1 &&
              own.after.gate.saved_sp == b->gate.saved_sp && own.after.base == base &&
              own.after.gate.native_sp == b->gate.native_sp,
          "C applications (R3): a task's handler run from the background thread, the task "
          "waiting: its code by the memory at &8000, entered with the task's base, its SWIs "
          "through the gate there; the task's saved and native sp left as they were",
          "kind %d done %d V %u R0 &%X base &%X identity &%X swi &%X sp &%X other thread %d",
          own.kind_mapped, own.bg_done, own.bg_v, own.bg_r[0], own.bg_r[1], own.bg_r[2],
          own.bg_r[3], own.bg_r[4], own.bg_other_thread);

    check(own.kind_unmapped == 0 && own.unmapped_v && strstr(own.unmapped_err, "not compiled") &&
              own.kind_other == 0 && own.other_v && strstr(own.other_err, "not compiled") &&
              own.kind_samefd == 0 && own.samefd_v && strstr(own.samefd_err, "not compiled"),
          "C applications (R3): with no memory at &8000, or other memory -- a copy of the "
          "task's, under another descriptor or under the task's own descriptor number -- the "
          "same address is no task's code, and is not entered",
          "unmapped %d [%s], other %d [%s], same descriptor %d [%s]", own.kind_unmapped,
          own.unmapped_err, own.kind_other, own.other_err, own.kind_samefd, own.samefd_err);

    /* A library handler (the ROM library's, SharedCLibrary's range) is the
     * program's to take only with its task's memory at &8000 */
    check(own.lib_code == ROS_CAPP_LIBRARY && own.lib_own == 1 && own.lib_other == 0 &&
              own.lib_samefd == 0 && own.lib_returned == 1,
          "C applications (R3): a library handler is delivered to the program on the thread "
          "only while its task's memory is at &8000 -- with a copy mapped, as the Wimp's switch "
          "maps another task's, it is not the program's (ros_capp_deliver returns)",
          "library &%X kind %d: own memory %d, other %d, same descriptor %d, deliver returned %d",
          LIB_HANDLER, own.lib_code, own.lib_own, own.lib_other, own.lib_samefd, own.lib_returned);

    /* Back in the program: its base in force on its thread, its statics;
     * the record as it was; after it, the task's record empty again */
    const struct ros_capp_task *now = ros_task_capp(t);
    check(val(bk, "in force") == base && val(bk, "identity") == 0x9E27A5C0u &&
              val(bk, "handled") == 1 && val(af, "base") == base && val(af, "in force") == base &&
              now->code_hi == idle.code_hi && now->base == idle.base && ros_capp_gs() == gs0,
          "C applications (R3): the program's own base in force after the other thread's entry; "
          "when it ends, the task's record is as it was before it", "%s | %s | code_hi &%X",
          bk, af, now->code_hi);
}
#endif

#if ROS_CAPP_NATIVE && !ROS_CAPP_A64
/* ---- faults ---------------------------------------------------------------------- */

#define R2B_STACK 0x52324253u           /* "R2BS": the probe asks for its stack's guard */
static uint32_t guard_at, guard_size;
static char guard_perms[8];
static int guard_invalid, above_valid;
static const os_error *guard_error;

/* The stand-in for the library's registration laying the stack out.  The
 * runtime is told, which makes the guard.  Then the memory is mapped
 * again, as a task switch or AMB growing it maps it.  The guard must still
 * be there, and OS_ValidateAddress must call it invalid and the page above
 * it valid. */
static int stack_guard(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] != R2B_STACK)
        return ROS_VECTOR_PASS;
    guard_at = s->r[1], guard_size = s->r[2] - s->r[1];
    guard_error = ros_capp_stack(s->r[1], s->r[2], s->r[3]);
    if (!guard_error && ros_slot_current)
        ros_slot_map(ros_slot_current);
    strcpy(guard_perms, perms(guard_at));
    int above_invalid = 1;
    xos_validate_address(guard_at, guard_at + 16, &guard_invalid);
    xos_validate_address(s->r[2], s->r[2] + 16, &above_invalid);
    above_valid = !above_invalid;
    s->r[0] = !guard_error && !strcmp(guard_perms, "---s") && guard_invalid && above_valid ? 0 : 1;
    return ROS_VECTOR_CLAIM;
}

/* The lines under a case's title in the probe's output, up to the next title */
static const char *case_lines(const char *title, char *buf, size_t n)
{
    buf[0] = 0;
    char t[96];
    snprintf(t, sizeof t, "\n%s\n", title);
    const char *p = strstr(out, t);
    if (!p)
        return buf;
    p += strlen(t);
    size_t k = 0;
    while (*p == ' ' && k < n - 1) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p + 1) : strlen(p);
        if (k + len >= n)
            break;
        memcpy(buf + k, p, len);
        k += len;
        p += len;
    }
    buf[k] = 0;
    return buf;
}

static const struct {
    const char *title, *want;           /* the error line, or the value line */
} fault_cases[] = {
    { "load from unmapped memory", "  error &80000002 [Internal error: abort on data transfer at &" },
    { "store to unmapped memory", "  error &80000002 [Internal error: abort on data transfer at &" },
    { "store to read-only memory", "  error &80000002 [Internal error: abort on data transfer at &" },
    { "load from address 0", "  error &80000002 [Internal error: abort on data transfer at &" },
    { "call to unmapped memory", "  error &80000001 [Internal error: abort on instruction fetch at &" },
    { "call to address 0", "  error &80000001 [Internal error: abort on instruction fetch at &00000000]" },
    { "undefined instruction", "  error &80000000 [Internal error: undefined instruction at &" },
    { "breakpoint", "  error &80000001 [Internal error: abort on instruction fetch at &" },
    { "privileged instruction", "  error &80000000 [Internal error: undefined instruction at &" },
    { "unaligned word load", "  value: &14131211" },
    { "integer 7 / 0", "  error &80000020 [Divide by zero]" },
    { "integer 7 % 0", "  error &80000020 [Divide by zero]" },
    { "INT_MIN / -1", "  value: -2147483648 remainder 0" },
    { "LLONG_MIN / -1", "  value: -9223372036854775808" },
    { "FP 1.0 / 0.0, traps on", "  error &80000202 [Floating point exception: division by zero]" },
    { "FP 0.0 / 0.0, traps on", "  error &80000200 [Floating point exception: invalid operation]" },
    { "FP DBL_MAX * DBL_MAX, traps on", "  error &80000201 [Floating point exception: overflow]" },
    { "FP DBL_MIN * DBL_MIN, traps on", "  value: 0" },
    { "OS_Write0 of unmapped memory", "  error &80000002 [Internal error: abort on data transfer at &" },
    { "OS_ConvertHex8 into read-only memory", "  error &80000002 [Internal error: abort on data transfer at &" },
    { "OS_GenerateError", "  error &00000123 [Probe error]" },
    { "load from unmapped memory in the UpCall handler",
      "  error &80000002 [Internal error: abort on data transfer at &" },
    { "load from unmapped memory, the program's own data abort handler",
      "  own handler: data abort, pc in the function yes, mode was &00000010" },
    { "undefined instruction, the program's own handler",
      "  own handler: undefined instruction, pc in the function yes, mode was &00000010" },
    { "OS_Write0 of unmapped memory, the program's own data abort handler",
      "  own handler: data abort, mode was &00000013" },
};

/* Whether a case's lines answer anything "no" */
static int says_no(const char *lines)
{
    return strstr(lines, " no,") || strstr(lines, " no\n");
}

static void faults(void)
{
    /* Every case: its error (5.30's, tests/capps/faults/expected.log), each
     * "where", "dump" and "handler" line all yes (but an FP error's text,
     * which has no address), and a report for each fault */
    unsigned n0 = ros_fault_count();
    const os_error *e = run("Run HostFS::CAppTest.$.faults");
    unsigned reports = ros_fault_count() - n0;
    char bad[400] = "", lines[1024];
    unsigned good = 0, nc = sizeof fault_cases / sizeof fault_cases[0];
    for (unsigned i = 0; i < nc; i++) {
        case_lines(fault_cases[i].title, lines, sizeof lines);
        char *fp = strstr(lines, "where: text address no,");     /* (FPEmulator's: no address) */
        if (fp)
            memcpy(fp, "where: text address --,", 23);
        if (!strncmp(lines, fault_cases[i].want, strlen(fault_cases[i].want)) && !says_no(lines))
            good++;
        else if (!bad[0])
            snprintf(bad, sizeof bad, "%s: [%.200s]", fault_cases[i].title, lines);
    }
    const char *last = ros_fault_last_report();
    check(!e && good == nc && reports == 20 && strstr(out, "\nend\n") &&
              strstr(last, "in SWI OS_Write0") && strstr(last, "to the program's data abort handler"),
          "C applications, faults: each of the probe's cases gives 5.30's error, the register "
          "block, the handler as the kernel enters it, FP traps kept -- or goes to the "
          "program's own abort handler; 20 reported",
          "%s; %u of %u cases, %u reports: %s", e ? e->errmess : "ran", good, nc, reports, bad);

    /* The stack's guard: the recursion faults into it; the error is the
     * library's stack overflow, delivered on the emergency stack */
    ros_vector_claim_native(ROS_UPCALLV, stack_guard, 0);
    guard_at = guard_size = 0;
    guard_perms[0] = 0;
    n0 = ros_fault_count();
    e = run("Run HostFS::CAppTest.$.faults stack");
    ros_vector_release_native(ROS_UPCALLV, stack_guard, 0);
    char gone[8];
    strcpy(gone, guard_size ? perms(guard_at) : "none");
    ros_console_printf("%s", out);
    check(guard_size && !guard_error && !strcmp(guard_perms, "---s") && guard_invalid &&
              above_valid && !strcmp(gone, "rwxs"),
          "C applications, faults: the stack's guard (ros_capp_stack) is inaccessible and stays "
          "so when the memory is mapped again; OS_ValidateAddress calls it invalid and the page "
          "above valid; it is gone when the program ends",
          "%s; guard %s (invalid %d, above valid %d), after the program %s",
          guard_error ? guard_error->errmess : "made", guard_perms, guard_invalid, above_valid,
          gone);
    check(!e && strstr(out, "stack guard: yes") &&
              strstr(out, "  error &80000021 [Not enough memory, stack overflow]") &&
              strstr(out, "handler sp in the emergency region yes") &&
              strstr(out, "  dump: pc in recurse yes, sp at the guard yes, mode &00000010") &&
              strstr(out, "  handler: R0 its R12 yes, mode &00000010") &&
              !strstr(out, "the recursion came back") && strstr(out, "\nend\n") &&
              ros_fault_count() - n0 == 1 && strstr(ros_fault_last_report(), "stack overflow in "),
          "C applications, faults: a stack overflow into its guard is \"Not enough memory, stack "
          "overflow\" (&80000021), the handler on the emergency stack, the frames above kept",
          "%s", e ? e->errmess : "ran");

    /* The guards' table (arena.c) holds as many guards as there can be
     * application spaces.  The test makes twenty slots, each with a guard.
     * Each guard is kept when its slot is mapped at &8000 again (a task
     * switch) and when it grows (ros_slot_resize, as AMB grows a node).
     * The page it grew by is usable. */
    enum { NSLOTS = 20 };
    struct ros_slot sl[NSLOTS];
    const struct ros_slot *was = ros_slot_current;
    const uint32_t g_lo = ROS_APP_BASE + 0x4000u, g_hi = g_lo + 0x1000u;
    unsigned made = 0, guarded = 0, kept = 0, grown = 0;
    while (made < NSLOTS && ros_slot_create(&sl[made], 0x10000) == 0) {
        if (ros_slot_map(&sl[made]) == 0 && ros_slot_guard(g_lo, g_hi) == 0)
            guarded++;
        made++;
    }
    for (unsigned i = 0; i < made; i++) {
        if (ros_slot_map(&sl[i]) == 0 && !strcmp(perms(g_lo), "---s") && ros_slot_in_guard(g_lo) &&
            !ros_slot_in_guard(g_hi))
            kept++;
        if (ros_slot_resize(&sl[i], 0x20000) == 0 && !strcmp(perms(g_lo), "---s") && ros_slot_in_guard(g_lo) &&
            !strcmp(perms(ROS_APP_BASE + 0x18000u), "rwxs"))
            grown++;
    }
    for (unsigned i = 0; i < made; i++)
        ros_slot_destroy(&sl[i]);
    if (was)
        ros_slot_map(was);
    check(made == NSLOTS && guarded == NSLOTS && kept == NSLOTS && grown == NSLOTS &&
              ros_slot_current == was,
          "C applications, faults: a stack guard for each of twenty application spaces at once "
          "(the table holds AMB's every node), each kept when its memory is mapped again and "
          "when it grows", "made %u, guarded %u, kept %u, kept grown %u", made, guarded, kept, grown);

    /* x86's division overflowing in its other forms: finished as ARM's */
    e = run("Run HostFS::CAppTest.$.faults divisions");
    check(!e && strstr(out, "divisions: &00000080 &00008000 &80000000 &00000000 &80000000 "
                            "&00000000 &00000000 &80000000"),
          "C applications, faults: IDIV of 8, 16, 32 (memory, %rip-relative) and 64 bits and DIV, "
          "each overflowing, give the quotient truncated and the remainder 0, and go on",
          "%s: %s", e ? e->errmess : "ran", out);

    /* SIGBUS: x86's alignment check, turned on by the program, faults an
     * unaligned load.  The result is a data abort, as 5.30's on an
     * unaligned word. */
    n0 = ros_fault_count();
    e = run("Run HostFS::CAppTest.$.faults bus");
    check(!e && strstr(out, "bus: a SWI with AC on and an unaligned register block yes") &&
              strstr(out, "  error &80000002 [Internal error: abort on data transfer at &") &&
              strstr(out, "  where: text address in the function yes, buffer pc yes, dump pc yes") &&
              strstr(out, "  handler: R0 its R12 yes, mode &00000010") && !strstr(out, "bus: &") &&
              strstr(out, "\nend\n") && ros_fault_count() - n0 == 1 &&
              strstr(ros_fault_last_report(), "data abort (alignment check) in the application"),
          "C applications, faults: a SWI made with EFLAGS.AC on and an unaligned register block "
          "works, AC on again after; SIGBUS (an unaligned load under AC) is a data abort at the "
          "load, delivered as the others", "%s: %s", e ? e->errmess : "ran", out);

    /* The gate itself reading a register block the program cannot: a data
     * abort at the program's call, the report saying so */
    n0 = ros_fault_count();
    e = run("Run HostFS::CAppTest.$.faults gate");
    check(!e && strstr(out, "  error &80000002 [Internal error: abort on data transfer at &") &&
              strstr(out, "  where: text address in the function yes, buffer pc yes, dump pc yes") &&
              strstr(out, "  handler: R0 its R12 yes, mode &00000010") && !strstr(out, "gate: &") &&
              strstr(out, "\nend\n") && ros_fault_count() - n0 == 1 &&
              strstr(ros_fault_last_report(), "the SWI gate, copying the program's registers"),
          "C applications, faults: the SWI gate given a register block the program cannot read "
          "is a data abort at its call, delivered as the others", "%s: %s", e ? e->errmess : "ran",
          out);

    /* The runtime faulting in a SWI given a bad pointer, from native code:
     * the error to its C handler, the SWI depth and SVC stack back; the
     * register block in the kernel's own, the program's being unusable */
    uint32_t dump_word = ros_ld32(ROS_ZEROPAGE + 0x958);
    ros_st32(ROS_ZEROPAGE + 0x958, 3);
    uint32_t depth = ros_call_depth, svc = ros_svc_sp;
    struct ros_handler h;
    const os_error *caught = NULL;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ROS_DA_LIMIT;
    n0 = ros_fault_count();
    if (ROS_TRY(&h)) {
        ros_swi(&s, XOS_Write0);
        ros_handler_pop(&h);
    } else {
        caught = h.error;
    }
    uint32_t used = ros_ld32(ROS_ZEROPAGE + 0x958);
    uint32_t mode = ros_ld32(used + 64) & 0x1F;
    ros_st32(ROS_ZEROPAGE + 0x958, dump_word);
    struct ros_cpu after;
    ros_cpu_enter(&after);
    ros_swi(&after, XOS_GetEnv);
    check(caught && caught->errnum == ROS_ERR_DATA_ABORT &&
              strstr(caught->errmess, "Internal error: abort on data transfer at &") &&
              ros_fault_count() - n0 == 1 && strstr(ros_fault_last_report(), "in SWI OS_Write0") &&
              used == ROS_ZEROPAGE + 0xAE8 && mode == ROS_MODE_SVC && ros_call_depth == depth &&
              ros_svc_sp == svc && !after.v,
          "C applications, faults: the runtime faulting in a SWI on a bad pointer raises 5.30's "
          "data abort to the caller's handler; the block in the kernel's own (the named one "
          "unusable), SVC mode; the SWI depth and SVC stack back, SWIs go on",
          "%s; block at &%X mode &%X", caught ? caught->errmess : "not caught", used, mode);
}
#endif

#if ROS_CAPP_A64
/* The FPCR probe's child: what SIGFPE it got, down the pipe */
static int fpe_pipe = -1;
static void fpe_child(int sig, siginfo_t *si, void *context)
{
    (void)context;
    int got[2] = { sig, si->si_code };
    ssize_t n = write(fpe_pipe, got, sizeof got);
    (void)n;
    _exit(3);
}
#endif

void ros_selftest_capps(void)
{
    char dir[256];
    const char *t = getpid() == 1 ? "/tmp" : getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rosgd-capps-%d", t ? t : "/tmp", (int)getpid());
    mkdir(dir, 0755);
    put(dir, "probe,ff8", capp_probe, CAPP_PROBE_SIZE);
    put(dir, "bigprobe,ff8", capp_bigprobe, CAPP_BIGPROBE_SIZE);
    put(dir, "arm,ff8", arm_absolute, sizeof arm_absolute);
#if X32_PROBES
    put(dir, "entries,ff8", capp_entries, CAPP_ENTRIES_SIZE);
    put(dir, "modclib,ff8", capp_modclib, CAPP_MODCLIB_SIZE);
    put(dir, "clibreg,ff8", capp_clibreg, CAPP_CLIBREG_SIZE);
    put(dir, "clibapp,ff8", capp_clibapp, CAPP_CLIBAPP_SIZE);
    put(dir, "pertask,ff8", capp_pertask, CAPP_PERTASK_SIZE);
    put(dir, "faults,ff8", capp_faults, CAPP_FAULTS_SIZE);
#else
    put(dir, "clibapp,ff8", capp_clibapp, CAPP_CLIBAPP_SIZE);
#endif
    put(dir, "modmalloc,ff8", capp_modmalloc, CAPP_MODMALLOC_SIZE);
    put_variant(dir, "linux,ff8", 0, 7, 0, 0);                  /* EI_OSABI: SysV */
    put_variant(dir, "nonote,ff8", 1, (uint32_t)-4, 'E', 0);    /* "ROSGE" */
    put_variant(dir, "version2,ff8", 1, 0, 2, 1);
    put_variant(dir, "flags,ff8", 1, 4, 1, 1);
    ros_hostfs_mount("CAppTest", dir);

#if X32_PROBES
    check(ros_capp_is_image(capp_probe, CAPP_PROBE_SIZE) &&
              !ros_capp_is_image(arm_absolute, sizeof arm_absolute),
          "C applications: an ELF32 x86-64 image is one; ARM code is not", NULL);
#else
    check(ros_capp_is_image(capp_probe, CAPP_PROBE_SIZE) &&
              !ros_capp_is_image(arm_absolute, sizeof arm_absolute),
          "C applications: an ELF32 A64X32 image is one; ARM code is not", NULL);
#endif

    /* ARM code is not one.  The ARM container runs it (selftest_armrun.c).
     * This one is MOV pc, lr, a return that exits. */
    const os_error *e = run("Run HostFS::CAppTest.$.arm");
    check(!e, "C applications: an &FF8 file of ARM code is not one -- the ARM container runs it "
              "(MOV pc, lr: an exit)", "%s", e ? e->errmess : "ran");

    /* Not a ROSGD image of version 1: refused, box and hosted, nothing run */
    char bad[300] = "";
    unsigned nbad = 0;
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        char cmd[80];
        snprintf(cmd, sizeof cmd, "Run HostFS::CAppTest.$.%s", refused[i].file);
        e = run(cmd);
        if (e && e->errnum == ROS_ERR_UNIMPLEMENTED && strstr(e->errmess, refused[i].why) &&
            strstr(e->errmess, "not a C application ROSGD can run") && !outn)
            nbad++;
        else if (!bad[0])
            snprintf(bad, sizeof bad, "%s: %s", refused[i].file, e ? e->errmess : "it ran");
    }
    check(nbad == sizeof refused / sizeof refused[0],
          "C applications: an x32 image with another OS/ABI (a Linux program), no ROSGD note, "
          "version 2 or unknown flags is refused before it runs", "%s", bad);

    /* OS_SynchroniseCodeAreas: everything, and a range */
    struct ros_cpu sc;
    ros_cpu_enter(&sc);
    sc.r[0] = 0;
    ros_swi(&sc, XOS_SynchroniseCodeAreas);
    int all_ok = !sc.v;
    ros_cpu_enter(&sc);
    sc.r[0] = 1, sc.r[1] = ROS_RMA_BASE, sc.r[2] = ROS_RMA_BASE + 0xFFF;
    ros_swi(&sc, XOS_SynchroniseCodeAreas);
    check(all_ok && !sc.v,
          "C applications: OS_SynchroniseCodeAreas, R0=0 (all) and R0=1 (a range), returns with "
          "V clear", "all %s, range %s", all_ok ? "ok" : "V set", sc.v ? "V set" : "ok");

#if defined(ROS_ARENA_HOSTED) || !ROS_CAPP_NATIVE
    e = run("Run HostFS::CAppTest.$.probe a  b");
    check(e && e->errnum == ROS_ERR_UNIMPLEMENTED && strstr(e->errmess, "hosted build") && !outn,
          "C applications, hosted: *Run of an x32 image is refused plainly, and nothing runs",
          "%s", e ? e->errmess : "no error");
    ros_console_printf("        the refusal: %s\n", e ? e->errmess : "none");
#elif ROS_CAPP_A64
    /* The A64X32 gate: the page, and the start-up probe
     * built A64X32 (capp.h's EM_AARCH64 contract) */
    char gpk[8], rpk[8];
    strcpy(gpk, perms(ROS_CAPP_GATE_PAGE));
    strcpy(rpk, perms(ROS_RMA_BASE));
    check(!strcmp(gpk, "r-xp") && !strcmp(rpk, "rwxs") && ros_ld32(ROS_CAPP_GATE) == 0x58000050u &&
              ros_ld32(ROS_CAPP_GATE + 4) == 0xD61F0200u && ros_ld32(ROS_CAPP_EXIT) == 0x58000050u,
          "C applications: the gate page at &FEEFF000 is read-only and executable, its trampolines "
          "ldr x16 / br x16; the RMA is executable", "gate %s, RMA %s", gpk, rpk);

    uint64_t fpcr0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr0));
    e = run("Run HostFS::CAppTest.$.probe a  b");
    uint64_t fpcr1;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr1));
    char slotp[8];
    strcpy(slotp, perms(ROS_APP_BASE));
    ros_console_printf("%s", out);
    uint32_t limit = value("OS_GetEnv R1 (RAM limit)");
    char env[256];
    strcpy(env, text("OS_GetEnv R0"));
    check(!e && !strcmp(env, "HostFS::CAppTest.$.probe a  b") &&
              value("entry R0") == value("OS_GetEnv R0") && !strcmp(text("entry R1"), "a  b"),
          "C applications, A64X32: *Run starts the image -- OS_GetEnv the command line as typed, "
          "w0/w1 at entry the line and its tail", "%s: [%s]", e ? e->errmess : "ran", env);
    check(value("CAO") == ROS_APP_BASE && value("MemoryLimit") == limit &&
              value("ApplicationSpace") == limit && limit > ROS_APP_BASE &&
              value("Wimp_SlotSize current") == limit - ROS_APP_BASE && !strcmp(slotp, "rwxs"),
          "C applications, A64X32: CAO &8000; RAM limit = MemoryLimit = ApplicationSpace = &8000 + "
          "the slot (as 5.30's); the slot executable", "limit &%X, CAO &%X, slot %s", limit,
          value("CAO"), slotp);
    check(value("entry sp") == (limit & ~15u) && value("return") == ROS_CAPP_EXIT &&
              strstr(out, "returning from the entry"),
          "C applications, A64X32: entered on a stack at the RAM limit (16-aligned), x30 &FEEFF010; "
          "a return exits and *Run comes back", "sp &%X return &%X", value("entry sp"),
          value("return"));
    uint32_t entry;
    memcpy(&entry, capp_probe + 24, 4);                     /* e_entry */
    check(value("entry registers: gpr") == 1u << 14 && value("x16") == entry && value("vreg") == 0 &&
              value("entry fp: fpcr") == 0 && value("fpsr") == 0 && value("nzcv") == 0,
          "C applications, A64X32: at entry every register the contract does not name is 0 -- "
          "x2-x29 (x18 the base, 0 before registration) but x16, the entry point (IP0, the branch "
          "into it), v0-v31 -- FPCR, FPSR and NZCV 0",
          "gpr &%X x16 &%X (entry &%X) vreg &%X fpcr &%X fpsr &%X nzcv &%X",
          value("entry registers: gpr"), value("x16"), entry, value("vreg"),
          value("entry fp: fpcr"), value("fpsr"), value("nzcv"));
    check(value("X SWI: psr") == (ROS_V_BIT | ROS_MODE_USR) && value("number") == 0x124 &&
              value("error") >= ROS_APP_BASE,
          "C applications, A64X32: the gate -- an X SWI's error comes back with V set, w0 the "
          "block, USR mode", "psr &%X error &%X number &%X", value("X SWI: psr"), value("error"),
          value("number"));
    check(value("FPCR at entry") == 0 && value("after a SWI") == 0x01800000u && fpcr1 == fpcr0,
          "C applications, A64X32: FPCR 0 at entry; the program's (round to -inf, flush to zero) "
          "kept across SWIs, the runtime's after it", "entry &%X after &%X runtime &%llX/&%llX",
          value("FPCR at entry"), value("after a SWI"), (unsigned long long)fpcr0,
          (unsigned long long)fpcr1);
    check(value("statics: data") == 1235 && value("bss") == 1,
          "C applications, A64X32: its statics -- data initialised, bss zeroed", NULL);

    e = run("HostFS::CAppTest.$.probe exit");
    int ex = !e && strstr(out, "leaving by OS_Exit") && !strstr(out, "OS_Exit returned");
    e = run("HostFS::CAppTest.$.probe error");
    int er = !e && strstr(out, "leaving by an error") &&
             strstr(out, "Error: Probe error (Error number &1234)") &&
             !strstr(out, "OS_GenerateError returned");
    check(ex && er,
          "C applications, A64X32: OS_Exit leaves; a SWI's error without X goes to the error "
          "handler, which ends it; *Run comes back each time", "%d %d", ex, er);

    e = run("Run HostFS::CAppTest.$.bigprobe");
    check(e && e->errnum == ROS_ERR_CORE_NOT_WRITABLE && !outn,
          "C applications, A64X32: an image bigger than application space is refused, &411, "
          "before it starts", "%s", e ? e->errmess : "it ran");

    /* The ROM C library's image, A64X32 (make -f capps.mk ABI=a64x32), and
     * a program through it (tests/capps/clibapp.c, crt0_a64x32).  The
     * checks cover its start (_kernel_init, _clib_initialise, the heap,
     * stdio), main, and its ways out. */
    clib_app(dir);
    mod_malloc();

    /* Faults in A64X32 code (fault.h, arch/aarch64/fault.c.inc): each the
     * error RISC OS 5.30 gives, to the program's error handler.  Here that
     * is the default one, which reports it and ends the program. */
    static const struct {
        const char *arg, *said, *error;
    } faults_a64[] = {
        { "div0", "dividing by zero", "Error: Divide by zero (Error number &80000020)" },
        { "udf", "an undefined instruction",
          "Error: Internal error: undefined instruction at &" },
        { "abort", "a load from &50000000", "Error: Internal error: abort on data transfer at &" },
        { "fpe", "1.0 / 0.0 with DZE set",
          "Error: Floating point exception: division by zero (Error number &80000202)" },
    };
    char fbad[400] = "";
    unsigned fgood = 0;
    for (unsigned i = 0; i < sizeof faults_a64 / sizeof faults_a64[0]; i++) {
        char cmd[80];
        snprintf(cmd, sizeof cmd, "HostFS::CAppTest.$.probe %s", faults_a64[i].arg);
        e = run(cmd);
        ros_console_printf("%s", out);
        if (!e && strstr(out, faults_a64[i].said) && strstr(out, faults_a64[i].error) &&
            !strstr(out, "returned") && !strstr(out, "the load gave"))
            fgood++;
        else if (!fbad[0])
            snprintf(fbad, sizeof fbad, "%s: %s", faults_a64[i].arg, e ? e->errmess : out);
    }
    check(fgood == sizeof faults_a64 / sizeof faults_a64[0],
          "C applications, A64X32: faults are RISC OS's errors -- brk #&5503 \"Divide by zero\" "
          "(&80000020), udf #0 an undefined instruction (&80000000), a load from nothing a data "
          "abort (&80000002), a trapped FP division by zero FPEmulator's (&80000202) -- to the "
          "program's error handler, which ends it", "%s", fbad);

    /* FPCR's trap enables.  Apple's cores were expected not to have them,
     * but they are implemented.  The bits stick, and a child process that
     * sets DZE and divides by 0.0 gets SIGFPE, FPE_FLTDIV.  So RISC OS's
     * floating point exceptions trap in hardware here, as they do on
     * x86-64 (the fpe probe, above). */
    uint32_t traps = ros_capp_fp_traps();
    int child_sig = -1, child_code = -1;
    int pfd[2];
    if (pipe(pfd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            struct sigaction sa = { .sa_sigaction = fpe_child, .sa_flags = SA_SIGINFO };
            sigemptyset(&sa.sa_mask);
            sigaction(SIGFPE, &sa, NULL);
            fpe_pipe = pfd[1];
            static volatile double one = 1.0, nought = 0.0;
            uint64_t c;
            __asm__ volatile("mrs %0, fpcr" : "=r"(c));
            __asm__ volatile("msr fpcr, %0" : : "r"(c | 0x200));
            volatile double r = one / nought;
            (void)r;
            _exit(0);
        }
        close(pfd[1]);
        int got[2] = { 0, 0 };
        if (pid > 0 && read(pfd[0], got, sizeof got) == (ssize_t)sizeof got)
            child_sig = got[0], child_code = got[1];
        close(pfd[0]);
        if (pid > 0)
            waitpid(pid, NULL, 0);
    }
    ros_console_printf("        FPCR trap enables that stick: &%X; DZE's trap: signal %d, code %d\n",
                       traps, child_sig, child_code);
    check(traps == 0x9F00u && child_sig == SIGFPE && child_code == FPE_FLTDIV,
          "C applications, A64X32: this CPU implements FPCR's trap enables -- IOE DZE OFE UFE IXE "
          "IDE stick, and 1.0/0.0 with DZE set is SIGFPE, FPE_FLTDIV", "&%X, signal %d code %d",
          traps, child_sig, child_code);
#else
    /* The gate page and executable memory */
    char gpk[8], rpk[8];
    strcpy(gpk, perms(ROS_CAPP_GATE_PAGE));
    strcpy(rpk, perms(ROS_RMA_BASE));
    check(!strcmp(gpk, "r-xp") && !strcmp(rpk, "rwxs") && ros_ld8(ROS_CAPP_GATE) == 0x49 &&
              ros_ld8(ROS_CAPP_EXIT) == 0x49,
          "C applications: the gate page at &FEEFF000 is read-only and executable; the RMA is "
          "executable", "gate %s, RMA %s", gpk, rpk);

    /* The start-up probe: returns from its entry */
    unsigned mx = __builtin_ia32_stmxcsr();
    e = run("Run HostFS::CAppTest.$.probe a  b");
    unsigned mx_after = __builtin_ia32_stmxcsr();
    char slotp[8];
    strcpy(slotp, perms(ROS_APP_BASE));
    ros_console_printf("%s", out);
    uint32_t limit = value("OS_GetEnv R1 (RAM limit)");
    char env[256];
    strcpy(env, text("OS_GetEnv R0"));
    check(!e && !strcmp(env, "HostFS::CAppTest.$.probe a  b") &&
              value("entry R0") == value("OS_GetEnv R0") && !strcmp(text("entry R1"), "a  b"),
          "C applications: *Run starts the image -- OS_GetEnv the command line as typed, "
          "R0/R1 at entry the line and its tail", "%s: [%s]", e ? e->errmess : "ran", env);
    check(value("CAO") == ROS_APP_BASE && value("MemoryLimit") == limit &&
              value("ApplicationSpace") == limit && limit > ROS_APP_BASE &&
              value("Wimp_SlotSize current") == limit - ROS_APP_BASE && !strcmp(slotp, "rwxs"),
          "C applications: CAO &8000; RAM limit = MemoryLimit = ApplicationSpace = &8000 + the "
          "slot (as 5.30's); the slot executable", "limit &%X, CAO &%X, slot %s", limit,
          value("CAO"), slotp);
    check(value("entry sp") == limit - 8 && value("return") == ROS_CAPP_EXIT &&
              strstr(out, "returning from the entry"),
          "C applications: entered on a stack at the RAM limit, returning to &FEEFF010; a return "
          "exits and *Run comes back", "sp &%X return &%X", value("entry sp"), value("return"));
    check(value("entry registers: gpr") == 0 && value("xmm") == 0 && value("control") == 0x37F &&
              value("status") == 0 && value("tag") == 0xFFFF && value("DF") == 0,
          "C applications: at entry every register the contract does not name is 0 -- the "
          "general ones, xmm0-15 -- the x87 control word &37F and its stack empty, DF clear",
          "gpr &%X xmm &%X x87 &%X/&%X/&%X DF %u", value("entry registers: gpr"), value("xmm"),
          value("control"), value("status"), value("tag"), value("DF"));
    check(value("psr") == (ROS_V_BIT | ROS_MODE_USR) && value("number") == 0x124 &&
              value("error") >= ROS_APP_BASE,
          "C applications: the gate -- an X SWI's error comes back with V set, R0 the block, "
          "USR mode", "psr &%X error &%X number &%X", value("psr"), value("error"),
          value("number"));
    check(value("MXCSR at entry") == 0x1F80 && value("after a SWI") == 0xBF80 && mx_after == mx,
          "C applications: MXCSR &1F80 at entry; the program's kept across SWIs, the runtime's "
          "after it", "entry &%X after &%X runtime &%X/&%X", value("MXCSR at entry"),
          value("after a SWI"), mx, mx_after);
    check(value("statics: data") == 1235 && value("bss") == 1,
          "C applications: its statics -- data initialised, bss zeroed", NULL);

    /* OS_Exit, and an error its handler ends it for */
    e = run("HostFS::CAppTest.$.probe exit");
    int ex = !e && strstr(out, "leaving by OS_Exit") && !strstr(out, "OS_Exit returned");
    e = run("HostFS::CAppTest.$.probe error");
    int er = !e && strstr(out, "leaving by an error") &&
             strstr(out, "Error: Probe error (Error number &1234)") &&
             !strstr(out, "OS_GenerateError returned");
    check(ex && er,
          "C applications: OS_Exit leaves; a SWI's error without X goes to the error handler, "
          "which ends it; *Run comes back each time", "%d %d", ex, er);

    /* Too big to fit: refused before anything starts */
    e = run("Run HostFS::CAppTest.$.bigprobe");
    check(e && e->errnum == ROS_ERR_CORE_NOT_WRITABLE && !outn,
          "C applications: an image bigger than application space is refused, &411, before it "
          "starts", "%s", e ? e->errmess : "it ran");

     /* An x32 module's SharedCLibrary registration (tests/capps/modclib.c,
     * roscc's modclib_x32.o and the stubs), run as an application.
     * SharedCLibrary_LibInitModuleAPCS_32 registers it, the block's top is
     * the base, and a slot reaches the library.  A spoiled call gets the
     * SWI's error back unchanged, as RISC OS's cl_stub returns it. */
    e = run("Run HostFS::CAppTest.$.modclib");
    ros_console_printf("%s", out);
    uint32_t mtp = value(", TP"), mgs = value("%gs:0");
    check(!e && strstr(out, "registration: ok\n") &&
              strstr(out, "strlen through its slot: &00000016") && mtp == mgs && mtp >= ROS_APP_BASE &&
              value("stack limit") == ROS_SVCSTACK_BASE &&
              strstr(out, "chunk 99: registration: &00800E81 [Unknown library chunk]"),
          "SharedCLibrary: an x32 module's registration (modclib_x32.c, LibInitModuleAPCS_32) -- its "
          "block's top the base, the stack limit the SVC stack's base, a slot reaching the library; the "
          "library's error returned unchanged (C02, not C62)", "%s: %s", e ? e->errmess : "ran", out);
    registration();
    clib_app(dir);
    mod_malloc();

    entries();
    clib_image();
    pertask();
    faults();
#endif
    signal_stacks();
    ros_hostfs_unmount("CAppTest");
#if X32_PROBES
    static const char *const files[] = { "probe,ff8", "bigprobe,ff8", "arm,ff8",      "entries,ff8",
                                         "linux,ff8", "nonote,ff8",   "version2,ff8", "flags,ff8",
                                         "modclib,ff8", "clibreg,ff8", "clibapp,ff8", "pertask,ff8",
                                         "faults,ff8", "modmalloc,ff8" };
#else
    static const char *const files[] = { "probe,ff8",  "bigprobe,ff8", "arm,ff8",
                                         "linux,ff8",  "nonote,ff8",   "version2,ff8",
                                         "flags,ff8",  "clibapp,ff8",   "modmalloc,ff8" };
#endif
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
        char p[400];
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        unlink(p);
    }
    rmdir(dir);
}
