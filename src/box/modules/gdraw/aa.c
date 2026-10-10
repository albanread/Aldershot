/* aa.c: GDraw's last stage. It turns the processed path's edges into pixels
 * or into a clip region.
 *
 * Native Draw's ProcessPath hands a fill's edges to gd_fillhook. The edges
 * are in device coordinates in 1/256 pixel, and each has a lower end, an
 * upper end and a winding. Then:
 *
 *   - Without anti-aliasing, style &38 (interior plus exterior boundary)
 *     takes GDraw's boundary rule below, with one sample at the centre of
 *     each pixel. Every other style is Draw's own scan, exactly as
 *     Draw_Fill plots it, with its plot action done through HLine. With a
 *     clip region set, the pixels are caught instead and plotted where the
 *     region covers them.
 *   - With anti-aliasing, each pixel is sampled at 4 x 4 points,
 *     (i + (2k+1)/8, j + (2l+1)/8). Its coverage n (0-16) is the count of
 *     samples that count. Under style &B0 a sample counts if it is inside
 *     under the winding rule. Under &B8 it counts by the boundary rule with
 *     arms of 1/8 pixel. Each channel is blended as (d(16 - n) + sn + 8) >> 4.
 *     At 32 and 16 bpp the channels are the colour channels. At 8 bpp and
 *     below the colour numbers themselves are blended. Every plot action
 *     blends as overwrite.
 *
 * The boundary rule: a sample (sx, sy) counts if the closed region meets its
 * horizontal arm, x in [sx - a, sx + a) at y = sy, or its vertical arm,
 * y in (sy - a, sy + a] at x = sx. Here a is half the sample spacing. The
 * horizontal arm is tested on the region's closed slice along y = sy. The
 * vertical arm meets the region if its top end is in the closed region (a
 * slice along y = sy + a). It also meets the region if an edge crosses the
 * arm strictly between its ends.
 *
 * Scan conversion: a slice of the region along a horizontal line is made
 * from its edges' crossings. They are computed exactly as fractions and
 * sorted, and the winding is accumulated from left to right. The closed
 * slice is the union of the slices just above and just below the line. Those
 * use the edges active on [lower, upper) and on (lower, upper]. The &B0
 * samples use the first slice alone.
 *
 * A region is a coverage map kept as runs. See encode() for the format. The
 * format is ROSGD's own, since clients only hand regions back.
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "gd.h"

#define REGION_MAGIC 0x47524447u        /* "GDRG" */
#define REGION_HDR   36u

static uint32_t swi(uint32_t n, uint32_t r[10], os_error **e)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 10 * sizeof r[0]);
    *e = c.v ? ros_ptr(c.r[0]) : NULL;
    return c.c;
}

/* ---- the destination ------------------------------------------------------------------------ */

struct dest {
    uint32_t l2bpp, linelen, screen, ywl, flags;
    int32_t lcol, brow, rcol, trow, xeig, yeig;
    uint32_t fg;                        /* the foreground's pixel value */
    uint32_t oe[16];                    /* the foreground's ORA and EOR words for eight
                                           rows (GcolOraEorAddr: what the kernel plots
                                           with) */
    int poked;                          /* set when the words are not the foreground's, because a module set them */
    uint32_t action;                    /* the plot action (0 is overwrite) */
};

