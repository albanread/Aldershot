/* screen.c: the mode, the palette, and the drawing the Wimp does itself.
 * That drawing is graphics windows, colours, clears and block copies.
 *
 * For now the drawing goes through the kernel's VDU and ColourTrans SWIs,
 * as the translated Wimp's did. The plan is for it to call the renderer's
 * C entry points directly once it draws to the pixel. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

/* The default Wimp palette: colours 0-15, as &BBGGRR00 */
const uint32_t wimp_default_palette[16] = {
    0xFFFFFF00u, 0xDDDDDD00u, 0xBBBBBB00u, 0x99999900u, 0x77777700u, 0x55555500u,
    0x33333300u, 0x00000000u, 0x99440000u, 0x00EEEE00u, 0x00CC0000u, 0x0000DD00u,
    0xBBEEEE00u, 0x00885500u, 0x00BBFF00u, 0xFFBB0000u,
};

/* The border and pointer colours 1-3 */
const uint32_t wimp_default_others[4] = { 0x00000000u, 0xFFFF0000u, 0x99000000u, 0x0000FF00u };

static uint32_t call(uint32_t swi, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                     uint32_t r4, uint32_t *out)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = r0, c.r[1] = r1, c.r[2] = r2, c.r[3] = r3, c.r[4] = r4;
    ros_swi(&c, swi);
    if (out)
        memcpy(out, c.r, 8 * sizeof c.r[0]);
    return c.v;
}

/* ---- the mode -------------------------------------------------------------- */

void wimp_mode_refresh(void)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t r[8];
    w->xeig = !call(XOS_ReadModeVariable, 0xFFFFFFFFu, 4, 0, 0, 0, r) ? r[2] : 1;
    w->yeig = !call(XOS_ReadModeVariable, 0xFFFFFFFFu, 5, 0, 0, 0, r) ? r[2] : 1;
    w->log2bpp = !call(XOS_ReadModeVariable, 0xFFFFFFFFu, 9, 0, 0, 0, r) ? r[2] : 5;
    uint32_t xw = !call(XOS_ReadModeVariable, 0xFFFFFFFFu, 11, 0, 0, 0, r) ? r[2] : 639;
    uint32_t yw = !call(XOS_ReadModeVariable, 0xFFFFFFFFu, 12, 0, 0, 0, r) ? r[2] : 511;
    w->dx = 1 << w->xeig;
    w->dy = 1 << w->yeig;
    w->screen_w = (int32_t)((xw + 1) << w->xeig);
    w->screen_h = (int32_t)((yw + 1) << w->yeig);
}

/* Room in the RMA for a translation table of at least size bytes. This
 * is the block at *at if it is big enough, or else a new one, as with
 * 5.30's pixtable_at and tpixtable_at. Returns 0 if there is no room. A
 * 32 bpp sprite plotted on a palette screen needs ColourTrans's 32K-entry
 * table, which is far too big for a fixed buffer. */
uint32_t wimp_table_space(uint32_t *at, uint32_t *have, uint32_t size)
{
    if (*at && size <= *have)
        return *at;
    if (*at)
        xos_module_free(ros_ptr(*at));
    *at = 0, *have = 0;
    void *b;
    if (!size || xos_module_claim(size, &b))
        return 0;
    *at = ros_addr(b), *have = size;
    return *at;
}

/* As read_current_configd_mode. currentmode is set to the configured
 * mode. */
void wimp_read_mode(void)
{
    uint32_t r[8];
    wimp_ws()->mode = !call(XOS_ReadSysInfo, 1, 0, 0, 0, 0, r) ? r[0] : 0xFFFFFFFFu;
}

/* As switchingtosprite_recache. If the mode variables, dx and dy were
 * read for another output destination, read them again for this one. */
void wimp_sprout_recache(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w->sprout_correct == w->sprout_current)
        return;
    w->sprout_correct = w->sprout_current;
    wimp_mode_refresh();
}

