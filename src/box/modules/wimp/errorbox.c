/* errorbox.c -- Wimp_ReportError.
 *
 * The error box is the `error' window of WindowManager:Templates, made at
 * the first Wimp_Initialise with its indirected data in the Wimp's
 * workspace.  A report builds it (title, sprites, buttons, the message's
 * width), opens it at the screen's middle, draws it at once, and waits in
 * a busy loop on the keyboard and the mouse.  No task runs meanwhile.  The
 * box saves nothing under it, so closing it leaves its outline invalid. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/swi.h"
#include "wimp.h"

#define XHourglass_Smash 0x606C3u
#define XScreenBlanker_Control 0x63100u
#define IF_DELETED  (1u << 23)
#define IF_SELECTED (1u << 21)

static const uint32_t progerrs[] = {
#include "progerrs.inc"
};

static struct wimp_err *er(void)
{
    return &wimp_ws()->err;
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

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

static uint32_t byte(uint32_t a, uint32_t x, uint32_t y)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = a, c.r[1] = x, c.r[2] = y;
    ros_swi(&c, XOS_Byte);
    return c.r[1] | (c.c ? 0x100u : 0);
}

static void vdu1(uint32_t ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ch;
    ros_swi(&c, XOS_WriteC);
}

/* Look a token up in the Wimp's Messages file, into buf (arena), %0 = arg */
static void lookup(const char *token, uint32_t buf, uint32_t size, uint32_t arg)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    if (!e->msgs_open) {
        char *name = (char *)w->scratch + 448;
        strcpy(name, "WindowManager:Messages");
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(e->msgs), c.r[1] = ros_addr(name), c.r[2] = 0;
        if (call(XMessageTrans_OpenFile, &c))
            e->msgs_open = 1;
    }
    char *tok = (char *)w->scratch + 480;
    snprintf(tok, 16, "%s", token);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = e->msgs_open ? ros_addr(e->msgs) : 0, c.r[1] = ros_addr(tok), c.r[2] = buf, c.r[3] = size;
    c.r[4] = arg, c.r[5] = c.r[6] = c.r[7] = 0;
    if (!call(XMessageTrans_Lookup, &c)) {
        uint32_t n = 0;
        for (; n + 1 < size && token[n]; n++)
            ros_st8(buf + n, (uint8_t)token[n]);
        ros_st8(buf + n, 0);
    }
}

/* A token's text where MessageTrans keeps it (R2 = 0), control-terminated,
 * or 0 if there is none.  The menu shortcut lists use it (draw.c). */
uint32_t wimp_message(const char *token)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    if (!e->msgs_open)
        lookup("NoError", ros_addr(e->scratch), sizeof e->scratch, 0);    /* opens the file */
    if (!e->msgs_open)
        return 0;
    char *tok = (char *)w->scratch + 480;
    snprintf(tok, 16, "%s", token);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(e->msgs), c.r[1] = ros_addr(tok), c.r[2] = 0, c.r[3] = 0;
    c.r[4] = c.r[5] = c.r[6] = c.r[7] = 0;
    return call(XMessageTrans_Lookup, &c) ? c.r[2] : 0;
}

/* ---- the window ---------------------------------------------------------------------- */

