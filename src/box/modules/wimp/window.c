/* window.c -- windows: their records, handles, stacks and placement, and
 * the SWIs that create, open, close and read them.
 *
 * Each window holds two placements.  REQ is what Wimp_OpenWindow and
 * Wimp_CloseWindow asked for, and APP is what the screen shows.  The SWIs
 * here change only REQ and mark the window.  A flush (redraw.c) works out
 * what to copy and what to redraw from the two, and makes APP equal REQ.
 * Stacks are lists from front to back.  There is one for each parent in
 * each placement. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "wimp.h"

#define FLAG_TITLE_OLD   (1u << 0)
#define FLAG_VSCROLL_OLD (1u << 2)
#define FLAG_HSCROLL_OLD (1u << 3)
#define FLAG_AUTOREDRAW  (1u << 4)
#define FLAG_PANE        (1u << 5)
#define FLAG_NO_CHECKS   (1u << 6)
#define FLAG_NOBACKCLOSE (1u << 7)
#define FLAG_BACK_WINDOW (1u << 11)
#define FLAG_ON_SCREEN   (1u << 13)
#define ST_OPEN          (1u << 16)
#define ST_TOP           (1u << 17)
#define ST_TOGGLED       (1u << 18)
#define ST_TOGGLING      (1u << 19)
#define ST_FORCE         (1u << 21)
#define ST_SHIFT_TOGGLED (1u << 22)
#define FLAG_FOREGROUND  (1u << 23)
#define F_BACK   (1u << 24)
#define F_CLOSE  (1u << 25)
#define F_TITLE  (1u << 26)
#define F_TOGGLE (1u << 27)
#define F_VBAR   (1u << 28)
#define F_SIZE   (1u << 29)
#define F_HBAR   (1u << 30)
#define F_NEW    (1u << 31)
#define STATUS_BITS 0x007F0000u

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

/* ---- handles ------------------------------------------------------------------- */

/* Positive, with bits 0-1 never both clear (1 mod 4), and not reused for a
 * while */
static uint32_t handle_of(uint32_t index, uint32_t generation)
{
    return ((generation & 0x3FFFu) << 14 | index) << 2 | 1u;
}

struct wimp_window *wimp_window(uint32_t handle)
{
    struct wimp_ws *w = wimp_ws();
    if ((handle & 3u) != 1u || (int32_t)handle <= 0)
        return NULL;
    uint32_t index = (handle >> 2) & 0x3FFFu;
    if (index >= WIMP_WINDOWS || !w->win[index] || w->win[index]->handle != handle)
        return NULL;
    return w->win[index];
}

/* ---- the furniture: which is present, the outline, the minimum size ----------- */

static uint32_t owner_version(const struct wimp_window *win)
{
    if ((int32_t)win->owner <= 0)
        return 380;                             /* the Wimp's own count as 380 */
    struct wimp_task *t = wimp_task_by_handle(win->owner, 0);
    return t ? t->version : 380;
}

/* Flag bits 24-30 made to describe the furniture present */
static uint32_t resolve(const struct wimp_window *win, uint32_t f)
{
    if (win->def[32] == 0xFF && owner_version(win) < 380)
        f &= ~(FLAG_TITLE_OLD | FLAG_VSCROLL_OLD | FLAG_HSCROLL_OLD | FLAG_NOBACKCLOSE |
               0x7F000000u);
    else if (!(f & F_NEW)) {
        f &= ~0x7F000000u;
        if (f & FLAG_TITLE_OLD)
            f |= F_BACK | F_CLOSE | F_TITLE;
        if (f & FLAG_NOBACKCLOSE)
            f &= ~(F_BACK | F_CLOSE);
        if (f & FLAG_VSCROLL_OLD)
            f |= F_TOGGLE | F_VBAR;
        if (f & (FLAG_VSCROLL_OLD | FLAG_HSCROLL_OLD))
            f |= F_SIZE;
        if (f & FLAG_HSCROLL_OLD)
            f |= F_HBAR;
    } else {
        f &= ~(FLAG_TITLE_OLD | FLAG_VSCROLL_OLD | FLAG_HSCROLL_OLD | FLAG_NOBACKCLOSE);
    }
    if (!(f & F_TITLE))
        f &= ~(F_BACK | F_CLOSE);
    if (!(f & (F_TITLE | F_VBAR)))
        f &= ~F_TOGGLE;
    if (!(f & (F_VBAR | F_HBAR)))
        f &= ~F_SIZE;
    return f & ~STATUS_BITS;
}

/* The iconise icon shows with a close icon, at the top level, if it is
 * configured on.  The CMOS setting is not read yet, so it is taken as on.
 * RISC OS 5.30's default is on, as the translated Wimp's screens show. */
static int iconise_shows(const struct wimp_window *win, int st)
{
    return (win->s[st].flags & F_CLOSE) && win->s[st].parent == NO_WINDOW;
}

/* The outline: the visible area with the frame and furniture added */
static struct wimp_box outline_of(const struct wimp_window *win, struct wimp_box v, uint32_t f)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_furniture *fu = &w->furn;
    struct wimp_box o = v;
    if (win->def[32] != 0xFF) {
        o.x0 -= w->dx, o.y0 -= w->dy, o.x1 += w->dx, o.y1 += w->dy;
    } else {
        if (f & F_TITLE) o.y1 += w->dy;
        if (f & F_VBAR) o.x1 += w->dx;
        if (f & F_HBAR) o.y0 -= w->dy;
    }
    if (f & F_TITLE) o.y1 += fu->T;
    if (f & F_VBAR) o.x1 += fu->V;
    if (f & F_HBAR) o.y0 -= fu->H;
    return o;
}

/* The title's width, for the minimum width: 16 OS units a character, as
 * in the system font.  Outline-font titles are not measured yet. */
static int32_t title_width(const struct wimp_window *win)
{
    uint32_t tf = rd(win->def, 56);
    if (!(tf & 1u))
        return 0;
    int32_t n = 0;
    if (tf & 0x100u) {                          /* indirected */
        uint32_t p = rd(win->def, 72), len = rd(win->def, 80);
        while (n < (int32_t)len && p && ros_ld8(p + (uint32_t)n) >= ' ')
            n++;
    } else {
        while (n < 12 && win->def[72 + n] >= ' ')
            n++;
    }
    return n * 16;
}

