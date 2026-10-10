/* menus.c -- menus: the one tree, its levels' windows and icons, tracking
 * the pointer through it, choosing, and closing.
 *
 * A menu block lives in its owner's application space.  RISC OS's Wimp
 * pages the owner in to read it while another task's poll tracks the
 * pointer or redraws a level.  Here the owner's slot is mapped at &8000
 * for that moment (ros_task_page_in), with no switch of task. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "wimp.h"

#define KEEP_WORD   0x5045454Bu          /* "KEEP" */
#define F_TITLE     (1u << 26)
#define F_VBAR      (1u << 28)
#define IF_TEXT     (1u << 0)
#define IF_SPRITE   (1u << 1)
#define IF_VCENTRE  (1u << 4)
#define IF_FILLED   (1u << 5)
#define IF_FONT     (1u << 6)
#define IF_INDIRECT (1u << 8)
#define IF_RJUST    (1u << 9)
#define IF_NUMERIC  (1u << 20)
#define IF_SELECTED (1u << 21)
#define IF_SHADED   (1u << 22)
#define IF_DELETED  (1u << 23)
#define MF_TICK     (1u << 0)
#define MF_DOTTED   (1u << 1)
#define MF_WRITABLE (1u << 2)
#define MF_WARNING  (1u << 3)
#define MF_TRAVERSE (1u << 4)
#define MF_LAST     (1u << 7)
#define MF_INDTITLE (1u << 8)
#define MAX_ITEMS   1024u

static struct wimp_menus *mn(void)
{
    return &wimp_ws()->mn;
}

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
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

static int before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint8_t cmos(uint32_t addr)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 161, c.r[1] = addr;
    ros_swi(&c, XOS_Byte);
    return (uint8_t)c.r[2];
}

/* A is the width of the arrow and tick column.  B is the 3D margin. */
int32_t wimp_menu_arrow_width(void)
{
    return (wimp_ws()->dr.threed & 1u) ? 32 : 24;
}

static int32_t margin(void)
{
    return (wimp_ws()->dr.threed & 1u) ? 4 : 0;
}

static int is_menu(uint32_t data)
{
    return (data & 3u) == 0;
}

static uint32_t root_of(uint32_t data)
{
    return is_menu(data) ? data - 28 : data;
}

/* ---- the state ---------------------------------------------------------------------- */

void wimp_menus_start(void)
{
    struct wimp_menus *m = mn();
    memset(m, 0, sizeof *m);
    m->top = -1;
    m->caretwin = 0xFFFFFFFFu;
    m->careticon = -1;
    m->which = -1;
    m->lastw = 0xFFFFFFFFu;
    m->lasti = -1;
    wimp_menu_config();
}

/* The menu delays and the switches, from the CMOS RAM */
void wimp_menu_config(void)
{
    struct wimp_menus *m = mn();
    uint8_t a = cmos(0x17), b = cmos(0x1B);
    m->timelimit = ((a & 15u) ^ 10u) * ((a & 16u) ? 10u : 1u) * 10u;
    m->dragdelay = ((b & 15u) ^ 10u) * ((b & 16u) ? 10u : 1u) * 10u;
    m->autoopen = (cmos(0xC5) >> 7) & 1u;
    m->clicksub = cmos(0xBC) & 1u;
}

int32_t wimp_menu_level(uint32_t handle)
{
    struct wimp_menus *m = mn();
    for (int32_t i = m->top; i >= 0; i--)
        if (m->handles[i] == handle)
            return i;
    return -1;
}

struct wimp_task *wimp_menu_owner(void)
{
    struct wimp_menus *m = mn();
    return m->task ? wimp_task_by_handle(m->task, 0) : NULL;
}

/* The owner's slot, for reading its blocks */
const void *wimp_menu_page_in(void)
{
    struct wimp_task *t = wimp_menu_owner();
    return ros_task_page_in(t ? t->rt : NULL);
}

void wimp_menu_page_back(const void *was)
{
    ros_task_page_back(was);
}

static void mousetrap(void)
{
    struct wimp_input *p = &wimp_ws()->in;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = (uint32_t)p->mx, c.r[1] = 0x52, c.r[2] = p->mb, c.r[3] = p->mt, c.r[4] = (uint32_t)p->my;
    ros_service_call(&c);                       /* Service_MouseTrap */
}

/* int_set_icon_state on a level's icon */
static void set_state(struct wimp_window *win, uint32_t i, uint32_t eor, uint32_t bic)
{
    uint8_t *ic = wimp_icon(win, i);
    if (!ic)
        return;
    uint32_t f = (rd(ic, 16) & ~bic) ^ eor;
    wr(ic, 16, f);
    wimp_icon_changed(win, i, wimp_icon_in_place(win, f));
}

static uint32_t icon_flags(struct wimp_window *win, uint32_t i)
{
    const uint8_t *ic = wimp_icon(win, i);
    return ic ? rd(ic, 16) : 0;
}

/* ---- MenusDeleted ------------------------------------------------------------------ */

static void menusdeleted(void)
{
    struct wimp_menus *m = mn();
    if (m->top < 0 || !m->task)
        return;
    uint8_t b[24];
    memset(b, 0, sizeof b);
    wr(b, 0, 24);
    wr(b, 16, 0x400C9u);
    wr(b, 20, root_of(m->data[0]));
    wimp_queue_message(17, b, 24, RECV_TASK, m->task, 0);
}

