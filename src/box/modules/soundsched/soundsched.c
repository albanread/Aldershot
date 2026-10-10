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
 * This file is a translation into C of RISC OS Open's Sound scheduler
 * (Sources/HWSupport/Sound/Sound2: s.Sound2).
 */

/* soundsched.c: SoundScheduler, the sound queue, as a native module.
 *
 * This is hand-converted from HWSupport/Sound/Sound2 ("SoundScheduler",
 * 1,303 lines of ObjAsm). On RISC OS the queue is what plays music.
 * BASIC's SOUND statement with its fifth (time) parameter compiles to
 * Sound_QSchedule. That queues an event which sounds when the beat counter
 * reaches the given time. Maestro queues a bar of notes at a time. It
 * watches BASIC's BEAT wrap round to queue the next bar.
 *
 * The DMA interrupt's dispatch walks the timing wheel and fires each
 * event. Control 0 becomes Sound_Control on the channel. Control
 * &0F000000+number becomes that SWI (Maestro uses this for MIDI note on
 * and off, when the MIDI module is present). Any other control value is a
 * call through the address.
 *
 * Here the wheel is a list. It holds a few hundred events at most.
 * Insertion is kept stable, so events with equal times fire in the order
 * they were scheduled. The wheel's buckets do the same. Each event has an
 * absolute time on the queue's own clock, which never stops.
 *
 * The beat counter is Sound_QBeat's, and BASIC's BEAT. It is Sound2's
 * QBeat, kept beside the clock. It counts only while a bar length is set.
 * It runs from 0 to the length less one and then starts again. A new
 * length takes over from where the counter stands (s/Sound2's
 * AdvanceSlot). A time given to Sound_QSchedule counts from the counter's
 * last 0, or from now when there is no bar.
 *
 * Earlier, the counter was the clock taken modulo the bar. A bar then
 * started at whatever phase the clock had reached since the box booted,
 * and a new bar length made the phase jump. Maestro's lead-in bar was cut
 * to what was left of it. The bars of its time signature came round off
 * the beat they had been queued for, which left a gap in the music (#62).
 *
 * The clock advances on the sound fill's tick instead of the interrupt's.
 * ros_soundq_tick is called by the Sound module for every buffer and is
 * the dispatch.
 *
 * Tempo is beats per centisecond in fixed point with 12 fraction bits.
 * s/Sound2's dispatch adds the low 16 bits of QTempo at bit 16 and counts
 * the carries past bit 28. So DefaultTempo, &1000, is one beat a
 * centisecond (the PRM's Sound_QTempo). Maestro computes the tempo as
 * BPM*128*4096/6000, because a crotchet is 128 beats. Earlier the fraction
 * point was at bit 19, which made the queue 128 times too slow. Maestro's
 * bar never came round and it played nothing (#62).
 */
#include <stdatomic.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "sound.h"
#include "soundsched.h"

/* ---- constants, the ObjAsm's ---------------------------------------------- */

#define QMAX            512             /* events: the wheel was 4K of RMA */
#define DEFAULT_TEMPO   0x1000          /* s/Sound2: DefaultTempo, &1000 */
#define ONE_BEAT        (1u << 12)      /* the tempo's fraction point: &1000 a beat */

/* ---- the workspace ---------------------------------------------------------- */

struct event {
    uint32_t time;                      /* the absolute beat it fires at */
    uint32_t control;                   /* 0 sound, &0F000000+SWI a SWI */
    uint32_t data0, data1;              /* its R2 and R3 */
    uint32_t seq;                       /* insertion order, equal times */
};

struct ws {
    uint32_t private_word_copy;

    struct event q[QMAX];               /* sorted by (time, seq) */
    uint32_t qdepth;
    uint32_t qseq;

    uint32_t tempo_cs;                  /* beats per centisecond, 2^12 fixed */
    uint64_t accum;                     /* the beats under one whole */
    uint32_t beat;                      /* the queue's clock: beats, never stopping */
    uint32_t qbeat;                     /* Sound2's QBeat: 0 to the bar length less
                                         * one, counting only while there is one */
    uint32_t beat_count;                /* the bar length, 0 off */
    uint32_t qlast;                     /* the clock's time last scheduled at */