static int32_t min_width(const struct wimp_window *win, uint32_t f, int st)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_furniture *fu = &w->furn;
    int32_t m = (int32_t)(rd(win->def, 68) & 0xFFFFu), x = 0;
    if ((f & F_TITLE) && (f & (F_BACK | F_CLOSE)))
        x = win->def[32] == 0xFF ? 8 + w->dx * ((f & F_VBAR) ? 1 : 2) : 8;
    if (m == 0 && (f & F_TITLE))
        x = title_width(win) + 2 * w->dx;
    if (f & F_BACK) x += fu->B;
    if (f & F_CLOSE) x += fu->C;
    if (iconise_shows(win, st)) x += fu->I;
    if ((f & F_TOGGLE) && !(f & F_VBAR)) x += fu->V;
    if (f & F_HBAR) {
        int32_t bar = (w->dx > w->dy ? w->dx : w->dy) + 32 + 4 * (w->dx > w->dy ? w->dx : w->dy);
        int32_t hx = m == 0 ? bar + fu->L + fu->R : 0;
        if ((f & F_SIZE) && !(f & F_VBAR)) hx += fu->V;
        if (hx > x) x = hx;
    }
    if (m == 1) m = 0;
    return x > m ? x : m;
}

static int32_t min_height(const struct wimp_window *win, uint32_t f)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_furniture *fu = &w->furn;
    int32_t n = (int32_t)(rd(win->def, 68) >> 16), y = n == 0 ? 2 * w->dy : 0;
    if ((f & F_VBAR) && n == 0) {
        int32_t p = w->dx > w->dy ? w->dx : w->dy;
        y = p + 32 + 4 * p + fu->U + fu->D;
    }
    if ((f & F_SIZE) && !(f & F_HBAR)) y += fu->H;
    if ((f & F_TOGGLE) && !(f & F_TITLE)) y += fu->T;
    if (n == 1) n = 0;
    return y > n ? y : n;
}

/* ---- stacks ------------------------------------------------------------------------- */

static uint32_t *head(uint32_t parent, int st)
{
    struct wimp_ws *w = wimp_ws();
    if (parent == NO_WINDOW)
        return &w->top[st];
    struct wimp_window *p = wimp_window(parent);
    return p ? &p->s[st].top_child : &w->top[st];
}

struct wimp_window *wimp_stack_front(uint32_t parent, int st)
{
    uint32_t h = *head(parent, st);
    return h ? wimp_window(h) : NULL;
}

struct wimp_window *wimp_window_below(const struct wimp_window *win, int st)
{
    return win->s[st].below ? wimp_window(win->s[st].below) : NULL;
}

static void unlink_window(struct wimp_window *win, int st)
{
    uint32_t *p = head(win->s[st].parent, st);
    while (*p && *p != win->handle) {
        struct wimp_window *x = wimp_window(*p);
        if (!x)
            break;
        p = &x->s[st].below;
    }
    if (*p == win->handle)
        *p = win->s[st].below;
    win->s[st].below = 0;
}

/* Linked in below "above", or at the front if above is 0 */
static void link_below(struct wimp_window *win, uint32_t above, int st)
{
    uint32_t *p = head(win->s[st].parent, st);
    if (above) {
        struct wimp_window *a = wimp_window(above);
        p = &a->s[st].below;
    }
    win->s[st].below = *p;
    *p = win->handle;
}

enum { L_FG, L_STD, L_BG };

static int layer(const struct wimp_window *win, int st)
{
    if (win->s[st].flags & FLAG_FOREGROUND)
        return L_FG;
    return (win->s[st].flags & FLAG_BACK_WINDOW) ? L_BG : L_STD;
}

/* The window to go below (0 for the front) */
static uint32_t place_below(struct wimp_window *win, uint32_t behind)
{
    const int st = REQ;
    uint32_t parent = win->s[st].parent;
    int me = layer(win, st);
    int kind;                                   /* -1, -2, -3, or a layer */
    struct wimp_window *b = NULL;
    if (behind == 0xFFFFFFFFu)
        kind = -1;
    else if (behind == 0xFFFFFFFDu)
        kind = -3;
    else if ((b = wimp_window(behind)) && b != win && b->s[st].open &&
             b->s[st].parent == parent)
        kind = layer(b, st);
    else
        kind = -2;
    enum { GIVEN, BELOW_FG, ABOVE_BG, BOTTOM, TOP } where;
    if (kind == L_FG)
        where = me == L_FG ? GIVEN : BELOW_FG;
    else if (kind == L_STD)
        where = me == L_FG ? BELOW_FG : GIVEN;
    else if (kind == L_BG)
        where = me == L_FG ? BOTTOM : GIVEN;
    else if (kind == -1)
        where = me == L_FG ? TOP : BELOW_FG;
    else if (kind == -2)
        where = me == L_FG ? BELOW_FG : ABOVE_BG;
    else
        where = BOTTOM;

    struct wimp_window *x = wimp_stack_front(parent, st), *last = NULL;
    switch (where) {
    case TOP:
        return 0;
    case GIVEN:
        return b ? b->handle : 0;
    case BELOW_FG:
        while (x && layer(x, st) == L_FG)
            last = x, x = wimp_window_below(x, st);
        return last ? last->handle : 0;
    case ABOVE_BG:
        while (x && layer(x, st) == L_FG)
            last = x, x = wimp_window_below(x, st);
        while (x && layer(x, st) != L_BG)
            last = x, x = wimp_window_below(x, st);
        return last ? last->handle : 0;
    case BOTTOM:
    default:
        while (x)
            last = x, x = wimp_window_below(x, st);
        return last ? last->handle : 0;
    }
}

/* The window directly above in its stack, or -1 */
static uint32_t behind_of(const struct wimp_window *win, int st)
{
    if (!win->s[st].open)
        return 0xFFFFFFFFu;
    uint32_t above = 0xFFFFFFFFu;
    for (struct wimp_window *x = wimp_stack_front(win->s[st].parent, st); x && x != win;
         x = wimp_window_below(x, st))
        above = x->handle;
    return above;
}

