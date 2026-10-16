/* icons.c: icons.  Each window has an array of 32-byte icon blocks, and
 * this file holds the SWIs that make, change and read them.
 *
 * The array keeps exactly the blocks that tasks gave, without parsing
 * them.  A deleted icon keeps its slot with bit 23 set, unless it is at
 * the end of the array.  Drawing is done in draw.c.  Wimp_GetRectangle
 * and the in-place update of Wimp_SetIconState both call wimp_draw_icons,
 * which draws over one rectangle. */
#include <string.h>

#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

#define IF_TEXT    (1u << 0)
#define IF_SPRITE  (1u << 1)
#define IF_FILLED  (1u << 5)
#define IF_HELP    (1u << 7)
#define IF_DELETED (1u << 23)

static uint32_t rd(const uint8_t *b, unsigned off)
{
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

uint8_t *wimp_icon(const struct wimp_window *win, uint32_t i)
{
    return i < win->nicons ? (uint8_t *)ros_ptr(win->icons) + 32 * i : NULL;
}

/* Make the array n slots long.  Returns 0 or an error. */
static os_error *resize_array(struct wimp_window *win, uint32_t n)
{
    if (n == win->nicons)
        return NULL;
    if (n == 0) {
        if (win->icons)
            xos_module_free(ros_ptr(win->icons));
        win->icons = 0;
        win->nicons = 0;
        return NULL;
    }
    void *mem;
    os_error *e = xos_module_claim(32 * n, &mem);
    if (e)
        return e;
    if (win->icons) {
        memcpy(mem, ros_ptr(win->icons), 32 * (n < win->nicons ? n : win->nicons));
        xos_module_free(ros_ptr(win->icons));
    }
    win->icons = ros_addr(mem);
    win->nicons = n;
    return NULL;
}

/* Wimp_CreateWindow's icons: the blocks after the window block */
os_error *wimp_icons_from_block(struct wimp_window *win, uint32_t block, uint32_t n)
{
    os_error *e = resize_array(win, n);
    if (!e && n)
        memcpy(ros_ptr(win->icons), ros_ptr(block), 32 * n);
    return e;
}

/* Set a menu level's icons, from host memory. */
os_error *wimp_icons_set(struct wimp_window *win, const uint8_t *blocks, uint32_t n)
{
    os_error *e = resize_array(win, n);
    if (!e && n)
        memcpy(ros_ptr(win->icons), blocks, 32 * n);
    return e;
}

void wimp_icons_free(struct wimp_window *win)
{
    resize_array(win, 0);
}

static struct wimp_window *window_of(struct ros_cpu *s, uint32_t handle)
{
    struct wimp_window *win = handle == 0xFFFFFFFFu ? wimp_window_ib(0xFFFFFFFEu) : wimp_window_ib(handle);
    if (!win)
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
    return win;
}

/* Put an icon in the window's lowest deleted slot, or in a new slot at
 * the end.  The slot used is returned in *slot. */
os_error *wimp_icon_add(struct wimp_window *win, const uint8_t *block, uint32_t *slot)
{
    uint32_t k = win->nicons;
    for (uint32_t i = 0; i < win->nicons; i++)
        if (rd(wimp_icon(win, i), 16) & IF_DELETED) {
            k = i;
            break;
        }
    if (k == win->nicons) {
        os_error *e = resize_array(win, win->nicons + 1);
        if (e)
            return e;
    }
    memcpy(wimp_icon(win, k), block, 32);
    *slot = k;
    return NULL;
}

/* Delete an icon from a window.  This is the same as Wimp_DeleteIcon's
 * last step below. */
void wimp_icon_remove(struct wimp_window *win, uint32_t i)
{
    if (i + 1 < win->nicons) {
        uint8_t *b = wimp_icon(win, i);
        uint32_t f = rd(b, 16) | IF_DELETED;
        memcpy(b + 16, &f, 4);
    } else if (i + 1 == win->nicons) {
        uint32_t n = i;
        while (n > 0 && (rd(wimp_icon(win, n - 1), 16) & IF_DELETED))
            n--;
        resize_array(win, n);
    }
}

/* ---- Wimp_CreateIcon, the window form ----------------------------------- */

void wimp_swi_CreateIcon(struct ros_cpu *s)
{
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint32_t handle = ros_ld32(s->r[1]);
    if (handle >= 0xFFFFFFF8u) {                /* an icon bar icon */
        uint32_t h;
        os_error *e = wimp_iconbar_create(handle, s->r[0], s->r[1] + 4, t, &h);
        if (e) {
            wimp_fail(s, e);
            return;
        }
        s->r[0] = h;
        s->v = 0;
        return;
    }
    struct wimp_window *win = window_of(s, handle);
    if (!win)
        return;
    if (win->owner != t->handle) {
        wimp_fail(s, wimp_error(E_OWNER_WINDOW));
        return;
    }
    uint32_t slot = win->nicons;
    for (uint32_t i = 0; i < win->nicons; i++)
        if (rd(wimp_icon(win, i), 16) & IF_DELETED) {
            slot = i;
            break;
        }
    if (slot == win->nicons) {
        os_error *e = resize_array(win, win->nicons + 1);
        if (e) {
            wimp_fail(s, e);
            return;
        }
    }
    memcpy(wimp_icon(win, slot), ros_ptr(s->r[1] + 4), 32);
    wimp_input_icon_created(win, s->r[1]);     /* an icon made under the pointer */
    s->r[0] = slot;
    s->v = 0;
}

/* ---- Wimp_DeleteIcon, the window form -------------------------------------- */

void wimp_swi_DeleteIcon(struct ros_cpu *s)
{
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint32_t handle = ros_ld32(s->r[1]), i = ros_ld32(s->r[1] + 4);
    if ((int32_t)i < 0) {                       /* a negative handle does nothing */
        s->v = 0;
        return;
    }
    if (handle == 0xFFFFFFFFu || handle == 0xFFFFFFFEu) {
        wimp_iconbar_delete((int32_t)i);        /* the icon bar: no owner check */
        s->v = 0;
        return;
    }
    struct wimp_window *win = window_of(s, handle);
    if (!win)
        return;
    if (win->owner != t->handle) {
        wimp_fail(s, wimp_error(E_OWNER_WINDOW));
        return;
    }
    /* The caret and the selection in the icon are not dealt with yet. */
    wimp_clipboard_going(win->handle, (int32_t)i);      /* a drag of its text out aborted */
    if (i + 1 < win->nicons) {                  /* not the last: mark it deleted */
        uint8_t *b = wimp_icon(win, i);
        uint32_t f = rd(b, 16) | IF_DELETED;
        memcpy(b + 16, &f, 4);
    } else if (i + 1 == win->nicons) {          /* the last: drop it and deleted ones before */
        uint32_t n = i;
        while (n > 0 && (rd(wimp_icon(win, n - 1), 16) & IF_DELETED))
            n--;
        os_error *e = resize_array(win, n);
        if (e) {
            wimp_fail(s, e);
            return;
        }
    }
    s->v = 0;
}

/* ---- Wimp_SetIconState --------------------------------------------------- */

/* Returns 1 if a changed icon is redrawn in place, or 0 if its area is
 * invalidated instead. */
int wimp_icon_in_place(const struct wimp_window *win, uint32_t f)
{
    if (f & IF_HELP)
        return 0;
    if (!(f & (IF_FILLED | IF_SPRITE)))
        return 0;
    if ((f & IF_TEXT) && win->handle == wimp_ws()->iconbar)
        return 0;                               /* every text icon on the icon bar */
    return 1;
}

void wimp_swi_SetIconState(struct ros_cpu *s)
{
    wimp_clipboard_unpark();                    /* the owner acted: drop a parked DataLoad */
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint32_t b = s->r[1];
    struct wimp_window *win = window_of(s, ros_ld32(b));
    if (!win)
        return;
    uint32_t i = ros_ld32(b + 4);
    uint8_t *icon = wimp_icon(win, i);
    if (!icon) {                                /* no such icon: do nothing */
        s->v = 0;
        return;
    }
    uint32_t f = (rd(icon, 16) & ~ros_ld32(b + 12)) ^ ros_ld32(b + 8);
    memcpy(icon + 16, &f, 4);
    wimp_icon_changed(win, i, wimp_icon_in_place(win, f));
    s->v = 0;
}

/* ---- Wimp_GetIconState, Wimp_WhichIcon -------------------------------- */

void wimp_swi_GetIconState(struct ros_cpu *s)
{
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint32_t b = s->r[1];
    struct wimp_window *win = window_of(s, ros_ld32(b));
    if (!win)
        return;
    uint8_t *icon = wimp_icon(win, ros_ld32(b + 4));
    if (icon) {
        memcpy(ros_ptr(b + 8), icon, 32);
    } else {
        ros_st32(b + 24, IF_DELETED);
        ros_st32(b + 28, 13);
    }
    s->v = 0;
}

void wimp_swi_WhichIcon(struct ros_cpu *s)
{
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    if (s->r[0] == 0xFFFFFFFFu) {
        wimp_fail(s, wimp_error(E_BAD_HANDLE));
        return;
    }
    struct wimp_window *win = window_of(s, s->r[0]);
    if (!win)
        return;
    struct wimp_task *me = wimp_current();
    int iconbar = s->r[0] == 0xFFFFFFFEu;       /* icon bar: the task's own icons only */
    uint32_t out = s->r[1];
    for (uint32_t i = 0; i < win->nicons; i++)
        if ((rd(wimp_icon(win, i), 16) & s->r[2]) == s->r[3] &&
            (!iconbar || (me && wimp_iconbar_icon_task((int32_t)i) == me->internal))) {
            ros_st32(out, i);
            out += 4;
        }
    ros_st32(out, 0xFFFFFFFFu);
    s->v = 0;
}

/* ---- Wimp_ResizeIcon, with 5.30's handle check fixed ------------------------- */

void wimp_swi_ResizeIcon(struct ros_cpu *s)
{
    struct wimp_window *win = window_of(s, s->r[0]);
    if (!win)
        return;
    uint8_t *icon = wimp_icon(win, s->r[1]);    /* handles 0 to n-1, no more */
    if (!icon) {
        wimp_fail(s, ros_error(0x2A1, "Illegal icon handle"));
        return;
    }
    int32_t box[4] = { (int32_t)s->r[2], (int32_t)s->r[3], (int32_t)s->r[4], (int32_t)s->r[5] };
    memcpy(icon, box, 16);
    s->v = 0;
}
