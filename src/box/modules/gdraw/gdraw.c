/* gdraw.c: GDraw, the anti-aliasing Draw. It holds the module, its SWIs and
 * its DrawV claim. Programs run under ARM emulation use the real GDraw
 * module. This native version is reimplemented from a specification.
 *
 * SWIs 0-11 are Draw's, with Draw's registers. They run native Draw's own
 * code (modules/draw). That is Draw's registers for each reason
 * (draw_reason_regs), and then its ProcessPath on GDraw's workspace. So
 * paths, buffers, counts and boxes come out byte for byte as Draw's. Fill
 * style bit 7 means anti-alias. A fill's edges go to GDraw's own last stage
 * (aa.c). That stage also has GDraw's own style &38 without bit 7. Other
 * styles plot as Draw's do. Bit 6 is Draw's clipping "buffer" bit and is
 * ignored, so &F0 draws as &B0. Every ...FP SWI gives "Facility not in this
 * version of Draw".
 *
 * Anti-aliasing is allowed only with winding rules 0 (non-zero) and 2
 * (even-odd), and plot bits &30 or &38. Anything else gives an error
 * "... not available in this version of Draw". A thin stroke is never
 * anti-aliased, and with bit 7 it gives an error. A thick stroke with plot
 * bits 0 takes &30.
 *
 * SWIs 12-24 are GDraw's own. They are the clip regions (made by
 * ProcessClipPath and ClipPath, and then set, cleared, read and filled),
 * the fill style, and the print flag, which is only recorded.
 * ClipPathToPath's registers are not known, so it is refused.
 *
 * The fill style's graduated fills (#137) were measured against GDraw 3.12
 * run by the ARM container, as ArtWorks' renderer uses them. SetFillStyle
 * type 2 (linear) or type 1 (radial) with bit 16 sets one. R1 points to a
 * table of COLORREFs. The table has 256 entries, or 2048 entries, eight to
 * a step and dithered by row, when R0 bits 8-9 are 1. The next Stroke is not
 * drawn. Its path's first two points are the axis. The next Fill or Stroke
 * after that is painted from the table, pixel by pixel, and the style is
 * then used up. gd_grad_colour has the arithmetic.
 *
 * Reasons 61-63 have no names. They read, claim and release DrawV. While
 * GDraw has DrawV, every Draw SWI is GDraw's SWI of the same number. GDraw's
 * own SWIs 0-13 go through DrawV, so that whoever claimed it later sees them
 * first.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "gd.h"
#include "gdraw.h"

#define DRAWV 0x20
#define LAST_REASON 24

struct gdraw gdraw_ws;

/* ---- errors --------------------------------------------------------------------------------- */

static os_error *facility(void)
{
    return draw_err(DE_UNIMPLEMENTED);
}

static os_error *no_winding(void)
{
    return ros_error(DE_UNIMPLEMENTED, "Winding rule not available in this version of Draw");
}

static os_error *no_plot(void)
{
    return ros_error(DE_UNIMPLEMENTED, "Plot rule not available in this version of Draw");
}

/* Check the winding rule and plot bits of an anti-aliased style. */
static os_error *aa_check(uint32_t style)
{
    uint32_t rule = style & F_RULE, plot = style & 0x3C;
    if (rule == 1 || rule == 3)
        return no_winding();
    if (plot != 0x30 && plot != 0x38)
        return no_plot();
    return NULL;
}

/* ---- the path SWIs ------------------------------------------------------------------------- */

/* Draw's ProcessPath on GDraw's workspace, with the fill's edges going to
 * aa.c. If out is not 0, a region is built at out, which has room bytes. */
static os_error *run(const uint32_t r[8], uint32_t *r0, int aa, uint32_t out, uint32_t room)
{
    struct gdraw *g = &gdraw_ws;
    g->aa = aa;
    g->region = out, g->regionsize = room;
    g->d.fillhook = gd_fillhook;
    g->d.spanhook = NULL;
    os_error *e = draw_process_path(&g->d, r, r0);
    g->aa = 0, g->region = 0;
    return e;
}

/* Whether ProcessPath's R7 asks for a fill (output types 1 and 2). */
static int fills(const uint32_t r[8])
{
    uint32_t flags = r[1], r7 = r[7];
    if (flags & F_R7IS32BIT)
        return !(flags & F_R7ISBBOX) && (r7 == SPEC_FILL || r7 == SPEC_FILLBYSUBPATHS);
    return !(r7 & 0x80000000u) && (r7 == SPEC_FILL || r7 == SPEC_FILLBYSUBPATHS);
}

