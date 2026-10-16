/* surface.c -- surface windows (#131).
 *
 * A window can be bound to a surface: a sprite, or the shown bank of a
 * virtual display (runtime/vdu/vdisplay.c).  The Wimp then draws the
 * window's work area itself, from the sprite, with a colour table it
 * makes.  The sprite is scaled by a whole number or fitted to the visible
 * area.  The Wimp draws it in the redraw scan, as it draws its own and
 * auto-redraw windows, so the owner never gets a Redraw_Window_Request
 * for it.
 *
 * A change is shown at once, and only where the window is visible.  The
 * Wimp learns of it in one of two ways:
 *
 *   - Wimp_Extend &5202 (SurfaceChanged), from the owner, with a rectangle
 *     of the sprite;
 *   - a virtual display tells the Wimp itself.  VDisplay notes each change
 *     (ros_vdisplay_changed_hook, which only sets surf_dirty).  The Wimp
 *     takes the display's changed box at two safe points.  One is each
 *     Wimp_Poll's search, before the redraw scan.  The other is the VSync's
 *     background work, when nothing is in the middle of the Wimp or of a
 *     redraw (vsync_present).  A task window's child runs for 10 cs between
 *     polls, so it is the VSync that shows its frames as they come.
 *
 * A change is drawn straight onto the screen, rectangle by rectangle, over
 * the window's visible work area less what is already invalid.  That is
 * what an update would draw, but without the owner.  Where that cannot be
 * done (icons or the caret in the window, or a mode change to follow), the
 * part is made invalid, for the redraw scan.
 *
 * The SWIs are Wimp_Extend reasons in a range of ROSGD's own, at the
 * native Wimp's extension point.  RISC OS 5.30's Wimp_Extend leaves R0
 * alone for a reason it does not know, so R0 = 0 on the way out says that
 * the native Wimp did it:
 *
 *   &5200 SurfaceBind     R1 window, R2 flags (bit 0: R3 is a VDisplay's
 *                         handle; bit 1: R3 is the compositor's id for an
 *                         external window), R3 sprite area or handle, R4
 *                         sprite (external: how far below the work area's
 *                         top the window starts, in OS units, to leave
 *                         room for a toolbar), R5 scale 1-16, or 0 fitted
 *   &5201 SurfaceUnbind   R1 window
 *   &5202 SurfaceChanged  R1 window, R2 flags (bit 0: all of it), R3-R6
 *                         x0, y0, x1, y1 in the sprite's pixels, inclusive,
 *                         y up
 *   &5203 SurfaceInfo     R1 window; out R1 flags or -1, R2 times shown,
 *                         R3 rectangles drawn, R4 scale
 *   &5204 SurfaceIcon     R1 window (-2 the icon bar), R2 icon, R3 kind
 *                         (screen.h's ROS_SURF_*; 0 unbinds), R4 the kind's
 *                         parameter, R5 bytes the kind draws from and R6
 *                         how many (up to 32; R5 0 for none), R7 the rate
 *                         it would be drawn again at, in Hz (0: when it
 *                         changes; a clock's smooth hand asks 30): the
 *                         presenter redraws its box alone at that rate, no
 *                         memory copied; out R1 1 if the GPU presenter draws
 *                         the icon's picture now, 0 if the caller must
 *
 * An icon surface is an icon whose picture the PC's GPU presenter draws
 * itself, over the desktop, so that screen memory under it stays still
 * (design 37, 3.10): the icon bar's clock, which would otherwise be
 * redrawn every second.  The Wimp publishes each one's box and the parts
 * of it that can be seen in the screen block's surface table, as it does
 * the external windows'.  The owner asks again whenever it would draw (a
 * call is cheap), and draws for itself whenever the answer is 0: no
 * presenter, or one that has stopped.
 *
 * An external window is a Linux program's Wayland window, which the
 * compositor shows (ports/compositor).  The Wimp draws its work area black
 * and tells the compositor, through the screen block (screen.h, struct
 * ros_screen_ext), where the window is and which parts of it are seen.
 * The compositor puts the Linux window there, under the desktop, and makes
 * the desktop clear over those parts.  The table is made again at each
 * Wimp_Poll and written only when it has changed.
 *
 * When a virtual display changes mode, the window is given the extent of
 * its new size (at whole-number scales), and its owner is sent
 * Message_SurfaceResized (&C01C2: +20 window, +24 width, +28 height in
 * pixels, +32 XEig, +36 YEig).
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/platform.h"
#include "rosgd/screen.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vdu.h"
#include "wimp.h"

#define E_NOT_DISPLAY 0xC01C0u          /* VDisplay's "Not a virtual display" */
#define IF_DELETED_ (1u << 23)          /* an icon's flags: deleted */

static uint32_t swi8(uint32_t n, uint32_t *r)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 8 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 8 * sizeof r[0]);
    return c.v;
}

static uint32_t mode_var(uint32_t mode, uint32_t var, uint32_t dflt)
{
    uint32_t r[8] = { mode, var };
    return swi8(XOS_ReadModeVariable, r) ? dflt : r[2];
}

/* ---- the sprite ------------------------------------------------------------------------------ */

/* Read the sprite's size and depth now.  A display's come from its view
 * (the bank that OS_Byte 113 shows), and a sprite's from its header.  0 if
 * there is none. */
static int source(struct wimp_surface *s)
{
    if (s->flags & WS_EXTERNAL)
        return 1;                       /* the compositor's */
    if (s->flags & WS_VDISPLAY) {
        if (ros_vdisplay_view(s->source, &s->view) != 0)
            return 0;
        s->width = s->view.width, s->height = s->view.height;
        s->xeig = s->view.xeig, s->yeig = s->view.yeig, s->log2bpp = s->view.log2bpp;
        return 1;
    }
    uint32_t sp = s->sprite, mode = ros_ld32(sp + 40);
    s->log2bpp = mode_var(mode, 9, 0);
    s->xeig = mode_var(mode, 4, 1);
    s->yeig = mode_var(mode, 5, 1);
    uint32_t words = ros_ld32(sp + 16) + 1, lbit = ros_ld32(sp + 24), rbit = ros_ld32(sp + 28);
    s->width = (words * 32 - lbit - (31 - rbit)) >> s->log2bpp;
    s->height = ros_ld32(sp + 20) + 1;
    return s->width && s->height;
}

/* Where it is drawn: its top left on the screen and its size, in OS units.
 * A whole-number scale puts it at the work area's origin, at its own size
 * in OS units times the scale.  A fitted one goes at the visible area's
 * top left, as big as fits with its shape kept, as GraphTask places it. */
static void place(const struct wimp_window *win, const struct wimp_surface *s, int32_t *x0, int32_t *top,
                  int32_t *dw, int32_t *dh)
{
    int32_t nw = (int32_t)(s->width << s->xeig), nh = (int32_t)(s->height << s->yeig);
    const struct wimp_box v = win->s[APP].vis;
    if (s->scale == 0) {
        int32_t vw = v.x1 - v.x0, vh = v.y1 - v.y0;
        if ((int64_t)vw * nh <= (int64_t)vh * nw)
            *dw = vw, *dh = (int32_t)((int64_t)vw * nh / nw);
        else
            *dh = vh, *dw = (int32_t)((int64_t)vh * nw / nh);
        *x0 = v.x0, *top = v.y1;
    } else {
        *dw = (int32_t)s->scale * nw, *dh = (int32_t)s->scale * nh;
        *x0 = wimp_origin_x(win, APP), *top = wimp_origin_y(win, APP);
    }
}

/* The colour table, for a sprite of 8 bpp or less.  It holds the desktop's
 * colour for each entry in the sprite's palette
 * (ColourTrans_ReturnColourNumber).  On 16 and 32 bpp the entries are
 * words, which makes a wide table (plot action bit 5).  Otherwise they are
 * bytes.  A display's bank sprite has its image some way after its
 * palette, so its palette is read from the view instead of by ColourTrans
 * from the sprite.  A plain sprite with a palette is plotted with it
 * (action bit 4).  One without a palette is plotted as SpriteExtend plots
 * it. */
static void make_table(struct wimp_surface *s)
{
    struct wimp_ws *w = wimp_ws();
    s->table_ok = 1;
    s->table_pal_gen = s->view.pal_gen;
    s->action = 0;
    if (s->log2bpp > 3)
        return;
    if (!(s->flags & WS_VDISPLAY)) {
        if (ros_ld32(s->sprite + 32) > 44)
            s->action = 1u << 4;
        return;
    }
    uint32_t n = 1u << (1u << s->log2bpp), pal = s->view.sprite + 44;
    if (n > s->view.pal_entries)
        n = s->view.pal_entries;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r[8] = { ros_ld32(pal + 8 * i) };
        uint32_t c = swi8(XColourTrans_ReturnColourNumber, r) ? 0 : r[0];
        if (w->log2bpp == 4)
            memcpy(s->table + 2 * i, &(uint16_t){ (uint16_t)c }, 2);
        else if (w->log2bpp > 4)
            memcpy(s->table + 4 * i, &c, 4);
        else
            s->table[i] = (uint8_t)c;
    }
    s->action = (w->log2bpp >= 4 ? 1u << 5 : 0) | 0x100u;     /* 0x100: a table follows */
}

