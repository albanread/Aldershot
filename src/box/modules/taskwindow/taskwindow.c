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
 * This file is a reimplementation in C of RISC OS Open's TaskWindow module
 * (Sources/Desktop/TaskWindow: s.Taskman, s.TaskWindow, s.Messages).
 */

/* taskwindow.c: the TaskWindow module, native.  It runs a command as a Wimp
 * task of its own.  What the command prints is sent to its parent as
 * messages, and what it reads is taken from them.  The source it was taken
 * from is RISC OS's Desktop/TaskWindow (s/Taskman, version 0.85).
 *
 * A task window is this module entered as the application (OS_Module 2) in
 * a task that the Wimp started, on that task's thread.  It provides the
 * command loop, and the vector claims and the CallBack that its program
 * calls into.  Each task window's state is a block in the RMA.  The Wimp
 * reads parts of it by address: its messages, its poll block and its poll
 * word.  The chain of blocks starts in the module's workspace.
 *
 * It yields to the other tasks by polling the Wimp.  It polls when it waits
 * for input, on every tenth full output buffer, and when its time slice has
 * run.  The time slice is counted on TickerV and taken at the program's
 * next outermost SWI exit by a CallBack.  RISC OS's module copies the SVC
 * stack away, so that it can poll from user mode with the stack empty.
 * Here each task is a thread with an SVC stack of its own.  So the poll is
 * made the outermost SWI where it stands, inside OS_WriteC, OS_ReadC or a
 * CallBack.  The Wimp's task switch is a CallBack on its way out.  It parks
 * the thread, with those frames, until the Wimp comes back (README.md,
 * "Time slices").
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"
#include "taskwindow.h"

#define TASK_WORD   0x4B534154u        /* "TASK" */
#define WIMP_VER    300u
#define MSG_QUIT    0u
#define MSG_DATASAVE 1u
#define MSG_RAMTRANSMIT 7u
#define USER_MESSAGE 17u
#define USER_RECORDED 18u
#define USER_ACK    19u

#define WRCHV 0x03u
#define RDCHV 0x04u
#define BYTEV 0x06u

#define UPCALL_SLEEP        6u
#define UPCALL_SLEEP_NO_MORE 7u

#define OS_CLI_SWI  0x05u              /* OS_CLI, not XOS_CLI, so an error goes to the error handler */

/* Wimp_Poll's mask for a task window (grotpollwimp).  It excludes redraws,
 * pointer events and caret events, and keeps the floating point.  What the
 * caller adds follows. */
#define POLL_MASK      0x01001832u
#define NULL_BIT       1u              /* no null events */
#define POLLWORD       (1u << 22)      /* the poll word, at low priority */
#define POLLWORD_FAST  (3u << 22)      /* ... at high priority */

#define EVENT_COUNT 10u                /* a time slice's centiseconds, by default */
#define OBUF 232u                      /* the output ring: 231 bytes used */
#define KBUF 256u                      /* the key ring: 255 */

#define ERR_WIMP_CANT_KILL ROS_ERR_WIMP_CANT_KILL
#define ERR_BAD_SWI        0x1E6u
#define ERR_ESCAPE         0x11u
#define ERR_EXIT           0x1000u

enum { OUT, KEYS };

/* One task window: RISC OS's task block, in the RMA */
struct tw {
    uint32_t next;                  /* the next task window's block, 0 none */
    uint32_t global;                /* the module's workspace */
    /* The parent is the task that hears the output.  It is 0 while there is
     * none yet, and that is the poll word that a task window asking for one
     * waits on.  parent_txt is the parent's handle for this task window,
     * which the Ego message echoes.  It is 1 while the task window is
     * asking. */
    uint32_t parent_task, parent_txt;
    uint32_t child_task;            /* its own task handle */
    uint32_t domain;                /* DomainId while it is the current task */
    /* Whether it is on the vectors (PassOnVectors).  It is 0 while its program
     * runs, 1 while its own code runs, and 2 while the Wimp reports an
     * error. */
    uint32_t pass_on;
    uint32_t moribund, suspended, req_die;
    uint32_t quit, ctrl;            /* -quit, -ctrl */
    uint32_t vdisplay;              /* -vdisplay: a virtual display's handle, 0 for none */
    uint32_t nice, slice;           /* a time slice's length, and how far it has run */
    uint32_t output_count;          /* full output buffers since it last polled for them */
    uint32_t inkey_count;           /* centiseconds an INKEY still waits */
    uint32_t poll_word;             /* what UpCall_Sleep sleeps on, 0 none */
    uint32_t poll_result;           /* the last Wimp_Poll's reason */
    uint32_t command;               /* what to run next, 0 none: the prompt */
    uint32_t name;                  /* the task's name */
    uint32_t exp_pointer;           /* a function key's expansion, still to read */
    uint32_t kb_head, kb_tail, ob_head, ob_tail;
    uint32_t old_error[3], old_exit[3], old_callback[3];
    uint32_t callback_regs[17];     /* the CallBack's register dump */
    uint32_t messages[8];           /* Wimp_Initialise's list */
    uint32_t msg[64];               /* the messages it sends */
    uint32_t block[64];             /* Wimp_Poll's block */
    uint32_t args[256];             /* OS_ReadArgs' output: the command, the options */
    uint8_t escape_disable;         /* *FX 229 as the program set it */
    uint8_t esc_pending;            /* an Escape to give the program: 125, or 0 */
    uint8_t esc_was_set, task_escape;
    uint8_t aborted, control_counter, exec_handle, spool_handle;
    uint8_t fn_flag, pad[3];
    uint8_t wimp_keys[8], task_keys[8];     /* OS_Byte 221-228 */
    uint8_t error_buffer[264];      /* the error handler's: pc, number, text */
    uint8_t exp_buffer[256];
    uint8_t kbuf[KBUF], obuf[OBUF];
    char title[16];                 /* "TaskWindow", the name by default */
    char scratch[264];
    char line[1028];                /* a command read at the prompt */
};

/* The module's workspace */
struct global {
    uint32_t first;                 /* the task windows (FirstWord) */
};

/* Where a task window's command loop stands, on its own thread.  The exit
 * handler goes back there.  RISC OS's handler resets its stack and carries
 * on. */
struct running {
    jmp_buf jb;
    const void *frame;
    struct ros_handler *handlers;
    struct tw *t;
};
static _Thread_local struct running *here;

/* The environment handlers' native entries (ros_native_entry) */
static uint32_t e_exit, e_error, e_callback;

static struct tw *TW(uint32_t b) { return ros_ptr(b); }
static struct global *G(uint32_t w) { return ros_ptr(w); }
static uint32_t at(const void *p) { return ros_addr(p); }

/* ---- calls ------------------------------------------------------------------- */

/* A SWI with R0-R7 from r, and back into it; *c its C flag if wanted */
static os_error *call(uint32_t swi, uint32_t *r, uint32_t *c)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, swi);
    memcpy(r, s.r, 8 * sizeof r[0]);
    if (c)
        *c = s.c;
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

#define REGS(...) ((uint32_t[8]){ __VA_ARGS__ })

static uint32_t osbyte(uint32_t a, uint32_t x, uint32_t y, uint32_t *y_out)
{
    uint32_t r[8] = { a, x, y };
    call(XOS_Byte, r, NULL);
    if (y_out)
        *y_out = r[2];
    return r[1];
}

static void find_close(uint32_t h)
{
    call(XOS_Find, REGS(0, h), NULL);
}

