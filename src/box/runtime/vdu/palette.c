/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2000 Pace Micro Technology plc
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
 * (Sources/Kernel: s.vdu.vdupalxx, s.vdu.vdupalette, s.NewIRQs,
 * s.ExtraSWIs).
 */
/* palette.c -- the palette: defaults, VDU 19, flashing, reading it back.
 *
 * This follows the kernel's PaletteV owner and its callers
 * (Kernel/s/vdu/vdupalxx and vdupalette, and NewIRQs for the flashing). The
 * palette is kept as the kernel keeps it. Each entry is &BBGGRRSS, in two
 * flash states. Entries 0-255 are the colours, 256 is the border, and
 * 257-259 are the pointer's. The state on show is written to GraphicsV's
 * driver.
 *
 *   - PaletteV. This is its default owner, as the kernel is, for Read (1),
 *     Set (2), SetDefaultPalette (5), BlankScreen (6), BulkRead (7) and
 *     BulkWrite (8). VDU 19, OS_Word 12, OS_ReadPalette and the defaults
 *     (VDU 20, and a mode change: PalInit) go through the vector, so a
 *     claimant can take them or see them go by (ITable). The other reasons
 *     (the flash states and gamma) are still done directly.
 *   - Blanking (PV_BlankScreen). R0 1 blanks, 0 unblanks, and anything else
 *     reads. R0 returns the old state. A change goes to the driver
 *     (GraphicsV SetBlank, with R1 the DPMS state, which is 0 because no
 *     ROSGD mode gives one) and the palette is written again. A mode
 *     change unblanks through the vector after its PalInit (UnblankScreen),
 *     as the kernel's does. The Screen Blanker blanks in this way.
 *   - Defaults (PV_SetDefaultPalette). These are the kernel's tables: 2, 4
 *     and 16 colours, with colours 8-15 flashing to their inverses;
 *     teletext's eight colours, then the same eight with supremacy set;
 *     hi-res mono's grey ramp; VIDC10's 256 colours from the pixel's bits;
 *     a linear ramp (the gamma table) at 16 and 32 bpp; greys for a
 *     greyscale mode; and black for the rest, with the border black. In
 *     BBC gap modes colours 2 and 3 are the border and its inverse.
 *   - VDU 19,l,p,r,g,b. If p < 16 it is a BBC colour, and bit 3 makes it
 *     flash where there are 16 colours or fewer. Values 16, 17 and 18 set
 *     both states or one. 24 sets the border and 25 sets the pointer's
 *     colours 1-3. At VIDC10's 256 colours, l & 15 sets all sixteen
 *     entries l + 16k. Each has the colour's bits that pixel bits 4-7
 *     control set from k.
 *   - Flashing. OS_Byte 9 and 10 set how many VSyncs each state lasts. 0
 *     stops it in the other state.
 */
#include <string.h>

#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/keyboard.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "vduws.h"

enum { GV_SET_BLANK = 4, GV_WRITE_PALETTE_ENTRY = 10, GV_WRITE_PALETTE_ENTRIES = 11 };
enum { PAL_NORMAL = 0, PAL_BORDER = 1, PAL_POINTER = 2 };
/* The flashing's OS_Byte variables: FlashCount (0 means frozen), SpacPeriod
 * and MarkPeriod. They are also read and written by OS_Byte 193-195. */
enum { FLASH_COUNT = 0xC1, SPAC_PERIOD = 0xC2, MARK_PERIOD = 0xC3 };
#define BORDER 256u

static uint32_t buffer;                 /* 256 entries for GraphicsV: RMA */

/* Writes the state on show to GraphicsV's driver. */
static void push(void)
{
    if (vdu.vd) {                       /* a virtual display has its own palette and no driver */
        vdisplay_palette_changed();
        return;
    }
    if (!vdu.screen_ok)
        return;
    if (!buffer)
        buffer = ros_addr(ros_rma_alloc(256 * 4));
    const uint32_t *p = vdu.pal[vdu.flash_state];
    memcpy(ros_ptr(buffer), p, 256 * 4);
    uint32_t r[4] = { PAL_NORMAL, buffer, 0, 256 };
    vdu_graphicsv(GV_WRITE_PALETTE_ENTRIES, r);
    uint32_t b[4] = { PAL_BORDER, p[BORDER], 0, 0 };
    vdu_graphicsv(GV_WRITE_PALETTE_ENTRY, b);
    for (uint32_t i = 1; i < 4; i++) {
        uint32_t q[4] = { PAL_POINTER, p[BORDER + i], i, 0 };
        vdu_graphicsv(GV_WRITE_PALETTE_ENTRY, q);
    }
}

