/* input.c -- the pointer, clicks, the window furniture and drags.
 *
 * The poll's input stages run in this order: the mouse is read, a drag
 * is stepped, the pointer is hit-tested, a pending drag may fire early,
 * entering and leaving are reported, and then clicks are handled.  Each
 * stage may end the poll with an event for one task, which the search
 * hands on as it does any other event. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "wimp.h"

#define F_MOVEABLE   (1u << 1)
#define F_PANE       (1u << 5)
#define F_NOCHECKS   (1u << 6)
#define F_SCROLLREQ  (3u << 8)
#define F_HOTKEYS    (1u << 12)
#define F_INBORDER   (1u << 23)
#define F_FOREGROUND (1u << 23)
#define F_BACKWIN    (1u << 11)
#define F_BACK       (1u << 24)
#define F_CLOSE      (1u << 25)
#define F_TITLE      (1u << 26)
#define F_TOGGLE     (1u << 27)
#define F_VBAR       (1u << 28)
#define F_SIZE       (1u << 29)
#define F_HBAR       (1u << 30)
#define ST_TOGGLED   ((1u << 18) | (1u << 22))
#define ST_TOGGLING  (1u << 19)
#define ST_FORCE     (1u << 21)

#define IF_DELETED  (1u << 23)
#define IF_SHADED   (1u << 22)
#define IF_SELECTED (1u << 21)

/* the pending flags of a press */
enum { P_REPEAT = 1, P_CLICK = 2, P_DRAG = 4, P_RELEASE = 8, P_DOUBLE = 16, P_DESELECT = 32 };
/* the properties of each button type */
enum { T_NOPRESS = 1, T_SELECT = 2, T_WRITABLE = 4, T_X256 = 8, T_SILENT = 16, T_REPORTS = 32, T_CNP = 64 };

static const struct { uint8_t props, pend; } types[16] = {
    [0] = { 0, 0 },
    [1] = { T_REPORTS | T_NOPRESS, 0 },
    [2] = { T_REPORTS, P_REPEAT },
    [3] = { T_REPORTS, 0 },
    [4] = { T_REPORTS | T_SELECT, P_RELEASE | P_DESELECT },
    [5] = { T_REPORTS | T_SELECT, P_DOUBLE },
    [6] = { T_REPORTS, P_DRAG },
    [7] = { T_REPORTS | T_SELECT, P_RELEASE | P_DRAG },
    [8] = { T_REPORTS | T_SELECT, P_DOUBLE | P_DRAG },
    [9] = { T_REPORTS | T_SELECT | T_NOPRESS, P_CLICK | P_DESELECT },
    [10] = { T_REPORTS | T_SELECT | T_X256, P_DOUBLE | P_DRAG },
    [11] = { T_REPORTS | T_SELECT, P_DRAG },
    [12] = { T_REPORTS | T_SELECT | T_SILENT, P_DOUBLE | P_DRAG },
    [13] = { T_REPORTS, P_DOUBLE },
    [14] = { T_REPORTS | T_WRITABLE, P_DRAG },
    [15] = { T_REPORTS | T_SELECT | T_X256 | T_CNP, P_DOUBLE | P_DRAG },
};

static struct wimp_input *in(void)
{
    return &wimp_ws()->in;
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

static uint32_t now_cs(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_ReadMonotonicTime);
    return c.r[0];
}

static int key_down(uint32_t internal)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 121, c.r[1] = internal ^ 0x80u;
    ros_swi(&c, XOS_Byte);
    return !c.v && c.r[1] == 0xFFu;
}

static uint8_t cmos(uint32_t addr)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 161, c.r[1] = addr;
    ros_swi(&c, XOS_Byte);
    return (uint8_t)c.r[2];
}

/* Reads the configuration that this file uses */
void wimp_input_config(void)
{
    struct wimp_input *p = in();
    uint8_t dd = cmos(0xDD), de = cmos(0xDE), df = cmos(0xDF), c16 = cmos(0x16);
    p->drag_time = ((dd & 15u) ^ 5u) * 10u * ((de & 1u) ? 10u : 1u);
    p->drag_move = (((de >> 2) & 31u) << 2) ^ 32u;
    p->dclick_time = ((df & 15u) ^ 10u) * 10u * ((c16 & 1u) ? 10u : 1u);
    p->dclick_move = (((c16 >> 2) & 31u) << 2) ^ 32u;
    p->release_furniture = (cmos(0x8C) >> 6) & 1u;
    p->wimpflags = cmos(0xC5);
    p->ptrwindow = 0xFFFFFFFFu;
    p->dash1 = 0xFC, p->dash2 = 0x05;
}

/* ---- pointer shapes --------------------------------------------------------- */

static struct wimp_autoscroll *as(void);

/* testptrshape: whether pointer shape 1 is selected now */
static int ptr_is_1(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 106, c.r[1] = 127;
    ros_swi(&c, XOS_Byte);
    return c.r[1] == 1;
}

/* setptr_shape: Wimp_SpriteOp 36 with a sprite from the Wimp's pools.
 * The shape is not reprogrammed while autoscrolling. */
static void setptr(const char *name, uint32_t r3, uint32_t x, uint32_t y)
{
    struct wimp_ws *w = wimp_ws();
    char *n = (char *)w->scratch + 448;
    strncpy(n, name, 31);
    n[31] = 0;
    if (as()->on)
        r3 |= 0x10u;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 36, c.r[2] = ros_addr(n), c.r[3] = r3, c.r[4] = x, c.r[5] = y, c.r[6] = c.r[7] = 0;
    ros_swi(&c, XWimp_SpriteOp);
}

/* The end of doubleptr_on and doubleptr_off.  Shape 1 is set from the
 * sprite.  It is selected and its palette set only if it is the shape in
 * use. */
static void dblptr(const char *name)
{
    setptr(name, ptr_is_1() ? 1u : 0x61u, 0, 0);
}

static void double_on(void)
{
    dblptr("ptr_double");
}

static void double_off(void)
{
    dblptr("ptr_default");
}

static const struct ros_slot *page_win(const struct wimp_window *win)
{
    struct wimp_task *t = win && (int32_t)win->owner > 0 ? wimp_task_by_handle(win->owner, 0) : NULL;
    return ros_task_page_in(t ? t->rt : NULL);
}

/* findcommand: the address after a validation string's command letter,
 * or 0 if there is no such command */
static uint32_t find_cmd(uint32_t v, uint32_t letter)
{
    if (v == 0 || (int32_t)v < 0)
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

/* doublewritable_on: sets a type 15 icon's double-click pointer.  This is
 * ptr_write2 if the icon's P command names ptr_write, and ptr_double
 * otherwise.  The name is compared without case, up to the first
 * character below 'A'. */
static void double_writable_on(struct wimp_window *win, int32_t icon)
{
    struct wimp_input *p = in();
    const uint8_t *ic = icon >= 0 ? wimp_icon(win, (uint32_t)icon) : NULL;
    p->cnp_write = 0;
    if (ic && (rd(ic, 16) & (1u << 8)) && (int32_t)rd(ic, 24) > 0) {
        const struct ros_slot *was = page_win(win);
        uint32_t q = find_cmd(rd(ic, 24), 'P');
        if (q) {
            static const char want[] = "ptr_write";
            int match = 1;
            for (unsigned k = 0;; k++) {
                uint32_t ch = ros_ld8(q + k);
                if (ch <= 'Z')
                    ch |= 32;
                if (ch < 'A')
                    ch = 0;
                if (ch != (uint8_t)want[k]) {
                    match = 0;
                    break;
                }
                if (!ch)
                    break;
            }
            p->cnp_write = match;
        }
        ros_task_page_back(was);
    }
    dblptr(p->cnp_write ? "ptr_write2" : "ptr_double");
}

static void double_writable_off(void)
{
    dblptr(in()->cnp_write ? "ptr_write" : "ptr_default");
}

/* getnumber: an unsigned decimal number, digits only.  It is 0 if there
 * are no digits. */
static uint32_t getnumber(uint32_t *q)
{
    uint32_t v = 0, ch;
    while ((ch = ros_ld8(*q) - '0') < 10)
        v = v * 10 + ch, (*q)++;
    return v;
}

/* Sets the pointer from the P command in validation string v.  The
 * owner's slot must be paged in. */
static int p_command(uint32_t v)
{
    struct wimp_input *p = in();
    uint32_t q = find_cmd(v, 'P');
    if (!q)
        return 0;
    unsigned n = 0;
    int comma = 0;
    for (;;) {
        uint32_t ch = ros_ld8(q++);
        if (ch == ',') {
            comma = 1;
            break;
        }
        if (ch == ';' || ch <= ' ')
            break;
        if (n < sizeof p->ptr_sprite - 1)
            p->ptr_sprite[n++] = (char)ch;
    }
    p->ptr_sprite[n] = 0;
    uint32_t x = 0, y = 0;
    if (comma) {
        x = getnumber(&q);
        if (ros_ld8(q++) == ',')
            y = getnumber(&q);
    }
    setptr(p->ptr_sprite, 1, x, y);
    p->special = 1;
    return 1;
}

/* Called when a new window or icon is under the pointer, outside drags.
 * The pointer takes the shape from the icon's P command, or goes back to
 * the default.  A menu's timer is restarted too, by wimp_menu_pointer. */
static void pointer_icon(struct wimp_window *win, int32_t icon)
{
    struct wimp_input *p = in();
    uint32_t h = win ? win->handle : 0xFFFFFFFFu;
    if (h == p->ptr_w && icon == p->ptr_i)
        return;
    p->ptr_w = h, p->ptr_i = icon;
    int set = 0;
    if (win && icon >= 0) {
        const struct ros_slot *was = NULL;
        int paged = 0;
        if (win->handle == wimp_ws()->iconbar) {
            struct wimp_task *t = wimp_task_by_handle(wimp_iconbar_icon_task(icon), 0);
            was = ros_task_page_in(t ? t->rt : NULL);
            paged = 1;
        } else {
            was = page_win(win);
            paged = 1;
        }
        const uint8_t *ic = wimp_icon(win, (uint32_t)icon);
        if (ic && (rd(ic, 16) & 1u) && (rd(ic, 16) & (1u << 8)))
            set = p_command(rd(ic, 24));
        if (paged)
            ros_task_page_back(was);
    }
    if (!set && p->special) {
        p->special = 0;
        double_off();
    }
}

/* Wimp_DeleteWindow: a pending press in the window is forgotten
 * (s/Wimp02:4348) */
void wimp_input_window_gone(uint32_t handle)
{
    struct wimp_input *p = in();
    if (p->pend.window != handle)
        return;
    if (p->pend.flags & P_DOUBLE)
        double_off();
    p->pend.flags = 0;
}

/* update_pointer_shape_for_icon_create.  An icon created under the
 * pointer (tested by its block's box, in the window's work area) ends a
 * press's pending actions, except a pending drag.  The pointer then takes
 * its shape from the icon's P command. */
void wimp_input_icon_created(struct wimp_window *win, uint32_t block)
{
    struct wimp_input *p = in();
    int32_t x = p->mx - wimp_origin_x(win, APP), y = p->my - wimp_origin_y(win, APP);
    int32_t x0 = (int32_t)ros_ld32(block + 4), y0 = (int32_t)ros_ld32(block + 8);
    int32_t x1 = (int32_t)ros_ld32(block + 12), y1 = (int32_t)ros_ld32(block + 16);
    if (x < x0 || y < y0 || x >= x1 || y >= y1)
        return;
    if (p->pend.flags & P_DOUBLE)
        double_off();
    p->pend.flags &= P_DRAG;
    uint32_t f = ros_ld32(block + 20);
    if (!(f & 1u) || !(f & (1u << 8)))
        return;
    const struct ros_slot *was = page_win(win);
    p_command(ros_ld32(block + 28));
    ros_task_page_back(was);
}

/* ---- the mouse ------------------------------------------------------------- */

static void read_mouse(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_input *p = in();
    if (p->reuse) {
        p->reuse = 0;
        return;
    }
    p->oldb = p->mb;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_Mouse);
    p->mx = (int32_t)c.r[0] & ~(w->dx - 1);
    p->my = (int32_t)c.r[1] & ~(w->dy - 1);
    p->mb = c.r[2] & 7u;
    p->mt = c.r[3] ? c.r[3] : now_cs();
}