static uint32_t str(char *buf, const char *s)
{
    strcpy(buf, s);
    return at(buf);
}

static int escape_state(void)
{
    uint32_t c = 0;
    call(XOS_ReadEscapeState, REGS(0), &c);
    return (int)c;
}

static os_error *dying(void)
{
    return ros_error(TASKWINDOW_ERR_DYING, "Task dying");
}

static void fail(struct ros_cpu *s, const os_error *e)
{
    s->v = 1;
    s->r[0] = ros_addr(e);
}

/* ---- the chain ---------------------------------------------------------------- */

/* The task window whose program is running now.  It is the current task's, and
 * it is on its vectors.  This is RISC OS's FindOwner, with FixDomain's check
 * of DomainId. */
static struct tw *owner(struct global *g)
{
    uint32_t domain = ros_ld32(ROS_ZP_DOMAINID);
    for (uint32_t b = g->first; b; b = TW(b)->next)
        if (TW(b)->domain == domain && TW(b)->pass_on == 0)
            return TW(b);
    return NULL;
}

/* A task window that is waiting to be given a parent (FindWaiting).  It has
 * no parent, and it has the text handle 1 that its NewTask left.  (RISC OS
 * 5.30 does not check for the parent: README.md.) */
static struct tw *waiting(struct global *g)
{
    for (uint32_t b = g->first; b; b = TW(b)->next)
        if (TW(b)->parent_task == 0 && TW(b)->parent_txt == 1)
            return TW(b);
    return NULL;
}

static void unlink_block(struct tw *t)
{
    struct global *g = G(t->global);
    for (uint32_t *p = &g->first; *p; p = &TW(*p)->next)
        if (*p == at(t)) {
            *p = t->next;
            break;
        }
    xos_module_free(t);
}

/* ---- the rings ---------------------------------------------------------------- */

static uint8_t *ring(struct tw *t, int which, uint32_t **head, uint32_t **tail, uint32_t *size)
{
    *head = which == KEYS ? &t->kb_head : &t->ob_head;
    *tail = which == KEYS ? &t->kb_tail : &t->ob_tail;
    *size = which == KEYS ? KBUF : OBUF;
    return which == KEYS ? t->kbuf : t->obuf;
}

/* Inserts a character.  It returns 1 if the ring was full.  An Escape put
 * into the keys, while the program allows Escape, becomes an escape
 * condition instead.  While one is pending, neither ring takes anything. */
static int insert(struct tw *t, int which, uint8_t ch)
{
    if (which == KEYS && ch == 27 && t->escape_disable == 0) {
        t->esc_pending = 125;
        return 0;
    }
    if (t->esc_pending)
        return 0;
    uint32_t *head, *tail, size;
    uint8_t *b = ring(t, which, &head, &tail, &size);
    if ((*head + size - *tail) % size == 1)
        return 1;
    b[*tail] = ch;
    *tail = (*tail + 1) % size;
    return 0;
}

static int extract(struct tw *t, int which, uint8_t *ch)
{
    uint32_t *head, *tail, size;
    uint8_t *b = ring(t, which, &head, &tail, &size);
    if (*head == *tail)
        return 0;
    *ch = b[*head];
    *head = (*head + 1) % size;
    return 1;
}

static void clear(struct tw *t, int which)
{
    uint32_t *head, *tail, size;
    ring(t, which, &head, &tail, &size);
    *head = *tail = 0;
}

/* ---- escape, keys, exec and spool ---------------------------------------------- */

/* SaveEscape: keeps any escape condition for the program and clears it.  The
 * escape key is off while the task window's own code runs. */
static void save_escape(struct tw *t)
{
    int esc = escape_state();
    t->esc_pending = esc ? 125 : 0;
    if (esc)
        osbyte(124, 0, 0, NULL);
    osbyte(229, 1, 0, NULL);
}

/* RestoreEscape: the escape key stays off, because the task window emulates
 * it.  The condition is set again if one was kept. */
static void restore_escape(struct tw *t)
{
    osbyte(229, 1, 0, NULL);
    t->esc_was_set = t->esc_pending;
    t->esc_pending = 0;
    if (t->esc_was_set == 125)
        osbyte(125, 0, 0, NULL);
}

static void read_keys(uint8_t k[8])
{
    for (int i = 7; i >= 0; i--)
        k[i] = (uint8_t)osbyte(221 + (uint32_t)i, 0, 0xFF, NULL);
}

static void set_keys(const uint8_t k[8])
{
    for (int i = 7; i >= 0; i--)
        osbyte(221 + (uint32_t)i, k[i], 0, NULL);
}

static void close_my_exec(struct tw *t)
{
    uint32_t h = t->exec_handle;
    t->exec_handle = 0;
    if (h)
        find_close(h);
}

/* The OS's *Exec file, if it has one, becomes the task window's own */
static void take_exec(struct tw *t)
{
    uint32_t h = osbyte(198, 0, 0, NULL);
    if (h) {
        close_my_exec(t);
        t->exec_handle = (uint8_t)h;
    }
}

static void redirection(uint32_t in, uint32_t out, uint32_t *old_in, uint32_t *old_out)
{
    uint32_t r[8] = { in, out };
    call(XOS_ChangeRedirection, r, NULL);
    if (old_in)
        *old_in = r[0];
    if (old_out)
        *old_out = r[1];
}

/* ---- the Wimp -------------------------------------------------------------------- */

/* Wimp_Poll (time 0) or Wimp_PollIdle, made as the outermost SWI wherever
 * the task window's code is.  Its way out is the Wimp's task switch.  That
 * parks this thread until the Wimp comes back to the task.  Every frame the
 * thread is inside, on its own C and SVC stacks, stays parked with it. */
