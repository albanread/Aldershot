/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* speakers.c: Speakers, a native module. It says which output the box
 * plays through, and has the commands to change it.
 *
 * A PC has more than one place sound can go. There is the board's own
 * socket, and one output for each display output of every graphics card.
 * An HDMI or DisplayPort monitor is a sound device as much as a screen.
 * Which of them a box should use is the owner's choice. The box picks a
 * reasonable one at the start (platform/audio_alsa.c picks a monitor that
 * says it takes sound, else the board's socket). This module lets that
 * choice be seen and changed:
 *
 *   *ListSpeakers           what there is, and which one is playing
 *   *SelectSpeaker <n>      play through that one from now on
 *
 * The choice holds until the box stops. To keep it, put the *SelectSpeaker
 * in the disc's !Boot, where the rest of a machine's own settings are.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "speakers.h"

#define ERR_SPEAKERS 0xC7u
#define MAX_OUTPUTS  16

static os_error *print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static os_error *print(const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    for (const char *s = text; *s; s++) {
        os_error *e = *s == '\n' ? xos_new_line() : xos_write_c((uint8_t)*s);
        if (e)
            return e;
    }
    return NULL;
}

/* "LG HDR 4K on DisplayPort", "ALC222 Analog" */
static void describe(const struct ros_audio_output *o, char *out, size_t max)
{
    if (o->how[0])
        snprintf(out, max, "%s on %s", o->name, o->how);
    else
        snprintf(out, max, "%s", o->name);
}

static os_error *cmd_list(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    struct ros_audio_output list[MAX_OUTPUTS];
    unsigned n = ros_audio_outputs(list, MAX_OUTPUTS);
    if (!n)
        return print("This box has no sound outputs.\n");
    os_error *e = NULL;
    for (unsigned i = 0; i < n && !e; i++) {
        char what[96];
        describe(&list[i], what, sizeof what);
        e = print("%c%2u  %-44s card %u device %u%s\n", list[i].playing ? '*' : ' ', i + 1, what,
                  list[i].card, list[i].device,
                  list[i].playing ? "  (playing)" : list[i].live ? "" : "  (nothing attached)");
    }
    return e;
}

static os_error *cmd_select(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    const char *t = ros_ptr(tail);
    unsigned which = 0;
    while (*t == ' ')
        t++;
    while (*t >= '0' && *t <= '9')
        which = which * 10 + (unsigned)(*t++ - '0');
    struct ros_audio_output list[MAX_OUTPUTS];
    unsigned n = ros_audio_outputs(list, MAX_OUTPUTS);
    if (!which || which > n)
        return n ? ros_error(ERR_SPEAKERS, "There is no speaker %u: *ListSpeakers gives "
                                           "%u to choose from", which, n)
                 : ros_error(ERR_SPEAKERS, "This box has no sound outputs");
    const struct ros_audio_output *o = &list[which - 1];
    char what[96];
    describe(o, what, sizeof what);
    /* The sound's own thread makes the change between two buffer fills.
     * The box waits for it with ros_idle, which lets the rest of the box
     * run, instead of holding everything up. */
    ros_audio_select(o->card, o->device);
    for (int i = 0; i < 50 && ros_audio_select_state() == ROS_AUDIO_SELECT_ASKED; i++)
        ros_idle(10);
    if (ros_audio_select_state() != ROS_AUDIO_SELECT_DONE)
        return ros_error(ERR_SPEAKERS, "%s will not play 16-bit stereo: the box is still on "
                                       "the output it had", what);
    return print("Playing through %s.\n", what);
}

/* *SoundCapture <seconds> [<file>]: keeps what the box sends the card, as
 * a WAV file. This is the only way to tell "nothing is playing" from
 * "something is playing silence". It needs no reboot, so it works on a
 * machine that can only be reached over SSH. */
static os_error *cmd_capture(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    const char *t = ros_ptr(tail);
    while (*t == ' ')
        t++;
    unsigned seconds = 0;
    while (*t >= '0' && *t <= '9')
        seconds = seconds * 10 + (unsigned)(*t++ - '0');
    while (*t == ' ')
        t++;
    char file[256];
    unsigned n = 0;
    while ((uint8_t)t[n] >= ' ' && n < sizeof file - 1)
        file[n] = t[n], n++;
    file[n] = 0;
    if (!seconds || seconds > 120)
        return ros_error(ERR_SPEAKERS, "Syntax: *SoundCapture <seconds, 1 to 120> [<file>]");
    /* The file is a RISC OS name, as the help says.  It is mapped to the
     * Linux file it is on HostFS or the share.  It must not reach open()
     * as typed: the box runs as root, and a name such as /etc/passwd or
     * ../x would then be written over. */
    char path[256];
    path[0] = 0;
    if (file[0]) {
        os_error *e = ros_hostfs_linux_path(file, path, sizeof path);
        if (e)
            return e;
    }
    if (ros_audio_capture(seconds, path) != 0)
        return ros_error(ERR_SPEAKERS, "The box cannot take that: one capture runs at a time, "
                                       "and a box with no card has nothing to take");
    return print("Keeping %u second%s of what goes to the card%s%s.\n", seconds,
                 seconds == 1 ? "" : "s", file[0] ? ", in " : " (the share's, or the disc's, "
                 "SoundCapture.wav)", file[0] ? file : "");
}

static const struct ros_command commands[] = {
    { "ListSpeakers", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *ListSpeakers",
      "*ListSpeakers lists the sound outputs: the board's own socket, and the display\r"
      "outputs of any graphics card, which carry sound to a monitor that takes it.\r"
      "The one playing is marked; *SelectSpeaker changes it.\r",
      cmd_list },
    { "SelectSpeaker", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *SelectSpeaker <number>",
      "*SelectSpeaker plays through the output with that number in *ListSpeakers, from\r"
      "now until the box stops. Put it in the disc's !Boot to keep it.\r",
      cmd_select },
    { "SoundCapture", ROS_CMD_INFO(1, 2, 0, 0), "Syntax: *SoundCapture <seconds> [<file>]",
      "*SoundCapture keeps what the box sends the sound card for that many seconds, as a WAV\r"
      "of 16-bit stereo -- the share's SoundCapture.wav, or the disc's where there is no share.\r"
      "It says what the box is playing, which a card that is silent cannot.\r",
      cmd_capture },
    { 0 },
};

struct ros_module speakers_module = {
    .title = "Speakers",
    .help = "Speakers\t1.00 (02 Oct 2026) ROSGD native",
    .commands = commands,
};
