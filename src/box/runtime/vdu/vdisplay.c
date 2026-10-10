/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* vdisplay.c -- virtual displays, and the VDU's contexts (GraphTask).
 *
 * A task can have a virtual display. This is a screen of its own, in a
 * mode of its own, which nothing else sees. While the task's own code
 * runs, the VDU drivers draw into it, and every call that reads or changes
 * the screen answers for it. Background work, callbacks and the Wimp go on
 * with the real display.
 *
 * Contexts. The VDU's state is struct vdu (vduws.h), plus a little kept
 * elsewhere. That is the changed box, the queue's parameters, the OR/EOR
 * tables and the mode selector's copy. These are the contents of RMA
 * blocks whose addresses are handed out, so their contents move, not the
 * blocks. The MOS's save area and teletext's state are also kept
 * elsewhere. A context is all of that. The real display has one
 * (real_ctx), and each virtual display has another. One is live, in the
 * drivers' workspace, and vdu.vd says which. It is NULL for the real
 * display. use() makes another live, saving the one that was. With no
 * virtual display nothing here runs (ros_vdu_displays is 0).
 *
 * Which context is live. Each task has a virtual display or none, and a
 * real depth, which counts the real sections it is inside. Its context is
 * the real display's during background work and while its depth is
 * non-zero. Otherwise its context is its own display's. It is made live
 * at each switch of the baton (ros_vdu_task_installed). It is also made
 * live on entry to and exit from a real section. Real sections are every
 * Wimp SWI, and the way out of the outermost SWI with its background work
 * and callbacks (swi.c). It is made live at each SWI that the task's own
 * code makes, which also clears a depth that a non-local exit left behind.
 * Background work swaps the real context in itself (background.c).
 *
 * A virtual display's screen is a dynamic area, "VDisplay <n>", which is
 * always mapped. It has two banks, each a sprite area of one sprite,
 * "screen", whose image is the bank's screen memory. The headers come
 * first and the two images follow each other, so the second bank starts
 * ScreenSize after the first, as on a real screen. Each sprite's image
 * offset reaches past the other's header to its image. GraphTask plots the
 * bank shown, and saves it, as it does any sprite.
 *
 * GraphicsV is never called for a virtual display (modes.c). Its mode
 * changes, palette and teletext banks stay its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vdu.h"
#include "vduws.h"
#include "sprite.h"

#define ERR_NOT_DISPLAY  0xC01C0u
#define ERR_IN_USE       0xC01C1u
#define ERR_NO_ROOM      0xC01C2u
#define ERR_ONLY_CALLER  0xC01C3u
#define ERR_BAD_SWI      0x1E6u

#define AREA_MAX   (64u << 20)          /* address space. Only touched pages are memory */
#define SAVE_SIZE  384                  /* the MOS's save area (sprout.c) */
#define SEL_BYTES  (4 * (5 + 2 * 60 + 1))       /* a selector's copy (modes.c) */
#define OS_SPRITEOP 0x2Eu

/* A context: the VDU's state, wherever it is kept */
struct ctx {
    struct vdu v;
    uint8_t mos[SAVE_SIZE];
    uint32_t cbox[5];
    uint8_t qq[16];
    uint32_t oe[48];                    /* FgEcfOraEor, BgEcfOraEor, BgEcfStore */
    uint8_t sel[SEL_BYTES];
    uint8_t ttx[VDU_TTX_STATE];
};