/* Draw the surface into r, with the graphics window already set to r.
 * This draws the sprite, and the window's background where the sprite does
 * not reach. */
static void paint(struct wimp_window *win, struct wimp_surface *s, struct wimp_box r)
{
    struct wimp_ws *w = wimp_ws();
    if (s->flags & WS_EXTERNAL) {
        (void)r;
        wimp_gcol(wimp_colour(7), 1);   /* black: the compositor shows the window here */
        wimp_clg();
        s->rects++;
        return;
    }
    int32_t x0, top, dw, dh;
    place(win, s, &x0, &top, &dw, &dh);
    struct wimp_box sb = { x0, top - dh, x0 + dw, top };
    if (win->def[35] != 0xFF &&
        !(sb.x0 <= r.x0 && sb.y0 <= r.y0 && sb.x1 >= r.x1 && sb.y1 >= r.y1)) {
        wimp_gcol(wimp_colour(win->def[35]), 1);
        struct wimp_box strips[4] = {
            { r.x0, sb.y1, r.x1, r.y1 },        /* above */
            { r.x0, r.y0, r.x1, sb.y0 },        /* below */
            { r.x0, sb.y0, sb.x0, sb.y1 },      /* left */
            { sb.x1, sb.y0, r.x1, sb.y1 },      /* right */
        };
        for (int k = 0; k < 4; k++) {
            struct wimp_box b = strips[k];
            if (b.x0 < r.x0) b.x0 = r.x0;
            if (b.y0 < r.y0) b.y0 = r.y0;
            if (b.x1 > r.x1) b.x1 = r.x1;
            if (b.y1 > r.y1) b.y1 = r.y1;
            if (b.x0 < b.x1 && b.y0 < b.y1) {
                wimp_graphics_window(b);
                wimp_clg();
            }
        }
        wimp_graphics_window(r);
    }
    if (dw <= 0 || dh <= 0 || sb.x1 <= r.x0 || sb.x0 >= r.x1 || sb.y1 <= r.y0 || sb.y0 >= r.y1)
        return;
    if (!s->table_ok || ((s->flags & WS_VDISPLAY) && s->table_pal_gen != s->view.pal_gen))
        make_table(s);
    s->factors[0] = dw, s->factors[1] = dh;
    s->factors[2] = (int32_t)(s->width << w->xeig), s->factors[3] = (int32_t)(s->height << w->yeig);
    uint32_t area = (s->flags & WS_VDISPLAY) ? s->view.area : s->source;
    uint32_t sprite = (s->flags & WS_VDISPLAY) ? s->view.sprite : s->sprite;
    uint32_t r8[8] = { 512 + 52, area, sprite, (uint32_t)x0, (uint32_t)(top - dh), s->action & 0xFFu,
                       ros_addr(s->factors), (s->action & 0x100u) ? ros_addr(s->table) : 0 };
    swi8(XOS_SpriteOp, r8);
    s->rects++;
}

/* Draw the redraw scan's rectangle (redraw.c), the graphics window set */
void wimp_surface_paint(struct wimp_window *win, struct wimp_box r)
{
    struct wimp_surface *s = win->surface;
    if (!s || !source(s))
        return;
    paint(win, s, r);
    s->presents++;
}

/* ---- showing a change ------------------------------------------------------------------------ */

/* A box of the sprite's pixels (inclusive, y up), on the screen */
static struct wimp_box screen_box(const struct wimp_window *win, const struct wimp_surface *s, const int32_t b[4])
{
    struct wimp_ws *w = wimp_ws();
    int32_t x0, top, dw, dh;
    place(win, s, &x0, &top, &dw, &dh);
    int32_t W = (int32_t)s->width, H = (int32_t)s->height, bot = top - dh;
    struct wimp_box r;
    r.x0 = x0 + (int32_t)((int64_t)b[0] * dw / W) - w->dx;
    r.x1 = x0 + (int32_t)(((int64_t)(b[2] + 1) * dw + W - 1) / W) + w->dx;
    r.y0 = bot + (int32_t)((int64_t)b[1] * dh / H) - w->dy;
    r.y1 = bot + (int32_t)(((int64_t)(b[3] + 1) * dh + H - 1) / H) + w->dy;
    return r;
}

/* Whether a change can be drawn straight away.  The window must have no
 * icons and no visible caret, because either would be drawn over
 * (GraphTask's caret is invisible).  No redraw or update may be under
 * way. */
static int direct_ok(const struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    int caret = w->caret.w == win->handle && !(w->caret.hf & (1u << 25));
    return win->s[APP].open && win->nicons == 0 && !caret && w->pending_window == 0;
}

/* Show box (in screen OS units) of the window.  It is drawn now where it
 * is visible and not already invalid.  Returns 0 if it could not be drawn
 * and was not made invalid either.  The background caller then tries
 * again at the next safe point. */
static int show(struct wimp_window *win, struct wimp_box box, int may_invalidate)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_rlist *l = wimp_rl_new();
    if (!l)
        return 0;
    /* a window with a backing (Sprint 5): all of its work area, covered
     * or not, painted into the backing */
    int backed = wimp_backed(win);
    if (backed) {
        wimp_drawable(win, l);
        wimp_minus_pending(win, l);
    } else {
        wimp_inner(win, APP, l);
        wimp_rl_minus(l, &w->invalid);
        wimp_rl_clip(l, wimp_screen_box());     /* a window may be partly off the screen */
    }
    wimp_rl_clip(l, box);
    int done = 1;
    if (!l->n) {
        /* nothing of it visible */
    } else if (!direct_ok(win)) {
        if (may_invalidate)
            wimp_invalidate_in(win, l);
        else
            done = 0;
    } else {
        uint32_t saved[4];
        int into = backed && wimp_backing_begin(win->surface->backing, win->s[APP].outline.x0,
                                                win->s[APP].outline.y0, saved);
        wimp_drag_hide();
        for (uint32_t k = 0; k < l->n; k++) {
            wimp_graphics_window(l->b[k]);
            paint(win, win->surface, l->b[k]);
            if (into)
                wimp_backing_damage(win, l->b[k]);
            else
                wimp_damage_add(l->b[k]);       /* block v2: painted directly */
        }
        wimp_default_windows();
        wimp_drag_show();
        if (into)
            wimp_backing_end(saved);
        win->surface->presents++;
    }
    wimp_rl_free(l);
    return done;
}

/* Mark all of the window's work area to be drawn again by the redraw scan */
static void invalidate_all(struct wimp_window *win)
{
    if (wimp_backed(win)) {
        wimp_invalidate_all_of(win);            /* its own list: not the windows in front */
        return;
    }
    struct wimp_rlist *l = wimp_rl_new();
    if (!l)
        return;
    wimp_inner(win, APP, l);
    wimp_rl_clip(l, wimp_screen_box());
    wimp_invalidate(l);
    wimp_rl_free(l);
}

/* A display's new mode.  The window's extent is set for its size, its
 * owner is told, and all of it is marked to be drawn again. */
static void resized(struct wimp_window *win, struct wimp_surface *s)
{
    s->mode_gen = s->view.mode_gen;
    s->table_ok = 0;
    if (s->scale) {
        int32_t box[4] = { 0, -(int32_t)(s->scale * (s->height << s->yeig)),
                           (int32_t)(s->scale * (s->width << s->xeig)), 0 };
        wimp_set_extent(win, box);
    }
    uint8_t b[40];
    memset(b, 0, sizeof b);
    uint32_t words[10] = { 40, 0, 0, 0, MSG_SURFACE_RESIZED, win->handle, s->width, s->height, s->xeig, s->yeig };
    memcpy(b, words, sizeof words);
    wimp_queue_message(17, b, 40, RECV_TASK, win->owner, 0);
    invalidate_all(win);
}

/* Take and show the changes of each window bound to a display.
 * background is set at the VSync, where nothing may be made invalid and a
 * new mode waits for the next poll.  Returns 1 if all was done. */
static int present_all(int background)
{
    struct wimp_ws *w = wimp_ws();
    int all_done = 1;
    for (uint32_t i = 0; i < WIMP_WINDOWS && w->nsurfaces; i++) {
        struct wimp_window *win = w->win[i];
        struct wimp_surface *s = win ? win->surface : NULL;
        if (!s || !(s->flags & WS_VDISPLAY))
            continue;
        if (!source(s))
            continue;                           /* the display has gone */
        if (s->view.mode_gen != s->mode_gen) {
            if (background) {
                all_done = 0;
                continue;
            }
            int32_t box[4];
            ros_vdisplay_take(s->source, box);
            resized(win, s);
            continue;
        }
        if (background && !direct_ok(win)) {
            all_done = 0;                       /* left in the display for the poll */
            continue;
        }
        int32_t box[4];
        if (ros_vdisplay_take(s->source, box) <= 0)
            continue;
        if (!win->s[APP].open)
            continue;
        if (!show(win, screen_box(win, s, box), !background))
            all_done = 0;
    }
    return all_done;
}

