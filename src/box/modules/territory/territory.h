/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* territory.h -- the Territory Manager and the territories, reimplemented
 * (territorymanager.c, uk.c; README.md lists the SWIs).
 *
 * A territory is RISC OS's territory module source compiled to data.  Its
 * values and tables are written by tools/mkterritory.py from s/<Territory>
 * and s/Tables ($(GEN)/territory_<name>.c).  uk.c is a territory module
 * over one of them.  The manager and a territory talk as RISC OS's do,
 * through Territory_Register's table of entry points.  Nothing else here
 * is shared but numbers.
 */
#ifndef ROSGD_TERRITORY_H
#define ROSGD_TERRITORY_H

#include <stdint.h>

#define TERRITORY_MAX_ZONES 4

/* One time zone within a territory: its names, and its offsets from UTC in
 * centiseconds (s/<Territory>'s NODSTn, DSTn, NODSTOffsetn, DSTOffsetn). */
struct territory_zone {
    const char *std, *dst;
    int32_t std_offset, dst_offset;
};

/* Territory_ReadSymbols' values (s/<Territory>'s Decimal ... ListSymbol). */
struct territory_symbols {
    const char *decimal, *thousand, *intcurr, *currency, *mdecimal, *mthousand;
    const char *mpositive, *mnegative, *listsymbol;
    uint8_t grouping[8], mgrouping[8];
    uint8_t grouping_length, mgrouping_length;
    /* int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space,
     * n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn: reasons 10-17 */
    int32_t numbers[8];
};

struct territory_def {
    const char *title, *help;           /* the module's title, and its help's name */
    int32_t number, alphabet;
    const char *alphabet_name;
    uint32_t write_direction, ime_swi_chunk;
    /* ReadCalendarInformation: first and last working day, months, then the
     * longest am/pm, weekday, short weekday, day, suffix, month, short
     * month and zone name */
    int32_t calendar[11];
    int32_t max_tz_length;
    int32_t zone_count;
    struct territory_zone zones[TERRITORY_MAX_ZONES];
    const char *date_format, *time_format, *date_and_time;
    struct territory_symbols symbols;
    /* s/Territory's collation options */
    uint8_t collate_latin1_ligatures, collate_oe_ligatures, collate_danish_aa;
    uint8_t collate_thorn_as_th, collate_german_sharp_s, collate_accents_backwards;
    uint8_t japanese_eras;
    uint8_t to_lower[256], to_upper[256], to_control[256], to_plain[256], to_value[256];
    uint8_t representation[16], sort_value[256], plain_for_collate[256];
    /* Territory_CharacterPropertyTable 0-10: control, upper case, lower
     * case, alpha, punctuation, space, digit, hex digit, accented, forward
     * flow, backward flow */
    uint32_t properties[11][8];
};

extern const struct territory_def territory_uk;

/* A time broken down (timecalc.c): s/DateTime's GetTimeValues' results */
struct territory_time {
    uint32_t dom, month, week, doy, dow, yearhi, yearlo, hours, mins, cs, secs;
};

/* A five-byte time plus a signed offset in centiseconds, broken down */
void territory_time_values(const uint8_t t[5], int32_t offset, struct territory_time *v);

/* Seven ordinals (centiseconds, seconds, minutes, hours, day, month, year)
 * to a five-byte time, as the Territory Manager 0.58 converts them; -1 if
 * it will not (then Bad time values) */
int territory_ordinals_to_time(const uint32_t o[7], uint8_t t[5]);

/* The SWI chunk, and the entries a territory registers: one for each SWI
 * from ReadTimeZones to DaylightRules. */
#define TERRITORY_CHUNK           0x43040u
#define TERRITORY_FIRST_ENTRY     10u           /* &4304A, ReadTimeZones */
#define TERRITORY_ENTRIES         26u

#define TERRITORY_ZONE_EXTENSION  0x454E4F5Au   /* "ZONE" */

/* Errors (hdr/Territory, ErrorBase_TerritoryManager) */
#define TERRITORY_ERR_UNKNOWN_TERRITORY 0x190u
#define TERRITORY_ERR_UNKNOWN_ALPHABET  0x191u
#define TERRITORY_ERR_NO_TERRITORY      0x192u
#define TERRITORY_ERR_UNKNOWN_PROPERTY  0x193u
#define TERRITORY_ERR_BAD_TIME_STRING   0x194u
#define TERRITORY_ERR_BAD_TIME_BLOCK    0x195u
#define TERRITORY_ERR_NO_MORE_ZONES     0x196u
#define TERRITORY_ERR_NO_RULE           0x197u
#define TERRITORY_ERR_OUT_OF_RANGE      0x1E8u  /* ErrorNumber_OutOfRange */
#define TERRITORY_ERR_BUFFER_OVERFLOW   0x2C1u  /* CDATBufferOverflow */
#define TERRITORY_ERR_BAD_FIELD         0x2C2u  /* CDATBadField */

/* Services */
#define TERRITORY_SERVICE_UKCONFIG      0x28u
#define TERRITORY_SERVICE_INTERNATIONAL 0x43u
#define TERRITORY_SERVICE_MANAGER_LOADED 0x64u
#define TERRITORY_SERVICE_POST_INIT     0x73u
#define TERRITORY_SERVICE_STARTED       0x75u
#define TERRITORY_SERVICE_RTC_SYNCED    0xDDu
#define TERRITORY_SERVICE_ZONE_CHANGED  0x81080u

/* CMOS */
#define TERRITORY_CMOS          0x18u   /* the configured territory, EOR 1 */
#define TERRITORY_CMOS_ZONE     0x8Bu   /* signed quarter hours from UTC */
#define TERRITORY_CMOS_DBTB     0x10u   /* bit 0: AutoDST */
#define TERRITORY_CMOS_ALARM    0xDCu   /* bit 7: DST */

/* Centiseconds */
#define TERRITORY_CS_HOUR       360000
#define TERRITORY_CS_DAY        8640000

struct ros_module;
extern struct ros_module territory_manager_module;
extern struct ros_module territory_uk_module;

#endif
