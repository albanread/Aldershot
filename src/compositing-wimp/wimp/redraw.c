/* redraw.c: the invalid region, the flush of deferred opens, block
 * copies, and the redraw protocol.
 *
 * Each window has two placements. APP is the applied placement, which is
 * what the screen shows. REQ is the requested placement. The flush
 * compares the two for each marked window and makes invalid what changed.
 * It copies on the screen what can be moved instead of redrawn, and then
 * makes APP equal REQ.
 *
 * The redraw scan in Wimp_Poll finds the front window that meets the
 * invalid region and sends its owner a Redraw_Window_Request. If the
 * window is the Wimp's own, or is auto-redraw, the Wimp redraws it itself.
 *
 * Each window, a child included, is copied and invalidated on its own. A
 * parent and its children are not moved together as one block. The border
 * is kept only when the whole window moves unchanged in size, flags and
 * scroll offsets. Otherwise only the work area is copied and the border
 * is redrawn. */
#include <stdlib.h>
#include <string.h>

#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "wimp.h"

#define FLAG_AUTOREDRAW (1u << 4)
#define F_FURNITURE     0x7F000000u
#define ST_SHIFT_TOGGLED (1u << 22)

/* ---- the invalid region ------------------------------------------------------ */

/* The screen's invalid region alone: what was uncovered, which a surface
 * window's backing already holds (a move's old and new places) */
static void invalidate_screen(const struct wimp_rlist *l)
{
    struct wimp_ws *w = wimp_ws();
    wimp_rl_union(&w->invalid, l);
    if (w->invalid.overflow) {                  /* too many boxes: redraw the whole screen */
        w->invalid.n = 0;
        w->invalid.overflow = 0;
        wimp_rl_append(&w->invalid, wimp_screen_box());
    }
}

/* Sprint 4: a surface window's backing holds all of the window, covered
 * parts included, and the presenter puts it where the stack says.  So a
 * window over it moving away asks it for nothing: its redraws come from
 * what changed in it, kept in its own list (binv), relative to its
 * outline's bottom left.  An external window (the browser) is one too:
 * its chrome -- title, toolbar, URL bar, scroll bars -- is all in its
 * backing, and its page area there stays clear for the compositor's page
 * drawn over it (left out, the chrome outside what was visible at the
 * last redraw was the framebuffer's stale pixels, and typing in the URL
 * bar went to the framebuffer, under the backing, 14 Oct) */
int wimp_backed(const struct wimp_window *win)
{
    return win && win->surface && (win->surface->flags & (WS_BACKED | WS_EXTERNAL)) &&
           win->surface->backing && win->s[APP].open;
}

static struct wimp_rlist *binv_of(struct wimp_window *win)
{
    struct wimp_surface *s = win->surface;
    if (!s->binv && (s->binv = wimp_rl_new()) != NULL)
        s->binv->n = 0, s->binv->overflow = 0;
    return s->binv;
}

/* A box of the screen into the window's list */
static void binv_add(struct wimp_window *win, struct wimp_box b)
{
    const struct wimp_box o = win->s[APP].outline;
    if (b.x0 < o.x0) b.x0 = o.x0;
    if (b.y0 < o.y0) b.y0 = o.y0;
    if (b.x1 > o.x1) b.x1 = o.x1;
    if (b.y1 > o.y1) b.y1 = o.y1;
    struct wimp_rlist *l = binv_of(win);
    if (!l || b.x0 >= b.x1 || b.y0 >= b.y1)
        return;
    wimp_rl_add(l, (struct wimp_box){ b.x0 - o.x0, b.y0 - o.y0, b.x1 - o.x0, b.y1 - o.y0 });
    if (l->overflow) {                          /* too many boxes: all of it */
        l->n = 0, l->overflow = 0;
        wimp_rl_append(l, (struct wimp_box){ 0, 0, o.x1 - o.x0, o.y1 - o.y0 });
    }
}

/* The window's list in screen coordinates, into out */
static void binv_screen(const struct wimp_window *win, struct wimp_rlist *out)
{
    out->n = 0, out->overflow = 0;
    const struct wimp_rlist *l = win->surface ? win->surface->binv : NULL;
    if (!l)
        return;
    const struct wimp_box o = win->s[APP].outline;
    for (uint32_t i = 0; i < l->n; i++)
        wimp_rl_append(out, (struct wimp_box){ l->b[i].x0 + o.x0, l->b[i].y0 + o.y0, l->b[i].x1 + o.x0,
                                               l->b[i].y1 + o.y0 });
}

static void binv_subtract(struct wimp_window *win, struct wimp_box b)
{
    struct wimp_rlist *l = win->surface ? win->surface->binv : NULL;
    if (!l || !l->n)
        return;
    const struct wimp_box o = win->s[APP].outline;
    wimp_rl_subtract(l, (struct wimp_box){ b.x0 - o.x0, b.y0 - o.y0, b.x1 - o.x0, b.y1 - o.y0 });
}

/* Changed content, of no one window in particular (a forced redraw of the
 * screen, a mode or theme change, a box an icon bar or a shadow left): on
 * the screen, and in every surface window it meets */
static void invalidate_backed(const struct wimp_rlist *l)
{
    struct wimp_ws *w = wimp_ws();
    for (uint32_t i = 0; i < WIMP_WINDOWS; i++) {
        struct wimp_window *x = w->win[i];
        if (!wimp_backed(x) || !wimp_rl_meets(l, x->s[APP].outline))
            continue;
        for (uint32_t k = 0; k < l->n; k++)
            binv_add(x, l->b[k]);
    }
}

