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
 * This file is a reimplementation of parts of RISC OS Open's Kernel
 * (Sources/Kernel). The Apache licence of RISC OS Open's source applies to it.
 */

/* osbyte.c: OS_Byte and OS_Word, and the timers that live in them.
 *
 * Both SWIs go through their vectors, ByteV and WordV, whose default owner
 * is the kernel (Kernel/s/PMF/osbyte, osword). Here the default owner is
 * this file. It knows the reasons that the runtime implements:
 *
 *   OS_Byte 0    the OS version: R1 = 6, or with R1 = 0 an error naming it
 *   OS_Byte 13   disable an event, 14 enable one: the event's semaphore
 *   OS_Byte 19   wait for the next VSync
 *   OS_Byte 143  a service call: R1 the service, R2 its parameter, both
 *                given back as the modules left them (Osbyte8F)
 *   OS_Byte 161  read CMOS, 162 write it: runtime/cmos.c, a file
 *   OS_Word 1/2  read / write the system clock, five bytes of centiseconds:
 *                the host's monotonic count, read live, plus what OS_Word 2
 *                set it to (an offset), so TIME = 0 starts it from 0
 *   OS_Word 3/4  read / write the interval timer, which raises Event 5 as
 *                it passes zero
 *   OS_Word 14   read the real-time clock: 0 a string, 1 BCD, 3 five bytes
 *                of centiseconds since 1900, from the host's clock
 *   OS_Word 15   set it: the clock is the host's, so this changes nothing
 *
 *   the VDU's reasons (OS_Byte 9, 10, 20, 25, 117, 134, 135, 160, 165, 218
 *                and OS_Word 10-13) are in runtime/vdu. It also flashes the
 *                cursor and the palette on each VSync
 *
 * Any other reason is offered round the modules as the kernel offers it,
 * as Service_UKByte or Service_UKWord. An OS_Byte that nobody claims gives
 * "Bad command". An OS_Word that nobody claims does nothing.
 *
 * The timers move on the centisecond tick (ticker.h): the interval timer
 * counts, and every second tick is a VSync. There is no display interrupt.
 * So, as the kernel does when a driver says "no VSync IRQ", the VSync is made
 * at 50 Hz. It raises Event 4 and wakes OS_Byte 19's waiters.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cmos.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/keyboard.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/ticker.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"

#define BYTEV 0x06u
#define WORDV 0x07u
#define SERVICE_UKBYTE 0x07u
#define SERVICE_UKWORD 0x08u
#define EVENT_VSYNC 4u
#define EVENT_INTERVAL_TIMER 5u
#define ERR_FX0 0xF7u
#define ERR_BAD_COMMAND 0xFEu
#define MOS_VER 6u
#define ESC_STATUS (ROS_ZEROPAGE + 0x104)     /* the kernel's ESC_Status */
#define ESC_CONDITION 0x40u

#define FIVE_BYTES 0xFFFFFFFFFFull

/* ---- the timers (lock held) ---------------------------------------------- */

static uint64_t clock_offset;           /* OS_Word 2's: the clock less the host's count */
static uint64_t interval_timer;         /* OS_Word 3/4 */
static unsigned half;                   /* ticks since the last VSync */
static int vsync_due;                   /* a VSync fell due in this batch */

static pthread_mutex_t vsync_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vsync_cv = PTHREAD_COND_INITIALIZER;
static uint32_t vsyncs;

static void event(uint32_t n)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = n;
    ros_event_generate(&s);
}

/* One centisecond: from the tick's work (ticker.c).  A VSync falls due
 * every second one, delivered once on a batch's last (ticker.h). */
void ros_timers_tick(int last)
{
    interval_timer = (interval_timer + 1) & FIVE_BYTES;
    if (interval_timer == 0)
        event(EVENT_INTERVAL_TIMER);    /* it passed zero */
    if (++half == 2) {
        half = 0;
        vsync_due = 1;
    }
    if (vsync_due && last) {
        vsync_due = 0;
        pthread_mutex_lock(&vsync_mu);
        vsyncs++;
        pthread_cond_broadcast(&vsync_cv);
        pthread_mutex_unlock(&vsync_mu);
        ros_vdu_vsync();                /* the cursor's and the palette's flashing */
        ros_display_vsync();            /* (rosgd.dirtyfb) the screen to the host */
        event(EVENT_VSYNC);
    }
}