/* ---- hit testing ---------------------------------------------------- */

static int inside(struct wimp_box b, int32_t x, int32_t y)
{
    return b.x0 <= x && x < b.x1 && b.y0 <= y && y < b.y1;
}

static int in_work(const struct wimp_window *win, int32_t x, int32_t y)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_box v = win->s[APP].vis;
    return v.x0 - w->dx <= x && x <= v.x1 && v.y0 - w->dy <= y && y <= v.y1;
}

static int icon_at(const struct wimp_window *win, int32_t x, int32_t y, int shaded)
{
    int32_t rx = x - wimp_origin_x(win, APP), ry = y - wimp_origin_y(win, APP);
    for (int32_t i = (int32_t)win->nicons - 1; i >= 0; i--) {
        const uint8_t *ic = wimp_icon(win, (uint32_t)i);
        int32_t b[4];
        memcpy(b, ic, 16);
        uint32_t f = rd(ic, 16);
        if (f & IF_DELETED)
            continue;
        if ((f & IF_SHADED) && !(win->owner == NO_WINDOW || (shaded && (win->def[39] & 0x10u))))
            continue;
        if (b[0] <= rx && rx < b[2] && b[1] <= ry && ry < b[3])
            return i;
    }
    return -1;
}

static struct wimp_window *hit_in(struct wimp_window *first, int32_t x, int32_t y, int border_only,
                                  int shaded, int32_t *icon)
{
    for (struct wimp_window *win = first; win; win = wimp_window_below(win, APP)) {
        if (!win->s[APP].open || !inside(win->s[APP].outline, x, y))
            continue;
        if (border_only && !(win->s[APP].flags & F_INBORDER))
            continue;
        int work = in_work(win, x, y);
        struct wimp_window *c = hit_in(wimp_stack_front(win->handle, APP), x, y, !work, shaded, icon);
        if (c)
            return c;
        *icon = work ? icon_at(win, x, y, shaded) : -2;
        return win;
    }
    return NULL;
}

struct wimp_window *wimp_hit(int32_t x, int32_t y, int shaded, int32_t *icon)
{
    *icon = -1;
    return hit_in(wimp_stack_front(NO_WINDOW, APP), x, y, 0, shaded, icon);
}

static int gt(struct wimp_box b, int32_t x, int32_t y)
{
    return b.x0 < x && x < b.x1 && b.y0 < y && y < b.y1;
}

/* Refines a point in a window's border to the furniture gadget there */
int32_t wimp_gadget(const struct wimp_window *win, int32_t x, int32_t y)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_furniture *fu = &w->furn;
    struct wimp_box O = win->s[APP].outline;
    uint32_t f = win->s[APP].flags;
    int32_t T1 = fu->T + w->dy, V1 = fu->V + w->dx, left = O.x0;
    int iconise = (f & F_CLOSE) && win->s[APP].parent == NO_WINDOW;
    int32_t right = O.x1 - ((f & F_TOGGLE) ? fu->V : 0);
    if ((f & F_BACK) && gt((struct wimp_box){ O.x0, O.y1 - T1, O.x0 + fu->B + w->dx, O.y1 }, x, y))
        return -2;
    if (f & F_BACK)
        left += fu->B;
    if ((f & F_CLOSE) && gt((struct wimp_box){ left, O.y1 - T1, left + fu->C + w->dx, O.y1 }, x, y))
        return -3;
    if (f & F_CLOSE)
        left += fu->C;
    if ((f & F_TITLE) && gt((struct wimp_box){ left, O.y1 - T1, right - (iconise ? fu->I : 0), O.y1 }, x, y))
        return -4;
    if ((f & F_TOGGLE) && gt((struct wimp_box){ O.x1 - V1, O.y1 - T1, O.x1, O.y1 }, x, y))
        return -5;
    struct wimp_box bar, well, s;
    if (f & F_VBAR) {
        wimp_scroll_geom(win, 1, &bar, &well, &s);
        if (gt(bar, x, y)) {
            if (bar.y1 - bar.y0 < fu->U + fu->D)
                return y >= (bar.y0 + bar.y1) / 2 ? -6 : -8;
            if (y >= bar.y1 - fu->U) return -6;
            if (y < bar.y0 + fu->D) return -8;
            return -7;
        }
    }
    if ((f & F_SIZE) && gt((struct wimp_box){ O.x1 - V1, O.y0, O.x1, O.y0 + fu->H + w->dy }, x, y))
        return -9;
    if (f & F_HBAR) {
        wimp_scroll_geom(win, 0, &bar, &well, &s);
        if (gt(bar, x, y)) {
            if (bar.x1 - bar.x0 < fu->L + fu->R)
                return x < (bar.x0 + bar.x1) / 2 ? -10 : -12;
            if (x < bar.x0 + fu->L) return -10;
            if (x >= bar.x1 - fu->R) return -12;
            return -11;
        }
    }
    if (iconise && gt((struct wimp_box){ right - fu->I - w->dx, O.y1 - T1, right, O.y1 }, x, y))
        return -14;
    return -13;
}

/* The window handle a task is told: -1 for the back windows, and -2 for
 * the iconbar */
uint32_t wimp_reported(const struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    if (win && win->handle == w->iconbar)
        return 0xFFFFFFFEu;
    if (!win || win->handle == w->back_window || (win->s[APP].flags & F_BACKWIN && win->owner == 0) ||
        (w->old_back && win->handle == w->old_back))
        return 0xFFFFFFFFu;
    return win->handle;
}

/* ---- Wimp_GetPointerInfo -------------------------------------------------------- */

void wimp_swi_GetPointerInfo(struct ros_cpu *s)
{
    struct wimp_input *p = in();
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    int32_t icon;
    struct wimp_window *win = wimp_hit(p->mx, p->my, 1, &icon);
    if (win && icon == -2)                      /* no gadget while single-tasking */
        icon = wimp_ws()->singletask >= 0 ? -1 : wimp_gadget(win, p->mx, p->my);
    uint32_t b = s->r[1];
    ros_st32(b, (uint32_t)p->mx);
    ros_st32(b + 4, (uint32_t)p->my);
    ros_st32(b + 8, p->mb);
    ros_st32(b + 12, win ? wimp_reported(win) : 0xFFFFFFFFu);
    ros_st32(b + 16, win ? (uint32_t)icon : 0xFFFFFFFFu);
    s->v = 0;
}

/* ---- events ------------------------------------------------------------------------------- */

static struct wimp_task *owner_of(const struct wimp_window *win)
{
    if (!win || (int32_t)win->owner <= 0)
        return NULL;
    return wimp_task_by_handle(win->owner, 0);
}

static int deliver(struct wimp_task *t, uint32_t reason, const uint8_t *data, uint32_t size,
                   struct wimp_task **to, struct wimp_event *ev)
{
    ev->reason = reason;
    ev->size = size;
    memcpy(ev->data, data, size);
    ev->set_r2 = 1;
    ev->r2 = 0;
    *to = t;
    return 1;
}