static os_error *poll_outermost(uint32_t mask, uint32_t block, uint32_t time, uint32_t word,
                                uint32_t *reason)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = mask, c.r[1] = block, c.r[2] = time, c.r[3] = word;
    c.mode = ROS_MODE_USR;
    uint32_t depth = ros_call_depth;
    ros_call_depth = 0;
    ros_swi(&c, time ? XWimp_PollIdle : XWimp_Poll);
    ros_call_depth = depth;
    *reason = c.r[0];
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* Wimp_StartTask, likewise made as the outermost SWI wherever the task
 * window's code is.  The new task runs at once.  The Wimp's switch to it is
 * a CallBack on the SWI's way out, and that parks this thread.  The Wimp
 * comes back here when the new task first polls.  Suppose it were made from
 * inside a SWI (WrchV's GetParentTask).  The switch would wait for the
 * outermost SWI's way out and meet the next Wimp_Poll's switch there.  That
 * would be two switches in one CallBack, so the task window would be saved
 * into one task's block and gone back to through another's. */
static os_error *start_task_outermost(uint32_t command)
{
    uint32_t in, out;
    redirection(0, 0, &in, &out);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = command;
    c.mode = ROS_MODE_USR;
    uint32_t depth = ros_call_depth;
    ros_call_depth = 0;
    ros_swi(&c, XWimp_StartTask);
    ros_call_depth = depth;
    redirection(in, out, NULL, NULL);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static os_error *send(struct tw *t, uint32_t reason, uint32_t to)
{
    return call(XWimp_SendMessage, REGS(reason, at(t->msg), to), NULL);
}

/* SendOutput: sends the output ring to the parent.  The ring is emptied even
 * when the output cannot go, because the task is dying or has no parent. */
static void send_output(struct tw *t)
{
    uint8_t *d = (uint8_t *)&t->msg[6], ch;
    uint32_t n = 0;
    while (extract(t, OUT, &ch))
        d[n++] = ch;
    if (!n || t->moribund)
        return;
    t->msg[0] = (24 + n + 3) & ~3u;
    t->msg[3] = 0;
    t->msg[4] = TASKWINDOW_OUTPUT;
    t->msg[5] = n;
    if (t->parent_task)
        send(t, USER_MESSAGE, t->parent_task);
}

static os_error *send_ego(struct tw *t)
{
    t->msg[0] = 24, t->msg[3] = 0, t->msg[4] = TASKWINDOW_EGO, t->msg[5] = t->parent_txt;
    return send(t, USER_MESSAGE, t->parent_task);
}

/* NewTask, sent to everyone, says that a parent is wanted.  Once it is sent,
 * the task window is waiting for one (text handle 1). */
static os_error *send_newtask(struct tw *t)
{
    const char *what = "TaskWindow";
    uint32_t n = (uint32_t)strlen(what) + 1;
    memcpy(&t->msg[5], what, n);
    t->msg[0] = (23 + n) & ~3u, t->msg[3] = 0, t->msg[4] = TASKWINDOW_NEWTASK;
    os_error *e = send(t, USER_MESSAGE, 0);
    if (!e)
        t->parent_txt = 1;
    return e;
}

/* PollWimpFn and grotpollwimp: polls the Wimp for the task window.  First it
 * takes its *Exec file from the OS, moves the spool and redirection handles
 * out of the Wimp's way, and puts the Wimp's function keys in place.  Then
 * it deals with what comes: Input is buffered and the parent's orders are
 * taken.  If it is suspended, it polls on until it is resumed or killed.
 * extra goes into the mask (NULL_BIT, the poll word bits).  Time 0 is
 * Wimp_Poll, and otherwise it is Wimp_PollIdle until that time. */
static os_error *poll(struct tw *t, uint32_t extra, uint32_t time, uint32_t word)
{
    take_exec(t);
    t->spool_handle = (uint8_t)osbyte(199, 0, 0, NULL);
    read_keys(t->task_keys);
    set_keys(t->wimp_keys);
    os_error *e;
    for (;;) {
        uint32_t in, out, reason;
        redirection(0, 0, &in, &out);
        e = poll_outermost(POLL_MASK | extra, at(t->block), time, word, &reason);
        t->poll_result = e ? 0 : reason;
        redirection(in, out, NULL, NULL);
        if (!e && (reason == USER_MESSAGE || reason == USER_RECORDED)) {
            uint32_t action = t->block[4], from = t->block[1];
            if (action == MSG_QUIT) {
                t->moribund = 1;
                break;
            }
            if (from == t->parent_task) {
                if (reason == USER_RECORDED || action == MSG_DATASAVE) {
                    /* acknowledged; a paste (DataSave) is not taken yet */
                    if (reason == USER_RECORDED) {
                        t->block[3] = t->block[2];
                        call(XWimp_SendMessage, REGS(USER_ACK, at(t->block), from), NULL);
                    }
                }
                if (action == TASKWINDOW_INPUT) {
                    const uint8_t *d = (const uint8_t *)&t->block[6];
                    for (uint32_t i = 0; i < t->block[5] && i < 256 - 24; i++)
                        insert(t, KEYS, d[i]);
                } else if (action == TASKWINDOW_MORITE) {
                    t->moribund = 1;
                    break;
                } else if (action == TASKWINDOW_SUSPEND) {
                    t->suspended = 1;
                    continue;
                } else if (action == TASKWINDOW_RESUME) {
                    t->suspended = 0;
                }
            }
        } else if (!e && reason == USER_ACK) {
            t->moribund = 1;            /* one of its messages came back: it dies */
            break;
        }
        if (t->moribund || !t->suspended)
            break;
    }
    read_keys(t->wimp_keys);
    set_keys(t->task_keys);
    osbyte(199, t->spool_handle, 0, NULL);
    return e;
}

/* GetParentTask: gets a parent if the task window has none.  It sends
 * NewTask, then polls on its parent-task word until an editor fills it in.
 * If nobody answers, it runs TaskWindow$Server (!Edit) and asks again. */
static os_error *need_parent(struct tw *t)
{
    if (t->parent_task)
        return NULL;
    os_error *e = send_newtask(t);
    while (!e) {
        e = poll(t, POLLWORD_FAST, 0, at(&t->parent_task));
        if (e)
            break;
        if (t->moribund)
            return dying();
        if (t->poll_result == 0) {
            /* a null event: nobody has taken the NewTask */
            read_keys(t->task_keys);
            set_keys(t->wimp_keys);
            uint32_t r[8] = { str(t->scratch, "TaskWindow$Server"), 0, 0xFFFFFFFFu, 0, 3 };
            call(XOS_ReadVarVal, r, NULL);
            if (r[2] == 0)
                return ros_error(TASKWINDOW_ERR_NO_EDITOR, "Task window cannot be opened");
            e = start_task_outermost(str(t->scratch, "%Run <TaskWindow$Server>"));
            if (e)
                break;
            read_keys(t->wimp_keys);
            set_keys(t->task_keys);
            e = send_newtask(t);
            if (!e)
                e = poll(t, POLLWORD_FAST, 0, at(&t->parent_task));
            if (e)
                break;
            if (t->moribund)
                return dying();
            if (t->poll_result == 0)
                return ros_error(TASKWINDOW_ERR_NO_EDITOR, "Task window cannot be opened");
        }
        if (t->parent_task)
            return send_ego(t);
    }
    return e;
}

/* ---- input ----------------------------------------------------------------------- */

/* GetKbInput: a character from the key ring, with function keys expanded.
 * A function key arrives as a 0 and then a code with bit 7 set.  OS_Byte
 * 221-228 say what to do for the code's range.  The code is ignored, or
 * expanded as Key$<n>, or passed as a 0 and the code, or turned into a base
 * plus its low four bits. */
static int get_kb(struct tw *t, uint8_t *ch)
{
    for (;;) {
        if (t->exp_pointer) {
            *ch = t->exp_buffer[--t->exp_pointer];
            return 1;
        }
        uint8_t c;
        if (!extract(t, KEYS, &c))
            return 0;
        if (!t->fn_flag) {
            if (c == 0) {
                t->fn_flag = 1;
                continue;
            }
            *ch = c;
            return 1;
        }
        t->fn_flag = 0;
        if (!(c & 0x80)) {
            *ch = c;
            return 1;
        }
        uint32_t base = osbyte(221 + (((c & 0x70u) ^ 0x40u) >> 4), 0, 0xFF, NULL);
        if (base == 0)
            continue;
        if (base == 2) {
            t->exp_buffer[0] = c;
            t->exp_pointer = 1;
            *ch = 0;
            return 1;
        }
        if (base != 1) {
            *ch = (uint8_t)((c & 15) + base);
            return 1;
        }
        char name[8];
        snprintf(name, sizeof name, "Key$%u", c & 15u);
        uint32_t r[8] = { str(t->scratch, name), at(t->line), 256, 0, 3 };
        if (!call(XOS_ReadVarVal, r, NULL) && r[2]) {
            for (uint32_t i = 0; i < r[2] && i < sizeof t->exp_buffer; i++)
                t->exp_buffer[r[2] - 1 - i] = (uint8_t)t->line[i];
            t->exp_pointer = r[2] < sizeof t->exp_buffer ? r[2] : sizeof t->exp_buffer;
        }
    }
}

/* Closes the redirection files and turns redirection off */
static void close_redirection(void)
{
    uint32_t in, out;
    redirection(0, 0, &in, &out);
    if (in)
        find_close(in);
    if (out)
        find_close(out);
}

static int bget(uint32_t h, uint8_t *ch, os_error **e)
{
    uint32_t r[8] = { 0, h }, c = 0;
    *e = call(XOS_BGet, r, &c);
    *ch = (uint8_t)r[0];
    return !*e && !c;
}

/* GetInput: a character from where the program reads.  That is the
 * redirection file, then the task window's *Exec file, then the key ring.
 * It returns 1 if a character came, or if there was an error, which is in
 * *e. */
static int get_input(struct tw *t, uint8_t *ch, os_error **e)
{
    *e = NULL;
    uint32_t in, out;
    redirection(0xFFFFFFFFu, 0xFFFFFFFFu, &in, &out);
    if (in) {
        if (bget(in, ch, e))
            return 1;
        if (*e) {
            close_redirection();
            return 1;
        }
        redirection(0, 0xFFFFFFFFu, NULL, NULL);            /* at its end */
        *e = call(XOS_Find, REGS(0, in), NULL);
        if (*e)
            return 1;
    }
    take_exec(t);
    if (t->exec_handle) {
        if (bget(t->exec_handle, ch, e))
            return 1;
        close_my_exec(t);
        if (*e)
            return 1;
    }
    return get_kb(t, ch);
}

/* ---- dying ------------------------------------------------------------------------- */

static void callback_request(struct tw *t)
{
    uint32_t r[8] = { ROS_ENV_CALLBACK, e_callback, at(t), at(t->callback_regs) };
    call(XOS_ChangeEnvironment, r, NULL);
    if (r[1] != e_callback)
        memcpy(t->old_callback, &r[1], sizeof t->old_callback);
    call(XOS_SetCallBack, REGS(0), NULL);
}

/* CheckDying: a dying task window asks for its CallBack, where it exits */
static void check_dying(struct tw *t)
{
    if (t->moribund)
        callback_request(t);
}

/* abex_exit: OS_Exit with "ABEX" 1, as a program that failed */
static void abex_exit(struct tw *t)
{
    t->pass_on = 0;
    os_error *e = ros_error(ERR_EXIT, "Exit");
    call(XOS_Exit, REGS(at(e), 0x58454241u, 1), NULL);
}

/* ---- the vectors ---------------------------------------------------------------- */

/* The number of bytes that follow each control character in its VDU sequence
 * (controlnumbers) */
static const uint8_t control_numbers[32] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 5, 0, 0, 1, 9, 8, 5, 0, 0, 4, 4, 0, 2,
};

int taskwindow_passes(uint8_t *counter, int ctrl, uint8_t ch)
{
    if (*counter) {
        (*counter)--;
        return ctrl;
    }
    if (ch == 10 || ch >= 32)
        return 1;
    *counter = control_numbers[ch];
    return ctrl;
}

/* WrchV: puts the character into the output ring, asking for a parent first
 * if there is none.  The ring is sent when it is full, and the Wimp is
 * polled every tenth time.  Then the character goes to the spool file,
 * which the claim keeps from the kernel's. */
static int wrch(struct ros_cpu *s, uint32_t r12)
{
    if (ros_in_background())
        return ROS_VECTOR_PASS;
    struct tw *t = owner(G(r12));
    if (!t)
        return ROS_VECTOR_PASS;
    if (t->vdisplay) {
        /* A virtual display: the VDU draws the character there, and the
         * parent is sent nothing. */
        check_dying(t);
        if (t->moribund) {
            fail(s, dying());
            return ROS_VECTOR_CLAIM;
        }
        return ROS_VECTOR_PASS;
    }
    uint8_t ch = (uint8_t)s->r[0];
    t->pass_on = 1;
    save_escape(t);
    os_error *e = NULL;
    if (taskwindow_passes(&t->control_counter, (int)t->ctrl, ch)) {
        e = need_parent(t);
        if (!e && insert(t, OUT, ch)) {
            send_output(t);
            insert(t, OUT, ch);
            if (++t->output_count >= 10) {
                t->output_count = 0;
                t->slice = 0;
                poll(t, 0, 0, 0);
            }
        }
    }
    restore_escape(t);
    t->pass_on = 0;
    if (e) {
        fail(s, e);
        return ROS_VECTOR_CLAIM;
    }
    uint32_t spool = osbyte(199, 0, 0xFF, NULL);
    if (spool) {
        e = call(XOS_BPut, REGS(ch, spool), NULL);
        if (e) {
            os_error keep = *e;
            t->spool_handle = 0;
            osbyte(199, 0, 0, NULL);
            find_close(spool);
            fail(s, ros_error(keep.errnum, "%s", keep.errmess));
            return ROS_VECTOR_CLAIM;
        }
    }
    check_dying(t);
    if (t->moribund) {
        fail(s, dying());
        return ROS_VECTOR_CLAIM;
    }
    s->v = 0;
    return ROS_VECTOR_CLAIM;
}

/* RdchV: a character from GetInput.  If there is none, the output is sent
 * and the Wimp is polled without null events until a character comes, or
 * Escape, or death.  Death gives an Escape. */
static int rdch(struct ros_cpu *s, uint32_t r12)
{
    if (ros_in_background())
        return ROS_VECTOR_PASS;
    struct tw *t = owner(G(r12));
    if (!t)
        return ROS_VECTOR_PASS;
    uint8_t ch = 0;
    os_error *e = NULL;
    int escape = escape_state() || t->esc_pending;
    if (!escape) {
        if (get_input(t, &ch, &e)) {
            if (e) {
                fail(s, e);
                return ROS_VECTOR_CLAIM;
            }
        } else {
            t->pass_on = 1;
            save_escape(t);
            e = need_parent(t);
            if (!e) {
                send_output(t);
                for (;;) {
                    poll(t, NULL_BIT, 0, 0);
                    if (t->esc_pending || t->moribund) {
                        t->esc_pending = 125;
                        break;
                    }
                    if (get_kb(t, &ch))
                        break;
                }
            }
            restore_escape(t);
            t->pass_on = 0;
            check_dying(t);
            if (e) {
                fail(s, e);
                return ROS_VECTOR_CLAIM;
            }
            escape = t->esc_was_set != 0;
        }
    }
    if (t->moribund) {
        fail(s, dying());
        return ROS_VECTOR_CLAIM;
    }
    s->r[0] = escape ? 27 : ch;
    s->c = (uint32_t)escape;
    s->v = 0;
    return ROS_VECTOR_CLAIM;
}

/* ByteV: INKEY with a time limit, which polls until the time runs out.  It
 * also handles the acknowledgement of an Escape, and *FX 229, which is the
 * task window's own. */
static int byte_v(struct ros_cpu *s, uint32_t r12)
{
    uint32_t a = s->r[0];
    if (!((a == 129 && !(s->r[2] & 0x80)) || a == 126 || a == 229))
        return ROS_VECTOR_PASS;
    if (ros_in_background())
        return ROS_VECTOR_PASS;
    struct tw *t = owner(G(r12));
    if (!t)
        return ROS_VECTOR_PASS;
    if (a == 126) {
        if (escape_state() && osbyte(230, 0, 0xFF, NULL) == 0)
            close_my_exec(t);           /* the side effects of an Escape */
        return ROS_VECTOR_PASS;
    }
    if (a == 229) {
        uint32_t old = t->escape_disable, next;
        t->escape_disable = (uint8_t)((old & s->r[2]) ^ s->r[1]);
        t->pass_on = 1;
        osbyte(229, 1, 0, &next);       /* the real one stays off */
        t->pass_on = 0;
        s->r[1] = old, s->r[2] = next;
        s->v = 0;
        return ROS_VECTOR_CLAIM;
    }
    t->inkey_count = (s->r[1] & 0xFF) | (s->r[2] & 0xFF) << 8;
    uint8_t ch = 0;
    os_error *e = NULL;
    int got = 0, timeout = 0, escape = 0;
    if (get_input(t, &ch, &e)) {
        if (e) {
            fail(s, e);
            return ROS_VECTOR_CLAIM;
        }
        got = 1;
    } else if (t->inkey_count == 0) {
        timeout = 1;
    } else {
        t->pass_on = 1;
        save_escape(t);
        need_parent(t);
        send_output(t);
        for (;;) {
            uint32_t now = 0;
            xos_read_monotonic_time(&now);
            poll(t, 0, now + t->inkey_count, 0);
            if (t->esc_pending || t->moribund)
                break;
            if (get_kb(t, &ch)) {
                got = 1;
                break;
            }
            if (t->inkey_count == 0) {
                timeout = 1;
                break;
            }
        }
        restore_escape(t);
        t->pass_on = 0;
        check_dying(t);
        escape = t->esc_was_set != 0;
    }
    if (t->moribund) {
        fail(s, dying());
        return ROS_VECTOR_CLAIM;
    }
    if (escape) {
        s->r[1] = 0xFF, s->r[2] = 27, s->c = 1;
    } else if (got && !timeout) {
        s->r[1] = ch, s->r[2] = 0, s->c = 0;
    } else {
        s->r[1] = 0xFF, s->r[2] = 0xFF, s->c = 1;
    }
    s->r[0] = 129;
    s->v = 0;
    return ROS_VECTOR_CLAIM;
}

/* UpCallV: UpCall_Sleep polls on the sleeper's word.  UpCall_SleepNoMore is
 * refused for a word that a task window sleeps on. */
static int upcall(struct ros_cpu *s, uint32_t r12)
{
    struct global *g = G(r12);
    if (s->r[0] == UPCALL_SLEEP_NO_MORE) {
        for (uint32_t b = g->first; b; b = TW(b)->next)
            if (TW(b)->poll_word && TW(b)->poll_word == s->r[1]) {
                fail(s, ros_error(TASKWINDOW_ERR_FILE_SLEEP,
                                  "You can't close that file - a task window is waiting for it"));
                return ROS_VECTOR_CLAIM;
            }
        return ROS_VECTOR_PASS;
    }
    if (s->r[0] != UPCALL_SLEEP || ros_in_background())
        return ROS_VECTOR_PASS;
    struct tw *t = owner(g);
    if (!t)
        return ROS_VECTOR_PASS;
    uint32_t word = s->r[1];
    t->pass_on = 1;
    save_escape(t);
    send_output(t);
    t->poll_word = word;
    int ready = word == 0 || ros_ld32(word) != 0;       /* then a yield, not a sleep */
    os_error *e = poll(t, ready ? 0 : POLLWORD | NULL_BIT, 0, word);
    t->poll_word = 0;
    restore_escape(t);
    t->pass_on = 0;
    check_dying(t);
    if (e)
        fail(s, e);
    else if (t->moribund)
        fail(s, dying());
    else if (t->esc_was_set)
        fail(s, ros_error(ERR_ESCAPE, "Escape"));
    else
        s->r[0] = 0, s->v = 0;          /* UpCall_Claimed */
    return ROS_VECTOR_CLAIM;
}

/* TickerV: counts down every INKEY.  It also runs the current task window's
 * time slice and asks for its CallBack. */
static int ticker(struct ros_cpu *s, uint32_t r12)
{
    (void)s;
    struct global *g = G(r12);
    for (uint32_t b = g->first; b; b = TW(b)->next)
        if (TW(b)->inkey_count)
            TW(b)->inkey_count--;
    struct tw *t = owner(g);
    if (!t || ++t->slice < t->nice)
        return ROS_VECTOR_PASS;
    t->slice = 0;
    t->pass_on = 1;
    callback_request(t);
    t->pass_on = 0;
    return ROS_VECTOR_PASS;
}

/* CnpV: when the key buffer is purged, the task window's is purged with it */
static int cnp(struct ros_cpu *s, uint32_t r12)
{
    if (s->v && s->r[1] == 0) {
        struct tw *t = owner(G(r12));
        if (t)
            clear(t, KEYS);
    }
    return ROS_VECTOR_PASS;
}

static const struct {
    uint32_t vector;
    ros_vector_fn *fn;
} claims[] = {
    { WRCHV, wrch }, { ROS_UPCALLV, upcall }, { RDCHV, rdch },
    { BYTEV, byte_v }, { ROS_TICKERV, ticker }, { ROS_CNPV, cnp },
};

static void vectors(uint32_t ws, int on)
{
    for (unsigned i = 0; i < sizeof claims / sizeof claims[0]; i++) {
        if (on)
            ros_vector_claim_native(claims[i].vector, claims[i].fn, ws);
        else
            ros_vector_release_native(claims[i].vector, claims[i].fn, ws);
    }
}

/* ---- the handlers ------------------------------------------------------------------ */

/* The time slice's CallBack, on the way out of the program's outermost SWI.
 * The output is sent and the Wimp is polled, so the other tasks run.  Then
 * the program carries on as it was.  If the task is dying, it exits. */
static void callback_handler(struct ros_cpu *s)
{
    struct tw *t = TW(s->r[12]);
    uint32_t r[8] = { ROS_ENV_CALLBACK, t->old_callback[0], t->old_callback[1],
                      t->old_callback[2] };
    call(XOS_ChangeEnvironment, r, NULL);
    if (t->domain != ros_ld32(ROS_ZP_DOMAINID) || t->pass_on)
        return;                         /* not its program's: its own code, or another task */
    if (t->moribund) {
        abex_exit(t);
        return;
    }
    t->pass_on = 1;
    save_escape(t);
    send_output(t);
    poll(t, 0, 0, 0);
    if (t->moribund) {
        abex_exit(t);
        return;
    }
    restore_escape(t);
    t->pass_on = 0;
}

/* The exit handler, entered when the command has ended.  It goes back to the
 * command loop, on this thread's stack where it stands. */
static void exit_handler(struct ros_cpu *s)
{
    struct tw *t = TW(s->r[12]);
    if (!here || here->t != t)
        return;                         /* not this thread's: the program ends */
    ros_resume_unwind(here->frame);
    longjmp(here->jb, 1);
}

/* The error handler, entered for an error that the command did not catch.
 * With a parent, its text is output.  With none, the Wimp reports it.  Then
 * the command has ended. */
static void error_handler(struct ros_cpu *s)
{
    struct tw *t = TW(s->r[0]);
    if (t->aborted) {
        call(XOS_Exit, REGS(0), NULL);
        return;
    }
    t->aborted = 'E';
    uint32_t r[8] = { ROS_ENV_ERROR, t->old_error[0], t->old_error[1], t->old_error[2] };
    call(XOS_ChangeEnvironment, r, NULL);
    if (t->pass_on == 0 && t->moribund) {
        /* killed: nothing to say */
    } else if (t->pass_on == 0 && t->parent_task) {
        call(XOS_Write0, REGS(at(t->error_buffer + 8)), NULL);
        call(XOS_WriteC, REGS(13), NULL);
        call(XOS_WriteC, REGS(10), NULL);
    } else {
        call(XWimp_ReportError, REGS(at(t->error_buffer + 4), 6, t->name), NULL);
    }
    t->aborted = 0;
    call(XOS_Exit, REGS(0), NULL);
}

/* ---- the task window's life ------------------------------------------------------ */

static __attribute__((noreturn)) void end(struct tw *t);

/* An error on starting, before Wimp_Initialise (Code_StartError).  It is
 * raised as the new task's error. */
static __attribute__((noreturn)) void start_error(struct tw *t, const os_error *e)
{
    os_error keep = *e;
    unlink_block(t);
    ros_raise(ros_error(keep.errnum, "%s", keep.errmess));
}

/* An error on starting, after Wimp_Initialise (Code_WimpStartError).  The
 * Wimp reports it and the task goes. */
static void wimp_start_error(struct tw *t, const os_error *e)
{
    call(XWimp_ReportError, REGS(at(e), 6, t->name), NULL);
    uint32_t task = t->child_task;
    unlink_block(t);
    call(XWimp_CloseDown, REGS(task, TASK_WORD), NULL);
    call(XOS_Exit, REGS(0), NULL);
}

/* The handlers the command loop puts in place before each command: its
 * exit and error handlers, and the defaults for the rest (Code_NewCommand) */
static void handlers(struct tw *t)
{
    call(XOS_ChangeEnvironment, REGS(ROS_ENV_EXIT, e_exit, at(t)), NULL);
    call(XOS_ChangeEnvironment, REGS(ROS_ENV_ERROR, e_error, at(t), at(t->error_buffer)), NULL);
    static const uint8_t defaults[] = {
        ROS_ENV_UNDEFINED, ROS_ENV_PREFETCH_ABORT, ROS_ENV_DATA_ABORT, ROS_ENV_ADDRESS_EXCEPTION,
        ROS_ENV_OTHER_EXCEPTION, ROS_ENV_CALLBACK, ROS_ENV_BREAKPOINT, ROS_ENV_ESCAPE,
        ROS_ENV_EVENT, ROS_ENV_UNUSED_SWI, ROS_ENV_EXCEPTION_REGISTERS,
    };
    for (unsigned i = 0; i < sizeof defaults; i++) {
        uint32_t r[8] = { defaults[i] };
        if (!call(XOS_ReadDefaultHandler, r, NULL)) {
            r[0] = defaults[i];
            call(XOS_ChangeEnvironment, r, NULL);
        }
    }
}

/* The prompt is Cli$Prompt, or "*", and then a line is read.  It returns 1
 * for a command in t->line.  It returns 0 for Escape, which is announced
 * before the prompt is given again.  It returns -1 for an error, which ends
 * the command loop. */
static int prompt(struct tw *t)
{
    uint32_t r[8] = { str(t->scratch, "Cli$Prompt"), at(t->line), 1024, 0, 3 };
    if (call(XOS_ReadVarVal, r, NULL))
        call(XOS_WriteN, REGS(str(t->scratch, "*"), 1), NULL);
    else
        call(XOS_WriteN, REGS(at(t->line), r[2]), NULL);
    /* OS_ReadLine32 is used because the line is in the RMA.  OS_ReadLine
     * would read flags from RMA addresses (arena.h). */
    uint32_t rl[8] = { at(t->line), 1024, ' ', 0xFF, 0 }, c = 0;
    if (call(XOS_ReadLine32, rl, &c))
        return -1;
    if (!c)
        return 1;
    osbyte(126, 0, 0, NULL);
    call(XOS_NewLine, REGS(0), NULL);
    uint32_t m[8] = { 0, str(t->scratch, "Escape"), 0 };
    if (call(0x41502u | ROS_X_BIT, m, NULL))           /* MessageTrans_Lookup */
        m[2] = at(t->scratch), m[3] = 6;
    call(XOS_WriteN, REGS(m[2], m[3]), NULL);
    call(XOS_NewLine, REGS(0), NULL);
    return 0;
}

/* Code_NewCommand onwards.  It runs one command at a time, until there is
 * none to run and nobody to ask, or until -quit. */
static __attribute__((noreturn)) void command_loop(struct tw *t)
{
    for (;;) {
        t->pass_on = 1;
        handlers(t);
        take_exec(t);
        if (t->exec_handle) {
            uint32_t r[8] = { 5, t->exec_handle };      /* OS_Args 5: at its end? */
            if (call(XOS_Args, r, NULL) || r[2])
                close_my_exec(t);
        }
        t->pass_on = 0;
        if (t->command) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = t->command;
            ros_swi(&c, OS_CLI_SWI);        /* its errors to the error handler */
            call(XOS_Exit, REGS(0), NULL);  /* and its end to the exit handler */
            end(t);
        }
        if (!t->parent_task)
            break;
        if (t->quit || t->req_die) {
            send_output(t);
            break;
        }
        int got = prompt(t);
        if (got < 0)
            break;
        if (got)
            t->command = at(t->line);
    }
    end(t);
}

