/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2016 Castle Technology Ltd
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
 * (Sources/Kernel: s.vdu.vdumodes, s.vdu.vdudriver, s.vdu.vdugrafl,
 * s.vdu.vduswis) and its ScreenModes module (Sources/Video/UserI/ScrModes:
 * c.ScrModes).
 */
/* modes.c -- screen modes: the numbered modes, mode selectors, changing
 * mode through GraphicsV, and the SWIs that read what a mode is.
 *
 * This follows the kernel's Kernel/s/vdu/vdumodes, vdudriver
 * (ModeChangeSub and GenerateModeSelectorVars), vdugrafl and vduswis.
 *
 *   - A mode is a number (0-53) or a mode selector. For a number, bit 7
 *     is the shadow bank, which is ignored because there is one bank. A
 *     mode selector is flags 1, xres, yres, log2bpp and frame rate,
 *     followed by (variable, value) pairs up to -1. Its variables start as
 *     GenerateModeSelectorVars makes them. NColour is 63 at 8 bpp unless
 *     the pairs say 255 and FullPalette. XEig is 1. YEig is 2 if yres is
 *     under half of xres. The rest come from the size.
 *   - The numbered modes' variables are exactly the kernel's table,
 *     Vwstab. They come to GraphicsV as a VIDC list, type 3. Its display
 *     size is the framestore's: LineLength*8 >> Log2BPP pixels wide by
 *     YWindLimit+1 rows. Double-pixel modes are twice their XWindLimit
 *     wide.
 *   - Changing mode calls GraphicsV_VetMode, Service_ModeChanging, SetMode
 *     and FramestoreAddress, as the kernel calls them for a driver with a
 *     framestore of its own. Then LineLength and ScreenSize are taken from
 *     the list, as the kernel recomputes them. After that come the VDU's
 *     workspace, the default palette, CLS and Service_ModeChange. A mode
 *     that the driver refuses gives "Screen mode not available", as it
 *     does when nothing translates it.
 *   - Teletext. MODE 7 (and 135) becomes what RISC OS 5.30 makes it on the
 *     farm's Pi. No monitor there lists mode 7's 640 x 500. ScreenModes
 *     (Service_ModeTranslation) takes the first of its fallback sizes that
 *     the monitor has and that holds it. The result is a mode selector for
 *     800 x 600 at 8 bpp, with teletext's flags, 255 colours and 40 x 25
 *     characters. This then sets the mode as any selector does. So
 *     OS_ScreenMode 1 and OS_Byte 135 give its address, and
 *     OS_CheckModeValid says that 7 is not valid but has that substitute.
 *     OS_ReadModeVariable 7 still reads Vwstab's value.
 *   - A selector whose flags say teletext is fixed up as
 *     GenerateModeSelectorVars does. This happens when the mode is set or
 *     checked, but not when OS_ReadModeVariable reads it. The fix-up makes
 *     the mode non-graphic, with gap and double-vertical set. It needs 16
 *     colours or more, but not 64. ScreenSize is doubled for the two flash
 *     banks, and the characters are 16 x 20 and must fit. ttx.c draws it,
 *     with the page centred on the screen (TextOffset).
 *
 * Scrolling always copies. The VDU never asks GraphicsV to move the
 * display's start (HardScrollDisabled's way), which the kernel does too
 * for a driver that cannot. The pixels are the same.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "drmvideo.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/heap.h"
#include "rosgd/keyboard.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "vduws.h"

#define ERR_BAD_MODE 0x19u               /* "Bad MODE" */
#define ERR_MODE_NOT_AVAILABLE 0x1EDu    /* "Screen mode not available" */
#define ERR_BAD_PIXEL_DEPTH 0x1EEu
#define ERR_BAD_PARAMETERS 0x1ECu        /* "Bad parameters" */
#define ERR_BUFF_OVERFLOW 0x1E4u         /* "Buffer overflow" */

#define SERVICE_MODECHANGE 0x46u
#define SERVICE_MODECHANGING 0x89u
#define SERVICE_ENUMERATESCREENMODES 0x8Du
#define MAXMODE 53u

enum {
    GV_SET_MODE = 2, GV_DISPLAY_FEATURES = 4, GV_VET_MODE = 7, GV_FRAMESTORE_ADDRESS = 9,
    GV_STARTUP_MODE = 16, GV_PIXEL_FORMATS = 17,
};
enum { CL_EXTRA_BYTES = 14, CL_NCOLOUR = 15, CL_MODE_FLAGS = 16 };

struct vdu vdu;

static uint32_t vidc_list;              /* the list GraphicsV is given: RMA */
static uint32_t pixel_rate;             /* kHz, VDU variable 173 */

/* ---- the numbered modes (vdumodes, VWSTAB) ------------------------------ */

#define K *1024
#define M23S (1152 * 896 / 8)
#define M25S (640 * 480 / 8)
#define M31S (800 * 600 / 8)
#define M37S (896 * 352 / 8)
#define M41S (640 * 352 / 8)
#define M44S (640 * 200 / 8)
#define M47S (360 * 480 / 8)
#define M50S (320 * 240 / 8)
#define NGG (MF_NONGRAPHIC | MF_GAP | MF_BBCGAP)

/* In VWSTAB's order: ScreenSize, LineLength, XWindLimit, YWindLimit,
 * YShftFactor, XEig, YEig, NColour, ScrRCol, ScrBRow, Log2BPC, Log2BPP,
 * ModeFlags */
static const uint32_t vwstab[MAXMODE + 1][13] = {
    { 20 K, 80, 639, 255, 4, 1, 2, 1, 79, 31, 0, 0, 0 },
    { 20 K, 80, 319, 255, 4, 2, 2, 3, 39, 31, 1, 1, 0 },
    { 40 K, 160, 159, 255, 5, 3, 2, 15, 19, 31, 3, 2, 0 },
    { 40 K, 160, 639, 249, 5, 1, 2, 1, 79, 24, 1, 1, NGG },
    { 20 K, 80, 319, 255, 4, 2, 2, 1, 39, 31, 1, 0, 0 },
    { 20 K, 80, 159, 255, 4, 3, 2, 3, 19, 31, 2, 1, 0 },
    { 20 K, 80, 319, 249, 4, 2, 2, 1, 39, 24, 1, 1, NGG },
    { 640 K, 640, 639, 499, 5, 1, 1, 255, 39, 24, 3, 3,
      MF_NONGRAPHIC | MF_GAP | MF_TELETEXT | MF_DOUBLEVERTICAL | MF_FULLPALETTE },
    { 40 K, 160, 639, 255, 5, 1, 2, 3, 79, 31, 1, 1, 0 },
    { 40 K, 160, 319, 255, 5, 2, 2, 15, 39, 31, 2, 2, 0 },
    { 80 K, 320, 159, 255, 6, 3, 2, 63, 19, 31, 4, 3, 0 },
    { 40 K, 160, 639, 249, 5, 1, 2, 3, 79, 24, 1, 1, MF_GAP },
    { 80 K, 320, 639, 255, 6, 1, 2, 15, 79, 31, 2, 2, 0 },
    { 80 K, 320, 319, 255, 6, 2, 2, 63, 39, 31, 3, 3, 0 },
    { 80 K, 320, 639, 249, 6, 1, 2, 15, 79, 24, 2, 2, MF_GAP },
    { 160 K, 640, 639, 255, 7, 1, 2, 63, 79, 31, 3, 3, 0 },
    { 132 K, 528, 1055, 255, 0, 1, 2, 15, 131, 31, 2, 2, 0 },
    { 132 K, 528, 1055, 249, 0, 1, 2, 15, 131, 24, 2, 2, MF_GAP },
    { 40 K, 80, 639, 511, 4, 1, 1, 1, 79, 63, 0, 0, 0 },
    { 80 K, 160, 639, 511, 5, 1, 1, 3, 79, 63, 1, 1, 0 },
    { 160 K, 320, 639, 511, 6, 1, 1, 15, 79, 63, 2, 2, 0 },
    { 320 K, 640, 639, 511, 7, 1, 1, 63, 79, 63, 3, 3, 0 },
    { 108 K, 384, 767, 287, 0, 0, 1, 15, 95, 35, 2, 2, 0 },
    { M23S, 144, 1151, 895, 0, 1, 1, 1, 143, 55, 0, 0, MF_HIRESMONO | MF_DOUBLEVERTICAL },
    { 264 K, 1056, 1055, 255, 0, 1, 2, 63, 131, 31, 3, 3, 0 },
    { M25S, 80, 639, 479, 4, 1, 1, 1, 79, 59, 0, 0, 0 },
    { M25S * 2, 160, 639, 479, 5, 1, 1, 3, 79, 59, 1, 1, 0 },
    { M25S * 4, 320, 639, 479, 6, 1, 1, 15, 79, 59, 2, 2, 0 },
    { M25S * 8, 640, 639, 479, 7, 1, 1, 63, 79, 59, 3, 3, 0 },
    { M31S, 100, 799, 599, 0, 1, 1, 1, 99, 74, 0, 0, 0 },
    { M31S * 2, 200, 799, 599, 0, 1, 1, 3, 99, 74, 1, 1, 0 },
    { M31S * 4, 400, 799, 599, 0, 1, 1, 15, 99, 74, 2, 2, 0 },
    { M31S * 8, 800, 799, 599, 0, 1, 1, 63, 99, 74, 3, 3, 0 },
    { 27 K, 96, 767, 287, 0, 1, 2, 1, 95, 35, 0, 0, 0 },
    { 54 K, 192, 767, 287, 0, 1, 2, 3, 95, 35, 1, 1, 0 },
    { 108 K, 384, 767, 287, 0, 1, 2, 15, 95, 35, 2, 2, 0 },
    { 216 K, 768, 767, 287, 0, 1, 2, 63, 95, 35, 3, 3, 0 },
    { M37S, 112, 895, 351, 0, 1, 2, 1, 111, 43, 0, 0, 0 },
    { M37S * 2, 224, 895, 351, 0, 1, 2, 3, 111, 43, 1, 1, 0 },
    { M37S * 4, 448, 895, 351, 0, 1, 2, 15, 111, 43, 2, 2, 0 },
    { M37S * 8, 896, 895, 351, 0, 1, 2, 63, 111, 43, 3, 3, 0 },
    { M41S, 80, 639, 351, 0, 1, 2, 1, 79, 43, 0, 0, 0 },
    { M41S * 2, 160, 639, 351, 0, 1, 2, 3, 79, 43, 1, 1, 0 },
    { M41S * 4, 320, 639, 351, 0, 1, 2, 15, 79, 43, 2, 2, 0 },
    { M44S, 80, 639, 199, 0, 1, 2, 1, 79, 24, 0, 0, 0 },
    { M44S * 2, 160, 639, 199, 0, 1, 2, 3, 79, 24, 1, 1, 0 },
    { M44S * 4, 320, 639, 199, 0, 1, 2, 15, 79, 24, 2, 2, 0 },
    { M47S * 8, 360, 359, 479, 0, 2, 2, 63, 44, 59, 3, 3, 0 },
    { 75 K, 160, 319, 479, 0, 2, 1, 15, 39, 59, 2, 2, 0 },
    { 150 K, 320, 319, 479, 0, 2, 1, 63, 39, 59, 3, 3, 0 },
    { M50S, 40, 319, 239, 0, 2, 2, 1, 39, 29, 0, 0, 0 },
    { M50S * 2, 80, 319, 239, 0, 2, 2, 3, 39, 29, 1, 1, 0 },
    { M50S * 4, 160, 319, 239, 0, 2, 2, 15, 39, 29, 2, 2, 0 },
    { M50S * 8, 320, 319, 239, 0, 2, 2, 63, 39, 29, 3, 3, 0 },
};