/* Exit_OpenWindow, for a proposed window state.  Nothing happens if it
 * matches the current state.  A window whose owner is not a task, or
 * masks the event, is opened by the Wimp.  Otherwise this returns 1 with
 * Open_Window_Request in ev. */
static int exit_open(struct wimp_window *win, uint8_t *b, struct wimp_task **to, struct wimp_event *ev)
{
    const struct wimp_place *pl = &win->s[REQ];
    struct wimp_box v = wimp_round_box((struct wimp_box){ (int32_t)rd(b, 4), (int32_t)rd(b, 8),
                                                         (int32_t)rd(b, 12), (int32_t)rd(b, 16) });
    struct wimp_box c = wimp_round_box(pl->vis);
    if (v.x0 == c.x0 && v.y0 == c.y0 && v.x1 == c.x1 && v.y1 == c.y1 &&
        wimp_round_x((int32_t)rd(b, 20)) == wimp_round_x(pl->scx) &&
        wimp_round_y((int32_t)rd(b, 24)) == wimp_round_y(pl->scy) && rd(b, 28) == wimp_behind(win, REQ) &&
        !(pl->flags & ST_TOGGLING))
        return 0;
    struct wimp_task *t = owner_of(win);
    if (!t || (t->mask & (1u << 2))) {
        wimp_open_own(win, b);
        return 0;
    }
    return deliver(t, 2, b, 32, to, ev);
}

static void state_block(const struct wimp_window *win, uint8_t *b)
{
    const struct wimp_place *pl = &win->s[REQ];
    wr(b, 0, win->handle);
    wr(b, 4, (uint32_t)pl->vis.x0);
    wr(b, 8, (uint32_t)pl->vis.y0);
    wr(b, 12, (uint32_t)pl->vis.x1);
    wr(b, 16, (uint32_t)pl->vis.y1);
    wr(b, 20, (uint32_t)pl->scx);
    wr(b, 24, (uint32_t)pl->scy);
    wr(b, 28, wimp_behind(win, REQ));
}

/* ---- the drag box ---------------------------------------------------------------- */

static void vdu(const uint8_t *b, uint32_t n)
{
    wimp_vdu_bytes(b, n);
}

static void plot(uint32_t op, int32_t x, int32_t y)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = op, c.r[1] = (uint32_t)x, c.r[2] = (uint32_t)y;
    ros_swi(&c, XOS_Plot);
}

static int32_t clampc(int32_t v)
{
    return v < -0x8000 ? -0x8000 : v > 0x7F00 ? 0x7F00 : v;
}

static void dashed(struct wimp_box b, uint8_t pattern)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x80808000u, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XColourTrans_SetGCOL);
    uint8_t v[10] = { 23, 6, pattern, pattern, pattern, pattern, pattern, pattern, pattern, pattern };
    vdu(v, 10);
    int32_t x0 = clampc(b.x0), y0 = clampc(b.y0), x1 = clampc(b.x1) - w->dx, y1 = clampc(b.y1) - w->dy;
    plot(4, x0, y0);
    plot(0x15, x1, y0);
    plot(0x35, x1, y1);
    plot(0x35, x0, y1);
    plot(0x3D, x0, y0);
}

static int visible_box(const struct wimp_drag *d)
{
    if (d->type == 7)
        return 0;
    if (d->type <= 4 || d->type == 12) {
        uint32_t bit = d->type == 12 ? 3 : d->type - 1;
        if (in()->wimpflags & (1u << bit))
            return 0;                           /* a continuous drag: nothing is drawn */
    }
    return 1;
}

static void routine(uint32_t addr, struct wimp_box b, struct wimp_box old, struct wimp_box *out)
{
    if (addr == 0 || addr == 0xFFFFFFFFu)
        return;
    struct wimp_drag *d = &in()->d;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = (uint32_t)b.x0, c.r[1] = (uint32_t)b.y0, c.r[2] = (uint32_t)b.x1, c.r[3] = (uint32_t)b.y1;
    c.r[4] = (uint32_t)old.x0, c.r[5] = (uint32_t)old.y0, c.r[6] = (uint32_t)old.x1, c.r[7] = (uint32_t)old.y1;
    c.r[12] = d->r12;
    c.mode = ROS_MODE_SVC;
    ros_call(&c, addr);
    if (out)
        *out = (struct wimp_box){ (int32_t)c.r[0], (int32_t)c.r[1], (int32_t)c.r[2], (int32_t)c.r[3] };
}

static void box_draw(int on)
{
    struct wimp_input *p = in();
    struct wimp_drag *d = &p->d;
    if (!d->type || !visible_box(d) || d->drawn == on)
        return;
    wimp_default_windows();
    if (d->type >= 8 && d->type <= 11)
        routine(on ? d->draw : d->remove, d->box, d->box, on ? &d->box : NULL);
    else if (d->type != 13)
        dashed(d->box, p->dash1);
    d->drawn = on;
}

/* The drag box is removed before the Wimp draws, and drawn again after */
void wimp_drag_hide(void)
{
    box_draw(0);
}

void wimp_drag_show(void)
{
    box_draw(1);
}

static void confine(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    struct wimp_ws *w = wimp_ws();
    int32_t v[4] = { x0, y0, x1 - 1, y1 - 1 };
    for (int i = 0; i < 4; i++)
        v[i] = v[i] < -0x7FFF ? -0x7FFF : v[i] > 0x7FFF ? 0x7FFF : v[i];
    uint8_t *b = w->scratch;
    b[0] = 1;
    for (int i = 0; i < 4; i++)
        b[1 + 2 * i] = (uint8_t)v[i], b[2 + 2 * i] = (uint8_t)(v[i] >> 8);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 21, c.r[1] = ros_addr(b);
    ros_swi(&c, XOS_Word);
}

static void unconfine(void)
{
    struct wimp_ws *w = wimp_ws();
    confine(0, 0, w->screen_w, w->screen_h);
}

/* pointeron and pointeroff: *Pointer and *Pointer 0, as 5.30 does it.  So
 * turning the pointer on programs ptr_default and frees the mouse. */
static void pointer_on(int on)
{
    struct wimp_ws *w = wimp_ws();
    char *cmd = (char *)w->scratch + 480;
    strcpy(cmd, on ? "Pointer" : "Pointer 0");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(cmd);
    ros_swi(&c, XOS_CLI);
}

/* The end of int_allbutmode.  The pointer is turned on, unless a type 12
 * drag has hidden it, and the mouse-ahead buffer is flushed. */
void wimp_input_mode_set(void)
{
    if (in()->d.type != 12)
        pointer_on(1);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 21, c.r[1] = 9;
    ros_swi(&c, XOS_Byte);
}

void wimp_drag_cancel(void)
{
    struct wimp_input *p = in();
    if (!p->d.type)
        return;
    if (p->d.type == 13)
        wimp_iconscroll_stop();
    box_draw(0);
    if (p->d.type == 12)
        pointer_on(1);
    unconfine();
    p->d.type = 0;
    p->d.window = 0;
}

/* dragbox_icon_select_start: a type 13 drag in the caret's icon.  The
 * pointer is kept inside the icon's box on screen. */
void wimp_drag_selection(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_input *p = in();
    if (p->pend.flags & P_DOUBLE) {             /* a pending double click is cancelled */
        p->pend.flags &= ~P_DOUBLE;
        double_off();
    }
    wimp_drag_cancel();
    struct wimp_window *win = wimp_window(w->caret.w);
    if (!win || (uint32_t)w->caret.i >= win->nicons)
        return;
    const uint8_t *ic = wimp_icon(win, (uint32_t)w->caret.i);
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    struct wimp_box v = win->s[APP].vis, s = wimp_screen_box();
    struct wimp_box b = { ox + (int32_t)rd(ic, 0), oy + (int32_t)rd(ic, 4), ox + (int32_t)rd(ic, 8),
                          oy + (int32_t)rd(ic, 12) };
    if (b.x0 < v.x0) b.x0 = v.x0;
    if (b.x1 > v.x1) b.x1 = v.x1;
    if (b.y1 > v.y1) b.y1 = v.y1;
    if (b.y0 < v.y0) b.y0 = v.y0;
    if (b.x0 < s.x0) b.x0 = s.x0;
    if (b.y0 < s.y0) b.y0 = s.y0;
    if (b.x1 > s.x1) b.x1 = s.x1;
    if (b.y1 > s.y1) b.y1 = s.y1;
    confine(b.x0, b.y0, b.x1, b.y1);
    struct wimp_drag *d = &p->d;
    d->type = 13;
    d->window = 0, d->task = 0, d->flags = 0;
    d->drawn = 0;
    d->lx = p->mx, d->ly = p->my;
    wimp_iconscroll_start(1);
}

/* The caret moving to another window or icon ends a type 13 drag */
void wimp_drag_selection_cancel(void)
{
    struct wimp_input *p = in();
    p->d.type = 0;
    unconfine();
    wimp_cnp_drag_end();
}