static int overlap(struct wimp_box a, struct wimp_box b)
{
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

static int within(struct wimp_box a, struct wimp_box b)
{
    return a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1;
}

/* "Fully visible", for the flag ST_TOP as the window's state is read */
static int fully_visible(const struct wimp_window *win, int st)
{
    if (!win->s[st].open)
        return 0;
    struct wimp_box o = win->s[st].outline;
    int panes = 1;
    /* windows above, nearest first: the run of panes just above is excused */
    struct wimp_window *above[WIMP_WINDOWS / 16];
    unsigned n = 0;
    for (struct wimp_window *x = wimp_stack_front(win->s[st].parent, st); x && x != win;
         x = wimp_window_below(x, st))
        if (n < sizeof above / sizeof above[0])
            above[n++] = x;
    for (unsigned i = n; i-- > 0;) {
        struct wimp_window *x = above[i];
        if (panes && (x->s[st].flags & FLAG_PANE))
            continue;
        panes = 0;
        if (overlap(x->s[st].outline, o))
            return 0;
    }
    const struct wimp_window *c = win;
    while (c->s[st].parent != NO_WINDOW) {
        struct wimp_window *p = wimp_window(c->s[st].parent);
        if (!p || !p->s[st].open)
            return 0;
        struct wimp_box bound = (c->s[st].flags & FLAG_FOREGROUND) ? p->s[st].outline
                                                                    : p->s[st].vis;
        if (!within(o, bound))
            return 0;
        for (struct wimp_window *x = wimp_stack_front(p->s[st].parent, st); x && x != p;
             x = wimp_window_below(x, st))
            if (overlap(x->s[st].outline, o))
                return 0;
        c = p;
    }
    return 1;
}

/* ---- visibility --------------------------------------------------------------------- */

int32_t wimp_origin_x(const struct wimp_window *win, int st)
{
    return win->s[st].vis.x0 - win->s[st].scx;
}

int32_t wimp_origin_y(const struct wimp_window *win, int st)
{
    return win->s[st].vis.y1 - win->s[st].scy;
}

static struct wimp_box clip_box(struct wimp_box a, struct wimp_box b)
{
    struct wimp_box r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
                          a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    return r;
}

/* visible(W, box): the box, less what is above W and outside its ancestors */
void wimp_visible(const struct wimp_window *win, struct wimp_box box, int st,
                  struct wimp_rlist *out)
{
    out->n = 0;
    out->overflow = 0;
    if (!win->s[st].open)
        return;
    wimp_rl_append(out, box);
    const struct wimp_window *c = win;
    for (;;) {
        struct wimp_window *parent = c->s[st].parent != NO_WINDOW ? wimp_window(c->s[st].parent)
                                                                   : NULL;
        for (struct wimp_window *x = wimp_stack_front(c->s[st].parent, st); x && x != c;
             x = wimp_window_below(x, st)) {
            struct wimp_box ox = x->s[st].outline;
            if (parent && (c->s[st].flags & FLAG_FOREGROUND) && !(x->s[st].flags & FLAG_FOREGROUND))
                ox = clip_box(ox, parent->s[st].vis);
            wimp_rl_subtract(out, ox);
        }
        if (!parent)
            return;
        if (!parent->s[st].open) {
            out->n = 0;
            return;
        }
        wimp_rl_clip(out, (c->s[st].flags & FLAG_FOREGROUND) ? parent->s[st].outline
                                                             : parent->s[st].vis);
        c = parent;
    }
}

/* inner(W): its visible work area, less its open children */
void wimp_inner(const struct wimp_window *win, int st, struct wimp_rlist *out)
{
    wimp_visible(win, win->s[st].vis, st, out);
    for (struct wimp_window *x = wimp_stack_front(win->handle, st); x; x = wimp_window_below(x, st))
        if (x->s[st].open)
            wimp_rl_subtract(out, x->s[st].outline);
}

/* ---- the entry checks --------------------------------------------------------------- */

static struct wimp_task *task_or_fail(struct ros_cpu *s)
{
    struct wimp_task *t = wimp_current();
    if (!t)
        wimp_fail(s, wimp_error(E_BAD_OP));
    return t;
}

static int r1_ok(struct ros_cpu *s, uint32_t r1)
{
    if (r1 >= ROS_APP_BASE)
        return 1;
    wimp_fail(s, wimp_error(E_BAD_PTR_R1));
    return 0;
}

static struct wimp_window *window_or_fail(struct ros_cpu *s, uint32_t handle)
{
    if (handle == 0xFFFFFFFEu && wimp_ws()->iconbar) {
        struct wimp_window *ib = wimp_window(wimp_ws()->iconbar);
        if (ib)
            return ib;
    }
    struct wimp_window *win = wimp_window(handle);
    if (!win)
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
    return win;
}

static int owned(struct ros_cpu *s, const struct wimp_window *win, const struct wimp_task *t)
{
    if (t && win->owner == t->handle)
        return 1;
    wimp_fail(s, wimp_error(E_OWNER_WINDOW));
    return 0;
}

static void mark(struct wimp_window *win)
{
    win->marked = 1;
    wimp_ws()->any_marked = 1;
    for (struct wimp_window *x = wimp_stack_front(win->handle, REQ); x; x = wimp_window_below(x, REQ))
        mark(x);
}

/* ---- creating ----------------------------------------------------------------------- */

static void enlarge_extent(struct wimp_window *win)
{
    uint32_t f = win->s[REQ].flags;
    int32_t ex0 = (int32_t)rd(win->def, 40), ey0 = (int32_t)rd(win->def, 44);
    int32_t ex1 = (int32_t)rd(win->def, 48), ey1 = (int32_t)rd(win->def, 52);
    int32_t mw = min_width(win, f, REQ), mh = min_height(win, f);
    if (ex1 - ex0 < mw) ex1 = ex0 + mw;
    if (ey1 - ey0 < mh) ey0 = ey1 - mh;
    wr(win->def, 40, (uint32_t)wimp_round_x(ex0));
    wr(win->def, 44, (uint32_t)wimp_round_y(ey0));
    wr(win->def, 48, (uint32_t)wimp_round_x(ex1));
    wr(win->def, 52, (uint32_t)wimp_round_y(ey1));
}

/* Make a window from an 88-byte block, owned by owner */
static struct wimp_window *create(const uint8_t *block, uint32_t owner, os_error **err)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t index = WIMP_WINDOWS;
    for (uint32_t i = 0; i < WIMP_WINDOWS && index == WIMP_WINDOWS; i++)
        if (!w->win[i])
            index = i;
    void *mem;
    if (index == WIMP_WINDOWS || xos_module_claim(sizeof(struct wimp_window), &mem)) {
        *err = ros_error(E_TOO_BIG, "There is not enough memory to create this window or menu");
        return NULL;
    }
    struct wimp_window *win = mem;
    memset(win, 0, sizeof *win);
    if (++w->win_generation > 0x3FFFu)
        w->win_generation = 1;
    win->handle = handle_of(index, w->win_generation);
    win->owner = owner;
    memcpy(win->def, block, 88);
    win->align = 0;
    for (int st = 0; st < 2; st++) {
        win->s[st].parent = NO_WINDOW;
        win->s[st].open = 0;
    }
    struct wimp_box v = { (int32_t)rd(block, 0), (int32_t)rd(block, 4), (int32_t)rd(block, 8),
                          (int32_t)rd(block, 12) };
    win->s[REQ].vis = v;
    win->s[REQ].scx = (int32_t)rd(block, 16);
    win->s[REQ].scy = (int32_t)rd(block, 20);
    win->toggle_vis = v;
    win->toggle_scx = win->s[REQ].scx;
    win->toggle_scy = win->s[REQ].scy;
    win->toggle_behind = 0xFFFFFFFFu;
    w->win[index] = win;
    win->s[REQ].flags = resolve(win, rd(block, 28)) | ST_FORCE;   /* on screen at its first open */
    if (w->singletask >= 0)
        wr(win->def, 68, 0);                    /* single-tasking: "they meant 0 really" */
    enlarge_extent(win);
    win->s[REQ].outline = outline_of(win, v, win->s[REQ].flags);
    win->s[APP] = win->s[REQ];
    win->icons = 0;
    win->nicons = 0;
    win->sel.icon = -1;                         /* no selection */
    win->sel.xoverride = SEL_BIGNUM;
    return win;
}

