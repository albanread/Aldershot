/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.vdu.vdupointer, s.PMF.mouse).
 */
/* pointer.c -- the mouse pointer and the mouse.
 *
 * This follows the kernel's Kernel/s/vdu/vdupointer and
 * Kernel/s/PMF/mouse.
 *
 *   - There are four pointer shapes. They are defined by OS_Word 21,0 into
 *     2 bpp buffers of 8 bytes a row, with up to 32 rows. Each definition
 *     goes into one of two holding shapes and is then swapped in, so the
 *     driver never sees a half-made one.
 *   - OS_Byte 106 chooses the shape. 0 is off, and bit 7 unlinks the shape
 *     from the mouse. It takes effect at the next VSync.
 *   - The mouse has a position, a bounding box and multipliers (OS_Word
 *     21,1-4). It is fed by PointerV. PointerV is polled with Request each
 *     VSync, and it is reported to (the kernel's default claimant).
 *     OS_Pointer also reads it.
 *   - Each VSync the pointer follows the mouse, unless it is unlinked
 *     (OS_Word 21,5 and 6 move and read it). It goes to the display driver
 *     through GraphicsV 5, UpdatePointer. DRMVideo has a hardware pointer,
 *     so the kernel's software pointer is not needed.
 *   - A mode change turns the pointer off and puts the mouse in the middle
 *     of a box the size of the screen.
 *   - The buttons Select, Menu and Adjust are keys. The keyboard handler
 *     (runtime/keyboard.c, modules/intkey) hands them back here. The extra
 *     buttons come with PointerV's wheel report. Each change is Event 10
 *     and a block for the mouse buffer. RISC OS 5 leaves the buffer empty
 *     (button_change), so OS_Mouse reads the mouse as it is now, as the
 *     kernel does when its buffer is empty.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/keyboard.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/ticker.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "vduws.h"

#define POINTERV 0x26u
#define GV_UPDATE_POINTER 5
enum { POINTER_REQUEST = 0, POINTER_REPORT = 3, POINTER_WHEEL = 9 };
#define ABSO 0x6F736241u                /* "Abso" */

/* A shape, as the driver is given it (PointerBlkHAL). It has the width in
 * bytes, the height, the active point and the buffer's address. */
enum { PB_WIDTH = 0, PB_HEIGHT = 1, PB_ACTIVEX = 2, PB_ACTIVEY = 3, PB_BUFF = 4, PB_SIZE = 12 };
#define SHAPE_BYTES (8 * 32)

static struct {
    uint32_t blocks;                    /* six shape blocks, then six buffers, in the RMA */
    uint32_t shape[4], hold[2];         /* PointerShapes, PointerShapesH */
    uint32_t shape_la;                  /* PointerShapeLA: the buffer last given */
    uint32_t number;                    /* PointerShapeNumber */
    int32_t px, py;                     /* PointerX, PointerY */
    uint32_t xeig;                      /* PointerXEigFactor */
    /* The mouse (KeyWorkSpace). */
    int32_t mx, my, bl, bb, br, bt;     /* MouseX, MouseY, MouseBounds */
    int32_t xmult, ymult;
    uint32_t buttons, type, reporting;
    int32_t altx, alty;
} P;

static int16_t s16(uint32_t a)
{
    return (int16_t)(ros_ld8(a) | ros_ld8(a + 1) << 8);
}

/* ---- OS_Word 21 ---------------------------------------------------------------------------- */

static void define(uint32_t b)
{
    uint32_t h = P.shape_la == ros_ld32(P.hold[0] + PB_BUFF) ? 1 : 0, blk = P.hold[h];
    uint32_t n = ros_ld8(b + 1) - 1;
    if (n >= 4)
        return;
    uint32_t w = ros_ld8(b + 2), ht = ros_ld8(b + 3), ax = ros_ld8(b + 4), ay = ros_ld8(b + 5);
    if (ht == 0 || w == 0) {            /* an empty shape means off */
        ros_st8(blk + PB_WIDTH, 0), ros_st8(blk + PB_HEIGHT, 0);
    } else {
        if (w > 8 || ht > 32 || ax >= w * 4 || ay >= ht)
            return;
        ros_st8(blk + PB_WIDTH, w), ros_st8(blk + PB_HEIGHT, ht);
        ros_st8(blk + PB_ACTIVEX, ax), ros_st8(blk + PB_ACTIVEY, ay);
        uint32_t src = ros_ld8(b + 6) | ros_ld8(b + 7) << 8 | ros_ld8(b + 8) << 16 |
                       ros_ld8(b + 9) << 24;
        uint8_t *dst = ros_ptr(ros_ld32(blk + PB_BUFF));
        for (uint32_t y = 0; y < ht; y++, dst += 8, src += w) {
            memcpy(dst, ros_ptr(src), w);
            memset(dst + w, 0, 8 - w);
        }
    }
    uint32_t t = P.shape[n];
    P.shape[n] = blk, P.hold[h] = t;
}

