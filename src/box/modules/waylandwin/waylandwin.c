/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* waylandwin.c -- WaylandWindows: Linux programs' windows as Wimp windows.
 *
 * In a box booted with rosgd.display=compositor, ROSGD's compositor
 * (ports/compositor) shows the screen, and Linux programs open Wayland
 * windows on it. This task is their window manager's RISC OS half. It
 * connects to the compositor's socket, /dev/rosgd-wm. For each Linux
 * window (a toplevel) that the compositor tells it of, it makes a Wimp
 * window with a title bar, a close icon, a back icon, a toggle size icon
 * and an adjust size icon. The work area is the toplevel's size. It binds
 * the window as an external surface (Wimp_Extend &5200, flags bit 1,
 * surface.c). The Wimp draws the work area black. It puts where the window
 * is, and what of it is seen, in the screen block for the compositor. The
 * compositor shows the Linux window there and the desktop over it
 * everywhere else. So windows in front, menus and the pointer stay in
 * front, and the Wimp moves, stacks and resizes the Linux window as it
 * does any other.
 *
 * Input stays RISC OS's. This task passes on to the compositor what the
 * Wimp gives it. That is the pointer while it is over a window
 * (Wimp_GetPointerInfo at each null event) and while a button is held
 * after a click in one. It is the buttons (Select as the left, Menu the
 * right, Adjust the middle). It is the keys pressed while a window has the
 * caret, which a click gives it (an invisible caret). It is also the scroll
 * wheel (extended scroll requests). A new size from the window's adjust
 * size icon is sent as a configure. A size that the program chooses itself
 * opens the window at that size. The close icon asks the program to close.
 * The window goes when the program's does.
 *
 * A new window is fitted to the screen, with its title bar below the top
 * and its foot above the icon bar. A program bigger than that is told the
 * size it has.
 *
 * A browser's window (rosgd-browser, ports/browser; !Browser) is known by
 * the control socket its program makes under its process id, which the
 * compositor reports. Everything such a window has beyond the page is
 * browser.c's. That is its toolbar, its menus, the icon bar's icon and the
 * files the desktop hands it. The page starts below the toolbar
 * (SurfaceBind's R4). The two halves share wwin.h.
 *
 * The module starts the compositor too, when it is initialised in a box
 * booted with rosgd.display=compositor. /usr/bin/wperun (in the ROM)
 * mounts the WPE edition's root and runs rosgd-compositor in it, detached,
 * with the box's settings (-b). rosgd.compositor=PATH names another
 * program in that root (a test's own build), and =none starts none.
 *
 * The socket's lines are listed in ports/compositor/rosgd-compositor.c,
 * under "the Wimp as window manager". If there is no compositor, the task
 * waits and tries the socket once a second.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waylandwin.h"
#include "wwin.h"
#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

static struct ws *workspace(struct ros_module *m)
{
    uint32_t pw = ros_ld32(m->private_word);
    return (int32_t)pw > 0 ? ros_ptr(pw) : NULL;
}

static void task_end(struct ros_module *m, struct ws *w);

/* The command line, built in the workspace: starting a program, opening
 * a directory (browser.c's) */
void wwin_cli(struct ws *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->cli, sizeof w->cli, fmt, ap);
    va_end(ap);
    uint32_t r[8] = { A(w->cli) };
    swi(XOS_CLI, r);
}

/* ---- the mode ---------------------------------------------------------------- */

static uint32_t mode_var(uint32_t var, uint32_t dflt)
{
    uint32_t r[8] = { 0xFFFFFFFFu, var };
    return swi(XOS_ReadModeVariable, r) ? dflt : r[2];
}

static void read_mode(struct ws *w)
{
    w->xeig = mode_var(4, 1);
    w->yeig = mode_var(5, 1);
    w->screen_w = (int32_t)((mode_var(11, 1279) + 1) << w->xeig);
    w->screen_h = (int32_t)((mode_var(12, 1023) + 1) << w->yeig);
    w->mode_changed = 0;
}

/* ---- the socket ---------------------------------------------------------------- */

static void drop(struct ws *w);

void wwin_say(struct ws *w, const char *fmt, ...)
{
    if (w->fd < 0)
        return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    if (send(w->fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno != EAGAIN)
        drop(w);
}

static void try_connect(struct ws *w)
{
    uint32_t t = now();
    if ((int32_t)(t - w->next_try) < 0)
        return;
    w->next_try = t + 100;
    int fd = wwin_unix_socket();
    if (fd < 0)
        return;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "/dev/rosgd-wm");
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return;
    }
    w->fd = fd;
    w->inlen = 0;
}

