/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_international.c: the International module, native
 * (modules/international), and what it answers for: what must always
 * hold. Its behaviour against RISC OS 5.30's, reason by reason and
 * command by command, is tests/desktop/international (compare.py). The
 * values here are the farm's.
 *
 *   - Service_International names the alphabet, as !Chars asks for it
 *     (FNchars_getsysencoding), and Font_FindFont takes that name with \E.
 *     That is the call that failed with "Encoding not found" with no module.
 *   - Names and numbers both ways, a country's alphabet, a UCS table.
 *   - OS_Byte 71 sets the alphabet, which the module draws in the system
 *     font (OS_Word 10 reads it back). OS_Byte 70 sets a country, its
 *     alphabet and keyboard. *Alphabet names the alphabet.
 *   - The International Keyboard module types in the alphabet. A key's
 *     character comes from the alphabet's UCS table, and is nothing if the
 *     table has none.
 *
 * What it changes (alphabet, keyboard, country, font) it puts back.
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
#define KEYV 0x13u

static char out[256];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* A SWI on R0-R9: 0 or the error number */
static uint32_t swi(uint32_t number, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, number);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

/* Service_International: 1 if claimed, R3-R5 back */
static int intl(uint32_t reason, uint32_t *r3, uint32_t *r4, uint32_t *r5)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x43, s.r[2] = reason, s.r[3] = *r3, s.r[4] = *r4, s.r[5] = *r5;
    ros_service_call(&s);
    *r3 = s.r[3], *r4 = s.r[4], *r5 = s.r[5];
    return s.r[1] == 0;
}

static uint32_t osbyte(uint32_t a, uint32_t x, uint32_t y, uint32_t *r2)
{
    uint32_t r[10] = { a, x, y };
    swi(XOS_Byte, r);
    if (r2)
        *r2 = r[2];
    return r[1];
}

/* A character's rows from the system font (OS_Word 10) */
static void glyph(uint32_t block, uint32_t c, uint8_t rows[8])
{
    ros_st8(block, (uint8_t)c);
    uint32_t r[10] = { 10, block };
    swi(XOS_Word, r);
    memcpy(rows, ros_ptr(block + 1), 8);
}

static void vdu(uint32_t c)
{
    uint32_t r[10] = { c };
    swi(XOS_WriteC, r);
}

static void key(uint32_t k, int down)
{
    uint32_t r[10] = { down ? 2u : 1u, k, 0, 0, 0, 0, 0, 0, 0, KEYV };
    swi(XOS_CallAVector, r);
}

/* Keys a, b (0 none) held, then let go; what the keyboard buffer then
 * holds, up to n bytes; how many */
static unsigned type(uint32_t a, uint32_t b, uint8_t *got, unsigned n)
{
    key(a, 1);
    if (b)
        key(b, 1), key(b, 0);
    key(a, 0);
    unsigned k = 0;
    for (;;) {
        uint32_t r2, c = osbyte(129, 0, 0, &r2);
        if (r2 != 0)
            break;
        if (k < n)
            got[k] = (uint8_t)c;
        k++;
    }
    return k;
}

static const uint8_t font_A3_latin2[8] = { 0x60, 0x60, 0x60, 0x78, 0xE0, 0x60, 0x7E, 0x00 };
static const uint8_t font_A3_latin1[8] = { 0x1C, 0x36, 0x30, 0x7C, 0x30, 0x30, 0x7E, 0x00 };
static const uint8_t font_80_utf8[8] = { 0x06, 0x09, 0x09, 0x69, 0x96, 0x60, 0x90, 0x60 };

