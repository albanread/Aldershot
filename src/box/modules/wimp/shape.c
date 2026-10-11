/* shape.c -- rounded window corners (RISCOSGrandDesign design 35, P3).
 *
 * A theme with Chrome.Radius gives its windows round corners.  The Wimp
 * has no alpha: a window owns a rectangle of the screen and draws all of
 * it.  So a round-cornered window is a shaped window, as X's Shape
 * extension makes one: its opaque area is its outline less a notch at
 * each corner, stepped a pixel row at a time along the arc.  The notches
 * belong to whatever is below -- wimp_visible gives them to the windows
 * underneath, which draw them as they draw the rest of themselves -- and
 * the window itself neither draws them nor takes clicks there.
 *
 * Which windows: top-level ones with a title bar, when the theme paints
 * its chrome; not panes, back windows or children.  Chrome.RoundBottom
 * says whether the bottom corners are round too.
 */
#include <stdint.h>
#include <string.h>

#include "rosgd/api.h"
#include "wimp.h"

#define FLAG_PANE        (1u << 5)
#define FLAG_BACK_WINDOW (1u << 11)
#define F_TITLE          (1u << 26)

#define MAX_RADIUS_PX 32

/* Whether a window is of the kind that has round corners and a shadow
 * (shadow.c), if the theme gives them */
int wimp_corner_kind(const struct wimp_window *win, int st)
{
    const struct wimp_ws *w = wimp_ws();
    const struct wimp_place *p = &win->s[st];
    if (p->parent != NO_WINDOW || (p->flags & (FLAG_PANE | FLAG_BACK_WINDOW)) || !(p->flags & F_TITLE))
        return 0;
    return win->handle != w->iconbar;
}

/* The corner radius of a window, OS units; 0 for a square one */
int32_t wimp_corner_radius(const struct wimp_window *win, int st)
{
    const struct wimp_ws *w = wimp_ws();
    const struct wimp_style *sty = &w->style;
    if (!sty->painted || sty->radius <= 0 || !wimp_corner_kind(win, st))
        return 0;
    const struct wimp_place *p = &win->s[st];
    int32_t r = sty->radius;
    int32_t ow = p->outline.x1 - p->outline.x0, oh = p->outline.y1 - p->outline.y0;
    if (r * 2 > ow)
        r = ow / 2;
    if (r * 2 > oh)
        r = oh / 2;
    if (r > MAX_RADIUS_PX * w->dy)
        r = MAX_RADIUS_PX * w->dy;
    return r < 2 * w->dy ? 0 : r;
}

/* The insets of the rows of a corner, outermost row first, in pixels: how
 * far in from the side the window starts on that row.  A pixel is in
 * the window if its centre is within the arc. */
static int corner_insets(int32_t r_os, int32_t *inset)
{
    const struct wimp_ws *w = wimp_ws();
    int rp = (int)(r_os / w->dy);               /* rows */
    int rx = (int)(r_os / w->dx);               /* columns */
    if (rp > MAX_RADIUS_PX)
        rp = MAX_RADIUS_PX;
    for (int k = 0; k < rp; k++) {
        /* the row's centre, from the arc's centre, in half pixels (y scaled to x) */
        int64_t dy2 = (int64_t)(2 * rp - 2 * k - 1) * rx / (rp ? rp : 1);
        int i = 0;
        while (i < rx) {
            int64_t dx2 = 2 * rx - 2 * i - 1;
            if (dx2 * dx2 + dy2 * dy2 <= 4LL * rx * rx)
                break;
            i++;
        }
        inset[k] = i;
    }
    return rp;
}

static int round_bottom(void)
{
    return wimp_ws()->style.square_bottom == 0;
}

/* The window's opaque shape as boxes, *n of them (at most max): its
 * outline less the notches.  A square window is its outline. */
unsigned wimp_shape(const struct wimp_window *win, int st, struct wimp_box *b, unsigned max)
{
    const struct wimp_ws *w = wimp_ws();
    struct wimp_box o = win->s[st].outline;
    int32_t r = wimp_corner_radius(win, st);
    if (!r || max < 3) {
        if (max)
            b[0] = o;
        return max ? 1 : 0;
    }
    int32_t inset[MAX_RADIUS_PX];
    int rp = corner_insets(r, inset);
    int bottom = round_bottom();
    unsigned n = 0;
    /* the top rows, then the middle, then the bottom rows; rows of the same
     * inset together */
    for (int k = 0; k < rp && n < max;) {
        int e = k;
        while (e + 1 < rp && inset[e + 1] == inset[k])
            e++;
        int32_t in = inset[k] * w->dx;
        b[n++] = (struct wimp_box){ o.x0 + in, o.y1 - (e + 1) * w->dy, o.x1 - in, o.y1 - k * w->dy };
        k = e + 1;
    }
    int32_t mid0 = bottom ? o.y0 + rp * w->dy : o.y0;
    if (n < max && o.y1 - rp * w->dy > mid0)
        b[n++] = (struct wimp_box){ o.x0, mid0, o.x1, o.y1 - rp * w->dy };
    if (bottom)
        for (int k = 0; k < rp && n < max;) {
            int e = k;
            while (e + 1 < rp && inset[e + 1] == inset[k])
                e++;
            int32_t in = inset[k] * w->dx;
            b[n++] = (struct wimp_box){ o.x0 + in, o.y0 + k * w->dy, o.x1 - in, o.y0 + (e + 1) * w->dy };
            k = e + 1;
        }
    return n;
}

