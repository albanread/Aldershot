/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_compresspng.c: CompressPNG, native over libpng
 * (modules/compresspng): what must always hold.
 *
 * A PNG that the farm's CompressPNG 0.07 made is made again byte for byte.
 * The size request, a buffer and a file all agree.  Every form the SWIs
 * take decodes with libpng's reader to the pixels written.  The forms are
 * RGB, RGBA, RGB0, grey, grey and alpha, G0, palettes of 2 to 256 colours
 * with transparency, interlaced images, and rows repeated for pixels
 * taller than wide.  The chunks are as the original writes them.  The
 * errors are 5.30's.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <png.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "fileswitch.h"
#include "selftest.h"

#define check ros_check

/* A scratch disc of the test's own, as selftest_files makes its "Rec".
 * It is a fresh directory (TMPDIR hosted, /tmp in the box), mounted as
 * name.  The result is 0 if there is none. */
static int scratch_disc(const char *name, char *dir, size_t max)
{
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = "/tmp";
    mkdir(tmp, 0777);
    snprintf(dir, max, "%s/rosgd-%s-XXXXXX", tmp, name);
    if (!mkdtemp(dir))
        return 0;
    return ros_hostfs_mount(name, dir) == 0;
}

static void scratch_gone(const char *name, const char *dir)
{
    ros_hostfs_unmount(name);
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[700];
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    if (d)
        closedir(d);
    rmdir(dir);
}

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

static uint32_t str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = ros_rma_alloc((uint32_t)n);
    memcpy(p, s, n);
    return ros_addr(p);
}

/* The farm's: an RGB 8x4 at 90 dpi, comment Author "Probe\r" */
static const uint8_t farm_p1[174] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x08, 0x02, 0x00, 0x00,
    0x00, 0x3c, 0xaf, 0xe9, 0xa7, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00,
    0x00, 0x0d, 0xd6, 0x00, 0x00, 0x0d, 0xd6, 0x01, 0x90, 0x6f, 0x79, 0x9c, 0x00, 0x00,
    0x00, 0x0d, 0x74, 0x45, 0x58, 0x74, 0x41, 0x75, 0x74, 0x68, 0x6f, 0x72, 0x00, 0x50,
    0x72, 0x6f, 0x62, 0x65, 0x0d, 0xc4, 0x80, 0x84, 0xbd, 0x00, 0x00, 0x00, 0x47, 0x49,
    0x44, 0x41, 0x54, 0x08, 0x99, 0x63, 0x60, 0x50, 0xf5, 0xca, 0x9f, 0xb2, 0xf3, 0x1e,
    0xb3, 0x86, 0x6f, 0xd1, 0xf4, 0x3d, 0x0f, 0xd9, 0xb4, 0x03, 0x4a, 0x67, 0xed, 0x7f,
    0xc2, 0xa9, 0x17, 0xcc, 0x10, 0xdd, 0xb0, 0xf4, 0xd4, 0x7b, 0x11, 0xcb, 0xb8, 0xe6,
    0x15, 0x67, 0x3f, 0x89, 0xdb, 0x24, 0xb6, 0xad, 0xbe, 0xf0, 0x55, 0xca, 0x3e, 0xa5,
    0x73, 0x1d, 0xc3, 0xb6, 0xdb, 0xd8, 0xf5, 0x30, 0x08, 0x9a, 0x61, 0xd7, 0x03, 0x00,
    0x8a, 0x1c, 0x2c, 0xc1, 0xa8, 0x41, 0xca, 0xf6, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

/* A parameter block: width, height, dpi, flags, then (type, a, b) to 0 */
struct blk {
    uint32_t addr;
    uint32_t n;
};

