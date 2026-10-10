/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2020 Julie Stamp
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
 * This file is a reimplementation in C of RISC OS Open's ShellCLI module
 * (Sources/Desktop/ShellCLI: c.module, s.veneer, s.Shell, cmhg.ShellCLIHdr).
 */

/* shellcli.c: the ShellCLI module, native. It gives a command line at the
 * bottom of the desktop's screen. The Task Manager's F12 key and its
 * *Commands menu item start it with Wimp_StartTask "ShellCLI". Return on an
 * empty line goes back to the desktop. The
 * source it was taken from is RISC OS 5's Desktop/ShellCLI (0.39: c/module,
 * s/veneer, Resources/UK; Copyright 2020 Julie Stamp, Apache License 2.0).
 *
 * *ShellCLI enters the module as the application (OS_Module 2), in the
 * task the Wimp started, on that task's thread. Shell_Create claims the
 * shell's block and puts in its exit and error handlers. The task then
 * initialises, sets VDU 4 text at the bottom of the screen, tells the Wimp
 * that a command window is active, and reads commands until an empty line.
 * It never polls. The desktop waits, as it does on RISC OS.
 *
 * A program the shell runs ends through the shell's exit handler. An error
 * that nobody caught goes through the shell's error handler, which prints
 * it and exits. Both come back to the command loop. RISC OS's veneer resets
 * its private stack there and runs the loop again. Here the loop's place on
 * this thread's stack is kept in a jmp_buf, and the handler goes back to
 * it, as TaskWindow's does (modules/taskwindow).
 */
#include <setjmp.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "shellcli.h"

#define TASK_WORD   0x4B534154u        /* "TASK" */
#define WIMP_VER    300u
#define COMMAND_BUFFER_SIZE 1024u

#define OSBYTE_VDU_QUEUE      218u     /* OsByte_RW_VDUQueue */
#define OSBYTE_ACK_ESCAPE     126u
#define OSBYTE_SELECT_POINTER 106u
#define VDUEXT_TWBROW         0x85u    /* the text window's bottom row */
#define VARTYPE_STRING        0u
#define VARTYPE_EXPANDED      3u
#define WIMP_TEXT_BACKGROUND  0x80u    /* Wimp_TextColour: set the background */
#define WIMP_COLOUR_WHITE     0u
#define WIMP_COLOUR_BLACK     7u
#define WIMP_ECANCEL          2u
#define WIMP_EHICANCEL        4u

#define SERVICE_MEMORY        0x11u
#define SERVICE_WIMPCLOSEDOWN 0x53u

#define ERR_BAD_SWI 0x1E6u

static const char messages_file[] = "Resources:$.Resources.ShellCLI.Messages";

/* The shell, while there is one (from Shell_Create to Shell_Destroy). This
 * is RISC OS's wk_t, kept in the RMA. The Wimp and MessageTrans read parts
 * of it by address. It is the module's workspace (wk). The private word
 * holds its address, and holds 0 when there is no shell. */
struct wk {
    char command_buffer[COMMAND_BUFFER_SIZE];
    /* The error handler's buffer. It holds the pc, then the error (error_t:
     * number and text). Errors this module returns are looked up into it. */
    uint32_t env_error[1 + 64];
    uint32_t old_exit[3];           /* the exit handler before Shell_Create */
    uint32_t task_handle;
    uint32_t msgdesc[4];            /* the Messages file, while an error is looked up */
    uint32_t list[2];               /* OS_ReadVduVariables' */
    char token[16];                 /* an error's token, as an error block */
    char name[48];                  /* a variable's or file's name, for a SWI */
};

/* The native entries (ros_native_entry) for the exit and error handlers of the environment */
static uint32_t e_exit, e_error;

/* Where the command loop stands on its thread. The exit handler goes back
 * there. RISC OS's exit_veneer resets the stack and runs the loop again. */
struct running {
    jmp_buf jb;
    const void *frame;
    struct ros_handler *handlers;
};
static _Thread_local struct running *here;