/* ---- external windows: the compositor's table ----------------------------------------------- */

/* The part of an external window the compositor or the GPU presenter
 * shows a Linux program in, in state st: its visible work area less the
 * toolbar strip at the top (the bind's R4, which RISC OS draws).  What
 * screen memory holds there is never seen -- the presenter draws the
 * program's buffer over it, the compositor makes the desktop clear there
 * -- so a move need not copy or redraw it (redraw.c).  Empty for any
 * other window. */
void wimp_surface_external_area(const struct wimp_window *win, int st, struct wimp_rlist *out)
{
    out->n = 0;
    const struct wimp_surface *s = win->surface;
    if (!s || !(s->flags & (WS_EXTERNAL | WS_BACKED)))
        return;
    if (s->backing) {
        /* Sprint 3: the chrome is in the backing and the page is the
         * compositor's, so nothing of the window's outline is the
         * framebuffer's to copy or redraw */
        wimp_visible(win, win->s[st].outline, st, out);
        return;
    }
    if (!(s->flags & WS_EXTERNAL) || !ros_display_ext())
        return;
    wimp_inner(win, st, out);
    struct wimp_box b = win->s[st].vis;
    b.y1 -= (int32_t)s->sprite;                 /* below the toolbar */
    wimp_rl_clip(out, b);
}

static struct ros_screen_ext ext_now, ext_was;
static int ext_any;                     /* the table written holds something */

static int16_t clamp16(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* The table made now, from the external windows open, and written to the
 * screen block if it differs from what is there */
static void publish(void)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_screen_ext *out = ros_display_ext();
    if (!out)
        return;
    memset(&ext_now, 0, sizeof ext_now);
    int32_t H = w->screen_h;
    for (uint32_t i = 0; i < WIMP_WINDOWS && w->nsurfaces; i++) {
        struct wimp_window *win = w->win[i];
        struct wimp_surface *s = win ? win->surface : NULL;
        if (!s || !(s->flags & WS_EXTERNAL) || !win->s[APP].open || ext_now.nwindows >= ROS_EXT_WINDOWS)
            continue;
        /* Its chrome first.  The presenter puts the page where the table
         * says at once, but the title bar and toolbar around it are drawn
         * by the Wimp and its task on their next redraws: published now,
         * the page would land before its frame ("the frame draws after
         * the window", 14 October).  So while any of the window's area is
         * still invalid its entry is the last one published -- page and
         * frame then move together -- and a new window's page appears only
         * once its frame is drawn. */
        if (wimp_rl_meets(&w->invalid, win->s[APP].outline)) {
            int kept = 0;
            for (uint32_t j = 0; j < ext_was.nwindows && !kept; j++)
                if (ext_was.window[j].id == s->source) {
                    ext_now.window[ext_now.nwindows++] = ext_was.window[j];
                    for (uint32_t k = 0; k < ext_was.nrects && ext_now.nrects < ROS_EXT_RECTS; k++)
                        if (ext_was.rect[k].id == s->source)
                            ext_now.rect[ext_now.nrects++] = ext_was.rect[k];
                    kept = 1;
                }
            continue;
        }
        struct ros_ext_window *e = &ext_now.window[ext_now.nwindows++];
        e->id = s->source;
        /* Use the visible area's top left.  The work area's origin would be
         * wrong, because an external window's scroll bar belongs to its
         * program (a browser's page scrolls).  So the program's window
         * stays where the Wimp's window is. */
        e->x = clamp16(win->s[APP].vis.x0 >> w->xeig);
        e->y = clamp16((H - (win->s[APP].vis.y1 - (int32_t)s->sprite)) >> w->yeig);
        struct wimp_rlist *l = wimp_rl_new();
        if (!l)
            continue;
        wimp_inner(win, APP, l);
        wimp_rl_clip(l, wimp_screen_box());
        for (uint32_t k = 0; k < l->n && ext_now.nrects < ROS_EXT_RECTS; k++) {
            struct wimp_box b = l->b[k];
            struct ros_ext_rect *r = &ext_now.rect[ext_now.nrects++];
            r->id = s->source;
            r->x0 = clamp16(b.x0 >> w->xeig), r->x1 = clamp16(b.x1 >> w->xeig);
            r->y0 = clamp16((H - b.y1) >> w->yeig), r->y1 = clamp16((H - b.y0) >> w->yeig);
        }
        wimp_rl_free(l);
    }
    /* a mode change empties the block, so it is written again then */
    if (ext_any && out->seq == ext_was.seq && out->nwindows == ext_was.nwindows &&
        memcmp(&ext_now.window, &ext_was.window, sizeof ext_now.window) == 0 &&
        memcmp(&ext_now.rect, &ext_was.rect, sizeof ext_now.rect) == 0 && ext_now.nrects == ext_was.nrects)
        return;
    if (!ext_any && !ext_now.nwindows && !out->nwindows)
        return;
    uint32_t seq = (out->seq & ~1u) + 1u;       /* odd: being written */
    __atomic_store_n(&out->seq, seq, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(out->window, ext_now.window, sizeof out->window);
    memcpy(out->rect, ext_now.rect, ext_now.nrects * sizeof out->rect[0]);
    out->nwindows = ext_now.nwindows;
    out->nrects = ext_now.nrects;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&out->seq, seq + 1u, __ATOMIC_RELEASE);
    ext_now.seq = seq + 1u;
    ext_was = ext_now;
    ext_any = ext_now.nwindows != 0;
}

/* ---- icon surfaces: the GPU presenter's (design 37, 3.10) ----------------------------------- */

static struct {
    uint32_t window;                    /* the window's real handle */
    int32_t icon;
    uint32_t kind, param;
    uint8_t data[ROS_SURF_DATA];
    uint32_t rate;                      /* Hz it asks to be drawn again at (R7) */
    int shown;                          /* the last answer: the presenter draws it */
} isurf[ROS_SURFACES];
static uint32_t nisurf;
static struct ros_screen_surfaces surf_now, surf_was;
static int surf_any;

static uint32_t icon_word(const struct wimp_window *win, uint32_t i, unsigned off)
{
    uint32_t v;
    memcpy(&v, wimp_icon(win, i) + off, 4);
    return v;
}

/* An icon's box on the screen, in OS units */
static struct wimp_box icon_box(const struct wimp_window *win, uint32_t i)
{
    const struct wimp_place *p = &win->s[APP];
    struct wimp_box b = {
        p->vis.x0 - p->scx + (int32_t)icon_word(win, i, 0),
        p->vis.y1 - p->scy + (int32_t)icon_word(win, i, 4),
        p->vis.x0 - p->scx + (int32_t)icon_word(win, i, 8),
        p->vis.y1 - p->scy + (int32_t)icon_word(win, i, 12) };
    return b;
}

/* ---- damage: block v2 (design 37, interim status 5.1) ------------------------------------------
 *
 * What the Wimp drew since the presenter last looked: the rectangles it
 * handed to tasks (once drawn), drew itself, or copied to.  Kept here in
 * OS units and written to the v2 area at Wimp_Poll, in screen pixels, y
 * down, under its seqlock.  The list is begun afresh only when the
 * presenter has acknowledged the last one; till then it grows, and a list
 * that would overflow becomes "the whole screen".  Nothing is recorded
 * when there is no v2 area (no presenter, or the Mac). */
static struct {
    uint32_t n;
    struct wimp_box b[ROS_V2_DAMAGE];
    int full, dirty;
    uint32_t published;                 /* the frame last written */
} dmg;
static int surf_dirty_v2;               /* the surfaces list is to be written again */

void wimp_surface_list_dirty(void)
{
    surf_dirty_v2 = 1;
}

void wimp_damage_full(void)
{
    if (!ros_display_v2_shown())
        return;
    dmg.full = 1;
    dmg.dirty = 1;
}

void wimp_damage_add(struct wimp_box b)
{
    if (!ros_display_v2_shown() || dmg.full)
        return;
    struct wimp_box s = wimp_screen_box();
    if (b.x0 < s.x0) b.x0 = s.x0;
    if (b.y0 < s.y0) b.y0 = s.y0;
    if (b.x1 > s.x1) b.x1 = s.x1;
    if (b.y1 > s.y1) b.y1 = s.y1;
    if (b.x0 >= b.x1 || b.y0 >= b.y1)
        return;
    dmg.dirty = 1;
    /* a box inside one already there adds nothing */
    for (uint32_t i = 0; i < dmg.n; i++) {
        struct wimp_box *o = &dmg.b[i];
        if (b.x0 >= o->x0 && b.y0 >= o->y0 && b.x1 <= o->x1 && b.y1 <= o->y1)
            return;
    }
    if (dmg.n >= ROS_V2_DAMAGE) {
        dmg.full = 1;
        return;
    }
    dmg.b[dmg.n++] = b;
}

