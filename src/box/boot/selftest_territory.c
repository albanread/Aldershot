/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_territory.c: the Territory Manager and the UK territory,
 * native (modules/territory): what must always hold. Their behaviour
 * against RISC OS 5.30's, SWI by SWI, is tests/desktop/territory
 * (compare.py). The values here are the farm's.
 *
 * The typed API (api/defs/territory.toml) is used where C clients will
 * use it, and the registers where compiled code sees them. The output of
 * *commands is caught on WrchV.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u

static char out[512];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* A SWI: its registers in and out, and the flags; 0 or the error number */
static uint32_t swi(uint32_t number, uint32_t r[8], struct ros_cpu *after)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, number);
    memcpy(r, s.r, 8 * sizeof r[0]);
    if (after)
        *after = s;
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

/* A command's output, without its newlines' CRs */
static const char *cli(uint32_t line, const char *cmd)
{
    strcpy(ros_ptr(line), cmd);
    outn = 0, out[0] = 0;
    uint32_t r[8] = { line };
    if (swi(XOS_CLI, r, NULL))
        snprintf(out, sizeof out, "error: %s", ((os_error *)ros_ptr(r[0]))->errmess);
    char *w = out;
    for (const char *p = out; *p; p++)
        if (*p != 13)
            *w++ = *p;
    *w = 0;
    return out;
}