/* ---- the caret --------------------------------------------------------------------------- */

static void setmenucaret(uint32_t handle, int32_t icon)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_menus *m = mn();
    if (handle == m->caretwin && icon == m->careticon)
        return;
    if (m->caretwin == 0xFFFFFFFFu)
        m->oldcaret = w->caret;
    m->caretwin = handle;
    m->careticon = icon;
    struct wimp_caretblk c = { handle, icon, 0x0FFFFFFF, (int32_t)handle, (uint32_t)icon, -1 };
    wimp_caret_set(&c);
}

static void unsetmenucaret(void)
{
    struct wimp_menus *m = mn();
    m->caretwin = 0xFFFFFFFFu;
    struct wimp_caretblk c = m->oldcaret;
    if (c.w != 0xFFFFFFFFu && !wimp_window(c.w))
        c.w = 0xFFFFFFFFu;
    wimp_caret_set(&c);
}

/* ---- closing --------------------------------------------------------------------------- */

static void closemenus(int32_t k)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_menus *m = mn();
    while (m->top > k && m->top >= 0) {
        uint32_t h = m->handles[m->top];
        if (h == m->caretwin)
            unsetmenucaret();
        if (w->in.d.type && w->in.d.window == h)
            wimp_drag_cancel();
        struct wimp_window *win = wimp_window(h);
        if (win) {
            if (is_menu(m->data[m->top])) {
                wimp_delete_menu_window(win);
            } else {
                wimp_caret_nocaret(h);
                if (m->caretwin == h)
                    m->caretwin = 0xFFFFFFFFu;
                wimp_close_system(win);
            }
        }
        m->top--;
    }
    if (m->top < 0)
        m->handle = 0;
}

/* ---- the width ----------------------------------------------------------------------------- */

static int32_t string_width(uint32_t s, uint32_t *last)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = s, c.r[2] = 0x0FFFFFFFu, c.r[3] = 0x0FFFFFFFu, c.r[4] = 0xFFFFFFFFu, c.r[5] = 0x0FFFFFFFu;
    if (!call(XFont_StringWidth, &c))
        return 0;
    *last = c.r[2];
    return (int32_t)c.r[2];
}

/* menu_checkforsprite: w plus a V-centred sprite's width */
static int32_t check_sprite(int32_t w, uint32_t item, uint32_t f, int *autoflag, int outline)
{
    struct wimp_ws *ws = wimp_ws();
    if ((f & (IF_SPRITE | IF_VCENTRE)) != (IF_SPRITE | IF_VCENTRE))
        return w;
    char *name = (char *)ws->scratch + 400;
    uint32_t src;
    if (!(f & IF_INDIRECT)) {
        src = item + 12;
    } else if (!(f & IF_TEXT)) {
        *autoflag = 1;
        return w;
    } else {
        uint32_t v = ros_ld32(item + 16), c;
        if (v == 0 || v == 0xFFFFFFFFu)
            return w;
        for (;; v++) {
            c = ros_ld8(v);
            if (c < 32)
                return w;
            if (c == 's' || c == 'S')
                break;
        }
        src = v + 1;
    }
    unsigned n = 0;
    for (; n < 12; n++) {
        uint32_t c = ros_ld8(src + n);
        if (c <= 32 || c == ';' || c == ',')
            break;
        name[n] = (char)c;
    }
    name[n] = 0;
    uint32_t area = 0, sprite = wimp_pool_find(ros_addr(name), &area);
    if (!sprite)
        return w;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x228, c.r[1] = area, c.r[2] = sprite;
    if (!call(XOS_SpriteOp, &c))
        return w;
    uint32_t p = c.r[3], mode = c.r[6];
    if (outline) {
        ros_cpu_enter(&c);
        c.r[1] = p, c.r[2] = 0;
        if (!call(XFont_Converttopoints, &c))
            return w;
        p = c.r[1];
    }
    ros_cpu_enter(&c);
    c.r[0] = mode, c.r[1] = 4;
    uint32_t e = call(XOS_ReadModeVariable, &c) ? c.r[2] : 1;
    return w + (int32_t)(p << e);
}

static uint32_t text_of(uint32_t item_data, uint32_t f)
{
    return (f & IF_INDIRECT) ? ros_ld32(item_data) : item_data;
}