void wimp_invalidate(const struct wimp_rlist *l)
{
    invalidate_backed(l);
    invalidate_screen(l);
}

void wimp_invalidate_box(struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    for (uint32_t i = 0; i < WIMP_WINDOWS; i++) {
        struct wimp_window *x = w->win[i];
        if (wimp_backed(x))
            binv_add(x, b);
    }
    wimp_rl_add(&w->invalid, b);
}

/* A window's own content changed: in its list if it is a surface window,
 * else on the screen as ever */
void wimp_invalidate_in(struct wimp_window *win, const struct wimp_rlist *l)
{
    if (!wimp_backed(win)) {
        wimp_invalidate(l);
        return;
    }
    for (uint32_t k = 0; k < l->n; k++)
        binv_add(win, l->b[k]);
}

void wimp_invalidate_box_in(struct wimp_window *win, struct wimp_box b)
{
    if (wimp_backed(win))
        binv_add(win, b);
    else
        wimp_invalidate_box(b);
}

/* All of a surface window: a new backing, a new size, a new theme */
void wimp_invalidate_all_of(struct wimp_window *win)
{
    if (!wimp_backed(win)) {
        if (win->s[APP].open)
            wimp_invalidate_box(win->s[APP].outline);
        return;
    }
    struct wimp_rlist *l = binv_of(win);
    if (!l)
        return;
    const struct wimp_box o = win->s[APP].outline;
    l->n = 0, l->overflow = 0;
    wimp_rl_append(l, (struct wimp_box){ 0, 0, o.x1 - o.x0, o.y1 - o.y0 });
}

/* Where a window's work area may be drawn: the visible part, less open
 * children -- or, for a surface window, all of its work area less open
 * children, for its backing holds the covered parts too */
void wimp_drawable(const struct wimp_window *win, struct wimp_rlist *out)
{
    if (!wimp_backed(win)) {
        wimp_inner(win, APP, out);
        return;
    }
    out->n = 0, out->overflow = 0;
    wimp_rl_append(out, win->s[APP].vis);
    for (struct wimp_window *x = wimp_stack_front(win->handle, APP); x; x = wimp_window_below(x, APP))
        if (x->s[APP].open)
            wimp_rl_subtract(out, x->s[APP].outline);
}

/* Less what is waiting to be redrawn anyway */
void wimp_minus_pending(const struct wimp_window *win, struct wimp_rlist *l)
{
    struct wimp_ws *w = wimp_ws();
    if (!wimp_backed(win)) {
        wimp_rl_minus(l, &w->invalid);
        return;
    }
    struct wimp_rlist *b = wimp_rl_new();
    if (!b)
        return;
    binv_screen(win, b);
    wimp_rl_minus(l, b);
    wimp_rl_free(b);
}

/* Output left in a backing by a redraw that did not go on (an error, a
 * task that stopped asking for rectangles): back to the screen.  The
 * redraw itself stands; its next rectangle switches again. */
void wimp_backing_safety(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w && w->pending_backing_on) {
        wimp_backing_end(w->pending_backing);
        w->pending_backing_on = 0;
    }
}

void wimp_cancel_redraw(void)
{
    struct wimp_ws *w = wimp_ws();
    wimp_backing_safety();
    w->pending_window = 0;
    w->pending.n = 0;
}

/* ---- the flush -------------------------------------------------------------------- */

struct copy {
    struct wimp_box dst;
    int32_t dx, dy;
    uint32_t window;
};

/* Do the copies in an order that never overwrites the source of a copy
 * still to be done. If the copies form a cycle, the last copy in it is
 * dropped and its destination is left invalid. */
static void do_copies_now(struct copy *c, uint32_t n);

/* Set while the copies are made in a window's backing (Wimp_BlockCopy on a
 * surface window): no screen damage, no screen invalid region, and the
 * drag box, which is on the screen, left alone */
static int copies_in_backing;

static void do_copies(struct copy *c, uint32_t n)
{
    if (!n)
        return;
    if (copies_in_backing) {
        do_copies_now(c, n);
        return;
    }
    wimp_drag_hide();                           /* remove the drag box while copying */
    do_copies_now(c, n);
    wimp_drag_show();
}

static void do_copies_now(struct copy *c, uint32_t n)
{
    struct wimp_ws *w = wimp_ws();
    int *done = calloc(n ? n : 1, sizeof *done);
    if (!done)
        return;
    for (uint32_t left = n; left;) {
        uint32_t pick = n, last = n;
        for (uint32_t i = 0; i < n && pick == n; i++) {
            if (done[i])
                continue;
            last = i;
            int clear = 1;
            for (uint32_t j = 0; j < n && clear; j++) {
                if (j == i || done[j])
                    continue;
                struct wimp_box src = { c[j].dst.x0 - c[j].dx, c[j].dst.y0 - c[j].dy,
                                        c[j].dst.x1 - c[j].dx, c[j].dst.y1 - c[j].dy };
                if (c[i].dst.x0 < src.x1 && src.x0 < c[i].dst.x1 && c[i].dst.y0 < src.y1 &&
                    src.y0 < c[i].dst.y1)
                    clear = 0;
            }
            if (clear)
                pick = i;
        }
        if (pick == n) {                        /* a cycle: drop one and leave it invalid */
            done[last] = 1;
            left--;
            continue;
        }
        struct copy *k = &c[pick];
        if (k->dx || k->dy) {
            struct wimp_box src = { k->dst.x0 - k->dx, k->dst.y0 - k->dy, k->dst.x1 - k->dx,
                                    k->dst.y1 - k->dy };
            wimp_filter_copy(k->window, k->dst, src, w->dx, w->dy);    /* the copy filters */
            wimp_copy_rect(src, k->dx, k->dy);
            if (!copies_in_backing)
                wimp_damage_add(k->dst);        /* block v2: the copy, once made */
        }
        if (!copies_in_backing)
            wimp_rl_subtract(&w->invalid, k->dst);
        done[pick] = 1;
        left--;
    }
    free(done);
}