/* ---- the windows ---------------------------------------------------------------- */

struct xwin *wwin_by_id(struct ws *w, uint32_t id)
{
    for (int i = 0; i < WINDOWS; i++)
        if (w->win[i].id && w->win[i].id == id)
            return &w->win[i];
    return NULL;
}

struct xwin *wwin_by_handle(struct ws *w, uint32_t handle)
{
    for (int i = 0; i < WINDOWS; i++)
        if (w->win[i].id && w->win[i].handle == handle)
            return &w->win[i];
    return NULL;
}

struct xwin *wwin_by_bar(struct ws *w, uint32_t handle)
{
    for (int i = 0; i < WINDOWS; i++)
        if (w->win[i].id && w->win[i].bar && w->win[i].bar == handle)
            return &w->win[i];
    return NULL;
}

/* the extent: the screen, or the window if bigger, so that the adjust
 * size icon can make it any size up to the screen's */
static void extent(struct ws *w, struct xwin *x, int32_t wd, int32_t ht)
{
    uint32_t r[8] = { x->handle, A(w->block) };
    int32_t *b = (int32_t *)w->block;
    b[0] = 0;
    b[1] = -(ht > w->screen_h ? ht : w->screen_h);
    b[2] = wd > w->screen_w ? wd : w->screen_w;
    b[3] = 0;
    swi(XWimp_SetExtent, r);
}

/* A client's window size comes over the line channel. It is shifted left by the
 * mode's pixel size to give OS units, so it is limited to keep that in 32 bits. */
#define MAX_WINDOW_PIXELS 16384

static void window_new(struct ws *w, uint32_t id, int32_t pw, int32_t ph, int pid, const char *title)
{
    if (wwin_by_id(w, id) || pw <= 0 || ph <= 0 || pw > MAX_WINDOW_PIXELS || ph > MAX_WINDOW_PIXELS)
        return;
    struct xwin *x = NULL;
    for (int i = 0; i < WINDOWS && !x; i++)
        if (!w->win[i].id)
            x = &w->win[i];
    if (!x) {
        /* The compositor has made the window and its program is running.
         * Dropping it in silence leaves that program alive and unreachable.
         * The program is asked to close, and the reader is told once. A
         * message for every window would be a wall of error boxes. */
        wwin_say(w, "close %u", id);
        if (!w->told_full) {
            w->told_full = 1;
            os_error *e = ros_error(0xC6, "No room for another window: %d are open",
                                    WINDOWS);
            uint32_t r[8] = { A(e), 1, A(w->task_name) };
            swi(XWimp_ReportError, r);
        }
        return;
    }
    memset(x, 0, sizeof *x);
    x->bfd = -1;
    snprintf(x->title, sizeof x->title, "%s", title);
    if (browser_connect(x, pid) == 0)
        x->inset = TOOLBAR;
    /* placed down and right of the last, and fitted to the screen: its
     * title bar below the top, its foot above the icon bar; a program
     * bigger than that is told the size it has (configure) */
    int32_t wd = pw << w->xeig, ht = (ph << w->yeig) + x->inset;
    int32_t x0 = 64 + 48 * w->cascade, y1 = w->screen_h - 64 - 48 * w->cascade;
    w->cascade = (w->cascade + 1) % 6;
    int32_t foot = 140;                          /* the icon bar */
    int fitted = 0;
    if (wd > w->screen_w - x0 - 16)
        wd = w->screen_w - x0 - 16, fitted = 1;
    if (ht > y1 - foot)
        ht = y1 - foot, fitted = 1;

    int32_t *b = (int32_t *)w->block;
    uint8_t *c = (uint8_t *)w->block;
    memset(w->block, 0, sizeof w->block);
    b[0] = x0, b[1] = y1 - ht, b[2] = x0 + wd, b[3] = y1;
    b[4] = 0, b[5] = 0;
    b[6] = -1;                                   /* at the front */
    b[7] = (int32_t)0xAF000002u;                 /* moveable; back, close, title,
                                                    toggle size, adjust size */
    if (x->inset)
        b[7] |= 1 << 28;                         /* a browser: the page's scroll bar,
                                                    and the size icon with it */
    c[32] = 7, c[33] = 2, c[34] = 7, c[35] = 7;  /* title, work area: black */
    c[36] = 3, c[37] = 1, c[38] = 12;
    c[39] = 2;                                   /* extended scroll requests: the wheel */
    b[10] = 0;
    b[11] = x->inset ? -ht : -(ht > w->screen_h ? ht : w->screen_h);
    b[12] = wd > w->screen_w ? wd : w->screen_w;
    b[13] = 0;
    b[14] = 0x27000119;                          /* the title: text, centred, indirected */
    b[15] = 3 << 12;                             /* the work area: click */
    b[16] = 1;                                   /* the Wimp's sprites */
    b[17] = 0;                                   /* minimum size: the title's */
    b[18] = (int32_t)A(x->title), b[19] = -1, b[20] = TITLE_LEN;
    b[21] = 0;                                   /* no icons */
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_CreateWindow, r))
        return;
    x->handle = r[0];
    x->id = id;
    x->cfg_w = pw, x->cfg_h = ph;
    uint32_t e[8] = { SURFACE_BIND, x->handle, SURFACE_EXTERNAL, id, (uint32_t)x->inset, 0 };
    swi(XWimp_Extend, e);
    if (x->inset)
        browser_bar_create(w, x);
    b[0] = (int32_t)x->handle;
    b[1] = x0, b[2] = y1 - ht, b[3] = x0 + wd, b[4] = y1;
    b[5] = 0, b[6] = 0, b[7] = -1;
    uint32_t o[8] = { 0, A(w->block) };
    swi(XWimp_OpenWindow, o);
    browser_bar_open(w, x);
    if (fitted) {
        x->cfg_w = wd >> w->xeig, x->cfg_h = (ht - x->inset) >> w->yeig;
        wwin_say(w, "configure %u %d %d", x->id, x->cfg_w, x->cfg_h);
    }
    if (x->bfd >= 0) {
        browser_started(w);                      /* its icon belongs on the bar */
        browser_tell(x, "state");
    }
}

