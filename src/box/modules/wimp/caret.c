/* caret.c -- the caret, the input focus, keys and writable icons, with
 * the exact computations and the drawing.
 *
 * Keys are read at the poll's keys stage.  A key for a writable icon is
 * edited in its owner's memory.  So it is handed to the owner as an
 * internal event, which the owner's own Wimp_Poll carries out
 * (wimp_edit_key).  The runtime pages in only the running task's slot,
 * and so the edit runs where the text is.  The edit then gives
 * Key_Pressed, or nothing. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/ticker.h"
#include "wimp.h"

#define F_PANE      (1u << 5)
#define F_GRABKEYS  (1u << 12)
#define F_FOCUS     (1u << 20)
#define IF_TEXT     (1u << 0)
#define IF_SPRITE   (1u << 1)
#define IF_HCENTRE  (1u << 3)
#define IF_RJUST    (1u << 9)
#define IF_INDIRECT (1u << 8)
#define IF_SHADED   (1u << 22)
#define IF_DELETED  (1u << 23)
#define KEY_MASK    (1u << 8)

static const struct wimp_caretblk none = { 0xFFFFFFFFu, -1, 0, 0, 0xFFFFFFFFu, -1 };

static void put_block(uint8_t *b, const struct wimp_caretblk *c)
{
    uint32_t v[6] = { c->w, (uint32_t)c->i, (uint32_t)c->x, (uint32_t)c->y, c->hf, (uint32_t)c->index };
    memcpy(b, v, 24);
}

static struct wimp_task *owner_task(const struct wimp_window *win)
{
    if (!win || (int32_t)win->owner <= 0)
        return NULL;
    return wimp_task_by_handle(win->owner, 0);
}

/* ---- the focus ---------------------------------------------------- */

static int is_open(const struct wimp_window *win)
{
    for (; win; win = win->s[REQ].parent == NO_WINDOW ? NULL : wimp_window(win->s[REQ].parent))
        if (!win->s[REQ].open)
            return 0;
    return 1;
}

/* The window marked as having the focus when win has the caret: win
 * itself, or the first window below it that is not a pane */
static struct wimp_window *focus_of(struct wimp_window *win)
{
    while (win && (win->s[REQ].flags & F_PANE))
        win = wimp_window_below(win, REQ);
    return win;
}

static void title_redraw(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    if (!win->s[APP].open)
        return;
    struct wimp_box o = win->s[APP].outline;
    wimp_invalidate_box((struct wimp_box){ o.x0, o.y1 - (w->furn.T + w->dy), o.x1, o.y1 });
}

static void mark_focus(struct wimp_window *win, int on)
{
    for (; win; win = win->s[REQ].parent == NO_WINDOW ? NULL : wimp_window(win->s[REQ].parent)) {
        uint32_t before = win->s[REQ].flags;
        for (int st = REQ; st <= APP; st++)
            win->s[st].flags = on ? win->s[st].flags | F_FOCUS : win->s[st].flags & ~F_FOCUS;
        if (before != win->s[REQ].flags && (win->s[REQ].flags & (1u << 26)))
            title_redraw(win);
    }
}

/* ---- the icon's text ------------------------------------------------------------- */

struct itext {
    uint32_t addr, cap, len, flags, validation;
};

static int icon_text(const struct wimp_window *win, int32_t i, struct itext *t)
{
    const uint8_t *icon = wimp_icon(win, (uint32_t)i);
    if (!icon)
        return 0;
    memcpy(&t->flags, icon + 16, 4);
    if (t->flags & IF_INDIRECT) {
        uint32_t d[3];
        memcpy(d, icon + 20, 12);
        t->addr = d[0], t->validation = d[1], t->cap = d[2];
    } else {
        t->addr = ros_addr((void *)(icon + 20)), t->validation = 0, t->cap = 12;
    }
    t->len = 0;
    while (t->len < t->cap && ros_ld8(t->addr + t->len) >= 32)
        t->len++;
    return 1;
}

/* ---- drawing the caret ------------------------------------------ */

/* forcecaret: draws the work area caret in the current graphics window */
void wimp_caret_paint(const struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    const struct wimp_caretblk *c = &w->caret;
    if (c->w != win->handle || c->i != -1)
        return;
    wimp_caret_draw(c, wimp_origin_x(win, APP) + c->x, wimp_origin_y(win, APP) + c->y);
}

/* upcaret: draws or erases a work area caret at once, by EOR.  It is
 * drawn in the visible part of its box that is not already waiting for a
 * redraw. */
static void upcaret(const struct wimp_caretblk *c)
{
    struct wimp_ws *w = wimp_ws();
    if (c->hf & (1u << 25))
        return;
    struct wimp_window *win = wimp_window(c->w);
    if (!win || !win->s[APP].open)
        return;
    wimp_flush();
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    int32_t h = (int32_t)(c->hf & 0xFFFFu);
    struct wimp_box box = { ox + c->x - 3 * w->dx, oy + c->y, ox + c->x + 4 * w->dx, oy + c->y + h + w->dx };
    struct wimp_rlist *l = wimp_rl_new();
    if (!l)
        return;
    wimp_inner(win, APP, l);
    wimp_rl_minus(l, &w->invalid);
    wimp_rl_clip(l, box);
    for (uint32_t k = 0; k < l->n; k++) {
        wimp_graphics_window(l->b[k]);
        wimp_caret_draw(c, ox + c->x, oy + c->y);
    }
    if (l->n)
        wimp_default_windows();
    wimp_rl_free(l);
}

/* int_set_icon_state with nothing changed.  The icon is redrawn, either
 * in place or by invalidating it. */
static void icon_redraw(uint32_t handle, int32_t i)
{
    struct wimp_window *win = wimp_window(handle);
    struct itext t;
    if (!win || i < 0 || !icon_text(win, i, &t))
        return;
    wimp_icon_changed(win, (uint32_t)i, wimp_icon_in_place(win, t.flags));
}

/* ---- Wimp_SetCaretPosition -------------------------------------------------- */

static void send(uint32_t reason, const struct wimp_caretblk *c)
{
    struct wimp_task *t = owner_task(wimp_window(c->w));
    if (!t)
        return;
    uint8_t b[24];
    put_block(b, c);
    wimp_queue_message(reason, b, 24, RECV_TASK, t->handle, 0);
}

static void focus(uint32_t handle, int on)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *f = handle == 0xFFFFFFFFu ? NULL : focus_of(wimp_window(handle));
    if (!f)
        return;
    mark_focus(f, on);
    w->focus = on ? f->handle : 0;
}

static uint32_t redraw_w;
static int32_t redraw_i;

enum { K_MAIN, K_GHOST, K_SEL };

/* int_set_icon_state with nothing changed, for a window's selection icon */
static void sel_icon_redraw(struct wimp_window *win)
{
    if (win && win->sel.icon >= 0)
        icon_redraw(win->handle, win->sel.icon);
}

/* check_shaded_selection_window: the selection in the new caret window
 * is drawn unshaded */
static void sel_shaded_check(uint32_t handle)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = wimp_window(handle);
    if (win && win->sel.icon >= 0) {
        w->selwin = handle;
        sel_icon_redraw(win);
    }
}

/* wscp_remove_current_caret, for the main caret, the ghost caret or a
 * selection.  r1 is the new icon. */
static void remove_current(uint32_t r0, int32_t r1, int kind)
{
    struct wimp_ws *w = wimp_ws();
    redraw_w = 0xFFFFFFFFu, redraw_i = -1;
    w->refresh_main = 0;
    struct wimp_window *nw = r0 != 0xFFFFFFFFu ? wimp_window(r0) : NULL;
    if (kind == K_MAIN) {
        if (nw && nw->sel.icon >= 0 && nw->sel.icon != r1) {     /* another icon's selection goes */
            int32_t old = nw->sel.icon;
            nw->sel.icon = -1;
            icon_redraw(nw->handle, old);
        }
        if (r0 != w->caret.w && w->caret.w != 0xFFFFFFFFu) {    /* the caret leaves its window */
            uint32_t old = w->caret.w;
            focus(old, 0);
            send(11, &w->caret);                /* Lose_Caret */
            struct wimp_window *ow = wimp_window(old);
            if (ow && ow->sel.icon >= 0) {      /* check_unshaded_selection_window */
                w->selwin = 0xFFFFFFFFu;
                sel_icon_redraw(ow);
            }
        }
    }
    if (kind == K_SEL) {                        /* wscp_rcc_selection_redraw */
        if (nw && w->selwin == w->caret.w && w->caret.i == nw->sel.icon)
            w->refresh_main = 1;
        if (w->selwin != 0xFFFFFFFFu && w->selwin != r0) {
            struct wimp_window *sw = wimp_window(w->selwin);
            w->selwin = 0xFFFFFFFFu;
            sel_icon_redraw(sw);                /* now shaded */
        } else if (nw && nw->sel.icon != r1 && nw->sel.icon >= 0) {
            redraw_w = nw->handle, redraw_i = nw->sel.icon;
        }
        return;
    }
    const struct wimp_caretblk *b = kind == K_GHOST ? &w->ghost : &w->caret;
    if (b->w == 0xFFFFFFFFu)
        return;
    if (b->i < 0)
        upcaret(b);                             /* erased now */
    else
        redraw_w = b->w, redraw_i = b->i;       /* always queued, as in 5.30 */
}

