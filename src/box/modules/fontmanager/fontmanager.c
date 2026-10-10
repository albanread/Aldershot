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
 * This file is a reimplementation in C of RISC OS Open's ARM assembler source
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts, s.Fonts01, s.Errors, s.FontsM, hdr.Font).
 */

/* fontmanager.c: the Font Manager, reimplemented as a native module.
 *
 * RISC OS 5's Font Manager (Video/Render/Fonts/FontManager) finds fonts
 * along Font$Path. It maps character codes through encodings, measures
 * strings and paints them from outlines or bitmaps. The sources used are
 * version 3.82's, matched to the behaviour of version 3.80, which is the
 * version in the farm's RISC OS 5.30.
 *
 * It is written again here from those sources. It is checked against the
 * 32-bit system on the farm (tests/vdu), result for result, with the
 * original's quirks included. The code marks the quirks it keeps on
 * purpose. The README lists what is not done yet.
 *
 * This file holds the module, its SWIs and state, and the errors.
 * catalogue.c has the fonts and encodings found along Font$Path (ListFonts
 * and DecodeMenu). encoding.c has the encodings. fonts.c has the handles
 * (FindFont, LoseFont and ReadDefn). files.c has the headers of the font
 * files.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "fm.h"

#define MODULE_VERSION 380

int32_t fm_xeig = 1, fm_yeig = 1, fm_log2bpp;
uint32_t fm_printerflag;                /* Service_Print's R2: non-zero while a page is drawn */
int32_t fm_xscalefactor = 400, fm_yscalefactor = 400;

/* the colours Font_CurrentFont and Font_FutureFont report */
struct fm_colours fm_current = { 0, 0, 0x80, 1, 0, 14 }, fm_future = { 0, 0, 0x80, 1, 0, 0 };

/* The anti-aliasing thresholds (defaultthresh).  There is one list for each
 * of 1 to 15 colours besides the background.  Each list ends in &FF. */
uint8_t fm_thresholds[135];
static const uint8_t default_thresh[135] = {
    5, 0xFF,
    4, 9, 0xFF,
    4, 8, 12, 0xFF,
    3, 6, 9, 12, 0xFF,
    3, 5, 8, 10, 13, 0xFF,
    3, 5, 8, 10, 12, 14, 0xFF,
    2, 4, 6, 8, 10, 12, 14, 0xFF,
    2, 4, 6, 8, 9, 11, 13, 15, 0xFF,
    2, 4, 5, 7, 8, 10, 11, 13, 14, 0xFF,
    2, 4, 5, 7, 8, 10, 11, 13, 14, 15, 0xFF,
    2, 4, 5, 7, 8, 9, 11, 12, 13, 14, 15, 0xFF,
    2, 3, 4, 6, 7, 8, 10, 11, 12, 13, 14, 15, 0xFF,
    2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 0xFF,
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0xFF,
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0xFF,
};

unsigned fm_thresh_offset(unsigned k)                      /* threshoffsets */
{
    return k * (k + 3) / 2;
}

/* ---- errors ---------------------------------------------------------------------------------------- */