static uint32_t at(const void *p) { return ros_addr(p); }

static struct wk *WK(void)
{
    uint32_t pw = shellcli_module.private_word, w = pw ? ros_ld32(pw) : 0;
    return w ? ros_ptr(w) : NULL;
}

/* The module's title, taken from its header. It is the task's name. */
static uint32_t title(void)
{
    uint32_t base = shellcli_module.base;
    return base + ros_ld32(base + 0x10);
}

/* ---- calls ------------------------------------------------------------------- */

/* Calls a SWI with R0-R7 from r, and stores R0-R7 back into r. If c is not
 * NULL, *c receives the C flag. */
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

static os_error *writei(uint8_t c)
{
    return call(XOS_WriteI + c, REGS(0), NULL);
}

static os_error *newline(void)
{
    return call(XOS_NewLine, REGS(0), NULL);
}

static uint32_t name(struct wk *w, const char *s)
{
    strncpy(w->name, s, sizeof w->name - 1);
    w->name[sizeof w->name - 1] = 0;
    return at(w->name);
}

/* Looks up one of the module's errors in its Messages file, into the
 * shell's error buffer (get_error). Returns that error, or MessageTrans's
 * own error if the lookup fails. */
static os_error *get_error(struct wk *w, uint32_t errnum, const char *token)
{
    uint32_t desc = at(w->msgdesc);
    os_error *e = call(XMessageTrans_OpenFile, REGS(desc, name(w, messages_file), 0), NULL);
    if (e)
        return e;
    memcpy(w->token, &errnum, 4);
    strncpy(w->token + 4, token, sizeof w->token - 5);
    w->token[sizeof w->token - 1] = 0;
    e = call(XMessageTrans_ErrorLookup,
             REGS(at(w->token), desc, at(&w->env_error[1]), 256, title(), 0, 0, 0), NULL);
    call(XMessageTrans_CloseFile, REGS(desc), NULL);
    return e;
}

/* ---- Shell_Create and Shell_Destroy --------------------------------------------- */

/* Sets every handler to its default, except the exit and error handlers,
 * which are the shell's. The exit handler that it replaced goes into
 * old_exit (set_handlers). */
static void set_handlers(struct wk *w, uint32_t *old_exit)
{
    for (uint32_t i = 0; i < ROS_ENV_HANDLERS; i++) {
        uint32_t r[8] = { i };
        if (call(XOS_ReadDefaultHandler, r, NULL))
            return;
        if (i == ROS_ENV_EXIT) {
            r[1] = e_exit;
            r[2] = at(w);
        } else if (i == ROS_ENV_ERROR) {
            r[1] = e_error;
            r[2] = at(w);
            r[3] = at(w->env_error);
        }
        r[0] = i;
        call(XOS_ChangeEnvironment, r, NULL);
        if (i == ROS_ENV_EXIT && old_exit)
            memcpy(old_exit, &r[1], 3 * sizeof r[0]);
    }
}

/* Shell_Create: a shell, one at a time */
os_error *xshell_create(void)
{
    struct wk *w = WK();
    if (w)
        return get_error(w, SHELLCLI_ERR_CREATION, "NoSpawn");
    void *block;
    os_error *e = xos_module_claim(sizeof *w, &block);
    if (e)
        return e;
    w = block;
    memset(w, 0, sizeof *w);
    ros_st32(shellcli_module.private_word, at(w));
    set_handlers(w, w->old_exit);
    return NULL;
}

/* Shell_Destroy: puts the exit handler back as it was and removes the
 * shell. With no shell it does nothing. */
os_error *xshell_destroy(void)
{
    struct wk *w = WK();
    if (w) {
        call(XOS_ChangeEnvironment,
             REGS(ROS_ENV_EXIT, w->old_exit[0], w->old_exit[1], w->old_exit[2]), NULL);
        ros_st32(shellcli_module.private_word, 0);
        xos_module_free(w);
    }
    return NULL;
}

/* ---- the shell ------------------------------------------------------------------ */

