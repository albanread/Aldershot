/* draw.c: drawing icons and the window furniture, to the pixel.
 *
 * This file follows WindowManager 5.88's drawing step by step, call by
 * call.  The screen belongs to the kernel.  It is reached through the VDU,
 * OS_Plot, OS_SpriteOp, ColourTrans and the Font Manager, as the
 * translated Wimp reached it, so that the pixels are the same.
 *
 * These are not yet here:
 * - the pressed furniture, which comes with the clicks;
 * - right-to-left text;
 * - border children (wf_inborder). */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "wimp.h"

enum { SC_NONE, SC_CENTRE, SC_ADD, SC_SEL };    /* what sets the text's scroll */

struct wimp_window;
static void sel_codes(void);
static void scroll_for(struct wimp_window *win, int32_t icon, int dest, int32_t v, int sel_ok);

#define IF_TEXT     (1u << 0)
#define IF_SPRITE   (1u << 1)
#define IF_BORDER   (1u << 2)
#define IF_HCENTRE  (1u << 3)
#define IF_VCENTRE  (1u << 4)
#define IF_FILLED   (1u << 5)
#define IF_FONT     (1u << 6)
#define IF_INDIRECT (1u << 8)
#define IF_RJUST    (1u << 9)
#define IF_HALF     (1u << 11)
#define IF_SELECTED (1u << 21)
#define IF_SHADED   (1u << 22)
#define IF_DELETED  (1u << 23)

#define F_BACK   (1u << 24)
#define F_CLOSE  (1u << 25)
#define F_TITLE  (1u << 26)
#define F_TOGGLE (1u << 27)
#define F_VBAR   (1u << 28)
#define F_SIZE   (1u << 29)
#define F_HBAR   (1u << 30)
#define F_FURNITURE 0x7F000000u

/* The theme words, as *WimpVisualFlags last left them */
const uint32_t wimp_theme_defaults[TH_COUNT] = {
    0xFFFFFF00u, 0x77777700u, 0xFFFFFF00u, 0x99999900u, 0xFFFFFF00u, 0xBBBBBB00u,
    0x77777700u, 0xDDDDDD00u, 0xBBEEEE00u, 0x99999900u, 0x00000000u,
};
#define BUTTON_BG      (wimp_ws()->dr.theme[TH_BC])
#define BUTTON_BG2     (wimp_ws()->dr.theme[TH_BHC])
#define BUTTON_FACE    (wimp_ws()->dr.theme[TH_BBFC])
#define BUTTON_OPP     (wimp_ws()->dr.theme[TH_BBOC])
#define BUTTON_SHALLOW (wimp_ws()->dr.theme[TH_BBSC])
#define BUTTON_WELL    (wimp_ws()->dr.theme[TH_BWC])
#define OUTLINE_COLOUR (wimp_ws()->dr.theme[TH_WOC])

static struct wimp_draw *dr(void)
{
    return &wimp_ws()->dr;
}

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

/* ---- primitives ----------------------------------------------------------- */

static void plot(uint32_t op, int32_t x, int32_t y)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = op, c.r[1] = (uint32_t)x, c.r[2] = (uint32_t)y;
    ros_swi(&c, XOS_Plot);
}

static int meets(struct wimp_box b)
{
    struct wimp_box c = dr()->clip;
    return b.x0 < c.x1 && b.y0 < c.y1 && c.x0 < b.x1 && c.y0 < b.y1;
}

static struct wimp_box meet(struct wimp_box a, struct wimp_box b)
{
    return (struct wimp_box){ a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
                              a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
}

static void solid(struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    if (b.x0 == b.x1 || b.y0 == b.y1)
        return;
    plot(4, b.x0, b.y0);
    plot(0x67, b.x1 - w->dx, b.y1 - w->dy);
}

static void hollow(struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    if (b.x0 == b.x1 || b.y0 == b.y1)
        return;
    int32_t x1 = b.x1 - w->dx, y1 = b.y1 - w->dy;
    plot(4, b.x0, b.y0);
    plot(5, x1, b.y0);
    plot(5, x1, y1);
    plot(5, b.x0, y1);
    plot(5, b.x0, b.y0);
}

static void fg(uint32_t c)
{
    wimp_gcol(c, 0);
}

static void bg(uint32_t c)
{
    wimp_gcol(c, 1);
}

static void gw(struct wimp_box b)
{
    wimp_graphics_window(b);
}

/* ---- the window's colours (setwindowcolours) ---------------------------- */

static void window_colours(const struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    const uint32_t *P = w->palette;
    const uint8_t *c = win->def + 32;
    d->win = win;
    d->titlefg = c[0] == 0xFF ? P[7] : P[c[0] & 15];
    d->titlebg = P[c[1] & 15];
    d->workfg = P[c[2] & 15];
    d->workbg = c[3] == 0xFF ? P[0] : P[c[3] & 15];
    d->scout = P[c[4] & 15];
    d->scin = P[c[5] & 15];
    d->titlebg2 = P[c[6] & 15];
    d->titlecolour = (win->s[APP].flags & (1u << 20)) ? d->titlebg2 : d->titlebg;
    uint32_t area;
    memcpy(&area, win->def + 64, 4);
    d->area = area;
}

/* ---- seticonptrs and the colours ---------------------------------------- */

static uint32_t findcommand(uint32_t v, uint32_t letter)
{
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

static int is_string(uint32_t v)
{
    return v != 0 && v != 0xFFFFFFFFu;
}

static uint32_t getnumber(uint32_t *p)
{
    uint32_t n = 0, c;
    while ((c = ros_ld8(*p)) >= '0' && c <= '9') {
        n = n * 10 + (c - '0');
        (*p)++;
    }
    return n;
}

static void getborder(uint32_t flags)
{
    struct wimp_draw *d = dr();
    uint32_t t = 0, h = 3;
    if ((flags & IF_INDIRECT) && (flags & IF_TEXT) && is_string(d->validation)) {
        uint32_t p = findcommand(d->validation, 'R');
        if (p) {
            t = getnumber(&p);
            if (ros_ld8(p) == ',') {
                p++;
                h = getnumber(&p);
            }
        }
    }
    if (t >= 8)
        t = 0;
    d->border_type = t;
    d->border_highlight = h;
}

static void read_true_colours(void)
{
    struct wimp_draw *d = dr();
    uint32_t p = findcommand(d->validation, 'C');
    if (!p)
        return;
    for (int k = 0; k < 8; k++) {
        if (ros_ld8(p) == '/') {
            p++;
            continue;
        }
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 16, c.r[1] = p;
        if (!call(XOS_ReadUnsigned, &c))
            return;
        d->tc[k] = c.r[2] << 8;
        p = c.r[1];
        if (ros_ld8(p) != '/')
            return;
        p++;
    }
}

static uint32_t fade(uint32_t c)
{
    uint32_t r = (c >> 8) & 0xFFu, g = (c >> 16) & 0xFFu, b = c >> 24;
    uint32_t t = 77 * r + 150 * g + 28 * b + 127;
    t = t + (t << 8) + 256;
    uint32_t y = t >> 16;
    uint32_t v = 255 - ((255 - y) >> 1);
    return v * 0x01010100u;
}

static uint32_t munge(uint32_t flags)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    const uint32_t *P = w->palette;
    uint32_t T = d->border_type, g = 0xFFFFFFFFu, *tc = d->tc;
    if (tc[TC_FG] == 0xFFFFFFFFu) {
        uint32_t i = ((flags & IF_FONT) ? d->fontfg : flags >> 24) & 15;
        g = i;
        tc[TC_FG] = P[i];
    }
    if (tc[TC_BG] == 0xFFFFFFFFu) {
        uint32_t j = ((flags & IF_FONT) ? d->fontbg : flags >> 28) & 15;
        tc[TC_BG] = ((flags & IF_BORDER) && T != 0 && j == 1 && g == 7) ? BUTTON_BG : P[j];
    }
    if (tc[TC_BG2] == 0xFFFFFFFFu) {
        uint32_t h = d->border_highlight & 15;
        tc[TC_BG2] = (h == 3 && g == 7) ? BUTTON_BG2 : P[h];
    }
    if (tc[TC_WELL] == 0xFFFFFFFFu)
        tc[TC_WELL] = T == 6 ? BUTTON_WELL : BUTTON_BG;
    if (tc[TC_FACE] == 0xFFFFFFFFu)
        tc[TC_FACE] = BUTTON_FACE;
    if (tc[TC_OPP] == 0xFFFFFFFFu)
        tc[TC_OPP] = (T == 3 || T == 4) ? BUTTON_SHALLOW : BUTTON_OPP;
    if (flags & IF_SELECTED) {
        if (T == 5 || T == 6) {
            tc[TC_BG] = tc[TC_BG2];
            if (!(flags & IF_SPRITE))
                flags |= IF_FILLED;
        } else if ((flags & IF_SHADED) || (flags & (IF_SPRITE | IF_FILLED))) {
            uint32_t nf = tc[TC_SELFG] != 0xFFFFFFFFu ? tc[TC_SELFG] : tc[TC_BG];
            uint32_t nb = tc[TC_SELBG] != 0xFFFFFFFFu ? tc[TC_SELBG] : tc[TC_FG];
            tc[TC_FG] = nf, tc[TC_BG] = nb;
            if (!(flags & IF_SPRITE))
                flags |= IF_FILLED;
        } else {
            uint32_t k = tc[TC_BG] ^ 0xFFFFFF00u;
            tc[TC_FG] ^= k;
            tc[TC_BG] = d->workbg ^ k;
        }
    }
    if (!(flags & (IF_SPRITE | IF_FILLED | IF_FONT)) && !(flags & IF_SELECTED))
        tc[TC_BG] = d->workbg;
    if (flags & IF_SHADED) {
        uint32_t old = tc[TC_FG];
        tc[TC_FG] = fade(tc[TC_FG]);
        if (tc[TC_FG] == tc[TC_BG] && tc[TC_FG] == d->workbg)
            tc[TC_FG] = old;
        if (!(T == 0 && tc[TC_BG] == d->workbg))
            tc[TC_BG] = fade(tc[TC_BG]);
        tc[TC_WELL] = fade(tc[TC_WELL]);
        tc[TC_FACE] = fade(tc[TC_FACE]);
        tc[TC_OPP] = fade(tc[TC_OPP]);
    }
    return flags;
}

/* Copies a name of up to 12 bytes.  A ';' or ',' ends it, stored as 0. */
static uint32_t scanname(uint32_t *p)
{
    struct wimp_draw *d = dr();
    uint32_t c = 0;
    unsigned n = 0;
    while (n < 12) {
        c = ros_ld8((*p)++);
        if (c < 32 || c == ';' || c == ',') {
            d->namebuf[n++] = 0;
            break;
        }
        d->namebuf[n++] = (uint8_t)c;
    }
    d->namebuf[n < 16 ? n : 15] = 0;
    return c;
}

static uint32_t seticonptrs(uint32_t flags, uint32_t data)
{
    struct wimp_draw *d = dr();
    d->this_area = d->area;
    d->spritename = data;
    d->validation = 0;
    d->border_type = 0;
    d->lengthflags = 1;
    d->linespacing = -1;
    memset(d->tc, 0xFF, sizeof d->tc);
    if (!(flags & IF_FONT)) {
        d->fontbg = flags >> 28;
        d->fontfg = (flags >> 24) & 15;
    } else {
        d->fontbg = 0;
        d->fontfg = 7;
    }
    if (!(flags & IF_INDIRECT))
        return munge(flags);
    uint32_t a = ros_ld32(data), b = ros_ld32(data + 4), c = ros_ld32(data + 8);
    d->spritename = a;
    if (!(flags & IF_TEXT))
        d->this_area = b;
    else
        d->validation = b;
    d->lengthflags = c;
    if (!(flags & IF_TEXT))
        return munge(flags);
    uint32_t v = d->validation;
    if (is_string(v)) {
        uint32_t p = findcommand(v, 'L');
        if (p) {
            d->linespacing = 40;
            uint32_t ch = ros_ld8(p);
            if (ch >= '0' && ch <= '9') {
                struct ros_cpu r;
                ros_cpu_enter(&r);
                r.r[0] = 10, r.r[1] = p;
                d->linespacing = call(XOS_ReadUnsigned, &r) ? (int32_t)r.r[2] : 40;
            }
        }
        if (flags & IF_FONT) {
            p = findcommand(v, 'F');
            if (p) {
                for (int k = 0; k < 2; k++) {
                    uint32_t ch = ros_ld8(p++), x = ch | 0x20u;
                    if (ch < 32)
                        break;
                    uint32_t val;
                    if (x >= '0' && x <= '9')
                        val = x - '0';
                    else if (x >= 'a' && x <= 'f')
                        val = x - 'a' + 10;
                    else
                        break;
                    if (k == 0)
                        d->fontbg = val;
                    else
                        d->fontfg = val;
                }
            }
        }
        getborder(flags);
        read_true_colours();
    }
    if (!(flags & IF_SPRITE) || !is_string(v))
        return munge(flags);
    uint32_t p = findcommand(v, 'S');
    if (!p)
        return munge(flags);
    uint32_t end = scanname(&p);
    if ((flags & IF_SELECTED) && end == ',') {
        flags &= ~IF_SELECTED;
        scanname(&p);
    }
    d->spritename = ros_addr(d->namebuf);
    d->lengthflags = 1;
    return munge(flags);
}

/* ---- borders ----------------------------------------------------------- */

static void slab_render(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint32_t c[4])
{
    plot(4, x0, y0);
    const int32_t to[4][2] = { { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
    for (int i = 0; i < 4; i++) {
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[0] = c[i];
        ros_swi(&r, XColourTrans_SetGCOL);
        plot(0x25, to[i][0], to[i][1]);
    }
}

static void slab(struct wimp_box *b, int kind)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    uint32_t c[4];
    if (kind == 0) {                            /* out */
        c[0] = c[1] = d->tc[TC_OPP];
        c[2] = c[3] = d->tc[TC_FACE];
    } else if (kind == 1) {                     /* in */
        c[0] = c[1] = d->tc[TC_FACE];
        c[2] = c[3] = d->tc[TC_OPP];
    } else {
        c[0] = c[1] = c[2] = c[3] = d->tc[TC_WELL];
    }
    int32_t x0 = b->x0 & ~(w->dx - 1), x1 = (b->x1 & ~(w->dx - 1)) - 1;
    int32_t y0 = b->y0 & ~(w->dy - 1), y1 = (b->y1 & ~(w->dy - 1)) - 1;
#define ADJ(k) (x0 += (k), y0 += (k), x1 -= (k), y1 -= (k))
    if (w->dx == 2 && w->dy == 4) {
        ADJ(2);
        slab_render(x0, y0, x1, y1, c);
        ADJ(-2);
        slab_render(x0, y0, x1, y1, c);
        ADJ(4);
    } else {
        int32_t m = w->dx < w->dy ? w->dx : w->dy;
        slab_render(x0, y0, x1, y1, c);
        ADJ(1);
        if (m == 1)
            slab_render(x0, y0, x1, y1, c);
        ADJ(1);
        slab_render(x0, y0, x1, y1, c);
        if (m == 1) {
            ADJ(1);
            slab_render(x0, y0, x1, y1, c);
            ADJ(1);
        } else {
            ADJ(2);
        }
    }
#undef ADJ
    *b = (struct wimp_box){ x0, y0, x1 + 1, y1 + 1 };
}

static void icon_fg(void)
{
    fg(dr()->tc[TC_FG]);
}

static void iconborder(uint32_t flags, struct wimp_box b)
{
    switch (dr()->border_type) {
    case 1: slab(&b, 0); break;
    case 2: slab(&b, 1); break;
    case 3: slab(&b, 0); slab(&b, 1); break;
    case 4: slab(&b, 1); slab(&b, 0); break;
    case 5: slab(&b, (flags & IF_SELECTED) ? 1 : 0); break;
    case 6:
        slab(&b, 1);
        slab(&b, 2);
        slab(&b, (flags & IF_SELECTED) ? 1 : 0);
        break;
    case 7:
        slab(&b, 1);
        slab(&b, 2);
        icon_fg();
        hollow(b);
        break;
    default: hollow(b); break;
    }
}

/* ---- sprites ----------------------------------------------------------------------- */

static int32_t mode_var(uint32_t mode, uint32_t var, int *ok)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = mode, c.r[1] = var;
    ros_swi(&c, XOS_ReadModeVariable);
    if (c.v || c.c) {
        *ok = 0;
        return 0;                       /* a defined value.  R2 after a failed call
                                           is whatever was left there.  A caller that
                                           forgets to look at ok must not use it. */
    }
    return (int32_t)c.r[2];
}

/* cachespriteaddress: returns the sprite, and the area it is in in *area.
 * Returns 0 if there is no such sprite. */
static uint32_t sprite_find(uint32_t *area)
{
    struct wimp_draw *d = dr();
    struct wimp_ws *w = wimp_ws();
    if (d->lengthflags == 0) {
        uint32_t s = d->spritename;
        uint32_t rom = w->rom_sprites;
        *area = (rom && s >= rom && s < rom + ros_ld32(rom)) ? rom : w->ram_sprites;
        return s;
    }
    unsigned n = 0;
    for (; n < 12; n++) {
        uint32_t c = ros_ld8(d->spritename + n);
        if (c <= 32)
            break;
        d->lookup[n] = (uint8_t)c;
    }
    d->lookup[n] = 0;
    uint32_t name = ros_addr(d->lookup), a = d->this_area;
    if (a == 0xFFFFFFFFu)
        a = 1;
    if (a == 0) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 3;
        a = call(XOS_ReadDynamicArea, &c) ? c.r[0] : 1;
    }
    if (a != 1) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0x118, c.r[1] = a, c.r[2] = name;
        if (call(XOS_SpriteOp, &c)) {
            *area = a;
            return c.r[2];
        }
    }
    return wimp_pool_find(name, area);
}

