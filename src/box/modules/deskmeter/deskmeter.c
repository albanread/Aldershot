/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* deskmeter.c implements DeskMeter. It shows how busy the Wimp is as a graph
 * on the icon bar.
 *
 * This is the box's fourth native desktop module, and it follows DeskClock's
 * code throughout (which in turn follows !Alarm's pipeline). The icon is a
 * 4 bpp sprite of 52 x 34 pixels, the size of an application's icon on the
 * icon bar, in an old-format mode number that matches the screen's pixel
 * size. The sprite has a palette of the style's colours and a mask.
 * DeskMeter draws the graph by writing its pixels straight into the sprite's
 * image; it does not switch VDU output into the sprite (see sprite_free and
 * #92). It addresses the icon by name and gives it Alarm's flags.
 *
 * DeskMeter graphs the runtime's own busyness counters (runtime/meter.c). It
 * reads them once a second through OS_ReadSysInfo 100, which returns now_ns,
 * swi_ns and idle_ns, and works out the busyness over each interval from
 * them. Busy is the complement of the time that the box spent idle (with
 * nothing to do, in the wait that Portable_Idle sits in). In SWIs is the time
 * spent inside SWIs. The third metric gives half the graph to each of these.
 * SELECT cycles through the metrics, ADJUST cycles back, and MENU offers the
 * same choice and Quit. DeskMeter takes a sample each time Wimp_PollIdle
 * returns at the next second, as DeskClock does.
 *
 * On a box that runs on the HAL (the hardware abstraction layer) there is more
 * to see, and there are two more metrics. Cores
 * shows how busy the processors are (the time not idle, across all of them
 * together). HAL shows the share of their time spent in the HAL or waiting for
 * its lock. Both come from the HAL's own counters (HAL_SYS_STATS,
 * ros_hal_stats). MENU then also offers "HAL...", which opens a window with a
 * graph for each core. The graph covers the last minute in three parts (the
 * box running, the HAL, and the wait for its lock). The window also shows what
 * the HAL did each second: calls, thread switches, wakes, interrupts, faults,
 * signals, network traffic, share traffic, screen flushes, memory, processes
 * and threads. Linux has no such call, so on Linux DeskMeter behaves as it
 * always has.
 *
 * On both kinds of box, DeskMeter shows the ARM container (runtime/armrun) in
 * the same way. The ARM metric is the share of a core that ARM code took.
 * MENU's "ARM..." opens a window showing the last minute's instructions per
 * second and where ARM code's time went (the engine, the native code that it
 * called, and the FPA). The window also shows each second's SWIs and calls,
 * the ARM tasks and modules, and the code that runs most (from
 * ros_armrun_view, the counters behind *ARMStats).
 *
 * The graph keeps twenty-four seconds of history in the workspace and redraws
 * all of it each second. The first version scrolled the pixels by hand and
 * garbled the graph, because it misread the word padding at the end of each
 * row. Redrawing the whole graph from the history avoids that.
 */
#include <stdio.h>
#include <string.h>

#include "deskmeter.h"
#include "rosgd/api.h"
#include "rosgd/armbox.h"
#include "rosgd/meter.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

/* ---- constants ------------------------------------------------------------- */

#define TASK_WORD   0x4B534154u        /* "TASK" */
#define ICONBAR     0xFFFFFFFEu        /* -2, the icon bar's window handle */
#define PRIORITY    0x00000000u        /* WimpPriority_Apps */

#define SPRITE_W    52u                /* pixels: 24 bars, two pixels a second */
#define SPRITE_H    34u
#define HIST        24u                /* seconds of history kept */
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
#define REDRAW_WINDOW  1u
#define OPEN_WINDOW    2u
#define CLOSE_WINDOW   3u
#define USER_MESSAGE   17u
#define USER_MESSAGE_RECORDED 18u
#define MESSAGE_QUIT   0u

#define BUTTON_SELECT 4u
#define BUTTON_ADJUST 1u
#define BUTTON_MENU   2u

/* The metrics, which a click cycles through. The last two exist on the HAL only. */
enum { M_BUSY, M_INSWI, M_SPLIT, M_ARM, M_CORES, M_HAL, METRICS };

/* Layout of the HAL window in OS units: one row for each core, then the figures. */
#define HAL_HIST  60u                   /* seconds of history kept for each core */
#define HW_W      1520
#define HW_LABEL  16                    /* left of the label, such as "Core 0" */
#define HW_GX     176                   /* left edge of the graph */
#define HW_BAR    8                     /* width of one second's bar */
#define HW_GH     56                    /* height of the graph */
#define HW_ROW    76
#define HW_LINE   44
#define HW_LINES  5
#define HW_TOP    20