static void from_table(uint32_t n, uint32_t mv[MV_COUNT])
{
    const uint32_t *t = vwstab[n];
    mv[MV_SCREENSIZE] = t[0], mv[MV_LINELENGTH] = t[1], mv[MV_XWIND] = t[2];
    mv[MV_YWIND] = t[3], mv[MV_YSHFT] = t[4], mv[MV_XEIG] = t[5], mv[MV_YEIG] = t[6];
    mv[MV_NCOLOUR] = t[7], mv[MV_SCRRCOL] = t[8], mv[MV_SCRBROW] = t[9];
    mv[MV_LOG2BPC] = t[10], mv[MV_LOG2BPP] = t[11], mv[MV_FLAGS] = t[12];
}

/* ---- mode selectors (GenerateModeSelectorVars) --------------------------- */

static int is_selector(uint32_t mode)
{
    return mode >= 256 && !(mode & 3);
}

static os_error *from_selector(uint32_t sel, uint32_t mv[MV_COUNT])
{
    if (!ros_arena_readable(sel, sel + 20))
        return ros_error(ERR_BAD_MODE, "Bad MODE");
    uint32_t flags = ros_ld32(sel), xres = ros_ld32(sel + 4), yres = ros_ld32(sel + 8);
    uint32_t log2bpp = ros_ld32(sel + 12);
    if ((flags & 0xFF) != 1 || !xres || !yres || xres > 16384 || yres > 16384)
        return ros_error(ERR_BAD_MODE, "Bad MODE");     /* ScreenModes refuses these first */
    if (log2bpp > 5)
        return ros_error(ERR_BAD_PIXEL_DEPTH, "Bad pixel depth");
    static const uint32_t ncolour[6] = { 1, 3, 15, 63, 0xFFFF, 0xFFFFFFFFu };
    mv[MV_LOG2BPP] = mv[MV_LOG2BPC] = log2bpp;
    mv[MV_NCOLOUR] = ncolour[log2bpp];
    mv[MV_FLAGS] = 0;
    mv[MV_XEIG] = 1;
    mv[MV_YEIG] = yres < xres / 2 ? 2 : 1;
    mv[MV_SCRRCOL] = xres / 8 - 1;
    mv[MV_SCRBROW] = yres / 8 - 1;
    mv[MV_XWIND] = xres - 1;
    mv[MV_YWIND] = yres - 1;
    mv[MV_LINELENGTH] = (xres << log2bpp) / 8;
    mv[MV_SCREENSIZE] = mv[MV_LINELENGTH] * yres;
    mv[MV_YSHFT] = 0;
    uint32_t p = sel + 20;
    for (int n = 0; n < 64; n++, p += 8) {
        if (!ros_arena_readable(p, p + 4))
            return ros_error(ERR_BAD_MODE, "Bad MODE");
        uint32_t index = ros_ld32(p);
        if (index == 0xFFFFFFFFu)
            break;
        if (index < MV_COUNT)
            mv[index] = ros_ld32(p + 4);
    }
    if (mv[MV_FLAGS] & MF_TELETEXT)
        return NULL;                    /* ttx_fixup's, when the mode is used */
    /* These are the limits of the kernel's character plotters. Double
     * vertical is allowed at 1 bpp only, and not with double pixels. BBC
     * gap modes are allowed at 2 bpp. Double pixels are allowed up to 16
     * bpp. */
    if (((mv[MV_FLAGS] & MF_DOUBLEVERTICAL) && (mv[MV_LOG2BPP] != 0 || mv[MV_LOG2BPC] != 0)) ||
        ((mv[MV_FLAGS] & MF_BBCGAP) && mv[MV_LOG2BPP] != 1) || mv[MV_LOG2BPC] > 5)
        return ros_error(ERR_BAD_PIXEL_DEPTH, "Bad pixel depth");
    return NULL;
}

/* ScreenModes' part (its Service_ModeExtension, Video/UserI/ScrModes). The
 * kernel asks it before anything else about a mode selector. It returns 1
 * if it would take the selector. The selector's pixel format must be one
 * that the driver lists (GraphicsV_PixelFormats). The format is NColour
 * and ModeFlags as the selector's pairs give them, or else as its depth
 * does (255 colours and FullPalette at 8 bpp). The flags that only the
 * kernel cares about are masked off, and 64 colours at 8 bpp is made 256.
 * A driver without the call has the old six formats, at a depth that its
 * DisplayFeatures has. The monitor list is represented here by a size,
 * which the driver's vet then judges. A selector that it would not take
 * gives "Screen mode not available", before the kernel's own checks. An
 * example is an 8 bpp selector whose flags are given without FullPalette,
 * as on the farm's Pi. */
#define MF_KERNEL_ONLY (MF_NONGRAPHIC | MF_TELETEXT | MF_GAP | MF_BBCGAP | MF_HIRESMONO | \
                        MF_DOUBLEVERTICAL | MF_HARDSCROLLOFF | MF_INTERLACED)
#define MF_FAMILY (3u << 12)

static int scrmodes_takes(uint32_t sel)
{
    static const uint32_t old[6][3] = {
        { 1, 0, 0 }, { 3, 0, 1 }, { 15, 0, 2 }, { 255, MF_FULLPALETTE, 3 }, { 65535, 0, 4 },
        { 0xFFFFFFFFu, 0, 5 },
    };
    if (!ros_arena_readable(sel, sel + 20))
        return 1;                       /* from_selector refuses it */
    uint32_t xres = ros_ld32(sel + 4), yres = ros_ld32(sel + 8), depth = ros_ld32(sel + 12);
    if ((ros_ld32(sel) & 0xFF) != 1 || !xres || !yres || xres > 16384 || yres > 16384)
        return 0;
    uint32_t ncolour = depth < 5 ? (1u << (1u << depth)) - 1 : 0xFFFFFFFFu;
    uint32_t flags = depth == 3 ? MF_FULLPALETTE : 0;
    for (uint32_t p = sel + 20, n = 0; n < 64; n++, p += 8) {
        if (!ros_arena_readable(p, p + 4) || ros_ld32(p) == 0xFFFFFFFFu)
            break;
        if (ros_ld32(p) == MV_FLAGS)
            flags = ros_ld32(p + 4);
        else if (ros_ld32(p) == MV_NCOLOUR)
            ncolour = ros_ld32(p + 4);
    }
    if ((flags & MF_BBCGAP) && depth == 1 && ncolour == 1)
        ncolour = 3;
    flags &= ~MF_KERNEL_ONLY;
    if ((flags & MF_FAMILY) == 0) {     /* RGB */
        if (ncolour == 63 && depth == 3)
            ncolour = 255, flags |= MF_FULLPALETTE;
        flags &= ~MF_GREYSCALE;
    }
    if (flags & ~(MF_FULLPALETTE | MF_GREYSCALE | MF_FORMAT))
        return 0;
    uint32_t r[4] = { 0 };
    if (vdu_graphicsv(GV_PIXEL_FORMATS, r)) {
        for (uint32_t i = 0, at = r[0]; i < r[1]; i++, at += 12)
            if (ros_ld32(at) == ncolour && ros_ld32(at + 4) == flags && ros_ld32(at + 8) == depth)
                return 1;
        return 0;
    }
    for (unsigned i = 0; i < 6; i++)
        if (old[i][0] == ncolour && old[i][1] == flags && old[i][2] == depth) {
            memset(r, 0, sizeof r);
            return !vdu_graphicsv(GV_DISPLAY_FEATURES, r) || (r[1] >> depth & 1);
        }
    return 0;
}