static void sync_def(struct wimp_window *win)
{
    struct wimp_place *p = &win->s[REQ];
    wr(win->def, 0, (uint32_t)p->vis.x0);
    wr(win->def, 4, (uint32_t)p->vis.y0);
    wr(win->def, 8, (uint32_t)p->vis.x1);
    wr(win->def, 12, (uint32_t)p->vis.y1);
    wr(win->def, 16, (uint32_t)p->scx);
    wr(win->def, 20, (uint32_t)p->scy);
}

void wimp_swi_CreateWindow(struct ros_cpu *s)
{
    struct wimp_task *t = task_or_fail(s);
    if (!t || !r1_ok(s, s->r[1]))
        return;
    int32_t n = (int32_t)ros_ld32(s->r[1] + 84);
    if ((int32_t)((uint32_t)n << 5) < 0) {
        wimp_fail(s, ros_error(E_TOO_BIG, "There is not enough memory to create this window or "
                                          "menu"));
        return;
    }
    wimp_cancel_redraw();
    os_error *e = NULL;
    struct wimp_window *win = create(ros_ptr(s->r[1]), t->handle, &e);
    if (win && n > 0)
        e = wimp_icons_from_block(win, s->r[1] + 88, (uint32_t)n);
    if (e && win) {
        wimp_ws()->win[(win->handle >> 2) & 0x3FFFu] = NULL;
        xos_module_free(win);
        win = NULL;
    }
    if (!win) {
        wimp_fail(s, e);
        return;
    }
    s->r[0] = win->handle;
    s->v = 0;
}

static os_error *open_window(struct wimp_window *win, uint8_t *block);

/* backdef: an old-style task's own back window, opened at the front over
 * the whole screen to hide what was there */
uint32_t wimp_old_back_window(uint32_t owner)
{
    struct wimp_ws *w = wimp_ws();
    uint8_t block[88];
    memset(block, 0, sizeof block);
    wr(block, 24, 0xFFFFFFFFu);
    wr(block, 28, (1u << 4) | (1u << 6) | (1u << 11));     /* auto-redraw, no checks, back window */
    block[35] = 4;                              /* work area background: colour 4 */
    wr(block, 44, (uint32_t)-SEL_BIGNUM);
    wr(block, 48, SEL_BIGNUM);
    os_error *e = NULL;
    struct wimp_window *bw = create(block, owner, &e);
    if (!bw)
        return 0;
    uint8_t open[36];
    wr(open, 0, bw->handle);
    wr(open, 4, 0);
    wr(open, 8, 0);
    wr(open, 12, (uint32_t)w->screen_w);
    wr(open, 16, (uint32_t)w->screen_h);
    wr(open, 20, 0);
    wr(open, 24, 0);
    wr(open, 28, 0xFFFFFFFFu);
    open_window(bw, open);
    return bw->handle;
}

/* ---- opening ------------------------------------------------------------------------- */

/* The toggle state: whether the window counts as at full size */
static void toggle_state(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_place *p = &win->s[REQ];
    int32_t vw = p->vis.x1 - p->vis.x0, vh = p->vis.y1 - p->vis.y0;
    int32_t ew = (int32_t)(rd(win->def, 48) - rd(win->def, 40));
    int32_t eh = (int32_t)(rd(win->def, 52) - rd(win->def, 44));
    int32_t ow = p->outline.x1 - p->outline.x0, oh = p->outline.y1 - p->outline.y0;
    if (vw < ew && ow < w->screen_w)
        p->flags &= ~(ST_TOGGLED | ST_TOGGLING | ST_SHIFT_TOGGLED);
    else if ((p->flags & ST_SHIFT_TOGGLED) && win->shift_toggle_h == vh)
        p->flags &= ~(ST_TOGGLED | ST_TOGGLING);
    else {
        p->flags &= ~(ST_TOGGLED | ST_TOGGLING | ST_SHIFT_TOGGLED);
        if (vh >= eh || oh >= w->screen_h)
            p->flags |= ST_TOGGLED;
    }
}

/* A window kept on the screen, or within its parent.  Its outline is
 * computed from the visible area v.  Returns the visible area that keeps
 * the window within its bounds. */
static struct wimp_box keep_on_screen(struct wimp_window *win, struct wimp_box v, int forced)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t f = win->s[REQ].flags;
    struct wimp_box o = outline_of(win, v, f);
    struct wimp_box bounds = wimp_screen_box();
    int top_level = win->s[REQ].parent == NO_WINDOW;
    if (!top_level) {
        struct wimp_window *p = wimp_window(win->s[REQ].parent);
        if (p && (f & FLAG_FOREGROUND))
            bounds = p->s[REQ].outline;
        else if (p) {
            int32_t ox = wimp_origin_x(p, REQ), oy = wimp_origin_y(p, REQ);
            bounds = (struct wimp_box){ ox + (int32_t)rd(p->def, 40) - w->dx,
                                        oy + (int32_t)rd(p->def, 44) - w->dy,
                                        ox + (int32_t)rd(p->def, 48) + w->dx,
                                        oy + (int32_t)rd(p->def, 52) + w->dy };
        }
    }
    int lax = top_level && !forced;             /* CMOS WimpFlags bits 5, 6: off by default */
    (void)lax;
    int32_t bw = bounds.x1 - bounds.x0, bh = bounds.y1 - bounds.y0;
    if (o.x1 - o.x0 > bw) o.x1 = o.x0 + bw;
    if (o.y1 - o.y0 > bh) o.y0 = o.y1 - bh;
    int32_t d;
    if (o.x0 < bounds.x0) d = bounds.x0 - o.x0, o.x0 += d, o.x1 += d;
    if (o.y1 > bounds.y1) d = o.y1 - bounds.y1, o.y0 -= d, o.y1 -= d;
    if (o.x1 > bounds.x1) d = o.x1 - bounds.x1, o.x0 -= d, o.x1 -= d;
    if (o.y0 < bounds.y0) d = bounds.y0 - o.y0, o.y0 += d, o.y1 += d;
    if (o.x0 < bounds.x0) o.x0 = bounds.x0;
    if (o.y1 > bounds.y1) o.y1 = bounds.y1;
    /* the visible area is the outline less the frame and furniture */
    struct wimp_box full = outline_of(win, v, f);
    struct wimp_box r = { v.x0 + (o.x0 - full.x0), v.y0 + (o.y0 - full.y0),
                          v.x1 + (o.x1 - full.x1), v.y1 + (o.y1 - full.y1) };
    return r;
}