/* refreshcaret_main: the main caret's x and y are measured again, and
 * its index is kept */
static void refresh_main_caret(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = w->caret.w != 0xFFFFFFFFu ? wimp_window(w->caret.w) : NULL;
    if (!win || w->caret.i < 0 || w->caret.index < 0)
        return;
    int32_t x, y, cx;
    uint32_t hw;
    if (wimp_caret_coords(win, w->caret.i, (uint32_t)w->caret.index, CC_MAIN, &x, &y, &hw, &cx)) {
        w->caret.x = x - wimp_origin_x(win, APP);
        w->caret.y = y - wimp_origin_y(win, APP);
        w->caretx = cx;
    }
}

static void finish(void)
{
    if (redraw_w != 0xFFFFFFFFu && redraw_i >= 0)
        icon_redraw(redraw_w, redraw_i);
    redraw_w = 0xFFFFFFFFu, redraw_i = -1;
}

/* wscp_clamp_length */
static int32_t clamp_length(const struct itext *t, int32_t index)
{
    int32_t m = (int32_t)t->len;
    if ((int32_t)t->cap - 1 < m)
        m = (int32_t)t->cap - 1;
    return index < m ? index : m;
}

/* int_set_caret_position.  It returns R0 for the exit: an error block if
 * a handle is refused, and otherwise R0 as given.  A removal with "TASK"
 * in R2 clears *r1. */
static uint32_t set_caret_form(uint32_t r0, int32_t r1, int32_t r2, int32_t r3, uint32_t r4, int32_t r5,
                               int32_t r6, int task_word, uint32_t *r1_out);

/* int_set_caret_position, with its exit.  The exit refreshes the main
 * caret and does the queued redraw.  For a selection made in a window
 * without the caret, it then moves the caret to the selection's high
 * end. */
static uint32_t set_caret(uint32_t r0, int32_t r1, int32_t r2, int32_t r3, uint32_t r4, int32_t r5,
                          int32_t r6, int task_word, uint32_t *r1_out)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t ret = set_caret_form(r0, r1, r2, r3, r4, r5, r6, task_word, r1_out);
    if (w->refresh_main == 1)
        refresh_main_caret();
    finish();
    if (w->refresh_main == 2) {
        w->refresh_main = 0;
        set_caret_form(r0, r1, r2, r3, r4 & ~((1u << 31) | (1u << 28)), r6, r6, 0, NULL);
        if (w->refresh_main == 1)
            refresh_main_caret();
        finish();
    }
    w->refresh_main = 0;
    return ret;
}

/* The selection form: a selection from r5 to r6 in icon r1 of window win */
static void selection_form(struct wimp_window *win, uint32_t r0, int32_t r1, int32_t r2, uint32_t r4,
                           int32_t r5, int32_t r6)
{
    struct wimp_ws *w = wimp_ws();
    if ((uint32_t)r1 >= win->nicons)                    /* no such icon */
        return;
    wimp_clipboard_sel_changed(r0);                     /* clipboard_check_current_drag_op */
    if (r5 > r6)
        return;
    struct itext t;
    if (!icon_text(win, r1, &t))
        return;
    if (r5 > 0)                                         /* the indices clamped */
        r5 = clamp_length(&t, r5);
    if (r5 < 0)
        r5 = 0;
    if (r6 != 0)
        r6 = clamp_length(&t, r6);
    if (r5 == r6) {                                     /* empty: the selection removed */
        int32_t old = win->sel.icon;
        win->sel.icon = -1;
        if (w->selwin == win->handle)
            w->selwin = 0xFFFFFFFFu;
        if (w->caret.w == win->handle && w->caret.i == old && old >= 0)
            refresh_main_caret();
        if (old >= 0)
            icon_redraw(win->handle, old);
        return;
    }
    win->sel.checksum = wimp_icon_checksum(win, r1);    /* the new selection */
    win->sel.icon = r1;
    win->sel.low = r5, win->sel.high = r6;
    int32_t x, y, cx;
    uint32_t hw;
    if (wimp_caret_coords(win, r1, (uint32_t)r5, CC_SEL_LOW, &x, &y, &hw, &cx)) {
        win->sel.xoff = cx;
        win->sel.yoff = y - wimp_origin_y(win, APP);
        win->sel.flags = (r4 & 0xFFFF0000u) | hw;
    }
    if (wimp_caret_coords(win, r1, (uint32_t)r6, CC_SEL_HIGH, &x, &y, &hw, &cx))
        win->sel.width = cx - win->sel.xoff;
    win->sel.xoverride = (r4 & (1u << 28)) ? r2 : SEL_BIGNUM;           /* bit 28: the scroll in R2 */
    if (w->caret.w != r0)                               /* the caret follows, at the exit */
        w->refresh_main = 2;
    w->selwin = r0;                                     /* this window has the selection */
    icon_redraw(r0, r1);
}

static uint32_t set_caret_form(uint32_t r0, int32_t r1, int32_t r2, int32_t r3, uint32_t r4, int32_t r5,
                               int32_t r6, int task_word, uint32_t *r1_out)
{
    struct wimp_ws *w = wimp_ws();
    if (r5 == -1 && r1 != -1)                   /* R5 = -1 with an icon ignores R4 */
        r4 = 0xFFFFFFFFu;
    if (r5 == -2)
        r5 = -1;
    if (w->caret.w != 0xFFFFFFFFu && !wimp_window(w->caret.w))
        w->caret.w = 0xFFFFFFFFu;
    if (w->ghost.w != 0xFFFFFFFFu && !wimp_window(w->ghost.w))
        w->ghost.w = 0xFFFFFFFFu;
    if (r4 == 0xFFFFFFFEu)
        r4 = 0xFFFFFFFFu;
    struct wimp_window *win = NULL;
    os_error *err = NULL;
    if (r0 != 0 && r0 != 0xFFFFFFFFu) {
        win = wimp_window(r0);
        if (!win)
            err = wimp_error(E_BAD_HANDLE);
        else if (!is_open(win) || !focus_of(win))
            err = ros_error(0x287, "Input focus window not found");
    }
    int selection = r4 != 0xFFFFFFFFu && (r4 & (1u << 31));
    int ghost = !selection && r4 != 0xFFFFFFFFu && (r4 & (1u << 30));
    int kind = selection ? K_SEL : ghost ? K_GHOST : K_MAIN;
    if (!win) {                                 /* a removal */
        if (r0 == 0 || r0 == 0xFFFFFFFFu) {
            if (!task_word)
                r4 = 0;
            else if (r1_out)
                *r1_out = 0;
            selection = r4 != 0xFFFFFFFFu && (r4 & (1u << 31));
            ghost = !selection && r4 != 0xFFFFFFFFu && (r4 & (1u << 30));
            kind = selection ? K_SEL : ghost ? K_GHOST : K_MAIN;
        }
        remove_current(0xFFFFFFFFu, r1, kind);
        if (kind == K_GHOST) {
            w->ghost.w = 0xFFFFFFFFu;
        } else if (kind == K_MAIN) {
            w->caret.w = 0xFFFFFFFFu;
        } else if (w->selwin != 0xFFFFFFFFu) {  /* a selection: no redraw follows */
            wimp_clipboard_sel_changed(w->selwin);
            struct wimp_window *sw = wimp_window(w->selwin);
            if (sw)
                sw->sel.icon = -1;
            w->selwin = 0xFFFFFFFFu;
        }
        return err ? ros_addr(err) : r0;
    }
    if (selection) {                            /* the selection form */
        remove_current(r0, r1, K_SEL);
        selection_form(win, r0, r1, r2, r4, r5, r6);
        return r0;
    }
    struct wimp_caretblk *block = ghost ? &w->ghost : &w->caret;
    uint32_t old_window = block->w;
    remove_current(r0, r1, kind);
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    if (r1 < 0) {                               /* the work area */
        struct wimp_caretblk c = { r0, r1, r2, r3, r4, r5 };
        upcaret(&c);
        if (c.hf == 0xFFFFFFFFu)
            c.hf = 0x02000000u;
        *block = c;
        if (!ghost && r0 != old_window) {
            send(12, &w->caret);                /* Gain_Caret */
            sel_shaded_check(r0);
            focus(r0, 1);
        }
        return r0;
    }
    struct itext t;                             /* an icon */
    if ((uint32_t)r1 >= win->nicons || !icon_text(win, r1, &t)) {
        block->w = 0xFFFFFFFFu;                 /* no such icon: a removal (5.30 differs) */
        return r0;
    }
    if (r5 > 0)
        r5 = clamp_length(&t, r5);
    int dest = ghost ? CC_GHOST : CC_MAIN;
    int32_t x, y, cx;
    uint32_t hword, index;
    if (r4 != 0xFFFFFFFFu && r5 >= 0) {
        wimp_caret_coords(win, r1, (uint32_t)r5, dest, &x, &y, &hword, &cx);
        r4 = (r4 & 0xFFFF0000u) | (hword & 0xFFFFu);
    } else if (r5 < 0) {
        if (r2 > 0x10000) r2 = 0x10000;
        if (r3 > 0x10000) r3 = 0x10000;
        wimp_caret_find(win, r1, ox + r2, oy + r3, dest, &x, &y, &hword, &index, &cx);
        r5 = (int32_t)index;
        r4 = r4 == 0xFFFFFFFFu ? (hword & 0xFFFFu) : (r4 & 0xFFFF0000u) | (hword & 0xFFFFu);
    } else {
        wimp_caret_coords(win, r1, (uint32_t)r5, dest, &x, &y, &hword, &cx);
        r4 = hword;
    }
    struct wimp_caretblk c = { r0, r1, x - ox, y - oy, r4, r5 };
    if (ghost) {
        if (win->sel.icon == r1 && win->sel.low <= r5 && r5 <= win->sel.high)
            c.hf |= 1u << 25;                   /* invisible inside the selection */
        w->ghostcaretx = cx;
        *block = c;
        if (w->caret.w == r0 && w->caret.i == r1) {
            refresh_main_caret();               /* refreshcaret_ghost_main */
            w->refresh_main = 0;
        }
    } else {
        w->caretx = cx;
        *block = c;
        if (r0 != old_window) {
            sel_shaded_check(r0);
            send(12, &w->caret);
            focus(r0, 1);
        }
    }
    icon_redraw(r0, r1);                        /* redrawn to show the new caret */
    return r0;
}