/* GenerateModeSelectorVars' teletext fix-ups, for a mode about to be used
 * or checked. OS_ReadModeVariable reads a selector as it stands, as the
 * kernel's does. The fix-ups make the mode non-graphic, with gap and
 * double-vertical set. It needs 16 colours or more, but not 64.
 * ScreenSize is doubled for the two flash banks. The characters are 16 x
 * 20 and must fit, with at most 255 columns (TTXDoubleCounts are bytes). */
static os_error *ttx_fixup(uint32_t mode, uint32_t mv[MV_COUNT])
{
    if (!is_selector(mode) || !(mv[MV_FLAGS] & MF_TELETEXT))
        return NULL;
    uint32_t xres = ros_ld32(mode + 4), yres = ros_ld32(mode + 8);
    mv[MV_FLAGS] |= MF_NONGRAPHIC | MF_GAP | MF_DOUBLEVERTICAL;
    if (mv[MV_NCOLOUR] < 15 || mv[MV_NCOLOUR] == 63)
        return ros_error(ERR_BAD_PIXEL_DEPTH, "Bad pixel depth");
    mv[MV_SCREENSIZE] <<= 1;
    if (mv[MV_SCRRCOL] > 254)
        mv[MV_SCRRCOL] = 254;
    if (mv[MV_SCRRCOL] > xres / 16 - 1)
        mv[MV_SCRRCOL] = xres / 16 - 1;
    if (mv[MV_SCRBROW] >= yres / 20)
        mv[MV_SCRBROW] = yres / 20 - 1;
    return NULL;
}

/* Whether a mode's variables describe a screen that fits the display of w
 * by h pixels. A mode selector can set any variable with its pairs. The
 * drawing code takes the window and the text grid from the variables and
 * addresses the screen with them, so a mode whose window or text grid is
 * larger than the screen is refused. All of the numbered modes fit. The
 * sums are done in 64 bits so that a large value cannot wrap. */
static int geometry_ok(const uint32_t mv[MV_COUNT], uint32_t w, uint32_t h)
{
    if (mv[MV_LOG2BPP] > 5 || mv[MV_LOG2BPC] > 5)
        return 0;
    uint64_t logical = ((uint64_t)w << mv[MV_LOG2BPP]) >> mv[MV_LOG2BPC];
    uint32_t f = mv[MV_FLAGS];
    uint64_t tx = f & MF_TELETEXT ? 16 : 8, ty = f & MF_DOUBLEVERTICAL ? 16 : 8;
    uint64_t rows = f & MF_GAP ? ty + ty / 4 : ty;
    return (uint64_t)mv[MV_XWIND] + 1 <= logical && (uint64_t)mv[MV_YWIND] + 1 <= h &&
           ((uint64_t)mv[MV_SCRRCOL] + 1) * tx <= (uint64_t)mv[MV_XWIND] + 1 &&
           ((uint64_t)mv[MV_SCRBROW] + 1) * rows <= (uint64_t)mv[MV_YWIND] + 1;
}

/* MODE 7 as the farm's ScreenModes translates it (Service_ModeTranslation).
 * It is 800 x 600 at 8 bpp, with the variables that it keeps from Vwstab's
 * mode 7. */
static const uint32_t mode7_selector[] = {
    1, 800, 600, 3, 0xFFFFFFFFu,
    MV_FLAGS, MF_NONGRAPHIC | MF_TELETEXT | MF_GAP | MF_DOUBLEVERTICAL | MF_FULLPALETTE,
    MV_NCOLOUR, 255, MV_XEIG, 1, MV_YEIG, 1, MV_SCRRCOL, 39, MV_SCRBROW, 24, 0xFFFFFFFFu,
};
static uint32_t translated;             /* ScreenModes' static_mode, in the RMA */

/* Returns the address of a numbered teletext mode's substitute. */
static uint32_t translate(void)
{
    memcpy(ros_ptr(translated), mode7_selector, sizeof mode7_selector);
    return translated;
}

/* The display's size, as the VIDC list gives it. */
static void display_size(uint32_t mode, const uint32_t mv[MV_COUNT], uint32_t *w, uint32_t *h)
{
    if (is_selector(mode)) {
        *w = ros_ld32(mode + 4);
        *h = ros_ld32(mode + 8);
    } else {
        *w = (mv[MV_LINELENGTH] * 8) >> mv[MV_LOG2BPP];
        *h = mv[MV_YWIND] + 1;
    }
}

/* A sprite mode word's variables (GenerateModeSelectorVars). The types are
 * 1-18. The resolution is dpi 180, 90, 45 or 22, or the RISC OS 5 form's
 * eig bits. The size is a sprite's own, so the size variables are left as
 * the table's mode 0 has them. */
static int dpi_eig(uint32_t dpi)
{
    return dpi == 180 ? 0 : dpi == 90 ? 1 : dpi == 45 ? 2 : (dpi == 22 || dpi == 23) ? 3 : -1;
}

static os_error *from_sprite_word(uint32_t m, uint32_t mv[MV_COUNT])
{
    static const uint8_t bpp_of[19] = { 0, 0, 1, 2, 3, 4, 5, 5, 5, 5, 4, 5, 5, 5, 5, 5, 4, 5, 5 };
    static const uint32_t ncolour[6] = { 1, 3, 15, 63, 0xFFFF, 0xFFFFFFFFu };
    uint32_t type, flags = 0, xeig, yeig;
    int old = 1;
    if (((m >> 27) & 15) == 15 && !(m & 0xF000E)) {
        xeig = (m >> 4) & 3, yeig = (m >> 6) & 3;
        flags = m & 0xFF00;
        type = (m >> 20) & 127;
        if (type == 0)
            type = 6;
        if (type == 10)
            flags |= MF_FULLPALETTE, type = 5;
        old = (flags & ~(uint32_t)(MF_RGB | MF_ALPHA | MF_FULLPALETTE)) || (type < 5 && flags) ||
              type > 18;
        if (old)
            flags = 0;
    }
    if (old) {                          /* the type<<27 form, or its substitute */
        type = (m >> 27) & 15;
        if (type == 15)
            type = 6;
        if (type == 10)
            flags = MF_FULLPALETTE, type = 5;
        int xe = dpi_eig((m >> 1) & 0x1FFF), ye = dpi_eig((m >> 14) & 0x1FFF);
        if (xe < 0 || ye < 0)
            return ros_error(0x719, "Illegal XDPI or YDPI in sprite");
        xeig = (uint32_t)xe, yeig = (uint32_t)ye;
        if (type >= 7 || type == 0)
            type = 6;
    }
    from_table(0, mv);
    uint32_t l2 = bpp_of[type];
    mv[MV_LOG2BPP] = mv[MV_LOG2BPC] = l2;
    mv[MV_NCOLOUR] = type == 16 ? 4095 : ncolour[l2];
    mv[MV_FLAGS] = flags;
    mv[MV_XEIG] = xeig, mv[MV_YEIG] = yeig;
    return NULL;
}

os_error *vdu_mode_vars(uint32_t mode, uint32_t mv[MV_COUNT])
{
    if (is_selector(mode))
        return from_selector(mode, mv);
    if (mode >= 256 && (mode & 1))
        return from_sprite_word(mode, mv);
    if (mode >= 256)
        return ros_error(ERR_BAD_MODE, "Bad MODE");
    mode &= 0x7F;                       /* bit 7: shadow */
    if (mode > MAXMODE)
        return ros_error(ERR_MODE_NOT_AVAILABLE, "Screen mode not available");
    from_table(mode, mv);
    return NULL;
}

/* ---- GraphicsV ------------------------------------------------------------ */

int vdu_graphicsv(uint32_t reason, uint32_t r[4])
{
    if (vdu.vd)
        return 0;                       /* a virtual display has no driver */
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 4 * sizeof r[0]);
    s.r[4] = reason;                    /* driver 0: the only one */
    int claimed = ros_vector_call(ROS_GRAPHICSV, &s) && s.r[4] == 0;
    memcpy(r, s.r, 4 * sizeof r[0]);
    return claimed;
}