/* ---- the overlay (Sprint 5): drag boxes above every surface -------------------------
 *
 * A screen-sized backing of the Wimp's own, published as the last entry in
 * the list while a drag box is shown.  It is filled with holes (the "below"
 * tag, which the presenter shows as transparent), the Wimp's dashed boxes
 * are plotted into it and wiped by refilling the holes, and a drag's own
 * routines (DragASprite's) plot into it: their save and restore keep the
 * tag, so what they put back is a hole again.  Drawn in the framebuffer,
 * under the surfaces, a drag box was never seen. */
static struct {
    uint32_t id, stride, w, h;
    int on;                             /* shown: published at the top */
    int full;                           /* its damage is all of it */
    uint32_t n;
    struct wimp_box dmg[16];            /* screen OS units */
    uint32_t published;                 /* the frame its last damage went out in */
    int fresh;                          /* damage added since that publish */
} ov;

static uint32_t *ov_pixels(void)
{
    uint32_t area = wimp_backing_area(ov.id);
    if (!area)
        return NULL;
    uint32_t sp = area + 16;
    return (uint32_t *)ros_ptr(sp + ros_ld32(sp + 32));
}

/* the screen OS box b's pixels in the overlay, clipped: 0 if none */
static int ov_rect(struct wimp_box b, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    struct wimp_ws *w = wimp_ws();
    int32_t H = w->screen_h;
    *x0 = b.x0 >> w->xeig, *x1 = b.x1 >> w->xeig;
    *y0 = (H - b.y1) >> w->yeig, *y1 = (H - b.y0) >> w->yeig;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > (int32_t)ov.w) *x1 = (int32_t)ov.w;
    if (*y1 > (int32_t)ov.h) *y1 = (int32_t)ov.h;
    return *x0 < *x1 && *y0 < *y1;
}

void wimp_overlay_damage(struct wimp_box b)
{
    ov.fresh = 1;
    if (!ov.id || ov.full)
        return;
    if (ov.n >= sizeof ov.dmg / sizeof ov.dmg[0]) {
        ov.full = 1;
        return;
    }
    ov.dmg[ov.n++] = b;
}

/* holes over b: what was drawn there is gone */
void wimp_overlay_clear(struct wimp_box b)
{
    uint32_t *px = ov_pixels();
    int32_t x0, y0, x1, y1;
    if (!px || !ov_rect(b, &x0, &y0, &x1, &y1))
        return;
    for (int32_t y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)px + (size_t)y * ov.stride);
        for (int32_t x = x0; x < x1; x++)
            row[x] = 0x80000000u;
    }
    wimp_overlay_damage(b);
}

/* Output into the overlay, in screen coordinates; 0 if there is none (no
 * presenter): the caller draws on the screen as before */
int wimp_overlay_begin(uint32_t saved[4])
{
    struct wimp_ws *w = wimp_ws();
    if (!ros_display_v2_shown() || !w)
        return 0;
    uint32_t sw = (uint32_t)w->screen_w >> w->xeig, sh = (uint32_t)w->screen_h >> w->yeig;
    if (ov.id && (ov.w != sw || ov.h != sh)) {
        wimp_backing_free(ov.id);
        ov.id = 0;
    }
    if (!ov.id) {
        ov.id = wimp_backing_new(0, sw, sh, &ov.stride);
        if (!ov.id)
            return 0;
        ov.w = sw, ov.h = sh;
        wimp_overlay_clear(wimp_screen_box());
        ov.full = 1;
    }
    ov.on = 1;
    return wimp_backing_begin(ov.id, 0, 0, saved);
}

void wimp_overlay_end(const uint32_t saved[4])
{
    wimp_backing_end(saved);
}

/* the drag is over: the overlay leaves the list (its holes are kept) */
void wimp_overlay_off(void)
{
    if (!ov.on)
        return;
    ov.on = 0;
    surf_dirty_v2 = 1;
}

/* A task's rectangle, drawn into its window's backing, made opaque where
 * its alpha is still 0.  A backing stands for the screen, whose top byte
 * RISC OS ignores; what plots through the VDU, SpriteExtend or the Font
 * Manager is written opaque already, but a renderer that writes the
 * output's memory itself (ArtWorks' AWRender) leaves the top byte 0, and
 * the presenter blended those pixels over what was below: the picture
 * washed out, the backdrop through it (15 Oct).  The rectangle was cleared
 * before the task drew it, so every pixel in it is the window's; the
 * Pinboard's holes (tagged &80) and the corners' alpha (given at the
 * publish, after) are left as they are. */
void wimp_backing_opaque(struct wimp_window *win, struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_surface *sf = win->surface;
    uint32_t area = sf && sf->backing ? wimp_backing_area(sf->backing) : 0;
    if (!area)
        return;
    uint32_t sp = area + 16;
    uint8_t *img = (uint8_t *)ros_ptr(sp + ros_ld32(sp + 32));
    const struct wimp_box o = win->s[APP].outline;
    int32_t x0 = (b.x0 - o.x0) >> w->xeig, x1 = (b.x1 - o.x0) >> w->xeig;
    int32_t y0 = (o.y1 - b.y1) >> w->yeig, y1 = (o.y1 - b.y0) >> w->yeig;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)sf->backing_w) x1 = (int32_t)sf->backing_w;
    if (y1 > (int32_t)sf->backing_h) y1 = (int32_t)sf->backing_h;
    for (int32_t y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)(img + (size_t)y * sf->backing_stride);
        for (int32_t x = x0; x < x1; x++)
            if (!(row[x] >> 24))
                row[x] |= 0xFF000000u;
    }
}

/* Round corners as alpha (Sprint 5).  A round-cornered window's backing
 * has its corner squares' pixels given the arc's coverage, by 4 x 4
 * samples a pixel: outside, transparent; on the arc, part, premultiplied.
 * Only opaque pixels are touched, so a corner done once is left alone and
 * one the window drew again is done again.  The presenter blends it over
 * what is below; the window's rectangles keep its notches for that. */
static void corners_apply(struct wimp_window *win, struct wimp_surface *sf)
{
    int changed = 0;
    struct wimp_ws *w = wimp_ws();
    int32_t r = wimp_corner_radius(win, APP);
    uint32_t area = r ? wimp_backing_area(sf->backing) : 0;
    if (!area)
        return;
    uint32_t sp = area + 16;
    uint8_t *img = (uint8_t *)ros_ptr(sp + ros_ld32(sp + 32));
    int rx = (int)(r / w->dx), ry = (int)(r / w->dy);
    int bw = (int)sf->backing_w, bh = (int)sf->backing_h;
    if (rx > 32) rx = 32;
    if (ry > 32) ry = 32;
    if (rx < 2 || ry < 2 || rx * 2 > bw || ry * 2 > bh)
        return;
    int bottom = w->style.square_bottom == 0;
    for (int corner = 0; corner < (bottom ? 4 : 2); corner++) {
        int right = corner & 1, low = corner >> 1;
        for (int k = 0; k < ry; k++) {
            uint32_t *row = (uint32_t *)(img + (size_t)(low ? bh - 1 - k : k) * sf->backing_stride);
            for (int i = 0; i < rx; i++) {
                uint32_t *px = &row[right ? bw - 1 - i : i];
                if ((*px >> 24) != 0xFFu)
                    continue;               /* done, or a hole: left alone */
                int in = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float dx = ((float)rx - (i + (sx + 0.5f) / 4)) / rx;
                        float dy = ((float)ry - (k + (sy + 0.5f) / 4)) / ry;
                        in += dx * dx + dy * dy <= 1.0f;
                    }
                if (in == 16)
                    continue;
                changed = 1;
                uint32_t p = *px, a = (uint32_t)in * 255u / 16u;
                *px = (a << 24) | (((p >> 16 & 0xFFu) * a / 255u) << 16) | (((p >> 8 & 0xFFu) * a / 255u) << 8) |
                      ((p & 0xFFu) * a / 255u);
            }
        }
    }
    /* the corners' rows as the backing's damage -- only when a pixel was
     * changed: else each publish damaged them again, the list never
     * emptied, overflowed to "all of it", and every backing went up whole
     * at every publish */
    if (!changed)
        return;
    const struct wimp_box o = win->s[APP].outline;
    wimp_backing_damage(win, (struct wimp_box){ o.x0, o.y1 - ry * w->dy, o.x1, o.y1 });
    if (bottom)
        wimp_backing_damage(win, (struct wimp_box){ o.x0, o.y0, o.x1, o.y0 + ry * w->dy });
}

/* A window, if it has a backing, then its open children, back to front,
 * each followed by its own */
static void stack_add(struct wimp_window *win, struct wimp_window **stack, uint32_t *depth)
{
    if (!win->s[APP].open)
        return;
    if (win->surface && win->surface->backing && *depth < ROS_V2_SURFACES)
        stack[(*depth)++] = win;
    struct wimp_window *kids[64];
    uint32_t n = 0;
    for (struct wimp_window *c = wimp_stack_front(win->handle, APP); c && n < 64; c = wimp_window_below(c, APP))
        kids[n++] = c;
    while (n-- > 0)
        stack_add(kids[n], stack, depth);
}