/* GDraw_ProcessPath is Draw's, but bit 7 anti-aliases a fill. */
static os_error *process_path(uint32_t r[8], uint32_t *r0)
{
    int aa = 0;
    if (fills(r) && !((r[1] & ~(uint32_t)F_STYLEMASK) & ~F_FLAGSMASK)) {
        aa = (r[1] & GD_AA) != 0;
        os_error *e = aa ? aa_check(r[1] & F_STYLEMASK) : NULL;
        if (e)
            return e;
        r[1] &= ~(GD_AA | GD_BIT6);
    }
    return run(r, r0, aa, 0, 0);
}

/* GDraw_Fill. */
static os_error *fill(uint32_t r[8], uint32_t *r0)
{
    os_error *e = draw_reason_regs(2, r);
    if (e)
        return e;
    int aa = (r[1] & GD_AA) != 0;
    if (aa && (e = aa_check(r[1] & F_STYLEMASK)))
        return e;
    r[1] &= ~(GD_AA | GD_BIT6);
    return run(r, r0, aa, 0, 0);
}

/* A graduated fill's axis. It is the path's first point (a move) and the
 * next (a line), taken through the matrix and converted to pixels. The
 * graphics origin is added, the eig factors are divided out, and the result
 * is rounded down to whole pixels. This returns 0 if the path does not
 * start with a move and a line. */
static int grad_axis(const uint32_t r[8])
{
    struct gdraw *g = &gdraw_ws;
    uint32_t p = r[0];
    if ((ros_ld32(p) & 0xFF) != 2 || (ros_ld32(p + 12) & 0xFF) != 8)
        return 0;
    double pt[4] = { (int32_t)ros_ld32(p + 4), (int32_t)ros_ld32(p + 8),
                     (int32_t)ros_ld32(p + 16), (int32_t)ros_ld32(p + 20) };
    double m[6] = { 65536, 0, 0, 65536, 0, 0 };
    if (r[2])
        for (int k = 0; k < 6; k++)
            m[k] = (int32_t)ros_ld32(r[2] + 4 * (uint32_t)k);
    /* OrgX, OrgY (OS units), XEigFactor, YEigFactor */
    uint32_t blk = g->work;
    static const uint32_t vars[] = { 136, 137, 4, 5, 0xFFFFFFFFu };
    for (unsigned k = 0; k < 5; k++)
        ros_st32(blk + 4 * k, vars[k]);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = blk, c.r[1] = blk + 64;
    ros_swi(&c, XOS_ReadVduVariables);
    if (c.v)
        return 0;
    double ox = (int32_t)ros_ld32(blk + 64), oy = (int32_t)ros_ld32(blk + 68);
    double ex = (double)(1u << (ros_ld32(blk + 72) & 7)), ey = (double)(1u << (ros_ld32(blk + 76) & 7));
    double q[4];
    for (int k = 0; k < 4; k += 2) {
        double x = (m[0] * pt[k] + m[2] * pt[k + 1]) / 65536.0 + m[4];
        double y = (m[1] * pt[k] + m[3] * pt[k + 1]) / 65536.0 + m[5];
        q[k] = floor((x / 256.0 + ox) / ex);    /* whole pixels, as GDraw 3.12 takes them */
        q[k + 1] = floor((y / 256.0 + oy) / ey);
    }
    g->grad_sx = q[0], g->grad_sy = q[1], g->grad_ex = q[2], g->grad_ey = q[3];
    return 1;
}

/* The colour at pixel (x, y), using the pixel's corner. First find the
 * step along the axis (linear) or out from its start (radial). The step is
 * 0-255, in 256ths of the axis's length. It is rounded down from just
 * below, so an exact multiple gives the step before it, as GDraw does. The
 * table entry is then the step itself. With the dither (bits 8-9 = 1) there
 * are eight entries to a step, and the entry is number 7 - (y mod 8) of
 * them. */