/* Builds the VIDC list for a mode. It returns 1 if the driver will show
 * the mode. The driver is given the pixel format's flags of the mode. It is
 * also given teletext's flag, which the kernel keeps back because
 * BCMVideo's framestore holds two screens anyway. DRMVideo needs the flag
 * to make room for the second flash bank. */
static int vet(uint32_t w, uint32_t h, const uint32_t mv[MV_COUNT], uint32_t extra)
{
    uint32_t words[16] = { 3, mv[MV_LOG2BPP], 32, 48, 0, w, 0, 16, 3, 13, 0, h, 0, 3,
                           w * h * 60 / 1000, 0 };
    for (unsigned i = 0; i < 16; i++)
        ros_st32(vidc_list + 4 * i, words[i]);
    uint32_t at = vidc_list + 64;
    if (extra) {
        ros_st32(at, CL_EXTRA_BYTES), ros_st32(at + 4, extra);
        at += 8;
    }
    ros_st32(at, CL_NCOLOUR), ros_st32(at + 4, mv[MV_NCOLOUR]);
    ros_st32(at + 8, CL_MODE_FLAGS);
    ros_st32(at + 12, mv[MV_FLAGS] & (MF_TELETEXT | MF_FULLPALETTE | MF_GREYSCALE | MF_FORMAT));
    ros_st32(at + 16, 0xFFFFFFFFu);
    uint32_t r[4] = { vidc_list };
    return vdu_graphicsv(GV_VET_MODE, r) && r[0] == 0;
}

/* Keeps the selector as the current mode (ModeNo), as the kernel copies
 * it. */
static void keep_selector(uint32_t sel)
{
    uint32_t n = 5;
    while (n < 5 + 2 * 60 && ros_ld32(sel + 4 * n) != 0xFFFFFFFFu)
        n += 2;
    memcpy(ros_ptr(vdu.selector), ros_ptr(sel), 4 * n);
    ros_st32(vdu.selector + 4 * n, 0xFFFFFFFFu);
}

/* Sets the workspace from the mode variables (SwitchOutputToSprite). */
void vdu_derive(void)
{
    uint32_t f = vdu.mv[MV_FLAGS], ttx = f & MF_TELETEXT;
    vdu.bpp = 1u << vdu.mv[MV_LOG2BPP];
    vdu.bpc = 1u << vdu.mv[MV_LOG2BPC];
    vdu.char_width = ttx ? vdu.bpc * 2 : vdu.bpc;       /* teletext: 16 pixels */
    if (f & MF_BBCGAP)
        vdu.cursor_fill = 0x55555555u;
    else if (vdu.mv[MV_LOG2BPP] == 2)
        vdu.cursor_fill = 0x77777777u;
    else if (ttx && vdu.mv[MV_LOG2BPP] == 3)
        vdu.cursor_fill = 0x07070707u;                  /* 256-colour teletext: colour 7 */
    else
        vdu.cursor_fill = 0xFFFFFFFFu;
    vdu.tchar_y = f & MF_DOUBLEVERTICAL ? 16 : 8;
    vdu.tchar_x = ttx ? 16 : 8;
    vdu.row_mult = f & MF_GAP ? vdu.tchar_y + vdu.tchar_y / 4 : vdu.tchar_y;
    vdu.row_length = vdu.mv[MV_LINELENGTH] * vdu.row_mult;
    vdu.text_offset = 0;
    if (ttx) {                          /* TextOffset: the page is centred and word-aligned */
        uint32_t rows = vdu.mv[MV_YWIND] + 1 - vdu.row_mult * (vdu.mv[MV_SCRBROW] + 1);
        uint32_t px = vdu.mv[MV_XWIND] + 1 - vdu.tchar_x * (vdu.mv[MV_SCRRCOL] + 1);
        vdu.text_offset = (rows >> 1) * vdu.mv[MV_LINELENGTH];
        vdu.text_offset += ((px >> 1) << vdu.mv[MV_LOG2BPP]) >> 3;
        vdu.text_offset &= ~3u;
    }
}

static void default_windows(void)
{
    vdu.gwl = vdu.gwb = 0;
    vdu.gwr = (int32_t)vdu.mv[MV_XWIND];
    vdu.gwt = (int32_t)vdu.mv[MV_YWIND];
    vdu.twl = vdu.twt = 0;
    vdu.twr = (int32_t)vdu.mv[MV_SCRRCOL];
    vdu.twb = (int32_t)vdu.mv[MV_SCRBROW];
    vdu.orgx = vdu.orgy = vdu.gcsx = vdu.gcsy = vdu.gcsix = vdu.gcsiy = 0;
    vdu.olderx = vdu.oldery = vdu.oldx = vdu.oldy = vdu.newptx = vdu.newpty = 0;
    if (vdu.dest_sprite)                /* text in a sprite always scrolls by copying */
        vdu.status |= VS_WINDOWING;
    else
        vdu.status &= ~VS_WINDOWING;
    vdu.cx = vdu.twl, vdu.cy = vdu.twt;
}

/* VDU 26 */
void vdu_default_windows(void)
{
    default_windows();
}

/* A mode change. A virtual display has the same change, but without the
 * hardware. There is no ScreenModes check, no GraphicsV and no services.
 * The screen is the display's own area (vdisplay_framestore), and nothing
 * outside the task hears of it. */
os_error *vdu_set_mode(uint32_t mode)
{
    uint32_t mv[MV_COUNT];
    int virt = vdu.vd != NULL;
    if (!virt && is_selector(mode) && !scrmodes_takes(mode))
        return ros_error(ERR_MODE_NOT_AVAILABLE, "Screen mode not available");
    os_error *e = vdu_mode_vars(mode, mv);
    if (e)
        return e;
    if (!is_selector(mode) && (mv[MV_FLAGS] & MF_TELETEXT)) {
        mode = translate();             /* MODE 7, as the farm's ScreenModes makes it */
        if ((e = vdu_mode_vars(mode, mv)) != NULL)
            return e;
    }
    if ((e = ttx_fixup(mode, mv)) != NULL)
        return e;
    uint32_t w, h;
    display_size(mode, mv, &w, &h);
    if (!geometry_ok(mv, w, h))
        return ros_error(ERR_MODE_NOT_AVAILABLE, "Screen mode not available");
    uint32_t bytes = ((w << mv[MV_LOG2BPP]) + 7) / 8;
    uint32_t extra = (4 - bytes % 4) % 4;   /* rows padded to a word, as the driver asks */
    uint32_t r[4] = { vidc_list };
    if (!virt) {
        if (!vet(w, h, mv, extra))
            return ros_error(ERR_MODE_NOT_AVAILABLE, "Screen mode not available");
        struct ros_cpu sc;              /* Service_ModeChanging, before the switch. This is
                                         * vdudriver's IssueModeService: R2 the new mode, R3 the
                                         * monitor type */
        ros_cpu_enter(&sc);
        sc.r[1] = SERVICE_MODECHANGING, sc.r[2] = mode, sc.r[3] = 0xFFFFFFFFu;
        ros_service_call(&sc);
        vdu_graphicsv(GV_SET_MODE, r);
        memset(r, 0, sizeof r);
        if (!vdu_graphicsv(GV_FRAMESTORE_ADDRESS, r))
            return ros_error(ERR_MODE_NOT_AVAILABLE, "Screen mode not available");
    }

    /* LineLength and ScreenSize are recomputed from the list, as the kernel
     * does. HardScrollDisabled is set, as for any driver that cannot move
     * the display. */
    mv[MV_LINELENGTH] = bytes + extra;
    mv[MV_FLAGS] |= MF_HARDSCROLLOFF;
    mv[MV_SCREENSIZE] = mv[MV_LINELENGTH] * h;
    if (mv[MV_FLAGS] & MF_TELETEXT)
        mv[MV_SCREENSIZE] <<= 1;        /* enough for two screens */
    if (virt) {
        memset(r, 0, sizeof r);
        if ((e = vdisplay_framestore(mv, w, h, &r[0], &r[1])) != NULL)
            return e;
    }
    memcpy(vdu.mv, mv, sizeof mv);
    memcpy(vdu.dmv, mv, sizeof mv);
    /* Teletext's HardScrollDisabled. The kernel's SwitchOutput remakes
     * DisplayModeFlags from the mode. It sets the flag only if the page
     * leaves a border (vdugrafl). ModeFlags keeps the driver's value until
     * the default palette's greyscale check copies DisplayModeFlags over
     * it, in modes of 256 colours or fewer (UpdateSettingCommon). So 640 x
     * 480 at 8 bpp has the flag clear, and 800 x 600 at 16 or 32 bpp has it
     * set, with or without a border. ROSGD scrolls by copying whatever the
     * flag says (RISC OS 5.30 hangs). */
    if ((mv[MV_FLAGS] & MF_TELETEXT) && mv[MV_YWIND] + 1 == 20 * (mv[MV_SCRBROW] + 1) &&
        mv[MV_XWIND] + 1 == 16 * (mv[MV_SCRRCOL] + 1)) {
        vdu.dmv[MV_FLAGS] &= ~MF_HARDSCROLLOFF;
        if (mv[MV_NCOLOUR] < 256)
            vdu.mv[MV_FLAGS] &= ~MF_HARDSCROLLOFF;
    }
    if (is_selector(mode)) {
        keep_selector(mode);
        vdu.mode_no = vdu.selector;
    } else {
        vdu.mode_no = mode & 0x7F;
    }
    vdu.dmode_no = vdu.mode_no;
    vdu.dscreen_start = r[0];
    vdu_output_to_screen();             /* a mode change ends output to a sprite */
    if (mv[MV_FLAGS] & MF_TELETEXT)
        vdu.save_area = 0;              /* teletext: no save area (ModeChangeSub's R3) */
    if (!virt)
        pixel_rate = w * h * 60 / 1000;
    vdu.screen_start = r[0];
    vdu.total_size = r[1];
    vdu.screen = ros_ptr(r[0]);
    /* Screen banks. Both are at the default, bank 1. OS_Byte 250 and 251
     * read 0 for "the default", as the kernel's mode change leaves them. */
    vdu.screen_base = vdu.display_start = r[0];
    ros_byte_var_set(0xFA, 0);
    ros_byte_var_set(0xFB, 0);
    vdu.screen_ok = 1;
    vdu_derive();

    vdu.qwant = 0;                      /* the queue purged */
    vdu.gchar_sx = vdu.gchar_sy = vdu.gchar_spx = vdu.gchar_spy = 8;
    vdu.cursor_flags &= 0xFFu | CF_DISABLED;
    vdu_cbox()[0] = 0;                  /* ChangedBox off (ClipBoxEnable) */
    if (mv[MV_FLAGS] & MF_TELETEXT) {   /* TeletextInit */
        vdu.cursor_flags |= CF_TELETEXT;
        vdu_ttx_init(1);
    } else {
        vdu_ttx_final();                /* TeletextFinalise */
    }
    vdu_cursor_init();
    vdu_default_colours();
    vdu_plot_mode();
    default_windows();
    vdu.cur_stack = 0;
    vdu_palette_default();
    vdu_palette_unblank();
    if (!virt)
        vdu_pointer_mode();             /* the pointer is the desktop's */
    vdu_cls();

    if (virt) {
        vdisplay_mode_changed();
        return NULL;
    }
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_MODECHANGE, s.r[2] = vdu.mode_no, s.r[3] = 0xFFFFFFFFu;
    ros_service_call(&s);
    return NULL;
}

