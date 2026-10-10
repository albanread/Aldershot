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
 * This file is a reimplementation in C of RISC OS Open's Screen Blanker
 * (Sources/Video/Render/ScrBlank: s.ScrBlank, hdr.ScrBlank).
 */

/* screenblanker.c implements the Screen Blanker as a native module.
 *
 * It blanks the screen after a time with no activity, and unblanks it on
 * activity. It follows the behaviour of RISC OS 5's module
 * (Video/Render/ScrBlank 2.34), which was checked against 5.30 on the farm.
 * It is new code written from that module's documentation (hdr/ScrBlank and
 * the source). It is not a translation of the original.
 *
 * These count as activity:
 *   - The mouse moving. The module sees this on a tick every 20 cs (it
 *     counts TickerV calls to make the tick).
 *   - A key going into the keyboard buffer (InsV).
 *   - A key going down (EventV).
 *   - A character being written (WrchV), when *BlankTime W asks for it.
 *
 * Blanking is PaletteV's BlankScreen (reason 6). The VDU drivers own that
 * vector, and they tell the display. Service_ScreenBlanking is issued first,
 * so that a screen saver can take over. Service_ScreenBlanked and
 * Service_ScreenRestored follow, from a callback.
 *
 * ScreenBlanker_Control (&43100) is typed (api/defs/screenblanker.toml).
 * *BlankTime prints from the module's Messages file.
 *
 * All the module's state is in the RMA, reached through its private word.
 * Nothing is in C statics (module.h). Times are in ticks of 20 cs, as the
 * original keeps them.
 */
#include <stdio.h>
#include <string.h>

#include "screenblanker.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"

#define TICK_CS        20u              /* TickDelta: centiseconds in a tick */
#define MIN_NORMAL     25u              /* ticks: at least 5 s before blanking */
#define MIN_FLASH      2u               /* ticks: at least 40 cs each way */
#define MAX_SECONDS    (1u << 18)       /* the limit for *BlankTime (about 3 days) */

enum { SCREEN_ON, SCREEN_BLANKED, SCREEN_STANDBY, SCREEN_EXTERNAL };

#define WRCHV              0x03u
#define PALETTEV           0x23u
#define PV_BLANKSCREEN     6u
#define EVENT_KEYBOARD     11u
#define KEYNO_LIDCLOSED    0x210u
#define MISC1CMOS          0xBCu
#define SERVICE_RESET      0x27u
#define SERVICE_BLANKED    0x7Au        /* Service_ScreenBlanked */
#define SERVICE_RESTORED   0x7Bu        /* Service_ScreenRestored */
#define SERVICE_BLANKING   0xA9u        /* Service_ScreenBlanking */

#define ERR_BAD_SWI        0x110u
#define ERR_SYNTAX         0xDCu

static const char messages_file[] = "Resources:$.Resources.ScrBlanker.Messages";

/* The time before blanking in centiseconds, indexed by Misc1CMOS bits 3-5. */
static const uint32_t cmos_times[8] = {
    0, 30 * 100, 60 * 100, 2 * 60 * 100, 5 * 60 * 100, 10 * 60 * 100, 15 * 60 * 100,
    30 * 60 * 100,
};

/* ---- the state, all in the RMA ------------------------------------------ */

struct workspace {
    uint32_t timer;             /* ticks until the screen changes, 0 for never */
    uint32_t time_on;           /* ticks it stays on: the blanking time, or the flash's on time */
    uint32_t time_off;          /* ticks it stays off when flashing, 0 if not flashing */
    uint32_t normal;            /* the time before blanking in ticks, 0 for never */
    uint32_t mouse;             /* the mouse when last looked at, as x | y << 16 */
    uint32_t screen;            /* SCREEN_* */
    uint32_t writec;            /* WrchV is claimed */
    uint32_t flash_flags;       /* Flash's R4, which is R0 for the service calls */
    uint32_t forced;            /* flash cycles before the user may stop the flash */
    uint32_t service;           /* the service that the pending callback issues */
    uint32_t callback;          /* a callback is pending */
    uint32_t cs;                /* centiseconds into the current tick */
    uint32_t messages;          /* the Messages file is open, in desc */
    uint32_t desc[4];           /* its MessageTrans descriptor */
    char file[44];              /* its name, which MessageTrans keeps */
    uint8_t word21[8];          /* OS_Word 21,4's block */
};

static struct workspace *ws(void)
{
    uint32_t pw = screenblanker_module.private_word, w = pw ? ros_ld32(pw) : 0;
    return w ? ros_ptr(w) : NULL;
}

/* Calls a SWI in its X form, with registers r[0..9]. */
static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* ---- blanking and unblanking --------------------------------------------- */