static void fix_width(uint32_t blk, uint32_t n)
{
    struct wimp_draw *d = &wimp_ws()->dr;
    uint32_t title = (ros_ld32(blk + 28) & MF_INDTITLE) ? ros_ld32(blk) : blk;
    int autoflag = 0;
    int32_t W;
    if (d->systemfont) {
        uint32_t last = 0;
        int32_t m = string_width(wimp_font_string(IF_INDIRECT | IF_TEXT, title), &last);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t item = blk + 28 + 24 * i, mf = ros_ld32(item), f = ros_ld32(item + 8);
            if (mf & MF_WRITABLE)
                autoflag = 1;
            int32_t w = 0;
            if (f & IF_TEXT)
                w = string_width(wimp_font_string(f, text_of(item + 12, f)), &last);
            w |= 1;
            w = check_sprite(w, item, f, &autoflag, 1);
            if (w > m)
                m = w;
        }
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = (uint32_t)m, c.r[2] = last;
        W = (call(XFont_ConverttoOS, &c) ? (int32_t)c.r[1] : 0) + 18;
    } else {
        int32_t m = 0;
        uint32_t lim = (ros_ld32(blk + 28) & MF_INDTITLE) ? 0x0FFFFFFFu : 12u;
        for (uint32_t k = 0; k < lim && ros_ld8(title + k) >= 32; k++)
            m += 16;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t item = blk + 28 + 24 * i, mf = ros_ld32(item), f = ros_ld32(item + 8);
            if (mf & MF_WRITABLE)
                autoflag = 1;
            int32_t w = 0;
            if (f & IF_TEXT) {
                uint32_t t = text_of(item + 12, f), l = (f & IF_INDIRECT) ? 0x0FFFFFFFu : 12u;
                for (uint32_t k = 0; k < l && ros_ld8(t + k) >= 32; k++)
                    w += 16;
            }
            w = check_sprite(w, item, f, &autoflag, 0);
            if (w > m)
                m = w;
        }
        W = m + 16;
    }
    int32_t old = (int32_t)ros_ld32(blk + 16);
    ros_st32(blk + 16, (uint32_t)(autoflag && old > W ? old : W));
}

/* ---- building a level ------------------------------------------------------------------------ */

static const uint8_t menuwindow[88] = {
    [24] = 0xFF, [25] = 0xFF, [26] = 0xFF, [27] = 0xFF,     /* behind -1 */
    [28] = 0x12, [31] = 0x80,                               /* flags &80000012 */
    [36] = 3, [37] = 1,                                     /* scroll outer, inner */
    [44] = 0x01, [45] = 0x00, [46] = 0x00, [47] = 0xF0,     /* extent y0 -&0FFFFFFF */
    [48] = 0xFF, [49] = 0xFF, [50] = 0xFF, [51] = 0x0F,     /* extent x1 &0FFFFFFF */
    [56] = 0x2D,                                            /* title flags */
    [64] = 1,                                               /* the Wimp's sprite pool */
};

/* goopenwindow: open the level, give it the caret, and push it on the tree */
static void open_level(struct wimp_window *win, struct wimp_box v, int32_t scx, int32_t scy)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_menus *m = mn();
    if (!m->external) {
        if (!m->reversed) {
            v.x0 += w->dx, v.x1 += w->dx;
        } else {
            v.x0 -= w->dx, v.x1 -= w->dx;
            int32_t width = v.x1 - v.x0;
            v.x0 -= width, v.x1 -= width;
        }
        m->external = 0;
    }
    uint8_t b[32];
    wr(b, 0, win->handle);
    wr(b, 4, (uint32_t)v.x0);
    wr(b, 8, (uint32_t)v.y0);
    wr(b, 12, (uint32_t)v.x1);
    wr(b, 16, (uint32_t)v.y1);
    wr(b, 20, (uint32_t)scx);
    wr(b, 24, (uint32_t)scy);
    wr(b, 28, 0xFFFFFFFFu);
    wimp_open_forced(win, b);
    for (uint32_t i = 0; i < win->nicons; i++) {
        uint32_t f = icon_flags(win, i), type = (f >> 12) & 15u;
        if ((type == 14 || type == 15) && !(f & (IF_SHADED | IF_DELETED))) {
            setmenucaret(win->handle, (int32_t)i);
            break;
        }
    }
    m->top++;
    m->handles[m->top] = win->handle;
    m->sel[m->top] = -1;
}

