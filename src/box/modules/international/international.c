/* Copyright 1996 Acorn Computers Ltd
 * Copyright 1998 Acorn Computers Ltd
 * Copyright 2012 Castle Technology Ltd
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
 * This file is a reimplementation in C of RISC OS Open's International module
 * (Sources/Internat/Inter: s.InterBody, s.InterFonts, s.UCSTables, hdr.Internatio).
 */

/* international.c: the International module, rewritten for ROSGD as a
 * native module.
 *
 * RISC OS's International module (Internat/Inter, version 1.70 in RISC OS
 * 5.30) names the countries and alphabets. It also draws the alphabets'
 * characters in the system font. It does all of this through one service
 * call, Service_International (&43). The kernel issues that call for
 * OS_Byte 70 and 71, and anyone may issue or answer it. This is new code
 * written to follow s/InterBody register by register. The data is RISC OS's
 * own. That is the names, the countries' alphabets, the alphabets' UCS
 * tables and the glyphs. It is generated
 * from s/InterBody, s/InterFonts and s/UCSTables by
 * tools/mkinternational.py.
 *
 *   - Service_International 0 to 4 and 9 convert names to numbers and
 *     numbers to names. They also give a country's alphabet and a
 *     country's ISO 3166 code.
 *   - Reason 5 defines an alphabet's characters in the system font
 *     (VDU 23). Each character is defined from its UCS code, as reason 7
 *     does it. The top half of UTF8 is drawn as hex pairs.
 *   - Reason 7 defines one character from a UCS code. Reason 8 returns an
 *     alphabet's UCS table.
 *   - The module also has the commands *Alphabet, *Alphabets, *Country,
 *     *Countries, *Keyboard and *Configure Country. Their texts come from
 *     its Messages file.
 *
 * The module's state, which is its Messages file, is in the RMA. It is
 * reached through the module's private word. The alphabets' UCS tables are
 * handed out by address by Service_International 8. They are copied into
 * the RMA once for the life of the image, as RISC OS's are in its ROM, so
 * that a caller can keep the address.
 */
#include <stdio.h>
#include <string.h>

#include "international.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define COUNTRY_CMOS            0xBAu           /* CountryCMOS */
#define NAME_BUFFER             16u             /* NameBufferSize */
#define INFO_BUFFER             512u            /* InfoBufferSize */
#define UCS_SOLID_BLOCK         0x2588u         /* What &7F (delete) is drawn as */

#define ERR_UNKNOWN_ALPHABET    0x640u
#define ERR_UNKNOWN_COUNTRY     0x641u
#define ERR_UNKNOWN_KEYBOARD    0x642u

static const char messages_file[] = "Resources:$.Resources.Internatio.Messages";

/* ---- the state ----------------------------------------------------------------- */

struct workspace {
    uint32_t desc[4];                   /* MessageFile_Block */
    uint32_t open;                      /* MessageFile_Open */
    char file[44];                      /* The file's name, which MessageTrans keeps */
    char token[8];
    uint32_t error[3];                  /* An error block: its number and token */
    uint8_t glyph[8];                   /* A character's rows, for VDU 23 */
    char name[NAME_BUFFER];             /* PrintAValue's buffer */
    char info[INFO_BUFFER];             /* PrintInfo's buffer */
};

/* The alphabets' UCS tables in the RMA, in FontPointers' order. They are
 * made once. */
static uint32_t ucs_block;

static struct workspace *ws(void)
{
    uint32_t pw = international_module.private_word, w = pw ? ros_ld32(pw) : 0;
    return w ? ros_ptr(w) : NULL;
}

/* Call a SWI in its X form, on registers r[0..9] */
static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static os_error *write_c(uint32_t c)
{
    uint32_t r[10] = { c };
    return swi(OS_WriteC, r);
}

static os_error *new_line(void)
{
    uint32_t r[10] = { 0 };
    return swi(OS_NewLine, r);
}

/* ---- the tables ------------------------------------------------------------------- */

/* LookupNumber over FontPointers. Returns the index of the alphabet's
 * table, or -1. */
static int font_pointer(uint32_t alphabet)
{
    for (const struct intl_number *p = intl_font_pointers; p->number >= 0; p++)
        if ((uint32_t)p->number == alphabet)
            return p->index;
    return -1;
}

static uint32_t table_word(int index, uint32_t c)
{
    return intl_alphabet_ucs[index][c];
}

