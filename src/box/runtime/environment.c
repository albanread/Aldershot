/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's kernel source
 * (Sources/Kernel: s.Middle, s.Kernel, hdr.KernelWS,
 * Resources.UK.Messages).
 */

/* environment.c: the environment handlers (environment.h).
 *
 * The table is the kernel's own (Kernel/s/Middle, AOS_Table). Each of the
 * seventeen handlers has up to three zero-page words. They are the code, its
 * R12 and its buffer, at the offsets that Kernel/hdr/KernelWS gives them.
 * The offsets were found by assembling the kernel's headers with rosasm and
 * emitting each symbol's value.
 *
 * The default handlers are the runtime's, in C. An error is reported and
 * ends the program. An exit ends it. The rest return. Ending a program ends
 * its task or, for /init, powers the box off.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/background.h"
#include "rosgd/capp.h"
#include "rosgd/environment.h"
#include "rosgd/module.h"
#include "rosgd/heap.h"
#include "rosgd/platform.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"

#define ZP(off) (ROS_ZEROPAGE + (off))

#define ERR_BAD_ENV_NUMBER 0x1B0u           /* "Bad environment number" */
#define ROS_CHANGEENVIRONMENTV 0x1Eu

/* The buffers the default handlers use. RISC OS's GeneralMOSBuffer is
 * outside the arena, so the runtime keeps one of its own in zero page.
 * DUMPER is the kernel's register dump area. */
#define DEFAULT_BUFFER ZP(0x6800)           /* 256 bytes */
#define DUMPER         ZP(0xAE8)

/* Kernel/s/Middle's AOS_Table. The columns are code, R12 and buffer. A
 * column is 0 where the handler has no such field. */
static const uint16_t fields[ROS_ENV_HANDLERS][3] = {
    [ROS_ENV_MEMORY_LIMIT]        = { 0x11C, 0, 0 },          /* MemLimit */
    [ROS_ENV_UNDEFINED]           = { 0x120, 0, 0 },          /* UndHan */
    [ROS_ENV_PREFETCH_ABORT]      = { 0x124, 0, 0 },          /* PAbHan */
    [ROS_ENV_DATA_ABORT]          = { 0x128, 0, 0 },          /* DAbHan */
    [ROS_ENV_ADDRESS_EXCEPTION]   = { 0x12C, 0, 0 },          /* AdXHan */
    [ROS_ENV_OTHER_EXCEPTION]     = { 0, 0, 0 },
    [ROS_ENV_ERROR]               = { 0x130, 0x138, 0x134 },  /* ErrHan, _ws, ErrBuf */
    [ROS_ENV_CALLBACK]            = { 0x140, 0x13C, 0x144 },  /* CallAd, _ws, CallBf */
    [ROS_ENV_BREAKPOINT]          = { 0x14C, 0x148, 0x150 },  /* BrkAd, _ws, BrkBf */
    [ROS_ENV_ESCAPE]              = { 0x158, 0x154, 0 },      /* EscHan, _ws */
    [ROS_ENV_EVENT]               = { 0x160, 0x15C, 0 },      /* EvtHan, _ws */
    [ROS_ENV_EXIT]                = { 0x57C, 0x580, 0 },      /* SExitA, _ws */
    [ROS_ENV_UNUSED_SWI]          = { 0x578, 0x574, 0 },      /* HiServ, _ws */
    [ROS_ENV_EXCEPTION_REGISTERS] = { 0x958, 0, 0 },          /* ExceptionDump */
    [ROS_ENV_APPLICATION_SPACE]   = { 0x368, 0, 0 },          /* AplWorkSize */
    [ROS_ENV_CAO]                 = { 0x7D4, 0, 0 },          /* Curr_Active_Object */
    [ROS_ENV_UPCALL]              = { 0x588, 0x584, 0 },      /* UpCallHan, _ws */
};

/* The words in the order a task's copy keeps them. */
static uint16_t words[ROS_ENV_WORDS];

static void list_words(void)
{
    if (words[0])
        return;
    unsigned k = 0;
    for (unsigned n = 0; n < ROS_ENV_HANDLERS; n++)
        for (unsigned f = 0; f < 3; f++)
            if (fields[n][f])
                words[k++] = fields[n][f];
}

/* The memory limit rises with the application slot (*WimpSlot, or a
 * language entered from the console), and the application space rises with
 * it. The kernel's MemLimit and AplWorkSize both end where the slot does.
 * The Wimp resizes a slot only where the two agree (Wimp_SlotSize). */