    /* what has passed through, for ros_soundq_state (the harness's view) */
    uint32_t scheduled;                 /* events queued */
    uint32_t fired_notes;               /* sound events fired */
    uint32_t fired_other;               /* SWI (and address) events fired */
    uint32_t bar_events;                /* Event_Sound raised: bars come round */
};

/* The queue is touched from the audio thread's tick and from SWIs on task
 * threads. One spinlock guards it, and it is held for the insert and for
 * the fire. The spinlock uses C11 atomics because the Mac's hosted build
 * has no pthread_spin. */
static _Atomic int qlock;

static void q_lock(void)
{
    while (atomic_exchange_explicit(&qlock, 1, memory_order_acquire))
        while (atomic_load_explicit(&qlock, memory_order_relaxed))
            ;
}

static void q_unlock(void)
{
    atomic_store_explicit(&qlock, 0, memory_order_release);
}

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

#define EVENT_SOUND 10                  /* Event_Sound: a bar come round */

static struct ws *workspace(struct ros_module *m)
{
    uint32_t pw = ros_ld32(m->private_word);
    return (int32_t)pw > 0 ? ros_ptr(pw) : NULL;
}

/* ---- the fire and the tick ------------------------------------------------ */

/* Fires one event that is due. Control 0 is a sound event. Its R2 and R3
 * are packed as BASIC packs them. In R2 byte 0 is the channel and bytes
 * 2-3 are the amplitude with its gate. R3 holds the pitch in its low half
 * and the duration in its high half (s/Stmt's SOUND, Sound2's
 * SoundControlDispatch). Control &0F000000+SWI calls that SWI with the
 * event's data as R0 and R1. */
static void fire(struct ws *w, const struct event *e)
{
    if (e->control == 0) {
        w->fired_notes++;
        ros_sound_note(e->data0 & 0xFFFF,
                       e->data0 >> 16,               /* amp, Sound1's coding */
                       (int32_t)(e->data1 & 0xFFFF),
                       e->data1 >> 16);
        return;
    }
    w->fired_other++;
    if ((e->control & 0x0F000000u) == 0x0F000000u) {
        uint32_t r[8];
        r[0] = e->data0;
        r[1] = e->data1;
        r[2] = 0; r[3] = 0; r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        swi(e->control & 0x00FFFFFFu, r);
        return;
    }
    /* a bare address to call: nothing on this box schedules one */
}

void ros_soundq_tick(unsigned centisecs)
{
    struct ws *w = workspace(&soundsched_module);
    if (!w || !centisecs)
        return;

    q_lock();
    w->accum += (uint64_t)w->tempo_cs * centisecs;
    for (;;) {
        /* Fire everything at or before now, in (time, seq) order. An event
         * queued for now fires at this dispatch, as the wheel's current
         * slot does. */
        while (w->qdepth && (int32_t)(w->q[0].time - w->beat) <= 0) {
            struct event e = w->q[0];
            memmove(&w->q[0], &w->q[1], (w->qdepth - 1) * sizeof e);
            w->qdepth--;
            q_unlock();
            fire(w, &e);                /* outside the lock: it SWIs */
            q_lock();
        }
        if (w->accum < ONE_BEAT)
            break;
        w->accum -= ONE_BEAT;
        w->beat++;
        /* The beat counter, in a bar, goes round to 0 at the bar length.
         * If the length is set below the counter, the counter goes to 0
         * next. */
        if (w->beat_count && ++w->qbeat >= w->beat_count) {
            /* A bar coming round raises Event_Sound, level 2, as Sound2's
             * AdvanceSlot does. It is raised only when the count reaches
             * the length. A shorter length that cuts the count back does
             * not raise it (#72). */
            int bar = w->qbeat == w->beat_count;
            w->qbeat = 0;
            if (bar) {
                q_unlock();
                struct ros_cpu c;
                ros_cpu_enter(&c);
                c.r[0] = EVENT_SOUND;
                c.r[1] = 2;
                c.r[2] = 0;
                ros_event_generate(&c);
                w->bar_events++;
                q_lock();
            }
        }
    }
    q_unlock();
}

