/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_vdu.c: the VDU drivers (runtime/vdu), pixel by pixel.
 *
 * What the kernel's VDU drivers would put in screen memory, byte for byte:
 * characters at 1, 4, 8 and 32 bpp and in a double-pixel mode; colours
 * through COLOUR's masks and 256-colour shuffle; text colours with bits
 * above the pixel's at 1 and 2 bpp and in 4 bpp teletext (painted,
 * cleared and read back lane by lane); scrolling; windows; the
 * palette as GraphicsV's driver is given it; the variables and
 * OS_Bytes that read the VDU's state; and the pointer as the driver is
 * given it. The sequences go straight to the
 * VDU (ros_vdu_write), so the console is not filled with them. One goes
 * through OS_WriteC, for the error that a bad MODE gives.
 *
 * With no display there is no screen to look at, so only the state is
 * checked.
 */
#include <string.h>

#include "drmvideo.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/rom.h"
#include "rosgd/screen.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

extern const uint8_t ros_vdu_hard_font[224][8];

static os_error *vdu(const uint8_t *b, size_t n)
{
    os_error *e = NULL;
    for (size_t i = 0; i < n; i++) {
        int plain;
        os_error *x = ros_vdu_write(b[i], &plain);
        if (x)
            e = x;
    }
    return e;
}

#define V(...)                                          \
    do {                                                \
        static const uint8_t b_[] = { __VA_ARGS__ };    \
        vdu(b_, sizeof b_);                             \
    } while (0)
#define CURSOR_OFF V(23, 1, 0, 0, 0, 0, 0, 0, 0, 0)

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return (int)s.v | (int)s.c << 1;
}

static uint32_t block_addr;

/* GraphicsV 5, UpdatePointer, as the driver is given it */
static uint32_t gv_n, gv_flags, gv_r[4];
static int gv_watch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if ((s->r[4] & 0xFFFF) == 5) {
        gv_n++, gv_flags |= s->r[0];
        memcpy(gv_r, s->r, sizeof gv_r);
    }
    return ROS_VECTOR_PASS;
}

static void vsyncs(int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t r[8] = { 19 };
        swi(XOS_Byte, r);
    }
}

static uint32_t var(uint32_t n)
{
    ros_st32(block_addr, n);
    ros_st32(block_addr + 4, 0xFFFFFFFFu);
    uint32_t r[8] = { block_addr, block_addr + 8 };
    swi(XOS_ReadVduVariables, r);
    return ros_ld32(block_addr + 8);
}

static uint32_t mvar(uint32_t n)
{
    uint32_t r[8] = { 0xFFFFFFFFu, n };
    swi(XOS_ReadModeVariable, r);
    return r[2];
}

static uint8_t *screen(void)
{
    return ros_ptr(var(148));
}

static uint8_t rev8(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        if (b & (1u << i))
            r |= (uint8_t)(0x80u >> i);
    return r;
}

static const uint8_t *glyph(char c)
{
    return ros_vdu_hard_font[(uint8_t)c - 32];
}

static const struct ros_screen_block *host_block(void)
{
    const struct ros_display *d = drmvideo_display();
    return d ? ros_ptr(d->base + d->size) : NULL;
}

/* OS_ReadModeVariable on sprite mode words, as RISC OS 5.30 answers
 * (read on the farm). The size variables are never answered. The eigen
 * factors are not answered without a dpi that it knows, and ColourTrans asks
 * for (type<<27)+1. The RISC OS 5 form's type, flags and eigen factors are
 * checked. */