struct vdisplay {
    uint32_t handle;
    struct vdisplay *next;
    struct ros_task *owner;             /* its creator; NULL once that has ended */
    struct ros_task *task;              /* the task it is attached to, or NULL */
    uint32_t area, base, size;          /* its dynamic area: number, base, size in use */
    uint32_t image, screen_size;        /* the first bank's image; ScreenSize, a bank */
    uint32_t hdr[2];                    /* each bank's sprite area */
    uint32_t pal_entries;               /* in each sprite's palette */
    uint32_t drv_bank, disp_bank;       /* OS_Byte 112 and 113: 1 or 2 */
    uint32_t mode_gen;                  /* its mode changes */
    uint32_t pal_gen;                   /* its palette changes, and mode changes */
    uint32_t selcopy;                   /* RMA: its mode selector, for Info */
    struct ctx c;                       /* its context, while another is live */
    /* where GraphTask shows it (SetFocus) */
    uint32_t focus, window;
    int shown;
    int32_t rect[4];
    int32_t mx, my;                     /* the mouse, OS units in the display: the last inside */
    /* what changed (Changed) */
    uint32_t changes;
    int dirty, all;
    int32_t box[4];
};

unsigned ros_vdu_displays;
void (*ros_vdisplay_changed_hook)(void);
void (*ros_vdisplay_vsync_hook)(void);
static struct vdisplay *displays;
static uint32_t next_handle = 1;
static struct ctx real_ctx;

/* Something in a display changed. Whoever shows it (the native Wimp's
 * surface windows) is told. That only notes the change, and the Wimp draws
 * at a safe point of its own. */
static void changed_now(struct vdisplay *d)
{
    d->changes++;
    if (ros_vdisplay_changed_hook)
        ros_vdisplay_changed_hook();
}

/* ---- contexts ------------------------------------------------------------------------------ */

static void save(struct ctx *c)
{
    c->v = vdu;
    memcpy(c->mos, vdu_mos_area(), SAVE_SIZE);
    memcpy(c->cbox, vdu_cbox(), sizeof c->cbox);
    memcpy(c->qq, ros_ptr(vdu.qq), sizeof c->qq);
    memcpy(c->oe, vdu.fg_oe, sizeof c->oe);
    memcpy(c->sel, ros_ptr(vdu.selector), SEL_BYTES);
    vdu_ttx_state_save(c->ttx);
}

static void load(const struct ctx *c)
{
    uint32_t sp_area = vdu.sp_area;     /* the system sprite area belongs to the machine */
    vdu = c->v;
    vdu.sp_area = sp_area;
    memcpy(vdu_mos_area(), c->mos, SAVE_SIZE);
    memcpy(vdu_cbox(), c->cbox, sizeof c->cbox);
    memcpy(ros_ptr(vdu.qq), c->qq, sizeof c->qq);
    memcpy(vdu.fg_oe, c->oe, sizeof c->oe);
    memcpy(ros_ptr(vdu.selector), c->sel, SEL_BYTES);
    vdu_ttx_state_load(c->ttx);
}

static struct ctx *ctx_of(struct vdisplay *d)
{
    return d ? &d->c : &real_ctx;
}

/* Makes d's context live. NULL means the real display's. */
static void use(struct vdisplay *d)
{
    if (vdu.vd == d)
        return;
    save(ctx_of(vdu.vd));
    load(ctx_of(d));
}

/* The context that the current task should have now. */
static struct vdisplay *effective(void)
{
    struct ros_task *t = ros_task_current();
    if (!t || ros_in_background() || *ros_task_vdu_real(t))
        return NULL;
    return ros_task_vdisplay(t);
}

static void settle(void)
{
    use(effective());
}

void ros_vdu_task_installed(void)
{
    settle();
}

/* A drawing SWI whose drawing the VDU does not see. The Font Manager's,
 * Draw's, SpriteExtend's (OS_SpriteOp) and JPEG's are such SWIs. All of the
 * display may have changed. */
static int unseen_drawing(uint32_t n)
{
    uint32_t chunk = n & ~0x3Fu;
    return n == OS_SPRITEOP || chunk == 0x40080u || chunk == 0x40540u || chunk == 0x49980u;
}