/* spritesize: the sprite's size in OS units.  The sprite's eigen factors,
 * and whether it needs scaling, are kept for the plot. */
static int sprite_size(uint32_t area, uint32_t sprite, uint32_t flags, int32_t *wos, int32_t *hos)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x228, c.r[1] = area, c.r[2] = sprite;
    if (!call(XOS_SpriteOp, &c)) {
        *wos = *hos = 0;
        return 0;
    }
    uint32_t wp = c.r[3], hp = c.r[4], m = c.r[6];
    if (m != d->sprite_lastmode) {
        int ok = 1;
        d->sprite_log2bpp = mode_var(m, 9, &ok);
        d->sprite_log2px = mode_var(m, 4, &ok);
        d->sprite_log2py = mode_var(m, 5, &ok);
        int32_t mf = mode_var(m, 0, &ok), nc = mode_var(m, 3, &ok);
        int sok = 1;
        int32_t smf = mode_var(0xFFFFFFFFu, 0, &sok), snc = mode_var(0xFFFFFFFFu, 3, &sok);
        uint32_t mask = 0xF000u | (w->log2bpp != 3 ? 0x80u : 0);
        int same = (w->log2bpp == 3 || nc == snc) && (((uint32_t)mf & mask) == ((uint32_t)smf & mask)) &&
                   !((uint32_t)mf & 0x8000u) && !(m & 0x80000000u) &&
                   d->sprite_log2bpp == (int32_t)w->log2bpp && d->sprite_log2px == (int32_t)w->xeig &&
                   d->sprite_log2py == (int32_t)w->yeig;
        d->sprite_needs = !same;
        int32_t sx = d->sprite_log2px, sy = d->sprite_log2py;
        int32_t mx = (int32_t)w->xeig, my = (int32_t)w->yeig;
        d->sprite_scale[0] = sx > mx ? 1u << (sx - mx) : 1u;
        d->sprite_scale[2] = sx < mx ? 1u << (mx - sx) : 1u;
        d->sprite_scale[1] = sy > my ? 1u << (sy - my) : 1u;
        d->sprite_scale[3] = sy < my ? 1u << (my - sy) : 1u;
        d->sprite_lastmode = ok ? m : 0xFFFFFFFFu;
        if (!ok) {
            *wos = *hos = 0;            /* the mode's variables could not be read.
                                           There is no size, so there is no plot. */
            return 0;
        }
    }
    /* An eigen factor is 0 to 3.  Shifting by anything else is undefined, but
     * the width it made used to be used.  A huge width sent the centred
     * position far off to the left.  There the caller's "is any of it on
     * screen" test inverts and lets it through, and the plot then walked off
     * the end of memory (#190). */
    if ((uint32_t)d->sprite_log2px > 3 || (uint32_t)d->sprite_log2py > 3) {
        *wos = *hos = 0;
        return 0;
    }
    *wos = (int32_t)(wp << d->sprite_log2px);
    *hos = (int32_t)(hp << d->sprite_log2py);
    if (flags & IF_HALF) {
        *wos >>= 1;
        *hos >>= 1;
    }
    return 1;
}

/* ---- selected and shaded sprites --------------------------------------------------------- */

/* inversefunc: the colour &BBGGRR00 as it looks when selected (flags bit
 * 21), then when shaded (flags bit 22) */
static uint32_t inverse(uint32_t c, uint32_t flags)
{
    uint32_t r = (c >> 8) & 255u, g = (c >> 16) & 255u, b = c >> 24;
    struct ros_cpu x;
    if (flags & IF_SELECTED) {
        ros_cpu_enter(&x);
        x.r[0] = (r << 8) + 128, x.r[1] = (g << 8) + 128, x.r[2] = (b << 8) + 128;
        ros_swi(&x, XColourTrans_ConvertRGBToHSV);
        uint32_t H = x.r[0], S = x.r[1], V = x.r[2];
        if (S >= 0x10000u) S = 0xFFFFu;
        if (V >= 0x10000u) V = 0xFFFFu;
        uint32_t sv = S * V, v = 0xFFFFu - V;
        v -= v >> 2;
        v += S >> 2;
        v += sv >> 18;
        ros_cpu_enter(&x);
        x.r[0] = H, x.r[1] = S, x.r[2] = v;
        ros_swi(&x, XColourTrans_ConvertHSVToRGB);
        r = x.r[0] >= 0x10000u ? 255 : x.r[0] >> 8;
        g = x.r[1] >= 0x10000u ? 255 : x.r[1] >> 8;
        b = x.r[2] >= 0x10000u ? 255 : x.r[2] >> 8;
    }
    if (flags & IF_SHADED) {
        uint32_t y = r * 77 + g * 150 + b * 28;
        y += 0x7Fu;
        y += y << 8;
        y += 0x100u;
        y >>= 16;
        r = g = b = 0xB0u + ((y * 0xFFu - y * 0xB0u) >> 8);
    }
    return r << 8 | g << 16 | b << 24;
}

/* ColourTrans_GenerateTable's transfer function.  R0 is the colour and R12
 * the flags. */
static void inversefunc(struct ros_cpu *s)
{
    s->r[0] = inverse(s->r[0] & ~0xFFu, s->r[12]);
    s->r[15] = s->r[14];
}

/* colourmapfunc: the routine of a colour-mapping descriptor.  R12 points
 * to its table of 4096 words, indexed by the top nibbles of red, green and
 * blue. */
static void colourmapfunc(struct ros_cpu *s)
{
    uint32_t c = s->r[0];
    uint32_t i = ((c >> 12) & 15u) + ((c >> 20) & 15u) * 16u + (c >> 28) * 256u;
    s->r[0] = ros_ld32(s->r[12] + 4 * i);
    s->r[15] = s->r[14];
}

static uint32_t inverse_entry(void)
{
    struct wimp_draw *d = dr();
    if (!d->inverse_entry)
        d->inverse_entry = ros_native_entry(inversefunc, "wimp:inversefunc");
    return d->inverse_entry;
}

/* checkandgenerateinversecolourmap: three descriptors and their tables.
 * They are for selected, shaded, and both.  Returns 0 if they cannot be
 * made. */
static uint32_t inverse_maps(void)
{
    struct wimp_draw *d = dr();
    if (d->inverse_maps)
        return d->inverse_maps;
    void *mem;
    if (xos_module_claim(4096 * 4 * 3 + 8 * 3, &mem))
        return 0;
    uint32_t base = ros_addr(mem), table = base + 24;
    uint32_t fn = ros_native_entry(colourmapfunc, "wimp:colourmapfunc");
    for (uint32_t k = 1; k <= 3; k++) {
        ros_st32(base + 8 * (k - 1), table);
        ros_st32(base + 8 * (k - 1) + 4, fn);
        for (uint32_t bl = 0; bl < 16; bl++)
            for (uint32_t gr = 0; gr < 16; gr++)
                for (uint32_t re = 0; re < 16; re++) {
                    uint32_t col = (re * 0x11u) << 8 | (gr * 0x11u) << 16 | (bl * 0x11u) << 24;
                    ros_st32(table, inverse(col, k << 21));
                    table += 4;
                }
    }
    d->inverse_maps = base;
    return base;
}

/* cachespritepixtable: the table, or 0 for none */
static uint32_t sprite_table(uint32_t sprite, uint32_t area, uint32_t flags)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    uint32_t img = ros_ld32(sprite + 32), trans = ros_ld32(sprite + 36);
    uint32_t pal = img < trans ? img : trans;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    if (d->sprite_log2bpp >= 3 || pal >= 48) {
        c.r[0] = area >= 256 ? area : w->ram_sprites, c.r[1] = sprite;
    } else {
        /* Pixel i is Wimp colour map[i], using Wimp_SetColourMapping's maps */
        const struct wimp_colourmap *cm = &w->cmap;
        const uint32_t *pal = cm->palette_kind == 1 ? cm->palette : w->palette;
        uint32_t *pt = (uint32_t *)d->fontstr;
        for (int k = 0; k < 16; k++) {
            uint32_t map = d->sprite_log2bpp == 0 ? (k < 2 ? cm->map1[k] : 0)
                         : d->sprite_log2bpp == 1 ? (k < 4 ? cm->map2[k] : 0) : cm->map4[k];
            pt[k] = pal[map & 15u] & 0xFFFFFF00u;
        }
        c.r[0] = ros_ld32(sprite + 40), c.r[1] = ros_addr(pt);
    }
    uint32_t r0 = c.r[0], r1 = c.r[1], r5 = 3, r6 = 0, r7 = 0;
    if (d->sprite_log2bpp <= 3 && (flags & (IF_SELECTED | IF_SHADED)))
        r5 |= 4, r6 = flags, r7 = inverse_entry();     /* through inversefunc */
    c.r[2] = c.r[3] = 0xFFFFFFFFu, c.r[4] = 0, c.r[5] = r5, c.r[6] = r6, c.r[7] = r7;
    if (!call(XColourTrans_GenerateTable, &c))
        return 0;
    uint32_t size = c.r[4];
    uint32_t buf = wimp_table_space(&d->pix_at, &d->pix_size, size);
    if (!buf)
        return 0;
    ros_cpu_enter(&c);
    c.r[0] = r0, c.r[1] = r1, c.r[2] = c.r[3] = 0xFFFFFFFFu;
    c.r[4] = buf, c.r[5] = r5, c.r[6] = r6, c.r[7] = r7;
    if (!call(XColourTrans_GenerateTable, &c))
        return 0;
    d->pix_used = size;
    int identity = size <= 256;
    for (uint32_t i = 0; identity && i < size; i++)
        identity = ros_ld8(buf + i) == i;
    if (!identity)
        d->sprite_needs = 1;
    return buf;
}

/* iconsprite: returns 0 if the sprite is not there */
static int iconsprite(uint32_t flags, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    struct wimp_ws *w = wimp_ws();
    uint32_t area = 0, s = sprite_find(&area);
    if (!s)
        return 0;
    int32_t sw, sh;
    if (!sprite_size(area, s, flags, &sw, &sh))
        return 0;
    /* Also return for a size that no sprite has.  This is tested before the
     * size is used, because the test below asks "is any of it on screen",
     * and a wild width passes that. */
    if (sw <= 0 || sh <= 0 || sw > 0x10000 || sh > 0x10000)
        return 1;
    static const char lx[16] = "LLRRHHHHLLRRHHHL", ly[16] = "BTBTBTBBVVVVVVVV";
    unsigned i = (flags & 1) + 2 * ((flags >> 9) & 1) + 4 * ((flags >> 3) & 1) + 8 * ((flags >> 4) & 1);
    int32_t sx = lx[i] == 'L' ? b.x0 : lx[i] == 'R' ? b.x1 - sw : (b.x0 + b.x1 - sw) >> 1;
    int32_t sy = ly[i] == 'B' ? b.y0 : ly[i] == 'T' ? b.y1 - sh : (b.y0 + b.y1 - sh) >> 1;
    if (!(sx < w->screen_w && sy < w->screen_h && 0 < sx + sw && 0 < sy + sh))
        return 1;
    uint32_t table = sprite_table(s, area, flags);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = area, c.r[2] = s, c.r[3] = (uint32_t)sx, c.r[4] = (uint32_t)sy, c.r[5] = 0x18;
    if (flags & (IF_SELECTED | IF_SHADED)) {
        c.r[5] &= ~0x10u;                       /* as 5.88 does, and calcinverse */
        if (d->sprite_log2bpp > 3) {
            uint32_t maps = inverse_maps();
            if (maps) {
                table = maps + (((flags & (IF_SELECTED | IF_SHADED)) - IF_SELECTED) >> 18);
                c.r[5] |= 0x80u;
                d->sprite_needs = 1;
            }
        }
    }
    if (d->sprite_needs || (flags & IF_HALF)) {
        uint32_t *blk = d->bbox;
        memcpy(blk, d->sprite_scale, 16);
        if (flags & IF_HALF) {
            blk[2] *= 2;
            blk[3] *= 2;
        }
        c.r[0] = 0x200 + 52, c.r[6] = ros_addr(blk), c.r[7] = table;
    } else {
        c.r[0] = 0x200 + 34;
    }
    ros_swi(&c, XOS_SpriteOp);
    return 1;
}

/* ---- text ------------------------------------------------------------------------------ */

static uint32_t dchar(uint32_t flags)
{
    struct wimp_draw *d = dr();
    if (!(flags & IF_INDIRECT) || !is_string(d->validation))
        return 0;
    uint32_t p = findcommand(d->validation, 'D');
    if (!p)
        return 0;
    uint32_t c = ros_ld8(p);
    if (c == '\\')
        c = ros_ld8(p + 1);
    return c >= 32 ? c : 0;
}

static int symbol_index(uint32_t ch)
{
    static const uint32_t latin[6] = { 0x80, 0x84, 0x88, 0x89, 0x8A, 0x8B };
    static const uint32_t uni[6] = { 0x2714, 0x2718, 0x21D0, 0x21D2, 0x21D3, 0x21D1 };
    for (int k = 0; k < 6; k++)
        if (ch == latin[k] || ch == uni[k])
            return k;
    return -1;
}

