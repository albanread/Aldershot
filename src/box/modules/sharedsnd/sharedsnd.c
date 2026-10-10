/* Copyright (c) 1995, Expressive Software Projects
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Expressive Software Projects nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL EXPRESSIVE SOFTWARE PROJECTS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is a translation into C of the SharedSound module of RISC OS Open
 * (Sources/Audio/SharedSnd: s.Module, s.SWIS, s.Handler, s.FillCode, s.Vars,
 * s.SampleRate, s.Volume).
 */

/* sharedsnd.c: SharedSound, RISC OS's sound mixer, as a native module.
 *
 * This is hand-converted from Sources/Audio/SharedSnd (the Expressive
 * Software Projects module, 7,018 lines of ObjAsm). The file
 * modules/sharedsnd/README.md says what is and is not done.
 *
 * Clients install linear handlers. A linear handler is a callback that
 * fills or mixes 16-bit stereo into the shared buffer. This module calls
 * every handler on each fill. The platform audio thread sets the pace
 * (the pump in platform/audio_alsa.c), and only one fill runs at a time.
 *
 * This module has the linear half only. The old 8-bit logarithmic voice
 * system (s/Log) is not here. The driver is always the platform's. There
 * is one sound device on this box and audio_alsa.c owns it. The ObjAsm's
 * Sound_LinearHandler, DMA probing and PowerWAVE are replaced, not ported.
 *
 * The handler table is the ObjAsm's. It has 10 slots of 64 bytes each. A
 * handler's identity is its address and parameter pair. The fill calls
 * each handler in turn with the original's registers (README.md). When the
 * table is empty the fill gives silence. The 16.16 fractional step
 * arithmetic is the ObjAsm's, integer for integer.
 */
#include <string.h>

#include "sharedsnd.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

/* ---- constants, the ObjAsm's ---------------------------------------------- */

#define HANDLER_MAX      10               /* s/Vars: handlerMax */
#define HANDLER_NAME_LEN 32
#define HANDLER_SIZE     (32 + HANDLER_NAME_LEN)   /* handlerTableLen: 64 */

/* handler types (s/Vars) */
#define HT_IMMEDIATE 0
#define HT_CALLBACK  1
#define HT_PROCESS   2

/* the system rate, which is the platform's */
#define SYSTEM_RATE (44100)

/* errors (s/Vars: ErrorNumber_SSound_*) */
#define ERR_INITMEM      0xB0
#define ERR_MAXHANDLERS 0xB1
#define ERR_BADHANDLER   0xB2

/* ---- the workspace ---------------------------------------------------------- */

struct ws {
    uint32_t private_word_copy;

    /* the handler table, the ObjAsm's layout */
    struct {
        uint32_t address;           /* 0: the code, 0 = empty */
        uint32_t parameter;         /* 4: R12 */
        uint32_t flags;             /* 8 */
        uint32_t sample_frequency;  /* 12 */
        uint32_t volume;            /* 16 */
        uint32_t type;              /* 20 */
        uint32_t fraction;          /* 24: 16.16 of the system rate */
        uint32_t volume_scaled;     /* 28 */
        char name[HANDLER_NAME_LEN]; /* 32 */
    } handler[HANDLER_MAX];

    uint32_t control_word;          /* Replay's 1-of-n */
    uint32_t system_rate;           /* the platform's, Hz */
    uint32_t system_period;         /* its period, OS units */
};

static uint32_t A(const void *p)
{
    return ros_addr(p);
}