static os_error *read_dest(struct dest *t)
{
    uint32_t blk = gdraw_ws.work;
    static const uint32_t vars[] = { 9, 6, 148, 12, 0, 128, 129, 130, 131, 4, 5, 171, 0xFFFFFFFFu };
    for (unsigned k = 0; k < sizeof vars / sizeof vars[0]; k++)
        ros_st32(blk + 4 * k, vars[k]);
    uint32_t r[10] = { blk, blk + 64 };
    os_error *e;
    swi(XOS_ReadVduVariables, r, &e);
    if (e)
        return e;
    uint32_t v = blk + 64;
    t->l2bpp = ros_ld32(v), t->linelen = ros_ld32(v + 4), t->screen = ros_ld32(v + 8);
    t->ywl = ros_ld32(v + 12), t->flags = ros_ld32(v + 16);
    t->lcol = (int32_t)ros_ld32(v + 20), t->brow = (int32_t)ros_ld32(v + 24);
    t->rcol = (int32_t)ros_ld32(v + 28), t->trow = (int32_t)ros_ld32(v + 32);
    t->xeig = (int32_t)ros_ld32(v + 36), t->yeig = (int32_t)ros_ld32(v + 40);
    /* The colour as plotted comes from the kernel's ORA and EOR tables. A
     * module may set these directly. DitherExtend_SetGCOL writes them
     * (#137). */
    uint32_t oe = ros_ld32(v + 44);
    for (unsigned k = 0; k < 16; k++)
        t->oe[k] = oe ? ros_ld32(oe + 4 * k) : 0;
    /* The graphics foreground, as OS_SetColour reads it back. This gives
     * its action and its pattern, whose first pixel is the colour. */
    uint32_t c[10] = { 0x80, blk + 128 };
    swi(XOS_SetColour, c, &e);
    if (e)
        return e;
    t->action = c[0] & 0x0F;
    uint32_t w = ros_ld32(blk + 128), bpp = 1u << (t->l2bpp & 31);
    t->fg = bpp >= 32 ? w : w & ((1u << bpp) - 1);
    /* Overwrite's tables are ORA all ones and EOR the colour inverted. If a
     * row's colour there is not the foreground's, a module has written the
     * tables directly, and the colour is the module's. DitherExtend_SetGCOL
     * does this without a SWI. It writes its dithered colour row by row
     * (#137). */
    t->poked = 0;
    if (t->action == 0)
        for (unsigned k = 0; k < 8; k++) {
            uint32_t ora = t->oe[2 * k], eor = t->oe[2 * k + 1];
            uint32_t m = bpp >= 32 ? 0xFFFFFFu : (1u << bpp) - 1;
            if (ora != 0xFFFFFFFFu || ((~eor) & m) != (t->fg & m))
                t->poked = 1;
        }
    return NULL;
}

/* ---- coverage maps ------------------------------------------------------------------------- */

static int cov_alloc(struct gd_cov *cv, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    cv->c = NULL;
    cv->x0 = x0, cv->y0 = y0, cv->w = x1 - x0 + 1, cv->h = y1 - y0 + 1;
    if (cv->w <= 0 || cv->h <= 0) {
        cv->w = cv->h = 0;
        return 0;
    }
    cv->c = calloc((size_t)cv->w * (size_t)cv->h, 1);
    return cv->c != NULL;
}

/* Clip a span of Draw's scan to the map and find its row in the map. */
static void span_clip(struct gd_cov *cv, int32_t *a, int32_t y, int32_t *b, uint8_t **line)
{
    if (*a > *b) {
        int32_t t = *a;
        *a = *b, *b = t;
    }
    *line = NULL;
    if (y < cv->y0 || y >= cv->y0 + cv->h)
        return;
    if (*a < cv->x0)
        *a = cv->x0;
    if (*b >= cv->x0 + cv->w)
        *b = cv->x0 + cv->w - 1;
    if (*a <= *b)
        *line = cv->c + (size_t)(y - cv->y0) * (size_t)cv->w - cv->x0;
}

static void mark_span(struct draw *d, int32_t a, int32_t y, int32_t b)
{
    (void)d;
    uint8_t *line;
    span_clip(gdraw_ws.mark, &a, y, &b, &line);
    if (line)
        for (int32_t x = a; x <= b; x++)
            line[x] = 16;
}

/* Run Draw's scan of the edges with the given style, and put its spans in
 * the map. */
static void scan_into(struct draw *d, struct gd_cov *cv, uint32_t style)
{
    uint32_t keep = d->flags;
    d->flags = (keep & ~(uint32_t)F_STYLEMASK) | style;
    gdraw_ws.mark = cv;
    d->spanhook = mark_span;
    draw_scan(d);
    d->spanhook = NULL;
    d->flags = keep;
}

/* ---- anti-aliased coverage ---------------------------------------------------------------- */

struct aedge {
    int64_t ly, uy, lx, dx, dy;
    int32_t w;
};

struct xing {
    int64_t m;
    int32_t w;
};

static int by_lower(const void *a, const void *b)
{
    const struct aedge *p = a, *q = b;
    return p->ly < q->ly ? -1 : p->ly > q->ly;
}

static int64_t ceil_div(int64_t a, int64_t b)       /* b > 0 */
{
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}

/* Whether winding number w is inside under rule (one of Draw's four). */
static int inside(uint32_t rule, int32_t w)
{
    switch (rule & 3) {
    case 0: return w != 0;
    case 1: return w < 0;
    case 2: return w & 1;
    default: return w > 0;
    }
}

/* Count the samples in columns [a, b) of one sub-scanline into line. A
 * column here is a quarter of a pixel. */
