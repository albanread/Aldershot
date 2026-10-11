/* shadow.c -- windows' drop shadows (RISCOSGrandDesign design 35, P3).
 *
 * A theme with Chrome.Shadow gives the windows that have round corners'
 * kind -- top-level, with a title bar, and menus -- a soft shadow, a
 * little below them, falling on whatever is underneath.  The Wimp has no
 * compositor: a window below draws its own pixels, shadow or no.  So the
 * shadow is laid over them afterwards, by darkening what was drawn:
 *
 *   - after each rectangle of a redraw (next_rectangle), and after the
 *     border pass (start_redraw), by the shadows of the windows above;
 *   - after the Wimp clears uncovered desktop, by every shadow.
 *
 * Round corners' notches are treated the same way: after what is below
 * is drawn there, the pixels the arc partly covers are blended with the
 * window's edge colour by how much of each it covers (smooth_corners), so
 * the corners are smooth.  So the notches count as overlay too, shadow or
 * no shadow.
 *
 * Darkening twice would darken twice, so nothing may draw into a shadow
 * except through a redraw, which repaints it all first:
 *
 *   - Wimp_UpdateWindow and an icon changed in place leave the shadowed
 *     parts out, and make them invalid instead;
 *   - Wimp_BlockCopy and the copies made when windows move or scroll
 *     leave out the shadowed parts, which are redrawn;
 *   - a window that moves, opens, closes or changes its place in the
 *     stack makes its old and new shadows invalid.
 *
 * In 32 bits per pixel only (the desktop's depth); in other modes there
 * are no shadows.  The darkening is done in the screen's memory.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

/* The shadow's extent around the window, OS units, or 0 for none */
static int32_t shadow_size(const struct wimp_window *win, int st)
{
    const struct wimp_style *sty = &wimp_ws()->style;
    if (!sty->painted || sty->shadow_size <= 0 || !win->s[st].open)
        return 0;
    if (!wimp_corner_kind(win, st))
        return 0;
    return sty->shadow_size;
}

/* The box the shadow falls in: the outline, lowered by a third of the
 * size, then widened by the size */
static int shadow_box(const struct wimp_window *win, int st, struct wimp_box *box, struct wimp_box *core)
{
    int32_t s = shadow_size(win, st);
    if (!s)
        return 0;
    struct wimp_box o = win->s[st].outline;
    int32_t drop = s / 3;
    *core = (struct wimp_box){ o.x0, o.y0 - drop, o.x1, o.y1 - drop };
    *box = (struct wimp_box){ core->x0 - s, core->y0 - s, core->x1 + s, core->y1 + s };
    return 1;
}

/* Whether the theme lays anything over what is below windows: shadows,
 * or round corners' smoothed edges */
static int overlays(void)
{
    const struct wimp_style *sty = &wimp_ws()->style;
    return sty->painted && (sty->shadow_size > 0 || sty->radius > 0);
}

/* Whether a window lays anything over what is below it */
static int overlay_of(const struct wimp_window *win, int st)
{
    return win->s[st].open && (shadow_size(win, st) || wimp_corner_radius(win, st));
}

/* The top-level window a window is in */
static const struct wimp_window *top_of(const struct wimp_window *win, int st)
{
    while (win && win->s[st].parent != NO_WINDOW)
        win = wimp_window(win->s[st].parent);
    return win;
}

/* The screen a window's overlay falls on -- its shadow's box and its
 * corners' notches -- less the window's shape and less every window above
 * it */
static void shadow_region(const struct wimp_window *win, int st, struct wimp_rlist *out)
{
    out->n = 0;
    out->overflow = 0;
    struct wimp_box box, core;
    if (shadow_box(win, st, &box, &core)) {
        wimp_rl_append(out, box);
    } else {
        struct wimp_box nb[128];
        unsigned n = wimp_notches(win, st, nb, sizeof nb / sizeof nb[0]);
        for (unsigned i = 0; i < n; i++)
            wimp_rl_append(out, nb[i]);
    }
    if (!out->n)
        return;
    wimp_rl_clip(out, wimp_screen_box());
    wimp_rl_subtract_shape(out, win, st, win->s[st].outline);
    for (struct wimp_window *x = wimp_stack_front(NO_WINDOW, st); x && x != win; x = wimp_window_below(x, st))
        if (x->s[st].open)
            wimp_rl_subtract_shape(out, x, st, x->s[st].outline);
}

/* out: the shadows falling on the window (that is, of the windows above
 * its top-level window), in state st */
