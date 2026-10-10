/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* term.c -- a VT100 over the VDU drivers, for programs on a pseudo-terminal
 * (pty.c): what *SSH draws with.
 *
 * The terminal is the text window, as it is when the command starts, with
 * the cursor where it is. A program's output is drawn from there. When the
 * program ends, the window, the colours and the cursor are as they were,
 * with the cursor below what it drew. What the terminal keeps:
 *
 *   - the cursor, the scroll region, and the attributes, as its own model.
 *     The VDU cursor is moved (VDU 31) only when the next character needs it
 *     somewhere other than where the VDU drivers put it;
 *   - VT100's deferred wrap. A character in the last column leaves the
 *     cursor there, with the newline waiting for the next character. This
 *     is the VDU drivers' own, VDU 23,16 bit 0 (C81Bit), set while it runs;
 *   - scrolling, by a newline at the region's foot, reverse index at its
 *     head, and insert and delete lines and characters. These are VDU 23,7
 *     in a text window (VDU 28) round the part that moves. Erasing is VDU 12
 *     in a window round the part erased, in the background colour (as xterm
 *     does);
 *   - colours: SGR 30-37, 90-97, 38;5;n and 38;2;r;g;b and their
 *     backgrounds, as ColourTrans_SetTextColour's nearest. Bold is the
 *     bright colour. Reverse is kept. The defaults are the colours the text
 *     had;
 *   - UTF-8, read and drawn in RISC OS's Latin-1. Acorn's characters at
 *     &80-&9F stand for the quotes, dashes, ellipsis, bullet and the like.
 *     Box drawing and DEC's line-drawing set are drawn as + - |, and
 *     anything else as ?. A wide character is drawn as ?? so that the columns
 *     stay where the program thinks them. A combining one is drawn as
 *     nothing;
 *   - replies to the cursor-position and device-attribute requests;
 *   - the alternate screen (1049) as a cleared one. What was under it is not
 *     kept.
 *
 * The keyboard goes to the program as it comes, from OS_Byte 129 (so an
 * *Exec file types too). The console's own escape sequences for the cursor
 * keys are rewritten to ESC O when the program asked for application cursor
 * keys. RISC OS's cursor codes (&8C-&8F, OS_Byte 4,2 while the session runs)
 * become the sequences. Backspace becomes DEL. Escape is ESC, not RISC OS's
 * Escape (OS_Byte 229,1 while it runs). The program ends when it ends: ssh
 * at its "exit", or "~.".
 *
 * In a task window the terminal is a stream. The task window's editor draws
 * it, with no cursor to move or text window to set. The program's text goes
 * as it comes, with CR, LF, tab and backspace, its escape sequences left
 * out, and UTF-8 as Latin-1 or ?. A full-screen program has no screen.
 *
 * Nothing here polls. The loop sleeps (ros_sleep_fd) on the program's side
 * of the terminal, so a task window's desktop goes on meanwhile.
 */
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pty.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define XCOLOURTRANS_SETTEXTCOLOUR (0x20000u | 0x40761u)
#define XOS_SETCOLOUR_N (0x20000u | 0x61u)

#define DEFAULT (-1)
#define RGB 0x1000000

struct term {
    struct pty *pty;                    /* where replies go, or NULL when replaying */
    int stream, sstate, lead;           /* in a task window: a stream (stream_byte) */
    int L, T, cols, rows;               /* the window, on the screen */
    int x, y, pending;                  /* the cursor, and a deferred wrap */
    int top, bot;                       /* the scroll region */
    int synced;                         /* the VDU cursor is at x, y */
    int autowrap, cursor_keys_app, cursor_on;
    int sx, sy, sfg, sbg, sbold, srev, sg0;    /* ESC 7's */
    int fg, bg, bold, reverse;          /* colour: DEFAULT, 0-255, or RGB | &RRGGBB */
    int dirty;                          /* the attributes to be set */
    uint32_t orig_fg, orig_bg;          /* the text colours before */
    int g0_graphics, g1_graphics, shifted;
    /* the parser */
    int state;
    int param[16], np, priv, inter;
    uint32_t cp;
    int need;
    /* VDU codes collected, written together */
    uint8_t out[2048];
    size_t n;
    /* the keyboard */
    int kstate;
};

/* ---- the VDU ---------------------------------------------------------------- */

