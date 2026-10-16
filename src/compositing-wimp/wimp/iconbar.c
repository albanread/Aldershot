/* iconbar.c: the icon bar.
 *
 * The icon bar is a window of the Wimp's own. It is made from the
 * `iconbar' template when the first icon is created, and deleted with the
 * last. Its icons are in two lists, left and right. Each list runs from
 * its screen edge inwards, in order of priority.
 *
 * recalcposns places the icons on one side and writes the extent and
 * scroll offset straight into the window. It then invalidates the visible
 * work area. The Wimp redraws that itself, as the bar is an auto-redraw
 * window. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

#define LEFT  0
#define RIGHT 1
#define PRIORITY_MAX 0x78000000

static struct wimp_ib *ib(void)
{
    return &wimp_ws()->ib;
}

static struct wimp_window *bar(void)
{
    struct wimp_ws *w = wimp_ws();
    return w->iconbar ? wimp_window(w->iconbar) : NULL;
}

static uint32_t rd(const uint8_t *b, unsigned off)
{
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

static void wr(uint8_t *b, unsigned off, uint32_t v)
{
    memcpy(b + off, &v, 4);
}

/* As findicon. Returns the index of an icon's record and sets its side,
 * or returns -1. */
static int find(int32_t icon, int *side)
{
    struct wimp_ib *b = ib();
    for (int s = LEFT; s <= RIGHT; s++)
        for (uint32_t i = 0; i < b->side[s].n; i++)
            if (b->side[s].r[i].icon == icon) {
                *side = s;
                return (int)i;
            }
    return -1;
}

/* The handle of the task that created an icon bar icon, or 0 if none */
int32_t wimp_iconbar_owner(int32_t icon)
{
    int s, i = find(icon, &s);
    if (i < 0)
        return 0;
    struct wimp_task *t = wimp_task_by_handle(ib()->side[s].r[i].task, 0);
    return t ? (int32_t)t->handle : 0;
}

/* ---- the window ----------------------------------------------------------------------- */

/* Open the bar across the bottom of the screen, behind the window given,
 * and set each side's extent. */
static void open_bar(uint32_t behind)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_ib *b = ib();
    struct wimp_window *win = bar();
    if (!win)
        return;
    int32_t cx0 = 0, cy0 = 0, cx1 = w->screen_w, cy1 = w->iconbar_height, ix = 0, iy = 0;
    if (w->dr.threed & (1u << 9)) {
        win->def[32] = 0xFF;                    /* NoIconBarBorder */
    } else {
        win->def[32] = 7;
        ix = w->dx, iy = w->dy;
    }
    cx0 += ix, cx1 -= ix, cy0 += iy;
    int32_t V = cx1 - cx0;
    int32_t ex0 = (int32_t)rd(win->def, 40), ex1 = (int32_t)rd(win->def, 48);
    (void)ex1;
    /* BOX differs from RISC OS 5.30 here, by the user's decision. 5.30
     * never lets the right-hand extent E(R) fall below the old ex1. So a
     * change to a narrower mode leaves the right-hand icons beyond the
     * screen. Nothing there can be clicked, and only scrolling the bar
     * brings them back. On a box whose window is resized, which is a mode
     * change, that happens all the time.
     *
     * Here the extent is as wide as the screen, or as wide as the icons
     * need if they do not fit. A bar with too many icons still scrolls.
     * The extent is never wider than that, so an icon is never left off
     * the end. */
    int32_t need = b->side[LEFT].W + b->side[RIGHT].W + 64;
    int32_t x1 = ex0 + (V > need ? V : need);
    b->side[LEFT].E = ex0, b->side[RIGHT].E = x1;
    wr(win->def, 48, (uint32_t)x1);
    b->side[LEFT].O = V, b->side[RIGHT].O = -V;
    w->iconbar_laid_w = w->screen_w;
    b->side[LEFT].M = b->side[LEFT].E + b->side[LEFT].W;
    b->side[RIGHT].M = b->side[RIGHT].E - b->side[RIGHT].W;
    uint8_t blk[32];
    wr(blk, 0, win->handle);
    wr(blk, 4, (uint32_t)cx0), wr(blk, 8, (uint32_t)cy0), wr(blk, 12, (uint32_t)cx1), wr(blk, 16, (uint32_t)cy1);
    wr(blk, 20, (uint32_t)win->s[REQ].scx), wr(blk, 24, (uint32_t)win->s[REQ].scy);
    wr(blk, 28, behind);
    wimp_open_own(win, blk);
}

/* Create the bar from its template and open it. */
static os_error *make_bar(void)
{
    struct wimp_ws *w = wimp_ws();
    uint8_t block[88];
    if (!wimp_template_load("iconbar", block))
        return ros_error(0x291, "Template entry not found");
    os_error *e = NULL;
    struct wimp_window *win = wimp_create_system(block, &e);
    if (!win)
        return e;
    wr(win->def, 64, 1);
    w->iconbar = win->handle;
    w->iconbar_height = win->s[REQ].vis.y1 - win->s[REQ].vis.y0;
    open_bar(win->handle);                      /* behind itself */
    return NULL;
}

