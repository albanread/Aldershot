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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts02, s.Fonts01).
 */

/* scan.c: measuring strings.
 *
 * This is s/Fonts02's scanstring and the routines it calls, and the SWIs
 * of s/Fonts01 that use it: Font_StringWidth, Font_FindCaret,
 * Font_FindCaretJ, Font_StringBBox and Font_ScanString.
 *
 * A string is read one unit at a time. A unit is 8, 16 or 32 bits, as the
 * plot type says. If the font's encoding is "utf8", the units are UTF-8
 * or UTF-16. Each printable character is mapped to the font's glyph, and
 * its advance is added to the pen. The advance is the metrics' width,
 * scaled and transformed, plus the space or letter spacing, plus the kern
 * with the character that follows.
 *
 * A scan stops at the terminator, at the limit of its box, or at its
 * length limit. It returns the last split point before that. When
 * finding the caret it returns the point nearest the target instead.
 * Control sequences change the font, the matrix and the future colours,
 * and move the pen.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/swi.h"
#include "fm.h"

#define BIGNUM 0x20000000

enum {
    ST_COORDBLK = 1 << 5, ST_MATRIX = 1 << 6, ST_LENGTH = 1 << 7, ST_FONT = 1 << 8,
    ST_KERN = 1 << 9, ST_RL = 1 << 10, ST_16BIT = 1 << 12, ST_32BIT = 1 << 13,
    ST_FINDCARET = 1 << 17, ST_RTNBBOX = 1 << 18, ST_RTNMATRIX = 1 << 19, ST_RTNSPLIT = 1 << 20,
    ST_RESERVED = 0x0F | 1 << 4 | 1 << 11 | 7 << 14 | 0xFFE00000u,
};

/* the string's state (workspace), shared with painting */
int32_t fm_xspaceadd, fm_yspaceadd, fm_xletteradd, fm_yletteradd, fm_xglueadd, fm_yglueadd;
uint32_t fm_extcode;                            /* externalcharcode */
static int32_t currbbox[4], scanbbox[4];
struct fm_future fm_tfuture;
int fm_futurechanged;

/* ---- reading units ------------------------------------------------------------------------------- */

int fm_width(void)                          /* 0: 8-bit, 1: 16-bit, 2: 32-bit */
{
    uint32_t w = fm_plottype & (ST_16BIT | ST_32BIT);
    return w == 0 ? 0 : w == ST_16BIT ? 1 : 2;
}

uint32_t fm_read_uint(uint32_t *p)          /* readuint1 */
{
    uint32_t v;
    switch (fm_width()) {
    case 0: v = ros_ld8(*p), *p += 1; break;
    case 1: v = ros_ld16(*p), *p += 2; break;
    default: v = ros_ld32(*p), *p += 4; break;
    }
    return v;
}

int32_t fm_read_int(uint32_t *p)            /* readint1 */
{
    switch (fm_width()) {
    case 0: return (int8_t)fm_read_uint(p);
    case 1: return (int16_t)fm_read_uint(p);
    default: return (int32_t)fm_read_uint(p);
    }
}

int32_t fm_read_int3(uint32_t *p)           /* readint3 */
{
    uint32_t a = *p, v;
    switch (fm_width()) {
    case 0:
        v = ros_ld8(a) | ros_ld8(a + 1) << 8 | ros_ld8(a + 2) << 16;
        *p += 3;
        return (int32_t)(v << 8) >> 8;
    case 1:
        v = ros_ld16(a) | ros_ld16(a + 2) << 16;
        *p += 4;
        return (int32_t)v;
    default:
        *p += 4;
        return (int32_t)ros_ld32(a);
    }
}

uint32_t fm_read_rgb(uint32_t *p)           /* readRGB: &BBGGRR00 */
{
    uint32_t a = *p;
    switch (fm_width()) {
    case 0:
        *p += 3;
        return ros_ld8(a) << 8 | ros_ld8(a + 1) << 16 | ros_ld8(a + 2) << 24;
    case 1:
        *p += 4;
        return ros_ld16(a) | ros_ld16(a + 2) << 16;
    default:
        *p += 4;
        return ros_ld32(a);
    }
}