/* Starts a drag at the stored mouse position */
static os_error *start_drag(uint32_t type, struct wimp_window *win, struct wimp_box box,
                            struct wimp_box parent, uint32_t flags, uint32_t task)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_input *p = in();
    if (p->pend.flags & P_DOUBLE) {             /* a pending double click is cancelled */
        p->pend.flags &= ~P_DOUBLE;
        double_off();
    }
    wimp_drag_cancel();
    struct wimp_drag *d = &p->d;
    int32_t sx = p->mx, sy = p->my;
    if (type <= 4 || type == 12)
        flags &= ~3u;
    if ((type == 1 || type == 2) && !(win->s[APP].flags & F_MOVEABLE))
        return NULL;                            /* not moveable: no drag, no error */
    d->off = (struct wimp_box){ 0, 0, 0, 0 };
    if (type == 7) {
        box = (struct wimp_box){ sx, sy, sx, sy };
    } else if (type == 1 || type == 2) {
        box = win->s[APP].outline;
        struct wimp_box v = win->s[APP].vis;
        d->off = (struct wimp_box){ v.x0 - box.x0, v.y0 - box.y0, v.x1 - box.x1, v.y1 - box.y1 };
    } else if (type == 3 || type == 4 || type == 12) {
        struct wimp_box bar, well, s;
        if (type != 3) {
            wimp_scroll_geom(win, 1, &bar, &well, &s);
            box = s;
            parent = well;
        }
        if (type != 4) {
            wimp_scroll_geom(win, 0, &bar, &well, &s);
            if (type == 3) {
                box = s;
                parent = well;
            } else {
                box.x0 = s.x0, box.x1 = s.x1;
                parent.x0 = well.x0, parent.x1 = well.x1;
            }
        }
    }
    int bounded = 1;
    if (type == 1 || type == 2) {
        int nochecks = (win->s[APP].flags & F_NOCHECKS) || (p->wimpflags & 0x40u);
        struct wimp_window *pw = win->s[APP].parent == NO_WINDOW ? NULL : wimp_window(win->s[APP].parent);
        if (type == 1) {
            if (!pw) {
                if (nochecks)
                    bounded = 0;
                parent = wimp_screen_box();
            } else {
                parent = pw->s[APP].vis;
            }
        } else {
            /* the largest size is the extent, but no larger than the screen */
            int32_t ex0 = (int32_t)rd(win->def, 40), ey0 = (int32_t)rd(win->def, 44);
            int32_t ex1 = (int32_t)rd(win->def, 48), ey1 = (int32_t)rd(win->def, 52);
            int32_t maxw = ex1 - ex0 - d->off.x0 + d->off.x1, maxh = ey1 - ey0 + d->off.y0 - d->off.y1;
            if (maxw > w->screen_w) maxw = w->screen_w;
            if (maxh > w->screen_h) maxh = w->screen_h;
            parent = (struct wimp_box){ box.x0, box.y1 - maxh, box.x0 + maxw, box.y1 };
            if (!pw && !nochecks) {
                if (parent.y0 < 0) parent.y0 = 0;
                if (parent.x1 > w->screen_w) parent.x1 = w->screen_w;
            }
        }
    }
    d->type = type;
    d->flags = flags;
    p->drag_sent_ok = 0;
    d->window = (type <= 4 || type == 12 || (flags & 3u)) && win ? win->handle : 0;
    d->task = task;
    d->box = box;
    d->lx = sx, d->ly = sy;
    d->drawn = 0;
    if (type == 12)
        pointer_on(0);
    if (bounded) {
        int32_t px0 = parent.x0 + (sx - box.x0), py0 = parent.y0 + (sy - box.y0);
        int32_t px1 = parent.x1 + (sx - box.x1) + w->dx, py1 = parent.y1 + (sy - box.y1) + w->dy;
        if (type == 2) {
            /* only x1 and y0 move, between the minimum and maximum sizes */
            int32_t mw, mh;
            wimp_min_size(win, &mw, &mh);
            mw += d->off.x0 - d->off.x1, mh += d->off.y1 - d->off.y0;
            px0 = sx + (box.x0 + mw - box.x1);
            px1 = sx + (parent.x1 - box.x1) + w->dx;
            py0 = sy + (parent.y0 - box.y0);
            py1 = sy + (box.y1 - mh - box.y0) + w->dy;
        }
        confine(px0, py0, px1, py1);
    } else {
        unconfine();
    }
    box_draw(1);
    return NULL;
}

/* ---- Wimp_DragBox ---------------------------------------------------------------- */

void wimp_swi_DragBox(struct ros_cpu *s)
{
    struct wimp_input *p = in();
    uint32_t flags = 0;
    if (s->r[2] == TASK_WORD) {
        flags = s->r[3];
        s->r[2] = 0;
    }
    s->v = 0;
    if (s->r[1] == 0 || s->r[1] == 0xFFFFFFFFu) {
        wimp_drag_cancel();
        return;
    }
    uint32_t b = s->r[1], type = ros_ld32(b + 4), handle = ros_ld32(b);
    struct wimp_window *win = NULL;
    if (type <= 4 || type == 12 || (flags & 3u)) {
        win = handle == 0xFFFFFFFEu ? wimp_window(wimp_ws()->iconbar) : wimp_window(handle);
        if (!win) {
            wimp_fail(s, wimp_error(E_BAD_HANDLE));
            return;
        }
    }
    if (type < 1 || type > 13) {
        wimp_drag_cancel();                     /* 5.30 gives undefined results: cancel */
        return;
    }
    if (type == 13) {
        wimp_drag_selection();
        return;
    }
    if (type >= 8 && type <= 11) {
        p->d.r12 = ros_ld32(b + 40);
        p->d.draw = ros_ld32(b + 44);
        p->d.remove = ros_ld32(b + 48);
        p->d.move = ros_ld32(b + 52);
    }
    struct wimp_box box = { (int32_t)ros_ld32(b + 8), (int32_t)ros_ld32(b + 12), (int32_t)ros_ld32(b + 16),
                            (int32_t)ros_ld32(b + 20) };
    struct wimp_box parent = { (int32_t)ros_ld32(b + 24), (int32_t)ros_ld32(b + 28),
                               (int32_t)ros_ld32(b + 32), (int32_t)ros_ld32(b + 36) };
    struct wimp_task *me = wimp_current();
    uint32_t task = (type <= 4 || type == 12) ? (win ? win->owner : 0) : (me ? me->handle : 0);
    os_error *e = start_drag(type, win, box, parent, flags, task);
    if (e)
        wimp_fail(s, e);
}

/* ---- each poll during a drag ------------------------------------------------ */

static int32_t new_scx(struct wimp_window *win, struct wimp_box s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_box bar, well, cur;
    wimp_scroll_geom(win, 0, &bar, &well, &cur);
    int32_t mx = w->furn.L + 2 * (w->dx > w->dy ? w->dx : w->dy);
    int32_t r0 = s.x0 - bar.x0 - mx;
    if (r0 < 0) r0 = 0;
    int32_t ex0 = (int32_t)rd(win->def, 40), ex1 = (int32_t)rd(win->def, 48);
    struct wimp_box v = win->s[APP].vis;
    int32_t E = (ex1 - ex0) - (v.x1 - v.x0);
    int32_t span = (bar.x1 - bar.x0) - (s.x1 - s.x0);
    if (r0 > span) r0 = span;
    span -= 2 * mx;
    return ex0 + wimp_muldiv(r0, E, span);
}

static int32_t new_scy(struct wimp_window *win, struct wimp_box s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_box bar, well, cur;
    wimp_scroll_geom(win, 1, &bar, &well, &cur);
    int32_t my = w->furn.U + 2 * (w->dx > w->dy ? w->dx : w->dy);
    int32_t r0 = bar.y1 - s.y1 - my;
    if (r0 < 0) r0 = 0;
    int32_t ey0 = (int32_t)rd(win->def, 44), ey1 = (int32_t)rd(win->def, 52);
    struct wimp_box v = win->s[APP].vis;
    int32_t E = (ey1 - ey0) - (v.y1 - v.y0);
    int32_t span = (bar.y1 - bar.y0) - (s.y1 - s.y0);
    if (r0 > span) r0 = span;
    span -= 2 * my;
    return ey1 - wimp_muldiv(r0, E, span);
}

/* The behind handle for a position or size drag, decided by the buttons */
static uint32_t drag_behind(struct wimp_window *win, uint32_t buttons)
{
    if (buttons & 1u)
        return wimp_behind(win, REQ);
    uint32_t f = win->s[REQ].flags;
    if ((f & F_PANE) || ((f & F_FOREGROUND) && win->s[REQ].parent == NO_WINDOW))
        return 0xFFFFFFFFu;
    for (struct wimp_window *a = wimp_stack_front(win->s[REQ].parent, REQ); a && a != win;
         a = wimp_window_below(a, REQ)) {
        uint32_t af = a->s[REQ].flags;
        if (!((af & F_PANE) || ((af & F_FOREGROUND) && a->s[REQ].parent == NO_WINDOW)))
            return 0xFFFFFFFFu;
    }
    return wimp_behind(win, REQ);
}

static int drag_result(struct wimp_task **to, struct wimp_event *ev, int continuous)
{
    struct wimp_input *p = in();
    struct wimp_drag *d = &p->d;
    uint32_t type = d->type;
    struct wimp_box b = { d->box.x0 + d->off.x0, d->box.y0 + d->off.y0, d->box.x1 + d->off.x1,
                          d->box.y1 + d->off.y1 };
    struct wimp_window *win = d->window ? wimp_window(d->window) : NULL;
    uint8_t blk[36];
    if (type >= 5 && type <= 9) {
        wr(blk, 0, (uint32_t)b.x0);
        wr(blk, 4, (uint32_t)b.y0);
        wr(blk, 8, (uint32_t)b.x1);
        wr(blk, 12, (uint32_t)b.y1);
        struct wimp_task *t = wimp_task_by_handle(d->task, 0);
        if (!t)
            return 0;
        return deliver(t, 7, blk, 16, to, ev);
    }
    if (!win)
        return 0;
    state_block(win, blk);
    if (type == 3 || type == 4 || type == 12) {
        if (type != 4)
            wr(blk, 20, (uint32_t)new_scx(win, d->box));
        if (type != 3)
            wr(blk, 24, (uint32_t)new_scy(win, d->box));
        return exit_open(win, blk, to, ev);
    }
    if (type == 2)
        win->s[REQ].flags |= ST_FORCE;
    wr(blk, 4, (uint32_t)b.x0);
    wr(blk, 8, (uint32_t)b.y0);
    wr(blk, 12, (uint32_t)b.x1);
    wr(blk, 16, (uint32_t)b.y1);
    wr(blk, 28, drag_behind(win, p->oldb));
    if (continuous) {
        /* A window that may not leave the screen is put back on it when
         * its owner opens it.  So its state never matches a proposal at
         * the edge, and the same request used to go on every poll, tens
         * of thousands a second (#135).  RISC OS sends a request again
         * only when the drag moves, so the same proposal is never sent
         * twice. */
        if (p->drag_sent_ok && !memcmp(p->drag_sent, blk, 32))
            return 0;
        memcpy(p->drag_sent, blk, 32);
        p->drag_sent_ok = 1;
    }
    return exit_open(win, blk, to, ev);
}

