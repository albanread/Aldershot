/* Copyright 1998 Acorn Computers Ltd
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
 * (Sources/Internat/Territory/TerritoryModule: s.ModHead, s.Entries,
 * s.DateTime, s.Transform, s.UK).
 */

/* uk.c -- the UK territory module, reimplemented.
 *
 * Written from RISC OS's territory module source (Internat/Territory/
 * TerritoryModule 0.64: s/ModHead, s/Entries, s/DateTime, s/Transform) and
 * checked against the 5.30 ROM's on the farm (tests/desktop/territory).
 * The territory's values and tables are s/UK's,
 * compiled to data by tools/mkterritory.py (territory_uk).
 *
 * It registers with the Territory Manager as a territory module does.  It
 * gives a table of entry points, one for each SWI from ReadTimeZones to
 * DaylightRules.  Each is entered with the SWI's registers and R12 its
 * workspace, and returns to R14 with its results in the registers.  Here
 * each is native code at a native entry's address.  What it does to the
 * registers, flags and memory is s/Entries' and s/DateTime's, to the byte,
 * including the buffer counting and the odd corners.
 *
 * What clients reach by address is in one RMA block, made once and kept.
 * That is the tables, the zone names and the symbol strings.  A ROM
 * module's tables are kept at their addresses when it is killed and started
 * again, and this does the same (the Filer keeps the lower-case table's
 * address).  The workspace, made at each start, holds the Messages
 * descriptor and the scratch buffers.
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

#define FI 0x9Eu                        /* the fi and fl ligatures, Acorn Latin1 */
#define FL 0x9Fu

static const struct territory_def *const def = &territory_uk;

/* What clients read by address: made once, never freed */
struct data {
    uint8_t to_lower[256], to_upper[256], to_control[256], to_plain[256], to_value[256];
    uint8_t representation[16], sort_value[256], plain_for_collate[256];
    uint32_t properties[11][8];
    uint8_t zones[TERRITORY_MAX_ZONES * 40];    /* s/Entries' tz_table */
    char alphabet_name[16];
    char strings[128];                          /* ReadSymbols' strings and groupings */
    uint32_t symbols[19];                       /* ReadSymbols: an address or a number */
    char null_string[4];                        /* "", ConvertDateAndTime's zone given an offset */
};

struct workspace {
    uint32_t messages_open;
    uint32_t messages[4];
    char filename[24];                  /* the file's name, which MessageTrans keeps */
    uint32_t zone_name;                 /* timezone_name_ptr: %TZ's string */
    char scratch[180];                  /* MessageLookup's buffer */
    char format[32];                    /* the standard formats, for ConvertDateAndTime */
    char token[16];
    uint8_t error[32];
    char enumerated[8];                 /* EnumerateTokens' buffer */
    uint32_t ords[16];
    uint32_t entries[TERRITORY_ENTRIES];
    uint32_t data;
};

static struct data *data_of(const struct workspace *w)
{
    return ros_ptr(w->data);
}

/* s/Entries' tz_table: each zone its standard and DST names in fields
 * of (MaxTZLength + 1) rounded up to words, then the two offsets */
static uint32_t tz_aligned(void)
{
    return ((uint32_t)def->max_tz_length + 1 + 3) & ~3u;
}

static uint32_t tz_entry(void)
{
    return tz_aligned() * 2 + 8;
}

/* ---- calling out ------------------------------------------------------------------ */

static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static uint32_t token(struct workspace *w, const char *t)
{
    snprintf(w->token, sizeof w->token, "%s", t);
    return ros_addr(w->token);
}

static os_error *open_messages(struct workspace *w)
{
    if (w->messages_open)
        return NULL;
    snprintf(w->filename, sizeof w->filename, "%s:Messages", def->title);
    uint32_t r[10] = { ros_addr(w->messages), ros_addr(w->filename), 0 };
    os_error *e = swi(XMessageTrans_OpenFile, r);
    w->messages_open = e == NULL;
    return e;
}

/* message_errorlookup: an error block by token (none of the UK's texts
 * has a parameter) */
static os_error *error(struct workspace *w, uint32_t errnum, const char *tok)
{
    os_error *e = open_messages(w);
    if (e)
        return e;
    ros_st32(ros_addr(w->error), errnum);
    snprintf((char *)w->error + 4, sizeof w->error - 4, "%s", tok);
    uint32_t r[10] = { ros_addr(w->error), ros_addr(w->messages), 0, 0, 0 };
    return swi(XMessageTrans_ErrorLookup, r);
}

/* MessageLookup: the token's text into the scratch buffer */
static os_error *message(struct workspace *w, const char *tok, uint32_t *text)
{
    os_error *e = open_messages(w);
    if (e)
        return e;
    uint32_t r[10] = { ros_addr(w->messages), token(w, tok), ros_addr(w->scratch),
                       sizeof w->scratch };
    if ((e = swi(XMessageTrans_Lookup, r)) != NULL)
        return e;
    *text = ros_addr(w->scratch);
    return NULL;
}

static struct workspace *ws(struct ros_cpu *s)
{
    return ros_ptr(s->r[12]);
}

static void done(struct ros_cpu *s, os_error *e)
{
    if (e) {
        s->r[0] = ros_addr(e);
        s->v = 1;
    } else {
        s->v = 0;
    }
    s->r[15] = s->r[14];
}