static void window_gone(struct ws *w, struct xwin *x)
{
    uint32_t r[8] = { 0, A(w->block) };
    browser_closed(w, x);
    if (x->bar) {
        w->block[0] = x->bar;
        swi(XWimp_DeleteWindow, r);
        x->bar = 0;
    }
    w->block[0] = x->handle;
    swi(XWimp_DeleteWindow, r);
    if (w->inside == x->handle)
        w->inside = 0;
    if (w->held_window == x->handle)
        w->held = 0, w->held_window = 0;
    if (w->caret == x->handle)
        w->caret = 0;
    x->id = 0;
    w->told_full = 0;                            /* there is room again */
}

static void window_title(struct ws *w, struct xwin *x, const char *title)
{
    (void)w;
    snprintf(x->title, sizeof x->title, "%s", title);
    uint32_t r[8] = { x->handle, TASK_WORD, 3 };      /* the title bar */
    swi(XWimp_ForceRedraw, r);
}

/* the program chose a size: the window opened at it, its top left kept */
static void window_size(struct ws *w, struct xwin *x, int32_t pw, int32_t ph)
{
    if (pw <= 0 || ph <= 0 || pw > MAX_WINDOW_PIXELS || ph > MAX_WINDOW_PIXELS ||
        (pw == x->cfg_w && ph == x->cfg_h))
        return;
    x->cfg_w = pw, x->cfg_h = ph;
    int32_t wd = pw << w->xeig, ht = (ph << w->yeig) + x->inset;
    extent(w, x, wd, ht);
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)x->handle;
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_GetWindowState, r))
        return;
    if (!(w->block[8] & (1u << 16)))
        return;                                  /* not open: it opens at the size */
    b[3] = b[1] + wd;
    b[2] = b[4] - ht;
    swi(XWimp_OpenWindow, r);
    browser_bar_open(w, x);
}

/* ---- from the compositor ---------------------------------------------------------- */

static void line(struct ws *w, char *l)
{
    char cmd[16];
    unsigned id;
    int a = 0, b = 0, n = 0;
    if (sscanf(l, "%15s %u%n", cmd, &id, &n) < 2)
        return;
    char *rest = l + n;
    struct xwin *x = wwin_by_id(w, id);
    if (strcmp(cmd, "new") == 0) {
        int m = 0, pid = 0;
        if (sscanf(rest, "%d %d %d %n", &a, &b, &pid, &m) >= 3)
            window_new(w, id, a, b, pid, rest + m);
    } else if (!x) {
        return;
    } else if (strcmp(cmd, "gone") == 0) {
        window_gone(w, x);
    } else if (strcmp(cmd, "title") == 0) {
        window_title(w, x, *rest == ' ' ? rest + 1 : rest);
    } else if (strcmp(cmd, "size") == 0 && sscanf(rest, "%d %d", &a, &b) == 2) {
        window_size(w, x, a, b);
    }
}