void ros_env_set_memory_limit(uint32_t end)
{
    ros_st32(ZP(fields[ROS_ENV_MEMORY_LIMIT][0]), end);
    ros_st32(ZP(fields[ROS_ENV_APPLICATION_SPACE][0]), end);
}

void ros_env_save(struct ros_environment *e)
{
    list_words();
    for (unsigned k = 0; k < ROS_ENV_WORDS; k++)
        e->w[k] = ros_ld32(ZP(words[k]));
}

void ros_env_load(const struct ros_environment *e)
{
    list_words();
    for (unsigned k = 0; k < ROS_ENV_WORDS; k++)
        ros_st32(ZP(words[k]), e->w[k]);
}

/* ---- the default handlers ---- */

/* End the current program: its task, or /init and with it the box. */
__attribute__((noreturn)) static void end_program(int status)
{
    struct ros_task *t = ros_task_current();
    if (t && ros_task_id(t) != 0)
        ros_task_end_program();
    /* An application entered from a command: its end returns there. */
    ros_module_app_exit();
    ros_poweroff(status);
}

void ros_env_end_program(int status)
{
    end_program(status);
}

static uint32_t task_id(void)
{
    struct ros_task *t = ros_task_current();
    return t ? ros_task_id(t) : 0;
}

/* The kernel's ERRORH (Kernel/s/Middle) writes the Kernel's "Error" message
 * to the VDU stream, so *SPOOL has it. %0 is the error's text and %1 is its
 * number in hex without leading zeros. A newline follows. The message is
 * from the UK Kernel Messages file (Kernel/Resources/UK/Messages). */
static const char error_message[] = "Error: %0 (Error number &%1)";

static void vdu_writec(uint32_t c)
{
    struct ros_cpu w;
    ros_cpu_enter(&w);
    w.r[0] = c;
    ros_swi(&w, XOS_WriteC);
}

static void vdu_write(const char *t)
{
    while (*t)
        vdu_writec((uint8_t)*t++);
}

static void default_error(struct ros_cpu *s)
{
    uint32_t buf = ros_ld32(ZP(fields[ROS_ENV_ERROR][2]));
    (void)s;
    for (const char *m = error_message; *m; m++) {
        if (*m != '%') {
            vdu_writec((uint8_t)*m);
            continue;
        }
        if (*++m == '0') {
            for (uint32_t p = buf + 8; ros_ld8(p); p++)
                vdu_writec(ros_ld8(p));
        } else if (*m == '1') {
            char hex[9];
            snprintf(hex, sizeof hex, "%X", ros_ld32(buf + 4));
            vdu_write(hex);
        }
    }
    struct ros_cpu w;
    ros_cpu_enter(&w);
    ros_swi(&w, XOS_NewLine);
    ros_console_printf("rosgd: task %u: error &%X: %s\n", task_id(), ros_ld32(buf + 4),
                       (const char *)ros_ptr(buf + 8));
    end_program(1);
}

static void default_exit(struct ros_cpu *s)
{
    (void)s;
    end_program(0);
}

static void default_exception(struct ros_cpu *s)
{
    ros_console_printf("rosgd: task %u: an exception, at &%08X\n", task_id(), s->r[14]);
    end_program(1);
}

static void default_return(struct ros_cpu *s)
{
    s->r[15] = s->r[14];
}

static uint32_t default_code[ROS_ENV_HANDLERS];

static void default_entries(void)
{
    if (default_code[ROS_ENV_ERROR])
        return;
    uint32_t exc = ros_native_entry(default_exception, "runtime:DefaultException");
    uint32_t ret = ros_native_entry(default_return, "runtime:DefaultReturn");
    default_code[ROS_ENV_UNDEFINED] = default_code[ROS_ENV_PREFETCH_ABORT] =
        default_code[ROS_ENV_DATA_ABORT] = default_code[ROS_ENV_ADDRESS_EXCEPTION] = exc;
    default_code[ROS_ENV_ERROR] = ros_native_entry(default_error, "runtime:DefaultError");
    default_code[ROS_ENV_EXIT] = ros_native_entry(default_exit, "runtime:DefaultExit");
    default_code[ROS_ENV_CALLBACK] = default_code[ROS_ENV_BREAKPOINT] =
        default_code[ROS_ENV_ESCAPE] = default_code[ROS_ENV_EVENT] =
            default_code[ROS_ENV_UNUSED_SWI] = default_code[ROS_ENV_UPCALL] = ret;
}