/* ---- the simple entries ----------------------------------------------------------- */

static void e_alphabet(struct ros_cpu *s)
{
    s->r[0] = (uint32_t)def->alphabet;
    done(s, NULL);
}

static void e_alphabet_identifier(struct ros_cpu *s)
{
    s->r[0] = ros_addr(data_of(ws(s))->alphabet_name);
    done(s, NULL);
}

static void e_select_keyboard_handler(struct ros_cpu *s)
{
    uint32_t r[10] = { 71, 128 + (uint32_t)def->number };
    os_error *e = swi(XOS_Byte, r);
    done(s, e);
}

static void e_write_direction(struct ros_cpu *s)
{
    s->r[0] = def->write_direction;
    done(s, NULL);
}

static void e_ime(struct ros_cpu *s)
{
    s->r[0] = def->ime_swi_chunk;
    done(s, NULL);
}

static void e_character_property_table(struct ros_cpu *s)
{
    struct workspace *w = ws(s);
    if (s->r[1] > 10) {
        done(s, error(w, TERRITORY_ERR_UNKNOWN_PROPERTY, "UnkProp"));
        return;
    }
    s->r[0] = ros_addr(data_of(w)->properties[s->r[1]]);
    done(s, NULL);
}

#define TABLE(entry, field)                                                 \
    static void entry(struct ros_cpu *s)                                    \
    {                                                                       \
        s->r[0] = ros_addr(data_of(ws(s))->field);                          \
        done(s, NULL);                                                      \
    }
TABLE(e_lower_case_table, to_lower)
TABLE(e_upper_case_table, to_upper)
TABLE(e_control_table, to_control)
TABLE(e_plain_table, to_plain)
TABLE(e_value_table, to_value)
TABLE(e_representation_table, representation)

static void e_read_symbols(struct ros_cpu *s)
{
    s->r[0] = s->r[1] <= 18 ? data_of(ws(s))->symbols[s->r[1]] : 0;
    done(s, NULL);
}

/* ReadTimeZones, on registers r[0..4]; the entry's and ConvertDateAndTime's */
static os_error *read_time_zones(struct workspace *w, uint32_t r[5])
{
    if (r[4] != TERRITORY_ZONE_EXTENSION)
        r[1] = 0;
    else
        r[4] = 0;
    if (r[1] >= (uint32_t)def->zone_count)
        return error(w, TERRITORY_ERR_NO_MORE_ZONES, "NMZones");
    uint32_t z = ros_addr(data_of(w)->zones) + r[1] * tz_entry();
    r[0] = z;
    r[1] = z + tz_aligned();
    r[2] = ros_ld32(z + 2 * tz_aligned());
    r[3] = ros_ld32(z + 2 * tz_aligned() + 4);
    return NULL;
}

static void e_read_time_zones(struct ros_cpu *s)
{
    uint32_t r[5] = { s->r[0], s->r[1], s->r[2], s->r[3], s->r[4] };
    os_error *e = read_time_zones(ws(s), r);
    if (!e)
        memcpy(s->r, r, sizeof r);
    done(s, e);
}

/* ---- ConvertDateAndTime -------------------------------------------------------------- */

struct field {
    char name[3];
    uint8_t item, digits;               /* the value's index (stack offset / 4); digits */
    uint8_t kind;
};

enum { NUMBER, TWELVE, AMPM, SUFFIX, SHORT_DAY, SHORT_MONTH, LONG_DAY, LONG_MONTH, ZONE };

/* s/DateTime's CDATEscTab: the values are GetTimeValues' registers --
 * 0 day, 1 month, 2 week, 3 day of the year, 4 day of the week, 5 century,
 * 6 year, 7 hours, 8 minutes, 9 centiseconds, 10 seconds */
static const struct field fields[] = {
    { "DY", 0, 2, NUMBER }, { "ST", 0, 2, SUFFIX }, { "MN", 1, 2, NUMBER },
    { "MO", 1, 2, LONG_MONTH }, { "M3", 1, 2, SHORT_MONTH }, { "WK", 2, 2, NUMBER },
    { "DN", 3, 3, NUMBER }, { "WN", 4, 1, NUMBER }, { "W3", 4, 1, SHORT_DAY },
    { "WE", 4, 1, LONG_DAY }, { "CE", 5, 2, NUMBER }, { "YR", 6, 2, NUMBER },
    { "24", 7, 2, NUMBER }, { "12", 7, 2, TWELVE }, { "AM", 7, 2, AMPM }, { "PM", 7, 2, AMPM },
    { "MI", 8, 2, NUMBER }, { "CS", 9, 2, NUMBER }, { "SE", 10, 2, NUMBER },
    { "TZ", 0, 2, ZONE },
};

