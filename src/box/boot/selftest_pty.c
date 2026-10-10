/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_pty.c: the PTY module (modules/pty). It covers OS_Byte 129,
 * which its terminal reads keys with; the terminal's VT100, drawn with VDU
 * codes in a text window and read back from the screen with OS_Byte 135;
 * the SWIs, running OpenSSH's ssh; and *SSH-KeyGen, making a key.
 *
 * ssh and ssh-keygen are the box's (/usr/bin, from the initramfs) or the
 * host's when hosted. The key goes in TMPDIR (or the box's /tmp) and is
 * removed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pty.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check

#define XPTY(n) (0x20000u | (0xC00C0u + (n)))

/* The window: 20 columns, 6 rows, at column 2, row 4 */
#define WL 2
#define WT 4
#define WC 20
#define WR 6

static void vdu(const char *s, size_t n)
{
    xos_write_n((const uint8_t *)s, (uint32_t)n);
}

#define VDU(lit) vdu(lit, sizeof lit - 1)

static int byte(uint32_t a, uint32_t x, uint32_t y, uint32_t out[3])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = a, s.r[1] = x, s.r[2] = y;
    ros_swi(&s, XOS_Byte);
    out[0] = s.r[1], out[1] = s.r[2], out[2] = s.c;
    return !s.v;
}

/* The character at (x, y) of the current text window */
static char at(int x, int y)
{
    char tab[3] = { 31, (char)x, (char)y };
    vdu(tab, 3);
    uint32_t r[3];
    byte(135, 0, 0, r);
    return (char)r[0];
}

/* A row of the window as the screen has it, trailing spaces kept */
static void row(int y, char *out)
{
    for (int x = 0; x < WC; x++)
        out[x] = at(x, y);
    out[WC] = 0;
}

static void play(const char *s)
{
    pty_term_replay(s, strlen(s));
}

static int row_is(int y, const char *want)
{
    char got[WC + 1], w[WC + 1];
    row(y, got);
    snprintf(w, sizeof w, "%-*s", WC, want);
    return memcmp(got, w, WC) == 0;
}

static void terminal(void)
{
    /* the cursor off, as it would be drawn over the cell OS_Byte 135 reads;
     * markers either side of the window, which nothing inside may touch */
    VDU("\x17\x01\x00\x00\x00\x00\x00\x00\x00\x00");
    VDU("\x1A\x1F\x01\x04#\x1F\x16\x04#\x1F\x01\x09#\x1F\x16\x09#");
    char w[] = { 28, WL, WT + WR - 1, WL + WC - 1, WT, 12 };
    vdu(w, sizeof w);

    play("hello\r\nworld");
    check(row_is(0, "hello") && row_is(1, "world"), "PTY terminal -- text, CR LF", "'%c%c'",
          at(0, 0), at(0, 1));

    play("\033[2J\033[3;5HX\033[1;20HZ");
    check(at(4, 2) == 'X' && at(19, 0) == 'Z' && at(0, 0) == ' ',
          "PTY terminal -- CSI H addresses the cell; CSI 2J clears", "'%c' '%c'", at(4, 2), at(19, 0));

    play("\033[2J\033[1;1Haaaaaaaaaaaaaaaaaaaab");
    check(at(19, 0) == 'a' && at(0, 1) == 'b' && at(0, 0) == 'a',
          "PTY terminal -- the last column's newline waits for the next character", "'%c' '%c'",
          at(19, 0), at(0, 1));

    play("\033[2J\033[1;1Htop\033[6;1Hbottom\r\n");
    check(row_is(0, "") && row_is(4, "bottom") && row_is(5, ""),
          "PTY terminal -- a newline at the foot scrolls the window", "'%c' '%c'", at(0, 0), at(0, 4));

    play("\033[2J\033[1;1HA\033[2;1HB\033[3;1HC\033[4;1HD\033[2;3r\033[3;1H\n\033[r");
    check(row_is(0, "A") && row_is(1, "C") && row_is(2, "") && row_is(3, "D"),
          "PTY terminal -- a scroll region (CSI r) scrolls alone", "'%c%c%c%c'", at(0, 0), at(0, 1),
          at(0, 2), at(0, 3));

    play("\033[2J\033[1;1HA\033[2;1HB\033[3;1HC\033[2;1H\033[L");
    int il = row_is(0, "A") && row_is(1, "") && row_is(2, "B") && row_is(3, "C");
    play("\033[2;1H\033[2M");
    check(il && row_is(0, "A") && row_is(1, "C") && row_is(2, ""),
          "PTY terminal -- insert and delete lines (CSI L, M)", "'%c%c%c'", at(0, 0), at(0, 1), at(0, 2));

    play("\033[2J\033[1;1Habcdef\033[1;2H\033[2P\033[2;1Hxyz\033[2;2H\033[K\033[3;1Hab\033[3;1H\033[2@");
    check(row_is(0, "adef") && row_is(1, "x") && row_is(2, "  ab"),
          "PTY terminal -- delete and insert characters, erase to the line's end", "'%c%c%c'",
          at(1, 0), at(1, 1), at(2, 2));

    play("\033[2J\033[1;1H\xE2\x80\x9Chi\xE2\x80\x9D \xE2\x94\x80\xE2\x94\x82 \xC3\xA9 \xE4\xB8\xAD|"
         "\033(0\033[2;1Hlqk\033(B");
    check((uint8_t)at(0, 0) == 0x94 && at(1, 0) == 'h' && (uint8_t)at(3, 0) == 0x95 && at(5, 0) == '-' &&
              at(6, 0) == '|' && (uint8_t)at(8, 0) == 0xE9 && at(10, 0) == '?' && at(11, 0) == '?' &&
              at(12, 0) == '|' && row_is(1, "+-+"),
          "PTY terminal -- UTF-8 in RISC OS Latin-1, a wide character in two, DEC line drawing",
          "&%02X '%c' &%02X '%c%c'", (uint8_t)at(0, 0), at(1, 0), (uint8_t)at(8, 0), at(10, 0), at(12, 0));

    /* an overlong UTF-8 form of BEL is ? and not the control code itself */
    play("\033[2J\033[1;1Ha\xE0\x80\x87z");
    check(row_is(0, "a?z"), "PTY terminal -- an overlong UTF-8 control character is drawn as ?", "'%c%c%c'",
          at(0, 0), at(1, 0), at(2, 0));

    play("\033[2J\033[1;1H\033[1;31mred\033[0m \033]0;title\007ok");
    check(row_is(0, "red ok"), "PTY terminal -- SGR colours and a window title leave the text",
          "'%c%c'", at(4, 0), at(5, 0));

    play("\033[2J");
    VDU("\x1A");
    check(at(1, 4) == '#' && at(22, 4) == '#' && at(1, 9) == '#' && at(22, 9) == '#',
          "PTY terminal -- nothing outside the text window touched", "'%c%c%c%c'", at(1, 4),
          at(22, 4), at(1, 9), at(22, 9));
    VDU("\x0C\x17\x01\x01\x00\x00\x00\x00\x00\x00\x00");
}