struct wimp_box wimp_screen_box(void)
{
    struct wimp_ws *w = wimp_ws();
    return (struct wimp_box){ 0, 0, w->screen_w, w->screen_h };
}

int32_t wimp_round_x(int32_t x)
{
    return x & ~(wimp_ws()->dx - 1);
}

int32_t wimp_round_y(int32_t y)
{
    return y & ~(wimp_ws()->dy - 1);
}

struct wimp_box wimp_round_box(struct wimp_box b)
{
    return (struct wimp_box){ wimp_round_x(b.x0), wimp_round_y(b.y0), wimp_round_x(b.x1),
                              wimp_round_y(b.y1) };
}

/* ---- the VDU ------------------------------------------------------------- */

static void vdu(const uint8_t *bytes, uint32_t n)
{
    struct wimp_ws *w = wimp_ws();
    memcpy(w->scratch, bytes, n);
    call(XOS_WriteN, ros_addr(w->scratch), n, 0, 0, 0, NULL);
}

void wimp_vdu5(void)
{
    static const uint8_t b[] = { 5 };
    vdu(b, 1);
}

/* Set the graphics window to the rectangle with VDU 24. x1 and y1 are
 * exclusive. An x0 or y0 below 0 is raised to 0, and the result is kept
 * as the clip. */
void wimp_graphics_window(struct wimp_box r)
{
    struct wimp_ws *w = wimp_ws();
    int32_t x0 = r.x0 < 0 ? 0 : r.x0, y0 = r.y0 < 0 ? 0 : r.y0;
    w->dr.clip = (struct wimp_box){ x0, y0, r.x1, r.y1 };
    int32_t x1 = r.x1 - 1, y1 = r.y1 - 1;
    uint8_t b[9] = { 24, (uint8_t)x0, (uint8_t)(x0 >> 8), (uint8_t)y0, (uint8_t)(y0 >> 8),
                     (uint8_t)x1, (uint8_t)(x1 >> 8), (uint8_t)y1, (uint8_t)(y1 >> 8) };
    vdu(b, 9);
}

void wimp_default_windows(void)
{
    static const uint8_t b[] = { 26 };
    wimp_ws()->dr.clip = wimp_screen_box();
    vdu(b, 1);
}

void wimp_vdu_bytes(const uint8_t *bytes, uint32_t n)
{
    vdu(bytes, n);
}

/* What a mode change sends. This is the VDU state, and then the glyphs
 * for the furniture unless the alphabet is UTF-8. */
void wimp_vdu_init(void)
{
    static const uint8_t s1[] = { 5, 23, 17, 4, 1, 0, 0, 0, 0, 0, 0,
                                  23, 16, 0x40, 0xBF, 0, 0, 0, 0, 0, 0, 26 };
    static const uint8_t s1a[] = {
        23, 0x83, 0xFE, 0x92, 0x92, 0xF2, 0x82, 0x82, 0xFE, 0x00,
        23, 0x84, 0x66, 0x99, 0x81, 0x42, 0x81, 0x99, 0x66, 0x00,
        23, 0x88, 0x18, 0x28, 0x4F, 0x81, 0x4F, 0x28, 0x18, 0x00,
        23, 0x89, 0x18, 0x14, 0xF2, 0x81, 0xF2, 0x14, 0x18, 0x00,
        23, 0x8A, 0x3C, 0x24, 0x24, 0xE7, 0x42, 0x24, 0x18, 0x00,
        23, 0x8B, 0x18, 0x24, 0x42, 0xE7, 0x24, 0x24, 0x3C, 0x00,
    };
    static const uint8_t s2[] = {
        23, 0x81, 0xF0, 0x90, 0xF0, 0x1F, 0x1F, 0x1F, 0x1F, 0x00,
        23, 0x82, 0xE0, 0xE0, 0xE0, 0x1F, 0x11, 0x11, 0x1F, 0x00,
        23, 0x85, 0xFC, 0xFC, 0xFF, 0xE1, 0xE1, 0x21, 0x3F, 0x00,
    };
    struct wimp_ws *w = wimp_ws();
    vdu(s1, sizeof s1);
    w->dr.clip = wimp_screen_box();
    wimp_set16x32();                            /* 16 x 32 OS units for VDU 5 text, as
                                                 * set16x32chars in int_allbutmode in
                                                 * s/Wimp02. This is the size of the
                                                 * desktop's system font and Edit's text. */
    uint32_t r[8];
    int utf8 = !call(XOS_Byte, 71, 127, 0, 0, 0, r) && r[1] == 111;
    if (utf8)
        return;
    vdu(s1a, sizeof s1a);
    if (!w->tl.built || (!w->tl.list[TOOL_BACK] && !w->tl.list[TOOL_PBACK]))
        vdu(s2, sizeof s2);
}