/* VDU 23: define character c from eight rows */
static os_error *vdu23(struct workspace *w, uint32_t c, const uint8_t rows[8])
{
    memcpy(w->glyph, rows, 8);
    os_error *e = write_c(23);
    if (!e)
        e = write_c(c);
    if (!e) {
        uint32_t r[10] = { ros_addr(w->glyph), 8 };
        e = swi(OS_WriteN, r);
    }
    return e;
}

/* DoDefineUCS: define character c from UCS code *u, if there is a glyph
 * for it. *u is left as RISC OS leaves R4. It is unchanged when no range
 * holds the code. Otherwise it is the glyph's offset in FontTable, or
 * &FFFF if there is no glyph. Returns 1 if the character was defined. */
static int define_ucs(struct workspace *w, uint32_t c, uint32_t *u, os_error **e)
{
    *e = NULL;
    if (*u >= 0x10000)
        return 0;
    for (unsigned i = 0; i < intl_ucs_range_count; i++) {
        const struct intl_ucs_range *r = &intl_ucs_ranges[i];
        if (*u < r->base)
            return 0;
        if (*u > r->top)
            continue;
        uint32_t g = intl_ucs_glyphs[r->first + *u - r->base];
        *u = g;
        if (g & 0x8000)
            return 0;
        *e = vdu23(w, c, intl_font_table + g);
        return 1;
    }
    return 0;
}

/* ---- Service_International ------------------------------------------------------ */

/* Reasons 0 and 1, ConvertNameToNumber. R3 points to a name, which ends at
 * a control character or a space. It is matched without regard to case
 * (bit 5 is ignored), and '.' abbreviates it. The first name in the table
 * that matches wins, and R4 returns its number. */
static void name_to_number(struct ros_cpu *s, const struct intl_name *t)
{
    for (; t->name; t++) {
        const uint8_t *n = (const uint8_t *)t->name;
        uint32_t p = s->r[3];
        for (;;) {
            uint32_t c = ros_ld8(p++);
            if (c <= ' ')
                break;                          /* The name ended first */
            if (c == '.')
                goto found;                     /* Abbreviated */
            if ((c ^ *n) & ~0x20u)
                break;
            if (*++n == 0) {
                if (ros_ld8(p) > ' ')
                    break;                      /* The name goes on */
                goto found;
            }
        }
        continue;
found:
        s->r[4] = (uint32_t)t->number;
        s->r[1] = 0;
        return;
    }
}

/* Reasons 2 and 3, CopyName. The name of the number in R3 goes into the
 * buffer at R4, which is R5 bytes long. The name is not terminated, and it
 * is cut short to fit. R5 returns the number of bytes put there. */
static void number_to_name(struct ros_cpu *s, const struct intl_name *t)
{
    for (; t->name; t++) {
        if ((uint32_t)t->number != s->r[3])
            continue;
        uint32_t n = 0;
        while (n != s->r[5] && t->name[n]) {
            ros_st8(s->r[4] + n, (uint8_t)t->name[n]);
            n++;
        }
        s->r[5] = n;
        s->r[1] = 0;
        return;
    }
}

/* Reason 4. The alphabet of the country in R3 goes into R4 (CToATable,
 * which ends with the Default entry). */
static void country_to_alphabet(struct ros_cpu *s)
{
    for (const uint8_t *p = intl_country_alphabet[0]; p[0]; p += 2)
        if (p[0] == s->r[3]) {
            s->r[4] = p[1];
            s->r[1] = 0;
            return;
        }
}

/* Reason 5, DoDefine. Define the characters R4 to R5 of the alphabet in
 * R3 in the system font, as its UCS table and FontTable have them. At most
 * &20 to &FF are defined. Characters without a glyph are left as they are,
 * and &7F is drawn as a solid block. For UTF8, the characters from &80 up
 * are drawn as their codes in hex, and the rest come from Latin1. The call
 * is claimed only when there is a rest. This is a quirk of RISC OS. The top
 * half alone is not claimed, and neither is the bottom half alone. */
