/* plot.c -- NetSurf's native RISC OS front end: the page's sprite, the
 * plotters, fonts and bitmaps (design 22, section 11).  ROSGD's own; MIT
 * licence.
 *
 * NetSurf draws into a 32 bpp sprite the size of the window's visible area,
 * with output switched to it (OS_SpriteOp 60), in RISC OS's own way:
 * ColourTrans for colours, OS_Plot for filled rectangles, Draw for lines,
 * polygons and paths, the Font Manager for text.  Bitmaps are copied into
 * the sprite's pixels here.  The window's redraw then plots the sprite.
 *
 * Text: Homerton for sans-serif, Trinity for serif, Corpus for monospace,
 * Medium or Bold, upright or slanted, in the UTF8 encoding, painted blended
 * with what is under it.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <kernel.h>
#include <swis.h>

#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"

#include "rosgd/rosgd.h"

int ro_eigx = 1, ro_eigy = 1;

/* ---- the canvas: a 32 bpp sprite ------------------------------------------- */

static struct {
    uint32_t *area;             /* a sprite area holding the one sprite */
    uint32_t *sprite;
    uint32_t *pixels;           /* &00BBGGRR, rows top down */
    int w, h;                   /* in pixels */
    uint32_t save[96];          /* its VDU state: OS_SpriteOp 60's save area */
    uint32_t prev[4];           /* where output went before */
    struct rect clip;           /* pixels, x1 and y1 exclusive */
} cv;

/* the canvas's mode word: 32 bpp at the screen's resolution, a pixel as
 * many OS units */
static uint32_t canvas_mode(void)
{
    return 6u << 27 | (uint32_t)(180 >> ro_eigy) << 14 | (uint32_t)(180 >> ro_eigx) << 1 | 1;
}

/* The canvas onto a screen of 256 colours or fewer needs ColourTrans's
 * table, a 32K one: a true colour sprite plotted there without one plots
 * nothing (SpriteExtend, as RISC OS 5's), and the page was blank after a
 * mode change to 256 colours (#87).  Made again at each mode change, as
 * ColourTrans's own 32K tables last only until one. */
static uint32_t *ttr;

static void make_ttr(void)
{
    free(ttr);
    ttr = NULL;
    int l2bpp = 5, size = 0;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 9, &l2bpp);
    if (l2bpp > 3)
        return;
    _kernel_oserror *e = _swix(ColourTrans_SelectTable, _INR(0, 5) | _OUT(4), canvas_mode(), -1, -1, -1, 0, 0,
                               &size);
    if (!e && size > 0 && (ttr = malloc((size_t)size)) != NULL &&
        (e = _swix(ColourTrans_SelectTable, _INR(0, 5), canvas_mode(), -1, -1, -1, ttr, 0)) != NULL) {
        free(ttr);
        ttr = NULL;
    }
    ro_log("mode: %d bpp, a %d-byte table%s%s", 1 << l2bpp, size, e ? ": " : "", e ? e->errmess : "");
}

void ro_mode_changed(void)
{
    int x = 1, y = 1;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &x);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &y);
    ro_eigx = x, ro_eigy = y;
    free(cv.area);                  /* made again at the new resolution */
    cv.area = NULL;
    cv.w = cv.h = 0;
    make_ttr();
}

bool ro_canvas_begin(int w, int h)
{
    if (w < 1 || h < 1)
        return false;
    if (w > cv.w || h > cv.h) {
        int nw = w > cv.w ? w : cv.w, nh = h > cv.h ? h : cv.h;
        size_t size = 16 + 44 + (size_t)nw * (size_t)nh * 4;
        free(cv.area);
        cv.area = malloc(size);
        if (!cv.area) {
            cv.w = cv.h = 0;
            return false;
        }
        cv.area[0] = (uint32_t)size;
        cv.area[2] = 16;
        uint32_t mode = canvas_mode();
        if (_swix(OS_SpriteOp, _INR(0, 1), 9 + 256, cv.area) ||
            _swix(OS_SpriteOp, _INR(0, 6), 15 + 256, cv.area, "page", 0, nw, nh, mode)) {
            free(cv.area);
            cv.area = NULL;
            cv.w = cv.h = 0;
            return false;
        }
        cv.sprite = cv.area + cv.area[2] / 4;
        cv.pixels = cv.sprite + cv.sprite[8] / 4;       /* the image's offset */
        cv.w = nw, cv.h = nh;
        cv.save[0] = 0;
    }
    if (_swix(OS_SpriteOp, _INR(0, 3) | _OUTR(0, 3), 60 + 512, cv.area, cv.sprite, cv.save, &cv.prev[0],
              &cv.prev[1], &cv.prev[2], &cv.prev[3]))
        return false;
    cv.clip = (struct rect){ 0, 0, w, h };
    return true;
}

