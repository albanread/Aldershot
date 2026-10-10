/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_compressjpeg.c: CompressJPEG, native over libjpeg-turbo
 * (modules/compressjpeg).  This test checks what must always hold.
 *
 * Its JPEGs match 5.30's in every marker.  That covers JFIF 1.01 with the
 * density given, the tables, and the frame and scan headers.  They also
 * match in the first of their data.  RISC OS's encoder is the IJG's
 * release 5, whose DCT rounds a little differently at the end of a scan.
 * The probes against the farm, tests/desktop/compress, compare the pixels.
 *
 * The JPEGs decode, with libjpeg's reader, to the size given and close to
 * the pixels given.
 *
 * The errors are 5.30's. Everything gives "Not enough memory", except that
 * a comment that is too long gives "String too long".
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[8])
{
    return ((const os_error *)ros_ptr(r[0]))->errnum;
}

static const char *errmess(const uint32_t r[8])
{
    return ((const os_error *)ros_ptr(r[0]))->errmess;
}

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    while (n--)
        h = (h ^ *p++) * 0x01000193u;
    return h;
}

/* The parameter block */
static uint32_t params(uint32_t w, uint32_t h, uint32_t q, uint32_t comps, uint32_t xdpi,
                       uint32_t ydpi)
{
    uint32_t p = ros_addr(ros_rma_alloc(24));
    uint32_t v[6] = { w, h, q, comps, xdpi, ydpi };
    memcpy(ros_ptr(p), v, sizeof v);
    return p;
}

/* The farm probe's image: R = x * 16, G = y * 32, B = 128 (both wrap); an
 * image wider than 16 pixels is a smooth gradient instead */
static void row_of(uint8_t *row, uint32_t w, uint32_t y)
{
    for (uint32_t x = 0; x < w; x++) {
        row[x * 3] = (uint8_t)(w > 16 ? x * 255 / (w - 1) : x * 16);
        row[x * 3 + 1] = (uint8_t)(w > 16 ? y * 2 : y * 32);
        row[x * 3 + 2] = (uint8_t)(w > 16 ? 255 - x * 255 / (w - 1) : 128);
    }
}

/* Start, rows, Finish: the JPEG's length, or 0; *err the first error */
static uint32_t make(uint32_t buf, uint32_t size, uint32_t p, uint32_t ws, uint32_t wsize,
                     uint32_t w, uint32_t h, uint32_t *err)
{
    *err = 0;
    uint32_t s[8] = { buf, size, p, ws, wsize };
    if (swi(XCompressJPEG_Start, s)) {
        *err = errnum(s);
        return 0;
    }
    uint32_t tag = s[0];
    uint8_t *row = ros_rma_alloc(w * 3 + 4);
    for (uint32_t y = 0; y < h; y++) {
        row_of(row, w, y);
        uint32_t l[8] = { tag, ros_addr(row) };
        if (swi(XCompressJPEG_WriteLine, l) && !*err)
            *err = errnum(l);
    }
    ros_rma_free(row);
    uint32_t f[8] = { tag };
    if (swi(XCompressJPEG_Finish, f)) {
        if (!*err)
            *err = errnum(f);
        return 0;
    }
    return *err ? 0 : f[0];
}

/* ---- decoding, with the same libjpeg -------------------------------------------------------- */

