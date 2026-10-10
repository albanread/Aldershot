/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_vdisplay.c: virtual displays (runtime/vdu/vdisplay.c), used by
 * GraphTask.
 *
 * /init, task 0, makes two virtual displays, A (mode 28) and B (mode 12),
 * and two tasks, TA and TB, and passes the baton between them as the Wimp
 * would. TA takes A and TB takes B, each by VDisplay_Attach, and draws:
 *
 *   TA: MODE 12 in A; a filled circle and four characters; its variables
 *       and its pixels                                          -> task 0
 *   task 0: the real display's mode and screen as they were    -> TB
 *   TB: draws in B, then MODE 28 in it                          -> TA
 *   TA: A as it left it: mode, cursor, pixels; output to a sprite
 *       and back; background work on the real display; a real section and
 *       its healing; the banks; the mouse and key scans by the focus;
 *       the dirty box                                           -> TB
 *   TB: B as it left it; ends                                   -> task 0
 *   task 0: TA ends; the displays destroyed; the real display unharmed
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/task.h"
#include "rosgd/vdu.h"
#include "selftest.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v;
}

#define XWIMP_EXTEND 0x600FBu

static unsigned changes_seen;

static void count_change(void)
{
    changes_seen++;
}

static uint32_t errnum(const uint32_t r[10])
{
    return ((const os_error *)ros_ptr(r[0]))->errnum;
}

static uint32_t mvar(uint32_t v)
{
    uint32_t r[10] = { 0xFFFFFFFFu, v };
    swi(XOS_ReadModeVariable, r);
    return r[2];
}

static uint32_t vduvar(uint32_t v)
{
    uint32_t blk = ros_vdu_scratch();
    ros_st32(blk, v);
    ros_st32(blk + 4, 0xFFFFFFFFu);
    uint32_t r[10] = { blk, blk + 8 };
    swi(XOS_ReadVduVariables, r);
    return ros_ld32(blk + 8);
}

static void vdu(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t r[10] = { b[i] };
        swi(XOS_WriteC, r);
    }
}
#define V(...) vdu((const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))
#define LO(x) (uint8_t)((x) & 0xFF)
#define HI(x) (uint8_t)(((x) >> 8) & 0xFF)

static uint32_t pos(void)
{
    uint32_t r[10] = { 134 };
    swi(XOS_Byte, r);
    return r[1];
}

static uint32_t read_point(int32_t x, int32_t y, uint32_t *off)
{
    uint32_t r[10] = { (uint32_t)x, (uint32_t)y };
    swi(XOS_ReadPoint, r);
    *off = r[4];
    return r[2];
}

