/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_smooth.c: Smooth, the desktop's anti-aliasing switch
 * (modules/smooth).
 *
 * The drawing goes into a 32 x 32 sprite, as selftest_gdraw.c draws (one
 * pixel is 2 OS units, and pixel p is p x 512 in path units). With Smooth
 * off, Draw's calls are exact. With it on, Draw_Fill and a thick
 * Draw_Stroke come out as GDraw's with SpecialFX's styles, while a thin
 * stroke, an EOR fill, an excluded task and an 8 bpp sprite stay exact.
 * Font_Paint paints blended. The setting goes to <Choices$Write>.Smooth, a
 * HostFS directory of the test's own, and comes back when the module starts
 * again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"
#include "smooth.h"

#define check ros_check

#define WRCHV 0x03u
#define GDRAW 0x44540u
#define DRAW 0x40700u
#define RED 0x0000FF00u
#define BLUE 0xFF000000u
#define WHITE 0xFFFFFF00u

static char out[1024];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

static uint32_t swi(uint32_t number, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static uint8_t *mem;
static uint32_t base;
enum {
    O_AREA = 0, AREA_SIZE = 8192, O_PATHR = 8192, O_LINE = 8448, O_MATRIX = 8704, O_CAPS = 8768,
    O_TEXT = 8832, MEM_SIZE = 12288,
};
static uint32_t at(uint32_t off) { return base + off; }

/* A command, its output in out: 0 or the error number */
static uint32_t cli(const char *cmd)
{
    strcpy((char *)mem + O_TEXT, cmd);
    outn = 0, out[0] = 0;
    uint32_t r[10] = { at(O_TEXT) };
    return swi(OS_CLI, r);
}

static void setvar(const char *name, const char *value)
{
    char *t = (char *)mem + O_TEXT;
    strcpy(t, name);
    uint32_t r[10] = { at(O_TEXT), 0, value ? 0 : 0xFFFFFFFFu, 0, 0 };
    if (value) {
        strcpy(t + 128, value);
        r[1] = at(O_TEXT + 128), r[2] = (uint32_t)strlen(value);
    }
    swi(OS_SetVarVal, r);
}

/* ---- the sprite ---- */

static uint32_t l2bpp, image, saved[4];

static uint32_t make_sprite(uint32_t l2)
{
    uint32_t a = at(O_AREA), sp = a + 16, words = (32u << l2) / 32, size = 44 + words * 4 * 32;
    memset(mem + O_AREA, 0, AREA_SIZE);
    ros_st32(a, AREA_SIZE), ros_st32(a + 4, 1), ros_st32(a + 8, 16), ros_st32(a + 12, 16 + size);
    ros_st32(sp, size);
    memcpy(ros_ptr(sp + 4), "smtest", 6);
    ros_st32(sp + 16, words - 1), ros_st32(sp + 20, 31), ros_st32(sp + 24, 0), ros_st32(sp + 28, 31);
    ros_st32(sp + 32, 44), ros_st32(sp + 36, 44);
    ros_st32(sp + 40, (l2 + 1) << 27 | 90u << 14 | 90u << 1 | 1);
    l2bpp = l2, image = sp + 44;
    return sp;
}

static uint32_t image_size(void) { return (32u << l2bpp) / 8 * 32; }

/* Every pixel one value: 32 bpp &00BBGGRR, else bytes of &FF */
static void clear(uint32_t pixel)
{
    uint8_t *p = ros_ptr(image);
    if (l2bpp == 5)
        for (int i = 0; i < 32 * 32; i++)
            ros_st32(image + 4 * (uint32_t)i, pixel);
    else
        memset(p, 0xFF, image_size());
}

static void to_sprite(uint32_t sp)
{
    uint32_t r[10] = { 512 + 60, at(O_AREA), sp, 0 };
    swi(OS_SpriteOp, r);
    memcpy(saved, r, sizeof saved);
}

static void to_screen(void)
{
    uint32_t r[10] = { saved[0], saved[1], saved[2], saved[3] };
    swi(OS_SpriteOp, r);
}

static void colour(uint32_t rgb, uint32_t action)
{
    uint32_t r[10] = { rgb, 0, 0, 0, action };
    swi(ColourTrans_SetGCOL, r);
}

static uint32_t pix(int x, int y)
{
    uint32_t row = image + (uint32_t)(31 - y) * ((32u << l2bpp) / 8);
    return l2bpp == 5 ? ros_ld32(row + 4 * (uint32_t)x) : ros_ld8(row + (uint32_t)x);
}

/* A drawing into the cleared sprite, kept: its SWI and style */
static uint32_t draw(uint8_t *keep, uint32_t n, uint32_t style, uint32_t width)
{
    clear(0xFFFFFF);
    uint32_t path = n == DRAW + 4 || n == GDRAW + 4 ? O_LINE : O_PATHR;
    uint32_t r[10] = { at(path), style, at(O_MATRIX), 0, width, at(O_CAPS), 0, 0 };
    uint32_t e = swi(n, r);
    memcpy(keep, ros_ptr(image), image_size());
    return e;
}

static int same(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, image_size()); }

