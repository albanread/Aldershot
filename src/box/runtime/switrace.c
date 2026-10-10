/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* switrace.c -- the SWI ring and counts (include/rosgd/switrace.h).
 *
 * Module code runs holding the personality lock, so one SWI records at a
 * time. The in-flight frames are kept for each thread, as each task's
 * calls are on its own thread's stack. A SWI left by a raise (longjmp)
 * never records its way out. Its record stays open ("no return"). The next
 * call on that thread drops the frames it has left behind. These are the
 * frames at or below its own, because the stack grows down.
 *
 * The ring and the counts are the runtime's own memory, not the arena's.
 * Nothing reads them by address, only through *SWITrace and *SWIStats.
 *
 * A loop's calls are folded. A call that is the same as the one before it
 * (the SWI, the task, the depth and R0-R3 on entry) goes into that record
 * rather than a new one. Its count goes up, and the last time it was made
 * and its latest result are kept. The same happens once the last k calls
 * (up to 8) have repeated the k before them. Each further pass of that
 * cycle goes call by call into the matching record. Examples are the
 * Wimp's idle loop and BASIC's SYS by name. A program polling in a loop
 * fills a few records, not the ring.
 */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/platform.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"

#define DONE   1u
#define ERROR  2u

struct record {
    uint64_t t, t_last, seq;
    uint32_t ns, number, in[4], out0, task, errnum;
    uint32_t count;                     /* calls it stands for: repeats folded in */
    uint8_t depth, flags;
    char errmess[26];
};

struct count {
    uint32_t key;                       /* the SWI number + 1; 0 unused */
    uint64_t max_ns;                    /* 64 bits, because a SWI that waits (Wimp_Poll or
                                         * an error box's) can take more than 4.29 s, #163 */
    uint64_t calls, errors, total_ns, self_ns;
};

#define COUNTS 2048u                    /* a power of two, above the SWIs there are */

static struct record *ring;
static uint32_t ring_size;
static uint64_t next_seq = 1;
static struct count counts[COUNTS];
static uint64_t epoch;
static int on, live;
static __thread struct ros_swi_frame *current;

#define MAX_PERIOD 8u
static uint32_t fold_period;            /* the cycle being folded; 0 none */
static uint64_t fold_start;             /* the seq of its block's first record */
static uint32_t fold_pos;               /* the member the next call should be */

static uint64_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static const char *name_of(uint32_t n, char *buf, size_t size)
{
    const char *name = ros_swi_name(n);
    if (name)
        return name;
    snprintf(buf, size, "&%X", n);
    return buf;
}

/* The character output SWIs, left out of a live trace of "1" */
static int chatty(uint32_t n)
{
    return n <= 3 || (n >= 0x100 && n < 0x200);
}

static void write_stats_at_exit(void)
{
    const char *path = getenv("ROSGD_SWISTATS");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f)
        return;
    ros_switrace_dump(f, ring_size);
    fclose(f);
}

/* A run killed for hanging (SIGTERM, as timeout(1) and the test harnesses
 * send) also leaves its counts and ring. They show what it was doing. */
static void write_stats_and_die(int sig)
{
    write_stats_at_exit();
    _exit(128 + sig);
}

int ros_switrace_live(void)
{
    return live;
}

/* A setting from the environment (hosted), or else from the kernel's
 * command line (the box). ROSGD_SWITRACE=all is rosgd.switrace=all, and a
 * bare rosgd.switrace is "1". The result is NULL if neither has it. */
static const char *setting(const char *env, const char *word)
{
    const char *v = getenv(env);
    if (v)
        return v;
    if (!ros_cmdline_has(word))
        return NULL;
    v = ros_cmdline_value(word);
    return v ? v : "1";
}

void ros_switrace_init(void)
{
    const char *size = setting("ROSGD_SWIRING", "rosgd.swiring");
    ring_size = size ? (uint32_t)strtoul(size, NULL, 0) : 131072;
    const char *t = setting("ROSGD_SWITRACE", "rosgd.switrace");
    live = t ? (strcmp(t, "all") ? 1 : 2) : 0;
    epoch = now();
    if (ring_size) {
        ring = calloc(ring_size, sizeof *ring);
        if (!ring)
            ring_size = 0;
    }
    on = ring_size != 0;
    if (getenv("ROSGD_SWISTATS")) {
        atexit(write_stats_at_exit);
        signal(SIGTERM, write_stats_and_die);
    }
}

static struct count *count_for(uint32_t n)
{
    uint32_t key = n + 1, i = (n * 2654435761u) & (COUNTS - 1);
    /* A program can call any SWI number, so the table can fill. When it
     * has, further numbers are not counted. */
    for (uint32_t probes = 0; counts[i].key && counts[i].key != key; probes++) {
        if (probes == COUNTS)
            return NULL;
        i = (i + 1) & (COUNTS - 1);
    }
    counts[i].key = key;
    return &counts[i];
}

static struct record *at(uint64_t seq)
{
    if (!seq || seq >= next_seq)
        return NULL;
    struct record *r = &ring[seq % ring_size];
    return r->seq == seq && (r->flags & DONE) ? r : NULL;
}

