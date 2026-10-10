/* font.c: the desktop font.  This finds it, works out which symbols it
 * lacks, and puts back the caller's font.
 *
 * The font is chosen by the CMOS DesktopFeatures value n:
 * - 0 reads the font from Wimp$Font.
 * - 1 is the system font.
 * - 2 or more is the (n-1)th font found by walking Resources:$.Fonts.
 * Each find broadcasts Message_FontChanged, even when nothing was found. */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

#define FONTS_DIR "Resources:$.Fonts"

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

/* Lose the desktop font and the symbol font. */
static void lose_font(void)
{
    struct wimp_draw *d = &wimp_ws()->dr;
    struct ros_cpu c;
    if (d->systemfont && d->systemfont != 0x80000000u) {
        ros_cpu_enter(&c);
        c.r[0] = d->systemfont;
        ros_swi(&c, XFont_LoseFont);
    }
    d->systemfont = 0;
    if (d->symbolfont) {
        ros_cpu_enter(&c);
        c.r[0] = d->symbolfont;
        ros_swi(&c, XFont_LoseFont);
    }
    d->symbolfont = 0;
}

/* The depth-first walk of the fonts directory.  It returns 1 when the
 * count reaches 0, with the font's name in d->fontname.  The name is
 * without the leading "Resources:$.Fonts.". */
static int walk(const char *dir, int *count)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = &w->dr;
    char *path = (char *)d->fontstr, *pattern = (char *)d->fontstr + 256;
    uint8_t *buf = d->pixtable;
    struct ros_cpu c;
    /* Does this directory hold an IntMetric* file? */
    uint32_t offset = 0;
    int found = 0;
    do {
        snprintf(path, 256, "%s", dir);
        strcpy(pattern, "IntMetric*");
        ros_cpu_enter(&c);
        c.r[0] = 10, c.r[1] = ros_addr(path), c.r[2] = ros_addr(buf), c.r[3] = 1;
        c.r[4] = offset, c.r[5] = 256, c.r[6] = ros_addr(pattern);
        if (!call(XOS_GBPB, &c))
            return 0;
        found = c.r[3] > 0;
        offset = c.r[4];
    } while (!found && offset != 0xFFFFFFFFu);
    if (found && --*count == 0) {
        size_t skip = strlen(FONTS_DIR) + 1;
        snprintf((char *)d->fontname, sizeof d->fontname, "%s", strlen(dir) > skip ? dir + skip : "");
        return 1;
    }
    /* Then walk each directory in it, in order. */
    offset = 0;
    while (offset != 0xFFFFFFFFu) {
        snprintf(path, 256, "%s", dir);
        strcpy(pattern, "*");
        ros_cpu_enter(&c);
        c.r[0] = 12, c.r[1] = ros_addr(path), c.r[2] = ros_addr(buf), c.r[3] = 1;
        c.r[4] = offset, c.r[5] = 256, c.r[6] = ros_addr(pattern);
        if (!call(XOS_GBPB, &c))
            return 0;
        offset = c.r[4];
        if (c.r[3] == 0)
            continue;
        uint32_t type;
        memcpy(&type, buf + 20, 4);
        if (type != 0x1000u)
            continue;
        char sub[256];
        snprintf(sub, sizeof sub, "%s.%s", dir, (const char *)buf + 24);
        if (walk(sub, count))
            return 1;
    }
    return 0;
}

static uint32_t read_number(const char *var, uint32_t dflt)
{
    struct wimp_draw *d = &wimp_ws()->dr;
    char *name = (char *)d->fontstr, *val = (char *)d->fontstr + 64;
    snprintf(name, 64, "%s", var);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(name), c.r[1] = ros_addr(val), c.r[2] = 4, c.r[3] = 0, c.r[4] = 1;
    if (!call(XOS_ReadVarVal, &c) || c.r[4] != 0)
        return dflt;
    val[c.r[2] < 4 ? c.r[2] : 3] = 0;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = ros_addr(val);
    return call(XOS_ReadUnsigned, &c) ? c.r[2] : dflt;
}

/* Bit k of the result is set when the font has symbol k.  It is clear
 * when the Wimp's symbol font must stand in for it. */