#define P(v) ((int32_t)((v) * 512))

static void paths(void)
{
    const int32_t r[] = { 2, 5171, P(5.5), 8, P(20.75), P(5.5), 8, P(20.75), P(25.25),
                          8, 5171, P(25.25), 5, 0 };
    memcpy(mem + O_PATHR, r, sizeof r);
    const int32_t l[] = { 2, P(4), P(15.5), 8, P(28), P(15.5), 0 };
    memcpy(mem + O_LINE, l, sizeof l);
    const int32_t id[] = { 65536, 0, 0, 65536, 0, 0 };
    memcpy(mem + O_MATRIX, id, sizeof id);
    const int32_t caps[] = { 0, 10 << 16, 0, 0 };
    memcpy(mem + O_CAPS, caps, sizeof caps);
}

/* ---- fonts ---- */

static uint32_t font;

/* "ab" in Trinity.Medium 14pt, black over blue, the font's background
 * white; R2's blend bit as given */
static void paint(uint8_t *keep, uint32_t blend)
{
    clear(0xFF0000);
    uint32_t c[10] = { font, WHITE, 0, 14 };
    swi(ColourTrans_SetFontColours, c);
    strcpy((char *)mem + O_TEXT, "ab");
    uint32_t r[10] = { font, at(O_TEXT), 1u << 8 | 1u << 4 | blend, 4, 16 };
    swi(Font_Paint, r);
    memcpy(keep, ros_ptr(image), image_size());
}