/* OS_ReadDefaultHandler's answer for handler n. A number with no handler
 * gives zeros, as the kernel's "wally entry" does. */
static void default_handler(uint32_t n, uint32_t *code, uint32_t *r12, uint32_t *buffer)
{
    default_entries();
    *code = n < ROS_ENV_HANDLERS ? default_code[n] : 0;
    *r12 = 0;
    *buffer = 0;
    if (n == ROS_ENV_ERROR || n == ROS_ENV_ESCAPE)
        *buffer = DEFAULT_BUFFER;
    else if (n == ROS_ENV_CALLBACK || n == ROS_ENV_BREAKPOINT)
        *buffer = DUMPER;
}

int ros_env_is_default(uint32_t n, uint32_t code)
{
    default_entries();
    return n < ROS_ENV_HANDLERS && code == default_code[n];
}

void ros_env_defaults(struct ros_environment *e, uint32_t app_end, uint32_t cao)
{
    struct ros_environment saved;
    ros_env_save(&saved);
    for (uint32_t n = 0; n < ROS_ENV_HANDLERS; n++) {
        uint32_t v[3];
        default_handler(n, &v[0], &v[1], &v[2]);
        if (n == ROS_ENV_MEMORY_LIMIT || n == ROS_ENV_APPLICATION_SPACE)
            v[0] = app_end;
        else if (n == ROS_ENV_CAO)
            v[0] = cao;
        else if (n == ROS_ENV_EXCEPTION_REGISTERS)
            v[0] = DUMPER;
        for (unsigned f = 0; f < 3; f++)
            if (fields[n][f])
                ros_st32(ZP(fields[n][f]), v[f]);
    }
    ros_env_save(e);
    ros_env_load(&saved);
}

/* ---- reading and changing ---- */

void ros_env_read(uint32_t n, uint32_t *code, uint32_t *r12, uint32_t *buffer)
{
    uint32_t *out[3] = { code, r12, buffer };
    for (unsigned f = 0; f < 3; f++)
        *out[f] = n < ROS_ENV_HANDLERS && fields[n][f] ? ros_ld32(ZP(fields[n][f])) : 0;
}

os_error *ros_env_change(uint32_t n, uint32_t *r1, uint32_t *r2, uint32_t *r3)
{
    if (n >= ROS_ENV_HANDLERS)
        return ros_error(ERR_BAD_ENV_NUMBER, "Bad environment number");
    uint32_t *r[3] = { r1, r2, r3 };
    for (unsigned f = 0; f < 3; f++) {
        if (!fields[n][f])
            continue;                   /* no such field, so the register is left alone */
        uint32_t at = ZP(fields[n][f]), was = ros_ld32(at);
        if (*r[f])
            ros_st32(at, *r[f]);
        *r[f] = was;
    }
    return NULL;
}

/* ---- raising, and exiting ---- */

static _Thread_local uint32_t fg[3];
static _Thread_local int fg_valid;

/* Call the program's exit handler as the kernel's SEXIT enters it
 * (Kernel/s/Kernel). R0 is 0, R1 is OS_Exit's R1 and R2 is the return code.
 * R10 and R11 are the caller's ("Try and recover caller R10-R12") and R12 is
 * its workspace. SharedCLibrary's ExitHandler passes R0-R2 on to the next
 * OS_Exit. The handler is entered in user mode, by SEXIT's MOVS pc, as the
 * error handler is (#1). In SVC mode, a handler that set up a stack of its
 * own in the application slot and polled the Wimp had the Wimp's frames put
 * on that stack, where the task switch pages them out (#66). The handler is
 * not expected to return. If it does, the program ends. */
static void call_exit_handler(uint32_t code, uint32_t r12, const uint32_t r[3])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.mode = ROS_MODE_USR;
    s.r[0] = r[0], s.r[1] = r[1], s.r[2] = r[2];
    if (fg_valid)
        s.r[10] = fg[0], s.r[11] = fg[1];
    s.r[12] = r12;
    ros_call(&s, code);
}
/* All of them, R0-R15, for an exception in the SWI (fault.h). */
static _Thread_local uint32_t fg_all[16];