uint32_t ros_vsync_count(void)
{
    pthread_mutex_lock(&vsync_mu);
    uint32_t n = vsyncs;
    pthread_mutex_unlock(&vsync_mu);
    return n;
}

/* OS_Byte 19: waits until the next VSync. It releases the lock and turns
 * interrupts on, because the VSync is background work and would never come
 * otherwise. */
static void wait_vsync(void)
{
    if (ros_ld32(ROS_ZP_IRQSEMA))
        return;                         /* not from background work: as the kernel */
    uint32_t n = ros_vsync_count();
    unsigned irq = ros_irq_suspend();
    unsigned d = ros_blocking_begin();
    pthread_mutex_lock(&vsync_mu);
    for (int tries = 0; vsyncs == n && tries < 10; tries++) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 50 * 1000 * 1000;              /* a missing ticker is not a hang */
        if (t.tv_nsec >= 1000000000) {
            t.tv_sec++;
            t.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&vsync_cv, &vsync_mu, &t);
    }
    pthread_mutex_unlock(&vsync_mu);
    ros_blocking_end(d);
    ros_irq_resume(irq);
}

/* ---- five-byte values ------------------------------------------------------ */

static uint64_t ld5(uint32_t a)
{
    uint64_t v = 0;
    for (int i = 4; i >= 0; i--)
        v = v << 8 | ros_ld8(a + (uint32_t)i);
    return v;
}

static void st5(uint32_t a, uint64_t v)
{
    for (int i = 0; i < 5; i++, v >>= 8)
        ros_st8(a + (uint32_t)i, (uint32_t)(v & 0xFF));
}

/* ---- the real-time clock: the host's ------------------------------------------ */

/* Centiseconds since 00:00:00 1 January 1900, UTC: RISC OS's five-byte time. */
static uint64_t realtime_cs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    const uint64_t epoch_1900 = 2208988800ull;      /* 1900 to 1970, seconds */
    return ((uint64_t)t.tv_sec + epoch_1900) * 100u + (uint64_t)t.tv_nsec / 10000000u;
}

static uint8_t bcd(int v)
{
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

static void realtime(uint32_t block)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    switch (ros_ld8(block)) {
    case 0: {
        /* The kernel formats "%w3,%dy %m3 %ce%yr.%24:%mi:%se" through the
         * Territory module; this is its UK English, in UTC, CR-terminated. */
        char s[32];
        strftime(s, sizeof s, "%a,%d %b %Y.%H:%M:%S", &tm);
        size_t n = strlen(s);
        memcpy(ros_ptr(block), s, n);
        ros_st8(block + (uint32_t)n, 13);
        break;
    }
    case 1:
        ros_st8(block + 0, bcd(tm.tm_year % 100));
        ros_st8(block + 1, bcd(tm.tm_mon + 1));
        ros_st8(block + 2, bcd(tm.tm_mday));
        ros_st8(block + 3, bcd(tm.tm_wday + 1));        /* 1 = Sunday */
        ros_st8(block + 4, bcd(tm.tm_hour));
        ros_st8(block + 5, bcd(tm.tm_min));
        ros_st8(block + 6, bcd(tm.tm_sec));
        break;
    case 3:
        st5(block, realtime_cs());
        break;
    default:
        break;                          /* 2 converts BCD to a string: not yet */
    }
}

/* ---- the defaults ------------------------------------------------------------ */

static void unknown(struct ros_cpu *s, uint32_t service, int is_byte)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = service;
    c.r[2] = s->r[0], c.r[3] = s->r[1], c.r[4] = s->r[2];
    ros_service_call(&c);
    if (c.r[1] == 0) {
        s->r[0] = c.r[2], s->r[1] = c.r[3], s->r[2] = c.r[4];
        return;
    }
    if (is_byte)
        ros_swi_fail(s, ros_error(ERR_BAD_COMMAND, "Bad command"));
}

/* An Escape acknowledged with its effects on silences the sound too
 * (Kernel s/PMF/osbyte's Osbyte7E): the queue emptied (Sound_QInit) and
 * channels 8 down to 1 given a note of amplitude &101 for a duration of
 * 1. A BASIC program's music stops with it (#90). If there is no sound
 * system, an error ends the loop. */
static void escape_sound(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XSound_QInit);
    for (uint32_t ch = 8; ch && !c.v; ch--) {
        ros_cpu_enter(&c);
        c.r[0] = 0x01010000u | ch, c.r[1] = 0x00010000u;
        ros_swi(&c, XSound_ControlPacked);
    }
}