static void drop(struct ws *w)
{
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    w->inlen = 0;
    for (int i = 0; i < WINDOWS; i++)
        if (w->win[i].id)
            window_gone(w, &w->win[i]);
}

static void receive(struct ws *w)
{
    for (;;) {
        ssize_t n = recv(w->fd, w->in + w->inlen, sizeof w->in - 1 - w->inlen, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            drop(w);
            return;
        }
        if (n < 0)
            return;
        w->inlen += (uint32_t)n;
        char *start = w->in, *nl;
        while ((nl = memchr(start, '\n', w->inlen - (uint32_t)(start - w->in))) != NULL) {
            *nl = 0;
            line(w, start);
            start = nl + 1;
            if (w->fd < 0)
                return;
        }
        w->inlen -= (uint32_t)(start - w->in);
        memmove(w->in, start, w->inlen);
        if (w->inlen == sizeof w->in - 1)
            w->inlen = 0;                       /* a line too long */
    }
}

/* ---- to the compositor ----------------------------------------------------------- */

/* where a point (OS units) is in a window's work area, its pixels */
static int local(struct ws *w, struct xwin *x, int32_t px, int32_t py, int32_t *lx, int32_t *ly)
{
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)x->handle;
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_GetWindowState, r))
        return 0;
    int32_t ox = b[1], oy = b[4] - x->inset;      /* the program's top left: the
                                                     visible area's, below any toolbar */
    *lx = (px - ox) >> w->xeig;
    *ly = (oy - py) >> w->yeig;
    return 1;
}

static int button_code(uint32_t bit)
{
    return bit == BUTTON_SELECT ? BTN_LEFT : bit == BUTTON_MENU ? BTN_RIGHT : BTN_MIDDLE;
}

/* the pointer, read at a null event: motion over the window it is in, or
 * the one a button is held in, and the buttons let go */
static void track(struct ws *w)
{
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_GetPointerInfo, r))
        return;
    int32_t px = (int32_t)w->block[0], py = (int32_t)w->block[1];
    uint32_t buttons = w->block[2] & 7u, under = w->block[3];
    /* Which of ours the pointer is over. Pointer_Entering_Window comes
     * only when it crosses an edge. It does not come when a window opens
     * under the pointer. */
    struct xwin *u = wwin_by_handle(w, under);
    if (!u && w->inside) {
        struct xwin *was = wwin_by_handle(w, w->inside);
        w->inside = 0;
        if (was && !w->held)
            wwin_say(w, "leave %u", was->id);
    } else if (u && w->inside != under) {
        w->inside = under;
        w->last_x = w->last_y = -1;
    }
    uint32_t target = w->held ? w->held_window : w->inside;
    struct xwin *x = target ? wwin_by_handle(w, target) : NULL;
    if (!x)
        return;
    int32_t lx, ly;
    if (!local(w, x, px, py, &lx, &ly))
        return;
    if (lx != w->last_x || ly != w->last_y) {
        w->last_x = lx, w->last_y = ly;
        wwin_say(w, "motion %u %d %d", x->id, lx, ly);
    }
    if (w->held) {
        uint32_t up = w->held & ~buttons;
        for (uint32_t bit = 1; bit <= 4; bit <<= 1)
            if (up & bit)
                wwin_say(w, "button %u %d 0", x->id, button_code(bit));
        w->held &= buttons;
        if (!w->held) {
            w->held_window = 0;
            if (w->inside != x->handle)
                wwin_say(w, "leave %u", x->id);
        }
    }
}

static void click(struct ws *w, struct xwin *x)
{
    int32_t px = (int32_t)w->poll[0], py = (int32_t)w->poll[1];
    uint32_t buttons = w->poll[2] & 7u;
    int32_t lx, ly;
    if (!local(w, x, px, py, &lx, &ly))
        return;
    if (w->caret != x->handle) {
        /* the keyboard: an invisible caret in the window */
        uint32_t r[8] = { x->handle, 0xFFFFFFFFu, 0, 0, (1u << 25) | 40u, 0xFFFFFFFFu };
        swi(XWimp_SetCaretPosition, r);
    }
    w->last_x = lx, w->last_y = ly;
    wwin_say(w, "motion %u %d %d", x->id, lx, ly);
    for (uint32_t bit = 1; bit <= 4; bit <<= 1)
        if (buttons & bit & ~w->held)
            wwin_say(w, "button %u %d 1", x->id, button_code(bit));
    w->held |= buttons;
    w->held_window = x->handle;
}