/* The real display's screen, hashed */
static uint32_t screen_hash(void)
{
    uint32_t start = vduvar(148), size = mvar(7);
    if (!start || !size)
        return 0;
    const uint8_t *p = ros_ptr(start);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static struct ros_task *main_task, *TA, *TB;
static uint32_t A, B;

static struct {
    /* the real display at the start */
    uint32_t xw, yw, l2, start, hash;
    /* TA, first */
    int a_attached, a_mode28, a_mode12, a_screen, a_point, a_pixel, a_pos, a_info;
    uint32_t a_xw, a_yw, a_l2, a_start, a_image, a_p, a_pixel_v;
    /* task 0, between */
    int real_kept_1;
    /* TB */
    int b_first, b_mode28, b_kept;
    uint32_t b_yw, b_pos;
    /* TA, second */
    int a_kept, a_sprite_in, a_sprite_back, a_background, a_real, a_heal, a_banks,
        a_mouse, a_keys, a_changed;
    uint32_t a_kept_yw, a_kept_pos, a_bg_yw, a_spr_xw, a_back_xw, a_mouse_x, a_mouse_y;
    uint32_t a_changed_flags;
    int32_t a_box[4];
    /* the end */
    int destroy_refused;
} r;

/* ---- background work, from TA ---- */

static uint32_t bg_yw, bg_l2;

static void bg_read(void *arg, uint32_t info)
{
    (void)arg, (void)info;
    bg_yw = mvar(12);
    bg_l2 = mvar(9);
}

/* ---- TA ---- */

static void task_a(void *arg)
{
    (void)arg;
    uint32_t q[10] = { A, 0 };
    r.a_attached = !swi(XVDisplay_Attach, q) && q[0] == 0;
    r.a_mode28 = mvar(11) == 639 && mvar(12) == 479 && mvar(9) == 3;

    /* MODE 12 in A: 640 x 256, 16 colours */
    V(22, 12);
    V(23, 1, 0, 0, 0, 0, 0, 0, 0, 0);   /* its cursor off: its flashing would draw */
    r.a_xw = mvar(11), r.a_yw = mvar(12), r.a_l2 = mvar(9);
    r.a_mode12 = r.a_xw == 639 && r.a_yw == 255 && r.a_l2 == 2;
    uint32_t i[10] = { A };
    r.a_info = !swi(XVDisplay_Info, i) && i[3] == 640 && i[4] == 256 && i[5] == 1 && i[6] == 2;
    r.a_image = i[2] + ros_ld32(i[2] + 32);
    r.a_start = vduvar(148);
    r.a_screen = r.a_start == r.a_image && vduvar(149) == r.a_image;

    /* a filled circle in colour 1, centred at (640, 512): pixel (320, 128) */
    V(18, 0, 1);
    V(25, 4, LO(640), HI(640), LO(512), HI(512));
    V(25, 157, LO(740), HI(740), LO(512), HI(512));
    uint32_t off;
    r.a_p = read_point(640, 512, &off);
    r.a_point = r.a_p == 1 && off == 0;
    /* in its pixels: row 255 - 128 from the top, 4 bpp, the low nibble */
    r.a_pixel_v = ros_ld8(r.a_start + (255 - 128) * 320 + 320 / 2) & 15;
    r.a_pixel = r.a_pixel_v == 1;
    V(30);
    V('A', 'A', 'A', 'A');
    r.a_pos = pos() == 4;

    ros_task_switch(main_task);

    /* ---- second: A as TA left it, though TB changed B's mode ---- */
    r.a_kept_yw = mvar(12), r.a_kept_pos = pos();
    r.a_kept = r.a_kept_yw == 255 && r.a_kept_pos == 4 && read_point(640, 512, &off) == 1 &&
               off == 0 && vduvar(148) == r.a_image;

    /* output to a sprite inside the virtual display, and back */
    uint32_t area = ros_addr(ros_rma_alloc(16 * 1024));
    ros_st32(area, 16 * 1024);
    ros_st32(area + 8, 16);
    uint32_t s[10] = { 256 + 9, area };
    swi(XOS_SpriteOp, s);                               /* initialise the area */
    uint32_t name = ros_addr(ros_rma_alloc(16));
    memcpy(ros_ptr(name), "nest", 5);
    uint32_t c[10] = { 256 + 15, area, name, 0, 32, 32, 28 };
    int made = !swi(XOS_SpriteOp, c);
    uint32_t o[10] = { 256 + 60, area, name, 0 };
    int in = made && !swi(XOS_SpriteOp, o);
    r.a_spr_xw = mvar(11);
    r.a_sprite_in = in && r.a_spr_xw == 31 && mvar(12) == 31 && vduvar(148) != r.a_image;
    V(18, 0, 3, 16);                                    /* the sprite cleared to colour 3 */
    uint32_t back[10] = { o[0], o[1], o[2], o[3] };
    int out = !swi(XOS_SpriteOp, back);
    r.a_back_xw = mvar(11);
    r.a_sprite_back = in && out && r.a_back_xw == 639 && mvar(12) == 255 &&
                      vduvar(148) == r.a_image && read_point(640, 512, &off) == 1;
    ros_rma_free(ros_ptr(name));
    ros_rma_free(ros_ptr(area));

    /* background work, here, sees the real display; TA then its own */
    bg_yw = bg_l2 = 0xFFFFFFFFu;
    ros_post(bg_read, NULL, 0);
    ros_background_run();
    r.a_bg_yw = bg_yw;
    r.a_background = bg_yw == r.yw && bg_l2 == r.l2 && mvar(12) == 255;

    /* a real section (the Wimp's SWIs, callbacks): the real display; and a
     * section an exit left open, healed by the task's next SWI */
    int e = ros_vdu_real_enter();
    ros_call_depth++;                   /* as if inside a SWI: not the task's own */
    uint32_t inside = mvar(12);
    ros_call_depth--;
    ros_vdu_real_leave(e);
    r.a_real = e == 1 && inside == r.yw && mvar(12) == 255;
    ros_call_depth++;                   /* as if inside a SWI */
    ros_vdu_real_enter();               /* ... and never left */
    uint32_t stuck = mvar(12);
    ros_call_depth--;
    r.a_heal = stuck == r.yw && mvar(12) == 255;        /* the task's own SWI heals it */

    /* the banks: OS_Byte 112 writes bank 2, 113 shows it */
    uint32_t ss = mvar(7);
    uint32_t b1[10] = { 112, 2 };
    swi(XOS_Byte, b1);
    uint32_t written = vduvar(148);
    uint32_t b2[10] = { 113, 2 };
    swi(XOS_Byte, b2);
    uint32_t shown = vduvar(149);
    uint32_t i2[10] = { A };
    swi(XVDisplay_Info, i2);
    uint32_t sprite2 = i2[2] + ros_ld32(i2[2] + 32);
    uint32_t b3[10] = { 112, 0 }, b4[10] = { 113, 1 };
    swi(XOS_Byte, b3);
    swi(XOS_Byte, b4);
    r.a_banks = b1[1] == 1 && written == r.a_image + ss && b2[1] == 1 && shown == written &&
                sprite2 == written && b3[1] == 2 && vduvar(148) == r.a_image &&
                vduvar(149) == r.a_image;

    /* the mouse: mapped through the rectangle A is shown in, buttons and
     * key scans only with the focus */
    uint32_t m0[10];
    ros_vdu_real_enter();
    ros_call_depth++;
    swi(XOS_Mouse, m0);                 /* the real mouse, OS units */
    ros_call_depth--;
    ros_vdu_real_leave(1);
    uint32_t rect = ros_addr(ros_rma_alloc(16));
    ros_st32(rect, 0), ros_st32(rect + 4, 0);
    ros_st32(rect + 8, 0x8000), ros_st32(rect + 12, 0x8000);
    uint32_t f[10] = { A, 0, 0xFFFFFFFFu, rect };
    swi(XVDisplay_SetFocus, f);
    uint32_t m1[10];
    swi(XOS_Mouse, m1);
    r.a_mouse_x = m1[0], r.a_mouse_y = m1[1];
    int32_t want_x = (int32_t)(((int64_t)(int32_t)m0[0] * 1280) / 0x8000);
    int32_t want_y = (int32_t)(((int64_t)(int32_t)m0[1] * 1024) / 0x8000);
    r.a_mouse = (int32_t)m1[0] == want_x && (int32_t)m1[1] == want_y && m1[2] == 0;
    uint32_t k[10] = { 129, 0xFF, 0xFF };
    swi(XOS_Byte, k);
    int blocked = ros_vdu_keys_blocked() && k[1] == 0 && k[2] == 0;
    f[1] = 1;
    swi(XVDisplay_SetFocus, f);
    r.a_keys = blocked && !ros_vdu_keys_blocked();
    f[1] = 0, f[3] = 0;
    swi(XVDisplay_SetFocus, f);
    ros_rma_free(ros_ptr(rect));

    /* the dirty box: cleared, then a rectangle drawn */
    uint32_t ch[10] = { A, 1 };
    swi(XVDisplay_Changed, ch);
    V(25, 4, LO(100), HI(100), LO(100), HI(100));
    V(25, 101, LO(200), HI(200), LO(200), HI(200));   /* a filled rectangle to (200, 200) */
    uint32_t ch2[10] = { A, 1 };
    swi(XVDisplay_Changed, ch2);
    r.a_changed_flags = ch2[0];
    for (int j = 0; j < 4; j++)
        r.a_box[j] = (int32_t)ch2[2 + j];
    r.a_changed = (ch2[0] & 1) && !(ch2[0] & 2) && ch2[1] != ch[1] && r.a_box[0] == 50 &&
                  r.a_box[1] == 25 && r.a_box[2] == 100 && r.a_box[3] == 50;

    ros_task_switch(TB);
}

/* ---- TB ---- */

static void task_b(void *arg)
{
    (void)arg;
    uint32_t q[10] = { B, 0 };
    int attached = !swi(XVDisplay_Attach, q);
    V(30, 'B');
    r.b_pos = pos();
    r.b_first = attached && mvar(12) == 255 && mvar(9) == 2 && r.b_pos == 1;
    V(22, 28);
    r.b_yw = mvar(12);
    r.b_mode28 = r.b_yw == 479 && mvar(9) == 3 && pos() == 0;
    V('B', 'B');

    ros_task_switch(TA);

    r.b_kept = mvar(12) == 479 && pos() == 2;
}

void ros_selftest_vdisplay(void)
{
    memset(&r, 0, sizeof r);
    main_task = ros_task_current();
    V(23, 1, 0, 0, 0, 0, 0, 0, 0, 0);   /* the real cursor off: its flashing would change it */
    r.xw = mvar(11), r.yw = mvar(12), r.l2 = mvar(9), r.start = vduvar(148);
    r.hash = screen_hash();

    uint32_t c[10] = { 0, 0xFFFFFFFFu };
    int a_ok = !swi(XVDisplay_Create, c);
    A = c[0];
    uint32_t i[10] = { A };
    int info = a_ok && !swi(XVDisplay_Info, i);
    uint32_t sp[10] = { 512 + 40, i[1], i[2] };
    int spr = info && !swi(XOS_SpriteOp, sp);
    check(a_ok && info && i[3] == 640 && i[4] == 480 && spr && sp[3] == 640 && sp[4] == 480 &&
              mvar(11) == r.xw && vduvar(148) == r.start,
          "VDisplay_Create: mode 28 by default, its bank a sprite of 640 x 480, the real display kept",
          "handle %u, info %ux%u, sprite %ux%u", A, i[3], i[4], sp[3], sp[4]);
    uint32_t c2[10] = { 0, 12 };
    int b_ok = !swi(XVDisplay_Create, c2);
    B = c2[0];

    TA = ros_task_create(64 * 1024, task_a, NULL);
    TB = ros_task_create(64 * 1024, task_b, NULL);
    if (!a_ok || !b_ok || !TA || !TB) {
        check(0, "VDisplay: the displays and the tasks", "A %d B %d", a_ok, b_ok);
        return;
    }
    ros_task_switch(TA);

    r.real_kept_1 = mvar(11) == r.xw && mvar(12) == r.yw && mvar(9) == r.l2 &&
                    vduvar(148) == r.start && screen_hash() == r.hash;
    uint32_t d[10] = { A };
    r.destroy_refused = swi(XVDisplay_Destroy, d) && errnum(d) == 0xC01C1u;
    ros_task_switch(TB);                /* TB, TA, TB again: it ends, back here */
    ros_task_switch(TA);                /* TA ends: back here */

    check(r.a_attached && r.a_mode28, "VDisplay_Attach: the task's mode is its display's",
          "attached %d", r.a_attached);
    check(r.a_mode12 && r.a_info, "MODE 12 in a virtual display: its variables and its Info",
          "%u x %u, log2bpp %u", r.a_xw + 1, r.a_yw + 1, r.a_l2);
    check(r.a_screen, "ScreenStart and DisplayStart: the display's own memory, its bank's sprite",
          "&%08X, image &%08X", r.a_start, r.a_image);
    check(r.a_point && r.a_pixel, "A circle drawn in it: OS_ReadPoint, and its pixels",
          "point %u, pixel %u", r.a_p, r.a_pixel_v);
    check(r.a_pos, "Text in it: the cursor its own", "POS");
    check(r.real_kept_1, "The real display after it: mode, screen address and pixels unchanged",
          "%u x %u", mvar(11) + 1, mvar(12) + 1);
    check(r.destroy_refused, "VDisplay_Destroy refused while a task has the display", "");
    check(r.b_first && r.b_mode28,
          "A second display: its own mode, cursor and MODE change",
          "first %d, MODE 28 rows %u", r.b_first, r.b_yw + 1);
    check(r.a_kept && r.b_kept, "Two displays independent across switches",
          "A rows %u pos %u, B %d", r.a_kept_yw + 1, r.a_kept_pos, r.b_kept);
    check(r.a_sprite_in && r.a_sprite_back,
          "OS_SpriteOp 60 inside a virtual display, and back to it (not the real one)",
          "in: XWind %u, back: XWind %u", r.a_spr_xw, r.a_back_xw);
    check(r.a_background, "Background work while the task runs: the real display",
          "rows %u, real %u", r.a_bg_yw + 1, r.yw + 1);
    check(r.a_real && r.a_heal,
          "A real section: the real display; one left open healed at the task's next SWI",
          "real %d heal %d", r.a_real, r.a_heal);
    check(r.a_banks, "OS_Byte 112 and 113: two banks, the sprite shown following", "");
    check(r.a_mouse && r.a_keys,
          "The mouse mapped into the display; buttons and key scans by the focus",
          "mouse %d,%d keys %d", (int32_t)r.a_mouse_x, (int32_t)r.a_mouse_y, r.a_keys);
    check(r.a_changed, "VDisplay_Changed: the box of what was drawn, in pixels",
          "flags %u box %d,%d,%d,%d", r.a_changed_flags, r.a_box[0], r.a_box[1], r.a_box[2],
          r.a_box[3]);

    int ended = ros_task_ended(TA) && ros_task_ended(TB);
    ros_task_destroy(TA);
    ros_task_destroy(TB);
    /* SetMode from outside (GraphTask's Mode menu): the display's mode,
     * not the real one's */
    uint32_t sm[10] = { B, 13 };
    uint32_t i4[10] = { B };
    int set = !swi(XVDisplay_SetMode, sm) && !swi(XVDisplay_Info, i4);
    check(set && i4[0] == 13 && i4[3] == 320 && i4[4] == 256 && mvar(11) == r.xw && vduvar(148) == r.start,
          "VDisplay_SetMode: the display's mode changed from outside, the real one kept",
          "mode %u, %u x %u", i4[0], i4[3], i4[4]);

    /* What the native Wimp's surface windows read of a display, and
     * how it is told of a change. The Wimp itself needs a task, so that is
     * checked by eye. */
    struct ros_vdisplay_view v;
    memset(&v, 0, sizeof v);
    int32_t box[4] = { 0 };
    int viewed = ros_vdisplay_view(B, &v) == 0;
    int took = ros_vdisplay_take(B, box), again = ros_vdisplay_take(B, box);
    check(viewed && v.width == 320 && v.height == 256 && v.sprite == i4[2] && v.pal_entries == 256 &&
              took == 3 && box[2] == 319 && box[3] == 255 && again == 0,
          "G4: a display's view (the shown bank's sprite, its size and palette) and its changes taken",
          "%u x %u sprite &%08X palette %u, took %d (%d,%d) then %d", v.width, v.height, v.sprite,
          v.pal_entries, took, box[2], box[3], again);
    void (*was_hook)(void) = ros_vdisplay_changed_hook;
    ros_vdisplay_changed_hook = count_change;
    changes_seen = 0;
    uint32_t sm2[10] = { B, 12 };
    int set2 = !swi(XVDisplay_SetMode, sm2);
    ros_vdisplay_changed_hook = was_hook;
    uint32_t gen = v.mode_gen;
    ros_vdisplay_view(B, &v);
    check(set2 && changes_seen > 0 && v.mode_gen != gen && v.width == 640,
          "G4: a change told to whoever shows the display (the hook), a new mode seen in the view",
          "hook %u, mode gen %u -> %u, width %u", changes_seen, gen, v.mode_gen, v.width);
    /* the native Wimp's Wimp_Extend reasons &5200-&5203: a bad window, and
     * a bind by no task, refused as its other window calls are */
    uint32_t e1[10] = { 0x5203, 0x12345678u }, e2[10] = { 0x5200, 0x12345678u, 1, B, 0, 1 };
    int bad = swi(XWIMP_EXTEND, e1) && errnum(e1) == 0x288u;
    int notask = swi(XWIMP_EXTEND, e2) && (errnum(e2) == 0x281u || errnum(e2) == 0x288u);
    check(bad && notask, "G4: Wimp_Extend's surface reasons -- an illegal window handle refused",
          "info %d bind %d", bad, notask);

    uint32_t d1[10] = { A }, d2[10] = { B };
    int gone = !swi(XVDisplay_Destroy, d1) && !swi(XVDisplay_Destroy, d2);
    uint32_t i3[10] = { A };
    int stale = swi(XVDisplay_Info, i3) && errnum(i3) == 0xC01C0u;
    check(ended && gone && stale && ros_vdu_displays == 0,
          "The tasks ended and detached; the displays destroyed", "ended %d gone %d", ended, gone);
    check(mvar(11) == r.xw && mvar(12) == r.yw && mvar(9) == r.l2 && vduvar(148) == r.start &&
              screen_hash() == r.hash,
          "The real display at the end: as it was", "%u x %u", mvar(11) + 1, mvar(12) + 1);
    V(23, 1, 1, 0, 0, 0, 0, 0, 0, 0);   /* the real cursor back */
}
