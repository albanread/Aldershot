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

/* international.h: the International module, a native module.
 *
 * It has no SWIs. Its interface is Service_International (&43), which any
 * module may answer, and the kernel's OS_Byte 70 and 71, which issue it.
 * The tables declared below are RISC OS's own.
 * They are generated at build time from Internat/Inter by
 * tools/mkinternational.py, into international_tables.c in the build tree.
 */
#ifndef ROSGD_INTERNATIONAL_H
#define ROSGD_INTERNATIONAL_H

#include <stdint.h>

#include "rosgd/international.h"        /* Service_International and its reasons */

/* The alphabets that the module and its callers name by number */
#define ALPHABET_LATIN1         101u
#define ALPHABET_UTF8           111u

/* UCSTable: the codes from base to top. Their glyphs are in
 * intl_ucs_glyphs, starting at index first. */
struct intl_ucs_range {
    uint32_t base, top, first;
};

/* A name and its number */
struct intl_name {
    const char *name;
    int number;
};

/* FontPointers: an alphabet's number, and the index of its table in
 * intl_alphabet_ucs */
struct intl_number {
    int number, index;
};

/* FontTable, as assembled. A glyph is eight bytes, with the top row first. */
extern const uint8_t intl_font_table[];
extern const uint32_t intl_font_table_size;
/* UCSTable: the ranges in order, and the offset in FontTable of each code's
 * glyph (0xFFFF means none) */
extern const struct intl_ucs_range intl_ucs_ranges[];
extern const unsigned intl_ucs_range_count;
extern const uint16_t intl_ucs_glyphs[];
/* The alphabets' UCS tables, with 256 codes each (0xFFFFFFFF means none).
 * They are in FontPointers' order. FontPointers ends with -1. */
extern const uint32_t intl_alphabet_ucs[][256];
extern const struct intl_number intl_font_pointers[];
/* CountryStrings and AlphabetStrings turn names into numbers. CountryList
 * and AlphabetList turn numbers into names. Each table is searched in the
 * order given here. Each ends with a NULL name. */
extern const struct intl_name intl_country_strings[];
extern const struct intl_name intl_alphabet_strings[];
extern const struct intl_name intl_country_list[];
extern const struct intl_name intl_alphabet_list[];
/* CToATable: a country and its alphabet. The table ends with { 0, 0 }. */
extern const uint8_t intl_country_alphabet[][2];
/* Alpha2Strings: two characters for each country, 0 to 99 */
extern const char intl_alpha2[201];
/* The UTF8 alphabet's top half. Each character shows its code as two hex
 * digits. The low digit is at the top right and the high digit is below it
 * on the left. This is DoDefineUTF8's Digits: five rows of four bits for
 * each digit. */
extern const uint8_t intl_digits[16][5];

/* The module, for the ROM's list of native modules. */
extern struct ros_module international_module;

#endif