static os_error *build_menu(uint32_t blk, int32_t x, int32_t y)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_menus *m = mn();
    os_error *e = NULL;
    struct wimp_window *win = wimp_create_menu_window(menuwindow, &e);
    if (!win)
        return e;
    m->handle = win->handle;
    uint32_t n = 0;
    while (n < MAX_ITEMS && !(ros_ld32(blk + 28 + 24 * n++) & MF_LAST))
        ;
    fix_width(blk, n);
    int32_t A = wimp_menu_arrow_width(), B = margin();
    /* the title */
    uint32_t t0 = ros_ld32(blk), t1 = ros_ld32(blk + 4), t2 = ros_ld32(blk + 8);
    int ind = (ros_ld32(blk + 28) & MF_INDTITLE) != 0;
    uint32_t c, bits = 0;
    if (ind) {
        t1 = t2 = 0;
        if (!t0)
            ind = 0, c = 0;
        else
            c = ros_ld8(t0);
        if (c >= 32)
            bits |= F_TITLE;
        if (c == '\\')
            t0++, m->reversed = 1;
    } else {
        c = t0 & 0xFFu;
        if (c >= 32)
            bits |= F_TITLE;
        if (c == '\\')
            t0 = (t0 & ~0xFFu) | 32u, m->reversed = 1;
    }
    wr(win->def, 72, t0);
    wr(win->def, 76, t1);
    wr(win->def, 80, t2);
    wr(win->def, 56, 0x2Du | (m->reversed ? 0 : 1u << 20) | (ind ? IF_INDIRECT : 0));
    /* the colours */
    for (int k = 0; k < 4; k++)
        win->def[32 + k] = (uint8_t)ros_ld8(blk + 12 + (uint32_t)k);
    win->def[38] = win->def[33];
    /* the layout */
    int32_t W = (int32_t)ros_ld32(blk + 16), H = (int32_t)ros_ld32(blk + 20), G = (int32_t)ros_ld32(blk + 24);
    int32_t y1 = -B - (G >> 1), y0 = y1 - H;
    int colour = 0;
    for (uint32_t i = 0; i < n; i++)
        if (ros_ld32(blk + 28 + 24 * i + 8) & 0xF0000000u)
            colour = 1;
    m->data[m->top + 1] = blk + 28;
    int textured = (w->dr.threed & 0x10u) != 0;
    uint8_t *icons;
    if (xos_module_claim(96 * n, (void **)&icons)) {
        wimp_delete_menu_window(win);
        return ros_error(E_TOO_BIG, "There is not enough memory to create this window or menu");
    }
    uint32_t prev1 = (uint32_t)H;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t item = blk + 28 + 24 * i;
        uint32_t mf = ros_ld32(item), sub = ros_ld32(item + 4), f = ros_ld32(item + 8);
        uint32_t d0 = ros_ld32(item + 12), d1 = ros_ld32(item + 16), d2 = ros_ld32(item + 20);
        uint32_t cc = (f & IF_FONT) ? 0x07000000u : (f & 0xFF000000u), sh = f & IF_SHADED;
        int hassub = sub != 0 && sub != 0xFFFFFFFFu;
        uint8_t *tk = icons + 96 * i, *it = tk + 32, *ar = tk + 64;
        /* the tick */
        int32_t bx0 = m->reversed ? W + A : 0;
        wr(tk, 0, (uint32_t)bx0);
        wr(tk, 4, (uint32_t)y0);
        wr(tk, 8, (uint32_t)(bx0 + A));
        wr(tk, 12, (uint32_t)y1);
        uint32_t tf = 0x000E001Au | cc | sh | (colour ? IF_FILLED : 0);
        if (!(mf & MF_TICK))
            tf &= ~IF_SPRITE;
        wr(tk, 16, tf);
        wr(tk, 20, (mf & MF_TICK) ? 0xD80u : 0xD00u);
        wr(tk, 24, prev1);
        wr(tk, 28, mf);
        /* the item */
        uint32_t g = f;
        if (!(g & IF_FILLED))
            g |= (uint32_t)(win->def[35] & 15u) << 28;
        g &= ~0xF000u;
        if (mf & MF_WRITABLE)
            g |= 0xF000u;
        else if (!m->reversed)
            g |= IF_NUMERIC;
        else if (g & IF_NUMERIC)
            g |= IF_RJUST;
        g &= ~0x000F0000u;
        if (!textured)
            g |= IF_FILLED;
        else if (colour || (g & IF_SPRITE))
            g |= IF_FILLED;
        else
            g &= ~(IF_FILLED | 0xF0000000u);
        g |= (((g >> 12) & 15u) < 14 ? 12u : 13u) << 16;
        if (textured) {                         /* writable items filled, others black behind */
            if (mf & MF_WRITABLE)
                g |= IF_FILLED;
            else if (!(g & IF_SPRITE) && !colour)
                g |= 0x70000000u;
        }
        wr(it, 0, (uint32_t)A);
        wr(it, 4, (uint32_t)y0);
        wr(it, 8, (uint32_t)(A + W));
        wr(it, 12, (uint32_t)y1);
        wr(it, 16, g);
        wr(it, 20, d0);
        wr(it, 24, d1);
        wr(it, 28, d2);
        /* the arrow */
        int32_t ax0 = m->reversed ? 0 : A + W;
        wr(ar, 0, (uint32_t)ax0);
        wr(ar, 4, (uint32_t)y0);
        wr(ar, 8, (uint32_t)(ax0 + A));
        wr(ar, 12, (uint32_t)y1);
        uint32_t af = 0x000F001Au | cc | sh | (colour ? IF_FILLED : 0);
        if (!hassub)
            af &= ~IF_SPRITE;
        wr(ar, 16, af);
        wr(ar, 20, hassub ? (m->reversed ? 0xD88u : 0xD89u) : 0xD00u);
        wr(ar, 24, d1);
        wr(ar, 28, d2);
        prev1 = d1;
        /* the rows */
        if (mf & MF_DOTTED)
            y0 -= 24;
        if (mf & MF_LAST) {
            y1 = y0 - (G >> 1);
            break;
        }
        y1 = y0 - G;
        y0 = y1 - H;
    }
    y1 -= B;
    e = wimp_icons_set(win, icons, 3 * n);
    xos_module_free(icons);
    if (e) {
        wimp_delete_menu_window(win);
        return e;
    }
    /* extent and scroll bar */
    int32_t E = y1;
    wr(win->def, 44, (uint32_t)E);
    if (w->screen_h - w->furn.T + E <= 0)
        bits |= F_VBAR;
    wimp_window_set_flags(win, bits);
    struct wimp_box v = { x, y + E, x + W + 2 * A, y };
    open_level(win, v, 0, m->scrolly);
    return NULL;
}