/* Pages in the task whose memory holds a window's icon text.  This is
 * the window's owner, or the menu's owner for a menu.  A caret may be
 * set from another thread, and 5.30 pages the owner in for it.  The
 * Clipboard Manager's paste is one such caller. */
static const struct ros_slot *page_owner(uint32_t handle)
{
    struct wimp_window *win = handle && handle != 0xFFFFFFFFu ? wimp_window(handle) : NULL;
    struct wimp_task *t = !win ? NULL : (int32_t)win->owner > 0 ? wimp_task_by_handle(win->owner, 0)
                                                               : win->owner == NO_WINDOW ? wimp_menu_owner() : NULL;
    return ros_task_page_in(t ? t->rt : NULL);
}

void wimp_swi_SetCaretPosition(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t r4 = s->r[4] == 0xFFFFFFFEu ? 0xFFFFFFFFu : s->r[4];
    if (w->in.d.type == 13 && (r4 == 0xFFFFFFFFu || !(r4 & ((1u << 30) | (1u << 31)))) &&
        (s->r[0] != w->caret.w || (int32_t)s->r[1] != w->caret.i))
        wimp_drag_selection_cancel();           /* the caret moves: a type 13 drag ends */
    const struct ros_slot *was = page_owner(s->r[0]);
    uint32_t r1 = s->r[1];
    s->r[0] = set_caret(s->r[0], (int32_t)s->r[1], (int32_t)s->r[2], (int32_t)s->r[3], s->r[4],
                        (int32_t)s->r[5], (int32_t)s->r[6], s->r[2] == TASK_WORD, &r1);
    s->r[1] = r1;
    s->v = 0;
    ros_task_page_back(was);
}

/* ---- selection by the mouse ------------------------------------- */

static uint32_t find_cmd(uint32_t v, uint32_t letter);
static uint32_t skipword_r(uint32_t a, uint32_t i);
static uint32_t skipword_l(uint32_t a, uint32_t i);

static uint32_t rd(const uint8_t *b, unsigned off)
{
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

static uint8_t cmos_byte(uint32_t addr)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 161, c.r[1] = addr;
    ros_swi(&c, XOS_Byte);
    return (uint8_t)c.r[2];
}

/* what the next drag on a type 15 icon does (cnp_pending_dragtype) */
enum { CNP_NONE, CNP_CHAR, CNP_WORD, CNP_DRAGDROP, CNP_HIGH, CNP_LOW };

#define CRF_NOCENTRE (1u << 28)
#define CRF_SEL      (1u << 31)

/* int_set_caret_position, with the owner's text paged in */
static void int_set_caret(uint32_t r0, int32_t r1, int32_t r2, int32_t r3, uint32_t r4, int32_t r5, int32_t r6)
{
    const struct ros_slot *was = page_owner(r0);
    set_caret(r0, r1, r2, r3, r4, r5, r6, 0, NULL);
    ros_task_page_back(was);
}

/* findclickindex: the text index under screen point x, y, for the icon
 * as it is drawn now.  The owner must be paged in. */
static uint32_t click_index(struct wimp_window *win, int32_t icon, int32_t x, int32_t y)
{
    int32_t cx, cy, ccx;
    uint32_t hw, index = 0;
    wimp_caret_find(win, icon, x, y, CC_NONE, &cx, &cy, &hw, &index, &ccx);
    return index;
}

/* cnp_get_word_offsets: the word around index i */
static void word_around(uint32_t a, uint32_t i, int32_t *lo, int32_t *hi)
{
    int32_t e = (int32_t)skipword_r(a, i) - 1;
    *hi = e < 0 ? 0 : ros_ld8(a + (uint32_t)e) == ' ' ? e : e + 1;
    *lo = (int32_t)skipword_l(a, i);
}

/* mouseaction_cnpwriteable: interprets the report for a type 15 icon.
 * The buttons are those that the report would give: &400 Select and &100
 * Adjust, &004 and &001 the same buttons double-clicked, and &040 and
 * &010 the same buttons dragged. */
void wimp_cnp_click(struct wimp_window *win, int32_t icon, uint32_t buttons, int32_t mx, int32_t my, int clicks)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t h = win->handle;
    int32_t rx = mx - wimp_origin_x(win, APP), ry = my - wimp_origin_y(win, APP);
    switch (buttons) {
    case 0x400: {                               /* Select: the caret goes to the click */
        int_set_caret(h, icon, rx, ry, 0xFFFFFFFFu, -1, 0);
        if (!wimp_window(h))
            return;
        if (w->caret.i == win->sel.icon) {
            if (w->caret.index >= win->sel.low && w->caret.index < win->sel.high) {
                w->cnp_drag = CNP_DRAGDROP;     /* inside the selection: a drag takes it out */
                return;
            }
            int_set_caret(w->caret.w, w->caret.i, 0, 0, CRF_SEL, 0, 0);
        }
        w->cnp_drag = CNP_CHAR;
        return;
    }
    case 0x004: {                               /* Select twice: the word.  Three times: all */
        if (clicks == 3) {
            w->cnp_drag = CNP_DRAGDROP;
            int_set_caret(h, icon, 0, 0, CRF_SEL, 0, SEL_BIGNUM);
            return;
        }
        w->cnp_drag = CNP_WORD;
        const struct ros_slot *was = page_owner(h);
        struct itext t;
        int32_t lo = 0, hi = 0;
        if (icon_text(win, icon, &t))
            word_around(t.addr, w->caret.index < 0 ? 0 : (uint32_t)w->caret.index, &lo, &hi);
        ros_task_page_back(was);
        int_set_caret(h, icon, 0, 0, CRF_SEL, lo, hi);
        return;
    }
    case 0x100: case 0x001: {                   /* Adjust: the selection extends to the click */
        if (w->caret.w != h || w->caret.i != icon) {
            int_set_caret(h, icon, rx, ry, 0xFFFFFFFFu, -1, 0);
            return;
        }
        const struct ros_slot *was = page_owner(h);
        int32_t at = (int32_t)click_index(win, icon, mx, my);
        ros_task_page_back(was);
        int32_t lo, hi;
        if (win->sel.icon == w->caret.i) {      /* the nearer end moves */
            lo = win->sel.low, hi = win->sel.high;
            int32_t mid = lo + (int32_t)((uint32_t)(hi - lo) >> 1);
            if (mid <= at) {
                hi = at;
                w->cnp_drag = CNP_HIGH;
            } else {
                lo = at;
                w->cnp_drag = CNP_LOW;
            }
        } else {                                /* from the caret to the click */
            int32_t ci = w->caret.index;
            if (ci <= at) {
                lo = ci, hi = at;
                w->cnp_drag = CNP_HIGH;
            } else {
                lo = at, hi = ci;
                w->cnp_drag = CNP_LOW;
            }
        }
        int_set_caret(w->caret.w, w->caret.i, 0, 0, CRF_SEL, lo, hi);
        return;
    }
    case 0x040:                                 /* Select dragged */
        if (w->cnp_drag == CNP_DRAGDROP) {
            w->cnp_drag = CNP_NONE;
            struct itext t;                     /* a password is not dragged out */
            const struct ros_slot *was = page_owner(h);
            int pw = icon_text(win, icon, &t) && (t.flags & IF_INDIRECT) && (int32_t)t.validation > 0 &&
                     find_cmd(t.validation, 'D');
            ros_task_page_back(was);
            if (!pw)
                wimp_clipboard_request(CB_PW_DRAGSTART);
            return;
        }
        wimp_drag_selection();
        return;
    case 0x010:                                 /* Adjust dragged */
        wimp_drag_selection();
        return;
    }
}