/* pushfontstring: builds the string for the Font Manager in d->fontstr */
static uint32_t fontstring(uint32_t flags, uint32_t text, uint32_t h)
{
    struct wimp_draw *d = dr();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 71, c.r[1] = 127;
    int utf8 = call(XOS_Byte, &c) && c.r[1] == 111;
    uint32_t max = (flags & IF_INDIRECT) ? 0x0FFFFFFFu : 12u;
    uint32_t D = dchar(flags);
    uint8_t *o = d->fontstr;
    unsigned n = 0, lim = sizeof d->fontstr - 8;
    if (D) {
        uint32_t k = 0;
        while (k < max && ros_ld8(text + k) >= 32)
            k++;
        o[n++] = 26, o[n++] = (uint8_t)h;
        for (uint32_t i = 0; i < k && n < lim; i++)
            o[n++] = (uint8_t)D;
        o[n] = 0;
        return ros_addr(o);
    }
    uint32_t map = 0;
    int have_map = 0;
    if (h == d->systemfont) {
        map = d->symbol_map;
        have_map = 1;
    }
    enum { CURRENT, ICON, SYMBOL } state = CURRENT;
    int next_cs = d->sel_insert ? 0 : 2;        /* the selection's colour changes still to come */
    for (uint32_t i = 0; i < max && n < lim;) {
        uint32_t ch = ros_ld8(text + i), len = 1;
        if (ch < 32)
            break;
        /* The selection's control sequences go at their byte indices, before
         * anything else at that index. */
        if (next_cs < 2 && (int32_t)i == (next_cs ? d->sel_hi : d->sel_lo)) {
            memcpy(o + n, d->sel_codes[next_cs], 8);
            n += 8;
            next_cs++;
        }
        if (utf8 && ch == 0xE2 && i + 2 < max) {
            ch = ((ch & 0x0Fu) << 12) | ((ros_ld8(text + i + 1) & 0x3Fu) << 6) |
                 (ros_ld8(text + i + 2) & 0x3Fu);
            len = 3;
        }
        int sym = 0;
        int k = (utf8 ? len == 3 : ch >= 0x80) ? symbol_index(ch) : -1;
        if (k >= 0) {
            if (!have_map) {
                map = wimp_measure_symbols(h);
                have_map = 1;
            }
            sym = !(map & (1u << k));
        }
        if (state == CURRENT || (state == ICON && sym) || (state == SYMBOL && !sym)) {
            o[n++] = 26;
            o[n++] = (uint8_t)(sym ? d->symbolfont : h);
            state = sym ? SYMBOL : ICON;
        }
        for (uint32_t j = 0; j < len; j++)
            o[n++] = (uint8_t)ros_ld8(text + i + j);
        i += len;
    }
    o[n] = 0;
    return ros_addr(o);
}

static uint32_t font_of(uint32_t flags)
{
    return (flags & IF_FONT) ? flags >> 24 : dr()->systemfont;
}

static int outline(uint32_t flags)
{
    return dr()->systemfont != 0 || (flags & IF_FONT);
}

struct textpos {
    int32_t tx, ty, W, b, t, temp_height;
};

/* textwidth */
static int32_t textwidth(uint32_t flags, uint32_t text, int32_t *temp_height)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    if (!outline(flags)) {
        int32_t n = 0;
        while (ros_ld8(text + (uint32_t)n) >= 32)
            n++;
        return 16 * n;
    }
    struct ros_cpu c;
    if (d->currentfont == 0) {
        ros_cpu_enter(&c);
        if (call(XFont_CurrentFont, &c)) {
            d->currentfont = c.r[0], d->currentbg = c.r[1];
            d->currentfg = c.r[2], d->currentoffset = c.r[3];
        }
        ros_cpu_enter(&c);
        ros_swi(&c, XFont_SwitchOutputToBuffer);
    }
    uint32_t h = font_of(flags), s = fontstring(flags, text, h);
    ros_cpu_enter(&c);
    c.r[0] = h, c.r[1] = s, c.r[2] = 0, c.r[3] = c.r[4] = 0x0FFFFFFFu;
    uint32_t X = call(XFont_ScanString, &c) ? c.r[3] : 0;
    ros_cpu_enter(&c);
    c.r[1] = X, c.r[2] = 0;
    int32_t wos = call(XFont_ConverttoOS, &c) ? (int32_t)c.r[1] : 0;
    *temp_height = d->systemfonty1;
    if ((flags & IF_SPRITE) && !(flags & IF_VCENTRE) && !((flags & IF_HCENTRE) && (flags & IF_RJUST))) {
        int32_t top = 0;
        uint32_t cur = h;
        for (uint32_t p = s;;) {
            uint32_t ch = ros_ld8(p++);
            if (ch == 0)
                break;
            if (ch == 26) {
                cur = ros_ld8(p++);
                continue;
            }
            if (ch == 19) {
                p += 7;
                continue;
            }
            ros_cpu_enter(&c);
            c.r[0] = cur, c.r[1] = ch, c.r[2] = 0x10;
            if (!call(XFont_CharBBox, &c)) {
                top = d->systemfonty1;
                break;
            }
            if ((int32_t)c.r[4] > top)
                top = (int32_t)c.r[4];
        }
        *temp_height = top;
    }
    return wos + 2 * w->dx;
}

/* findtextorigin.  The flags may change: a centred text that is too wide
 * becomes right-justified. */
static void textorigin(uint32_t *flags, uint32_t text, struct wimp_box b, struct textpos *tp)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    tp->temp_height = w->dy;
    tp->W = textwidth(*flags, text, &tp->temp_height);
    static int32_t font_b, font_t;
    if (!outline(*flags)) {
        tp->t = w->dy;
        tp->b = w->dy - 32;
        tp->temp_height = w->dy;
    } else if (!(*flags & IF_FONT)) {
        tp->b = d->systemfonty0;
        tp->t = d->systemfonty1;
    } else {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = *flags >> 24;
        if (call(XFont_ReadInfo, &c)) {
            font_b = (int32_t)c.r[2];
            font_t = (int32_t)c.r[4];
        }
        tp->b = font_b;
        tp->t = font_t;
    }
    int32_t space = (b.x1 - b.x0) - tp->W;
    if (space < 0 && !(*flags & IF_SPRITE) && (*flags & IF_HCENTRE)) {
        *flags &= ~IF_HCENTRE;
        *flags |= IF_RJUST;
    }
    static const char lx[16] = "LLRRHHHHLXRLHHHR", ly[16] = "VBVBVBVTVVVVVVVV";
    uint32_t f = *flags;
    unsigned i = ((f >> 1) & 1) + 2 * ((f >> 9) & 1) + 4 * ((f >> 3) & 1) + 8 * ((f >> 4) & 1);
    switch (lx[i]) {
    case 'L': tp->tx = b.x0 + 6; break;
    case 'R': tp->tx = b.x1 - tp->W - 6; break;
    case 'H': tp->tx = b.x0 + ((b.x1 - b.x0 - tp->W) >> 1); break;
    default: {
        uint32_t area;
        int32_t sw = 0, sh;
        uint32_t s = sprite_find(&area);
        if (s)
            sprite_size(area, s, f, &sw, &sh);
        tp->tx = b.x0 + sw + 6;
    }
    }
    switch (ly[i]) {
    case 'T': tp->ty = b.y1 - tp->t; break;
    case 'B': tp->ty = b.y0 - tp->b; break;
    default: tp->ty = (b.y0 + b.y1 - (tp->b + tp->t)) >> 1; break;
    }
    /* The text is scrolled to keep the caret, the ghost caret or the
     * selection in view. */
    int32_t c = d->scroll_v;
    if (d->scroll_mode == SC_ADD) {
        tp->tx += d->scroll_v;
        return;
    }
    if (d->scroll_mode == SC_SEL) {
        if (b.x1 - b.x0 > d->scroll_sw) {
            c = d->scroll_sx + (int32_t)((uint32_t)d->scroll_sw >> 1);
        } else {
            tp->tx = b.x1 - (d->scroll_sx + d->scroll_sw) - 6;  /* sx taken as absolute, as 5.30 */
            return;
        }
    }
    if (d->scroll_mode != SC_NONE) {
        int32_t best = ((b.x0 + b.x1) >> 1) - c, L = b.x0 + 6, R = b.x0 - 6 + space;
        if (best > tp->tx) {
            if (tp->tx < L) tp->tx = L;
            if (best < L) tp->tx = best;
        } else {
            if (tp->tx > R) tp->tx = R;
            if (best > R) tp->tx = best;
        }
    }
}

static void lose_desktop_font(void)
{
    struct wimp_draw *d = dr();
    if (d->systemfont && d->systemfont != 0x80000000u) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = d->systemfont;
        ros_swi(&c, XFont_LoseFont);
    }
    d->systemfont = 0x80000000u;
}

/* set16x32chars (s/Wimp02): sets the size and spacing of characters to
 * 16 x 32 OS units, whatever the mode's eigen factors.  The Wimp sets it
 * for the desktop when it starts a mode.  This is because applications use
 * VDU 5 text too, and Edit's text is drawn with it.  The Wimp sets it again
 * for its own icon text when that goes to a sprite, where the sizes are
 * reset (s/Wimp04 95). */
void wimp_set16x32(void)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t x = 16u >> w->xeig, y = 32u >> w->yeig;
    uint8_t v[10] = { 23, 17, 7, 6, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), 0, 0 };
    wimp_vdu_bytes(v, sizeof v);
}

static void font_colours(void)
{
    struct wimp_draw *d = dr();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = d->tc[TC_BG], c.r[2] = d->tc[TC_FG], c.r[3] = 14;
    ros_swi(&c, XColourTrans_SetFontColours);
}

/* Formatted text */
static void formatted(uint32_t flags, uint32_t text, struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    int32_t L = d->linespacing;
    if (!outline(flags)) {
        int32_t N = ((b.x1 - b.x0) >> 4) - 1;
        if (N < 6)
            N = 6;
        uint32_t starts[64], lens[64];
        int k = 0;
        uint32_t p = text;
        while (k < 64) {
            while (ros_ld8(p) == 32)
                p++;
            uint32_t start = p, split = start, scanned = 0;
            while ((int32_t)scanned < N) {
                uint32_t c = ros_ld8(p);
                if (c < 32)
                    break;
                p++, scanned++;
                if (c <= 32)
                    split = p - 1;
            }
            uint32_t len = split - start;
            if (len == 0)
                len = scanned;
            if (len == 0)
                break;
            starts[k] = start, lens[k] = len, k++;
            p = start + len;
        }
        if (d->counting) {
            d->line_count = k;
            return;
        }
        int32_t h = k * L - L + 32;
        int32_t y = ((((b.y0 + b.y1 - h) >> 1) + 31 - (w->dy - 1)) & ~(w->dy - 1));
        int32_t xc = (b.x0 + b.x1) >> 1;
        for (int i = k - 1; i >= 0; i--) {
            plot(4, xc - 8 * (int32_t)lens[i], y);
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = starts[i], c.r[1] = lens[i];
            ros_swi(&c, XOS_WriteN);
            y += L;
        }
        return;
    }
    uint32_t h = font_of(flags);
    font_colours();
    uint32_t s = fontstring(flags, text, h);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = h;
    ros_swi(&c, XFont_SetFont);
    int32_t avail = b.x1 - b.x0 - 16;
    if (avail < 0)
        avail = 0;
    ros_cpu_enter(&c);
    c.r[1] = (uint32_t)avail, c.r[2] = s;
    uint32_t A = call(XFont_Converttopoints, &c) ? c.r[1] : 0;
    int32_t xc = (b.x0 + b.x1) >> 1, H = b.y1 - b.y0, y = 0;
    int k = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && d->counting) {
            d->line_count = k ? k : -1;
            return;
        }
        if (pass == 1)
            y = ((b.y1 - ((H - (k * L - L + 32)) >> 1)) - 28) & ~(w->dy - 1);
        uint32_t p = s;
        for (;;) {
            while (ros_ld8(p) == 32)
                p++;
            if (ros_ld8(p) == 0)
                break;
            ros_cpu_enter(&c);
            c.r[1] = p, c.r[2] = A, c.r[3] = 0x0FFFFFFFu, c.r[4] = 32, c.r[5] = 0x0FFFFFFFu;
            call(XFont_StringWidth, &c);
            uint32_t wx = c.r[2], len = c.r[5];
            if (len == 0) {
                ros_cpu_enter(&c);
                c.r[1] = p, c.r[2] = A, c.r[3] = 0x0FFFFFFFu, c.r[4] = 0xFFFFFFFFu, c.r[5] = 0x0FFFFFFFu;
                call(XFont_StringWidth, &c);
                wx = c.r[2], len = c.r[5];
            }
            if (len == 0)
                len = 1, wx = A;
            if (pass == 0) {
                k++;
            } else {
                ros_cpu_enter(&c);
                c.r[2] = wx;
                int32_t wos = call(XFont_ConverttoOS, &c) ? (int32_t)c.r[2] : 0;
                uint32_t r2 = 0x90u | (!(d->threed & 0x40u) ? 0x800u : 0);
                for (int tries = 0; tries < 2; tries++) {
                    ros_cpu_enter(&c);
                    c.r[1] = p, c.r[2] = r2, c.r[3] = (uint32_t)(xc - (wos >> 1));
                    c.r[4] = (uint32_t)y, c.r[7] = len;
                    if (call(XFont_Paint, &c))
                        break;
                    if (r2 & 0x800u || tries == 1) {     /* 5.88's inverted retry */
                        lose_desktop_font();
                        break;
                    }
                }
                y -= L;
            }
            p += len;
        }
    }
}

/* ---- menu items' keyboard shortcuts -------------------------------- */

/* spanlongestelement: the length of the longest token in the list that
 * the candidate starts with.  Returns 0 if there is none. */
static uint32_t span_longest(uint32_t list, const uint8_t *cand, uint32_t n)
{
    uint32_t best = 0;
    if (!list)
        return 0;
    for (uint32_t p = list;;) {
        uint32_t k = 0;
        int match = 1;
        while (k < n) {
            uint32_t t = ros_ld8(p + k);
            if (t <= ' ')
                break;
            if (t != cand[k]) {
                match = 0;
                break;
            }
            k++;
        }
        if (ros_ld8(p + k) > ' ')
            match = 0;
        if (match && best < k)
            best = k;
        while (ros_ld8(p) > ' ')
            p++;
        while (ros_ld8(p) == ' ')
            p++;
        if (ros_ld8(p) < ' ')
            return best;
    }
}

/* isthereashortcut: whether the text after the last space is a shortcut */
static int shortcut(uint32_t text, uint32_t max)
{
    int32_t sp = -1;
    uint32_t len = 0;
    while (len <= max) {
        uint32_t c = ros_ld8(text + len);
        if (c < ' ')
            break;
        if (c == ' ')
            sp = (int32_t)len;
        len++;
    }
    if (sp < 0)
        return 0;
    uint8_t cand[256];
    uint32_t n = len - (uint32_t)sp - 1;
    if (n > sizeof cand)
        n = sizeof cand;
    for (uint32_t i = 0; i < n; i++)
        cand[i] = (uint8_t)ros_ld8(text + (uint32_t)sp + 1 + i);
    if (span_longest(wimp_message("Modifiers"), cand, n))
        return 1;
    uint32_t k = span_longest(wimp_message("KeyNames"), cand, n);
    return k && k == n;
}

/* fixupfontstring: makes every space in the paint string a hard space.
 * If the text has a shortcut, the last space stays as it is, and the
 * graphics cursor is moved to the point to justify to. */
static void fixup_font_string(uint32_t text, uint32_t max)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 71, c.r[1] = 127;
    int utf8 = call(XOS_Byte, &c) && c.r[1] == 111;
    int sc = shortcut(text, max);
    uint8_t *s = d->fontstr;
    size_t cap = sizeof d->fontstr;
    uint32_t i = 0, last = 0;
    while (i < cap && s[i]) {
        if (s[i] == 26) {
            i += 2;
            continue;
        }
        if (s[i] != ' ') {
            i++;
            continue;
        }
        last = i;
        if (utf8) {
            size_t n = strnlen((char *)s + i, cap - i - 1);
            memmove(s + i + 1, s + i, n + 1);
            s[i++] = 0xC2;
        }
        s[i++] = 0xA0;
    }
    if (!sc || !d->win)
        return;
    if (utf8) {
        size_t n = strnlen((char *)s + last, cap - last);
        memmove(s + last, s + last + 1, n);
    }
    s[last] = ' ';
    const struct wimp_window *win = d->win;
    int32_t x = win->s[APP].outline.x1;
    if (win->s[APP].flags & F_VBAR)
        x -= w->furn.V;
    x -= 10 + wimp_menu_arrow_width();
    plot(4, x, (int32_t)text);
}