/* int_create_menu: a menu or a dialogue box as the next level */
static os_error *create_level(uint32_t r1, int32_t x, int32_t y, int keep_scroll)
{
    struct wimp_menus *m = mn();
    if (!keep_scroll)
        m->scrolly = 0;
    if (m->top >= WIMP_MENU_LEVELS - 1)
        return ros_error(0x289, "Too many menus");
    if (is_menu(r1))
        return build_menu(r1, x, y);
    m->handle = r1;                             /* a dialogue box */
    struct wimp_window *win = wimp_window(r1);
    if (!win)
        return wimp_error(E_BAD_HANDLE);
    struct wimp_box v = win->s[REQ].vis;
    v.x1 += x - v.x0, v.x0 = x;
    v.y0 += y - v.y1, v.y1 = y;
    m->data[m->top + 1] = r1;
    open_level(win, v, win->s[REQ].scx, win->s[REQ].scy);
    return NULL;
}

/* ---- the highlight ----------------------------------------------------------------------------- */

static struct wimp_window *top_window(void)
{
    struct wimp_menus *m = mn();
    return m->top >= 0 ? wimp_window(m->handles[m->top]) : NULL;
}

static uint32_t item_flags(int32_t level, int32_t i)
{
    return ros_ld32(mn()->data[level] + 24 * (uint32_t)i);
}

static void deselecticon(struct wimp_window *win, uint32_t n)
{
    set_state(win, n, 0, IF_SELECTED);
}

static void selecticon(struct wimp_window *win, uint32_t n)
{
    uint32_t e = (icon_flags(win, n) >> 16) & 15u;
    for (uint32_t k = win->nicons; k-- > 0;) {
        uint32_t f = icon_flags(win, k);
        if (k != n && ((f >> 16) & 15u) == e && (f & IF_SELECTED))
            set_state(win, k, 0, IF_SELECTED);
    }
    if (!(icon_flags(win, n) & IF_SELECTED))
        set_state(win, n, IF_SELECTED, IF_SELECTED);
}

static void menuunhighlight(struct wimp_window *win, int32_t o)
{
    struct wimp_menus *m = mn();
    if (o < 0 || !win)
        return;
    if (is_menu(m->data[m->top]) && (item_flags(m->top, o) & MF_WRITABLE))
        return;
    deselecticon(win, 3 * (uint32_t)o + 1);
}

/* menuh_altentry: 0 when the item is shaded */
static int altentry(int32_t i, int32_t r)
{
    struct wimp_menus *m = mn();
    struct wimp_window *win = top_window();
    if (i < 0 || !win)
        return 1;
    uint32_t n = 3 * (uint32_t)i + 1;
    if (icon_flags(win, n) & IF_SHADED)
        return 0;
    if (i == r)
        return 1;
    if (item_flags(m->top, i) & MF_WRITABLE)
        setmenucaret(win->handle, (int32_t)n);
    else
        selecticon(win, n);
    return 1;
}

static int menuhighlight(int32_t i)
{
    struct wimp_menus *m = mn();
    int32_t o = m->sel[m->top];
    m->sel[m->top] = i;
    menuunhighlight(top_window(), o);
    return altentry(i, -1);
}

static int trymenuhighlight(int32_t i)
{
    struct wimp_menus *m = mn();
    if (i == m->sel[m->top])
        return altentry(i, i);
    mousetrap();
    return menuhighlight(i);
}

/* ---- Wimp_CreateMenu, Wimp_CreateSubMenu --------------------------------------------------------- */

static void justcreate(uint32_t r1, int32_t x, int32_t y, os_error **e)
{
    struct wimp_menus *m = mn();
    if (r1 != 0 && r1 != 0xFFFFFFFFu)
        *e = create_level(r1, x, y, 0);
    if (m->top < 0)
        m->task = 0;
    m->external = 0;
}

void wimp_swi_CreateMenu(struct ros_cpu *s)
{
    struct wimp_menus *m = mn();
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    uint32_t r1 = s->r[1];
    m->temporary = 0;
    if (r1 == KEEP_WORD) {
        s->v = 0;
        return;
    }
    m->reversed = 0;
    m->external = 1;
    int32_t k = -1;
    if (m->task != t->handle) {
        menusdeleted();
        m->task = t->handle;
    } else {
        if (m->top >= 0 && r1 != root_of(m->data[0]))
            menusdeleted();
        uint32_t p = r1;
        while (k < m->top) {
            if (root_of(m->data[k + 1]) != p)
                break;
            k++;
            int32_t sel = m->sel[k];
            if (sel < 0)
                break;
            int past = 0;
            for (int32_t j = 0; j < sel && !past; j++)
                if (ros_ld32(p + 28 + 24 * (uint32_t)j) & MF_LAST)
                    past = 1;
            if (past)
                break;
            uint32_t item = p + 28 + 24 * (uint32_t)sel;
            if ((ros_ld32(item + 8) & IF_SHADED) && !(ros_ld32(item) & MF_TRAVERSE))
                break;
            p = ros_ld32(item + 4);
        }
    }
    closemenus(k);
    os_error *e = NULL;
    if (k < 0) {
        justcreate(r1, (int32_t)s->r[2], (int32_t)s->r[3], &e);
    } else {
        struct { uint32_t r1; int32_t x, y, sel, scy; } saved[WIMP_MENU_LEVELS];
        for (int32_t i = k; i >= 0; i--) {
            struct wimp_window *win = wimp_window(m->handles[i]);
            saved[i].r1 = root_of(m->data[i]);
            saved[i].x = win ? win->s[REQ].vis.x0 : 0;
            saved[i].y = win ? win->s[REQ].vis.y1 : 0;
            saved[i].sel = m->sel[i];
            saved[i].scy = win ? win->s[REQ].scy : 0;
        }
        closemenus(-1);
        for (int32_t i = 0; i <= k && !e; i++) {
            m->external = 1;
            m->scrolly = saved[i].scy;
            e = create_level(saved[i].r1, saved[i].x, saved[i].y, 1);
            if (!e)
                menuhighlight(saved[i].sel);
        }
        m->external = 0;
    }
    m->external = 0;
    if (e)
        wimp_fail(s, e);
    else
        s->v = 0;
}