/* The end.  The machine is put back as it was and the parent is told
 * (Morio).  The block is freed and the task is closed down. */
static __attribute__((noreturn)) void end(struct tw *t)
{
    t->pass_on = 1;
    osbyte(229, t->task_escape, 0, NULL);
    close_redirection();
    uint32_t spool = osbyte(199, 0, 0, NULL);
    if (spool)
        find_close(spool);
    if (t->parent_task) {
        t->msg[0] = 24, t->msg[3] = 0, t->msg[4] = TASKWINDOW_MORIO, t->msg[5] = t->child_task;
        send(t, USER_MESSAGE, t->parent_task);
    }
    if (!t->aborted)
        call(XOS_ChangeEnvironment,
             REGS(ROS_ENV_ERROR, t->old_error[0], t->old_error[1], t->old_error[2]), NULL);
    uint32_t cb[8] = { ROS_ENV_CALLBACK, 0, 0, 0 };
    call(XOS_ChangeEnvironment, cb, NULL);
    if (cb[1] == e_callback)
        call(XOS_ChangeEnvironment,
             REGS(ROS_ENV_CALLBACK, t->old_callback[0], t->old_callback[1], t->old_callback[2]),
             NULL);
    if (t->old_exit[0] != e_exit)
        call(XOS_ChangeEnvironment, REGS(ROS_ENV_EXIT, t->old_exit[0], t->old_exit[1]), NULL);
    struct global *g = G(t->global);
    if (g->first == at(t) && t->next == 0)
        vectors(t->global, 0);          /* the last task window */
    set_keys(t->wimp_keys);
    uint32_t task = t->child_task;
    here = NULL;
    unlink_block(t);
    call(XWimp_CloseDown, REGS(task, TASK_WORD), NULL);
    call(XOS_Exit, REGS(0), NULL);
    ros_task_exit(NULL);                /* (an exit handler that came back) */
}