static void publish_v2(void)
{
    struct ros_screen_v2 *v = ros_display_v2_shown();
    if (!v || v->magic != ROS_V2_MAGIC || (!dmg.dirty && !surf_dirty_v2 && !wimp_backing_any_dirty()))
        return;
    surf_dirty_v2 = 0;
    struct wimp_ws *w = wimp_ws();
    int32_t H = w->screen_h;
    uint32_t taken = __atomic_load_n(&v->ack, __ATOMIC_ACQUIRE) == dmg.published;
    uint32_t frame = (v->frame & ~1u) + 1u;    /* odd: being written */
    __atomic_store_n(&v->frame, frame, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (taken) {                                /* the last list was consumed: a fresh one */
        v->ndamage = 0;
        v->full = 0;
    }
    if (dmg.full || v->ndamage + dmg.n > ROS_V2_DAMAGE) {
        v->full = 1;
        v->ndamage = 0;
    } else {
        for (uint32_t i = 0; i < dmg.n; i++) {
            struct wimp_box b = dmg.b[i];
            struct ros_v2_rect *r = &v->damage[v->ndamage++];
            r->x0 = clamp16(b.x0 >> w->xeig), r->x1 = clamp16(b.x1 >> w->xeig);
            r->y0 = clamp16((H - b.y1) >> w->yeig), r->y1 = clamp16((H - b.y0) >> w->yeig);
        }
    }
    /* the surfaces list (5.2): the icon surfaces the table has, as its
     * first entries, so that the clock and the meter draw from either;
     * windows join it in Sprint 3 */
    uint32_t nr = 0, ns = 0, nsd = 0;
    for (uint32_t k = 0; k < nisurf && ns < ROS_V2_SURFACES; k++) {
        struct wimp_window *win = wimp_window(isurf[k].window);
        int32_t i = isurf[k].icon;
        if (!win || !win->s[APP].open || i < 0 || (uint32_t)i >= win->nicons ||
            (icon_word(win, (uint32_t)i, 16) & IF_DELETED_))
            continue;
        struct wimp_box b = icon_box(win, (uint32_t)i);
        struct ros_v2_surface e;
        memset(&e, 0, sizeof e);
        e.kind = isurf[k].kind;
        e.param = isurf[k].param;
        e.window = isurf[k].window;
        memcpy(e.data, isurf[k].data, sizeof e.data);
        e.stride = isurf[k].rate;               /* an icon surface's: the rate it asks */
        e.x = clamp16(b.x0 >> w->xeig);
        e.y = clamp16((H - b.y1) >> w->yeig);
        e.w = clamp16((b.x1 - b.x0) >> w->xeig);
        e.h = clamp16((b.y1 - b.y0) >> w->yeig);
        struct wimp_rlist *l = wimp_rl_new();
        if (!l)
            continue;
        wimp_inner(win, APP, l);
        wimp_rl_clip(l, b);
        wimp_rl_clip(l, wimp_screen_box());
        e.rects = nr;
        for (uint32_t r = 0; r < l->n && nr < ROS_V2_RECTS; r++) {
            struct wimp_box c = l->b[r];
            v->rect[nr].x0 = clamp16(c.x0 >> w->xeig), v->rect[nr].x1 = clamp16(c.x1 >> w->xeig);
            v->rect[nr].y0 = clamp16((H - c.y1) >> w->yeig), v->rect[nr].y1 = clamp16((H - c.y0) >> w->yeig);
            nr++;
        }
        e.nrects = nr - e.rects;
        wimp_rl_free(l);
        /* the frame it last changed in: kept when nothing about it moved */
        struct ros_v2_surface *was = ns < v->nsurfaces ? &v->surface[ns] : NULL;
        e.frame = frame + 1u;
        if (was && was->kind == e.kind && was->window == e.window && was->x == e.x && was->y == e.y &&
            was->w == e.w && was->h == e.h && was->nrects == e.nrects &&
            memcmp(was->data, e.data, sizeof e.data) == 0 &&
            memcmp(&v->rect[was->rects], &v->rect[e.rects], e.nrects * sizeof v->rect[0]) == 0)
            e.frame = was->frame;
        v->surface[ns++] = e;
    }
    /* the external windows with a backing (Sprint 3): the window's outline
     * is its place, its visible rectangles the outline's, back to front as
     * the stack has them.  The presenter draws the backing there once it
     * knows how; till then the entry is information. */
    /* back to front, each window followed by its children that are
     * surfaces (a toolbar, a pane): nested surfaces, drawn over their
     * parent as the stack has them, clipped to it by their rectangles */
    struct wimp_window *stack[ROS_V2_SURFACES];
    uint32_t depth = 0;
    {
        static struct wimp_window *top[WIMP_WINDOWS];     /* static: 32 KB is too much for the stack */
        uint32_t ntop = 0;
        for (struct wimp_window *win = wimp_stack_front(NO_WINDOW, APP); win && ntop < WIMP_WINDOWS;
             win = wimp_window_below(win, APP))
            top[ntop++] = win;
        while (ntop-- > 0)
            stack_add(top[ntop], stack, &depth);
    }
    for (uint32_t d = 0; d < depth && ns < ROS_V2_SURFACES; d++) {
        struct wimp_window *win = stack[d];       /* back to front */
        struct wimp_surface *sf = win->surface;
        if (!sf || !sf->backing || !win->s[APP].open)
            continue;
        struct ros_v2_surface e;
        memset(&e, 0, sizeof e);
        e.kind = ROS_SURF_WINDOW;
        e.param = sf->source;                   /* the compositor's id of its page */
        e.window = win->handle;
        const struct wimp_box o = win->s[APP].outline;
        e.x = clamp16(o.x0 >> w->xeig);
        e.y = clamp16((H - o.y1) >> w->yeig);
        e.w = clamp16((o.x1 - o.x0) >> w->xeig);
        e.h = clamp16((o.y1 - o.y0) >> w->yeig);
        e.backing = sf->backing;
        e.stride = sf->backing_stride;
        if ((sf->nbdmg || sf->bdmg_full) && wimp_corner_radius(win, APP))
            corners_apply(win, sf);             /* drawn into since: its corners' alpha */
        {
            int32_t rr = wimp_corner_radius(win, APP) >> w->xeig;
            e.data[1] = (uint8_t)(rr > 255 ? 255 : rr);     /* its corners, for the shadow */
        }
        /* the caret's window, or the parent of the pane that has it: where a
         * change lost on the way shows first (15 Oct) */
        {
            struct wimp_window *cw = w->caret.w != 0xFFFFFFFFu ? wimp_window(w->caret.w) : NULL;
            if (cw && (cw == win || (cw->s[APP].parent != NO_WINDOW && wimp_window(cw->s[APP].parent) == win &&
                                     !wimp_backed(cw))))
                e.data[0] = ROS_SURF_WIN_CARET;
        }
        /* its damage: what was drawn into the backing since the presenter
         * last took the list (its ack), in the backing's own pixels */
        if (sf->bdmg_frame && __atomic_load_n(&v->ack, __ATOMIC_ACQUIRE) >= sf->bdmg_frame) {
            /* the presenter has the frame the list was last published in:
             * what that frame carried is done with -- not what was drawn
             * since, which is the next frame's (a Filer selection, drawn
             * between two publishes, was lost here and never shown) */
            uint32_t done = sf->nbdmg_pub < sf->nbdmg ? sf->nbdmg_pub : sf->nbdmg;
            memmove(sf->bdmg, sf->bdmg + done, (sf->nbdmg - done) * sizeof sf->bdmg[0]);
            sf->nbdmg -= done;
            if (sf->bdmg_full && !sf->bdmg_more)
                sf->bdmg_full = 0;
            sf->bdmg_more = 0;
        }
        sf->nbdmg_pub = sf->nbdmg;
        sf->bdmg_more = 0;
        if (sf->bdmg_full || sf->nbdmg == 0) {
            e.damage = 0, e.ndamage = 0;        /* none, or all: the frame says which */
            if (sf->bdmg_full)
                e.ndamage = 0xFFFFFFFFu;        /* all of it */
        } else {
            e.damage = nsd;
            for (uint32_t k = 0; k < sf->nbdmg && nsd < ROS_V2_SDAMAGE; k++) {
                struct wimp_box c = sf->bdmg[k];
                v->sdamage[nsd].x0 = clamp16((c.x0 - o.x0) >> w->xeig);
                v->sdamage[nsd].x1 = clamp16((c.x1 - o.x0) >> w->xeig);
                v->sdamage[nsd].y0 = clamp16((o.y1 - c.y1) >> w->yeig);
                v->sdamage[nsd].y1 = clamp16((o.y1 - c.y0) >> w->yeig);
                nsd++;
            }
            e.ndamage = nsd - e.damage;
        }
        sf->bdmg_frame = frame + 1u;
        struct wimp_rlist *l = wimp_rl_new();
        if (!l)
            continue;
        wimp_visible_outline(win, o, APP, l);   /* its notches kept: alpha (corners_apply) */
        /* less its open children that are not surfaces themselves: they
         * are in the framebuffer, under the backings, and the parent's
         * backing would cover them */
        for (struct wimp_window *c = wimp_stack_front(win->handle, APP); c; c = wimp_window_below(c, APP))
            if (c->s[APP].open && !wimp_backed(c))
                wimp_rl_subtract(l, c->s[APP].outline);
        wimp_rl_clip(l, wimp_screen_box());
        e.rects = nr;
        for (uint32_t r = 0; r < l->n && nr < ROS_V2_RECTS; r++) {
            struct wimp_box c = l->b[r];
            v->rect[nr].x0 = clamp16(c.x0 >> w->xeig), v->rect[nr].x1 = clamp16(c.x1 >> w->xeig);
            v->rect[nr].y0 = clamp16((H - c.y1) >> w->yeig), v->rect[nr].y1 = clamp16((H - c.y0) >> w->yeig);
            nr++;
        }
        e.nrects = nr - e.rects;
        /* its shadow (5.2): where it shows, and the theme's values, for
         * the presenter's darkening pass */
        int32_t drop = 0, blur = 0;
        uint32_t strength = 0;
        l->n = 0;
        if (wimp_shadow_rects(win, APP, l, &drop, &blur, &strength)) {
            wimp_rl_clip(l, wimp_screen_box());
            e.shadow_rects = nr;
            for (uint32_t r = 0; r < l->n && nr < ROS_V2_RECTS; r++) {
                struct wimp_box c = l->b[r];
                v->rect[nr].x0 = clamp16(c.x0 >> w->xeig), v->rect[nr].x1 = clamp16(c.x1 >> w->xeig);
                v->rect[nr].y0 = clamp16((H - c.y1) >> w->yeig), v->rect[nr].y1 = clamp16((H - c.y0) >> w->yeig);
                nr++;
            }
            e.shadow_nrects = nr - e.shadow_rects;
            e.shadow_dx = 0;
            e.shadow_dy = clamp16(drop >> w->yeig);
            e.shadow_blur = clamp16(blur >> w->yeig);
            e.shadow_strength = clamp16((int32_t)strength);
        }
        wimp_rl_free(l);
        struct ros_v2_surface *was = ns < v->nsurfaces ? &v->surface[ns] : NULL;
        e.frame = frame + 1u;
        if (was && was->kind == e.kind && was->window == e.window && was->x == e.x && was->y == e.y &&
            was->w == e.w && was->h == e.h && was->nrects == e.nrects && was->backing == e.backing &&
            was->data[0] == e.data[0] && was->data[1] == e.data[1] &&
            was->shadow_nrects == e.shadow_nrects &&
            (!e.shadow_nrects || memcmp(&v->rect[was->shadow_rects], &v->rect[e.shadow_rects],
                                        e.shadow_nrects * sizeof v->rect[0]) == 0) &&
            !wimp_backing_dirty(e.backing) &&
            memcmp(&v->rect[was->rects], &v->rect[e.rects], e.nrects * sizeof v->rect[0]) == 0)
            e.frame = was->frame;
        v->surface[ns++] = e;
        /* an external window's page: a surface nested in it, in the same
         * list, so that it moves with the window and is covered by what is
         * in front of it in the same frame (the table at ROS_SCREEN_EXT,
         * published apart, let the page lag a window dragged over it) */
        if ((sf->flags & WS_EXTERNAL) && sf->source && ns < ROS_V2_SURFACES) {
            struct ros_v2_surface p;
            memset(&p, 0, sizeof p);
            p.kind = ROS_SURF_PAGE;
            p.param = sf->source;
            p.window = win->handle;
            p.x = clamp16(win->s[APP].vis.x0 >> w->xeig);
            p.y = clamp16((H - (win->s[APP].vis.y1 - (int32_t)sf->sprite)) >> w->yeig);
            p.w = clamp16((win->s[APP].vis.x1 - win->s[APP].vis.x0) >> w->xeig);
            p.h = clamp16((win->s[APP].vis.y1 - win->s[APP].vis.y0) >> w->yeig);
            struct wimp_rlist *pl = wimp_rl_new();
            if (pl) {
                wimp_inner(win, APP, pl);
                wimp_rl_clip(pl, wimp_screen_box());
                p.rects = nr;
                for (uint32_t r = 0; r < pl->n && nr < ROS_V2_RECTS; r++) {
                    struct wimp_box c = pl->b[r];
                    v->rect[nr].x0 = clamp16(c.x0 >> w->xeig), v->rect[nr].x1 = clamp16(c.x1 >> w->xeig);
                    v->rect[nr].y0 = clamp16((H - c.y1) >> w->yeig), v->rect[nr].y1 = clamp16((H - c.y0) >> w->yeig);
                    nr++;
                }
                p.nrects = nr - p.rects;
                wimp_rl_free(pl);
            }
            /* the frame it last changed in: kept while nothing about it
             * moved (its page's own frames come from the compositor) */
            struct ros_v2_surface *pw = ns < v->nsurfaces ? &v->surface[ns] : NULL;
            p.frame = frame + 1u;
            if (pw && pw->kind == p.kind && pw->param == p.param && pw->x == p.x && pw->y == p.y &&
                pw->w == p.w && pw->h == p.h && pw->nrects == p.nrects &&
                memcmp(&v->rect[pw->rects], &v->rect[p.rects], p.nrects * sizeof v->rect[0]) == 0)
                p.frame = pw->frame;
            v->surface[ns++] = p;
        }
    }
    if (ov.on && ov.id && ns < ROS_V2_SURFACES) {
        /* the overlay, over everything: drag boxes */
        struct ros_v2_surface o;
        memset(&o, 0, sizeof o);
        o.kind = ROS_SURF_WINDOW;
        o.window = 0;
        o.x = 0, o.y = 0, o.w = clamp16((int32_t)ov.w), o.h = clamp16((int32_t)ov.h);
        o.backing = ov.id;
        o.stride = ov.stride;
        o.rects = nr;
        if (nr < ROS_V2_RECTS) {
            v->rect[nr] = (struct ros_v2_rect){ 0, 0, o.w, o.h };
            nr++;
        }
        o.nrects = nr - o.rects;
        /* its damage accumulates until the presenter has taken the frame it
         * went out in, as a window's does: a frame the presenter skips
         * still has its rows uploaded */
        if (ov.published && __atomic_load_n(&v->ack, __ATOMIC_ACQUIRE) >= ov.published && !ov.fresh) {
            ov.full = 0, ov.n = 0;
            ov.published = 0;
        }
        ov.fresh = 0;
        if (ov.full || ov.n > 0) {
            ov.published = frame + 1u;
            if (ov.full) {
                o.ndamage = 0xFFFFFFFFu;
            } else {
                o.damage = nsd;
                for (uint32_t k = 0; k < ov.n && nsd < ROS_V2_SDAMAGE; k++) {
                    int32_t x0, y0, x1, y1;
                    if (!ov_rect(ov.dmg[k], &x0, &y0, &x1, &y1))
                        continue;
                    v->sdamage[nsd++] = (struct ros_v2_rect){ clamp16(x0), clamp16(y0), clamp16(x1), clamp16(y1) };
                }
                o.ndamage = nsd - o.damage;
            }
            o.frame = frame + 1u;
        } else {
            struct ros_v2_surface *was = ns < v->nsurfaces ? &v->surface[ns] : NULL;
            o.frame = was && was->backing == ov.id ? was->frame : frame + 1u;
        }
        wimp_backing_dirty(ov.id);
        v->surface[ns++] = o;
    }
    v->nsurfaces = ns;
    v->look = w->look;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&v->frame, frame + 1u, __ATOMIC_RELEASE);
    dmg.published = frame + 1u;
    dmg.n = 0;
    dmg.full = 0;
    dmg.dirty = 0;
}