int ros_vdu_swi_enter(uint32_t n, int user)
{
    struct ros_task *t = ros_task_current();
    if (!t)
        return 0;
    unsigned *depth = ros_task_vdu_real(t);
    if (user)
        *depth = 0;                     /* the task's own code is in no real section */
    if (n >= 0x400C0u && n < 0x40100u && !ros_in_background()) {
        (*depth)++;                     /* the Wimp uses the real display */
        settle();
        return 1;
    }
    if (user) {
        settle();
        if (vdu.vd && unseen_drawing(n)) {
            vdu.vd->all = vdu.vd->dirty = 1;
            changed_now(vdu.vd);
        }
    }
    return 0;
}

int ros_vdu_real_enter(void)
{
    struct ros_task *t = ros_task_current();
    if (!t || ros_in_background())
        return 0;
    (*ros_task_vdu_real(t))++;
    settle();
    return 1;
}

void ros_vdu_real_leave(int entered)
{
    if (!entered)
        return;
    struct ros_task *t = ros_task_current();
    if (t) {
        unsigned *depth = ros_task_vdu_real(t);
        if (*depth)
            (*depth)--;
    }
    settle();
}

void *ros_vdu_background_enter(void)
{
    struct vdisplay *was = vdu.vd;
    use(NULL);
    return was;
}

void ros_vdu_background_leave(void *was)
{
    use(was);
}

uint32_t ros_vdu_context_id(void)
{
    struct vdisplay *d = vdu.vd;
    return d ? d->handle << 16 | (d->mode_gen & 0xFFFFu) : 0;
}

void ros_vdu_output(uint32_t *select, uint32_t *area, uint32_t *sprite)
{
    *select = vdu.dest_select;
    *area = vdu.dest_area;
    *sprite = vdu.dest_sprite;
}

int ros_vdu_keys_blocked(void)
{
    return vdu.vd && !vdu.vd->focus;
}

/* ---- the screen ---------------------------------------------------------------------------- */

static os_error *err(uint32_t n)
{
    switch (n) {
    case ERR_NOT_DISPLAY: return ros_error(n, "Not a virtual display");
    case ERR_IN_USE:      return ros_error(n, "Virtual display in use");
    case ERR_NO_ROOM:     return ros_error(n, "No room for the virtual display");
    case ERR_ONLY_CALLER: return ros_error(n, "Only the calling task can be given a virtual display");
    default:              return ros_error(ERR_BAD_SWI, "SWI value out of range for module VDisplay");
    }
}

/* The sprite mode word of a mode's variables. It has a new-format type and
 * a dpi. */
static uint32_t sprite_mode(const uint32_t mv[MV_COUNT])
{
    static const uint32_t types[6] = { 1, 2, 3, 4, 5, 6 };
    uint32_t l2 = mv[MV_LOG2BPP] > 5 ? 5 : mv[MV_LOG2BPP];
    uint32_t type = types[l2];
    if (l2 == 4 && mv[MV_NCOLOUR] == 0xFFFF)
        type = 10;                      /* 64K colours: 5:6:5 */
    uint32_t xdpi = 180u >> mv[MV_XEIG], ydpi = 180u >> mv[MV_YEIG];
    return type << 27 | ydpi << 14 | xdpi << 1 | 1;
}

static int resize(struct vdisplay *d, uint32_t bytes)
{
    bytes = (bytes + 4095u) & ~4095u;
    if (bytes == d->size)
        return 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = d->area;
    c.r[1] = bytes - d->size;           /* signed: a shrink is negative */
    ros_swi(&c, XOS_ChangeDynamicArea);
    if (c.v)
        return -1;
    d->size = bytes;
    return 0;
}