void ros_soundq_state(struct ros_soundq_state *st)
{
    struct ws *w = workspace(&soundsched_module);
    memset(st, 0, sizeof *st);
    if (!w)
        return;
    st->beat = w->beat;
    st->qbeat = w->qbeat;
    st->bar = w->beat_count;
    st->tempo = w->tempo_cs;
    st->depth = w->qdepth;
    st->scheduled = w->scheduled;
    st->fired_notes = w->fired_notes;
    st->fired_other = w->fired_other;
    st->bar_events = w->bar_events;
}

uint32_t ros_soundq_beat(void)
{
    struct ws *w = workspace(&soundsched_module);
    return w ? w->qbeat : 0;
}

/* ---- the SWIs ---------------------------------------------------------------- */

static struct ws *ws_or_fail(struct ros_cpu *s)
{
    struct ws *w = workspace(&soundsched_module);
    if (!w)
        ros_swi_fail(s, ros_error(0x1A6, "Sound system not present"));
    return w;
}

void ros_thunk_Sound_QInit(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    q_lock();
    w->qdepth = 0;
    w->qseq = 0;
    w->beat = 0;
    w->qbeat = 0;                      /* the counter off and at 0 */
    w->beat_count = 0;
    w->qlast = 0;
    w->accum = 0;
    w->tempo_cs = DEFAULT_TEMPO;
    q_unlock();
    s->r[0] = 0;                       /* success, as the ObjAsm reports */
}

void ros_thunk_Sound_QSchedule(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    int32_t t = (int32_t)s->r[0];
    q_lock();
    uint32_t at;
    if (t < 0)
        /* Any negative time, -2 too, means the same time as the last
         * event scheduled. Sound2's SoundQSchedule_SWI does this (CMP
         * r0,#0; LDRMI QLast). The PRM says -2 is "immediate", but that
         * is not what 5.30 does (#72). */
        at = w->qlast;
    else {
        /* The time counts from the bar's last 0, which is now less the
         * counter. With no bar the counter is 0, so it counts from now.
         * A time already past becomes now. */
        int32_t rel = t - (int32_t)w->qbeat;
        if (rel < 0)
            rel = 0;
        at = w->beat + (uint32_t)rel;
        w->qlast = at;
    }

    if (w->qdepth >= QMAX) {            /* QFull, the ObjAsm's negative */
        q_unlock();
        s->r[0] = (uint32_t)-1;
        return;
    }
    struct event e = { at, s->r[1], s->r[2], s->r[3], w->qseq++ };
    unsigned i = w->qdepth;
    while (i > 0 && (w->q[i - 1].time > e.time ||
                     (w->q[i - 1].time == e.time && w->q[i - 1].seq > e.seq)))
        i--;
    memmove(&w->q[i + 1], &w->q[i], (w->qdepth - i) * sizeof e);
    w->q[i] = e;
    w->qdepth++;
    w->scheduled++;
    q_unlock();
    s->r[0] = 0;                       /* success */
}

void ros_thunk_Sound_QRemove(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    /* Pulling events back early is part of the sound driver's interface.
     * This driver never does it, so there is never an event to hand
     * back. */
    (void)w;
    s->r[0] = (uint32_t)-1;
}

void ros_thunk_Sound_QFree(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    s->r[0] = (uint32_t)(QMAX - w->qdepth);
}

void ros_thunk_Sound_QSDispatch(struct ros_cpu *s)
{
    if (!ws_or_fail(s))
        return;
    ros_soundq_tick(s->r[0]);
}

void ros_thunk_Sound_QTempo(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t old = w->tempo_cs & 0xFFFF;
    if (s->r[0] != 0 && s->r[0] < 0x10000)
        w->tempo_cs = s->r[0];
    s->r[0] = old;
}