static void add_samples(const struct gd_cov *cv, uint8_t *line, int64_t a, int64_t b)
{
    int64_t lo = (int64_t)cv->x0 * 4, hi = (int64_t)(cv->x0 + cv->w) * 4;
    if (a < lo)
        a = lo;
    if (b > hi)
        b = hi;
    uint8_t *p = line - cv->x0;
    while (a < b && (a & 3))
        p[a >> 2]++, a++;
    while (b - a >= 4)
        p[a >> 2] += 4, a += 4;
    while (a < b)
        p[a >> 2]++, a++;
}

static void raster(struct draw *d, struct gd_cov *cv, uint32_t rule)
{
    uint32_t n = draw_edge_count(d);
    struct aedge *e = malloc((n ? n : 1) * sizeof *e);
    uint32_t *act = malloc((n ? n : 1) * sizeof *act);
    struct xing *x = malloc((n ? n : 1) * sizeof *x);
    if (!e || !act || !x)
        goto out;
    int64_t ybot = (int64_t)cv->y0 * 256, ytop = (int64_t)(cv->y0 + cv->h) * 256;
    uint32_t ne = 0;
    for (uint32_t k = 1; k < n; k++) {
        int32_t w[4];
        int32_t wd = draw_edge(d, k, w);
        int64_t lx = w[0], ly = w[1], ux = w[2], uy = w[3];
        if (!wd || ly == uy)
            continue;
        if (ly > uy) {
            int64_t t = ly;
            ly = uy, uy = t, t = lx, lx = ux, ux = t;
        }
        if (uy <= ybot || ly >= ytop)
            continue;
        e[ne].ly = ly, e[ne].uy = uy, e[ne].lx = lx;
        e[ne].dx = ux - lx, e[ne].dy = uy - ly, e[ne].w = wd;
        ne++;
    }
    qsort(e, ne, sizeof *e, by_lower);
    uint32_t next = 0, na = 0;
    for (int32_t row = 0; row < cv->h; row++) {
        uint8_t *line = cv->c + (size_t)row * (size_t)cv->w;
        for (int l = 0; l < 4; l++) {
            int64_t ys = ((int64_t)cv->y0 + row) * 256 + 32 + 64 * l;
            while (next < ne && e[next].ly <= ys)
                act[na++] = next++;
            uint32_t keep = 0, nx = 0;
            for (uint32_t i = 0; i < na; i++) {
                const struct aedge *g = &e[act[i]];
                if (g->uy <= ys)
                    continue;
                act[keep++] = act[i];
                int64_t num = (g->lx - 32) * g->dy + (ys - g->ly) * g->dx;
                int64_t m = ceil_div(num, 64 * g->dy);
                uint32_t j = nx++;
                while (j > 0 && x[j - 1].m > m) {
                    x[j] = x[j - 1];
                    j--;
                }
                x[j].m = m, x[j].w = g->w;
            }
            na = keep;
            /* Draw's ProcessPath leaves out edges that are wholly to the
             * right of the graphics window. Its own scan carries the winding
             * on to the window's edge. So a sample that is inside after the
             * last crossing is inside to the map's right end (#137). */
            int32_t wsum = 0;
            for (uint32_t i = 0; i < nx; i++) {
                wsum += x[i].w;
                if (i + 1 < nx && x[i + 1].m == x[i].m)
                    continue;
                if (inside(rule, wsum))
                    add_samples(cv, line, x[i].m,
                                i + 1 < nx ? x[i + 1].m : (int64_t)(cv->x0 + cv->w) * 4);
            }
        }
    }
out:
    free(e), free(act), free(x);
}

/* ---- the boundary rule ---------------------------------------------------------------------- */

typedef __int128 i128;

struct bedge {
    int64_t lx, ly, ux, uy;             /* the lower and upper ends (ly <= uy) */
    int32_t w;                          /* the winding, as Draw's */
};

struct frac {
    i128 n;
    int64_t d;                          /* > 0 */
    int32_t w;
};

static int frac_less(const struct frac *a, const struct frac *b)
{
    return a->n * b->d < b->n * a->d;
}