/* *TaskWindow from a task window's program starts a new task, as a task
 * window starts one.  The output is sent, the Wimp is polled until a null
 * event, and then Wimp_StartTask is called.  Then the command is over. */
static void start_from_inside(struct tw *t, uint32_t tail)
{
    t->pass_on = 1;
    strcpy(t->line, "TaskWindow ");
    size_t n = strlen(t->line);
    for (uint32_t p = tail; ros_ld8(p) >= ' ' && n < sizeof t->line - 1; p++)
        t->line[n++] = (char)ros_ld8(p);
    t->line[n] = 0;
    save_escape(t);
    uint32_t in, out;
    redirection(0, 0, &in, &out);
    send_output(t);
    os_error *e;
    do
        e = poll(t, 0, 0, 0);
    while (!e && !t->moribund && t->poll_result != 0);
    if (!e && !t->moribund)
        e = start_task_outermost(at(t->line));
    os_error keep;
    if (e)
        keep = *e;
    redirection(in, out, NULL, NULL);
    restore_escape(t);
    t->pass_on = 0;
    if (e)
        ros_raise(ros_error(keep.errnum, "%s", keep.errmess));
    call(XOS_Exit, REGS(0), NULL);
}

static const char keydef[] =
    ",wimpslot,name,display/K/S,quit/K/S,ctrl/K/S,task/K/E,txt/K/E,nice,vdisplay/K/E";