uint32_t wimp_measure_symbols(uint32_t h)
{
    static const uint8_t latin[6] = { 0x80, 0x84, 0x88, 0x89, 0x8A, 0x8B };
    static const uint8_t utf8[6][3] = { { 0xE2, 0x9C, 0x94 }, { 0xE2, 0x9C, 0x98 },
                                        { 0xE2, 0x87, 0x90 }, { 0xE2, 0x87, 0x92 },
                                        { 0xE2, 0x87, 0x93 }, { 0xE2, 0x87, 0x91 } };
    struct wimp_draw *d = &wimp_ws()->dr;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 71, c.r[1] = 127;
    int is_utf8 = call(XOS_Byte, &c) && c.r[1] == 111;
    uint32_t map = 0;
    uint8_t *s = d->fontstr + 400;
    for (int k = 0; k < 6; k++) {
        if (is_utf8) {
            memcpy(s, utf8[k], 3);
            s[3] = 0;
        } else {
            s[0] = latin[k];
            s[1] = 0;
        }
        uint32_t blk[9] = { 0, 0, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0 };
        memcpy(d->bbox, blk, sizeof blk);
        ros_cpu_enter(&c);
        c.r[0] = h, c.r[1] = ros_addr(s), c.r[2] = 0x40120u;
        c.r[3] = c.r[4] = 0x0FFFFFFFu, c.r[5] = ros_addr(d->bbox);
        if (!call(XFont_ScanString, &c))
            continue;
        int32_t bw = (int32_t)(d->bbox[7] - d->bbox[5]), bh = (int32_t)(d->bbox[8] - d->bbox[6]);
        if (!(bw <= 0 && bh <= 0 && c.r[3] == 0 && c.r[4] == 0))
            map |= 1u << k;
    }
    return map;
}

/* Find the desktop font and the symbol font. */
void wimp_find_font(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_draw *d = &w->dr;
    lose_font();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 161, c.r[1] = 0x8C;
    uint32_t n = call(XOS_Byte, &c) ? (c.r[2] & 0x1Eu) >> 1 : 1;
    uint32_t size = 192, width = 192, handle = 0;
    int have = 0;
    if (n == 0) {
        size = read_number("Wimp$FontSize", 192);
        width = read_number("Wimp$FontWidth", size);
        char *name = (char *)d->fontstr, *val = (char *)d->fontname;
        strcpy(name, "Wimp$Font");
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(name), c.r[1] = ros_addr(val), c.r[2] = 40, c.r[3] = 0, c.r[4] = 3;
        if (call(XOS_ReadVarVal, &c) && c.r[2] > 0) {
            val[c.r[2]] = 0;
            have = 1;
        }
    } else if (n >= 2) {
        if (d->fontnumber == (int)n && d->fontname[0])
            have = 1;
        else {
            int count = (int)n - 1;
            d->fontname[0] = 0;
            have = walk(FONTS_DIR, &count);
            d->fontnumber = have ? (int)n : 0;
        }
    }
    if (have) {
        ros_cpu_enter(&c);
        c.r[1] = ros_addr(d->fontname), c.r[2] = width, c.r[3] = size, c.r[4] = c.r[5] = 0;
        if (call(XFont_FindFont, &c))
            handle = c.r[0];
    }
    if (!handle)
        size = width = 192;
    d->systemfont = handle;
    if (handle) {
        ros_cpu_enter(&c);
        c.r[0] = handle;
        if (call(XFont_ReadInfo, &c)) {
            d->systemfonty1 = (int32_t)c.r[4];
            d->systemfonty0 = (int32_t)c.r[2];
            d->systemfontwidth = (int32_t)(c.r[3] - c.r[1]);
        }
    }
    d->symbol_map = wimp_measure_symbols(handle);
    w->ib.needs_rs = 1;                         /* the icon bar refits */
    uint8_t b[24];
    memset(b, 0, sizeof b);
    uint32_t v = 24;
    memcpy(b, &v, 4);
    v = 0x400CFu;
    memcpy(b + 16, &v, 4);
    memcpy(b + 20, &handle, 4);
    wimp_queue_message(17, b, 24, RECV_BROADCAST, 0, 0);
    strcpy((char *)d->fontstr, "WIMPSymbol");
    ros_cpu_enter(&c);
    c.r[1] = ros_addr(d->fontstr), c.r[2] = width, c.r[3] = size, c.r[4] = c.r[5] = 0;
    if (call(XFont_FindFont, &c))
        d->symbolfont = c.r[0];
}

/* ExitWimp: put back the font that the first measurement found. */
void wimp_font_exit(void)
{
    struct wimp_draw *d = &wimp_ws()->dr;
    if (d->currentfont && d->currentfont != d->systemfont) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = d->currentfont, c.r[1] = d->currentbg, c.r[2] = d->currentfg;
        c.r[3] = d->currentoffset;
        ros_swi(&c, XFont_SetFontColours);
    }
    d->currentfont = 0;
}