uint32_t gd_grad_colour(int32_t x, int32_t y)
{
    const struct gdraw *g = &gdraw_ws;
    double ax = g->grad_ex - g->grad_sx, ay = g->grad_ey - g->grad_sy;
    double px = x - g->grad_sx, py = y - g->grad_sy, len2 = ax * ax + ay * ay, f;
    if (len2 <= 0) {
        f = 255;
    } else if (g->grad_type == 1) {
        /* The distance in whole pixels, times 256 divided by the radius as a
         * 16.16 fraction rounded down. This is exact for a radius of 2^n
         * pixels and a little under otherwise, as GDraw 3.12's is. */
        double d = floor(sqrt(px * px + py * py) + 1e-9);
        double step = floor(256.0 * 65536.0 / sqrt(len2));
        f = floor(d * step / 65536.0);
    } else {
        /* The distance along the axis in pixels, times 256 divided by the
         * axis's length as a 16.16 fraction rounded to nearest, then rounded
         * down. This gives GDraw 3.12's steps, every one (#137). */
        double len = sqrt(len2);
        double scale = floor(256.0 * 65536.0 / len + 0.5);
        f = floor((px * ax + py * ay) / len * scale / 65536.0);
    }
    uint32_t k = f < 0 ? 0 : f > 255 ? 255 : (uint32_t)f;
    uint32_t i = g->grad_dither ? 8 * k + (7 - ((uint32_t)y & 7)) : k;
    return ros_ld32(g->grad_table + 4 * i);
}

/* GDraw_Stroke. Thin strokes are Draw's and are never anti-aliased. A thick
 * one with no plot bits takes &30. */
static os_error *stroke(uint32_t r[8], uint32_t *r0)
{
    uint32_t r1 = r[1];
    if (gdraw_ws.grad == GD_GRAD_ARMED) {           /* this stroke gives the axis and is not drawn */
        if (grad_axis(r))
            gdraw_ws.grad = GD_GRAD_READY;
        return NULL;
    }
    if ((r1 & 0x7FFFFFFFu) & ~(uint32_t)F_STYLEMASK)
        return draw_err(DE_RESERVED);
    int aa = (r1 & GD_AA) != 0;
    if (aa && r[4] == 0)
        return no_plot();
    if (aa && !(r1 & 0x3C))
        r[1] = r1 | F_FULLINTERIOR | F_INTERIORBDRY;
    os_error *e = draw_reason_regs(4, r);
    if (e)
        return e;
    if (aa && (e = aa_check(r[1] & F_STYLEMASK)))
        return e;
    r[1] &= ~(GD_AA | GD_BIT6);
    return run(r, r0, aa, 0, 0);
}

/* StrokePath, FlattenPath and TransformPath are Draw's. */
static os_error *path(uint32_t reason, uint32_t r[8], uint32_t *r0)
{
    os_error *e = draw_reason_regs(reason, r);
    return e ? e : run(r, r0, 0, 0, 0);
}

/* ---- clip regions ---------------------------------------------------------------------------- */

/* Find where a region is to be built. It is GDraw's own buffer, claimed the
 * first time, or the caller's buffer, whose word 1 is the room in it. */
static os_error *region_out(uint32_t r7, uint32_t *out, uint32_t *room)
{
    struct gdraw *g = &gdraw_ws;
    if (r7 == 0) {
        if (!g->ownbuf) {
            void *p = ros_rma_alloc(GD_OWNBUF_SIZE);
            if (!p)
                return ros_error(0x101, "No room in RMA");
            g->ownbuf = ros_addr(p);
            ros_st32(g->ownbuf, 0), ros_st32(g->ownbuf + 4, GD_OWNBUF_SIZE);
        }
        *out = g->ownbuf, *room = GD_OWNBUF_SIZE;
        return NULL;
    }
    if (r7 & 3)
        return draw_err(DE_ADDRESS);
    *out = r7, *room = ros_ld32(r7 + 4);
    return NULL;
}

/* Build a region. This is a fill's ProcessPath with its coverage kept. The
 * OS's ChangedBox is left as it was, because nothing is plotted. */
static os_error *build_region(uint32_t r[8], uint32_t r7, int aa, uint32_t *r0)
{
    uint32_t out, room;
    os_error *e = region_out(r7, &out, &room);
    if (e)
        return e;
    uint32_t c[10] = { 0xFFFFFFFFu }, box[5] = { 0 };
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, c, sizeof c);
    ros_swi(&s, XOS_ChangedBox);
    uint32_t cb = s.v ? 0 : s.r[1];
    for (int k = 0; cb && k < 5; k++)
        box[k] = ros_ld32(cb + 4 * (uint32_t)k);
    r[7] = SPEC_FILL;
    uint32_t dummy;
    e = run(r, &dummy, aa, out, room);
    for (int k = 0; cb && k < 5; k++)
        ros_st32(cb + 4 * (uint32_t)k, box[k]);
    if (!e)
        *r0 = out;
    return e;
}

/* GDraw_ProcessClipPath. It takes ProcessPath's registers, with R7 as the
 * region's buffer. */