static uint32_t upper(uint32_t c)
{
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

/* The conversion proper, from a time broken down: s/DateTime's
 * CDATMainLoop, byte for byte.  out, left: the buffer; fmt the format. */
static os_error *format_time(struct workspace *w, const uint32_t values[11], uint32_t *out_p,
                             uint32_t *left_p, uint32_t fmt)
{
    uint32_t out = *out_p, left = *left_p;
    os_error *e = NULL;
    for (;;) {
        if (left-- == 0)
            goto overflow;
        uint32_t c = ros_ld8(fmt++);
        if (c != '%') {
            ros_st8(out++, c);
            if (c == 0)
                break;
            continue;
        }
        c = ros_ld8(fmt++);
        if (c == '0' || c == '%') {
            ros_st8(out++, c == '0' ? 0 : '%');
            continue;
        }
        c = upper(c);
        uint32_t keep = c ^ 'Z';                /* 0: %Z, leading zeros suppressed */
        if (keep == 0)
            c = upper(ros_ld8(fmt++));
        if (c == 0) {
            e = error(w, TERRITORY_ERR_BAD_FIELD, "BadFld");
            goto out;
        }
        uint32_t c2 = upper(ros_ld8(fmt++));
        const struct field *f = NULL;
        for (unsigned i = 0; i < sizeof fields / sizeof fields[0] && !f; i++)
            if ((uint8_t)fields[i].name[0] == c && (uint8_t)fields[i].name[1] == c2)
                f = &fields[i];
        if (!f) {
            e = error(w, TERRITORY_ERR_BAD_FIELD, "BadFld");
            goto out;
        }
        uint32_t v = values[f->item];
        const char *tok = NULL;
        char tokbuf[8];
        switch (f->kind) {
        case TWELVE:
            if (v >= 12)
                v -= 12;
            if (v == 0)
                v = 12;
            /* fall through */
        case NUMBER: {
            static const uint32_t powers[4] = { 0, 1, 10, 100 };
            left++;                             /* the "%"'s byte back */
            for (uint32_t d = f->digits; d > 0; d--) {
                uint32_t count = 0;
                while (v >= powers[d])
                    v -= powers[d], count++;
                if (d == 1)
                    keep = 1;
                keep |= count;
                if (keep == 0)
                    continue;
                if (left-- == 0)
                    goto overflow;
                ros_st8(out++, '0' | count);
            }
            continue;
        }
        case AMPM:
            tok = v < 12 ? "am" : "pm";
            break;
        case SUFFIX:
            tok = v == 1 || v == 21 || v == 31 ? "st" : v == 2 || v == 22 ? "nd"
                : v == 3 || v == 23 ? "rd" : "th";
            break;
        case SHORT_DAY:
            snprintf(tokbuf, sizeof tokbuf, "D%02u", v);
            tok = tokbuf;
            break;
        case SHORT_MONTH:
            snprintf(tokbuf, sizeof tokbuf, "M%02u", v);
            tok = tokbuf;
            break;
        case LONG_DAY:
            snprintf(tokbuf, sizeof tokbuf, "D%02uL", v);
            tok = tokbuf;
            break;
        case LONG_MONTH:
            snprintf(tokbuf, sizeof tokbuf, "M%02uL", v);
            tok = tokbuf;
            break;
        case ZONE:
            break;
        }
        uint32_t text = w->zone_name;
        if (tok && (e = message(w, tok, &text)) != NULL)
            goto out;
        /* the first character uses the "%"'s byte; each after, one more */
        c = ros_ld8(text++);
        if (c == 0)
            continue;
        for (;;) {
            ros_st8(out++, c);
            c = ros_ld8(text++);
            if (c == 0)
                break;
            if (left-- == 0)
                goto overflow;
        }
    }
    *out_p = out - 1;                           /* the terminator */
    *left_p = left;
    return NULL;
overflow:
    e = error(w, TERRITORY_ERR_BUFFER_OVERFLOW, "BOvFlow");
out:
    return e;
}

static void time_values(const struct territory_time *t, uint32_t v[11])
{
    v[0] = t->dom, v[1] = t->month, v[2] = t->week, v[3] = t->doy, v[4] = t->dow;
    v[5] = t->yearhi, v[6] = t->yearlo, v[7] = t->hours, v[8] = t->mins, v[9] = t->cs;
    v[10] = t->secs;
}

/* R1 -> time, R2 buffer, R3 size and flags, R4 -> format, R5 */
static os_error *convert_date_and_time(struct workspace *w, uint32_t r[6])
{
    uint32_t flags = r[3];
    int32_t offset;
    os_error *e;
    if (flags & 1u << 31 && flags & 0x0FFF0000u)
        return error(w, TERRITORY_ERR_BAD_FIELD, "BadFld");
    if (flags & 1u << 31 && flags & 1u << 30) {
        if (flags & 1u << 29) {
            uint32_t z[5] = { 0, r[5], 0, 0, TERRITORY_ZONE_EXTENSION };
            if ((e = read_time_zones(w, z)) != NULL)
                return e;
            int dst = (flags & 1u << 28) != 0;
            w->zone_name = dst ? z[1] : z[0];
            offset = (int32_t)(dst ? z[3] : z[2]);
        } else {
            if (flags & 1u << 28)
                return error(w, TERRITORY_ERR_BAD_FIELD, "BadFld");
            w->zone_name = ros_addr(data_of(w)->null_string);
            offset = (int32_t)r[5];
        }
    } else {
        uint32_t z[10] = { 0, 0, 0 };
        if ((e = swi(XTerritory_ReadCurrentTimeZone, z)) != NULL)
            return e;
        w->zone_name = z[0];
        offset = (int32_t)z[1];
    }
    struct territory_time t;
    territory_time_values(ros_ptr(r[1]), offset, &t);
    uint32_t v[11];
    time_values(&t, v);
    uint32_t out = r[2], left = flags & 1u << 31 ? flags & 0xFFFF : flags;
    if ((e = format_time(w, v, &out, &left, r[4])) != NULL)
        return e;
    r[0] = r[2];
    r[1] = out;
    r[2] = left;
    r[3] = flags & 1u << 31 ? flags & ~(1u << 31) : r[4];
    return NULL;
}

static void e_convert_date_and_time(struct ros_cpu *s)
{
    uint32_t r[6];
    memcpy(r, s->r, sizeof r);
    os_error *e = convert_date_and_time(ws(s), r);
    if (!e)
        memcpy(s->r, r, 4 * sizeof r[0]);
    done(s, e);
}

/* ConvertStandardDateAndTime, Date, Time: ConvertDateAndTime (the SWI) with
 * the territory's format; R3 and R4 kept, R3's bit 31 cleared */
static void standard(struct ros_cpu *s, const char *format)
{
    struct workspace *w = ws(s);
    snprintf(w->format, sizeof w->format, "%s", format);
    uint32_t r[10] = { s->r[0], s->r[1], s->r[2], s->r[3], ros_addr(w->format), s->r[5] };
    os_error *e = swi(XTerritory_ConvertDateAndTime, r);
    if (!e)
        s->r[0] = r[0], s->r[1] = r[1], s->r[2] = r[2];
    if (s->r[3] & 1u << 31)
        s->r[3] &= ~(1u << 31);
    done(s, e);
}

static void e_convert_standard_date_and_time(struct ros_cpu *s)
{
    standard(s, def->date_and_time);
}

static void e_convert_standard_date(struct ros_cpu *s)
{
    standard(s, def->date_format);
}

static void e_convert_standard_time(struct ros_cpu *s)
{
    standard(s, def->time_format);
}

/* ---- ordinals ---------------------------------------------------------------------- */

static void e_convert_time_to_ordinals(struct ros_cpu *s)
{
    uint32_t z[10] = { 0, 0, 0 };
    os_error *e = swi(XTerritory_ReadCurrentTimeZone, z);
    if (!e) {
        struct territory_time t;
        territory_time_values(ros_ptr(s->r[1]), (int32_t)z[1], &t);
        uint32_t o[9] = { t.cs, t.secs, t.mins, t.hours, t.dom, t.month,
                          t.yearhi * 100 + t.yearlo, t.dow, t.doy };
        for (int i = 0; i < 9; i++)
            ros_st32(s->r[2] + 4u * (uint32_t)i, o[i]);
    }
    done(s, e);
}

/* The current offset, DST included, whether or not DST would be in force
 * at the time given, as the original implementation did */
static void e_convert_ordinals_to_time(struct ros_cpu *s)
{
    uint32_t z[10] = { 0, 0, 0 };
    os_error *e = swi(XTerritory_ReadCurrentTimeZone, z);
    if (!e) {
        uint32_t r[10] = { (uint32_t)def->number, s->r[2], s->r[1], 0x80300u, z[1] };
        e = swi(XTerritory_ConvertTimeFormats, r);
    }
    done(s, e);
}

/* ---- ConvertTimeStringToOrdinals --------------------------------------------------- */

struct scan {
    struct workspace *w;
    uint32_t p;                         /* the next character */
    uint32_t c;                         /* the current one */
};

static void next(struct scan *k)
{
    k->c = ros_ld8(k->p++);
}

/* SkipSpaces1, then SkipSpaces for as long as it is a space */
static void skip_spaces(struct scan *k)
{
    while (k->c == ' ')
        next(k);
}

/* CheckSeparator: spaces, then an optional sep and spaces after it */
static void separator(struct scan *k, uint32_t sep)
{
    skip_spaces(k);
    if (k->c == sep) {
        next(k);
        skip_spaces(k);
    }
}

/* GetInteger: digits to a value no more than most; -1 bad */
static int integer(struct scan *k, uint32_t most, uint32_t *value)
{
    if (k->c - '0' >= 10)
        return -1;
    uint32_t v = 0;
    do {
        v = v * 10 + (k->c - '0');
        next(k);
    } while (k->c - '0' < 10);
    if (v > most)
        return -1;
    *value = v;
    return 0;
}

static const uint8_t month_lengths[13] = { 28, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static uint32_t days_in(uint32_t month, uint32_t year)
{
    uint32_t n = month_lengths[month];
    if (month == 2) {
        int32_t y = (int32_t)year - 2000;
        if (y == -100 || y == 100 || y == 200 || year & 3)
            n = 28;
    }
    return n;
}

/* The date part: day, month and year into out's words 4 to 6 */
static os_error *scan_date(struct scan *k, uint32_t out)
{
    struct workspace *w = k->w;
    os_error *e;
    open_messages(w);
    if (k->c - '0' >= 10) {
        for (;;) {                      /* a day of the week, to its "," */
            if (k->c < ' ')
                goto bad;
            if (k->c == ',')
                break;
            next(k);
        }
        next(k);
        skip_spaces(k);
    }
    uint32_t day, year;
    if (integer(k, 31, &day) || day == 0)
        goto bad;
    separator(k, '-');

    /* the month: the first M?? token whose text the string starts with */
    uint32_t month = 1, start = k->p - 1, context = 0;
    for (;; month++) {
        uint32_t r[10] = { ros_addr(w->messages), token(w, "M??"), ros_addr(w->enumerated), 8,
                           context };
        if ((e = swi(XMessageTrans_EnumerateTokens, r)) != NULL)
            return e;
        if (r[2] == 0)
            goto bad;
        context = r[4];
        uint32_t l[10] = { ros_addr(w->messages), ros_addr(w->enumerated), 0 };
        if ((e = swi(XMessageTrans_Lookup, l)) != NULL)
            return e;
        uint32_t m = l[2], u = start;
        for (;;) {
            uint32_t mc = ros_ld8(m++);
            if (mc == 10)
                goto matched;
            uint32_t uc = ros_ld8(u++);
            if (uc < ' ')
                goto bad;
            if ((mc | 0x20) != (uc | 0x20))
                break;
        }
        continue;
    matched:
        /* the rest of the month's word, to a space or "-" */
        for (;;) {
            uint32_t uc = ros_ld8(u++);
            if (uc < ' ')
                goto bad;
            if (uc == ' ' || uc == '-') {
                k->c = uc;
                k->p = u;
                break;
            }
        }
        break;
    }
    separator(k, '-');
    uint32_t first = k->p;
    if (integer(k, 0xFFFFFFFFu, &year))
        goto bad;
    if (k->p - first < 3) {             /* two digits: 26-Jan-66 on is 19xx */
        int later = year > 66 || (year == 66 && (month > 1 || day >= 26));
        year += later ? 1900 : 2000;
    }
    if (days_in(month, year) < day)
        goto bad;
    ros_st32(out + 16, day);
    ros_st32(out + 20, month);
    ros_st32(out + 24, year);
    separator(k, '.');
    return NULL;
bad:
    return error(w, TERRITORY_ERR_BAD_TIME_STRING, "BadStr");
}

/* Each field is stored as it is read, as the original stores them: a
 * bad string leaves those before it written */
static os_error *time_string_to_ordinals(struct workspace *w, uint32_t reason, uint32_t string,
                                         uint32_t out, uint32_t *r0)
{
    struct scan k = { w, string, 0 };
    uint32_t v;
    next(&k);
    skip_spaces(&k);
    os_error *e;
    *r0 = 0xFFFFFFFFu;
    if (reason & 2) {
        if ((e = scan_date(&k, out)) != NULL)
            return e;
    } else {
        for (uint32_t i = 4; i < 7; i++)
            ros_st32(out + 4 * i, 0xFFFFFFFFu);
    }
    if (!(reason & 1)) {
        for (uint32_t i = 0; i < 4; i++)
            ros_st32(out + 4 * i, 0xFFFFFFFFu);
        return NULL;
    }
    *r0 = 0;
    ros_st32(out, 0);
    if (integer(&k, 23, &v))
        goto bad;
    ros_st32(out + 12, v);
    separator(&k, ':');
    if (integer(&k, 59, &v))
        goto bad;
    ros_st32(out + 8, v);
    separator(&k, ':');
    if (integer(&k, 61, &v))
        goto bad;
    ros_st32(out + 4, v);
    return NULL;
bad:
    return error(w, TERRITORY_ERR_BAD_TIME_STRING, "BadStr");
}

static void e_convert_time_string_to_ordinals(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0];
    os_error *e = time_string_to_ordinals(ws(s), s->r[1], s->r[2], s->r[3], &r0);
    if (!e)
        s->r[0] = r0;
    done(s, e);
}

/* ---- Collate and TransformString ---------------------------------------------------- */

static int is_digit(const struct data *d, uint32_t c)
{
    for (int i = 0; i < 10; i++)
        if (d->representation[i] == c)
            return 1;
    return 0;
}

static int ligature(uint32_t c)
{
    return def->collate_latin1_ligatures && (c == FI || c == FL);
}

/* s/Entries' Collate: R0 and the flags as its SUBS or EORS leaves them */
static void collate(const struct data *d, uint32_t a, uint32_t b, uint32_t given,
                    struct ros_cpu *s)
{
    uint32_t flags = (given & 0xFFFF) | 3;          /* first ignoring case and accents */
    uint32_t p1 = a, p2 = b;
    int32_t result = 0;
    for (int passes = 0;; passes++) {
        for (;;) {
            uint32_t c1 = ros_ld8(p1++), c2 = ros_ld8(p2++);
            if (flags & 4 && is_digit(d, c1) && is_digit(d, c2)) {
                /* numbers: their lengths after leading zeros, else on
                 * from their first significant digits */
                uint32_t q1 = p1, q2 = p2, x = c1;
                while (x == d->representation[0])
                    x = ros_ld8(q1++);
                uint32_t s1 = q1 - 1;
                while (is_digit(d, x))
                    x = ros_ld8(q1++);
                x = c2;
                while (x == d->representation[0])
                    x = ros_ld8(q2++);
                uint32_t s2 = q2 - 1;
                while (is_digit(d, x))
                    x = ros_ld8(q2++);
                uint32_t l1 = q1 - s1, l2 = q2 - s2;
                if (l1 != l2) {
                    result = (int32_t)(l1 - l2);
                    s->n = result < 0, s->z = 0, s->c = l1 >= l2;
                    goto out;
                }
                c1 = ros_ld8(s1), c2 = ros_ld8(s2);
                p1 = s1 + 1, p2 = s2 + 1;
            }
            if (flags & 2)
                c1 = d->plain_for_collate[c1], c2 = d->plain_for_collate[c2];
            if (flags & 1)
                c1 = d->to_lower[c1], c2 = d->to_lower[c2];
            if (ligature(c1)) {
                flags ^= 0x20000000u;
                if (flags & 0x20000000u)
                    c1 = 'f', p1--;
                else
                    c1 = c1 == FI ? 'i' : 'l';
            }
            if (ligature(c2)) {
                flags ^= 0x10000000u;
                if (flags & 0x10000000u)
                    c2 = 'f', p2--;
                else
                    c2 = c2 == FI ? 'i' : 'l';
            }
            c1 = d->sort_value[c1], c2 = d->sort_value[c2];
            if (c1 != c2) {
                result = (int32_t)c1 - (int32_t)c2;
                s->n = result < 0, s->z = 0, s->c = c1 >= c2;
                goto out;
            }
            if (c1 == 0)
                break;
        }
        /* equal this pass: done if it was the pass asked for.  Otherwise go
         * again, minding accents, then case.  The original loops for ever
         * here if the flags' top half is not 0.  This stops after the case
         * pass. */
        uint32_t differ = flags ^ given;
        if (differ == 0 || passes >= 2) {
            s->n = 0, s->z = 1, s->c = 1;
            result = 0;
            goto out;
        }
        p1 = a, p2 = b;
        if (differ & 2)
            flags &= ~2u;
        else
            flags &= ~1u;
    }
out:
    s->r[0] = (uint32_t)result;
}

static void e_collate(struct ros_cpu *s)
{
    collate(data_of(ws(s)), s->r[1], s->r[2], s->r[3], s);
    s->v = 0;
    s->r[15] = s->r[14];
}

/* s/Transform: three passes of sort values.  They are plain and lower
 * case, lower case, and as they are.  Each is ended by 1, 1 and 2 escaped
 * by 2 */
static void e_transform_string(struct ros_cpu *s)
{
    const struct data *d = data_of(ws(s));
    uint32_t out = s->r[1], src = s->r[2], size = s->r[3];
    uint32_t len = 0, extra = 0;
    for (uint32_t p = src;; p++) {
        uint32_t c = ros_ld8(p);
        if (c <= 2)
            extra++;
        if (ligature(c))
            extra++;
        if (c == 0)
            break;
        len++;
    }
    uint32_t total = (len + extra) * 3;
    s->r[0] = total;
    if (total >= size) {
        done(s, NULL);
        return;
    }
    for (int pass = 0; pass < 3; pass++) {
        for (uint32_t p = src; p != src + len; p++) {
            uint32_t c = ros_ld8(p);
            if (pass == 0)
                c = d->plain_for_collate[c];
            if (pass < 2)
                c = d->to_lower[c];
            if (ligature(c)) {
                ros_st8(out++, d->sort_value['f']);
                c = c == FI ? 'i' : 'l';
            }
            c = d->sort_value[c];
            if (c <= 2)
                ros_st8(out++, 2);
            ros_st8(out++, c);
        }
        ros_st8(out++, 1);
    }
    ros_st8(out, 0);
    done(s, NULL);
}

/* ---- the calendar, names, and daylight saving rules ---------------------------------- */

static void e_read_calendar_information(struct ros_cpu *s)
{
    uint32_t info = s->r[2];
    const int32_t *c = def->calendar;
    ros_st32(info + 0, (uint32_t)c[0]);
    ros_st32(info + 4, (uint32_t)c[1]);
    ros_st32(info + 8, (uint32_t)c[2]);
    for (int i = 3; i < 11; i++)
        ros_st32(info + 16 + 4 * (uint32_t)(i - 3), (uint32_t)c[i]);
    uint32_t z[10] = { 0, 0, 0 };
    os_error *e = swi(XTerritory_ReadCurrentTimeZone, z);
    if (!e) {
        struct territory_time t;
        territory_time_values(ros_ptr(s->r[1]), (int32_t)z[1], &t);
        uint32_t n = month_lengths[t.month];
        if (t.month == 2) {
            if (t.yearlo & 3)
                n = 28;
            else if (t.yearlo == 0 && t.yearhi & 3)
                n = 28;
        }
        ros_st32(info + 12, n);
    }
    done(s, e);
}

/* s/Entries' NameToNumber: the TR* tokens in order, the name compared
 * through the lower-case table; a name that ends inside a token's text
 * gives 0 at once */
static os_error *name_to_number(struct workspace *w, uint32_t name, uint32_t *number)
{
    const uint8_t *lower = data_of(w)->to_lower;
    open_messages(w);
    uint32_t context = 0;
    for (;;) {
        uint32_t r[10] = { ros_addr(w->messages), token(w, "TR*"), ros_addr(w->enumerated), 8,
                           context };
        os_error *e = swi(XMessageTrans_EnumerateTokens, r);
        if (e)
            return e;
        if (r[2] == 0) {
            *number = 0;
            return NULL;
        }
        context = r[4];
        uint32_t l[10] = { ros_addr(w->messages), ros_addr(w->enumerated), 0 };
        if ((e = swi(XMessageTrans_Lookup, l)) != NULL)
            return e;
        for (uint32_t m = l[2], u = name;;) {
            uint32_t mc = ros_ld8(m++);
            if (mc == 10) {
                if (ros_ld8(u) >= ' ')
                    break;              /* the name goes on: the next token */
                uint32_t n[10] = { 10, ros_addr(w->enumerated) + 2 };
                if ((e = swi(XOS_ReadUnsigned, n)) != NULL)
                    return e;
                *number = n[2];
                return NULL;
            }
            uint32_t uc = ros_ld8(u++);
            if (uc == 0) {
                *number = 0;
                return NULL;
            }
            if (lower[mc] != lower[uc])
                break;
        }
    }
}

static void e_name_to_number(struct ros_cpu *s)
{
    uint32_t n;
    os_error *e = name_to_number(ws(s), s->r[1], &n);
    if (!e)
        s->r[0] = n;
    done(s, e);
}

static uint32_t dec2(uint32_t text, uint32_t at)
{
    return (ros_ld8(text + at) - '0') * 10 + (ros_ld8(text + at + 1) - '0');
}

/* s/DateTime's DaylightBound: text "hhmm_wd_mm" in local standard time,
 * for zone and year, to a five-byte UTC time */
static os_error *bound(struct workspace *w, uint32_t zone, uint32_t year, uint32_t text,
                       uint32_t *lo, uint32_t *hi)
{
    uint32_t o = ros_addr(w->ords);
    uint32_t set[7] = { 0, 0, dec2(text, 2), dec2(text, 0), 1, dec2(text, 8), year };
    for (int i = 0; i < 7; i++)
        ros_st32(o + 4u * (uint32_t)i, set[i]);
    uint32_t r[10] = { 1, o, o, 0 };
    os_error *e = swi(XTerritory_ConvertTimeFormats, r);
    if (e)
        return e;
    uint32_t carry = ros_ld32(o + 16) - 1 ? (uint32_t)TERRITORY_CS_DAY : 0;   /* 24:00 */
    uint32_t month = ros_ld32(o + 20), y = ros_ld32(o + 24);
    uint32_t length = month <= 12 ? days_in(month, y) : 0;
    uint32_t which = ros_ld8(text + 5), dow = ros_ld8(text + 6);
    if (dow == '*') {
        ros_st32(o + 16, which == 'L' ? length : which - '0');
    } else {
        /* the which-th such day in the month, or the last found */
        uint32_t n = which - '0', want = dow - '0', now = ros_ld32(o + 28);
        for (uint32_t day = 1; day <= length; day++) {
            if (want == now) {
                ros_st32(o + 16, day);
                if (--n == 0)
                    break;
            }
            if (++now == 8)
                now = 1;
        }
    }
    uint32_t t[10] = { (uint32_t)def->number, o, o, 0x300, zone };
    if ((e = swi(XTerritory_ConvertTimeFormats, t)) != NULL)
        return e;
    uint64_t v = ((uint64_t)ros_ld8(o + 4) << 32 | ros_ld32(o)) + carry;
    *lo = (uint32_t)v;
    *hi = (uint32_t)(v >> 32);
    return NULL;
}

static os_error *daylight_rules(struct workspace *w, uint32_t r[6])
{
    if (r[1] > 1)
        return error(w, TERRITORY_ERR_OUT_OF_RANGE, "UnkSWI");
    if (r[1] == 0) {
        r[0] = 1;
        return NULL;
    }
    char tok[16];
    char zone[8];
    snprintf(zone, sizeof zone, "%u", (r[2] + 100) & 0xFF);
    zone[0] = 'Z';
    snprintf(tok, sizeof tok, "%s_%u", zone, r[3] & 0xFFFF);
    uint32_t text;
    os_error *e = message(w, tok, &text);
    if (e)
        return e;
    if (ros_ld8(text + 4) != '_' || ros_ld8(text + 7) != '_' || ros_ld8(text + 15) != '_' ||
        ros_ld8(text + 18) != '_')
        return error(w, TERRITORY_ERR_BAD_TIME_STRING, "BadStr");
    uint32_t lo, hi;
    if ((e = bound(w, r[2], r[3], text, &lo, &hi)) != NULL)
        return e;
    ros_st32(r[4], lo);
    ros_st8(r[4] + 4, hi);
    if ((e = bound(w, r[2], r[3], text + 11, &lo, &hi)) != NULL)
        return e;
    ros_st32(r[5], lo);
    ros_st8(r[5] + 4, hi);
    r[0] = lo;
    return NULL;
}

static void e_daylight_rules(struct ros_cpu *s)
{
    uint32_t r[6];
    memcpy(r, s->r, sizeof r);
    os_error *e = daylight_rules(ws(s), r);
    if (!e)
        s->r[0] = r[0];
    done(s, e);
}

/* ---- the module ------------------------------------------------------------------- */

static ros_code *const entry_code[TERRITORY_ENTRIES] = {
    e_read_time_zones, e_convert_date_and_time, e_convert_standard_date_and_time,
    e_convert_standard_date, e_convert_standard_time, e_convert_time_to_ordinals,
    e_convert_time_string_to_ordinals, e_convert_ordinals_to_time, e_alphabet,
    e_alphabet_identifier, e_select_keyboard_handler, e_write_direction,
    e_character_property_table, e_lower_case_table, e_upper_case_table, e_control_table,
    e_plain_table, e_value_table, e_representation_table, e_collate, e_read_symbols,
    e_read_calendar_information, e_name_to_number, e_transform_string, e_ime,
    e_daylight_rules,
};

/* The entries' code addresses and the data block: made once for the life
 * of the image, as a ROM module's code and tables have their addresses */
static uint32_t entry_addresses[TERRITORY_ENTRIES];
static uint32_t data_block;

static uint32_t symbol_string(char **at, const char *s)
{
    uint32_t a = ros_addr(*at);
    size_t n = strlen(s) + 1;
    memcpy(*at, s, n);
    *at += n;
    return a;
}

static uint32_t symbol_bytes(char **at, const uint8_t *b, unsigned n)
{
    uint32_t a = ros_addr(*at);
    memcpy(*at, b, n);
    *at += n;
    return a;
}

static os_error *make_data(void)
{
    if (data_block)
        return NULL;
    struct data *d = ros_rma_alloc(sizeof *d);
    if (!d)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(d, 0, sizeof *d);
    memcpy(d->to_lower, def->to_lower, 256);
    memcpy(d->to_upper, def->to_upper, 256);
    memcpy(d->to_control, def->to_control, 256);
    memcpy(d->to_plain, def->to_plain, 256);
    memcpy(d->to_value, def->to_value, 256);
    memcpy(d->representation, def->representation, 16);
    memcpy(d->sort_value, def->sort_value, 256);
    memcpy(d->plain_for_collate, def->plain_for_collate, 256);
    memcpy(d->properties, def->properties, sizeof d->properties);
    for (int i = 0; i < def->zone_count; i++) {
        uint8_t *z = d->zones + (uint32_t)i * tz_entry();
        strcpy((char *)z, def->zones[i].std);
        strcpy((char *)z + tz_aligned(), def->zones[i].dst);
        memcpy(z + 2 * tz_aligned(), &def->zones[i].std_offset, 4);
        memcpy(z + 2 * tz_aligned() + 4, &def->zones[i].dst_offset, 4);
    }
    snprintf(d->alphabet_name, sizeof d->alphabet_name, "%s", def->alphabet_name);
    const struct territory_symbols *y = &def->symbols;
    char *at = d->strings;
    d->symbols[0] = symbol_string(&at, y->decimal);
    d->symbols[1] = symbol_string(&at, y->thousand);
    d->symbols[2] = symbol_bytes(&at, y->grouping, y->grouping_length);
    d->symbols[3] = symbol_string(&at, y->intcurr);
    d->symbols[4] = symbol_string(&at, y->currency);
    d->symbols[5] = symbol_string(&at, y->mdecimal);
    d->symbols[6] = symbol_string(&at, y->mthousand);
    d->symbols[7] = symbol_bytes(&at, y->mgrouping, y->mgrouping_length);
    d->symbols[8] = symbol_string(&at, y->mpositive);
    d->symbols[9] = symbol_string(&at, y->mnegative);
    for (int i = 0; i < 8; i++)
        d->symbols[10 + i] = (uint32_t)y->numbers[i];
    d->symbols[18] = symbol_string(&at, y->listsymbol);
    data_block = ros_addr(d);
    for (unsigned i = 0; i < TERRITORY_ENTRIES; i++)
        entry_addresses[i] = ros_native_entry(entry_code[i], "UK:Entry");
    return NULL;
}

static struct workspace *module_ws(void)
{
    return ros_ptr(ros_ld32(territory_uk_module.private_word));
}

static void register_territory(struct workspace *w)
{
    uint32_t r[10] = { (uint32_t)def->number, ros_addr(w->entries), ros_addr(w) };
    swi(XTerritory_Register, r);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    os_error *e = make_data();
    if (e)
        return e;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    w->data = data_block;
    memcpy(w->entries, entry_addresses, sizeof w->entries);
    ros_st32(m->private_word, ros_addr(w));

    /* UK$Path, if it is not set */
    snprintf(w->scratch, sizeof w->scratch, "%s$Path", def->title);
    uint32_t v[10] = { ros_addr(w->scratch), 0, 0xFFFFFFFFu, 0, 3 };
    swi(XOS_ReadVarVal, v);
    if (v[2] == 0) {
        int n = snprintf(w->scratch + 32, sizeof w->scratch - 32, "Resources:$.Resources.%s.",
                         def->title);
        uint32_t r[10] = { ros_addr(w->scratch), ros_addr(w->scratch + 32), (uint32_t)n, 0, 0 };
        swi(XOS_SetVarVal, r);
    }
    register_territory(w);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = module_ws();
    if (w->messages_open) {
        uint32_t r[10] = { ros_addr(w->messages) };
        swi(XMessageTrans_CloseFile, r);
    }
    uint32_t r[10] = { (uint32_t)def->number };
    swi(XTerritory_Deregister, r);
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

/* The manager started again: register with it */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] == TERRITORY_SERVICE_MANAGER_LOADED)
        register_territory(module_ws());
}

struct ros_module territory_uk_module = {
    .title = "UK",
    .help = "UK Territory\t0.64 (01 Feb 2021) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
};