/* The notches: the parts of the outline the window does not own */
unsigned wimp_notches(const struct wimp_window *win, int st, struct wimp_box *b, unsigned max)
{
    const struct wimp_ws *w = wimp_ws();
    struct wimp_box o = win->s[st].outline;
    int32_t r = wimp_corner_radius(win, st);
    if (!r)
        return 0;
    int32_t inset[MAX_RADIUS_PX];
    int rp = corner_insets(r, inset);
    int bottom = round_bottom();
    unsigned n = 0;
    for (int k = 0; k < rp; k++) {
        if (!inset[k])
            continue;
        int32_t in = inset[k] * w->dx;
        int32_t ty1 = o.y1 - k * w->dy, by0 = o.y0 + k * w->dy;
        if (n + 4 > max)
            break;
        b[n++] = (struct wimp_box){ o.x0, ty1 - w->dy, o.x0 + in, ty1 };
        b[n++] = (struct wimp_box){ o.x1 - in, ty1 - w->dy, o.x1, ty1 };
        if (bottom) {
            b[n++] = (struct wimp_box){ o.x0, by0, o.x0 + in, by0 + w->dy };
            b[n++] = (struct wimp_box){ o.x1 - in, by0, o.x1, by0 + w->dy };
        }
    }
    return n;
}

/* Whether the screen point x, y is in the window's shape (its outline
 * already known to hold it) */
int wimp_in_shape(const struct wimp_window *win, int st, int32_t x, int32_t y)
{
    struct wimp_box nb[4 * MAX_RADIUS_PX];
    unsigned n = wimp_notches(win, st, nb, sizeof nb / sizeof nb[0]);
    for (unsigned i = 0; i < n; i++)
        if (x >= nb[i].x0 && x < nb[i].x1 && y >= nb[i].y0 && y < nb[i].y1)
            return 0;
    return 1;
}

/* The arc's edge, a pixel wide, for the border painter: for each row of
 * a corner, the run from its inset to the inset of the row nearer the
 * edge (at least a pixel), at the sides and, if round, the bottom.
 * *n boxes, at most max. */
unsigned wimp_corner_edges(const struct wimp_window *win, int st, struct wimp_box *b, unsigned max)
{
    const struct wimp_ws *w = wimp_ws();
    struct wimp_box o = win->s[st].outline;
    int32_t r = wimp_corner_radius(win, st);
    if (!r)
        return 0;
    int32_t inset[MAX_RADIUS_PX];
    int rp = corner_insets(r, inset);
    int bottom = round_bottom();
    unsigned n = 0;
    for (int k = 0; k < rp; k++) {
        int32_t a = inset[k], z = k ? inset[k - 1] : inset[k] + 1;
        if (z <= a)
            z = a + 1;
        if (k == 0)
            z = rp;                             /* the top row: to where the edge is straight */
        int32_t x0 = a * w->dx, x1 = z * w->dx;
        int32_t ty1 = o.y1 - k * w->dy, by0 = o.y0 + k * w->dy;
        if (n + 4 > max)
            break;
        b[n++] = (struct wimp_box){ o.x0 + x0, ty1 - w->dy, o.x0 + x1, ty1 };
        b[n++] = (struct wimp_box){ o.x1 - x1, ty1 - w->dy, o.x1 - x0, ty1 };
        if (bottom) {
            b[n++] = (struct wimp_box){ o.x0 + x0, by0, o.x0 + x1, by0 + w->dy };
            b[n++] = (struct wimp_box){ o.x1 - x1, by0, o.x1 - x0, by0 + w->dy };
        }
    }
    return n;
}

/* l less the window's shape (it is above what l describes) */
void wimp_rl_subtract_shape(struct wimp_rlist *l, const struct wimp_window *win, int st, struct wimp_box clip)
{
    struct wimp_box sb[2 * MAX_RADIUS_PX + 1];
    unsigned n = wimp_shape(win, st, sb, sizeof sb / sizeof sb[0]);
    for (unsigned i = 0; i < n; i++) {
        struct wimp_box c = { sb[i].x0 > clip.x0 ? sb[i].x0 : clip.x0, sb[i].y0 > clip.y0 ? sb[i].y0 : clip.y0,
                              sb[i].x1 < clip.x1 ? sb[i].x1 : clip.x1, sb[i].y1 < clip.y1 ? sb[i].y1 : clip.y1 };
        if (c.x0 < c.x1 && c.y0 < c.y1)
            wimp_rl_subtract(l, c);
    }
}

/* l less the window's own notches */
void wimp_rl_subtract_notches(struct wimp_rlist *l, const struct wimp_window *win, int st)
{
    struct wimp_box nb[4 * MAX_RADIUS_PX];
    unsigned n = wimp_notches(win, st, nb, sizeof nb / sizeof nb[0]);
    for (unsigned i = 0; i < n; i++)
        wimp_rl_subtract(l, nb[i]);
}