/* Shows an error that the shell cannot print, in an error box (setup_error) */
static void setup_error(const os_error *e)
{
    call(XWimp_CommandWindow, REGS(0xFFFFFFFFu), NULL);
    call(XWimp_ReportError, REGS(at(e), WIMP_ECANCEL | WIMP_EHICANCEL, title()), NULL);
}

/* Sets up VDU 4 text over the whole screen, black on white. The cursor goes
 * on the bottom row with a line fed, and the pointer is turned off
 * (setup_vdu). */
static void setup_vdu(struct wk *w)
{
    static const uint8_t vdu_commands[] = {
        6,                              /* enable VDU */
        4,                              /* VDU 4 mode */
        23, 1, 1, 0, 0, 0, 0, 0, 0, 0,  /* the text cursor on */
        26,                             /* text and graphics windows to default */
        30,                             /* the text cursor home */
        29, 0, 0, 0, 0,                 /* the graphics origin at (0,0) */
        25, 4, 0, 0, 0, 0,              /* the graphics cursor to (0,0) */
        15,                             /* paged mode off */
        18, 0, 7,                       /* graphics foreground colour 7 */
        18, 0, 128,                     /* graphics background colour 0 */
    };
    call(XOS_Byte, REGS(OSBYTE_VDU_QUEUE, 0, 0), NULL);     /* the VDU queue flushed */
    memcpy(w->command_buffer, vdu_commands, sizeof vdu_commands);
    call(XOS_WriteN, REGS(at(w->command_buffer), sizeof vdu_commands), NULL);
    call(XWimp_TextColour, REGS(WIMP_COLOUR_BLACK), NULL);
    call(XWimp_TextColour, REGS(WIMP_TEXT_BACKGROUND | WIMP_COLOUR_WHITE), NULL);
    w->list[0] = VDUEXT_TWBROW, w->list[1] = 0xFFFFFFFFu;
    call(XOS_ReadVduVariables, REGS(at(w->list), at(w->list)), NULL);
    uint8_t *move = (uint8_t *)w->command_buffer;
    move[0] = 31, move[1] = 0, move[2] = (uint8_t)w->list[0], move[3] = 10;
    call(XOS_WriteN, REGS(at(move), 4), NULL);
    call(XOS_Byte, REGS(OSBYTE_SELECT_POINTER, 0), NULL);  /* the mouse gone */
}

/* Prints the prompt, which is CLI$Prompt or "*" (display_prompt) */
static void display_prompt(struct wk *w)
{
    writei(4);
    uint32_t r[8] = { name(w, "CLI$Prompt"), at(w->command_buffer), COMMAND_BUFFER_SIZE, 0,
                      VARTYPE_EXPANDED };
    if (call(XOS_ReadVarVal, r, NULL))
        writei('*');
    else
        call(XOS_WriteN, REGS(at(w->command_buffer), r[2]), NULL);
}

/* Escape at the prompt: acknowledged, then a newline, "Escape" and a
 * newline (acknowledge_escape) */
static void acknowledge_escape(void)
{
    if (call(XOS_Byte, REGS(OSBYTE_ACK_ESCAPE), NULL) || newline())
        return;
    struct wk *w = WK();
    if (!w)
        return;
    uint32_t r[8] = { 0, name(w, "Escape"), 0 };
    if (call(XMessageTrans_Lookup, r, NULL))
        return;
    if (call(XOS_WriteN, REGS(r[2], r[3]), NULL))
        return;
    newline();
}

/* Writes an error's text and a newline. Returns 0, or the error from writing them. */
static int say(const os_error *e)
{
    return call(XOS_Write0, REGS(at(e->errmess)), NULL) || newline();
}