void wimp_errorbox_make(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    if (e->handle && wimp_window(e->handle))
        return;
    char *file = (char *)w->scratch + 448, *name = (char *)w->scratch + 480;
    strcpy(file, "WindowManager:Templates");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = ros_addr(file);
    if (!call(XWimp_OpenTemplate, &c))
        return;
    memset(name, 0, 12);
    strcpy(name, "error");
    ros_cpu_enter(&c);
    c.r[1] = 0, c.r[2] = 0, c.r[3] = 0, c.r[4] = 0, c.r[5] = ros_addr(name), c.r[6] = 0;
    void *blk = NULL;
    if (call(XWimp_LoadTemplate, &c) && c.r[6] && !xos_module_claim(c.r[1], &blk)) {
        ros_cpu_enter(&c);
        c.r[1] = ros_addr(blk), c.r[2] = ros_addr(e->title), c.r[3] = ros_addr(e->title) + sizeof e->title;
        c.r[4] = 0, c.r[5] = ros_addr(name), c.r[6] = 0;
        if (call(XWimp_LoadTemplate, &c) && c.r[6]) {
            os_error *err = NULL;
            struct wimp_window *win = wimp_create_system_icons(blk, &err);
            if (win) {
                wr(win->def, 64, 1);
                e->handle = win->handle;
                const uint8_t *i1 = wimp_icon(win, 1), *i4 = wimp_icon(win, 4);
                e->but_y0_def = (int32_t)rd(i1, 4), e->but_y1_def = (int32_t)rd(i1, 12);
                e->but_fl_def = rd(i1, 16) & ~(IF_SELECTED | IF_DELETED);
                e->but_va_def = rd(i1, 24), e->but_w_def = (int32_t)(rd(i1, 8) - rd(i1, 0));
                e->but_y0 = (int32_t)rd(i4, 4), e->but_y1 = (int32_t)rd(i4, 12);
                e->but_fl = rd(i4, 16), e->but_va = rd(i4, 24), e->but_w = (int32_t)(rd(i4, 8) - rd(i4, 0));
                e->app_x0 = (int32_t)rd(wimp_icon(win, 2), 0);
                e->type_x0 = (int32_t)rd(wimp_icon(win, 3), 0);
                e->mess_x0 = (int32_t)rd(wimp_icon(win, 0), 0);
            }
        }
    }
    if (blk)
        xos_module_free(blk);
    ros_cpu_enter(&c);
    ros_swi(&c, XWimp_CloseTemplate);
}

/* ---- the text form ---------------------------------------------------------------------- */

static void cli(const char *s)
{
    struct wimp_ws *w = wimp_ws();
    char *t = (char *)w->scratch + 448;
    snprintf(t, 32, "%s", s);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(t);
    ros_swi(&c, XOS_CLI);
}

static uint32_t mouse_buttons(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_Mouse);
    return c.r[2];
}

static void flush_kbd_mouse(struct wimp_err *e)
{
    byte(15, 1, 0);
    byte(21, 9, 0);
    e->old_mouse = mouse_buttons();
}

/* Copy the caller's error to the Wimp's own block, as s/Wimp07's
 * starterrorbox does "because messagetrans is unhelpful".  This must come
 * before anything that may make errors.  The runtime's error blocks are a
 * ring of eight, and the "Exec" before the box goes round it several
 * times, because each alias it looks for makes an error.  Without the
 * copy, the error shown would be whatever came last (#145). */
static void keep(struct wimp_err *e, uint32_t *r0)
{
    if (*r0 == ros_addr(e->wderror) || *r0 == ros_addr(e->copy))
        return;
    uint32_t n = 0;
    while (n < 240 && ros_ld8(*r0 + 4 + n) >= 32)
        n++;
    if (n + 1 < 237) {
        memcpy(e->copy, ros_ptr(*r0), 4 + n + 1);
        *r0 = ros_addr(e->copy);
    }
}

static uint32_t text_form(struct ros_cpu *s, uint32_t r0, uint32_t r1, uint32_t old, uint32_t caller_r1)
{
    struct wimp_err *e = er();
    if (!(r1 & 0x40u) && !((old & 0x80u) && (r1 & 0x20u)))
        keep(e, &r0);
    if (!(r1 & 8u))
        cli("Exec");
    if (r1 & 0x40u) {
        e->commandflag &= ~0x80u;
        return r1 & 3u;
    }
    if (!((old & 0x80u) && (r1 & 0x20u))) {
        vdu1(4);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_NewLine);
        ros_cpu_enter(&c);
        c.r[0] = r0 + 4;
        ros_swi(&c, XOS_Write0);
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_NewLine);
        if (r1 & 8u) {
            e->commandflag &= ~0x80u;
            return r1;
        }
        if (!(r1 & 0x20u)) {
            uint8_t *buf = e->scratch;
            lookup("Space", ros_addr(buf), 64, 0);
            ros_cpu_enter(&c);
            c.r[0] = ros_addr(buf);
            ros_swi(&c, XOS_Write0);
            ros_cpu_enter(&c);
            ros_swi(&c, XOS_NewLine);
        }
        flush_kbd_mouse(e);
    }
    for (;;) {
        uint32_t b = mouse_buttons(), key;
        int got = 0;
        if (b & ~e->old_mouse) {
            key = 0, got = 1;
        } else {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = 0x81, c.r[1] = 0, c.r[2] = 0;
            ros_swi(&c, XOS_Byte);
            if (c.r[1] == 0xFFu && c.c) {
                /* pollforkey: no key */
            } else if (c.c) {
                byte(124, 0, 0);
                key = 27, got = 1;
            } else {
                key = c.r[1], got = 1;
            }
        }
        e->old_mouse = b;
        if (!got) {
            if (caller_r1 & 0x20u)
                return 0;                       /* cf_suspended stays */
            ros_idle(2);
            continue;
        }
        uint32_t choice = key == 27 ? 2 : 1;
        if (caller_r1 & 4u)
            choice ^= 3;
        uint32_t allowed = caller_r1 & 3u ? caller_r1 & 3u : 1;
        if (!(choice & allowed))
            continue;
        (void)s;
        e->commandflag &= ~0x80u;
        return choice;
    }
}