static void flush(struct term *t)
{
    if (t->n)
        xos_write_n(t->out, (uint32_t)t->n);
    t->n = 0;
}

static void emit(struct term *t, uint8_t b)
{
    if (t->n == sizeof t->out)
        flush(t);
    t->out[t->n++] = b;
}

static void emit2(struct term *t, uint8_t a, uint8_t b)
{
    emit(t, a), emit(t, b);
}

/* A text window in the terminal's coordinates */
static void win(struct term *t, int l, int top, int r, int b)
{
    emit(t, 28);
    emit2(t, (uint8_t)(t->L + l), (uint8_t)(t->T + b));
    emit2(t, (uint8_t)(t->L + r), (uint8_t)(t->T + top));
}

static void full(struct term *t)
{
    win(t, 0, 0, t->cols - 1, t->rows - 1);
    t->synced = 0;
}

static void move_vdu(struct term *t)
{
    if (t->synced)
        return;
    emit(t, 31);
    emit2(t, (uint8_t)t->x, (uint8_t)t->y);
    t->synced = 1;
}

/* VDU 23,7,0,d,0 moves the window's contents a character. d is 0 for right,
 * 1 for left, 2 for down and 3 for up */
static void scroll(struct term *t, int l, int top, int r, int b, int d, int times)
{
    win(t, l, top, r, b);
    for (int i = 0; i < times; i++) {
        emit2(t, 23, 7);
        emit2(t, 0, (uint8_t)d);
        for (int k = 0; k < 6; k++)
            emit(t, 0);
    }
    full(t);
}

static void erase(struct term *t, int l, int top, int r, int b)
{
    if (l > r || top > b)
        return;
    win(t, l, top, r, b);
    emit(t, 12);
    full(t);
}

static uint32_t swi(uint32_t n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = r0, c.r[1] = r1, c.r[2] = r2, c.r[3] = r3;
    ros_swi(&c, n);
    return c.v ? 0xFFFFFFFFu : c.r[1];
}

/* ---- colours ------------------------------------------------------------------ */

static uint32_t rgb_of(int c)
{
    static const uint32_t base[16] = {
        0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
        0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    };
    if (c & RGB)
        return (uint32_t)c & 0xFFFFFF;
    if (c < 16)
        return base[c];
    if (c < 232) {
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        c -= 16;
        return (uint32_t)lv[c / 36] << 16 | (uint32_t)lv[c / 6 % 6] << 8 | lv[c % 6];
    }
    uint32_t g = 8 + 10 * (uint32_t)(c - 232);
    return g << 16 | g << 8 | g;
}

/* One of the two text colours: a colour, or one of the originals */
static void set_one(int spec, int background, uint32_t orig)
{
    if (spec == DEFAULT) {
        swi(XOS_SETCOLOUR_N, 0x40u | (background ? 0x10u : 0), orig, 0, 0);
        return;
    }
    uint32_t rgb = rgb_of(spec);        /* &RRGGBB to ColourTrans's &BBGGRR00 */
    uint32_t pal = (rgb & 0xFF) << 24 | (rgb & 0xFF00) << 8 | (rgb & 0xFF0000) >> 8;
    swi(XCOLOURTRANS_SETTEXTCOLOUR, pal, 0, 0, background ? 0x80u : 0);
}

static void apply(struct term *t)
{
    if (!t->dirty)
        return;
    t->dirty = 0;
    int fg = t->fg, bg = t->bg;
    if (t->bold && fg >= 0 && fg < 8)
        fg += 8;
    uint32_t ofg = t->orig_fg, obg = t->orig_bg;
    if (t->reverse) {
        int k = fg;
        fg = bg, bg = k;
        uint32_t o = ofg;
        ofg = obg, obg = o;
    }
    flush(t);
    set_one(fg, 0, ofg);
    set_one(bg, 1, obg);
}

/* ---- characters ------------------------------------------------------------------ */

static void linefeed(struct term *t)
{
    if (t->y == t->bot)
        scroll(t, 0, t->top, t->cols - 1, t->bot, 3, 1);
    else if (t->y < t->rows - 1)
        t->y++;
    t->synced = 0;
}

static void reverse_index(struct term *t)
{
    if (t->y == t->top)
        scroll(t, 0, t->top, t->cols - 1, t->bot, 2, 1);
    else if (t->y > 0)
        t->y--;
    t->synced = 0;
}