/* One poll's step of a drag.  Returns 1 with an event, 2 for a null event
 * (the end of a selection drag), and 3 for no event while a selection
 * drag is held (5.30 repolls).  *busy is set when a drag is running and
 * the poll goes on to keys. */
static int drag_step(struct wimp_task **to, struct wimp_event *ev, int *busy)
{
    struct wimp_input *p = in();
    struct wimp_drag *d = &p->d;
    *busy = 0;
    if (!d->type)
        return 0;
    if (d->type == 13) {                        /* a selection drag */
        int r = wimp_cnp_drag_step(p->mx, p->my, p->mb);
        if (!r)
            return 3;                           /* repoll: nothing else runs meanwhile */
        d->type = 0;
        unconfine();
        wimp_cnp_drag_end();
        return r == 1 ? 2 : 0;
    }
    int32_t ddx = p->mx - d->lx, ddy = p->my - d->ly;
    d->lx = p->mx, d->ly = p->my;
    struct wimp_box old = d->box;
    if (d->type == 2 || d->type == 6 || d->type == 9 || d->type == 11) {
        d->box.x1 += ddx, d->box.y0 += ddy;
    } else {
        d->box.x0 += ddx, d->box.x1 += ddx, d->box.y0 += ddy, d->box.y1 += ddy;
    }
    if (d->type == 10 || d->type == 11 || p->mb) {
        if (ddx || ddy) {
            if (visible_box(d) && d->drawn) {
                wimp_default_windows();
                if (d->type >= 8 && d->type <= 11 && d->move && d->move != 0xFFFFFFFFu) {
                    routine(d->move, d->box, old, &d->box);
                } else {
                    struct wimp_box nb = d->box;
                    d->box = old;
                    box_draw(0);
                    d->box = nb;
                    box_draw(1);
                }
            }
        }
        if (d->type <= 4 || d->type == 12) {
            uint32_t bit = d->type == 12 ? 3 : d->type - 1;
            if (p->wimpflags & (1u << bit)) {
                /* continuous: a request is made on every poll, but it is
                 * sent only when the state would change */
                struct wimp_drag keep = *d;
                int r = drag_result(to, ev, 1);
                *d = keep;
                if (r)
                    return 1;
            }
        }
        *busy = d->type != 10 && d->type != 11;
        return 0;
    }
    box_draw(0);                                /* the drag ends */
    if (d->type == 12)
        pointer_on(1);
    unconfine();
    int r = drag_result(to, ev, 0);
    d->type = 0;
    p->reuse = 0;
    return r;
}

/* ---- the furniture ------------------------------------------------------------------- */

static void iconise_messages(struct wimp_window *win, uint32_t flags)
{
    struct wimp_input *p = in();
    uint8_t b[48];
    memset(b, 0, sizeof b);
    wr(b, 0, 40);
    wr(b, 16, 0x400D0u);
    wr(b, 20, win->handle);
    wr(b, 24, win->owner);
    wr(b, 28, (uint32_t)p->mx);
    wr(b, 32, (uint32_t)(p->my - 49));
    wr(b, 36, flags);
    wimp_queue_message(17, b, 40, RECV_BROADCAST, 0, 0);
    memset(b, 0, sizeof b);
    wr(b, 0, 48);
    wr(b, 16, 0x400CAu);
    wr(b, 20, win->handle);
    wr(b, 24, win->owner);
    /* the title's leaf: the part of its first word after the last '.' */
    uint32_t tf = rd(win->def, 56), t = (tf & (1u << 8)) ? rd(win->def, 72) : ros_addr((void *)(win->def + 72));
    uint32_t max = (tf & (1u << 8)) ? 256 : 12, i = 0;
    while (i < max && ros_ld8(t + i) == ' ')
        i++;
    uint32_t start = i, leaf = i;
    while (i < max && ros_ld8(t + i) > ' ') {
        if (ros_ld8(t + i) == '.')
            leaf = i + 1;
        i++;
    }
    if (leaf >= i)
        leaf = start;
    for (uint32_t k = 0; k < 19 && leaf + k < i; k++)
        b[28 + k] = (uint8_t)ros_ld8(t + leaf + k);
    wimp_queue_message(17, b, 48, RECV_BROADCAST, 0, 0);
}

static int toggle(struct wimp_window *win, uint32_t buttons, struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_place *pl = &win->s[REQ];
    uint8_t b[36];
    state_block(win, b);
    if (!(pl->flags & ST_TOGGLED)) {
        win->toggle_vis = pl->vis;
        win->toggle_scx = pl->scx, win->toggle_scy = pl->scy;
        win->toggle_behind = wimp_behind(win, REQ);
        int32_t ex0 = (int32_t)rd(win->def, 40), ey0 = (int32_t)rd(win->def, 44);
        int32_t ex1 = (int32_t)rd(win->def, 48), ey1 = (int32_t)rd(win->def, 52);
        struct wimp_box v = { pl->vis.x0, pl->vis.y1 - (ey1 - ey0), pl->vis.x0 + (ex1 - ex0), pl->vis.y1 };
        int shift = key_down(0x80) ^ ((cmos(0x1C) >> 4) & 1);
        if (shift && pl->parent == NO_WINDOW) {
            int32_t L = w->iconbar_height - 8 + ((pl->flags & F_HBAR) ? w->furn.H : 0);
            if (v.y0 < L) {
                v.y1 += L - v.y0, v.y0 = L;
                int32_t top = w->screen_h - ((pl->flags & F_TITLE) ? w->furn.T : 0);
                if (v.y1 > top) {
                    v.y1 = top;
                    pl->flags |= (1u << 22);
                }
            }
        }
        wr(b, 4, (uint32_t)v.x0), wr(b, 8, (uint32_t)v.y0), wr(b, 12, (uint32_t)v.x1), wr(b, 16, (uint32_t)v.y1);
        pl->flags |= ST_TOGGLING | ST_FORCE;
        wr(b, 28, drag_behind(win, buttons));
    } else {
        struct wimp_box v = win->toggle_vis;
        wr(b, 4, (uint32_t)v.x0), wr(b, 8, (uint32_t)v.y0), wr(b, 12, (uint32_t)v.x1), wr(b, 16, (uint32_t)v.y1);
        wr(b, 20, (uint32_t)win->toggle_scx), wr(b, 24, (uint32_t)win->toggle_scy);
        uint32_t behind = win->toggle_behind;
        if (behind != 0xFFFFFFFFu) {
            int found = 0;
            for (struct wimp_window *a = wimp_stack_front(pl->parent, REQ); a; a = wimp_window_below(a, REQ)) {
                if (a == win || a->handle == behind) {
                    found = 1;
                    break;
                }
                if (a->s[REQ].flags & F_BACKWIN)
                    break;
            }
            if (!found)
                behind = 0xFFFFFFFEu;
        }
        wr(b, 28, behind);
        pl->flags |= ST_TOGGLING;
    }
    return exit_open(win, b, to, ev);
}

static int scroll_req(struct wimp_window *win, int32_t xd, int32_t yd, struct wimp_task **to,
                      struct wimp_event *ev)
{
    uint8_t b[40];
    state_block(win, b);
    wr(b, 28, win->handle);
    wr(b, 32, (uint32_t)xd);
    wr(b, 36, (uint32_t)yd);
    struct wimp_task *t = owner_of(win);
    return t ? deliver(t, 10, b, 40, to, ev) : 0;
}