void wimp_swi_CreateSubMenu(struct ros_cpu *s)
{
    struct wimp_menus *m = mn();
    struct wimp_task *t = wimp_current();
    if (!t || m->task != t->handle) {
        wimp_fail(s, ros_error(0x29A, "Submenus require a parent menu tree"));
        return;
    }
    m->external = 0;
    os_error *e = NULL;
    justcreate(s->r[1], (int32_t)s->r[2], (int32_t)s->r[3], &e);
    if (e)
        wimp_fail(s, e);
    else
        s->v = 0;
}

/* ---- Wimp_GetMenuState, Wimp_DecodeMenu --------------------------------------------------------- */

void wimp_swi_GetMenuState(struct ros_cpu *s)
{
    struct wimp_menus *m = mn();
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3];
    if (r0 > 1) {
        wimp_fail(s, wimp_error(E_BAD_SYSINFO));
        return;
    }
    s->v = 0;
    if (r0 == 0)
        r2 = 0xFFFFFFFFu;
    struct wimp_task *t = wimp_current();
    if (!t || t->handle != m->task || m->top < 0) {
        ros_st32(r1, 0xFFFFFFFFu);
        return;
    }
    if (m->data[m->top] == r2)
        r3 = 0xFFFFFFFFu;
    uint32_t v = (int32_t)r3 < 0 ? 0xFFFFFFFFu : r3 / 3;
    uint32_t p = r1;
    int matched = 0;
    for (int32_t i = 0; i <= m->top; i++, p += 4) {
        if (m->handles[i] == r2) {
            ros_st32(p, v);
            p += 4;
            matched = 1;
            break;
        }
        ros_st32(p, (uint32_t)m->sel[i]);
    }
    if (!matched && r0 == 1)
        p = r1;
    ros_st32(p, 0xFFFFFFFFu);
}

void wimp_swi_DecodeMenu(struct ros_cpu *s)
{
    uint32_t menu = s->r[1], list = s->r[2], out = s->r[3], first = out;
    for (;;) {
        uint32_t n = ros_ld32(list);
        list += 4;
        if ((int32_t)n < 0) {
            ros_st8(out, 13);
            break;
        }
        uint32_t item = menu + 28 + 24 * n, next = ros_ld32(item + 4), f = ros_ld32(item + 8);
        uint32_t t, k;
        if (f & IF_INDIRECT)
            t = ros_ld32(item + 12), k = ros_ld32(item + 20);
        else
            t = item + 12, k = 11;
        if (out != first)
            ros_st8(out++, '.');
        for (uint32_t j = 0; j <= k; j++) {
            uint32_t c = ros_ld8(t + j);
            if (c < 32)
                break;
            ros_st8(out++, c);
        }
        menu = next;
    }
    s->v = 0;
}

/* ---- the poll's hooks ------------------------------------------------------------------------------ */

/* A tree left open by a choice is closed when any task next enters
 * Wimp_Poll */
void wimp_menu_poll_entry(void)
{
    struct wimp_menus *m = mn();
    if (!m->temporary)
        return;
    m->task = 0;
    m->temporary = 0;
    closemenus(-1);
}

/* wipewindows: the owner has gone */
void wimp_menu_task_gone(uint32_t task)
{
    struct wimp_menus *m = mn();
    if (m->task != task)
        return;
    m->task = 0;
    closemenus(-1);
}

/* Escape closes the menu tree.  1 if there was one. */
int wimp_menu_escape(void)
{
    struct wimp_menus *m = mn();
    if (m->top < 0)
        return 0;
    menusdeleted();
    closemenus(-1);
    return 1;
}

/* The automatic-open deadline, set at the hit test */
void wimp_menu_pointer(struct wimp_window *win, int32_t icon, int nodrag)
{
    struct wimp_menus *m = mn();
    uint32_t h = win ? win->handle : 0xFFFFFFFFu;
    if (h == m->lastw && icon == m->lasti)
        return;
    m->lastw = h, m->lasti = icon;
    if (nodrag && win && win->owner == NO_WINDOW) {
        m->timeout = now_cs() + m->timelimit;
        m->byclick = 0;
    }
}

/* woggleicon: flash the chosen item */
static void woggle(struct wimp_window *win, uint32_t n)
{
    if (wimp_ws()->dr.threed & 0x10u)
        set_state(win, n, 0x07000020u, 0xFF000020u);
    for (int k = 0; k < 6; k++) {
        struct ros_cpu c;
        for (int j = 0; j < 2; j++) {
            ros_cpu_enter(&c);
            c.r[0] = 19;
            ros_swi(&c, XOS_Byte);
        }
        set_state(win, n, IF_SELECTED, 0);
    }
}

