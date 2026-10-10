/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* deskclock.c implements DeskClock, an analogue clock on the icon bar.
 *
 * It draws as DeskMeter does. It writes pixels straight into the image of a
 * 4 bpp sprite. RISC OS packs these pixels with pixel 0 in a byte's low
 * nibble. Each row is padded to a whole word, and row 0 is the top. DeskClock
 * never switches the VDU's output to the sprite. That switch stays open
 * across SWIs, so whatever runs between them would be drawn into the image.
 *
 * The sprite is old-format, in the mode that matches the screen's pixel size
 * (as !Alarm's CASE chooses it), and 34 pixels square. It has a palette of
 * the style's colours. It also has a mask, which SpriteOp 29 makes with the
 * depth and layout of the image, and the round face is shaped into it.
 *
 * The face has a rim, twelve marks (or four dots), hour, minute and second
 * hands and a hub. The fifth style shows the time as seven-segment digits
 * instead. The time is the string that OS_Word 14,0 returns. DeskClock takes
 * a sample each time Wimp_PollIdle returns at the next second. SELECT cycles
 * through the five styles, ADJUST cycles back, and MENU offers the same
 * choice and Quit.
 */
#include <math.h>
#include <string.h>

#include "deskclock.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

/* ---- constants ------------------------------------------------------------- */

#define TASK_WORD   0x4B534154u        /* "TASK" */
#define ICONBAR     0xFFFFFFFEu        /* -2, the icon bar's window */
#define PRIORITY    0x00000000u        /* WimpPriority_Apps */

#define SPRITE_W    34u                /* px: the round face */
#define SPRITE_H    34u
#define AREA_SIZE   4096u

/* offsets of words in a sprite header: width, height, image and mask */
#define SP_WIDTH  16u
#define SP_HEIGHT 20u
#define SP_IMAGE  32u
#define SP_TRANS  36u

#define M_HEADERSIZE 28u
#define MI_SIZE      24u

#define NULL_REASON    0u
#define MOUSE_CLICK    6u
#define MENU_SELECTION 9u
#define USER_MESSAGE   17u
#define USER_MESSAGE_RECORDED 18u
#define MESSAGE_QUIT   0u

#define BUTTON_SELECT 4u
#define BUTTON_ADJUST 1u
#define BUTTON_MENU   2u



/* A face style. Every colour is &00BBGGRR. */
struct style {
    uint32_t face, hour, minute, second;
    uint32_t centre, tick, rim, inner;
    uint8_t dots;        /* dots at 12, 3, 6 and 9 instead of twelve marks */
    uint8_t digital;     /* the time as digits */
};
static const struct style styles[] = {
    { 0x00E0F2F8u, 0x00202020u, 0x00A08040u, 0x002020C0u,   /* Classic: ivory */
      0x00202020u, 0x00202020u, 0x00201A10u, 0x00C0A070u, 0, 0 },
    { 0x001F1410u, 0x00F0F0F0u, 0x00E0D0B0u, 0x002A8AFFu,   /* Night: navy */
      0x00F0F0F0u, 0x00CFF4C4u, 0x00443C2Eu, 0x00A8C56Fu, 0, 0 },
    { 0x000E1014u, 0x00E5FF00u, 0x007F3DFFu, 0x004DE9FFu,   /* Deco: neon */
      0x00FFFFFFu, 0x00E5FF00u, 0x007F3DFFu, 0x00FFE500u, 0, 0 },
    { 0x00EEF6EAu, 0x002B3124u, 0x007B8A7Bu, 0x004D67E8u,   /* Mint */
      0x002B3124u, 0x002F6B4Fu, 0x002F6B4Fu, 0x00BFDCCBu, 1, 0 },
    { 0x00181212u, 0x002020F0u, 0x002020F0u, 0x002020F0u,   /* Digital */
      0x002020F0u, 0x00504040u, 0x00504040u, 0x00504040u, 1, 1 },
};
#define STYLES (sizeof styles / sizeof styles[0])

/* ---- the workspace --------------------------------------------------------- */

struct ws {
    uint32_t task_handle;
    uint32_t pollword;

    int32_t icon_handle;
    uint32_t sprite_area;
    uint32_t sprite;
    uint8_t style;               /* the face's style, cycled by a click */
    uint8_t mode_changed;

    int32_t was_swi;                       /* the second the face shows */

    uint32_t poll[64];
    uint32_t state[64];
    uint32_t messages[2];
    uint32_t menu[(M_HEADERSIZE + 2 * MI_SIZE) / 4];
    uint8_t vdu[8];
    char name[8];                          /* the sprite name, "clock" */
    char task_name[12];
    char command[12];
};

static uint32_t A(const void *p)
{
    return ros_addr(p);
}

static os_error *swi(uint32_t number, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

static struct ws *workspace(struct ros_module *m)
{
    uint32_t pw = ros_ld32(m->private_word);
    return (int32_t)pw > 0 ? ros_ptr(pw) : NULL;
}

static void shutdown(struct ros_module *m, struct ws *w);
static void sample_and_draw(struct ws *w);

/* ---- the sprite: DeskClock's code with the sizes changed ------------------- */

/* Frees the sprite. VDU output is never switched into the sprite (its pixels
 * are written straight into the image), so this function switches nothing.
 * An OS_SpriteOp 60 to the screen with R3 = 0 used to reset the screen's VDU
 * state (its graphics window and origin) and leave it no save area. Every
 * later switch back, including Font_Paint's own, then reset it again in the
 * middle of whatever redraw was going on. After each mode change, which
 * rebuilds this sprite, other tasks' windows drew torn (#92). */
static void sprite_free(struct ws *w)
{
    if (!w->sprite_area)
        return;
    xos_module_free(ros_ptr(w->sprite_area));
    w->sprite_area = w->sprite = 0;
}

static uint32_t palette_of(const struct style *st, int i)
{
    static uint32_t pal[16];
    pal[0] = st->face, pal[1] = st->hour, pal[2] = st->minute,
    pal[3] = st->second, pal[4] = st->centre, pal[5] = st->tick,
    pal[6] = st->rim, pal[7] = st->inner;
    for (int j = 0; j < 8; j++)
        pal[8 + j] = (st->face >> 1 & 0x007F7F7Fu) +
                     (pal[j] >> 1 & 0x007F7F7Fu);      /* half-way blends with the background */
    return pal[i];
}


static void sprite_palette(struct ws *w)
{
    const struct style *st = &styles[w->style % STYLES];
    for (int i = 0; i < 16; i++) {
        uint32_t c = palette_of(st, i);
        uint32_t word = c << 8;                 /* &BBGGRR00, the form of a palette entry */
        ros_st32(w->sprite + 44 + i * 8, word);
        ros_st32(w->sprite + 44 + i * 8 + 4, word);
    }
}

/* The image's rows. Each row is padded to a whole word, which is the stride
 * the kernel uses for a 4 bpp sprite's rows. The pixels are packed from the
 * low end of each byte, as RISC OS packs them (pixel 0 in bits 0-3). Row 0 is
 * the top. */
#define ROW_BYTES (((SPRITE_W + 1) / 2) + 3 & ~3u)

static uint8_t *image_row(struct ws *w, uint32_t y)
{
    return ros_ptr(w->sprite + ros_ld32(w->sprite + SP_IMAGE) + y * ROW_BYTES);
}

static void put_px(struct ws *w, uint32_t x, uint32_t y, uint8_t idx)
{
    if (x >= SPRITE_W || y >= SPRITE_H)
        return;                                   /* never write outside the image */
    uint8_t *b = image_row(w, y) + (x >> 1);
    *b = x & 1 ? (*b & 0x0F) | idx << 4 : (*b & 0xF0) | idx;
}

/* Draws a small square of one palette entry. The rim, marks and hands are made of these. */
static void block(struct ws *w, double cx, double cy, double half, uint8_t idx)
{
    for (int32_t y = (int32_t)lround(cy - half); y <= (int32_t)lround(cy + half); y++)
        for (int32_t x = (int32_t)lround(cx - half); x <= (int32_t)lround(cx + half); x++)
            put_px(w, (uint32_t)x, (uint32_t)y, idx);
}

/* Clears a pixel of the mask, making it transparent. */
static void mask_px(uint8_t *msk, uint32_t stride, uint32_t x, uint32_t y)
{
    msk[y * stride + (x >> 1)] &= x & 1 ? 0x0F : 0xF0;
}

/* Makes the mask. An old-format sprite's mask has the same layout as its
 * image: the same depth and the same rows, with each pixel all ones where the
 * sprite shows and all zeros where it does not (PRM 1-781). The mask lies
 * inside the sprite and counts in its size. SpriteOp 29 makes such a mask with
 * every pixel solid, and this function then clears the shape into it. */
static os_error *sprite_make_mask(struct ws *w)
{
    uint32_t r[8] = { 0x100 | 29, w->sprite_area, A(w->name) };
    os_error *e = swi(XOS_SpriteOp, r);
    if (e)
        return e;
    r[0] = 0x100 | 24, r[1] = w->sprite_area, r[2] = A(w->name);   /* the sprite may have moved */
    if ((e = swi(XOS_SpriteOp, r)) != NULL)
        return e;
    uint32_t sp = w->sprite = r[2];
    uint32_t stride = (ros_ld32(sp + SP_WIDTH) + 1) * 4;
    uint32_t rows = ros_ld32(sp + SP_HEIGHT) + 1;
    uint8_t *msk = ros_ptr(sp + ros_ld32(sp + SP_TRANS));
    int32_t rad = (int32_t)((SPRITE_W - 1) / 2);
    double rr = (double)rad * rad + 0.28;        /* a hair inside */
    for (uint32_t row = 0; row < rows; row++) {
        double dy = (double)row - rad;
        double under = rr - dy * dy;
        int32_t half = under > 0 ? (int32_t)(sqrt(under) + 0.5) : -1;
        for (uint32_t px = 0; px < SPRITE_W; px++)
            if (!((int32_t)px >= rad - half && (int32_t)px <= rad + half))
                mask_px(msk, stride, px, row);
    }
    return NULL;
}

static os_error *sprite_init(struct ws *w)
{
    sprite_free(w);
    uint32_t mode = 0, xeig = 1, yeig = 1;
    {
        uint32_t r[8] = { 1 };
        if (!swi(XOS_ScreenMode, r))
            mode = r[1];
        uint32_t v[8] = { mode, 4 };
        if (!swi(XOS_ReadModeVariable, v))
            xeig = v[2];
        v[1] = 5;
        if (!swi(XOS_ReadModeVariable, v))
            yeig = v[2];
    }
    uint32_t sprite_mode;                    /* as Alarm's CASE, by pixel size */
    if (xeig == 2 && yeig == 2)
        sprite_mode = 9;                     /* 45 x 45 dpi */
    else if (xeig == 1 && yeig == 1)
        sprite_mode = 27;                    /* 90 x 90 dpi */
    else if (xeig == 1 && yeig == 2)
        sprite_mode = 12;                    /* 90 x 45 dpi */
    else
        sprite_mode = 3u << 27 | (180u >> xeig) << 1 | (180u >> yeig) << 14 | 1u;

    void *area;
    if (xos_module_claim(AREA_SIZE, &area))
        return ros_error(0x101, "No room in RMA");
    w->sprite_area = A(area);
    memset(area, 0, AREA_SIZE);
    /* Write the area's four header words, as the kernel's SpriteOp 9 does. */
    ros_st32(w->sprite_area, AREA_SIZE);
    ros_st32(w->sprite_area + 4, 0);
    ros_st32(w->sprite_area + 8, 16);
    ros_st32(w->sprite_area + 12, 16);
    {
        uint32_t r[8] = { 0x100 | 9, w->sprite_area, AREA_SIZE, 1 };
        os_error *e = swi(XOS_SpriteOp, r);
        if (e)
            return e;
    }
    {
        uint32_t r[8] = { 0x100 | 15, w->sprite_area, A(w->name), 1,
                          SPRITE_W, SPRITE_H, sprite_mode };
        os_error *e = swi(XOS_SpriteOp, r);
        if (e)
            return e;
    }
    {
        uint32_t r[8] = { 0x100 | 24, w->sprite_area, A(w->name) };
        os_error *e = swi(XOS_SpriteOp, r);
        if (e)
            return e;
        w->sprite = r[2];
    }
    sprite_palette(w);
    os_error *e = sprite_make_mask(w);
    if (e)
        return e;
    return NULL;
}

/* ---- the face, written pixel by pixel into the sprite ------------------------ */

/* Draws the digital face: seven-segment digits in pixel space, in palette entry 1. */
static void digital_face(struct ws *w, uint32_t h, uint32_t m)
{
    static const uint8_t segs[10] = {             /* a b c d e f g */
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    uint32_t digits[4] = { h / 10, h % 10, m / 10, m % 10 };
    int32_t wpx = 5, hpx = 9, gapx = 3;
    int32_t total = 4 * wpx + 3 * gapx;
    int32_t x0 = ((int32_t)SPRITE_W - total) / 2, y0 = ((int32_t)SPRITE_H - hpx) / 2;
    for (int d = 0; d < 4; d++) {
        int32_t dx = x0 + d * (wpx + gapx);
        uint8_t on = segs[digits[d]];
        for (int32_t yy = 0; yy < hpx; yy++)
            for (int32_t xx = 0; xx < wpx; xx++) {
                int seg = -1;
                if (yy == 0) seg = 0;
                else if (yy < hpx / 2) seg = xx == 0 ? 5 : xx == wpx - 1 ? 1 : -1;
                else if (yy == hpx / 2) seg = 6;
                else seg = xx == 0 ? 4 : xx == wpx - 1 ? 2 : -1;
                if (yy == hpx - 1) seg = 3;
                if (seg >= 0 && (on & (1u << seg)))
                    put_px(w, (uint32_t)(dx + xx), (uint32_t)(y0 + yy), 1);
            }
    }
    int32_t kx = x0 + 2 * wpx + gapx + gapx / 2;  /* the colon, mid-gap */
    put_px(w, (uint32_t)kx, (uint32_t)(y0 + hpx / 2 - 2), 1);
    put_px(w, (uint32_t)kx, (uint32_t)(y0 + hpx / 2 + 2), 1);
}

/* Draws the face for hour h, minute m and second s. It is made of loops of
 * blocks, as the meter's bars are. The rim is a ring, and there are twelve
 * bold marks (or dots). The hour and minute hands are thick strokes. The
 * second hand is thin and has a counterweight. The hub is drawn last.
 * Palette entries: 0 face, 1 hour and digits, 2 minute, 3 second, 4 centre,
 * 5 tick, 6 rim, 7 inner. */
static void draw_face(struct ws *w, uint32_t h, uint32_t m, uint32_t s)
{
    const struct style *st = &styles[w->style % STYLES];
    double c = (SPRITE_W - 1) / 2.0;
    double r = c - 1;
    double ha = ((h % 12) + m / 60.0 + s / 3600.0) / 12.0 * 2 * M_PI;
    double ma = (m + s / 60.0) / 60.0 * 2 * M_PI;
    double sa = s / 60.0 * 2 * M_PI;

    for (uint32_t y = 0; y < SPRITE_H; y++)
        memset(image_row(w, y), 0, ROW_BYTES);    /* the face, entry 0 */

    if (st->digital) {
        digital_face(w, h, m);
        return;
    }

    for (double a = 0; a < 2 * M_PI; a += 0.03)   /* the rim, a ring */
        block(w, c + (r - 0.6) * sin(a), c - (r - 0.6) * cos(a), 0.7, 6);
    if (st->dots) {
        for (int i = 0; i < 4; i++)
            block(w, c + (r - 4.0) * sin(i * M_PI / 2),
                  c - (r - 4.0) * cos(i * M_PI / 2), 1.2, 5);
    } else {
        for (int i = 0; i < 12; i++) {
            double a = i / 12.0 * 2 * M_PI;
            int major = i % 3 == 0;
            for (double t = r - (major ? 5.5 : 4.5); t < r - 2.0; t += 0.5)
                block(w, c + t * sin(a), c - t * cos(a), major ? 0.7 : 0.4,
                      major ? 6 : 5);
        }
    }

    for (double t = 0; t < r - 7.0; t += 0.5)     /* the hour, fat */
        block(w, c + t * sin(ha), c - t * cos(ha), 1.2, 1);
    for (double t = 0; t < r - 3.5; t += 0.5)     /* the minute, lean */
        block(w, c + t * sin(ma), c - t * cos(ma), 0.8, 2);
    for (double t = -3.5; t < r - 3.0; t += 0.5)  /* the second, thin, tailed */
        block(w, c + t * sin(sa), c - t * cos(sa), 0.4, 3);
    block(w, c, c, 1.0, 4);                       /* the hub */
}

/* Samples the time, and redraws when the second has moved. */
static void sample_and_draw(struct ws *w)
{
    uint32_t h = 0, m = 0, s = 0;
    {
        ((uint8_t *)w->poll)[0] = 0;              /* reason 0: the string */
        uint32_t r[8] = { 14, A(w->poll) };
        if (swi(XOS_Word, r))
            return;
        const uint8_t *t = (const uint8_t *)w->poll;
        if (t[24] != 13)
            return;                               /* not the string that was expected */
        h = (t[16] - '0') * 10 + t[17] - '0';
        m = (t[19] - '0') * 10 + t[20] - '0';
        s = (t[22] - '0') * 10 + t[23] - '0';
    }
    if ((int32_t)s == w->was_swi)                 /* the second shown */
        return;
    w->was_swi = s;
    draw_face(w, h, m, s);

    uint32_t *b = w->state;                       /* the icon replotted */
    b[0] = ICONBAR, b[1] = (uint32_t)w->icon_handle, b[2] = 0, b[3] = 0;
    uint32_t q[8] = { 0, A(b) };
    swi(XWimp_SetIconState, q);
}

/* ---- the icon ---------------------------------------------------------------- */

static os_error *icon_init(struct ws *w)
{
    uint32_t mode = 0, xeig = 1, yeig = 1;
    uint32_t r[8] = { 1 };
    if (!swi(XOS_ScreenMode, r))
        mode = r[1];
    r[0] = mode, r[1] = 4;
    if (!swi(XOS_ReadModeVariable, r))
        xeig = r[2];
    r[0] = mode, r[1] = 5;
    if (!swi(XOS_ReadModeVariable, r))
        yeig = r[2];

    uint32_t *b = w->state;
    b[0] = 0xFFFFFFF8u;                           /* -8: on the right, scanning from the right */
    b[1] = 0, b[2] = 0;
    b[3] = SPRITE_W << xeig, b[4] = SPRITE_H << yeig;
    b[5] = 0x07003112u;                           /* Alarm's flags */
    b[6] = A(w->name);                            /* the sprite's name */
    b[7] = w->sprite_area;                        /* DeskClock's sprite area */
    b[8] = 12;
    uint32_t c[8] = { PRIORITY, A(b) };
    os_error *e = swi(XWimp_CreateIcon, c);
    w->icon_handle = e ? 0 : (int32_t)c[0];
    return e;
}

static void icon_delete(struct ws *w)
{
    if (!w->icon_handle)
        return;
    uint32_t *b = w->state;
    b[0] = ICONBAR, b[1] = (uint32_t)w->icon_handle;
    uint32_t r[8] = { 0, A(b) };
    swi(XWimp_DeleteIcon, r);
    w->icon_handle = 0;
}

static void clock_remake(struct ws *w)
{
    icon_delete(w);
    sprite_init(w);
    icon_init(w);
    w->mode_changed = 0;
    sample_and_draw(w);
}

/* ---- the menu ----------------------------------------------------------------- */

enum { MENU_METRIC, MENU_QUIT, MENU_ITEMS };

static void menu_item(struct ws *w, uint32_t k, const char *text, int last)
{
    uint32_t *item = &w->menu[(M_HEADERSIZE + k * MI_SIZE) / 4];
    item[0] = last ? 0x80u : 0;
    item[1] = 0xFFFFFFFFu;
    item[2] = 0x07000021u;
    memset(&item[3], 0, 12);
    memcpy(&item[3], text, strlen(text));
}

static void menu_show(struct ws *w, uint32_t x)
{
    memset(w->menu, 0, M_HEADERSIZE);
    memcpy(w->menu, "DeskClock", 9);
    ((uint8_t *)w->menu)[12] = 7;
    ((uint8_t *)w->menu)[13] = 2;
    ((uint8_t *)w->menu)[14] = 7;
    ((uint8_t *)w->menu)[15] = 0;
    w->menu[4] = 9 * 16 + 16;
    w->menu[5] = 44;
    w->menu[6] = 0;
    menu_item(w, MENU_METRIC, "Next style", 0);
    menu_item(w, MENU_QUIT, "Quit", 1);
    uint32_t r[8] = { 0, A(w->menu), x - 64, 96 + MENU_ITEMS * 44 };
    swi(XWimp_CreateMenu, r);
}

/* ---- the task ------------------------------------------------------------------ */

static void run(struct ros_module *m, struct ws *w)
{
    strcpy(w->name, "clock");
    strcpy(w->task_name, "DeskClock");
    w->messages[0] = MESSAGE_QUIT;
    w->messages[1] = 0;
    os_error *e;
    {
        uint32_t r[8] = { 310, TASK_WORD, A(w->task_name), A(w->messages) };
        e = swi(XWimp_Initialise, r);
        if (!e)
            w->task_handle = r[1];
    }
    if (!e)
        e = sprite_init(w);
    if (!e)
        e = icon_init(w);
    if (e) {
        uint32_t r[8] = { A(e), 2, A(w->task_name) };
        swi(XWimp_ReportError, r);
        shutdown(m, w);
        ros_st32(m->private_word, 0xFFFFFFFFu);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        ros_swi(&s, OS_Exit);
        return;
    }
    w->was_swi = -1;                              /* nothing drawn yet */
    sample_and_draw(w);
    uint32_t deadline = 0;
    {
        uint32_t t[8] = { 0 };
        if (!swi(XOS_ReadMonotonicTime, t))
            deadline = t[0] + 100;
    }

    for (;;) {
        uint32_t p[8] = { 0x30 | (3u << 22), A(w->poll), deadline, A(&w->pollword) };
        e = swi(XWimp_PollIdle, p);
        if (e) {
            uint32_t r[8] = { A(e), 1, A(w->task_name) };
            swi(XWimp_ReportError, r);
            continue;
        }
        switch (p[0]) {
        case NULL_REASON:
            if (w->mode_changed)
                clock_remake(w);
            else
                sample_and_draw(w);
            {
                uint32_t t[8] = { 0 };
                if (!swi(XOS_ReadMonotonicTime, t))
                    deadline = t[0] + 100;
            }
            break;
        case MOUSE_CLICK:
            if (w->poll[3] == ICONBAR && w->poll[4] == (uint32_t)w->icon_handle) {
                if (w->poll[2] & (BUTTON_SELECT | BUTTON_ADJUST)) {
                    int step = w->poll[2] & BUTTON_ADJUST ? -1 : 1;
                    w->style = (uint8_t)((w->style + STYLES + step) % STYLES);
                    sprite_palette(w);
                    w->was_swi = -1;              /* redraw at once */
                    sample_and_draw(w);
                } else if (w->poll[2] & BUTTON_MENU) {
                    menu_show(w, w->poll[0]);
                }
            }
            break;
        case MENU_SELECTION:
            if (w->poll[0] == MENU_METRIC) {
                w->style = (uint8_t)((w->style + 1) % STYLES);
                sprite_palette(w);
                w->was_swi = -1;
                sample_and_draw(w);
            } else if (w->poll[0] == MENU_QUIT)
                goto quit;
            break;
        case USER_MESSAGE:
        case USER_MESSAGE_RECORDED:
            if (w->poll[4] == MESSAGE_QUIT)
                goto quit;
            break;
        default:
            break;
        }
        continue;
    quit:
        shutdown(m, w);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        ros_swi(&s, OS_Exit);
        return;
    }
}

/* ---- the module ----------------------------------------------------------------- */

static void shutdown(struct ros_module *m, struct ws *w)
{
    if (w->task_handle) {
        uint32_t r[8] = { w->task_handle, TASK_WORD };
        swi(XWimp_CloseDown, r);
        w->task_handle = 0;
    }
    icon_delete(w);
    sprite_free(w);
    ros_st32(m->private_word, 0);
    xos_module_free(w);
}

#define SERVICE_RESET      0x27u
#define SERVICE_MODECHANGE 0x46u
#define SERVICE_STARTWIMP  0x49u
#define SERVICE_STARTEDWIMP 0x4Au

int deskclock_autostart = -1;

static void service(struct ros_module *m, struct ros_cpu *s)
{
    struct ws *w = workspace(m);
    switch (s->r[1]) {
    case SERVICE_STARTWIMP:
        if (w || !(deskclock_autostart >= 0 ? deskclock_autostart : ros_cmdline_has("rosgd.deskclock")))
            return;
        {
            void *block;
            if (xos_module_claim(sizeof *w, &block)) {
                s->r[1] = 0;
                return;
            }
            w = block;
            memset(w, 0, sizeof *w);
            ros_st32(m->private_word, A(w));
            strcpy(w->command, "DeskClock");
            s->r[0] = A(w->command);
            s->r[1] = 0;
        }
        return;
    case SERVICE_STARTEDWIMP:
        if (ros_ld32(m->private_word) == 0xFFFFFFFFu)
            ros_st32(m->private_word, 0);
        return;
    case SERVICE_RESET:
        if (!w) {
            if (ros_ld32(m->private_word) == 0xFFFFFFFFu)
                ros_st32(m->private_word, 0);
            return;
        }
        w->task_handle = 0;
        shutdown(m, w);
        return;
    case SERVICE_MODECHANGE:
        if (w && w->task_handle)
            w->mode_changed = 1;
        return;
    default:
        return;
    }
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct ws *w = workspace(m);
    if (w)
        shutdown(m, w);
    return NULL;
}

static void start(struct ros_module *m, uint32_t tail)
{
    (void)tail;
    struct ws *w = workspace(m);
    if (w && !w->task_handle) {
        run(m, w);
        return;
    }
    struct ros_cpu s;                   /* The error is made in the arena.
                                           One in /init's own memory would stop
                                           the box (ros_addr). */
    ros_cpu_enter(&s);
    s.r[0] = A(ros_error(0xC6, "Already running"));
    ros_swi(&s, OS_GenerateError);
}

static os_error *cmd_deskmeter(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)tail, (void)argc;
    if (!workspace(m)) {
        return ros_error(0xC6, "Not started");
    }
    uint32_t r[8] = { 2, m->base + ros_ld32(m->base + 0x10), 0 };
    return swi(XOS_Module, r);
}

static const struct ros_command commands[] = {
    { "DeskClock", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *DeskClock",
      "Runs the Desk Clock on the icon bar.\r"
      "SELECT or ADJUST changes the face; MENU offers more.\r",
      cmd_deskmeter },
    { 0 },
};

struct ros_module deskclock_module = {
    .title = "DeskClock",
    .help = "DeskClock\t1.10 (29-Sep-26) ROSGD native",
    .init = NULL,
    .final = final,
    .service = service,
    .start = start,
    .commands = commands,
};