/* After a mode change, open the bar again and lay out both sides. */
void wimp_iconbar_mode(void)
{
    struct wimp_window *win = bar();
    if (!win)
        return;
    open_bar(win->handle);
    wimp_iconbar_layout(LEFT);
    wimp_iconbar_layout(RIGHT);
}

/* ---- the layout ------------------------------------------------------------------------ */

void wimp_iconbar_layout(int s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_ib *b = ib();
    struct wimp_window *win = bar();
    if (!win)
        return;
    struct ib_side *sd = &b->side[s];
    int32_t W = 0;
    for (uint32_t i = 0; i < sd->n; i++) {
        const uint8_t *ic = wimp_icon(win, (uint32_t)sd->r[i].icon);
        if (ic)
            W += 16 + ((int32_t)rd(ic, 8) - (int32_t)rd(ic, 0));
    }
    sd->W = W;
    int32_t V = b->side[LEFT].O;
    if (b->side[LEFT].W + b->side[RIGHT].W > V - 64) {
        if (s == LEFT) {
            sd->M = b->side[RIGHT].M - 64;
            sd->E = sd->M - sd->W;
        } else {
            sd->M = b->side[LEFT].M + 64;
            sd->E = sd->M + sd->W;
        }
    } else if (s == LEFT) {
        sd->E = b->side[RIGHT].E - V;
        sd->M = sd->E + sd->W;
    } else {
        sd->E = b->side[LEFT].E + V;
        sd->M = sd->E - sd->W;
    }
    int32_t a = sd->E;
    for (uint32_t i = 0; i < sd->n; i++) {
        uint8_t *ic = wimp_icon(win, (uint32_t)sd->r[i].icon);
        if (!ic)
            continue;
        int32_t wd = (int32_t)rd(ic, 8) - (int32_t)rd(ic, 0);
        if (s == LEFT) {
            wr(ic, 0, (uint32_t)(a + 16));
            wr(ic, 8, (uint32_t)(a + 16 + wd));
            a += 16 + wd;
        } else {
            a -= 16;
            wr(ic, 8, (uint32_t)a);
            wr(ic, 0, (uint32_t)(a - wd));
            a -= wd;
        }
    }
    /* Write the extent and scroll offset straight into the window. */
    int32_t ex0 = b->side[LEFT].E & ~(w->dx - 1), ex1 = b->side[RIGHT].E & ~(w->dx - 1);
    int32_t x = win->s[REQ].scx;
    if (x < ex0) x = ex0;
    if (x > ex1 + b->side[RIGHT].O) x = ex1 + b->side[RIGHT].O;
    wr(win->def, 40, (uint32_t)ex0);
    wr(win->def, 48, (uint32_t)ex1);
    win->s[REQ].scx = win->s[APP].scx = x;
    wr(win->def, 16, (uint32_t)x);
    /* Invalidate the visible work area. */
    wimp_flush();
    if (win->s[APP].open) {
        struct wimp_rlist *l = wimp_rl_new();
        if (l) {
            wimp_inner(win, APP, l);
            wimp_invalidate(l);
            wimp_rl_free(l);
        }
    }
}

/* ---- Wimp_CreateIcon on the icon bar --------------------------------------------- */

os_error *wimp_iconbar_create(uint32_t form, uint32_t r0, uint32_t block, struct wimp_task *t, uint32_t *handle)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_ib *b = ib();
    static const uint8_t bytes[8] = { 1, 0, 0x82, 0x83, 0x84, 4, 5, 0x85 };
    uint8_t fb = bytes[(uint32_t)(-(int32_t)form) - 1];
    if (b->flipped)
        fb ^= 1;
    int s = fb & 1;
    int32_t p = (fb & 4) ? (int32_t)r0 : 0;
    int at = -1, side = s;
    if ((fb & 2) && (int32_t)r0 >= 0) {         /* next to an icon */
        int fs, i = find((int32_t)r0, &fs);
        if (i < 0)
            return ros_error(0x2A1, "Illegal icon handle");
        p = b->side[fs].r[i].prio;
        side = fs;
        at = fs == s ? i : i + 1;
    } else {
        if (fb & 2)
            p = PRIORITY_MAX;                   /* at the far end of the side */
        struct ib_side *sd = &b->side[s];
        at = 0;
        while ((uint32_t)at < sd->n &&
               (p < sd->r[at].prio || (p == sd->r[at].prio && !(fb & 0x80))))
            at++;
    }
    if (!bar()) {                               /* the first icon makes the bar */
        os_error *e = make_bar();
        if (e)
            return e;
    }
    struct wimp_window *win = bar();
    uint8_t ic[32];
    memcpy(ic, ros_ptr(block), 32);
    int32_t d = (int32_t)rd(ic, 8) - (int32_t)rd(ic, 0);
    uint32_t flags = rd(ic, 16);
    if (flags & 1u) {                           /* a text icon is widened to fit */
        uint32_t text = (flags & (1u << 8)) ? rd(ic, 20) : block + 20;
        int noauto = 0;
        if (flags & (1u << 8)) {
            uint32_t v = rd(ic, 24), c;
            for (uint32_t p2 = v; v && v != 0xFFFFFFFFu && (c = ros_ld8(p2)) >= 32; p2++) {
                if ((c & 0xDFu) == 'X' && (p2 == v || ros_ld8(p2 - 1) == ';')) {
                    noauto = 1;
                    break;
                }
            }
        }
        if (!noauto) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = 1, c.r[1] = text, c.r[2] = 0;
            ros_swi(&c, XWimp_TextOp);
            int32_t wd = (int32_t)c.r[0] + 2 * w->dx;
            if (!c.v && wd - d >= 0)
                wr(ic, 8, rd(ic, 0) + (uint32_t)wd);
        }
    }
    uint32_t slot;
    os_error *e = wimp_icon_add(win, ic, &slot);
    if (e)
        return e;
    struct ib_side *sd = &b->side[side];
    if (sd->n >= WIMP_IB_MAX)
        return ros_error(E_TOO_BIG, "There is not enough memory to create this window or menu");
    memmove(&sd->r[at + 1], &sd->r[at], (sd->n - (uint32_t)at) * sizeof sd->r[0]);
    sd->r[at] = (struct ib_rec){ (int32_t)slot, t ? t->internal : 0, p, d };
    sd->n++;
    wimp_iconbar_layout(side);
    *handle = slot;
    return NULL;
}