void ro_canvas_end(void)
{
    _swix(OS_SpriteOp, _INR(0, 3), cv.prev[0], cv.prev[1], cv.prev[2], cv.prev[3]);
}

/* The canvas plotted on the screen within the redraw rectangle.  The
 * graphics window the Wimp set for the rectangle may not survive the
 * switch of output to the canvas and back: a screen with no save area is
 * switched back to with its VDU state reset -- the graphics window the
 * whole screen -- and the whole canvas, older rectangles' pixels and all,
 * was plotted over the window (torn bands, the page over its toolbar).  So
 * the window is set to the rectangle here, before the plot. */
void ro_canvas_put(int x, int y, const int32_t *clip)
{
    int x0 = clip[0], y0 = clip[1], x1 = clip[2] - 1, y1 = clip[3] - 1;
    char v[9] = { 24, (char)x0, (char)(x0 >> 8), (char)y0, (char)(y0 >> 8),
                  (char)x1, (char)(x1 >> 8), (char)y1, (char)(y1 >> 8) };
    _swix(OS_WriteN, _INR(0, 1), v, 9);
    _kernel_oserror *e = _swix(OS_SpriteOp, _INR(0, 7), 52 + 512, cv.area, cv.sprite, x, y - (cv.h << ro_eigy), 0, 0,
                               ttr);
    if (e)
        ro_log("put: %s", e->errmess);
}

/* ---- the plotters ------------------------------------------------------------ */

/* A pixel's corner in OS units, and in Draw's units (1/256 OS unit) */
#define OSX(x) ((x) << ro_eigx)
#define OSY(y) ((cv.h - (y)) << ro_eigy)
#define DX(x)  ((int32_t)lround((x) * (1 << ro_eigx) * 256.0))
#define DY(y)  ((int32_t)lround((cv.h - (y)) * (1 << ro_eigy) * 256.0))

static bool clipped(void)
{
    return cv.clip.x0 >= cv.clip.x1 || cv.clip.y0 >= cv.clip.y1;
}

static void gcol(colour c)
{
    _swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), (c & 0xFFFFFF) << 8, 0, 0);
}

static nserror p_clip(const struct redraw_context *ctx, const struct rect *r)
{
    (void)ctx;
    cv.clip.x0 = r->x0 > 0 ? r->x0 : 0;
    cv.clip.y0 = r->y0 > 0 ? r->y0 : 0;
    cv.clip.x1 = r->x1 < cv.w ? r->x1 : cv.w;
    cv.clip.y1 = r->y1 < cv.h ? r->y1 : cv.h;
    if (clipped())
        return NSERROR_OK;
    /* VDU 24: the graphics window, inclusive */
    int x0 = OSX(cv.clip.x0), y0 = OSY(cv.clip.y1), x1 = OSX(cv.clip.x1) - 1, y1 = OSY(cv.clip.y0) - 1;
    char v[9] = { 24, (char)x0, (char)(x0 >> 8), (char)y0, (char)(y0 >> 8),
                  (char)x1, (char)(x1 >> 8), (char)y1, (char)(y1 >> 8) };
    _swix(OS_WriteN, _INR(0, 1), v, 9);
    return NSERROR_OK;
}

/* Draw paths: move 2, line 8, bezier 6, close 5, end 0 */
static int32_t *path;
static size_t path_n, path_cap;

static void path_put(int32_t v)
{
    if (path_n == path_cap) {
        size_t cap = path_cap ? path_cap * 2 : 256;
        int32_t *grown = realloc(path, cap * sizeof *grown);
        if (!grown)
            return;
        path = grown, path_cap = cap;
    }
    path[path_n++] = v;
}