/* A style holds the graph's colours as &00BBGGRR, in DeskClock's eight fields. */
struct style {
    uint32_t face, hour, minute, second;   /* background, low, middle, high */
    uint32_t centre, tick, rim, inner;     /* unused, grid, frame, unused */
};
static const struct style styles[] = {
    { 0x00000A00u, 0x0020C020u, 0x0020C0E0u, 0x002040E0u,   /* Meter: like an oscilloscope */
      0x0020C020u, 0x00205020u, 0x0020C044u, 0x0020C020u },
    { 0x001E1414u, 0x0060C020u, 0x0020C0E0u, 0x004040E0u,   /* Dark */
      0x0060C020u, 0x00303030u, 0x00606060u, 0x0060C020u },
};
#define STYLES (sizeof styles / sizeof styles[0])

/* ---- the workspace --------------------------------------------------------- */

struct ws {
    uint32_t task_handle;
    uint32_t pollword;

    int32_t icon_handle;
    uint32_t sprite_area;
    uint32_t sprite;
    uint8_t style;
    uint8_t metric;
    uint8_t mode_changed;

    uint64_t was_now, was_swi, was_idle;   /* the counters one second ago */
    uint8_t hist[HIST];                    /* busy fractions, oldest first */

    uint8_t hal;                           /* the HAL answers, so show its metrics and window */
    uint8_t hal_open;
    uint8_t have_hal_sample;
    uint32_t hal_window;
    uint32_t cores_frac, hal_frac;         /* the last second's values, in thousandths */
    struct ros_hal_stats hs_was, hs_now;
    uint8_t core_box[ROS_HAL_STATS_CPUS][HAL_HIST];   /* in 255ths, oldest first */
    uint8_t core_hal[ROS_HAL_STATS_CPUS][HAL_HIST];
    uint8_t core_spin[ROS_HAL_STATS_CPUS][HAL_HIST];
    uint64_t rate[12];                     /* the last second's rates, per second */
    uint32_t wblock[24];                   /* block for Wimp_CreateWindow, then Wimp_OpenWindow */

    uint8_t arm_open, have_arm_sample;
    uint32_t arm_window;
    uint32_t arm_frac;                     /* ARM code's share of a core, in thousandths */
    uint64_t arm_was_ns;                   /* when the last sample was taken */
    struct ros_arm_view av_was, av_now;
    uint32_t arm_kips[HAL_HIST];           /* thousands of instructions a second, oldest first */
    uint8_t arm_eng[HAL_HIST], arm_nat[HAL_HIST], arm_fpa[HAL_HIST];   /* in 255ths of a core */
    uint64_t arm_rate[4];                  /* instructions, SWIs and calls a second; engine speed */
    uint8_t menu_ids[4];                   /* the menu's items, in the order shown */
    uint32_t redraw[16];
    char text[160];

    uint32_t poll[64];
    uint32_t state[64];
    uint32_t meter[8];                     /* block for OS_ReadSysInfo 100 */
    uint32_t messages[2];
    uint32_t menu[(M_HEADERSIZE + 4 * MI_SIZE) / 4];
    uint8_t vdu[8];
    char name[8];                          /* the sprite name, "meter" */
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

/* Frees the sprite. DeskMeter never switches VDU output into the sprite (it
 * writes the pixels straight into the image), so this function switches
 * nothing. An OS_SpriteOp 60 to the screen with R3 = 0 used to reset the
 * screen's VDU state (its graphics window and origin) and leave it no save
 * area, so every later switch back, including Font_Paint's own, reset it again
 * in the middle of whatever redraw was going on. After each mode change, which
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
    uint8_t *msk = ros_ptr(sp + ros_ld32(sp + SP_TRANS));
    static const uint8_t corner[][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
    for (uint32_t k = 0; k < sizeof corner / sizeof corner[0]; k++) {
        uint32_t x = corner[k][0], y = corner[k][1];
        mask_px(msk, stride, x, y);                             /* clear the corners */
        mask_px(msk, stride, SPRITE_W - 1 - x, y);
        mask_px(msk, stride, x, SPRITE_H - 1 - y);
        mask_px(msk, stride, SPRITE_W - 1 - x, SPRITE_H - 1 - y);
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
    memset(w->hist, 0, sizeof w->hist);
    return NULL;
}

/* ---- the graph, written pixel by pixel into the sprite -------------------- */

/* Draws the history as bars, writing the image pixel by pixel. There is a
 * frame two pixels deep and a dashed line at half height. Each second is a
 * bar two pixels wide: green below half, amber up to three quarters, and red
 * above that. Nothing here touches the VDU. The box's background work, which
 * includes the Wimp's caret flash, runs whenever a SWI exits; if output were
 * switched to the sprite at that moment, those bytes would go into the sprite
 * (this is how the clock came to find characters on its face). */