/* UpdateSettingAndVIDC. The states are 1 for the first, 2 for the second
 * and 3 for both. */
static void set(uint32_t index, uint32_t states, uint32_t colour)
{
    if (states & 1)
        vdu.pal[0][index] = colour;
    if (states & 2)
        vdu.pal[1][index] = colour;
}

/* A 12-bit &SBGR entry, as the kernel's tables hold them */
static uint32_t sbgr(uint32_t e)
{
    uint32_t r = e & 15, g = (e >> 4) & 15, b = (e >> 8) & 15;
    return b * 0x11u << 24 | g * 0x11u << 16 | r * 0x11u << 8 | (e & 0x1000 ? 0xFF : 0);
}

static const uint16_t paldat1[] = { 0x0000, 0x0FFF };
static const uint16_t paldat2[] = { 0x0000, 0x000F, 0x00FF, 0x0FFF };
static const uint16_t paldat4[] = {
    0x0000, 0x000F, 0x00F0, 0x00FF, 0x0F00, 0x0F0F, 0x0FF0, 0x0FFF,
    0x8000, 0x800F, 0x80F0, 0x80FF, 0x8F00, 0x8F0F, 0x8FF0, 0x8FFF,
};
static const uint16_t paldatT[] = {
    0x0000, 0x000F, 0x00F0, 0x00FF, 0x0F00, 0x0F0F, 0x0FF0, 0x0FFF,
    0x1000, 0x100F, 0x10F0, 0x10FF, 0x1F00, 0x1F0F, 0x1FF0, 0x1FFF,
};
static const uint16_t paldatHR[] = {
    0x0000, 0x0111, 0x0222, 0x0333, 0x0444, 0x0555, 0x0666, 0x0777,
    0x0888, 0x0999, 0x0AAA, 0x0BBB, 0x0CCC, 0x0DDD, 0x0EEE, 0x0FFF,
    0x0000, 0x0010, 0x0020, 0x0030,
};

/* GetPalIndex */
static uint32_t pal_index(void)
{
    uint32_t i = vdu.dmv[MV_LOG2BPP], f = vdu.dmv[MV_FLAGS];
    if (i >= 4)
        return 4;
    if (i != 0 && (f & MF_BBCGAP))
        i--;
    if (f & MF_TELETEXT)
        i = 5;
    if (f & MF_HIRESMONO)
        i = 6;
    return i;
}

/* BorderColour. This sets the border, and in BBC gap modes colours 2 and
 * 3. */
static void border(uint32_t states, uint32_t colour)
{
    set(BORDER, states, colour);
    if (vdu.dmv[MV_FLAGS] & MF_BBCGAP) {
        set(2, states, colour);
        set(3, states, (~colour & 0xFFFFFF00u) | (colour & 0xFF));
    }
}

static void set_defaults(void)
{
    uint32_t f = vdu.dmv[MV_FLAGS], n = 0, nc = vdu.dmv[MV_NCOLOUR];
    memset(vdu.pal, 0, sizeof vdu.pal);
    if ((f & MF_GREYSCALE) && !(f & (3u << 12)) && nc != 63 && vdu.dmv[MV_LOG2BPP] <= 3 &&
        nc <= 255) {
        static const uint32_t mult[4] = { 0xFFFFFF00u, 0x55555500u, 0x11111100u, 0x01010100u };
        for (n = 0; n <= nc; n++)
            set(n, 3, n * mult[vdu.dmv[MV_LOG2BPP]]);
    } else {
        const uint16_t *t = NULL;
        switch (pal_index()) {
        case 0: t = paldat1, n = 2; break;
        case 1: t = paldat2, n = 4; break;
        case 2: t = paldat4, n = 16; break;
        case 5: t = paldatT, n = 16; break;
        case 6: t = paldatHR, n = 20; break;
        case 3:                         /* VIDC10's 256 colours */
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = (i & 3) | (i & 3) << 4;
                c = (c | c << 8 | c << 16 | c << 24) & ~0xFFu;
                if (i & 0x04) c |= 0x00004400u;
                if (i & 0x08) c |= 0x44000000u;
                if (i & 0x10) c |= 0x00008800u;
                if (i & 0x20) c |= 0x00440000u;
                if (i & 0x40) c |= 0x00880000u;
                if (i & 0x80) c |= 0x88000000u;
                set(i, 3, c);
            }
            break;
        default:                        /* 4: the linear ramp */
            for (uint32_t i = 0; i < 256; i++)
                set(i, 3, i * 0x01010100u);
            break;
        }
        for (uint32_t i = 0; t && i < n; i++) {
            set(i, 1, sbgr(t[i] & 0x7FFF));
            set(i, 2, sbgr((t[i] & 0x7FFF) ^ (t[i] & 0x8000 ? 0xFFF : 0)));
        }
    }
    border(3, 0);
    push();
}