/* ---- Wimp_DeleteIcon on the icon bar --------------------------------------------- */

void wimp_iconbar_delete(int32_t icon)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_ib *b = ib();
    struct wimp_window *win = bar();
    if (!win)
        return;                                 /* no icon bar: no effect, by choice */
    if ((uint32_t)icon < win->nicons) {
        struct wimp_caretblk none = { 0xFFFFFFFFu, -1, 0, 0, 0, -1 };
        if (w->caret.w == win->handle && w->caret.i == icon)
            w->caret = none;
        wimp_icon_remove(win, (uint32_t)icon);
    }
    int s, i = find(icon, &s);
    if (i >= 0) {
        struct ib_side *sd = &b->side[s];
        memmove(&sd->r[i], &sd->r[i + 1], (sd->n - (uint32_t)i - 1) * sizeof sd->r[0]);
        sd->n--;
    } else {
        s = RIGHT;
    }
    wimp_iconbar_layout(s);
    if (b->side[LEFT].n || b->side[RIGHT].n)
        return;
    w->iconbar = 0;
    wimp_delete_system(win);
}

/* When a task closes down, its icons are deleted, left side then right,
 * one at a time. */
void wimp_iconbar_task_gone(uint32_t internal)
{
    struct wimp_ib *b = ib();
    for (int s = LEFT; s <= RIGHT; s++)
        for (uint32_t i = 0; i < b->side[s].n;) {
            if (b->side[s].r[i].task == internal) {
                wimp_iconbar_delete(b->side[s].r[i].icon);
                i = 0;
                if (!bar())
                    return;
            } else {
                i++;
            }
        }
}

/* The internal handle of the icon's creator, or 0. This is for
 * Wimp_WhichIcon with window handle -2. */
uint32_t wimp_iconbar_icon_task(int32_t icon)
{
    int s, i = find(icon, &s);
    return i < 0 ? 0 : ib()->side[s].r[i].task;
}

/* ---- refitting the icons to the desktop font -------------------------------------- */

void wimp_iconbar_refit(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_ib *b = ib();
    b->needs_rs = 0;
    struct wimp_window *win = bar();
    if (!win || !win->nicons)
        return;
    for (uint32_t h = 0; h < win->nicons; h++) {
        int s, i = find((int32_t)h, &s);
        uint8_t *ic = wimp_icon(win, h);
        uint32_t flags = rd(ic, 16);
        if (i < 0 || !(flags & 1u))
            continue;
        uint32_t text = (flags & (1u << 8)) ? rd(ic, 20) : ros_addr(ic + 20);
        if (flags & (1u << 8)) {
            uint32_t v = rd(ic, 24), c;
            int noauto = 0;
            for (uint32_t p = v; v && v != 0xFFFFFFFFu && (c = ros_ld8(p)) >= 32; p++)
                if ((c & 0xDFu) == 'X' && (p == v || ros_ld8(p - 1) == ';'))
                    noauto = 1;
            if (noauto)
                continue;
        }
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 1, c.r[1] = text, c.r[2] = 0;
        ros_swi(&c, XWimp_TextOp);
        uint32_t wd = c.r[0] + 2u * (uint32_t)w->dx;
        uint32_t def = (uint32_t)b->side[s].r[i].defw;
        wr(ic, 8, rd(ic, 0) + (wd > def ? wd : def));
    }
    wimp_iconbar_layout(LEFT);
    wimp_iconbar_layout(RIGHT);
    wimp_iconbar_layout(LEFT);
}