/* the table made now, and written to the block if it differs */
static void publish_surfaces(void)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_screen_surfaces *out = ros_display_surfaces();
    if (!out)
        return;
    memset(&surf_now, 0, sizeof surf_now);
    int32_t H = w->screen_h;
    for (uint32_t k = 0; k < nisurf; k++) {
        struct wimp_window *win = wimp_window(isurf[k].window);
        int32_t i = isurf[k].icon;
        if (!win || !win->s[APP].open || i < 0 || (uint32_t)i >= win->nicons ||
            (icon_word(win, (uint32_t)i, 16) & IF_DELETED_))
            continue;
        struct wimp_box b = icon_box(win, (uint32_t)i);
        struct ros_surface *e = &surf_now.s[surf_now.n++];
        e->kind = isurf[k].kind;
        e->param = isurf[k].param;
        memcpy(e->data, isurf[k].data, sizeof e->data);
        e->x = clamp16(b.x0 >> w->xeig);
        e->y = clamp16((H - b.y1) >> w->yeig);
        e->w = clamp16((b.x1 - b.x0) >> w->xeig);
        e->h = clamp16((b.y1 - b.y0) >> w->yeig);
        struct wimp_rlist *l = wimp_rl_new();
        if (!l)
            continue;
        wimp_inner(win, APP, l);
        wimp_rl_clip(l, b);
        wimp_rl_clip(l, wimp_screen_box());
        for (uint32_t r = 0; r < l->n && e->nrects < ROS_SURF_RECTS; r++) {
            struct wimp_box c = l->b[r];
            int16_t *q = e->rect[e->nrects++];
            q[0] = clamp16(c.x0 >> w->xeig), q[2] = clamp16(c.x1 >> w->xeig);
            q[1] = clamp16((H - c.y1) >> w->yeig), q[3] = clamp16((H - c.y0) >> w->yeig);
        }
        wimp_rl_free(l);
    }
    /* a mode change empties the block, so it is written again then */
    if (surf_any && out->seq == surf_was.seq && out->n == surf_was.n &&
        memcmp(surf_now.s, surf_was.s, sizeof surf_now.s) == 0)
        return;
    if (!surf_any && !surf_now.n && !out->n)
        return;
    surf_dirty_v2 = 1;                          /* the v2 list follows the table (publish_v2) */
    uint32_t seq = (out->seq & ~1u) + 1u;       /* odd: being written */
    __atomic_store_n(&out->seq, seq, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(out->s, surf_now.s, sizeof out->s);
    out->n = surf_now.n;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&out->seq, seq + 1u, __ATOMIC_RELEASE);
    surf_now.seq = seq + 1u;
    surf_was = surf_now;
    surf_any = surf_now.n != 0;
}