/* A /E argument's value: its type byte, then the word */
static uint32_t evaluated(uint32_t p)
{
    return ros_ld8(p + 1) | ros_ld8(p + 2) << 8 | ros_ld8(p + 3) << 16 |
           (uint32_t)ros_ld8(p + 4) << 24;
}

int taskwindow_parent_handles(const char *tail, uint32_t *task, uint32_t *txt)
{
    const char *p = tail;
    while (*p == ' ')
        p++;
    uint32_t v[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 8; i++, p++) {
            char c = *p;
            uint32_t d;
            if (c >= '0' && c <= '9')
                d = (uint32_t)(c - '0');
            else if ((c & ~0x20) >= 'A' && (c & ~0x20) <= 'F')
                d = (uint32_t)((c & ~0x20) - 'A' + 10);
            else
                return 0;
            v[k] = v[k] << 4 | d;
        }
        if (*p != ' ')
            return 0;
        while (*p == ' ')
            p++;
    }
    *task = v[0];
    *txt = v[1];
    return 1;
}

/* The module's start entry: OS_Module 2, *TaskWindow and *ShellCLI_Task
 * (Code_StartEntry), in the new task, as its application */
static void start(struct ros_module *m, uint32_t tail)
{
    uint32_t ws = ros_ld32(m->private_word);
    struct global *g = G(ws);
    struct tw *t = owner(g);
    if (t) {
        start_from_inside(t, tail);
        return;
    }
    char text[1024];
    size_t n = 0;
    for (uint32_t p = tail; ros_ld8(p) >= ' ' && n < sizeof text - 1; p++)
        text[n++] = (char)ros_ld8(p);
    text[n] = 0;
    uint32_t task = 0, txt = 0;
    int old = taskwindow_parent_handles(text, &task, &txt);
    struct tw *w = old ? waiting(g) : NULL;
    if (w) {
        /* an editor's answer to a NewTask: the handles are the waiting
         * one's, and this task has done its job */
        w->parent_task = task;
        w->parent_txt = txt;
        call(XOS_Exit, REGS(0), NULL);
        return;
    }
    void *block;
    os_error *e = xos_module_claim(sizeof *t, &block);
    if (e)
        ros_raise(e);
    t = block;
    memset(t, 0, sizeof *t);
    t->global = ws;
    t->parent_task = task;
    t->parent_txt = txt;
    t->pass_on = 1;
    t->nice = EVENT_COUNT;
    t->name = str(t->title, "TaskWindow");
    t->domain = ros_ld32(ROS_ZP_DOMAINID);
    t->next = g->first;
    g->first = at(t);

    if ((e = call(XOS_ChangeRedirection, REGS(0xFFFFFFFFu, 0xFFFFFFFFu), NULL)) != NULL)
        start_error(t, e);
    if (!t->parent_task) {
        /* *TaskWindow <command> [<options>] */
        uint32_t r[8] = { str(t->scratch, keydef), old ? 0 : tail, at(t->args), sizeof t->args };
        if (old)
            r[1] = str(t->line, "");
        if ((e = call(XOS_ReadArgs, r, NULL)) != NULL)
            start_error(t, e);
        t->command = t->args[0];
        if (t->args[6])
            t->parent_task = evaluated(t->args[6]);
        if (t->args[7])
            t->parent_txt = evaluated(t->args[7]);
        if (t->args[2])
            t->name = t->args[2];
        t->quit = t->args[4] != 0;
        t->ctrl = t->args[5] != 0;
        if (t->args[8]) {
            uint32_t u[8] = { 10, t->args[8] };
            if (!call(XOS_ReadUnsigned, u, NULL))
                t->nice = u[2];
        }
        if (t->args[9])
            t->vdisplay = evaluated(t->args[9]);
    }

    t->messages[0] = MSG_DATASAVE, t->messages[1] = MSG_RAMTRANSMIT;
    t->messages[2] = TASKWINDOW_INPUT, t->messages[3] = TASKWINDOW_MORITE;
    t->messages[4] = TASKWINDOW_SUSPEND, t->messages[5] = TASKWINDOW_RESUME;
    uint32_t wi[8] = { WIMP_VER, TASK_WORD, t->name, at(t->messages) };
    if ((e = call(XWimp_Initialise, wi, NULL)) != NULL)
        start_error(t, e);
    t->child_task = wi[1];

    /* Keep at least 96K free for the rest */
    uint32_t sl[8] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
    if (!call(XWimp_SlotSize, sl, NULL) && sl[2] < 96u * 1024)
        call(XWimp_SlotSize, REGS(sl[0] - (96u * 1024 - sl[2]), 0xFFFFFFFFu), NULL);

    if (t->parent_task && (e = send_ego(t)) != NULL) {
        wimp_start_error(t, e);
        return;
    }
    if (t->args[1]) {
        char slot[64];
        snprintf(slot, sizeof slot, "%s", (const char *)ros_ptr(t->args[1]));
        snprintf(t->line, sizeof t->line, "WimpSlot %s %s", slot, slot);
        if ((e = call(XOS_CLI, REGS(at(t->line)), NULL)) != NULL) {
            wimp_start_error(t, e);
            return;
        }
    }

    /* inittask */
    read_keys(t->wimp_keys);
    static const uint8_t task_keys[8] = { 0x01, 0xD0, 0xE0, 0xF0, 0x01, 0x80, 0x90, 0xA0 };
    memcpy(t->task_keys, task_keys, sizeof task_keys);
    set_keys(t->task_keys);
    uint32_t r[8] = { ROS_ENV_EXIT, e_exit, at(t) };
    call(XOS_ChangeEnvironment, r, NULL);
    memcpy(t->old_exit, &r[1], sizeof t->old_exit);
    uint32_t re[8] = { ROS_ENV_ERROR, e_error, at(t), at(t->error_buffer) };
    call(XOS_ChangeEnvironment, re, NULL);
    memcpy(t->old_error, &re[1], sizeof t->old_error);
    if (t->vdisplay && (e = call(XVDisplay_Attach, REGS(t->vdisplay, 0), NULL)) != NULL) {
        wimp_start_error(t, e);         /* from here on the task has its own screen */
        return;
    }
    if (t->next == 0)
        vectors(ws, 1);                 /* the first task window */
    t->pass_on = 1;
    take_exec(t);
    t->task_escape = (uint8_t)osbyte(229, 0, 0xFF, NULL);

    /* The command loop's place on this thread.  The exit handler comes back
     * here, and the loop starts again from Code_NewCommand. */
    struct running run;
    run.t = t;
    run.frame = __builtin_frame_address(0);
    run.handlers = ros_handler_chain(NULL);
    ros_handler_chain(run.handlers);
    here = &run;
    if (setjmp(run.jb)) {
        /* An exit.  The SWIs it was in, and their frames, are gone.  The SVC
         * stack is flat, the depth is 0, and no C handlers remain but the
         * loop's.  The error handler gets a fresh base (environment.h). */
        ros_call_depth = 0;
        ros_svc_sp = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;
        ros_handler_chain(run.handlers);
        struct ros_env_base fresh = { .active = 0 };
        ros_env_base_swap(&fresh);
        t = run.t;
        t->command = 0;
        if (t->aborted || t->moribund)
            end(t);
    } else if (t->args[3]) {
        /* -display */
        if ((e = need_parent(t)) != NULL)
            ros_raise(e);
    }
    command_loop(t);
}