static void draw_graph(struct ws *w)
{
    for (uint32_t y = 0; y < SPRITE_H; y++) {
        memset(image_row(w, y), 0, ROW_BYTES);      /* background, palette entry 0 */
        for (uint32_t x = 0; x < SPRITE_W; x++) {
            int frame = y < 2 || y >= SPRITE_H - 2 || x < 2 || x >= SPRITE_W - 2;
            if (frame) {
                put_px(w, x, y, 6);                 /* frame, palette entry 6 */
                continue;
            }
            int in = y >= 3 && y < SPRITE_H - 3 && x >= 3 && x < SPRITE_W - 3;
            uint32_t gy = y - 3;                    /* rows down from the graph's top */
            uint32_t gh = SPRITE_H - 6;
            if (in && gy == gh / 2 && ((x - 3) & 3) < 2)
                put_px(w, x, y, 5);                 /* the dashed line at half height */
        }
    }
    int32_t bottom = 3, graph_h = (int32_t)SPRITE_H - 6;
    for (uint32_t i = 0; i < HIST; i++) {
        uint32_t frac = w->hist[i] * 1000u / 255u;
        if (!frac)
            continue;
        int32_t h = (int32_t)((graph_h * frac + 500) / 1000);
        uint8_t idx = frac < 500 ? 1 : frac < 750 ? 2 : 3;
        for (int32_t dy = 0; dy < h; dy++)
            for (uint32_t dx = 0; dx < 2; dx++) {
                uint32_t x = 4 + i * 2 + dx;
                if (x < SPRITE_W - 3)
                    put_px(w, x, SPRITE_H - 1 - (uint32_t)(bottom + dy), idx);   /* top is row 0 */
            }
    }
}

/* ---- the HAL's counters and its window ----------------------------------------- */

enum { R_CALLS, R_SWITCHES, R_WAKES, R_IRQS, R_FAULTS, R_SIGNALS, R_NET_IN, R_NET_OUT,
       R_SHARE_CALLS, R_SHARE_BYTES, R_FLUSHES, R_FLUSH_BYTES };

static unsigned cores(const struct ws *w)
{
    unsigned n = (unsigned)w->hs_now.ncpu;
    return n > ROS_HAL_STATS_CPUS ? ROS_HAL_STATS_CPUS : n ? n : 1;
}

static int32_t hal_height(const struct ws *w)
{
    return HW_TOP + (int32_t)cores(w) * HW_ROW + 16 + HW_LINES * HW_LINE + 12;
}

static uint8_t of255(uint64_t part, uint64_t whole)
{
    return (uint8_t)(part >= whole ? 255 : part * 255 / whole);
}

/* Takes one second's sample of the HAL's counters, and from it works out
 * each core's three parts, the two metrics and the rates. */
static void hal_sample(struct ws *w)
{
    if (!w->hal || ros_hal_stats(&w->hs_now))
        return;
    const struct ros_hal_stats *a = &w->hs_was, *b = &w->hs_now;
    uint64_t dt = b->now_ns - a->now_ns;
    if (!w->have_hal_sample || !dt) {
        w->hs_was = w->hs_now, w->have_hal_sample = 1;
        return;
    }
    uint64_t busy = 0, inhal = 0;
    for (unsigned c = 0; c < cores(w); c++) {
        uint64_t idle = b->cpu[c].idle_ns - a->cpu[c].idle_ns;
        uint64_t hal = b->cpu[c].hal_ns - a->cpu[c].hal_ns;
        uint64_t spin = b->cpu[c].spin_ns - a->cpu[c].spin_ns;
        if (idle > dt)
            idle = dt;
        uint64_t used = dt - idle;
        if (hal > used)
            hal = used;
        if (spin > used - hal)
            spin = used - hal;
        memmove(w->core_box[c], w->core_box[c] + 1, HAL_HIST - 1);
        memmove(w->core_hal[c], w->core_hal[c] + 1, HAL_HIST - 1);
        memmove(w->core_spin[c], w->core_spin[c] + 1, HAL_HIST - 1);
        w->core_box[c][HAL_HIST - 1] = of255(used - hal - spin, dt);
        w->core_hal[c][HAL_HIST - 1] = of255(hal, dt);
        w->core_spin[c][HAL_HIST - 1] = of255(spin, dt);
        busy += used, inhal += hal + spin;
    }
    uint64_t all = dt * cores(w);
    w->cores_frac = (uint32_t)(busy * 1000 / all);
    w->hal_frac = (uint32_t)(inhal * 1000 / all);
    const uint64_t d[12] = {
        b->syscalls - a->syscalls, 0, 0, b->irqs - a->irqs,     /* switches, wakes: see below */
        b->faults - a->faults, b->signals - a->signals, b->net_rx_bytes - a->net_rx_bytes,
        b->net_tx_bytes - a->net_tx_bytes, b->share_calls - a->share_calls,
        b->share_bytes - a->share_bytes, b->screen_flushes - a->screen_flushes,
        b->screen_bytes - a->screen_bytes };
    for (int i = 0; i < 12; i++)
        w->rate[i] = d[i] * 1000000000ull / dt;
    uint64_t sw = 0, wk = 0;
    for (unsigned c = 0; c < cores(w); c++) {
        sw += b->cpu[c].switches - a->cpu[c].switches;
        wk += b->cpu[c].wakes - a->cpu[c].wakes;
    }
    w->rate[R_SWITCHES] = sw * 1000000000ull / dt;
    w->rate[R_WAKES] = wk * 1000000000ull / dt;
    w->hs_was = w->hs_now;
}