/* Open a window as Wimp_OpenWindow does.  block is the open block in host
 * memory, and the position used is written back to it. */
static os_error *open_window(struct wimp_window *win, uint8_t *block)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_place *p = &win->s[REQ];
    uint32_t behind = rd(block, 28);
    if (behind == win->handle) {                /* behind itself: where it is */
        behind = behind_of(win, REQ);
        wr(block, 28, behind);
    }
    /* rounding */
    struct wimp_box v = wimp_round_box((struct wimp_box){ (int32_t)rd(block, 4),
                                                         (int32_t)rd(block, 8),
                                                         (int32_t)rd(block, 12),
                                                         (int32_t)rd(block, 16) });
    int32_t scx = wimp_round_x((int32_t)rd(block, 20)), scy = wimp_round_y((int32_t)rd(block, 24));
    /* the size, within the minimum and the extent */
    int32_t ex0 = (int32_t)rd(win->def, 40), ey0 = (int32_t)rd(win->def, 44);
    int32_t ex1 = (int32_t)rd(win->def, 48), ey1 = (int32_t)rd(win->def, 52);
    int32_t mw = win->owner == NO_WINDOW ? 0 : min_width(win, p->flags, REQ);
    int32_t mh = min_height(win, p->flags);
    int32_t vw = v.x1 - v.x0, vh = v.y1 - v.y0;
    if (vw < mw) vw = mw;
    if (vw > ex1 - ex0) vw = ex1 - ex0;
    if (vh < mh) vh = mh;
    if (vh > ey1 - ey0) vh = ey1 - ey0;
    v.x1 = wimp_round_x(v.x0 + vw);
    v.y0 = wimp_round_y(v.y1 - vh);
    /* on the screen */
    int forced = !p->open || (p->flags & (ST_FORCE | FLAG_ON_SCREEN));
    if (!(p->flags & FLAG_NO_CHECKS))
        v = keep_on_screen(win, v, forced);
    /* the scroll offsets */
    vw = v.x1 - v.x0, vh = v.y1 - v.y0;
    if (scx < ex0) scx = ex0;
    if (scx > ex1 - vw) scx = ex1 - vw;
    if (scy > ey1) scy = ey1;
    if (scy < ey0 + vh) scy = ey0 + vh;
    scx = wimp_round_x(scx), scy = wimp_round_y(scy);
    /* the write-back */
    wr(block, 4, (uint32_t)v.x0);
    wr(block, 8, (uint32_t)v.y0);
    wr(block, 12, (uint32_t)v.x1);
    wr(block, 16, (uint32_t)v.y1);
    wr(block, 20, (uint32_t)scx);
    wr(block, 24, (uint32_t)scy);
    /* the window marked and moved in its stack */
    mark(win);
    if (p->open)
        unlink_window(win, REQ);
    p->vis = v;
    p->scx = scx;
    p->scy = scy;
    p->outline = outline_of(win, v, p->flags);
    sync_def(win);
    uint32_t above = place_below(win, behind);
    link_below(win, above, REQ);
    p->open = 1;
    p->flags |= ST_OPEN;
    toggle_state(win);                          /* the toggle state */
    if (!(p->flags & FLAG_NO_CHECKS))
        p->flags &= ~ST_FORCE;                  /* applied: a window never checked keeps it */
    (void)w;
    return NULL;
}

static int wimp_template(const char *name, uint8_t out[88]);
static void delete_window(struct wimp_window *win);
static void delete_quietly(struct wimp_window *win);
static void close_window(struct wimp_window *win);

/* For iconbar.c: a window of the Wimp's own from a template block, and its
 * deletion */
int wimp_template_load(const char *name, uint8_t out[88])
{
    return wimp_template(name, out);
}

struct wimp_window *wimp_create_system(const uint8_t *block, os_error **err)
{
    return create(block, 0, err);
}

/* ...with icons, from a whole template entry */
struct wimp_window *wimp_create_system_icons(const uint8_t *block, os_error **err)
{
    struct wimp_window *win = create(block, 0, err);
    if (win && rd(block, 84)) {
        *err = wimp_icons_from_block(win, ros_addr((void *)(block + 88)), rd(block, 84));
        if (*err) {
            delete_window(win);
            return NULL;
        }
    }
    return win;
}

void wimp_close_system(struct wimp_window *win)
{
    close_window(win);
}

void wimp_delete_system(struct wimp_window *win)
{
    delete_window(win);
}

/* For menus.c: a window owned by -1, furniture set in the stored flags
 * after the create-time resolution, a forced open, and the delete that
 * announces nothing */
struct wimp_window *wimp_create_menu_window(const uint8_t *block, os_error **err)
{
    return create(block, NO_WINDOW, err);
}

void wimp_window_set_flags(struct wimp_window *win, uint32_t bits)
{
    win->s[REQ].flags |= bits;
    win->s[APP].flags |= bits;
    wr(win->def, 28, rd(win->def, 28) | bits);
}

void wimp_open_forced(struct wimp_window *win, uint8_t *block)
{
    win->s[REQ].flags |= ST_FORCE;
    wimp_open_own(win, block);
}

void wimp_delete_menu_window(struct wimp_window *win)
{
    delete_quietly(win);
}

/* -2, the iconbar, where a SWI takes it */
struct wimp_window *wimp_window_ib(uint32_t handle)
{
    if (handle == 0xFFFFFFFEu)
        return wimp_ws()->iconbar ? wimp_window(wimp_ws()->iconbar) : NULL;
    return wimp_window(handle);
}

/* For input.c: behind, and the minimum size */
uint32_t wimp_behind(const struct wimp_window *win, int st)
{
    return behind_of(win, st);
}

void wimp_min_size(const struct wimp_window *win, int32_t *w, int32_t *h)
{
    *w = min_width(win, win->s[APP].flags, APP);
    *h = min_height(win, win->s[APP].flags);
}