/* Calls PaletteV's BlankScreen. R0 is 1 to blank and 0 to unblank. */
static os_error *palette_blank(uint32_t blank)
{
    uint32_t r[10] = { blank, 0, 0, 0, PV_BLANKSCREEN, 0, 0, 0, 0, PALETTEV };
    return swi(OS_CallAVector, r);
}

/* Issues Service_ScreenBlanked or Service_ScreenRestored from a callback,
 * with the flags. It issues the latest one asked for, and only once. */
static void issue(void *arg)
{
    (void)arg;
    struct workspace *w = ws();
    if (!w || !w->callback)
        return;                         /* dropped: the module has died, or the callback is done */
    w->callback = 0;
    uint32_t r[10] = { w->flash_flags, w->service };
    swi(OS_ServiceCall, r);
}

static void announce(struct workspace *w, uint32_t service)
{
    w->service = service;
    if (!w->callback) {
        w->callback = 1;
        ros_callback_add_native(issue, NULL);
    }
}

/* TurnScreenOff. A normal blank issues Service_ScreenBlanking first. If
 * a screen saver claims it, the claimant blanks instead. Standby does not
 * issue the service. */
static void screen_off(struct workspace *w, uint32_t how)
{
    uint32_t state = how;
    if (how == SCREEN_BLANKED) {
        uint32_t r[10] = { how, SERVICE_BLANKING };
        if (swi(OS_ServiceCall, r))
            return;
        if (r[1] == 0)
            state = SCREEN_EXTERNAL;
    }
    if (state != SCREEN_EXTERNAL && palette_blank(1))
        return;
    w->screen = state;
    announce(w, SERVICE_BLANKED);
}

/* TurnScreenOn. It calls PaletteV, unless a claimant blanked the screen. */
static void screen_on(struct workspace *w)
{
    if (w->screen != SCREEN_EXTERNAL && palette_blank(0))
        return;
    w->screen = SCREEN_ON;
    announce(w, SERVICE_RESTORED);
}

/* CancelFlash. Returns 1 if the user may unblank. That is so when the screen
 * is not flashing. It is also so when the forced cycles of a flash are done,
 * and this ends the flash. */
static int cancel_flash(struct workspace *w)
{
    if (!w->time_off)
        return 1;
    if (w->forced)
        return 0;
    w->flash_flags = 0;
    w->time_off = 0;
    w->time_on = w->normal;
    return 1;
}

/* Handles activity. The time starts again and the screen comes on. */
static void activity(struct workspace *w)
{
    if (w->screen == SCREEN_STANDBY || !cancel_flash(w))
        return;
    w->timer = w->time_on;
    if (w->screen != SCREEN_ON)
        screen_on(w);
}

/* Returns the mouse's position from OS_Word 21,4 as one word. */
static uint32_t mouse(struct workspace *w)
{
    uint8_t *b = w->word21;
    b[0] = 4;
    uint32_t r[10] = { 21, ros_addr(b) };
    if (swi(OS_Word, r))
        return w->mouse;
    return (uint32_t)b[1] | (uint32_t)b[2] << 8 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24;
}

/* Runs every 20 cs. The mouse moving is activity. Otherwise the time counts
 * down. When the screen is on, it counts down to blanking. When the screen is
 * blanked, it counts down to the next time the flash turns the screen on. */
static void tick(struct workspace *w)
{
    if (w->screen == SCREEN_STANDBY)
        return;
    uint32_t m = mouse(w);
    int moved = m != w->mouse;
    if (moved)
        w->mouse = m;
    if (w->screen == SCREEN_ON) {
        if (moved && cancel_flash(w)) {
            w->timer = w->time_on;
            return;
        }
        if (!w->timer || --w->timer)
            return;
        if (w->forced)
            w->forced--;
        w->timer = w->time_off;
        screen_off(w, SCREEN_BLANKED);
        return;
    }
    if (!(moved && cancel_flash(w))) {
        if (!w->timer || --w->timer)
            return;
    }
    screen_on(w);
    w->timer = w->time_on;
}

/* ---- the vectors ---------------------------------------------------------- */

static int tickerv(struct ros_cpu *s, uint32_t r12)
{
    (void)s, (void)r12;
    struct workspace *w = ws();
    if (w && ++w->cs >= TICK_CS) {
        w->cs = 0;
        tick(w);
    }
    return ROS_VECTOR_PASS;
}

/* Called when a key goes into the keyboard buffer. */
static int insv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct workspace *w = ws();
    if (w && s->r[1] == 0)
        activity(w);
    return ROS_VECTOR_PASS;
}

/* Called when a key goes down, except for the lid closing. */
static int eventv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct workspace *w = ws();
    if (w && s->r[0] == EVENT_KEYBOARD && s->r[1] == 1 && s->r[2] != KEYNO_LIDCLOSED)
        activity(w);
    return ROS_VECTOR_PASS;
}