static void define(struct workspace *w, struct ros_cpu *s)
{
    uint32_t alphabet = s->r[3], first = s->r[4], last = s->r[5];
    os_error *e = NULL;
    if (alphabet == ALPHABET_UTF8 && last >= 0x80) {
        for (uint32_t c = first <= 0x80 ? 0x80 : first; c <= last; c++) {
            const uint8_t *lo = intl_digits[c & 15], *hi = intl_digits[c >> 4 & 15];
            uint8_t rows[8] = { lo[0], lo[1], lo[2], (uint8_t)(lo[3] | hi[0] << 4),
                                (uint8_t)(lo[4] | hi[1] << 4), (uint8_t)(hi[2] << 4),
                                (uint8_t)(hi[3] << 4), (uint8_t)(hi[4] << 4) };
            if (vdu23(w, c, rows))
                return;
        }
        if (first >= 0x80)
            return;
        alphabet = ALPHABET_LATIN1;
        last = 0x7F;
    }
    int t = font_pointer(alphabet);
    if (t < 0)
        return;
    s->r[1] = 0;
    if (last > 0xFF)
        last = 0xFF;
    if (first < 0x20)
        first = 0x20;
    for (uint32_t c = first; c <= last && !e; c++) {
        uint32_t u = table_word(t, c);
        if (u == 0x7F)
            u = UCS_SOLID_BLOCK;
        if ((int32_t)u >= 0x20)
            define_ucs(w, c, &u, &e);
    }
}

/* Reason 7. Define character R3 from the UCS code in R4. R4 is left as
 * DoDefineUCS leaves it. */
static void define_one(struct workspace *w, struct ros_cpu *s)
{
    os_error *e;
    if (define_ucs(w, s->r[3], &s->r[4], &e))
        s->r[1] = 0;
}

/* Reason 8. R4 returns the address of the table of 256 UCS codes for the
 * alphabet in R3. */
static void ucs_table(struct ros_cpu *s)
{
    int t = font_pointer(s->r[3]);
    if (t < 0)
        return;
    s->r[4] = ucs_block + (uint32_t)t * 1024;
    s->r[1] = 0;
}

/* Reason 9. The ISO 3166-1 alpha-2 code of the country in R3 goes into the
 * buffer at R4, which is R5 bytes long. It is not terminated. R5 returns the
 * number of bytes put there. A code whose second letter is a space means
 * that the country has none. */
static void alpha2(struct ros_cpu *s)
{
    if (s->r[3] >= 100)
        return;
    char c0 = intl_alpha2[s->r[3] * 2], c1 = intl_alpha2[s->r[3] * 2 + 1];
    if (c1 == ' ')
        return;
    if (s->r[5] >= 1)
        ros_st8(s->r[4], (uint8_t)c0);
    if (s->r[5] > 1) {
        ros_st8(s->r[4] + 1, (uint8_t)c1);
        s->r[5] = 2;
    }
    s->r[1] = 0;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    struct workspace *w = ws();
    if (s->r[1] != SERVICE_INTERNATIONAL || !w)
        return;
    switch (s->r[2]) {
    case INTER_CNA_TO_CNO:
        name_to_number(s, intl_country_strings);
        break;
    case INTER_ANA_TO_ANO:
        name_to_number(s, intl_alphabet_strings);
        break;
    case INTER_CNO_TO_CNA:
        number_to_name(s, intl_country_list);
        break;
    case INTER_ANO_TO_ANA:
        number_to_name(s, intl_alphabet_list);
        break;
    case INTER_CNO_TO_ANO:
        country_to_alphabet(s);
        break;
    case INTER_DEFINE:
        define(w, s);
        break;
    case INTER_DEFINE_UCS:
        define_one(w, s);
        break;
    case INTER_UCS_TABLE:
        ucs_table(s);
        break;
    case INTER_ISO3166_ALPHA2:
        alpha2(s);
        break;
    default:                            /* Reason 6, the new keyboard, is not ours */
        break;
    }
}

/* ---- the messages ------------------------------------------------------------------- */

static os_error *open_messages(struct workspace *w)
{
    if (w->open)
        return NULL;
    memcpy(w->file, messages_file, sizeof messages_file);
    uint32_t r[10] = { ros_addr(w->desc), ros_addr(w->file), 0 };
    os_error *e = swi(MessageTrans_OpenFile, r);
    if (!e)
        w->open = 1;
    return e;
}

/* message_write0: write the token's text, up to its first control
 * character. */