/* gomenuselect: 1 with Menu_Selection for the owner */
static int gomenuselect(uint32_t buttons, int *suppress, struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_menus *m = mn();
    int32_t L = m->which, sel = m->sel[L];
    struct wimp_window *win = wimp_window(m->handles[L]);
    if (sel >= 0 && win) {
        uint32_t item = m->data[L] + 24 * (uint32_t)sel, sub = ros_ld32(item + 4);
        uint32_t n = 3 * (uint32_t)sel + 1;
        int shaded = (icon_flags(win, n) & IF_SHADED) != 0;
        if (m->clicksub && sub != 0 && sub != 0xFFFFFFFFu) {
            if (!shaded || (ros_ld32(item) & MF_TRAVERSE))
                m->byclick = 1;
            return 0;
        }
        if (!shaded) {
            woggle(win, n);
            uint8_t b[4 * (WIMP_MENU_LEVELS + 1)];
            uint32_t size = 0;
            for (int32_t i = 0; i <= L; i++, size += 4)
                wr(b, size, (uint32_t)m->sel[i]);
            wr(b, size, 0xFFFFFFFFu);
            size += 4;
            struct wimp_task *t = wimp_menu_owner();
            if (w->singletask < 0)
                m->temporary = 1;
            else
                closemenus(-1);
            if (!t)
                return 0;
            ev->reason = 9;
            ev->size = size;
            memcpy(ev->data, b, size);
            ev->set_r2 = 1;
            ev->r2 = 0;
            *to = t;
            return 1;
        }
    }
    /* clickongreyitem */
    if (buttons & 1u)
        return 0;
    *suppress = 1;
    menusdeleted();
    closemenus(-1);
    return 0;
}

/* exitscanmenu */
static int click_test(int32_t icon, int *suppress, struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_input *p = &wimp_ws()->in;
    struct wimp_menus *m = mn();
    if (m->which >= 0 && m->which <= m->top && !is_menu(m->data[m->which]))
        return 0;
    if (!(p->mb & ~p->oldb))
        return 0;
    if (icon == -2 && m->which >= 0)
        return 0;
    if (m->which < 0 || m->which > m->top) {
        menusdeleted();
        closemenus(-1);
        return 0;
    }
    return gomenuselect(p->mb, suppress, to, ev);
}

static int notinamenu(int32_t icon, int *suppress, struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_menus *m = mn();
    if (m->top >= 0) {
        int32_t s = m->sel[m->top];
        m->sel[m->top] = -1;
        struct wimp_window *win = top_window();
        if (s >= 0 && win)
            deselecticon(win, 3 * (uint32_t)s + 1);
    }
    return click_test(icon, suppress, to, ev);
}

static void send_warning(uint32_t sub, int32_t x, int32_t y)
{
    struct wimp_menus *m = mn();
    uint8_t b[40 + 4 * WIMP_MENU_LEVELS];
    memset(b, 0, sizeof b);
    uint32_t size = 4 * (uint32_t)m->top + 40;
    wr(b, 0, size);
    wr(b, 16, 0x400C0u);
    wr(b, 20, sub);
    wr(b, 24, (uint32_t)x);
    wr(b, 28, (uint32_t)y);
    for (int32_t i = 0; i <= m->top; i++)
        wr(b, 32 + 4 * (uint32_t)i, (uint32_t)m->sel[i]);
    wr(b, 36 + 4 * (uint32_t)m->top, 0xFFFFFFFFu);
    wimp_queue_message(17, b, size, RECV_TASK, m->task, 0);
}

/* The top level */
static int in_top(int32_t icon, int *suppress, struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_menus *m = mn();
    if (!is_menu(m->data[m->top]))
        return click_test(icon, suppress, to, ev);
    if (icon < 0)
        return notinamenu(icon, suppress, to, ev);
    int32_t i = icon / 3;
    int z = trymenuhighlight(i);
    uint32_t now = now_cs();
    if (icon != 3 * i + 2) {
        if (!m->byclick) {
            if (!m->autoopen || before(now, m->timeout))
                return click_test(icon, suppress, to, ev);
        }
        m->inactive = now + m->dragdelay;
    }
    uint32_t item = m->data[m->top] + 24 * (uint32_t)i, mf = ros_ld32(item), sub = ros_ld32(item + 4);
    if ((!z && !(mf & MF_TRAVERSE)) || sub == 0 || sub == 0xFFFFFFFFu)
        return click_test(icon, suppress, to, ev);
    mousetrap();
    struct wimp_window *win = top_window();
    const uint8_t *ic = win ? wimp_icon(win, (uint32_t)icon) : NULL;
    if (!ic)
        return click_test(icon, suppress, to, ev);
    const struct wimp_place *pl = &win->s[REQ];
    int32_t x = m->reversed ? pl->vis.x0 : pl->vis.x0 + (int32_t)rd(ic, 8);
    int32_t y = pl->vis.y1 + (int32_t)rd(ic, 12) - pl->scy + ((w->dr.threed & 1u) ? 4 : 0);
    if (mf & MF_WARNING) {
        send_warning(sub, x, y);
    } else {
        m->external = 0;
        os_error *e = create_level(sub, x, y, 0);
        (void)e;                                /* RISC OS returns it from the poll */
    }
    return click_test(icon, suppress, to, ev);
}