/* The Wimp opening one of its own windows, from a 32-byte open block as
 * Wimp_OpenWindow takes it */
void wimp_open_own(struct wimp_window *win, uint8_t *block)
{
    wimp_cancel_redraw();
    open_window(win, block);
}

void wimp_swi_OpenWindow(struct ros_cpu *s)
{
    struct wimp_task *t = task_or_fail(s);
    if (!t)
        return;
    if (s->r[1] == 0xFFFFFFFFu) {               /* -1: a flush only */
        wimp_flush();
        s->v = 0;
        return;
    }
    if (!r1_ok(s, s->r[1]))
        return;
    wimp_cancel_redraw();                       /* any redraw in progress cancelled */
    uint8_t block[36];
    memcpy(block, ros_ptr(s->r[1]), 36);
    struct wimp_window *win = window_or_fail(s, rd(block, 0));
    if (!win)
        return;
    if (s->r[2] == TASK_WORD) {                 /* the parent, for a nested window */
        uint32_t parent = s->r[3];
        if (parent != NO_WINDOW) {
            struct wimp_window *pw = wimp_window(parent);
            if (!pw) {
                wimp_fail(s, wimp_error(E_BAD_HANDLE));
                return;
            }
            for (struct wimp_window *a = pw; a;
                 a = a->s[REQ].parent != NO_WINDOW ? wimp_window(a->s[REQ].parent) : NULL)
                if (a == win) {
                    wimp_fail(s, ros_error(E_BAD_PARENT, "Bad parent window"));
                    return;
                }
        }
        if (s->r[4] & 1u)
            win->s[REQ].flags = resolve(win, rd(block, 32)) | (win->s[REQ].flags & STATUS_BITS);
        if (win->s[REQ].open && win->s[REQ].parent != parent) {
            unlink_window(win, REQ);
            win->s[REQ].open = 0;
        }
        win->s[REQ].parent = parent;
        win->align = s->r[4] & ~1u;
        s->r[2] = 0;
    }
    os_error *e = open_window(win, block);
    if (e) {
        wimp_fail(s, e);
        return;
    }
    memcpy(ros_ptr(s->r[1]), block, 28);        /* +0 to +28: the position used */
    s->v = 0;
}

/* ---- closing and deleting ---------------------------------------------------------- */

static void close_window(struct wimp_window *win)
{
    struct wimp_place *p = &win->s[REQ];
    mark(win);
    if (p->open) {
        unlink_window(win, REQ);
        p->parent = NO_WINDOW;
    }
    p->open = 0;
    p->flags &= ~(ST_OPEN | ST_TOP);
}

void wimp_swi_CloseWindow(struct ros_cpu *s)
{
    struct wimp_task *t = task_or_fail(s);
    if (!t || !r1_ok(s, s->r[1]))
        return;
    struct wimp_window *win = window_or_fail(s, ros_ld32(s->r[1]));
    if (!win)
        return;
    wimp_cancel_redraw();
    close_window(win);
    wimp_flush();                               /* the close applied at once */
    s->v = 0;
}

static void delete_quietly(struct wimp_window *win);

static void delete_window(struct wimp_window *win)
{
    uint8_t b[24];
    memset(b, 0, sizeof b);
    wr(b, 0, 24);
    wr(b, 16, 0x400CBu);                        /* Message_WindowClosed */
    wr(b, 20, win->handle);
    wimp_queue_message(17, b, 24, RECV_BROADCAST, 0, 0);
    delete_quietly(win);
}

/* int_delete_window: no Message_WindowClosed */
static void delete_quietly(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    wimp_clipboard_sel_changed(win->handle);    /* a drag of its selection aborted */
    wimp_queue_forget_window(win->handle);
    wimp_clipboard_going(win->handle, -1);
    wimp_input_window_gone(win->handle);
    wimp_caret_window_gone(win->handle);
    close_window(win);
    for (struct wimp_window *c = wimp_stack_front(win->handle, REQ), *next; c; c = next) {
        next = wimp_window_below(c, REQ);
        close_window(c);
    }
    wimp_flush();
    if (w->pending_window == win->handle)
        wimp_cancel_redraw();
    wimp_surface_gone(win);                     /* its surface binding freed */
    /* off the applied stacks too: the flush made them the same */
    w->win[(win->handle >> 2) & 0x3FFFu] = NULL;
    wimp_icons_free(win);
    xos_module_free(win);
}

void wimp_swi_DeleteWindow(struct ros_cpu *s)
{
    struct wimp_task *t = task_or_fail(s);
    if (!t || !r1_ok(s, s->r[1]))
        return;
    struct wimp_window *win = window_or_fail(s, ros_ld32(s->r[1]));
    if (!win || !owned(s, win, t))
        return;
    delete_window(win);
    s->v = 0;
}

void wimp_windows_of_task_delete(uint32_t task)
{
    struct wimp_ws *w = wimp_ws();
    for (uint32_t i = 0; i < WIMP_WINDOWS; i++)
        if (w->win[i] && w->win[i]->owner == task)
            delete_window(w->win[i]);
}

/* ---- reading ---------------------------------------------------------------------- */

static uint32_t flags_as_read(const struct wimp_window *win)
{
    uint32_t f = win->s[REQ].flags & ~ST_TOP;
    if (fully_visible(win, REQ))
        f |= ST_TOP;
    return f;
}

void wimp_swi_GetWindowState(struct ros_cpu *s)
{
    if (!r1_ok(s, s->r[1]))
        return;
    uint32_t b = s->r[1];
    struct wimp_window *win = window_or_fail(s, ros_ld32(b));
    if (!win)
        return;
    struct wimp_place *p = &win->s[REQ];
    ros_st32(b, win->handle);
    ros_st32(b + 4, (uint32_t)p->vis.x0);
    ros_st32(b + 8, (uint32_t)p->vis.y0);
    ros_st32(b + 12, (uint32_t)p->vis.x1);
    ros_st32(b + 16, (uint32_t)p->vis.y1);
    ros_st32(b + 20, (uint32_t)p->scx);
    ros_st32(b + 24, (uint32_t)p->scy);
    ros_st32(b + 28, behind_of(win, REQ));
    ros_st32(b + 32, flags_as_read(win));
    if (s->r[2] == TASK_WORD) {
        s->r[2] = 0;
        s->r[3] = p->parent;
        s->r[4] = win->align;
    }
    s->v = 0;
}

