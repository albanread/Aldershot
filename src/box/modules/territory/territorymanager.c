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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/Internat/Territory/TerritoryManager: version 0.58, as in the 5.30
 * ROM, and hdr.Territory).
 */

/* territorymanager.c -- the Territory Manager, reimplemented.
 *
 * Written from RISC OS 5.30's Territory Manager 0.58.  That is ObjAsm,
 * read from the 5.30 ROM, because 5.31's sources hold its C rewrite, 0.59,
 * which has the same interface.  It is checked against 0.58 on the farm
 * (tests/desktop/territory).  README.md lists the SWIs and the differences
 * from 5.30.  In short:
 *
 *   - There is a list of territories, each a node in the RMA.  A node holds
 *     the next node, the number, the territory's private word, and its 26
 *     entry points.
 *   - The configured territory comes from CMOS &18 EOR 1.
 *   - The SWIs from ReadTimeZones to DaylightRules are passed to the
 *     territory as 0.58 passes them.  R0 -1 is made the configured number.
 *     The entry is entered with the caller's registers.  What it leaves in
 *     R0-R5 and the flags is given back.
 *   - Its own SWIs are register by register as 0.58's.  They are the names,
 *     the current zone, ordinals, DaylightSaving and ConvertTimeFormats.
 *   - Daylight saving is decided from the territory's rules when AutoDST
 *     is configured, with a ticker event at the next change.
 *   - There are *Territory, *Territories, and its *Configure keywords.
 *
 * Its SWIs are register-level here.  C clients use the typed functions that
 * api/gen.py writes from api/defs/territory.toml, which call them.
 */
#include <stdio.h>
#include <string.h>

#include "territory.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define SWI_COUNT 57u                   /* &43040 to &43078 */
#define NODE_SIZE (12u + 4u * TERRITORY_ENTRIES)
#define N_NEXT    0u
#define N_NUMBER  4u
#define N_PW      8u
#define N_ENTRIES 12u

#define ERR_MESSAGE_BLOCK 0x10u         /* where the manager builds error blocks */
#define ERR_SYNTAX        0xDCu

/* The workspace, in the RMA.  0.58's 208 bytes held the same things. */
struct workspace {
    uint32_t list;                      /* the first node, or 0 */
    int32_t configured;
    uint32_t messages_open;
    uint32_t messages[4];               /* MessageTrans' descriptor */
    char filename[28];                  /* its file's name, which it keeps a pointer to */
    char custom[20];                    /* "Custom": ReadCurrentTimeZone gives it out */
    char text[64];                      /* tokens and names, for the SWIs it calls */
    char param[32];                     /* a status line's number */
    uint8_t error[64];                  /* an error block with a token, to translate */
    int32_t zone;                       /* the zone the CMOS names, -1 if none */
    int32_t zone_offset;                /* the CMOS's offset, centiseconds */
    int32_t last_offset;                /* as last announced (Service_TimeZoneChanged) */
    int32_t last_bits;                  /* likewise the DST bits; -1 before the first */
    uint8_t bounds[16];                 /* this year's DST start (+0) and end (+8) */
    uint8_t clock[16];                  /* OS_Word 14,3 and 15,5 blocks */
    uint8_t convert[8];                 /* ConvertTimeFormats' time, for DaylightSaving */
    uint32_t ords[10];                  /* ordinals, for the SWIs it calls */
    uint32_t switchover;                /* the ticker routine's code address */
    uint32_t loaded;                    /* the callback's */
};

static struct workspace *ws(void)
{
    return ros_ptr(ros_ld32(territory_manager_module.private_word));
}

/* A SWI by number, the registers in and out, V as the result */
static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static uint32_t text(const char *s)
{
    struct workspace *w = ws();
    snprintf(w->text, sizeof w->text, "%s", s);
    return ros_addr(w->text);
}

static os_error *print(const char *s)
{
    os_error *e = NULL;
    for (; *s && !e; s++)
        e = xos_write_c((uint8_t)*s);
    return e;
}

/* ---- the Messages --------------------------------------------------------------- */

static os_error *messages(void)
{
    struct workspace *w = ws();
    if (w->messages_open)
        return NULL;
    strcpy(w->filename, "TerritoryManager:Messages");
    uint32_t r[10] = { ros_addr(w->messages), ros_addr(w->filename), 0 };
    os_error *e = swi(XMessageTrans_OpenFile, r);
    w->messages_open = e == NULL;
    return e;
}

/* A token's text into buffer (an arena address): 0, or -1 if it fails */
static int lookup(const char *token, uint32_t buffer, uint32_t size)
{
    if (messages())
        return -1;
    uint32_t r[10] = { ros_addr(ws()->messages), text(token), buffer, size };
    return swi(XMessageTrans_Lookup, r) ? -1 : 0;
}

/* An error by token, %0 the string param (arena) or 0 */
static os_error *error(uint32_t errnum, const char *token, uint32_t param)
{
    os_error *e = messages();
    if (e)
        return e;
    struct workspace *w = ws();
    ros_st32(ros_addr(w->error), errnum);
    snprintf((char *)w->error + 4, sizeof w->error - 4, "%s", token);
    uint32_t r[10] = { ros_addr(w->error), ros_addr(w->messages), 0, 0, param };
    return swi(XMessageTrans_ErrorLookup, r);
}

/* A token's message, "%0" replaced by param (arena, or 0), as 0.58 prints
 * its status and syntax lines: to a control character, no newline */