static int pty(uint32_t n, uint32_t r[4])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 4 * sizeof r[0]);
    ros_swi(&s, XPTY(n));
    memcpy(r, s.r, 4 * sizeof r[0]);
    return s.v ? (int)((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static int cli(const char *cmd)
{
    size_t n = strlen(cmd);
    char *c = ros_rma_alloc((uint32_t)n + 1);
    memcpy(c, cmd, n);
    c[n] = '\r';
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(c);
    ros_swi(&s, XOS_CLI);
    ros_rma_free(c);
    return s.v ? (int)((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static void swis(void)
{
    char *line = ros_rma_alloc(64), *buf = ros_rma_alloc(512);
    strcpy(line, "ssh -V");
    uint32_t r[4] = { 0, ros_addr(line), 80, 24 };
    int e = pty(0, r);
    uint32_t h = r[0];
    size_t got = 0;
    int ended = 0, status = -1;
    for (int i = 0; !e && i < 500 && !ended; i++) {
        r[0] = h, r[1] = ros_addr(buf + got), r[2] = (uint32_t)(511 - got);
        pty(1, r);
        got += r[3];
        r[0] = h;
        pty(3, r);
        ended = r[1] == 1, status = (int)r[2];
        if (!ended && !r[3])
            usleep(10000);
    }
    buf[got] = 0;
    check(!e && ended && status == 0 && strstr(buf, "OpenSSH_"),
          "PTY_Open, _Read, _Status -- ssh -V on a pseudo-terminal", "error &%X, ended %d, status %d, "
          "\"%.40s\"", e, ended, status, buf);
    r[0] = h;
    int ce = e ? -1 : pty(5, r);
    r[0] = h;
    int bad = pty(3, r);
    check(ce == 0 && bad == (int)PTY_ERR_BAD_HANDLE, "PTY_Close frees the handle; a freed one is bad",
          "close &%X, then &%X", ce, bad);

    strcpy(line, "no-such-program-here");
    r[0] = 0, r[1] = ros_addr(line), r[2] = 80, r[3] = 24;
    check(pty(0, r) == (int)PTY_ERR_CANNOT_RUN, "PTY_Open -- a program that is not there is an error",
          "");
    ros_rma_free(line);
    ros_rma_free(buf);
}

static void keygen(void)
{
    const char *tmp = getpid() == 1 ? "/tmp" : getenv("TMPDIR");
    char key[256], pub[260], cmd[400];
    snprintf(key, sizeof key, "%s/rosgd-pty-key-%d", tmp ? tmp : "/tmp", (int)getpid());
    snprintf(pub, sizeof pub, "%s.pub", key);
    unlink(key), unlink(pub);
    snprintf(cmd, sizeof cmd, "SSH-KeyGen -q -t ed25519 -N \"\" -C rosgd-selftest -f %s", key);
    int e = cli(cmd);
    char line[200] = "";
    FILE *f = fopen(pub, "r");
    if (f) {
        if (!fgets(line, sizeof line, f))
            line[0] = 0;
        fclose(f);
    }
    check(!e && strncmp(line, "ssh-ed25519 ", 12) == 0 && strstr(line, "rosgd-selftest") &&
              access(key, R_OK) == 0,
          "*SSH-KeyGen -- an Ed25519 key pair from OpenSSH's ssh-keygen", "error &%X, \"%.40s\"", e, line);
    unlink(key), unlink(pub);
}

void ros_selftest_pty(void)
{
    uint32_t r[3];
    byte(129, 0, 0xFF, r);
    int id = r[0] == 0xAA;
    byte(129, 0x80, 0xFF, r);           /* INKEY(-128), Shift: not down */
    check(id && r[0] == 0 && r[1] == 0, "OS_Byte 129 -- INKEY(-256) is RISC OS 5's &AA; no key is down",
          "&%X", r[0]);
    byte(129, 1, 0, r);                 /* no one types: it times out */
    check(r[1] == 0xFF && r[2] == 1, "OS_Byte 129 -- INKEY(1) times out: R2 &FF, C set",
          "R2 &%X, C %u", r[1], r[2]);
    terminal();
    swis();
    keygen();
}