static i128 floor_div(i128 a, i128 b)   /* b > 0 */
{
    i128 q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static i128 ceil_div128(i128 a, i128 b) /* b > 0 */
{
    return -floor_div(-a, b);
}

/* Find the closed intervals [a, b] of the region along y = c under one
 * convention. If below is set, the edges active are those on (lower, upper].
 * Otherwise they are those on [lower, upper). The intervals are appended to
 * iv in pairs, starting at index n. This returns the new count. */
static uint32_t slice1(const struct bedge *e, uint32_t ne, int64_t c, int below, uint32_t rule,
                       struct frac *x, struct frac *iv, uint32_t n)
{
    uint32_t nx = 0;
    for (uint32_t k = 0; k < ne; k++) {
        const struct bedge *g = &e[k];
        if (g->ly == g->uy)
            continue;
        if (below ? !(g->ly < c && c <= g->uy) : !(g->ly <= c && c < g->uy))
            continue;
        int64_t dy = g->uy - g->ly;
        struct frac f = { (i128)g->lx * dy + (i128)(c - g->ly) * (g->ux - g->lx), dy, g->w };
        uint32_t j = nx++;
        while (j > 0 && frac_less(&f, &x[j - 1])) {
            x[j] = x[j - 1];
            j--;
        }
        x[j] = f;
    }
    /* Inside after the last crossing runs to the right without end, as in
     * raster(). Draw drops edges to the right of the window. */
    int32_t wsum = 0;
    for (uint32_t i = 0; i < nx; i++) {
        wsum += x[i].w;
        if (!inside(rule, wsum))
            continue;
        iv[n++] = x[i];
        if (i + 1 < nx)
            iv[n++] = x[i + 1];
        else
            iv[n++] = (struct frac){ (i128)1 << 80, 1, 0 };
    }
    return n;
}

static uint32_t slice(const struct bedge *e, uint32_t ne, int64_t c, uint32_t rule, struct frac *x,
                      struct frac *iv)
{
    return slice1(e, ne, c, 1, rule, x, iv, slice1(e, ne, c, 0, rule, x, iv, 0));
}

/* Find the samples that count under the boundary rule, at spacing S. A
 * spacing of 64 is &B8's 16 samples a pixel, and 256 is the pixel centre.
 * Each is counted into its pixel's coverage. A sample counts for 16 divided
 * by the number of samples a pixel has, so a pixel of full samples has
 * coverage 16. */
static void boundary_raster(struct draw *d, struct gd_cov *cv, uint32_t rule, int64_t S)
{
    int64_t per = 256 / S, half = S / 2;
    int64_t m0 = (int64_t)cv->x0 * per, j0 = (int64_t)cv->y0 * per;
    int64_t mw = (int64_t)cv->w * per, jh = (int64_t)cv->h * per;
    uint32_t n = draw_edge_count(d);
    struct bedge *e = malloc((n ? n : 1) * sizeof *e);
    struct frac *x = malloc((n ? n : 1) * sizeof *x);
    struct frac *iv = malloc((4 * n + 4) * sizeof *iv);
    uint8_t *hit = calloc((size_t)(mw * jh), 1);
    if (!e || !x || !iv || !hit)
        goto out;
    uint32_t ne = 0;
    for (uint32_t k = 1; k < n; k++) {
        int32_t w[4];
        int32_t wd = draw_edge(d, k, w);
        if (!wd)
            continue;
        struct bedge g = { w[0], w[1], w[2], w[3], wd };
        if (g.ly > g.uy) {
            int64_t t = g.ly;
            g.ly = g.uy, g.uy = t, t = g.lx, g.lx = g.ux, g.ux = t;
        }
        e[ne++] = g;
    }
#define MARK(m, j) do { \
        int64_t mm_ = (m) - m0, jj_ = (j) - j0; \
        if (mm_ >= 0 && mm_ < mw && jj_ >= 0 && jj_ < jh) \
            hit[jj_ * mw + mm_] = 1; \
    } while (0)
    for (int64_t j = j0; j < j0 + jh; j++) {
        /* The horizontal arms. The closed slice at the samples' y meets
         * [S m, S m + S). */
        uint32_t ni = slice(e, ne, S * j + half, rule, x, iv);
        for (uint32_t i = 0; i < ni; i += 2) {
            i128 a = floor_div(iv[i].n, (i128)S * iv[i].d);
            i128 b = floor_div(iv[i + 1].n, (i128)S * iv[i + 1].d);
            if (a < m0) a = m0;
            if (b > m0 + mw - 1) b = m0 + mw - 1;
            for (i128 m = a; m <= b; m++)
                MARK((int64_t)m, j);
        }
        /* the vertical arms' top ends, which are in the closed slice at
         * y = S (j + 1) */
        ni = slice(e, ne, S * (j + 1), rule, x, iv);
        for (uint32_t i = 0; i < ni; i += 2) {
            i128 a = ceil_div128(iv[i].n - (i128)half * iv[i].d, (i128)S * iv[i].d);
            i128 b = floor_div(iv[i + 1].n - (i128)half * iv[i + 1].d, (i128)S * iv[i + 1].d);
            if (a < m0) a = m0;
            if (b > m0 + mw - 1) b = m0 + mw - 1;
            for (i128 m = a; m <= b; m++)
                MARK((int64_t)m, j);
        }
    }
    /* the vertical arms crossed by an edge strictly between their ends */
    for (uint32_t k = 0; k < ne; k++) {
        const struct bedge *g = &e[k];
        if (g->lx == g->ux) {
            if (g->ly == g->uy || floor_div((i128)g->lx - half, S) * S != (i128)g->lx - half)
                continue;
            int64_t m = (int64_t)floor_div((i128)g->lx - half, S);
            int64_t ja = (int64_t)floor_div(g->ly, S), jb = (int64_t)ceil_div128(g->uy, S) - 1;
            for (int64_t j = ja < j0 ? j0 : ja; j <= jb && j < j0 + jh; j++)
                MARK(m, j);
            continue;
        }
        int64_t xa = g->lx < g->ux ? g->lx : g->ux, xb = g->lx < g->ux ? g->ux : g->lx;
        i128 ma = ceil_div128((i128)xa - half, S), mb = floor_div((i128)xb - half, S);
        if (ma < m0) ma = m0;
        if (mb > m0 + mw - 1) mb = m0 + mw - 1;
        int64_t dx = g->ux - g->lx, dy = g->uy - g->ly;
        for (i128 m = ma; m <= mb; m++) {
            i128 sx = m * S + half;
            i128 num = (i128)g->ly * dx + (sx - g->lx) * dy, den = dx;
            if (den < 0)
                num = -num, den = -den;
            if (num % (S * den) == 0)
                continue;                       /* on a cell's edge, so the top end's test covers it */
            MARK((int64_t)m, (int64_t)floor_div(num, S * den));
        }
    }
#undef MARK
    for (int32_t r = 0; r < cv->h; r++)
        for (int32_t c = 0; c < cv->w; c++) {
            uint32_t cnt = 0;
            for (int64_t jj = 0; jj < per; jj++)
                for (int64_t mm = 0; mm < per; mm++)
                    cnt += hit[((int64_t)r * per + jj) * mw + (int64_t)c * per + mm];
            cv->c[(size_t)r * (size_t)cv->w + (size_t)c] = (uint8_t)(cnt * 16 / (uint32_t)(per * per));
        }
out:
    free(e), free(x), free(iv), free(hit);
}