/* readnextchar: a character, UTF-8 or UTF-16 decoded if the current
 * font's encoding is "utf8" */
os_error *fm_next_char(uint32_t *pp, uint32_t *out)
{
    uint32_t p = *pp, c = fm_read_uint(&p);
    if (c > 0x7F) {
        struct fm_font *f;
        os_error *e = fm_font_ptr(fm_currentfont, &f);
        if (e)
            return e;
        static const uint8_t utf8[8] = "utf8";
        int w = fm_width();
        if (!memcmp(f->encoding, utf8, 8) && w < 2) {
            if (w == 0) {
                if (!(c & 0x40) || c >= 0xFE) {
                    c = 0xFFFD;
                } else {
                    uint32_t min = 0x80, ctl = 0x800;
                    c &= ~0xC0u;
                    for (;;) {
                        uint32_t b = ros_ld8(p++);
                        if ((b & 0xC0) != 0x80) {
                            p--;
                            c = 0xFFFD;
                            break;
                        }
                        c = (c << 6) | (b & 0x3F);
                        if (c & 0x80000000u) {
                            c = 0xFFFD;
                            break;
                        }
                        if (!(c & ctl)) {
                            if (c < min)
                                c = 0xFFFD;
                            break;
                        }
                        c &= ~ctl;
                        min = ctl;
                        ctl <<= 5;
                    }
                }
            } else if (c >= 0xD800 && c < 0xDC00) {
                uint32_t v = ros_ld16(p) - 0xDC00;
                p += 2;
                if (v < 0x400) {
                    c = ((c - 0xD800) << 10) + v + 0x10000;
                } else {
                    p -= 2;
                    c = 0xFFFD;
                }
            } else if (c >= 0xDC00 && c < 0xE000) {
                c = 0xFFFD;
            }
        }
    }
    *pp = p;
    *out = c;
    return NULL;
}

/* ---- widths ------------------------------------------------------------------------------------- */

static void transform_width(int32_t *x, int32_t *y)      /* transformwidth */
{
    const struct fm_mat *m = fm_met.matrix;
    fm_transform_pt(m, x, y);
    *x = (int32_t)((uint32_t)*x - (uint32_t)m->v[4]);
    *y = (int32_t)((uint32_t)*y - (uint32_t)m->v[5]);
}

static const uint8_t skip[3][32] = {
    { [9] = 4, [11] = 4, [17] = 2, [18] = 4, [19] = 8, [25] = 3 },
    { [9] = 6, [11] = 6, [17] = 4, [18] = 8, [19] = 12, [25] = 6 },
    { [9] = 8, [11] = 8, [17] = 8, [18] = 16, [19] = 16, [25] = 12 },
};

/* getgluewidth: the glue between this character (g) and the next one. It
 * is the letter spacing plus the kern of g with the next printable
 * character. Control sequences in between are skipped. */
static os_error *glue(int32_t g, uint32_t p)
{
    fm_xglueadd = fm_xletteradd, fm_yglueadd = fm_yletteradd;
    if (!(fm_plottype & ST_KERN))
        return NULL;
    os_error *e;
    struct fm_font *f, *m;
    int32_t next;
    for (;;) {
        uint32_t q = p, c;
        if ((e = fm_next_char(&q, &c)))
            return e;
        if (c >= 32) {
            if ((e = fm_font_ptr(fm_currentfont, &f)) || (e = fm_map_char(f, (int32_t)c, &next)))
                return e;
            break;
        }
        uint8_t s = skip[fm_width()][c];
        if (s) {
            p += s;
            continue;
        }
        if (c != 21)
            return NULL;
        fm_read_uint(&p);
        do
            c = fm_read_uint(&p);
        while (c >= 32);
    }
    /* The original means to swap the pair for right to left text. But it
     * tests bit 10 of R14 after a BL, which is whatever mapchar left
     * there and not the plot type. So the pair is swapped only when bit
     * 10 of that value is set. The value is the next character's offset
     * within its run of the mapping, or the high part of its table entry
     * (fm_map_r14 models it). */
    int32_t l = g, r = next;
    if (fm_map_r14 & ST_RL)
        l = next, r = g;
    const uint8_t *blk;
    if ((e = fm_font_ptr(fm_currentfont, &f)) || (e = fm_font_ptr(f->masterfont, &m)) ||
        (e = fm_kerns(m, &blk)))
        return e;
    int32_t kx, ky;
    if (!blk || !fm_kern_pair(blk, (uint32_t)l, (uint32_t)r, &kx, &ky))
        return NULL;
    fm_scale_width(&kx, &ky);
    if (fm_met.matrix)
        transform_width(&kx, &ky);
    fm_xglueadd = (int32_t)((uint32_t)fm_xglueadd + (uint32_t)kx);
    fm_yglueadd = (int32_t)((uint32_t)fm_yglueadd + (uint32_t)ky);
    return NULL;
}