static void path_move(double x, double y) { path_put(2), path_put(DX(x)), path_put(DY(y)); }
static void path_line(double x, double y) { path_put(8), path_put(DX(x)), path_put(DY(y)); }
static void path_close(void) { path_put(5); }

static bool path_end(void)
{
    path_put(0), path_put(0);
    return path && path_n <= path_cap;
}

static void fill(colour c)
{
    gcol(c);
    _swix(Draw_Fill, _INR(0, 3), path, 0, 0, 0);
}

static void stroke(const plot_style_t *style)
{
    static const struct { uint8_t join, lead, trail, spare; int32_t mitre; uint16_t tri[4]; } caps = {
        0, 0, 0, 0, 0xA0000, { 0, 0, 0, 0 }
    };
    struct { int32_t start, count, len[2]; } dash = { 0, 2, { 0, 0 } };
    double w = plot_style_fixed_to_double(style->stroke_width);
    int32_t thick = w > 1 ? (int32_t)(w * (1 << ro_eigx) * 256) : 0;
    int32_t unit = (w > 1 ? (int32_t)w : 1) << ro_eigx << 8;
    const void *pattern = NULL;
    if (style->stroke_type == PLOT_OP_TYPE_DOT)
        dash.len[0] = dash.len[1] = unit, pattern = &dash;
    else if (style->stroke_type == PLOT_OP_TYPE_DASH)
        dash.len[0] = 3 * unit, dash.len[1] = 2 * unit, pattern = &dash;
    gcol(style->stroke_colour);
    _swix(Draw_Stroke, _INR(0, 6), path, 0, 0, 0, thick, &caps, pattern);
}

static void shape(const plot_style_t *style)
{
    if (!path_end())
        return;
    if (style->fill_type != PLOT_OP_TYPE_NONE)
        fill(style->fill_colour);
    if (style->stroke_type != PLOT_OP_TYPE_NONE)
        stroke(style);
}

static nserror p_rectangle(const struct redraw_context *ctx, const plot_style_t *style, const struct rect *r)
{
    (void)ctx;
    if (clipped())
        return NSERROR_OK;
    if (style->fill_type != PLOT_OP_TYPE_NONE && r->x1 > r->x0 && r->y1 > r->y0) {
        gcol(style->fill_colour);
        _swix(OS_Plot, _INR(0, 2), 4, OSX(r->x0), OSY(r->y1));
        _swix(OS_Plot, _INR(0, 2), 96 + 5, OSX(r->x1) - 1, OSY(r->y0) - 1);
    }
    if (style->stroke_type != PLOT_OP_TYPE_NONE) {
        path_n = 0;
        path_move(r->x0, r->y0), path_line(r->x1, r->y0), path_line(r->x1, r->y1), path_line(r->x0, r->y1), path_close();
        if (path_end())
            stroke(style);
    }
    return NSERROR_OK;
}

static nserror p_line(const struct redraw_context *ctx, const plot_style_t *style, const struct rect *l)
{
    (void)ctx;
    if (clipped() || style->stroke_type == PLOT_OP_TYPE_NONE)
        return NSERROR_OK;
    path_n = 0;
    path_move(l->x0, l->y0), path_line(l->x1, l->y1);
    if (path_end())
        stroke(style);
    return NSERROR_OK;
}

static nserror p_polygon(const struct redraw_context *ctx, const plot_style_t *style, const int *p, unsigned int n)
{
    (void)ctx;
    if (clipped() || n < 2)
        return NSERROR_OK;
    path_n = 0;
    path_move(p[0], p[1]);
    for (unsigned i = 1; i < n; i++)
        path_line(p[2 * i], p[2 * i + 1]);
    path_close();
    shape(style);
    return NSERROR_OK;
}