/* ---- regions ------------------------------------------------------------------------------- */

/* A region, from word 0 of its buffer:
 *
 *   +0  0, and +4 the room the caller gave (Draw's output-buffer header,
 *       which is kept)
 *   +8  "GDRG"      +12 its size in bytes, from +0
 *   +16 x0, +20 y0, +24 width, +28 height: the pixels it spans
 *   +32 flags: bit 0 is set if it was made anti-aliased
 *   +36 each row from the bottom: a count of runs, then for each run its x,
 *       its length n, and n coverage bytes (0-16), padded to a word
 *
 * Every offset is relative, so a region can be copied. */

static uint32_t pad4(uint32_t n) { return (n + 3) & ~3u; }

/* Write a map into the region buffer as runs. Each run is a [start, end)
 * stretch of non-zero coverage in one row. This returns the region's size in
 * bytes, or sets *err if it does not fit. */
static uint32_t encode(struct gdraw *g, const struct gd_cov *cv, os_error **err)
{
    uint32_t size = REGION_HDR;
    for (int32_t r = 0; r < cv->h; r++) {
        const uint8_t *line = cv->c + (size_t)r * (size_t)cv->w;
        size += 4;
        for (int32_t x = 0; x < cv->w;) {
            if (!line[x]) {
                x++;
                continue;
            }
            int32_t s = x;
            while (x < cv->w && line[x])
                x++;
            size += 8 + pad4((uint32_t)(x - s));
        }
    }
    *err = NULL;
    if (size > g->regionsize) {
        *err = draw_err(DE_PATHFULL);
        return 0;
    }
    uint32_t p = g->region;
    ros_st32(p, 0);
    ros_st32(p + 8, REGION_MAGIC), ros_st32(p + 12, size);
    ros_st32(p + 16, (uint32_t)cv->x0), ros_st32(p + 20, (uint32_t)cv->y0);
    ros_st32(p + 24, (uint32_t)cv->w), ros_st32(p + 28, (uint32_t)cv->h);
    ros_st32(p + 32, g->aa ? 1 : 0);
    uint32_t q = p + REGION_HDR;
    for (int32_t r = 0; r < cv->h; r++) {
        const uint8_t *line = cv->c + (size_t)r * (size_t)cv->w;
        uint32_t count = q, runs = 0;
        q += 4;
        for (int32_t x = 0; x < cv->w;) {
            if (!line[x]) {
                x++;
                continue;
            }
            int32_t s = x;
            while (x < cv->w && line[x])
                x++;
            uint32_t len = (uint32_t)(x - s);
            ros_st32(q, (uint32_t)(cv->x0 + s)), ros_st32(q + 4, len);
            q += 8;
            memcpy(ros_ptr(q), line + s, len);
            memset((uint8_t *)ros_ptr(q) + len, 0, pad4(len) - len);
            q += pad4(len);
            runs++;
        }
        ros_st32(count, runs);
    }
    return size;
}