/* getcharwidth: this character's advance, and the glue after it */
os_error *fm_char_width(int32_t g, uint32_t next, int32_t *w, int32_t *h)
{
    os_error *e;
    if (!fm_met.valid && (e = fm_set_metrics()))
        return e;
    int32_t x = 0, y = 0;
    if (!fm_met.mapsize || (uint32_t)g < fm_met.mapsize) {
        uint32_t idx = fm_met.mapsize ? fm_met.map[g] : (uint32_t)g;
        int valid = idx < fm_met.nchars;        /* else the arrays have no entry for it */
        if (fm_met.xoff && valid)
            x = (int16_t)(fm_met.xoff[2 * idx] | fm_met.xoff[2 * idx + 1] << 8);
        else if (fm_met.misc && !fm_met.xoff)
            x = (int16_t)(fm_met.misc[8] | fm_met.misc[9] << 8);
        if (fm_met.yoff && valid)
            y = (int16_t)(fm_met.yoff[2 * idx] | fm_met.yoff[2 * idx + 1] << 8);
        fm_scale_width(&x, &y);
    }
    if (fm_met.matrix)
        transform_width(&x, &y);
    if (fm_extcode == 32) {
        x = (int32_t)((uint32_t)x + (uint32_t)fm_xspaceadd);
        y = (int32_t)((uint32_t)y + (uint32_t)fm_yspaceadd);
    }
    if ((e = glue(g, next)))
        return e;
    *w = x, *h = y;
    return NULL;
}

/* ---- the bounding box ---------------------------------------------------------------------------- */

static int32_t s16at(const uint8_t *a, uint32_t i) { return (int16_t)(a[2 * i] | a[2 * i + 1] << 8); }

static int32_t add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

/* computebbox: the character's box at (ox, oy) into currbbox */
static os_error *char_box(int32_t g, int32_t ox, int32_t oy)
{
    int32_t b[4] = { 0, 0, 0, 0 };
    if (!fm_met.bbox[0]) {
        os_error *e = fm_outline_metrics_bbox(g, b);
        if (e)
            return e;
    } else if (fm_met.matrix) {
        if (!fm_met.mapsize || (uint32_t)g < fm_met.mapsize) {
            uint32_t i = fm_met.mapsize ? fm_met.map[g] : (uint32_t)g;
            if (i >= fm_met.nchars)
                i = 0;
            int32_t x1 = s16at(fm_met.bbox[2], i), y1 = s16at(fm_met.bbox[3], i);
            fm_scale_width(&x1, &y1);
            int32_t x0 = s16at(fm_met.bbox[0], i), y0 = s16at(fm_met.bbox[1], i);
            fm_scale_width(&x0, &y0);
            b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1;
        }
        fm_transform_box(fm_met.matrix, b);
    } else if (!fm_met.mapsize || (uint32_t)g < fm_met.mapsize) {
        /* edge by edge */
        uint32_t i = fm_met.mapsize ? fm_met.map[g] : (uint32_t)g;
        if (i >= fm_met.nchars)
            i = 0;
        for (int k = 0; k < 4; k++) {
            int32_t v = s16at(fm_met.bbox[k], i);
            if (v)
                v = k & 1 ? fm_scale_y(v) : fm_scale_x(v);
            v = add(k & 1 ? oy : ox, v);
            if (k < 2 ? v < currbbox[k] : v > currbbox[k])
                currbbox[k] = v;
        }
        return NULL;
    }
    /* computebbox_update */
    int32_t x0 = add(ox, b[0]), y0 = add(oy, b[1]), x1 = add(ox, b[2]), y1 = add(oy, b[3]);
    if (x0 < currbbox[0]) currbbox[0] = x0;
    if (y0 < currbbox[1]) currbbox[1] = y0;
    if (x1 > currbbox[2]) currbbox[2] = x1;
    if (y1 > currbbox[3]) currbbox[3] = y1;
    return NULL;
}