void wimp_clg(void)
{
    static const uint8_t b[] = { 16 };
    vdu(b, 1);
}

void wimp_ecf_origin(int32_t x, int32_t y)
{
    call(XOS_SetECFOrigin, (uint32_t)x, (uint32_t)y, 0, 0, 0, NULL);
}

/* Set a true colour as the graphics foreground or background, with GCOL
 * action 0, by ColourTrans_SetGCOL. */
void wimp_gcol(uint32_t colour, int background)
{
    uint32_t f = background ? 0x80u : 0;
    uint32_t r = (colour >> 8) & 0xFFu, g = (colour >> 16) & 0xFFu, b = colour >> 24;
    if (wimp_ws()->log2bpp == 0 && r == g && g == b)
        f |= 0x100u;                            /* a grey is dithered at 1 bpp */
    call(XColourTrans_SetGCOL, colour, 0, 0, f, 0, NULL);
}

/* A Wimp colour, through the mapping palette */
uint32_t wimp_colour(uint32_t n)
{
    return wimp_ws()->palette[n & 15];
}

/* The VDU's rectangle copy, by OS_Plot 4, 4 and 190 */
void wimp_copy_rect(struct wimp_box src, int32_t dx, int32_t dy)
{
    struct wimp_ws *w = wimp_ws();
    wimp_default_windows();
    call(XOS_Plot, 4, (uint32_t)src.x0, (uint32_t)src.y0, 0, 0, NULL);
    call(XOS_Plot, 4, (uint32_t)(src.x1 - w->dx), (uint32_t)(src.y1 - w->dy), 0, 0, NULL);
    call(XOS_Plot, 190, (uint32_t)(src.x0 + dx), (uint32_t)(src.y0 + dy), 0, 0, NULL);
}

/* ---- the colour SWIs -------------------------------------------------------------- */

static uint32_t log2bpp(void)
{
    uint32_t r[8];
    return !call(XOS_ReadModeVariable, 0xFFFFFFFFu, 9, 0, 0, 0, r) ? r[2] : 5;
}

/* Wimp_SetColour calls ColourTrans_SetGCOL(P[colour], flags, action),
 * where P is the mapping palette. */
void wimp_swi_SetColour(struct ros_cpu *s)
{
    uint32_t flags = (s->r[0] & 0x80u) ? 0x80u : 0;
    if (log2bpp() == 0 && (s->r[0] & 15u) < 8)
        flags |= 0x100u;
    uint32_t r[8];
    if (call(XColourTrans_SetGCOL, wimp_colour(s->r[0]), 0, 0, flags, (s->r[0] >> 4) & 7u, r)) {
        wimp_fail(s, ros_ptr(r[0]));
        return;
    }
    s->v = 0;
}

/* Wimp_TextColour */
void wimp_swi_TextColour(struct ros_cpu *s)
{
    uint32_t r[8];
    if (call(XColourTrans_SetTextColour, wimp_colour(s->r[0]), 0, 0, s->r[0] & 0x80u, 0, r)) {
        wimp_fail(s, ros_ptr(r[0]));
        return;
    }
    s->v = 0;
}