/* mf_writeablerelease: the buttons are released over a type 15 icon.  A
 * single click inside the selection, with no drag, clears the selection. */
void wimp_cnp_release(struct wimp_window *win, int32_t icon, int clicks)
{
    struct wimp_ws *w = wimp_ws();
    if (clicks != 1 || w->cnp_drag != CNP_DRAGDROP)
        return;
    w->cnp_drag = CNP_NONE;
    int_set_caret(win->handle, icon, 0, 0, CRF_SEL, 0, 0);
}

/* The Wimp's side of the end of a type 13 drag.  The held scroll is
 * released and icon autoscrolling stops.  input.c deals with the pointer
 * box and the drag type. */
void wimp_cnp_drag_end(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = wimp_window(w->caret.w);
    if (win)
        win->sel.xoverride = SEL_BIGNUM;
    wimp_iconscroll_stop();
}

/* dragging_iconselection: a poll with the button held.  Returns 0 if the
 * drag goes on.  Returns 1 if it ended because all buttons are up, and
 * the poll then returns a null event.  Returns 2 if it ended with nothing
 * to do. */
int wimp_cnp_drag_step(int32_t mx, int32_t my, uint32_t mb)
{
    struct wimp_ws *w = wimp_ws();
    if (!mb)
        return 1;
    struct wimp_window *win = wimp_window(w->caret.w);
    if (!win)
        return 2;
    int32_t icon = w->caret.i, ci = w->caret.index, lo, hi;
    const struct ros_slot *was = page_owner(win->handle);
    int32_t at = (int32_t)click_index(win, icon, mx, my);
    switch (w->cnp_drag) {
    case CNP_CHAR:                              /* from the caret */
        lo = at < ci ? at : ci;
        hi = at < ci ? ci : at;
        break;
    case CNP_WORD: {                            /* whole words, from the caret's word */
        struct itext t;
        if (!icon_text(win, icon, &t)) {
            ros_task_page_back(was);
            return 0;
        }
        if (at < ci) {
            lo = (int32_t)skipword_l(t.addr, (uint32_t)at);
            hi = (int32_t)skipword_r(t.addr, (uint32_t)ci);
        } else {
            hi = (int32_t)skipword_r(t.addr, (uint32_t)at);
            lo = (int32_t)skipword_l(t.addr, ci < 0 ? 0 : (uint32_t)ci);
        }
        break;
    }
    case CNP_HIGH:                              /* the high end moves, swapping past the low */
        if (win->sel.low <= at) {
            lo = win->sel.low, hi = at;
        } else {
            lo = at, hi = win->sel.low;
            w->cnp_drag = CNP_LOW;
        }
        break;
    case CNP_LOW:
        if (at > win->sel.high) {
            lo = win->sel.high, hi = at;
            w->cnp_drag = CNP_HIGH;
        } else {
            lo = at, hi = win->sel.high;
        }
        break;
    default:
        ros_task_page_back(was);
        return 2;
    }
    ros_task_page_back(was);
    int_set_caret(win->handle, icon, win->sel.xoverride, 0, CRF_SEL | CRF_NOCENTRE, lo, hi);
    return 0;
}

/* ---- icon autoscrolling ------------------------------------------------------ */

enum { IS_SEL = 1, IS_GHOST = 2, IS_ON = 3, IS_LEFT = 4, IS_RIGHT = 8, IS_PAUSE_L = 16, IS_PAUSE_R = 32,
       IS_PTR = 128 };

static uint32_t select_pointer(uint32_t n)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 106, c.r[1] = n;
    ros_swi(&c, XOS_Byte);
    return c.r[1];
}

/* iconautoscroll_set_limits: how far the text may scroll */
static void is_limits(int32_t tw, int32_t iw, uint32_t flags)
{
    struct wimp_iconscroll *is = &wimp_ws()->is;
    int32_t r = tw - iw + 16;
    if (!(flags & IF_SPRITE)) {
        is->max = r, is->min = 0;
    } else if (!(flags & IF_HCENTRE)) {
        is->min = -r, is->max = 0;
    } else if (!(flags & IF_RJUST)) {
        is->max = (int32_t)((uint32_t)r >> 1), is->min = -is->max;
    } else {
        is->max = r, is->min = 0;
    }
}

/* iconautoscroll_start: starts for the icon under the pointer, if it is
 * type 15 and its text is wider than the icon.  For a selection the
 * scroll is held, so that the text stays where it is. */
void wimp_iconscroll_start(int selection)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_iconscroll *is = &w->is;
    is->state = selection ? IS_SEL : IS_GHOST;
    int32_t icon;
    struct wimp_window *win = wimp_hit(w->in.mx, w->in.my, 0, &icon);
    if (!win || (int32_t)wimp_reported(win) < 0 || icon < 0 ||
        ((rd(wimp_icon(win, (uint32_t)icon), 16) >> 12) & 15) != 15) {
        is->state = 0;
        return;
    }
    const struct ros_slot *was = page_owner(win->handle);
    is->win = win->handle, is->icon = icon;
    int32_t tw, iw, hold;
    uint32_t flags;
    if (!wimp_sel_hold(win, icon, 0, &tw, &iw, &flags, &hold)) {
        ros_task_page_back(was);
        is->win = 0xFFFFFFFFu, is->icon = -1;
        is->state = 0;
        return;
    }
    is_limits(tw, iw, flags);
    is->next = 0;
    ros_task_page_back(was);
    if (win->sel.icon >= 0 && win->sel.icon != icon)
        int_set_caret(win->handle, icon, 0, 0, CRF_SEL, 0, 0);    /* a selection in another icon goes */
    was = page_owner(win->handle);
    if (wimp_sel_hold(win, icon, 0, &tw, &iw, &flags, &hold))
        win->sel.xoverride = hold;
    ros_task_page_back(was);
}

/* iconautoscroll_swi: Wimp_AutoScroll with bit 3 set, for a ghost caret
 * in a writable icon.  R1 points to the window and icon handles.  Bit 0
 * clear stops it.  R0 is returned unchanged. */
void wimp_iconscroll_swi(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_iconscroll *is = &w->is;
    s->v = 0;
    if (!(s->r[0] & 1u)) {
        wimp_iconscroll_stop();
        return;
    }
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint32_t handle = ros_ld32(s->r[1]);
    int32_t icon = (int32_t)ros_ld32(s->r[1] + 4);
    is->win = handle, is->icon = icon;
    struct wimp_window *win = wimp_window(handle);
    if (!win) {
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
        return;
    }
    if ((uint32_t)icon >= win->nicons)
        return;
    uint32_t type = (rd(wimp_icon(win, (uint32_t)icon), 16) >> 12) & 15;
    if (type != 14 && type != 15)
        return;
    const struct ros_slot *was = page_owner(handle);
    int32_t tw, iw, hold;
    uint32_t flags;
    if (!wimp_sel_hold(win, icon, 1, &tw, &iw, &flags, &hold)) {
        ros_task_page_back(was);
        is->win = 0xFFFFFFFFu, is->icon = -1;
        return;
    }
    is_limits(tw, iw, flags);
    is->next = 0;
    is->state = IS_GHOST;
    w->ghost_xoverride = hold;
    ros_task_page_back(was);
}

/* iconautoscroll_stop: the pointer is restored, the held scrolls are
 * released, and the caret is placed again so that the text scrolls to it */
void wimp_iconscroll_stop(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_iconscroll *is = &w->is;
    if (is->state & IS_PTR)
        select_pointer(1);
    is->state = 0;
    if (is->icon < 0)
        return;
    w->ghost_xoverride = SEL_BIGNUM;
    struct wimp_window *win = wimp_window(is->win);
    if (win)
        win->sel.xoverride = SEL_BIGNUM;
    struct wimp_caretblk c = w->caret;
    int_set_caret(c.w, c.i, c.x, c.y, c.hf, c.index, 0);
}