/* ---- starting ------------------------------------------------------------- */

static uint32_t start_mode = 28;        /* the driver's start mode, for OS_ReadSysInfo 1 */

uint32_t ros_vdu_start_mode(void)
{
    return start_mode;
}

uint32_t ros_vdu_screen_size(void)
{
    return vdu.total_size;
}

void ros_vdu_init(void)
{
    vidc_list = ros_addr(ros_rma_alloc(128));
    vdu_plot_init();
    vdu_pointer_init();
    vdu_palette_init();
    vdu.selector = ros_addr(ros_rma_alloc(4 * (5 + 2 * 60 + 1)));
    vdu.qq = ros_addr(ros_rma_alloc(16));
    translated = ros_addr(ros_rma_alloc(sizeof mode7_selector));
    memset(ros_ptr(vdu.qq), 0, 16);
    memcpy(vdu.font, ros_vdu_hard_font, sizeof vdu.font);
    vdu.cursor_flags = 0;
    vdu.status = 0;

    vdu_sprite_init();

    /* Until a mode is set, use mode 28's variables, with nowhere to draw. */
    from_table(28, vdu.mv);
    memcpy(vdu.dmv, vdu.mv, sizeof vdu.dmv);
    vdu.mode_no = vdu.dmode_no = 28;
    vdu_derive();
    vdu_cursor_init();
    vdu_default_colours();
    vdu_plot_mode();
    default_windows();

    uint32_t r[4] = { 28 };
    if (!vdu_graphicsv(GV_STARTUP_MODE, r))
        return;                         /* no driver: no screen */
    start_mode = r[0];
    os_error *e = vdu_set_mode(r[0]);
    if (e)
        ros_console_printf("VDU: the startup mode: %s\n", e->errmess);
}

/* ---- enumerating the mode list (OS_ScreenMode 2) ----------------------------
 *
 * The kernel issues Service_EnumerateScreenModes, and ScreenModes answers
 * with its monitor description's modes at every pixel format the driver
 * lists (Video/UserI/ScrModes service_enumeratescreenmodes). Here the
 * answer is made by this reason itself. It is the display's list
 * (platform.h) crossed with GraphicsV_PixelFormats, as DRMVideo lists
 * them. The registers are the kernel's (ScreenMode_EnumerateModes). On
 * entry R2 is the enumeration index, R6 is the block or 0 to count, and R7
 * is its size. On exit R1 = 0 when the block ran short (the service's
 * "claimed"). R2 is stepped down once for each mode. R6 is moved past what
 * was written. R7 is the room left, and it counts negative as it counts.
 * The descriptors are ScreenModes'. Each has a block size, flags (bit 0
 * valid, bit 1 a pixel format follows), xres, yres, then the depth or the
 * format, the frame rate, and the name, word-aligned, such as "1024 x 768",
 * as a monitor description names its modes. Formats that the old six cover
 * are format 0, and the rest are format 1. */
static const uint32_t old_formats[6][3] = {
    { 1, 0, 0 }, { 3, 0, 1 }, { 15, 0, 2 }, { 255, MF_FULLPALETTE, 3 }, { 0xFFFF, 0, 4 },
    { 0xFFFFFFFFu, 0, 5 },
};

/* Writes one descriptor, or only counts it. The index is R2 as it stands.
 * An index of 0 or less writes and counts. An index above 0 only counts
 * down towards the modes asked for. It returns 0 when the block has no
 * room, which is the enumeration's end. */
static int put_entry(uint32_t at, uint32_t room, int32_t index, uint32_t width,
                     uint32_t height, uint32_t rate, uint32_t ncolour, uint32_t flags,
                     uint32_t depth, const char *name, int32_t *r2, uint32_t *r6,
                     int32_t *r7)
{
    int old = -1;
    for (int i = 0; i < 6; i++)
        if (old_formats[i][0] == ncolour &&
            (old_formats[i][1] ? (flags & MF_FULLPALETTE) != 0
                               : (flags & MF_FULLPALETTE) == 0) &&
            old_formats[i][2] == depth)
            old = i;
    uint32_t words = old >= 0 ? 6 : 8;
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t entrysize = 4 * words + ((nlen + 1 + 3) & ~3u);
    if (index <= 0) {
        if (at) {
            if (room < entrysize)
                return 0;
            ros_st32(at, entrysize);
            ros_st32(at + 4, old >= 0 ? 1u : 3u);
            ros_st32(at + 8, width);
            ros_st32(at + 12, height);
            if (old >= 0) {
                ros_st32(at + 16, depth);
                ros_st32(at + 20, rate);
            } else {
                ros_st32(at + 16, ncolour);
                ros_st32(at + 20, flags);
                ros_st32(at + 24, depth);
                ros_st32(at + 28, rate);
            }
            for (uint32_t i = 0; i < ((nlen + 1 + 3) & ~3u); i++)
                ros_st8(at + 4 * words + i, i < nlen ? (uint8_t)name[i] : 0);
            *r6 = at + entrysize;
        }
        *r7 -= (int32_t)entrysize;
    }
    (*r2)--;
    return 1;
}

static void enumerate_modes(struct ros_cpu *s)
{
    const struct ros_display *d = drmvideo_display();
    if (!d || d->nmodes == 0) {
        s->r[1] = SERVICE_ENUMERATESCREENMODES;   /* nobody claims it, so all are left */
        return;
    }
    uint32_t formats[8][3];
    uint32_t nformats = 6;
    memcpy(formats, old_formats, sizeof old_formats);
    uint32_t r[4] = { 0 };
    if (vdu_graphicsv(GV_PIXEL_FORMATS, r)) {
        for (uint32_t i = 0; i < r[1] && i < 8; i++) {
            formats[i][0] = ros_ld32(r[0] + 12 * i);
            formats[i][1] = ros_ld32(r[0] + 12 * i + 4);
            formats[i][2] = ros_ld32(r[0] + 12 * i + 8);
            nformats = i + 1;
        }
    }
    int32_t r2 = (int32_t)s->r[2];
    uint32_t r6 = s->r[6];
    int32_t r7 = (int32_t)s->r[7];
    char name[24];
    for (uint32_t m = 0; m < d->nmodes; m++)
        for (uint32_t f = 0; f < nformats; f++) {
            snprintf(name, sizeof name, "%u x %u", d->modes[m].width, d->modes[m].height);
            if (!put_entry(r6, r7 > 0 ? (uint32_t)r7 : 0, r2, d->modes[m].width,
                           d->modes[m].height, d->modes[m].rate, formats[f][0],
                           formats[f][1], formats[f][2], name, &r2, &r6, &r7)) {
                s->r[1] = 0;              /* the block ran short, so it is claimed */
                s->r[2] = (uint32_t)r2;
                s->r[6] = r6;
                s->r[7] = (uint32_t)r7;
                return;
            }
        }
    s->r[1] = SERVICE_ENUMERATESCREENMODES;
    s->r[2] = (uint32_t)r2;
    s->r[6] = r6;
    s->r[7] = (uint32_t)r7;
}