/* Reads and runs commands until an empty line (command_loop) */
static void command_loop(struct wk *w)
{
    for (;;) {
        /* A program the shell ran may have called Shell_Destroy: its block
         * is then freed, and there is no shell to go on with. */
        w = WK();
        if (!w)
            return;
        display_prompt(w);
        uint32_t r[8] = { at(w->command_buffer), COMMAND_BUFFER_SIZE, ' ', 255, 0 }, c = 0;
        os_error *e = call(XOS_ReadLine32, r, &c);
        if (e) {
            if (say(e)) {
                setup_error(e);
                return;
            }
            continue;
        }
        if (c) {
            acknowledge_escape();
            continue;
        }
        if (r[1] == 0)
            return;
        e = call(XOS_CLI, REGS(at(w->command_buffer)), NULL);
        if (e && say(e)) {
            setup_error(e);
            return;
        }
    }
}

/* The exit handler, called when a program the shell ran has ended
 * (exit_veneer). It goes back to the command loop, on this thread's stack
 * where the loop stands. */
static void exit_handler(struct ros_cpu *s)
{
    (void)s;
    if (!here)
        return;                         /* this is not this thread's shell, so the program ends */
    ros_resume_unwind(here->frame);
    longjmp(here->jb, 1);
}

/* The error handler (error_handlerC). It is called for an error that nobody
 * caught. It prints the error and ends the program. The program's end goes
 * to the exit handler and then to the command loop. */
static void error_handler(struct ros_cpu *s)
{
    (void)s;
    uint32_t code, r12, buffer;
    ros_env_read(ROS_ENV_ERROR, &code, &r12, &buffer);
    writei(4);
    call(XOS_Write0, REGS(buffer + 8), NULL);
    newline();
    call(XOS_Exit, REGS(0), NULL);
}

/* The start entry. It runs *ShellCLI in the new task, as its application
 * (module_enter, exit_handlerC). */
static void start(struct ros_module *m, uint32_t tail)
{
    (void)m, (void)tail;
    if (xshell_create()) {
        call(XOS_Exit, REGS(0), NULL);  /* nothing is set up, so give up */
        return;
    }
    struct wk *w = WK();
    uint32_t wi[8] = { WIMP_VER, TASK_WORD, title(), 0 };
    call(XWimp_Initialise, wi, NULL);
    uint32_t task = wi[1];
    w->task_handle = task;
    setup_vdu(w);
    /* Tell the Wimp about the VDU 4 output, so that it redraws when the task has gone. */
    os_error *e = call(XWimp_CommandWindow, REGS(1), NULL);
    if (!e)                             /* make this module not the current
                                           object, so the Wimp may take
                                           application space */
        e = call(XOS_ChangeEnvironment, REGS(ROS_ENV_CAO, e_exit, 0, 0), NULL);
    if (e) {
        setup_error(e);
    } else {
        uint32_t r[8] = { name(w, "CLI$Greeting"), at(&w->env_error[2]), 252 - 1, 0,
                          VARTYPE_EXPANDED };
        if (!call(XOS_ReadVarVal, r, NULL)) {
            writei(4);
            ((char *)&w->env_error[2])[r[2]] = 0;
            call(XOS_PrettyPrint, REGS(at(&w->env_error[2]), 0, 0), NULL);
            newline();
        }

        /* This is the command loop's place on this thread. The exit handler
         * comes back here, and the loop runs again. */
        struct running run;
        run.frame = __builtin_frame_address(0);
        run.handlers = ros_handler_chain(NULL);
        ros_handler_chain(run.handlers);
        here = &run;
        if (setjmp(run.jb)) {
            /* An exit has happened. The SWIs it was in and their frames are
             * gone. So the SVC stack is made flat, the call depth is 0 and
             * only the loop's C handlers remain. The error handler gets a
             * fresh base (environment.h). */
            ros_call_depth = 0;
            ros_svc_sp = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;
            ros_handler_chain(run.handlers);
            struct ros_env_base fresh = { .active = 0 };
            ros_env_base_swap(&fresh);
            w = WK();
            if (w) {                    /* (there is none if its program destroyed it) */
                task = w->task_handle;
                set_handlers(w, NULL);
                command_loop(w);
            }
            here = NULL;
            xshell_destroy();
            call(XWimp_CloseDown, REGS(task, TASK_WORD), NULL);
            call(XWimp_CommandWindow, REGS(0xFFFFFFFFu), NULL);
            call(XOS_Exit, REGS(0), NULL);
            return;
        }
        command_loop(w);
        here = NULL;
    }
    xshell_destroy();
    writei(0);                          /* send something, so that the Wimp redraws */
    call(XWimp_CloseDown, REGS(task, TASK_WORD), NULL);
    call(XWimp_CommandWindow, REGS(0xFFFFFFFFu), NULL);
    call(XOS_Exit, REGS(0), NULL);
}