/* poll_iconautoscroll: runs on each poll while icon autoscrolling is on.
 * The pause zones are a quarter of the icon's width at each side.  When
 * the pointer is in one, after the pause (there is none for a selection),
 * the text scrolls every 8 cs by how far the pointer is into the zone.
 * The pointer is then ptr_autoscrh. */
void wimp_iconscroll_poll(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_iconscroll *is = &w->is;
    int32_t icon;
    struct wimp_window *win = wimp_hit(w->in.mx, w->in.my, 0, &icon);
    if (!win || win->handle != is->win || icon != is->icon) {
        wimp_iconscroll_stop();
        return;
    }
    uint32_t st = is->state & ~(uint32_t)(IS_PAUSE_L | IS_PAUSE_R);
    const uint8_t *ic = wimp_icon(win, (uint32_t)icon);
    int32_t x = w->in.mx - wimp_origin_x(win, APP);
    int32_t x0 = (int32_t)rd(ic, 0), x1 = (int32_t)rd(ic, 8);
    int32_t q = (int32_t)((uint32_t)(x1 - x0) >> 2), by = 0;
    int zone = 1;
    if (x <= x0 + q) {
        st |= IS_PAUSE_L;
        by = x0 + q - x;
    } else if (x < x1 - q) {
        st &= ~(uint32_t)(IS_LEFT | IS_RIGHT);
        zone = 0;
    } else {
        st |= IS_PAUSE_R;
        by = x1 - q - x;
    }
    if (zone) {
        if (((st & IS_PAUSE_L) && (st & IS_LEFT)) || ((st & IS_PAUSE_R) && (st & IS_RIGHT))) {
            uint32_t now = ros_monotonic_cs();
            if ((int32_t)(now - is->next) >= 0) {
                is->next = now + 8;
                int32_t *v = (st & IS_GHOST) ? &w->ghost_xoverride : &win->sel.xoverride;
                int32_t n = *v + by;
                if (n < is->min) n = is->min;
                if (n > is->max) n = is->max;
                *v = n;
                icon_redraw(win->handle, icon);
            }
        } else {
            st &= ~(uint32_t)(IS_LEFT | IS_RIGHT);
            uint32_t now = ros_monotonic_cs();
            int32_t prev = is->prevx;
            is->prevx = x;
            if (x != prev) {                    /* moved: the pause starts again */
                uint32_t pause = 0;
                if (!(st & IS_SEL)) {
                    uint8_t dd = cmos_byte(0xDD), de = cmos_byte(0xDE);
                    pause = (((uint32_t)dd >> 4) ^ 5u) * ((de & 2u) ? 10u : 1u) * 10u;
                }
                is->next = now + pause;
            } else if ((int32_t)(now - is->next) >= 0) {
                st |= (st & IS_PAUSE_L) ? IS_LEFT : IS_RIGHT;
            }
            is->state = st;
            return;
        }
    }
    if (is->state & IS_PTR) {                   /* out of the zones: the pointer restored */
        if (st & (IS_PAUSE_L | IS_PAUSE_R))
            return;
        is->state = st & ~(uint32_t)IS_PTR;
        select_pointer(is->oldptr);
        return;
    }
    if (!(st & (IS_PAUSE_L | IS_PAUSE_R))) {
        is->state = st;
        return;
    }
    is->state = st | IS_PTR;
    is->oldptr = select_pointer(127) & 15u;
    char *name = (char *)w->scratch + 448;
    strcpy(name, "ptr_autoscrh");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 40, c.r[2] = ros_addr(name);
    ros_swi(&c, XWimp_SpriteOp);
    uint32_t ax = c.r[3] >> 1, ay = c.r[4] >> 1;
    ros_cpu_enter(&c);
    c.r[0] = 36, c.r[2] = ros_addr(name), c.r[3] = 4, c.r[4] = ax, c.r[5] = ay, c.r[6] = c.r[7] = 0;
    ros_swi(&c, XWimp_SpriteOp);
}

/* ---- Wimp_GetCaretPosition ---------------------------------------------------- */

void wimp_swi_GetCaretPosition(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint8_t b[24];
    s->v = 0;
    if (s->r[2] == TASK_WORD) {
        s->r[2] = 0;
        if (s->r[0] == 1) {
            put_block(b, &w->ghost);
            memcpy(ros_ptr(s->r[1]), b, 24);
        } else if (s->r[0] == 2) {              /* R0 = 2: a window's selection */
            uint32_t h = s->r[3] == 0xFFFFFFFFu ? w->selwin : s->r[3];
            if (h == 0xFFFFFFFFu) {
                ros_st32(s->r[1], 0xFFFFFFFFu);
                return;
            }
            struct wimp_window *win = wimp_window_ib(h);
            if (!win) {
                wimp_fail(s, wimp_error(E_BAD_HANDLE));
                return;
            }
            const struct wimp_sel *sel = &win->sel;
            uint32_t v[8] = { sel->icon < 0 ? 0xFFFFFFFFu : win->handle, (uint32_t)sel->icon,
                              (uint32_t)sel->xoff, (uint32_t)sel->width, (uint32_t)sel->yoff,
                              sel->flags, (uint32_t)sel->low, (uint32_t)sel->high };
            memcpy(ros_ptr(s->r[1]), v, 32);
        } else if (s->r[0] == 0) {
            put_block(b, &w->caret);
            memcpy(ros_ptr(s->r[1]), b, 24);
        }
        return;
    }
    put_block(b, &w->caret);
    memcpy(ros_ptr(s->r[1]), b, 24);
}

/* At the first task's start: no caret, no focus and no keys waiting */
void wimp_caret_reset(void)
{
    struct wimp_ws *w = wimp_ws();
    w->caret = w->ghost = w->saved = none;
    w->cnp_drag = 0;
    w->is = (struct wimp_iconscroll){ 0, 0xFFFFFFFFu, -1, 0, 0, 0, 0, 0 };
    w->ghost_xoverride = SEL_BIGNUM;
    w->focus = 0;
    w->hotkeyptr = 0;
    w->ninj = w->nexp = w->pexp = 0;
}

/* For the menu code: int_set_caret_position with a whole caret block,
 * and nocaret for a dialogue box that is closed */
void wimp_caret_set(const struct wimp_caretblk *c)
{
    set_caret(c->w, c->i, c->x, c->y, c->hf, c->index, 0, 0, NULL);
}

static int is_parent_of(uint32_t parent, uint32_t handle)
{
    for (struct wimp_window *win = handle == 0xFFFFFFFFu ? NULL : wimp_window(handle); win;
         win = win->s[REQ].parent == NO_WINDOW ? NULL : wimp_window(win->s[REQ].parent))
        if (win->handle == parent)
            return 1;
    return 0;
}

void wimp_caret_nocaret(uint32_t handle)
{
    struct wimp_ws *w = wimp_ws();
    if (is_parent_of(handle, w->ghost.w))
        w->ghost = none;
    if (is_parent_of(handle, w->caret.w)) {
        focus(w->caret.w, 0);
        w->caret = none;
    }
}

/* A window is going: its caret is turned off without events */
void wimp_caret_window_gone(uint32_t handle)
{
    struct wimp_ws *w = wimp_ws();
    if (w->caret.w == handle)
        w->caret = none;
    if (w->ghost.w == handle)
        w->ghost = none;
    if (w->focus == handle)
        w->focus = 0;
    if ((int32_t)w->hotkeyptr > 0 && w->hotkeyptr == handle)
        w->hotkeyptr = 0;
}

/* ---- the hot-key chain ------------------------------------------------------- */

static struct wimp_window *parent_of(const struct wimp_window *win)
{
    return win->s[APP].parent == NO_WINDOW ? NULL : wimp_window(win->s[APP].parent);
}

/* The next window in the walk: the children from front to back, then the
 * next sibling down, then the parent's next sibling, and so on */
static struct wimp_window *walk_next(const struct wimp_window *win)
{
    struct wimp_window *c = wimp_stack_front(win->handle, APP);
    if (c)
        return c;
    for (; win; win = parent_of(win)) {
        struct wimp_window *below = wimp_window_below(win, APP);
        if (below)
            return below;
    }
    return NULL;
}

static void default_action(uint32_t code);

static void hotkey(uint32_t code)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = w->hotkeyptr && (int32_t)w->hotkeyptr > 0 ? wimp_window(w->hotkeyptr) : NULL;
    if (!win || !win->s[APP].open)
        win = wimp_stack_front(NO_WINDOW, APP);
    for (; win; win = walk_next(win)) {
        if (!win->s[APP].open || !(win->s[APP].flags & F_GRABKEYS))
            continue;
        struct wimp_task *t = owner_task(win);
        if (!t)
            continue;
        uint8_t b[28];
        put_block(b, &w->saved);
        memcpy(b + 24, &code, 4);
        wimp_queue_message(8, b, 28, RECV_TASK, t->handle, 0);
        struct wimp_window *next = walk_next(win);
        w->hotkeyptr = next ? next->handle : 0xFFFFFFFFu;
        return;
    }
    w->hotkeyptr = 0;
    default_action(code);
}