void ros_selftest_territory(void)
{
    uint8_t *mem = ros_rma_alloc(1024);
    if (!mem) {
        check(0, "Territory: memory for the test", NULL);
        return;
    }
    uint32_t a = ros_addr(mem);

    /* The manager and the UK: the configured territory, which is there */
    int32_t number = 0;
    int exists = 0, other = 1;
    check(!xterritory_number(&number) && number == 1 && !xterritory_exists(1, &exists) &&
              exists && !xterritory_exists(2, &other) && !other,
          "Territory_Number, Exists -- the UK, 1, registered; 2 not", "number %d", number);

    /* The tables: in the arena, the same from -1 and 1, RISC OS's bytes */
    uint8_t *lower = NULL, *upper = NULL, *lower1 = NULL;
    uint32_t *digit = NULL;
    int t1 = !xterritory_lower_case_table(-1, &lower) && !xterritory_lower_case_table(1, &lower1) &&
             !xterritory_upper_case_table(-1, &upper) &&
             !xterritory_character_property_table(-1, 6, &digit) && lower == lower1 &&
             ros_in_arena(lower) && lower['A'] == 'a' && lower[0xC0] == 0xE0 &&
             lower[0xD7] == 0xD7 && lower[0x81] == 0x82 && upper[0xFF] == 'Y' &&
             upper[0xDF] == 0xDF && digit[1] == 0x03FF0000u && digit[0] == 0;
    check(t1, "Territory_LowerCaseTable, UpperCaseTable, CharacterPropertyTable -- the UK's",
          NULL);

    /* Collate goes through the plain and lower-case tables, then accents, then
     * case. "A" sorts before "a", and the fi ligature sorts as "f" and "i". */
    int32_t same = 1, fi = 1;
    strcpy(ros_ptr(a), "abc");
    strcpy(ros_ptr(a + 16), "ABC");
    strcpy(ros_ptr(a + 32), "\x9E" "le");
    strcpy(ros_ptr(a + 48), "file");
    struct ros_cpu f;
    uint32_t r[8] = { 0xFFFFFFFFu, a, a + 16, 0 };
    uint32_t e = swi(XTerritory_Collate, r, &f);
    int32_t order = (int32_t)r[0];
    int c1 = !e && order == 1 && !f.n && !f.z && f.c;
    c1 = c1 && !xterritory_collate(-1, ros_ptr(a), ros_ptr(a + 16), 1, &same) && same == 0 &&
         !xterritory_collate(-1, ros_ptr(a + 32), ros_ptr(a + 48), 0, &fi) && fi == 0;
    r[0] = 0xFFFFFFFFu, r[1] = a, r[2] = a + 16, r[3] = 1;
    swi(XTerritory_Collate, r, &f);
    c1 = c1 && r[0] == 0 && f.z;
    check(c1, "Territory_Collate -- \"abc\" after \"ABC\", equal ignoring case; fi as f i",
          "order %d, ignoring case %d, fi %d", order, same, fi);

    /* The date, in every field, at 12:34:56.78 on Saturday 1 January 2000 */
    uint32_t t = a + 64;
    ros_st32(t, 0x798CE60Eu);
    ros_st8(t + 4, 0x49);
    strcpy(ros_ptr(a + 80),
           "%W3 %WE %WN %DY%ST %MO %M3 %MN %CE%YR %WK %DN %24:%MI:%SE.%CS %12%AM %ZDY %TZ");
    uint8_t *end = NULL;
    uint32_t left = 0;
    e = (uint32_t)(xterritory_convert_date_and_time(-1, ros_ptr(t), mem + 256, 200,
                                                    ros_ptr(a + 80), 0, &end, &left) != NULL);
    const char *want = "Sat Saturday 7 01st January Jan 01 2000 52 001 12:34:56.78 12pm 1 GMT";
    check(!e && strcmp((char *)mem + 256, want) == 0 && end == mem + 256 + strlen(want) &&
              left == 200 - strlen(want) - 1,
          "Territory_ConvertDateAndTime -- every field, the week of 1 January 2000 its 52nd",
          "%s", (char *)mem + 256);

    /* OS_ConvertDateAndTime is the territory's */
    uint32_t k[8] = { t, a + 512, 100, a + 80 };
    e = swi(XOS_ConvertDateAndTime, k, NULL);
    check(!e && strcmp(ros_ptr(a + 512), want) == 0 && k[1] == a + 512 + strlen(want),
          "OS_ConvertDateAndTime -- the territory's conversion", "%s", (char *)ros_ptr(a + 512));

    /* Ordinals both ways, and a time string */
    uint32_t *o = ros_ptr(a + 400);
    int o1 = !xterritory_convert_time_to_ordinals(-1, ros_ptr(t), o) && o[0] == 78 &&
             o[1] == 56 && o[2] == 34 && o[3] == 12 && o[4] == 1 && o[5] == 1 &&
             o[6] == 2000 && o[7] == 7 && o[8] == 1;
    memset(mem + 380, 0xEE, 8);
    o1 = o1 && !xterritory_convert_ordinals_to_time(-1, mem + 380, o) &&
         memcmp(mem + 380, ros_ptr(t), 5) == 0 && mem[385] == 0xEE;
    strcpy(ros_ptr(a + 440), "Mon, 25-Jan-66.23:59:61");
    o1 = o1 && !xterritory_convert_time_string_to_ordinals(-1, 3, ros_ptr(a + 440), o) &&
         o[0] == 0 && o[1] == 61 && o[2] == 59 && o[3] == 23 && o[4] == 25 && o[5] == 1 &&
         o[6] == 2066;
    check(o1, "ConvertTimeToOrdinals, ConvertOrdinalsToTime, ConvertTimeStringToOrdinals",
          NULL);

    /* 0.58's ordinals: month 0 reads the word before its table */
    uint32_t z[7] = { 0, 0, 0, 0, 0, 0, 2000 };
    memcpy(o, z, sizeof z);
    int m0 = !xterritory_convert_time_formats(-1, o, mem + 380, 0x100, 0) &&
             ros_ld32(a + 380) == 0x79295F69u && mem[384] == 0x49;
    check(m0, "Territory_ConvertTimeFormats -- ordinals as the 5.30 manager converts them",
          "%02X%08X", mem[384], ros_ld32(a + 380));

    /* Names, numbers, symbols, zones, and the UK's daylight saving rules */
    uint32_t sym = 0;
    char *std = NULL, *dst = NULL;
    int32_t so = -1, dso = -1, n4 = 0;
    uint32_t key = 0;
    strcpy(ros_ptr(a + 440), "Italy");
    int n1 = !xterritory_number_to_name(1, mem + 256, 16) && strcmp((char *)mem + 256, "UK") == 0 &&
             !xterritory_name_to_number(-1, ros_ptr(a + 440), &n4) && n4 == 4 &&
             !xterritory_read_symbols(-1, 4, &sym) && ros_ld8(sym) == 0xA3 &&
             !xterritory_read_time_zones(-1, 0, 0, &std, &dst, &so, &dso, &key) &&
             strcmp(std, "GMT") == 0 && strcmp(dst, "BST") == 0 && so == 0 && dso == 360000;
    int b1 = !xterritory_dst_bounds(-1, 0, 2026, mem + 256, mem + 264) &&
             ros_ld32(a + 256) == 0xC0E8E640u && mem[260] == 0x5C &&
             ros_ld32(a + 264) == 0x2D0E7240u && mem[268] == 0x5D;
    check(n1 && b1, "NumberToName, NameToNumber, ReadSymbols, ReadTimeZones, the 2026 DST bounds",
          "name %s, Italy %d", (char *)mem + 256, n4);

    /* A SWI past the table, and a territory not there */
    uint32_t x[8] = { 0 };
    uint32_t y[8] = { 2 };
    check(swi(XTerritory_ConvertDateAndTime + 0x19, x, NULL) == 0x1E8 &&
              swi(XTerritory_LowerCaseTable, y, NULL) == 0x192,
          "Territory_Reserved1 out of range (&1E8); territory 2 not present (&192)", NULL);

    /* *Territory and *Status, printed */
    ros_vector_claim_native(WRCHV, wrch, 0);
    uint32_t line = a + 600;
    int p1 = strcmp(cli(line, "Territory"), "UK\n") == 0 &&
             strcmp(cli(line, "Territories"), "1 UK\n") == 0 &&
             strcmp(cli(line, "Status TimeZone"), "TimeZone   +0:0\n") == 0 &&
             strcmp(cli(line, "Status DST"), "NoDST\n") == 0;
    check(p1, "*Territory, *Territories, *Status TimeZone and DST", "%s", out);
    ros_vector_release_native(WRCHV, wrch, 0);
    ros_rma_free(mem);
}