/* Wimp_SetFontColours */
void wimp_swi_SetFontColours(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t r[8];
    if (call(XColourTrans_SetFontColours, 0, w->palette[s->r[1] & 15u], w->palette[s->r[2] & 15u],
             14, 0, r)) {
        wimp_fail(s, ros_ptr(r[0]));
        return;
    }
    s->v = 0;
}

/* As setmousepalette. The pointer's colours 1-3 are set from the Wimp's
 * palette by VDU 19,c,25. If border is set, the border colour is set
 * first by VDU 19,0,24. The Wimp's pointer sprites carry no palette, so
 * this is where their colours come from. They are 5.30's cyan, dark blue
 * and red. */
void wimp_mouse_palette(int border)
{
    struct wimp_ws *w = wimp_ws();
    for (unsigned i = border ? 0 : 1; i < 4; i++) {
        uint32_t v = w->others[i];
        uint8_t q[6] = { 19, (uint8_t)i, (uint8_t)(i ? 25 : 24), (uint8_t)(v >> 8),
                         (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        wimp_vdu_bytes(q, 6);
    }
}

/* Wimp_SetPalette */
void wimp_swi_SetPalette(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    wimp_surface_desktop_changed();             /* surface windows' tables */
    int full = s->r[2] == 0x45555254u;
    for (unsigned i = 0; i < 20; i++) {
        uint32_t v;
        if (s->r[1] == 0 || s->r[1] == 0xFFFFFFFFu)
            v = i < 16 ? wimp_default_palette[i] : wimp_default_others[i - 16];
        else
            v = ros_ld32(s->r[1] + 4 * i);
        if (full) {
            v &= 0xFFFFFF00u;
        } else {
            v &= 0xF0F0F000u;
            v |= v >> 4;
        }
        if (i < 16)
            w->palette[i] = v;
        else
            w->others[i - 16] = v;
    }
    call(XColourTrans_InvalidateCache, 0, 0, 0, 0, 0, NULL);    /* recalculate the palette */
    if (w->ntasks)
        wimp_mouse_palette(1);                  /* as recalcpalette, while the Wimp is active */
    s->v = 0;
}

/* Wimp_ReadPalette */
void wimp_swi_ReadPalette(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    if (s->r[1] < 0x8000u) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    uint32_t mask = s->r[2] == 0x45555254u ? 0xFFFFFF00u : 0xF0F0F000u, out = s->r[1];
    if (log2bpp() > 3) {
        /* ColourTrans_SelectTable builds a 16-byte table from mode 12
         * with the emergency palette, to this mode with the mapping
         * palette. */
        uint32_t *src = (uint32_t *)(w->scratch + 128), *dst = (uint32_t *)(w->scratch + 192);
        uint8_t *table = w->scratch + 256;
        memcpy(src, wimp_default_palette, 64);
        memcpy(dst, w->palette, 64);
        memset(table, 0, 16);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 12, c.r[1] = ros_addr(src), c.r[2] = 0xFFFFFFFFu, c.r[3] = ros_addr(dst);
        c.r[4] = ros_addr(table), c.r[5] = 0;
        ros_swi(&c, XColourTrans_SelectTable);
        if (c.v) {
            wimp_fail(s, ros_ptr(c.r[0]));
            return;
        }
        for (unsigned i = 0; i < 16; i++)
            ros_st32(out + 4 * i, (w->palette[i] & mask) | table[i]);
    } else {
        for (unsigned i = 0; i < 16; i++) {
            uint32_t v = w->palette[i] & mask, g[8];
            call(XColourTrans_ReturnGCOL, v, 0, 0, 0, 0, g);
            ros_st32(out + 4 * i, v | (g[0] & 0xFFu));
        }
    }
    for (unsigned i = 0; i < 4; i++)
        ros_st32(out + 64 + 4 * i, w->others[i] & mask);
    s->v = 0;
}