static void gcol(uint32_t rgb)                 /* sets the foreground colour, &BBGGRR */
{
    uint32_t r[8] = { rgb << 8, 0, 0, 0, 0 };
    swi(XColourTrans_SetGCOL, r);
}

static void rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    uint32_t r[8] = { 4, (uint32_t)x0, (uint32_t)y0 };
    swi(XOS_Plot, r);
    r[0] = 96 + 5, r[1] = (uint32_t)(x1 - 1), r[2] = (uint32_t)(y1 - 1);
    swi(XOS_Plot, r);
}

static void text_at(struct ws *w, int32_t x, int32_t y, uint32_t rgb)
{
    uint32_t c[8] = { 0, rgb << 8, 0x10101000u };
    swi(XWimp_TextOp, c);
    uint32_t r[8] = { 2, A(w->text), 0, 0, (uint32_t)x, (uint32_t)y };   /* R4, R5: baseline's left end */
    swi(XWimp_TextOp, r);
}

/* Writes a byte rate, with its unit, into s. */
static void bytes(char *s, size_t n, uint64_t v)
{
    if (v >= 10u << 20)
        snprintf(s, n, "%llu MB/s", (unsigned long long)(v >> 20));
    else if (v >= 10u << 10)
        snprintf(s, n, "%llu KB/s", (unsigned long long)(v >> 10));
    else
        snprintf(s, n, "%llu B/s", (unsigned long long)v);
}

/* Draws the window's contents, with the work area's top left at (ox, oy). */
static void hal_contents(struct ws *w, int32_t ox, int32_t oy)
{
    int32_t h = hal_height(w);
    gcol(0x101010);
    rect(ox, oy - h, ox + HW_W, oy);
    unsigned n = cores(w);
    for (unsigned c = 0; c < n; c++) {
        int32_t top = oy - HW_TOP - (int32_t)c * HW_ROW;
        int32_t y0 = top - HW_GH, x0 = ox + HW_GX;
        gcol(0x202020);
        rect(x0 - 4, y0 - 4, x0 + (int32_t)HAL_HIST * HW_BAR + 4, top + 4);
        for (unsigned i = 0; i < HAL_HIST; i++) {
            int32_t x = x0 + (int32_t)i * HW_BAR;
            int32_t hb = w->core_box[c][i] * HW_GH / 255, hh = w->core_hal[c][i] * HW_GH / 255;
            int32_t hs = w->core_spin[c][i] * HW_GH / 255;
            gcol(0x20C020);
            rect(x, y0, x + HW_BAR - 2, y0 + hb);
            gcol(0x00B0F0);
            rect(x, y0 + hb, x + HW_BAR - 2, y0 + hb + hh);
            gcol(0x2040F0);
            rect(x, y0 + hb + hh, x + HW_BAR - 2, y0 + hb + hh + hs);
        }
        snprintf(w->text, sizeof w->text, "Core %u", c);
        text_at(w, ox + HW_LABEL, y0 + 12, 0xFFFFFF);
        unsigned box = w->core_box[c][HAL_HIST - 1] * 100u / 255u,
                 hal = w->core_hal[c][HAL_HIST - 1] * 100u / 255u,
                 spin = w->core_spin[c][HAL_HIST - 1] * 100u / 255u;
        snprintf(w->text, sizeof w->text, "box %u%%", box);
        text_at(w, x0 + (int32_t)HAL_HIST * HW_BAR + 24, y0 + 12, 0x20C020);
        snprintf(w->text, sizeof w->text, "HAL %u%%", hal);
        text_at(w, x0 + (int32_t)HAL_HIST * HW_BAR + 184, y0 + 12, 0x00B0F0);
        snprintf(w->text, sizeof w->text, "lock %u%%", spin);
        text_at(w, x0 + (int32_t)HAL_HIST * HW_BAR + 344, y0 + 12, 0x2040F0);
    }
    const struct ros_hal_stats *s = &w->hs_now;
    int32_t y = oy - HW_TOP - (int32_t)n * HW_ROW - 16 - HW_LINE + 12;
    char in[24], out[24], sh[24], fl[24];
    bytes(in, sizeof in, w->rate[R_NET_IN]);
    bytes(out, sizeof out, w->rate[R_NET_OUT]);
    bytes(sh, sizeof sh, w->rate[R_SHARE_BYTES]);
    bytes(fl, sizeof fl, w->rate[R_FLUSH_BYTES]);
    snprintf(w->text, sizeof w->text, "Each second: %llu calls, %llu thread switches, %llu wakes, %llu interrupts",
             (unsigned long long)w->rate[R_CALLS], (unsigned long long)w->rate[R_SWITCHES],
             (unsigned long long)w->rate[R_WAKES], (unsigned long long)w->rate[R_IRQS]);
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    snprintf(w->text, sizeof w->text, "%llu page faults, %llu signals; network in %s, out %s",
             (unsigned long long)w->rate[R_FAULTS], (unsigned long long)w->rate[R_SIGNALS], in, out);
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    snprintf(w->text, sizeof w->text, "Share (HostFS) %llu calls, %s; screen %llu flushes, %s",
             (unsigned long long)w->rate[R_SHARE_CALLS], sh, (unsigned long long)w->rate[R_FLUSHES], fl);
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    snprintf(w->text, sizeof w->text, "Memory %llu MB free of %llu MB; %llu process%s, %llu threads",
             (unsigned long long)(s->mem_free_pages >> 8), (unsigned long long)(s->mem_pages >> 8),
             (unsigned long long)s->processes, s->processes == 1 ? "" : "es", (unsigned long long)s->threads);
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    snprintf(w->text, sizeof w->text, "%u cores, %u%% busy, %u%% in the HAL.  Green the box, amber the HAL, red its lock",
             n, w->cores_frac / 10, w->hal_frac / 10);
    text_at(w, ox + HW_LABEL, y, 0xA0A0A0);
}

