/* spin.c -- the Task Manager's cog turns when it is clicked (design 35).
 *
 * A click on an icon bar icon whose sprite is the cog ("taskmanager", or
 * "switcher", its older name) still goes to its task as any click does;
 * the Wimp also turns the cog for SPIN_CS: while it does, sprite_find
 * (draw.c) gives the icon the frame for the moment -- cogframe1 to
 * cogframe7, the cog turned on by an eighth of a tooth each (tools/
 * desktopart.py, in the ROM's sprites and the alpha icon set) -- and the
 * redraw scan redraws the icon every FRAME_CS.  The turn slows to a stop,
 * TURN_FRAMES frames in all.  A theme or sprite file without the frames
 * leaves the cog still: the plain sprite is drawn.
 */
#include <stdint.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/ticker.h"
#include "wimp.h"

#define IF_TEXT     (1u << 0)
#define IF_SPRITE   (1u << 1)
#define IF_INDIRECT (1u << 8)

#define SPIN_CS     500                 /* how long it turns */
#define FRAME_CS    4                   /* how often it is redrawn while it does */
#define FRAMES      8                   /* the frames of a tooth's turn: the plain cog and cogframe1-7 */
#define TURN_FRAMES 48                  /* the frames it turns through: six teeth, three-quarters of a turn */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Whether the name (to a control character or 12 characters) is the cog's */
static int cog_name(uint32_t p)
{
    static const char *const names[] = { "taskmanager", "switcher" };
    for (unsigned k = 0; k < 2; k++) {
        const char *n = names[k];
        unsigned i = 0;
        for (; n[i]; i++)
            if (lower((int)ros_ld8(p + i)) != n[i])
                break;
        if (!n[i] && ros_ld8(p + i) < 33)
            return 1;
    }
    return 0;
}

/* The address of an icon's sprite name: a sprite-only icon's own, or a
 * text-and-sprite icon's validation string's S command; 0 for none */
static uint32_t icon_sprite_name(const struct wimp_window *win, uint32_t i)
{
    const uint8_t *ic = wimp_icon(win, i);
    if (!ic)
        return 0;
    uint32_t f, d0, d1;
    memcpy(&f, ic + 16, 4);
    memcpy(&d0, ic + 20, 4);
    memcpy(&d1, ic + 24, 4);
    if (!(f & IF_SPRITE))
        return 0;
    if (!(f & IF_INDIRECT))
        return ros_addr((void *)(ic + 20));
    if (!(f & IF_TEXT))
        return d0;
    if (!d1 || d1 == 0xFFFFFFFFu)
        return 0;
    for (uint32_t p = d1; ros_ld8(p) >= 32; p++)        /* the validation string: ...;S<name>;... */
        if ((p == d1 || ros_ld8(p - 1) == ';') && lower((int)ros_ld8(p)) == 's')
            return p + 1;
    return 0;
}

/* A click on an icon bar icon: the cog's starts it turning */
void wimp_spin_click(const struct wimp_window *win, int32_t icon)
{
    struct wimp_ws *w = wimp_ws();
    if (!win || win->handle != w->iconbar || icon < 0)
        return;
    uint32_t name = icon_sprite_name(win, (uint32_t)icon);
    if (!name || !cog_name(name))
        return;
    w->spin_start = ros_monotonic_cs();
    w->spin_icon = icon;
    w->spin_on = 1;
    w->spin_shown = 0xFFFFFFFFu;
}

/* The frame for now, 0 to FRAMES - 1: TURN_FRAMES in all, slowing to a stop */
static uint32_t frame_now(void)
{
    const struct wimp_ws *w = wimp_ws();
    uint32_t t = ros_monotonic_cs() - w->spin_start;
    if (t >= SPIN_CS)
        return 0;
    /* u from 0 to 1; the distance turned 2u - u^2 of the whole, so it slows evenly */
    uint32_t u1000 = t * 1000 / SPIN_CS;
    uint32_t done = TURN_FRAMES * (2000 * u1000 - u1000 * u1000) / 1000000;
    return done % FRAMES;
}

/* sprite_find's lookup name, for an icon bar icon: while the cog turns, the
 * cog's name becomes the frame's ("cogframe3"); 1 if it did */
int wimp_spin_name(const struct wimp_window *win, char *name, size_t size)
{
    const struct wimp_ws *w = wimp_ws();
    if (!w->spin_on || !win || win->handle != w->iconbar || !cog_name(ros_addr(name)))
        return 0;
    uint32_t f = frame_now();
    if (!f)
        return 0;
    if (size < 10)
        return 0;
    memcpy(name, "cogframe", 8);
    name[8] = (char)('0' + f);
    name[9] = 0;
    return 1;
}

/* From the redraw scan: while the cog turns, its icon is redrawn when its
 * frame changes; when the turn is over, once more, still */
void wimp_spin_tick(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w->spin_on)
        return;
    struct wimp_window *bar = wimp_window(w->iconbar);
    const uint8_t *ic = bar ? wimp_icon(bar, (uint32_t)w->spin_icon) : NULL;
    if (!ic || !bar->s[APP].open) {
        w->spin_on = 0;
        return;
    }
    uint32_t now = ros_monotonic_cs();
    uint32_t f = frame_now();
    if (now - w->spin_start >= SPIN_CS)
        w->spin_on = 0;                         /* over: drawn once more, still */
    else if (f == w->spin_shown || now - w->spin_last < FRAME_CS)
        return;
    w->spin_shown = f;
    w->spin_last = now;
    int32_t b[4];
    memcpy(b, ic, 16);
    int32_t ox = wimp_origin_x(bar, APP), oy = wimp_origin_y(bar, APP);
    wimp_invalidate_box((struct wimp_box){ ox + b[0], oy + b[1], ox + b[2], oy + b[3] });
}