/* scanmenus: 1 with an event.  *suppress is set when the click is eaten. */
int wimp_menu_scan(struct wimp_window *win, int32_t icon, int *suppress, struct wimp_task **to,
                   struct wimp_event *ev)
{
    struct wimp_input *p = &wimp_ws()->in;
    struct wimp_menus *m = mn();
    *suppress = 0;
    if (m->top < 0)
        return 0;
    const void *was = wimp_menu_page_in();
    /* findmenu */
    struct wimp_window *a = win;
    while (a && a->s[APP].parent != NO_WINDOW)
        a = wimp_window(a->s[APP].parent);
    int32_t L = a ? wimp_menu_level(a->handle) : -1;
    m->which = L;
    int r;
    if (L < 0) {
        r = notinamenu(icon, suppress, to, ev);
    } else if (L < m->top) {
        closemenus(L + 1);
        int32_t s = m->sel[L], arrow = 3 * s + 2, d = arrow - icon;
        uint32_t now = now_cs();
        if (icon == arrow)
            r = notinamenu(icon, suppress, to, ev);
        else if (!(p->mb & ~p->oldb) && before(now, m->inactive))
            r = notinamenu(icon, suppress, to, ev);
        else if (d >= 0 && d <= 2 && (m->byclick || (m->autoopen && before(m->timeout, now))))
            r = notinamenu(icon, suppress, to, ev);
        else {
            closemenus(L);
            r = in_top(icon, suppress, to, ev);
        }
    } else {
        r = in_top(icon, suppress, to, ev);
    }
    wimp_menu_page_back(was);
    return r;
}

/* crmenuselection: Return in a writable item, on the owner's own
 * poll */
int wimp_menu_return(struct wimp_window *win, int32_t icon, struct wimp_event *ev)
{
    struct wimp_menus *m = mn();
    int32_t L = wimp_menu_level(win->handle);
    if (L < 0)
        return 0;
    m->which = L;
    m->sel[L] = icon / 3;
    struct wimp_task *to = NULL;
    int suppress;
    return gomenuselect(wimp_ws()->in.mb, &suppress, &to, ev);
}

/* ---- the separators ---------------------------------------------------------------------------------- */

static void plot_at(uint32_t op, int32_t x, int32_t y)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = op, c.r[1] = (uint32_t)x, c.r[2] = (uint32_t)y;
    ros_swi(&c, XOS_Plot);
}

static void set_gcol(uint32_t colour)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = colour, c.r[3] = 0, c.r[4] = 0;
    ros_swi(&c, XColourTrans_SetGCOL);
}

/* With Use3DBorders and TexturedMenus, a separator is a groove: a bar in
 * the border face colour with its right end cut, under a bar in the
 * opposite colour with its left end cut (s/Wimp05:6136-6213).  The pixel
 * rounding uses dy for x too, as the source does. */
static void groove(int32_t x0, int32_t x1, int32_t y1)
{
    struct wimp_ws *w = wimp_ws();
    int32_t dy = w->dy, r = dy - 1;
    y1 = (y1 + 4 + r) & ~r;
    x1 -= dy << 1;
    x0 = (x0 + dy) & ~r;
    x1 = (x1 + r) & ~r;
    int alt = (w->dr.threed & 2u) != 0;
    set_gcol(w->dr.theme[alt ? TH_MBFC : TH_WBFC]);
    plot_at(96 + 4, x0 - 16, y1);
    plot_at(96 + 5, x1 - 4, y1 + 3);
    plot_at(80 + 0, 4, 0);
    plot_at(80 + 1, -3, -3);
    set_gcol(w->dr.theme[alt ? TH_MBOC : TH_WBOC]);
    plot_at(96 + 4, x1 + 16, y1 + 7);
    plot_at(96 + 5, x0 + 4, y1 + 4);
    plot_at(80 + 0, -4, 0);
    plot_at(80 + 1, 3, 3);
}

void wimp_menu_separators(struct wimp_window *win)
{
    struct wimp_menus *m = mn();
    int32_t L = wimp_menu_level(win->handle);
    if (L < 0 || !is_menu(m->data[L]))
        return;
    const void *was = wimp_menu_page_in();
    uint32_t data = m->data[L];
    int32_t H = (int32_t)ros_ld32(data - 8), G = (int32_t)ros_ld32(data - 4);
    wimp_gcol(wimp_colour(win->def[34] & 15u), 0);
    uint8_t v[10] = { 23, 6, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0 };
    wimp_vdu_bytes(v, sizeof v);
    const struct wimp_place *pl = &win->s[APP];
    int32_t x0 = pl->vis.x0, x1 = pl->vis.x1, y1 = pl->vis.y1 - pl->scy - (G >> 1);
    for (uint32_t i = 0; i < MAX_ITEMS; i++) {
        uint32_t mf = ros_ld32(data + 24 * i);
        y1 -= H;
        int32_t y0 = y1;
        y1 -= G;
        if (mf & MF_DOTTED) {
            y1 -= 24;
            int32_t ly = (y0 + y1) >> 1;
            if ((wimp_ws()->dr.threed & 0x11u) == 0x11u) {
                groove(x0, x1, y1);
            } else {
                plot_at(4, x0, ly);
                plot_at(0x15, x1, ly);
            }
        }
        if (mf & MF_LAST)
            break;
    }
    wimp_menu_page_back(was);
}