/* the window opened where the Wimp says, and the program told of a new size */
static void open_request(struct ws *w, struct xwin *x)
{
    if (x->bfd >= 0)
        browser_scrolled(w, x, -(int32_t)w->poll[6] >> w->yeig);
    uint32_t r[8] = { 0, A(w->poll) };
    swi(XWimp_OpenWindow, r);
    browser_bar_open(w, x);
    int32_t pw = ((int32_t)w->poll[3] - (int32_t)w->poll[1]) >> w->xeig;
    int32_t ph = ((int32_t)w->poll[4] - (int32_t)w->poll[2] - x->inset) >> w->yeig;
    if (pw > 0 && ph > 0 && (pw != x->cfg_w || ph != x->cfg_h)) {
        x->cfg_w = pw, x->cfg_h = ph;
        wwin_say(w, "configure %u %d %d", x->id, pw, ph);
    }
}

/* Whether a key is one the compositor makes a Linux key of (wm_key in
 * rosgd-compositor.c): the printable characters, CTRL and a letter, the
 * editing and cursor keys, and F1-F11. Every other key goes back to the
 * Wimp (Wimp_ProcessKey), as a task that does not use a key must. They are
 * F12 and its kin above all, the Wimp's hot keys, and characters past
 * ASCII. */
static int passed_on(uint32_t code)
{
    if (code >= 1 && code <= 127)
        return 1;
    if (code >= 0x181 && code <= 0x189)                 /* F1-F9 */
        return 1;
    switch (code) {
    case 0x1CA: case 0x1CB:                             /* F10, F11 */
    case 0x18B: case 0x18C: case 0x18D: case 0x18E: case 0x18F:
    case 0x19C: case 0x19D: case 0x19E: case 0x19F:
    case 0x1AC: case 0x1AD: case 0x1AE: case 0x1AF:
    case 0x1CD:                                         /* Insert */
        return 1;
    default:
        return 0;
    }
}

/* ---- the task ------------------------------------------------------------------ */