static void mode_words(void)
{
    static const struct { uint32_t word; uint16_t bad; uint32_t v[13]; } words[] = {
        { 0x28000001u, 0x18F6, { 0x0u, 0, 0, 0xFFFFu, 0, 0, 0, 0, 0x0u, 0x4u, 0x4u, 0, 0 } },
        { 0x201680B5u, 0x18C6, { 0x0u, 0, 0, 0xFFu, 0x1u, 0x1u, 0, 0, 0x0u, 0x3u, 0x3u, 0, 0 } },
        { 0x3013409Bu, 0x18F6, { 0x0u, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0, 0x0u, 0x5u, 0x5u, 0, 0 } },
        { 0x50000001u, 0x18F6, { 0x80u, 0, 0, 0xFFFFu, 0, 0, 0, 0, 0x0u, 0x4u, 0x4u, 0, 0 } },
        { 0x78000091u, 0x1FFF, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { 0x78A04001u, 0x18C6, { 0x4080u, 0, 0, 0xFFFFu, 0x0u, 0x0u, 0, 0, 0x0u, 0x4u, 0x4u, 0, 0 } },
        { 0x78600101u, 0x18C7, { 0, 0, 0, 0xFFFFFFFFu, 0x0u, 0x0u, 0, 0, 0x0u, 0x5u, 0x5u, 0, 0 } },
        { 0x79000091u, 0x18C6, { 0x0u, 0, 0, 0xFFFu, 0x1u, 0x2u, 0, 0, 0x0u, 0x4u, 0x4u, 0, 0 } },
        { 0x79300091u, 0x1FFF, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { 0x78610001u, 0x18F6, { 0x0u, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0, 0x0u, 0x5u, 0x5u, 0, 0 } },
    };
    uint32_t wrong = 0, which = 0, got = 0;
    for (unsigned i = 0; i < sizeof words / sizeof words[0] && !wrong; i++)
        for (uint32_t v = 0; v < 13 && !wrong; v++) {
            uint32_t r[8] = { words[i].word, v, 0xDEADu };
            int bad = (swi(XOS_ReadModeVariable, r) >> 1) & 1;
            if (bad != ((words[i].bad >> v) & 1) || r[2] != (bad ? 0xDEADu : words[i].v[v]))
                wrong = words[i].word, which = v, got = bad ? 0xFFFFFFFFu : r[2];
        }
    check(!wrong, "OS_ReadModeVariable on sprite mode words -- no size; no eig without a known dpi; RISC OS 5 form",
          "&%08X variable %u gave &%X", wrong, which, got);
}

/* Text colours with bits above the pixel's, below 8 bpp. The farm has not
 * these depths, so the expected values are as the kernel's source gives
 * them. OS_SetColour's text background 2 at 1 bpp (4 at 2 bpp) is spread by
 * SetColours to &00000001, which is set only in the word's lane 0:
 *   - A character row is its font byte k's TextExpand entry, bpc bytes at
 *     k * bpc in a table made a word at a time. So byte i takes lane
 *     (k * bpc + i) & 3 of the colours. The background's low bit appears
 *     only in rows whose k is a multiple of four (1 bpc) or even (2 bpc).
 *   - OS_Byte 135 (ReadCharacter) exclusive-ORs the screen with the
 *     background word. At 1 bpc four rows are packed into a word, the first
 *     on top, so row r is against lane 3 - (r & 3). At 2 bpc it is the
 *     cell's own word.
 *   - ClearThisBox stores whole words where aligned, and the low byte
 *     either side.
 *   - 2 bpc gap rows are the table's first byte, twice. */
static void wide_colours(void)
{
    static const uint8_t k[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x18, 0x3C, 0x7E };
    uint8_t def[10] = { 23, 201 }, back[10] = { 23, 202 };
    uint8_t *s;
    int ok = 1;

    /* 1 bpp: MODE 0 */
    V(22, 0);
    CURSOR_OFF;
    memcpy(def + 2, k, 8);
    vdu(def, sizeof def);
    uint32_t sc[8] = { 0x50, 2 };
    swi(XOS_SetColour, sc);
    V(30, 201);
    s = screen();
    for (int y = 0; y < 8; y++) {
        uint8_t want = (uint8_t)(rev8(k[y]) | (k[y] & 3 ? 0 : 1));
        ok &= s[y * 80] == want;
        back[2 + y] = rev8((uint8_t)(want ^ ((y & 3) == 3)));
    }
    vdu(back, sizeof back);
    uint32_t b135[8] = { 135 };
    V(30);
    swi(XOS_Byte, b135);
    check(ok && b135[1] == 202,
          "VDU: 1 bpp, a text background above the pixel -- TextExpand's lane by font byte; "
          "OS_Byte 135 row by row", "&%02X &%02X, read %u", s[80], s[3 * 80], b135[1]);
    V(28, 1, 1, 8, 1, 12, 26);
    s = screen();
    ok = 1;
    for (int y = 0; y < 8; y++) {
        static const uint8_t want[8] = { 1, 1, 1, 1, 0, 0, 0, 1 };
        ok &= !memcmp(s + (8 + y) * 80 + 1, want, 8);
    }
    check(ok, "VDU: 1 bpp, a window's CLS -- whole words, the low byte either side", "%02X %02X",
          s[641], s[644]);

    /* 2 bpp: MODE 8 */
    V(22, 8);
    CURSOR_OFF;
    vdu(def, sizeof def);
    sc[0] = 0x50, sc[1] = 4;
    swi(XOS_SetColour, sc);
    V(30, 201);
    s = screen();
    ok = 1;
    back[1] = 203;
    for (int y = 0; y < 8; y++) {
        uint32_t plain = 0;
        for (int x = 0; x < 8; x++)
            if (k[y] >> (7 - x) & 1)
                plain |= 3u << (2 * x);
        uint8_t b0 = (uint8_t)(plain | (k[y] & 1 ? 0 : 1)), b1 = (uint8_t)(plain >> 8);
        ok &= s[y * 160] == b0 && s[y * 160 + 1] == b1;
        back[2 + y] = (uint8_t)(k[y] | (k[y] & 1 ? 0x80 : 0));  /* pixel 0 not 01: set */
    }
    vdu(back, sizeof back);
    V(30);
    swi(XOS_Byte, (b135[0] = 135, b135));
    check(ok && b135[1] == 203,
          "VDU: 2 bpp, a text background above the pixel -- TextExpand's lanes; OS_Byte 135 "
          "by the cell's word", "&%02X &%02X, read %u", s[160], s[161], b135[1]);

    /* 2 bpp gap rows: MODE 11 */
    V(22, 11);
    CURSOR_OFF;
    swi(XOS_SetColour, (sc[0] = 0x50, sc[1] = 4, sc));
    V(30, 'A');
    s = screen();
    check(s[8 * 160] == 1 && s[8 * 160 + 1] == 1 && s[9 * 160] == 1 && s[9 * 160 + 1] == 1,
          "VDU: MODE 11, a text background above the pixel -- both gap-row bytes its low byte",
          "&%02X &%02X", s[8 * 160], s[8 * 160 + 1]);
    uint32_t b25[8] = { 25, 6 };                    /* characters 201-203 back */
    swi(XOS_Byte, b25);
}

/* Cursor editing (DoCursorEdit): the screen editor the cursor keys and
 * COPY drive while OS_Byte 4's state is 0.  Driven here at the VDU's own
 * entry, as the keyboard drives it, so no keys are needed. */
static void cursor_editing(void)
{
    uint8_t c;
    int ok = 1;
    uint32_t step = 0, st[8];
#define STEP(n, expr) do { ok &= (expr); if (!ok && !step) step = (n); } while (0)

    V(26);                              /* the windows back to the whole screen, */
    CURSOR_OFF;                         /* and a steady cursor: no read over a flash */
    V(12, 30);
    vdu((const uint8_t *)"ABC", 3);
    V(13, 10);                          /* the next line down: typing goes here */

    st[0] = 117;
    swi(XOS_Byte, st);
    STEP(1, (st[1] & 64) == 0);         /* not split yet */

    STEP(2, ros_vdu_cursor_edit(0x8B, &c) == 0);        /* up: splits, no character */
    st[0] = 117;
    swi(XOS_Byte, st);
    STEP(3, (st[1] & 64) != 0);         /* OS_Byte 117 bit 6: cursor editing */

    st[0] = 134;                        /* and the output cursor has not moved */
    swi(XOS_Byte, st);
    STEP(4, (st[1] & 0xFF) == 0 && (st[2] & 0xFF) == 1);

    char got[5] = { 0 };                /* COPY three times: the line above */
    for (int i = 0; i < 3; i++) {
        STEP(5, ros_vdu_cursor_edit(0x87, &c) == 1);
        got[i] = (char)c;
    }
    STEP(6, memcmp(got, "ABC", 3) == 0);

    st[0] = 135;                        /* OS_Byte 135 reads under that cursor too */
    swi(XOS_Byte, st);
    STEP(7, (st[1] & 0xFF) == ' ');     /* the cell after "ABC", COPY having moved on */

    V(13);                              /* a carriage return ends editing */
    st[0] = 117;
    swi(XOS_Byte, st);
    STEP(8, (st[1] & 64) == 0);
    STEP(9, ros_vdu_cursor_edit(0x87, &c) == 0);        /* COPY unsplit: a beep only */

    /* Left at the left edge is the right of the line above: the second
     * cursor wraps round the window, and the screen never scrolls for it.
     * The last column is the mode's, asked for (ScrRCol) rather than
     * assumed, and "Z" goes down before the cursors split so that writing
     * it cannot disturb what is being tested. */
    uint32_t mv[8] = { 0xFFFFFFFFu, 1 };
    swi(XOS_ReadModeVariable, mv);
    uint32_t rcol = mv[2];
    /* VDU 31 takes byte coordinates, so on a screen of more than 255
     * columns (2560 pixels at a scale of 2 gives 320) it cannot reach the
     * last one.  There the test uses a text window 80 columns wide: the
     * second cursor wraps round the window, not the screen. */
    if (rcol > 254) {
        uint32_t bv[8] = { 0xFFFFFFFFu, 2 };            /* ScrBRow */
        swi(XOS_ReadModeVariable, bv);
        uint32_t brow = bv[2] > 255 ? 255 : bv[2];
        rcol = 79;
        vdu((const uint8_t[]){ 28, 0, (uint8_t)brow, (uint8_t)rcol, 0 }, 5);
    }
    V(31);
    vdu((const uint8_t[]){ (uint8_t)rcol, 0 }, 2);      /* the top line's last cell */
    vdu((const uint8_t *)"Z", 1);
    V(31);
    vdu((const uint8_t[]){ 0, 1 }, 2);  /* typing goes to the start of the next */
    STEP(10, ros_vdu_cursor_edit(0x88, &c) == 0);       /* left, over the line's end */
    STEP(11, ros_vdu_cursor_edit(0x87, &c) == 1);
    STEP(12, c == 'Z');
    got[3] = (char)c;
    V(13);

    check(ok, "VDU: cursor editing -- a cursor key splits the cursors (OS_Byte 117 bit 6) "
          "without moving the output one, COPY reads the character under the second cursor "
          "and moves it on, a carriage return ends it, COPY unsplit gives nothing, and the "
          "second cursor wraps round the window",
          "step %u, got \"%s\", the last column %u", step, got, rcol);
    V(26);                              /* the windows back to the whole screen */
#undef STEP
}

/* Input that comes from outside the runtime is refused rather than
 * followed: a module image, a sprite header, and a program that calls
 * thousands of different SWI numbers. */
static void hardening(void)
{
    uint8_t elf[128] = { 0x7F, 'E', 'L', 'F' };
    uint32_t phoff = 0xFFFFFFF0u, base, size;
    void *blk;
    elf[44] = 1;
    memcpy(elf + 28, &phoff, 4);
    check(ros_module_image_load(elf, sizeof elf, "Evil", &base, &blk, &size) != NULL,
          "A module image whose program header offset wraps is refused", NULL);

    uint8_t *area = ros_rma_alloc(256);
    memset(area, 0, 256);
    uint32_t a = ros_addr(area), sp = a + 16;
    ros_st32(a, 256), ros_st32(a + 4, 1), ros_st32(a + 8, 16), ros_st32(a + 12, 16 + 56);
    ros_st32(sp, 56);
    memcpy(ros_ptr(sp + 4), "evil", 4);
    ros_st32(sp + 16, 0x7FFFFFFFu), ros_st32(sp + 20, 3), ros_st32(sp + 28, 31);
    ros_st32(sp + 32, 44), ros_st32(sp + 36, 44), ros_st32(sp + 40, 0);
    uint32_t r[8] = { 0x200 + 33, a, sp };
    check(swi(XOS_SpriteOp, r) & 1, "SpriteOp flip x on a sprite whose header overstates it is refused",
          NULL);
    r[0] = 0x200 + 47, r[1] = a, r[2] = sp;
    check(swi(XOS_SpriteOp, r) & 1, "SpriteOp flip y on a sprite whose header overstates it is refused",
          NULL);
    ros_rma_free(area);

    int errors = 0;
    for (uint32_t n = 0x70000; n < 0x70000 + 5000; n++) {
        uint32_t q[8] = { 0 };
        errors += swi(0x20000 | n, q) & 1;
    }
    check(errors > 4900, "5000 different unknown SWI numbers are called and counted without a hang",
          "%d errors", errors);
}

void ros_selftest_vdu(void)
{
    hardening();
    block_addr = ros_addr(ros_rma_alloc(64));
    mode_words();
    const struct ros_display *d = drmvideo_display();

    /* ---- the mode at start: the driver's, a selector ---- */
    uint32_t r[8] = { 1 };
    swi(XOS_ScreenMode, r);
    uint32_t start = r[1];
    uint32_t keep = ros_addr(ros_rma_alloc(64));
    if (start >= 256)
        memcpy(ros_ptr(keep), ros_ptr(start), 64);
    if (!d) {
        check(start == 28 && mvar(9) == 3,
              "VDU: no display -- mode 28's variables, nothing drawn", "mode %u", start);
        ros_rma_free(ros_ptr(keep));
        ros_rma_free(ros_ptr(block_addr));
        return;
    }
    check(start >= 256 && ros_ld32(start) == 1 && ros_ld32(start + 4) == d->width &&
              ros_ld32(start + 12) == 5 && mvar(9) == 5 && mvar(3) == 0xFFFFFFFFu &&
              mvar(11) == d->width - 1 && mvar(1) == d->width / 8 - 1,
          "VDU: the start-up mode -- GraphicsV_StartupMode's selector, the display's size, 32 bpp",
          "&%08X: %ux%u", start, mvar(11) + 1, mvar(12) + 1);

    /* ---- MODE 0: 1 bpp, the variables, the host told ---- */
    V(22, 0);
    CURSOR_OFF;
    const struct ros_screen_block *b = host_block();
    swi(XOS_ScreenMode, (r[0] = 1, r));
    check(r[1] == 0 && mvar(11) == 639 && mvar(12) == 255 && mvar(6) == 80 &&
              mvar(7) == 20480 && mvar(4) == 1 && mvar(5) == 2 && b->xres == 640 &&
              b->yres == 256 && b->bpp == 1,
          "VDU 22,0 -- MODE 0's variables; the display 640 x 256 x 1", "%ux%u %u bpp",
          b->xres, b->yres, b->bpp);

    V('A');
    uint8_t *s = screen();
    int ok = 1;
    for (int y = 0; y < 8; y++)
        ok &= s[y * 80] == rev8(glyph('A')[y]);
    check(ok && var(155) == 1 && var(156) == 0,
          "VDU: 'A' in MODE 0 -- the font's rows, bit 7 leftmost in the lowest bit", "&%02X",
          s[0]);

    /* the character back, and where the cursor is */
    uint32_t b134[8] = { 134 }, b135[8] = { 135 };
    swi(XOS_Byte, b134);
    V(8);
    swi(XOS_Byte, b135);
    check(b134[1] == 1 && b134[2] == 0 && b135[1] == 'A' && b135[2] == 0,
          "OS_Byte 134, 135 -- POS, VPOS; the character at the cursor", "%u,%u '%c'", b134[1],
          b134[2], b135[1]);

    /* immediate wrap: 80 characters, then the next row */
    V(12);
    for (int i = 0; i < 80; i++)
        V('x');
    swi(XOS_Byte, (b134[0] = 134, b134));
    check(b134[1] == 0 && b134[2] == 1, "VDU: the 80th character wraps at once",
          "%u,%u", b134[1], b134[2]);

    /* scrolling: a character on the bottom row, then a line feed */
    V(12, 31, 5, 31, 'B', 10);
    s = screen();
    ok = 1;
    for (int y = 0; y < 8; y++)
        ok &= s[(30 * 8 + y) * 80 + 5] == rev8(glyph('B')[y]) && s[(31 * 8 + y) * 80 + 5] == 0;
    check(ok, "VDU: scrolled -- the bottom row moves up, the new one is clear", NULL);

    /* a text window: CLS clears only it, and the window scrolls alone */
    V(12, 'Z', 28, 10, 20, 30, 10, 'W', 12);
    swi(XOS_Byte, (b134[0] = 134, b134));
    uint32_t b117[8] = { 117 };
    swi(XOS_Byte, b117);
    ok = s[0] == rev8(glyph('Z')[0]) && s[(10 * 8) * 80 + 10] == 0;
    check(ok && b134[1] == 0 && b134[2] == 0 && (b117[1] & 8) && var(256) == 20 &&
              var(257) == 10 && var(132) == 10 && var(135) == 10,
          "VDU 28 -- a text window: CLS clears it alone; WindowWidth is its width less one",
          "117 &%X, %u x %u", b117[1], var(256), var(257));
    V(26);

    /* VDU 23: a character defined, printed */
    V(12, 23, 200, 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81, 200);
    uint32_t w10 = ros_addr(ros_rma_alloc(16));
    ros_st8(w10, 200);
    uint32_t wr[8] = { 10, w10 };
    swi(XOS_Word, wr);
    check(s[0] == 0x81 && s[80] == 0x42 && ros_ld8(w10 + 1) == 0x81 && ros_ld8(w10 + 2) == 0x42,
          "VDU 23,200 -- a character defined, printed; OS_Word 10 reads it", NULL);
    uint32_t b25[8] = { 25, 6 };
    swi(XOS_Byte, b25);
    ros_st8(w10, 200);
    swi(XOS_Word, (wr[0] = 10, wr[1] = w10, wr));
    check(ros_ld8(w10 + 1) == glyph((char)200)[0], "OS_Byte 25 -- the font's characters 192-223 reset",
          NULL);

    /* the queue: OS_Byte 218 says what VDU 17 still wants */
    V(17);
    uint32_t b218[8] = { 218, 0, 0xFF };
    swi(XOS_Byte, b218);
    V(1);
    uint32_t b218b[8] = { 218, 0, 0xFF };
    swi(XOS_Byte, b218b);
    check(b218[1] == 0xFF && b218b[1] == 0 && var(155) == 1,
          "OS_Byte 218 -- one byte wanted after VDU 17, none after", "&%X &%X", b218[1],
          b218b[1]);

    wide_colours();

    /* ---- MODE 12: 4 bpp, colours as nibbles, the leftmost low ---- */
    V(22, 12);
    CURSOR_OFF;
    V(17, 3, 17, 132, 'H');
    s = screen();
    uint32_t row0 = s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24;
    uint32_t want = 0;
    for (int x = 0; x < 8; x++)
        want |= (glyph('H')[0] >> (7 - x) & 1 ? 3u : 4u) << (4 * x);
    check(row0 == want && mvar(6) == 320, "VDU: MODE 12 -- COLOUR 3 on 4, four bits a pixel",
          "&%08X, want &%08X", row0, want);

    /* the palette: VDU 19 to GraphicsV's driver; 16 colours' flashing */
    V(19, 1, 16, 10, 20, 30);
    uint32_t rp[8] = { 1, 16 }, rf[8] = { 9, 16 };
    swi(XOS_ReadPalette, rp);
    swi(XOS_ReadPalette, rf);
    b = host_block();
    check(rp[2] == 0x1E140A10 && rp[3] == 0x1E140A10 && b->palette[1] == 0x1E140A &&
              (rf[2] & 0xFF) == 17 && (rf[3] & 0xFF) == 18 && rf[2] >> 8 == 0x0000FF &&
              rf[3] >> 8 == 0xFFFF00,
          "VDU 19 -- the palette; the host's; colour 9 flashes red and cyan",
          "&%08X &%08X", rp[2], rf[3]);
    V(20);
    swi(XOS_ReadPalette, (rp[0] = 1, rp[1] = 16, rp));
    check(rp[2] == 0x0000FF10 && var(155) == 7, "VDU 20 -- the default palette and colours back",
          "&%08X", rp[2]);

    /* PLOT at 4 bpp: a rectangle, a point read back, an EOR line */
    V(16, 18, 0, 5, 25, 4, 0, 0, 0, 0, 25, 101, 3, 0, 7, 0);    /* GCOL 0,5: (0,0)-(3,7) */
    uint32_t rpt[8] = { 2, 4 }, rpo[8] = { 2000, 0 };
    swi(XOS_ReadPoint, rpt);
    swi(XOS_ReadPoint, rpo);
    s = screen();
    uint32_t bottom = 255 * 320;                /* internal row 0 is the screen's last */
    int prow = s[bottom] == 0x55 && s[bottom + 1] == 0 && s[bottom - 320] == 0x55;
    V(18, 3, 2, 25, 4, 0, 0, 0, 0, 25, 5, 3, 0, 0, 0);          /* EOR 2 along row 0 */
    check(prow && rpt[2] == 5 && rpt[4] == 0 && rpo[4] == 0xFFFFFFFFu && s[bottom] == 0x77 &&
              s[bottom + 1] == 0,
          "PLOT at 4 bpp -- a rectangle, OS_ReadPoint, an EOR line over it", "&%02X &%02X",
          s[bottom], s[bottom + 1]);

    /* ---- MODE 2: double pixels, a character pixel a byte ---- */
    V(22, 2);
    CURSOR_OFF;
    V('I');
    s = screen();
    b = host_block();
    ok = 1;
    for (int x = 0; x < 8; x++)
        ok &= s[x] == (glyph('I')[0] >> (7 - x) & 1 ? 0x77 : 0x00);
    check(ok && mvar(11) == 159 && b->xres == 320,
          "VDU: MODE 2 -- 160 pixels on 320, each painted twice", "%u", b->xres);

    /* ---- MODE 28: 256 colours, COLOUR through the tint shuffle ---- */
    V(22, 28);
    CURSOR_OFF;
    V('M');
    uint8_t fg255 = screen()[1];            /* 'M' has its second pixel set */
    V(17, 7, 13, 'M');
    s = screen();
    uint32_t rp8[8] = { 0x37, 16 };
    swi(XOS_ReadPalette, rp8);
    check(fg255 == 0xFF && s[1] == 0x37 && var(155) == 7 && rp8[2] == 0x3377FF10u &&
              mvar(3) == 63,
          "VDU: MODE 28 -- white is 255; COLOUR 7 is &37, VIDC10's &FF7733", "&%02X, &%08X",
          s[1], rp8[2]);

    /* ---- MODE 7: teletext, as the farm's RISC OS 5.30 gives it ---- */
    if (d) {
        V(22, 7);
        CURSOR_OFF;
        V(136, 'A', 137, 31, 5, 0, '#', 31, 5, 0);      /* a flashing A; a hash, read back */
        uint32_t r135[8] = { 135 };
        swi(XOS_Byte, r135);
        uint32_t size = mvar(7), half = size / 2, ll = mvar(6);
        const uint8_t *t = screen() + 50 * ll + 80 + 16;    /* the page centred; cell 1 */
        int lit0 = 0, lit1 = 0;
        for (uint32_t y = 0; y < 20; y++)
            for (uint32_t x = 0; x < 16; x++)
                lit0 += t[y * ll + x] == 7, lit1 += t[half + y * ll + x] != 0;
        /* the display shows each bank in turn, 48 VSyncs and 16: both
         * within 70, whichever it began with */
        const struct ros_screen_block *tb = host_block();
        int seen0 = 0, seen1 = 0, other = 0, n = 0;
        for (; n < 70 && !(seen0 && seen1); n++, vsyncs(1)) {
            uint32_t y = tb->yoffset;
            seen0 |= y == 0, seen1 |= y == 600, other |= y != 0 && y != 600;
        }
        check(mvar(0) == 0xE7 && mvar(11) == 799 && mvar(12) == 599 && size == 960000 &&
                  var(167) == 16 && var(150) >= size && r135[1] == '#' && lit0 > 20 &&
                  lit1 == 0 && tb->bpp == 8 && tb->xres == 800 && seen0 && seen1 && !other,
              "VDU: MODE 7 -- 800 x 600 teletext, two banks in the framestore, the flash between them",
              "flags &%X, %ux%u, size %u of %u, char &%X, lit %d/%d, pans %d %d %d in %d VSyncs",
              mvar(0), mvar(11) + 1, mvar(12) + 1, size, var(150), r135[1], lit0, lit1, seen0,
              seen1, other, n);

        /* 4 bpp teletext (not the farm's): OS_SetColour's text background
         * &10 is spread to &00000001, and used by a cell in the colours last
         * painted. Each half of the cell is a TextExpand word
         * (WrchHiResTTX4), so pixels 0 and 8 alone are in colour 1. */
        uint32_t tsel = ros_addr(ros_rma_alloc(32));
        const uint32_t tw[8] = { 1, 800, 600, 2, 0xFFFFFFFFu, 0, 2, 0xFFFFFFFFu };
        memcpy(ros_ptr(tsel), tw, sizeof tw);
        uint32_t tsm[8] = { 0, tsel };
        int tv = swi(XOS_ScreenMode, tsm) & 1;
        CURSOR_OFF;
        V('A', 'B');
        uint32_t tsc[8] = { 0x50, 0x10 };
        swi(XOS_SetColour, tsc);
        V(' ');
        ll = mvar(6);
        t = screen() + 2 * 8;                           /* 50 x 30, the whole screen; cell 2 */
        static const uint8_t want16[8] = { 1, 0, 0, 0, 1, 0, 0, 0 };
        check(!tv && mvar(9) == 2 && (mvar(0) & 2) && !memcmp(t, want16, 8) &&
                  !memcmp(t + 19 * ll, want16, 8),
              "VDU: 4 bpp teletext, a text background above the pixel -- nibble x & 7 of the "
              "word", "%d, %u bpp, &%02X &%02X", tv, 1u << mvar(9), t[0], t[1]);
        ros_rma_free(ros_ptr(tsel));
    }

    /* ---- the start-up mode again: 32 bpp ---- */
    uint32_t sm[8] = { 0, keep };
    swi(XOS_ScreenMode, sm);
    CURSOR_OFF;
    V(17, 7, 'M');
    s = screen();
    uint32_t px;
    memcpy(&px, s + 4, 4);                  /* the second pixel */
    uint32_t bgpx;
    memcpy(&bgpx, s, 4);
    check(px == 0x3377FF && bgpx == 0 && mvar(9) == 5,
          "VDU: 32 bpp -- COLOUR 7 is &FF7733 with its tint, &xBGR", "&%08X", px);

    /* a mode the display cannot show, and one there is not, through
     * OS_WriteC: an error */
    uint32_t sel = ros_addr(ros_rma_alloc(32));
    uint32_t big[6] = { 1, d->max_width + 64, 480, 5, 0xFFFFFFFFu, 0xFFFFFFFFu };
    memcpy(ros_ptr(sel), big, sizeof big);
    uint32_t cm[8] = { sel };
    int cv = swi(XOS_CheckModeValid, cm);
    uint32_t wc[8] = { 22 }, wc2[8] = { 54 };
    swi(XOS_WriteC, wc);
    int v = swi(XOS_WriteC, wc2);
    check((cv & 2) && cm[0] == 0xFFFFFFFFu && (v & 1) &&
              ((os_error *)ros_ptr(wc2[0]))->errnum == 0x1ED && mvar(9) == 5,
          "VDU 22,54 and a mode too wide -- \"Screen mode not available\", the mode kept", NULL);
    uint32_t cm0[8] = { 28 };
    check(swi(XOS_CheckModeValid, cm0) == 0 && cm0[0] == 28, "OS_CheckModeValid -- mode 28 is",
          NULL);

    /* the pointer: a shape by OS_Word 21, chosen by OS_Byte 106, at the
     * mouse, given to the driver at VSync (GraphicsV 5) */
    ros_vector_claim_native(ROS_GRAPHICSV, gv_watch, 0);
    uint32_t pw = ros_addr(ros_rma_alloc(32)), pd = pw + 16;
    static const uint8_t data[6] = { 0x1B, 0xE4, 0x55, 0xAA, 0xFF, 0x00 };
    memcpy(ros_ptr(pd), data, 6);
    uint8_t def[10] = { 0, 1, 2, 3, 2, 1 };
    memcpy(def + 6, &pd, 4);
    memcpy(ros_ptr(pw), def, 10);
    uint32_t w21[8] = { 21, pw }, b106[8] = { 106, 1 };
    swi(XOS_Word, w21);
    swi(XOS_Byte, b106);
    uint8_t at[5] = { 3, 100, 0, 200, 0 };
    memcpy(ros_ptr(pw), at, 5);
    swi(XOS_Word, w21);
    gv_n = gv_flags = 0;
    vsyncs(3);
    uint32_t ptx = (100u >> mvar(4)) - 2, pty = mvar(12) - (200u >> mvar(5)) - 1;
    const uint8_t *img = gv_n ? ros_ptr(ros_ld32(gv_r[3] + 4)) : NULL;
    check(b106[1] == 0 && gv_n && gv_flags == 3 && gv_r[1] == ptx && gv_r[2] == pty &&
              ros_ld8(gv_r[3]) == 2 && ros_ld8(gv_r[3] + 1) == 3 && img[0] == 0x1B &&
              img[1] == 0xE4 && img[2] == 0 && img[8] == 0x55 && img[16] == 0xFF && img[17] == 0,
          "Pointer: OS_Word 21 and OS_Byte 106 -- the shape, padded, at the mouse, to GraphicsV",
          "%u calls, flags %u, at %d,%d", gv_n, gv_flags, (int)gv_r[1], (int)gv_r[2]);
    uint32_t mo[8] = { 0 };
    swi(XOS_Mouse, mo);
    check(mo[0] == 100 && mo[1] == 200 && mo[2] == 0, "OS_Mouse -- the mouse where OS_Word 21,3 put it",
          "%d,%d", (int)mo[0], (int)mo[1]);
    uint32_t off[8] = { 106, 0 };
    swi(XOS_Byte, off);
    gv_n = gv_flags = 0;
    vsyncs(2);
    check(off[1] == 1 && gv_n && gv_flags == 0 && gv_r[3] == 0, "Pointer: OS_Byte 106,0 -- off",
          "flags %u", gv_flags);
    ros_vector_release_native(ROS_GRAPHICSV, gv_watch, 0);
    ros_rma_free(ros_ptr(pw));

    cursor_editing();

    V(12);
    V(23, 1, 1, 0, 0, 0, 0, 0, 0, 0);       /* the cursor as it was */
    ros_rma_free(ros_ptr(sel));
    ros_rma_free(ros_ptr(w10));
    ros_rma_free(ros_ptr(keep));
    ros_rma_free(ros_ptr(block_addr));
}