static void default_byte(struct ros_cpu *s)
{
    if (ros_streams_byte(s) || ros_vdu_byte(s) || ros_keyboard_byte(s))
        return;                         /* 3, 129, 198, 199, 236; the VDU's; the keyboard's */
    switch (s->r[0]) {
    case 0:
        if (s->r[1])
            s->r[1] = MOS_VER;
        else
            /* The kernel's NewFX0Error is "$SystemName $VString (%dy %m3
             * %ce%yr)", using the ROM's build date. This is RISC OS 5.30's
             * string, as ROSGD behaves. The Task Manager's Info shows it
             * (Switcher: from the ninth character, "5.30 (15-May-24)"). */
            ros_swi_fail(s, ros_error(ERR_FX0, "RISC OS 5.30 (15 May 2024)"));
        return;
    case 13:
    case 14:
        s->r[1] = s->r[2] = ros_event_semaphore(s->r[1], s->r[0] == 14);
        return;
    case 19:
        wait_vsync();
        return;
    case 127: {                         /* EOF#: OS_Args 5 (Osbyte7F) */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 5, c.r[1] = s->r[1];
        ros_swi(&c, XOS_Args);
        if (c.v) {
            ros_swi_fail(s, ros_ptr(c.r[0]));
            return;
        }
        s->r[1] = c.r[2];
        return;
    }
    case 143: {                         /* issue a service call */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = s->r[1], c.r[2] = s->r[2];
        ros_service_call(&c);
        s->r[1] = c.r[1], s->r[2] = c.r[2];
        return;
    }
    case 161:                           /* read CMOS */
    case 162:                           /* write it */
        ros_cmos_byte(s);
        return;
    case 124:                           /* clear the Escape condition */
    case 125:                           /* set it */
    case 126: {                         /* acknowledge it: R1 &FF if there was one */
        uint32_t st = ros_ld8(ESC_STATUS);
        int was = (st & ESC_CONDITION) != 0;
        if (s->r[0] == 126 && was && ros_byte_var(0xE6) == 0) {
            ros_keyboard_flush_all();   /* acknowledged: the buffers flushed (ESCeffect 0) */
            escape_sound();
        }
        ros_env_escape(s->r[0] == 125);  /* DoOsbyte7D / 7C: the handler told */
        if (s->r[0] == 126)
            s->r[1] = was ? 0xFF : 0;
        return;
    }
    default:
        unknown(s, SERVICE_UKBYTE, 1);
        return;
    }
}

static void default_word(struct ros_cpu *s)
{
    uint32_t block = s->r[1];
    if (ros_vdu_word(s))                /* 9-13, 21 */
        return;
    switch (s->r[0]) {
    case 7: {                           /* SOUND (OsWord07): the 8 bytes, as
                                         * Sound_ControlPacked's R0, R1.
                                         * This is BASIC's SOUND without a time */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_ld32(block), c.r[1] = ros_ld32(block + 4);
        ros_swi(&c, XSound_ControlPacked);
        if (c.v)
            ros_swi_fail(s, ros_ptr(c.r[0]));
        return;
    }
    /* The clock reads live: the host's centiseconds, as the ticker
     * counts them (BASIC's TIME reads this every time), from where
     * OS_Word 2 last set it. */
    case 1: st5(block, ((uint64_t)ros_monotonic_cs() + clock_offset) & FIVE_BYTES); return;
    case 2: clock_offset = (ld5(block) - ros_monotonic_cs()) & FIVE_BYTES; return;
    case 3: st5(block, interval_timer); return;
    case 4: interval_timer = ld5(block); return;
    case 14: realtime(block); return;
    case 15: return;                    /* the clock is the host's */
    default: unknown(s, SERVICE_UKWORD, 0); return;
    }
}

/* The SWIs: the vector, then its default owner.  Registers pass straight
 * through, as the vector's claimants see them. */
static void through(struct ros_cpu *s, uint32_t vector, void (*fallback)(struct ros_cpu *))
{
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    s->v = 0;
    if (!ros_vector_call(vector, s))
        fallback(s);
    ros_svc_sp = outer_sp;
    s->r[10] = r10, s->r[11] = r11, s->r[12] = r12;
}

void ros_thunk_OS_Byte(struct ros_cpu *s)
{
    through(s, BYTEV, default_byte);
}

void ros_thunk_OS_Word(struct ros_cpu *s)
{
    through(s, WORDV, default_word);
}
