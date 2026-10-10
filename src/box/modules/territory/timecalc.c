/* Copyright 1996 Acorn Computers Ltd
 * Copyright 1998 Acorn Computers Ltd
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
 * (Sources/Internat/Territory/TerritoryModule: s.DateTime, and the Territory
 * Manager 0.58).
 */

/* timecalc.c -- five-byte times and ordinals, as RISC OS's territory code
 * computes them.
 *
 * There are two routines.  Each is RISC OS's own arithmetic to the bit,
 * wrap-arounds included, because clients see what they give for any input.
 *
 *   - territory_time_values is s/DateTime's GetTimeValues (the UK
 *     territory).  The Territory Manager 0.58 has the same routine without
 *     the offset.  It takes a five-byte time plus a signed offset, in the
 *     original's 32-bit words.  It gives the day, month, ISO-style week,
 *     day of the year and of the week, year, hours, minutes, seconds and
 *     centiseconds.
 *   - territory_ordinals_to_time is the Territory Manager 0.58's routine
 *     from ordinals to a five-byte time (under ConvertTimeFormats and so
 *     ConvertOrdinalsToTime).  Its checks and table are its own.
 */
#include "territory.h"

/* s/DateTime's MonthLengths: February first (as 28), then January to
 * December with February as 29 */
static const uint8_t month_lengths[13] = { 28, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int leap(uint32_t yearhi, uint32_t yearlo)
{
    if (yearlo & 3)
        return 0;
    if (yearlo != 0)
        return 1;
    return (yearhi & 3) == 0;
}

void territory_time_values(const uint8_t t[5], int32_t offset, struct territory_time *v)
{
    uint32_t lo = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 |
                  (uint32_t)t[3] << 24;
    uint32_t hi = t[4];
    uint64_t sum = (uint64_t)lo + (uint32_t)offset;     /* ADDS, ADC with the sign */
    hi += (uint32_t)(offset >> 31) + (uint32_t)(sum >> 32);
    lo = (uint32_t)sum;

    /* centiseconds DIV 256, then days and the centiseconds of the day */
    uint32_t q = lo >> 8 | hi << 24;
    uint32_t days = q / (TERRITORY_CS_DAY / 256);
    uint32_t today = (lo & 0xFF) | (q % (TERRITORY_CS_DAY / 256)) << 8;
    v->hours = today / TERRITORY_CS_HOUR;
    today %= TERRITORY_CS_HOUR;
    v->mins = today / 6000;
    today %= 6000;
    v->secs = today / 100;
    v->cs = today % 100;

    uint32_t dow = (days + 1) % 7 + 1;                  /* 1 January 1900 was a Monday */
    uint32_t yearlo = 0, yearhi = 19;
    int isleap;
    for (;;) {
        isleap = leap(yearhi, yearlo);
        uint32_t len = 365u + (uint32_t)isleap;
        if (days < len)
            break;
        days -= len;
        if (++yearlo == 100)
            yearlo = 0, yearhi++;
    }
    v->yearhi = yearhi;
    v->yearlo = yearlo;
    v->doy = days + 1;
    v->dow = dow;

    /* The week: counted from the Monday on or before the day, week 1 the
     * first with four days in the year */
    uint32_t monday = dow >= 2 ? dow - 2 : dow + 5;     /* Mon 0 .. Sun 6 */
    uint32_t start = days - monday + 6;
    uint32_t week = start / 7, rem = start % 7;
    if (rem >= 3)
        week++;
    uint32_t most = ((rem == 4 && isleap) || rem == 3) ? 53 : 52;
    if (week > most)
        week = 1;
    if (week == 0) {
        week = 52 + (rem >= 1);                         /* Fri 53, Sun 52 */
        if (rem == 1) {                                 /* Sat: 53 if last year was leap */
            uint32_t plo = yearlo, phi = yearhi;
            if (plo == 0)
                plo = 99, phi--;
            else
                plo--;
            if (!leap(phi, plo))
                week = 52;
        }
    }
    v->week = week;

    uint32_t month = 1;
    for (const uint8_t *m = &month_lengths[1];; month++, m++) {
        uint32_t len = *m - (month == 2 ? (uint32_t)!isleap : 0);
        if (days < len)
            break;
        days -= len;
    }
    v->month = month;
    v->dom = days + 1;
}

/* The start of each month in centiseconds, as 0.58's table; month 0 reads
 * the word before it, the end of its "BadTime" token ("ime" and a zero) */
static const uint32_t month_starts[13] = {
    0x00656D69u, 0x00000000u, 0x0FF6EA00u, 0x1E625200u, 0x2E593C00u, 0x3DCC5000u, 0x4DC33A00u,
    0x5D364E00u, 0x6D2D3800u, 0x7D242200u, 0x8C973600u, 0x9C8E2000u, 0xAC013400u,
};

int territory_ordinals_to_time(const uint32_t o[7], uint8_t t[5])
{
    uint32_t cs = o[0], secs = o[1], mins = o[2], hours = o[3], day = o[4], month = o[5];
    uint32_t yearhi = o[6] / 100, yearlo = o[6] % 100;

    /* the time of day and the days of the month, in one 32-bit word */
    uint32_t w = ((((day - 1) * 24 + hours) * 60 + mins) * 60 + secs) * 100 + cs;
    if (month > 12)
        return -1;
    w += month_starts[month];

    /* leap days up to the year before (January and February: the year
     * before that), less those to 1899 */
    uint32_t borrow = month < 3 && yearlo == 0;
    uint32_t ylo = month < 3 ? (yearlo == 0 ? 99 : yearlo - 1) : yearlo;
    uint32_t yhi = yearhi - borrow;
    uint32_t leaps = (ylo >> 2) + (yhi >> 2) + (yhi << 4) + (yhi << 3);
    if (leaps < 460 || leaps - 460 >= 86)
        return -1;
    w += (uint32_t)TERRITORY_CS_DAY * (leaps - 460);

    if (yearhi < 19)
        return -1;
    uint64_t years = (uint64_t)(yearhi - 19) * 100 + yearlo;
    uint64_t total = (uint64_t)w + years * 0xBBF81E00ull;   /* 365 days */
    if (total >> 40)
        return -1;
    for (int i = 0; i < 5; i++)
        t[i] = (uint8_t)(total >> (8 * i));
    return 0;
}