static os_error *write_message(struct workspace *w, const char *token)
{
    os_error *e = open_messages(w);
    if (e)
        return e;
    snprintf(w->token, sizeof w->token, "%s", token);
    uint32_t r[10] = { ros_addr(w->desc), ros_addr(w->token), 0, 0, 0, 0, 0, 0 };
    if ((e = swi(MessageTrans_Lookup, r)) != NULL)
        return e;
    for (uint32_t p = r[2]; ros_ld8(p) >= 32; p++)
        if ((e = write_c(ros_ld8(p))) != NULL)
            return e;
    return NULL;
}

/* CopyError: make the error, with its text from the Messages file */
static os_error *error(struct workspace *w, uint32_t number, const char *token)
{
    os_error *e = open_messages(w);
    if (e)
        return e;
    w->error[0] = number;
    memset(&w->error[1], 0, 8);
    memcpy(&w->error[1], token, strlen(token));
    uint32_t r[10] = { ros_addr(w->error), ros_addr(w->desc), 0, 0, 0, 0, 0, 0 };
    return swi(MessageTrans_ErrorLookup, r);
}

/* ---- the commands -------------------------------------------------------------------- */

/* OfferInterService. The caller gives R2 to R5. R1 is 0 on return if the
 * service was claimed. */
static int offer(uint32_t r[10])
{
    r[1] = SERVICE_INTERNATIONAL;
    swi(OS_ServiceCall, r);
    return r[1] == 0;
}

/* PrintAValue3. Print the number's name, found by the reason, or
 * "Unknown". Then print a new line. */
static os_error *print_name(struct workspace *w, uint32_t reason, uint32_t number)
{
    uint32_t r[10] = { 0, 0, reason, number, ros_addr(w->name), NAME_BUFFER - 1 };
    os_error *e;
    if (offer(r)) {
        w->name[r[5] < NAME_BUFFER ? r[5] : NAME_BUFFER - 1] = 0;
        uint32_t p[10] = { ros_addr(w->name) };
        e = swi(OS_Write0, p);
    } else {
        e = write_message(w, "M03");
    }
    return e ? e : new_line();
}

/* PrintAValue. Print the name of the number that OS_Byte returns. */
static os_error *print_value(struct workspace *w, uint32_t osbyte, uint32_t r1, uint32_t reason)
{
    uint32_t r[10] = { osbyte, r1 };
    os_error *e = swi(OS_Byte, r);
    return e ? e : print_name(w, reason, r[1]);
}

/* PrintInfo. Print the title. Then print every name that the reason gives
 * for the numbers 0 to 127, with a tab between names, using OS_PrettyPrint. */
static os_error *print_info(struct workspace *w, const char *title, uint32_t reason)
{
    os_error *e = write_message(w, title);
    if (!e)
        e = write_c(':');
    if (!e)
        e = write_c(10);
    if (!e)
        e = write_c(13);
    if (e)
        return e;
    uint32_t start = ros_addr(w->info), at = start, end = start + INFO_BUFFER - 1;
    int full = 0;
    for (uint32_t n = 0; n < 128; n++) {
        if (at > end) {
            full = 1;
            break;
        }
        uint32_t r[10] = { 0, 0, reason, n, at, end - at };
        if (offer(r)) {
            at += r[5];
            ros_st8(at++, 9);
        }
    }
    if (!full)
        ros_st8(at > start ? at - 1 : start, 0);
    uint32_t r[10] = { start, 0, 0 };
    e = swi(OS_PrettyPrint, r);
    return e ? e : new_line();
}

static os_error *cmd_alphabets(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    return print_info(ws(), "M06", INTER_ANO_TO_ANA);
}

static os_error *cmd_countries(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    return print_info(ws(), "M07", INTER_CNO_TO_CNA);
}

/* *Alphabet. With no parameter it prints the alphabet's name. Otherwise it
 * sets the alphabet from an alphabet's name or a country's name. OS_Byte
 * 71 takes either number. */
static os_error *cmd_alphabet(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct workspace *w = ws();
    if (argc == 0)
        return print_value(w, 71, 0x7F, INTER_ANO_TO_ANA);
    uint32_t r[10] = { tail, 0, INTER_ANA_TO_ANO, tail };
    if (!offer(r)) {
        r[2] = INTER_CNA_TO_CNO;
        if (!offer(r))
            return error(w, ERR_UNKNOWN_ALPHABET, "M00");
    }
    uint32_t b[10] = { 71, r[4] };
    return swi(OS_Byte, b);
}

/* *Keyboard. With no parameter it prints the keyboard's name. Otherwise it
 * sets the keyboard from a country's name. */
