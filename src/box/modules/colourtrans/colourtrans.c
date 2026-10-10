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
 * This file is a reimplementation in C of RISC OS Open's ARM assembler source
 * (Sources/Video/Render/Colours: s.MainSWIs, s.Header, s.MsgCode, hdr.ColourTran).
 */

/* colourtrans.c: ColourTrans, reimplemented as a native module.
 *
 * RISC OS 5's ColourTrans (Video/Render/Colours, "Colours" 1.97) finds the
 * colour numbers, GCOLs, patterns and translation tables that come nearest
 * to physical colours. It is written again here from its sources. It is
 * checked against the 32-bit system on the farm (tests/vdu), result for
 * result. That includes the original's quirks. The README lists the few
 * cases that give a defined result here instead.
 * As in the original, every SWI goes through ColourV, with R8 holding the
 * SWI's offset. This lets a claimant of the vector, such as a host
 * renderer, take over any of them.
 *
 * This file holds the module, ColourV, the workspace and services, and the
 * colour SWIs. The other files are as follows:
 *   match.c    palettes and colour matching
 *   dither.c   dither patterns
 *   tables.c   translation tables
 *   models.c   calibration and the colour models
 *   fontcol.c  the font colours
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "ct.h"

#define COLOURV 0x22u

struct ct_ws ct;

/* ---- errors ---------------------------------------------------------------------------------- */

os_error *ct_err(uint32_t n)
{
    switch (n) {
    case CT_BADMODE: return ros_error(n, "Bad MODE");
    case CT_CANTKILL: return ros_error(n, "The ColourTrans module is currently active");
    case CT_BADSWI: return ros_error(n, "SWI value out of range for module ColourTrans");
    case CT_BADCALIB: return ros_error(n, "Bad calibration table");
    case CT_CONVOVER: return ros_error(n, "Overflow in conversion");
    case CT_BADHSV: return ros_error(n, "Hue should be undefined in achromatic colours");
    case CT_SWITCHED: return ros_error(n, "Not whilst output switched to sprite");
    case CT_BADMISCOP: return ros_error(n, "Unknown MiscOp call");
    case CT_BADFLAGS: return ros_error(n, "Reserved fields must be zero");
    case CT_BUFFOVER: return ros_error(n, "Buffer too small to read palette into");
    default: return ros_error(n, "Not supported in this depth of display");
    }
}

uint32_t ct_swi(uint32_t n, uint32_t r[10], os_error **e)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 10 * sizeof r[0]);
    *e = c.v ? ros_ptr(c.r[0]) : NULL;
    return c.c;
}

int ct_mode_var(uint32_t mode, uint32_t var, uint32_t *v)
{
    uint32_t r[10] = { mode, var };
    os_error *e;
    if (ct_swi(XOS_ReadModeVariable, r, &e) || e)
        return 1;
    *v = r[2];
    return 0;
}

/* ---- the workspace (validateworkspace) ------------------------------------------------------- */

static void validate(void)
{
    if (ct.valid)
        return;
    memset(&ct, 0, sizeof ct);
    ct.valid = 1;
    ct.loading = 0x01040200;
    ct.cache_empty = 0xFF;
    ct.pattern_temp = ros_addr(ros_rma_alloc(64));
    ct.scratch = ros_addr(ros_rma_alloc(4096));
    ct.grey8 = ros_addr(ros_rma_alloc(1024));           /* ResourceFS's 8greys */
    for (uint32_t i = 0; i < 256; i++)
        ros_st32(ct.grey8 + 4 * i, i * 0x01010100u);
    ct_init_cache();
}

/* ---- the colour SWIs ------------------------------------------------------------------------- */

static os_error *for_mode(uint32_t c, uint32_t mode, uint32_t pal, int worst, uint32_t *out)
{
    struct ct_pal p;
    os_error *e = ct_build(mode, pal, &p);
    if (e)
        return e;
    *out = worst ? ct_worst(&p, c) : ct_best(&p, c);
    return NULL;
}

/* ColourNumberToGCOL_testing. It converts through cn2g only at VIDC10's 256
 * colours. */