static os_error *print_message(const char *token, uint32_t param)
{
    os_error *e = messages();
    if (e)
        return e;
    uint32_t r[10] = { ros_addr(ws()->messages), text(token), 0, 0 };
    if ((e = swi(XMessageTrans_Lookup, r)) != NULL)
        return e;
    for (uint32_t p = r[2];;) {
        uint32_t c = ros_ld8(p++);
        if (c == '%') {
            c = ros_ld8(p++);
            if (c == '0') {
                for (uint32_t q = param; q && ros_ld8(q) != 0 && !e; q++)
                    e = xos_write_c((uint8_t)ros_ld8(q));
                continue;
            }
        }
        if (c < ' ' || e)
            return e;
        e = xos_write_c((uint8_t)c);
    }
}

/* ---- the list ------------------------------------------------------------------- */

static uint32_t find(int32_t number)
{
    for (uint32_t n = ws()->list; n; n = ros_ld32(n + N_NEXT))
        if ((int32_t)ros_ld32(n + N_NUMBER) == number)
            return n;
    return 0;
}

/* ---- the CMOS, and daylight saving ---------------------------------------------- */

static os_error *cmos_read(uint32_t address, uint32_t *value)
{
    uint32_t r[10] = { 161, address };
    os_error *e = swi(XOS_Byte, r);
    *value = e ? 0 : r[2] & 0xFF;
    return e;
}

static void cmos_write(uint32_t address, uint32_t value)
{
    uint32_t r[10] = { 162, address, value & 0xFF };
    swi(XOS_Byte, r);
}

/* 0.58's: the AutoDST bit (CMOS &10 bit 0) and the DST bit (&DC bit 7),
 * each new = old AND mask EOR eor, written if it changed; the old ones
 * back, as bits 0 and 7 */
static uint32_t modify_bits(uint32_t and, uint32_t eor)
{
    uint32_t dbtb, alarm;
    if (cmos_read(TERRITORY_CMOS_DBTB, &dbtb))
        return 0;
    uint32_t v = dbtb;
    if (!(and & 1))
        v &= ~1u;
    if (eor & 1)
        v ^= 1;
    if (v != dbtb)
        cmos_write(TERRITORY_CMOS_DBTB, v);
    if (cmos_read(TERRITORY_CMOS_ALARM, &alarm))
        return 0;
    v = alarm;
    if (!(and & 0x80))
        v &= ~0x80u;
    if (eor & 0x80)
        v ^= 0x80;
    if (v != alarm)
        cmos_write(TERRITORY_CMOS_ALARM, v);
    return (dbtb & 1) | (alarm & 0x80);
}

/* The zone the CMOS names in the current territory, -1 if none; its offset
 * (the CMOS's signed quarter hours) */
static int32_t which_zone(int32_t *offset)
{
    uint32_t q;
    *offset = 0;
    if (cmos_read(TERRITORY_CMOS_ZONE, &q))
        return -1;
    *offset = (int32_t)(int8_t)q * 90000;
    for (uint32_t z = 0;; z++) {
        uint32_t r[10] = { 0xFFFFFFFFu, z, 0, 0, TERRITORY_ZONE_EXTENSION };
        if (swi(XTerritory_ReadTimeZones, r))
            return -1;
        if ((int32_t)r[2] == *offset)
            return (int32_t)z;
        if (r[4] != 0)
            return -1;
    }
}