/* &5204 SurfaceIcon */
static void icon_surface(struct ros_cpu *s)
{
    struct wimp_window *win = wimp_window_ib(s->r[1]);
    if (!win) {
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
        return;
    }
    int32_t icon = (int32_t)s->r[2];
    if (icon == -1 && s->r[3] == ROS_SURF_WINDOW) {
        /* the window itself a surface (5.4): a backing of its own, the
         * task drawing it as ever; the presenter composites it */
        struct wimp_ws *w = wimp_ws();
        if (!ros_display_v2_shown()) {
            s->r[0] = 0, s->r[1] = 0;           /* no presenter: a plain window */
            s->v = 0;
            return;
        }
        if (!win->surface) {
            void *mem;
            if (xos_module_claim(sizeof *win->surface, &mem)) {
                wimp_fail(s, wimp_error(E_TOO_BIG));
                return;
            }
            win->surface = mem;
            memset(win->surface, 0, sizeof *win->surface);
            win->surface->flags = WS_BACKED;
            w->nsurfaces++;
        }
        wimp_surface_backing(win);
        wimp_invalidate_all_of(win);            /* drawn afresh, into the backing */
        s->r[0] = 0, s->r[1] = win->surface->backing ? 1u : 0u;
        s->v = 0;
        return;
    }
    uint32_t k = 0;
    while (k < nisurf && !(isurf[k].window == win->handle && isurf[k].icon == icon))
        k++;
    if (s->r[3] == 0) {                         /* unbound: the owner's again */
        if (k < nisurf)
            isurf[k] = isurf[--nisurf];
        s->r[0] = 0, s->r[1] = 0;
        s->v = 0;
        return;
    }
    if (k == nisurf) {
        if (nisurf >= ROS_SURFACES) {
            wimp_fail(s, wimp_error(E_TOO_BIG));
            return;
        }
        nisurf++;
        isurf[k].shown = 0;
    }
    isurf[k].window = win->handle;
    isurf[k].icon = icon;
    isurf[k].kind = s->r[3];
    isurf[k].param = s->r[4];
    isurf[k].rate = s->r[7] > 120 ? 120 : s->r[7];
    memset(isurf[k].data, 0, sizeof isurf[k].data);
    if (s->r[5])
        memcpy(isurf[k].data, ros_ptr(s->r[5]), s->r[6] < ROS_SURF_DATA ? s->r[6] : ROS_SURF_DATA);
    publish_surfaces();                         /* the presenter sees it now */
    int shown = ros_display_presenter(ROS_PRESENT_SURFACES);
    /* handed to the presenter: the icon's owner makes its picture clear,
     * and a clear icon is plotted over what is there -- the icon bar
     * behind it must be drawn again, or the last picture stays in screen
     * memory under the presenter's */
    if (shown && !isurf[k].shown && icon >= 0 && (uint32_t)icon < win->nicons)
        wimp_invalidate_box(icon_box(win, (uint32_t)icon));
    isurf[k].shown = shown;
    s->r[0] = 0;
    s->r[1] = shown ? 1u : 0u;
    s->v = 0;
}

/* An external window's backing, the size of its outline, made when it is
 * first open and again when its size changes (Sprint 3: the chrome is to
 * be drawn into it).  Called from the flush, once a window's place is
 * settled. */
void wimp_surface_backing(struct wimp_window *win)
{
    struct wimp_surface *s = win->surface;
    if (!s || !(s->flags & (WS_EXTERNAL | WS_BACKED)) || !ros_display_v2_shown())
        return;
    if (!win->s[APP].open) {
        if (s->backing) {
            wimp_backing_free(s->backing);
            s->backing = 0;
        }
        return;
    }
    struct wimp_ws *w = wimp_ws();
    const struct wimp_box o = win->s[APP].outline, sc = wimp_screen_box();
    if (o.x1 <= sc.x0 || o.x0 >= sc.x1 || o.y1 <= sc.y0 || o.y0 >= sc.y1) {
        /* off the screen (a task's parked window): no slot for it until it
         * comes on, when the poll makes one */
        if (s->backing) {
            wimp_backing_free(s->backing);
            s->backing = 0;
            surf_dirty_v2 = 1;
        }
        return;
    }
    uint32_t bw = (uint32_t)(o.x1 - o.x0) >> w->xeig, bh = (uint32_t)(o.y1 - o.y0) >> w->yeig;
    if (s->backing && s->backing_w == bw && s->backing_h == bh)
        return;
    if (s->backing)
        wimp_backing_free(s->backing);
    s->backing = wimp_backing_new(win->handle, bw, bh, &s->backing_stride);
    s->backing_w = bw, s->backing_h = bh;
    surf_dirty_v2 = 1;
    /* a new backing is blank: the whole window drawn into it.  (The Filer
     * opens its window, then again at its final size: the second backing
     * showed nothing but the shadow, 14 Oct.) */
    if (s->backing)
        wimp_invalidate_all_of(win);            /* its own list: a surface window's redraw */
}

/* A rectangle drawn into a window's backing: kept for the entry's damage
 * range, so the presenter uploads those rows and not the backing (5.4) */
void wimp_backing_damage(struct wimp_window *win, struct wimp_box b)
{
    struct wimp_surface *s = win->surface;
    if (!s || !s->backing)
        return;
    if (s->bdmg_full) {
        s->bdmg_more = 1;                       /* drawn into since the list was published */
        return;
    }
    if (s->nbdmg >= sizeof s->bdmg / sizeof s->bdmg[0]) {
        s->bdmg_full = 1;
        return;
    }
    s->bdmg[s->nbdmg++] = b;
}

/* Wimp_Poll's search, before the redraw scan (task.c) */
void wimp_surface_poll(void)
{
    struct wimp_ws *w = wimp_ws();
    /* Sprint 3: a backing for each open external window; Sprint 4: and for
     * each window bound by &5204 (w->nsurfaces counts those, with no
     * external window about -- BackMe on its own, 14 Oct, never got one) */
    if (w && (ext_any || w->nsurfaces))
        for (uint32_t i = 0; i < WIMP_WINDOWS; i++) {
            struct wimp_window *win = w->win[i];
            if (win && win->surface && (win->surface->flags & (WS_EXTERNAL | WS_BACKED)) &&
                win->s[APP].open && !win->surface->backing)
                wimp_surface_backing(win);      /* which invalidates the window: drawn afresh into it */
        }
    if (w && (nisurf || surf_any))
        publish_surfaces();
    if (w && (w->nsurfaces || ext_any))
        publish();
    if (w)
        publish_v2();                           /* what was drawn since the last poll */
    if (!w || !w->surf_dirty || !w->nsurfaces)
        return;
    w->surf_dirty = 0;
    present_all(0);
}

/* At the VSync, as background work with the real display live, show the
 * changes now.  Do not do it in the middle of something: a Wimp SWI on
 * this task's thread (the real depth counts it, because every Wimp SWI is
 * a real section), a redraw or update loop, or output to a sprite. */
static void vsync_present(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w || !w->surf_dirty || !w->nsurfaces || !w->ntasks)
        return;
    struct ros_task *t = ros_task_current();
    if (!t || *ros_task_vdu_real(t) || w->pending_window || w->poller >= 0 || ros_irq_off_count())
        return;
    uint32_t sel, area, sprite;
    ros_vdu_output(&sel, &area, &sprite);
    if (sprite)
        return;
    ros_vdu_snapshot_take();
    w->surf_dirty = 0;
    if (!present_all(1))
        w->surf_dirty = 1;
    /* a program's own display window (cmdwin.c): no Wimp_Poll publishes
     * while it runs, so its changes are published here */
    if (w->disp_vd)
        publish_v2();
    ros_vdu_snapshot_restore();
}