/* ---- mode strings (OS_ScreenMode 13 and 14) ---------------------------------
 *
 * ScreenMode_ModeStringToSpecifier and ScreenMode_ModeSpecifierToString
 * follow the kernel's Kernel/s/vdu/vduswis. They use the same tokens in
 * the same order, such as "X1024 Y768 C16M F60". The tokens are X, Y, a
 * colour (C2 C4 C16 C64 C256 C4K C32K C64K C16M) or grey count (G2 G4 G16
 * G256 G16M), a layout (LTRGB LABGR LARGB), eigen factors (EX EY), the
 * frame rate (F), and for teletext a T before the colour, with TX TY the
 * text size. A mode number gives that number's selector. Unknown or
 * repeated tokens, and a string without X, Y and a colour, give "Bad
 * parameters". */

/* pixelformat_list: NColour, the flags of its own, log2bpp, and the token */
static const uint32_t pf_list[][3] = {
    { 1, 0, 0 }, { 3, 0, 1 }, { 15, 0, 2 }, { 63, 0, 3 }, { 255, MF_FULLPALETTE, 3 },
    { 4095, 0, 4 }, { 0xFFFF, 0, 4 }, { 0xFFFF, MF_FULLPALETTE, 4 }, { 0xFFFFFFFFu, 0, 5 },
    { 1, MF_GREYSCALE, 0 }, { 3, MF_GREYSCALE, 1 }, { 15, MF_GREYSCALE, 2 },
    { 255, MF_FULLPALETTE | MF_GREYSCALE, 3 }, { 0xFFFFFF, 0, 6 },
};
static const char *const pf_token[] = {
    "C2", "C4", "C16", "C64", "C256", "C4K", "C32K", "C64K", "C16M",
    "G2", "G4", "G16", "G256", "G16M",
};

/* Reads a base-10 number at *pp, or returns -1. The limit max tops the
 * range, as OS_ReadUnsigned was asked to. It is 0-255 for a mode number,
 * 0-3 for an eigen factor, and plain for the rest. */
static int32_t read_number(const char **pp, uint32_t max)
{
    const char *p = *pp;
    if (!isdigit((unsigned char)*p))
        return -1;
    uint32_t n = 0;
    while (isdigit((unsigned char)*p)) {
        if ((uint32_t)(*p - '0') > max || n > (max - (uint32_t)(*p - '0')) / 10)
            return -1;
        n = n * 10 + (uint32_t)(*p++ - '0');
    }
    *pp = p;
    return (int32_t)n;
}

enum {                          /* the kernel's strtospec_got_* flags */
    GOT_XRES = 1, GOT_YRES = 2, GOT_COLOURS = 4, IS_TTX = 8, GOT_LAYOUT = 16,
    GOT_TTX_RCOL = 32, GOT_TTX_BROW = 64,
};

/* Sets a variable pair in the selector being built. It sets the one that
 * is there, or adds it if there is room (the kernel's strtospec_setvar). */
static os_error *set_var(uint32_t *vars, uint32_t *nvars, uint32_t index, uint32_t value,
                         int32_t *room)
{
    for (uint32_t i = 0; i < *nvars; i++)
        if (vars[2 * i] == index) {
            vars[2 * i + 1] = value;
            return NULL;
        }
    if (*room < 8)
        return ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow");
    vars[2 * *nvars] = index;
    vars[2 * *nvars + 1] = value;
    (*nvars)++;
    *room -= 8;
    return NULL;
}

/* Returns a colour or grey token's number and letter, as the kernel
 * resolves it (strtospec_get_colours, strtospec_get_greys), or -1. */
static int parse_colour(const char **pp, int grey)
{
    int32_t n = read_number(pp, 0xFFFFFF);
    if (n < 0)
        return -1;
    int c = toupper((unsigned char)**pp);
    int kt = c == 'K' || c == 'T', m = c == 'M';
    int match = -1;
    if (!grey) {
        if (n == 2 && !kt && !m) match = 0;
        else if (n == 4 && !kt && !m) match = 1;
        else if (n == 4 && kt) match = 5;
        else if (n == 16 && !m && !kt) match = 2;
        else if (n == 16 && m) match = 8;
        else if (n == 64 && !kt && !m) match = 3;
        else if (n == 64 && kt) match = 7;
        else if (n == 256 && !kt && !m) match = 4;
        else if (n == 32 && kt) match = 6;
    } else {
        if (n == 2 && !kt && !m) match = 9;
        else if (n == 4 && !kt && !m) match = 10;
        else if (n == 16 && !m && !kt) match = 11;
        else if (n == 256 && !kt && !m) match = 12;
        else if (n == 16 && m) match = 13;
    }
    if (match >= 0 && (kt || m))
        (*pp)++;
    return match;
}

static void string_to_specifier(struct ros_cpu *s)
{
    const char *p = ros_ptr(s->r[1]);
    uint32_t out = s->r[2];
    int32_t room = (int32_t)s->r[3] - 24;         /* the header, and the -1 */
    uint32_t vars[16], nvars = 0;
    uint32_t got = 0;
    int32_t xres = -1, yres = -1, depth = -1, rate = -1;
    os_error *e = NULL;
    if (room < 0)                                 /* this is checked before any write, as the kernel does */
        e = ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow");
    else
        ros_st32(out, 1);                         /* ModeSelectorFlags_ValidFormat */
    while (!e && (*p == ' ' || *p == ','))
        p++;
    if (!e && isdigit((unsigned char)*p)) {
        /* A mode number gives the selector that its own variables make. */
        int32_t n = read_number(&p, 255);
        uint32_t mv[MV_COUNT];
        if (n < 0 || vdu_mode_vars((uint32_t)n, mv))
            e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
        else {
            xres = (int32_t)mv[MV_XWIND] + 1;
            yres = (int32_t)mv[MV_YWIND] + 1;
            depth = (int32_t)mv[MV_LOG2BPP];
            rate = -1;
            got |= GOT_XRES | GOT_YRES | GOT_COLOURS;   /* a number says it all */
            const uint32_t four[4][2] = {
                /* Only the pixel format's flags are kept, as the kernel
                 * keeps them (FullPalette/64k, Greyscale/Chroma, the data
                 * format). Mode 7's &A7 becomes &80, and mode 14's &4
                 * becomes 0 (#73). */
                { MV_NCOLOUR, mv[MV_NCOLOUR] }, { MV_FLAGS, mv[MV_FLAGS] & (MF_FULLPALETTE | MF_GREYSCALE | MF_FORMAT) },
                { MV_XEIG, mv[MV_XEIG] }, { MV_YEIG, mv[MV_YEIG] },
            };
            for (int i = 0; !e && i < 4; i++)
                e = set_var(vars, &nvars, four[i][0], four[i][1], &room);
        }
    }
    while (!e && *p >= ' ') {
        if (*p == ' ' || *p == ',') {
            p++;
            continue;
        }
        char c = (char)toupper((unsigned char)*p);
        p++;
        int32_t n;
        switch (c) {
        case 'E':
            if (toupper((unsigned char)*p) == 'X' || toupper((unsigned char)*p) == 'Y') {
                uint32_t var = toupper((unsigned char)p[0]) == 'X' ? MV_XEIG : MV_YEIG;
                p++;
                if ((n = read_number(&p, 3)) < 0)
                    e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                else
                    e = set_var(vars, &nvars, var, (uint32_t)n, &room);
            } else {
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
            }
            break;
        case 'X':
            if ((got & GOT_XRES) || (n = read_number(&p, 16383)) < 0)
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
            else
                xres = n, got |= GOT_XRES;
            break;
        case 'Y':
            if ((got & GOT_YRES) || (n = read_number(&p, 16383)) < 0)
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
            else
                yres = n, got |= GOT_YRES;
            break;
        case 'F':
            if ((n = read_number(&p, 16383)) < 0)
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
            else
                rate = n;
            break;
        case 'T':
            if (toupper((unsigned char)*p) == 'X' || toupper((unsigned char)*p) == 'Y') {
                uint32_t isx = toupper((unsigned char)*p) == 'X';
                p++;
                uint32_t bit = isx ? GOT_TTX_RCOL : GOT_TTX_BROW;
                if ((got & bit) || (n = read_number(&p, 16383)) < 0)
                    e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                else {
                    got |= bit;
                    e = set_var(vars, &nvars, isx ? MV_SCRRCOL : MV_SCRBROW, (uint32_t)n,
                                &room);
                }
            } else {
                /* Teletext with its colour in one token, such as "T16". */
                got |= IS_TTX;
                int match = parse_colour(&p, 0);
                if (match < 0 || (got & GOT_COLOURS))
                    e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                else {
                    got |= GOT_COLOURS;
                    depth = (int32_t)pf_list[match][2];
                    e = set_var(vars, &nvars, MV_NCOLOUR, pf_list[match][0], &room);
                    if (!e)
                        e = set_var(vars, &nvars, MV_FLAGS,
                                    pf_list[match][1] | MF_TELETEXT | MF_GAP, &room);
                }
            }
            break;
        case 'C':
        case 'G': {
            if (got & GOT_COLOURS) {
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                break;
            }
            int match = parse_colour(&p, c == 'G');
            if (match < 0) {
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                break;
            }
            got |= GOT_COLOURS;
            depth = (int32_t)pf_list[match][2];
            e = set_var(vars, &nvars, MV_NCOLOUR, pf_list[match][0], &room);
            if (!e) {
                uint32_t flags = pf_list[match][1];
                if (got & IS_TTX)
                    flags |= MF_TELETEXT | MF_GAP;
                e = set_var(vars, &nvars, MV_FLAGS, flags, &room);
            }
            break;
        }
        case 'L': {
            if (got & GOT_LAYOUT) {
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                break;
            }
            uint32_t sub = 0;
            int a = toupper((unsigned char)*p++);
            if (a != 'A' && a != 'T') {
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                break;
            }
            if (a == 'A')
                sub |= MF_ALPHA;
            int r = toupper((unsigned char)*p++);
            if (r != 'R' && r != 'B') {
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                break;
            }
            if (r == 'R')
                sub |= MF_RGB;
            if (toupper((unsigned char)*p++) != 'G' ||
                toupper((unsigned char)*p++) != (r == 'R' ? 'B' : 'R')) {
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                break;
            }
            got |= GOT_LAYOUT;
            for (uint32_t i = 0; !e && i < nvars; i++)
                if (vars[2 * i] == MV_FLAGS) {
                    uint32_t v = vars[2 * i + 1];
                    if (v & (3u << 12))        /* not the RGB family */
                        e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
                    else
                        vars[2 * i + 1] = (v & ~MF_FORMAT) | sub;
                }
            if (!e && !(got & GOT_COLOURS))
                e = set_var(vars, &nvars, MV_FLAGS, sub, &room);
            break;
        }
        default:
            e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
            break;
        }
    }
    if (!e && (xres < 0 || yres < 0 || depth < 0 || !(got & GOT_COLOURS)))
        e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
    if (!e && (got & (GOT_TTX_RCOL | GOT_TTX_BROW)) && !(got & IS_TTX))
        e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
    if (!e) {
        ros_st32(out + 4, (uint32_t)xres);
        ros_st32(out + 8, (uint32_t)yres);
        ros_st32(out + 12, (uint32_t)depth);
        ros_st32(out + 16, (uint32_t)rate);
        uint32_t at = out + 20;
        for (uint32_t i = 0; i < nvars; i++, at += 8) {
            ros_st32(at, vars[2 * i]);
            ros_st32(at + 4, vars[2 * i + 1]);
        }
        ros_st32(at, 0xFFFFFFFFu);
        s->r[2] = out;
        return;
    }
    ros_swi_fail(s, e);
}