static os_error *swi(uint32_t number, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

static struct ws *workspace(struct ros_module *m)
{
    uint32_t pw = ros_ld32(m->private_word);
    return (int32_t)pw > 0 ? ros_ptr(pw) : NULL;
}

/* ---- the fill: the platform's callback, the handlers' caller ----------------- */

/* The platform thread calls this with the personality lock held (the
 * pump's safe point). Each handler in turn is called with the original's
 * registers, with the whole buffer. Every call has R0 = 0, so each handler
 * fills the buffer and overwrites what the one before wrote. What the last
 * handler wrote is what plays. */
static int32_t fill_depth;           /* guard against a recursive fill */

static void fill(int16_t *out, size_t frames)
{
    struct ros_module *m = &sharedsnd_module;
    struct ws *w = workspace(m);
    if (!w || fill_depth)
        return;
    fill_depth++;

    for (int i = 0; i < HANDLER_MAX; i++) {
        if (!w->handler[i].address)
            continue;
        /* The original's registers are as follows (README.md). R0 is the
         * flags. 0 means fill, and 1 would mean mix, but this code always
         * passes 0. R1 is the buffer and R2 is its end. R4 and R5 are a source span, and
         * here they are the buffer's own span. R6 is the fraction, R7 the
         * volume, R9 the accumulator and R12 the parameter. */
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 0;                          /* fill, not mix */
        s.r[1] = A(out);
        s.r[2] = A(out) + frames * 4;
        s.r[3] = 0;
        s.r[4] = A(out);
        s.r[5] = A(out) + frames * 4;
        s.r[6] = w->handler[i].fraction;
        s.r[7] = w->handler[i].volume_scaled;
        s.r[9] = 0;                          /* the accumulator, fresh */
        s.r[12] = w->handler[i].parameter;
        ros_call(&s, w->handler[i].address);
        /* The handler wrote the buffer through R1. Nothing is kept from
         * its output registers. The next handler writes over the buffer. */
    }

    fill_depth--;
}

/* ---- the SWIs ---------------------------------------------------------------- */

static os_error *err_bad(void)
{
    return ros_error(ERR_BADHANDLER, "Bad handler");
}

static os_error *err_max(void)
{
    return ros_error(ERR_MAXHANDLERS, "Too many handlers installed");
}

/* The handler's 16.16 fraction of the system rate (SampleRate's arithmetic) */
static uint32_t fraction_of(uint32_t freq)
{
    if (!freq)
        return 0x10000;                 /* 1:1 */
    return (uint32_t)(((uint64_t)freq << 16) / SYSTEM_RATE);
}

/* ---- the SWI thunks: one per SWI, as in the rest of the tree ------------------- */

static struct ws *ws_or_fail(struct ros_cpu *s)
{
    struct ws *w = workspace(&sharedsnd_module);
    if (!w)
        ros_swi_fail(s, ros_error(ERR_INITMEM, "NoMem"));
    return w;
}

void ros_thunk_SharedSound_InstallHandler(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t address = s->r[0], parameter = s->r[1];
    uint32_t flags = s->r[2], type = flags & 1 ? s->r[4] : HT_IMMEDIATE;
    if (!address) {
        ros_swi_fail(s, err_bad());
        return;
    }
    int slot = -1;
    for (int i = 0; i < HANDLER_MAX; i++) {
        if (w->handler[i].address == address &&
            w->handler[i].parameter == parameter)
            { slot = i; break; }
        if (slot < 0 && !w->handler[i].address)
            slot = i;
    }
    if (slot < 0) {
        ros_swi_fail(s, err_max());
        return;
    }
    w->handler[slot].address = address;
    w->handler[slot].parameter = parameter;
    w->handler[slot].flags = flags;
    w->handler[slot].sample_frequency = SYSTEM_RATE;
    w->handler[slot].volume = 0x10000;
    w->handler[slot].volume_scaled = 0x10000;
    w->handler[slot].type = type;
    w->handler[slot].fraction = 0x10000;
    if (s->r[3] >= 0x8000u) {
        const char *name = ros_ptr(s->r[3]);
        uint32_t n = 0;
        while (n < HANDLER_NAME_LEN - 1 && name[n] >= ' ')
            n++;
        memcpy(w->handler[slot].name, name, n);
        w->handler[slot].name[n] = 0;
    } else {
        w->handler[slot].name[0] = 0;
    }
    s->r[0] = (uint32_t)slot;
}

void ros_thunk_SharedSound_RemoveHandler(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    int slot = -1;
    if (s->r[0] < HANDLER_MAX && w->handler[s->r[0]].address)
        slot = (int)s->r[0];
    else
        for (int i = 0; i < HANDLER_MAX; i++)
            if (w->handler[i].address == s->r[0] &&
                w->handler[i].parameter == s->r[1])
                { slot = i; break; }
    if (slot < 0) {
        ros_swi_fail(s, err_bad());
        return;
    }
    w->handler[slot].address = 0;
}

void ros_thunk_SharedSound_HandlerType(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w || s->r[0] >= HANDLER_MAX || !w->handler[s->r[0]].address) {
        if (w) ros_swi_fail(s, err_bad());
        return;
    }
    s->r[0] = w->handler[s->r[0]].type;
}

void ros_thunk_SharedSound_HandlerInfo(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    int slot = (int)s->r[0];
    if (slot) {
        if (slot < 0 || slot >= HANDLER_MAX || !w->handler[slot].address) {
            ros_swi_fail(s, err_bad());
            return;
        }
    } else {
        slot = -1;
        for (int i = 0; i < HANDLER_MAX; i++)
            if (w->handler[i].address)
                { slot = i; break; }
        if (slot < 0) {
            ros_swi_fail(s, err_bad());
            return;
        }
    }
    s->r[0] = (uint32_t)slot;
    s->r[1] = w->handler[slot].address;
    s->r[2] = w->handler[slot].parameter;
    s->r[3] = w->handler[slot].flags;
    uint32_t room = s->r[4];
    const char *name = w->handler[slot].name;
    uint32_t n = 0;
    while (name[n] && n < HANDLER_NAME_LEN)
        n++;
    if (room >= 0x8000u) {
        uint32_t copy = n < room - 1 ? n : (room ? room - 1 : 0);
        if (room) {
            memcpy(ros_ptr(room), name, copy);
            ros_st8(room + copy, 0);
        }
        s->r[5] = copy;
    } else {
        s->r[5] = n;
    }
}