/* Called when a character is written, after *BlankTime W. */
static int wrchv(struct ros_cpu *s, uint32_t r12)
{
    (void)s, (void)r12;
    struct workspace *w = ws();
    if (w)
        activity(w);
    return ROS_VECTOR_PASS;
}

static void writec(struct workspace *w, int claim)
{
    if (claim == (int)w->writec)
        return;
    if (claim)
        ros_vector_claim_native(WRCHV, wrchv, 0);
    else
        ros_vector_release_native(WRCHV, wrchv, 0);
    w->writec = (uint32_t)claim;
}

/* ---- ScreenBlanker_Control ------------------------------------------------ */

static uint32_t ticks(uint32_t cs, uint32_t least)
{
    uint32_t t = cs / TICK_CS;
    return t < least ? least : t;
}

static os_error *blank(uint32_t how)
{
    struct workspace *w = ws();
    w->forced = w->flash_flags = w->time_off = w->timer = 0;
    w->time_on = w->normal;
    screen_off(w, how);
    return NULL;
}

os_error *xscreenblanker_blank(void)
{
    return blank(SCREEN_BLANKED);
}

os_error *xscreenblanker_strict_blank(void)
{
    return blank(SCREEN_STANDBY);
}

os_error *xscreenblanker_strict_unblank(void)
{
    struct workspace *w = ws();
    w->forced = w->flash_flags = w->time_off = 0;
    w->time_on = w->timer = w->normal;
    screen_on(w);
    return NULL;
}

os_error *xscreenblanker_unblank(void)
{
    return ws()->screen == SCREEN_STANDBY ? NULL : xscreenblanker_strict_unblank();
}

os_error *xscreenblanker_flash(uint32_t on_cs, uint32_t off_cs, uint32_t cycles, uint32_t flags)
{
    struct workspace *w = ws();
    if (w->screen == SCREEN_STANDBY)
        return NULL;
    w->forced = cycles;
    w->flash_flags = flags;
    screen_on(w);
    w->time_on = w->timer = ticks(on_cs, MIN_FLASH);
    w->time_off = ticks(off_cs, MIN_FLASH);
    return NULL;
}

os_error *xscreenblanker_set_timeout(uint32_t cs)
{
    ws()->normal = cs ? ticks(cs, MIN_NORMAL) : 0;
    return xscreenblanker_unblank();
}

os_error *xscreenblanker_read_timeout(uint32_t *seconds)
{
    *seconds = ws()->normal / (100 / TICK_CS);
    return NULL;
}

os_error *xscreenblanker_read_timeout2(uint32_t *cs)
{
    *cs = ws()->normal * TICK_CS;
    return NULL;
}

/* Reads the Misc1CMOS byte, giving 0 if it cannot be read. */
static uint32_t misc1(os_error **e)
{
    uint32_t r[10] = { 161, MISC1CMOS };
    *e = swi(OS_Byte, r);
    return *e ? 0 : r[2];
}

os_error *xscreenblanker_reread_timeout(void)
{
    os_error *e;
    uint32_t c = misc1(&e);
    return e ? e : xscreenblanker_set_timeout(cmos_times[c >> 3 & 7]);
}

/* ---- *BlankTime ------------------------------------------------------------ */

static os_error *open_messages(struct workspace *w)
{
    if (w->messages)
        return NULL;
    memcpy(w->file, messages_file, sizeof messages_file);
    uint32_t r[10] = { ros_addr(w->desc), ros_addr(w->file), 0 };
    os_error *e = swi(MessageTrans_OpenFile, r);
    if (!e)
        w->messages = 1;
    return e;
}

/* Prints the state: Off, or the seconds, and whether WriteC counts. */
static os_error *show(struct workspace *w)
{
    os_error *e = open_messages(w);
    if (e)
        return e;
    uint32_t cs = w->normal * TICK_CS;
    const char *token = !cs ? "Off" : w->writec ? "WTm" : "Tm";
    uint8_t *b = ros_rma_alloc(128);
    if (!b)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    strcpy((char *)b, token);
    snprintf((char *)b + 8, 20, "%u", cs / 100);
    uint32_t r[10] = { ros_addr(w->desc), ros_addr(b), ros_addr(b) + 32, 80, ros_addr(b) + 8 };
    e = swi(MessageTrans_Lookup, r);
    if (!e) {
        uint32_t p[10] = { r[2] };
        if (!(e = swi(OS_Write0, p)))
            e = xos_new_line();
    }
    ros_rma_free(b);
    return e;
}