static int32_t lo_word(const uint8_t *t)
{
    return (int32_t)((uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 |
                     (uint32_t)t[3] << 24);
}

/* A five-byte time as a 40-bit value */
static uint64_t as_40(const uint8_t *t)
{
    return (uint64_t)t[4] << 32 | (uint32_t)lo_word(t);
}

/* A five-byte time as 0.58's DaylightSaving compares them: the fifth byte
 * sign-extended */
static uint64_t as_wide(const uint8_t *t)
{
    return (uint64_t)(int64_t)(int8_t)t[4] << 32 | (uint32_t)lo_word(t);
}

/* 0.58's check, made after anything that may change the zone or daylight
 * saving.  The ticker event is cancelled.  With AutoDST, the DST bit is set
 * by this year's rule and the event is booked for the next change.  Then,
 * if the zone's offset or the bits changed, Service_TimeZoneChanged is
 * issued */
static os_error *switch_check(void)
{
    struct workspace *w = ws();
    os_error *e = NULL;
    xos_remove_ticker_event(w->switchover, ros_addr(w));
    w->zone = which_zone(&w->zone_offset);
    uint32_t bits = modify_bits(0x81, 0);
    if (bits & 1) {
        uint8_t *now = w->clock;
        now[0] = 3;
        uint32_t r[10] = { 14, ros_addr(now) };
        e = swi(XOS_Word, r);
        if (!e) {
            uint32_t f[10] = { 14, ros_addr(now), ros_addr(w->ords), 0x203 };
            e = swi(XTerritory_ConvertTimeFormats, f);
        }
        if (!e) {
            int32_t year = (int32_t)w->ords[6];
            uint32_t delta = 0, dst = 0;
            uint32_t b[10] = { 0xFFFFFFFFu, 5, (uint32_t)w->zone, (uint32_t)year,
                               ros_addr(w->bounds), ros_addr(w->bounds + 8) };
            int ruled = w->zone != -1 && !swi(XTerritory_DaylightSaving, b);
            if (ruled) {
                /* the start and end as 40-bit values; the end first in
                 * the southern hemisphere */
                const uint8_t *earlier = w->bounds, *later = w->bounds + 8;
                uint32_t north = 0x80;
                if (as_40(later) < as_40(earlier)) {
                    const uint8_t *x = earlier;
                    earlier = later, later = x, north = 0;
                }
                uint64_t n = as_40(now), a = as_40(earlier), z = as_40(later);
                if (z < n) {
                    dst = north ^ 0x80, ruled = 0;              /* past both: next year */
                } else if (a < n) {
                    dst = north;
                    delta = (uint32_t)lo_word(later) - (uint32_t)lo_word(now);
                } else {
                    dst = north ^ 0x80;
                    delta = (uint32_t)lo_word(earlier) - (uint32_t)lo_word(now);
                }
            } else {
                memset(w->bounds, 0xFF, 4);
                memset(w->bounds + 8, 0xFF, 4);
            }
            modify_bits(1, dst);
            if (!ruled) {
                /* 00:00 on 1 January next year, UTC */
                uint32_t *o = w->ords;
                o[0] = o[1] = o[2] = o[3] = 0, o[4] = o[5] = 1, o[6] = (uint32_t)year + 1;
                uint32_t t[10] = { 0, ros_addr(o), ros_addr(o + 7), 0x302 };
                e = swi(XTerritory_ConvertTimeFormats, t);
                delta = (uint32_t)lo_word((const uint8_t *)(o + 7)) - (uint32_t)lo_word(now);
            }
            if (!e)
                e = xos_call_after(delta, w->switchover, ros_addr(w));
        }
    }
    bits = modify_bits(0x81, 0);
    if ((int32_t)bits != w->last_bits || w->zone_offset != w->last_offset) {
        w->last_bits = (int32_t)bits;
        w->last_offset = w->zone_offset;
        uint32_t r[10] = { 0, TERRITORY_SERVICE_ZONE_CHANGED,
                           (bits & 0x80 ? 1u : 0) | (bits & 1 ? 2u : 0), (uint32_t)w->zone_offset };
        swi(XOS_ServiceCall, r);
    }
    return e;
}

/* The ticker event: the next change is due */
static void switchover(struct ros_cpu *s)
{
    switch_check();
    s->r[15] = s->r[14];
}

/* A territory is selected: the check, and Service_TerritoryStarted */
static void selected(void)
{
    switch_check();
    uint32_t r[10] = { 0, TERRITORY_SERVICE_STARTED };
    swi(XOS_ServiceCall, r);
}

/* The callback after starting: territory modules register now */
static void loaded(struct ros_cpu *s)
{
    uint32_t r[10] = { 0, TERRITORY_SERVICE_MANAGER_LOADED };
    swi(XOS_ServiceCall, r);
    s->r[15] = s->r[14];
}

/* ---- the SWIs ------------------------------------------------------------------- */

typedef os_error *swi_fn(struct ros_cpu *s);

/* A SWI on the caller's registers, on the SVC stack below its frames
 * (cpu.h); its error into R0 and V */
static void run(struct ros_cpu *s, swi_fn *fn)
{
    uint32_t outer = ros_svc_sp_enter(s);
    os_error *e = fn(s);
    ros_svc_sp = outer;
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

static void flags_clear(struct ros_cpu *s)
{
    s->n = s->z = s->c = 0;
}

static os_error *number(struct ros_cpu *s)
{
    s->r[0] = (uint32_t)ws()->configured;
    flags_clear(s);
    return NULL;
}

static os_error *reg(struct ros_cpu *s)
{
    struct workspace *w = ws();
    int32_t t = (int32_t)s->r[0];
    flags_clear(s);
    if (find(t))
        return NULL;
    uint8_t *n = ros_rma_alloc(NODE_SIZE);
    if (!n)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    uint32_t a = ros_addr(n);
    ros_st32(a + N_NUMBER, (uint32_t)t);
    ros_st32(a + N_PW, s->r[2]);
    memcpy(n + N_ENTRIES, ros_ptr(s->r[1]), 4 * TERRITORY_ENTRIES);
    ros_st32(a + N_NEXT, w->list);
    w->list = a;
    if (t == w->configured)
        selected();
    return NULL;
}

static os_error *dereg(struct ros_cpu *s)
{
    struct workspace *w = ws();
    flags_clear(s);
    for (uint32_t *link = &w->list, n; (n = *link) != 0; link = ros_ptr(n + N_NEXT))
        if ((int32_t)ros_ld32(n + N_NUMBER) == (int32_t)s->r[0]) {
            *link = ros_ld32(n + N_NEXT);
            ros_rma_free(ros_ptr(n));
            break;
        }
    return NULL;
}

/* NumberToName and AlphabetNumberToName: token letter and the number as
 * unsigned decimal, converted (as 0.58 does) in a buffer of the caller's
 * size; then the name into the caller's buffer */
static os_error *name(struct ros_cpu *s, char letter, uint32_t errnum, const char *errtoken)
{
    char token[16];
    int n = snprintf(token, sizeof token, "%c%u", letter, s->r[0]);
    if ((uint32_t)(n - 1) + 1 > s->r[2] || lookup(token, s->r[1], s->r[2]))
        return error(errnum, errtoken, 0);
    s->n = 1, s->z = s->c = 0;
    return NULL;
}

static os_error *number_to_name(struct ros_cpu *s)
{
    return name(s, 'T', TERRITORY_ERR_UNKNOWN_TERRITORY, "UnkTerr");
}

static os_error *alphabet_number_to_name(struct ros_cpu *s)
{
    return name(s, 'A', TERRITORY_ERR_UNKNOWN_ALPHABET, "UnkAlph");
}

static os_error *exists(struct ros_cpu *s)
{
    if (find((int32_t)s->r[0]))
        s->n = 0, s->z = 1, s->c = 1;
    else
        s->n = 1, s->z = 0, s->c = 0;
    return NULL;
}

static os_error *select_alphabet(struct ros_cpu *s)
{
    uint32_t r[10] = { s->r[0] };
    os_error *e = swi(XTerritory_Alphabet, r);
    if (e)
        return e;
    uint32_t v[10] = { 0, TERRITORY_SERVICE_INTERNATIONAL, 5, r[0], 0, 255 };
    swi(XOS_ServiceCall, v);
    flags_clear(s);
    return NULL;
}

static os_error *set_time(struct ros_cpu *s)
{
    struct workspace *w = ws();
    w->clock[0] = 5;
    memcpy(w->clock + 1, ros_ptr(s->r[0]), 5);
    uint32_t r[10] = { 15, ros_addr(w->clock) };
    os_error *e = swi(XOS_Word, r);
    return e ? e : switch_check();
}

static os_error *read_current_time_zone(struct ros_cpu *s)
{
    struct workspace *w = ws();
    int32_t offset;
    int32_t zone = which_zone(&offset);
    if (s->r[2] == TERRITORY_ZONE_EXTENSION)
        s->r[2] = (uint32_t)zone;
    uint32_t alarm;
    cmos_read(TERRITORY_CMOS_ALARM, &alarm);
    if (zone == -1) {
        s->r[0] = ros_addr(w->custom);
        s->r[1] = (uint32_t)(offset + (alarm & 0x80 ? TERRITORY_CS_HOUR : 0));
    } else {
        uint32_t r[10] = { 0xFFFFFFFFu, (uint32_t)zone, 0, 0, TERRITORY_ZONE_EXTENSION };
        os_error *e = swi(XTerritory_ReadTimeZones, r);
        if (e)
            return e;
        s->r[0] = r[0];
        s->r[1] = (uint32_t)offset;
        if (alarm & 0x80)
            s->r[0] = r[1], s->r[1] = r[3];
    }
    flags_clear(s);
    return NULL;
}

/* Nine ordinals from a time broken down */
static void store_ordinals(uint32_t out, const struct territory_time *v)
{
    uint32_t o[9] = { v->cs, v->secs, v->mins, v->hours, v->dom, v->month,
                      v->yearhi * 100 + v->yearlo, v->dow, v->doy };
    for (int i = 0; i < 9; i++)
        ros_st32(out + 4u * (uint32_t)i, o[i]);
}

static os_error *time_to_utc_ordinals(struct ros_cpu *s)
{
    struct territory_time v;
    territory_time_values(ros_ptr(s->r[1]), 0, &v);
    store_ordinals(s->r[2], &v);
    return NULL;
}

static os_error *convert_text_to_string(struct ros_cpu *s)
{
    (void)s;
    return NULL;
}

static os_error *select_territory(struct ros_cpu *s)
{
    if (!find((int32_t)s->r[0]))
        return error(TERRITORY_ERR_NO_TERRITORY, "NoTerr", 0);
    ws()->configured = (int32_t)s->r[0];
    selected();
    return NULL;
}

/* ---- DaylightSaving and ConvertTimeFormats -------------------------------------- */

static int configured_or_current(int32_t t)
{
    return t == -1 || t == ws()->configured;
}

/* Reasons 3 and 4: whether DST is in force at the five-byte time t, by the
 * rules of territory's zone for year (0.58: inclusive at both bounds) */
static os_error *apply_rule(struct ros_cpu *s, const uint8_t t[5], int32_t year)
{
    uint8_t se[16];
    memset(se, 0, sizeof se);
    struct workspace *w = ws();
    uint32_t r[10] = { s->r[0], 1, s->r[2], (uint32_t)year, ros_addr(w->bounds + 8),
                       ros_addr(w->bounds) };
    if (swi(XTerritory_DaylightRules, r)) {
        s->r[0] = 0;
        s->n = s->z = 0, s->c = 1;
        return NULL;
    }
    memcpy(se, w->bounds, 16);                      /* end at +0, start at +8 */
    uint64_t end = as_wide(se), start = as_wide(se + 8), time = as_wide(t);
    uint64_t earlier = start, later = end;
    uint32_t south = 0;
    if (end < start)
        earlier = end, later = start, south = 1;
    uint32_t in = time >= earlier && later >= time;
    s->r[0] = in ^ south;
    s->n = s->z = 0, s->c = 1;
    return NULL;
}

static os_error *daylight_saving(struct ros_cpu *s)
{
    int32_t t = (int32_t)s->r[0];
    switch (s->r[1]) {
    case 0: {
        if (!configured_or_current(t))
            return error(TERRITORY_ERR_UNKNOWN_TERRITORY, "UnkTerr", 0);
        uint32_t bits = modify_bits(0x81, 0);
        s->r[0] = (bits & 0x80 ? 1u : 0) | (bits & 1 ? 2u : 0);
        return NULL;
    }
    case 1: {
        if (!configured_or_current(t))
            return error(TERRITORY_ERR_UNKNOWN_TERRITORY, "UnkTerr", 0);
        if (s->r[2] & 2)
            modify_bits(0x80, 1);
        else
            modify_bits(0, s->r[2] & 1 ? 0x80 : 0);
        switch_check();
        return NULL;
    }
    case 2: {
        uint32_t r[10] = { s->r[0], 0 };
        s->r[0] = swi(XTerritory_DaylightRules, r) ? 0 : r[0];
        return NULL;
    }
    case 3: {
        uint8_t time[5];
        uint32_t o[7];
        memcpy(o, ros_ptr(s->r[3]), sizeof o);
        if (territory_ordinals_to_time(o, time))
            memset(time, 0, sizeof time);           /* 0.58 goes on with what it has */
        return apply_rule(s, time, (int32_t)o[6]);
    }
    case 4: {
        uint8_t time[5];
        memcpy(time, ros_ptr(s->r[3]), 5);
        struct territory_time v;
        territory_time_values(time, 0, &v);
        return apply_rule(s, time, (int32_t)(v.yearhi * 100 + v.yearlo));
    }
    case 5: {
        int32_t year = (int32_t)s->r[3];
        if (year == -1) {
            struct workspace *w = ws();
            w->clock[0] = 3;
            uint32_t r[10] = { 14, ros_addr(w->clock) };
            os_error *e = swi(XOS_Word, r);
            if (e)
                return e;
            struct territory_time v;
            territory_time_values(w->clock, 0, &v);
            year = (int32_t)(v.yearhi * 100 + v.yearlo);
        }
        uint32_t r[10] = { s->r[0], 1, s->r[2], (uint32_t)year, s->r[4], s->r[5] };
        if (swi(XTerritory_DaylightRules, r))
            return error(TERRITORY_ERR_NO_RULE, "NoRule", 0);
        s->r[2] = r[2], s->r[4] = r[4], s->r[5] = r[5];
        return NULL;
    }
    default:
        return error(TERRITORY_ERR_OUT_OF_RANGE, "UnkSub", 0);
    }
}

static os_error *convert_time_formats(struct ros_cpu *s)
{
    uint32_t flags = s->r[3];
    if (flags >> 20 || flags & 0xFC || flags & 0xFC00)
        return error(TERRITORY_ERR_OUT_OF_RANGE, "UnkSub", 0);
    uint32_t in = flags & 3, out = flags >> 8 & 3;
    uint8_t t[5];
    if (!(in & 1)) {
        uint32_t o[7];
        memcpy(o, ros_ptr(s->r[1]), sizeof o);
        if (territory_ordinals_to_time(o, t))
            return error(TERRITORY_ERR_BAD_TIME_BLOCK, "BadTime", 0);
    } else {
        memcpy(t, ros_ptr(s->r[1]), 5);
    }
    int64_t time = (int64_t)as_wide(t);

    /* local to UTC or UTC to local: an offset; else as it is */
    if ((in >> 1) != (out >> 1)) {
        int32_t offset;
        if (flags & 1u << 19) {
            if (flags & (3u << 17))
                return error(TERRITORY_ERR_OUT_OF_RANGE, "UnkSub", 0);
            offset = (int32_t)s->r[4] + (flags & 1u << 16 ? TERRITORY_CS_HOUR : 0);
        } else {
            if (flags & 1u << 18) {
                uint32_t r[10] = { s->r[0], 4, s->r[4], 0 };
                uint8_t *w = ws()->convert;
                for (int i = 0; i < 5; i++)
                    w[i] = (uint8_t)(time >> (8 * i));
                r[3] = ros_addr(w);
                os_error *e = swi(XTerritory_DaylightSaving, r);
                if (e && !(flags & 1u << 17))
                    return e;
                if (e)
                    r[0] = 0;
                flags = r[0] & 1 ? flags | 1u << 16 : flags & ~(1u << 16);
            }
            uint32_t r[10] = { s->r[0], s->r[4], 0, 0, TERRITORY_ZONE_EXTENSION };
            os_error *e = swi(XTerritory_ReadTimeZones, r);
            if (e)
                return e;
            offset = (int32_t)(flags & 1u << 16 ? r[3] : r[2]);
        }
        if (in & 2)
            offset = -offset;
        time -= offset;
    }
    for (int i = 0; i < 5; i++)
        t[i] = (uint8_t)(time >> (8 * i));
    if (out & 1) {
        memcpy(ros_ptr(s->r[2]), t, 5);
    } else {
        struct territory_time v;
        territory_time_values(t, 0, &v);
        store_ordinals(s->r[2], &v);
    }
    s->n = s->z = 0, s->c = 1;
    return NULL;
}

/* ---- the SWIs a territory answers ----------------------------------------------- */

/* Entry i of the territory in R0 (-1 the configured one), on the caller's
 * registers; R0-R5 and the flags back, as 5.31's manager gives R0-R5 back
 * (0.58 gave all, which differs only for an entry that corrupts R6-R10) */
static os_error *forward(struct ros_cpu *s, uint32_t i)
{
    struct workspace *w = ws();
    int32_t t = (int32_t)s->r[0];
    if (t == -1)
        t = w->configured;
    uint32_t n = find(t);
    if (!n)
        return error(TERRITORY_ERR_NO_TERRITORY, "NoTerr", 0);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, s->r, 11 * sizeof c.r[0]);
    c.r[0] = (uint32_t)t;
    c.r[11] = ros_ld32(n + N_ENTRIES + 4 * i);
    c.r[12] = ros_ld32(n + N_PW);
    c.z = c.c = 1;                      /* as 0.58's search of its list leaves them */
    ros_call(&c, c.r[11]);
    if (c.r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(&c, ROS_RETURN_TO_NATIVE);
    memcpy(s->r, c.r, 6 * sizeof c.r[0]);
    s->n = c.n, s->z = c.z, s->c = c.c;
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

#define FORWARD(i)                                                          \
    static void forward_##i(struct ros_cpu *s)                              \
    {                                                                       \
        uint32_t outer = ros_svc_sp_enter(s);                               \
        os_error *e = forward(s, i);                                        \
        ros_svc_sp = outer;                                                 \
        if (e)                                                              \
            ros_swi_fail(s, e);                                             \
        else                                                                \
            s->v = 0;                                                       \
    }
FORWARD(0) FORWARD(1) FORWARD(2) FORWARD(3) FORWARD(4) FORWARD(5) FORWARD(6)
FORWARD(7) FORWARD(8) FORWARD(9) FORWARD(10) FORWARD(11) FORWARD(12) FORWARD(13)
FORWARD(14) FORWARD(15) FORWARD(16) FORWARD(17) FORWARD(18) FORWARD(19) FORWARD(20)
FORWARD(21) FORWARD(22) FORWARD(23) FORWARD(24) FORWARD(25)

#define THUNK(fn)                                                           \
    static void thunk_##fn(struct ros_cpu *s) { run(s, fn); }
THUNK(number) THUNK(reg) THUNK(dereg) THUNK(number_to_name) THUNK(exists)
THUNK(alphabet_number_to_name) THUNK(select_alphabet) THUNK(set_time)
THUNK(read_current_time_zone) THUNK(time_to_utc_ordinals) THUNK(convert_text_to_string)
THUNK(select_territory) THUNK(daylight_saving) THUNK(convert_time_formats)

static ros_swi_thunk *const swis[SWI_COUNT] = {
    thunk_number, thunk_reg, thunk_dereg, thunk_number_to_name, thunk_exists,
    thunk_alphabet_number_to_name, thunk_select_alphabet, thunk_set_time,
    thunk_read_current_time_zone, thunk_time_to_utc_ordinals,
    forward_0, forward_1, forward_2, forward_3, forward_4, forward_5, forward_6, forward_7,
    forward_8, forward_9, forward_10, forward_11, forward_12, forward_13, forward_14,
    forward_15, forward_16, forward_17, forward_18, forward_19, forward_20, forward_21,
    forward_22, forward_23, forward_24, forward_25,
    [0x35] = thunk_convert_text_to_string, thunk_select_territory, thunk_daylight_saving,
    thunk_convert_time_formats,
};

static const char *const swi_names[SWI_COUNT] = {
    "Number", "Register", "Deregister", "NumberToName", "Exists", "AlphabetNumberToName",
    "SelectAlphabet", "SetTime", "ReadCurrentTimeZone", "ConvertTimeToUTCOrdinals",
    "ReadTimeZones", "ConvertDateAndTime", "ConvertStandardDateAndTime", "ConvertStandardDate",
    "ConvertStandardTime", "ConvertTimeToOrdinals", "ConvertTimeStringToOrdinals",
    "ConvertOrdinalsToTime", "Alphabet", "AlphabetIdentifier", "SelectKeyboardHandler",
    "WriteDirection", "CharacterPropertyTable", "LowerCaseTable", "UpperCaseTable",
    "ControlTable", "PlainTable", "ValueTable", "RepresentationTable", "Collate", "ReadSymbols",
    "ReadCalendarInformation", "NameToNumber", "TransformString", "IME", "DaylightRules",
    "Reserved1", "Reserved2", "Reserved3", "Reserved4", "Reserved5", "Reserved6", "Reserved7",
    "Reserved8", "Reserved9", "Reserved10", "Reserved11", "Reserved12", "Reserved13",
    "Reserved14", "Reserved15", "Reserved16", "Reserved17", "ConvertTextToString", "Select",
    "DaylightSaving", "ConvertTimeFormats",
};

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    os_error *e = messages();
    if (e)
        return e;
    struct workspace *w = ws();
    ros_st32(ros_addr(w->error), TERRITORY_ERR_OUT_OF_RANGE);
    strcpy((char *)w->error + 4, "BadSWI");
    strcpy((char *)w->error + 32, "TerritoryManager");
    uint32_t r[10] = { ros_addr(w->error), ros_addr(w->messages), 0, 0,
                       ros_addr(w->error + 32) };
    return swi(XMessageTrans_ErrorLookup, r);
}

/* ---- *commands and *Configure keywords ------------------------------------------ */

/* A name or a decimal number 0-255, alone: the territory it names */
static os_error *parse_territory(uint32_t tail, uint32_t *out)
{
    uint32_t r[10] = { 0xFFFFFFFFu, tail };
    os_error *e = swi(XTerritory_NameToNumber, r);
    if (e)
        return e;
    if (r[0]) {
        *out = r[0];
        return NULL;
    }
    uint32_t u[10] = { 0xC000000Au, tail };
    if ((e = swi(XOS_ReadUnsigned, u)) != NULL)
        return e;
    uint32_t p = u[1];
    while (ros_ld8(p) == ' ')
        p++;
    if (ros_ld8(p) > ' ')
        return ros_configure_error(3);
    *out = u[2];
    return NULL;
}

/* Cardinal conversion into the workspace's text, as 0.58 converts with
 * OS_ConvertCardinal1 or 4 */
static uint32_t cardinal(uint32_t swi_number, uint32_t value)
{
    struct workspace *w = ws();
    uint32_t r[10] = { value, ros_addr(w->param), sizeof w->param };
    if (swi(swi_number, r))
        w->param[0] = 0;
    return ros_addr(w->param);
}

static os_error *cmd_territory(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct workspace *w = ws();
    if (argc == 0) {
        uint32_t r[10] = { (uint32_t)w->configured, ros_addr(w->text), 32 };
        const char *name = w->text;
        if (swi(XTerritory_NumberToName, r))
            name = ros_ptr(cardinal(XOS_ConvertCardinal1, (uint32_t)w->configured));
        os_error *e = print(name);
        return e ? e : xos_new_line();
    }
    uint32_t t;
    os_error *e = parse_territory(tail, &t);
    if (e)
        return e;
    uint32_t r[10] = { t };
    return swi(XTerritory_Select, r);
}

static os_error *cmd_territories(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    struct workspace *w = ws();
    os_error *e = NULL;
    for (uint32_t n = w->list; n && !e; n = ros_ld32(n + N_NEXT)) {
        uint32_t t = ros_ld32(n + N_NUMBER);
        e = print((const char *)ros_ptr(cardinal(XOS_ConvertCardinal4, t)));
        if (!e)
            e = xos_write_c(' ');
        uint32_t r[10] = { t, ros_addr(w->text), 32 };
        if (!e)
            e = swi(XTerritory_NumberToName, r) ? print_message("Unused", 0) : print(w->text);
        if (!e)
            e = xos_new_line();
    }
    return e;
}

/* *Configure Territory: 0 its syntax, 1 its status, else the tail */
static os_error *cfg_territory(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    os_error *e;
    if (tail == 0) {
        e = print_message("CTSynt", 0);
        return e ? e : xos_new_line();
    }
    if (tail == 1) {
        uint32_t v;
        if ((e = cmos_read(TERRITORY_CMOS, &v)) != NULL)
            return e;
        e = print_message("CTStat", cardinal(XOS_ConvertCardinal1, v ^ 1));
        return e ? e : xos_new_line();
    }
    uint32_t t;
    if ((e = parse_territory(tail, &t)) != NULL)
        return e;
    cmos_write(TERRITORY_CMOS, t ^ 1);
    return NULL;
}

static os_error *cfg_dst(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    os_error *e;
    if (tail == 0) {
        e = print_message("CDSTSyn", 0);
        return e ? e : xos_new_line();
    }
    if (tail == 1) {
        uint32_t bits = modify_bits(0x81, 0);
        e = print(bits & 1 ? "AutoDST" : bits & 0x80 ? "DST" : "NoDST");
        return e ? e : xos_new_line();
    }
    modify_bits(0, 0x80);
    return switch_check();
}

static os_error *cfg_nodst(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    if (tail <= 1)
        return NULL;
    modify_bits(0, 0);
    return switch_check();
}

static os_error *cfg_autodst(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    if (tail <= 1)
        return NULL;
    modify_bits(0x80, 1);
    return switch_check();
}

/* *Configure TimeZone [+|-]<hours>[:<minutes>], stored as quarter hours
 * rounded as 0.58 rounds them: (minutes + 3) / 16 */
static os_error *cfg_timezone(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    os_error *e;
    struct workspace *w = ws();
    if (tail == 0) {
        e = print_message("TZSynt", 0);
        return e ? e : xos_new_line();
    }
    if (tail == 1) {
        uint32_t q;
        if ((e = cmos_read(TERRITORY_CMOS_ZONE, &q)) != NULL)
            return e;
        char sign = '+';
        if (q & 0x80)
            q = 256 - q, sign = '-';
        char h[8], mm[8];
        snprintf(h, sizeof h, "%s", (char *)ros_ptr(cardinal(XOS_ConvertCardinal1, q >> 2)));
        uint32_t m15 = cardinal(XOS_ConvertCardinal1, (q & 3) * 15);
        snprintf(mm, sizeof mm, "%s", (char *)ros_ptr(m15));
        snprintf(w->param, sizeof w->param, "%c%s:%s", sign, h, mm);
        e = print_message("TZStat", ros_addr(w->param));
        return e ? e : xos_new_line();
    }
    uint32_t c = ros_ld8(tail), p = tail;
    int negative = c == '-';
    if (c == '-' || c == '+')
        p++;
    uint32_t r[10] = { 0x2000000Au, p, 13 };
    if ((e = swi(XOS_ReadUnsigned, r)) != NULL)
        return e;
    uint32_t quarters = r[2] << 2;
    if (ros_ld8(r[1]) == ':') {
        uint32_t u[10] = { 0xA000000Au, r[1] + 1, 59 };
        if ((e = swi(XOS_ReadUnsigned, u)) != NULL)
            return e;
        quarters += (u[2] + 3) >> 4;
    }
    cmos_write(TERRITORY_CMOS_ZONE, negative ? 0u - quarters : quarters);
    return switch_check();
}

/* A zone name, as Service_UKConfig offers *Configure's unknown keywords:
 * the name to its end at a space or control character, case ignored */
static int same_zone(uint32_t name, uint32_t arg)
{
    for (;; name++, arg++) {
        uint32_t u = ros_ld8(arg), n = ros_ld8(name);
        if (u <= ' ')
            u = 0;
        if ((n ^ u) & ~0x20u)
            return 0;
        if (u == 0)
            return 1;
    }
}

static int configure_zone(uint32_t arg)
{
    uint32_t dst = 0;
    int32_t offset = 0;
    for (uint32_t z = 0;; z++) {
        uint32_t r[10] = { 0xFFFFFFFFu, z, 0, 0, TERRITORY_ZONE_EXTENSION };
        if (swi(XTerritory_ReadTimeZones, r))
            return 0;
        offset = (int32_t)r[2];
        if (same_zone(r[0], arg))
            break;
        if (same_zone(r[1], arg)) {
            dst = 0x80;
            break;
        }
        if (r[4] != 0)
            return 0;
    }
    modify_bits(0, dst);
    int32_t q = -60;
    while (q != 60 && q * 90000 != offset)
        q++;
    cmos_write(TERRITORY_CMOS_ZONE, (uint32_t)q);
    switch_check();
    return 1;
}

/* Help and syntax as the Messages' CmdHelp has them (a native module's are
 * text: CR a new line) */
#define CONFIGURE (ROS_CMD_CONFIGURE)
static const struct ros_command commands[] = {
    { "Territory", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *Territory <Territory Number>",
      "*Territory sets the current territory.\r"
      "*Territory with no parameter displays the current territory.", cmd_territory },
    { "Territories", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Territories",
      "*Territories lists the currently loaded territory modules.", cmd_territories },
    { "Territory", ROS_CMD_INFO(1, 1, 0, CONFIGURE),
      "Syntax: *Configure Territory <Territory Number>",
      "*Configure Territory sets the default territory for the machine.", cfg_territory },
    { "DST", ROS_CMD_INFO(0, 0, 0, CONFIGURE), "Syntax: *Configure DST | NoDST | AutoDST",
      "*Configure DST sets the clock for Daylight Saving Time,\r"
      "*Configure NoDST sets the clock for Local Standard Time,\r"
      "*Configure AutoDST leaves the computer to make the decision itself.", cfg_dst },
    { "NoDST", ROS_CMD_INFO(0, 0, 0, CONFIGURE), "Syntax: *Configure DST | NoDST | AutoDST",
      "*Configure DST sets the clock for Daylight Saving Time,\r"
      "*Configure NoDST sets the clock for Local Standard Time,\r"
      "*Configure AutoDST leaves the computer to make the decision itself.", cfg_nodst },
    { "AutoDST", ROS_CMD_INFO(0, 0, 0, CONFIGURE), "Syntax: *Configure DST | NoDST | AutoDST",
      "*Configure DST sets the clock for Daylight Saving Time,\r"
      "*Configure NoDST sets the clock for Local Standard Time,\r"
      "*Configure AutoDST leaves the computer to make the decision itself.", cfg_autodst },
    { "TimeZone", ROS_CMD_INFO(1, 1, 0, CONFIGURE),
      "Syntax: *Configure TimeZone [+/-]<Hours>[:<Minutes>]",
      "*Configure TimeZone sets the time zone as an offset from UTC", cfg_timezone },
    { NULL, 0, NULL, NULL, NULL },
};

/* ---- the module ----------------------------------------------------------------- */

/* The configured territory is there by now, or the UK is taken */
static void post_init(void)
{
    struct workspace *w = ws();
    if (!find(w->configured))
        w->configured = 1;
    selected();
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    switch (s->r[1]) {
    case TERRITORY_SERVICE_POST_INIT:
        post_init();
        break;
    case TERRITORY_SERVICE_UKCONFIG:
        if (s->r[0] != 0 && configure_zone(s->r[0])) {
            s->r[0] = 0xFFFFFFFFu;
            s->r[1] = 0;
        }
        break;
    case TERRITORY_SERVICE_RTC_SYNCED:
        switch_check();
        break;
    }
}

static uint32_t code_address(int which)
{
    /* code addresses, made once for the life of the image, as a ROM
     * module's code has its addresses */
    static uint32_t made[2];
    if (!made[0]) {
        made[0] = ros_native_entry(switchover, "TerritoryManager:Switchover");
        made[1] = ros_native_entry(loaded, "TerritoryManager:Loaded");
    }
    return made[which];
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    ros_st32(m->private_word, ros_addr(w));
    w->switchover = code_address(0);
    w->loaded = code_address(1);

    uint32_t v[10] = { text("TerritoryManager$Path"), 0, 0xFFFFFFFFu, 0, 3 };
    swi(XOS_ReadVarVal, v);
    if (v[2] == 0) {
        static const char path[] = "Resources:$.Resources.TerrMgr.";
        uint32_t n = text("TerritoryManager$Path");
        strcpy(w->text + 32, path);
        uint32_t r[10] = { n, ros_addr(w->text + 32), sizeof path - 1, 0, 0 };
        swi(XOS_SetVarVal, r);
    }

    uint32_t config;
    w->configured = cmos_read(TERRITORY_CMOS, &config) ? 1 : (int32_t)(config ^ 1);
    w->zone = -1;
    w->last_bits = -1;
    w->last_offset = -1;
    if (lookup("Custom", ros_addr(w->custom), sizeof w->custom))
        strcpy(w->custom, "Custom");
    return xos_add_call_back(w->loaded, ros_addr(w));
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    xos_remove_ticker_event(w->switchover, ros_addr(w));
    if (w->messages_open) {
        uint32_t r[10] = { ros_addr(w->messages) };
        swi(XMessageTrans_CloseFile, r);
    }
    for (uint32_t n = w->list; n;) {
        uint32_t next = ros_ld32(n + N_NEXT);
        ros_rma_free(ros_ptr(n));
        n = next;
    }
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module territory_manager_module = {
    .title = "TerritoryManager",
    .help = "Territory Mgr\t0.58 (25 Jan 2021) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = TERRITORY_CHUNK,
    .swi_count = SWI_COUNT,
    .swi_thunks = swis,
    .swi_names = swi_names,
    .swi_prefix = "Territory",
    .commands = commands,
};
