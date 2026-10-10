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

void wimp_invalidate(const struct wimp_rlist *l)
{
    struct wimp_ws *w = wimp_ws();
    wimp_rl_union(&w->invalid, l);
    if (w->invalid.overflow) {                  /* too many boxes: redraw the whole screen */
        w->invalid.n = 0;
        w->invalid.overflow = 0;
        wimp_rl_append(&w->invalid, wimp_screen_box());
    }
}

void wimp_invalidate_box(struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    wimp_rl_add(&w->invalid, b);
}

void wimp_cancel_redraw(void)
{
    struct wimp_ws *w = wimp_ws();
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

static void do_copies(struct copy *c, uint32_t n)
{
    if (!n)
        return;
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
        }
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
    struct wimp_rlist *i0 = wimp_rl_new(), *ov = wimp_rl_new(), *nv = wimp_rl_new();
    struct wimp_rlist *src = wimp_rl_new(), *dst = wimp_rl_new(), *add = wimp_rl_new();
    struct copy *copies = calloc(WIMP_RL_MAX, sizeof *copies);
    if (!i0 || !ov || !nv || !src || !dst || !add || !copies)
        goto out;
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
        if (!ov->n || !nv->n)
            continue;                           /* opened or closed: nothing to copy */
        int linked = ((a->flags ^ r->flags) & (F_FURNITURE | ST_SHIFT_TOGGLED | (1u << 20))) == 0 &&
                     a->vis.x1 - a->vis.x0 == r->vis.x1 - r->vis.x0 &&
                     a->vis.y1 - a->vis.y0 == r->vis.y1 - r->vis.y0 &&
                     a->scx == r->scx && a->scy == r->scy;
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
        wimp_rl_translate(src, dx, dy);
        wimp_rl_and(src, dst);
        for (uint32_t k = 0; k < src->n && ncopies < WIMP_RL_MAX; k++)
            copies[ncopies++] = (struct copy){ src->b[k], dx, dy, win->handle };
    }
    wimp_invalidate(add);
    /* The screen now shows what was asked for. */
    for (uint32_t i = 0; i < WIMP_WINDOWS; i++) {
        struct wimp_window *win = w->win[i];
        if (!win)
            continue;
        win->s[APP] = win->s[REQ];
        win->marked = 0;
    }
    w->top[APP] = w->top[REQ];
    w->any_marked = 0;
    do_copies(copies, ncopies);                 /* then copy the pixels */
out:
    free(copies);
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
        if (win->owner == NO_WINDOW)
            wimp_menu_separators(win);          /* a menu's separator lines */
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
    if (!w->pending_update)
        wimp_rl_subtract(&w->invalid, r);       /* a redraw makes it valid */
    wimp_filter_rect(win->handle, task, r);     /* the rectangle filters */
    wimp_graphics_window(r);                    /* clip to the rectangle */
    if (!w->pending_update && win->def[35] != 0xFF && !win->surface) {   /* surface.c fills it */
        wimp_gcol(wimp_colour(win->def[35]), 1);    /* clear to the background, or tile it */
        if (!wimp_draw_tile(win))
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
    wimp_flush();
    if (w->pending_window != win->handle) {
        struct wimp_rlist *l = wimp_rl_new();
        wimp_visible(win, win->s[APP].outline, APP, l);
        wimp_rl_copy(&w->pending, l);
        wimp_rl_free(l);
    }
    w->pending_window = win->handle;
    w->pending_update = 0;
    w->pending_first = 1;
    wimp_drag_hide();
    wimp_vdu5();
    wimp_ecf_origin(win->s[APP].outline.x0, win->s[APP].outline.y1 - 1);
    /* Draw the border, rectangle by rectangle. */
    wimp_draw_border(win, &w->pending);
    /* The border parts are valid now. */
    struct wimp_rlist *border = wimp_rl_new();
    if (border) {
        wimp_rl_copy(border, &w->pending);
        wimp_rl_subtract(border, win->s[APP].vis);
        wimp_rl_minus(&w->invalid, border);
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
    wimp_inner(win, APP, &w->pending);
    wimp_rl_minus(&w->pending, &w->invalid);
    wimp_rl_clip(&w->pending, box);
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
    struct wimp_ws *w = wimp_ws();
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
            struct wimp_box o = win->s[APP].outline;
            wimp_invalidate_box((struct wimp_box){ o.x0, o.y1 - (w->furn.T + w->dy), o.x1, o.y1 });
        }
        s->r[1] = 0;
        s->v = 0;
        return;
    }
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    box.x0 += ox, box.x1 += ox, box.y0 += oy, box.y1 += oy;
    struct wimp_rlist *l = wimp_rl_new();
    if (l) {
        wimp_inner(win, APP, l);
        wimp_rl_minus(l, &w->invalid);
        wimp_rl_clip(l, box);
        wimp_invalidate(l);
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
    if (part && inner && dest) {
        wimp_inner(win, APP, inner);
        wimp_rl_copy(part, inner);
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
    struct wimp_ws *w = wimp_ws();
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
    wimp_inner(win, APP, l);
    wimp_rl_minus(l, &w->invalid);
    wimp_rl_clip(l, box);
    if (!in_place) {
        wimp_invalidate(l);
    } else if (l->n) {
        /* The icons' text is in their owner's memory. Page in the owner's
         * slot, in case another task's thread is drawing. */
        struct wimp_task *ot = (int32_t)win->owner > 0 ? wimp_task_by_handle(win->owner, 0) : NULL;
        const void *was = win->owner == NO_WINDOW ? wimp_menu_page_in() : ros_task_page_in(ot ? ot->rt : NULL);
        wimp_drag_hide();
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
        }
        wimp_default_windows();
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
        if (win->surface)
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
    }
    w->invalid.n = 0;
    wimp_default_windows();
}

/* Returns 1 with a Redraw_Window_Request for a task. Returns 0 when there
 * is nothing to redraw, or when the window's owner masks the event. The
 * poll then goes on. */
int wimp_redraw_scan(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    for (;;) {
        wimp_flush();
        if (!w->invalid.n)
            return 0;
        struct wimp_window *chosen = NULL;
        struct wimp_rlist *l = wimp_rl_new();
        if (!l)
            return 0;
        for (struct wimp_window *x = wimp_stack_front(NO_WINDOW, APP); x && !chosen;
             x = wimp_window_below(x, APP)) {
            if (!x->s[APP].open || !wimp_rl_meets(&w->invalid, x->s[APP].outline))
                continue;
            chosen = x;
            wimp_rl_copy(l, &w->invalid);
            wimp_rl_clip(l, x->s[APP].outline);
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
        if ((int32_t)chosen->owner <= 0 || (chosen->s[APP].flags & FLAG_AUTOREDRAW) || chosen->surface) {
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