static os_error *process_clip_path(uint32_t r[8], uint32_t *r0)
{
    uint32_t flags = r[1];
    if (flags & 0x03FFFF00u)
        return draw_err(DE_RESERVED);
    if (flags & F_FLOAT)
        return facility();
    int aa = (flags & GD_AA) != 0;
    os_error *e = aa ? aa_check(flags & F_STYLEMASK) : NULL;
    if (e)
        return e;
    uint32_t r7 = r[7];
    r[1] = flags & ~(GD_AA | GD_BIT6 | F_R7IS32BIT);
    return build_region(r, r7, aa, r0);
}

/* GDraw_ClipPath turns a fill's path, flattened and closed, into a region. */
static os_error *clip_path(const uint32_t in[8], uint32_t *r0)
{
    uint32_t style = in[1] ? in[1] : F_FULLINTERIOR | F_INTERIORBDRY;
    if (style & ~0x3Fu)
        return draw_err(DE_RESERVED);
    uint32_t r[8] = { in[0], style | F_CLOSEOPEN | F_FLATTEN, in[2], in[3], 0, 0, 0, 0 };
    return build_region(r, in[4], 0, r0);
}

static os_error *set_clip_region(uint32_t reg)
{
    if (reg && reg != 0xFFFFFFFFu && !gd_region_valid(reg))
        return draw_err(DE_ADDRESS);
    gdraw_ws.clip = reg;
    return NULL;
}

static os_error *fill_region(const uint32_t r[8])
{
    if (r[0] == 0xFFFFFFFFu)
        return NULL;
    if (r[1] & ~(uint32_t)GD_BIT6)
        return draw_err(DE_RESERVED);
    if (!gd_region_valid(r[0]))
        return draw_err(DE_ADDRESS);
    return gd_fill_region(r[0], (int32_t)r[2], (int32_t)r[3]);
}

/* ---- the fill style and the print flag: recorded ------------------------------------------ */

static os_error *set_fill_style(const uint32_t r[8])
{
    uint32_t type = r[0] & 0x7F;
    if ((r[0] & ~(0x7Fu | 0x300u | 0x10000u)) || type > 3 || ((r[0] >> 8) & 3) == 3)
        return draw_err(DE_RESERVED);
    if (type && (r[1] & 3))
        return draw_err(DE_ADDRESS);
    uint32_t b = gdraw_ws.fillstyle;
    ros_st32(b, r[0]);
    /* Types 1 (radial) and 2 (linear) with bit 16 make a graduated fill.
     * The next Stroke gives its axis, and the next plot after that uses it.
     * This was measured against GDraw 3.12 (#137). */
    struct gdraw *g = &gdraw_ws;
    g->grad = 0;
    if ((type == 1 || type == 2) && (r[0] & 0x10000u) && r[1]) {
        g->grad = GD_GRAD_ARMED;
        g->grad_type = type;
        g->grad_table = r[1];
        g->grad_dither = ((r[0] >> 8) & 3) == 1;
    }
    if (type)
        for (uint32_t k = 1; k < 6; k++)
            ros_st32(b + 4 * k, r[k]);
    return NULL;
}

/* ---- the reasons ------------------------------------------------------------------------------ */

/* GDraw's reason n (0-24, 61). R0-R7 come in and R0 goes out. */
static void reason(struct ros_cpu *s, uint32_t n)
{
    struct gdraw *g = &gdraw_ws;
    uint32_t r[8], r0 = s->r[0];
    memcpy(r, s->r, sizeof r);
    os_error *e = NULL;
    switch (n) {
    case 0: e = process_path(r, &r0); break;
    case 2: {
        int ready = g->grad == GD_GRAD_READY;
        e = fill(r, &r0);
        if (ready)
            g->grad = 0;                    /* used up, so the next plot is flat */
        break;
    }
    case 4: {
        int ready = g->grad == GD_GRAD_READY;
        e = stroke(r, &r0);
        if (ready)
            g->grad = 0;                    /* used up, so the next plot is flat */
        break;
    }
    case 6: case 8: case 10: e = path(n, r, &r0); break;
    case 12: e = process_clip_path(r, &r0); break;
    case 14: e = clip_path(r, &r0); break;
    case 18: g->clip = 0; break;
    case 19: e = set_clip_region(r[0]); break;
    case 20: e = set_fill_style(r); break;
    case 21: e = fill_region(r); break;
    case 22: g->printflag = r[0]; break;
    case 23: r0 = g->fillstyle; break;
    case 24: r0 = g->clip; break;
    case 61: r0 = (uint32_t)g->claimed; break;
    default: e = facility(); break;       /* the ...FP forms, and ClipPathToPath */
    }
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0, s->r[0] = r0;
}