/* Draws the window's contents in a rectangle loop. If update is 0, the Wimp
 * has asked for a redraw, and this runs Wimp_RedrawWindow's loop. Otherwise it
 * runs Wimp_UpdateWindow's loop over the whole window, which draws each
 * second's new figures over the old ones without flicker. */
static void hal_redraw(struct ws *w, int update)
{
    uint32_t *b = w->redraw;
    b[0] = w->hal_window;
    b[1] = 0, b[2] = (uint32_t)-hal_height(w), b[3] = HW_W, b[4] = 0;
    uint32_t r[8] = { 0, A(b) };
    if (swi(update ? XWimp_UpdateWindow : XWimp_RedrawWindow, r))
        return;
    while (r[0]) {
        int32_t ox = (int32_t)b[1] - (int32_t)b[5], oy = (int32_t)b[4] - (int32_t)b[6];
        hal_contents(w, ox, oy);
        r[0] = 0, r[1] = A(b);
        if (swi(XWimp_GetRectangle, r))
            return;
    }
}

/* Opens one of DeskMeter's windows. The first time it is called, it creates
 * the window with the given title and work area width and height, and opens
 * it at x, y. After that it brings the window to the front. Returns 1 if the
 * window is open. */
static int window_open(struct ws *w, uint32_t *handle, const char *title, int32_t width, int32_t h,
                       int32_t x, int32_t y)
{
    uint32_t *b = w->wblock;
    if (!*handle) {
        memset(b, 0, sizeof w->wblock);
        b[0] = (uint32_t)x, b[1] = (uint32_t)y, b[2] = (uint32_t)(x + width), b[3] = (uint32_t)(y + h);
        b[4] = 0, b[5] = 0, b[6] = 0xFFFFFFFFu;
        b[7] = 0x87000002u;                     /* movable; back, close and title icons */
        b[8] = 0x01070207u;                     /* title colours 7 on 2; work area 7 on 1 */
        b[9] = 0x000C0103u;                     /* scroll bar colours 3, 1; input focus 12 */
        b[10] = 0, b[11] = (uint32_t)-h, b[12] = (uint32_t)width, b[13] = 0;
        b[14] = 0x07000019u;                    /* title flags: text, centred */
        b[15] = 0;                              /* the work area ignores clicks */
        b[16] = 1;                              /* sprite area 1: the Wimp's sprites */
        b[17] = 0;
        strncpy((char *)&b[18], title, 11);
        b[21] = 0;                              /* no icons */
        uint32_t r[8] = { 0, A(b) };
        if (swi(XWimp_CreateWindow, r))
            return 0;
        *handle = r[0];
        b[0] = *handle;
        b[1] = (uint32_t)x, b[2] = (uint32_t)y, b[3] = (uint32_t)(x + width), b[4] = (uint32_t)(y + h);
        b[5] = 0, b[6] = 0, b[7] = 0xFFFFFFFFu;
    } else {
        b[0] = *handle;
        uint32_t r[8] = { 0, A(b) };
        if (swi(XWimp_GetWindowState, r))
            return 0;
        b[7] = 0xFFFFFFFFu;                     /* to the front */
    }
    uint32_t r[8] = { 0, A(b) };
    return !swi(XWimp_OpenWindow, r);
}

static void hal_open(struct ws *w)
{
    if (window_open(w, &w->hal_window, "HAL", HW_W, hal_height(w), 240, 400))
        w->hal_open = 1;
}

/* ---- the ARM container's window ------------------------------------------------ */

#define AW_GH    64                             /* height of each graph */
#define AW_ROW   88
#define AW_LINES 6

static int32_t arm_height(void)
{
    return HW_TOP + 2 * AW_ROW + 16 + AW_LINES * HW_LINE + 12;
}