void wimp_swi_GetWindowInfo(struct ros_cpu *s)
{
    uint32_t b = s->r[1] & ~3u;
    if (!r1_ok(s, b))
        return;
    struct wimp_window *win = window_or_fail(s, ros_ld32(b));
    if (!win)
        return;
    sync_def(win);
    ros_st32(b, win->handle);
    memcpy(ros_ptr(b + 4), win->def, 88);
    ros_st32(b + 4 + 24, behind_of(win, REQ));
    ros_st32(b + 4 + 28, flags_as_read(win));
    ros_st32(b + 4 + 84, win->nicons);
    if (!(s->r[1] & 1u) && win->nicons)         /* the icons too, deleted ones included */
        memcpy(ros_ptr(b + 92), ros_ptr(win->icons), 32 * win->nicons);
    s->v = 0;
}

void wimp_swi_GetWindowOutline(struct ros_cpu *s)
{
    struct wimp_task *t = task_or_fail(s);
    if (!t || !r1_ok(s, s->r[1]))
        return;
    struct wimp_window *win = window_or_fail(s, ros_ld32(s->r[1]));
    if (!win)
        return;
    struct wimp_box o = win->s[REQ].outline;
    ros_st32(s->r[1] + 4, (uint32_t)o.x0);
    ros_st32(s->r[1] + 8, (uint32_t)o.y0);
    ros_st32(s->r[1] + 12, (uint32_t)o.x1);
    ros_st32(s->r[1] + 16, (uint32_t)o.y1);
    s->v = 0;
}

void wimp_swi_SetExtent(struct ros_cpu *s)
{
    struct wimp_task *t = task_or_fail(s);
    if (!t || !r1_ok(s, s->r[1]))
        return;
    struct wimp_window *win = window_or_fail(s, s->r[0]);
    if (!win || !owned(s, win, t))
        return;
    wimp_cancel_redraw();
    wimp_set_extent(win, ros_ptr(s->r[1]));
    s->v = 0;
}

/* Wimp_SetExtent's work, the checks made: for the SWI, and for a surface
 * window whose surface has a new size (surface.c) */
void wimp_set_extent(struct wimp_window *win, const void *box)
{
    memcpy(win->def + 40, box, 16);
    enlarge_extent(win);
    if (within(win->s[REQ].outline, wimp_screen_box()))
        win->s[REQ].flags |= ST_FORCE;
    uint32_t keep = win->s[REQ].flags & (ST_TOGGLING | ST_SHIFT_TOGGLED);
    toggle_state(win);
    win->s[REQ].flags = (win->s[REQ].flags & ~(ST_TOGGLING | ST_SHIFT_TOGGLED)) | keep;
    /* The scroll bars are drawn again for the new extent.  These are the
     * strips of the outline to the right of and below the visible area.
     * The redraw scan draws whatever is on top there. */
    const struct wimp_place *a = &win->s[APP];
    if (a->open) {
        struct wimp_box right = { a->vis.x1, a->outline.y0, a->outline.x1, a->outline.y1 };
        struct wimp_box below = { a->outline.x0, a->outline.y0, a->outline.x1, a->vis.y0 };
        if (right.x0 < right.x1 && right.y0 < right.y1)
            wimp_invalidate_box(right);
        if (below.x0 < below.x1 && below.y0 < below.y1)
            wimp_invalidate_box(below);
    }
}

/* ---- the Wimp's own windows ----------------------------------------------------------------- */

/* A window from the Wimp's own templates (Resources:$.Resources.Wimp.
 * Templates), by name.  Its 88-byte block is put in out.  This serves only
 * windows with no icons and no indirected data, as the back window is. */
static int wimp_template(const char *name, uint8_t out[88])
{
    struct wimp_ws *w = wimp_ws();
    char *file = (char *)w->scratch;
    strcpy(file, "WindowManager:Templates");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17;
    c.r[1] = ros_addr(file);
    ros_swi(&c, XOS_File);
    if (c.v || c.r[0] != 1)
        return 0;
    uint32_t size = c.r[4];
    void *buf;
    if (xos_module_claim(size, &buf))
        return 0;
    ros_cpu_enter(&c);
    c.r[0] = 16;
    c.r[1] = ros_addr(file);
    c.r[2] = ros_addr(buf);
    c.r[3] = 0;
    ros_swi(&c, XOS_File);
    int found = 0;
    const uint8_t *d = buf;
    for (uint32_t i = 16; !c.v && !found && i + 24 <= size; i += 24) {
        uint32_t off = rd(d, i), len = rd(d, i + 4);
        if (!off)
            break;
        char n[13];
        unsigned k = 0;
        while (k < 12 && d[i + 12 + k] >= ' ')
            n[k] = (char)d[i + 12 + k], k++;
        n[k] = 0;
        if (!strcmp(n, name) && off + 88 <= size && len >= 88) {
            memcpy(out, d + off, 88);
            found = 1;
        }
    }
    xos_module_free(buf);
    return found;
}

/* The first task's start: the mode read, the tools measured, and the back
 * window made and opened over the whole screen */
void wimp_windows_start(void)
{
    struct wimp_ws *w = wimp_ws();
    w->lastmode_w = w->lastmode_h = 0x7FFFFFFF; /* forced on screen the first time */
    wimp_mode_refresh();
    {
        /* orig_applicationspacesize (s/Wimp08s findpages): application
         * space's base plus the most it can be, from OS_ReadDynamicArea -1 */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0xFFFFFFFFu;
        ros_swi(&c, XOS_ReadDynamicArea);
        w->start_app_space = c.v ? 0 : c.r[0] + c.r[2];
    }
    if (w->mode == 0xFFFFFFFFu)
        wimp_read_mode();                       /* the configured mode, if none is set */
    memcpy(w->palette, wimp_default_palette, sizeof w->palette);
    memcpy(w->others, wimp_default_others, sizeof w->others);
    wimp_caret_reset();
    wimp_menus_start();
    wimp_input_config();
    w->dr.sprite_lastmode = 0xFFFFFFFFu;
    wimp_tools_refresh();
    wimp_sprites_start();
    wimp_find_font();                           /* the desktop font */
    uint8_t block[88];
    if (!w->back_window && wimp_template("backwindow", block)) {
        os_error *e = NULL;
        wr(block, 84, 0);
        struct wimp_window *bw = create(block, 0, &e);
        if (bw) {
            w->back_window = bw->handle;
            uint8_t open[36];
            wr(open, 0, bw->handle);
            wr(open, 4, 0);
            wr(open, 8, 0);
            wr(open, 12, (uint32_t)w->screen_w);
            wr(open, 16, (uint32_t)w->screen_h);
            wr(open, 20, 0);
            wr(open, 24, 0);
            wr(open, 28, 0xFFFFFFFEu);
            open_window(bw, open);
        }
    }
    wimp_errorbox_make();                       /* the error box */
    wimp_cmdwin_make();                         /* the command window */
    wimp_invalidate_box(wimp_screen_box());    /* the mode set: all of it */
    w->mode_changed = 1;                        /* Wimp_SetMode again */
}