/* Whether the icon is a menu item.  The title and writable items are not. */
static int menu_item(uint32_t flags)
{
    struct wimp_draw *d = dr();
    uint32_t type = (flags >> 12) & 15u;
    if (!d->win)
        return 1;                               /* Wimp_PlotIcon outside a loop */
    return d->win->owner == NO_WINDOW && type != 14 && type != 15;
}

/* pushfontstring as the menu code calls it.  There is no validation
 * string and the font is the desktop font.  Returns the string's
 * address. */
uint32_t wimp_font_string(uint32_t flags, uint32_t text)
{
    struct wimp_draw *d = dr();
    uint32_t v = d->validation;
    d->validation = 0;
    uint32_t s = fontstring(flags, text, d->systemfont);
    d->validation = v;
    return s;
}

/* clipboard_draw_selection_block_font: draws a rectangle under the
 * selected text, in the foreground colour or the shaded grey.  Its ends
 * come from the widths of the painted string up to each end of the
 * selection. */
static void sel_block_font(uint32_t s, int32_t tx, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    if (!d->buffered || d->sel_lo == d->sel_hi)
        return;
    int32_t xs[2];
    for (int k = 0; k < 2; k++) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0, c.r[1] = s, c.r[2] = 1u << 7, c.r[3] = c.r[4] = 0x20000000u;
        c.r[7] = (uint32_t)(k ? d->sel_hi + 10 : d->sel_lo + 2);
        if (!call(XFont_ScanString, &c))
            return;
        int32_t X = (int32_t)(c.r[3] / 400u);
        if (wimp_ws()->writedir)
            X = 12 - X;
        xs[k] = X + tx;
    }
    plot(4, xs[0], b.y0 + 4);
    int32_t top = b.y0 + (b.y1 - b.y0) - 6;
    struct ros_cpu c;
    if (d->sel_shaded) {
        ros_cpu_enter(&c);
        c.r[0] = 0x80808000u, c.r[3] = 0, c.r[4] = 0;
        ros_swi(&c, XColourTrans_SetGCOL);
        plot(101, xs[1], top);
        ros_cpu_enter(&c);
        c.r[0] = d->tc[TC_FG], c.r[3] = 0, c.r[4] = 0;
        ros_swi(&c, XColourTrans_SetGCOL);
    } else {
        plot(101, xs[1], top);
    }
}

/* clipboard_draw_selection_block_vdu5: the same for system-font text.  It
 * works relative to the graphics cursor at the start of the text, and puts
 * the cursor back afterwards. */
static void sel_block_vdu5(struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    int32_t lo = d->sel_lo, hi = d->sel_hi;
    if (hi == lo)
        return;
    struct ros_cpu c;
    if (d->sel_shaded) {
        ros_cpu_enter(&c);
        c.r[0] = 0x80808000u, c.r[3] = 0, c.r[4] = 0;
        ros_swi(&c, XColourTrans_SetGCOL);
    }
    int32_t h = (b.y1 - b.y0) - 6;
    uint32_t *v = (uint32_t *)(w->scratch + 480);
    v[0] = 138, v[1] = 139, v[2] = 0xFFFFFFFFu;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(v), c.r[1] = ros_addr(v);
    ros_swi(&c, XOS_ReadVduVariables);
    int32_t gx = (int32_t)v[0], gy = (int32_t)v[1];
    int32_t r1 = hi << 4;
    if (w->writedir)
        r1 = ~r1 + 16;
    plot(0, r1, (int32_t)((uint32_t)h >> 1) - 16);
    int32_t r1b = (lo - hi) << 4;
    if (w->writedir)
        r1b = ~r1b;
    plot(97, r1b, ~h + 4);
    plot(4, gx, gy);
    if (d->sel_shaded) {
        ros_cpu_enter(&c);
        c.r[0] = d->tc[TC_FG], c.r[3] = 0, c.r[4] = 0;
        ros_swi(&c, XColourTrans_SetGCOL);
    }
}

/* ploticonbackgroundsprite: clears the box, then draws the window's tile */
static void plot_icon_background(struct wimp_box b)
{
    struct wimp_draw *d = dr();
    struct wimp_box old = d->clip, r = meet(b, old);
    if (r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    gw(r);
    wimp_clg();
    if (d->win)
        wimp_draw_tile(d->win);
    gw(old);
}

static void icontext(uint32_t *flags, uint32_t text, struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    if (d->linespacing != -1) {
        formatted(*flags, text, b);
        return;
    }
    getborder(*flags);
    struct wimp_box in = b;
    if (*flags & IF_BORDER) {
        static const int32_t inset[8] = { 0, 4, 4, 8, 8, 4, 12, 8 };
        uint32_t T = d->border_type;
        if (T == 0 || T == 7)
            in.x0 += w->dx, in.x1 -= w->dx, in.y0 += w->dy, in.y1 -= w->dy;
        in.x0 += inset[T], in.x1 -= inset[T], in.y0 += inset[T], in.y1 -= inset[T];
    }
    struct textpos tp;
    textorigin(flags, text, in, &tp);
    if (d->sel_on && !d->sel_shaded)
        w->sel_origin = tp.tx;                  /* selection_text_origin, for dragging the text out */
    struct wimp_box old = d->clip;
    gw(meet(b, old));
    /* the background behind the text in a text-plus-sprite icon */
    if ((*flags & IF_SPRITE) && ((*flags & IF_SELECTED) || !(*flags & IF_FILLED))) {
        int skip = (d->threed & 0x20u) && d->win && d->win->def[35] == 0xFF && !(*flags & IF_SELECTED);
        if (!skip) {
            if (ros_ld8(text) < 32) {
                *flags &= ~IF_FILLED;
            } else {
                *flags |= IF_FILLED;
                int32_t ax = tp.tx > b.x0 ? tp.tx : b.x0, ay = tp.ty + tp.b > b.y0 ? tp.ty + tp.b : b.y0;
                if (ax > b.x1)
                    ax = b.x1;
                int32_t bx = ax + tp.W;
                if (bx < b.x0) bx = b.x0;
                if (bx > b.x1) bx = b.x1;
                int32_t by = tp.ty + tp.temp_height < b.y1 ? tp.ty + tp.temp_height : b.y1;
                uint32_t ibg = ((*flags & IF_FONT) ? d->fontbg : *flags >> 28) & 15;
                if (!(*flags & IF_SELECTED) && (d->threed & 0x08u) && d->win && ibg == d->win->def[35]) {
                    *flags &= ~IF_FILLED;           /* the window's tile goes behind it */
                    plot_icon_background((struct wimp_box){ ax, ay, bx, by });
                } else {
                    solid((struct wimp_box){ ax, ay, bx, by });
                }
            }
        }
    }
    plot(4, tp.tx, tp.ty);
    if (outline(*flags)) {
        font_colours();
        uint32_t h = font_of(*flags), s;
        int menu = menu_item(*flags);
        if (!menu && d->sel_on && d->buffered && d->sel_lo != d->sel_hi) {
            sel_codes();                        /* clipboard_set_font_colour_codes */
            d->sel_insert = 1;
            s = fontstring(*flags, text, h);
            d->sel_insert = 0;
            sel_block_font(s, tp.tx, b);        /* clipboard_draw_selection_block_font */
        } else {
            s = fontstring(*flags, text, h);
        }
        uint32_t r2 = 0x10u;
        if (menu && ((*flags >> 16) & 31u) != 13u) {
            fixup_font_string(text, (*flags & IF_INDIRECT) ? 0x0FFFFFFFu : 11u);
            r2 = 0x11u;
        }
        if (!(*flags & IF_FILLED) && !(d->threed & 0x40u))
            r2 |= 0x800u;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = s, c.r[2] = r2, c.r[3] = (uint32_t)(tp.tx + 2), c.r[4] = (uint32_t)tp.ty;
        if (!call(XFont_Paint, &c)) {
            int ok = 0;
            if (r2 & 0x800u) {
                ros_cpu_enter(&c);
                c.r[1] = s, c.r[2] = r2 & ~0x800u, c.r[3] = (uint32_t)(tp.tx + 2), c.r[4] = (uint32_t)tp.ty;
                ok = call(XFont_Paint, &c);
            }
            if (!ok)
                lose_desktop_font();
        }
    } else {
        wimp_vdu5();
        wimp_set16x32();
        int sel = d->sel_on;
        if (sel)
            sel_block_vdu5(b);                  /* clipboard_draw_selection_block_vdu5 */
        uint32_t D = dchar(*flags), n = (*flags & IF_INDIRECT) ? 0x0FFFFFFFu : 12u;
        uint8_t buf[64];
        unsigned k = 0;
        for (uint32_t p = text; n > 0; p++, n--) {
            uint32_t ch = ros_ld8(p);
            if (ch < 32)
                break;
            int32_t at = (int32_t)(p - text);
            if (sel && (at == d->sel_lo || at == d->sel_hi)) {
                if (k)
                    wimp_vdu_bytes(buf, k);
                k = 0;                          /* the colours are swapped inside the selection */
                struct ros_cpu g;
                ros_cpu_enter(&g);
                g.r[0] = at == d->sel_lo ? d->tc[TC_BG] : d->tc[TC_FG], g.r[3] = 0, g.r[4] = 0;
                ros_swi(&g, XColourTrans_SetGCOL);
            }
            buf[k++] = (uint8_t)(D >= 32 ? D : ch);
            if (k == sizeof buf) {
                wimp_vdu_bytes(buf, k);
                k = 0;
            }
        }
        if (k)
            wimp_vdu_bytes(buf, k);
    }
    gw(old);
}

/* ---- an icon ---------------------------------------------------------------------------------- */

/* iconfilledCheckMenu: an unselected menu item gets the menu's tile in
 * place of the fill.  This is in a redraw or update with TexturedMenus, for
 * ESG 13, or for ESG 12 with a sprite. */
static void filled(uint32_t flags, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    uint32_t esg = (flags >> 16) & 15u;
    if (!(flags & IF_SELECTED) && (d->threed & 0x10u) && d->win && d->win->owner == NO_WINDOW &&
        (esg == 13 || (esg == 12 && (flags & IF_SPRITE))))
        plot_icon_background(b);
    else
        solid(b);
}

static void godrawicon(uint32_t flags, uint32_t data, struct wimp_box b)
{
    if (flags & IF_FILLED)
        filled(flags, b);
    if (flags & IF_SPRITE) {
        if (!iconsprite(flags, b) && !(flags & IF_INDIRECT))
            return;
    }
    if (flags & IF_TEXT) {
        uint32_t text = (flags & IF_INDIRECT) ? ros_ld32(data) : data;
        icontext(&flags, text, b);
    }
    if (flags & IF_BORDER)
        iconborder(flags, b);
}

static void drawcolouredicon(uint32_t flags, uint32_t data, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    if (!meets(b))
        return;
    uint32_t T = d->border_type;
    if (T != 5 && T != 6 && !(flags & (IF_SPRITE | IF_FILLED)) && !(flags & IF_SHADED) &&
        (flags & IF_SELECTED)) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = d->workbg;
        uint32_t n1 = call(XColourTrans_ReturnColourNumber, &c) ? c.r[0] : 0;
        ros_cpu_enter(&c);
        c.r[0] = d->tc[TC_BG];
        uint32_t n2 = call(XColourTrans_ReturnColourNumber, &c) ? c.r[0] : 0;
        ros_cpu_enter(&c);
        c.r[0] = 0x13, c.r[1] = n1 ^ n2;
        ros_swi(&c, XOS_SetColour);
        solid(b);
    }
    fg(d->tc[TC_FG]);
    bg(d->tc[TC_BG]);
    if ((flags & IF_TEXT) && outline(flags)) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0, c.r[1] = d->tc[TC_BG], c.r[2] = d->tc[TC_FG], c.r[3] = 14;
        if (!call(XColourTrans_SetFontColours, &c))
            return;
    }
    godrawicon(flags, data, b);
}

/* The furniture's icons, in the colours that the border pass set */
static void drawicon_system(uint32_t flags, uint32_t data, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    flags = seticonptrs(flags, data);
    d->tc[TC_FG] = d->titlefg;
    d->tc[TC_BG] = d->titlecolour;
    if (!meets(b))
        return;
    godrawicon(flags, data, b);
}

static void drawicon_system_sysf(uint32_t flags, uint32_t data, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    uint32_t keep = d->systemfont;
    d->systemfont = 0;
    drawicon_system(flags, data, b);
    d->systemfont = keep;
}

/* A furniture glyph: a filled, bordered system-font character */
static void glyph(uint32_t ch, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    if (ch) {
        d->glyph[0] = (uint8_t)ch;
        d->glyph[1] = 13;
    } else {
        d->glyph[0] = 13;
    }
    drawicon_system_sysf(0x2Du, ros_addr(d->glyph), b);
}

/* writable_calc_checksum: the OS_CRC of the text up to and including its
 * terminator, and at most the buffer's length */