static void clamp_mouse(void)
{
    if (P.mx < P.bl) P.mx = P.bl;
    if (P.br < P.mx) P.mx = P.br;
    if (P.my < P.bb) P.my = P.bb;
    if (P.bt < P.my) P.my = P.bt;
}

static void mouse_box(int32_t l, int32_t b, int32_t r, int32_t t)
{
    l = (int16_t)(l + vdu.orgx), r = (int16_t)(r + vdu.orgx);
    b = (int16_t)(b + vdu.orgy), t = (int16_t)(t + vdu.orgy);
    if (r < l || t < b)
        return;
    P.bl = l, P.bb = b, P.br = r, P.bt = t;
    clamp_mouse();
}

static void get_pair(uint32_t b, int32_t *x, int32_t *y)
{
    *x = (int16_t)(s16(b + 1) + vdu.orgx);
    *y = (int16_t)(s16(b + 3) + vdu.orgy);
}

static void put_pair(uint32_t b, int32_t x, int32_t y)
{
    x -= vdu.orgx, y -= vdu.orgy;
    ros_st8(b + 1, (uint32_t)x & 0xFF), ros_st8(b + 2, ((uint32_t)x >> 8) & 0xFF);
    ros_st8(b + 3, (uint32_t)y & 0xFF), ros_st8(b + 4, ((uint32_t)y >> 8) & 0xFF);
}

static void flush_mouse(void)
{
    /* The mouse buffer is not kept yet (see the top). */
}

static void poll(void);

void vdu_pointer_word(uint32_t b)
{
    int32_t x, y;
    if (vdu.vd) {
        /* A virtual display. The pointer and the mouse are the desktop's.
         * Reading them gives the mouse mapped into the display, and the
         * rest does nothing. */
        uint32_t buttons = 0;
        if (ros_ld8(b) == 4 || ros_ld8(b) == 6) {
            poll();
            x = P.mx, y = P.my;
            vdisplay_mouse(&x, &y, &buttons);
            put_pair(b, x, y);
        }
        return;
    }
    switch (ros_ld8(b)) {
    case 0: define(b); break;
    case 1: mouse_box(s16(b + 1), s16(b + 3), s16(b + 5), s16(b + 7)); break;
    case 2: P.xmult = (int8_t)ros_ld8(b + 1), P.ymult = (int8_t)ros_ld8(b + 2); break;
    case 3:
        get_pair(b, &x, &y);
        if (x >= P.bl && P.br >= x && y >= P.bb && P.bt >= y)
            P.mx = x, P.my = y, flush_mouse();
        break;
    case 4:
        poll();
        put_pair(b, P.mx, P.my);
        break;
    case 5: get_pair(b, &P.px, &P.py); break;
    case 6: put_pair(b, P.px, P.py); break;
    default: break;                     /* the kernel ignores the others */
    }
}

/* ---- OS_Byte 106 --------------------------------------------------------------------------- */

void vdu_pointer_select(struct ros_cpu *s)
{
    if (vdu.vd) {                       /* a virtual display keeps the desktop's pointer */
        s->r[2] = s->r[1] & 0x7F;
        s->r[1] = 0;
        return;
    }
    uint32_t old = P.number, n = s->r[1];
    s->r[2] = n & 0x7F;                 /* as the kernel leaves it */
    if (s->r[2] <= 4 && n != old)
        P.number = n & 0xFF;
    s->r[1] = old;
}

/* ---- the mouse ----------------------------------------------------------------------------- */

static int32_t bound(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) v = lo;
    if (hi < v) v = hi;
    return v;
}