void wimp_flush(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w || !w->any_marked)
        return;
    /* a window moved, opened, closed or changed its place in the stack:
     * the surfaces list goes out again at the next poll.  Until the move
     * set this itself, the screen damage of what it uncovered did -- and
     * with layer 1 not shown that went (kernel #77): a drag published once
     * a second, when the clock's hands did (15 Oct, "slow and jerky") */
    wimp_surface_list_dirty();
    struct wimp_rlist *i0 = wimp_rl_new(), *ov = wimp_rl_new(), *nv = wimp_rl_new();
    struct wimp_rlist *src = wimp_rl_new(), *dst = wimp_rl_new(), *add = wimp_rl_new();
    struct wimp_rlist *skip = wimp_rl_new(), *ext = wimp_rl_new();
    struct copy *copies = calloc(WIMP_RL_MAX, sizeof *copies);
    if (!i0 || !ov || !nv || !src || !dst || !add || !skip || !ext || !copies)
        goto out;
    skip->n = 0;
    wimp_rl_copy(i0, &w->invalid);
    add->n = 0;
    uint32_t ncopies = 0;
    for (uint32_t i = 0; i < WIMP_WINDOWS; i++) {
        struct wimp_window *win = w->win[i];
        if (!win || !win->marked)
            continue;
        struct wimp_place *a = &win->s[APP], *r = &win->s[REQ];
        wimp_visible(win, a->outline, APP, ov);
        wimp_visible(win, r->outline, REQ, nv);
        wimp_rl_union(add, ov);                 /* old and new areas are both made invalid */
        wimp_rl_union(add, nv);
        wimp_shadow_add(win, APP, add);         /* and its shadows, old and new */
        wimp_shadow_add(win, REQ, add);
        /* a child (a pane, a toolbar) of a surface window: where it was, the
         * parent's backing must now hold the parent -- the uncovering alone
         * would be dropped, the parent being a surface window, and the
         * child's old picture would stay there */
        if (a->open && a->parent != NO_WINDOW) {
            struct wimp_window *pw = wimp_window(a->parent);
            /* not when it moved with its parent, keeping its place in it:
             * the parent's picture under it is as it was */
            int along = r->open && pw && r->outline.x0 - pw->s[REQ].outline.x0 == a->outline.x0 - pw->s[APP].outline.x0 &&
                        r->outline.y0 - pw->s[REQ].outline.y0 == a->outline.y0 - pw->s[APP].outline.y0 &&
                        r->outline.x1 - r->outline.x0 == a->outline.x1 - a->outline.x0 &&
                        r->outline.y1 - r->outline.y0 == a->outline.y1 - a->outline.y0;
            if (wimp_backed(pw) && !along)
                binv_add(pw, a->outline);
        }
        if (!ov->n || !nv->n)
            continue;                           /* opened or closed: nothing to copy */
        int linked = ((a->flags ^ r->flags) & (F_FURNITURE | ST_SHIFT_TOGGLED | (1u << 20))) == 0 &&
                     a->vis.x1 - a->vis.x0 == r->vis.x1 - r->vis.x0 &&
                     a->vis.y1 - a->vis.y0 == r->vis.y1 - r->vis.y0 &&
                     a->scx == r->scx && a->scy == r->scy;
        if (wimp_backed(win)) {
            /* a surface window: its backing goes with it, so a move copies
             * nothing (the screen memory there is never shown); anything
             * else -- a new size, a scroll, other furniture -- and all of it
             * is drawn again (relative to the outline: the list moves with
             * the window) */
            if (!linked) {
                struct wimp_rlist *bl = binv_of(win);
                if (bl) {
                    bl->n = 0, bl->overflow = 0;
                    wimp_rl_append(bl, (struct wimp_box){ 0, 0, r->outline.x1 - r->outline.x0,
                                                          r->outline.y1 - r->outline.y0 });
                }
            }
            continue;
        }
        int32_t dx = wimp_origin_x(win, REQ) - wimp_origin_x(win, APP);
        int32_t dy = wimp_origin_y(win, REQ) - wimp_origin_y(win, APP);
        /* Copy the old visible part, less what was already invalid, moved
         * to its new place and clipped to the new visible part. */
        if (linked) {
            wimp_rl_copy(src, ov);
            wimp_rl_copy(dst, nv);
        } else {
            /* Use the visible work area less its children's outlines, old
             * and new. A child that stays put keeps its pixels. NetSurf's
             * toolbar, nested along the top of a scrolled window, is an
             * example. */
            wimp_inner(win, APP, src);
            wimp_inner(win, REQ, dst);
        }
        wimp_rl_minus(src, i0);
        struct wimp_rlist *sh = wimp_rl_new();
        if (sh) {                               /* shadowed pixels are not copied, but redrawn */
            wimp_shadows_above(win, APP, sh);
            wimp_rl_minus(src, sh);
        }
        wimp_rl_translate(src, dx, dy);
        wimp_rl_and(src, dst);
        if (sh) {
            wimp_shadows_above(win, REQ, sh);
            wimp_rl_minus(src, sh);
            wimp_rl_free(sh);
        }
        /* A composited window moving -- the browser's: its page area at the
         * new place is neither copied nor redrawn, for the screen memory
         * there is never seen (the presenter draws the page over it).  Its
         * toolbar and furniture, which RISC OS draws, move as any window's
         * do; the desktop it uncovers is redrawn as ever. */
        wimp_surface_external_area(win, REQ, ext);
        if (ext->n) {
            wimp_rl_minus(src, ext);
            wimp_rl_union(skip, ext);
        }
        for (uint32_t k = 0; k < src->n && ncopies < WIMP_RL_MAX; k++)
            copies[ncopies++] = (struct copy){ src->b[k], dx, dy, win->handle };
    }
    wimp_rl_minus(add, skip);
    invalidate_screen(add);                     /* uncovered: no surface window's business */
    /* The screen now shows what was asked for. */
    for (uint32_t i = 0; i < WIMP_WINDOWS; i++) {
        struct wimp_window *win = w->win[i];
        if (!win)
            continue;
        int was_marked = win->marked;
        win->s[APP] = win->s[REQ];
        win->marked = 0;
        if (was_marked && win->surface)
            wimp_surface_backing(win);          /* Sprint 3: its chrome's backing, sized to it */
    }
    w->top[APP] = w->top[REQ];
    w->any_marked = 0;
    do_copies(copies, ncopies);                 /* then copy the pixels */
    wimp_front_check();                         /* Chrome.Focus front: the active look moves */