/* ---- building the box ---------------------------------------------------------------- */

static int prog_error(uint32_t n)
{
    if ((n & 0x80000000u) || (n & 0x3F000000u) == 0x1B000000u)
        return 1;
    for (unsigned i = 0; i < sizeof progerrs / sizeof progerrs[0]; i++)
        if (progerrs[i] == n)
            return 1;
    return 0;
}

static uint8_t *icon(struct wimp_window *win, uint32_t i)
{
    return wimp_icon(win, i);
}

static void set_text_ptr(struct wimp_window *win, uint32_t i, uint32_t p)
{
    wr(icon(win, i), 20, p);
}

static uint32_t sprite_there(struct wimp_window *win, uint32_t name)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t area = rd(win->def, 64);
    struct ros_cpu c;
    if (area != 1) {
        ros_cpu_enter(&c);
        c.r[0] = 0x118, c.r[1] = area, c.r[2] = name;
        if (call(XOS_SpriteOp, &c))
            return 1;
    }
    uint32_t areas[2] = { w->ram_sprites, w->rom_sprites };
    for (int k = 0; k < 2; k++) {
        if (!areas[k])
            continue;
        ros_cpu_enter(&c);
        c.r[0] = 0x118, c.r[1] = areas[k], c.r[2] = name;
        if (call(XOS_SpriteOp, &c))
            return 1;
    }
    return 0;
}