void wimp_shadows_above(const struct wimp_window *win, int st, struct wimp_rlist *out)
{
    out->n = 0;
    out->overflow = 0;
    if (!overlays())
        return;
    const struct wimp_window *top = top_of(win, st);
    struct wimp_rlist *one = wimp_rl_new();
    if (!one)
        return;
    for (struct wimp_window *x = wimp_stack_front(NO_WINDOW, st); x && x != top; x = wimp_window_below(x, st)) {
        if (!overlay_of(x, st))
            continue;
        shadow_region(x, st, one);
        wimp_rl_union(out, one);
    }
    wimp_rl_free(one);
}

/* A window's own shadow, in state st, added to l (for wimp_flush, when the
 * window has moved, opened, closed or changed places) */
void wimp_shadow_add(const struct wimp_window *win, int st, struct wimp_rlist *l)
{
    if (!overlay_of(win, st))
        return;
    struct wimp_rlist *one = wimp_rl_new();
    if (!one)
        return;
    shadow_region(win, st, one);
    wimp_rl_union(l, one);
    wimp_rl_free(one);
}

/* l less the shadows falling on the window; what is taken out is made
 * invalid, to be redrawn whole and darkened once */
void wimp_shadows_out(const struct wimp_window *win, struct wimp_rlist *l)
{
    if (!l->n || !overlays())
        return;
    struct wimp_rlist *sh = wimp_rl_new(), *hit = wimp_rl_new();
    if (sh && hit) {
        wimp_shadows_above(win, APP, sh);
        if (sh->n) {
            wimp_rl_copy(hit, l);
            wimp_rl_and(hit, sh);
            if (hit->n) {
                wimp_rl_minus(l, sh);
                wimp_invalidate(hit);
            }
        }
    }
    wimp_rl_free(hit);
    wimp_rl_free(sh);
}

/* ---- the darkening --------------------------------------------------------------------- */

struct screen {
    uint8_t *base;
    uint32_t line;
    int32_t xeig, yeig, h_px, w_px;
};

static int screen_now(struct screen *sc)
{
    static const uint32_t vars[] = { 148, 6, 9, 4, 5, 11, 12, 0xFFFFFFFFu };
    uint32_t *in = (uint32_t *)(wimp_ws()->scratch + 320), *out = in + 8;
    memcpy(in, vars, sizeof vars);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(in), c.r[1] = ros_addr(out);
    ros_swi(&c, XOS_ReadVduVariables);
    if (c.v || out[2] != 5 || !out[0])
        return 0;                               /* not 32 bits per pixel: no shadows */
    sc->base = ros_ptr(out[0]);
    sc->line = out[1];
    sc->xeig = (int32_t)out[3], sc->yeig = (int32_t)out[4];
    sc->w_px = (int32_t)out[5] + 1, sc->h_px = (int32_t)out[6] + 1;
    return 1;
}

/* Darken r (screen OS units) by the shadow of window win */
static void darken(const struct screen *sc, const struct wimp_window *win, struct wimp_box r)
{
    struct wimp_box box, core;
    if (!shadow_box(win, APP, &box, &core))
        return;
    struct wimp_box a = { r.x0 > box.x0 ? r.x0 : box.x0, r.y0 > box.y0 ? r.y0 : box.y0,
                          r.x1 < box.x1 ? r.x1 : box.x1, r.y1 < box.y1 ? r.y1 : box.y1 };
    if (a.x0 >= a.x1 || a.y0 >= a.y1)
        return;
    const struct wimp_style *sty = &wimp_ws()->style;
    float size = (float)shadow_size(win, APP);
    float rad = (float)wimp_corner_radius(win, APP);
    /* the core shrunk by the corner radius: the distance to a round-cornered
     * box is the distance to that, less the radius */
    float cx0 = (float)core.x0 + rad, cx1 = (float)core.x1 - rad;
    float cy0 = (float)core.y0 + rad, cy1 = (float)core.y1 - rad;
    float strength = (float)sty->shadow_alpha / 255.0f;
    int32_t px0 = a.x0 >> sc->xeig, px1 = (a.x1 + (1 << sc->xeig) - 1) >> sc->xeig;
    int32_t py0 = a.y0 >> sc->yeig, py1 = (a.y1 + (1 << sc->yeig) - 1) >> sc->yeig;
    if (px0 < 0) px0 = 0;
    if (px1 > sc->w_px) px1 = sc->w_px;
    if (py0 < 0) py0 = 0;
    if (py1 > sc->h_px) py1 = sc->h_px;
    for (int32_t py = py0; py < py1; py++) {
        float y = (float)((py << sc->yeig) + (1 << sc->yeig) / 2);
        float dy = y < cy0 ? cy0 - y : y > cy1 ? y - cy1 : 0.0f;
        uint32_t *row = (uint32_t *)(sc->base + (size_t)(sc->h_px - 1 - py) * sc->line);
        for (int32_t px = px0; px < px1; px++) {
            float x = (float)((px << sc->xeig) + (1 << sc->xeig) / 2);
            float dx = x < cx0 ? cx0 - x : x > cx1 ? x - cx1 : 0.0f;
            float d = sqrtf(dx * dx + dy * dy) - rad;
            if (d >= size)
                continue;
            float t = d <= 0.0f ? 1.0f : 1.0f - d / size;
            unsigned k = 256u - (unsigned)(strength * t * t * 256.0f);
            uint32_t p = row[px];
            uint32_t rb = ((p & 0x00FF00FFu) * k >> 8) & 0x00FF00FFu;
            uint32_t g = ((p & 0x0000FF00u) * k >> 8) & 0x0000FF00u;
            row[px] = (p & 0xFF000000u) | rb | g;
        }
    }
}