out:
    free(copies);
    wimp_rl_free(skip);
    wimp_rl_free(ext);
    wimp_rl_free(add);
    wimp_rl_free(dst);
    wimp_rl_free(src);
    wimp_rl_free(nv);
    wimp_rl_free(ov);
    wimp_rl_free(i0);
}

/* ---- handing out rectangles ------------------------------------------------------------ */

static uint32_t work_bg(const struct wimp_window *win)
{
    if (win->def[35] == 0 && wimp_menu_themed(win))
        return wimp_ws()->style.menu_bg;        /* a menu the theme draws (design 35) */
    if (wimp_iconbar_themed(win) && wimp_ws()->style.iconbar_bg != 0xFFFFFFFFu)
        return wimp_ws()->style.iconbar_bg;     /* the theme's icon bar */
    return win->def[35] == 0xFF ? 0xFFFFFF00u : wimp_colour(win->def[35]);
}

/* The work of Wimp_GetRectangle. Finish the previous rectangle and hand
 * out the next. Returns 1 with a rectangle, or 0 at the end. */
static int next_rectangle(struct wimp_window *win, uint32_t block)
{
    struct wimp_ws *w = wimp_ws();
    wimp_vdu5();
    /* Over the previous rectangle, draw the icons, then the caret and any
     * 3D border. */
    struct wimp_task *ct = wimp_current();
    uint32_t task = ct ? ct->internal : 0;
    if (!w->pending_first) {
        wimp_draw_icons(win, w->pending_rect);
        wimp_filter_posticon(win->handle, task, w->dr.clip);   /* the post-icon filters */
        wimp_caret_paint(win);                  /* the caret, over the icons */
        wimp_draw_3d_border(win);
        wimp_draw_corner_edges(win);            /* round corners' edges over the work area */
        if (!w->pending_update && !w->pending_backing_on)
            wimp_shadows_over(win, w->pending_rect);    /* the shadows of windows above (not
                                                           into a backing: they would stay
                                                           when the window above moved) */
        if (win->owner == NO_WINDOW)
            wimp_menu_separators(win);          /* a menu's separator lines */
        wimp_damage_add(w->pending_rect);       /* block v2: the rectangle, now drawn */
    }
    if (w->pending_backing_on) {                /* Sprint 3: the task drew that one into the backing */
        wimp_backing_end(w->pending_backing);
        w->pending_backing_on = 0;
        wimp_backing_opaque(win, w->pending_rect);  /* a renderer's own writes, opaque */
        wimp_backing_damage(win, w->pending_rect);  /* 5.4: the backing's own damage */
    }
    if (!w->pending.n) {
        w->pending_window = 0;
        wimp_default_windows();
        wimp_drag_show();                       /* put the drag box back */
        if (block) {
            const struct wimp_place *p = &win->s[APP];
            ros_st32(block + 4, (uint32_t)p->vis.x0);
            ros_st32(block + 8, (uint32_t)p->vis.y0);
            ros_st32(block + 12, (uint32_t)p->vis.x1);
            ros_st32(block + 16, (uint32_t)p->vis.y1);
            ros_st32(block + 20, (uint32_t)p->scx);
            ros_st32(block + 24, (uint32_t)p->scy);
        }
        return 0;
    }
    struct wimp_box r = w->pending.b[0];
    memmove(&w->pending.b[0], &w->pending.b[1], (w->pending.n - 1) * sizeof r);
    w->pending.n--;
    w->pending_rect = r;
    w->pending_first = 0;
    if (!w->pending_update) {
        wimp_rl_subtract(&w->invalid, r);       /* a redraw makes it valid */
        binv_subtract(win, r);
    }
    wimp_filter_rect(win->handle, task, r);     /* the rectangle filters */
    /* Sprint 3: an external window's toolbar -- the top of its work area,
     * above the page -- is chrome, drawn by the task into the window's
     * backing: output goes there for this rectangle, and comes back when
     * the next is fetched */
    if (wimp_backed(win) &&
        wimp_backing_begin(win->surface->backing, win->s[APP].outline.x0, win->s[APP].outline.y0,
                           w->pending_backing))
        w->pending_backing_on = 1;
    wimp_graphics_window(r);                    /* clip to the rectangle */
    if (!w->pending_update && win->def[35] != 0xFF && !wimp_surface_fills(win)) {   /* surface.c fills it */
        wimp_gcol(work_bg(win), 1);             /* clear to the background, or tile it */
        if (wimp_menu_themed(win) || (wimp_iconbar_themed(win) && wimp_ws()->style.iconbar_bg != 0xFFFFFFFFu) ||
            !wimp_draw_tile(win))
            wimp_clg();
    }
    wimp_gcol(wimp_colour(win->def[34]), 0);    /* the work area colours for the task */
    wimp_gcol(work_bg(win), 1);
    if (w->pending_update)
        wimp_caret_paint(win);                  /* erase the caret until the icons are drawn */
    wimp_filter_postrect(win->handle, task, r);
    if (block) {
        const struct wimp_place *p = &win->s[APP];
        ros_st32(block + 4, (uint32_t)p->vis.x0);
        ros_st32(block + 8, (uint32_t)p->vis.y0);
        ros_st32(block + 12, (uint32_t)p->vis.x1);
        ros_st32(block + 16, (uint32_t)p->vis.y1);
        ros_st32(block + 20, (uint32_t)p->scx);
        ros_st32(block + 24, (uint32_t)p->scy);
        ros_st32(block + 28, (uint32_t)r.x0);
        ros_st32(block + 32, (uint32_t)r.y0);
        ros_st32(block + 36, (uint32_t)r.x1);
        ros_st32(block + 40, (uint32_t)r.y1);
    }
    return 1;
}

