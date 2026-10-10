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
 * (Sources/Video/Render/Fonts/FontManager: s.Fonts02).
 */

/* vduhooks.c: the Font Manager through the VDU stream.  It follows
 * s/Fonts02's UKVDU23Ventry, UKPLOTVentry, VDUXVentry, setfx3 and clrfx3.
 * Four sequences are handled.
 *
 *   - VDU 23,26,n,size,xres,yres,xscale,yscale, then a font name.  The
 *     name is collected, then DefineFont gives handle n the font at size
 *     times the scale.  The scale is in 16ths and 0 means 16.  The
 *     encoding is the one the last FindFont found.
 *   - VDU 23,25,b,t1..t7 with b below &80.  These are the thresholds for
 *     2^b colours.  A b of 4 means the current 16-colour thresholds.  They
 *     are set through settransfer, as Font_SetThresholds does.
 *   - VDU 23,25,&80+bg,fg,r,g,b,R,G,B.  These are the colours, set as
 *     Font_SetPalette sets them, with 12-bit RGBs.
 *   - PLOT &D0 to &D7, then a string.  The VDU's variables are read at
 *     the plot and the string is collected. It is then painted at the
 *     plot's point.  Bits 0 and 1 of the plot code are Font_Paint's
 *     justify and rub-out flags.  Bit 2 does nothing.
 *
 * Errors from these, such as too many colours or a font not found, are
 * returned by OS_WriteC, as errors from the VDU are.
 *
 * UKVDU23V and UKPLOTV are claimed while the module lives.  A string is
 * collected by setting bit 5 of OS_Byte 3, so that the VDU's characters
 * go to VDUXV.  VDUXV is claimed only while a string is being collected.
 * The collector skips the parameters of the string's control sequences
 * without looking at them.  It ends at the first other control character,
 * which is kept as the terminator.  As in the original, it does not know
 * 27 and 28, so they end the string.  A comment starts with 21 and ends at
 * the next control character, which is kept as data.  Leading NULs are
 * dropped.  A string of 256 bytes gives BuffOverflow.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "fm.h"

#define UKVDU23V 0x17u
#define UKPLOTV  0x19u
#define VDUXV    0x1Bu

enum {                                  /* control codes in the string */
    MOVEX = 9, MOVEY = 11, COLOUR = 17, COLOUR3 = 18, COLOURRGB = 19, COMMENT = 21,
    UNDERLINE = 25, FONTCHAR = 26,
};

static uint32_t wrchblk;                /* the string, 256 bytes in the RMA */
static uint32_t wrchblkptr;             /* the VDU's queue, holding the parameters */
static uint8_t wrchvflag;               /* 23 or 25: what the string is for */
static uint8_t wrchindex, wrchflag;     /* bytes stored, bytes of a sequence still to skip */
static uint32_t plottype;
static int fx3flag;                     /* VDUXV claimed */

static int vduxv(struct ros_cpu *s, uint32_t r12);

static uint32_t os_byte(uint32_t n, uint32_t r1, uint32_t r2)
{
    uint32_t r[10] = { n, r1, r2 };
    os_error *e;
    fm_swi(XOS_Byte, r, &e);
    return r[1];
}

/* setfx3: start collecting the string that follows, for kind 23 or 25 */
static void setfx3(uint8_t kind, uint32_t block)
{
    wrchvflag = kind;
    wrchblkptr = block;
    wrchindex = wrchflag = 0;
    os_byte(236, 0x20, 0xDF);
    fx3flag = 1;
    ros_vector_claim_native(VDUXV, vduxv, 0);
}

static void clrfx3(void)
{
    os_byte(236, 0, 0xDF);
    fx3flag = 0;
    ros_vector_release_native(VDUXV, vduxv, 0);
}

static int fail(struct ros_cpu *s, os_error *e)
{
    if (e) {
        s->r[0] = ros_addr(e);
        s->v = 1;
    }
    return ROS_VECTOR_CLAIM;
}

/* ---- UKVDU23V: VDU 23,25 and 23,26 ------------------------------------------------------ */