static const char *err_text(uint32_t n)
{
    switch (n) {
    case FE_CANTKILL: return "Font Manager is in use";
    case FE_BUFFOVERFLOW: return "Buffer overflow";
    case FE_BADPARM: return "Bad parameters";
    case FE_NOROOM: return "Not enough memory for Font cache";
    case FE_CACHEFULL: return "The area of memory reserved for fonts is full. Quit any unwanted "
                              "applications or see the RISC OS User Guide for ways to maximise memory.";
    case FE_NOCACHE: return "No font cache present";
    case FE_TOOLONG: return "String too long";
    case FE_64K: return "Font definition too large";
    case FE_PALTOOBIG: return "Undefined font colour";
    case FE_BADTRANBITS: return "Invalid data passed to Font_SetTransfer";
    case FE_NOTENOUGHBITS: return "Invalid font colour";
    case FE_NOFONT: return "Undefined font handle";
    case FE_NOPIXELS: return "No pixel data for this font";
    case FE_BADFONTNUMBER: return "Font handle out of range";
    case FE_NOTFOUND: return "%0 font not found";
    case FE_BADFONTFILE: return "Illegal font file";
    case FE_NOHANDLES: return "No more font handles";
    case FE_BADCOUNTER: return "Fonts must be read sequentially";
    case FE_BADCTRLCHAR: return "Illegal control character in font string";
    case FE_SINUSE: return "Font manager in use";
    case FE_BADSEGMENT: return "Illegal line segment in outline font";
    case FE_BADPREFIX: return "%1 (while scanning Font$Path)";
    case FE_RESERVED: return "Reserved fields must be zero";
    case FE_BADCHARCODE: return "Character code out of range";
    case FE_NOBITMAPS: return "ROM font directory cannot contain bitmaps";
    case FE_NOBITMAPS2: return "Can't convert bitmap characters into outlines";
    case FE_BADCACHEFILE: return "Invalid font cache file";
    case FE_FIELDNOTFOUND: return "%0 field not present in font string";
    case FE_BADMATRIX: return "Invalid matrix passed to Font Manager";
    case FE_OVERFLOW: return "Number too big";
    case FE_DIVBY0: return "Division by zero";
    case FE_BADREADMETRICS: return "Font_ReadFontMetrics not allowed on a transformed font";
    case FE_BADRGB: return "Undefined RGB font colours";
    case FE_ENCNOTFOUND: return "%0 encoding not found";
    case FE_MUSTHAVESLASH: return "Identifier '%1' should be preceded by '/' in encoding '%0'";
    case FE_BADENCSIZE: return "Max total size of input and output encoding files is 16k";
    case FE_TOOMANYIDS: return "Too many identifiers in %0 encoding";
    case FE_TOOFEWIDS: return "Not enough identifiers in %0 encoding";
    case FE_NOBASEENC: return "Base encoding %0 not found";
    case FE_IDNOTFOUND: return "Identifier %1 not found in encoding %0";
    case FE_TOOMANYCHUNKS: return "Too many characters in %0";
    case FE_BADFONTFILE2: return "Illegal font file in %0";
    case FE_SUPREMACY: return "Supremacy blending not supported in this mode";
    case FE_NOPATHDOT: return "Font prefix is not a path";
    case FE_DATANOTFOUND: return "Font data not found";
    case FE_DATANOTFOUND2: return "No suitable font data for %0";
    default: return "Font Manager error";
    }
}

/* MyGenerateError_2: does MessageTrans's substitution of %0 and %1.  A
 * missing parameter is replaced by nothing. */
os_error *fm_err(uint32_t n, const char *p0, const char *p1)
{
    const char *t = err_text(n);
    char buf[252];
    size_t k = 0;
    for (; *t && k + 1 < sizeof buf; t++) {
        if (t[0] == '%' && (t[1] == '0' || t[1] == '1')) {
            const char *p = t[1] == '0' ? p0 : p1;
            for (; p && *p && k + 1 < sizeof buf; p++)
                buf[k++] = *p;
            t++;
            continue;
        }
        buf[k++] = *t;
    }
    buf[k] = 0;
    return ros_error(n & 0xFFFF, "%s", buf);
}

uint32_t fm_swi(uint32_t n, uint32_t r[10], os_error **e)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 10 * sizeof r[0]);
    *e = c.v ? ros_ptr(c.r[0]) : NULL;
    return c.c;
}