/* The default action, when no hot-key window takes a key */
static void default_action(uint32_t code)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t n;
    if (code >= 0x180 && code <= 0x189)
        n = code - 0x180;
    else if (code >= 0x1CA && code <= 0x1CC)
        n = code - 0x1CA + 10;
    else
        return;                                 /* Shift-F12's iconbar toggle is not done */
    struct wimp_window *win = w->caret.w != 0xFFFFFFFFu ? wimp_window(w->caret.w) : NULL;
    if (!win || w->caret.i < 0)
        return;
    struct itext t;
    if (!icon_text(win, w->caret.i, &t))
        return;
    char *name = (char *)w->scratch, *val = (char *)w->scratch + 32;
    name[0] = 'K', name[1] = 'e', name[2] = 'y', name[3] = '$';
    if (n >= 10) {
        name[4] = '1', name[5] = (char)('0' + n - 10), name[6] = 0;
    } else {
        name[4] = (char)('0' + n), name[5] = 0;
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(name), c.r[1] = ros_addr(val), c.r[2] = 256 - 32 - 32, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    if (c.v)
        return;
    w->nexp = w->pexp = 0;
    for (uint32_t i = 0; i < c.r[2] && w->nexp + 2 <= sizeof w->exp; i++) {
        w->exp[w->nexp++] = (uint8_t)val[i];
        if (val[i] == 0)
            w->exp[w->nexp++] = 0;
    }
}

/* ---- Wimp_ProcessKey ------------------------------------------------------------ */

void wimp_swi_ProcessKey(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t code = s->r[0];
    s->v = 0;
    if (w->singletask >= 0) {                   /* single-tasking: the default action */
        default_action(code);
        return;
    }
    if ((int32_t)w->hotkeyptr > 0) {
        hotkey(code);
        return;
    }
    if (w->hotkeyptr == 0xFFFFFFFFu) {
        default_action(code);
        return;
    }
    if (code == 0 || code >= 0x100) {
        if (w->ninj + 2 > sizeof w->inj)
            return;
        w->inj[w->ninj++] = 0;
        w->inj[w->ninj++] = (uint8_t)code;
    } else {
        if (w->ninj + 1 > sizeof w->inj)
            return;
        w->inj[w->ninj++] = (uint8_t)code;
    }
}

/* ---- reading keys ------------------------------------------------------------------ */

static int take(uint8_t *buf, uint32_t *n, uint32_t *p, uint32_t *code)
{
    if (*p >= *n) {
        *n = *p = 0;
        return 0;
    }
    uint32_t b = buf[(*p)++];
    if (b == 0) {
        if (*p >= *n) {                         /* a lone 0: dropped */
            *n = *p = 0;
            return 0;
        }
        uint32_t b2 = buf[(*p)++];
        *code = b2 ? 0x100 + b2 : 0;
    } else {
        *code = b;
    }
    if (*p >= *n)
        *n = *p = 0;
    return 1;
}

static int keyboard(uint32_t *code)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 129, c.r[1] = 0, c.r[2] = 0;
    ros_swi(&c, XOS_Byte);
    if (c.v || c.c || c.r[2] != 0)
        return 0;
    uint32_t b = c.r[1] & 0xFFu;
    if (b != 0) {
        *code = b;
        return 1;
    }
    ros_cpu_enter(&c);
    c.r[0] = 129, c.r[1] = 0, c.r[2] = 0;
    ros_swi(&c, XOS_Byte);
    if (c.v || c.c || c.r[2] != 0)
        return 0;                               /* a lone 0 */
    b = c.r[1] & 0xFFu;
    *code = b ? 0x100 + b : 0;
    return 1;
}

static int next_key(uint32_t *code)
{
    struct wimp_ws *w = wimp_ws();
    if (take(w->exp, &w->nexp, &w->pexp, code))
        return 1;
    uint32_t p = 0;
    if (w->ninj) {
        int got = take(w->inj, &w->ninj, &p, code);
        if (got && w->ninj) {
            memmove(w->inj, w->inj + p, w->ninj - p);
            w->ninj -= p;
        }
        if (got)
            return 1;
    }
    return keyboard(code);
}

/* Key_Pressed for the caret's owner.  It starts the hot-key chain at the
 * top of the stack. */
static void key_pressed(struct wimp_event *ev, uint32_t code)
{
    struct wimp_ws *w = wimp_ws();
    if (w->singletask < 0) {                    /* the hot-key chain only when multitasking */
        struct wimp_window *top = wimp_stack_front(NO_WINDOW, APP);
        w->hotkeyptr = top ? top->handle : 0;
    }
    ev->reason = 8;
    ev->size = 28;
    put_block(ev->data, &w->saved);
    memcpy(ev->data + 24, &code, 4);
    ev->set_r2 = 1;
    ev->r2 = 0;
}

/* The keys stage: returns 1 with an event for *to */
int wimp_key_event(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = w->caret.w != 0xFFFFFFFFu ? wimp_window(w->caret.w) : NULL;
    if (w->caret.w != 0xFFFFFFFFu && !win)
        w->caret = none;                        /* its window has gone: no caret */
    struct wimp_task *t = owner_task(win);
    if (t && (t->mask & KEY_MASK))
        return 0;                               /* Key_Pressed masked: no key read */
    for (;;) {
        uint32_t code;
        if (!next_key(&code))
            return 0;
        if (code == 0x1B && wimp_clipboard_escape())
            continue;                           /* Escape aborts a drag of text out */
        if (code == 0x1B && wimp_menu_escape())
            continue;                           /* Escape closes the menu tree */
        w->saved = w->caret;                    /* the snapshot for Key_Pressed */
        if (win && !t && w->caret.i >= 0 && wimp_menu_level(win->handle) >= 0)
            t = wimp_menu_owner();              /* a writable menu item */
        if (!win || !t) {                       /* no caret */
            if (w->singletask >= 0)
                continue;                       /* tryhotkeys: dropped while single-tasking */
            w->hotkeyptr = 0;
            hotkey(code);
            continue;
        }
        if (w->caret.i < 0) {
            key_pressed(ev, code);
            *to = t;
            return 1;
        }
        /* an icon: edited by its owner, on the owner's thread */
        ev->reason = WIMP_EV_EDIT;
        ev->size = 0;
        ev->r2 = code;
        ev->set_r2 = 0;
        *to = t;
        return 1;
    }
}

/* ---- writable icon editing ------------------------------------------------------ */

static uint32_t find_cmd(uint32_t v, uint32_t letter)
{
    if (v == 0 || v == 0xFFFFFFFFu)
        return 0;
    uint32_t p = v, c;
    for (;;) {
        c = ros_ld8(p++);
        if (c < 32)
            return 0;
        if ((c & 0xDFu) == letter)
            return p;
        for (;;) {
            c = ros_ld8(p++);
            if (c < 32)
                return 0;
            if (c == ';')
                break;
            if (c == '\\' && ros_ld8(p++) < 32)
                return 0;
        }
    }
}

/* findKcommand */
static int kletter(uint32_t v, uint32_t letter)
{
    uint32_t p = find_cmd(v, 'K'), k;
    if (!p)
        return 0;
    while ((k = ros_ld8(p++)) >= 32) {
        if ((k & 0xDFu) == letter)
            return 1;
        if ((k & 0xDFu) == 0x1B)                /* ';' */
            return 0;
    }
    return 0;
}

/* checkvalid, without the rule for spaces, which the caller applies */
static int valid(uint32_t v, uint32_t c)
{
    uint32_t p = find_cmd(v, 'A');
    if (!p)
        return 1;
    int ok = ros_ld8(p) == '~', include = 1;
    for (;;) {
        uint32_t x = ros_ld8(p++);
        if (x < 32 || x == ';')
            break;
        if (x == '~') {
            include = !include;
            continue;
        }
        if (x == '\\')
            x = ros_ld8(p++);
        uint32_t y = ros_ld8(p++);
        if (y == '-') {
            uint32_t z = ros_ld8(p++);
            if (z < 32 || z == ';' || z == '-' || z == '~')
                break;                          /* 5.30 raises &28F, with effects not known */
            if (z == '\\')
                z = ros_ld8(p++);
            if (x <= c && c <= z)
                ok = include;
        } else {
            p--;
            if (c == x)
                ok = include;
        }
    }
    return ok;
}