/* GDraw's DrawV handler. Draw's reasons are GDraw's SWIs. */
static int gdraw_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    s->v = 0;
    if (s->r[8] > LAST_REASON)
        ros_swi_fail(s, draw_err(DE_REASON));
    else
        reason(s, s->r[8]);
    return ROS_VECTOR_CLAIM;
}

static void claim(struct ros_cpu *s, int on)
{
    struct gdraw *g = &gdraw_ws;
    s->v = 0;
    if (on && !g->claimed) {
        os_error *e = ros_vector_claim_native(DRAWV, gdraw_v, 0);
        if (e) {
            ros_swi_fail(s, e);
            return;
        }
        g->claimed = 1;
    } else if (!on && g->claimed) {
        ros_vector_release_native(DRAWV, gdraw_v, 0);
        g->claimed = 0;
    }
}

/* Every SWI keeps R1-R11. SWIs 0-13 go through DrawV while GDraw has it. */
static void via(struct ros_cpu *s, uint32_t n)
{
    uint32_t keep[11];
    memcpy(keep, s->r + 1, sizeof keep);
    s->v = 0;
    if (n == 62 || n == 63) {
        claim(s, n == 62);
    } else if (gdraw_ws.claimed && n <= 13) {
        s->r[8] = n, s->r[9] = DRAWV;
        ros_vector_call(DRAWV, s);
    } else {
        reason(s, n);
    }
    memcpy(s->r + 1, keep, sizeof keep);
}

#define T(n, name) \
    void ros_thunk_GDraw_##name(struct ros_cpu *s) { via(s, n); }
T(0, ProcessPath) T(1, ProcessPathFP) T(2, Fill) T(3, FillFP) T(4, Stroke) T(5, StrokeFP)
T(6, StrokePath) T(7, StrokePathFP) T(8, FlattenPath) T(9, FlattenPathFP) T(10, TransformPath)
T(11, TransformPathFP) T(12, ProcessClipPath) T(13, ProcessClipPathFP) T(14, ClipPath)
T(15, ClipPathFP) T(16, ClipPathToPath) T(17, ClipPathToPathFP) T(18, ClearClipRegion)
T(19, SetClipRegion) T(20, SetFillStyle) T(21, FillRegion) T(22, SetPrintFlag)
T(23, ReadFillStyle) T(24, GetClipRegion)
#undef T

static void reason61(struct ros_cpu *s) { via(s, 61); }
static void reason62(struct ros_cpu *s) { via(s, 62); }
static void reason63(struct ros_cpu *s) { via(s, 63); }

/* The chunk holds the 25 named SWIs and then the three unnamed reasons.
 * Names past the 25th are empty, which ends the header's name table
 * there. */
#define CHUNK 64
static ros_swi_thunk *thunks[CHUNK];
static const char *names[CHUNK];

/* ---- the module ---------------------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    struct gdraw *g = &gdraw_ws;
    void *fs = ros_rma_alloc(64), *work = ros_rma_alloc(2048);
    if (!fs || !work) {
        if (fs) ros_rma_free(fs);
        if (work) ros_rma_free(work);
        return ros_error(0x101, "No room in RMA");
    }
    memset(fs, 0, 64);
    g->fillstyle = ros_addr(fs), g->work = ros_addr(work);
    g->claimed = 0, g->clip = 0, g->printflag = 0, g->ownbuf = 0;
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    struct gdraw *g = &gdraw_ws;
    if (g->claimed)
        ros_vector_release_native(DRAWV, gdraw_v, 0);
    g->claimed = 0;
    draw_freeworkspace(&g->d);
    if (g->ownbuf)
        ros_rma_free(ros_ptr(g->ownbuf));
    ros_rma_free(ros_ptr(g->fillstyle));
    ros_rma_free(ros_ptr(g->work));
    g->ownbuf = g->fillstyle = g->work = 0;
    g->clip = 0;
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6, "SWI value out of range for module GDraw");
}

struct ros_module gdraw_module = {
    .title = "GDraw",
    .help = "GDraw\t3.12 (27 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x44540,
    .swi_thunks = thunks,
    .swi_names = names,
    .swi_prefix = "GDraw",
    .swi_count = CHUNK,
};

__attribute__((constructor)) static void table(void)
{
    for (unsigned k = 0; k < CHUNK; k++) {
        thunks[k] = k < ros_swi_count_GDraw ? ros_swi_thunks_GDraw[k] : NULL;
        names[k] = k < ros_swi_count_GDraw ? ros_swi_names_GDraw[k] : "";
    }
    thunks[61] = reason61, thunks[62] = reason62, thunks[63] = reason63;
}