void ros_thunk_SharedSound_SampleRate(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    if (s->r[0]) {
        int slot = (int)s->r[1];
        if (slot < 0 || slot >= HANDLER_MAX || !w->handler[slot].address) {
            ros_swi_fail(s, err_bad());
            return;
        }
        w->handler[slot].sample_frequency = s->r[0];
        w->handler[slot].fraction = fraction_of(s->r[0]);
    }
    s->r[1] = w->system_rate;
    s->r[2] = fraction_of(w->system_rate);
}

void ros_thunk_SharedSound_HandlerVolume(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    int slot = (int)s->r[0];
    if (slot) {
        if (slot < 0 || slot >= HANDLER_MAX || !w->handler[slot].address) {
            ros_swi_fail(s, err_bad());
            return;
        }
        if (s->r[1])
            w->handler[slot].volume = s->r[1];
        s->r[0] = w->handler[slot].volume;
    } else {
        s->r[0] = 0x10000;
    }
}

void ros_thunk_SharedSound_HandlerSampleType(struct ros_cpu *s)
{
    (void)s;                                /* 16-bit stereo, always */
}

void ros_thunk_SharedSound_HandlerPause(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    int slot = (int)s->r[0];
    if (slot > 0 && slot < HANDLER_MAX && w->handler[slot].address)
        w->handler[slot].flags = (w->handler[slot].flags & ~1u) |
                                (s->r[1] ? 1u : 0u);
}

void ros_thunk_SharedSound_InstallDriver(struct ros_cpu *s)
{
    s->r[0] = 1;                           /* the platform's, always */
}

void ros_thunk_SharedSound_RemoveDriver(struct ros_cpu *s)
{
    ros_swi_fail(s, ros_error(0xB3, "Cannot remove the only driver"));
}

void ros_thunk_SharedSound_DriverInfo(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    s->r[0] = 1;
    s->r[1] = w->system_rate;
    s->r[2] = 0;
    s->r[3] = 0;
}

void ros_thunk_SharedSound_DriverVolume(struct ros_cpu *s)
{
    s->r[0] = 0x10000;
}

void ros_thunk_SharedSound_DriverMixer(struct ros_cpu *s)
{
    ros_swi_fail(s, ros_error(0xB4, "No mixer"));
}

void ros_thunk_SharedSound_ControlWord(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    s->r[0] = A(&w->control_word);
}

void ros_thunk_SharedSound_Info(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    s->r[0] = w->system_rate;
    s->r[1] = w->system_period;
}

/* ---- the module ----------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    void *block;
    if (xos_module_claim(sizeof(struct ws), &block))
        return ros_error(ERR_INITMEM, "NoMem");
    struct ws *w = block;
    memset(w, 0, sizeof *w);
    w->system_rate = SYSTEM_RATE;
    w->system_period = 100;                   /* the OS's period units */
    ros_st32(m->private_word, A(w));

    ros_audio_start(fill);                    /* the platform's callback */
    return NULL;
}

static void final_shutdown(struct ros_module *m)
{
    struct ws *w = workspace(m);
    if (!w)
        return;
    ros_audio_start(NULL);                   /* silence: no handler calls */
    ros_st32(m->private_word, 0);
    xos_module_free(w);
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    final_shutdown(m);
    return NULL;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    (void)s;
    /* The ObjAsm broadcasts Service_SharedSoundAlive at initialisation and
     * Service_SharedSoundDying at finalisation. This module does not do so
     * yet, because nothing listens for them on this box. */
}

static const struct ros_command commands[] = {
    { "SharedSound", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *SharedSound",
      "The Shared Sound module mixes sound handlers into one stream.\r",
      NULL },
    { 0 },
};

struct ros_module sharedsnd_module = {
    .title = "SharedSound",
    .help = "SharedSound\t1.20 (29-Sep-26) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .commands = commands,
    .swi_chunk = 0x4B440,
    .swi_thunks = ros_swi_thunks_SharedSound,
    .swi_names = ros_swi_names_SharedSound,
    .swi_prefix = "SharedSound",
};

__attribute__((constructor)) static void sharedsnd_swi_count(void)
{
    sharedsnd_module.swi_count = ros_swi_count_SharedSound;
}