static uint32_t to_gcol(uint32_t n, uint32_t mode)
{
    uint32_t l2 = 0, f = 0;
    ct_mode_var(mode, 9, &l2);
    if (ct_mode_var(mode, 0, &f))
        f = 0;
    return l2 == 3 && !(f & MF_FULLPALETTE) ? ct_cn2g(n) : n;
}

static int vidc10(void)
{
    return ct.c_l2bpp == 3 && !(ct.c_flags & MF_FULLPALETTE);
}

static os_error *return_colour_number(uint32_t c, uint32_t *out)
{
    struct ct_cache *e = ct_lookup(c);
    if (e) {
        *out = e->colour;
        return NULL;
    }
    uint32_t n;
    os_error *err = for_mode(c, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, &n);
    if (err)
        return err;
    ct_write_cache(n, vidc10() ? ct_cn2g(n) : n, c);
    *out = n;
    return NULL;
}

static os_error *return_gcol(uint32_t c, uint32_t *out)
{
    struct ct_cache *e = ct_lookup(c);
    if (e) {
        *out = e->gcol;
        return NULL;
    }
    uint32_t n;
    os_error *err = for_mode(c, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, &n);
    if (err)
        return err;
    uint32_t g = to_gcol(n, 0xFFFFFFFFu);
    ct_write_cache(vidc10() ? ct_g2cn(g) : g, g, c);
    *out = g;
    return NULL;
}

static void set_colour(uint32_t flags, uint32_t v)
{
    uint32_t r[10] = { flags, v };
    os_error *e;
    ct_swi(XOS_SetColour, r, &e);
}

/* SetGCOL */
static os_error *set_gcol(struct ros_cpu *s)
{
    uint32_t d = ct.c_l2bpp, act = s->r[4] & 15, c = s->r[0];
    struct ct_cache *e = ct_lookup(c);
    if (!e) {
        uint32_t n;
        os_error *err = for_mode(c, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, &n);
        if (err)
            return err;
        e = ct_write_cache(n, d == 3 ? ct_cn2g(n) : n, c);
    }
    uint32_t bg = (s->r[3] & 0x80) >> 3;
    if ((s->r[3] & 0x100) && d <= 4) {
        if (e->pvalid == 0xFF) {
            e->pvalid = (uint8_t)d;
            ct_pattern(e, d, 1);
        }
        uint32_t blk = ct.pattern_temp + 32;
        for (uint32_t i = 0; i < 8; i++)
            ros_st32(blk + 4 * i, e->pat[i & 3]);
        set_colour(act | bg | 0x20, blk);
    } else {
        set_colour(act | bg, e->colour);
    }
    s->r[0] = e->gcol, s->r[2] = d, s->r[3] &= 0x80;
    return NULL;
}

/* SetColour. It takes a GCOL and converts it through g2cn only at VIDC10's
 * 256 colours. */
static void set_colour_swi(struct ros_cpu *s)
{
    uint32_t v = s->r[0], l2 = 0, f = 0;
    ct_mode_var(0xFFFFFFFFu, 9, &l2);
    if (ct_mode_var(0xFFFFFFFFu, 0, &f))
        f = 0;
    if (l2 == 3 && !(f & MF_FULLPALETTE))
        v = ct_g2cn(s->r[0]);
    set_colour(((s->r[3] & 0x280) >> 3) | s->r[4], v);
}

/* MiscOp 1: returns the pattern for a colour. */
static os_error *return_pattern(struct ros_cpu *s)
{
    uint32_t d = ct.c_l2bpp, c = s->r[1];
    struct ct_cache *e = ct_lookup(c);
    if (!e) {
        uint32_t n;
        os_error *err = for_mode(c, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, &n);
        if (err)
            return err;
        e = ct_write_cache(n, d == 3 ? ct_cn2g(n) : n, c);
    }
    if (e->pvalid == 0xFF) {
        ct_pattern(e, d, 0);
        e->pvalid = 0;
    }
    for (uint32_t i = 0; i < 8; i++)
        ros_st32(ct.pattern_temp + 4 * i, e->pat[i & 3]);
    s->r[0] = e->gcol & 255, s->r[1] = e->colour & 255, s->r[2] = d, s->r[3] = ct.pattern_temp;
    return NULL;
}