/* Takes one second's sample of the ARM container's counters. */
static void arm_sample(struct ws *w)
{
    uint64_t now = ros_meter_now_ns();
    ros_armrun_view(&w->av_now);
    uint64_t dt = now - w->arm_was_ns;
    if (!w->have_arm_sample || !dt) {
        w->av_was = w->av_now, w->arm_was_ns = now, w->have_arm_sample = 1;
        return;
    }
    const struct ros_arm_view *a = &w->av_was, *b = &w->av_now;
    uint64_t ins = b->instructions - a->instructions, eng = b->ns_engine - a->ns_engine;
    uint64_t nat = b->ns_native - a->ns_native, fpa = b->ns_fpa - a->ns_fpa;
    w->arm_rate[0] = ins * 1000000000ull / dt;
    w->arm_rate[1] = (b->swis - a->swis) * 1000000000ull / dt;
    w->arm_rate[2] = (b->calls - a->calls) * 1000000000ull / dt;
    w->arm_rate[3] = eng ? ins * 1000ull / eng : 0;                     /* millions a second of engine time */
    uint64_t all = eng + nat + fpa;
    w->arm_frac = (uint32_t)(all >= dt ? 1000 : all * 1000 / dt);
    memmove(w->arm_kips, w->arm_kips + 1, (HAL_HIST - 1) * sizeof w->arm_kips[0]);
    memmove(w->arm_eng, w->arm_eng + 1, HAL_HIST - 1);
    memmove(w->arm_nat, w->arm_nat + 1, HAL_HIST - 1);
    memmove(w->arm_fpa, w->arm_fpa + 1, HAL_HIST - 1);
    uint64_t kips = w->arm_rate[0] / 1000;
    w->arm_kips[HAL_HIST - 1] = kips > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)kips;
    w->arm_eng[HAL_HIST - 1] = of255(eng, dt);
    w->arm_nat[HAL_HIST - 1] = of255(nat, dt);
    w->arm_fpa[HAL_HIST - 1] = of255(fpa, dt);
    w->av_was = w->av_now, w->arm_was_ns = now;
}

/* Writes a count with its thousands separated by commas, as 12,345,678. */
static void commas(char *s, size_t n, uint64_t v)
{
    char t[32];
    int k = snprintf(t, sizeof t, "%llu", (unsigned long long)v), o = 0;
    for (int i = 0; i < k && (size_t)o < n - 1; i++) {
        if (i && (k - i) % 3 == 0 && (size_t)o < n - 2)
            s[o++] = ',';
        s[o++] = t[i];
    }
    s[o] = 0;
}

static void arm_contents(struct ws *w, int32_t ox, int32_t oy)
{
    int32_t h = arm_height();
    gcol(0x101010);
    rect(ox, oy - h, ox + HW_W, oy);
    int32_t x0 = ox + HW_GX, right = x0 + (int32_t)HAL_HIST * HW_BAR;
    uint32_t peak = 1;
    for (unsigned i = 0; i < HAL_HIST; i++)
        if (w->arm_kips[i] > peak)
            peak = w->arm_kips[i];
    /* Instructions a second, scaled to the peak over the last minute. */
    int32_t top = oy - HW_TOP, y0 = top - AW_GH;
    gcol(0x202020);
    rect(x0 - 4, y0 - 4, right + 4, top + 4);
    gcol(0x40E0E0);                             /* yellow */
    for (unsigned i = 0; i < HAL_HIST; i++) {
        int32_t x = x0 + (int32_t)i * HW_BAR;
        rect(x, y0, x + HW_BAR - 2, y0 + (int32_t)((uint64_t)w->arm_kips[i] * AW_GH / peak));
    }
    snprintf(w->text, sizeof w->text, "Instructions");
    text_at(w, ox + HW_LABEL, y0 + 12, 0xFFFFFF);
    snprintf(w->text, sizeof w->text, "%.1f million/s (peak %.1f)", w->arm_rate[0] / 1e6, peak / 1e3);
    text_at(w, right + 24, y0 + 12, 0x40E0E0);
    /* Time in the engine, in native code and in the FPA, as a share of a core. */
    top -= AW_ROW, y0 = top - AW_GH;
    gcol(0x202020);
    rect(x0 - 4, y0 - 4, right + 4, top + 4);
    for (unsigned i = 0; i < HAL_HIST; i++) {
        int32_t x = x0 + (int32_t)i * HW_BAR;
        int32_t he = w->arm_eng[i] * AW_GH / 255, hn = w->arm_nat[i] * AW_GH / 255, hf = w->arm_fpa[i] * AW_GH / 255;
        gcol(0x20C020);
        rect(x, y0, x + HW_BAR - 2, y0 + he);
        gcol(0x00B0F0);
        rect(x, y0 + he, x + HW_BAR - 2, y0 + he + hn);
        gcol(0xF08040);
        rect(x, y0 + he + hn, x + HW_BAR - 2, y0 + he + hn + hf);
    }
    snprintf(w->text, sizeof w->text, "Time");
    text_at(w, ox + HW_LABEL, y0 + 12, 0xFFFFFF);
    snprintf(w->text, sizeof w->text, "%u.%u%% of a core", w->arm_frac / 10, w->arm_frac % 10);
    text_at(w, right + 24, y0 + 12, 0x20C020);

    const struct ros_arm_view *v = &w->av_now;
    int32_t y = top - AW_GH - 16 - HW_LINE + 12;
    char a[32], b[32];
    commas(a, sizeof a, w->arm_rate[1]), commas(b, sizeof b, w->arm_rate[2]);
    snprintf(w->text, sizeof w->text, "Each second: %.2f million ARM instructions, %s SWIs, %s calls into ARM code",
             w->arm_rate[0] / 1e6, a, b);
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    unsigned e = w->arm_eng[HAL_HIST - 1] * 1000u / 255u, n = w->arm_nat[HAL_HIST - 1] * 1000u / 255u,
             f = w->arm_fpa[HAL_HIST - 1] * 1000u / 255u;
    snprintf(w->text, sizeof w->text, "Of a core: %u.%u%% in the engine, %u.%u%% in native code it called, %u.%u%% in the FPA",
             e / 10, e % 10, n / 10, n % 10, f / 10, f % 10);
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    snprintf(w->text, sizeof w->text, "The engine runs %llu million instructions a second of its time",
             (unsigned long long)w->arm_rate[3]);
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    commas(a, sizeof a, v->instructions), commas(b, sizeof b, v->swis);
    snprintf(w->text, sizeof w->text, "Since the start: %s instructions, %s SWIs; %u ARM task%s, %u ARM module%s",
             a, b, v->tasks, v->tasks == 1 ? "" : "s", v->modules, v->modules == 1 ? "" : "s");
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    if (v->hot && v->samples) {
        int k = snprintf(w->text, sizeof w->text, "Most run:");
        for (unsigned i = 0; i < v->hot && k < (int)sizeof w->text; i++)
            k += snprintf(w->text + k, sizeof w->text - (size_t)k, "%s %s (%llu%%)", i ? "," : "", v->hot_name[i],
                          (unsigned long long)(v->hot_count[i] * 100ull / v->samples));
    } else {
        snprintf(w->text, sizeof w->text, "No ARM code has run yet");
    }
    text_at(w, ox + HW_LABEL, y, 0xFFFFFF);
    y -= HW_LINE;
    snprintf(w->text, sizeof w->text, "Green the engine, amber native code, blue the FPA (a core is the graph's height)");
    text_at(w, ox + HW_LABEL, y, 0xA0A0A0);
}