uint32_t wimp_icon_checksum(struct wimp_window *win, int32_t icon)
{
    const uint8_t *ic = icon >= 0 ? wimp_icon(win, (uint32_t)icon) : NULL;
    if (!ic)
        return 0;
    uint32_t f, p, max;
    memcpy(&f, ic + 16, 4);
    if (f & IF_INDIRECT) {
        memcpy(&p, ic + 20, 4);
        memcpy(&max, ic + 28, 4);
    } else {
        p = ros_addr((void *)(ic + 20)), max = 12;
    }
    uint32_t e = p;
    for (;;) {
        uint32_t ch = ros_ld8(e++);
        if (ch < 32 || --max == 0)
            break;
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = p, c.r[2] = e, c.r[3] = 1;
    ros_swi(&c, XOS_CRC);
    return c.v ? 0 : c.r[0];
}

/* Whether the window's selection, if any, is still on the text it was
 * made on.  If it is not, it is removed (selectioncaret_checksum_fail). */
int wimp_sel_valid(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    if (win->sel.icon < 0)
        return 0;
    if (wimp_icon_checksum(win, win->sel.icon) == win->sel.checksum)
        return 1;
    int32_t icon = win->sel.icon;
    win->sel.icon = -1;
    if (w->selwin == win->handle)
        w->selwin = 0xFFFFFFFFu;
    if (w->caret.w == win->handle && w->caret.i == icon && w->caret.index >= 0) {
        int32_t x, y, cx;                       /* the main caret measured again */
        uint32_t hw;
        if (wimp_caret_coords(win, icon, (uint32_t)w->caret.index, CC_MAIN, &x, &y, &hw, &cx)) {
            w->caret.x = x - wimp_origin_x(win, APP);
            w->caret.y = y - wimp_origin_y(win, APP);
            w->caretx = cx;
        }
    }
    return 0;
}

/* clipboard_set_font_colour_codes: the colour changes at the ends of the
 * selection, for outline text.  Each is control 19 with the background,
 * the foreground and an offset of 14. */
static void sel_codes(void)
{
    struct wimp_draw *d = dr();
    uint32_t bg = d->sel_shaded ? 0x80808000u : d->tc[TC_FG], fg = d->sel_shaded ? 0xFFFFFF00u : d->tc[TC_BG];
    d->sel_codes[0][0] = bg | 19u;
    d->sel_codes[0][1] = (fg >> 8) | 0x0E000000u;
    d->sel_codes[1][0] = d->tc[TC_BG] | 19u;
    d->sel_codes[1][1] = (d->tc[TC_FG] >> 8) | 0x0E000000u;
}

void wimp_draw_icons(struct wimp_window *win, struct wimp_box rect)
{
    struct wimp_draw *d = dr();
    (void)rect;                                 /* the clip is the rectangle */
    if (win->handle == wimp_ws()->iconbar && wimp_ws()->ib.needs_rs)
        wimp_iconbar_refit();                   /* the icon bar refits first */
    window_colours(win);
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    for (uint32_t i = 0; i < win->nicons; i++) {
        const uint8_t *icon = wimp_icon(win, i);
        int32_t b[5];
        memcpy(b, icon, 20);
        if ((uint32_t)b[4] & IF_DELETED)
            continue;
        uint32_t data = ros_addr((void *)(icon + 20));
        struct wimp_ws *ws = wimp_ws();
        const struct wimp_caretblk *c = &ws->caret, *g = &ws->ghost;
        int has = c->w == win->handle && c->i == (int32_t)i;
        int ghost = g->w == win->handle && g->i == (int32_t)i;
        /* pageiniconbartask: an icon bar icon's text is in its task's memory */
        const void *was = NULL;
        int paged = 0;
        if (win->handle == wimp_ws()->iconbar && ((uint32_t)b[4] & IF_INDIRECT)) {
            struct wimp_task *t = wimp_task_by_handle(wimp_iconbar_icon_task((int32_t)i), 0);
            if (t) {
                was = ros_task_page_in(t->rt);
                paged = 1;
            }
        }
        /* If there is a valid selection here, the main caret is not drawn */
        int sel = win->sel.icon == (int32_t)i && wimp_sel_valid(win);
        if (sel)
            has = 0;
        scroll_for(win, (int32_t)i, CC_NONE, 0, sel);
        d->sel_on = sel;
        d->sel_shaded = ws->selwin != win->handle;
        d->sel_lo = win->sel.low, d->sel_hi = win->sel.high;
        uint32_t flags = seticonptrs((uint32_t)b[4], data);
        struct wimp_box box = { ox + b[0], oy + b[1], ox + b[2], oy + b[3] };
        uint32_t bt = ((uint32_t)b[4] >> 12) & 15;
        int ghost_shown = ghost && !(sel && win->sel.low <= g->index && g->index <= win->sel.high);
        if (has || sel || bt == 14 || bt == 15) {
            /* Drawn as if through a buffer.  The icon is filled and cut at
             * its box, and the caret is EORed on its fresh pixels. */
            if (box.x1 > box.x0 && box.y1 > box.y0 && meets(box)) {
                struct wimp_box old = d->clip;
                gw(meet(box, old));
                d->buffered = 1;
                d->buf_x0 = box.x0, d->buf_y0 = box.y0;
                drawcolouredicon((uint32_t)b[4] | IF_FILLED, data, box);
                d->buffered = 0;
                if (has)
                    wimp_caret_draw(c, ox + c->x, oy + c->y);
                if (ghost_shown) {
                    struct wimp_caretblk gb = *g;
                    gb.hf |= 1u << 30;
                    wimp_caret_draw(&gb, ox + g->x, oy + g->y);
                }
                gw(old);
            }
        } else {
            drawcolouredicon(flags, data, box);
        }
        d->scroll_mode = SC_NONE;
        d->sel_on = 0;
        if (paged)
            ros_task_page_back(was);
        if (d->systemfont & 0x80000000u) {
            if (!((uint32_t)b[4] & IF_FONT)) {
                d->systemfont = 0;
                i--;                            /* the same icon again, in the system font */
            } else {
                d->systemfont = 0;
            }
        }
    }
    wimp_font_exit();
}

/* ---- Wimp_ReadPixTrans, Wimp_SetColourMapping ------------------------------------- */

void wimp_colourmap_reset(void)
{
    struct wimp_colourmap *cm = &wimp_ws()->cmap;
    static const uint8_t t1[2] = { 0, 7 }, t2[4] = { 0, 2, 5, 7 };
    memcpy(cm->map1, t1, 2);
    memcpy(cm->map2, t2, 4);
    for (int k = 0; k < 16; k++)
        cm->map4[k] = (uint8_t)k;
    cm->palette_kind = 0;
}

void wimp_swi_SetColourMapping(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_colourmap *cm = &w->cmap;
    if (s->r[1] == 0xFFFFFFFFu) {
        memcpy(cm->palette, wimp_default_palette, sizeof cm->palette);
        cm->palette_kind = 1;
    } else if (s->r[1] == 0) {
        cm->palette_kind = 0;
    } else {
        memcpy(cm->palette, ros_ptr(s->r[1]), sizeof cm->palette);
        cm->palette_kind = 1;
    }
    static const uint8_t t1[2] = { 0, 7 }, t2[4] = { 0, 2, 5, 7 }, t4[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                                                                               10, 11, 12, 13, 14, 15 };
    uint8_t *maps[3] = { cm->map1, cm->map2, cm->map4 };
    const uint8_t *defs[3] = { t1, t2, t4 };
    static const unsigned len[3] = { 2, 4, 16 };
    for (int k = 0; k < 3; k++) {
        uint32_t p = s->r[2 + k];
        if (p == 0xFFFFFFFFu)
            memcpy(maps[k], defs[k], len[k]);
        else if (p)
            memcpy(maps[k], ros_ptr(p), len[k]);
    }
    dr()->sprite_lastmode = 0xFFFFFFFFu;        /* work out tables and metrics again */
    wimp_tiles_forget();
    wimp_tools_refresh();
    if (w->log2bpp < 3)
        wimp_invalidate_box(wimp_screen_box());
    s->v = 0;
}

void wimp_swi_ReadPixTrans(struct ros_cpu *s)
{
    struct wimp_draw *d = dr();
    uint32_t r0 = s->r[0], area = s->r[1], sprite = 0;
    struct ros_cpu c;
    if (r0 >= 0x200) {
        sprite = s->r[2];
        if (area == 1)
            area = wimp_ws()->ram_sprites;
    } else {
        if (r0 < 0x100) {
            ros_cpu_enter(&c);
            c.r[0] = 3;
            area = call(XOS_ReadDynamicArea, &c) ? c.r[0] : 1;
        }
        if (area != 1) {
            ros_cpu_enter(&c);
            c.r[0] = 0x118, c.r[1] = area, c.r[2] = s->r[2];
            if (call(XOS_SpriteOp, &c))
                sprite = c.r[2];
        }
        if (!sprite)                            /* then try the Wimp's pool */
            sprite = wimp_pool_find(s->r[2], &area);
        if (!sprite) {
            ros_cpu_enter(&c);
            c.r[0] = 0x118, c.r[1] = wimp_ws()->ram_sprites, c.r[2] = s->r[2];
            ros_swi(&c, XOS_SpriteOp);
            wimp_fail(s, c.v ? ros_ptr(c.r[0]) : wimp_error(E_BAD_OP));
            return;
        }
    }
    int32_t sw, sh;
    if (!sprite_size(area, sprite, 0, &sw, &sh)) {
        ros_cpu_enter(&c);
        c.r[0] = 0x228, c.r[1] = area, c.r[2] = sprite;
        ros_swi(&c, XOS_SpriteOp);
        wimp_fail(s, c.v ? ros_ptr(c.r[0]) : wimp_error(E_BAD_OP));
        return;
    }
    if (s->r[6])
        memcpy(ros_ptr(s->r[6]), d->sprite_scale, 16);
    uint32_t table = sprite_table(sprite, area, 0);
    if (s->r[7] && table)
        memcpy(ros_ptr(s->r[7]), ros_ptr(table), 16);
    s->v = 0;
}

/* ---- Wimp_PlotIcon -------------------------------------------------------------- */

void wimp_swi_PlotIcon(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    if (w->sprout_current)
        wimp_sprout_recache();                  /* output to a sprite: use its sizes */
    struct wimp_window *win = w->pending_window ? wimp_window(w->pending_window) : NULL;
    int32_t ox, oy;
    if (win) {
        struct wimp_task *t = wimp_current();
        if (!t || win->owner != t->handle) {
            wimp_fail(s, wimp_error(E_OWNER_WINDOW));
            return;
        }
        window_colours(win);
        ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    } else {
        d->area = 1;                            /* the Wimp's pool, and VDU 26 */
        d->win = NULL;                          /* redrawhandle -1: a menu item's text */
        wimp_default_windows();
        ox = (int32_t)s->r[4], oy = (int32_t)s->r[5];
    }
    wimp_vdu5();
    uint32_t blk = s->r[1];
    int32_t b[5];
    memcpy(b, ros_ptr(blk), 20);
    s->v = 0;
    if ((uint32_t)b[4] & IF_DELETED)
        return;
    d->scroll_mode = SC_NONE;
    d->sel_on = 0;
    for (;;) {
        uint32_t flags = seticonptrs((uint32_t)b[4], blk + 20);
        drawcolouredicon(flags, blk + 20, (struct wimp_box){ ox + b[0], oy + b[1], ox + b[2], oy + b[3] });
        if (!(d->systemfont & 0x80000000u))
            break;
        if ((uint32_t)b[4] & IF_FONT) {
            d->systemfont &= ~0x80000000u;
            break;
        }
        d->systemfont = 0;                      /* again, in the system font */
    }
    wimp_font_exit();
}

/* ---- the tools ------------------------------------------------------------------------ */

static void toolop(uint32_t op, uint32_t entry, int32_t x, int32_t y, uint32_t scale, uint32_t table)
{
    struct wimp_ws *w = wimp_ws();
    if (!entry)
        return;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = op, c.r[1] = w->tools, c.r[2] = entry & ~1u;
    c.r[3] = (uint32_t)x, c.r[4] = (uint32_t)y;
    c.r[5] = w->tl.action, c.r[6] = scale, c.r[7] = table;
    ros_swi(&c, XOS_SpriteOp);
}

static uint32_t tool(unsigned slot)
{
    if (wimp_ws()->tl.regen) {                  /* Tool_SpriteOpCommon: after InvalidateCache */
        wimp_ws()->tl.regen = 0;
        wimp_tools_refresh();
    }
    struct wimp_tools *t = &wimp_ws()->tl;
    return t->built ? t->list[slot] : 0;
}

/* plot_windowglyph: returns 0 when there is no tool, and then the glyph is
 * to be drawn instead */
static int windowglyph(unsigned slot, struct wimp_box b, uint32_t scale)
{
    struct wimp_ws *w = wimp_ws();
    if (!meets(b))
        return 1;
    uint32_t s = tool(slot);
    if (!s)
        return 0;
    if (s & 1u) {
        solid(b);
        hollow(b);
    }
    toolop(0x234, s, b.x0, b.y0, scale, wimp_tool_table(w->dr.titlecolour));
    return 1;
}

static void windowglyph_clipped(unsigned slot, struct wimp_box extent, struct wimp_box cb)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_box c = meet(w->dr.clip, cb);
    if (c.x1 <= c.x0 || c.y1 <= c.y0)
        return;
    struct wimp_box old = w->dr.clip;
    gw(c);
    windowglyph(slot, extent, ros_addr(w->tl.scale));
    gw(old);
}

/* An arrow squashed to fit its box */
static int windowglyph_scaled(unsigned slot, int32_t size, struct wimp_box b, int vertical)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t *blk = w->dr.bbox;
    int32_t k = size + 2;
    memcpy(blk, w->tl.scale, 16);
    if (vertical) {
        blk[1] *= (uint32_t)(b.y1 - b.y0);
        blk[3] *= (uint32_t)k;
    } else {
        blk[0] *= (uint32_t)(b.x1 - b.x0);
        blk[2] *= (uint32_t)k;
    }
    int r = windowglyph(slot, b, ros_addr(blk));
    if (vertical) {
        windowglyph_clipped(slot, (struct wimp_box){ b.x0, b.y0, b.x1, b.y0 + k },
                            (struct wimp_box){ b.x0, b.y0, b.x1, b.y0 + w->dy });
        windowglyph_clipped(slot, (struct wimp_box){ b.x0, b.y1 - k, b.x1, b.y1 },
                            (struct wimp_box){ b.x0, b.y1 - w->dy, b.x1, b.y1 });
    } else {
        windowglyph_clipped(slot, (struct wimp_box){ b.x0, b.y0, b.x0 + k, b.y1 },
                            (struct wimp_box){ b.x0, b.y0, b.x0 + w->dx, b.y1 });
        windowglyph_clipped(slot, (struct wimp_box){ b.x1 - k, b.y0, b.x1, b.y1 },
                            (struct wimp_box){ b.x1 - w->dx, b.y0, b.x1, b.y1 });
    }
    return r;
}

static void spriteglyph(uint32_t s, struct wimp_box b)
{
    struct wimp_ws *w = wimp_ws();
    if (!meets(b))
        return;
    if (s & 1u) {
        solid(b);
        hollow(b);
    }
    toolop(0x234, s, b.x0, b.y0, ros_addr(w->tl.scale), wimp_tool_table(w->dr.titlecolour));
}

/* ---- the title bar --------------------------------------------------------------------------------- */

static void title_bar(const struct wimp_window *win, uint32_t F, struct wimp_box T)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_tools *t = &w->tl;
    struct wimp_draw *d = dr();
    uint32_t data = ros_addr((void *)(win->def + 72));
    if (!meets(T))
        return;
    struct wimp_box old = d->clip;
    gw(meet(T, old));
    int32_t s = t->title_top + t->title_bottom + 4;
    if (s < w->furn.T || (tool(TOOL_TBARMIDT) & 1u))
        drawicon_system(F & ~(IF_SPRITE | IF_TEXT), data, T);
    int32_t ytop = T.y1 - w->dy - t->title_top;
    uint32_t scale = ros_addr(t->scale), table = wimp_tool_table(d->titlecolour);
    uint32_t l = tool(TOOL_TBARLCAP), mt = tool(TOOL_TBARMIDT), mb = tool(TOOL_TBARMIDB);
    uint32_t r = tool(TOOL_TBARRCAP);
    if (l || mt || mb || r) {
        int32_t step = t->title_section + w->dx;
        toolop(0x234, l, T.x0, T.y0, scale, table);
        for (int32_t x = T.x0 + t->title_left + w->dx; x <= T.x1; x += step) {
            toolop(0x234, mb, x, T.y0, scale, table);
            toolop(0x234, mt, x, ytop, scale, table);
        }
        toolop(0x234, r, T.x1 - t->title_right - w->dx, T.y0, scale, table);
    }
    gw(old);
    int32_t x0 = T.x0 + 4, x1 = T.x1 - 4;
    if (x0 > x1)
        x0 = x1;
    drawicon_system(F & ~(IF_BORDER | IF_FILLED), data, (struct wimp_box){ x0, T.y0, x1, T.y1 });
}

/* ---- the scroll bars -------------------------------------------------------------------------------- */

/* muldiv, computed exactly as the Wimp does */
static uint32_t lz(uint32_t v)
{
    uint32_t n = 0;
    if (!(v >> 16)) n += 16, v <<= 16;
    if (!(v >> 24)) n += 8, v <<= 8;
    if (!(v >> 28)) n += 4, v <<= 4;
    if (!(v >> 30)) n += 2, v <<= 2;
    return n;
}

static int32_t muldiv(int32_t a, int32_t b, int32_t c)
{
    uint32_t z = lz((uint32_t)a) + lz((uint32_t)b);
    if (z < 34) {
        b >>= 34 - z;
        c >>= 34 - z;
    }
    if (c == 0)
        c = 1;
    return (int32_t)(((uint32_t)a * (uint32_t)b + (uint32_t)(c >> 1)) / (uint32_t)c);
}

static int32_t pixel(void)
{
    struct wimp_ws *w = wimp_ws();
    return w->dx > w->dy ? w->dx : w->dy;
}