static int ukvdu23(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t q = s->r[1];
    if (s->r[0] == 26) {
        setfx3(23, q);
        return ROS_VECTOR_CLAIM;
    }
    if (s->r[0] != 25)
        return ROS_VECTOR_PASS;
    uint32_t b = ros_ld8(q + 1);
    if (b >= 0x80)
        return fail(s, fm_vdu_palette(q));
    /* 2^b colours.  Too many is an error, which the VDU returns. */
    const uint8_t *t = b == 4 ? &fm_thresholds[fm_thresh_offset(14)] : ros_ptr(q + 2);
    return fail(s, fm_settransfer((int32_t)((b < 32 ? 1u << b : 0) - 2), t));
}

/* ---- UKPLOTV: PLOT &D0-&D7 ------------------------------------------------------------- */

static int ukplot(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] - 0xD0 >= 8)
        return ROS_VECTOR_PASS;
    plottype = s->r[0] & 7;
    fm_check_blend();
    os_error *e = fm_mode_vars();       /* read the plot's point and the cursors now */
    if (!e)
        setfx3(25, s->r[1]);
    return fail(s, e);
}

/* ---- VDUXV: the string -------------------------------------------------------------------- */

static os_error *finished(void)
{
    uint32_t q = wrchblkptr;
    if (wrchvflag == 25)
        return fm_paint_vdu(wrchblk, plottype);
    uint32_t size = ros_ld8(q + 2), xs = ros_ld8(q + 5), ys = ros_ld8(q + 6);
    int32_t xscale = (int32_t)(size * (xs ? xs : 16)), yscale = (int32_t)(size * (ys ? ys : 16));
    os_error *e = fm_mode_vars();
    if (!e)
        e = fm_define_font(ros_ld8(q + 1), ros_ptr(wrchblk), xscale, yscale, ros_ld8(q + 3),
                           ros_ld8(q + 4));
    return e;
}

static int vduxv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint8_t c = (uint8_t)s->r[0];
    uint32_t i = wrchindex;
    s->c = 0;                           /* clear C: the character is not for the printer */
    if (!i && !c)
        return ROS_VECTOR_CLAIM;        /* leading NULs */
    if (wrchflag == 0xFF) {             /* a comment: runs to a control character */
        if (c < 32)
            wrchflag = 0;
    } else if (wrchflag) {
        wrchflag--;                     /* the parameters of a sequence */
    } else if (c == COLOUR || c == FONTCHAR) {
        wrchflag = 1;
    } else if (c == UNDERLINE) {
        wrchflag = 2;
    } else if (c == COLOURRGB) {
        wrchflag = 7;
    } else if (c == COLOUR3 || c == MOVEX || c == MOVEY) {
        wrchflag = 3;
    } else if (c == COMMENT) {
        wrchflag = 0xFF;
    } else if (c < 32) {                /* the terminator */
        ros_st8(wrchblk + i, c);
        clrfx3();
        return fail(s, finished());
    }
    ros_st8(wrchblk + i, c);
    if (++i >= 256) {
        clrfx3();
        return fail(s, fm_err(FE_BUFFOVERFLOW, NULL, NULL));
    }
    wrchindex = (uint8_t)i;
    return ROS_VECTOR_CLAIM;
}

/* ---- the module ------------------------------------------------------------------------------ */

os_error *fm_vdu_hooks_claim(void)
{
    if (!wrchblk)
        wrchblk = ros_addr(ros_rma_alloc(256));
    os_error *e = ros_vector_claim_native(UKPLOTV, ukplot, 0);
    if (!e)
        e = ros_vector_claim_native(UKVDU23V, ukvdu23, 0);
    return e;
}

/* Die: release VDUXV if it is claimed.  The bit set in OS_Byte 3 is left
 * as it is. */
void fm_vdu_hooks_release(void)
{
    if (fx3flag)
        ros_vector_release_native(VDUXV, vduxv, 0);
    fx3flag = 0;
    ros_vector_release_native(UKVDU23V, ukvdu23, 0);
    ros_vector_release_native(UKPLOTV, ukplot, 0);
}