static void changed_hook(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w && w->nsurfaces)
        w->surf_dirty = 1;
}

void wimp_surface_start(void)
{
    ros_vdisplay_changed_hook = changed_hook;
    ros_vdisplay_vsync_hook = vsync_present;
}

/* The desktop's mode or palette changed: every table made again */
void wimp_surface_desktop_changed(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w)
        return;
    for (uint32_t i = 0; i < WIMP_WINDOWS && w->nsurfaces; i++)
        if (w->win[i] && w->win[i]->surface)
            w->win[i]->surface->table_ok = 0;
}

/* ---- binding ---------------------------------------------------------------------------------- */

/* Sprint 4: every top-level window a surface when the presenter is there,
 * at its first open -- a backing of its own, composited by the presenter,
 * as &5204 with R2 = -1 gives on request.  Every window but the Wimp's
 * back window: the Pinboard's backdrop window, menus, the icon bar, children
 * (nested in their parent's surface).  The layers, from the bottom: the
 * presenter's backdrop, the framebuffer (transparent where surfaces cover
 * it), the surfaces (design 37, Sprint 5).  *Set Wimp$Surfaces off turns
 * it off for windows opened from then on; a window that gets no backing
 * (the slots used up) draws to the screen as before. */
void wimp_surface_default(struct wimp_window *win, uint32_t behind)
{
    struct wimp_ws *w = wimp_ws();
    if (!w || !ros_display_v2_shown())
        return;
    if (win->surface) {
        /* bound already (a VDisplay: GraphTask, a task window; a sprite):
         * a backing as well, the display painted into it */
        if (!(win->surface->flags & WS_EXTERNAL))
            win->surface->flags |= WS_BACKED;
        return;
    }
    if (win->s[REQ].parent != NO_WINDOW) {
        /* a child (a toolbar, a pane nested in its window): a surface nested
         * in its parent's, when the parent is a surface (15 Oct) */
        struct wimp_window *pw = wimp_window(win->s[REQ].parent);
        if (!pw || !pw->surface || !(pw->surface->flags & (WS_BACKED | WS_EXTERNAL)))
            return;
    }
    /* every window but the Wimp's back window: the Pinboard's backdrop
     * window too, whose "below" pixels (the Composite tile) are
     * transparent in its backing, over the presenter's backdrop.  The back
     * window paints the desktop colour, opaque: a surface, it would show
     * through the Pinboard's and hide the backdrop; in the framebuffer it
     * is never drawn while the Pinboard's surface covers it, the screen
     * there being made transparent instead (Sprint 5) */
    (void)behind;
    if (win->handle == w->back_window)
        return;
    {
        char *var = (char *)w->scratch + 256, *val = (char *)w->scratch + 300;
        strcpy(var, "Wimp$Surfaces");
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(var), c.r[1] = ros_addr(val), c.r[2] = 16, c.r[3] = 0, c.r[4] = 3;
        ros_swi(&c, XOS_ReadVarVal);
        if (!c.v && c.r[2] >= 3 && (val[0] | 0x20) == 'o' && (val[1] | 0x20) == 'f' && (val[2] | 0x20) == 'f')
            return;
    }
    void *mem;
    if (xos_module_claim(sizeof *win->surface, &mem))
        return;
    win->surface = mem;
    memset(win->surface, 0, sizeof *win->surface);
    win->surface->flags = WS_BACKED;
    w->nsurfaces++;                             /* the backing comes at the poll, and the redraw into it */
}

void wimp_surface_gone(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    if (!win->surface)
        return;
    if (win->surface->backing)
        wimp_backing_free(win->surface->backing);
    if (win->surface->binv)
        wimp_rl_free(win->surface->binv);
    xos_module_free(win->surface);
    win->surface = NULL;
    if (w->nsurfaces)
        w->nsurfaces--;
}


/* The window R1, which the caller must own */
static struct wimp_window *owned(struct ros_cpu *s)
{
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return NULL;
    }
    struct wimp_window *win = wimp_window(s->r[1]);
    if (!win) {
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
        return NULL;
    }
    if (win->owner != t->handle) {
        wimp_fail(s, wimp_error(E_OWNER_WINDOW));
        return NULL;
    }
    return win;
}

/* &5200's work, for the SWI and for the Wimp's own display windows
 * (cmdwin.c) */
os_error *wimp_surface_bind(struct wimp_window *win, uint32_t flags, uint32_t src, uint32_t spr, uint32_t scale)
{
    struct wimp_ws *w = wimp_ws();
    flags &= WS_VDISPLAY | WS_EXTERNAL;
    if (scale > 16)
        return wimp_error(E_BAD_OP);
    struct wimp_surface probe;
    memset(&probe, 0, sizeof probe);
    probe.flags = flags, probe.source = src, probe.sprite = spr;
    if (flags == (WS_VDISPLAY | WS_EXTERNAL) || (flags & WS_EXTERNAL && !probe.source))
        return wimp_error(E_BAD_OP);
    if (!flags && (probe.source < ROS_APP_BASE || probe.sprite < ROS_APP_BASE))
        return wimp_error(E_BAD_PTR_R1);
    if (!source(&probe))
        return (flags & WS_VDISPLAY) ? ros_error(E_NOT_DISPLAY, "Not a virtual display") : wimp_error(E_BAD_OP);
    struct wimp_surface *b = win->surface;
    if (!b) {
        void *mem;
        if (xos_module_claim(sizeof *b, &mem))
            return wimp_error(E_TOO_BIG);
        b = mem;
        memset(b, 0, sizeof *b);
        win->surface = b;
        w->nsurfaces++;
    }
    uint32_t presents = b->presents, rects = b->rects;
    uint32_t backing = b->backing, backing_w = b->backing_w, backing_h = b->backing_h,
             backing_stride = b->backing_stride;
    *b = probe;
    b->scale = scale;
    b->presents = presents, b->rects = rects;
    b->backing = backing, b->backing_w = backing_w, b->backing_h = backing_h;
    b->backing_stride = backing_stride;
    b->mode_gen = b->view.mode_gen;
    if (ros_display_v2_shown() && !(flags & WS_EXTERNAL))
        b->flags |= WS_BACKED;                  /* a backing of its own like every window
                                                   (Sprint 5): the display painted into it */
    wimp_surface_backing(win);                  /* Sprint 3: its chrome's backing, if open */
    if (flags & WS_VDISPLAY) {
        int32_t box[4];
        ros_vdisplay_take(b->source, box);      /* all of it is drawn now */
    }
    if (scale && !(flags & WS_EXTERNAL)) {
        int32_t box[4] = { 0, -(int32_t)(scale * (b->height << b->yeig)), (int32_t)(scale * (b->width << b->xeig)), 0 };
        wimp_set_extent(win, box);
    }
    invalidate_all(win);
    return NULL;
}

static void bind(struct ros_cpu *s)
{
    struct wimp_window *win = owned(s);
    if (!win)
        return;
    os_error *e = wimp_surface_bind(win, s->r[2], s->r[3], s->r[4], s->r[5]);
    if (e) {
        wimp_fail(s, e);
        return;
    }
    s->r[0] = 0;
    s->v = 0;
}

static void unbind(struct ros_cpu *s)
{
    struct wimp_window *win = owned(s);
    if (!win)
        return;
    if (win->surface) {
        wimp_surface_gone(win);
        /* a plain surface window from now (Sprint 5), drawn by its owner */
        if (win->s[APP].open)
            wimp_surface_default(win, 0);
        invalidate_all(win);                    /* the owner's to draw from now */
    }
    s->r[0] = 0;
    s->v = 0;
}

static void changed(struct ros_cpu *s)
{
    struct wimp_window *win = owned(s);
    if (!win)
        return;
    struct wimp_surface *b = win->surface;
    if (!b) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    if (!(b->flags & WS_EXTERNAL) && source(b) && win->s[APP].open) {
        wimp_flush();
        int32_t box[4] = { 0, 0, (int32_t)b->width - 1, (int32_t)b->height - 1 };
        if (!(s->r[2] & 1))
            for (int k = 0; k < 4; k++)
                box[k] = (int32_t)s->r[3 + k];
        show(win, screen_box(win, b, box), 1);
    }
    s->r[0] = 0;
    s->v = 0;
}

static void info(struct ros_cpu *s)
{
    struct wimp_window *win = wimp_window(s->r[1]);
    if (!win) {
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
        return;
    }
    struct wimp_surface *b = win->surface;
    s->r[0] = 0;
    s->r[1] = b ? b->flags : 0xFFFFFFFFu;
    s->r[2] = b ? b->presents : 0;
    s->r[3] = b ? b->rects : 0;
    s->r[4] = b ? b->scale : 0;
    s->v = 0;
}

void wimp_extend_surface(struct ros_cpu *s)
{
    switch (s->r[0] - WIMP_SURFACE_REASON) {
    case 0: bind(s); break;
    case 1: unbind(s); break;
    case 2: changed(s); break;
    case 4: icon_surface(s); break;
    default: info(s); break;
    }
}