/* The vertical slider in the well */
static void vslider(const struct wimp_window *win, struct wimp_box wl, int32_t *sy0, int32_t *sy1)
{
    struct wimp_ws *w = wimp_ws();
    const struct wimp_place *p = &win->s[APP];
    int32_t ey0, ey1;
    memcpy(&ey0, win->def + 44, 4);
    memcpy(&ey1, win->def + 52, 4);
    int32_t e = 2 * pixel(), m = pixel() + 32, M = m + w->tl.vblip_h - w->dy;
    int32_t A = (wl.y1 - wl.y0) - 2 * e, E = ey1 - ey0, h = p->vis.y1 - p->vis.y0;
    if (M >= A) {
        *sy0 = wl.y0 + e;
        *sy1 = wl.y1 - e;
        if (*sy1 - *sy0 < 0)
            *sy0 = *sy1 = (int32_t)((uint32_t)(wl.y0 + wl.y1) >> 1);
        return;
    }
    int32_t S = muldiv(A, h, E), wy0 = M > S ? wl.y0 + (M - S) : wl.y0;
    int32_t P = muldiv(ey1 - p->scy, (wl.y1 - wy0) - 2 * e, E) + e;
    int32_t top = (wl.y1 - P) & ~(w->dy - 1);
    int32_t bot = (wl.y1 - P - (S > M ? S : M)) & ~(w->dy - 1);
    if (bot - wl.y0 < e) {
        top += wl.y0 + e - bot;
        bot = wl.y0 + e;
    }
    if (wl.y1 - top < e)
        top = wl.y1 - e;
    *sy0 = bot, *sy1 = top;
}

static void hslider(const struct wimp_window *win, struct wimp_box wl, int32_t *sx0, int32_t *sx1)
{
    struct wimp_ws *w = wimp_ws();
    const struct wimp_place *p = &win->s[APP];
    int32_t ex0, ex1;
    memcpy(&ex0, win->def + 40, 4);
    memcpy(&ex1, win->def + 48, 4);
    int32_t e = 2 * pixel(), m = pixel() + 32, M = m + w->tl.hblip_w - w->dx;
    int32_t A = (wl.x1 - wl.x0) - 2 * e, E = ex1 - ex0, wd = p->vis.x1 - p->vis.x0;
    if (M >= A) {
        *sx0 = wl.x0 + e;
        *sx1 = wl.x1 - e;
        if (*sx1 - *sx0 < 0)
            *sx0 = *sx1 = (int32_t)((uint32_t)(wl.x0 + wl.x1) >> 1);
        return;
    }
    int32_t S = muldiv(A, wd, E), wx1 = M > S ? wl.x1 - (M - S) : wl.x1;
    int32_t P = muldiv(p->scx - ex0, (wx1 - wl.x0) - 2 * e, E) + e;
    int32_t left = (wl.x0 + P) & ~(w->dx - 1);
    int32_t right = (wl.x0 + P + (S > M ? S : M)) & ~(w->dx - 1);
    if (wl.x1 - right < e) {
        left -= right - (wl.x1 - e);
        right = wl.x1 - e;
    }
    if (left - wl.x0 < e)
        left = wl.x0 + e;
    *sx0 = left, *sx1 = right;
}

static struct wimp_box scrollclip;

static int set_vclip(int32_t lo, int32_t hi)
{
    struct wimp_box b = { scrollclip.x0, scrollclip.y0 > lo ? scrollclip.y0 : lo, scrollclip.x1,
                          scrollclip.y1 < hi ? scrollclip.y1 : hi };
    gw(b);
    return b.y1 > b.y0;
}

static int set_hclip(int32_t lo, int32_t hi)
{
    struct wimp_box b = { scrollclip.x0 > lo ? scrollclip.x0 : lo, scrollclip.y0,
                          scrollclip.x1 < hi ? scrollclip.x1 : hi, scrollclip.y1 };
    gw(b);
    return b.x1 > b.x0;
}

static void funky_vscroll(struct wimp_box wl, int32_t sy0, int32_t sy1)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_tools *t = &w->tl;
    struct wimp_draw *d = dr();
    if (!meets(wl))
        return;
    struct wimp_box old = d->clip;
    scrollclip = meet(wl, old);
    uint32_t sc = ros_addr(t->scale), table = wimp_tool_table(d->scout);
    int32_t x = wl.x0;
    if (set_vclip(wl.y0, sy0)) {
        toolop(0x234, tool(TOOL_VWELLBCAP), x, wl.y0, sc, table);
        int32_t y = wl.y0 + t->vs_bot + w->dy;
        if (set_vclip(y, sy0))
            toolop(0x241, tool(TOOL_VWELLB), x, y, sc, table);
    }
    if (set_vclip(sy1, wl.y1)) {
        int32_t y = wl.y1 - t->vs_top - w->dy;
        toolop(0x234, tool(TOOL_VWELLTCAP), x, y, sc, table);
        if (set_vclip(sy1, y))
            toolop(0x241, tool(TOOL_VWELLT), x, y, sc, table);
    }
    if (set_vclip(sy0, sy1)) {
        table = wimp_tool_table(d->scin);
        toolop(0x234, tool(TOOL_VBARB), x, sy0, sc, table);
        int32_t y = sy0 + t->vs_blobbot + w->dy;
        if (set_vclip(y, sy1))
            toolop(0x241, tool(TOOL_VBARMID), x, y, sc, table);
        set_vclip(sy0, sy1);
        toolop(0x234, tool(TOOL_VBART), x, sy1 - t->vs_blobtop - w->dy, sc, table);
        if (tool(TOOL_VBLIP)) {
            int32_t b0 = sy0 + t->vs_blobbot + w->dy, b1 = sy1 - (t->vs_blobtop + w->dy);
            int32_t yb = b0 + ((b1 - b0 - t->vblip_h) >> 1);
            yb = (yb + w->dy - 1) & ~(w->dy - 1);
            toolop(0x234, tool(TOOL_VBLIP), x, yb, sc, table);
        }
    }
    gw(old);
}

static void funky_hscroll(struct wimp_box wl, int32_t sx0, int32_t sx1)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_tools *t = &w->tl;
    struct wimp_draw *d = dr();
    if (!meets(wl))
        return;
    struct wimp_box old = d->clip;
    scrollclip = meet(wl, old);
    uint32_t sc = ros_addr(t->scale), table = wimp_tool_table(d->scout);
    int32_t y = wl.y0;
    if (set_hclip(wl.x0, sx0)) {
        toolop(0x234, tool(TOOL_HWELLLCAP), wl.x0, y, sc, table);
        int32_t x = wl.x0 + t->hs_left + w->dx;
        if (set_hclip(x, sx0))
            toolop(0x241, tool(TOOL_HWELLL), x, y, sc, table);
    }
    if (set_hclip(sx1, wl.x1)) {
        int32_t x = wl.x1 - t->hs_right - w->dx;
        toolop(0x234, tool(TOOL_HWELLRCAP), x, y, sc, table);
        if (set_hclip(sx1, x))
            toolop(0x241, tool(TOOL_HWELLR), x, y, sc, table);
    }
    if (set_hclip(sx0, sx1)) {
        table = wimp_tool_table(d->scin);
        toolop(0x234, tool(TOOL_HBARL), sx0, y, sc, table);
        int32_t x = sx0 + t->hs_blobleft + w->dx;
        if (set_hclip(x, sx1))
            toolop(0x241, tool(TOOL_HBARMID), x, y, sc, table);
        set_hclip(sx0, sx1);
        toolop(0x234, tool(TOOL_HBARR), sx1 - t->hs_blobright - (w->dx >> 1), y, sc, table);
        if (tool(TOOL_HBLIP)) {
            int32_t b0 = sx0 + (t->hs_blobleft - w->dx), b1 = sx1 - (t->hs_blobright - w->dx);
            int32_t xb = b0 + ((b1 - b0 - t->hblip_w) >> 1);
            xb = (xb - (w->dx - 1)) & ~(w->dx - 1);
            toolop(0x234, tool(TOOL_HBLIP), xb, y, sc, table);
        }
    }
    gw(old);
}

static void flat_bar(struct wimp_box wl, struct wimp_box sl)
{
    struct wimp_draw *d = dr();
    bg(d->scout);
    drawicon_system(0x24u, ros_addr(d->glyph), wl);
    bg(d->scin);
    drawicon_system(0x24u, ros_addr(d->glyph), sl);
}

static void vertical_bar(const struct wimp_window *win, uint32_t f, struct wimp_box O)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_furniture *fu = &w->furn;
    struct wimp_box N = { O.x1 - (fu->V + w->dx), O.y0 + ((f & (F_SIZE | F_HBAR)) ? fu->H : 0), O.x1,
                          O.y1 - ((f & (F_TOGGLE | F_TITLE)) ? fu->T : 0) };
    struct wimp_box V = N;
    if (V.y0 > N.y1 - w->dy) V.y0 = N.y1 - w->dy;
    if (V.y1 < N.y0 + w->dy) V.y1 = N.y0 + w->dy;
    if (V.y1 < V.y0 + w->dy) V.y1 = V.y0 + w->dy;
    int32_t len = V.y1 - V.y0;
    if (len <= w->dy)
        return;
    if (fu->U + fu->D > len) {
        int32_t h2 = (int32_t)((uint32_t)len >> 1) & ~(w->dy - 1);
        struct wimp_box up = { V.x0, V.y0 + h2, V.x1, V.y1 }, dn = { V.x0, V.y0, V.x1, V.y0 + h2 + w->dy };
        if (!windowglyph_scaled(TOOL_UP, fu->U, up, 1))
            glyph(0x8B, up);
        if (!windowglyph_scaled(TOOL_DOWN, fu->D, dn, 1))
            glyph(0x8A, dn);
        return;
    }
    uint32_t sc = ros_addr(w->tl.scale);
    struct wimp_box up = { V.x0, V.y1 - (fu->U + w->dy), V.x1, V.y1 };
    struct wimp_box dn = { V.x0, V.y0, V.x1, V.y0 + fu->D + w->dy };
    if (!windowglyph(TOOL_UP, up, sc))
        glyph(0x8B, up);
    if (!windowglyph(TOOL_DOWN, dn, sc))
        glyph(0x8A, dn);
    struct wimp_box wl = { N.x0, N.y0 + fu->D, N.x1, N.y1 - fu->U };
    int32_t sy0, sy1;
    vslider(win, wl, &sy0, &sy1);
    if (tool(TOOL_VWELLTCAP))
        funky_vscroll(wl, sy0, sy1);
    else
        flat_bar(wl, (struct wimp_box){ wl.x0 + 8, sy0, wl.x1 - 8, sy1 });
}

static void horizontal_bar(const struct wimp_window *win, uint32_t f, struct wimp_box O)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_furniture *fu = &w->furn;
    struct wimp_box N = { O.x0, O.y0, O.x1 - ((f & (F_SIZE | F_VBAR)) ? fu->V : 0), O.y0 + fu->H + w->dy };
    struct wimp_box V = N;
    if (V.x0 > N.x1 - w->dx) V.x0 = N.x1 - w->dx;
    if (V.x1 < N.x0 + w->dx) V.x1 = N.x0 + w->dx;
    if (V.x1 < V.x0 + w->dx) V.x1 = V.x0 + w->dx;
    int32_t len = V.x1 - V.x0;
    if (len <= w->dx)
        return;
    if (fu->L + fu->R > len) {
        int32_t h2 = (len >> 1) & ~(w->dx - 1);
        struct wimp_box l = { V.x0, V.y0, V.x0 + h2 + w->dx, V.y1 }, r = { V.x0 + h2, V.y0, V.x1, V.y1 };
        if (!windowglyph_scaled(TOOL_LEFT, fu->L, l, 0))
            glyph(0x88, l);
        if (!windowglyph_scaled(TOOL_RIGHT, fu->R, r, 0))
            glyph(0x89, r);
        return;
    }
    uint32_t sc = ros_addr(w->tl.scale);
    struct wimp_box l = { V.x0, V.y0, V.x0 + fu->L + w->dx, V.y1 };
    struct wimp_box r = { V.x1 - (fu->R + w->dx), V.y0, V.x1, V.y1 };
    if (!windowglyph(TOOL_LEFT, l, sc))
        glyph(0x88, l);
    if (!windowglyph(TOOL_RIGHT, r, sc))
        glyph(0x89, r);
    struct wimp_box wl = { N.x0 + fu->L, N.y0, N.x1 - fu->R, N.y1 };
    int32_t sx0, sx1;
    hslider(win, wl, &sx0, &sx1);
    if (tool(TOOL_HWELLLCAP))
        funky_hscroll(wl, sx0, sx1);
    else
        flat_bar(wl, (struct wimp_box){ sx0, wl.y0 + 8, sx1, wl.y1 - 8 });
}

/* ---- the border pass --------------------------------------------------------------------------------- */

static void border_rect(const struct wimp_window *win, struct wimp_box R)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    struct wimp_furniture *fu = &w->furn;
    const struct wimp_place *p = &win->s[APP];
    uint32_t f = p->flags;
    struct wimp_task *ct = wimp_current();
    wimp_filter_rect(win->handle, ct ? ct->internal : 0, R);    /* the filters come first */
    gw(R);
    if (win->def[32] == 0xFF && !(f & F_FURNITURE))
        return;
    if (win->def[32] == 0xFF) {
        fg(d->titlefg);
    } else {
        struct wimp_box B = { p->vis.x0 - w->dx, p->vis.y0 - w->dy, p->vis.x1 + w->dx, p->vis.y1 + w->dy };
        fg(d->titlefg == 0 ? OUTLINE_COLOUR : d->titlefg);
        hollow(B);
        if (d->titlefg == 0)
            fg(d->titlefg);
    }
    bg(d->titlecolour);
    if (!w->tl.built && w->tools)
        wimp_tools_refresh();
    struct wimp_box O = p->outline;
    int32_t T1 = fu->T + w->dy, V1 = fu->V + w->dx;
    uint32_t sc = ros_addr(w->tl.scale);
    int32_t left = O.x0;
    if (f & F_BACK) {
        struct wimp_box b = { O.x0, O.y1 - T1, O.x0 + fu->B + w->dx, O.y1 };
        if (!windowglyph(TOOL_BACK, b, sc))
            glyph(0x85, b);
        left += fu->B;
    }
    if (f & F_CLOSE) {
        struct wimp_box b = { left, O.y1 - T1, left + fu->C + w->dx, O.y1 };
        if (!windowglyph(TOOL_CLOSE, b, sc))
            glyph(0x84, b);
        left += fu->C;
    }
    int iconise = (f & F_CLOSE) && p->parent == NO_WINDOW;
    int32_t right = O.x1 - ((f & F_TOGGLE) ? fu->V : 0);
    if (f & F_TITLE) {
        struct wimp_box b = { left, O.y1 - T1, right - (iconise ? fu->I : 0), O.y1 };
        uint32_t tf;
        memcpy(&tf, win->def + 56, 4);
        uint32_t F = ((tf | IF_BORDER | IF_FILLED) & ~0xF000u) | 0xF000u;
        if (tool(TOOL_TBARLCAP))
            title_bar(win, F, b);
        else
            drawicon_system(F, ros_addr((void *)(win->def + 72)), b);
    }
    if (f & F_TOGGLE) {
        struct wimp_box b = { O.x1 - V1, O.y1 - T1, O.x1, O.y1 };
        int toggled = (f & ((1u << 18) | (1u << 22))) != 0;
        uint32_t s = tool(toggled ? TOOL_TOGGLE1 : TOOL_TOGGLE);
        if (s)
            spriteglyph(s, b);
        else
            glyph(toggled ? 0x82 : 0x81, b);
    }
    if (iconise) {
        struct wimp_box b = { right - fu->I - w->dx, O.y1 - T1, right, O.y1 };
        if (!windowglyph(TOOL_ICONISE, b, sc))
            glyph(0x98, b);
    }
    if (f & F_VBAR)
        vertical_bar(win, f, O);
    bg(d->titlecolour);
    struct wimp_box sz = { O.x1 - V1, O.y0, O.x1, O.y0 + fu->H + w->dy };
    if (f & F_SIZE) {
        if (!windowglyph(TOOL_SIZE, sz, sc))
            glyph(0x83, sz);
    } else if ((f & F_VBAR) && (f & F_HBAR)) {
        if (tool(TOOL_BLANK))
            spriteglyph(tool(TOOL_BLANK), sz);
        else
            glyph(0, sz);
    }
    if (f & F_HBAR)
        horizontal_bar(win, f, O);
}