static void specifier_to_string(struct ros_cpu *s)
{
    uint32_t sel = s->r[1], buf = s->r[2];
    int32_t size = (int32_t)s->r[3];
    os_error *e = NULL;
    if (sel < 256 || (ros_ld32(sel) & 0xFF) != 1) {
        e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");   /* no mode numbers */
    } else {
        uint32_t xres = ros_ld32(sel + 4), yres = ros_ld32(sel + 8);
        uint32_t depth = ros_ld32(sel + 12);
        uint32_t ncolour = depth < 5 ? (1u << (1u << depth)) - 1 : 0xFFFFFFFFu;
        uint32_t modeflags = depth == 3 ? MF_FULLPALETTE : 0;
        int32_t xeig = -1, yeig = -1, rate = (int32_t)ros_ld32(sel + 16);
        uint32_t tx = 0, ty = 0;
        for (uint32_t at = sel + 20; ros_ld32(at) != 0xFFFFFFFFu; at += 8) {
            switch (ros_ld32(at)) {
            case MV_NCOLOUR: ncolour = ros_ld32(at + 4); break;
            case MV_FLAGS:   modeflags = ros_ld32(at + 4); break;
            case MV_XEIG:    xeig = (int32_t)ros_ld32(at + 4); break;
            case MV_YEIG:    yeig = (int32_t)ros_ld32(at + 4); break;
            case MV_SCRRCOL: tx = ros_ld32(at + 4) + 1; break;
            case MV_SCRBROW: ty = ros_ld32(at + 4) + 1; break;
            default: break;
            }
        }
        if (modeflags & (3u << 12))               /* not the RGB family */
            e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
        else {
            int match = -1;
            for (unsigned i = 0; i < sizeof pf_list / sizeof pf_list[0]; i++)
                if (pf_list[i][0] == ncolour && pf_list[i][2] == depth &&
                    pf_list[i][1] == (modeflags & (MF_FULLPALETTE | MF_GREYSCALE)))
                    match = (int)i;
            if (match < 0)
                e = ros_error(ERR_BAD_PARAMETERS, "Bad parameters");
            else {
                /* The items are X, Y, the colour (T in teletext's place),
                 * the layout, the eigen factors, the rate and teletext's
                 * text size. There is a space between each, and the
                 * terminator counts, exactly as the kernel writes them. */
                uint32_t n = 0;
                char item[32];
#define PUT(...)                                                       \
                do {                                                    \
                    int len = snprintf(item, sizeof item, __VA_ARGS__);  \
                    if (n + (uint32_t)len + 1 <= (uint32_t)size)         \
                        memcpy(ros_ptr(buf + n), item, (size_t)len + 1); \
                    n += (uint32_t)len;                                  \
                } while (0)
                PUT("X%u", xres);
                PUT(" Y%u", yres);
                char colour[8];
                snprintf(colour, sizeof colour, "%s", pf_token[match]);
                if (modeflags & MF_TELETEXT)
                    colour[0] = 'T';
                PUT(" %s", colour);
                if (modeflags & MF_FORMAT & ~(MF_FULLPALETTE | MF_GREYSCALE))
                    PUT(" %s%s", modeflags & MF_ALPHA ? "LA" : "LT",
                        modeflags & MF_RGB ? "RGB" : "BGR");
                if (xeig >= 0)
                    PUT(" EX%u", (unsigned)xeig);
                if (yeig >= 0)
                    PUT(" EY%u", (unsigned)yeig);
                if (rate >= 0)
                    PUT(" F%u", (unsigned)rate);
                if (modeflags & MF_TELETEXT)
                    PUT(" TX%u TY%u", tx, ty);
#undef PUT
                if (n < (uint32_t)size)
                    ros_st8(buf + n, 0);
                n++;                               /* the terminator */
                s->r[3] = n <= (uint32_t)size ? 0 : -(int32_t)n;
                return;
            }
        }
    }
    ros_swi_fail(s, e);
}

/* ---- the SWIs --------------------------------------------------------------- */


/* A VDU variable (OS_ReadVduVariables), 0 if unknown */
static uint32_t vdu_variable(uint32_t n)
{
    if (n < MV_COUNT)
        return vdu.mv[n];
    int32_t w;
    switch (n) {
    case 128: return (uint32_t)vdu.gwl;
    case 129: return (uint32_t)vdu.gwb;
    case 130: return (uint32_t)vdu.gwr;
    case 131: return (uint32_t)vdu.gwt;
    case 132: return (uint32_t)vdu.twl;
    case 133: return (uint32_t)vdu.twb;
    case 134: return (uint32_t)vdu.twr;
    case 135: return (uint32_t)vdu.twt;
    case 136: return (uint32_t)vdu.orgx;
    case 137: return (uint32_t)vdu.orgy;
    case 138: return (uint32_t)vdu.gcsx;
    case 139: return (uint32_t)vdu.gcsy;
    case 140: return (uint32_t)vdu.olderx;
    case 141: return (uint32_t)vdu.oldery;
    case 142: return (uint32_t)vdu.oldx;
    case 143: return (uint32_t)vdu.oldy;
    case 144: return (uint32_t)vdu.gcsix;
    case 145: return (uint32_t)vdu.gcsiy;
    case 146: return (uint32_t)vdu.newptx;
    case 147: return (uint32_t)vdu.newpty;
    case 148: return vdu.screen_start;          /* ScreenStart */
    case 149:                                   /* DisplayStart: the bank shown */
        return vdu.vd ? vdisplay_display_start() : vdu.display_start;
    case 150: return vdu.total_size;            /* TotalScreenSize */
    case 151: return vdu.gplfmd;
    case 152: return vdu.gplbmd;
    case 153: return vdu.gfcol;
    case 154: return vdu.gbcol;
    case 155: return vdu.tfore;
    case 156: return vdu.tback;
    case 157: return vdu.gftint;
    case 158: return vdu.gbtint;
    case 159: return vdu.tftint;
    case 160: return vdu.tbtint;
    case 161: return MAXMODE;
    case 162: return vdu.gchar_sx;
    case 163: return vdu.gchar_sy;
    case 164: return vdu.gchar_spx;
    case 165: return vdu.gchar_spy;
    case 167: return vdu.tchar_x;               /* TCharSizeX: 16 in teletext */
    case 168: return vdu.tchar_y;
    case 169: return vdu.tchar_x;               /* TCharSpaceX */
    case 170: return vdu.row_mult;
    case 173: return pixel_rate;
    case 171: return vdu.oe_tables;             /* GcolOraEorAddr */
    case 256:                                   /* WindowWidth */
    case 257: {                                 /* WindowHeight */
        uint32_t f = vdu.cursor_flags, uw, uh;
        if (f & CF_VDU5) {                      /* characters in the graphics window */
            uint32_t rx = f & 2 ? vdu.gchar_sx - 1 : 0, ry = f & 4 ? vdu.gchar_sy - 1 : 0;
            uw = (uint32_t)(vdu.gwr - vdu.gwl) - rx;
            uh = (uint32_t)(vdu.gwt - vdu.gwb) - ry;
            uw = vdu.gchar_spx ? uw / vdu.gchar_spx : 0;
            uh = vdu.gchar_spy ? uh / vdu.gchar_spy : 0;
        } else {
            uw = (uint32_t)(vdu.twr - vdu.twl), uh = (uint32_t)(vdu.twb - vdu.twt);
        }
        if (f & (1u << 3))
            w = (int32_t)uw, uw = uh, uh = (uint32_t)w;
        if (!(f & CF_VDU5) && (f & CF_81COLUMN))
            uw++;
        return n == 256 ? uw : uh;
    }
    default:
        return 0;           /* 166 (a kernel routine), 172, 174-177, 192 */
    }
}