static nserror p_path(const struct redraw_context *ctx, const plot_style_t *style, const float *p, unsigned int n,
                      const float t[6])
{
    (void)ctx;
    if (clipped())
        return NSERROR_OK;
#define TX(i) (t[0] * p[i] + t[2] * p[(i) + 1] + t[4])
#define TY(i) (t[1] * p[i] + t[3] * p[(i) + 1] + t[5])
    path_n = 0;
    for (unsigned i = 0; i < n;) {
        switch ((int)p[i]) {
        case PLOTTER_PATH_MOVE:
            if (i + 2 >= n)
                return NSERROR_OK;
            path_move(TX(i + 1), TY(i + 1));
            i += 3;
            break;
        case PLOTTER_PATH_LINE:
            if (i + 2 >= n)
                return NSERROR_OK;
            path_line(TX(i + 1), TY(i + 1));
            i += 3;
            break;
        case PLOTTER_PATH_BEZIER:
            if (i + 6 >= n)
                return NSERROR_OK;
            path_put(6);
            for (unsigned k = 1; k <= 5; k += 2)
                path_put(DX(TX(i + k))), path_put(DY(TY(i + k)));
            i += 7;
            break;
        case PLOTTER_PATH_CLOSE:
            path_close();
            i += 1;
            break;
        default:
            return NSERROR_OK;
        }
    }
#undef TX
#undef TY
    shape(style);
    return NSERROR_OK;
}

/* An arc of a circle as short lines, angles in degrees anticlockwise from
 * three o'clock */
static void arc_path(int x, int y, int r, double a0, double a1, bool whole)
{
    int steps = (int)ceil(fabs(a1 - a0) / 6) + 1;
    for (int i = 0; i <= steps; i++) {
        double a = (a0 + (a1 - a0) * i / steps) * M_PI / 180;
        double px = x + r * cos(a), py = y - r * sin(a);
        if (i)
            path_line(px, py);
        else
            path_move(px, py);
    }
    if (whole)
        path_close();
}

static nserror p_disc(const struct redraw_context *ctx, const plot_style_t *style, int x, int y, int r)
{
    (void)ctx;
    if (clipped())
        return NSERROR_OK;
    path_n = 0;
    arc_path(x, y, r, 0, 360, true);
    shape(style);
    return NSERROR_OK;
}

static nserror p_arc(const struct redraw_context *ctx, const plot_style_t *style, int x, int y, int r, int a1,
                     int a2)
{
    (void)ctx;
    if (clipped())
        return NSERROR_OK;
    if (a2 < a1)
        a2 += 360;
    path_n = 0;
    arc_path(x, y, r, a1, a2, false);
    plot_style_t s = *style;
    s.fill_type = PLOT_OP_TYPE_NONE;
    if (s.stroke_type == PLOT_OP_TYPE_NONE)
        s.stroke_type = PLOT_OP_TYPE_SOLID, s.stroke_colour = style->fill_colour;
    shape(&s);
    return NSERROR_OK;
}

/* ---- bitmaps ---------------------------------------------------------------- */

struct bitmap {
    int w, h;
    bool opaque;
    uint32_t *px;               /* R G B A bytes: &AABBGGRR words, as the sprite's are &00BBGGRR */
};

static void *b_create(int w, int h, enum gui_bitmap_flags flags)
{
    struct bitmap *b = calloc(1, sizeof *b);
    if (!b || w <= 0 || h <= 0) {
        free(b);
        return NULL;
    }
    size_t n = (size_t)w * (size_t)h * 4;
    b->px = flags & BITMAP_CLEAR ? calloc(1, n) : malloc(n);
    if (!b->px) {
        free(b);
        return NULL;
    }
    b->w = w, b->h = h;
    b->opaque = flags & BITMAP_OPAQUE;
    return b;
}

static void b_destroy(void *v)
{
    struct bitmap *b = v;
    if (b)
        free(b->px);
    free(b);
}

static void b_set_opaque(void *v, bool opaque) { ((struct bitmap *)v)->opaque = opaque; }
static bool b_get_opaque(void *v) { return ((struct bitmap *)v)->opaque; }
static unsigned char *b_get_buffer(void *v) { return (unsigned char *)((struct bitmap *)v)->px; }
static size_t b_get_rowstride(void *v) { return (size_t)((struct bitmap *)v)->w * 4; }
static int b_get_width(void *v) { return ((struct bitmap *)v)->w; }
static int b_get_height(void *v) { return ((struct bitmap *)v)->h; }
static void b_modified(void *v) { (void)v; }