/* A gadget's action, once its button type has fired with these buttons */
static int furniture(struct wimp_window *win, int32_t g, uint32_t buttons, int drag,
                     struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_input *p = in();
    struct wimp_place *pl = &win->s[REQ];
    int adjust = (buttons & 1u) != 0;
    uint8_t b[36];
    switch (g) {
    case -4:                                    /* title */
        start_drag(1, win, (struct wimp_box){ 0, 0, 0, 0 }, (struct wimp_box){ 0, 0, 0, 0 }, 0, win->owner);
        return 0;
    case -9:
        start_drag(2, win, (struct wimp_box){ 0, 0, 0, 0 }, (struct wimp_box){ 0, 0, 0, 0 }, 0, win->owner);
        return 0;
    case -2: {                                  /* back */
        state_block(win, b);
        uint32_t behind = adjust ? 0xFFFFFFFFu : 0xFFFFFFFEu;
        if (key_down(0x80)) {
            if (!adjust) {
                behind = 0xFFFFFFFEu;
                for (struct wimp_window *a = wimp_window_below(win, REQ); a; a = wimp_window_below(a, REQ)) {
                    if (a->s[REQ].flags & F_BACKWIN)
                        break;
                    if (!(a->s[REQ].flags & (F_PANE | F_HOTKEYS))) {
                        behind = a->handle;
                        break;
                    }
                }
            } else {
                struct wimp_window *above[64];
                unsigned n = 0;
                for (struct wimp_window *a = wimp_stack_front(pl->parent, REQ); a && a != win && n < 64;
                     a = wimp_window_below(a, REQ))
                    above[n++] = a;
                behind = 0xFFFFFFFFu;
                int skipped = 0;
                while (n-- > 0) {
                    if (above[n]->s[REQ].flags & (F_PANE | F_HOTKEYS))
                        continue;
                    if (!skipped) {
                        skipped = 1;
                        continue;
                    }
                    behind = wimp_behind(above[n], REQ);
                    break;
                }
            }
        }
        wr(b, 28, behind);
        return exit_open(win, b, to, ev);
    }
    case -3: {                                  /* close */
        int ctrl = key_down(0x81), alt = key_down(0x82), shift = key_down(0x80);
        if (!adjust && ctrl && alt) {
            p->mb = p->pend.buttons;
            struct wimp_window *all[256];
            unsigned n = 0;
            for (struct wimp_window *a = wimp_stack_front(pl->parent, REQ); a && n < 256; a = wimp_window_below(a, REQ))
                if ((a->s[REQ].flags & F_CLOSE) && !(a->s[REQ].flags & F_HOTKEYS))
                    all[n++] = a;
            while (n-- > 0) {
                struct wimp_task *t = owner_of(all[n]);
                if (t) {
                    wr(b, 0, all[n]->handle);
                    wimp_queue_message(3, b, 4, RECV_TASK, t->handle, 0);
                }
            }
            return 0;
        }
        if (!adjust && shift) {
            if (pl->parent == NO_WINDOW)
                iconise_messages(win, 1);
            return 0;
        }
        p->mb = p->pend.buttons;
        struct wimp_task *t = owner_of(win);
        if (!t)
            return 0;
        wr(b, 0, win->handle);
        return deliver(t, 3, b, 4, to, ev);
    }
    case -14:
        iconise_messages(win, 0);
        return 0;
    case -5:
        return toggle(win, buttons, to, ev);
    case -6: case -8: case -10: case -12: {     /* arrows */
        int32_t xd = g == -10 ? -1 : g == -12 ? 1 : 0, yd = g == -6 ? 1 : g == -8 ? -1 : 0;
        if (adjust)
            xd = -xd, yd = -yd;
        if (pl->flags & F_SCROLLREQ)
            return scroll_req(win, xd, yd, to, ev);
        int32_t ex0 = (int32_t)rd(win->def, 40), ey0 = (int32_t)rd(win->def, 44);
        int32_t ex1 = (int32_t)rd(win->def, 48), ey1 = (int32_t)rd(win->def, 52);
        int32_t scx = pl->scx + 32 * xd, scy = pl->scy + 32 * yd;
        int32_t vw = pl->vis.x1 - pl->vis.x0, vh = pl->vis.y1 - pl->vis.y0;
        if ((xd < 0 && pl->scx <= ex0) || (xd > 0 && pl->scx >= ex1 - vw) || (yd > 0 && pl->scy >= ey1) ||
            (yd < 0 && pl->scy <= ey0 + vh))
            return 0;
        state_block(win, b);
        wr(b, 20, (uint32_t)scx);
        wr(b, 24, (uint32_t)scy);
        return exit_open(win, b, to, ev);
    }
    case -7: case -11: {                        /* the bars: on the sausage, or in the well */
        int vert = g == -7;
        struct wimp_box bar, well, s;
        wimp_scroll_geom(win, vert, &bar, &well, &s);
        int on = vert ? (p->pend.y >= s.y0 && p->pend.y < s.y1) : (p->pend.x >= s.x0 && p->pend.x < s.x1);
        if (on) {
            if (!drag) {
                uint32_t type = vert ? 4 : 3;
                if (adjust && (pl->flags & F_VBAR) && (pl->flags & F_HBAR))
                    type = 12;
                start_drag(type, win, s, well, 0, win->owner);
            }
            return 0;
        }
        int32_t dir = vert ? (p->pend.y >= s.y1 ? 1 : -1) : (p->pend.x >= s.x1 ? 1 : -1);
        if (adjust)
            dir = -dir;
        if (pl->flags & F_SCROLLREQ)
            return scroll_req(win, vert ? 0 : 2 * dir, vert ? 2 * dir : 0, to, ev);
        state_block(win, b);
        if (vert)
            wr(b, 24, (uint32_t)(pl->scy + dir * (pl->vis.y1 - pl->vis.y0)));
        else
            wr(b, 20, (uint32_t)(pl->scx + dir * (pl->vis.x1 - pl->vis.x0)));
        wr(b, 28, win->handle);
        return exit_open(win, b, to, ev);
    }
    }
    return 0;
}

/* The button type of a furniture gadget */
static uint32_t gadget_type(const struct wimp_window *win, int32_t g)
{
    int rel = in()->release_furniture;
    switch (g) {
    case -2: case -3: case -5: case -14: return rel ? 4 : 3;
    case -4: case -9: case -7: case -11: return 3;
    case -6: case -8: case -10: case -12: return (win->s[APP].flags & (1u << 9)) ? 3 : 2;
    default: return 0;
    }
}

/* ---- selection and Mouse_Click ----------------------------------------- */

static void set_flags(struct wimp_window *win, int32_t i, uint32_t eor, uint32_t bic)
{
    uint8_t *ic = wimp_icon(win, (uint32_t)i);
    uint32_t f = (rd(ic, 16) & ~bic) ^ eor;
    wr(ic, 16, f);
    wimp_icon_changed(win, (uint32_t)i, wimp_icon_in_place(win, f));
}

static void select_icon(struct wimp_window *win, int32_t i, uint32_t buttons)
{
    if (i < 0)
        return;
    uint32_t f = rd(wimp_icon(win, (uint32_t)i), 16), esg = (f >> 16) & 15;
    if (esg == 0 || ((f & (1u << 10)) && (buttons & 1u))) {
        set_flags(win, i, IF_SELECTED, 0);
        return;
    }
    for (uint32_t k = 0; k < win->nicons; k++) {
        uint32_t g = rd(wimp_icon(win, k), 16);
        if ((int32_t)k != i && ((g >> 16) & 15) == esg && (g & IF_SELECTED) && !(g & IF_DELETED))
            set_flags(win, (int32_t)k, 0, IF_SELECTED);
    }
    if (!(f & IF_SELECTED))
        set_flags(win, i, IF_SELECTED, 0);
}

static int click_at(struct wimp_window *win, int32_t icon, uint32_t buttons, int32_t x, int32_t y,
                    struct wimp_task **to, struct wimp_event *ev);
static uint32_t type_of(struct wimp_window *win, int32_t icon);

/* Reports the pending press, at the position of the press */
static int click(struct wimp_window *win, int32_t icon, uint32_t buttons, struct wimp_task **to,
                 struct wimp_event *ev)
{
    return click_at(win, icon, buttons, in()->pend.x, in()->pend.y, to, ev);
}

static int click_at(struct wimp_window *win, int32_t icon, uint32_t buttons, int32_t x, int32_t y,
                    struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_input *p = in();
    uint32_t rw = wimp_reported(win);
    int old_back = w->old_back && win->handle == w->old_back;
    if (rw == 0xFFFFFFFFu && !old_back)
        return 0;                               /* background or back window: dropped */
    if (icon >= 0 && !(buttons & 0x222u) && !p->early && type_of(win, icon) == 15) {
        if (buttons)                            /* type 15: the Wimp's own text selection */
            wimp_cnp_click(win, icon, buttons, x, y, p->pend.clicks);
        return 0;
    }
    struct wimp_task *t = owner_of(win);
    if (win->handle == w->iconbar && icon >= 0) {
        int32_t owner = wimp_iconbar_owner(icon);
        t = owner ? wimp_task_by_handle((uint32_t)owner, 0) : NULL;
    }
    if (!t || (t->mask & (1u << 6)))
        return 0;
    uint8_t b[24];
    wr(b, 0, (uint32_t)x);
    wr(b, 4, (uint32_t)y);
    wr(b, 8, buttons);
    wr(b, 12, rw);
    wr(b, 16, (uint32_t)icon);
    wr(b, 20, p->oldb);
    if (w->queue) {                             /* after the queued messages */
        wimp_queue_message(6, b, 24, RECV_TASK, t->handle, 0);
        return wimp_queue_next(to, ev);
    }
    return deliver(t, 6, b, 24, to, ev);
}

/* Reports the pending press: a furniture action, or Mouse_Click */
static int report(struct wimp_window *win, int32_t icon, uint32_t buttons, int drag, struct wimp_task **to,
                  struct wimp_event *ev)
{
    if (icon <= -2)
        return furniture(win, icon, in()->pend.buttons, drag, to, ev);
    return click(win, icon, buttons, to, ev);
}

static uint32_t type_of(struct wimp_window *win, int32_t icon)
{
    if (icon >= 0)
        return (rd(wimp_icon(win, (uint32_t)icon), 16) >> 12) & 15;
    if (icon == -1)
        return (rd(win->def, 60) >> 12) & 15;
    return gadget_type(win, icon);
}

static int dist(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int32_t a = x1 - x0, b = y1 - y0;
    return (a < 0 ? -a : a) + (b < 0 ? -b : b);
}

static int report_drag(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_input *p = in();
    p->pend.flags &= ~P_DRAG;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = (uint32_t)p->mx, c.r[1] = 0x52, c.r[2] = p->mb, c.r[3] = p->mt, c.r[4] = (uint32_t)p->my;
    ros_service_call(&c);                       /* Service_MouseTrap */
    p->mx = p->pend.x, p->my = p->pend.y;
    struct wimp_window *win = wimp_window(p->pend.window);
    if (!win)
        return 0;
    return report(win, p->pend.icon, p->pend.buttons << 4, 1, to, ev);
}