/* PaletteV_Set's normal colour (UpdateNormalColour) */
static void normal(uint32_t l, uint32_t states, uint32_t colour)
{
    uint32_t nc = vdu.dmv[MV_NCOLOUR];
    if (nc != 63) {
        set(l & nc & 255, states, colour);
        return;
    }
    for (l &= 15; l < 256; l += 16) {
        uint32_t c = colour & ~0x88CC8800u;
        if (l & 0x10) c |= 0x00008800u;
        if (l & 0x20) c |= 0x00440000u;
        if (l & 0x40) c |= 0x00880000u;
        if (l & 0x80) c |= 0x88000000u;
        set(l, states, c);
    }
}

/* PaletteV_Set for type 16, 17, 18, 24 or 25. It returns 0 for any other
 * type. */
static int palette_set(uint32_t l, uint32_t type, uint32_t colour)
{
    switch (type) {
    case 16: normal(l, 3, colour); break;
    case 17: normal(l, 1, colour); break;
    case 18: normal(l, 2, colour); break;
    case 24: border(3, colour); break;
    case 25:
        if (l & 3)
            set(BORDER + (l & 3), 3, colour);
        break;
    default: return 0;
    }
    return 1;
}

#define PALETTEV 0x23u
enum {
    PV_READ = 1, PV_SET = 2, PV_SETDEFAULT = 5, PV_BLANKSCREEN = 6, PV_BULKREAD = 7, PV_BULKWRITE = 8
};

/* CallPaletteV */
static void call_pv(struct ros_cpu *s, uint32_t reason)
{
    s->r[4] = reason;
    ros_vector_call(PALETTEV, s);
}

/* PalInit. This sets the defaults, through PaletteV. */
void vdu_palette_default(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    call_pv(&s, PV_SETDEFAULT);
}

/* UnblankScreen, after a mode change's PalInit */
void vdu_palette_unblank(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0;
    call_pv(&s, PV_BLANKSCREEN);
}

static void pv_set(uint32_t l, uint32_t type, uint32_t colour)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = l, s.r[1] = type, s.r[2] = colour;
    call_pv(&s, PV_SET);
}

/* For teletext's VDU 23,18,0 (SetTTXPalette). vdu_palette_bulk writes
 * entries 0 to n - 1 of one flash state, with type 17 or 18
 * (PV_BulkWrite). vdu_palette_set writes one entry (PV_Set). */
void vdu_palette_bulk(const uint32_t *entries, uint32_t n, uint32_t type)
{
    static uint32_t block;
    if (!block)
        block = ros_addr(ros_rma_alloc(256 * 4));
    memcpy(ros_ptr(block), entries, 4 * (n < 256 ? n : 256));
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = type << 24 | n, s.r[2] = block;
    call_pv(&s, PV_BULKWRITE);
}

void vdu_palette_set(uint32_t l, uint32_t type, uint32_t colour)
{
    pv_set(l, type, colour);
}

/* VDU 19 and OS_Word 12 (SetPal): l, p, r, g, b */
void vdu_palette_vdu19(const uint8_t q[5])
{
    if (vdu.dmv[MV_FLAGS] & MF_TELETEXT)
        return;
    uint32_t l = q[0], p = q[1];
    uint32_t colour = p & 0x80 ? 0xFF : 0;
    p &= 0x7F;
    if (p < 16) {
        if (p & 1) colour |= 0x0000FF00u;
        if (p & 2) colour |= 0x00FF0000u;
        if (p & 4) colour |= 0xFF000000u;
        if (pal_index() >= 3)
            p &= ~8u;
        if (p & 8) {
            pv_set(l, 17, colour);
            pv_set(l, 18, (~colour & 0xFFFFFF00u) | (colour & 0xFF));
        } else {
            pv_set(l, 16, colour);
        }
    } else {
        colour |= (uint32_t)q[2] << 8 | (uint32_t)q[3] << 16 | (uint32_t)q[4] << 24;
        pv_set(l, p, colour);
    }
}

/* The VSync's flashing (NewIRQs). FlashCount counts down. Each state lasts
 * its period's number of VSyncs. MarkPeriod is the first state's and
 * SpacPeriod is the second's. The period is read as the state changes. */