/* Blend the notch pixels of win in r with its edge colour, by how much of
 * each pixel the arc covers */
static void smooth_corners(const struct screen *sc, const struct wimp_window *win, struct wimp_box r)
{
    int32_t rad = wimp_corner_radius(win, APP);
    if (!rad)
        return;
    struct wimp_box nb[128];
    unsigned n = wimp_notches(win, APP, nb, sizeof nb / sizeof nb[0]);
    if (!n)
        return;
    struct wimp_box o = win->s[APP].outline;
    uint32_t edge = wimp_corner_edge_colour(win);            /* &BBGGRR00 */
    uint32_t er = (edge >> 8) & 255, eg = (edge >> 16) & 255, eb = (edge >> 24) & 255;
    float rr = (float)rad;
    int bottom = !wimp_ws()->style.square_bottom;
    for (unsigned i = 0; i < n; i++) {
        struct wimp_box a = { nb[i].x0 > r.x0 ? nb[i].x0 : r.x0, nb[i].y0 > r.y0 ? nb[i].y0 : r.y0,
                              nb[i].x1 < r.x1 ? nb[i].x1 : r.x1, nb[i].y1 < r.y1 ? nb[i].y1 : r.y1 };
        if (a.x0 >= a.x1 || a.y0 >= a.y1)
            continue;
        for (int32_t py = a.y0 >> sc->yeig; py < (a.y1 >> sc->yeig); py++) {
            float y = (float)((py << sc->yeig) + (1 << sc->yeig) / 2);
            float cy = y > (o.y0 + o.y1) / 2 ? (float)o.y1 - rr : (float)o.y0 + rr;
            if (!bottom && y < (o.y0 + o.y1) / 2)
                continue;
            uint32_t *row = (uint32_t *)(sc->base + (size_t)(sc->h_px - 1 - py) * sc->line);
            for (int32_t px = a.x0 >> sc->xeig; px < (a.x1 >> sc->xeig); px++) {
                float x = (float)((px << sc->xeig) + (1 << sc->xeig) / 2);
                float cx = x > (o.x0 + o.x1) / 2 ? (float)o.x1 - rr : (float)o.x0 + rr;
                float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
                float cover = (rr - d) / (float)(1 << sc->xeig) + 0.5f;   /* how much of the pixel is inside */
                if (cover <= 0.0f)
                    continue;
                if (cover > 1.0f)
                    cover = 1.0f;
                unsigned k = (unsigned)(cover * 256.0f);
                uint32_t p = row[px];
                uint32_t pr = p & 255, pg = (p >> 8) & 255, pb = (p >> 16) & 255;
                pr = (pr * (256 - k) + er * k) >> 8;
                pg = (pg * (256 - k) + eg * k) >> 8;
                pb = (pb * (256 - k) + eb * k) >> 8;
                row[px] = (p & 0xFF000000u) | pr | pg << 8 | pb << 16;
            }
        }
    }
}

/* Lay the shadows of the windows above win (all windows, for NULL: the
 * desktop) over r, which has just been drawn whole */
void wimp_shadows_over(const struct wimp_window *win, struct wimp_box r)
{
    if (!overlays() || r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    /* the windows above, back to front: each one's shadow, then its smoothed
     * corners, over what is below it */
    const struct wimp_window *top = win ? top_of(win, APP) : NULL;
    const struct wimp_window *above[512];
    unsigned n = 0;
    for (struct wimp_window *x = wimp_stack_front(NO_WINDOW, APP); x && x != top && n < 512;
         x = wimp_window_below(x, APP))
        if (overlay_of(x, APP))
            above[n++] = x;
    struct screen sc;
    if (!n || !screen_now(&sc))
        return;
    while (n--) {
        darken(&sc, above[n], r);
        smooth_corners(&sc, above[n], r);
    }
}