/* addwidth: the pen past this character */
static os_error *add_width(int32_t g, uint32_t next, int32_t *x, int32_t *y)
{
    int32_t w, h, ox, oy, nx, ny;
    os_error *e;
    if (!(fm_plottype & ST_RL)) {
        ox = add(*x, fm_xglueadd), oy = add(*y, fm_yglueadd);
        if ((e = fm_char_width(g, next, &w, &h)))
            return e;
        nx = add(ox, w), ny = add(oy, h);
    } else {
        int32_t tx = (int32_t)((uint32_t)*x - (uint32_t)fm_xglueadd);
        int32_t ty = (int32_t)((uint32_t)*y - (uint32_t)fm_yglueadd);
        if ((e = fm_char_width(g, next, &w, &h)))
            return e;
        nx = (int32_t)((uint32_t)tx - (uint32_t)w), ny = (int32_t)((uint32_t)ty - (uint32_t)h);
        ox = nx, oy = ny;
    }
    *x = nx, *y = ny;
    if (fm_plottype & ST_RTNBBOX)
        return char_box(g, ox, oy);
    return NULL;
}

/* ---- scanstring ---------------------------------------------------------------------------------- */

static int32_t iabs(int32_t v) { return v < 0 ? (int32_t)(0u - (uint32_t)v) : v; }

/* Scan the string at p. Returns the parameters at the point where the scan
 * stopped. */