/* The work of Wimp_RedrawWindow, for a task or for the Wimp itself */
static int start_redraw(struct wimp_window *win, uint32_t block)
{
    struct wimp_ws *w = wimp_ws();
    if (w->pending_backing_on) {                /* a redraw abandoned with output in a backing */
        wimp_backing_end(w->pending_backing);
        w->pending_backing_on = 0;
    }
    wimp_flush();
    if (w->pending_window != win->handle) {
        struct wimp_rlist *l = wimp_rl_new();
        if (wimp_backed(win)) {                 /* all of it: the backing holds the covered parts */
            l->n = 0, l->overflow = 0;
            wimp_rl_append(l, win->s[APP].outline);
        } else {
            wimp_visible(win, win->s[APP].outline, APP, l);
        }
        wimp_rl_copy(&w->pending, l);
        wimp_rl_free(l);
    }
    w->pending_window = win->handle;
    w->pending_update = 0;
    w->pending_first = 1;
    wimp_drag_hide();
    wimp_vdu5();
    wimp_ecf_origin(win->s[APP].outline.x0, win->s[APP].outline.y1 - 1);
    /* Draw the border, rectangle by rectangle -- into the window's
     * backing when it has one (Sprint 3: the presenter draws the chrome
     * from there, and the framebuffer under it is never shown), else
     * on the screen */
    uint32_t saved[4];
    int into = win->surface && win->surface->backing &&
               wimp_backing_begin(win->surface->backing, win->s[APP].outline.x0, win->s[APP].outline.y0, saved);
    wimp_draw_border(win, &w->pending);
    if (into)
        wimp_backing_end(saved);
    /* The border parts are valid now. */
    struct wimp_rlist *border = wimp_rl_new();
    if (border) {
        wimp_rl_copy(border, &w->pending);
        wimp_rl_subtract(border, win->s[APP].vis);
        wimp_rl_minus(&w->invalid, border);
        for (uint32_t k = 0; k < border->n; k++) {
            if (into) {
                wimp_backing_damage(win, border->b[k]); /* 5.4: the backing's own damage */
                binv_subtract(win, border->b[k]);
            } else {
                wimp_shadows_over(win, border->b[k]);   /* the shadows over the border (a
                                                           backing has none: the presenter's) */
                wimp_damage_add(border->b[k]);          /* block v2: the chrome, drawn */
            }
        }
        wimp_rl_free(border);
    }
    /* What is left to hand out is the work area, less open children. */
    wimp_rl_clip(&w->pending, win->s[APP].vis);
    for (struct wimp_window *c = wimp_stack_front(win->handle, APP); c; c = wimp_window_below(c, APP))
        if (c->s[APP].open)
            wimp_rl_subtract(&w->pending, c->s[APP].outline);
    wimp_ecf_origin(wimp_origin_x(win, APP), wimp_origin_y(win, APP));
    return next_rectangle(win, block);
}

static struct wimp_window *owned_window(struct ros_cpu *s, uint32_t handle)
{
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return NULL;
    }
    struct wimp_window *win = wimp_window(handle);
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

void wimp_swi_RedrawWindow(struct ros_cpu *s)
{
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    wimp_flush();
    struct wimp_window *win = owned_window(s, ros_ld32(s->r[1]));
    if (!win)
        return;
    s->r[0] = start_redraw(win, s->r[1]) ? 0xFFFFFFFFu : 0;
    s->v = 0;
}