/* ---- the command ------------------------------------------------------------------ */

/* *ShellCLI: enters the module as the application. It gives an error if a shell already exists. */
static os_error *cmd_shellcli(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct wk *w = WK();
    if (w)
        return get_error(w, SHELLCLI_ERR_CREATION, "NoSpawn");
    return call(XOS_Module, REGS(2, title(), tail), NULL);
}

static const struct ros_command commands[] = {
    { "ShellCLI", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *ShellCLI",
      "ShellCLI - used by a Wimp Program to create a CLI shell", cmd_shellcli },
    { NULL, 0, NULL, NULL, NULL },
};

/* ---- the module ------------------------------------------------------------------ */

/* Sets CLI$Greeting from the Messages file. Failing to find a greeting is no reason to fail. */
static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    if (!e_exit) {
        e_exit = ros_native_entry(exit_handler, "shellcli:ExitHandler");
        e_error = ros_native_entry(error_handler, "shellcli:ErrorHandler");
    }
    struct {
        uint32_t desc[4];
        char file[48], token[16], var[16];
        char buffer[256];
    } *b;
    if (xos_module_claim(sizeof *b, (void **)&b))
        return NULL;
    strcpy(b->file, messages_file);
    strcpy(b->token, "Greeting");
    strcpy(b->var, "CLI$Greeting");
    if (!call(XMessageTrans_OpenFile, REGS(at(b->desc), at(b->file), 0), NULL)) {
        uint32_t r[8] = { at(b->desc), at(b->token), at(b->buffer), sizeof b->buffer };
        if (!call(XMessageTrans_Lookup, r, NULL))
            call(XOS_SetVarVal, REGS(at(b->var), at(b->buffer), r[3], 0, VARTYPE_STRING), NULL);
        call(XMessageTrans_CloseFile, REGS(at(b->desc)), NULL);
    }
    xos_module_free(b);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    struct wk *w = WK();
    return w ? get_error(w, SHELLCLI_ERR_REMOVAL, "SActive") : NULL;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    switch (s->r[1]) {
    case SERVICE_MEMORY:
        /* The Wimp is asking with this module as the current object, so
         * the application space is kept. */
        if (s->r[2] == m->base && s->r[0] == 0x80000000u)
            s->r[1] = 0;
        break;
    case SERVICE_WIMPCLOSEDOWN: {
        /* Wimp_Initialise is about to close the task that this domain ran
         * before it (R0 > 0, and R2 is that task). The request is refused
         * when that task is the shell's. This happens when a Wimp program
         * is started at the shell's prompt. */
        struct wk *w = WK();
        if ((int32_t)s->r[0] > 0 && w && w->task_handle == s->r[2])
            s->r[0] = at(get_error(w, SHELLCLI_ERR_WIMP_ACTIVE, "WActive"));
        break;
    }
    }
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ERR_BAD_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module shellcli_module = {
    .title = "ShellCLI",
    .help = "ShellCLI\t0.39 (09 Aug 2023) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .start = start,
    .swi_chunk = SHELLCLI_CHUNK,
    .swi_thunks = ros_swi_thunks_ShellCLI,
    .swi_names = ros_swi_names_ShellCLI,
    .swi_prefix = "Shell",
    .commands = commands,
};

/* The SWI count is known only at run time (api_gen.c). This constructor
 * stores it so that the runtime can read it when the module is added. */
__attribute__((constructor)) static void count(void)
{
    shellcli_module.swi_count = ros_swi_count_ShellCLI;
}