static os_error *syntax(struct workspace *w)
{
    os_error *e = open_messages(w);
    if (e)
        return e;
    uint32_t *block = ros_rma_alloc(8);
    if (!block)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    block[0] = ERR_SYNTAX;
    memcpy(&block[1], "Err", 4);
    uint32_t r[10] = { ros_addr(block), ros_addr(w->desc) };
    e = swi(MessageTrans_ErrorLookup, r);
    ros_rma_free(block);
    return e;
}

/* Reads the parameters a character at a time: numbers (seconds), W, O and
 * dashes. Anything else is a syntax error. What came before it has been done. */
static os_error *cmd_blanktime(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct workspace *w = ws();
    if (!argc)
        return show(w);
    for (uint32_t p = tail;;) {
        uint32_t c = ros_ld8(p);
        if (c == ' ') {
            p++;
            continue;
        }
        if (c < ' ')
            return NULL;
        if (c >= '0' && c <= '9') {
            uint32_t r[10] = { 1u << 29, p, MAX_SECONDS };
            os_error *e = swi(OS_ReadUnsigned, r);
            if (e)
                return e;
            p = r[1];
            xscreenblanker_set_timeout(r[2] * 100);
            continue;
        }
        p++;
        c |= 0x20;
        if (c == 'w')
            writec(w, 1);
        else if (c == 'o')
            writec(w, 0);
        else if (c != '-')
            return syntax(w);
    }
}

/* ---- the module ---------------------------------------------------------- */

/* Sets the state from CMOS and claims the vectors (ClaimVectorsAndStuff). */
static os_error *start(struct workspace *w)
{
    os_error *e;
    uint32_t c = misc1(&e) >> 3;
    w->time_off = w->mouse = 0;
    w->screen = SCREEN_ON;
    w->flash_flags = w->forced = w->cs = 0;
    w->normal = w->time_on = w->timer = cmos_times[c & 7] / TICK_CS;
    if ((e = ros_vector_claim_native(ROS_TICKERV, tickerv, 0)) != NULL)
        return e;
    if (c & 8)
        writec(w, 1);
    if ((e = ros_vector_claim_native(ROS_EVENTV, eventv, 0)) != NULL ||
        (e = ros_vector_claim_native(ROS_INSV, insv, 0)) != NULL) {
        ros_vector_release_native(ROS_EVENTV, eventv, 0);
        ros_vector_release_native(ROS_TICKERV, tickerv, 0);
        writec(w, 0);
        return e;
    }
    uint32_t r[10] = { 14, EVENT_KEYBOARD };
    swi(OS_Byte, r);
    return NULL;
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    ros_st32(m->private_word, ros_addr(w));
    os_error *e = start(w);
    if (e) {
        ros_st32(m->private_word, 0);
        ros_rma_free(w);
    }
    return e;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    uint32_t r[10] = { 13, EVENT_KEYBOARD };
    swi(OS_Byte, r);
    writec(w, 0);
    ros_vector_release_native(ROS_INSV, insv, 0);
    ros_vector_release_native(ROS_EVENTV, eventv, 0);
    ros_vector_release_native(ROS_TICKERV, tickerv, 0);
    activity(w);                        /* turns the screen on if it was left blanked */
    w->callback = 0;
    if (w->messages) {
        uint32_t c[10] = { ros_addr(w->desc) };
        swi(MessageTrans_CloseFile, c);
    }
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

/* Handles Service_Reset. After a soft reset, it starts again. */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] != SERVICE_RESET || !ws())
        return;
    uint32_t r[10] = { 253, 0, 255 };
    if (!swi(OS_Byte, r) && r[1] == 0)
        start(ws());
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ERR_BAD_SWI, "SWI value out of range for module %s", m->title);
}

static const struct ros_command commands[] = {
    { "BlankTime", ROS_CMD_INFO(0, 2, 0, 0), "Syntax: *\x1B\x00 [W|O] [Time]",
      "*\x1B\x00 sets options or \x1B\x00 (seconds) for the Blanker.\r"
      "\t-W claims WriteCV.\r"
      "\t-O releases WriteCV.\r"
      "If used with no parameters, it displays the current status.\r"
      "To turn screen blanking off use *\x1B\x00 0", cmd_blanktime },
    { 0 },
};

struct ros_module screenblanker_module = {
    .title = "ScreenBlanker",
    .help = "ScreenBlanker\t2.34 (26 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x43100,
    .swi_thunks = ros_swi_thunks_ScreenBlanker,
    .swi_names = ros_swi_names_ScreenBlanker,
    .swi_prefix = "ScreenBlanker",
    .commands = commands,
};

/* The count is known only at run time (api_gen.c). The runtime reads it
 * when the module is added. */
__attribute__((constructor)) static void count(void)
{
    screenblanker_module.swi_count = ros_swi_count_ScreenBlanker;
}
