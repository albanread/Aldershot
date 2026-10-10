/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_speakers.c: Speakers, native (modules/speakers): the two
 * commands that say where sound goes and change it.
 *
 * What a box's outputs are is the machine's business. A PC has the
 * board's socket and a device per display output, and a hosted build has
 * none. So the test holds the module to what must be true whatever they are.
 * *ListSpeakers says something. Every line it prints names a card and a
 * device. No more than one is playing. *SelectSpeaker refuses a number
 * that is not on the list, and changes nothing.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"
#include "speakers.h"

#define check ros_check
#define WRCHV 0x03u

static uint32_t line;
static const os_error *last;
static char out[2048];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1 && s->r[0] != 13)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI: the error number or 0, with what it printed in out */
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

void ros_selftest_speakers(void)
{
    line = ros_addr(ros_rma_alloc(512));

    struct ros_audio_output list[16];
    unsigned n = ros_audio_outputs(list, 16);
    unsigned playing = 0;
    for (unsigned i = 0; i < n; i++)
        playing += list[i].playing != 0;
    check(playing <= 1, "Speakers -- the box plays through one output at a time",
          "%u of %u outputs say they are playing", playing, n);

    uint32_t e = cli("ListSpeakers");
    int said = strlen(out) > 8;
    int lines = 0, named = 0;
    for (const char *s = out; *s; s++)
        if (*s == '\n') {
            lines++;
            named += 1;                         /* counted again below, per line */
        }
    named = 0;
    for (const char *s = out; (s = strstr(s, "card ")) != NULL; s++)
        if (strstr(s, "device "))
            named++;
    check(!e && said && (n == 0 ? strstr(out, "no sound outputs") != NULL : named == (int)n),
          "*ListSpeakers -- every output, with its card and device",
          "%u output(s), %d line(s), %d named: \"%s\"", n, lines, named, out);

    e = cli("SelectSpeaker 99");
    const char *why = last ? last->errmess : "";
    check(e != 0 && strstr(why, n ? "99" : "no sound outputs") != NULL,
          "*SelectSpeaker -- a number that is not on the list is refused",
          "%u output(s): error &%X \"%s\"", n, e, why);

    /* The file of *SoundCapture is a RISC OS name, mapped to a Linux file:
     * a name that is not on HostFS is refused as a file name, before the
     * capture is asked for (the capture's own refusal is this module's
     * error number) */
    e = cli("SoundCapture 1 Resources:$.Nope");
    check(e != 0 && e != 0xC7, "*SoundCapture -- the file is mapped as a RISC OS name",
          "error &%X \"%s\"", e, last ? last->errmess : "");

    unsigned after = ros_audio_outputs(list, 16);
    unsigned still = 0;
    for (unsigned i = 0; i < after; i++)
        still += list[i].playing != 0;
    check(after == n && still == playing, "Speakers -- a refused choice changes nothing",
          "%u outputs and %u playing, was %u and %u", after, still, n, playing);
}