static os_error *misc_op(struct ros_cpu *s)
{
    switch (s->r[0]) {
    case 0: {
        uint32_t old = ct.loading, now = (old & s->r[2]) ^ s->r[1];
        ct.loading = now;
        s->r[1] = now, s->r[2] = old;
        if (now != old)
            ct_init_cache();
        return NULL;
    }
    case 1:
        return return_pattern(s);
    case 2:
        s->r[0] = !ct.calib ? 0 : ct.calib_new ? 2 : 1;
        return NULL;
    default:
        return ct_err(CT_BADMISCOP);
    }
}

/* ---- ReadPalette and WritePalette ------------------------------------------------------------ */

/* Classifies R0 as MainSWIs does. It tells whether R0 is a sprite area. */
static int is_area(uint32_t r0)
{
    if (r0 == 0xFFFFFFFFu || r0 < 256 || (r0 & 1))
        return 0;
    if (r0 == 0x8000 || r0 == 256)
        return 1;
    return !(ros_ld32(r0) & 1);
}

/* DecodeSprite. It returns the mode word, the palette (0 if none) and the
 * getpalentry flags. *sp receives the sprite. */
static os_error *decode_sprite(uint32_t area, uint32_t name, int ptr, uint32_t *mode, uint32_t *pal,
                               uint32_t *flags, uint32_t *sp)
{
    os_error *e;
    if (area == 0) {
        uint32_t r[10] = { 3 };
        ct_swi(XOS_ReadDynamicArea, r, &e);
        if (e)
            return e;
        area = r[0];
    }
    if (!ptr) {
        uint32_t r[10] = { 24 + 256, area, name };
        ct_swi(XOS_SpriteOp, r, &e);
        if (e)
            return e;
        name = r[2];
    }
    uint32_t r[10] = { 37 + 512, area, name, 0xFFFFFFFFu };
    ct_swi(XOS_SpriteOp, r, &e);
    if (e)
        return e;
    *mode = r[5], *pal = r[4], *sp = name;
    if (r[4]) {
        *flags |= PF_DOUBLE;
        uint32_t l2 = 0;
        ct_mode_var(r[5], 9, &l2);
        if (l2 == 3 && r[3] != 256)
            *flags |= PF_BRAINDAMAGED;
    }
    return NULL;
}

static os_error *read_palette(struct ros_cpu *s)
{
    uint32_t mode = s->r[0], pal = s->r[1], buf = s->r[2], flags = 0, sp;
    if (s->r[4] & ~3u)
        return ct_err(CT_BADFLAGS);
    os_error *e;
    if (is_area(mode) && (e = decode_sprite(mode, pal, s->r[4] & 1, &mode, &pal, &flags, &sp)))
        return e;
    uint32_t l2;
    if (ct_mode_var(mode, 9, &l2))
        return ct_err(CT_BADMODE);
    if (l2 > 3)
        return ct_err(CT_BADDEPTH);
    uint32_t dn, defs = 0;
    if (pal == 0) {
        if (l2 == 3)
            flags |= PF_BRAINDAMAGED;
        defs = 1;
    }
    uint32_t count = 1u << (1u << l2), bytes = count * 4 * (s->r[4] & 2 ? 2 : 1);
    if (!buf) {
        s->r[3] = bytes;
        return NULL;
    }
    if ((int32_t)(s->r[3] - bytes) < 0)
        return ct_err(CT_BUFFOVER);
    s->r[3] -= bytes;
    const uint32_t *def = ct_defpal(l2, &dn);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t w1, w2;
        if (pal == 0xFFFFFFFFu) {
            ct_my_read_palette(i, &w1, &w2);
        } else if (defs) {
            /* The default palette, taken from the module's own table. This is what
             * getpalentry does with it. */
            uint32_t j = flags & PF_BRAINDAMAGED ? i & 15 : i;
            w1 = w2 = def[j];
            if (flags & PF_BRAINDAMAGED) {
                uint32_t v = (w1 & ~0x80C08000u) | (i & 0x80) << 24 | (i & 0x60) << 17 |
                             (i & 0x10) << 11;
                v &= 0xF0F0F000u;
                w1 = w2 = v | v >> 4;
            }
        } else {
            ct_getpalentry(pal, flags, i, &w1, &w2);
        }
        ros_st32(buf, w1), buf += 4;
        if (s->r[4] & 2)
            ros_st32(buf, w2), buf += 4;
    }
    s->r[2] = buf;
    return NULL;
}

