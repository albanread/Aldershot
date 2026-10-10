/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* wimprec.c -- what each Wimp task was given, written down. This is the
 * recorder that a differential test harness uses to compare two Window
 * Managers. It sits at the SWI boundary, not in the Wimp, so that it
 * records any Wimp in the same way, whether the translated one or a native
 * one.
 *
 * Only a task's own calls are recorded. These are the outermost SWI, the
 * one the task made. The SWIs that a Wimp makes of itself to do its work
 * are never recorded, because a different Wimp need not make them.
 * Wimp_Poll returns on the thread of the task it returns to
 * (runtime/callback.c), so its record is that task's. It records the event
 * that the task was given, after the filters. Null events are left out,
 * because how many a task gets is a matter of time. Everything the task
 * does with what it is given is also left out, because that is the task's
 * business.
 *
 * There is one line for each record. The task comes first (the Wimp's
 * handle for it, as the DomainId word holds it). Numbers are in
 * hexadecimal.
 *
 *   I <task> <name>                       Wimp_Initialise returned it
 *   C <task> <window>                     Wimp_CreateWindow returned it
 *   P <task> <reason> <word>...           Wimp_Poll / PollIdle gave it this
 *   S <task> <reason> <R2> <R3> <word>... Wimp_SendMessage, as it was called
 *   E <task> <swi> <error number>         a Wimp SWI it called failed
 *   X <task>                              Wimp_CloseDown
 *
 * A record's words are the block's. For a message that is its whole length
 * (word 0). For an event it is the length that event has (PRM3's
 * Wimp_Poll). Handles, references and addresses are written as they are.
 * tests/wimprec makes them comparable between two runs.
 *
 * The recorder is switched on with rosgd.wimprec[=<leaf>] or with
 * ROSGD_WIMPREC=<path>. In the box the file is the share's <leaf>, or
 * WimpRec if none is given. ROSGD_WIMPREC is for the hosted build. When it
 * is off, it costs a test of one word on each SWI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/wimprec.h"

int ros_wimprec_on;
static FILE *out;
static char path[256];

#define WIMP_INITIALISE   0x400C0u
#define WIMP_CREATEWINDOW 0x400C1u
#define WIMP_POLL         0x400C7u
#define WIMP_CLOSEDOWN    0x400DDu
#define WIMP_POLLIDLE     0x400E1u
#define WIMP_SENDMESSAGE  0x400E7u

/* The setting is read at the start. The file is opened at the first
 * record, because the share is mounted after the runtime starts
 * (boot/main.c). */
void ros_wimprec_init(void)
{
    const char *env = getenv("ROSGD_WIMPREC");
    if (env) {
        snprintf(path, sizeof path, "%s", env);
    } else if (ros_cmdline_has("rosgd.wimprec")) {
        const char *leaf = ros_cmdline_value("rosgd.wimprec");
        snprintf(path, sizeof path, "/host/%s", leaf && *leaf ? leaf : "WimpRec");
    } else {
        return;
    }
    ros_wimprec_on = 1;
}

static int ready(void)
{
    if (out)
        return 1;
    out = fopen(path, "w");
    if (!out) {
        fprintf(stderr, "rosgd: wimprec: cannot write %s\n", path);
        ros_wimprec_on = 0;
        return 0;
    }
    setvbuf(out, NULL, _IOLBF, 0);      /* a box may be stopped at any line */
    return 1;
}

static uint32_t current_task(void)
{
    return ros_ld32(ROS_ZP_DOMAINID);
}

static void words(uint32_t at, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        fprintf(out, " %X", ros_ld32(at + 4 * i));
}

/* How long an event's block is, in words. The lengths are PRM3's for
 * Wimp_Poll. A menu selection runs up to its -1, and a message has its own
 * length. */
static unsigned event_words(uint32_t reason, uint32_t block)
{
    switch (reason) {
    case 1: case 3: case 4: case 5:
        return 1;                       /* a window handle */
    case 2:
        return 8;                       /* the open block */
    case 6:
        return 5;                       /* x, y, buttons, window, icon */
    case 7:
        return 4;                       /* the drag box */
    case 8:
        return 7;                       /* the caret block and the key */
    case 9: {
        unsigned n = 0;
        while (n < 16 && ros_ld32(block + 4 * n) != 0xFFFFFFFFu)
            n++;
        return n + 1;                   /* the selections and their -1 */
    }
    case 10:
        return 10;                      /* the open block, and x and y */
    case 11: case 12:
        return 6;                       /* the caret block */
    case 13:
        return 2;                       /* the pollword's address and value */
    case 17: case 18: case 19: {
        uint32_t size = ros_ld32(block);
        return size >= 20 && size <= 256 ? size / 4 : 5;
    }
    default:
        return 0;
    }
}

void ros_wimprec_call(uint32_t n, const uint32_t in[4])
{
    if (!ready())
        return;
    if (n == WIMP_SENDMESSAGE) {
        uint32_t reason = in[0], block = in[1];
        fprintf(out, "S %X %X %X %X", current_task(), reason, in[2], in[3]);
        words(block, event_words(reason, block));
        fputc('\n', out);
    } else if (n == WIMP_CLOSEDOWN) {
        fprintf(out, "X %X\n", current_task());
    }
}

void ros_wimprec_return(uint32_t n, const uint32_t in[4], const struct ros_cpu *s)
{
    if (!ready())
        return;
    if (s->v) {
        fprintf(out, "E %X %X %X\n", current_task(), n, ros_ld32(s->r[0]));
        return;
    }
    switch (n) {
    case WIMP_INITIALISE: {
        char name[64];
        unsigned i = 0;
        for (uint32_t p = in[2]; p && i < sizeof name - 1; i++, p++) {
            uint8_t c = ros_ld8(p);
            if (c < ' ')
                break;
            name[i] = (char)c;
        }
        name[i] = 0;
        fprintf(out, "I %X %s\n", s->r[1], name);
        break;
    }
    case WIMP_CREATEWINDOW:
        fprintf(out, "C %X %X\n", current_task(), s->r[0]);
        break;
    case WIMP_POLL:
    case WIMP_POLLIDLE:
        if (s->r[0] != 0) {             /* not a null */
            /* The block the task gave. R1 comes back as it went in. */
            fprintf(out, "P %X %X", current_task(), s->r[0]);
            words(in[1], event_words(s->r[0], in[1]));
            fputc('\n', out);
        }
        break;
    default:
        break;
    }
}