/* ---- the commands ------------------------------------------------------------------ */

/* *TaskWindow and *ShellCLI_Task: the module entered, as the application */
static os_error *cmd_enter(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)argc;
    uint32_t title = m->base + ros_ld32(m->base + 0x10);
    return call(XOS_Module, REGS(2, title, tail), NULL);
}

static os_error *cmd_quit(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)tail, (void)argc;
    uint32_t ws = ros_ld32(m->private_word);
    struct tw *t = ws ? owner(G(ws)) : NULL;
    if (t)
        t->req_die = 1;
    return NULL;
}

static const struct ros_command commands[] = {
    { "ShellCLI_Task", ROS_CMD_INFO(2, 2, 0, 0), "Syntax: *ShellCLI_Task XXXXXXXX XXXXXXXX",
      "*ShellCLI_Task runs an application in a window. The first argument is an 8 digit hex. "
      "number giving the task handle of the parent task. The second argument is an 8 digit "
      "hex. number giving a handle which may be used by the parent task to identify the "
      "task.\rThis command is intended for use only within applications.",
      cmd_enter },
    { "ShellCLI_TaskQuit", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *ShellCLI_TaskQuit",
      "*ShellCLI_TaskQuit quits the current task window", cmd_quit },
    { "TaskWindow", ROS_CMD_INFO(1, 255, 0, 0),
      "Syntax: *TaskWindow <command> [[-wimpslot] <n>K] [[-name] <taskname>] [-ctrl] "
      "[-display] [-quit] [-nice <n>] [-vdisplay <handle>]",
      "The *TaskWindow command allows a background task to be started, which will obtain a "
      "task window if it needs to do any screen I/O.\rOptions:\r"
      "\t-wimpslot sets the memory to be allocated\r"
      "\t-name sets the task name\r"
      "\t-ctrl allows control characters through\r"
      "\t-display opens the task window immediately, rather than waiting for a character to "
      "be printed\r"
      "\t-quit makes the task quit after the command even if the task window has been opened\r"
      "\t-nice alters number of timeslices given to task\r"
      "\t-vdisplay gives the task a virtual display (VDisplay_Create's handle): its output is "
      "drawn there\r"
      "Note that fields must be in \" \" if they comprise more than one word",
      cmd_enter },
    { NULL, 0, NULL, NULL, NULL },
};

