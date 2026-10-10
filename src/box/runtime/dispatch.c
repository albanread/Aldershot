/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* dispatch.c: from a 32-bit code address to the compiled code for it.
 *
 * In RISC OS a function pointer is a code address: vectors, module header
 * offsets, handler tables, callbacks.  72% of the corpus's indirect
 * transfers are dynamic.  But every target is a label whose address the
 * source takes.  Each compiled module registers exactly those labels, so
 * the table is complete by construction.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/armbox.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/heap.h"
#include "rosgd/platform.h"
#include "rosgd/switrace.h"

#define MAX_ENTRIES 16384

static struct ros_code_entry table[MAX_ENTRIES];
static unsigned entries;
static int sorted;

static int by_addr(const void *a, const void *b)
{
    uint32_t x = ((const struct ros_code_entry *)a)->addr;
    uint32_t y = ((const struct ros_code_entry *)b)->addr;
    return x < y ? -1 : x > y;
}

void ros_code_register(const struct ros_code_entry *e, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (entries == MAX_ENTRIES) {
            ros_console_printf("rosgd: dispatcher full at %s\n", e[i].name);
            abort();
        }
        table[entries++] = e[i];
    }
    sorted = 0;
}

ros_code *ros_code_lookup(uint32_t addr)
{
    if (!sorted) {
        qsort(table, entries, sizeof table[0], by_addr);
        sorted = 1;
    }
    /* Every indirect call and SWI comes here.  The binary search is written
     * inline to avoid bsearch's comparisons through a function pointer. */
    unsigned lo = 0, hi = entries;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < entries && table[lo].addr == addr ? table[lo].fn : NULL;
}

uint32_t ros_native_entry(ros_code *fn, const char *name)
{
    static uint32_t next = ROS_NATIVE_BASE;
    struct ros_code_entry e = { next, fn, name };
    ros_code_register(&e, 1);
    next += 4;
    return e.addr;
}

/* A fault in compiled code outside any SWI.  The error handler gets the
 * program's R10-R12 from here, as it would from an abort. */
static void foreground(const struct ros_cpu *s)
{
    if (ros_call_depth == 0)
        ros_env_foreground(s);
}

/* Code that a program built in RAM, which compiled code cannot contain.
 * The one kind ROSGD runs is a SWI followed by MOV pc, lr.  BASIC's SYS
 * writes the SWI into its SWICODE area and calls it (Basic.s MAIN,
 * Stmt2.s SYSCALL).  Anything else in RAM is still an error. */
static int ram_swi(struct ros_cpu *s, uint32_t addr)
{
    if (addr & 3 || !ros_arena_readable(addr, addr + 8))
        return 0;
    uint32_t swi = ros_ld32(addr), ret = ros_ld32(addr + 4);
    if (swi >> 24 != 0xEF || ret != 0xE1A0F00Eu)        /* SWI (always); MOV pc, lr */
        return 0;
    /* Make this a compiled SWI call, and do not jump to the word.  R15
     * holds the return address (past the SWI, in BASIC) and R14 is
     * preserved.  On ARM, entering a SWI saves the PC as the instruction
     * after it.  Code that saves "where the task was" resumes it there.
     * The Wimp's modal poll does this, above all in Wimp_ReportError.
     * With the jump's own address in R15 it would resume on the SWI word,
     * and the caller's checked return would fail. */
    uint32_t back = s->r[14];
    s->r[15] = back;
    ros_swi(s, swi & 0x00FFFFFFu);
    s->r[15] = back;
    return 1;
}

void ros_call(struct ros_cpu *s, uint32_t addr)
{
    /* A jump to the return-to-native marker is the code ending itself:
     * the caller's checked return sees the marker and goes on. */
    if (addr == ROS_RETURN_TO_NATIVE) {
        s->r[15] = ROS_RETURN_TO_NATIVE;
        return;
    }
    ros_code *fn = ros_code_lookup(addr);
    if (!fn) {
        /* x32 code: an application's, the C library's or a C module's
         * (capp.h).  It is called with the register block. */
        if (ros_capp_call(s, addr))
            return;
        /* ARM code: the running ARM task's, on a nested engine */
        if (ros_armrun_call(s, addr))
            return;
        if (ram_swi(s, addr))
            return;
        foreground(s);
        ros_raise(ros_error(ROS_ERR_BAD_ADDRESS,
                            "Call to &%08X, which is not compiled code", addr));
    }
    fn(s);
}

void ros_fault(struct ros_cpu *s, uint32_t addr, const char *why)
{
    foreground(s);
    ros_raise(ros_error(ROS_ERR_BAD_ADDRESS, "Compiled code at &%08X: %s", addr, why));
}

/* A return that came back somewhere other than where its call expected
 * is, to the processor, a jump there.  The code goes on at that address,
 * through the dispatcher.  BASIC's error handler CALLs its own MSGATLINE,
 * which returns through the CALL2 link its caller made, and CALL2 returns
 * where the CALL expected.  So ros_continue runs the code at R15, hop by
 * hop.  It stops when the code comes back to one of the runtime's own
 * return addresses (&FFFFFFF0 up), reaches what is not compiled code, or
 * runs away. */
#define ROS_RETURN_HOPS 64

void ros_continue(struct ros_cpu *s)
{
    for (unsigned hops = 0; s->r[15] < 0xFFFFFFF0u; hops++) {
        uint32_t at = s->r[15];
        if (hops == ROS_RETURN_HOPS || !ros_code_lookup(at))
            return;
        ros_call(s, at);
    }
}

_Thread_local struct ros_resume *ros_resume_top;