os_error *vdisplay_framestore(const uint32_t mv[MV_COUNT], uint32_t w, uint32_t h,
                              uint32_t *start, uint32_t *total)
{
    struct vdisplay *d = vdu.vd;
    uint32_t ll = mv[MV_LINELENGTH], ss = mv[MV_SCREENSIZE], l2 = mv[MV_LOG2BPP];
    uint32_t npal = l2 <= 3 ? 1u << (1u << l2) : 0;
    uint32_t one = (16 + 44 + 8 * npal + 15) & ~15u;    /* a bank's headers and palette */
    uint32_t head = (2 * one + 4095) & ~4095u;
    if (!d || ss > (AREA_MAX - head) / 2 || h == 0 || ll == 0)
        return err(ERR_NO_ROOM);
    if (resize(d, head + 2 * ss) != 0)
        return err(ERR_NO_ROOM);
    memset(ros_ptr(d->base), 0, head + 2 * ss);
    uint32_t mode = sprite_mode(mv);
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t a = d->base + k * one, sp = a + 16, img = d->base + head + k * ss;
        uint32_t size = (img - sp) + ll * h;
        ros_st32(a + SA_END, 16 + size);
        ros_st32(a + SA_NUMBER, 1);
        ros_st32(a + SA_FIRST, 16);
        ros_st32(a + SA_FREE, 16 + size);
        ros_st32(sp + SP_NEXT, size);
        memcpy(ros_ptr(sp + SP_NAME), "screen\0\0\0\0\0\0", 12);
        ros_st32(sp + SP_WIDTH, ll / 4 - 1);
        ros_st32(sp + SP_HEIGHT, h - 1);
        ros_st32(sp + SP_LBIT, 0);
        ros_st32(sp + SP_RBIT, ((w << l2) - 1) & 31);
        ros_st32(sp + SP_IMAGE, img - sp);
        ros_st32(sp + SP_TRANS, img - sp);
        ros_st32(sp + SP_MODE, mode);
        d->hdr[k] = a;
    }
    d->image = d->base + head;
    d->screen_size = ss;
    d->pal_entries = npal;
    d->drv_bank = d->disp_bank = 1;
    *start = d->image;
    *total = 2 * ss;
    return NULL;
}

void vdisplay_mode_changed(void)
{
    struct vdisplay *d = vdu.vd;
    d->mode_gen++;
    d->pal_gen++;
    d->all = d->dirty = 1;
    changed_now(d);
    d->mx = (int32_t)((vdu.dmv[MV_XWIND] + 1) << vdu.dmv[MV_XEIG]) / 2;
    d->my = (int32_t)((vdu.dmv[MV_YWIND] + 1) << vdu.dmv[MV_YEIG]) / 2;
}

void vdisplay_dirty(int32_t l, int32_t b, int32_t r, int32_t t)
{
    struct vdisplay *d = vdu.vd;
    int32_t xw = (int32_t)vdu.mv[MV_XWIND], yw = (int32_t)vdu.mv[MV_YWIND];
    if (d->drv_bank != d->disp_bank)
        return;                         /* the bank is not shown. It is seen when OS_Byte 113 shows it */
    if (l < 0) l = 0;
    if (b < 0) b = 0;
    if (r > xw) r = xw;
    if (t > yw) t = yw;
    if (r < l || t < b)
        return;
    if (!d->dirty) {
        d->box[0] = l, d->box[1] = b, d->box[2] = r, d->box[3] = t;
    } else {
        if (l < d->box[0]) d->box[0] = l;
        if (b < d->box[1]) d->box[1] = b;
        if (r > d->box[2]) d->box[2] = r;
        if (t > d->box[3]) d->box[3] = t;
    }
    d->dirty = 1;
    changed_now(d);
}

void vdisplay_palette_changed(void)
{
    struct vdisplay *d = vdu.vd;
    d->pal_gen++;
    d->all = d->dirty = 1;
    changed_now(d);
}

uint32_t vdisplay_display_start(void)
{
    struct vdisplay *d = vdu.vd;
    return d->image + (d->disp_bank - 1) * d->screen_size;
}

/* OS_Byte 112 (the bank written) and 113 (the bank shown), as the kernel's
 * DoSetDriverBank and DoSetDisplayBank. 0 means bank 1, a bank past the
 * second changes nothing, and R1 returns the old one. */