/* For the Clipboard Manager (clipboard.c): pasted bytes checked against
 * an icon's A command, and the icon's U limit (0 for none) */
int wimp_valid_chars(uint32_t validation, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (!valid(validation, p[i]))
            return 0;
    return 1;
}

uint32_t wimp_valid_ulimit(uint32_t validation)
{
    uint32_t up = find_cmd(validation, 'U');
    if (!up)
        return 0;
    struct ros_cpu rc;
    ros_cpu_enter(&rc);
    rc.r[0] = 10, rc.r[1] = up;
    ros_swi(&rc, XOS_ReadUnsigned);
    return rc.v ? 0 : rc.r[2];
}

static int is_sep(uint32_t c)
{
    return c == ' ' || c == '.';
}

static uint32_t skipword_r(uint32_t a, uint32_t i)
{
    while (ros_ld8(a + i) >= 32 && !is_sep(ros_ld8(a + i)))
        i++;
    while (ros_ld8(a + i) >= 32 && is_sep(ros_ld8(a + i)))
        i++;
    return i;
}

static uint32_t skipword_l(uint32_t a, uint32_t i)
{
    while (i > 0) {
        i--;
        if (!is_sep(ros_ld8(a + i)))
            break;
    }
    while (i > 0) {
        i--;
        if (is_sep(ros_ld8(a + i))) {
            i++;
            break;
        }
    }
    return i;
}

/* nextfield and prevfield: the next or previous writable icon, or -1 */
static int32_t field(struct wimp_window *win, int32_t from, int dir, int wrap)
{
    struct itext t;
    if (!icon_text(win, from, &t))
        return -1;
    uint32_t esg = (t.flags >> 16) & 15;
    int32_t n = (int32_t)win->nicons, i = from;
    for (int32_t k = 0; k < n; k++) {
        i += dir;
        if (i >= n || i < 0) {
            if (!wrap)
                return -1;
            i = dir > 0 ? 0 : n - 1;
        }
        struct itext u;
        if (icon_text(win, i, &u) && ((u.flags >> 12) & 15) >= 14 && ((u.flags >> 16) & 15) == esg &&
            !(u.flags & (IF_SHADED | IF_DELETED)))
            return i;
    }
    return -1;
}

/* donechar and donearrow: the caret is set again at an index, with its
 * flags kept */
static void caret_index(uint32_t index)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_caretblk c = w->caret;
    set_caret(c.w, c.i, c.x, c.y, c.hf, (int32_t)index, 0, 0, NULL);
}

static void move_to_field(struct wimp_window *win, int32_t i)
{
    struct itext u;
    icon_text(win, i, &u);
    set_caret(win->handle, i, 0, 0, 0xFFFFFFFFu, (int32_t)u.len, 0, 0, NULL);
}

enum { USED, REPORT };

/* ---- selections and the clipboard keys -------------------------------------------- */

/* int_set_caret_position's selection form, as the key code calls it */
static void sel_set(struct wimp_window *win, int32_t icon, int32_t lo, int32_t hi)
{
    set_caret(win->handle, icon, 0, 0, 1u << 31, lo, hi, 0, NULL);
}

/* clipboard_check_disabled: a K command with C */
static int cb_disabled(uint32_t v)
{
    return kletter(v, 'C');
}

/* clipboard_paste_move_data and _copy_data, on the icon's text t.  The
 * text has room for cap bytes, and u is its U limit, or 0.  The data
 * replaces the selection if the selection is in this icon.  Otherwise it
 * goes in at index.  Returns the number of bytes put in, and sets *start
 * to where they went. */
uint32_t wimp_paste_text(struct wimp_window *win, int32_t icon, uint8_t *t, uint32_t cap, uint32_t u,
                         uint32_t index, const uint8_t *data, uint32_t n, uint32_t *start)
{
    if (u && u + 1 < cap)
        cap = u + 1;
    int in_sel = win->sel.icon == icon;
    uint32_t lo = in_sel ? (uint32_t)win->sel.low : index, hi = in_sel ? (uint32_t)win->sel.high : index;
    uint32_t used = 0;                          /* the bytes kept, and the terminator */
    for (uint32_t i = 0;; i++) {
        if (i == lo)
            i = hi;
        used++;
        if (t[i] < 32)
            break;
    }
    uint32_t room = cap > used ? cap - used : 0;
    if (n > room)
        n = room;
    uint32_t len = 0;
    while (len < cap && t[len] >= 32)
        len++;
    memmove(t + lo + n, t + hi, len - hi + 1);
    memcpy(t + lo, data, n);
    *start = lo;
    return n;
}

/* The Wimp's own clipboard (clipboard_flexblock_clipdata) */
static void clip_store(const uint8_t *p, uint32_t n)
{
    struct wimp_ws *w = wimp_ws();
    if (w->clipdata)
        xos_module_free(ros_ptr(w->clipdata));
    w->clipdata = 0, w->cliplen = 0;
    void *mem;
    if (n && !xos_module_claim(n, &mem)) {
        memcpy(mem, p, n);
        w->clipdata = ros_addr(mem), w->cliplen = n;
    }
}

void wimp_clip_forget(void)
{
    clip_store(NULL, 0);
}

/* clipboard_wcopy_perform, from the window's selection.  Returns 0 if
 * there is none. */
static int sel_copy(struct wimp_window *win)
{
    struct itext s;
    if (win->sel.icon < 0 || !icon_text(win, win->sel.icon, &s))
        return 0;
    clip_store(ros_ptr(s.addr + (uint32_t)win->sel.low), (uint32_t)(win->sel.high - win->sel.low));
    wimp_clipboard_request(CB_PW_COPY);
    return 1;
}

/* clipboard_wdelete_perform: the selected bytes and the selection are
 * removed, and the caret goes to the selection's low end */
static void sel_delete(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    struct itext s;
    int32_t icon = win->sel.icon;
    if (icon < 0 || !icon_text(win, icon, &s))
        return;
    uint32_t lo = (uint32_t)win->sel.low, hi = (uint32_t)win->sel.high, i = 0;
    uint8_t c;
    do {
        c = (uint8_t)ros_ld8(s.addr + hi + i);
        ros_st8(s.addr + lo + i, c);
        i++;
    } while (c >= 32);
    sel_set(win, icon, 0, 0);
    set_caret(win->handle, w->caret.i, 0, 0, 0, (int32_t)lo, 0, 0, NULL);
}

/* clipboard_kp_delete_selection: typing or deleting over a selection in
 * the caret's icon removes it.  Returns the new insertion index, or -1 if
 * there was no selection. */
static int32_t sel_cut_in_place(struct wimp_window *win, uint32_t a)
{
    struct wimp_ws *w = wimp_ws();
    if (win->sel.icon != w->caret.i)
        return -1;
    uint32_t lo = (uint32_t)win->sel.low, hi = (uint32_t)win->sel.high, i = 0;
    uint8_t c;
    do {
        c = (uint8_t)ros_ld8(a + hi + i);
        ros_st8(a + lo + i, c);
        i++;
    } while (c >= 32);
    return (int32_t)lo;
}

/* clipboard_kp_remove_selection, _l and _r: an arrow key drops the
 * selection in the caret's icon.  The caret goes first to the selection's
 * low or high end, as end says: 0 neither, 1 low, 2 high.  Returns the
 * index to move from. */
static uint32_t sel_drop(struct wimp_window *win, uint32_t idx, int end)
{
    struct wimp_ws *w = wimp_ws();
    if (win->sel.icon != w->caret.i)
        return idx;
    int32_t at = end == 1 ? win->sel.low : end == 2 ? win->sel.high : 0;
    sel_set(win, w->caret.i, at, at);
    if (end) {
        set_caret(win->handle, w->caret.i, 0, 0, 0xFFFFFFFFu, at, 0, 0, NULL);
        return (uint32_t)at;
    }
    return idx;
}

/* passbacktouser: Key_Pressed to the owner.  For a writable menu item,
 * Return chooses the item and any other key goes to the hot-key chain. */
static int pass_back(struct wimp_event *ev, uint32_t code)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = wimp_window(w->caret.w);
    if (win && win->owner == NO_WINDOW && wimp_menu_level(win->handle) >= 0) {
        if (code == 0x0D)
            return wimp_menu_return(win, w->caret.i, ev);
        w->hotkeyptr = 0;
        hotkey(code);
        return 0;
    }
    key_pressed(ev, code);
    return 1;
}

/* The owner's half of a key for its writable icon.  Returns 1 if ev now
 * holds the Key_Pressed to return, and 0 if the key was used. */