int gd_region_valid(uint32_t region)
{
    if (!region || (region & 3))
        return 0;
    return ros_ld32(region + 8) == REGION_MAGIC && ros_ld32(region + 12) >= REGION_HDR &&
           (int32_t)ros_ld32(region + 24) >= 0 && (int32_t)ros_ld32(region + 28) >= 0;
}

/* Put the region's coverage, moved by (dx, dy), into a map. With mult 0 it
 * is copied in. With mult 1 it is multiplied into the map, giving 0 where
 * the region has no coverage. */
static void region_apply(uint32_t reg, int32_t dx, int32_t dy, struct gd_cov *cv, int mult)
{
    uint8_t *tmp = cv->c;
    size_t size = (size_t)cv->w * (size_t)cv->h;
    if (!size)
        return;
    if (mult && !(tmp = calloc(size, 1)))
        return;
    int32_t y0 = (int32_t)ros_ld32(reg + 20) + dy, h = (int32_t)ros_ld32(reg + 28);
    uint32_t q = reg + REGION_HDR;
    for (int32_t r = 0; r < h; r++) {
        uint32_t runs = ros_ld32(q);
        q += 4;
        int32_t y = y0 + r;
        for (uint32_t k = 0; k < runs; k++) {
            int32_t x = (int32_t)ros_ld32(q) + dx;
            uint32_t len = ros_ld32(q + 4);
            const uint8_t *c = ros_ptr(q + 8);
            q += 8 + pad4(len);
            if (y < cv->y0 || y >= cv->y0 + cv->h)
                continue;
            uint8_t *line = tmp + (size_t)(y - cv->y0) * (size_t)cv->w;
            for (uint32_t i = 0; i < len; i++) {
                int32_t px = x + (int32_t)i;
                if (px >= cv->x0 && px < cv->x0 + cv->w)
                    line[px - cv->x0] = c[i] > 16 ? 16 : c[i];
            }
        }
    }
    if (mult) {
        for (size_t i = 0; i < size; i++)
            cv->c[i] = (uint8_t)((cv->c[i] * tmp[i] + 8) >> 4);
        free(tmp);
    }
}

/* Clip later plotting to the current region, if there is one. */
static void clip_to_region(struct gd_cov *cv)
{
    uint32_t reg = gdraw_ws.clip;
    if (reg && reg != 0xFFFFFFFFu && gd_region_valid(reg))
        region_apply(reg, 0, 0, cv, 1);
}

static int clipping(void)
{
    uint32_t reg = gdraw_ws.clip;
    return reg && reg != 0xFFFFFFFFu && gd_region_valid(reg);
}

/* ---- plotting ------------------------------------------------------------------------------ */

static uint32_t blend(uint32_t d, uint32_t s, uint32_t n, const uint32_t *masks, int count)
{
    uint32_t out = 0;
    for (int k = 0; k < count; k++) {
        uint32_t m = masks[k], sh = (uint32_t)__builtin_ctz(m);
        uint32_t dv = (d & m) >> sh, sv = (s & m) >> sh;
        out |= (((dv * (16 - n) + sv * n + 8) >> 4) << sh) & m;
    }
    return out;
}

/* A graduated fill's colour as the destination's pixel value. At 32 bpp it
 * is the COLORREF's own bytes (&00BBGGRR). Below that it is ColourTrans's
 * colour number for the colour. The last one is kept, because neighbouring
 * pixels repeat it. */
static uint32_t grad_pixel(const struct dest *t, int32_t x, int32_t y)
{
    static uint32_t last_ref = 0xFFFFFFFFu, last_pix, last_l2bpp;
    uint32_t ref = gd_grad_colour(x, y) & 0xFFFFFFu;
    if (t->l2bpp == 5)
        return ref;
    if (ref != last_ref || t->l2bpp != last_l2bpp) {
        uint32_t r[10] = { ref << 8 };
        os_error *e;
        swi(XColourTrans_ReturnColourNumber, r, &e);
        last_ref = ref, last_l2bpp = t->l2bpp, last_pix = e ? 0 : r[0];
    }
    return last_pix;
}