/* A new press */
static int press(struct wimp_window *win, int32_t icon, uint32_t edges, struct wimp_task **to,
                 struct wimp_event *ev)
{
    struct wimp_input *p = in();
    uint32_t type = type_of(win, icon);
    uint8_t props = types[type].props;
    if (!(props & T_NOPRESS) && !edges)
        return 0;
    p->pend = (struct wimp_pending){ p->mx, p->my, edges, win->handle, icon, p->mt, types[type].pend, 0, 0, 1 };
    if (p->pend.flags & P_REPEAT) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 196, c.r[1] = 0, c.r[2] = 255;
        ros_swi(&c, XOS_Byte);
        p->pend.wait = c.r[1] & 0xFFu;
        p->pend.rate = c.r[2] & 0xFFu;
    }
    if (p->pend.flags & P_DOUBLE) {             /* the double-click pointer */
        if (type == 15)
            double_writable_on(win, icon);
        else
            double_on();
    }
    if ((props & T_WRITABLE) && icon >= 0 && type == 14) {
        int32_t rx = p->mx - wimp_origin_x(win, APP), ry = p->my - wimp_origin_y(win, APP);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = win->handle, c.r[1] = (uint32_t)icon, c.r[2] = (uint32_t)rx, c.r[3] = (uint32_t)ry;
        c.r[4] = 0xFFFFFFFFu, c.r[5] = 0xFFFFFFFFu;
        ros_swi(&c, XWimp_SetCaretPosition);
    }
    if (!(props & T_REPORTS))
        return 0;
    if (!(props & T_SELECT))
        return report(win, icon, edges, 0, to, ev);
    if (props & T_X256)
        return report(win, icon, edges << 8, 0, to, ev);
    if (props & T_SILENT)
        return 0;
    if (icon >= 0 && win->handle != wimp_ws()->iconbar)
        select_icon(win, icon, edges);
    if ((p->pend.flags & ~P_DRAG) == 0 || ((p->pend.flags & P_CLICK) && edges))
        return report(win, icon, edges, 0, to, ev);
    return 0;
}

static int clicks(struct wimp_window *win, int32_t icon, struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_input *p = in();
    uint32_t b = p->mb, edges = b & ~p->oldb;
    if (!win) {
        p->pend.flags = 0;
        return 0;
    }
    if (icon == -2)
        icon = wimp_gadget(win, p->mx, p->my);
    if ((edges & 2u) && icon >= -1)             /* Menu is reported at once, where it is */
        return click_at(win, icon, b, p->mx, p->my, to, ev);
    b &= ~2u, edges &= ~2u;
    struct wimp_pending *pd = &p->pend;
    if (pd->window != win->handle || pd->icon != icon) {
        struct wimp_window *ow = wimp_window(pd->window);
        if ((pd->flags & P_DESELECT) && ow && pd->icon >= 0)
            set_flags(ow, pd->icon, 0, IF_SELECTED);
        if (pd->flags & P_DOUBLE)
            double_off();
        pd->flags &= P_DRAG;
        if (!p->mb && type_of(win, icon) == 15)
            wimp_cnp_release(win, icon, pd->clicks);
        return press(win, icon, edges, to, ev);
    }
    if (!p->mb && type_of(win, icon) == 15)     /* may clear a type 15 selection */
        wimp_cnp_release(win, icon, pd->clicks);
    uint32_t dt = p->mt - pd->time;
    int d = dist(pd->x, pd->y, p->mx, p->my);
    if (pd->flags & P_DRAG) {
        if (!b)
            pd->flags &= ~P_DRAG;
        else if (d >= (int)p->drag_move || dt >= p->drag_time)
            return report_drag(to, ev);
    }
    if (pd->flags & P_RELEASE) {
        if (!b) {
            pd->flags &= ~P_RELEASE;
            return report(win, icon, pd->buttons, 0, to, ev);
        }
        return 0;
    }
    if (pd->flags & P_DOUBLE) {
        int cnp = type_of(win, icon) == 15;
        if (d >= (int)p->dclick_move || dt >= p->dclick_time) {
            pd->flags &= ~P_DOUBLE;
            cnp ? double_writable_off() : double_off();
        } else if (!edges) {
            return 0;
        } else if (edges & pd->buttons) {
            pd->flags &= ~P_DOUBLE;
            if (cnp) {                          /* counted: a third click or drag may follow */
                double_writable_off();
                pd->flags |= ++pd->clicks == 2 ? P_DRAG | P_DOUBLE : P_DRAG;
                pd->time = p->mt;
                if (pd->clicks != 2)
                    double_writable_off();
                return report(win, icon, edges, 0, to, ev);
            }
            double_off();
            return report(win, icon, pd->buttons, 0, to, ev);
        } else {
            pd->flags &= ~P_DOUBLE;
            cnp ? double_writable_off() : double_off();
        }
        return press(win, icon, edges, to, ev);
    }
    if (pd->flags & P_CLICK) {
        if (edges)
            return report(win, icon, edges, 0, to, ev);
        return press(win, icon, edges, to, ev);
    }
    if (pd->flags & P_REPEAT) {
        if (!(b & pd->buttons)) {
            pd->flags &= ~P_REPEAT;
            return press(win, icon, edges, to, ev);
        }
        if (dt >= pd->wait) {
            pd->time = p->mt;
            pd->wait = pd->rate;
            return report(win, icon, b, 0, to, ev);
        }
        return 0;
    }
    return press(win, icon, edges, to, ev);
}

/* ---- entering and leaving --------------------------------------------------------- */

static int enter_leave(struct wimp_window *win, int32_t icon, struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_input *p = in();
    uint32_t pw = win ? win->handle : 0xFFFFFFFFu;
    if (p->d.type && p->d.type != 10 && p->d.type != 11)
        pw = 0xFFFFFFFFu;
    if (w->as.on)
        pw = 0xFFFFFFFFu;                       /* not while autoscrolling */
    if (win && (win->handle == w->iconbar || wimp_reported(win) == 0xFFFFFFFFu || icon == -2))
        pw = 0xFFFFFFFFu;
    if (pw == p->ptrwindow)
        return 0;
    uint8_t b[4];
    if (p->ptrwindow != 0xFFFFFFFFu) {
        uint32_t old = p->ptrwindow;
        p->ptrwindow = 0xFFFFFFFFu;
        struct wimp_task *t = wimp_task_by_handle(p->ptrtask, 1);
        if (t) {
            if (t->mask & (1u << 4))
                return 0;
            wr(b, 0, old);
            p->reuse = 1;
            return deliver(t, 4, b, 4, to, ev);
        }
        if (pw == 0xFFFFFFFFu)
            return 0;
    }
    p->ptrwindow = pw;
    struct wimp_window *nw = wimp_window(pw);
    p->ptrtask = nw ? nw->owner : 0;
    struct wimp_task *t = owner_of(nw);
    if (!t || (t->mask & (1u << 5)))
        return 0;
    wr(b, 0, pw);
    p->reuse = 1;
    return deliver(t, 5, b, 4, to, ev);
}

/* ---- Wimp_AutoScroll ---------------------------------------------------------------------- */

static struct wimp_autoscroll *as(void)
{
    return &wimp_ws()->as;
}

/* Tells the pointer routine of a change of state */
static void as_tell(uint32_t now, uint32_t old)
{
    struct wimp_autoscroll *a = as();
    if (a->routine >= 0x8000u) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = now, c.r[1] = old, c.r[12] = a->ws;
        ros_call(&c, a->routine);
        return;
    }
    if (!(a->routine & 1u))
        return;
    /* The Wimp's own routine.  It shows its pointer while scrolling, when
     * the window can scroll some way. */
    uint32_t ways[2] = { old, now };
    int show[2];
    for (int k = 0; k < 2; k++) {
        uint32_t s = ways[k];
        show[k] = (s & (1u << 8)) && (((s >> 12) & (s >> 16)) & 15u);
    }
    if (show[0] == show[1])
        return;
    struct wimp_ws *w = wimp_ws();
    char *name = (char *)w->scratch + 448;
    uint32_t r3 = 0x61u;
    if (show[1]) {
        int h = (a->flags & 1u) != 0, v = (a->flags & 2u) != 0;
        strcpy(name, h && !v ? "ptr_autoscrh" : v && !h ? "ptr_autoscrv" : "ptr_autoscr");
    } else {
        strcpy(name, "ptr_default");
    }
    struct ros_cpu c;
    int32_t ax = 0, ay = 0;
    if (show[1]) {
        ros_cpu_enter(&c);
        c.r[0] = 40, c.r[2] = ros_addr(name);
        ros_swi(&c, XWimp_SpriteOp);             /* the active point at its centre */
        if (!c.v)
            ax = (int32_t)c.r[3] / 2, ay = (int32_t)c.r[4] / 2;
    }
    ros_cpu_enter(&c);
    c.r[0] = 36, c.r[2] = ros_addr(name), c.r[3] = r3, c.r[4] = (uint32_t)ax, c.r[5] = (uint32_t)ay;
    c.r[6] = c.r[7] = 0;
    ros_swi(&c, XWimp_SpriteOp);
}