/* PlotWindowBorders: with Use3DBorders, this draws a slab out on the
 * visible work area, clipped to the rectangle.  It is drawn for:
 * - the icon bar, with its ends off the screen unless Fully3DIconBar;
 * - menus, in their own colours with UseAlternateMenuTexture;
 * - windows of background colour 1, by bits 3-2 of the byte at +39.  10
 *   means always and 01 means never.  Otherwise the slab is drawn when the
 *   window has no scroll bars or size icon, is not a pane and is not a
 *   child. */
void wimp_draw_3d_border(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    uint32_t t = d->threed;
    if (!(t & 1u))
        return;
    const struct wimp_place *p = &win->s[APP];
    struct wimp_box b = p->vis;
    uint32_t face = d->theme[TH_WBFC], opp = d->theme[TH_WBOC];
    if (win->handle == w->iconbar) {
        if (!(t & 4u))
            b.x0 -= 16, b.x1 += 16;
    } else if (win->owner == NO_WINDOW) {
        if (t & 2u)
            face = d->theme[TH_MBFC], opp = d->theme[TH_MBOC];
    } else {
        if (win->def[35] != 1)
            return;
        uint32_t f2 = win->def[39] & 0x0Cu;
        if (f2 == 0x04u)
            return;
        if (f2 != 0x08u && ((p->flags & 0x70000000u) || (p->flags & 0x20u) || p->parent != NO_WINDOW))
            return;
    }
    uint32_t sf = d->tc[TC_FACE], so = d->tc[TC_OPP];
    d->tc[TC_FACE] = face, d->tc[TC_OPP] = opp;
    slab(&b, 0);
    d->tc[TC_FACE] = sf, d->tc[TC_OPP] = so;
    if (d->tc[TC_FG] != 0xFFFFFFFFu)
        fg(d->tc[TC_FG]);                       /* put the foreground back */
}

void wimp_draw_border(struct wimp_window *win, const struct wimp_rlist *rects)
{
    if (wimp_ws()->tl.unlisted && wimp_ws()->tools)
        wimp_tools_refresh();                   /* maketoollist, the first time it is needed */
    window_colours(win);
    for (uint32_t i = 0; i < rects->n; i++)
        border_rect(win, rects->b[i]);
    wimp_font_exit();
}

/* ---- the caret: where it sits in an icon, and its pixels ------------ */

/* The icon's inset box, flags and text, for the caret's computations */
static int caret_icon(struct wimp_window *win, int32_t icon, struct wimp_box *box, uint32_t *flags,
                      uint32_t *text)
{
    struct wimp_ws *w = wimp_ws();
    const uint8_t *ic = wimp_icon(win, (uint32_t)icon);
    if (!ic)
        return 0;
    window_colours(win);
    int32_t b[4];
    memcpy(b, ic, 16);
    memcpy(flags, ic + 16, 4);
    uint32_t data = ros_addr((void *)(ic + 20));
    *flags = seticonptrs(*flags, data);
    *text = (*flags & IF_INDIRECT) ? ros_ld32(data) : data;
    int32_t ox = wimp_origin_x(win, APP), oy = wimp_origin_y(win, APP);
    *box = (struct wimp_box){ ox + b[0], oy + b[1], ox + b[2], oy + b[3] };
    getborder(*flags);
    if (*flags & IF_BORDER) {
        static const int32_t inset[8] = { 0, 4, 4, 8, 8, 4, 12, 8 };
        uint32_t T = dr()->border_type;
        if (T == 0 || T == 7)
            box->x0 += w->dx, box->x1 -= w->dx, box->y0 += w->dy, box->y1 -= w->dy;
        box->x0 += inset[T], box->x1 -= inset[T], box->y0 += inset[T], box->y1 -= inset[T];
    }
    return 1;
}

/* shrinkcaret: keeps the caret inside the icon's height */
static void shrink(const struct wimp_box *in, int32_t *y, uint32_t *hword)
{
    if (*y < in->y0)
        *y = in->y0;
    int32_t top = *y + (int16_t)(*hword & 0xFFFFu);
    if (top > in->y1)
        *hword += (uint32_t)(in->y1 - top);
}

/* Chooses what scrolls an icon's text: the ghost caret, else the
 * selection, else the main caret.  dest says which offset is being
 * measured.  That offset counts as being in the icon, with value v.
 * sel_ok says whether the window's selection counts, if it is in this
 * icon. */
static void scroll_for(struct wimp_window *win, int32_t icon, int dest, int32_t v, int sel_ok)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    int ghost = dest == CC_GHOST || (w->ghost.w == win->handle && w->ghost.i == icon);
    int sel = dest == CC_SEL_LOW || dest == CC_SEL_HIGH || (sel_ok && win->sel.icon == icon && icon >= 0);
    int main = dest == CC_MAIN || (w->caret.w == win->handle && w->caret.i == icon);
    d->scroll_mode = SC_NONE;
    if ((w->is.state & 1u) && sel && win->sel.xoverride != SEL_BIGNUM) {
        d->scroll_mode = SC_ADD, d->scroll_v = win->sel.xoverride;     /* a selection drag's held scroll */
    } else if (ghost && w->ghost_xoverride != SEL_BIGNUM) {
        d->scroll_mode = SC_ADD, d->scroll_v = w->ghost_xoverride;      /* the ghost's, while autoscrolling */
    } else if (ghost) {
        d->scroll_mode = SC_CENTRE, d->scroll_v = dest == CC_GHOST ? v : w->ghostcaretx;
    } else if (sel) {
        if (win->sel.xoverride != SEL_BIGNUM) {
            d->scroll_mode = SC_ADD, d->scroll_v = win->sel.xoverride;
        } else {
            d->scroll_mode = SC_SEL;
            d->scroll_sx = dest == CC_SEL_LOW ? v : win->sel.xoff;
            d->scroll_sw = dest == CC_SEL_HIGH ? v : win->sel.width;
        }
    } else if (main) {
        d->scroll_mode = SC_CENTRE, d->scroll_v = dest == CC_MAIN ? v : w->caretx;
    }
}

/* The text origin with the scroll that applies for dest at offset v */
static void origin_with(struct wimp_window *win, int32_t icon, uint32_t *flags, uint32_t text,
                        struct wimp_box in, int dest, int32_t v, struct textpos *tp)
{
    struct wimp_draw *d = dr();
    int mode = d->scroll_mode;
    int32_t sv = d->scroll_v, sx = d->scroll_sx, sw = d->scroll_sw;
    scroll_for(win, icon, dest, v, 1);
    textorigin(flags, text, in, tp);
    d->scroll_mode = mode, d->scroll_v = sv, d->scroll_sx = sx, d->scroll_sw = sw;
}

/* setcaretcoords: for an index into the text, gives the screen x and y,
 * the height word and the offset from the text origin */
int wimp_caret_coords(struct wimp_window *win, int32_t icon, uint32_t index, int dest, int32_t *x,
                      int32_t *y, uint32_t *hword, int32_t *cx)
{
    struct wimp_box in;
    uint32_t flags, text;
    if (!caret_icon(win, icon, &in, &flags, &text))
        return 0;
    int32_t offx, offy;
    if (!outline(flags)) {
        offx = (int32_t)index * 16, offy = -32;
        *hword = 40u | (1u << 24);
    } else {
        uint32_t h = font_of(flags), s = fontstring(flags, text, h), m = 0, n = 0;
        while (n < index && ros_ld8(text + n) >= 32)
            n++;
        for (uint32_t k = 0, p = s;; ) {        /* m: n characters into s, skipping controls */
            uint32_t ch = ros_ld8(p);
            if (ch == 26) { p += 2, m += 2; continue; }
            if (ch == 19) { p += 8, m += 8; continue; }
            if (k == n || ch == 0)
                break;
            p++, m++, k++;
        }
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = h, c.r[1] = s, c.r[2] = 0x80u, c.r[3] = c.r[4] = 0x20000000u, c.r[7] = m;
        uint32_t X = call(XFont_ScanString, &c) ? c.r[3] : 0;
        ros_cpu_enter(&c);
        c.r[1] = X, c.r[2] = X;
        int32_t Wx = call(XFont_ConverttoOS, &c) ? (int32_t)c.r[1] : 0;
        ros_cpu_enter(&c);
        c.r[0] = h;
        int32_t by0 = 0, by1 = 0;
        if (call(XFont_ReadInfo, &c))
            by0 = (int32_t)c.r[2], by1 = (int32_t)c.r[4];
        offx = Wx + 2, offy = by0;
        *hword = (uint32_t)(by1 - by0);
    }
    *cx = offx;
    struct textpos tp;
    origin_with(win, icon, &flags, text, in, dest, offx, &tp);
    *x = tp.tx + offx;
    *y = tp.ty + offy;
    shrink(&in, y, hword);
    return 1;
}

/* findcaret: the index nearest the screen point x, y, and its caret */
int wimp_caret_find(struct wimp_window *win, int32_t icon, int32_t px, int32_t py, int dest, int32_t *x,
                    int32_t *y, uint32_t *hword, uint32_t *index, int32_t *cx)
{
    struct wimp_box in;
    uint32_t flags, text;
    if (!caret_icon(win, icon, &in, &flags, &text))
        return 0;
    struct textpos tp;
    origin_with(win, icon, &flags, text, in, CC_NONE, 0, &tp);     /* as the icon is now */
    int32_t X0 = tp.tx, Y0 = tp.ty, offx, offy;
    if (!outline(flags)) {
        int32_t k = (px + 8 - X0) >> 4;
        uint32_t i = 0;
        while (!((int32_t)i >= k || ros_ld8(text + i) < 32))
            i++;
        *index = i;
        offx = (int32_t)i * 16, offy = -32;
        *hword = 40u | (1u << 24);
    } else {
        uint32_t f = font_of(flags), s = fontstring(flags, text, f);
        int32_t dx = px - X0, dy = py - Y0;
        if (dx > 0x100000) dx = 0x100000;
        if (dy > 0x100000) dy = 0x100000;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = f, c.r[1] = (uint32_t)dx, c.r[2] = (uint32_t)dy;
        uint32_t mx = 0, my = 0;
        if (call(XFont_Converttopoints, &c))
            mx = c.r[1], my = c.r[2];
        ros_cpu_enter(&c);
        c.r[0] = f, c.r[1] = s, c.r[2] = 0x20000u, c.r[3] = mx, c.r[4] = my;
        uint32_t at = s, rx = 0, ry = 0;
        if (call(XFont_ScanString, &c))
            at = c.r[1], rx = c.r[3], ry = c.r[4];
        uint32_t n = 0;
        for (uint32_t p = s; p < at;) {
            uint32_t ch = ros_ld8(p);
            if (ch == 26) { p += 2; continue; }
            if (ch == 19) { p += 8; continue; }
            p++, n++;
        }
        *index = n;
        ros_cpu_enter(&c);
        c.r[0] = f, c.r[1] = rx, c.r[2] = ry;
        int32_t ox = 0, oy = 0;
        if (call(XFont_ConverttoOS, &c))
            ox = (int32_t)c.r[1], oy = (int32_t)c.r[2];
        ros_cpu_enter(&c);
        c.r[0] = f;
        int32_t by0 = 0, by1 = 0;
        if (call(XFont_ReadInfo, &c))
            by0 = (int32_t)c.r[2], by1 = (int32_t)c.r[4];
        offx = ox + 2, offy = oy + by0;
        *hword = (uint32_t)(by1 - by0);
    }
    *cx = offx;
    origin_with(win, icon, &flags, text, in, dest, offx, &tp);
    *x = tp.tx + offx;
    *y = tp.ty + offy;
    shrink(&in, y, hword);
    return 1;
}

/* iconautoscroll_start's measurements.  They are taken on the icon's own
 * box, without the border inset, as 5.30 does.  Returns 0 if the text is
 * no wider than the box.  Otherwise it gives the text's width, the box's
 * width, and in *hold the held scroll that keeps the text where it is now.
 * For a selection, the main caret is taken as being in the icon.  For the
 * ghost caret (ghost set), the icon is taken as it is. */
int wimp_sel_hold(struct wimp_window *win, int32_t icon, int ghost, int32_t *tw, int32_t *iw, uint32_t *flags,
                  int32_t *hold)
{
    struct wimp_draw *d = dr();
    const uint8_t *ic = wimp_icon(win, (uint32_t)icon);
    if (!ic)
        return 0;
    window_colours(win);
    int32_t b[4];
    memcpy(b, ic, 16);
    uint32_t f, data = ros_addr((void *)(ic + 20));
    memcpy(&f, ic + 16, 4);
    f = seticonptrs(f, data);
    uint32_t text = (f & IF_INDIRECT) ? ros_ld32(data) : data;
    int32_t th;
    *tw = textwidth(f, text, &th);
    *iw = b[2] - b[0];
    *flags = f;
    if (*tw <= *iw)
        return 0;
    struct wimp_box box = { b[0], b[1], b[2], b[3] };
    int mode = d->scroll_mode;
    int32_t sv = d->scroll_v, sx = d->scroll_sx, sw = d->scroll_sw;
    struct textpos now, zero;
    uint32_t f1 = f, f2 = f;
    if (ghost)
        scroll_for(win, icon, CC_NONE, 0, 1);
    else
        scroll_for(win, icon, CC_MAIN, wimp_ws()->caretx, 1);
    textorigin(&f1, text, box, &now);
    d->scroll_mode = SC_ADD, d->scroll_v = 0;
    textorigin(&f2, text, box, &zero);
    d->scroll_mode = mode, d->scroll_v = sv, d->scroll_sx = sx, d->scroll_sw = sw;
    *hold = now.tx - zero.tx;
    return 1;
}

/* Draws the caret's pixels by EOR at the screen point x, y */
void wimp_caret_draw(const struct wimp_caretblk *c, int32_t x, int32_t y)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t hf = c->hf;
    if (hf & (1u << 25))
        return;
    struct ros_cpu r;
    uint32_t G;
    if (hf & (1u << 30)) {
        ros_cpu_enter(&r);
        r.r[0] = 0x80808000u;
        if (!call(XColourTrans_ReturnGCOL, &r))
            return;
        G = r.r[0];
    } else {
        uint32_t col = (hf & (1u << 26)) ? (hf >> 16) & 0xFFu : 11;
        if ((hf & (1u << 26)) && (hf & (1u << 27))) {
            G = col;
        } else {
            ros_cpu_enter(&r);
            r.r[0] = w->palette[col & 15];
            if (!call(XColourTrans_ReturnGCOL, &r))
                return;
            G = r.r[0];
            ros_cpu_enter(&r);
            r.r[0] = w->palette[0];
            if (!call(XColourTrans_ReturnGCOL, &r))
                return;
            G ^= r.r[0];
        }
    }
    int32_t h = (int16_t)(hf & 0xFFFFu);
    if (hf & (1u << 24)) {
        ros_cpu_enter(&r);
        r.r[0] = G, r.r[3] = 0, r.r[4] = 3;
        if (!call(XColourTrans_SetColour, &r))
            return;
        plot(4, x, y);
        plot(1, 0, h - 1);
        plot(0, -2, 0);
        plot(1, 4, 0);
        plot(4, x - 2, y);
        plot(1, 4, 0);
    } else {
        ros_cpu_enter(&r);
        r.r[0] = G, r.r[1] = (uint32_t)h, r.r[2] = 0x14u, r.r[3] = (uint32_t)x, r.r[4] = (uint32_t)y;
        ros_swi(&r, XFont_Caret);
    }
}

