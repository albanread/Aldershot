/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* selftest_fontfiles.c -- the Font Manager over font files that lie.
 *
 * A font file comes from a disc or a download, so every length, offset and
 * count in it is untrusted.  Each case below starts from the ROM's
 * Trinity.Medium, changes the fields named, puts the result on a HostFS
 * disc of the test's own, and asks the Font Manager for what reads those
 * fields.  Any error is an acceptable answer.  Reading outside the file, or
 * not returning, is not.  The cases are made to show up under the address
 * sanitizer as well as by a crash.
 */
#include <dirent.h>
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
#include "selftest.h"

#define check ros_check

static uint8_t *mem;
static uint32_t base;
enum { O_TEXT = 0, O_NAME = 512, O_STR = 1024, O_OUT = 2048, O_AREA = 16384, MEM_SIZE = 65536 };
static uint32_t at(uint32_t off) { return base + off; }

static uint32_t swi(uint32_t number, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
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

static uint32_t name(const char *s)
{
    strcpy((char *)mem + O_NAME, s);
    return at(O_NAME);
}

/* ---- files ---- */

static uint8_t metrics[4096], outlines[65536];
static uint32_t metrics_len, outlines_len;

static uint32_t load(const char *n, uint8_t *buf, uint32_t cap)
{
    uint32_t q[10] = { 17, name(n) };
    if (swi(OS_File, q) || q[0] != 1 || q[4] > cap)
        return 0;
    uint32_t len = q[4];
    uint32_t l[10] = { 16, name(n), at(O_AREA), 0 };
    if (swi(OS_File, l))
        return 0;
    memcpy(buf, mem + O_AREA, len);
    return len;
}

static uint32_t save(const char *n, const uint8_t *buf, uint32_t len)
{
    memcpy(mem + O_AREA, buf, len);
    uint32_t r[10] = { 10, name(n), 0xFFFFFF60u, 0, at(O_AREA), at(O_AREA) + len };
    return swi(OS_File, r);
}

static void mkdir_ros(const char *n)
{
    uint32_t r[10] = { 8, name(n), 0, 0, 0 };
    swi(OS_File, r);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- what is asked of a font ---- */

static uint32_t l2bpp = 5, image, saved[4];

static uint32_t make_sprite(void)
{
    uint32_t a = at(O_AREA), sp = a + 16, words = 32, size = 44 + words * 4 * 32;
    memset(mem + O_AREA, 0, 8192);
    ros_st32(a, 8192), ros_st32(a + 4, 1), ros_st32(a + 8, 16), ros_st32(a + 12, 16 + size);
    ros_st32(sp, size);
    memcpy(ros_ptr(sp + 4), "fttest", 6);
    ros_st32(sp + 16, words - 1), ros_st32(sp + 20, 31), ros_st32(sp + 24, 0), ros_st32(sp + 28, 31);
    ros_st32(sp + 32, 44), ros_st32(sp + 36, 44);
    ros_st32(sp + 40, (l2bpp + 1) << 27 | 90u << 14 | 90u << 1 | 1);
    image = sp + 44;
    return sp;
}

/* Everything the Font Manager does with a font's three files: its handle,
 * boxes, widths, metrics and a painting into a sprite.  The errors are
 * not looked at. */
static void ask(const char *font, uint32_t size)
{
    uint32_t ff[10] = { 0, 0, size * 16, size * 16, 96, 96 };
    strcpy((char *)mem + O_STR, font);
    ff[1] = at(O_STR);
    if (swi(Font_FindFont, ff))
        return;
    uint32_t h = ff[0];
    for (uint32_t c = 'A'; c <= 'C'; c++) {
        uint32_t b[10] = { h, c, 0x10 };
        swi(Font_CharBBox, b);
        uint32_t m[10] = { h, c, 0 };
        swi(Font_CharBBox, m);
    }
    uint32_t sf[10] = { h };
    swi(Font_SetFont, sf);
    strcpy((char *)mem + O_STR, "AVAT ABC");
    uint32_t sw[10] = { 0, at(O_STR), 0x7FFFFFFF, 0x7FFFFFFF, 0xFFFFFFFFu, 8 };
    swi(Font_StringWidth, sw);
    uint32_t rm[10] = { h, at(O_OUT), at(O_OUT + 8192), at(O_OUT + 8192 + 1024), at(O_OUT + 16384), 0, 0 };
    swi(Font_ReadFontMetrics, rm);
    uint32_t rk[10] = { h, 0, 0, 0, 0, at(O_OUT + 24576), at(O_OUT + 24576 + 16384) };
    swi(Font_ReadFontMetrics, rk);
    uint32_t sp = make_sprite();
    uint32_t r0[10] = { 512 + 60, at(O_AREA), sp, 0 };
    if (!swi(OS_SpriteOp, r0)) {
        memcpy(saved, r0, sizeof saved);
        uint32_t c[10] = { h, 0xFFFFFF00u, 0, 14 };
        swi(ColourTrans_SetFontColours, c);
        strcpy((char *)mem + O_STR, "ABC");
        uint32_t p[10] = { h, at(O_STR), 1u << 8 | 1u << 4, 4, 16 };
        swi(Font_Paint, p);
        uint32_t mono[10] = { h, 0xFFFFFF00u, 0, 0 };
        swi(ColourTrans_SetFontColours, mono);
        swi(Font_Paint, p);
        uint32_t back[10] = { saved[0], saved[1], saved[2], saved[3] };
        swi(OS_SpriteOp, back);
    }
    uint32_t l[10] = { h };
    swi(Font_LoseFont, l);
}

/* ---- the cases ---- */

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            remove_tree(p);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static char root[512];
static int ncase;

static void put_font(const char *dir, const uint8_t *m, uint32_t mlen, const uint8_t *o, uint32_t olen)
{
    char p[160];
    snprintf(p, sizeof p, "HostFS::FtTest.$.%s", dir);
    mkdir_ros(p);
    snprintf(p, sizeof p, "HostFS::FtTest.$.%s.IntMetric0", dir);
    save(p, m, mlen);
    snprintf(p, sizeof p, "HostFS::FtTest.$.%s.Outlines0", dir);
    save(p, o, olen);
}

/* One case: the files after change(), then everything asked of the font */
static void run(const char *label, void (*change)(uint8_t *m, uint8_t *o))
{
    static uint8_t m[sizeof metrics], o[sizeof outlines];
    char font[16];
    snprintf(font, sizeof font, "Ev%d", ++ncase);
    memcpy(m, metrics, metrics_len);
    memcpy(o, outlines, outlines_len);
    change(m, o);
    put_font(font, m, metrics_len, o, outlines_len);
    ask(font, 12);
    ask(font, 40);
    check(1, label, NULL);
}

/* The chunk table starts where the header says, 4 GiB less a little */
static void c_pixoff(uint8_t *m, uint8_t *o)
{
    (void)m;
    put32(o + 16, 0xFFFFFFF0u);
}

/* The chunk table's first entries point at the end of the file */
static void c_chunk_end(uint8_t *m, uint8_t *o)
{
    (void)m;
    uint32_t t = get32(o + 16);
    for (unsigned i = 0; i < 16; i++)
        put32(o + t + 4 * i, outlines_len - 2 - i);
}

/* Every character's offset in its chunk is far beyond the file */
static void c_char_off(uint8_t *m, uint8_t *o)
{
    (void)m;
    uint32_t t = get32(o + 16);
    for (unsigned c = 0; c < 15; c++) {
        uint32_t a = get32(o + t + 4 * c), b = get32(o + t + 4 * c + 4);
        if (a + 128 <= b && b <= outlines_len)
            for (unsigned i = 0; i < 32; i++)
                if (get32(o + a + 4 * i))
                    put32(o + a + 4 * i, 0x7FFFFFF0u + i);
    }
}

/* The last characters' paths have no end: the file ends in path bytes */
static void c_path_end(uint8_t *m, uint8_t *o)
{
    (void)m;
    uint32_t t = get32(o + 16);
    uint32_t a = get32(o + t + 4 * 14);
    for (unsigned i = 0; i < 32; i++)
        if (get32(o + a + 4 * i))
            put32(o + a + 4 * i, outlines_len - 20);
    memset(o + outlines_len - 20, 3, 20);
}

/* Character 65's scaffold lines are taken from character 65 */
static void c_scaffold_loop(uint8_t *m, uint8_t *o)
{
    (void)m;
    uint8_t *tbl = o + 52;
    tbl[2 * 65] = 3000 & 0xFF, tbl[2 * 65 + 1] = 3000 >> 8;
    tbl[3000] = 65, tbl[3001] = 0xFF, tbl[3002] = 0xFF, tbl[3003] = 0, tbl[3004] = 0;
}

/* The metrics claim 65535 characters */
static void c_nchars(uint8_t *m, uint8_t *o)
{
    (void)o;
    m[48] = 0xFF, m[51] = 0xFF;
}

/* The metrics claim no characters, so no arrays, but the map still indexes */
static void c_nchars_zero(uint8_t *m, uint8_t *o)
{
    (void)o;
    m[48] = 0, m[51] = 0;
}

/* The pointers to the misc area and the kerns lie beyond the file */
static void c_misc(uint8_t *m, uint8_t *o)
{
    (void)o;
    uint32_t n = m[48] | m[51] << 8;
    uint32_t q = 48 + 6 + (m[52] | m[53] << 8) + n * 2;
    m[q] = 0xF0, m[q + 1] = 0xFF, m[q + 2] = 0xF0, m[q + 3] = 0xFF;
}

/* The kerns have no end */
static void c_kerns(uint8_t *m, uint8_t *o)
{
    (void)o;
    uint32_t n = m[48] | m[51] << 8;
    uint32_t q = 48 + 6 + (m[52] | m[53] << 8) + n * 2;
    uint32_t k = q + (m[q + 2] | m[q + 3] << 8);
    memset(m + k, 1, metrics_len - k);
}

/* A 4 bpp bitmap file of exactly this size, whose one character is 255 x
 * 255 pixels and has one byte of data */
static void bitmap_file(int bpp1, uint8_t flags, uint8_t fill)
{
    static uint8_t f[256 + 4 * 8 + 4 * 32 + 16];
    memset(f, 0, sizeof f);
    memcpy(f, "FONT", 4);
    f[4] = bpp1 ? 1 : 4, f[5] = 4;
    f[8] = 0, f[9] = 0, f[10] = 0, f[11] = 0, f[12] = 255, f[14] = 255;
    /* the chunk table: chunk 2 (characters 64 to 95) is the last 160 bytes */
    uint32_t chunk = 64;
    for (unsigned i = 0; i <= 8; i++)
        put32(f + 16 + 4 * i, i < 3 ? chunk : chunk + 4 * 32 + 8);
    put32(f + 54, 0);
    f[54] = 12 * 16 & 0xFF, f[55] = 12 * 16 >> 8, f[56] = 96;
    f[58] = 12 * 16 & 0xFF, f[59] = 12 * 16 >> 8, f[60] = 96;
    put32(f + chunk + 4 * 1, 4 * 32);                 /* 'A' */
    uint8_t *c = f + chunk + 4 * 32;
    c[0] = flags, c[1] = 0, c[2] = 0, c[3] = 255, c[4] = 255;
    c[5] = fill;
    char p[96];
    snprintf(p, sizeof p, "HostFS::FtTest.$.Ev%d.%c256x256", ncase, bpp1 ? 'b' : 'f');
    save(p, f, chunk + 4 * 32 + 8);
}

static void c_bitmap_unpacked(uint8_t *m, uint8_t *o)
{
    (void)m, (void)o;
    bitmap_file(0, 0, 0x11);
}

static void c_bitmap_packed(uint8_t *m, uint8_t *o)
{
    (void)m, (void)o;
    bitmap_file(1, 0x12, 0x00);
}

void ros_selftest_fontfiles(void)
{
    ros_console_printf("Font Manager (native): font files that lie\n");
    mem = ros_rma_alloc(MEM_SIZE);
    if (!mem) {
        check(0, "Font files: RMA for the tests", NULL);
        return;
    }
    base = ros_addr(mem);
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-fonts-XXXXXX", tmp);
    if (!mkdtemp(root) || ros_hostfs_mount("FtTest", root)) {
        check(0, "Font files: a HostFS disc of the test's own", NULL);
        ros_rma_free(mem);
        return;
    }
    metrics_len = load("Resources:$.Fonts.Trinity.Medium.IntMetric0", metrics, sizeof metrics);
    outlines_len = load("Resources:$.Fonts.Trinity.Medium.Outlines0", outlines, sizeof outlines);
    check(metrics_len > 100 && outlines_len > 1000, "Font files: Trinity.Medium's files to start from",
          "%u %u", metrics_len, outlines_len);
    if (metrics_len > 100 && outlines_len > 1000) {
        uint32_t r[10] = { at(O_TEXT) };
        strcpy((char *)mem + O_TEXT, "Font$Path");
        r[1] = at(O_OUT), r[2] = 512, r[3] = 0, r[4] = 3;
        uint32_t e = swi(OS_ReadVarVal, r);
        char old[512] = "";
        if (!e && r[2] < sizeof old)
            memcpy(old, mem + O_OUT, r[2]), old[r[2]] = 0;
        setvar("Font$Path", "HostFS::FtTest.$.,Resources:$.Fonts.");

        run("Font files: a chunk table placed 4 GiB less a little into the file", c_pixoff);
        run("Font files: chunk entries pointing at the end of the file", c_chunk_end);
        run("Font files: character offsets far beyond the file", c_char_off);
        run("Font files: a path with no end at the end of the file", c_path_end);
        run("Font files: a scaffold line taken from the same character", c_scaffold_loop);
        run("Font files: 65535 characters claimed by the metrics", c_nchars);
        run("Font files: no characters claimed by the metrics", c_nchars_zero);
        run("Font files: the misc area and the kerns beyond the metrics", c_misc);
        run("Font files: kerns with no end", c_kerns);
        run("Font files: a 4 bpp bitmap larger than its data", c_bitmap_unpacked);
        run("Font files: a packed 1 bpp bitmap with no data", c_bitmap_packed);

        if (*old)
            setvar("Font$Path", old);
        else
            setvar("Font$Path", NULL);
    }
    /* ColourTrans: an old-style calibration table whose counts would wrap */
    ros_st32(at(O_OUT), 0x40000000u), ros_st32(at(O_OUT) + 4, 0x40000000u);
    ros_st32(at(O_OUT) + 8, 0x40000000u);
    uint32_t cal[10] = { at(O_OUT) };
    uint32_t ce = swi(ColourTrans_SetCalibration, cal);
    check(ce == 0xA00, "ColourTrans_SetCalibration: counts of 2^30 are a bad calibration table", "&%X", ce);
    uint32_t none[10] = { 0 };
    swi(ColourTrans_SetCalibration, none);

    ros_hostfs_unmount("FtTest");
    remove_tree(root);
    ros_rma_free(mem);
}