void vdu_palette_flash(void)
{
    uint8_t n = ros_byte_var(FLASH_COUNT);
    if (!n)
        return;                         /* frozen */
    if (--n) {
        ros_byte_var_set(FLASH_COUNT, n);
        return;
    }
    vdu.flash_state ^= 1;
    ros_byte_var_set(FLASH_COUNT, ros_byte_var(vdu.flash_state ? SPAC_PERIOD : MARK_PERIOD));
    push();
}

/* OS_Byte 9 (first) and 10 (second). R1 is the new period, and the old one
 * is returned. R2 comes back as the kernel leaves it (Osbyte910): zero page
 * plus the variable's offset from SpacPeriod, which is 1 for the first and
 * 0 for the second. */
void vdu_palette_period(int second, struct ros_cpu *s)
{
    uint32_t var = second ? SPAC_PERIOD : MARK_PERIOD;
    uint8_t period = (uint8_t)s->r[1];
    s->r[1] = ros_byte_var(var);
    ros_byte_var_set(var, period);
    s->r[2] = ROS_ZEROPAGE + (second ? 0 : 1);
    if (ros_byte_var(FLASH_COUNT))
        return;
    ros_byte_var_set(FLASH_COUNT, period);      /* it was frozen, so start it in that state */
    vdu.flash_state = second;
    push();
}

/* The entry PaletteV reads: 0-255, 256 for the border, or 257-259 for the
 * pointer's. It returns -1 for pointer colour 0, which is none. */
static int32_t pv_index(uint32_t l, uint32_t type)
{
    uint32_t nc = vdu.dmv[MV_NCOLOUR];
    if (nc == 63)
        nc = 255;
    if (type == 24)
        return BORDER;
    if (type == 25)
        return l & 3 ? (int32_t)(BORDER + (l & 3)) : -1;
    uint32_t index = l & nc & 0x1FF;
    return (int32_t)(index > 259 ? index & 255 : index);   /* the kernel reads past the end here */
}

/* PV_BulkRead. R0 is a list of colours or 0. R1 is the type << 24 | the
 * count. R2 is the first states (both, interleaved, if R3 is 0). R3 is the
 * second. */
static void bulk_read(const struct ros_cpu *s)
{
    uint32_t type = s->r[1] >> 24, n = s->r[1] & 0xFFFFFF, list = s->r[0];
    uint32_t a = s->r[2], b = s->r[3];
    int first = type != 18, second = type != 17;
    uint32_t mask = vdu.dmv[MV_NCOLOUR] == 63 ? 255 : vdu.dmv[MV_NCOLOUR];
    if (type > 24)
        mask = 3;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = (list ? ros_ld32(list + 4 * i) : i) & mask;
        if (type == 24)
            c = BORDER;
        else if (type > 24) {
            if (c == 0)
                break;                  /* the kernel stops at colour 0 */
            c += BORDER;
        }
        if (c > 259)
            c &= 255;
        if (first)
            ros_st32(a, vdu.pal[0][c]), a += 4;
        if (second) {
            if (b)
                ros_st32(b, vdu.pal[1][c]), b += 4;
            else
                ros_st32(a, vdu.pal[1][c]), a += 4;
        }
    }
}

/* PV_BulkWrite. R0 is a list or 0. R1 is the type << 24 | the count. R2 is
 * the colours. Afterwards the greyscale flag follows the palette. */
static void bulk_write(const struct ros_cpu *s)
{
    uint32_t type = s->r[1] >> 24, n = s->r[1] & 0xFFFFFF, list = s->r[0], p = s->r[2];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = list ? ros_ld32(list + 4 * i) : i;
        if (type == 16 || type == 17)
            normal(c, 1, ros_ld32(p)), p += 4;
        if (type == 16 || type == 18)
            normal(c, 2, ros_ld32(p)), p += 4;
        if (type == 24)
            border(3, ros_ld32(p)), p += 4;
        if (type == 25) {
            if (c & 3)
                set(BORDER + (c & 3), 3, ros_ld32(p));
            p += 4;
        }
    }
    uint32_t nc = vdu.dmv[MV_NCOLOUR];
    if (type >= 19 || nc >= 256)
        return;
    uint32_t f = vdu.dmv[MV_FLAGS] & ~MF_GREYSCALE;
    if (nc == 63)
        nc = 255;
    int grey = 1;
    for (uint32_t i = 0; i <= nc && grey; i++) {
        uint32_t x = vdu.pal[0][i] ^ vdu.pal[0][i] << 8, y = vdu.pal[1][i] ^ vdu.pal[1][i] << 8;
        if (x >= 0x10000 || y >= 0x10000)
            grey = 0;
    }
    if (grey)
        f |= MF_GREYSCALE;
    vdu.dmv[MV_FLAGS] = f;
    if (!vdu.dest_sprite)              /* the live copy, unless output is to a sprite */
        vdu.mv[MV_FLAGS] = f;
}