static void arm_redraw(struct ws *w, int update)
{
    uint32_t *b = w->redraw;
    b[0] = w->arm_window;
    b[1] = 0, b[2] = (uint32_t)-arm_height(), b[3] = HW_W, b[4] = 0;
    uint32_t r[8] = { 0, A(b) };
    if (swi(update ? XWimp_UpdateWindow : XWimp_RedrawWindow, r))
        return;
    while (r[0]) {
        int32_t ox = (int32_t)b[1] - (int32_t)b[5], oy = (int32_t)b[4] - (int32_t)b[6];
        arm_contents(w, ox, oy);
        r[0] = 0, r[1] = A(b);
        if (swi(XWimp_GetRectangle, r))
            return;
    }
}

static void arm_open(struct ws *w)
{
    if (window_open(w, &w->arm_window, "ARM", HW_W, arm_height(), 320, 320))
        w->arm_open = 1;
}

/* Samples the runtime's counters, keeps the fraction for the interval, and redraws. */
static void sample_and_draw(struct ws *w)
{
    hal_sample(w);
    if (w->hal_open)
        hal_redraw(w, 1);
    arm_sample(w);
    if (w->arm_open)
        arm_redraw(w, 1);

    uint32_t r[8] = { 100, A(w->meter) };
    if (swi(XOS_ReadSysInfo, r))
        return;
    uint64_t now = (uint64_t)w->meter[1] << 32 | w->meter[0];
    uint64_t swi_ns = (uint64_t)w->meter[3] << 32 | w->meter[2];
    uint64_t idle_ns = (uint64_t)w->meter[5] << 32 | w->meter[4];
    uint64_t dnow = now - w->was_now, dswi = swi_ns - w->was_swi,
             didle = idle_ns - w->was_idle;
    w->was_now = now, w->was_swi = swi_ns, w->was_idle = idle_ns;
    if (!dnow)
        return;                                    /* only at the first sample */

    uint64_t busy = 1000 - (didle > dnow ? dnow : didle) * 1000 / dnow;
    uint64_t in_swi = dswi > dnow ? 1000 : dswi * 1000 / dnow;
    uint32_t frac = w->metric == M_BUSY ? (uint32_t)busy
        : w->metric == M_INSWI ? (uint32_t)in_swi
        : w->metric == M_CORES ? w->cores_frac
        : w->metric == M_ARM ? w->arm_frac
        : w->metric == M_HAL ? w->hal_frac
        : (uint32_t)(busy / 2 + in_swi / 2);
    if (frac > 1000)
        frac = 1000;

    memmove(w->hist, w->hist + 1, HIST - 1);       /* the oldest value drops off */
    w->hist[HIST - 1] = (uint8_t)(frac * 255u / 1000u);
    draw_graph(w);

    uint32_t *b = w->state;                        /* replot the icon */
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
    b[7] = w->sprite_area;                        /* DeskMeter's sprite area */
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

static void meter_remake(struct ws *w)
{
    icon_delete(w);
    sprite_init(w);
    icon_init(w);
    w->mode_changed = 0;
    sample_and_draw(w);
}

/* ---- the menu ----------------------------------------------------------------- */

enum { MENU_METRIC, MENU_ARM, MENU_HAL, MENU_QUIT, MENU_ITEMS };   /* "HAL..." on the HAL only */

static unsigned metrics(const struct ws *w)
{
    return w->hal ? METRICS : M_CORES;          /* Cores and HAL exist on the HAL only */
}

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
    memcpy(w->menu, "DeskMeter", 9);
    ((uint8_t *)w->menu)[12] = 7;
    ((uint8_t *)w->menu)[13] = 2;
    ((uint8_t *)w->menu)[14] = 7;
    ((uint8_t *)w->menu)[15] = 0;
    w->menu[4] = 9 * 16 + 16;
    w->menu[5] = 44;
    w->menu[6] = 0;
    unsigned k = 0;
    w->menu_ids[k] = MENU_METRIC, menu_item(w, k++, "Next metric", 0);
    w->menu_ids[k] = MENU_ARM, menu_item(w, k++, "ARM...", 0);
    if (w->hal)
        w->menu_ids[k] = MENU_HAL, menu_item(w, k++, "HAL...", 0);
    w->menu_ids[k] = MENU_QUIT, menu_item(w, k++, "Quit", 1);
    uint32_t r[8] = { 0, A(w->menu), x - 64, 96 + k * 44 };
    swi(XWimp_CreateMenu, r);
}