static struct blk block(uint32_t w, uint32_t h, uint32_t xdpi, uint32_t ydpi, uint32_t flags)
{
    struct blk b = { ros_addr(ros_rma_alloc(20 + 12 * 8 + 4)), 0 };
    ros_st32(b.addr, w);
    ros_st32(b.addr + 4, h);
    ros_st32(b.addr + 8, xdpi);
    ros_st32(b.addr + 12, ydpi);
    ros_st32(b.addr + 16, flags);
    ros_st32(b.addr + 20, 0);
    return b;
}

static void param(struct blk *b, uint32_t type, uint32_t x, uint32_t y)
{
    uint32_t p = b->addr + 20 + 12 * b->n++;
    ros_st32(p, type);
    ros_st32(p + 4, x);
    ros_st32(p + 8, y);
    ros_st32(p + 12, 0);
}

/* The pattern a test row y holds, bpp bytes a pixel */
static void row_bytes(uint8_t *row, uint32_t w, uint32_t bpp, uint32_t y)
{
    for (uint32_t x = 0; x < w * bpp; x++)
        row[x] = (uint8_t)((x * 37 + y * 91) & 255);
}

/* Start, rows (made by fill), Finish into a buffer: the PNG's length, or 0 */
typedef void filler(uint8_t *row, uint32_t y, void *arg);
static uint32_t make(struct blk b, uint32_t buf, uint32_t size, uint32_t rows, filler *fill,
                     void *arg)
{
    uint32_t s[8] = { buf, size, b.addr };
    if (swi(XCompressPNG_Start, s))
        return 0;
    uint32_t tag = s[0];
    uint8_t *row = ros_rma_alloc(4096);
    for (uint32_t y = 0; y < rows; y++) {
        fill(row, y, arg);
        uint32_t w[8] = { tag, ros_addr(row) };
        if (swi(XCompressPNG_WriteLine, w)) {
            ros_rma_free(row);
            return 0;
        }
    }
    ros_rma_free(row);
    uint32_t f[8] = { tag };
    if (swi(XCompressPNG_Finish, f) || f[0] != buf)
        return 0;
    return f[1];
}

/* Decoded by libpng, as RGBA: malloc'd, or NULL */
static uint8_t *decode(uint32_t buf, uint32_t len, uint32_t *w, uint32_t *h)
{
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&im, ros_ptr(buf), len))
        return NULL;
    im.format = PNG_FORMAT_RGBA;
    uint8_t *px = malloc(PNG_IMAGE_SIZE(im));
    if (!px || !png_image_finish_read(&im, NULL, px, 0, NULL)) {
        free(px);
        png_image_free(&im);
        return NULL;
    }
    *w = im.width;
    *h = im.height;
    return px;
}

/* The chunk named type in a PNG: its data's offset, or 0 */
static uint32_t chunk(const uint8_t *p, uint32_t len, const char *type, uint32_t *n)
{
    for (uint32_t o = 8; o + 12 <= len;) {
        uint32_t l = (uint32_t)p[o] << 24 | p[o + 1] << 16 | p[o + 2] << 8 | p[o + 3];
        if (!memcmp(p + o + 4, type, 4)) {
            *n = l;
            return o + 8;
        }
        o += 12 + l;
    }
    return 0;
}

/* ---- the forms ------------------------------------------------------------------------------- */

struct form {
    uint32_t bpp;                       /* bytes a pixel in the rows given */
    int kind;                           /* how a pixel reads as RGBA */
    uint32_t colours;                   /* a palette's */
};
enum { K_RGB, K_RGBA, K_RGB0, K_G, K_GA, K_G0, K_PAL };

static void fill_form(uint8_t *row, uint32_t y, void *arg)
{
    const struct form *f = arg;
    row_bytes(row, 8, f->bpp, y);
    if (f->kind == K_PAL)
        for (uint32_t x = 0; x < 8; x++)
            row[x] = (uint8_t)((x * 7 + y * 3) % f->colours);
}