void ros_env_foreground(const struct ros_cpu *s)
{
    fg[0] = s->r[10], fg[1] = s->r[11], fg[2] = s->r[12];
    fg_valid = 1;
    memcpy(fg_all, s->r, sizeof fg_all);
}

int ros_env_foreground_regs(uint32_t r[16])
{
    memcpy(r, fg_all, sizeof fg_all);
    return fg_valid;
}

/* Enter the error handler. R0 is R12's value and R10-R12 are the program's
 * (ErrHandler). The handler is entered in user mode, as the kernel's MOVS pc
 * enters it. It carries on as the program, and BASIC's goes back to its
 * interpreter. So the mode is the program's from here on. The handler was
 * once entered in SVC mode. Then a BASIC program that had trapped an error
 * called Wimp_Poll in SVC mode, the SWI kept the program's R13 as its stack,
 * and the Wimp's frames went onto BASIC's stack in the application slot. The
 * task switch then paged that stack out (!Maestro: a data abort in
 * wimp_ExitPoll, #1). */
static void call_error_handler(uint32_t code, uint32_t r12)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.mode = ROS_MODE_USR;
    s.r[0] = r12;
    s.r[12] = r12;
    if (fg_valid)
        s.r[10] = fg[0], s.r[11] = fg[1], s.r[12] = fg[2];
    ros_call(&s, code);
}

/* An error goes to the program's error handler as the kernel sends it
 * (Kernel/s/Kernel, ErrHandler). The redirection and temporary filing
 * system are tidied (OscliTidy). The handler and its buffer are checked, and
 * the defaults are put back if either is unusable. The error is copied into
 * the buffer. The stacks are flattened before the handler is entered.
 *
 * The handler does not return. It carries on as the program, and BASIC's
 * goes back to its interpreter. So the next error is raised from inside the
 * handler. Flattening keeps that from nesting. The first error on a task
 * sets a base here. Each later error unwinds to the base, leaving the SWIs
 * it was in and putting the SVC stack back where it was. It then enters the
 * handler from there. A handler that fails every time goes round for ever,
 * as on RISC OS, but the stack does not grow. */
static _Thread_local struct ros_env_base base;

/* This is called for a longjmp out to the frame at `to` (dispatch.c: a
 * resume point taken, or the runtime's own unwinding). Suppose the base is
 * in a frame that the jump leaves. The handler has then jumped out of the
 * error it was entered for, as BASIC's does when an FN with an ON ERROR
 * LOCAL returns to EXPR. The base names a frame that has returned, and the
 * next error would longjmp into dead stack. So the base is cleared. The next
 * error makes a base of its own frame (#65). */
void ros_env_base_unwind(const void *to)
{
    if (base.active && (uintptr_t)base.frame < (uintptr_t)to)
        base.active = 0;
}

void ros_env_base_swap(struct ros_env_base *other)
{
    struct ros_env_base t = base;
    base = *other;
    *other = t;
}

static void tidy_oscli(void)
{
    ros_redirect_tidy();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 19;                        /* OS_FSControl RestoreCurrent */
    /* This is the runtime's own call. The program's R10-R12, which its
     * handler is to be entered with, stay as the error left them. */
    uint32_t keep[3] = { fg[0], fg[1], fg[2] }, keep_all[16];
    memcpy(keep_all, fg_all, sizeof keep_all);
    int valid = fg_valid;
    ros_swi(&c, XOS_FSControl);
    memcpy(fg, keep, sizeof fg);
    memcpy(fg_all, keep_all, sizeof fg_all);
    fg_valid = valid;
}

__attribute__((noreturn)) static void enter_error_handler(void)
{
    for (;;) {
        uint32_t code, r12, buf;
        ros_env_read(ROS_ENV_ERROR, &code, &r12, &buf);
        call_error_handler(code, r12);
        /* It returned, so the program is over. */
        end_program(1);
    }
}