void ros_selftest_smooth(void)
{
    ros_console_printf("Smooth (native): the desktop's anti-aliasing switch\n");
    mem = ros_rma_alloc(MEM_SIZE);
    if (!mem) {
        check(0, "Smooth: RMA for the tests", NULL);
        return;
    }
    base = ros_addr(mem);
    paths();
    ros_vector_claim_native(WRCHV, wrch, 0);

    /* a Choices directory of the test's own */
    char root[512];
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-smooth-XXXXXX", tmp);
    int dir = mkdtemp(root) != NULL;
    if (dir)
        ros_hostfs_mount("SmTest", root);
    setvar("Choices$Write", "HostFS::SmTest.$");
    setvar("Choices$Path", "HostFS::SmTest.$.");

    uint32_t e = cli("Smooth");
    check(!e && strstr(out, "Smoothing is off.") && strstr(out, "No tasks are excluded."),
          "*Smooth: off by default, nothing excluded", "&%X \"%s\"", e, out);
    e = cli("Smooth Sideways");
    check(e == 0xDC, "*Smooth Sideways -- \"Syntax: *Smooth [On|Off]\"", "&%X", e);

    static uint8_t exact_fill[4096], exact_thick[4096], exact_thin[4096], exact_eor[4096];
    static uint8_t g_fill[4096], g_thick[4096], got[4096];
    static uint8_t f_plain[4096], f_blend[4096], exact_neg[4096], exact_ext[4096];
    uint32_t sp = make_sprite(5);
    to_sprite(sp);

    /* off: Draw's, exactly */
    colour(RED, 0);
    uint32_t e1 = draw(exact_fill, DRAW + 2, 0, 0);
    int full = pix(20, 16) == 0xFF && pix(10, 16) == 0xFF && pix(21, 16) == 0xFFFFFF;
    colour(0, 0);
    e1 |= draw(exact_thick, DRAW + 4, 0, 1536);
    e1 |= draw(exact_thin, DRAW + 4, 0, 0);
    colour(RED, 3);
    e1 |= draw(exact_eor, DRAW + 2, 0, 0);
    colour(RED, 0);
    e1 |= draw(exact_neg, DRAW + 2, 0x31, 0);
    e1 |= draw(exact_ext, DRAW + 2, 0x0C, 0);
    draw(g_fill, GDRAW + 2, 0xB0, 0);
    colour(0, 0);
    draw(g_thick, GDRAW + 4, 0x800000B8u, 1536);
    check(!e1 && full && !same(exact_fill, g_fill) && !same(exact_thick, g_thick),
          "Smooth off: Draw_Fill and Draw_Stroke exact (x 20 of the rectangle fully red)", "&%X, x 20 &%X",
          e1, pix(20, 16));
    font = 0;
    strcpy((char *)mem + O_TEXT, "Trinity.Medium");
    uint32_t ff[10] = { 0, at(O_TEXT), 14 * 16, 14 * 16, 90, 90 };
    uint32_t fe = swi(Font_FindFont, ff);
    font = ff[0];
    paint(f_plain, 0);
    paint(f_blend, 1u << 11);
    paint(got, 0);
    check(!fe && !same(f_plain, f_blend) && same(got, f_plain),
          "Smooth off: Font_Paint not blended unless asked", "&%X", fe);

    /* on */
    e = cli("Smooth On");
    colour(RED, 0);
    draw(got, DRAW + 2, 0, 0);
    int fill_on = same(got, g_fill) && pix(20, 16) == 0x4040FF;
    draw(got, DRAW + 2, 0x30, 0);
    fill_on &= same(got, g_fill);
    check(!e && fill_on, "Smooth On: Draw_Fill (style 0, &30) is GDraw_Fill &B0's (x 20 &4040FF)",
          "&%X, x 20 &%X", e, pix(20, 16));
    colour(0, 0);
    draw(got, DRAW + 4, 0, 1536);
    int thick = same(got, g_thick);
    draw(got, DRAW + 4, 0, 0);
    int thin = same(got, exact_thin);
    check(thick && thin, "Smooth On: a thick Draw_Stroke is GDraw_Stroke &800000B8's; a thin one exact",
          "thick %d thin %d", thick, thin);
    colour(RED, 3);
    draw(got, DRAW + 2, 0, 0);
    int eor = same(got, exact_eor);
    colour(RED, 0);
    draw(got, DRAW + 2, 0x31, 0);
    int neg = same(got, exact_neg);
    draw(got, DRAW + 2, 0x0C, 0);
    int ext = same(got, exact_ext);
    check(eor && neg && ext, "Smooth On: an EOR fill, winding rule 1 and style &0C stay Draw's",
          "eor %d neg %d ext %d", eor, neg, ext);
    paint(got, 0);
    check(same(got, f_blend), "Smooth On: Font_Paint without R2 bit 11 paints as blended", NULL);

    /* tasks */
    smooth_test_task("TestTask");
    e = cli("SmoothExclude TestTask");
    colour(RED, 0);
    draw(got, DRAW + 2, 0, 0);
    int excl = same(got, exact_fill);
    paint(got, 0);
    excl &= same(got, f_plain);
    check(!e && excl, "*SmoothExclude TestTask: that task's Draw_Fill and Font_Paint exact", "&%X", e);
    smooth_test_task("Other");
    colour(RED, 0);
    draw(got, DRAW + 2, 0, 0);
    check(same(got, g_fill), "Smooth On: another task's Draw_Fill anti-aliased", NULL);
    smooth_test_task(NULL);
    draw(got, DRAW + 2, 0, 0);
    check(same(got, g_fill), "Smooth On: outside the desktop (no task), the setting applies", NULL);
    smooth_test_task("testtask");
    e = cli("Smooth");
    check(!e && strstr(out, "Smoothing is on.") && strstr(out, "TestTask"),
          "*Smooth: on, and TestTask listed", "\"%s\"", out);
    draw(got, DRAW + 2, 0, 0);
    check(same(got, exact_fill), "Smooth: task names compare without case", NULL);
    to_screen();

    /* 8 bpp stays exact */
    sp = make_sprite(3);
    to_sprite(sp);
    smooth_test_task(NULL);
    colour(RED, 0);
    draw(got, DRAW + 2, 0, 0);
    uint32_t x20 = pix(20, 16), x10 = pix(10, 16);
    paint(got, 0);
    check(x20 == x10 && x10 != 0xFF, "Smooth On, 8 bpp: Draw_Fill exact (x 20 the fill's colour)",
          "&%X &%X", x10, x20);
    to_screen();

    /* the choices file, and the module started again */
    char path[600], text[256] = "";
    snprintf(path, sizeof path, "%s/Smooth", root);     /* (a Text file needs no suffix) */
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
    }
    check(!strcmp(text, "Smooth On\nExclude TestTask\n"),
          "Smooth: the setting saved as <Choices$Write>.Smooth, a Text file", "\"%s\"", text);
    e = cli("RMReInit Smooth");
    uint32_t e2 = cli("Smooth");
    sp = make_sprite(5);
    to_sprite(sp);
    smooth_test_task("Other");
    colour(RED, 0);
    draw(got, DRAW + 2, 0, 0);
    int on = same(got, g_fill);
    smooth_test_task("TestTask");
    draw(got, DRAW + 2, 0, 0);
    int ex = same(got, exact_fill);
    to_screen();
    check(!e && !e2 && strstr(out, "Smoothing is on.") && strstr(out, "TestTask") && on && ex,
          "*RMReInit Smooth: on again, TestTask excluded again, from Choices:Smooth", "&%X &%X \"%s\" %d %d",
          e, e2, out, on, ex);
    e = cli("SmoothInclude TestTask");
    e2 = cli("Smooth Off");
    check(!e && !e2 && (f = fopen(path, "rb")) && fread(text, 1, sizeof text - 1, f) == 11 &&
              !memcmp(text, "Smooth Off\n", 11),
          "*SmoothInclude, *Smooth Off: saved", NULL);
    if (f)
        fclose(f);

    /* as it was: no choices, the module started again, off */
    smooth_test_task_clear();
    if (font) {
        uint32_t l[10] = { font };
        swi(Font_LoseFont, l);
    }
    setvar("Choices$Write", NULL);
    setvar("Choices$Path", NULL);
    cli("RMReInit Smooth");
    ros_vector_release_native(WRCHV, wrch, 0);
    unlink(path);
    if (dir) {
        ros_hostfs_unmount("SmTest");
        rmdir(root);
    }
    ros_rma_free(mem);
}
