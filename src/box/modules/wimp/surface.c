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
    wimp_inner(win, APP, l);
    wimp_rl_minus(l, &w->invalid);
    wimp_rl_clip(l, box);
    wimp_rl_clip(l, wimp_screen_box());         /* a window may be partly off the screen */
    int done = 1;
    if (!l->n) {
        /* nothing of it visible */
    } else if (!direct_ok(win)) {
        if (may_invalidate)
            wimp_invalidate(l);
        else
            done = 0;
    } else {
        wimp_drag_hide();
        for (uint32_t k = 0; k < l->n; k++) {
            wimp_graphics_window(l->b[k]);
            paint(win, win->surface, l->b[k]);
        }
        wimp_default_windows();
        wimp_drag_show();
        win->surface->presents++;
    }
    wimp_rl_free(l);
    return done;
}

/* Mark all of the window's work area to be drawn again by the redraw scan */
static void invalidate_all(struct wimp_window *win)
{
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

/* Wimp_Poll's search, before the redraw scan (task.c) */
void wimp_surface_poll(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w && (w->nsurfaces || ext_any))
        publish();
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

void wimp_surface_gone(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    if (!win->surface)
        return;
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

static void bind(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = owned(s);
    if (!win)
        return;
    uint32_t flags = s->r[2] & (WS_VDISPLAY | WS_EXTERNAL), scale = s->r[5];
    if (scale > 16) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    struct wimp_surface probe;
    memset(&probe, 0, sizeof probe);
    probe.flags = flags, probe.source = s->r[3], probe.sprite = s->r[4];
    if (flags == (WS_VDISPLAY | WS_EXTERNAL) || (flags & WS_EXTERNAL && !probe.source)) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    if (!flags && (probe.source < ROS_APP_BASE || probe.sprite < ROS_APP_BASE)) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    if (!source(&probe)) {
        wimp_fail(s, (flags & WS_VDISPLAY) ? ros_error(E_NOT_DISPLAY, "Not a virtual display")
                                           : wimp_error(E_BAD_OP));
        return;
    }
    struct wimp_surface *b = win->surface;
    if (!b) {
        void *mem;
        if (xos_module_claim(sizeof *b, &mem)) {
            wimp_fail(s, wimp_error(E_TOO_BIG));
            return;
        }
        b = mem;
        memset(b, 0, sizeof *b);
        win->surface = b;
        w->nsurfaces++;
    }
    uint32_t presents = b->presents, rects = b->rects;
    *b = probe;
    b->scale = scale;
    b->presents = presents, b->rects = rects;
    b->mode_gen = b->view.mode_gen;
    if (flags & WS_VDISPLAY) {
        int32_t box[4];
        ros_vdisplay_take(b->source, box);      /* all of it is drawn now */
    }
    if (scale && !(flags & WS_EXTERNAL)) {
        int32_t box[4] = { 0, -(int32_t)(scale * (b->height << b->yeig)), (int32_t)(scale * (b->width << b->xeig)), 0 };
        wimp_set_extent(win, box);
    }
    invalidate_all(win);
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
    default: info(s); break;
    }
}