static os_error *write_palette(struct ros_cpu *s)
{
    if (s->r[4] & ~7u)
        return ct_err(CT_BADFLAGS);
    if (ct.palette_at)
        return ct_err(CT_SWITCHED);
    uint32_t mode = 0xFFFFFFFFu, pal = 0, flags = 0, sp = 0, area = s->r[0], tab = s->r[2];
    os_error *e;
    int sprite = s->r[0] != 0xFFFFFFFFu;
    if (sprite && (e = decode_sprite(area, s->r[1], s->r[4] & 1, &mode, &pal, &flags, &sp)))
        return e;
    uint32_t l2;
    if (ct_mode_var(mode, 9, &l2))
        return ct_err(CT_BADMODE);
    if (l2 > 3)
        return ct_err(CT_BADDEPTH);
    uint32_t n = 1u << (1u << l2), tflags = s->r[4] & 2 ? PF_DOUBLE : 0;
    if (sprite) {
        if (!(s->r[4] & 4) && n == 256) {
            int compact = 1;
            for (uint32_t i = 256; i-- && compact;) {
                uint32_t a1, a2, b1, b2;
                ct_getpalentry(tab, tflags | PF_BRAINDAMAGED, i, &a1, &a2);
                ct_getpalentry(tab, tflags, i, &b1, &b2);
                if (a1 != b1 || a2 != b2)
                    compact = 0;
            }
            if (compact)
                n = 16;
        }
        if (area == 0) {
            uint32_t r[10] = { 3 };
            ct_swi(XOS_ReadDynamicArea, r, &e);
            area = r[0];
        }
        uint32_t r[10] = { 37 + 512, area, sp, 1 | (n == 256 ? 0x80000000u : 0) };
        ct_swi(XOS_SpriteOp, r, &e);
        if (e)
            return e;
        uint32_t q[10] = { 37 + 512, area, sp, 0xFFFFFFFFu };
        ct_swi(XOS_SpriteOp, q, &e);
        if (e)
            return e;
        uint32_t p = q[4];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w1 = s->r[4] & 2 ? ros_ld32(tab + 8 * i) : ros_ld32(tab + 4 * i);
            uint32_t w2 = s->r[4] & 2 ? ros_ld32(tab + 8 * i + 4) : w1;
            ros_st32(p + 8 * i, w1), ros_st32(p + 8 * i + 4, w2);
        }
        return NULL;
    }
    struct ros_cpu v;
    ros_cpu_enter(&v);
    v.r[0] = 0, v.r[1] = n | (s->r[4] & 2 ? 16u : 17u) << 24, v.r[2] = tab, v.r[4] = 8;
    ros_vector_call(0x23, &v);
    if (v.r[4] == 0) {
        if (!(s->r[4] & 2)) {
            ros_cpu_enter(&v);
            v.r[0] = 0, v.r[1] = n | 18u << 24, v.r[2] = tab, v.r[4] = 8;
            ros_vector_call(0x23, &v);
        }
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w1 = s->r[4] & 2 ? ros_ld32(tab + 8 * i) : ros_ld32(tab + 4 * i);
        uint32_t w2 = s->r[4] & 2 ? ros_ld32(tab + 8 * i + 4) : w1;
        ros_cpu_enter(&v);
        v.r[0] = i, v.r[1] = 16, v.r[2] = w1, v.r[3] = w2, v.r[4] = 2;
        ros_vector_call(0x23, &v);
        if (v.r[4] != 0) {
            uint8_t b[5] = { (uint8_t)i, 16, (uint8_t)(w1 >> 8), (uint8_t)(w1 >> 16),
                             (uint8_t)(w1 >> 24) };
            memcpy(ros_ptr(ct.scratch), b, 5);
            uint32_t r[10] = { 12, ct.scratch };
            ct_swi(XOS_Word, r, &e);
        }
    }
    return NULL;
}

/* ---- ColourV --------------------------------------------------------------------------------- */