static void build(struct wimp_window *win, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    e->flags = r1;
    /* the pointer and the character size */
    char *ptr = (char *)w->scratch + 448;
    strcpy(ptr, "ptr_default");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 36, c.r[2] = ros_addr(ptr), c.r[3] = 1, c.r[4] = c.r[5] = c.r[6] = c.r[7] = 0;
    ros_swi(&c, XWimp_SpriteOp);
    uint8_t chars[10] = { 23, 17, 7, 6, (uint8_t)(16u >> w->xeig), 0, (uint8_t)(32u >> w->yeig), 0, 0, 0 };
    wimp_vdu_bytes(chars, 10);
    /* the title */
    int none = (int32_t)r2 <= 0;
    uint32_t arg = 0;
    if (!none) {
        arg = ros_ld8(r2) == '\\' ? r2 + 1 : r2;
    }
    if (none)
        r1 &= ~0x10u;
    lookup(none ? "Error" : (r1 & 0x10u) ? "NoError" : "ErrorF", ros_addr(e->title), sizeof e->title, arg);
    /* the category sprite */
    static const char *const cats[8] = { "error", "information", "warning", "program", "question",
                                         "user1", "user2", "program" };
    int newtype = (r1 & 0x100u) != 0;
    unsigned k = newtype ? (r1 >> 9) & 7u : 0;
    if (r1 & (1u << 23))
        k += 3;
    uint8_t *i3 = icon(win, 3);
    memset(i3 + 20, 0, 12);
    memcpy(i3 + 20, cats[k & 7], strlen(cats[k & 7]));
    /* the default label and the application sprite */
    wr(win->def, 64, 1);
    uint8_t *i2 = icon(win, 2);
    memset(i2 + 20, 0, 12);
    if (newtype && (int32_t)r3 > 0) {
        if (e->spritearea > 1)
            wr(win->def, 64, e->spritearea);
        lookup("Continue", ros_addr(e->conttext), 24, 0);
        set_text_ptr(win, 1, ros_addr(e->conttext));
        for (unsigned n = 0; n < 11; n++)
            i2[20 + n] = (uint8_t)ros_ld8(r3 + n);
    } else {
        if (newtype) {
            lookup("Continue", ros_addr(e->conttext), 24, 0);
            set_text_ptr(win, 1, ros_addr(e->conttext));
        } else {
            lookup("OK", ros_addr(e->oktext), 24, 0);
            set_text_ptr(win, 1, ros_addr(e->oktext));
        }
        if ((int32_t)r2 <= 0 || ros_ld8(r2) <= 32) {
            memcpy(i2 + 20, "switcher", 8);
        } else {
            i2[20] = '!';
            for (unsigned n = 0; n < 10; n++)
                i2[21 + n] = (uint8_t)ros_ld8(r2 + n);
        }
    }
    char *nm = (char *)w->scratch + 448;
    memcpy(nm, i2 + 20, 12);
    nm[12] = 0;
    if (sprite_there(win, ros_addr(nm)))
        wr(i3, 12, rd(i2, 4) - 60);
    else
        wr(i3, 12, rd(i2, 12));
    /* which buttons */
    uint32_t D[16];
    unsigned nd = 0;
    if ((!newtype || (int32_t)e->buttonlist <= 0) && !(r1 & 3u))
        r1 |= 1u;
    for (uint32_t i = 1; i < win->nicons; i++)
        if (i == 1 || i == 4 || i == 6)
            wr(icon(win, i), 16, rd(icon(win, i), 16) | IF_DELETED);
    if (r1 & 1u)
        D[nd++] = 1;
    if (r1 & 2u)
        D[nd++] = 4;
    if ((r1 & 7u) == 7u && nd >= 2) {
        uint32_t t = D[0];
        D[0] = D[1], D[1] = t;
    }
    if (r1 & (1u << 23))
        D[nd++] = 6;
    e->iconend = 7;
    unsigned nlabels = 0;
    if (newtype && (int32_t)e->buttonlist > 0) {
        uint32_t src = e->buttonlist, o = 0;
        uint32_t starts[8];
        starts[nlabels++] = 0;
        for (; o < 255; o++) {
            uint32_t ch = ros_ld8(src + o);
            if (!(ch & 224u))
                break;
            if (ch == ',') {
                e->buttons[o] = 0;
                if (nlabels == 8)
                    break;
                starts[nlabels++] = o + 1;
                continue;
            }
            e->buttons[o] = (uint8_t)ch;
        }
        e->buttons[o] = 0;
        for (unsigned m = 0; m < nlabels; m++)
            set_text_ptr(win, 7 + m, ros_addr(e->buttons + starts[nlabels - 1 - m]));
        e->iconend = 7 + nlabels;
        for (unsigned m = nlabels; m > 0; m--)
            D[nd++] = 6 + m;
    }
    for (uint32_t i = e->iconend; i <= 14 && i < win->nicons; i++)
        wr(icon(win, i), 16, rd(icon(win, i), 16) | IF_DELETED);
    while (nd > 8) {
        nd--;
        wr(icon(win, D[nd]), 16, rd(icon(win, D[nd]), 16) | IF_DELETED);
    }
    /* sizes and positions */
    int32_t r = (int32_t)rd(win->def, 48) - 20;
    for (unsigned i = 0; i < nd; i++) {
        uint8_t *ic = icon(win, D[i]);
        int first = i == 0;
        wr(ic, 4, (uint32_t)(first ? e->but_y0_def : e->but_y0));
        wr(ic, 12, (uint32_t)(first ? e->but_y1_def : e->but_y1));
        wr(ic, 16, first ? e->but_fl_def : e->but_fl);
        wr(ic, 24, first ? e->but_va_def : e->but_va);
        int32_t W = wimp_icon_text_width(win, D[i]);
        int32_t wd = first ? e->but_w_def : e->but_w;
        if (W + 36 > wd)
            wd = W + 36;
        wr(ic, 0, (uint32_t)(r - wd));
        wr(ic, 8, (uint32_t)r);
        r -= wd + 20;
    }
    /* the width */
    int32_t L = r < 0 ? r : 0;
    uint8_t *i0 = icon(win, 0);
    wr(i0, 0, (uint32_t)(L + e->mess_x0));
    set_text_ptr(win, 0, r0 + 4);
    for (;;) {
        struct wimp_box b = { (int32_t)rd(i0, 0), (int32_t)rd(i0, 4), (int32_t)rd(i0, 8), (int32_t)rd(i0, 12) };
        int lines = wimp_count_lines(win, rd(i0, 16), r0 + 4, b);
        if (lines <= 7)
            break;
        wr(i0, 0, rd(i0, 0) - 48);
    }
    L = (int32_t)rd(i0, 0) - e->mess_x0;
    wr(win->def, 40, (uint32_t)L);
    uint8_t *i5 = icon(win, 5);
    wr(i2, 8, (uint32_t)(L + e->app_x0) + (rd(i2, 8) - rd(i2, 0)));
    wr(i2, 0, (uint32_t)(L + e->app_x0));
    wr(i3, 8, (uint32_t)(L + e->type_x0) + (rd(i3, 8) - rd(i3, 0)));
    wr(i3, 0, (uint32_t)(L + e->type_x0));
    wr(i5, 0, (uint32_t)L);
    wr(i5, 8, rd(win->def, 48));
    e->L = L;
}