/* ---- the task ------------------------------------------------------------------ */

static void run(struct ros_module *m, struct ws *w)
{
    strcpy(w->name, "meter");
    strcpy(w->task_name, "DeskMeter");
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
    {                                              /* the first sample, from
                                                      which differences start */
        uint32_t r[8] = { 100, A(w->meter) };
        swi(XOS_ReadSysInfo, r);
        w->was_now = (uint64_t)w->meter[1] << 32 | w->meter[0];
        w->was_swi = (uint64_t)w->meter[3] << 32 | w->meter[2];
        w->was_idle = (uint64_t)w->meter[5] << 32 | w->meter[4];
    }
    w->hal = ros_hal_stats(&w->hs_now) == 0;      /* if the HAL answers, there is more to show */
    hal_sample(w);
    arm_sample(w);
    draw_graph(w);                                 /* show the empty frame */
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
                meter_remake(w);
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
                    w->metric = (uint8_t)((w->metric + metrics(w) + (unsigned)step) % metrics(w));
                } else if (w->poll[2] & BUTTON_MENU) {
                    menu_show(w, w->poll[0]);
                }
            }
            break;
        case MENU_SELECTION: {
            uint32_t item = w->poll[0] < MENU_ITEMS ? w->menu_ids[w->poll[0]] : MENU_ITEMS;
            if (item == MENU_METRIC)
                w->metric = (uint8_t)((w->metric + 1) % metrics(w));
            else if (item == MENU_HAL)
                hal_open(w);
            else if (item == MENU_ARM)
                arm_open(w);
            else if (item == MENU_QUIT)
                goto quit;
            break;
        }
        case REDRAW_WINDOW:
            if (w->poll[0] == w->hal_window)
                hal_redraw(w, 0);
            else if (w->poll[0] == w->arm_window)
                arm_redraw(w, 0);
            break;
        case OPEN_WINDOW:
            if (w->poll[0] && (w->poll[0] == w->hal_window || w->poll[0] == w->arm_window)) {
                uint32_t r[8] = { 0, A(w->poll) };
                swi(XWimp_OpenWindow, r);
            }
            break;
        case CLOSE_WINDOW:
            if (w->poll[0] && (w->poll[0] == w->hal_window || w->poll[0] == w->arm_window)) {
                uint32_t r[8] = { 0, A(w->poll) };
                swi(XWimp_CloseWindow, r);
                if (w->poll[0] == w->hal_window)
                    w->hal_open = 0;
                else
                    w->arm_open = 0;
            }
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

int deskmeter_autostart = -1;

static void service(struct ros_module *m, struct ros_cpu *s)
{
    struct ws *w = workspace(m);
    switch (s->r[1]) {
    case SERVICE_STARTWIMP:
        if (w || !(deskmeter_autostart >= 0 ? deskmeter_autostart : ros_cmdline_has("rosgd.deskmeter")))
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
            strcpy(w->command, "DeskMeter");
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
    { "DeskMeter", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *DeskMeter",
      "Runs the Desk Meter on the icon bar: a graph of how busy the Wimp is.\r"
      "SELECT or ADJUST changes the metric; MENU offers more.\r",
      cmd_deskmeter },
    { 0 },
};

struct ros_module deskmeter_module = {
    .title = "DeskMeter",
    .help = "DeskMeter\t1.00 (29-Sep-26) ROSGD native",
    .init = NULL,
    .final = final,
    .service = service,
    .start = start,
    .commands = commands,
};