os_error *fm_scan_string(uint32_t p, int32_t limx, int32_t limy, int32_t split, int32_t maxidx,
                             int32_t spx, int32_t spy, struct fm_pars *out)
{
    uint32_t savefont = fm_currentfont, start = p;
    fm_inscanstring = 1;
    fm_xspaceadd = spx, fm_yspaceadd = spy;
    int32_t bx0, by0, bx1, by1;
    if (limx < 0) bx0 = limx, bx1 = BIGNUM; else bx0 = -BIGNUM, bx1 = limx;
    if (limy < 0) by0 = limy, by1 = BIGNUM; else by0 = -BIGNUM, by1 = limy;
    fm_tfuture.c = fm_current, fm_tfuture.font = fm_currentfont;
    fm_future = fm_current, fm_futurefont = fm_currentfont;
    fm_futurechanged = 0;
    currbbox[0] = currbbox[1] = scanbbox[0] = scanbbox[1] = BIGNUM;
    currbbox[2] = currbbox[3] = scanbbox[2] = scanbbox[3] = -BIGNUM;
    int32_t best = BIGNUM;
    fm_met.valid = 0;
    int32_t x = 0, y = 0, n = 0;
    fm_xglueadd = fm_yglueadd = 0;
    fm_oldpaintmatrix = FM_NOMATRIX;
    struct fm_pars saved = { p, 0, 0, 0 };
    os_error *e = NULL;
    struct fm_font *f;
    for (;;) {
        int32_t idx = (int32_t)(p - start);
        if (fm_plottype & ST_FINDCARET) {
            if (idx > maxidx)
                break;
            int32_t cx = x, cy = y;
            if (!(fm_plottype & ST_RL))
                cx = add(cx, fm_xglueadd >> 1), cy = add(cy, fm_yglueadd >> 1);
            else
                cx = add(cx, -(fm_xglueadd >> 1)), cy = add(cy, -(fm_yglueadd >> 1));
            int32_t d = add(iabs(add(limx, -cx)), iabs(add(limy, -cy)));
            if (d < best) {
                best = d;
                saved = (struct fm_pars){ p, x, y, n };
                memcpy(scanbbox, currbbox, sizeof scanbbox);
            }
        } else {
            if (!(bx0 <= x && by0 <= y && x <= bx1 && y <= by1) || idx > maxidx)
                break;
            uint32_t q = p, c;
            if ((e = fm_next_char(&q, &c)))
                break;
            if (c == 0 || c == 10 || c == 13 || c == (uint32_t)split || split == -1) {
                saved = (struct fm_pars){ p, x, y, n };
                if (fm_futurechanged) {
                    fm_future = fm_tfuture.c, fm_futurefont = fm_tfuture.font;
                    fm_futurechanged = 0;
                }
                memcpy(scanbbox, currbbox, sizeof scanbbox);
            }
        }
        uint32_t c;
        int32_t g;
        if ((e = fm_next_char(&p, &c)))
            break;
        fm_extcode = c;
        if (c >= 32) {
            if ((e = fm_font_ptr(fm_currentfont, &f)) || (e = fm_map_char(f, (int32_t)c, &g)))
                break;
            if (g == -1)
                continue;
            if (c == (uint32_t)split || split == -1)
                n++;
            if ((e = add_width(g, p, &x, &y)))
                break;
            continue;
        }
        /* scanctrl */
        if (c == 0 || c == 10 || c == 13)
            break;
        switch (c) {
        case 21:                                           /* comment */
            while (fm_read_uint(&p) >= 32)
                ;
            continue;
        case 17: {                                         /* colour */
            uint32_t v = fm_read_uint(&p);
            if ((int32_t)v < 0x80)
                fm_tfuture.c.fcol = (uint8_t)(v & 0x7F);
            else
                fm_tfuture.c.bcol = (uint8_t)(v & 0x7F);
            fm_futurechanged = 1;
            continue;
        }
        case 18:                                           /* colours and offset */
            fm_tfuture.c.bcol = (uint8_t)fm_read_uint(&p);
            fm_tfuture.c.fcol = (uint8_t)fm_read_uint(&p);
            fm_tfuture.c.acol = (uint8_t)fm_read_int(&p);
            fm_futurechanged = 1;
            continue;
        case 26:                                           /* font */
            fm_currentfont = fm_tfuture.font = fm_read_uint(&p) & 0xFF;
            fm_futurechanged = 1;
            fm_oldpaintmatrix = FM_NOMATRIX;
            if ((e = fm_set_metrics()))
                break;
            continue;
        case 25:                                           /* underline */
            fm_read_int(&p);
            fm_read_uint(&p);
            continue;
        case 9:
            x = add(x, fm_read_int3(&p));
            continue;
        case 11:
            y = add(y, fm_read_int3(&p));
            continue;
        case 19:                                           /* RGB colours */
            fm_futurechanged = 1;
            fm_tfuture.c.rgb_b = fm_read_rgb(&p);
            fm_tfuture.c.rgb_f = fm_read_rgb(&p);
            fm_tfuture.c.rgb_a = fm_read_uint(&p);
            continue;
        case 27: {                                         /* a matrix, no translation */
            p = ((p + 3) & ~3u) + 16;
            for (int i = 0; i < 4; i++)
                fm_paintmatrixbuffer[i] = (int32_t)ros_ld32(p - 16 + 4 * i);
            fm_paintmatrixbuffer[4] = fm_paintmatrixbuffer[5] = 0;
            if (fm_oldpaintmatrix == (const uint8_t *)fm_paintmatrixbuffer)
                fm_oldpaintmatrix = FM_NOMATRIX;
            fm_set_paint_matrix((const uint8_t *)fm_paintmatrixbuffer);
            fm_met.valid = 0;
            continue;
        }
        case 28:                                           /* a matrix */
            p = ((p + 3) & ~3u) + 24;
            fm_set_paint_matrix(ros_ptr(p - 24));
            fm_met.valid = 0;
            continue;
        default:
            e = fm_err(FE_BADCTRLCHAR, NULL, NULL);
            break;
        }
        break;
    }
    fm_currentfont = savefont;
    fm_inscanstring = 0;
    if (e)
        return e;
    *out = saved;
    return NULL;
}

/* ---- the SWIs ---------------------------------------------------------------------------------------- */

static int fail(struct ros_cpu *s, os_error *e)
{
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
    return !e;
}

static void results(struct ros_cpu *s, uint32_t start, const struct fm_pars *r)
{
    s->r[1] = r->p;
    s->r[2] = (uint32_t)r->x, s->r[3] = (uint32_t)r->y, s->r[4] = (uint32_t)r->n;
    s->r[5] = r->p - start;
}