void ros_env_raise(const os_error *e)
{
    uint32_t pc = ros_error_pc_take();
    tidy_oscli();
    uint32_t code, r12, buf;
    ros_env_read(ROS_ENV_ERROR, &code, &r12, &buf);
    if (!code || !buf) {
        ros_console_printf("rosgd: unhandled error &%X: %s\n", e->errnum, e->errmess);
        end_program(1);
    }
    /* x32 code has no alignment, and may be execute-only. */
    int x32 = ros_capp_code(code) != 0;
    if ((!x32 && ((code & 3) || !ros_arena_readable(code, code + 4))) || (buf & 3) ||
        !ros_arena_valid(buf, buf + 256 + 4)) {
        default_handler(ROS_ENV_ERROR, &code, &r12, &buf);      /* obviously unusable */
        ros_st32(ZP(fields[ROS_ENV_ERROR][0]), code);
        ros_st32(ZP(fields[ROS_ENV_ERROR][1]), r12);
        ros_st32(ZP(fields[ROS_ENV_ERROR][2]), buf);
    }
    /* The buffer holds the pc of the error (an exception's, else 0), then
     * the error block. */
    uint32_t errnum = e->errnum;
    char mess[252];
    size_t n = strnlen(e->errmess, sizeof mess - 1);
    memcpy(mess, e->errmess, n);        /* the block may be in the buffer itself */
    ros_st32(buf, pc);
    ros_st32(buf + 4, errnum);
    memcpy(ros_ptr(buf + 8), mess, n);
    ros_st8(buf + 8 + (uint32_t)n, 0);
    /* A C application's handler is delivered here. Everything since the
     * program was entered is flattened (capp.h). R0 = R12 as for compiled
     * code. */
    ros_capp_deliver(code, r12, (const uint32_t[3]){ r12, 0, 0 }, 1);
    if (base.active) {
        ros_svc_sp = base.svc_sp;
        ros_call_depth = base.call_depth;
        ros_resume_unwind(base.frame);
        longjmp(base.jb, 1);
    }
    base.active = 1;
    base.svc_sp = ros_svc_sp;
    base.call_depth = ros_call_depth;
    base.frame = __builtin_frame_address(0);
    setjmp(base.jb);
    enter_error_handler();
}

/* OS_Exit, as the kernel's SEXIT (Kernel/s/Kernel). The redirection is shut
 * and the filing system restored first (OscliTidy). So what the exit handler
 * prints is not redirected. SharedCLibrary's exit handler runs the atexit
 * functions. Next comes the return code, Sys$ReturnCode. It is R2 if R1 is
 * "ABEX" and R0 could be an error pointer (word-aligned), and otherwise 0.
 * Past Sys$RCLimit (unsigned, BHI) it is an error, raised whatever the X bit
 * (OS_GenerateError). The error is R0's if it looks like one (at &4000 or
 * above, with 8 bytes readable). Otherwise it is the kernel's own, "Return
 * code limit exceeded" or "Negative return code", as Sys$ReturnCode's
 * setting gives them. If there is no error, the exit goes to the program's
 * exit handler, as the kernel enters it. OS_Exit does not return, so the SVC
 * stack is flat and the handler's SWIs are the outermost. Their exits run
 * callbacks, and the Wimp's exit handler polls to leave a dead task, because
 * a task switch is a callback. When this was nested in OS_Exit, that
 * Wimp_Poll came back to the dead task, because the switch to the next task
 * never ran. */
os_error *xos_exit(const os_error *error, uint32_t abex, uint32_t rc)
{
    tidy_oscli();
    uint32_t r0 = error ? ros_addr(error) : 0;
    if (!(abex == 0x58454241u && !(r0 & 3)))
        rc = 0;
    ros_st32(ROS_ZP_RETURN_CODE, rc);
    uint32_t limit = ros_ld32(ROS_ZP_RC_LIMIT);
    if (rc > limit) {
        if (r0 < 0x4000 || !ros_arena_readable(r0, r0 + 8))
            error = (int32_t)rc > (int32_t)limit
                        ? ros_error(0x1E2u, "Return code limit exceeded")
                        : ros_error(0x1E2u, "Negative return code");
        ros_env_raise(error);
    }
    uint32_t code, r12, unused;
    ros_env_read(ROS_ENV_EXIT, &code, &r12, &unused);
    /* The handler's R0-R2 as SEXIT gives them. R0 is 0, R1 is as it came
     * and R2 is the return code. */
    uint32_t regs[3] = { 0, abex, rc };
    ros_capp_deliver(code, r12, regs, 0);       /* a C application's handler is delivered */
    ros_svc_sp = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;
    ros_call_depth = 0;
    call_exit_handler(code, r12, regs);
    end_program(0);
}

os_error *xos_read_default_handler(uint32_t handler, uint32_t *code, uint32_t *r12,
                                   uint32_t *buffer)
{
    default_handler(handler, code, r12, buffer);
    return NULL;
}