/* Plot the map's pixels. The colour is the foreground colour, or a
 * graduated fill's colour pixel by pixel (grad). With blending, the pixels
 * are blended by coverage, and every plot action acts as overwrite. Without
 * blending, the pixels are plotted Draw's way, through HLine with the plot
 * action, where they are at least half covered. */
static void plot(const struct dest *t, const struct gd_cov *cv, int blending, int grad)
{
    if (!blending) {
        for (int32_t r = 0; r < cv->h; r++) {
            const uint8_t *line = cv->c + (size_t)r * (size_t)cv->w;
            for (int32_t x = 0; x < cv->w;) {
                if (line[x] < 8) {
                    x++;
                    continue;
                }
                int32_t s = x;
                while (x < cv->w && line[x] >= 8)
                    x++;
                ros_vdu_hline(cv->x0 + s, cv->y0 + r, cv->x0 + x - 1, 1);
            }
        }
        return;
    }
    /* The channels are the colour channels at 16 and 32 bpp, and the colour
     * number below that. */
    uint32_t masks[3], keep = 0, bpp = 1u << t->l2bpp;
    int count = 3;
    if (t->l2bpp == 5) {
        masks[0] = 0xFF, masks[1] = 0xFF00, masks[2] = 0xFF0000, keep = 0xFF000000u;
    } else if (t->l2bpp == 4) {
        switch ((t->flags >> 12) & 3) {
        case 1: masks[0] = 0x1F, masks[1] = 0x7E0, masks[2] = 0xF800; break;
        case 2: masks[0] = 0xF, masks[1] = 0xF0, masks[2] = 0xF00, keep = 0xF000; break;
        default: masks[0] = 0x1F, masks[1] = 0x3E0, masks[2] = 0x7C00, keep = 0x8000; break;
        }
    } else if (t->l2bpp <= 3) {
        masks[0] = (1u << bpp) - 1, count = 1;
    } else {
        return;
    }
    uint32_t s = t->fg;
    for (int32_t r = 0; r < cv->h; r++) {
        const uint8_t *line = cv->c + (size_t)r * (size_t)cv->w;
        int32_t y = cv->y0 + r;
        uint8_t *row = (uint8_t *)ros_ptr(t->screen + (t->ywl - (uint32_t)y) * t->linelen);
        uint32_t erow = (t->ywl - (uint32_t)y) & 7;     /* the kernel's plot_erow */
        uint32_t ora = t->oe[2 * erow], eor = t->oe[2 * erow + 1];
        for (int32_t i = 0; i < cv->w; i++) {
            uint32_t n = line[i];
            if (!n)
                continue;
            int32_t x = cv->x0 + i;
            if (t->l2bpp == 5) {
                uint32_t *p = (uint32_t *)(row + 4 * x);
                s = grad ? grad_pixel(t, x, y) : t->poked ? (ora ^ eor) : t->fg;
                *p = n >= 16 ? s : (s & keep) | blend(*p, s, n, masks, count);
            } else if (t->l2bpp == 4) {
                uint16_t *p = (uint16_t *)(row + 2 * x);
                uint32_t sh = 16 * ((uint32_t)x & 1);
                s = grad ? grad_pixel(t, x, y) : t->poked ? ((ora ^ eor) >> sh) & 0xFFFF : t->fg;
                *p = (uint16_t)(n >= 16 ? s : (s & keep) | blend(*p, s, n, masks, count));
            } else {
                uint32_t bit = (uint32_t)x * bpp, sh = bit & 7, m = masks[0];
                uint8_t *p = row + (bit >> 3);
                uint32_t dv = (*p >> sh) & m, wsh = bit & 31;
                s = grad ? grad_pixel(t, x, y) : t->poked ? ((ora ^ eor) >> wsh) & m : t->fg;
                uint32_t v = n >= 16 ? s & m : blend(dv, s, n, masks, 1);
                *p = (uint8_t)((*p & ~(m << sh)) | (v << sh));
            }
        }
    }
}

/* ---- the fill hook -------------------------------------------------------------------------- */