/* Open the box at the middle, draw it now, and keep the pointer in it */
static void show(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    int32_t bw = (int32_t)rd(win->def, 48) - e->L;
    int32_t bh = win->s[REQ].vis.y1 - win->s[REQ].vis.y0;
    struct ros_cpu c;
    int32_t v[4];
    uint32_t vars[4] = { 12, 5, 11, 4 };
    for (int i = 0; i < 4; i++) {
        ros_cpu_enter(&c);
        c.r[0] = 0xFFFFFFFFu, c.r[1] = vars[i];
        ros_swi(&c, XOS_ReadModeVariable);
        v[i] = (int32_t)c.r[2];
    }
    int32_t Y = (int32_t)((uint32_t)(v[0] << v[1]) >> 1), X = (int32_t)((uint32_t)(v[2] << v[3]) >> 1);
    uint8_t blk[32];
    int32_t x0 = X - (int32_t)((uint32_t)bw >> 1), y0 = Y - (int32_t)((uint32_t)bh >> 1);
    wr(blk, 0, win->handle);
    wr(blk, 4, (uint32_t)x0), wr(blk, 8, (uint32_t)y0), wr(blk, 12, (uint32_t)(x0 + bw)), wr(blk, 16, (uint32_t)(y0 + bh));
    wr(blk, 20, (uint32_t)win->s[REQ].scx), wr(blk, 24, (uint32_t)win->s[REQ].scy);
    wr(blk, 28, 0xFFFFFFFFu);
    wimp_open_own(win, blk);
    wimp_flush();
    wimp_redraw_now(win);
    wimp_invalidate_box(win->s[APP].outline);   /* the outline stays invalid */
    ros_cpu_enter(&c);
    ros_swi(&c, XHourglass_Smash);
    struct wimp_box o = win->s[APP].outline;
    uint8_t *b = w->scratch;
    int32_t q[4] = { o.x0, o.y0, o.x1 - 1, o.y1 - 1 };
    b[0] = 1;
    for (int i = 0; i < 4; i++) {
        int32_t t = q[i] < -0x7FFF ? -0x7FFF : q[i] > 0x7FFF ? 0x7FFF : q[i];
        b[1 + 2 * i] = (uint8_t)t, b[2 + 2 * i] = (uint8_t)(t >> 8);
    }
    ros_cpu_enter(&c);
    c.r[0] = 21, c.r[1] = ros_addr(b);
    ros_swi(&c, XOS_Word);
    ros_cpu_enter(&c);
    c.r[0] = 1;
    ros_swi(&c, XScreenBlanker_Control);
    flush_kbd_mouse(e);
}