/* The registers that a jump to a resume point carries.  On ARM there is
 * one register file.  Here each entry into compiled code from C has a
 * struct of its own.  The program's error handler is the main case, and
 * the runtime enters it with a fresh struct (environment.c).  The frame
 * that a longjmp lands in reads the struct it was called with.  BASIC's
 * handler for an error in a SYS inside an FN runs on its own struct, and
 * the FN's return longjmps to EXPR's call of FACTOR.  Without this copy,
 * EXPR read the struct the SYS had left, with R15 still the SYS's return
 * (#1).  So the jump copies the jumper's registers here, and the frame it
 * lands in takes them (ros_resume_take) before anything else.  rosasm's
 * resume point checks its return first thing, and ros_check_return starts
 * by taking them. */
static _Thread_local struct ros_cpu resume_regs;
/* Visible, so that cpu.h's ros_check_return can take the fast path without
 * a call.  The fast path applies when nothing is held and the return is
 * where the call expected it. */
_Thread_local int ros_resume_regs_held;

__attribute__((noreturn)) static void resume_jump(struct ros_cpu *s, struct ros_resume *p)
{
    ros_env_base_unwind(p);             /* the error base, if it is in a frame left */
    resume_regs = *s;
    ros_resume_regs_held = 1;
    longjmp(p->jb, 1);
}

void ros_resume_take(struct ros_cpu *s)
{
    if (ros_resume_regs_held) {
        ros_resume_regs_held = 0;
        *s = resume_regs;
    }
}

/* Is there a point at `at` with this sp?  That is, is it the stored lr of
 * one live frame's BL, in that frame and no other?  A caller needs this
 * to tell a transfer to a frame that is still live from a return that
 * merely left R15 lying about.  It must not take another level's point
 * for it.  Code that recurses leaves one point per level at the same
 * address, so the address alone names several (modules/basicvfp's FN
 * return). */
int ros_resume_point_at(uint32_t at, uint32_t sp)
{
    for (struct ros_resume *p = ros_resume_top; p; p = p->prev)
        if (p->at == at && p->sp == sp)
            return 1;
    return 0;
}

void ros_resume(struct ros_cpu *s, uint32_t t)
{
    struct ros_resume *first = NULL;
    for (struct ros_resume *p = ros_resume_top; p; p = p->prev)
        if (p->at == t) {
            if (p->sp == s->r[13])
                resume_jump(s, p);
            if (!first)
                first = p;
        }
    if (first)
        resume_jump(s, first);
    ros_call(s, t);
}

/* The checked return.  It is good if R15 is the expected address, or if
 * the code has unwound to a stored lr of an ancestor frame.  The Wimp's
 * FindFont hands recurse_down_fonts a frame pointer in a register ("keep
 * track of stack for fast rewind"), and the innermost instance returns
 * straight to the outermost call.  The chain knows that address.  Only an
 * address that nothing knows is bad, and it is reported at once.  This
 * does not use ros_resume, whose last resort would run the code at R15
 * here, in the wrong frame, before the fault could be reported. */
/* The out-of-line half.  cpu.h handles "nothing held and R15 is back",
 * which is what a checked return nearly always is.  There are 700 of them
 * in BASIC alone and 1,976 in the Wimp, on every lifted module's hot
 * path. */
void ros_check_return_slow(struct ros_cpu *s, uint32_t back)
{
    ros_resume_take(s);
    if (s->r[15] == back)
        return;
    struct ros_resume *first = NULL;
    for (struct ros_resume *p = ros_resume_top; p; p = p->prev)
        if (p->at == s->r[15]) {
            if (p->sp == s->r[13])
                resume_jump(s, p);
            if (!first)
                first = p;
        }
    if (first)
        resume_jump(s, first);
    ros_bad_return(s, back);
}

/* The stack grows down, so the frames that a longjmp leaves are below its
 * target.  This discards what belongs to those frames:
 * - their resume points;
 * - the SWIs the trace has in them (a later SWI's frame could otherwise
 *   take a dead one as its outer);
 * - the entries into x32 code, whose base and stacks go back as the
 *   outermost found them;
 * - BASIC's assembler statements in them (modules/basicvfp/asmlib);
 * - the alias expansions OS_CLI has in progress there (oscli.c);
 * - the error base, if it is in one (environment.c). */
void ros_resume_unwind(const void *to)
{
    extern void basicasm_unwind_below(const void *to);
    struct ros_resume *p = ros_resume_top;
    while (p && (uintptr_t)p < (uintptr_t)to)
        p = p->prev;
    ros_resume_top = p;
    ros_env_base_unwind(to);
    ros_switrace_unwind_below(to);
    ros_capp_unwind(to);
    basicasm_unwind_below(to);
    ros_oscli_unwind_below(to);
}

void ros_bad_return(struct ros_cpu *s, uint32_t expected)
{
    if (expected >= 0xFFFFFFF0u)
        ros_continue(s);
    else
        for (unsigned hops = 0; s->r[15] != expected; hops++) {
            uint32_t at = s->r[15];
            if (hops == ROS_RETURN_HOPS || at >= 0xFFFFFFF0u || !ros_code_lookup(at))
                break;
            ros_call(s, at);
        }
    if (s->r[15] != expected) {
        /* This is at a line's start because the box's test runner looks for it there. */
        fprintf(stderr, "\nrosgd: return to &%08X where &%08X was expected; the last SWIs:\n",
                s->r[15], expected);
        ros_switrace_dump(stderr, 64);
        foreground(s);
        ros_raise(ros_error(ROS_ERR_BAD_ADDRESS,
                            "Return to &%08X where &%08X was expected", s->r[15], expected));
    }
}