/* What pixel (x, y) of a form must decode to */
static void expect(const struct form *f, uint32_t x, uint32_t y, const uint32_t *pal,
                   const uint8_t *trns, uint8_t out[4])
{
    uint8_t row[64];
    fill_form(row, y, (void *)f);
    const uint8_t *p = row + x * f->bpp;
    switch (f->kind) {
    case K_RGB: case K_RGB0: out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255; break;
    case K_RGBA: out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = p[3]; break;
    case K_G: case K_G0: out[0] = out[1] = out[2] = p[0], out[3] = 255; break;
    case K_GA: out[0] = out[1] = out[2] = p[0], out[3] = p[1]; break;
    default: {
        uint32_t c = pal[p[0]];
        out[0] = (uint8_t)c, out[1] = (uint8_t)(c >> 8), out[2] = (uint8_t)(c >> 16);
        out[3] = trns && p[0] < 4 ? trns[p[0]] : 255;     /* four transparencies given */
    }
    }
}

static int form_ok(uint32_t buf, uint32_t len, const struct form *f, uint32_t rowdup,
                   const uint32_t *pal, const uint8_t *trns)
{
    uint32_t w, h;
    uint8_t *px = len ? decode(buf, len, &w, &h) : NULL;
    if (!px)
        return 0;
    int ok = w == 8 && h == 4 * rowdup;
    for (uint32_t y = 0; ok && y < h; y++)
        for (uint32_t x = 0; ok && x < 8; x++) {
            uint8_t e[4];
            expect(f, x, y / rowdup, pal, trns, e);
            ok = !memcmp(px + (y * 8 + x) * 4, e, 4);
        }
    free(px);
    return ok;
}