void fm_thunk_StringWidth(struct ros_cpu *s)
{
    fm_xletteradd = fm_yletteradd = 0;
    fm_plottype = 0;
    fm_paintmatrix = NULL;
    int32_t lx = (int32_t)s->r[2], ly = (int32_t)s->r[3];
    struct fm_pars r;
    os_error *e = fm_scan_string(s->r[1], lx < 0 ? 0 : lx, ly < 0 ? 0 : ly, (int32_t)s->r[4],
                              (int32_t)s->r[5], 0, 0, &r);
    if (fail(s, e))
        results(s, s->r[1], &r);
}

/* findcaret */
static void find_caret(struct ros_cpu *s, int32_t jx, int32_t jy)
{
    fm_xletteradd = fm_yletteradd = 0;
    fm_plottype = ST_FINDCARET;
    fm_paintmatrix = NULL;
    struct fm_pars r;
    os_error *e = fm_scan_string(s->r[1], (int32_t)s->r[2], 0, -1, BIGNUM, jx, jy, &r);
    if (fail(s, e))
        results(s, s->r[1], &r);
}

void fm_thunk_FindCaret(struct ros_cpu *s) { find_caret(s, 0, 0); }
void fm_thunk_FindCaretJ(struct ros_cpu *s) { find_caret(s, (int32_t)s->r[4], 0); }

void fm_thunk_StringBBox(struct ros_cpu *s)
{
    fm_plottype = ST_RTNBBOX;
    fm_xletteradd = fm_yletteradd = 0;
    fm_paintmatrix = NULL;
    struct fm_pars r;
    os_error *e = fm_scan_string(s->r[1], BIGNUM, BIGNUM, -1, BIGNUM, 0, 0, &r);
    if (!fail(s, e))
        return;
    int null = scanbbox[0] > scanbbox[2];
    for (int i = 0; i < 4; i++)
        s->r[1 + i] = null ? 0 : (uint32_t)scanbbox[i];
}

void fm_thunk_ScanString(struct ros_cpu *s)
{
    uint32_t flags = s->r[2], reserved = ST_RESERVED;
    if (!(flags & ST_COORDBLK))
        reserved |= ST_RTNBBOX;
    if (flags & reserved) {
        ros_swi_fail(s, fm_err(FE_RESERVED, NULL, NULL));
        return;
    }
    fm_plottype = flags;
    uint32_t save = fm_currentfont;
    if ((flags & ST_FONT) && s->r[0])
        fm_currentfont = s->r[0] & 0xFF;
    int32_t cb[5] = { 0, 0, 0, 0, -1 };
    if (flags & ST_COORDBLK)
        for (int i = 0; i < 5; i++)
            cb[i] = (int32_t)ros_ld32(s->r[5] + 4 * i);
    fm_set_paint_matrix(flags & ST_MATRIX ? ros_ptr(s->r[6]) : NULL);
    int32_t maxidx = flags & ST_LENGTH ? (int32_t)s->r[7] : BIGNUM;
    fm_xletteradd = cb[2], fm_yletteradd = cb[3];
    struct fm_pars r;
    os_error *e = fm_scan_string(s->r[1], (int32_t)s->r[3], (int32_t)s->r[4], cb[4], maxidx, cb[0],
                              cb[1], &r);
    fm_currentfont = save;
    if (!fail(s, e))
        return;
    s->r[1] = r.p;
    s->r[3] = (uint32_t)r.x, s->r[4] = (uint32_t)r.y;
    if (flags & ST_RTNSPLIT)
        s->r[7] = (uint32_t)r.n;
    if (flags & ST_RTNBBOX)
        for (int i = 0; i < 4; i++)
            ros_st32(s->r[5] + 20 + 4 * i, (uint32_t)scanbbox[i]);
    if (flags & ST_RTNMATRIX) {
        static const int32_t unit[6] = { 0x10000, 0, 0, 0x10000, 0, 0 };
        const uint8_t *m = fm_paintmatrix ? fm_paintmatrix : (const uint8_t *)unit;
        memcpy(ros_ptr(s->r[6]), m, 24);
    }
}