static os_error *dispatch(struct ros_cpu *s, uint32_t n)
{
    uint32_t v;
    os_error *e;
    switch (n) {
    case 0: return ct_generate_table(s, is_area(s->r[0]) ? s->r[5] : 0);
    case 1: return ct_generate_table(s, 8);
    case 2: if ((e = return_gcol(s->r[0], &v))) return e; s->r[0] = v; return NULL;
    case 3: return set_gcol(s);
    case 4: if ((e = return_colour_number(s->r[0], &v))) return e; s->r[0] = v; return NULL;
    case 5:
    case 10:
        if ((e = for_mode(s->r[0], s->r[1], s->r[2], n == 10, &v)))
            return e;
        s->r[0] = to_gcol(v, s->r[1]);
        return NULL;
    case 6:
    case 11:
        if ((e = for_mode(s->r[0], s->r[1], s->r[2], n == 11, &v)))
            return e;
        s->r[0] = v;
        return NULL;
    case 7:
    case 9:
        if ((e = for_mode(s->r[0], 0xFFFFFFFFu, 0xFFFFFFFFu, 1, &v)))
            return e;
        s->r[0] = n == 7 ? to_gcol(v, 0xFFFFFFFFu) : v;
        return NULL;
    case 8: {                               /* SetOppGCOL does not check the error from its lookup. */
        if ((e = for_mode(s->r[0], 0xFFFFFFFFu, 0xFFFFFFFFu, 1, &v)))
            v = ros_addr(e);
        else
            v = to_gcol(v, 0xFFFFFFFFu);
        struct ros_cpu c = *s;
        c.r[0] = v;
        set_colour_swi(&c);
        s->r[0] = v, s->r[2] = ct.c_l2bpp;
        return NULL;
    }
    case 12: s->r[0] = ct_g2cn(s->r[0]); return NULL;
    case 13: s->r[0] = ct_cn2g(s->r[0]); return NULL;
    case 14: return ct_font_colours(s, 0);
    case 15: return ct_font_colours(s, 1);
    case 16: ct_init_cache(); return NULL;
    case 28: return read_palette(s);
    case 29: return write_palette(s);
    case 30: set_colour_swi(s); return NULL;
    case 31: return misc_op(s);
    case 33:
    case 34: {                              /* Set[Opp]TextColour do not check errors and do not mask R3. */
        if (n == 33)
            e = return_colour_number(s->r[0], &v);
        else
            e = for_mode(s->r[0], 0xFFFFFFFFu, 0xFFFFFFFFu, 1, &v);
        if (e)
            v = ros_addr(e);
        set_colour((s->r[3] >> 3) | 0x40, v);
        return NULL;
    }
    case 35: return ct_generate_table(s, s->r[5] | 0x80000000u);
    default:
        if (n >= 17 && n <= 32)
            return ct_models_swi(s, n);
        return ct_err(CT_BADSWI);
    }
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ct_err(CT_BADSWI);
}

static void output_to_sprite(struct ros_cpu *s);

/* Called when the VDU's live context has moved. That happens when a task
 * with a virtual display comes or goes, or when its mode changes. In those
 * cases neither service call arrives. This code therefore refreshes what a
 * mode change and an output switch would refresh. */
static void context_moved(void)
{
    static uint32_t seen;
    uint32_t id = ros_vdu_context_id();
    if (id == seen)
        return;
    seen = id;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_vdu_output(&c.r[2], &c.r[3], &c.r[4]);
    if (c.r[4] == 0)
        c.r[3] = 0;
    ct_free_tables();
    output_to_sprite(&c);
    ct_init_cache();
}

static int colour_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t n = s->r[8];
    if ((n == 16 || n == 23) && !ct.valid)
        return ROS_VECTOR_CLAIM;
    validate();
    context_moved();
    if (ct.palette_at != ct.palette_copy)
        ct_init_cache();
    os_error *e = dispatch(s, n);
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
    return ROS_VECTOR_CLAIM;
}

/* Every SWI goes through ColourV. R8 and R9 are preserved. */
static void via_colourv(struct ros_cpu *s, uint32_t n)
{
    uint32_t r8 = s->r[8], r9 = s->r[9];
    s->r[8] = n, s->v = 0;
    if (!ros_vector_call(COLOURV, s))
        ros_swi_fail(s, ct_err(CT_BADSWI));
    s->r[8] = r8, s->r[9] = r9;
}