static void done(struct ros_cpu *s, os_error *e)
{
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

/* divide: floor division.  A negative quotient is rounded down as well.
 * b must be positive. */
int32_t fm_divide(int32_t a, int32_t b)
{
    if (a >= 0)
        return (int32_t)((uint32_t)a / (uint32_t)b);
    return ~(int32_t)((uint32_t)~a / (uint32_t)b);
}

/* setmodedata: switch output back from the rasteriser's sprite, then read
 * the VDU's variables as they are now.  They give where painting writes,
 * and the mode's depth and eigen factors. */
struct fm_vdu fm_vdu;

os_error *fm_mode_vars(void)
{
    static const uint32_t vars[] = { 148, 128, 129, 130, 131, 140, 141, 142, 143, 144, 145, 146,
                                     147, 3, 0, 9, 4, 5, 6, 12 };
    enum { N = sizeof vars / sizeof vars[0] };
    fm_restore_output();
    uint32_t in = ros_addr(ros_rma_alloc(4 * N + 4)), out = ros_addr(ros_rma_alloc(4 * N));
    for (unsigned i = 0; i < N; i++)
        ros_st32(in + 4 * i, vars[i]);
    ros_st32(in + 4 * N, 0xFFFFFFFFu);
    uint32_t r[10] = { in, out };
    os_error *e;
    fm_swi(XOS_ReadVduVariables, r, &e);
    if (!e) {
        int32_t v[N];
        for (unsigned i = 0; i < N; i++)
            v[i] = (int32_t)ros_ld32(out + 4 * i);
        fm_vdu.scrtop = (uint32_t)v[0];
        fm_vdu.gx0 = v[1], fm_vdu.gy0 = v[2], fm_vdu.gx1 = v[3], fm_vdu.gy1 = v[4];
        for (int i = 0; i < 8; i++)
            fm_vdu.cursors[i] = v[5 + i];
        fm_vdu.ncolour = (uint32_t)v[13], fm_vdu.modeflags = (uint32_t)v[14];
        fm_log2bpp = v[15];
        fm_depth(fm_log2bpp);
        fm_xeig = v[16], fm_yeig = v[17];
        fm_vdu.linelen = (uint32_t)v[18], fm_vdu.ywindlimit = v[19];
    }
    ros_rma_free(ros_ptr(in)), ros_rma_free(ros_ptr(out));
    return e;
}

/* dividex, dividey: convert millipoints to pixels, rounded down, and give
 * the quarter pixel as well */
void fm_dividex(int32_t v, int32_t *pix, uint8_t *sub)
{
    int32_t q = fm_divide((int32_t)((uint32_t)v << 2), (int32_t)((uint32_t)fm_xscalefactor << fm_xeig));
    *sub = (uint8_t)(q & 3), *pix = q >> 2;
}

void fm_dividey(int32_t v, int32_t *pix, uint8_t *sub)
{
    int32_t q = fm_divide((int32_t)((uint32_t)v << 2), (int32_t)((uint32_t)fm_yscalefactor << fm_yeig));
    *sub = (uint8_t)(q & 3), *pix = q >> 2;
}

/* calcxcoord: converts the pen (xco72, yco72) to pixels and quarter pixels.
 * The y value goes through a cache of one value. Only a mode change clears
 * it. The quarter pixel is not recalculated while the cache is used. It
 * is kept from the last y that was worked out. */
int32_t fm_xco72, fm_yco72, fm_xcoord, fm_ycoord, fm_oldxcoord, fm_oldycoord;
uint8_t fm_antialiasx, fm_antialiasy;
static int32_t old_yco72 = (int32_t)0x80000000;

void fm_calcxcoord(void)
{
    static uint32_t context;            /* the VDU's context number.  A change means a mode change */
    if (ros_vdu_context_id() != context) {
        context = ros_vdu_context_id();
        old_yco72 = (int32_t)0x80000000;
    }
    fm_dividex(fm_xco72, &fm_xcoord, &fm_antialiasx);
    if (fm_yco72 == old_yco72) {
        fm_ycoord = fm_oldycoord;
    } else {
        old_yco72 = fm_yco72;
        fm_dividey(fm_yco72, &fm_ycoord, &fm_antialiasy);
    }
    fm_oldxcoord = fm_xcoord, fm_oldycoord = fm_ycoord;
}

/* ---- the state SWIs -------------------------------------------------------------------------------- */

static void cache_addr(struct ros_cpu *s)
{
    s->r[0] = MODULE_VERSION;
    s->r[2] = fm_maxcache;              /* (ROSGD keeps no cache of its own size) */
    s->r[3] = 0;
    s->v = 0;
}

static void set_font(struct ros_cpu *s)
{
    uint32_t h = s->r[0];
    if (h == 0 || h >= 256) {
        ros_swi_fail(s, fm_err(FE_BADFONTNUMBER, NULL, NULL));
        return;
    }
    fm_currentfont = fm_futurefont = h;
    s->v = 0;
}

static void current_font(struct ros_cpu *s)
{
    s->r[0] = fm_currentfont, s->r[1] = fm_current.bcol, s->r[2] = fm_current.fcol,
    s->r[3] = fm_current.acol;
    s->v = 0;
}

static void future_font(struct ros_cpu *s)
{
    s->r[0] = fm_futurefont, s->r[1] = fm_future.bcol, s->r[2] = fm_future.fcol,
    s->r[3] = fm_future.acol;
    s->v = 0;
}

/* Font_ConverttoOS: converts millipoints to pixels, rounded down, using
 * calcxcoord and its cache of y.  The pixels are then converted to OS
 * units. */
static void convert_to_os(struct ros_cpu *s)
{
    os_error *e = fm_mode_vars();
    if (!e) {
        fm_xco72 = (int32_t)s->r[1], fm_yco72 = (int32_t)s->r[2];
        fm_calcxcoord();
        s->r[1] = (uint32_t)fm_xcoord << fm_xeig;
        s->r[2] = (uint32_t)fm_ycoord << fm_yeig;
    }
    done(s, e);
}

static void convert_to_points(struct ros_cpu *s)
{
    os_error *e = fm_mode_vars();
    if (!e) {
        int32_t x = (int32_t)(s->r[1] * (uint32_t)fm_xscalefactor << fm_xeig);
        int32_t y = (int32_t)(s->r[2] * (uint32_t)fm_yscalefactor << fm_yeig);
        s->r[1] = (uint32_t)(x >> fm_xeig);
        s->r[2] = (uint32_t)(y >> fm_yeig);
    }
    done(s, e);
}

static void read_thresholds(struct ros_cpu *s)
{
    uint32_t at = s->r[1];
    unsigned a = fm_current.acol >= 128 ? 256u - fm_current.acol : fm_current.acol;
    ros_st8(at++, fm_current.acol);
    for (unsigned i = 0; i < a + 2; i++)
        ros_st8(at++, fm_thresholds[fm_thresh_offset(a) + i]);
    s->v = 0;
}

/* settransfer: v is the offset from the first colour to the last, from -14
 * to 14.  It is a byte from Font_SetThresholds and a word from VDU 23,25.
 * The thresholds are at src. */
os_error *fm_settransfer(int32_t v, const uint8_t *src)
{
    int32_t k = v >= 128 ? 256 - v : v;
    if (k > 14)
        return fm_err(FE_BADTRANBITS, NULL, NULL);
    /* The original reuses R14 after setting it to setout_invalid (0).  So
     * whatever the count, one threshold goes into the two-colour table. */
    fm_outputvalid = 0;
    fm_current.acol = (uint8_t)v;
    fm_thresholds[0] = src[0];
    fm_thresholds[1] = 0xFF;
    return NULL;
}

static void set_thresholds(struct ros_cpu *s)
{
    uint32_t at = s->r[1];
    os_error *e = fm_settransfer((int32_t)ros_ld8(at), ros_ptr(at + 1));
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

static void set_font_max(struct ros_cpu *s)
{
    fm_maxcache = s->r[0];
    int changed = 0;
    for (int i = 0; i < 5; i++)
        if (fm_threshold[i] != (int32_t)s->r[1 + i])
            changed = 1;
    if (changed) {
        for (int i = 0; i < 5; i++)
            fm_threshold[i] = (int32_t)s->r[1 + i];
        fm_reset_fontmax();
    }
    s->v = 0;
}

static void read_font_max(struct ros_cpu *s)
{
    s->r[0] = fm_maxcache;
    for (int i = 0; i < 5; i++)
        s->r[1 + i] = (uint32_t)fm_threshold[i];
    s->r[6] = s->r[7] = 0;
    s->v = 0;
}

/* ---- Font_FindField, Font_ApplyFields ------------------------------------------------------------ */

/* findfield, reading arena memory as the original does.  This includes
 * reading past a lone '\'. */
static int find_field(uint32_t s, uint8_t c, uint32_t *at)
{
    uint32_t p = s;
    uint32_t term = ' ';
    if (ros_ld8(p) != '\\') {
        if (c == 'F') {
            *at = s;
            return 1;
        }
    } else {
        p++;
        goto field;
    }
    for (;;) {
        uint32_t ch = ros_ld8(p++);
        if (ch == '\\') {
field:
            term = ' ' - 1;
            if (ros_ld8(p++) == c) {
                *at = p;
                return 1;
            }
            continue;
        }
        if (ch <= term)
            return 0;
    }
}

static void find_field_swi(struct ros_cpu *s)
{
    uint32_t at;
    if (find_field(s->r[1], (uint8_t)s->r[2], &at))
        s->r[1] = at;
    else
        s->r[2] = 0;
    s->v = 0;
}

/* findmatchingfield: is the field at src also in test?  If so, *test is
 * updated and *term is the terminator for the copy.  &10000 is added to
 * *term if the field is null, which means nothing is copied. */
static int matching_field(uint32_t src, uint32_t *test, uint32_t *term)
{
    uint32_t start = *test;
    uint8_t q = 'F';
    if (ros_ld8(src) == '\\')
        q = (uint8_t)ros_ld8(src + 1), *term = 31;
    else
        *term = 32;
    uint32_t at;
    if (ros_ld8(*test) < 33 || !find_field(*test, q, &at))
        return 0;
    *test = at;
    if (at > start)
        *term = 31, *test = at - 2;
    else
        *term = 32;
    uint32_t c = ros_ld8(at);
    if (c == '\\' || c <= *term)
        *term += 0x10000;
    return 1;
}

/* copyfield: copies into the buffer at *out.  If *out is 0 it only counts.
 * *size is the room left, or the count. */
static os_error *copy_field(uint32_t src, uint32_t *out, uint32_t *size, uint32_t term)
{
    uint32_t stop = 0;
    for (;;) {
        uint32_t c = ros_ld8(src++);
        if (c == stop || (int32_t)c <= (int32_t)term)
            return NULL;
        if (!*out) {
            (*size)++;
        } else {
            if ((int32_t)--*size <= 0)
                return fm_err(FE_BUFFOVERFLOW, NULL, NULL);
            ros_st8((*out)++, c);
        }
        stop = '\\';
    }
}

static uint32_t skip_field(uint32_t p)
{
    uint32_t term = ros_ld8(p) == '\\' ? 31 : 32, stop = 0;
    for (;;) {
        uint32_t c = ros_ld8(p++);
        if (c == stop || c <= term)
            return p - 1;
        stop = '\\';
    }
}

static void apply_fields(struct ros_cpu *s)
{
    uint32_t a = s->r[0], b = s->r[1], out = s->r[2], size = s->r[3], term;
    os_error *e = NULL;
    while (!e && ros_ld8(a) > 32) {
        uint32_t t = b;
        e = copy_field(matching_field(a, &t, &term) ? t : a, &out, &size, term);
        a = skip_field(a);
    }
    uint32_t p = b;
    while (!e && ros_ld8(p) > 32) {
        uint32_t t = s->r[0];
        if (!matching_field(p, &t, &term))
            e = copy_field(p, &out, &size, term);
        p = skip_field(p);
    }
    s->r[3] = size;
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    if (out)
        ros_st8(out, 0);
    s->r[0] = p;
    s->v = 0;
}

/* ---- Font_ChangeArea --------------------------------------------------------------------------------- */

static void change_area(struct ros_cpu *s)
{
    s->r[2] = 0;                        /* the minimum size is 0, because ROSGD manages its own cache */
    s->v = 0;
}

/* ---- the SWI table ------------------------------------------------------------------------------------- */

void fm_reset_fontmax(void)
{
    for (uint32_t h = 1; h < 256; h++)
        if (fm_fonts[h])
            fm_set_thresholds(fm_fonts[h]);
    /* The original also decides again which data each font uses.  That is
     * not done here. */
}

#define RAW(name, fn) void ros_thunk_Font_##name(struct ros_cpu *s) { fn(s); }
RAW(CacheAddr, cache_addr)
RAW(FindFont, fm_thunk_FindFont)
RAW(LoseFont, fm_thunk_LoseFont)
RAW(ReadDefn, fm_thunk_ReadDefn)
RAW(ReadInfo, fm_thunk_ReadInfo)
RAW(StringWidth, fm_thunk_StringWidth)
RAW(Paint, fm_thunk_Paint)
RAW(Caret, fm_thunk_Caret)
RAW(ConverttoOS, convert_to_os)
RAW(Converttopoints, convert_to_points)
RAW(SetFont, set_font)
RAW(CurrentFont, current_font)
RAW(FutureFont, future_font)
RAW(FindCaret, fm_thunk_FindCaret)
RAW(CharBBox, fm_thunk_CharBBox)
void ros_thunk_Font_ReadScaleFactor(struct ros_cpu *s)
{
    s->r[1] = (uint32_t)fm_xscalefactor, s->r[2] = (uint32_t)fm_yscalefactor, s->v = 0;
}
void ros_thunk_Font_SetScaleFactor(struct ros_cpu *s)
{
    fm_xscalefactor = (int32_t)s->r[1], fm_yscalefactor = (int32_t)s->r[2], s->v = 0;
}
RAW(ListFonts, fm_thunk_ListFonts)
RAW(SetFontColours, fm_thunk_SetFontColours)
RAW(SetPalette, fm_thunk_SetPalette)
RAW(ReadThresholds, read_thresholds)
RAW(SetThresholds, set_thresholds)
RAW(FindCaretJ, fm_thunk_FindCaretJ)
RAW(StringBBox, fm_thunk_StringBBox)
RAW(ReadColourTable, fm_thunk_ReadColourTable)
RAW(MakeBitmap, fm_thunk_MakeBitmap)
RAW(UnCacheFile, fm_thunk_UnCacheFile)
RAW(SetFontMax, set_font_max)
RAW(ReadFontMax, read_font_max)
RAW(ReadFontPrefix, fm_thunk_ReadFontPrefix)
RAW(SwitchOutputToBuffer, fm_thunk_SwitchOutputToBuffer)
RAW(ReadFontMetrics, fm_thunk_ReadFontMetrics)
RAW(DecodeMenu, fm_thunk_DecodeMenu)
RAW(ScanString, fm_thunk_ScanString)
RAW(SetColourTable, fm_thunk_SetColourTable)
RAW(CurrentRGB, fm_thunk_CurrentRGB)
RAW(FutureRGB, fm_thunk_FutureRGB)
RAW(ReadEncodingFilename, fm_thunk_ReadEncodingFilename)
RAW(FindField, find_field_swi)
RAW(ApplyFields, apply_fields)
RAW(LookupFont, fm_thunk_LookupFont)
RAW(EnumerateCharacters, fm_thunk_EnumerateCharacters)
RAW(ChangeArea, change_area)
#undef RAW

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(FE_NOSUCHSWI, "SWI value out of range for module FontManager");
}

/* ---- the module ----------------------------------------------------------------------------------------- */

/* The configured FontMax values.  They come from CMOS RAM, read as the
 * original reads it.  Each byte is EORed with the value that zero stands
 * for.  ROSGD has no CMOS RAM, so the values are RISC OS 5's defaults. */
static void read_cmos(void)
{
    static const struct { uint8_t loc, zero, def; } cmos[6] = {
        { 0xC8, 0, 64 }, { 0xC9, 16, 16 }, { 0xCA, 12, 36 }, { 0xCB, 24, 36 }, { 0xCC, 0, 16 },
        { 0xCD, 0, 0 },
    };
    uint32_t v[6];
    for (int i = 0; i < 6; i++) {
        uint32_t r[10] = { 161, cmos[i].loc };
        os_error *e;
        fm_swi(XOS_Byte, r, &e);
        v[i] = e ? cmos[i].def : (r[2] & 0xFF) ^ cmos[i].zero;
    }
    fm_maxcache = v[0] << 16;
    for (int i = 0; i < 5; i++)
        fm_threshold[i] = (int32_t)v[1 + i];
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    switch (s->r[1]) {
    case 0x41:                          /* Print: R2 is 0 or -1.  Non-zero means the driver wants Font_Paint */
        fm_printerflag = s->r[2];
        break;
    case 0x46:                          /* ModeChange: the cached y value is no longer valid */
        old_yco72 = (int32_t)0x80000000;
        break;
    default:
        break;
    }
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    memcpy(fm_thresholds, default_thresh, sizeof fm_thresholds);
    read_cmos();
    uint32_t r[10];
    os_error *e;
    static const char *const commands[] = {
        "IF \"<Font$Path>\"=\"\" THEN SetMacro Font$Path <Font$Prefix>.",
        "Set File$Type_FCF FontCache",
        "Set Alias$@RunType_FCF LoadFontCache ",
        "Set Alias$@LoadType_FCF LoadFontCache ",
    };
    for (unsigned i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        uint32_t a = ros_addr(ros_rma_alloc((uint32_t)strlen(commands[i]) + 1));
        strcpy(ros_ptr(a), commands[i]);
        r[0] = a;
        fm_swi(XOS_CLI, r, &e);         /* (errors are ignored) */
        ros_rma_free(ros_ptr(a));
    }
    return fm_vdu_hooks_claim();
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    for (uint32_t h = 1; h < 256; h++)
        if (fm_fonts[h] && fm_fonts[h]->usage)
            return fm_err(FE_CANTKILL, NULL, NULL);
    fm_vdu_hooks_release();
    return NULL;
}

struct ros_module fontmanager_module = {
    .title = "FontManager",
    .help = "Font Manager\t3.80 (20 Jul 2020) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x40080,
    .swi_thunks = ros_swi_thunks_FontManager,
    .swi_names = ros_swi_names_FontManager,
    .swi_prefix = "Font",
};

__attribute__((constructor)) static void count(void)
{
    fontmanager_module.swi_count = ros_swi_count_FontManager;
}