/* A Latin-1 character (32-126, 128-255) at the cursor */
static void put(struct term *t, uint8_t ch)
{
    if (t->pending) {
        t->pending = 0;
        if (t->autowrap) {
            t->x = 0;
            linefeed(t);
        }
    }
    apply(t);
    move_vdu(t);
    emit(t, ch);
    if (t->x < t->cols - 1) {
        t->x++;
    } else {
        t->pending = 1;                 /* the VDU drivers' C81Bit, too */
        t->synced = 0;
    }
}

static uint8_t dec_graphics(uint8_t c)
{
    switch (c) {
    case 'j': case 'k': case 'l': case 'm': case 'n':
    case 't': case 'u': case 'v': case 'w': return '+';
    case 'q': case 'o': case 'p': case 'r': case 's': return '-';
    case 'x': return '|';
    case 'a': return '#';
    case '`': return '*';
    case '~': return 0x8F;              /* bullet */
    case 'f': return 0xB0;              /* degree */
    case 'g': return 0xB1;              /* plus-minus */
    case '{': return 0xB6;              /* pi, as near as Latin-1 has */
    case '}': return 0xA3;
    case 'y': return '<';
    case 'z': return '>';
    default: return c;
    }
}

static int wide(uint32_t c)
{
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
           (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD);
}

static int zero_width(uint32_t c)
{
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F) ||
           c == 0xFEFF || (c >= 0x20D0 && c <= 0x20FF);
}

/* A character beyond ASCII, in RISC OS's Latin-1 (Acorn's &80-&9F) */
static uint8_t latin1(uint32_t c)
{
    if (c >= 0xA0 && c <= 0xFF)
        return (uint8_t)c;
    switch (c) {
    case 0x20AC: return 0x80;           /* euro */
    case 0x0174: return 0x81;
    case 0x0175: return 0x82;
    case 0x0176: return 0x85;
    case 0x0177: return 0x86;
    case 0x2026: return 0x8C;           /* ellipsis */
    case 0x2122: return 0x8D;           /* trade mark */
    case 0x2030: return 0x8E;           /* per mille */
    case 0x2022: case 0x25CF: case 0x2219: return 0x8F;     /* bullet */
    case 0x2018: return 0x90;
    case 0x2019: return 0x91;
    case 0x2039: return 0x92;
    case 0x203A: return 0x93;
    case 0x201C: return 0x94;
    case 0x201D: return 0x95;
    case 0x201E: return 0x96;
    case 0x2013: return 0x97;           /* en dash */
    case 0x2014: return 0x98;           /* em dash */
    case 0x2212: return 0x99;           /* minus */
    case 0x0152: return 0x9A;
    case 0x0153: return 0x9B;
    case 0x2020: return 0x9C;
    case 0x2021: return 0x9D;
    case 0xFB01: return 0x9E;
    case 0xFB02: return 0x9F;
    }
    if (c >= 0x2500 && c <= 0x257F) {   /* box drawing */
        if (c == 0x2500 || c == 0x2501 || c == 0x2504 || c == 0x2505 || c == 0x2508 || c == 0x2509 ||
            c == 0x254C || c == 0x254D || c == 0x2550)
            return '-';
        if (c == 0x2502 || c == 0x2503 || c == 0x2506 || c == 0x2507 || c == 0x250A || c == 0x250B ||
            c == 0x254E || c == 0x254F || c == 0x2551)
            return '|';
        return '+';
    }
    if (c >= 0x2580 && c <= 0x259F)     /* blocks */
        return '#';
    return '?';
}

static void codepoint(struct term *t, uint32_t c)
{
    if (zero_width(c))
        return;
    if (c < 0x80) {
        int graphics = t->shifted ? t->g1_graphics : t->g0_graphics;
        put(t, graphics ? dec_graphics((uint8_t)c) : (uint8_t)c);
        return;
    }
    if (c < 0xA0)                       /* C1 controls: nothing */
        return;
    put(t, latin1(c));
    if (wide(c))
        put(t, '?');
}

/* ---- escape sequences ---------------------------------------------------------- */

enum { GROUND, ESC, CSI, OSC, OSC_ESC, CHARSET0, CHARSET1, SKIP1 };

static void reply(struct term *t, const char *s)
{
    if (t->pty)
        pty_write(t->pty, s, strlen(s));
}