#define T(n, name) \
    void ros_thunk_ColourTrans_##name(struct ros_cpu *s) { via_colourv(s, n); }
T(0, SelectTable) T(1, SelectGCOLTable) T(2, ReturnGCOL) T(3, SetGCOL)
T(4, ReturnColourNumber) T(5, ReturnGCOLForMode) T(6, ReturnColourNumberForMode)
T(7, ReturnOppGCOL) T(8, SetOppGCOL) T(9, ReturnOppColourNumber) T(10, ReturnOppGCOLForMode)
T(11, ReturnOppColourNumberForMode) T(12, GCOLToColourNumber) T(13, ColourNumberToGCOL)
T(14, ReturnFontColours) T(15, SetFontColours) T(16, InvalidateCache) T(17, SetCalibration)
T(18, ReadCalibration) T(19, ConvertDeviceColour) T(20, ConvertDevicePalette)
T(21, ConvertRGBToCIE) T(22, ConvertCIEToRGB) T(23, WriteCalibrationToFile)
T(24, ConvertRGBToHSV) T(25, ConvertHSVToRGB) T(26, ConvertRGBToCMYK) T(27, ConvertCMYKToRGB)
T(28, ReadPalette) T(29, WritePalette) T(30, SetColour) T(31, MiscOp)
T(32, WriteLoadingsToFile) T(33, SetTextColour) T(34, SetOppTextColour) T(35, GenerateTable)
#undef T

/* ---- services -------------------------------------------------------------------------------- */

/* Service_SwitchingOutputToSprite (output_to_sprite). */
static void output_to_sprite(struct ros_cpu *s)
{
    if (s->r[4] == 0) {
        ct.palette_at = 0;
    } else {
        uint32_t mode = 0, pal = 0, flags = 0, sp;
        if (!decode_sprite(s->r[3], s->r[4], 1, &mode, &pal, &flags, &sp)) {
            if ((s->r[2] & 1) && (mode & 0x80000000u))
                ct.palette_at = ct.grey8, ct.palette_flags = 0;
            else
                ct.palette_at = pal, ct.palette_flags = flags;
        }
    }
    uint32_t l2 = 0, f = 0, nc = 0;
    ct_mode_var(0xFFFFFFFFu, 9, &l2);
    ct_mode_var(0xFFFFFFFFu, 0, &f);
    ct_mode_var(0xFFFFFFFFu, 3, &nc);
    ct.c_l2bpp = l2 & 255, ct.c_flags = f & 0xFFFF;
    uint32_t bits = 0;
    for (uint32_t v = nc; v; v >>= 1)
        bits++;
    ct.c_l2nc = bits;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (!ct.valid)
        return;
    switch (s->r[1]) {
    case 0x46:                              /* ModeChange */
        ct_free_tables();
        ct.persist = 0;
        ct_init_cache();
        break;
    case 0x72:                              /* SwitchingOutputToSprite */
        output_to_sprite(s);
        break;
    default:
        break;
    }
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    return ros_vector_claim_native(COLOURV, colour_v, 0);
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m;
    if (ct.persist || (!fatal && ct.calib))
        return ct_err(CT_CANTKILL);
    ct_free_tables();
    ros_vector_release_native(COLOURV, colour_v, 0);
    if (ct.valid) {
        ros_rma_free(ros_ptr(ct.pattern_temp));
        ros_rma_free(ros_ptr(ct.scratch));
        ros_rma_free(ros_ptr(ct.grey8));
        if (ct.calib)
            ros_rma_free(ros_ptr(ct.calib));
    }
    ct.valid = 0;
    return NULL;
}

struct ros_module colourtrans_module = {
    .title = "ColourTrans",
    .help = "Colour Selector\t1.97 (30 Aug 2023) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x40740,
    .swi_thunks = ros_swi_thunks_ColourTrans,
    .swi_names = ros_swi_names_ColourTrans,
    .swi_prefix = "ColourTrans",
};

/* The SWI count is known only at run time (api_gen.c). */
__attribute__((constructor)) static void count(void)
{
    colourtrans_module.swi_count = ros_swi_count_ColourTrans;
}