/* Message_ModeChange from the Wimp, then an Open_Window_Request for each
 * open window that a task owns, from back to front, leaving out panes.
 * Each is addressed by its handle, so that one deleted meanwhile is lost.
 * Behind is the window itself, which means "where it is". */
void wimp_mode_change_requests(void)
{
    struct wimp_ws *w = wimp_ws();
    w->lastmode_w = w->screen_w;                /* for the next mode change */
    w->lastmode_h = w->screen_h;
    uint8_t b[36];
    memset(b, 0, sizeof b);
    wr(b, 0, 20);
    wr(b, 16, 0x400C1u);
    wimp_queue_message(17, b, 20, RECV_BROADCAST, 0, 0);
    struct wimp_window *order[WIMP_WINDOWS / 4];
    unsigned n = 0;
    for (struct wimp_window *x = wimp_stack_front(NO_WINDOW, REQ); x && n < sizeof order / sizeof order[0];
         x = wimp_window_below(x, REQ))
        order[n++] = x;
    while (n-- > 0) {
        struct wimp_window *x = order[n];
        if (w->forceflags && !(x->s[REQ].flags & FLAG_PANE))
            x->s[REQ].flags |= ST_FORCE;        /* ws_onscreenonce: back on the smaller screen */
        if ((int32_t)x->owner <= 0 || (x->s[REQ].flags & FLAG_PANE) || !x->s[REQ].open)
            continue;
        struct wimp_place *p = &x->s[REQ];
        wr(b, 0, x->handle);
        wr(b, 4, (uint32_t)p->vis.x0);
        wr(b, 8, (uint32_t)p->vis.y0);
        wr(b, 12, (uint32_t)p->vis.x1);
        wr(b, 16, (uint32_t)p->vis.y1);
        wr(b, 20, (uint32_t)p->scx);
        wr(b, 24, (uint32_t)p->scy);
        wr(b, 28, x->handle);
        wimp_queue_message(2, b, 32, RECV_TASK, x->owner, 0);
        wimp_ws()->queue_tail->window = x->handle;
    }
}

/* ---- the scroll wheel (ROSGD's own, with WindowScroll's behaviour) ---------------- */

#define WHEEL_STEP 160                          /* OS units a notch: WindowScroll's speed 40, x4 */

/* The window that a notch scrolls, found from the one under the pointer.
 * It is that window if it asks for extended scroll requests, or has a
 * scroll bar in the direction of movement.  Otherwise, for a pane or a
 * nested child, the parent is tried in the same way.  Otherwise there is
 * none. */
static struct wimp_window *wheel_target(struct wimp_window *win, int32_t dx, int32_t dy, int *extended)
{
    for (int depth = 0; win && depth < 8; depth++) {
        uint32_t f = win->s[REQ].flags;
        if (win->def[39] & 2u) {                /* extra flags bit 1: extended scroll requests */
            *extended = 1;
            return win;
        }
        int vbar = (f & 0x80000000u) ? (f & F_VBAR) != 0 : (f & 4u) != 0;
        int hbar = (f & 0x80000000u) ? (f & F_HBAR) != 0 : (f & 8u) != 0;
        if ((dy && vbar) || (dx && hbar)) {
            *extended = 0;
            return win;
        }
        if (win->s[REQ].parent != NO_WINDOW)
            win = wimp_window(win->s[REQ].parent);
        else if (f & FLAG_PANE)                 /* a pane: the window it sits on, below it */
            win = wimp_window_below(win, REQ);
        else
            return NULL;
    }
    return NULL;
}

/* Wheel notches since the last look scroll the window under the pointer.
 * A window that asks for extended requests gets a Scroll_Request (dx and
 * dy times 4).  Otherwise its owner gets an Open_Window_Request with the
 * scroll offsets moved.  The Wimp's own window (a menu) is opened at once.
 * Returns 1 if something was queued. */
int wimp_wheel(void)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 2;
    ros_swi(&c, XOS_Pointer);
    if (c.v)
        return 0;
    int32_t x = (int32_t)c.r[0], y = (int32_t)c.r[1];
    if (!w->wheel_seen) {
        w->wheel_seen = 1;
        w->wheel_x = x, w->wheel_y = y;
        return 0;
    }
    int32_t dx = x - w->wheel_x, dy = y - w->wheel_y;
    w->wheel_x = x, w->wheel_y = y;
    if (!dx && !dy)
        return 0;
    int32_t icon;
    struct wimp_input *p = &w->in;
    int extended = 0;
    struct wimp_window *win = wheel_target(wimp_hit(p->mx, p->my, 1, &icon), dx, dy, &extended);
    if (!win || !win->s[REQ].open)
        return 0;
    struct wimp_place *pl = &win->s[REQ];
    uint8_t b[40];
    memset(b, 0, sizeof b);
    wr(b, 0, win->handle);
    wr(b, 4, (uint32_t)pl->vis.x0);
    wr(b, 8, (uint32_t)pl->vis.y0);
    wr(b, 12, (uint32_t)pl->vis.x1);
    wr(b, 16, (uint32_t)pl->vis.y1);
    if (extended) {
        wr(b, 20, (uint32_t)pl->scx);
        wr(b, 24, (uint32_t)pl->scy);
        wr(b, 28, win->handle);
        wr(b, 32, (uint32_t)(dx * 4));
        wr(b, 36, (uint32_t)(dy * 4));
        if ((int32_t)win->owner <= 0)
            return 0;
        wimp_queue_message(10, b, 40, RECV_TASK, win->owner, 0);    /* Scroll_Request */
        w->queue_tail->window = win->handle;
        return 1;
    }
    wr(b, 20, (uint32_t)(pl->scx + dx * WHEEL_STEP));
    wr(b, 24, (uint32_t)(pl->scy + dy * WHEEL_STEP));
    wr(b, 28, win->handle);                     /* behind itself: where it is */
    if ((int32_t)win->owner <= 0) {             /* the Wimp's own (a menu): opened now */
        wimp_open_own(win, b);
        return 0;
    }
    wimp_queue_message(2, b, 32, RECV_TASK, win->owner, 0);         /* Open_Window_Request */
    w->queue_tail->window = win->handle;
    return 1;
}

/* The last task gone: the Wimp's own windows with it */
void wimp_windows_end(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *bw = w->back_window ? wimp_window(w->back_window) : NULL;
    w->back_window = 0;
    if (bw)
        delete_window(bw);
    wimp_queue_discard();
    w->invalid.n = 0;
}