static void run(struct ros_module *m, struct ws *w)
{
    strcpy(w->task_name, "WaylandWindows");
    strcpy(w->t_back, "Back");
    strcpy(w->t_fwd, "Forward");
    strcpy(w->t_reload, "Reload");
    strcpy(w->t_stop, "Stop");
    strcpy(w->t_home, "Home");
    strcpy(w->t_dl, "Downloads");
    strcpy(w->t_ok, "OK");
    strcpy(w->t_cancel, "Cancel");
    strcpy(w->find_val, "Ktar");                /* the writable icons' validation */
    browser_messages(w->messages, (int)(sizeof w->messages / sizeof w->messages[0]));
    w->fd = -1;
    os_error *e;
    {
        uint32_t r[8] = { 310, TASK_WORD, A(w->task_name), A(w->messages) };
        e = swi(XWimp_Initialise, r);
        if (!e)
            w->task_handle = r[1];
    }
    if (e) {
        uint32_t r[8] = { A(e), 2, A(w->task_name) };
        swi(XWimp_ReportError, r);
        task_end(m, w);
        ros_st32(m->private_word, 0xFFFFFFFFu);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        ros_swi(&s, OS_Exit);
        return;
    }
    read_mode(w);
    browser_aliases(w);

    for (;;) {
        if (w->mode_changed) {
            read_mode(w);
            browser_mode_change(w);
        }
        if (w->want_icon)
            browser_started(w);
        if (w->fd < 0)
            try_connect(w);
        int any = 0;
        for (int i = 0; i < WINDOWS; i++)
            any |= w->win[i].id != 0;
        uint32_t wait = w->fd < 0 ? 100 : (w->inside || w->held) ? 2 : any ? 4 : 10;
        uint32_t p[8] = { 1u << 7, A(w->poll), now() + wait, A(&w->pollword) };
        e = swi(XWimp_PollIdle, p);
        if (e) {
            uint32_t r[8] = { A(e), 1, A(w->task_name) };
            swi(XWimp_ReportError, r);
            continue;
        }
        if (w->fd >= 0)
            receive(w);
        for (int i = 0; i < WINDOWS; i++)
            if (w->win[i].id && w->win[i].bfd >= 0)
                browser_receive(w, &w->win[i]);
        struct xwin *x;
        switch (p[0]) {
        case NULL_REASON:
            track(w);
            break;
        case OPEN_REQUEST:
            if ((x = wwin_by_handle(w, w->poll[0])) != NULL)
                open_request(w, x);
            else {
                uint32_t r[8] = { 0, A(w->poll) };
                swi(XWimp_OpenWindow, r);
            }
            break;
        case SCROLL_REQUEST:
            /* the wheel: +36 is 4 a notch, up positive; Wayland's down */
            if ((x = wwin_by_handle(w, w->poll[0])) != NULL && (int32_t)w->poll[9] / 4)
                wwin_say(w, "axis %u %d", x->id, -((int32_t)w->poll[9] / 4));
            break;
        case CLOSE_REQUEST:
            if ((x = wwin_by_handle(w, w->poll[0])) != NULL)
                wwin_say(w, "close %u", x->id);
            else if (browser_save_close(w, w->poll[0]))
                break;
            break;
        case USER_DRAG_BOX:
            browser_drag_end(w);
            break;
        case POINTER_LEAVING:
            if ((x = wwin_by_handle(w, w->poll[0])) != NULL) {
                if (w->inside == x->handle)
                    w->inside = 0;
                if (!w->held)
                    wwin_say(w, "leave %u", x->id);
            }
            break;
        case POINTER_ENTERING:
            if (wwin_by_handle(w, w->poll[0])) {
                w->inside = w->poll[0];
                w->last_x = w->last_y = -1;
            }
            break;
        case MOUSE_CLICK:
            if (browser_save_click(w))
                break;
            if ((x = wwin_by_handle(w, w->poll[3])) != NULL) {
                /* A Menu click over a browser's page opens the browser menu
                 * that browser.c builds, and is not passed to the page. Any
                 * other Wayland program has the button. */
                if (x->bfd >= 0 && (w->poll[2] & BUTTON_MENU))
                    browser_menu(w, x);
                else
                    click(w, x);
            } else if ((x = wwin_by_bar(w, w->poll[3])) != NULL) {
                if (w->poll[2] & BUTTON_MENU)
                    browser_menu(w, x);
                else
                    browser_bar_click(w, x);
            } else {
                browser_icon_click(w);
            }
            break;
        case MENU_SELECTION:
            browser_selection(w);
            break;
        case KEY_PRESSED: {
            uint32_t code = w->poll[6];
            if (browser_save_key(w))
                break;
            if ((x = wwin_by_bar(w, w->poll[0])) != NULL) {
                if (!browser_bar_key(w, x, code)) {
                    uint32_t r[8] = { code };
                    swi(XWimp_ProcessKey, r);
                }
                break;
            }
            x = wwin_by_handle(w, w->poll[0]);
            if (x && x->bfd >= 0 && browser_key(w, x, code))
                break;
            if (x && passed_on(code))
                wwin_say(w, "key %u %u", x->id, code);
            else {
                uint32_t r[8] = { code };
                swi(XWimp_ProcessKey, r);
            }
            break;
        }
        case GAIN_CARET:
            if ((x = wwin_by_handle(w, w->poll[0])) != NULL) {
                w->caret = x->handle;
                wwin_say(w, "focus %u", x->id);
            }
            break;
        case LOSE_CARET:
            if (w->caret && w->caret == w->poll[0]) {
                w->caret = 0;
                wwin_say(w, "focus 0");
            }
            break;
        case USER_MESSAGE:
        case USER_MESSAGE_RECORDED:
            if (w->poll[4] == MESSAGE_QUIT)
                goto quit;
            browser_message(w);
            break;
        default:
            break;
        }
        continue;
    quit:
        task_end(m, w);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        ros_swi(&s, OS_Exit);
        return;
    }
}

/* ---- the module ----------------------------------------------------------------- */

static void task_end(struct ros_module *m, struct ws *w)
{
    if (w->task_handle) {
        browser_icon_gone(w);
        drop(w);
        uint32_t r[8] = { w->task_handle, TASK_WORD };
        swi(XWimp_CloseDown, r);
        w->task_handle = 0;
    } else if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }
    ros_st32(m->private_word, 0);
    xos_module_free(w);
}

#define SERVICE_RESET      0x27u
#define SERVICE_MODECHANGE 0x46u
#define SERVICE_STARTWIMP  0x49u
#define SERVICE_STARTEDWIMP 0x4Au

int waylandwin_autostart = -1;