/* The autoscroll state for the pointer's position now */
static uint32_t as_state(void)
{
    struct wimp_input *p = in();
    struct wimp_autoscroll *a = as();
    struct wimp_window *win = wimp_window(a->win);
    if (!win)
        return 0;
    struct wimp_ws *w = wimp_ws();
    struct wimp_box v = win->s[APP].vis;
    int32_t z[4];
    for (int k = 0; k < 4; k++)
        z[k] = a->zones[k] < 0 ? 0 : a->zones[k];
    if (!(a->flags & 1u))
        z[0] = z[2] = 0;
    if (!(a->flags & 2u))
        z[1] = z[3] = 0;
    struct wimp_box c = { v.x0 + z[0], v.y0 + z[1], v.x1 - z[2], v.y1 - z[3] };
    if (c.x0 > c.x1)
        c.x0 = c.x1 = (c.x0 + c.x1) >> 1;
    if (c.y0 > c.y1)
        c.y0 = c.y1 = (c.y0 + c.y1) >> 1;
    int32_t x = p->mx, y = p->my;
    uint32_t s = 0;
    int in_vis = x >= v.x0 && x <= v.x1 - 1 && y >= v.y0 && y <= v.y1 - 1;
    int in_c = x >= c.x0 && x <= c.x1 - 1 && y >= c.y0 && y <= c.y1 - 1;
    s |= !in_vis ? 1u << 9 : in_c ? 1u << 11 : 1u << 10;
    if (x < c.x0) s |= 1u << 12;
    if (y < c.y0) s |= 1u << 13;
    if (x > c.x1 - 1) s |= 1u << 14;
    if (y > c.y1 - 1) s |= 1u << 15;
    const struct wimp_place *pl = &win->s[APP];
    int32_t ex0 = (int32_t)rd(win->def, 40), ey0 = (int32_t)rd(win->def, 44);
    int32_t ex1 = (int32_t)rd(win->def, 48), ey1 = (int32_t)rd(win->def, 52);
    if (pl->scx > ex0) s |= 1u << 16;
    if (pl->scy - (v.y1 - v.y0) > ey0) s |= 1u << 17;
    if (pl->scx + (v.x1 - v.x0) < ex1) s |= 1u << 18;
    if (pl->scy < ey1) s |= 1u << 19;
    if (!(a->flags & 1u))
        s &= ~((1u << 12) | (1u << 14) | (1u << 16) | (1u << 18));
    if (!(a->flags & 2u))
        s &= ~((1u << 13) | (1u << 15) | (1u << 17) | (1u << 19));
    (void)w;
    return s | (a->state & (1u << 8));
}

static void as_set(uint32_t s)
{
    struct wimp_autoscroll *a = as();
    uint32_t old = a->state;
    a->state = s;
    if (s != old)
        as_tell(s, old);
}

void wimp_swi_AutoScroll(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_autoscroll *a = as();
    uint32_t r0 = s->r[0], r1 = s->r[1];
    s->v = 0;
    if (r0 & 0x80u) {                           /* bit 7: read only */
        if (a->on && r1 >= ROS_APP_BASE) {
            ros_st32(r1, a->win);
            for (int k = 0; k < 4; k++)
                ros_st32(r1 + 4 + 4 * (uint32_t)k, (uint32_t)a->zones[k]);
            ros_st32(r1 + 20, a->pause), ros_st32(r1 + 24, a->routine), ros_st32(r1 + 28, a->ws);
        }
        s->r[0] = a->on ? (a->flags | a->state) : 0;
        return;
    }
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    if (r0 & 8u) {                              /* bit 3: an icon, for a ghost caret */
        wimp_iconscroll_swi(s);
        return;
    }
    if (a->on) {                                /* stop the last one and tell its routine */
        as_set(0);
        a->on = 0;
    }
    if (!(r0 & 3u)) {                           /* bits 0, 1 clear: off, shape 1 reset */
        a->state = 0;
        s->r[0] = 0;
        setptr(in()->special ? in()->ptr_sprite : "ptr_default", 0x61u, 0, 0);
        return;
    }
    if (r1 < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    struct wimp_window *win = wimp_window(ros_ld32(r1));
    if (!win) {
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
        return;
    }
    if (win->owner != t->handle) {
        wimp_fail(s, wimp_error(E_OWNER_WINDOW));
        return;
    }
    a->flags = r0 & 7u;
    a->win = win->handle;
    for (int k = 0; k < 4; k++)
        a->zones[k] = (int32_t)ros_ld32(r1 + 4 + 4 * (uint32_t)k);
    a->pause = ros_ld32(r1 + 20);
    if (a->pause == 0xFFFFFFFFu) {              /* WimpAutoScrollDelay, in ds */
        uint8_t dd = cmos(0xDD), de = cmos(0xDE);
        a->pause = (((uint32_t)dd >> 4) ^ 5u) * ((de & 2u) ? 10u : 1u) * 10u;
    }
    a->routine = ros_ld32(r1 + 24);
    a->ws = ros_ld32(r1 + 28);
    if (a->routine < 0x8000u)
        a->routine &= 1u;                       /* odd: the Wimp's routine */
    a->on = 1;
    a->pausing = 0;
    a->state = 0;
    as_set(as_state());
    s->r[0] = a->flags | a->state;
    (void)w;
}

/* Pausing, starting and scrolling, on each poll.  Returns 1 with a
 * request for the window's owner. */
static int autoscroll_step(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_autoscroll *a = as();
    struct wimp_input *p = in();
    if (!a->on)
        return 0;
    struct wimp_window *win = wimp_window(a->win);
    if (!win) {
        a->on = 0;
        return 0;
    }
    as_set(as_state());
    uint32_t s = a->state, now = now_cs();
    if (!(s & (1u << 8))) {
        if (s & (1u << 11)) {                   /* in the centre: nothing */
            a->pausing = 0;
            return 0;
        }
        if (a->pause && !a->pausing) {
            if (!(s & (1u << 10)))
                return 0;
            a->pausing = 1;
            a->pause_end = now + a->pause;
            a->px = p->mx, a->py = p->my;
            return 0;
        }
        if (a->pausing) {
            if (!(s & (1u << 10))) {
                a->pausing = 0;
                return 0;
            }
            if (p->mx != a->px || p->my != a->py) {
                a->pause_end = now + a->pause;
                a->px = p->mx, a->py = p->my;
                return 0;
            }
            if ((int32_t)(now - a->pause_end) < 0)
                return 0;
        }
        a->pausing = 0;                         /* starting */
        as_set(s | (1u << 8));
        a->last = now;
        a->next = now + 8;
        if (a->flags & 4u)
            return scroll_req(win, 0, 0, to, ev);
        return 0;
    }
    if (s & (1u << 11)) {                       /* back in the centre: it stops */
        as_set(s & ~(1u << 8));
        return 0;
    }
    if ((int32_t)(now - a->next) < 0)
        return 0;
    uint32_t t = now - a->last;
    a->last = now;
    a->next = now + 8;
    uint32_t ways = (s >> 12) & (s >> 16) & 15u;
    if (a->flags & 4u) {
        int32_t xd = 0, yd = 0;
        if (ways & 1u) xd = -3;
        if (ways & 4u) xd = 3;
        if (ways & 2u) yd = -3;
        if (ways & 8u) yd = 3;
        return (xd || yd) ? scroll_req(win, xd, yd, to, ev) : 0;
    }
    if (!ways)
        return 0;
    struct wimp_ws *w = wimp_ws();
    struct wimp_box v = win->s[REQ].vis;
    int32_t z[4];
    for (int k = 0; k < 4; k++)
        z[k] = a->zones[k] < 0 ? 0 : a->zones[k];
    struct wimp_box c = { v.x0 + z[0], v.y0 + z[1], v.x1 - z[2], v.y1 - z[3] };
    uint8_t b[32];
    state_block(win, b);
    if (a->flags & 1u) {
        int32_t edge = (s & (1u << 16)) ? c.x0 : c.x1 - w->dx;
        int32_t d = (int32_t)(((int64_t)(p->mx - edge) * (int32_t)t) >> 5);
        if (d >= 0)
            d += w->dx;
        wr(b, 20, (uint32_t)(win->s[REQ].scx + d));
    }
    if (a->flags & 2u) {
        int32_t edge = (s & (1u << 17)) ? c.y0 : c.y1 - w->dy;
        int32_t d = (int32_t)(((int64_t)(p->my - edge) * (int32_t)t) >> 5);
        if (d >= 0)
            d += w->dy;
        wr(b, 24, (uint32_t)(win->s[REQ].scy + d));
    }
    return exit_open(win, b, to, ev);
}

/* ---- the input stages of the poll ------------------------------------------ */

int wimp_input_event(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_input *p = in();
    read_mouse();
    if (wimp_ws()->is.state & 3u)               /* icon autoscrolling */
        wimp_iconscroll_poll();
    int busy, r = drag_step(to, ev, &busy);
    if (r)
        return r;
    if (autoscroll_step(to, ev))                /* window autoscrolling */
        return 1;
    int32_t icon;
    struct wimp_window *win = wimp_hit(p->mx, p->my, 0, &icon);
    if ((p->pend.flags & P_DRAG) && (!win || win->handle != p->pend.window ||
                                     (icon == -2 ? wimp_gadget(win, p->mx, p->my) : icon) != p->pend.icon)) {
        p->reuse = 1;                           /* a pending drag fires early */
        p->early = 1;                           /* reported as a plain Mouse_Click */
        int r = report_drag(to, ev);
        p->early = 0;
        if (r)
            return 1;
    }
    if (enter_leave(win, icon, to, ev))
        return 1;
    wimp_menu_pointer(win, icon, !p->d.type && !(p->pend.flags & P_DRAG));
    if (!p->d.type && !(p->pend.flags & P_DRAG))
        pointer_icon(win, icon);
    if (!p->d.type || p->d.type == 10 || p->d.type == 11) {
        int suppress;                           /* menu tracking */
        uint32_t h = win ? win->handle : 0;
        if (wimp_menu_scan(win, icon, &suppress, to, ev))
            return 1;
        if (suppress)
            return 0;
        if (win && !wimp_window(h))
            win = wimp_hit(p->mx, p->my, 0, &icon);
    }
    if (busy)
        return 0;
    return clicks(win, icon, to, ev);
}