void wimp_swi_GetRectangle(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    wimp_flush();
    uint32_t handle = ros_ld32(s->r[1]);
    if (!w->pending_window || handle != w->pending_window) {
        wimp_fail(s, ros_error(E_GET_RECT, "Get_Rectangle not called correctly"));
        return;
    }
    struct wimp_window *win = owned_window(s, handle);
    if (!win)
        return;
    s->r[0] = next_rectangle(win, s->r[1]) ? 0xFFFFFFFFu : 0;
    s->v = 0;
}

void wimp_swi_UpdateWindow(struct ros_cpu *s)
{
    wimp_clipboard_unpark();                    /* the task acted: forget a held DataLoad */
    struct wimp_ws *w = wimp_ws();
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint32_t b = s->r[1];
    struct wimp_window *win = owned_window(s, ros_ld32(b));
    if (!win)
        return;
    wimp_flush();
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    struct wimp_box box = { ox + (int32_t)ros_ld32(b + 4), oy + (int32_t)ros_ld32(b + 8),
                            ox + (int32_t)ros_ld32(b + 12), oy + (int32_t)ros_ld32(b + 16) };
    wimp_drawable(win, &w->pending);            /* a surface window's covered parts too */
    wimp_minus_pending(win, &w->pending);
    wimp_rl_clip(&w->pending, box);
    if (!wimp_backed(win))
        wimp_shadows_out(win, &w->pending);     /* shadowed parts are redrawn, not updated */
    w->pending_window = win->handle;
    w->pending_update = 1;
    w->pending_first = 1;
    wimp_drag_hide();
    wimp_ecf_origin(ox, oy);
    s->r[0] = next_rectangle(win, b) ? 0xFFFFFFFFu : 0;
    s->v = 0;
}

/* ---- Wimp_ForceRedraw and Wimp_BlockCopy ---------------------------------------------- */

void wimp_swi_ForceRedraw(struct ros_cpu *s)
{
    wimp_clipboard_unpark();
    if (!wimp_current()) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    wimp_cancel_redraw();
    wimp_flush();
    struct wimp_box box = { (int32_t)s->r[1], (int32_t)s->r[2], (int32_t)s->r[3],
                            (int32_t)s->r[4] };
    if (s->r[0] == 0xFFFFFFFFu) {
        struct wimp_rlist *l = wimp_rl_new();
        if (l) {
            wimp_rl_append(l, wimp_round_box(box));
            wimp_invalidate(l);
            wimp_rl_free(l);
        }
        s->v = 0;
        return;
    }
    struct wimp_window *win = wimp_window_ib(s->r[0]);
    if (!win) {
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
        return;
    }
    if (s->r[1] == TASK_WORD) {                 /* the window furniture */
        /* R2 = 3 redraws the title bar, as int_force_redraw_border does in
         * ROOL's Wimp02. Filer_Action does this after it writes a new title
         * into its window's indirected title ("Counting files - Finished").
         * The caret's focus change does it here too (title_redraw in
         * caret.c). The other border icons, and R2 = 0 for the whole border,
         * are not done yet. */
        if (s->r[2] == 3 && win->s[APP].open) {
            wimp_invalidate_box_in(win, wimp_title_strip(win));
        }
        s->r[1] = 0;
        s->v = 0;
        return;
    }
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    box.x0 += ox, box.x1 += ox, box.y0 += oy, box.y1 += oy;
    struct wimp_rlist *l = wimp_rl_new();
    if (l) {
        wimp_drawable(win, l);
        wimp_minus_pending(win, l);
        wimp_rl_clip(l, box);
        wimp_invalidate_in(win, l);             /* a surface window's own list */
        wimp_rl_free(l);
    }
    s->v = 0;
}