static void service(struct ros_module *m, struct ros_cpu *s)
{
    struct ws *w = workspace(m);
    switch (s->r[1]) {
    case SERVICE_STARTWIMP:
        if (w || !(waylandwin_autostart >= 0 ? waylandwin_autostart
                                             : ros_cmdline_has("rosgd.display=compositor")))
            return;
        {
            void *block;
            if (xos_module_claim(sizeof *w, &block)) {
                s->r[1] = 0;
                return;
            }
            w = block;
            memset(w, 0, sizeof *w);
            w->fd = -1;
            ros_st32(m->private_word, A(w));
            strcpy(w->command, "WaylandWindows");
            s->r[0] = A(w->command);
            s->r[1] = 0;
        }
        return;
    case SERVICE_STARTEDWIMP:
        if (ros_ld32(m->private_word) == 0xFFFFFFFFu)
            ros_st32(m->private_word, 0);
        return;
    case SERVICE_RESET:
        if (!w) {
            if (ros_ld32(m->private_word) == 0xFFFFFFFFu)
                ros_st32(m->private_word, 0);
            return;
        }
        w->task_handle = 0;
        task_end(m, w);
        return;
    case SERVICE_MODECHANGE:
        if (w && w->task_handle)
            w->mode_changed = 1;
        return;
    default:
        return;
    }
}

/* ---- the compositor ---------------------------------------------------------- */

extern char **environ;

/* /usr/bin/wperun -d and argv: a program in the WPE edition's root, on
 * its own (its output the console's).  wperun returns once the program is
 * in a session of its own, so it is waited for.  0, or an errno; 127 if
 * there is no WPE root to run it in, 1 if wperun failed otherwise */
int wwin_wperun(char *const words[])
{
    char *argv[40] = { "/usr/bin/wperun", "-d" };
    int n = 2;
    for (int i = 0; words[i] && n < 39; i++)
        argv[n++] = words[i];
    argv[n] = NULL;
    if (access(argv[0], X_OK) != 0)
        return ENOENT;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/console", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/console", O_WRONLY, 0);
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    int e = posix_spawn(&pid, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (e)
        return e;
    int st = 0;
    ROS_BLOCKING(waitpid(pid, &st, 0));
    if (!WIFEXITED(st))
        return 1;
    return WEXITSTATUS(st) == 127 ? 127 : WEXITSTATUS(st) ? 1 : 0;   /* 127: no root */
}

/* rosgd-compositor -b, as the box starts it; -p as well with
 * rosgd.softpointer (run-vz.sh's: VZ's display has no cursor plane the Mac
 * shows, so the pointer is drawn into the frame) */
static void start_compositor(void)
{
    const char *path = ros_cmdline_value("rosgd.compositor");
    if (!path)
        path = "/usr/bin/rosgd-compositor";
    if (!strcmp(path, "none"))
        return;
    char *words[] = { (char *)path, "-b", NULL, NULL };
    if (ros_cmdline_has("rosgd.softpointer"))
        words[2] = "-p";
    int e = wwin_wperun(words);
    if (e == 127)
        ros_console_printf("rosgd: compositor: none -- this box has not got the WPE edition's root\n");
    else if (e == 1)
        ros_console_printf("rosgd: compositor: wperun could not run %s\n", path);
    else if (e)
        ros_console_printf("rosgd: compositor: %s\n", strerror(e));
    else
        ros_console_printf("rosgd: compositor: %s started\n", path);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    static int started;
    if (!started && ros_cmdline_has("rosgd.display=compositor")) {
        started = 1;                            /* once: not at each *RMReInit */
        start_compositor();
    }
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct ws *w = workspace(m);
    if (w)
        task_end(m, w);
    return NULL;
}

static void start(struct ros_module *m, uint32_t tail)
{
    (void)tail;
    struct ws *w = workspace(m);
    if (w && !w->task_handle) {
        run(m, w);
        return;
    }
    /* errors are made in the arena (ros_error): a block of /init's own
     * given to RISC OS stops the box (ros_addr) */
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = A(ros_error(0xC6, "Already running"));
    ros_swi(&s, OS_GenerateError);
}

static os_error *cmd_wayland(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)tail, (void)argc;
    if (!workspace(m)) {
        void *block;
        if (xos_module_claim(sizeof(struct ws), &block)) {
            return ros_error(0xC6, "Not enough memory");
        }
        struct ws *w = block;
        memset(w, 0, sizeof *w);
        w->fd = -1;
        ros_st32(m->private_word, A(w));
    }
    uint32_t r[8] = { 2, m->base + ros_ld32(m->base + 0x10), 0 };
    return swi(XOS_Module, r);
}