static int same(const struct record *r, uint32_t number, uint32_t task, uint8_t depth, const uint32_t *in)
{
    return r->number == number && r->task == task && r->depth == depth &&
           !memcmp(r->in, in, sizeof r->in);
}

/* The record a call folds into, if it goes on a cycle. That is either the
 * next member of the cycle being folded, or the start of a cycle that the
 * ring's end has just repeated. A repeat means the last k records are the
 * same as the k before them. For k = 1, it means the call is the same as
 * the last. */
static struct record *folds_into(uint32_t number, uint32_t task, uint8_t depth, const uint32_t *in)
{
    if (fold_period) {
        struct record *r = at(fold_start + fold_pos);
        if (r && same(r, number, task, depth, in)) {
            fold_pos = (fold_pos + 1) % fold_period;
            return r;
        }
        fold_period = 0;
    }
    uint64_t last = next_seq - 1;
    for (uint32_t k = 1; k <= MAX_PERIOD && last >= 2 * k - 1; k++) {
        struct record *r = at(last - k + 1);
        if (!r || !same(r, number, task, depth, in))
            continue;
        int repeated = 1;
        for (uint32_t j = 0; k > 1 && j < k && repeated; j++) {
            struct record *x = at(last - k + 1 + j), *y = at(last - 2 * k + 1 + j);
            repeated = x && y && same(y, x->number, x->task, x->depth, x->in);
        }
        if (repeated) {
            fold_period = k;
            fold_start = last - k + 1;
            fold_pos = 1 % k;
            return r;
        }
    }
    return NULL;
}

void ros_switrace_in(struct ros_swi_frame *f, uint32_t number, const struct ros_cpu *s)
{
    f->seq = 0;
    while (current && (uintptr_t)current <= (uintptr_t)f)
        current = current->outer;       /* left by a raise */
    f->number = number;
    f->outer = current;
    current = f;                        /* always: its kind is the caller's (ros_caller_kind) */
    if (!on && !live)
        return;
    f->t0 = now();
    f->child_ns = 0;
    if (live && (live > 1 || !chatty(number))) {
        char buf[16];
        fprintf(stderr, "%*s> %s  %08X %08X %08X %08X %08X  [%X]\n", 2 * (int)ros_call_depth, "",
                name_of(number, buf, sizeof buf), s->r[0], s->r[1], s->r[2], s->r[3], s->r[4],
                ros_ld32(ROS_ZP_DOMAINID));
    }
    if (!on)
        return;
    uint32_t task = ros_ld32(ROS_ZP_DOMAINID);
    uint8_t depth = (uint8_t)(ros_call_depth > 255 ? 255 : ros_call_depth);
    struct record *fold = folds_into(number, task, depth, s->r);
    if (fold) {
        f->seq = fold->seq;
        fold->count++;
        fold->t_last = f->t0 - epoch;
        fold->flags = 0;
        return;
    }
    fold_period = 0;
    f->seq = next_seq++;
    struct record *r = &ring[f->seq % ring_size];
    r->t = r->t_last = f->t0 - epoch;
    r->count = 1;
    r->seq = f->seq;
    r->ns = 0;
    r->number = number;
    memcpy(r->in, s->r, sizeof r->in);
    r->out0 = 0;
    r->task = task;
    r->depth = depth;
    r->flags = 0;
}

/* The kind of the innermost call, which is the caller of whatever runs now. */
int ros_caller_kind(void)
{
    return current ? current->kind : ROS_KIND_NATIVE;
}

/* When a longjmp out of SWIs lands, for example an application's exit
 * going to the frame that entered it, the calls it left have ended. Their
 * frames' memory is the next call's. So the innermost call before it was
 * entered is the innermost again, without reading the frames that were
 * left. ros_switrace_top gives that call and ros_switrace_unwind puts it
 * back. */
struct ros_swi_frame *ros_switrace_top(void)
{
    return current;
}

void ros_switrace_unwind(struct ros_swi_frame *top)
{
    current = top;
}

void ros_switrace_unwind_below(const void *to)
{
    while (current && (uintptr_t)current < (uintptr_t)to)
        current = current->outer;
}