static void start(struct wimp_window *win, uint32_t *r0, uint32_t *r1, uint32_t *r2, uint32_t *r3)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    keep(e, r0);
    *r1 &= ~(1u << 23);
    e->icondata = 0;
    if (!e->open) {
        uint32_t num = ros_ld32(*r0);
        if (prog_error(num)) {                  /* a program error */
            e->progsave[0] = *r0, e->progsave[1] = *r1, e->progsave[2] = *r2, e->progsave[3] = *r3;
            wr(e->wderror, 0, 0x07000000u);
            e->cancelstr = rd(icon(win, 4), 20);
            e->icondata = 1;
            lookup("Quit", ros_addr(e->quittext), 24, 0);
            set_text_ptr(win, 4, ros_addr(e->quittext));
            uint32_t a = ((int32_t)*r2 > 0) ? (ros_ld8(*r2) == '\\' ? *r2 + 1 : *r2) : 0;
            if (!a) {
                strcpy((char *)e->scratch, "Application");
                a = ros_addr(e->scratch);
            }
            lookup("ErrorP", ros_addr(e->wderror) + 4, 244, a);
            *r0 = ros_addr(e->wderror);
            e->describebuttons = e->buttonlist;
            uint32_t orig = *r1;
            *r1 = (1u << 23) | 0x100u | 3u;
            if (!(orig & 0x100u))
                *r3 = 0, e->spritearea = 0;
            e->buttonlist = 0;
        }
        e->open = 1;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 1, c.r[1] = 0x57;
        ros_service_call(&c);                   /* Service_WimpReportError */
        ros_cpu_enter(&c);
        c.r[0] = 60, c.r[1] = 0, c.r[2] = 0, c.r[3] = wimp_ws()->save_context;
        ros_swi(&c, XOS_SpriteOp);              /* output to the screen, with a save area */
        memcpy(e->sprite_save, c.r, 16);
        ros_cpu_enter(&c);
        c.r[0] = 0, c.r[1] = 0;
        ros_swi(&c, XOS_ChangeRedirection);
        e->redir[0] = c.r[0], e->redir[1] = c.r[1];
        e->old_fx3 = byte(3, 0x14, 0) & 0xFFu;
        byte(218, 0, 0);
        e->vdu_status = byte(117, 0, 0) & 0xFFu;
        if (e->vdu_status & 0x80u)
            vdu1(6);
        uint8_t wf;
        ros_cpu_enter(&c);
        c.r[0] = 161, c.r[1] = 0xC5;
        ros_swi(&c, XOS_Byte);
        wf = (uint8_t)c.r[2];
        if (!(wf & 0x10u) && !(*r1 & 0x80u))
            vdu1(7);                            /* the beep */
        e->saved_pending = w->pending_window;
        w->pending_window = 0;
    }
    build(win, *r0, *r1, *r2, *r3);
    show(win);
}

/* ---- finishing ----------------------------------------------------------------------------- */

static uint32_t finish(struct wimp_window *win, uint32_t i)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    uint8_t *ic = icon(win, i);
    uint32_t f = rd(ic, 16) | IF_SELECTED;
    wr(ic, 16, f);
    wimp_icon_changed(win, i, wimp_icon_in_place(win, f));
    uint8_t *b = w->scratch;
    int32_t q[4] = { 0, 0, w->screen_w - 1, w->screen_h - 1 };
    b[0] = 1;
    for (int k = 0; k < 4; k++)
        b[1 + 2 * k] = (uint8_t)q[k], b[2 + 2 * k] = (uint8_t)(q[k] >> 8);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 21, c.r[1] = ros_addr(b);
    ros_swi(&c, XOS_Word);
    wimp_close_system(win);
    wimp_flush();
    uint32_t code = i == 1 ? 1 : i == 4 ? 2 : e->iconend - (i - 2);
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0x400C1, c.r[2] = code, c.r[3] = e->buttonlist;
    ros_service_call(&c);                       /* Service_ErrorButtonPressed */
    ros_cpu_enter(&c);
    c.r[1] = 0x400C2, c.r[2] = code;
    ros_service_call(&c);                       /* Service_ErrorEnding */
    code = c.r[2];
    if (e->icondata) {                          /* a program error's report */
        set_text_ptr(win, 4, e->cancelstr);
        e->icondata = 0;
        uint32_t o = e->progsave[1];
        if (code != 2)
            code = (o >> 24) & 15u ? (o >> 24) & 15u : code;
        else
            code = (o >> 28) & 15u ? (o >> 28) & 15u : (o & 2u) ? 2 : 99;
    }
    if (e->vdu_status & 0x80u)
        vdu1(21);
    byte(3, e->old_fx3, 0);
    ros_cpu_enter(&c);
    c.r[0] = e->redir[0], c.r[1] = e->redir[1];
    ros_swi(&c, XOS_ChangeRedirection);
    w->pending_window = e->saved_pending;
    if (e->sprite_save[0]) {
        ros_cpu_enter(&c);
        memcpy(c.r, e->sprite_save, 16);
        ros_swi(&c, XOS_SpriteOp);
    }
    byte(124, 0, 0);
    e->commandflag &= ~0x80u;
    wimp_font_exit();
    byte(229, e->old_escape, 0);
    e->open = 0;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0x57;
    ros_service_call(&c);
    if (code == 99) {
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_Exit);
    }
    return code;
}