/* *WaylandRun <program> [arguments...]: a Linux program in the WPE
 * edition's root, started on its own; its windows come to the desktop
 * through the compositor.  Words split at spaces, "quoted" kept whole. */
static os_error *cmd_waylandrun(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    static char buf[1024];
    static char *words[36];
    const char *t = ros_ptr(tail);
    size_t k = 0;
    int n = 0;
    while ((unsigned char)*t >= ' ') {
        while (*t == ' ')
            t++;
        if ((unsigned char)*t < ' ')
            break;
        if (n == 35 || k >= sizeof buf - 2) {
            return ros_error(0xC6, "WaylandRun: too many words");
        }
        words[n++] = buf + k;
        int q = *t == '"';
        if (q)
            t++;
        while ((unsigned char)*t >= ' ' && (q ? *t != '"' : *t != ' ') && k < sizeof buf - 2)
            buf[k++] = *t++;
        if (q && *t == '"')
            t++;
        buf[k++] = 0;
    }
    words[n] = NULL;
    if (!n)
        return ros_error(0xDC, "Syntax: *WaylandRun <program> [arguments...]");
    /* the program's windows need the compositor, which a box shows its
     * screen through only when started so: without it a Wayland program
     * has nowhere to go */
    if (!ros_cmdline_has("rosgd.display=compositor"))
        return ros_error(0xC6, "WaylandRun needs the box started with its compositor "
                               "(the WPE edition: rosgd.display=compositor)");
    int e = wwin_wperun(words);
    if (e == ENOENT)
        return ros_error(0xC6, "WaylandRun: this box has no wperun");
    if (e == 127)
        return ros_error(0xC6, "WaylandRun: this box has not got the WPE edition's Linux root "
                               "(its disk, labelled boxwpe)");
    if (e)
        return ros_error(0xC6, "WaylandRun: %s could not be started", words[0]);
    return NULL;
}

/* *BrowserOpen <file or address>: shown in a browser window of its own.
 * The desktop's Alias$URLOpen_, Alias$Open_URI_ and Alias$@RunType_
 * variables all come through here (browser.c). */
static os_error *cmd_browseropen(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)argc;
    if (!browser_present())
        return ros_error(0xC6, "This box has no browser (it is the WPE edition's)");
    struct ws *w = workspace(m);
    const char *t = ros_ptr(tail);
    while (*t == ' ')
        t++;
    if ((unsigned char)*t < ' ') {
        browser_new_window(w, NULL);              /* no argument: the start page */
        return NULL;
    }
    static char what[URL_LEN];
    size_t n = 0;
    while ((unsigned char)*t >= ' ' && n < sizeof what - 1)
        what[n++] = *t++;
    while (n && what[n - 1] == ' ')
        n--;
    what[n] = 0;
    if (w)
        w->want_icon = 1;                         /* the task's job, not this one's */
    if (browser_is_url(what)) {
        browser_open(what, 0);
        return NULL;
    }
    /* A file. Its type is read where the name stands. The command tail
     * is arena memory, and a copy of ours is this program's own, which
     * compiled code may not be given (ros_addr aborts on it). */
    uint32_t r[8] = { 5, tail };
    if (swi(XOS_File, r) || r[0] != 1)
        return ros_error(0xD6, "File '%s' not found", what);
    uint32_t type = ((r[2] >> 20) == 0xFFFu) ? (r[2] >> 8) & 0xFFFu : 0xFFFu;
    if (!browser_open(what, type))
        return ros_error(0xC6, "The browser cannot show '%s'", what);
    return NULL;
}

static const struct ros_command commands[] = {
    { "WaylandWindows", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *WaylandWindows",
      "Runs the task that gives Linux programs' windows Wimp windows, when\r"
      "ROSGD's compositor shows the screen (rosgd.display=compositor).\r",
      cmd_wayland },
    { "BrowserOpen", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *BrowserOpen [<file or address>]",
      "Shows a web page, a web address, or a URL file in a browser window of its own.\r"
      "The desktop's URLOpen, Open_URI and RunType variables all lead here.\r",
      cmd_browseropen },
    { "WaylandRun", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *WaylandRun <program> [arguments...]",
      "Starts a Linux program in the WPE edition's root (its path there), on its own;\r"
      "its windows are Wimp windows when the compositor shows the screen.\r",
      cmd_waylandrun },
    { 0 },
};

struct ros_module waylandwin_module = {
    .title = "WaylandWindows",
    .help = "WaylandWindows\t0.10 (06-Oct-26) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .start = start,
    .commands = commands,
};