void ros_thunk_Sound_QBeat(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    int32_t r0 = (int32_t)s->r[0];
    if (r0 == 0) {
        s->r[0] = w->qbeat;            /* the beat, BASIC's BEAT */
        return;
    }
    if (r0 == -1) {
        s->r[0] = w->beat_count;       /* the bar length, BEATS */
        return;
    }
    q_lock();
    uint32_t old = w->beat_count;
    if (r0 > 0)
        w->beat_count = (uint32_t)r0 & 0xFFFF;  /* the counter goes on from where it is */
    else {
        w->beat_count = 0;             /* any other negative: off, and 0 */
        w->qbeat = 0;
    }
    q_unlock();
    s->r[0] = old;
}

/* ---- the module ----------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    void *block;
    if (xos_module_claim(sizeof(struct ws), &block))
        return ros_error(0x1A6, "Sound system not present");
    struct ws *w = block;
    memset(w, 0, sizeof *w);
    w->tempo_cs = DEFAULT_TEMPO;
    ros_st32(m->private_word, A(w));
    return NULL;
}

static void final_shutdown(struct ros_module *m)
{
    struct ws *w = workspace(m);
    if (!w)
        return;
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
}

/* ---- the star commands (from s/Sound2, its TokHelpSrc) ----------------------------- */

static os_error *xswi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static os_error *bad_parameter(void)
{
    return ros_error(0x20000, "Sound command parameters not recognised");
}

/* Reads a number with OS_ReadUnsigned in base 10. Bit 31 of R0 is set, so
 * a bad terminator gives an error. */
static os_error *read_number(uint32_t *at, uint32_t *value)
{
    uint32_t r[10] = { 10 | 0x80000000u, *at, 0 };
    os_error *e = xswi(XOS_ReadUnsigned, r);
    if (!e)
        *at = r[1], *value = r[2];
    return e;
}

/* *Tempo <n> calls Sound_QTempo. A value past &FFFF is quietly ignored,
 * as in Sound2. */
static os_error *cmd_tempo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t v;
    if (read_number(&tail, &v))
        return bad_parameter();
    if (!(v >> 16)) {
        uint32_t r[10] = { v };
        xswi(XSound_QTempo, r);
    }
    return NULL;
}

/* *QSound <chan> <amp> <pitch> <duration> <nTicks> calls Sound_QSchedule.
 * Its errors are cleared, as in Sound2. */
static os_error *cmd_qsound(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t c, a, p, d, t;
    if (read_number(&tail, &c))
        return bad_parameter();
    if (c - 1 >= 8)
        return ros_error(0x20001, "Channel number must be in the range 1-8");
    if (read_number(&tail, &a) || read_number(&tail, &p) || read_number(&tail, &d) ||
        read_number(&tail, &t))
        return bad_parameter();
    uint32_t r[10] = { t, 0, (c & 0xFFFF) | a << 16, (p & 0xFFFF) | d << 16 };
    xswi(XSound_QSchedule, r);
    return NULL;
}

static const struct ros_command commands[] = {
    { "Tempo", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *Tempo <n> (0 - &FFFF, default is &1000)",
      "*Tempo sets the system tempo.", cmd_tempo },
    { "QSound", ROS_CMD_INFO(5, 5, 0, 0), "Syntax: *QSound <chan> <amp> <pitch> <duration> <nTicks>",
      "*QSound queues a sound after the specified number of tempo ticks.", cmd_qsound },
    { 0 },
};

struct ros_module soundsched_module = {
    .title = "SoundScheduler",
    .help = "SoundScheduler\t1.34 (29-Sep-26) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .commands = commands,
    .swi_chunk = 0x401C0,
    .swi_thunks = ros_swi_thunks_SoundScheduler,
    .swi_names = ros_swi_names_SoundScheduler,
    .swi_prefix = "Sound",             /* so SWI names such as Sound_QInit are shared with the Sound module's */
};

__attribute__((constructor)) static void soundsched_swi_count(void)
{
    soundsched_module.swi_count = ros_swi_count_SoundScheduler;
}