void ros_selftest_international(void)
{
    /* the buffer, OS_Word 10's block, a name, and the font: 224 characters of 8 rows */
    uint8_t *mem = ros_rma_alloc(512 + 224 * 8);
    if (!mem) {
        check(0, "International: memory for the test", NULL);
        return;
    }
    uint32_t a = ros_addr(mem), buf = a, word = a + 256, name = a + 320, font = a + 512;
    uint32_t country = osbyte(70, 127, 0, NULL), alphabet = osbyte(71, 127, 0, NULL);
    uint32_t keyboard = osbyte(71, 255, 0, NULL);
    for (uint32_t c = 32; c < 256; c++)
        glyph(word, c, ros_ptr(font + (c - 32) * 8));

    /* !Chars's FNchars_getsysencoding: OS_Byte 71's alphabet, named by
     * Service_International 3 into a buffer of 255, R5 its length; then
     * its Font_FindFont with \E and that name */
    uint32_t r3 = alphabet, r4 = buf, r5 = 255;
    int claimed = intl(3, &r3, &r4, &r5);
    ros_st8(buf + (r5 < 255 ? r5 : 255), 0);
    check(alphabet == 101 && claimed && r5 == 6 && !strcmp(ros_ptr(buf), "Latin1"),
          "Service_International 3 -- !Chars's alphabet name: Latin1, 6 characters",
          "alphabet %u, claimed %d, R5 %u, '%s'", alphabet, claimed, r5, (char *)ros_ptr(buf));
    strcpy(ros_ptr(name), "\\ELatin1\\FHomerton.Medium");
    uint32_t r[10] = { 0, name, 14 * 16, 14 * 16, 0, 0 };
    uint32_t e = swi(XFont_FindFont, r);
    check(e == 0, "Font_FindFont \"\\ELatin1\\FHomerton.Medium\" -- !Chars's font in the alphabet's encoding",
          "error &%X", e);
    if (!e) {
        uint32_t lose[10] = { r[0] };
        swi(XFont_LoseFont, lose);
    }

    /* Names and numbers both ways, a country's alphabet, and a UCS table */
    strcpy(ros_ptr(name), "latin2");
    r3 = name, r4 = 0, r5 = 0;
    int c1 = intl(1, &r3, &r4, &r5) && r4 == 102;
    strcpy(ros_ptr(name), "Fr.");
    r3 = name, r4 = 0;
    int c0 = intl(0, &r3, &r4, &r5) && r4 == 6;
    r3 = 6, r4 = 0;
    int c4 = intl(4, &r3, &r4, &r5) && r4 == 112;
    r3 = 49, r4 = buf, r5 = 4;
    int c2 = intl(2, &r3, &r4, &r5) && r5 == 4 && !memcmp(ros_ptr(buf), "Wale", 4);
    r3 = 102, r4 = 0;
    int c8 = intl(8, &r3, &r4, &r5) && r4 && ros_ld32(r4 + 0xA3 * 4) == 0x141 &&
             ros_ld32(r4 + 0x80 * 4) == 0x20AC;
    r3 = 1, r4 = buf, r5 = 8;
    int c9 = intl(9, &r3, &r4, &r5) && r5 == 2 && !memcmp(ros_ptr(buf), "uk", 2);
    check(c1 && c0 && c4 && c2 && c8 && c9,
          "Service_International 0-4, 8, 9 -- latin2 is 102, Fr. France (6), whose alphabet is "
          "Latin9 (112); 49 Wales2 cut to 4; Latin2's table (&A3 U+0141); the UK's code uk",
          "%d %d %d %d %d %d", c1, c0, c4, c2, c8, c9);

    /* OS_Byte 71 sets the alphabet: the module draws it in the system font */
    uint8_t rows[8];
    uint32_t r2, old = osbyte(71, 102, 0x55, &r2);
    glyph(word, 0xA3, rows);
    check(old == 101 && r2 == 5 && osbyte(71, 127, 0, NULL) == 102 &&
              !memcmp(rows, font_A3_latin2, 8),
          "OS_Byte 71,102 -- Latin2 selected (R1 the old, R2 5), &A3 drawn as L with stroke",
          "R1 %u R2 %u", old, r2);

    /* The keyboard types in it: Shift-3's pound is not in Latin2, so
     * nothing; AltGr-4's euro is &80 */
    uint8_t got[8] = { 0 };
    uint32_t flush[10] = { 21, 0 };
    swi(XOS_Byte, flush);
    unsigned pound = type(0x4C, 0x13, got, 8);
    unsigned euro = type(0x60, 0x14, got, 8);
    check(pound == 0 && euro == 1 && got[0] == 0x80,
          "KeyV in Latin2 -- Shift-3 (U+00A3, not in the alphabet) nothing, AltGr-4 (the euro) &80",
          "%u %u &%X", pound, euro, got[0]);

    /* *Alphabet names it */
    ros_vector_claim_native(WRCHV, wrch, 0);
    strcpy(ros_ptr(name), "Alphabet");
    outn = 0, out[0] = 0;
    uint32_t cl[10] = { name };
    e = swi(XOS_CLI, cl);
    ros_vector_release_native(WRCHV, wrch, 0);
    check(!e && !strcmp(out, "Latin2\n\r"), "*Alphabet -- Latin2", "'%s'", out);

    /* OS_Byte 70: a country, with its alphabet and keyboard; UTF8's top
     * half drawn as hex */
    old = osbyte(70, 32, 0, &r2);
    glyph(word, 0x80, rows);
    check(old == country && r2 == 5 && osbyte(70, 127, 0, NULL) == 32 &&
              osbyte(71, 127, 0, NULL) == 111 && osbyte(71, 255, 0, NULL) == 32 &&
              !memcmp(rows, font_80_utf8, 8),
          "OS_Byte 70,32 -- Japan: alphabet UTF8, keyboard Japan, &80 drawn as \"80\"",
          "R1 %u R2 %u", old, r2);

    /* Back as it was; Latin1's pound again */
    osbyte(71, alphabet, 0, NULL);
    osbyte(71, 0x80 | keyboard, 0, NULL);
    osbyte(240, country, 0, NULL);
    glyph(word, 0xA3, rows);
    check(osbyte(71, 127, 0, NULL) == alphabet && osbyte(71, 255, 0, NULL) == keyboard &&
              osbyte(70, 127, 0, NULL) == country && !memcmp(rows, font_A3_latin1, 8),
          "OS_Byte 71, 240 -- the alphabet, keyboard and country put back", NULL);
    for (uint32_t c = 32; c < 256; c++) {
        vdu(23), vdu(c);
        for (int i = 0; i < 8; i++)
            vdu(ros_ld8(font + (c - 32) * 8 + (uint32_t)i));
    }
    swi(XOS_Byte, flush);
    ros_rma_free(mem);
}