/* OS_ChangeEnvironment goes through ChangeEnvironmentV, whose default owner
 * is the table above. Registers pass straight through, as the vector's
 * claimants see them. */
void ros_thunk_OS_ChangeEnvironment(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    s->v = 0;
    if (!ros_vector_call(ROS_CHANGEENVIRONMENTV, s)) {
        os_error *e = ros_env_change(s->r[0], &s->r[1], &s->r[2], &s->r[3]);
        if (e)
            ros_swi_fail(s, e);
    }
    ros_svc_sp = outer_sp;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
}

/* ---- the program's environment: OS_GetEnv, OS_WriteEnv ---- */

/* The command line the program was started with, and the time it started.
 * These are the kernel's EnvString and EnvTime. EnvTime is at its zero page
 * offset (Kernel/hdr/KernelWS). EnvString is in the kernel's buffers, which
 * are outside what the arena maps, so it is kept in zero page here. */
#define ENV_STRING ZP(0x6900)               /* 1024 bytes */
#define ENV_STRING_SIZE 1024u
#define ENV_TIME ZP(0xADC)                  /* five bytes */

void ros_thunk_OS_GetEnv(struct ros_cpu *s)
{
    s->r[0] = ENV_STRING;
    s->r[1] = ros_ld32(ZP(fields[ROS_ENV_MEMORY_LIMIT][0]));
    s->r[2] = ENV_TIME;
    s->v = 0;
}

/* OS_WriteEnv: R0 points to the command line, up to a control character
 * (0 leaves it as it is). R1 points to five bytes of time (0 leaves it as it
 * is). */
void ros_thunk_OS_WriteEnv(struct ros_cpu *s)
{
    if (s->r[0]) {
        uint32_t i = 0;
        for (uint8_t c; i < ENV_STRING_SIZE - 1 && (c = ros_ld8(s->r[0] + i)) >= ' '; i++)
            ros_st8(ENV_STRING + i, c);
        ros_st8(ENV_STRING + i, 0);
    }
    if (s->r[1])
        for (uint32_t i = 0; i < 5; i++)
            ros_st8(ENV_TIME + i, ros_ld8(s->r[1] + i));
    s->v = 0;
}

/* OS_EnterOS: SVC mode, which is the runtime's one mode already. */
void ros_thunk_OS_EnterOS(struct ros_cpu *s)
{
    s->mode = ROS_MODE_SVC;
    s->v = 0;
}

/* OS_UpCall goes through UpCallV. If no one claims it, it goes to the
 * program's UpCall handler with R12 set to the handler's own, as the
 * kernel's default owner does. */
void ros_thunk_OS_UpCall(struct ros_cpu *s)
{
    uint32_t outer = ros_svc_sp_enter(s);
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    s->v = 0;
    if (!ros_vector_call(ROS_UPCALLV, s)) {
        uint32_t code, h12, unused;
        ros_env_read(ROS_ENV_UPCALL, &code, &h12, &unused);
        if (code) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            memcpy(c.r, s->r, 10 * sizeof c.r[0]);
            c.r[12] = h12;
            ros_call(&c, code);
            memcpy(s->r, c.r, 10 * sizeof c.r[0]);
            s->v = c.v;
        }
    }
    s->r[10] = r10, s->r[11] = r11, s->r[12] = r12;
    ros_svc_sp = outer;
}

/* The escape condition is set or cleared, as the kernel's DoOsbyte7D and
 * DoOsbyte7C do it. The flag goes in ESC_Status. Then the program's escape
 * handler is told, with R11 &FF or 0 and R12 its own. This is how BASIC hears
 * of it. BASIC's OSESCR keeps its own flag, which the interpreter loop
 * polls. A handler that hands back R12 = 1 asks for a callback (Exit7D). */
void ros_env_escape(int set)
{
    uint32_t st = ros_ld8(ZP(0x104));
    ros_st8(ZP(0x104), set ? st | 0x40u : st & ~0x40u);
    uint32_t code, r12, buffer;
    ros_env_read(ROS_ENV_ESCAPE, &code, &r12, &buffer);
    if (!code || ros_env_is_default(ROS_ENV_ESCAPE, code))
        return;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[11] = set ? 0xFFu : 0;
    c.r[12] = r12;
    ros_call(&c, code);
    if (c.r[12] == 1) {
        struct ros_cpu cb;
        ros_cpu_enter(&cb);
        ros_swi(&cb, XOS_SetCallBack);
    }
}