void ros_thunk_OS_ReadVduVariables(struct ros_cpu *s)
{
    uint32_t in = s->r[0], out = s->r[1];
    for (;; in += 4, out += 4) {
        uint32_t n = ros_ld32(in);
        if (n == 0xFFFFFFFFu)
            break;
        ros_st32(out, vdu_variable(n));
    }
    s->v = 0;
}

/* OS_ReadModeVariable on a sprite mode word, one variable at a time as the
 * kernel's NewSpriteModeWord answers it (Kernel/s/vdu/vduswis). It
 * returns 1 and the value, or 0 (C set). A sprite has no screen, so the
 * size variables are never answered. The eigen factors fail only for a dpi
 * other than 180, 90, 45 or 22/23. ColourTrans asks for (type<<27)+1,
 * which has no dpi. The types past 6 are the kernel's table's
 * (NColourTable, NSM_bpptable). */
static int sprite_word_var(uint32_t m, uint32_t var, uint32_t *v)
{
    enum { NEW64K = 10, RISCOS5 = 15, RO5MAX = 19, SUBSTITUTE = 6 };
    static const uint8_t bpp[RO5MAX] = { 0, 0, 1, 2, 3, 4, 5, 5, 5, 5, 4, 5, 5, 5, 5, 5, 4, 5, 5 };
    static const uint32_t ncolour[6] = { 1, 3, 15, 255, 0xFFFF, 0xFFFFFFFFu };
    uint32_t type = (m >> 27) & 15, flags = 0;
    int ro5 = 0;
    if (type == 0)
        return 0;
    if (type == RISCOS5) {
        if ((m & 0xF0000) || (m & 0xE)) {
            type = SUBSTITUTE;
        } else {
            type = (m >> 20) & 127;
            if (type == 0 || type >= RO5MAX)
                return 0;
            ro5 = 1;
            flags = m & 0xFF00;
        }
    }
    switch (var) {
    case MV_FLAGS:
        if (ro5 && (flags & ~(MF_RGB | MF_ALPHA)))
            return 0;
        *v = flags | (type == NEW64K ? MF_FULLPALETTE : 0);
        return 1;
    case MV_NCOLOUR:
        *v = type == 16 ? 4095 : ncolour[bpp[type]];
        return 1;
    case MV_XEIG:
    case MV_YEIG: {
        if (ro5) {
            *v = (m >> (var == MV_XEIG ? 4 : 6)) & 3;
            return 1;
        }
        uint32_t dpi = (m >> (var == MV_XEIG ? 1 : 14)) & 0x1FFF;
        int e = dpi_eig(dpi);
        if (e < 0)
            return 0;
        *v = (uint32_t)e;
        return 1;
    }
    case MV_YSHFT:
        *v = 0;
        return 1;
    case MV_LOG2BPP:
    case MV_LOG2BPC:
        *v = bpp[type];
        return 1;
    default:                            /* the screen's size: no sprite's */
        return 0;
    }
}

void ros_thunk_OS_ReadModeVariable(struct ros_cpu *s)
{
    uint32_t mode = s->r[0], var = s->r[1];
    s->v = 0;
    s->c = 1;
    if (var >= MV_COUNT)
        return;
    if (mode == 0xFFFFFFFFu) {
        s->r[2] = vdu.mv[var];
        s->c = 0;
        return;
    }
    if (mode >= 256 && (mode & 1)) {
        uint32_t v;
        if (sprite_word_var(mode, var, &v))
            s->r[2] = v, s->c = 0;
        return;
    }
    uint32_t mv[MV_COUNT];
    if (vdu_mode_vars(mode, mv))
        return;
    s->r[2] = mv[var];
    s->c = 0;
}

/* OS_CheckModeValid. C is clear if the mode can be had. Otherwise R0 is -1
 * and R1 is a substitute, or -2 for none (the kernel gives -2 for any
 * error on the way). */
void ros_thunk_OS_CheckModeValid(struct ros_cpu *s)
{
    uint32_t mode = s->r[0], mv[MV_COUNT], w, h;
    s->v = 0;
    s->c = 0;
    int virt = vdu.vd != NULL;          /* a virtual display takes any mode the VDU draws */
    if ((!virt && is_selector(mode) && !scrmodes_takes(mode)) || vdu_mode_vars(mode, mv)) {
        s->r[0] = 0xFFFFFFFFu;
        s->r[1] = 0xFFFFFFFEu;
        s->c = 1;
        return;
    }
    if (!is_selector(mode) && (mv[MV_FLAGS] & MF_TELETEXT)) {
        s->r[0] = 0xFFFFFFFFu;          /* not as it is, but this substitute */
        s->r[1] = translate();
        s->c = 1;
        return;
    }
    display_size(mode, mv, &w, &h);
    uint32_t bytes = ((w << mv[MV_LOG2BPP]) + 7) / 8;
    if (ttx_fixup(mode, mv) || !geometry_ok(mv, w, h) ||
        (!virt && !vet(w, h, mv, (4 - bytes % 4) % 4))) {
        s->r[0] = 0xFFFFFFFFu;
        s->r[1] = 0xFFFFFFFEu;
        s->c = 1;
    }
}

/* OS_ScreenMode, as ScreenModeSub (Kernel/s/vdu/vduswis) dispatches it.
 * Reasons 0-15 go through one table and 64-68 through another. Anything
 * else gives the kernel's "Unknown OS_ScreenMode reason code" (&1F2, as the
 * farm gives it), with the registers kept. That includes the table's 7-10
 * and 12 (screen banks, ROL's device details). Reason 4
 * (ConfigureAcceleration) answers R1 1 and R2 1 whatever it is asked.
 * Reasons 5 and 6 (clean the screen cache) do nothing. RISC OS 5.30 has no
 * cached screen ("Screen caching isn't supported yet"), and the Wimp calls
 * reason 5 after each rectangle it redraws. */
#define ERR_SCREENMODE_BAD_REASON 0x1F2u

void ros_thunk_OS_ScreenMode(struct ros_cpu *s)
{
    os_error *e;
    uint32_t reason = s->r[0];
    s->v = 0;
    switch (reason) {
    case 0:                             /* select a mode */
        vdu_pre_wrch();
        e = vdu_set_mode(s->r[1]);
        vdu_post_wrch();
        if (e)
            ros_swi_fail(s, e);
        return;
    case 1:                             /* the current mode */
        s->r[1] = vdu.mode_no;
        return;
    case 4:                             /* ConfigureAcceleration: nothing to configure */
        s->r[1] = 1;
        s->r[2] = 1;
        return;
    case 5:                             /* CleanCache */
    case 6:                             /* ForceCleanCache */
        return;
    case 2:
        enumerate_modes(s);
        return;
    case 13:
        string_to_specifier(s);
        return;
    case 14:
        specifier_to_string(s);
        return;
    case 3: case 11: case 15:
    case 64: case 65: case 66: case 67: case 68:
        ros_swi_fail(s, ros_error(ROS_ERR_UNIMPLEMENTED,
                                  "OS_ScreenMode %u is not implemented", reason));
        return;
    default:
        ros_swi_fail(s, ros_error(ERR_SCREENMODE_BAD_REASON, "Unknown OS_ScreenMode reason code"));
        return;
    }
}

/* Gives the display's pixel shape, for a host that shows the screen (BBC
 * BASIC V for Mac's window). The shape is the display's XEigFactor and
 * YEigFactor. It is the display's mode, not that of a sprite that output
 * is redirected to. The two words are read without the lock. A mode change
 * under the reader shows at worst one frame's old shape. */
void ros_vdu_display_eig(uint32_t *xeig, uint32_t *yeig)
{
    *xeig = vdu.dmv[MV_XEIG];
    *yeig = vdu.dmv[MV_YEIG];
}