void ros_selftest_compresspng(void)
{
    struct ros_module *m = NULL;
    for (struct ros_module *i = ros_module_first(); i; i = i->next)
        if (!strcmp(i->title, "CompressPNG"))
            m = i;
    uint32_t nm[8] = { 0, str("CompressPNG_Finish") };
    uint32_t bad[8] = { 0, 0 };
    check(m && ros_module_version(m) == 0x700 && m->swi_chunk == 0x59E00 && m->swi_count == 4 &&
          !swi(XOS_SWINumberFromString, nm) && nm[0] == 0x59E03 &&
          swi(XCompressPNG_WriteLine, bad) && errnum(bad) == 0x821600 &&
          !strcmp(errmess(bad), "Invalid tag pointer passed to CompressPNG_ SWI"),
          "CompressPNG 0.07: its 4 SWIs at &59E00; a bad tag is &821600, as 5.30's", NULL);

    uint32_t buf = ros_addr(ros_rma_alloc(20000));
    uint8_t *bp = ros_ptr(buf);

    /* 5.30's bytes: RGB with a comment */
    struct form rgb = { 3, K_RGB, 0 };
    struct blk b1 = block(8, 4, 90, 90, 0);
    uint32_t s[8] = { buf, 20000, b1.addr };
    int same = !swi(XCompressPNG_Start, s);
    uint32_t tag = s[0];
    uint32_t c[8] = { tag, str("Author"), str("Probe\r") };
    same = same && !swi(XCompressPNG_Comment, c) && c[0] == tag;
    uint8_t *row = ros_rma_alloc(64);
    for (uint32_t y = 0; same && y < 4; y++) {
        fill_form(row, y, &rgb);
        uint32_t wl[8] = { tag, ros_addr(row) };
        same = !swi(XCompressPNG_WriteLine, wl) && wl[0] == tag;
    }
    uint32_t f[8] = { tag };
    same = same && !swi(XCompressPNG_Finish, f) && f[0] == buf && f[1] == 174 &&
           !memcmp(bp, farm_p1, 174);
    uint32_t after[8] = { tag, ros_addr(row) };
    same = same && swi(XCompressPNG_WriteLine, after) && errnum(after) == 0x821600;
    check(same, "CompressPNG makes 5.30's PNG byte for byte (RGB 8x4, pHYs 3542, a tEXt by "
          "CompressPNG_Comment); the tag is gone after Finish", NULL);

    /* the size, then a buffer of it, then a file: the same 149 bytes */
    uint32_t sz = make(b1, 0, 0, 4, fill_form, &rgb);
    uint32_t s0[8] = { 0, 0, b1.addr };
    int sized = !swi(XCompressPNG_Start, s0) && s0[0] >= ROS_DA_BASE;
    uint32_t t0 = s0[0];
    for (uint32_t y = 0; sized && y < 4; y++) {
        fill_form(row, y, &rgb);
        uint32_t wl[8] = { t0, ros_addr(row) };
        sized = !swi(XCompressPNG_WriteLine, wl);
    }
    uint32_t f0[8] = { t0, 77 };
    sized = sized && !swi(XCompressPNG_Finish, f0) && f0[0] == 0 && f0[1] == 149 && sz == 149;
    uint32_t inbuf = make(b1, buf, 149, 4, fill_form, &rgb);
    uint8_t *copy = malloc(149);
    if (copy)
        memcpy(copy, bp, 149);
    char dir[600];
    int disc = scratch_disc("PTest", dir, sizeof dir);
    struct blk bf = block(8, 4, 90, 90, 1);
    uint32_t fname = str("HostFS::PTest.$.PNGFile");
    uint32_t sf[8] = { fname, 0, bf.addr };
    int tofile = !swi(XCompressPNG_Start, sf);
    for (uint32_t y = 0; tofile && y < 4; y++) {
        fill_form(row, y, &rgb);
        uint32_t wl[8] = { sf[0], ros_addr(row) };
        tofile = !swi(XCompressPNG_WriteLine, wl);
    }
    uint32_t ff[8] = { sf[0], 5 };
    uint32_t info[8] = { 17, fname }, load[8] = { 255, fname, buf, 0 };
    tofile = tofile && !swi(XCompressPNG_Finish, ff) && ff[0] == sf[0] && ff[1] == 5 &&
             !swi(XOS_File, info) && info[0] == 1 && ((info[2] >> 8) & 0xFFF) == 0xB60 &&
             info[4] == 149 && !swi(XOS_File, load) && copy && !memcmp(bp, copy, 149);
    free(copy);
    if (disc)
        scratch_gone("PTest", dir);
    check(sized && inbuf == 149 && tofile && disc, "CompressPNG's three destinations: R0 0 asks the size "
          "(149, R0 0 out), a buffer of it takes the PNG, a file gets it (type &B60, R0 and R1 "
          "kept), all the same bytes", "%u %u", sz, inbuf);

    /* errors: too few rows (R1 what was made), too many, no room, palettes */
    uint32_t e1[8] = { buf, 20000, b1.addr };
    int errs = !swi(XCompressPNG_Start, e1);
    fill_form(row, 0, &rgb);
    uint32_t ew[8] = { e1[0], ros_addr(row) }, ef[8] = { e1[0] };
    errs = errs && !swi(XCompressPNG_WriteLine, ew) && swi(XCompressPNG_Finish, ef) &&
           errnum(ef) == 0x821609 && ef[1] == 54 &&
           !strcmp(errmess(ef), "Not enough rows have been output for this image");
    uint32_t e2[8] = { buf, 20000, b1.addr };
    errs = errs && !swi(XCompressPNG_Start, e2);
    int many = 0;
    for (uint32_t y = 0; errs && y < 5; y++) {
        fill_form(row, y, &rgb);
        uint32_t wl[8] = { e2[0], ros_addr(row) };
        if (swi(XCompressPNG_WriteLine, wl))
            many = y == 4 && errnum(wl) == 0x821606;
    }
    uint32_t e2f[8] = { e2[0] };
    errs = errs && many && !swi(XCompressPNG_Finish, e2f) && e2f[1] == 149;
    uint32_t e3[8] = { buf, 40, b1.addr };
    errs = errs && !swi(XCompressPNG_Start, e3);
    uint32_t small[8] = { e3[0], ros_addr(row) }, e3f[8] = { e3[0] };
    errs = errs && swi(XCompressPNG_WriteLine, small) && errnum(small) == 0x821607 &&
           !strcmp(errmess(small), "Failed to output PNG data: The supplied buffer is too small "
                                   "to hold the PNG image") &&
           swi(XCompressPNG_Finish, e3f) && errnum(e3f) == 0x821609 && e3f[1] == 33;
    uint32_t pal = ros_addr(ros_rma_alloc(1024)), trns = ros_addr(ros_rma_alloc(256));
    struct blk pa = block(8, 4, 90, 90, 2);
    param(&pa, 5, 4, pal);
    uint32_t p1[8] = { buf, 20000, pa.addr };
    errs = errs && swi(XCompressPNG_Start, p1) && errnum(p1) == 0x821602 &&
           !strcmp(errmess(p1), "Failed to initialise PNG output: Alpha channels are not "
                                "supported for palettised images");
    struct blk pb = block(8, 4, 90, 90, 0);
    param(&pb, 5, 3, pal);
    uint32_t p2[8] = { buf, 20000, pb.addr };
    errs = errs && swi(XCompressPNG_Start, p2) &&
           !strcmp(errmess(p2), "Failed to initialise PNG output: Palette data may only be 2, 4, "
                                "8, 16 or 256 entries");
    struct blk pc = block(8, 4, 90, 90, 0);
    param(&pc, 5, 4, pal);
    param(&pc, 6, 8, trns);
    uint32_t p3[8] = { buf, 20000, pc.addr };
    errs = errs && swi(XCompressPNG_Start, p3) &&
           !strcmp(errmess(p3), "Failed to initialise PNG output: Transparency count exceeds "
                                "palette size");
    check(errs, "CompressPNG's errors, as 5.30's: too few rows (&821609, R1 the bytes made), too "
          "many (&821606), no room (&821607), and three palettes it will not take (&821602)", NULL);

    /* every form decodes to what was written */
    static const struct { struct form f; uint32_t flags; const char *name; } forms[] = {
        { { 3, K_RGB, 0 }, 0, "RGB" }, { { 4, K_RGBA, 0 }, 2, "RGBA" },
        { { 4, K_RGB0, 0 }, 8, "RGB0" }, { { 1, K_G, 0 }, 4, "grey" },
        { { 2, K_GA, 0 }, 6, "grey+alpha" }, { { 2, K_G0, 0 }, 12, "G0" },
    };
    int forms_ok = 1;
    const char *bad_form = "";
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        struct blk b = block(8, 4, 90, 90, forms[i].flags);
        param(&b, 3, 9, 0);
        uint32_t len = make(b, buf, 20000, 4, fill_form, (void *)&forms[i].f);
        uint32_t n, ih = len ? chunk(bp, len, "IHDR", &n) : 0;
        static const uint8_t types[] = { 2, 6, 2, 0, 4, 0 };
        if (!ih || bp[ih + 9] != types[i] || bp[ih + 8] != 8 ||
            !form_ok(buf, len, &forms[i].f, 1, NULL, NULL)) {
            forms_ok = 0;
            bad_form = forms[i].name;
        }
    }
    /* palettes: 2 colours (1 bit), 4 with transparency (2 bits), 16
     * interlaced (4 bits), 256 (8 bits); red the low byte of each word */
    uint32_t *pw = ros_ptr(pal);
    uint8_t *tp = ros_ptr(trns);
    for (int i = 0; i < 256; i++) {
        pw[i] = (uint32_t)(i * 0x010203 + 0x102030) & 0xFFFFFF;
        tp[i] = (uint8_t)(255 - i * 60);
    }
    static const struct { uint32_t colours; int trns; int interlace; uint8_t bits; } pals[] = {
        { 2, 0, 0, 1 }, { 4, 1, 0, 2 }, { 16, 0, 1, 4 }, { 256, 1, 0, 8 },
    };
    for (size_t i = 0; i < sizeof pals / sizeof pals[0]; i++) {
        struct form pf = { 1, K_PAL, pals[i].colours };
        struct blk b = block(8, 4, 90, 90, 0);
        param(&b, 5, pals[i].colours, pal);
        if (pals[i].trns)
            param(&b, 6, 4, trns);
        if (pals[i].interlace)
            param(&b, 4, 1, 0);
        uint32_t len = make(b, buf, 20000, 4, fill_form, &pf);
        uint32_t n, ih = len ? chunk(bp, len, "IHDR", &n) : 0, pl = len ? chunk(bp, len, "PLTE", &n) : 0;
        int ok = ih && bp[ih + 8] == pals[i].bits && bp[ih + 9] == 3 &&
                 bp[ih + 12] == (pals[i].interlace ? 1 : 0) && pl && n == pals[i].colours * 3 &&
                 bp[pl] == 0x30 && bp[pl + 1] == 0x20 && bp[pl + 2] == 0x10 &&
                 (!pals[i].trns || chunk(bp, len, "tRNS", &n)) &&
                 form_ok(buf, len, &pf, 1, pw, pals[i].trns ? tp : NULL);
        if (!ok) {
            forms_ok = 0;
            bad_form = "palette";
        }
    }
    check(forms_ok, "CompressPNG's forms decode (libpng) to the pixels written: RGB, RGBA, RGB0, "
          "grey, grey+alpha, G0; palettes of 2, 4 (tRNS), 16 (interlaced) and 256 colours, "
          "words of red, green, blue", "%s", bad_form);

    /* rows doubled for 90 x 45 dpi; gamma as an FPA double; pHYs from x */
    struct blk bd = block(8, 4, 90, 45, 0);
    param(&bd, 2, 0x3FE00000u, 0);              /* 0.5, the high word first */
    uint32_t len = make(bd, buf, 20000, 4, fill_form, &rgb);
    uint32_t n, gm = len ? chunk(bp, len, "gAMA", &n) : 0, ph = len ? chunk(bp, len, "pHYs", &n) : 0;
    uint32_t ih = len ? chunk(bp, len, "IHDR", &n) : 0;
    int dup = gm && bp[gm] == 0 && bp[gm + 1] == 0 && bp[gm + 2] == 0xC3 && bp[gm + 3] == 0x50 &&
              ph && bp[ph + 2] == 0x0D && bp[ph + 3] == 0xD6 && bp[ph + 6] == 0x0D &&
              bp[ph + 7] == 0xD6 && ih && bp[ih + 7] == 8;
    /* decoded without the gamma (libpng would correct for it) */
    struct blk bd2 = block(8, 4, 90, 45, 0);
    uint32_t len2 = make(bd2, buf, 20000, 4, fill_form, &rgb);
    dup = dup && form_ok(buf, len2, &rgb, 2, NULL, NULL);
    check(dup, "CompressPNG at 90 x 45 dpi: each row twice (8 rows), pHYs from the x dpi (3542 "
          "both ways); gamma 0.5 read as FPA's double, gAMA 50000, as 5.30 reads it", NULL);

    /* An interlaced image whose rows (4097 * 4 bytes, 262145 of them) come to
     * 4GB and a little over: in 32 bits that wraps to about 1MB, which the
     * image used to be given.  It is refused for want of memory. */
    {
        struct blk bw = block(4097, 262145, 90, 90, 2);
        param(&bw, 4, 1, 0);
        uint32_t big[8] = { 0, 0, bw.addr };
        int refused = swi(XCompressPNG_Start, big);
        if (!refused) {
            uint32_t fin[8] = { big[0] };
            swi(XCompressPNG_Finish, fin);
        }
        check(refused && errnum(big) == 0x821601,
              "CompressPNG refuses an interlaced image of over 2GB (its size would wrap in 32 bits)",
              "%s &%X", refused ? "error" : "accepted", refused ? errnum(big) : 0);
    }
}