static os_error *cmd_keyboard(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct workspace *w = ws();
    if (argc == 0)
        return print_value(w, 71, 0xFF, INTER_CNO_TO_CNA);
    uint32_t r[10] = { tail, 0, INTER_CNA_TO_CNO, tail };
    if (!offer(r))
        return error(w, ERR_UNKNOWN_KEYBOARD, "M01");
    uint32_t b[10] = { 71, r[4] | 0x80 };
    return swi(OS_Byte, b);
}

/* *Country. With no parameter it prints the country's name. Otherwise it
 * sets the country, and its alphabet and keyboard. */
static os_error *cmd_country(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct workspace *w = ws();
    if (argc == 0)
        return print_value(w, 70, 0x7F, INTER_CNO_TO_CNA);
    uint32_t r[10] = { tail, 0, INTER_CNA_TO_CNO, tail };
    if (!offer(r))
        return error(w, ERR_UNKNOWN_COUNTRY, "M02");
    uint32_t b[10] = { 70, r[4] };
    return swi(OS_Byte, b);
}

/* *Configure Country. A tail of 0 prints its syntax and a tail of 1 prints
 * its status. Otherwise the tail is a country's name, and its number is
 * written into the CMOS. */
static os_error *cfg_country(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct workspace *w = ws();
    os_error *e;
    if (tail == 0) {
        e = write_message(w, "M04");
        return e ? e : new_line();
    }
    if (tail == 1) {
        uint32_t r[10] = { 161, COUNTRY_CMOS };
        if ((e = swi(OS_Byte, r)) != NULL)
            return e;
        if ((e = write_message(w, "M05")) != NULL)
            return e;
        for (int i = 0; i < 4 && !e; i++)
            e = write_c(' ');
        return e ? e : print_name(w, INTER_CNO_TO_CNA, r[2]);
    }
    uint32_t r[10] = { tail, 0, INTER_CNA_TO_CNO, tail };
    if (!offer(r))
        return error(w, ERR_UNKNOWN_COUNTRY, "M02");
    uint32_t b[10] = { 162, COUNTRY_CMOS, r[4] };
    return swi(OS_Byte, b);
}

/* Help and syntax as RISC OS's (its HelpSrc). A native module's help is
 * plain text, with CR for a new line. */
static const struct ros_command commands[] = {
    { "Alphabet", ROS_CMD_INFO(0, 1, 0, 0),
      "Syntax: *Alphabet [<country name> | <alphabet name>]",
      "*Alphabet with no parameter displays the currently selected alphabet.\r"
      "Type *Alphabets to list available alphabets.", cmd_alphabet },
    { "Country", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *Country [<country name>]",
      "*Country sets the appropriate alphabet and keyboard driver for a particular country.\r"
      "*Country with no parameter displays the currently selected country.", cmd_country },
    { "Keyboard", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *Keyboard [<country name>]",
      "*Keyboard with no parameter displays the currently selected keyboard.\r"
      "Type *Countries to list available countries.", cmd_keyboard },
    { "Country", ROS_CMD_INFO(0, 0, 0, ROS_CMD_CONFIGURE),
      "Syntax: *Configure Country <country name>",
      "*Configure Country controls which country setting the computer will use on a "
      "hard reset, which in turn determines which alphabet and keyboard driver is used.\r"
      "Type *Countries to list available countries.", cfg_country },
    { "Alphabets", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Alphabets",
      "*Alphabets lists the available alphabets.", cmd_alphabets },
    { "Countries", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Countries",
      "*Countries lists the available countries.", cmd_countries },
    { 0 },
};

/* ---- the module ------------------------------------------------------------------------ */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    if (!ucs_block) {
        uint32_t n = 0;
        while (intl_font_pointers[n].number >= 0)
            n++;
        uint8_t *b = ros_rma_alloc(n * 1024);
        if (!b)
            return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t c = 0; c < 256; c++)
                ros_st32(ros_addr(b) + i * 1024 + c * 4, table_word((int)i, c));
        ucs_block = ros_addr(b);
    }
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    ros_st32(m->private_word, ros_addr(w));
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    if (!w)
        return NULL;
    if (w->open) {
        uint32_t r[10] = { ros_addr(w->desc) };
        swi(MessageTrans_CloseFile, r);
    }
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module international_module = {
    .title = "International",
    .help = "International\t1.70 (06 Feb 2021) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .commands = commands,
};