/* ---- the loop ------------------------------------------------------------------------ */

static int key_choice(uint32_t key, uint32_t *i)
{
    struct wimp_err *e = er();
    uint32_t f = e->flags, ret = 0, esc = 0;
    if (f & 1u)
        esc = 1;
    if (f & 2u)
        ret = esc, esc = 4;
    if ((f & 0x100u) && (int32_t)e->buttonlist > 0) {
        if (e->iconend >= 8 && !ret)
            ret = esc, esc = e->iconend - 1;
        if (e->iconend >= 9 && !ret)
            ret = esc, esc = e->iconend - 2;
    }
    if ((f & 7u) == 7u) {
        uint32_t t = ret;
        ret = esc, esc = t;
    }
    if (!esc)
        esc = 1;
    *i = (!ret || key == 27) ? esc : ret;
    return 1;
}

static int pass(struct wimp_window *win, uint32_t *chosen)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x81, c.r[1] = 0, c.r[2] = 0;
    ros_swi(&c, XOS_Byte);
    if (c.r[2] != 0xFF && !c.c && (c.r[1] == 13 || c.r[1] == 27))
        return key_choice(c.r[1], chosen);
    uint32_t old = e->old_mouse;
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_Mouse);
    int32_t mx = (int32_t)c.r[0] & ~(w->dx - 1), my = (int32_t)c.r[1] & ~(w->dy - 1);
    uint32_t b = c.r[2];
    e->old_mouse = b;
    int32_t icon_hit;
    struct wimp_window *hit = wimp_hit(mx, my, 0, &icon_hit);
    if (hit == win && (b & ~old & ~2u) &&
        (icon_hit == 1 || icon_hit == 4 || (icon_hit >= 6 && icon_hit <= 14))) {
        *chosen = (uint32_t)icon_hit;
        return 1;
    }
    return 0;
}

void wimp_swi_ReportError(struct ros_cpu *s)
{
    struct wimp_err *e = er();
    uint32_t caller_r1 = s->r[1];
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = 0x400C0;
    c.r[2] = s->r[0], c.r[3] = s->r[1], c.r[4] = s->r[2], c.r[5] = s->r[3], c.r[6] = s->r[4], c.r[7] = s->r[5];
    ros_service_call(&c);                       /* Service_ErrorStarting */
    uint32_t r0 = c.r[2], r1 = c.r[3], r2 = c.r[4], r3 = c.r[5];
    e->spritearea = c.r[6], e->buttonlist = c.r[7];
    uint32_t old = e->commandflag;
    e->commandflag = old | 0x80u;
    struct wimp_window *win = e->handle ? wimp_window(e->handle) : NULL;
    s->v = 0;
    if ((old | 0x80u) == (2u | 0x80u) || !win) {
        s->r[1] = text_form(s, r0, r1, old, caller_r1);
        return;
    }
    if (!(r1 & 0x40u) && !((old & 0x80u) && (r1 & 0x20u)))
        keep(e, &r0);
    cli("Exec");
    if (r1 & 0x40u) {                           /* finish */
        if (!(old & 0x80u)) {
            e->commandflag &= ~0x80u;
            s->r[1] = r1;
            return;
        }
        s->r[1] = finish(win, (r1 & 1u) ? 1 : 4);
        return;
    }
    if (!((old & 0x80u) && (r1 & 0x20u)))
        start(win, &r0, &r1, &r2, &r3);
    e->old_escape = byte(229, 0, 255) & 0xFFu;
    byte(229, 1, 0);
    for (;;) {
        uint32_t i;
        if (pass(win, &i)) {
            s->r[1] = finish(win, i);
            return;
        }
        if (caller_r1 & 0x20u) {
            s->r[1] = 0;
            return;
        }
        ros_idle(2);
    }
}