static nserror b_render(struct bitmap *b, struct hlcache_handle *c)
{
    (void)b, (void)c;
    return NSERROR_NOT_IMPLEMENTED;             /* thumbnails: none yet */
}

static struct gui_bitmap_table bitmap_table = {
    .create = b_create, .destroy = b_destroy, .set_opaque = b_set_opaque, .get_opaque = b_get_opaque,
    .get_buffer = b_get_buffer, .get_rowstride = b_get_rowstride, .get_width = b_get_width,
    .get_height = b_get_height, .modified = b_modified, .render = b_render,
};
struct gui_bitmap_table *ro_bitmap_table = &bitmap_table;

static int wrap(int v, int n)
{
    v %= n;
    return v < 0 ? v + n : v;
}

/* Scaled to width x height at (x, y), repeated if asked, into the sprite's
 * pixels inside the clip, blended by its alpha */
static nserror p_bitmap(const struct redraw_context *ctx, struct bitmap *b, int x, int y, int width, int height,
                        colour bg, bitmap_flags_t flags)
{
    (void)ctx, (void)bg;
    if (!b || width <= 0 || height <= 0 || clipped())
        return NSERROR_OK;
    bool rx = flags & BITMAPF_REPEAT_X, ry = flags & BITMAPF_REPEAT_Y;
    int x0 = rx || x < cv.clip.x0 ? cv.clip.x0 : x;
    int x1 = rx || x + width > cv.clip.x1 ? cv.clip.x1 : x + width;
    int y0 = ry || y < cv.clip.y0 ? cv.clip.y0 : y;
    int y1 = ry || y + height > cv.clip.y1 ? cv.clip.y1 : y + height;
    for (int dy = y0; dy < y1; dy++) {
        int ty = ry ? wrap(dy - y, height) : dy - y;
        const uint32_t *src = b->px + (size_t)(ty * b->h / height) * (size_t)b->w;
        uint32_t *dst = cv.pixels + (size_t)dy * (size_t)cv.w;
        for (int dx = x0; dx < x1; dx++) {
            int tx = rx ? wrap(dx - x, width) : dx - x;
            uint32_t s = src[tx * b->w / width], a = s >> 24;
            if (b->opaque || a == 255) {
                dst[dx] = s & 0xFFFFFF;
            } else if (a) {
                uint32_t d = dst[dx], out = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    uint32_t c = ((s >> sh & 255) * a + (d >> sh & 255) * (255 - a) + 127) / 255;
                    out |= c << sh;
                }
                dst[dx] = out;
            }
        }
    }
    return NSERROR_OK;
}

/* ---- fonts -------------------------------------------------------------------- */

static const char *const faces[3][2][2] = {
    { { "Homerton.Medium", "Homerton.Medium.Oblique" }, { "Homerton.Bold", "Homerton.Bold.Oblique" } },
    { { "Trinity.Medium", "Trinity.Medium.Italic" }, { "Trinity.Bold", "Trinity.Bold.Italic" } },
    { { "Corpus.Medium", "Corpus.Medium.Oblique" }, { "Corpus.Bold", "Corpus.Bold.Oblique" } },
};

#define FONTS 32
static struct {
    const char *face;
    int size;                   /* 1/16 point */
    int handle;
    unsigned used;
} fonts[FONTS];
static unsigned font_clock;