int vdisplay_byte(struct ros_cpu *s)
{
    struct vdisplay *d = vdu.vd;
    uint32_t bank = s->r[1] & 0xFF;
    if (bank == 0)
        bank = 1;
    if (s->r[0] == 112) {
        uint32_t old = d->drv_bank;
        if (bank <= 2 && bank != old) {
            vdu_pre_wrch();
            d->drv_bank = bank;
            uint32_t a = d->image + (bank - 1) * d->screen_size;
            vdu.dscreen_start = a;
            if (!vdu.dest_sprite) {
                vdu.screen_start = a;
                vdu.screen = ros_ptr(a);
            }
            vdu_post_wrch();
        }
        s->r[1] = old;
    } else {
        uint32_t old = d->disp_bank;
        if (bank <= 2 && bank != old) {
            d->disp_bank = bank;
            d->all = d->dirty = 1;
            changed_now(d);
        }
        s->r[1] = old;
    }
    return 1;
}

/* The mouse, in OS units on the screen, is mapped into the display while
 * it is over the rectangle that GraphTask shows it in. Outside the
 * rectangle the position is the last one inside. The buttons are given
 * only with the input focus. */
void vdisplay_mouse(int32_t *x, int32_t *y, uint32_t *buttons)
{
    struct vdisplay *d = vdu.vd;
    int32_t w = (int32_t)((vdu.dmv[MV_XWIND] + 1) << vdu.dmv[MV_XEIG]);
    int32_t h = (int32_t)((vdu.dmv[MV_YWIND] + 1) << vdu.dmv[MV_YEIG]);
    int32_t *r = d->rect;
    if (d->shown && *x >= r[0] && *x < r[2] && *y >= r[1] && *y < r[3]) {
        d->mx = (int32_t)((int64_t)(*x - r[0]) * w / (r[2] - r[0]));
        d->my = (int32_t)((int64_t)(*y - r[1]) * h / (r[3] - r[1]));
    }
    *x = d->mx;
    *y = d->my;
    if (!d->focus)
        *buttons = 0;
}

/* Each attached display's flashing, on the VSync. This is background work
 * with the real display live. It covers the display's cursor, palette and
 * teletext banks. */
void vdisplay_vsync(void)
{
    if (!ros_vdu_displays)
        return;
    struct vdisplay *was = vdu.vd;
    for (struct vdisplay *d = displays; d; d = d->next) {
        if (!d->task)
            continue;
        use(d);
        uint32_t before = vdu.cursor_flags & CF_ACTUAL;
        vdu_flash_vsync();
        if ((vdu.cursor_flags & CF_ACTUAL) != before && !vdu.dest_sprite) {
            int32_t l = vdu.cx * (int32_t)vdu.tchar_x;
            int32_t t = (int32_t)vdu.mv[MV_YWIND] - vdu.cy * (int32_t)vdu.row_mult;
            if (vdu.cursor_flags & CF_TELETEXT)
                vdisplay_dirty(0, 0, (int32_t)vdu.mv[MV_XWIND], (int32_t)vdu.mv[MV_YWIND]);
            else
                vdisplay_dirty(l, t - (int32_t)vdu.row_mult + 1, l + (int32_t)vdu.tchar_x - 1, t);
        }
    }
    use(was);
    if (ros_vdisplay_vsync_hook)
        ros_vdisplay_vsync_hook();      /* The surface windows are shown, if that is safe. */
}

/* ---- displays ------------------------------------------------------------------------------ */

static struct vdisplay *find(uint32_t handle)
{
    for (struct vdisplay *d = displays; d; d = d->next)
        if (d->handle == handle && handle)
            return d;
    return NULL;
}

static void remove_area(uint32_t area)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 1, c.r[1] = area;
    ros_swi(&c, XOS_DynamicArea);
}

static void destroy(struct vdisplay *d)
{
    for (struct vdisplay **p = &displays; *p; p = &(*p)->next)
        if (*p == d) {
            *p = d->next;
            break;
        }
    if (vdu.vd == d)
        use(NULL);
    vdu_ttx_state_free(d->c.ttx);
    remove_area(d->area);
    if (d->selcopy)
        ros_rma_free(ros_ptr(d->selcopy));
    free(d);
    ros_vdu_displays--;
}