/* ---- the SWI -------------------------------------------------------------------------- */

os_error *xtaskwindow_task_info(uint32_t index, uint32_t *value)
{
    (void)index;
    uint32_t ws = ros_ld32(taskwindow_module.private_word);
    struct tw *t = ws ? owner(G(ws)) : NULL;
    *value = t ? at(t) : 0;
    return NULL;
}

/* ---- the module ------------------------------------------------------------------------ */

/* The file types and their run actions, where not already set */
static const char *const aliases[][2] = {
    { "Alias$@RunType_FD7", "TaskWindow \"Obey %*0\" -name \"Task Obey\" -quit" },
    { "File$Type_FD7", "TaskObey" },
    { "Alias$@RunType_FD6", "TaskWindow \"Exec %*0\" -name \"Task Exec\" -display" },
    { "File$Type_FD6", "TaskExec" },
};

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    if (!e_exit) {
        e_exit = ros_native_entry(exit_handler, "taskwindow:ExitHandler");
        e_error = ros_native_entry(error_handler, "taskwindow:ErrorHandler");
        e_callback = ros_native_entry(callback_handler, "taskwindow:CallBackHandler");
    }
    char *buf = NULL;
    os_error *e = xos_module_claim(512, (void **)&buf);
    if (e)
        return e;
    for (unsigned i = 0; i < sizeof aliases / sizeof aliases[0]; i++) {
        strcpy(buf, aliases[i][0]);
        strcpy(buf + 64, aliases[i][1]);
        uint32_t r[8] = { at(buf), 0, 0xFFFFFFFFu, 0, 0 };
        call(XOS_ReadVarVal, r, NULL);
        if ((int32_t)r[2] < 0)
            continue;                   /* it exists */
        call(XOS_SetVarVal, REGS(at(buf), at(buf + 64), 0, 0, 0), NULL);
    }
    xos_module_free(buf);
    struct global *g;
    if ((e = xos_module_claim(sizeof *g, (void **)&g)) != NULL)
        return e;
    g->first = 0;
    ros_st32(m->private_word, at(g));
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    uint32_t ws = ros_ld32(m->private_word);
    if (!ws)
        return NULL;
    if (G(ws)->first)
        return ros_error(TASKWINDOW_ERR_CANT_KILL, "A task window is still active");
    xos_module_free(G(ws));
    ros_st32(m->private_word, 0);
    return NULL;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    uint32_t ws = ros_ld32(m->private_word);
    if (!ws)
        return;
    struct global *g = G(ws);
    switch (s->r[1]) {
    case 0x11:                          /* Service_Memory: refused when it comes from the Wimp while a task window exists */
        if (g->first && s->r[0] == 0x80000000u && s->r[2] == m->base)
            s->r[1] = 0;
        break;
    case 0x53:                          /* Service_WimpCloseDown: one task closing another */
        if (s->r[0] == 0)
            break;
        for (uint32_t b = g->first; b; b = TW(b)->next)
            if (TW(b)->child_task == s->r[2]) {
                s->r[0] = at(ros_error(ERR_WIMP_CANT_KILL, "Window Manager is currently in use"));
                break;
            }
        break;
    case 0x57:                          /* Service_WimpReportError: off the vectors while
                                           a box is open */
        if (s->r[0]) {
            struct tw *t = owner(g);
            if (t)
                t->pass_on = 2;
        } else {
            for (uint32_t b = g->first; b; b = TW(b)->next)
                if (TW(b)->pass_on == 2) {
                    TW(b)->pass_on = 0;
                    break;
                }
        }
        break;
    }
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ERR_BAD_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module taskwindow_module = {
    .title = "TaskWindow",
    .help = "TaskWindow\t0.85 (17 Oct 2022) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .start = start,
    .swi_chunk = 0x43380,
    .swi_thunks = ros_swi_thunks_TaskWindow,
    .swi_names = ros_swi_names_TaskWindow,
    .swi_prefix = "TaskWindow",
    .commands = commands,
};

/* The count is only known at run time (api_gen.c), so the runtime reads
 * it when the module is added. */
__attribute__((constructor)) static void count(void)
{
    taskwindow_module.swi_count = ros_swi_count_TaskWindow;
}