int wimp_edit_key(uint32_t code, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = wimp_window(w->caret.w);
    struct itext t;
    if (!win || w->caret.i < 0 || !icon_text(win, w->caret.i, &t)) {
        key_pressed(ev, code);
        return 1;
    }
    uint32_t a = t.addr, v = t.validation, len = t.len;
    uint32_t idx = w->caret.index < 0 ? 0 : (uint32_t)w->caret.index;
    if (idx > len)
        idx = len;
    int r = USED, kd = kletter(v, 'D');
    /* Backspace and Delete first try clipboard_possible_wcut */
    if ((code == 0x08 || code == 0x7F) && !cb_disabled(v) && win->sel.icon >= 0 &&
        win->sel.low != win->sel.high)
        code = 0x18;
    switch (code) {
    case 0x01:                                  /* Ctrl-A: select all */
        if (cb_disabled(v)) {
            r = REPORT;
            break;
        }
        sel_set(win, w->caret.i, 0, SEL_BIGNUM);
        break;
    case 0x03: case 0x18: case 0x0B: {          /* Ctrl-C, Ctrl-X, Ctrl-K */
        if (cb_disabled(v)) {
            r = REPORT;
            break;
        }
        if (win->sel.icon < 0)
            break;
        struct itext s;                         /* clipboard_setupdata: the selection's icon */
        if (!icon_text(win, win->sel.icon, &s))
            break;
        if (code != 0x0B && find_cmd(s.validation, 'D')) {
            struct ros_cpu b;                   /* a password is not copied */
            ros_cpu_enter(&b);
            b.r[0] = 7;
            ros_swi(&b, XOS_WriteC);
            break;
        }
        if (code != 0x0B)
            sel_copy(win);
        if (code == 0x18)
            wimp_clipboard_request(CB_PW_CUT);
        if (code != 0x03)
            sel_delete(win);
        break;
    }
    case 0x1A:                                  /* Ctrl-Z: the selection is dropped */
        if (cb_disabled(v)) {
            r = REPORT;
            break;
        }
        if (win->sel.icon >= 0)
            sel_set(win, win->sel.icon, 0, 0);
        break;
    case 0x16: case 0x1CD:                      /* Ctrl-V, Insert: paste */
        if (cb_disabled(v)) {
            r = REPORT;
            break;
        }
        if (w->cliplen) {
            uint8_t *p = ros_ptr(w->clipdata);
            uint32_t n = 0;
            while (n < w->cliplen && p[n] != 0 && p[n] != 10 && p[n] != 13)
                n++;
            int ok = 1;
            for (uint32_t i = 0; i < n && ok; i++)
                ok = p[i] >= 32 && valid(v, p[i]);
            if (!ok) {
                struct ros_cpu b;
                ros_cpu_enter(&b);
                b.r[0] = 7;
                ros_swi(&b, XOS_WriteC);
                break;
            }
            uint32_t start, m = wimp_paste_text(win, w->caret.i, ros_ptr(a), t.cap, wimp_valid_ulimit(v),
                                               idx, p, n, &start);
            int32_t ci = w->caret.i;
            sel_set(win, ci, (int32_t)start, (int32_t)(start + m));
            set_caret(win->handle, ci, 0, 0, 0xFFFFFFFFu, (int32_t)(start + m), 0, 0, NULL);
        } else {
            wimp_clipboard_request(CB_PW_PASTE);
        }
        break;
    case 0x1BC: case 0x1BD: {                   /* Shift-Ctrl-Left, -Right */
        if (cb_disabled(v)) {
            r = REPORT;
            break;
        }
        int right = (code == 0x1BD) != (w->writedir != 0);
        int32_t lo, hi, ci = (int32_t)idx;
        if (win->sel.icon != w->caret.i) {
            lo = hi = ci;
            win->sel.low = win->sel.high = ci;
        } else {
            lo = win->sel.low, hi = win->sel.high;
        }
        if (!right) {
            if (w->caret.index == hi) lo--; else hi--;
        } else {
            if (w->caret.index == lo) hi++; else lo++;
        }
        sel_set(win, w->caret.i, lo, hi);
        break;
    }
    case 0x08: {                                /* Backspace */
        int32_t at = sel_cut_in_place(win, a);
        if (at >= 0) {
            set_caret(win->handle, w->caret.i, w->caret.x, w->caret.y, 0xFFFFFFFFu, at, 0, 0, NULL);
            break;
        }
        if (idx == 0)
            break;
        for (uint32_t i = idx - 1; i < len; i++)
            ros_st8(a + i, ros_ld8(a + i + 1));
        caret_index(idx - 1);
        r = kd ? REPORT : USED;
        break;
    }
    case 0x7F: {                                /* Delete */
        int32_t at = sel_cut_in_place(win, a);
        if (at >= 0) {
            set_caret(win->handle, w->caret.i, w->caret.x, w->caret.y, 0xFFFFFFFFu, at, 0, 0, NULL);
            break;
        }
        if (idx >= len)
            break;
        for (uint32_t i = idx; i < len; i++)
            ros_st8(a + i, ros_ld8(a + i + 1));
        caret_index(idx);
        r = kd ? REPORT : USED;
        break;
    }
    case 0x1E: case 0x1AC:                      /* Home, Ctrl-Left */
        sel_drop(win, idx, 0);
        caret_index(0);
        break;
    case 0x18B: case 0x1AD:                     /* Copy, Ctrl-Right */
        sel_drop(win, idx, 0);
        icon_text(win, w->caret.i, &t);
        caret_index(t.len);
        break;
    case 0x15:                                  /* Ctrl-U */
        if (len == 0)
            break;
        ros_st8(a, ros_ld8(a + len));
        caret_index(0);
        r = kd ? REPORT : USED;
        break;
    case 0x0D:                                  /* Return */
        if (kletter(v, 'R')) {
            int32_t i = field(win, w->caret.i, 1, 0);
            if (i >= 0) {
                move_to_field(win, i);
                break;
            }
        }
        r = REPORT;
        break;
    case 0x18E: case 0x18F: case 0x18A: case 0x19A: {
        int arrows = code == 0x18E || code == 0x18F;
        if (!kletter(v, arrows ? 'A' : 'T')) {
            r = REPORT;
            break;
        }
        int32_t i = field(win, w->caret.i, (code == 0x18E || code == 0x18A) ? 1 : -1, 1);
        if (i >= 0)
            move_to_field(win, i);
        break;
    }
    case 0x18C:                                 /* Left */
        idx = sel_drop(win, idx, 1);
        if (idx > 0)
            caret_index(idx - 1);
        break;
    case 0x18D:                                 /* Right */
        idx = sel_drop(win, idx, 2);
        if (idx < len)
            caret_index(idx + 1);
        break;
    case 0x19C:                                 /* Shift-Left */
        idx = sel_drop(win, idx, 1);
        if (idx > 0)
            caret_index(skipword_l(a, idx));
        break;
    case 0x19D:                                 /* Shift-Right */
        idx = sel_drop(win, idx, 2);
        if (ros_ld8(a + idx) >= 32)
            caret_index(skipword_r(a, idx));
        break;
    case 0x19B: {                               /* Shift-Copy */
        if (ros_ld8(a + idx) < 32)
            break;
        uint32_t e = skipword_r(a, idx), i = 0;
        do
            ros_st8(a + idx + i, ros_ld8(a + e + i));
        while (ros_ld8(a + e + i++) >= 32);
        caret_index(idx);
        r = kd ? REPORT : USED;
        break;
    }
    case 0x1AB:                                 /* Ctrl-Copy */
        if (ros_ld8(a + idx) < 32)
            break;
        ros_st8(a + idx, ros_ld8(a + len));
        caret_index(idx);
        r = kd ? REPORT : USED;
        break;
    default:
        if (code < 0x20 || code >= 0x100) {
            r = REPORT;                         /* passbacktouser */
            break;
        }
        {
            uint32_t c = code;
            if (len + 1 + 1 > t.cap)            /* no room */
                break;
            uint32_t up = find_cmd(v, 'U');
            if (up) {
                struct ros_cpu rc;
                ros_cpu_enter(&rc);
                rc.r[0] = 10, rc.r[1] = up;
                ros_swi(&rc, XOS_ReadUnsigned);
                if (!rc.v && len >= rc.r[2])
                    break;
            }
            if (!valid(v, c)) {
                if (c == ' ' && valid(v, 0xA0))
                    c = 0xA0;                   /* SpacesInFilenames */
                else
                    return pass_back(ev, code); /* backtouser, skipping the KN test */
            }
            /* typing over a selection in this icon */
            uint32_t at = idx;
            int32_t cut = sel_cut_in_place(win, a);
            if (cut >= 0)
                at = (uint32_t)cut;
            for (uint32_t i = len + 1; i > at; i--)
                ros_st8(a + i, ros_ld8(a + i - 1));
            ros_st8(a + at, c);
            caret_index((uint32_t)w->caret.index + 1);
        }
    }
    if (r == REPORT || kletter(v, 'N'))
        return pass_back(ev, code);
    return 0;
}