static os_error *create(uint32_t *r)
{
    uint32_t mode = r[1] == 0xFFFFFFFFu ? 28 : r[1];
    struct vdisplay *d = calloc(1, sizeof *d);
    char *name = ros_rma_alloc(32);
    if (!d || !name) {
        free(d);
        if (name)
            ros_rma_free(name);
        return err(ERR_NO_ROOM);
    }
    d->handle = next_handle++;
    snprintf(name, 32, "VDisplay %u", d->handle);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0xFFFFFFFFu, c.r[2] = 0, c.r[3] = 0xFFFFFFFFu;
    c.r[4] = 0x80;                      /* user read/write, not draggable */
    c.r[5] = AREA_MAX, c.r[6] = 0, c.r[7] = 0, c.r[8] = ros_addr(name);
    ros_swi(&c, XOS_DynamicArea);
    ros_rma_free(name);
    if (c.v) {
        free(d);
        return err(ERR_NO_ROOM);
    }
    d->area = c.r[1];
    d->base = c.r[3];
    d->selcopy = ros_addr(ros_rma_alloc(SEL_BYTES));
    d->owner = ros_task_current();

    /* Its context. The live one's is used as a base and made the VDU's as it
     * starts. It has no teletext map and no save area, it uses the hard
     * font, and output goes to the screen. Then the mode is set in it, as a
     * mode change sets it. */
    save(&d->c);
    memset(d->c.ttx, 0, sizeof d->c.ttx);
    memset(d->c.mos, 0, sizeof d->c.mos);
    memset(d->c.cbox, 0, sizeof d->c.cbox);
    memset(d->c.qq, 0, sizeof d->c.qq);
    struct vdu *v = &d->c.v;
    v->vd = d;
    v->status = 0;
    v->cursor_flags = 0;
    v->cur_stack = 0;
    v->blanked = 0;
    v->flash_state = 0;
    v->dest_select = 0x23C, v->dest_area = v->dest_sprite = 0, v->save_area = 1;
    memcpy(v->font, ros_vdu_hard_font, sizeof v->font);
    d->drv_bank = d->disp_bank = 1;
    d->next = displays;
    displays = d;
    ros_vdu_displays++;

    struct vdisplay *was = vdu.vd;
    use(d);
    vdu_pre_wrch();
    os_error *e = vdu_set_mode(mode);
    vdu_post_wrch();
    use(was);
    if (e) {
        os_error keep = *e;
        destroy(d);
        return ros_error(keep.errnum, "%s", keep.errmess);
    }
    r[0] = d->handle;
    r[1] = d->area;
    return NULL;
}

static os_error *destroy_swi(uint32_t *r)
{
    struct vdisplay *d = find(r[0]);
    if (!d)
        return err(ERR_NOT_DISPLAY);
    if (d->task)
        return err(ERR_IN_USE);
    destroy(d);
    return NULL;
}

static os_error *attach(uint32_t *r)
{
    if (r[1] != 0)
        return err(ERR_ONLY_CALLER);
    struct ros_task *t = ros_task_current();
    struct vdisplay *d = NULL;
    if (r[0]) {
        if (!(d = find(r[0])))
            return err(ERR_NOT_DISPLAY);
        if (d->task && d->task != t)
            return err(ERR_IN_USE);
    }
    struct vdisplay *prev = ros_task_vdisplay(t);
    if (prev && prev != d)
        prev->task = NULL;
    if (d)
        d->task = t;
    ros_task_set_vdisplay(t, d);
    settle();
    r[0] = prev ? prev->handle : 0;
    return NULL;
}

/* A context's own state, whether live or kept. */
static const struct vdu *state(struct vdisplay *d)
{
    return vdu.vd == d ? &vdu : &d->c.v;
}