void ros_switrace_out(struct ros_swi_frame *f, const struct ros_cpu *s)
{
    /* Calls below this one that a longjmp left, for example an
     * application's exit from inside its own SWIs, ended when it did. Their
     * memory is the next call's. */
    while (current && (uintptr_t)current < (uintptr_t)f)
        current = current->outer;
    if (current == f)
        current = f->outer;
    if (!on && !live)
        return;
    uint64_t ns = now() - f->t0;
    if (f->outer)
        f->outer->child_ns += ns;
    const os_error *e = s->v ? (const os_error *)ros_ptr(s->r[0]) : NULL;
    if (live && (live > 1 || !chatty(f->number))) {
        char buf[16];
        const char *name = name_of(f->number, buf, sizeof buf);
        uint32_t task = ros_ld32(ROS_ZP_DOMAINID);
        if (e)
            fprintf(stderr, "%*s< %s  error &%X %s  [%X]\n", 2 * (int)ros_call_depth, "", name,
                    e->errnum, e->errmess, task);
        else
            fprintf(stderr, "%*s< %s  %08X %08X %08X %08X %08X  [%X]\n", 2 * (int)ros_call_depth, "",
                    name, s->r[0], s->r[1], s->r[2], s->r[3], s->r[4], task);
    }
    if (!on)
        return;
    struct count *c = count_for(f->number);
    if (c) {
        c->calls++;
        c->total_ns += ns;
        c->self_ns += ns > f->child_ns ? ns - f->child_ns : 0;
        if (ns > c->max_ns)
            c->max_ns = ns;
        if (e)
            c->errors++;
    }
    struct record *r = &ring[f->seq % ring_size];
    if (r->seq != f->seq)
        return;                         /* the ring has come round past it */
    r->ns = ns > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ns;
    r->out0 = s->r[0];
    r->flags = DONE;
    if (e) {
        r->flags |= ERROR;
        r->errnum = e->errnum;
        snprintf(r->errmess, sizeof r->errmess, "%s", e->errmess);
    }
}

static void format(const struct record *r, char *line, size_t size)
{
    char buf[16], result[64];
    if (!(r->flags & DONE))
        snprintf(result, sizeof result, "(no return)");
    else if (r->flags & ERROR)
        snprintf(result, sizeof result, "error &%X %s", r->errnum, r->errmess);
    else
        snprintf(result, sizeof result, "-> %08X", r->out0);
    char repeats[48] = "";
    if (r->count > 1)
        snprintf(repeats, sizeof repeats, "  x%u to %.6f", r->count, r->t_last / 1e9);
    snprintf(line, size, "%10.6f %9.1fus %08X %*s%s  %08X %08X %08X %08X  %s%s",
             r->t / 1e9, r->ns / 1e3, r->task, 2 * r->depth, "",
             name_of(r->number, buf, sizeof buf), r->in[0], r->in[1], r->in[2], r->in[3], result,
             repeats);
}

int ros_switrace_recent(unsigned n, int (*emit)(const char *line, void *ctx), void *ctx)
{
    if (!on)
        return emit("SWI recording is off (ROSGD_SWIRING=0)", ctx);
    uint64_t last = next_seq - 1;
    if (n > ring_size)
        n = ring_size;
    if (n > last)
        n = (unsigned)last;
    int stop = emit("      time      took task     SWI  R0-R3 in  result  [xN to: repeats folded in]", ctx);
    for (uint64_t seq = last - n + 1; !stop && seq <= last; seq++) {
        const struct record *r = &ring[seq % ring_size];
        if (r->seq != seq)
            continue;
        char line[256];
        format(r, line, sizeof line);
        stop = emit(line, ctx);
    }
    return stop;
}

static int by_total(const void *a, const void *b)
{
    const struct count *x = a, *y = b;
    return x->total_ns < y->total_ns ? 1 : x->total_ns > y->total_ns ? -1 : 0;
}

int ros_switrace_stats(unsigned top, int (*emit)(const char *line, void *ctx), void *ctx)
{
    if (!on)
        return emit("SWI recording is off (ROSGD_SWIRING=0)", ctx);
    static struct count sorted[COUNTS];
    unsigned used = 0;
    uint64_t calls = 0;
    for (unsigned i = 0; i < COUNTS; i++)
        if (counts[i].key && counts[i].calls) {
            sorted[used++] = counts[i];
            calls += counts[i].calls;
        }
    qsort(sorted, used, sizeof sorted[0], by_total);
    char line[160];
    snprintf(line, sizeof line, "%llu calls of %u SWIs in %.3f s", (unsigned long long)calls, used,
             (now() - epoch) / 1e9);
    int stop = emit(line, ctx);
    if (!stop)
        stop = emit("SWI                                calls  errors   total ms    self ms    mean us     max us", ctx);
    for (unsigned i = 0; !stop && i < used && (!top || i < top); i++) {
        const struct count *c = &sorted[i];
        char buf[16];
        snprintf(line, sizeof line, "%-32s %8llu %7llu %10.3f %10.3f %10.2f %10.1f",
                 name_of(c->key - 1, buf, sizeof buf), (unsigned long long)c->calls,
                 (unsigned long long)c->errors, c->total_ns / 1e6, c->self_ns / 1e6,
                 c->total_ns / 1e3 / (double)c->calls, c->max_ns / 1e3);
        stop = emit(line, ctx);
    }
    return stop;
}

void ros_switrace_reset(void)
{
    memset(counts, 0, sizeof counts);
}

static int to_file(const char *line, void *ctx)
{
    fprintf(ctx, "%s\n", line);
    return 0;
}

void ros_switrace_dump(FILE *f, unsigned n)
{
    if (n >= ring_size) {
        ros_switrace_stats(0, to_file, f);
        fputc('\n', f);
    }
    ros_switrace_recent(n, to_file, f);
}