/* A handle for the style's font, found once and kept; 0 if none */
static int font(const plot_font_style_t *f)
{
    int family = f->family == PLOT_FONT_FAMILY_SERIF ? 1 : f->family == PLOT_FONT_FAMILY_MONOSPACE ? 2 : 0;
    const char *face = faces[family][f->weight >= 600][(f->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0];
    int size = (int)(f->size * 16 / PLOT_STYLE_SCALE);
    if (size < 64)
        size = 64;
    int slot = 0;
    for (int i = 0; i < FONTS; i++) {
        if (fonts[i].face == face && fonts[i].size == size) {
            fonts[i].used = ++font_clock;
            return fonts[i].handle;
        }
        if (fonts[i].used < fonts[slot].used)
            slot = i;
    }
    if (fonts[slot].handle)
        _swix(Font_LoseFont, _IN(0), fonts[slot].handle);
    char name[64];
    strcpy(name, face);
    strcat(name, "\\EUTF8");
    int handle = 0;
    if (_swix(Font_FindFont, _INR(1, 5) | _OUT(0), name, size, size, 0, 0, &handle))
        handle = 0;
    fonts[slot].face = face, fonts[slot].size = size, fonts[slot].handle = handle;
    fonts[slot].used = ++font_clock;
    return handle;
}

/* Millipoints a pixel: an OS unit is 400 */
#define MP (400 << ro_eigx)

/* Font_ScanString to x (millipoints): where it stopped, and how far across */
static void scan(int handle, const char *s, size_t len, int x, bool caret, size_t *at, int *mx)
{
    const char *end = s;
    int got = 0;
    if (_swix(Font_ScanString, _INR(0, 4) | _IN(7) | _OUT(1) | _OUT(3), handle, s,
              1 << 7 | 1 << 8 | (caret ? 1 << 17 : 0), x, 0x7FFFFFFF, (int)len, &end, &got))
        end = s + len, got = 0;
    *at = (size_t)(end - s) <= len ? (size_t)(end - s) : len;
    *mx = got;
}

static nserror l_width(const plot_font_style_t *f, const char *s, size_t len, int *width)
{
    int h = font(f);
    if (!h) {
        *width = (int)len * 8;
        return NSERROR_OK;
    }
    size_t at;
    int mx;
    scan(h, s, len, 0x7FFFFFFF, false, &at, &mx);
    *width = (mx + MP / 2) / MP;
    return NSERROR_OK;
}

static nserror l_position(const plot_font_style_t *f, const char *s, size_t len, int x, size_t *offset,
                          int *actual)
{
    int h = font(f);
    if (!h) {
        *offset = x / 8 < (int)len ? (size_t)(x > 0 ? x / 8 : 0) : len;
        *actual = (int)*offset * 8;
        return NSERROR_OK;
    }
    int mx;
    scan(h, s, len, x > 0 ? x * MP : 0, true, offset, &mx);
    *actual = (mx + MP / 2) / MP;
    return NSERROR_OK;
}

/* At the last space that fits in x, else the first after it, else the end */
static nserror l_split(const plot_font_style_t *f, const char *s, size_t len, int x, size_t *offset, int *actual)
{
    int h = font(f);
    size_t fit;
    int mx;
    if (h)
        scan(h, s, len, x > 0 ? x * MP : 0, false, &fit, &mx);
    else
        fit = x / 8 < (int)len ? (size_t)(x > 0 ? x / 8 : 0) : len;
    size_t i = fit;
    if (i < len) {
        while (i > 0 && s[i] != ' ')
            i--;
        if (i == 0) {
            i = fit;
            while (i < len && s[i] != ' ')
                i++;
        }
    }
    if (i == 0)
        i = len ? 1 : 0;
    *offset = i;
    return l_width(f, s, i, actual);
}

static struct gui_layout_table layout_table = { .width = l_width, .position = l_position, .split = l_split };
struct gui_layout_table *ro_layout_table = &layout_table;

static nserror p_text(const struct redraw_context *ctx, const plot_font_style_t *f, int x, int y, const char *s,
                      size_t len)
{
    (void)ctx;
    if (clipped() || !len)
        return NSERROR_OK;
    int h = font(f);
    if (!h)
        return NSERROR_OK;
    _swix(ColourTrans_SetFontColours, _INR(0, 3), h, (f->background & 0xFFFFFF) << 8,
          (f->foreground & 0xFFFFFF) << 8, 14);
    /* OS units (bit 4), R7 the length (7), R0 the handle (8), blended (11) */
    _swix(Font_Paint, _INR(0, 7), h, s, 1 << 4 | 1 << 7 | 1 << 8 | 1 << 11, OSX(x), OSY(y), 0, 0, (int)len);
    return NSERROR_OK;
}

const struct plotter_table ro_plotters = {
    .clip = p_clip,
    .arc = p_arc,
    .disc = p_disc,
    .line = p_line,
    .rectangle = p_rectangle,
    .polygon = p_polygon,
    .path = p_path,
    .bitmap = p_bitmap,
    .text = p_text,
    .option_knockout = true,
};