struct dec_err {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void dec_exit(j_common_ptr cinfo)
{
    longjmp(((struct dec_err *)cinfo->err)->jb, 1);
}

static void dec_quiet(j_common_ptr cinfo)
{
    (void)cinfo;
}

struct decoded {
    uint32_t w, h, comps, unit, xd, yd;
    uint8_t *px;                        /* malloc'd */
};

static int decode(const uint8_t *jpg, uint32_t len, struct decoded *d)
{
    struct jpeg_decompress_struct c;
    struct dec_err e;
    memset(d, 0, sizeof *d);
    memset(&c, 0, sizeof c);
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = dec_exit;
    e.pub.output_message = dec_quiet;
    if (setjmp(e.jb)) {
        jpeg_destroy_decompress(&c);
        free(d->px);
        d->px = NULL;
        return 0;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, jpg, len);
    jpeg_read_header(&c, TRUE);
    jpeg_start_decompress(&c);
    d->w = c.output_width;
    d->h = c.output_height;
    d->comps = (uint32_t)c.output_components;
    d->unit = c.density_unit;
    d->xd = c.X_density;
    d->yd = c.Y_density;
    d->px = malloc((size_t)d->w * d->h * d->comps);
    while (d->px && c.output_scanline < c.output_height) {
        JSAMPROW r = d->px + (size_t)c.output_scanline * d->w * d->comps;
        jpeg_read_scanlines(&c, &r, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return d->px != NULL;
}

/* The largest difference from the source image, decoded */
static int worst(const struct decoded *d)
{
    int most = 0;
    uint8_t row[3 * 2048];
    for (uint32_t y = 0; y < d->h; y++) {
        row_of(row, d->w, y);
        for (uint32_t x = 0; x < d->w; x++)
            for (uint32_t k = 0; k < d->comps; k++) {
                int want = d->comps == 3 ? row[x * 3 + k] : row[x];   /* grey: the row's bytes */
                int got = d->px[(y * d->w + x) * d->comps + k];
                int diff = got > want ? got - want : want - got;
                if (diff > most)
                    most = diff;
            }
    }
    return most;
}

/* The mean difference from the source, rounded up */
static int mean_diff(const struct decoded *d)
{
    uint64_t sum = 0, n = 0;
    uint8_t row[3 * 2048];
    for (uint32_t y = 0; y < d->h; y++) {
        row_of(row, d->w, y);
        for (uint32_t x = 0; x < d->w * d->comps; x++, n++) {
            int want = row[x], got = d->px[y * d->w * d->comps + x];
            sum += (uint64_t)(got > want ? got - want : want - got);
        }
    }
    return n ? (int)((sum + n - 1) / n) : 999;
}

void ros_selftest_compressjpeg(void)
{
    struct ros_module *m = NULL;
    for (struct ros_module *i = ros_module_first(); i; i = i->next)
        if (!strcmp(i->title, "CompressJPEG"))
            m = i;
    char *cname = ros_rma_alloc(32);
    strcpy(cname, "CompressJPEG_Comment");
    uint32_t nm[8] = { 0, ros_addr(cname) };
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, 0x20000u | 0x4A504u);
    int unknown = s.v && ((os_error *)ros_ptr(s.r[0]))->errnum == 0x1E6 &&
                  !strcmp(((os_error *)ros_ptr(s.r[0]))->errmess,
                          "SWI value out of range for module CompressJPEG");
    check(m && ros_module_version(m) == 0x800 && m->swi_chunk == 0x4A500 && m->swi_count == 4 &&
          !swi(XOS_SWINumberFromString, nm) && nm[0] == 0x4A503 && unknown,
          "CompressJPEG 0.08: Start, WriteLine, Finish and Comment at &4A500; others &1E6, as 5.30's",
          NULL);

    uint32_t buf = ros_addr(ros_rma_alloc(400000));
    uint8_t *bp = ros_ptr(buf);
    uint32_t err;

    /* the farm's colour image: 5.30's markers and first data */
    uint32_t p1 = params(16, 8, 75, 3, 90, 45);
    uint32_t s1[8] = { buf, 20000, p1, 0, 0 }, s2[8] = { buf, 20000, p1, 0, 0 };
    int tag = !swi(XCompressJPEG_Start, s1) && !swi(XCompressJPEG_Start, s2) && s1[0] == s2[0] &&
              s1[0] >= ROS_RMA_BASE && s1[0] < ROS_RMA_BASE + ROS_RMA_SIZE;
    uint32_t len = make(buf, 20000, p1, 0, 0, 16, 8, &err);
    struct decoded d = {0};
    int colour = len == 656 && fnv(bp, 641) == 0x2D2FAEA3u && bp[len - 2] == 0xFF &&
                 bp[len - 1] == 0xD9 && decode(bp, len, &d);
    int w1 = colour ? worst(&d) : 999;
    colour = colour && d.w == 16 && d.h == 8 && d.comps == 3 && d.unit == 1 && d.xd == 90 &&
             d.yd == 45 && w1 <= 32;
    free(d.px);
    check(tag && colour, "CompressJPEG: one tag (the module's, in the RMA), and the farm's 16x8 "
          "colour image -- 656 bytes, 5.30's markers (JFIF 1.01, 90 x 45 dpi, tables) and first "
          "data, decoding within 32 of the source (5.30's own: 21)", "%u bytes, worst %d", len, w1);

    /* grey, density unknown */
    uint32_t pg = params(16, 8, 75, 1, 0, 0);
    uint32_t lg = make(buf, 20000, pg, 0, 0, 16, 8, &err);
    struct decoded g = {0};
    int grey = lg && fnv(bp, 328) == 0x319A13FCu && decode(bp, lg, &g);
    int wg = grey ? worst(&g) : 999;
    grey = grey && g.comps == 1 && g.unit == 0 && g.xd == 1 && g.yd == 1 && wg <= 12;
    free(g.px);
    check(grey, "CompressJPEG greyscale, dpi 0: JFIF density 1:1, no unit; 5.30's markers; "
          "decodes close", "%u bytes, worst %d", lg, wg);

    /* a large one, workspace as the PRM sizes it, against the RMA's */
    uint32_t pw = params(640, 120, 90, 3, 90, 90);
    uint32_t wsz = 20000 + 640 * 30;
    uint32_t ws = ros_addr(ros_rma_alloc(wsz));
    uint32_t lr = make(buf, 400000, pw, 0, 0, 640, 120, &err);
    uint8_t *copy = malloc(lr ? lr : 1);
    if (copy && lr)
        memcpy(copy, bp, lr);
    uint32_t lw = make(buf, 400000, pw, ws, wsz, 640, 120, &err);
    uint32_t werr = err;
    struct decoded big = {0};
    int large = lr && lw == lr && copy && !memcmp(copy, bp, lr) && decode(bp, lw, &big);
    int wb = large ? worst(&big) : 999;
    large = large && big.w == 640 && big.h == 120 && wb <= 8 && mean_diff(&big) <= 1;
    free(big.px);
    free(copy);
    uint32_t sw[8] = { buf, 400000, pw, ws, 1000 }, sw2[8] = { buf, 400000, pw, ws, wsz - 1 };
    large = large && swi(XCompressJPEG_Start, sw) && errnum(sw) == 0x8183C4 &&
            swi(XCompressJPEG_Start, sw2) && errnum(sw2) == 0x8183C4;
    check(large, "CompressJPEG in a workspace of the PRM's size (20000 + 30 a pixel across): the "
          "same 640x120 JPEG as from the RMA; 1000 bytes, or one short, is too few",
          "%u %u err &%X worst %d",
          lr, lw, werr, wb);

    /* errors: no room, comments, components, too few rows */
    uint32_t sm[8] = { buf, 100, p1, 0, 0 };
    int errs = !swi(XCompressJPEG_Start, sm);
    uint8_t *row = ros_rma_alloc(64);
    row_of(row, 16, 0);
    uint32_t wl[8] = { sm[0], ros_addr(row) }, wl2[8] = { sm[0], ros_addr(row) }, fi[8] = { sm[0] };
    errs = errs && swi(XCompressJPEG_WriteLine, wl) && errnum(wl) == 0x8183C4 &&
           !strcmp(errmess(wl), "Not enough memory") && swi(XCompressJPEG_WriteLine, wl2) &&
           errnum(wl2) == 0x8183C4 && swi(XCompressJPEG_Finish, fi) && errnum(fi) == 0x8183C4;
    uint32_t p2 = params(16, 8, 75, 2, 0, 0), p3 = params(16, 8, 101, 3, 0, 0);
    uint32_t c2[8] = { buf, 20000, p2, 0, 0 }, q[8] = { buf, 20000, p3, 0, 0 };
    errs = errs && swi(XCompressJPEG_Start, c2) && errnum(c2) == 0x8183C4 &&
           !swi(XCompressJPEG_Start, q);
    uint32_t qf[8] = { q[0] };
    errs = errs && swi(XCompressJPEG_Finish, qf) && errnum(qf) == 0x8183C4;
    check(errs, "CompressJPEG's errors, as 5.30's: a full buffer, two components, too few rows, "
          "and everything after, \"Not enough memory\" (&8183C4)", NULL);

    /* comments: a COM marker before the rows; too long; after them */
    uint32_t cs[8] = { buf, 20000, p1, 0, 0 };
    int com = !swi(XCompressJPEG_Start, cs);
    uint32_t t = cs[0];
    char *text = ros_rma_alloc(70000);
    memset(text, 'x', 70000);
    uint32_t lc[8] = { t, 0, ros_addr(text), 65000 };
    com = com && swi(XCompressJPEG_Comment, lc) && errnum(lc) == 0x8183C5 &&
          !strcmp(errmess(lc), "String too long");
    strcpy(text, "Hello\tthere");
    text[11] = 0;
    uint32_t c1[8] = { t, 1, ros_addr(text) };
    com = com && !swi(XCompressJPEG_Comment, c1) && c1[0] == t;
    uint32_t c3[8] = { t, 0, ros_addr(text), 3 };
    com = com && !swi(XCompressJPEG_Comment, c3);
    for (uint32_t y = 0; com && y < 8; y++) {
        row_of(row, 16, y);
        uint32_t l[8] = { t, ros_addr(row) };
        com = !swi(XCompressJPEG_WriteLine, l);
    }
    uint32_t late[8] = { t, 1, ros_addr(text) }, cf[8] = { t };
    com = com && swi(XCompressJPEG_Comment, late) && errnum(late) == 0x8183C4 &&
          swi(XCompressJPEG_Finish, cf);
    /* again, without the late one: the markers are in the file */
    uint32_t cs2[8] = { buf, 20000, p1, 0, 0 };
    com = com && !swi(XCompressJPEG_Start, cs2);
    uint32_t c4[8] = { cs2[0], 1, ros_addr(text) };
    com = com && !swi(XCompressJPEG_Comment, c4);
    for (uint32_t y = 0; com && y < 8; y++) {
        row_of(row, 16, y);
        uint32_t l[8] = { cs2[0], ros_addr(row) };
        com = !swi(XCompressJPEG_WriteLine, l);
    }
    uint32_t cf2[8] = { cs2[0] };
    com = com && !swi(XCompressJPEG_Finish, cf2);
    int found = 0;
    for (uint32_t i = 0; com && i + 15 < cf2[0]; i++)
        if (bp[i] == 0xFF && bp[i + 1] == 0xFE && bp[i + 2] == 0 && bp[i + 3] == 13 &&
            !memcmp(bp + i + 4, "Hello\tthere", 11))
            found = 1;
    check(com && found, "CompressJPEG_Comment: control-terminated (tab kept) or counted; a COM "
          "marker; 65000 bytes \"String too long\" (&8183C5); after the rows, the compression "
          "fails, as 5.30's", NULL);
}