os_error *gd_fillhook(struct draw *d)
{
    struct gdraw *g = &gdraw_ws;
    struct dest t;
    os_error *e = read_dest(&t);
    if (e)
        return e;
    uint32_t style = d->flags & F_STYLEMASK;
    int boundary = (style & 0x3C) == 0x38;      /* &B8, or GDraw's own &38 */
    if (!g->region) {
        if (!t.screen)
            return NULL;
        if (!g->aa && !boundary && !clipping() && g->grad != GD_GRAD_READY) {
            draw_scan(d);                       /* Draw's own plotting, exactly */
            return NULL;
        }
    }
    /* The map covers the edges' box, a pixel wider for the boundary rule,
     * and lies within the graphics window. */
    int32_t x0 = d->rcol + 1, y0 = d->trow + 1, x1 = d->lcol - 1, y1 = d->brow - 1;
    uint32_t n = draw_edge_count(d);
    int64_t balance = 0;                        /* the sum of winding times height, which is 0 for a closed outline */
    for (uint32_t k = 1; k < n; k++) {
        int32_t w[4];
        int32_t wd = draw_edge(d, k, w);
        balance += (int64_t)wd * (w[3] > w[1] ? w[3] - w[1] : w[1] - w[3]);
        for (int j = 0; j < 4; j += 2) {
            int32_t px = w[j] >> 8, py = w[j + 1] >> 8;
            if (px < x0) x0 = px;
            if (px > x1) x1 = px;
            if (py < y0) y0 = py;
            if (py > y1) y1 = py;
        }
    }
    /* Draw left out edges that are to the right of the window, and kept the
     * ones to the left. So the shape goes on to the window's right edge. */
    if (balance)
        x1 = d->rcol;
    if (boundary && x0 <= x1)
        x0--, y0--, x1++, y1++;
    if (x0 < d->lcol) x0 = d->lcol;
    if (y0 < d->brow) y0 = d->brow;
    if (x1 > d->rcol) x1 = d->rcol;
    if (y1 > d->trow) y1 = d->trow;
    struct gd_cov cv;
    if (!cov_alloc(&cv, x0, y0, x1, y1) && cv.w)
        return ros_error(0x101, "No room in RMA");
    if (cv.w) {
        if (boundary)
            boundary_raster(d, &cv, style & 3, g->aa ? 64 : 256);
        else if (g->aa)
            raster(d, &cv, style & 3);
        else
            scan_into(d, &cv, style);
    }
    if (g->region) {
        encode(g, &cv, &e);
    } else if (cv.w) {
        clip_to_region(&cv);
        int grad = g->grad == GD_GRAD_READY;
        plot(&t, &cv, g->aa || grad, grad);
    }
    free(cv.c);
    return e;
}

/* ---- FillRegion ----------------------------------------------------------------------------- */

os_error *gd_fill_region(uint32_t reg, int32_t dx, int32_t dy)
{
    struct dest t;
    os_error *e = read_dest(&t);
    if (e)
        return e;
    if (!t.screen || (t.flags & 1))
        return NULL;
    dx >>= t.xeig, dy >>= t.yeig;
    int32_t x0 = (int32_t)ros_ld32(reg + 16) + dx, y0 = (int32_t)ros_ld32(reg + 20) + dy;
    int32_t x1 = x0 + (int32_t)ros_ld32(reg + 24) - 1, y1 = y0 + (int32_t)ros_ld32(reg + 28) - 1;
    if (x0 < t.lcol) x0 = t.lcol;
    if (y0 < t.brow) y0 = t.brow;
    if (x1 > t.rcol) x1 = t.rcol;
    if (y1 > t.trow) y1 = t.trow;
    struct gd_cov cv;
    if (!cov_alloc(&cv, x0, y0, x1, y1))
        return cv.w ? ros_error(0x101, "No room in RMA") : NULL;
    region_apply(reg, dx, dy, &cv, 0);
    clip_to_region(&cv);
    uint32_t r[10] = { 0 };
    swi(XOS_RemoveCursors, r, &e);
    plot(&t, &cv, ros_ld32(reg + 32) & 1, 0);     /* an anti-aliased region is blended */
    memset(r, 0, sizeof r);
    swi(XOS_RestoreCursors, r, &e);
    /* add to the OS's ChangedBox, as Draw's fills do */
    uint32_t c[10] = { 0xFFFFFFFFu };
    swi(XOS_ChangedBox, c, &e);
    if (!e && (ros_ld32(c[1]) & 1)) {
        uint32_t a = c[1] + 4;
        int32_t b[4] = { x0, y0, x1, y1 };
        for (int k = 0; k < 4; k++) {
            int32_t v = (int32_t)ros_ld32(a + 4 * (uint32_t)k);
            if (k < 2 ? b[k] < v : b[k] > v)
                ros_st32(a + 4 * (uint32_t)k, (uint32_t)b[k]);
        }
    }
    free(cv.c);
    return NULL;
}