/* ProcessMouseXY. */
static void move_mouse(uint32_t dx, uint32_t dy, uint32_t abso)
{
    if (abso == ABSO) {
        P.mx = bound((int16_t)dx, P.bl, P.br);
        P.my = bound((int16_t)dy, P.bb, P.bt);
        return;
    }
    if (dx)
        P.mx = bound((int32_t)((dx << 16) * (uint32_t)P.xmult + ((uint32_t)P.mx << 16)) >> 16,
                     P.bl, P.br);
    if (dy)
        P.my = bound((int32_t)((dy << 16) * (uint32_t)P.ymult + ((uint32_t)P.my << 16)) >> 16,
                     P.bb, P.bt);
}

/* PollPointer. This makes a PointerV Request, unless the device reports by
 * itself. */
static void poll(void)
{
    if (P.reporting)
        return;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = POINTER_REQUEST, s.r[1] = P.type;
    ros_vector_call(POINTERV, &s);
    move_mouse(s.r[2], s.r[3], s.r[4]);
}

/* PointerScrollApply, with RISCOS Ltd's wrap-to-zero. */
static int32_t wrap_add(int32_t a, int32_t d)
{
    int32_t r = (int32_t)((uint32_t)a + (uint32_t)d);
    if (((a ^ r) & (d ^ r)) < 0)
        r = (int32_t)((uint32_t)r - (a < 0 ? 0x7FFFFFFFu : 0x80000000u));
    return r;
}

/* MouseButtonChange. The buttons are bit 0 for Adjust, bit 1 for Menu, bit
 * 2 for Select, and the extras above. A change generates Event 10 and
 * inserts a block into the mouse buffer. Neither RISC OS 5's kernel nor its
 * Buffer Manager answers the insert, so here too the buffer stays empty and
 * OS_Mouse reads the mouse as it is. */
static void button_change(uint32_t all)
{
    P.buttons = all & 0xFF;
    uint32_t t = ros_ld32(ROS_ZP_METROGNOME);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = (uint32_t)(P.mx - vdu.orgx), c.r[2] = (uint32_t)(P.my - vdu.orgy);
    c.r[3] = P.buttons, c.r[4] = t;
    ros_event_generate(&c);
    uint8_t *blk = ros_rma_alloc(12);
    uint32_t w[3] = { ((uint32_t)P.my << 16) | ((uint32_t)P.mx & 0xFFFF), P.buttons | t << 8, t >> 24 };
    memcpy(blk, w, sizeof w);
    ros_cpu_enter(&c);
    c.r[1] = 9 | 1u << 31, c.r[2] = ros_addr(blk), c.r[3] = 9;
    ros_vector_call(0x14, &c);                          /* InsV */
    ros_rma_free(blk);
}

/* The keyboard handler's way in (the kernel's ReturnVector). */
void ros_key_mouse_buttons(uint32_t lcr)
{
    button_change((P.buttons & ~7u) | (lcr & 7));
}

/* The kernel's own PointerV claimant, at the chain's foot. It never
 * claims. */
static int pointer_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == POINTER_REPORT) {
        if (s->r[1] == P.type)
            P.reporting = 1;
        move_mouse(s->r[2], s->r[3], s->r[4]);
    } else if (s->r[0] == POINTER_WHEEL) {
        uint32_t extra = s->r[2] & 0x1F;
        if ((extra ^ (P.buttons >> 3)) != 0)
            button_change((P.buttons & 7) | extra << 3);
        if (s->r[1] ^ s->r[3]) {
            struct ros_cpu e;
            ros_cpu_enter(&e);
            e.r[0] = 21, e.r[1] = 4, e.r[2] = s->r[3], e.r[3] = -s->r[1];   /* Expansion, PointerScroll */
            ros_event_generate(&e);
            /* If the event is disabled, the kernel applies the scroll here.
             * If it is enabled, the kernel's default EventV owner applies
             * it, unless a claimant suppresses it. ROSGD's events do not
             * offer that yet, so it is always applied. */
            P.altx = wrap_add(P.altx, (int32_t)s->r[3]);
            P.alty = wrap_add(P.alty, -(int32_t)s->r[1]);
        }
    }
    return ROS_VECTOR_PASS;
}