/* Brings each bank's sprite palette up to date from the context's. */
static void bank_palettes(struct vdisplay *d)
{
    const struct vdu *v = state(d);
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t pal = d->hdr[k] + 16 + 44;
        for (uint32_t i = 0; i < d->pal_entries; i++) {
            ros_st32(pal + 8 * i, v->pal[0][i] & 0xFFFFFF00u);
            ros_st32(pal + 8 * i + 4, v->pal[1][i] & 0xFFFFFF00u);
        }
    }
}

static os_error *info(uint32_t *r)
{
    struct vdisplay *d = find(r[0]);
    if (!d)
        return err(ERR_NOT_DISPLAY);
    const struct vdu *v = state(d);
    bank_palettes(d);
    uint32_t mode = v->dmode_no;
    if (mode >= 256) {                          /* its selector, wherever it is kept */
        memcpy(ros_ptr(d->selcopy), vdu.vd == d ? ros_ptr(vdu.selector) : d->c.sel, SEL_BYTES);
        mode = d->selcopy;
    }
    r[0] = mode;
    r[1] = d->hdr[d->disp_bank - 1];
    r[2] = r[1] + 16;
    r[3] = v->dmv[MV_XWIND] + 1;
    r[4] = v->dmv[MV_YWIND] + 1;
    r[5] = v->dmv[MV_XEIG];
    r[6] = v->dmv[MV_YEIG];
    r[7] = d->changes;
    return NULL;
}

static os_error *set_focus(uint32_t *r)
{
    struct vdisplay *d = find(r[0]);
    if (!d)
        return err(ERR_NOT_DISPLAY);
    d->focus = r[1] & 1;
    d->window = r[2];
    d->shown = 0;
    if (r[3]) {
        for (int i = 0; i < 4; i++)
            d->rect[i] = (int32_t)ros_ld32(r[3] + 4 * (uint32_t)i);
        d->shown = d->rect[2] > d->rect[0] && d->rect[3] > d->rect[1];
    }
    return NULL;
}

static os_error *changed(uint32_t *r)
{
    struct vdisplay *d = find(r[0]);
    if (!d)
        return err(ERR_NOT_DISPLAY);
    const struct vdu *v = state(d);
    uint32_t flags = (d->dirty ? 1u : 0) | (d->all ? 2u : 0);
    if (d->all) {
        d->box[0] = d->box[1] = 0;
        d->box[2] = (int32_t)v->dmv[MV_XWIND], d->box[3] = (int32_t)v->dmv[MV_YWIND];
    }
    uint32_t clear = r[1] & 1;
    r[0] = flags;
    r[1] = d->changes;
    for (int i = 0; i < 4; i++)
        r[2 + i] = (uint32_t)(d->dirty ? d->box[i] : 0);
    if (clear)
        d->dirty = d->all = 0;
    return NULL;
}

/* SetMode. The display's mode is changed from outside, by the task that
 * shows it (GraphTask's Mode menu), as the program's own MODE would change
 * it. */
static os_error *set_mode(uint32_t *r)
{
    struct vdisplay *d = find(r[0]);
    if (!d)
        return err(ERR_NOT_DISPLAY);
    uint32_t mode = r[1] == 0xFFFFFFFFu ? 28 : r[1];
    struct vdisplay *was = vdu.vd;
    use(d);
    vdu_pre_wrch();
    os_error *e = vdu_set_mode(mode);
    vdu_post_wrch();
    use(was);
    settle();
    return e;
}

/* ---- for the Wimp's surface windows ------------------------------------------------------- */

int ros_vdisplay_view(uint32_t handle, struct ros_vdisplay_view *out)
{
    struct vdisplay *d = find(handle);
    if (!d)
        return -1;
    const struct vdu *v = state(d);
    if (out->pal_gen != d->pal_gen || out->area != d->hdr[d->disp_bank - 1])
        bank_palettes(d);
    out->area = d->hdr[d->disp_bank - 1];
    out->sprite = out->area + 16;
    out->width = v->dmv[MV_XWIND] + 1;
    out->height = v->dmv[MV_YWIND] + 1;
    out->xeig = v->dmv[MV_XEIG];
    out->yeig = v->dmv[MV_YEIG];
    out->log2bpp = v->dmv[MV_LOG2BPP];
    out->mode_gen = d->mode_gen;
    out->pal_gen = d->pal_gen;
    out->pal_entries = d->pal_entries;
    return 0;
}