/* ---- geometry for input ----------------------------------------- */

int32_t wimp_muldiv(int32_t a, int32_t b, int32_t c)
{
    return muldiv(a, b, c);
}

/* A scroll bar's box (calc_w_iconposn 5 or 7, unclamped), its well and its
 * sausage, as the border pass draws them */
void wimp_scroll_geom(const struct wimp_window *win, int vertical, struct wimp_box *bar,
                      struct wimp_box *well, struct wimp_box *sausage)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_furniture *fu = &w->furn;
    struct wimp_box O = win->s[APP].outline;
    uint32_t f = win->s[APP].flags;
    if (vertical) {
        *bar = (struct wimp_box){ O.x1 - (fu->V + w->dx), O.y0 + ((f & (F_SIZE | F_HBAR)) ? fu->H : 0), O.x1,
                                  O.y1 - ((f & (F_TOGGLE | F_TITLE)) ? fu->T : 0) };
        *well = (struct wimp_box){ bar->x0, bar->y0 + fu->D, bar->x1, bar->y1 - fu->U };
        int32_t s0, s1;
        vslider(win, *well, &s0, &s1);
        *sausage = (struct wimp_box){ well->x0, s0, well->x1, s1 };
    } else {
        *bar = (struct wimp_box){ O.x0, O.y0, O.x1 - ((f & (F_SIZE | F_VBAR)) ? fu->V : 0), O.y0 + fu->H + w->dy };
        *well = (struct wimp_box){ bar->x0 + fu->L, bar->y0, bar->x1 - fu->R, bar->y1 };
        int32_t s0, s1;
        hslider(win, *well, &s0, &s1);
        *sausage = (struct wimp_box){ s0, well->y0, s1, well->y1 };
    }
}

/* ---- tiles ------------------------------------------------------------------ */

/* The slot's tile, looked up once.  Returns 0 when the slot has none. */
static uint32_t tile_lookup(int slot, uint32_t *area)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_tile *t = &w->tiles[slot];
    if (t->looked) {
        *area = t->area;
        return t->sprite;
    }
    t->looked = 1;
    t->sprite = 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 161, c.r[1] = 0x8C;
    ros_swi(&c, XOS_Byte);
    if (!c.v && (c.r[2] & 0x80u))
        return 0;                               /* tiling disabled */
    char base[8];
    for (int pass = 0; pass < 2 && !t->sprite; pass++) {
        if (slot == 17 && pass == 1)
            snprintf(base, sizeof base, "tile_1");
        else if (slot == 16)
            snprintf(base, sizeof base, "tile_m");
        else if (slot == 17)
            snprintf(base, sizeof base, "tile_i");
        else if (pass == 1)
            break;
        else
            snprintf(base, sizeof base, "tile_%d", slot);
        uint32_t bpp = 1u << w->log2bpp;
        char names[6][16];
        int n = 0;
        for (uint32_t b = bpp; b >= 8 || b == bpp; b >>= 1) {
            snprintf(names[n++], 16, "%s-%u", base, b);
            if (b < 8 || n == 4)
                break;
        }
        snprintf(names[n++], 16, "%s", base);
        uint32_t areas[2] = { w->ram_sprites, w->rom_sprites };
        for (int k = 0; k < n && !t->sprite; k++)
            for (int a = 0; a < 2 && !t->sprite; a++) {
                if (!areas[a])
                    continue;
                char *nm = (char *)w->scratch + 400;
                snprintf(nm, 16, "%s", names[k]);
                ros_cpu_enter(&c);
                c.r[0] = 0x118, c.r[1] = areas[a], c.r[2] = ros_addr(nm);
                ros_swi(&c, XOS_SpriteOp);
                if (!c.v) {
                    t->sprite = c.r[2];
                    t->area = areas[a];
                }
            }
    }
    if (t->sprite) {
        int32_t sw, sh;
        sprite_size(t->area, t->sprite, 0, &sw, &sh);
        memcpy(t->scale, dr()->sprite_scale, sizeof t->scale);
        t->table = sprite_table(t->sprite, t->area, 0);
        if (t->table) {                         /* its own copy, as the icons' is reused */
            struct wimp_draw *d = dr();
            uint32_t buf = wimp_table_space(&t->pix_at, &t->pix_size, d->pix_used);
            if (buf)
                memcpy(ros_ptr(buf), ros_ptr(d->pix_at), d->pix_used);
            t->table = buf;
        }
    }
    *area = t->area;
    return t->sprite;
}

void wimp_tiles_forget(void)
{
    struct wimp_ws *w = wimp_ws();
    for (unsigned i = 0; i < sizeof w->tiles / sizeof w->tiles[0]; i++)
        w->tiles[i].looked = 0;
}

/* Clears the work area by plotting its tile over the graphics window, if
 * it has one.  Returns 0 when the caller is to clear to the background
 * colour instead. */
int wimp_draw_tile(const struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t c = win->def[35];
    if (c > 15 || (win->def[39] & 1u))
        return 0;
    int slot = (int)c;
    if (win->handle == w->iconbar)
        slot = 17;
    else if (win->owner == NO_WINDOW) {
        if (!(w->dr.threed & 0x10u))
            return 0;
        slot = (w->dr.threed & 2u) ? 16 : 1;    /* tile_m with UseAlternateMenuTexture */
    }
    uint32_t area = 0, sprite = 0, table = 0, scale = 0;
    uint32_t warea;
    memcpy(&warea, win->def + 64, 4);
    if (slot == 1 && warea >= 2) {
        char *nm = (char *)w->scratch + 400;
        strcpy(nm, "tile_1");
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[0] = 0x118, r.r[1] = warea, r.r[2] = ros_addr(nm);
        ros_swi(&r, XOS_SpriteOp);
        if (!r.v) {
            sprite = r.r[2], area = warea;
            int32_t sw, sh;
            sprite_size(area, sprite, 0, &sw, &sh);
            table = sprite_table(sprite, area, 0);
            scale = ros_addr(dr()->sprite_scale);
        }
    }
    if (!sprite) {
        sprite = tile_lookup(slot, &area);
        if (!sprite)
            return 0;
        table = w->tiles[slot].table;
        scale = ros_addr(w->tiles[slot].scale);
    }
    struct ros_cpu r;
    ros_cpu_enter(&r);
    r.r[0] = 0x241, r.r[1] = area, r.r[2] = sprite;
    int32_t ax = wimp_origin_x(win, APP), ay = wimp_origin_y(win, APP);
    if (dr()->buffered)                         /* screen coordinates stand in for the */
        ax += dr()->buf_x0, ay += dr()->buf_y0; /* buffer's, so the tile moves by its place */
    r.r[3] = (uint32_t)ax, r.r[4] = (uint32_t)ay;
    r.r[5] = 0, r.r[6] = scale, r.r[7] = table;
    ros_swi(&r, XOS_SpriteOp);
    return 1;
}

/* ---- Wimp_TextOp ---------------------------------------------------------- */

/* Wimp_TextOp's string.  There is no icon, so there is no validation
 * string and no selection, whatever icon was drawn last.  That icon's
 * strings may be in another slot. */
static uint32_t textop_string(uint32_t text)
{
    struct wimp_draw *d = dr();
    uint32_t v = d->validation;
    int sel = d->sel_insert;
    d->validation = 0;
    d->sel_insert = 0;
    uint32_t s = fontstring(IF_INDIRECT, text, d->systemfont);
    d->validation = v;
    d->sel_insert = sel;
    return s;
}

static int32_t textop_width(uint32_t text, int32_t n)
{
    struct wimp_draw *d = dr();
    if (!d->systemfont) {
        int32_t k = 0;
        while (ros_ld8(text + (uint32_t)k) >= 32 && (n <= 0 || k < n))
            k++;
        return 16 * k;
    }
    uint32_t s = textop_string(text);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = d->systemfont, c.r[1] = s, c.r[2] = n > 0 ? 0x80u : 0, c.r[3] = c.r[4] = 0x0FFFFFFFu;
    c.r[7] = (uint32_t)n + 2u;
    uint32_t X = call(XFont_ScanString, &c) ? c.r[3] : 0;
    ros_cpu_enter(&c);
    c.r[1] = X, c.r[2] = 0;
    return call(XFont_ConverttoOS, &c) ? (int32_t)c.r[1] : 0;
}

static uint32_t textop_split(uint32_t text, int32_t width, uint32_t split)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    if (!d->systemfont) {
        int32_t k = width >> 4;
        if (split == 0xFFFFFFFFu) {
            int32_t i = 0;
            while (i < k && ros_ld8(text + (uint32_t)i) >= 32)
                i++;
            return text + (uint32_t)i;
        }
        uint32_t at = text;
        for (int32_t i = 1; i <= k; i++) {
            uint32_t ch = ros_ld8(text + (uint32_t)i);
            if (ch < 32) {
                at = text + (uint32_t)i;
                break;
            }
            if (ch == split)
                at = text + (uint32_t)i;
        }
        return at;
    }
    uint32_t s = textop_string(text);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = d->systemfont, c.r[1] = (uint32_t)width, c.r[2] = 0;
    uint32_t mx = call(XFont_Converttopoints, &c) ? c.r[1] : 0;
    uint32_t *blk = d->bbox;
    memset(blk, 0, 9 * 4);
    blk[4] = split;
    ros_cpu_enter(&c);
    c.r[0] = d->systemfont, c.r[1] = s, c.r[2] = (split != 0xFFFFFFFFu ? 0x20u : 0) | 0x100u;
    c.r[3] = mx, c.r[4] = 0x0FFFFFFFu, c.r[5] = ros_addr(blk);
    uint32_t end = call(XFont_ScanString, &c) ? c.r[1] : s;
    uint32_t n = 0;
    for (uint32_t p = s; p < end;) {
        uint32_t ch = ros_ld8(p);
        if (ch == 26) { p += 2; continue; }
        if (ch == 19) { p += 8; continue; }
        p++, n++;
    }
    (void)w;
    return text + n;
}

void wimp_swi_TextOp(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = dr();
    uint32_t reason = s->r[0] & 0xFFu;
    s->v = 0;
    switch (reason) {
    case 0:
        d->textop_fg = s->r[1], d->textop_bg = s->r[2], d->textop_set = 1;
        if (!d->systemfont) {
            wimp_gcol(s->r[2], 1);
            wimp_gcol(s->r[1], 0);
        }
        return;
    case 1:
        s->r[0] = (uint32_t)textop_width(s->r[1], (int32_t)s->r[2]);
        return;
    case 2: {
        uint32_t f = s->r[0];
        int32_t x = (int32_t)s->r[4], y = (int32_t)s->r[5];
        if (f & (1u << 29)) {
            struct wimp_window *win = w->pending_window ? wimp_window(w->pending_window) : NULL;
            if (win)
                x += win->s[APP].vis.x0, y += win->s[APP].vis.y1;
        }
        if (d->systemfont) {
            uint8_t *o = d->fontstr;
            unsigned n = 0;
            uint32_t text = s->r[1];
            /* Make the font string of the text first, and copy it out.  It
             * is made in d->fontstr, which is where the colour change ahead
             * of it is about to go.  Without the copy, a TextGadgets
             * scrolling list, which sets its colours for every row, had each
             * row's first characters drawn ahead of it. */
            uint8_t sub[sizeof d->fontstr];
            uint32_t m = 0;
            for (uint32_t p = textop_string(text); ros_ld8(p) && m < sizeof sub - 1; p++)
                sub[m++] = (uint8_t)ros_ld8(p);
            if (d->textop_set) {
                uint32_t bg = d->textop_bg, fg = d->textop_fg;
                o[n++] = 19;
                o[n++] = (uint8_t)(bg >> 8), o[n++] = (uint8_t)(bg >> 16), o[n++] = (uint8_t)(bg >> 24);
                o[n++] = (uint8_t)(fg >> 8), o[n++] = (uint8_t)(fg >> 16), o[n++] = (uint8_t)(fg >> 24);
                o[n++] = 14;
                d->textop_set = 0;
            }
            for (uint32_t i = 0; i < m && n < sizeof d->fontstr - 1; i++)
                o[n++] = sub[i];
            o[n] = 0;
            if (f & (1u << 30))
                y -= (d->systemfonty1 - 28) >> 1;
            if (f & (1u << 28))
                y -= d->systemfonty1 >> 1;
            if (f & (1u << 31))
                x -= textop_width(text, 0);
            x &= ~(w->dx - 1);
            y = (y + w->dy - 1) & ~(w->dy - 1);
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[1] = ros_addr(o), c.r[2] = 0x10u, c.r[3] = (uint32_t)x, c.r[4] = (uint32_t)y;
            ros_swi(&c, XFont_Paint);
        } else {
            if (f & (1u << 28))
                y -= 16;
            x &= ~(w->dx - 1);
            y = (y - (w->dy - 1)) & ~(w->dy - 1);
            if (f & (1u << 31)) {
                int32_t k = 0;
                while (ros_ld8(s->r[1] + (uint32_t)k) > 31)
                    k++;
                x -= 16 * k;
            }
            plot(4, x, y + 28);
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = s->r[1];
            ros_swi(&c, XOS_Write0);
        }
        return;
    }
    case 3:
        s->r[0] = textop_split(s->r[1], (int32_t)s->r[2], s->r[3]);
        return;
    case 4: {
        uint32_t text = s->r[1], buf = s->r[2], size = s->r[3];
        uint32_t len = 0;
        while (ros_ld8(text + len) >= 32)
            len++;
        int32_t width = textop_width(text, 0);
        if ((uint32_t)width <= s->r[4]) {
            s->r[0] = len + 1;
            for (uint32_t i = 0; i < size && i <= len; i++)
                ros_st8(buf + i, ros_ld8(text + i));
            return;
        }
        static const uint8_t dots[3] = { '.', '.', '.' };
        uint8_t one = 0x8C;                     /* Latin-1's ellipsis */
        const uint8_t *e = d->systemfont ? &one : dots;
        uint32_t en = d->systemfont ? 1 : 3;
        int32_t ew = d->systemfont ? 0 : 48;
        if (d->systemfont) {
            uint8_t *t = d->fontstr + 480;
            t[0] = 0x8C, t[1] = 0;
            ew = (textop_width(ros_addr(t), 0) + w->dx - 1) & ~(w->dx - 1);
        }
        uint32_t k = textop_split(text, (int32_t)s->r[4] - ew, 0xFFFFFFFFu) - text;
        s->r[0] = k + en + 1;
        uint32_t o = 0;
        for (uint32_t i = 0; i < k && o < size; i++)
            ros_st8(buf + o++, ros_ld8(text + i));
        for (uint32_t i = 0; i < en && o < size; i++)
            ros_st8(buf + o++, e[i]);
        if (o < size)
            ros_st8(buf + o, 0);
        return;
    }
    }
    wimp_fail(s, ros_error(0x297, "Bad reason code"));
}

/* The number of lines that formatted text takes in a box.  This is the
 * first, counting pass of the formatted text code, with a line spacing of
 * 40. */
int wimp_count_lines(const struct wimp_window *win, uint32_t flags, uint32_t text, struct wimp_box b)
{
    struct wimp_draw *d = dr();
    window_colours(win);
    d->linespacing = 40;
    d->counting = 1;
    d->line_count = 0;
    formatted(flags, text, b);
    d->counting = 0;
    return d->line_count;
}

/* textwidth of an icon's text, for the error box's buttons */
int32_t wimp_icon_text_width(const struct wimp_window *win, uint32_t i)
{
    const uint8_t *ic = wimp_icon(win, i);
    window_colours(win);
    uint32_t flags, data = ros_addr((void *)(ic + 20));
    memcpy(&flags, ic + 16, 4);
    flags = seticonptrs(flags, data);
    uint32_t text = (flags & IF_INDIRECT) ? ros_ld32(data) : data;
    int32_t th;
    return textwidth(flags, text, &th);
}