void wimp_swi_BlockCopy(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = owned_window(s, s->r[0]);
    if (!win)
        return;
    wimp_flush();
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    int32_t dx = (int32_t)(s->r[5] - s->r[1]), dy = (int32_t)(s->r[6] - s->r[2]);
    struct wimp_box from = { ox + (int32_t)s->r[1], oy + (int32_t)s->r[2], ox + (int32_t)s->r[3],
                             oy + (int32_t)s->r[4] };
    struct wimp_box to = { from.x0 + dx, from.y0 + dy, from.x1 + dx, from.y1 + dy };
    struct wimp_rlist *part = wimp_rl_new(), *inner = wimp_rl_new(), *dest = wimp_rl_new();
    if (part && inner && dest && wimp_backed(win)) {
        /* a surface window: the copy is made in its backing, all of the
         * work area (covered or not); what it could not fill is the task's
         * to draw again */
        wimp_drawable(win, inner);
        wimp_rl_copy(part, inner);
        wimp_minus_pending(win, part);
        wimp_rl_clip(part, from);
        wimp_rl_translate(part, dx, dy);
        wimp_rl_and(part, inner);
        wimp_rl_copy(dest, inner);
        wimp_rl_clip(dest, to);
        wimp_invalidate_in(win, dest);
        uint32_t saved[4];
        wimp_backing_safety();                  /* not inside another's redraw output */
        if (part->n && wimp_backing_begin(win->surface->backing, win->s[APP].outline.x0,
                                          win->s[APP].outline.y0, saved)) {
            struct copy *c = calloc(part->n, sizeof *c);
            if (c) {
                for (uint32_t k = 0; k < part->n; k++)
                    c[k] = (struct copy){ part->b[k], dx, dy, win->handle };
                copies_in_backing = 1;
                do_copies(c, part->n);
                copies_in_backing = 0;
                free(c);
            }
            wimp_backing_end(saved);
            for (uint32_t k = 0; k < part->n; k++) {
                binv_subtract(win, part->b[k]);     /* copied: valid */
                wimp_backing_damage(win, part->b[k]);
            }
        }
    } else if (part && inner && dest) {
        wimp_inner(win, APP, inner);
        wimp_rl_copy(part, inner);
        struct wimp_rlist *sh = wimp_rl_new();
        if (sh) {                               /* shadowed pixels are not copied, but redrawn */
            wimp_shadows_above(win, APP, sh);
            wimp_rl_minus(part, sh);
            wimp_rl_free(sh);
        }
        wimp_rl_minus(part, &w->invalid);
        wimp_rl_clip(part, from);
        wimp_rl_translate(part, dx, dy);
        wimp_rl_and(part, inner);
        wimp_rl_copy(dest, inner);              /* make the destination invalid */
        wimp_rl_clip(dest, to);
        wimp_invalidate(dest);
        struct copy *c = calloc(part->n ? part->n : 1, sizeof *c);
        if (c) {
            for (uint32_t k = 0; k < part->n; k++)
                c[k] = (struct copy){ part->b[k], dx, dy, win->handle };
            do_copies(c, part->n);
            free(c);
        }
    }
    if (w->pending_window == win->handle)       /* end any redraw loop on this window */
        w->pending.n = 0;
    wimp_rl_free(dest);
    wimp_rl_free(inner);
    wimp_rl_free(part);
    s->v = 0;
}

/* ---- an icon changed ------------------------------------------------------------------ */

void wimp_icon_changed(struct wimp_window *win, uint32_t i, int in_place)
{
    wimp_flush();
    const uint8_t *icon = wimp_icon(win, i);
    if (!icon || !win->s[APP].open)
        return;
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP), bx[4];
    memcpy(bx, icon, 16);
    struct wimp_box box = { ox + bx[0], oy + bx[1], ox + bx[2], oy + bx[3] };
    struct wimp_rlist *l = wimp_rl_new();
    if (!l)
        return;
    int backed = wimp_backed(win);
    wimp_drawable(win, l);                      /* a surface window's covered parts too */
    wimp_minus_pending(win, l);
    wimp_rl_clip(l, box);
    if (in_place && !backed)
        wimp_shadows_out(win, l);               /* shadowed parts are redrawn, not drawn over */
    if (!in_place) {
        wimp_invalidate_in(win, l);
    } else if (l->n) {
        /* The icons' text is in their owner's memory. Page in the owner's
         * slot, in case another task's thread is drawing. */
        struct wimp_task *ot = (int32_t)win->owner > 0 ? wimp_task_by_handle(win->owner, 0) : NULL;
        const void *was = win->owner == NO_WINDOW ? wimp_menu_page_in() : ros_task_page_in(ot ? ot->rt : NULL);
        wimp_drag_hide();
        /* a surface window's icons are drawn in its backing: on the screen
         * they would be under the presenter's picture of it, and never seen
         * (a Filer icon's selection, a writable icon's typing) */
        uint32_t saved[4];
        int into = 0;
        if (backed) {
            wimp_backing_safety();
            into = wimp_backing_begin(win->surface->backing, win->s[APP].outline.x0, win->s[APP].outline.y0,
                                      saved);
        }
        wimp_vdu5();
        wimp_ecf_origin(ox, oy);
        for (uint32_t k = 0; k < l->n; k++) {
            wimp_graphics_window(l->b[k]);
            wimp_caret_paint(win);
            wimp_draw_icons(win, l->b[k]);
            wimp_caret_paint(win);
            wimp_draw_3d_border(win);
            if (win->owner == NO_WINDOW)
                wimp_menu_separators(win);
            if (into)
                wimp_backing_damage(win, l->b[k]);  /* 5.4: the backing's own damage */
            else
                wimp_damage_add(l->b[k]);       /* block v2: drawn in place */
        }
        wimp_default_windows();
        if (into)
            wimp_backing_end(saved);
        wimp_drag_show();
        ros_task_page_back(was);
    }
    wimp_rl_free(l);
}

/* ---- the redraw scan ------------------------------------------------------------------- */

/* The Wimp redrawing a window itself: its own, or an auto-redraw one */
static void redraw_by_wimp(struct wimp_window *win)
{
    /* A menu's texts belong to its owner, and an auto-redraw window's
     * icons belong to its task. Page in that slot, whichever task is
     * polling. */
    struct wimp_task *ot = (int32_t)win->owner > 0 ? wimp_task_by_handle(win->owner, 0) : NULL;
    const void *was = win->owner == NO_WINDOW ? wimp_menu_page_in() : ros_task_page_in(ot ? ot->rt : NULL);
    int more = start_redraw(win, 0);
    while (more) {
        if (wimp_surface_fills(win))
            wimp_surface_paint(win, wimp_ws()->pending_rect);  /* a surface window */
        more = next_rectangle(win, 0);
    }
    if (win->owner == NO_WINDOW)
        wimp_menu_page_back(was);
    else
        ros_task_page_back(was);
}

/* The Wimp drawing the whole of a window of its own, now */
void wimp_redraw_now(struct wimp_window *win)
{
    redraw_by_wimp(win);
}