/* The live context is kept whole and put back, around drawing that
 * background work does on the real display (the Wimp's surface windows at
 * the VSync). Whatever was in the middle of using the VDU finds it as it
 * was. That includes a queue half filled, a graphics window and colours. */
static struct ctx snapshot;

void ros_vdu_snapshot_take(void)
{
    save(&snapshot);
}

void ros_vdu_snapshot_restore(void)
{
    load(&snapshot);
}

int ros_vdisplay_take(uint32_t handle, int32_t box[4])
{
    struct vdisplay *d = find(handle);
    if (!d || !d->dirty)
        return d ? 0 : -1;
    const struct vdu *v = state(d);
    int flags = 1 | (d->all ? 2 : 0);
    if (d->all) {
        box[0] = box[1] = 0;
        box[2] = (int32_t)v->dmv[MV_XWIND], box[3] = (int32_t)v->dmv[MV_YWIND];
    } else {
        memcpy(box, d->box, sizeof d->box);
    }
    d->dirty = d->all = 0;
    return flags;
}

/* ---- the module ---------------------------------------------------------------------------- */

static void dispatch(struct ros_cpu *s, unsigned which)
{
    static os_error *(*const fn[])(uint32_t *) = { create, destroy_swi, attach, info,
                                                   set_focus, changed, set_mode };
    os_error *e = which < sizeof fn / sizeof fn[0] ? fn[which](s->r) : err(ERR_BAD_SWI);
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

void ros_thunk_VDisplay_Create(struct ros_cpu *s) { dispatch(s, 0); }
void ros_thunk_VDisplay_Destroy(struct ros_cpu *s) { dispatch(s, 1); }
void ros_thunk_VDisplay_Attach(struct ros_cpu *s) { dispatch(s, 2); }
void ros_thunk_VDisplay_Info(struct ros_cpu *s) { dispatch(s, 3); }
void ros_thunk_VDisplay_SetFocus(struct ros_cpu *s) { dispatch(s, 4); }
void ros_thunk_VDisplay_Changed(struct ros_cpu *s) { dispatch(s, 5); }
void ros_thunk_VDisplay_SetMode(struct ros_cpu *s) { dispatch(s, 6); }

/* A task's end. The task's display is detached. A display that the task
 * created is destroyed if no other task has attached it. If another task
 * has attached it, it is destroyed when that task ends. */
static void task_ended(struct ros_task *t)
{
    struct vdisplay *mine = ros_task_vdisplay(t);
    if (mine) {
        mine->task = NULL;
        ros_task_set_vdisplay(t, NULL);
        settle();
    }
    for (struct vdisplay *d = displays, *next; d; d = next) {
        next = d->next;
        if (d->owner == t)
            d->owner = NULL;
        if (!d->owner && !d->task)
            destroy(d);
    }
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    ros_task_on_end(task_ended);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m;
    for (struct vdisplay *d = displays; d; d = d->next)
        if (d->task && !fatal)
            return err(ERR_IN_USE);
    while (displays) {
        if (displays->task)
            ros_task_set_vdisplay(displays->task, NULL);
        displays->task = NULL;
        destroy(displays);
    }
    settle();
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return err(ERR_BAD_SWI);
}

struct ros_module vdisplay_module = {
    .title = "VDisplay",
    .help = "VDisplay\t1.00 (03 Oct 2026) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0xC01C0,
    .swi_thunks = ros_swi_thunks_VDisplay,
    .swi_names = ros_swi_names_VDisplay,
    .swi_prefix = "VDisplay",
};

__attribute__((constructor)) static void count(void)
{
    vdisplay_module.swi_count = ros_swi_count_VDisplay;
}