/* OS_Mouse. With no buffer it gives the mouse as it is now, after a safe
 * point, even inside a SWI. On RISC OS the mouse's interrupt breaks into a
 * loop that polls it (Wimp_ReportError's and Wimp_CommandWindow's;
 * streams.c). */
void ros_thunk_OS_Mouse(struct ros_cpu *s)
{
    ros_background_run();
    int32_t x = P.mx, y = P.my;
    uint32_t buttons = P.buttons;
    if (vdu.vd)                         /* mapped into a virtual display */
        vdisplay_mouse(&x, &y, &buttons);
    s->r[0] = (uint32_t)(x - vdu.orgx);
    s->r[1] = (uint32_t)(y - vdu.orgy);
    s->r[2] = buttons;
    s->r[3] = ros_monotonic_cs();
    s->v = 0;
}

/* OS_Pointer. Reason 0 reads the pointer type, 1 sets it, and 2 gives the
 * scroll wheels' total. */
void ros_thunk_OS_Pointer(struct ros_cpu *s)
{
    switch (s->r[0]) {
    case 0:
        s->r[0] = P.type;
        break;
    case 1: {
        P.type = s->r[1] & 0xFF;
        P.reporting = 0;
        struct ros_cpu c = *s;
        c.r[0] = 2;                     /* PointerV Selected */
        ros_vector_call(POINTERV, &c);
        break;
    }
    case 2:
        s->r[0] = (uint32_t)P.altx, s->r[1] = (uint32_t)P.alty;
        break;
    default:
        ros_swi_fail(s, ros_error(0x1EA, "Bad parameters"));
        return;
    }
    s->v = 0;
}

/* ---- each VSync, and a mode change --------------------------------------------------------- */

static void update(void)
{
    if (!(P.number & 0x80))
        P.px = P.mx, P.py = P.my;
    uint32_t r[4] = { 0 }, n = P.number & 0x7F, blk = n ? P.shape[n - 1] : 0;
    if (!n || ros_ld8(blk + PB_HEIGHT) == 0) {
        P.shape_la = 0;
    } else {
        r[0] = 1;
        uint32_t buf = ros_ld32(blk + PB_BUFF);
        if (buf != P.shape_la)
            P.shape_la = buf, r[0] |= 2;
        r[1] = (uint32_t)((P.px >> P.xeig) - (int32_t)ros_ld8(blk + PB_ACTIVEX));
        r[2] = (uint32_t)((int32_t)vdu.dmv[MV_YWIND] - (P.py >> vdu.dmv[MV_YEIG]) -
                          (int32_t)ros_ld8(blk + PB_ACTIVEY));
        r[3] = blk;
    }
    if (vdu.screen_ok)
        vdu_graphicsv(GV_UPDATE_POINTER, r);
}

void vdu_pointer_vsync(void)
{
    poll();
    update();
}

/* SetMouseRectangle, and the pointer off. */
void vdu_pointer_mode(void)
{
    P.number = 0;
    P.xeig = vdu.dmv[MV_XEIG] - vdu.dmv[MV_LOG2BPC] + vdu.dmv[MV_LOG2BPP];
    int32_t w = (int32_t)((vdu.dmv[MV_XWIND] + 1) << vdu.dmv[MV_XEIG]);
    int32_t h = (int32_t)((vdu.dmv[MV_YWIND] + 1) << vdu.dmv[MV_YEIG]);
    P.mx = (int32_t)((uint32_t)w >> 1), P.my = (int32_t)((uint32_t)h >> 1);
    flush_mouse();
    mouse_box(0, 0, w - 1, h - 1);
}

void vdu_pointer_init(void)
{
    uint8_t *m = ros_rma_alloc(6 * (PB_SIZE + SHAPE_BYTES));
    memset(m, 0, 6 * (PB_SIZE + SHAPE_BYTES));
    P.blocks = ros_addr(m);
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t blk = P.blocks + i * PB_SIZE;
        ros_st32(blk + PB_BUFF, P.blocks + 6 * PB_SIZE + i * SHAPE_BYTES);
        if (i < 4)
            P.shape[i] = blk;
        else
            P.hold[i - 4] = blk;
    }
    P.xmult = P.ymult = 1;              /* MouseStep's CMOS default */
    P.bl = P.bb = -0x8000, P.br = P.bt = 0x7FFF;
    ros_vector_claim_native(POINTERV, pointer_v, 0);
}