/* Invalid area that no window meets is cleared to Wimp colour 4. */
static void clear_uncovered(void)
{
    struct wimp_ws *w = wimp_ws();
    wimp_ecf_origin(0, w->screen_h - w->dy);
    wimp_gcol(wimp_colour(4), 1);
    for (uint32_t i = 0; i < w->invalid.n; i++) {
        wimp_graphics_window(w->invalid.b[i]);
        wimp_clg();
        wimp_shadows_over(NULL, w->invalid.b[i]);
        wimp_damage_add(w->invalid.b[i]);       /* block v2: cleared */
    }
    w->invalid.n = 0;
    wimp_default_windows();
}

/* The first surface window, front to back and children after their
 * parent (a nested surface: a toolbar, a pane, 15 Oct), whose own list has
 * something: that list, less its open children, into l */
static struct wimp_window *binv_next(uint32_t parent, struct wimp_rlist *l)
{
    for (struct wimp_window *x = wimp_stack_front(parent, APP); x; x = wimp_window_below(x, APP)) {
        if (!x->s[APP].open)
            continue;
        if (wimp_backed(x) && x->surface->binv && x->surface->binv->n) {
            binv_screen(x, l);
            wimp_rl_clip(l, x->s[APP].outline);
            for (struct wimp_window *c = wimp_stack_front(x->handle, APP); c; c = wimp_window_below(c, APP))
                if (c->s[APP].open)
                    wimp_rl_subtract(l, c->s[APP].outline);
            x->surface->binv->n = 0;
            if (l->n)
                return x;
        }
        struct wimp_window *c = binv_next(x->handle, l);
        if (c)
            return c;
    }
    return NULL;
}

/* Returns 1 with a Redraw_Window_Request for a task. Returns 0 when there
 * is nothing to redraw, or when the window's owner masks the event. The
 * poll then goes on. */
int wimp_redraw_scan(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    wimp_spin_tick();                           /* the Task Manager's cog, turning */
    for (;;) {
        wimp_flush();
        struct wimp_window *chosen = NULL;
        struct wimp_rlist *l = wimp_rl_new();
        if (!l)
            return 0;
        /* Sprint 4: a surface window whose content changed -- all of what
         * changed, covered or not, for its backing holds all of it.  Not
         * its open children: they are on the screen, drawn for themselves */
        chosen = binv_next(NO_WINDOW, l);

        if (chosen)
            goto dispatch;
        if (!w->invalid.n) {
            wimp_rl_free(l);
            return 0;
        }
        for (struct wimp_window *x = wimp_stack_front(NO_WINDOW, APP); x && !chosen;
             x = wimp_window_below(x, APP)) {
            if (!x->s[APP].open || !wimp_rl_meets(&w->invalid, x->s[APP].outline))
                continue;
            wimp_rl_copy(l, &w->invalid);
            wimp_rl_clip(l, x->s[APP].outline);
            if (wimp_corner_radius(x, APP)) {
                wimp_rl_subtract_notches(l, x, APP);    /* its notches are for what is below */
                if (!l->n)
                    continue;
            }
            chosen = x;
        }
        if (!chosen) {
            wimp_rl_free(l);
            clear_uncovered();
            continue;
        }
        /* Children come first. */
        for (int descended = 1; descended;) {
            descended = 0;
            for (struct wimp_window *c = wimp_stack_front(chosen->handle, APP); c;
                 c = wimp_window_below(c, APP)) {
                if (!c->s[APP].open)
                    continue;
                struct wimp_box co = c->s[APP].outline;
                if (!wimp_rl_meets(l, co))
                    continue;
                wimp_rl_clip(l, co);
                chosen = c;
                descended = 1;
                break;
            }
        }
        if (wimp_backed(chosen)) {
            /* uncovered, or a screen-wide redraw already put in its list: a
             * surface window's backing has it, and the screen memory here is
             * not shown -- nothing to draw */
            for (uint32_t k = 0; k < l->n; k++)
                wimp_screen_holes(l->b[k]);     /* and the framebuffer there transparent */
            wimp_rl_minus(&w->invalid, l);
            wimp_rl_free(l);
            w->uncovered_skipped++;
            continue;
        }
    dispatch:
        if ((int32_t)chosen->owner <= 0 || (chosen->s[APP].flags & FLAG_AUTOREDRAW) || wimp_surface_fills(chosen)) {
            wimp_rl_copy(&w->pending, l);       /* the Wimp redraws it (surface.c
                                                   for a surface window) */
            w->pending_window = chosen->handle;
            wimp_rl_free(l);
            redraw_by_wimp(chosen);
            continue;
        }
        struct wimp_task *t = wimp_task_by_handle(chosen->owner, 0);
        if (!t || (t->mask & (1u << 1))) {     /* masked: nothing this poll */
            wimp_rl_free(l);
            return 0;
        }
        wimp_rl_copy(&w->pending, l);           /* send the task the request */
        w->pending_window = chosen->handle;
        wimp_rl_free(l);
        t->redraws++;
        ev->reason = 1;
        ev->size = 4;
        memcpy(ev->data, &chosen->handle, 4);
        ev->set_r2 = 1;
        ev->r2 = 0;
        *to = t;
        return 1;
    }
}

uint32_t wimp_window_owner_task(uint32_t handle)
{
    struct wimp_window *win = wimp_window(handle);
    return win ? win->owner : NO_WINDOW;
}