/* PV_BlankScreen. R0 is 1 to blank, 0 to unblank, or anything else to
 * read. R0 returns the old state. */
static void blank_screen(struct ros_cpu *s)
{
    uint32_t old = vdu.blanked;
    if (s->r[0] <= 1 && s->r[0] != old) {
        vdu.blanked = s->r[0];
        uint32_t r[4] = { vdu.blanked, 0, 0, 0 };
        vdu_graphicsv(GV_SET_BLANK, r);
        push();                         /* UpdateAllPalette */
    }
    s->r[0] = old;
}

/* OS_UpdateMEMC (&1A) is MEMC1's control register, which RISC OS 5 keeps
 * as a soft copy for old programs (Kernel s/ExtraSWIs, SSETMEMC). R0 is
 * the new bits and R1 is the bits to change. R0 returns the old value. The
 * top 12 bits are forced to &036, where MEMC1 sat in the address map. Of
 * the other bits only bit 10 does anything. It is video DMA, and when it is
 * clear the screen is blanked through GraphicsV SetBlank. This happens on
 * every call, as 5.30 does it. The soft copy starts at &400, as 5.30's
 * does. The top bits are not forced until a write (#120). */
static uint32_t memc_soft = 0x400;

void ros_thunk_OS_UpdateMEMC(struct ros_cpu *s)
{
    uint32_t old = memc_soft;
    uint32_t v = (old & ~s->r[1]) | (s->r[0] & s->r[1]);
    memc_soft = (v & 0x000FFFFFu) | 0x03600000u;
    uint32_t r[4] = { memc_soft & 1u << 10 ? 0u : 1u, 0, 0, 0 };
    vdu_graphicsv(GV_SET_BLANK, r);
    s->r[0] = old;
    s->v = 0;
}

/* The kernel's PaletteV owner (MOSPaletteV), for the reasons above. */
static int palette_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    switch (s->r[4]) {
    case PV_READ: {
        int32_t i = pv_index(s->r[0], s->r[1]);
        if (i >= 0)
            s->r[2] = vdu.pal[0][i], s->r[3] = vdu.pal[1][i];
        break;
    }
    case PV_SET:
        if (!palette_set(s->r[0], s->r[1], s->r[2]))
            return ROS_VECTOR_PASS;
        push();
        break;
    case PV_SETDEFAULT:
        set_defaults();
        break;
    case PV_BLANKSCREEN:
        blank_screen(s);
        break;
    case PV_BULKREAD:
        bulk_read(s);
        break;
    case PV_BULKWRITE:
        bulk_write(s);
        push();
        break;
    default:
        return ROS_VECTOR_PASS;
    }
    s->r[4] = 0;
    return ROS_VECTOR_CLAIM;
}

void vdu_palette_init(void)
{
    ros_vector_claim_native(PALETTEV, palette_v, 0);
}

/* OS_ReadPalette (SWIReadPalette). R0 is the colour and R1 is 16, 24 or
 * 25. */
void vdu_palette_read(struct ros_cpu *s)
{
    struct ros_cpu c = *s;
    call_pv(&c, PV_READ);
    uint32_t type = s->r[1];
    uint32_t a = (c.r[2] & ~0x7Fu) | type, b = (c.r[3] & ~0x7Fu) | type;
    if (type == 16 && a != b)
        a |= 1, b |= 2;
    s->r[2] = a, s->r[3] = b;
}

/* OS_Word 11. Byte 0 is the colour. Byte 1 is PP and bytes 2 to 4 are red,
 * green and blue, as on show. */
void vdu_palette_word11(uint32_t block)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_ld8(block), s.r[1] = 16;
    vdu_palette_read(&s);
    uint32_t c = vdu.flash_state ? s.r[3] : s.r[2];
    for (int i = 0; i < 4; i++)
        ros_st8(block + 1 + (uint32_t)i, (c >> (8 * i)) & 0xFF);
}

/* OS_Word 12 takes VDU 19's five bytes. */
void vdu_palette_word12(uint32_t block)
{
    uint8_t q[5];
    for (int i = 0; i < 5; i++)
        q[i] = (uint8_t)ros_ld8(block + (uint32_t)i);
    vdu_palette_vdu19(q);
}