static int arg(struct term *t, int i, int dflt)
{
    return i < t->np && t->param[i] > 0 ? t->param[i] : dflt;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void move_to(struct term *t, int x, int y)
{
    t->x = clamp(x, 0, t->cols - 1);
    t->y = clamp(y, 0, t->rows - 1);
    t->pending = 0;
    t->synced = 0;
}

static void save_cursor(struct term *t)
{
    t->sx = t->x, t->sy = t->y;
    t->sfg = t->fg, t->sbg = t->bg, t->sbold = t->bold, t->srev = t->reverse;
    t->sg0 = t->g0_graphics;
}

static void restore_cursor(struct term *t)
{
    move_to(t, t->sx, t->sy);
    t->fg = t->sfg, t->bg = t->sbg, t->bold = t->sbold, t->reverse = t->srev;
    t->g0_graphics = t->sg0;
    t->dirty = 1;
}

static void sgr(struct term *t)
{
    if (t->np == 0)
        t->param[t->np++] = 0;
    for (int i = 0; i < t->np; i++) {
        int p = t->param[i] < 0 ? 0 : t->param[i];
        if (p == 0)
            t->fg = t->bg = DEFAULT, t->bold = t->reverse = 0;
        else if (p == 1)
            t->bold = 1;
        else if (p == 22)
            t->bold = 0;
        else if (p == 7)
            t->reverse = 1;
        else if (p == 27)
            t->reverse = 0;
        else if (p >= 30 && p <= 37)
            t->fg = p - 30;
        else if (p == 39)
            t->fg = DEFAULT;
        else if (p >= 40 && p <= 47)
            t->bg = p - 40;
        else if (p == 49)
            t->bg = DEFAULT;
        else if (p >= 90 && p <= 97)
            t->fg = p - 90 + 8;
        else if (p >= 100 && p <= 107)
            t->bg = p - 100 + 8;
        else if ((p == 38 || p == 48) && i + 1 < t->np) {
            int *which = p == 38 ? &t->fg : &t->bg;
            if (t->param[i + 1] == 5 && i + 2 < t->np) {
                *which = clamp(t->param[i + 2], 0, 255);
                i += 2;
            } else if (t->param[i + 1] == 2 && i + 4 < t->np) {
                *which = RGB | clamp(t->param[i + 2], 0, 255) << 16 |
                         clamp(t->param[i + 3], 0, 255) << 8 | clamp(t->param[i + 4], 0, 255);
                i += 4;
            } else {
                i = t->np;
            }
        }
        /* underline, blink, italic and the rest: the VDU has none */
    }
    t->dirty = 1;
}

static void cursor_visible(struct term *t, int on)
{
    t->cursor_on = on;
    emit2(t, 23, 1);
    emit(t, (uint8_t)on);
    for (int k = 0; k < 7; k++)
        emit(t, 0);
}

static void mode(struct term *t, int set)
{
    for (int i = 0; i < t->np; i++) {
        int p = t->param[i];
        if (!t->priv)
            continue;                   /* ANSI modes (insert, LNM): not kept */
        switch (p) {
        case 1: t->cursor_keys_app = set; break;
        case 7: t->autowrap = set; break;
        case 25: cursor_visible(t, set); break;
        case 47: case 1047: case 1049:
            if (set && p == 1049)
                save_cursor(t);
            apply(t);
            erase(t, 0, 0, t->cols - 1, t->rows - 1);
            if (!set && p == 1049)
                restore_cursor(t);
            break;
        }
    }
}

static void csi(struct term *t, uint8_t f)
{
    int n = arg(t, 0, 1);
    switch (f) {
    case 'A': move_to(t, t->x, t->y >= t->top ? (t->y - n < t->top ? t->top : t->y - n) : t->y - n); break;
    case 'B': move_to(t, t->x, t->y <= t->bot ? (t->y + n > t->bot ? t->bot : t->y + n) : t->y + n); break;
    case 'C': case 'a': move_to(t, t->x + n, t->y); break;
    case 'D': move_to(t, t->x - n, t->y); break;
    case 'E': move_to(t, 0, t->y + n); break;
    case 'F': move_to(t, 0, t->y - n); break;
    case 'G': case '`': move_to(t, n - 1, t->y); break;
    case 'd': move_to(t, t->x, n - 1); break;
    case 'H': case 'f': move_to(t, arg(t, 1, 1) - 1, n - 1); break;
    case 'J':
        apply(t);
        switch (arg(t, 0, 0)) {
        case 0:
            erase(t, t->x, t->y, t->cols - 1, t->y);
            erase(t, 0, t->y + 1, t->cols - 1, t->rows - 1);
            break;
        case 1:
            erase(t, 0, 0, t->cols - 1, t->y - 1);
            erase(t, 0, t->y, t->x, t->y);
            break;
        default:
            erase(t, 0, 0, t->cols - 1, t->rows - 1);
            break;
        }
        break;
    case 'K':
        apply(t);
        switch (arg(t, 0, 0)) {
        case 0: erase(t, t->x, t->y, t->cols - 1, t->y); break;
        case 1: erase(t, 0, t->y, t->x, t->y); break;
        default: erase(t, 0, t->y, t->cols - 1, t->y); break;
        }
        break;
    case 'X':
        apply(t);
        erase(t, t->x, t->y, clamp(t->x + n - 1, 0, t->cols - 1), t->y);
        break;
    case 'L': case 'M':                 /* insert, delete lines: within the region */
        if (t->y >= t->top && t->y <= t->bot) {
            apply(t);
            int k = clamp(n, 1, t->bot - t->y + 1);
            if (k == t->bot - t->y + 1)
                erase(t, 0, t->y, t->cols - 1, t->bot);
            else
                scroll(t, 0, t->y, t->cols - 1, t->bot, f == 'L' ? 2 : 3, k);
            move_to(t, 0, t->y);
        }
        break;
    case '@': case 'P': {               /* insert, delete characters */
        apply(t);
        int k = clamp(n, 1, t->cols - t->x);
        if (k == t->cols - t->x)
            erase(t, t->x, t->y, t->cols - 1, t->y);
        else
            scroll(t, t->x, t->y, t->cols - 1, t->y, f == '@' ? 0 : 1, k);
        t->pending = 0;
        break;
    }
    case 'S': apply(t); scroll(t, 0, t->top, t->cols - 1, t->bot, 3, clamp(n, 1, t->rows)); break;
    case 'T': apply(t); scroll(t, 0, t->top, t->cols - 1, t->bot, 2, clamp(n, 1, t->rows)); break;
    case 'm': sgr(t); break;
    case 'r': {
        int top = arg(t, 0, 1) - 1, bot = arg(t, 1, t->rows) - 1;
        if (top < bot && bot < t->rows) {
            t->top = top, t->bot = bot;
            move_to(t, 0, 0);
        }
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    case 'h': mode(t, 1); break;
    case 'l': mode(t, 0); break;
    case 'n':
        if (arg(t, 0, 0) == 6) {
            char b[32];
            snprintf(b, sizeof b, "\033[%d;%dR", t->y + 1, t->x + 1);
            reply(t, b);
        } else if (arg(t, 0, 0) == 5) {
            reply(t, "\033[0n");
        }
        break;
    case 'c':
        if (!t->priv && arg(t, 0, 0) == 0)
            reply(t, "\033[?1;2c");     /* a VT100 with the advanced video option */
        break;
    }
}

static void reset(struct term *t)
{
    t->top = 0, t->bot = t->rows - 1;
    t->fg = t->bg = DEFAULT, t->bold = t->reverse = 0, t->dirty = 1;
    t->autowrap = 1, t->cursor_keys_app = 0;
    t->g0_graphics = t->g1_graphics = t->shifted = 0;
    t->state = GROUND;
    save_cursor(t);
}

static void control(struct term *t, uint8_t c)
{
    switch (c) {
    case 7: emit(t, 7); break;
    case 8:
        if (t->x > 0)
            t->x--;
        t->pending = 0, t->synced = 0;
        break;
    case 9: move_to(t, (t->x / 8 + 1) * 8, t->y); break;
    case 10: case 11: case 12: linefeed(t); break;
    case 13: t->x = 0, t->pending = 0, t->synced = 0; break;
    case 14: t->shifted = 1; break;
    case 15: t->shifted = 0; break;
    }
}

static void byte(struct term *t, uint8_t b)
{
    switch (t->state) {
    case ESC:
        t->state = GROUND;
        switch (b) {
        case '[': t->state = CSI, t->np = 0, t->priv = 0, t->inter = 0, t->param[0] = -1; return;
        case ']': t->state = OSC; return;
        case '(': t->state = CHARSET0; return;
        case ')': t->state = CHARSET1; return;
        case '#': case '%': case '*': case '+': t->state = SKIP1; return;
        case '7': save_cursor(t); return;
        case '8': restore_cursor(t); return;
        case 'D': linefeed(t); return;
        case 'E': t->x = 0; linefeed(t); return;
        case 'M': reverse_index(t); return;
        case 'c':
            reset(t);
            apply(t);
            erase(t, 0, 0, t->cols - 1, t->rows - 1);
            move_to(t, 0, 0);
            return;
        default: return;                /* ESC = and ESC >, the keypad: as it is */
        }
    case CSI:
        if (b >= '0' && b <= '9') {
            if (t->np == 0)
                t->np = 1, t->param[0] = 0;
            if (t->param[t->np - 1] < 0)
                t->param[t->np - 1] = 0;
            if (t->param[t->np - 1] < 10000)
                t->param[t->np - 1] = t->param[t->np - 1] * 10 + (b - '0');
        } else if (b == ';' || b == ':') {
            if (t->np == 0)
                t->np = 1, t->param[0] = -1;
            if (t->np < 16)
                t->param[t->np++] = -1;
        } else if (b >= '<' && b <= '?') {
            t->priv = b;
        } else if (b >= 0x20 && b <= 0x2F) {
            t->inter = b;
        } else if (b >= 0x40 && b <= 0x7E) {
            t->state = GROUND;
            if (!t->inter)
                csi(t, b);
        } else if (b < 0x20) {
            control(t, b);              /* a control inside one acts at once */
        } else {
            t->state = GROUND;
        }
        return;
    case OSC:                           /* a window title: there is no window */
        if (b == 7)
            t->state = GROUND;
        else if (b == 27)
            t->state = OSC_ESC;
        return;
    case OSC_ESC:
        t->state = b == '\\' ? GROUND : OSC;
        return;
    case CHARSET0:
    case CHARSET1:
        *(t->state == CHARSET0 ? &t->g0_graphics : &t->g1_graphics) = b == '0';
        t->state = GROUND;
        return;
    case SKIP1:
        t->state = GROUND;
        return;
    }
    /* GROUND */
    if (t->need) {
        if ((b & 0xC0) == 0x80) {
            t->cp = t->cp << 6 | (b & 0x3F);
            if (--t->need == 0) {
                /* An overlong form of an ASCII character, E0 80 87 for BEL
                 * say, is not one, and must not reach the VDU as a control code */
                if (t->cp < 0x80)
                    put(t, '?');
                else
                    codepoint(t, t->cp);
            }
            return;
        }
        t->need = 0;
        put(t, '?');                    /* a sequence cut short */
    }
    if (b == 27)
        t->state = ESC;
    else if (b < 0x20)
        control(t, b);
    else if (b < 0x7F)
        codepoint(t, b);
    else if (b >= 0xC2 && b <= 0xDF)
        t->cp = b & 0x1F, t->need = 1;
    else if (b >= 0xE0 && b <= 0xEF)
        t->cp = b & 0x0F, t->need = 2;
    else if (b >= 0xF0 && b <= 0xF4)
        t->cp = b & 0x07, t->need = 3;
    else if (b != 0x7F)
        put(t, '?');
}

/* ---- beginning and ending ----------------------------------------------------- */

static void begin(struct term *t)
{
    memset(t, 0, sizeof *t);
    uint32_t *blk = ros_rma_alloc(9 * 4);    /* five in, four out */
    blk[0] = 132, blk[1] = 133, blk[2] = 134, blk[3] = 135, blk[4] = 0xFFFFFFFFu;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(blk), c.r[1] = ros_addr(blk + 5);
    ros_swi(&c, XOS_ReadVduVariables);
    int l = (int)blk[5], b = (int)blk[6], r = (int)blk[7], top = (int)blk[8];
    ros_rma_free(blk);
    t->L = l, t->T = top, t->cols = r - l + 1, t->rows = b - top + 1;
    if (t->cols < 1)
        t->cols = 1;
    if (t->rows < 1)
        t->rows = 1;

    ros_cpu_enter(&c);                  /* POS and VPOS: where to start */
    c.r[0] = 134;
    ros_swi(&c, XOS_Byte);
    t->x = clamp((int)c.r[1], 0, t->cols - 1), t->y = clamp((int)c.r[2], 0, t->rows - 1);

    t->orig_fg = swi(XOS_SETCOLOUR_N, 0xC0u, 0, 0, 0);
    t->orig_bg = swi(XOS_SETCOLOUR_N, 0xD0u, 0, 0, 0);
    t->cursor_on = 1;
    reset(t);
    t->dirty = 0;                       /* the colours are the defaults already */
    emit2(t, 23, 16);                   /* C81Bit: the deferred wrap */
    emit2(t, 1, 0xFE);
    for (int k = 0; k < 6; k++)
        emit(t, 0);
    t->synced = 0;
}

static void end(struct term *t)
{
    if (t->stream) {
        flush(t);
        return;
    }
    t->fg = t->bg = DEFAULT, t->bold = t->reverse = 0, t->dirty = 1;
    apply(t);
    t->synced = 0;                      /* VDU 31 first: it drops a waiting newline, */
    move_vdu(t);                            /* where clearing C81Bit would do it */
    emit2(t, 23, 16);
    emit2(t, 0, 0xFE);
    for (int k = 0; k < 6; k++)
        emit(t, 0);
    if (!t->cursor_on)
        cursor_visible(t, 1);
    if (t->pending || t->x > 0)         /* the next line for what follows */
        emit2(t, 13, 10);
    flush(t);
}

/* A byte of the program's, in a task window. Text goes as it comes, with CR,
 * LF, tab and backspace. Escape sequences are left out: ESC [ ... final,
 * ESC ] ... BEL or ESC \, and ESC with one more byte. UTF-8 becomes Latin-1
 * where it can */
static void stream_byte(struct term *t, uint8_t b)
{
    switch (t->sstate) {
    case 1:                                     /* after ESC */
        t->sstate = b == '[' ? 2 : b == ']' ? 3 : 0;
        return;
    case 2:                                     /* CSI, to its final byte */
        if (b >= 0x40 && b <= 0x7E)
            t->sstate = 0;
        return;
    case 3:                                     /* OSC, to BEL or ST */
        if (b == 7)
            t->sstate = 0;
        else if (b == 27)
            t->sstate = 1;
        return;
    default:
        break;
    }
    if (b == 27) {
        t->sstate = 1;
    } else if (b >= 0x80 && b < 0xC0) {         /* a continuation */
        if (t->lead) {
            uint32_t cp = (uint32_t)(t->lead & 0x1F) << 6 | (b & 0x3F);
            emit(t, cp >= 0xA0 && cp <= 0xFF ? (uint8_t)cp : '?');
            t->lead = 0;
        }
    } else if (b >= 0xC0) {                     /* a lead: two bytes may be Latin-1 */
        t->lead = b < 0xE0 ? b : 0;
        if (!t->lead)
            emit(t, '?');
    } else if (b >= 32 || b == 13 || b == 10 || b == 9 || b == 8) {
        t->lead = 0;
        emit(t, b);
    }
}

/* In a task window: TaskWindow_TaskInfo 0 */
static int in_task_window(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0;
    ros_swi(&c, XTaskWindow_TaskInfo);
    return !c.v && c.r[0] != 0;
}

void pty_term_replay(const void *bytes, size_t n)
{
    static struct term t;
    begin(&t);
    for (size_t i = 0; i < n; i++)
        byte(&t, ((const uint8_t *)bytes)[i]);
    end(&t);
}

/* ---- the keyboard --------------------------------------------------------------- */

/* A key, if one has come: OS_Byte 129 with no wait */
static int inkey(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 129, c.r[1] = 0, c.r[2] = 0;
    ros_swi(&c, XOS_Byte);
    return c.v || c.r[2] != 0 ? -1 : (int)(c.r[1] & 0xFF);
}

static void send_key(struct term *t, int k)
{
    uint8_t b[4];
    size_t n = 0;
    if (k == 8)                         /* Backspace: the terminal's erase, DEL */
        k = 0x7F;
    if (k >= 0x8C && k <= 0x8F) {       /* RISC OS's cursor keys: left right down up */
        b[n++] = 27, b[n++] = t->cursor_keys_app ? 'O' : '[', b[n++] = (uint8_t)("DCBA"[k - 0x8C]);
        t->kstate = 0;
    } else if (t->kstate == 1 && k == '[' && t->cursor_keys_app) {
        t->kstate = 2;                  /* ESC [ held back: a cursor key's? */
        return;
    } else if (t->kstate == 2) {
        t->kstate = 0;
        b[n++] = 27;
        b[n++] = (k >= 'A' && k <= 'D') ? 'O' : '[';
        b[n++] = (uint8_t)k;
    } else {
        t->kstate = k == 27 ? 1 : 0;
        b[n++] = (uint8_t)k;
    }
    pty_write(t->pty, b, n);
}

/* An OS_Byte variable set for the session. Its old value is put back after.
 * Returns -1 if the OS does not know the variable */
static int byte_set(uint32_t n, uint32_t v)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = n, c.r[1] = v, c.r[2] = 0;
    ros_swi(&c, XOS_Byte);
    return c.v ? -1 : (int)(c.r[1] & 0xFF);
}

/* ---- the terminal ---------------------------------------------------------------- */

/* One byte of the program's output to the terminal */
static void out_byte(struct term *t, uint8_t b)
{
    if (t->stream)
        stream_byte(t, b);
    else
        byte(t, b);
}

/* With a line to drop (pty_terminal_dropping), the output goes a line at a
 * time. Each line that does not contain it is passed on whole */
static char drop_line[1024];
static size_t drop_n;

static void drop_flush(struct term *t, const char *drop)
{
    drop_line[drop_n] = 0;
    if (!strstr(drop_line, drop))
        for (size_t i = 0; i < drop_n; i++)
            out_byte(t, (uint8_t)drop_line[i]);
    drop_n = 0;
}

os_error *pty_terminal(const char *cmdline, int *status)
{
    return pty_terminal_dropping(cmdline, NULL, status);
}

os_error *pty_terminal_dropping(const char *cmdline, const char *drop, int *status)
{
    static struct term t;
    drop_n = 0;
    begin(&t);
    if (in_task_window()) {
        t.n = 0;                        /* no VDU state to set: a stream */
        t.stream = 1;
    }
    flush(&t);
    struct pty *p;
    os_error *e = pty_open(cmdline, t.cols, t.rows, &p);
    if (e) {
        end(&t);
        return e;
    }
    t.pty = p;
    /* The keys as the program wants them. Escape is a character (OS_Byte
     * 229), not RISC OS's Escape. The cursor keys are codes (OS_Byte 4,2:
     * &8C-&8F), not the copy cursor */
    int old229 = byte_set(229, 1), old4 = byte_set(4, 2);
    uint8_t buf[4096];
    os_error *err = NULL;
    for (;;) {
        int k, keys = 0;
        while ((k = inkey()) >= 0)
            send_key(&t, k), keys++;
        if (t.kstate == 2) {            /* ESC [ alone, after all */
            t.kstate = 0;
            pty_write(p, "\033[", 2);
        }
        long got = pty_read(p, buf, sizeof buf);
        for (long i = 0; i < got; i++) {
            if (!drop) {
                out_byte(&t, buf[i]);
                continue;
            }
            drop_line[drop_n++] = (char)buf[i];
            if (buf[i] == '\n' || drop_n == sizeof drop_line - 1)
                drop_flush(&t, drop);
        }
        flush(&t);
        if (got || keys)
            continue;
        if (pty_ended(p, NULL))
            break;
        /* Nothing yet. Sleep until the program writes, or until 40 cs pass for
         * the keys (the console's come through the keyboard: inkey), so that a
         * task window's desktop goes on meanwhile (runtime/sleep.c). A
         * program that has ended, with its last words not yet read, is given
         * time rather than watched. */
        struct pollfd h = { .fd = pty_fd(p), .events = POLLIN };
        poll(&h, 1, 0);
        int gone = (h.revents & (POLLHUP | POLLERR)) != 0;
        os_error *slept;
        if (ros_sleep_fd(gone ? -1 : pty_fd(p), POLLIN, gone ? 20 : 40, &slept) < 0) {
            err = slept;                /* the task window is dying */
            break;
        }
    }
    if (drop && drop_n)
        drop_flush(&t, drop);
    int st = pty_close(p);
    if (old4 >= 0)
        byte_set(4, (uint32_t)old4);
    if (old229 >= 0)
        byte_set(229, (uint32_t)old229);
    t.pty = NULL;
    end(&t);
    if (status)
        *status = st;
    return err;
}
