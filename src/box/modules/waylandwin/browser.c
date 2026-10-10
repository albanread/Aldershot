/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* browser.c -- !Browser's RISC OS side: WaylandWindows' browser part.
 *
 * rosgd-browser (ports/browser) is a WebKit view in a Wayland window and
 * nothing else. It has no toolbar, no menus and no idea that it is on a
 * RISC OS desktop. Everything a browser does besides showing the page is
 * here, in RISC OS, where it can be done the desktop's way:
 *
 *   The icon bar.  One icon, there from the start in a box with the WPE
 *   edition's root. Select opens a window, Menu opens the icon bar's menu,
 *   and a page dropped on it opens in a window of its own.
 *
 *   The toolbar.  A window nested along the top of each browser window:
 *   Back, Forward, Reload, Stop, Home, the address, Downloads, and below
 *   them a status line. It is laid out to the window's width, and again
 *   whenever that changes, so the address field grows with the window.
 *
 *   The menu.  Menu over a page opens the Wimp menu, and what is in it
 *   depends on what is under the pointer. The browser reports every hit
 *   test as the pointer moves (the one thing that WebKit2 will still
 *   answer about a page), so Link and Image are live when the pointer is
 *   on one and shaded when it is not. There is no script, no round trip
 *   and no waiting.
 *
 *   The Wimp's messages.  An HTML page, a text file, an image, a URL file
 *   or a URI file can be dropped on a window, dropped on the icon bar, or
 *   double-clicked in a Filer window (Message_DataOpen, which nothing else
 *   in the WPE edition claims). It is then shown: file:// for a file, and
 *   the address inside for a URL file. Interactive help answers for the
 *   icon, the toolbar and the page.
 *
 *   The keys.  Escape stops, F3 saves the page, F5 reloads, Ctrl-L puts
 *   the caret in the address, and Ctrl-N opens a window. The page has every
 *   other key.
 *
 * A window is a process. rosgd-browser names its control socket after its
 * own process id, and the compositor tells us each window's process, so
 * every window has a toolbar and a browser of its own. New windows come
 * from *WaylandRun, which is ours, and from the page's own window.open or
 * target="_blank", which the browser turns into the same thing.
 *
 * The lines over the socket are listed at the head of
 * ports/browser/rosgd-browser.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "wwin.h"
#include "rosgd/api.h"
#include "rosgd/platform.h"
#include "rosgd/swi.h"
#include "fileswitch.h"

#define BROWSER_IN_ROOT "/usr/bin/rosgd-browser"   /* its path inside the root */
#define SPRITES "Resources:$.Resources.Browser.Sprites"
#define DOWNLOADS_HOST "/host/Downloads"
#define DOWNLOADS_ROS  "HostFS::Host.$.Downloads"
#define START_PAGE "https://www.riscosopen.org/"
#define ICONBAR 0xFFFFFFFEu                        /* -2, the icon bar's window */
/* DragASprite (its module is in the ROM; the generated api.h has no name
 * for its SWIs, which belong to the module rather than to ROSGD) */
#define XDragASprite_Start 0x62400u
#define XDragASprite_Stop  0x62401u

/* the file types a page may be: HTML, text, a URL or URI file, an image */
#define T_HTML 0xFAFu
#define T_TEXT 0xFFFu
#define T_URL  0xB28u
#define T_URI  0xF91u
#define T_PNG  0xB60u
#define T_JPEG 0xC85u
#define T_GIF  0x695u
#define T_SVG  0xAADu

/* ---- the Wimp's messages we want ------------------------------------------------ */

void browser_messages(uint32_t *list, int max)
{
    static const uint32_t want[] = { MESSAGE_QUIT, MESSAGE_DATASAVEACK, MESSAGE_DATALOAD,
                                     MESSAGE_DATALOADACK, MESSAGE_DATAOPEN,
                                     MESSAGE_HELPREQUEST, 0 };
    int n = 0;
    for (; want[n] && n < max - 1; n++)
        list[n] = want[n];
    list[n] = 0;
}

/* ---- is there a browser in this box? ------------------------------------------- */

/* The program lives in the WPE edition's root, which wperun mounts at
 * /wpe: a box without that root (the standard edition) has no browser,
 * and so no icon on the icon bar. */
int browser_present(void)
{
    return access(BROWSER_PROG, X_OK) == 0;
}

/* ---- to the browser ------------------------------------------------------------- */

void browser_tell(struct xwin *x, const char *fmt, ...)
{
    if (x->bfd < 0)
        return;
    char line[URL_LEN + 16];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    if (send(x->bfd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno != EAGAIN) {
        close(x->bfd);
        x->bfd = -1;
    }
}

/* A window whose program is a rosgd-browser: its control socket is there,
 * named after the process the compositor reports */
int browser_connect(struct xwin *x, int pid)
{
    if (pid <= 0)
        return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, BROWSER_SOCKS "/%d.sock", pid);
    if (access(addr.sun_path, F_OK) != 0)
        return -1;
    int fd = wwin_unix_socket();
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    x->bfd = fd;
    x->binlen = 0;
    x->zoom = 100;
    return 0;
}

/* A window of its own, which is a browser of its own: the program run
 * in the WPE edition's root, as *WaylandRun does it. It does not go
 * through OS_CLI, because an address is as long as it likes and a command
 * line is 1024 characters with quoting in it. */
void browser_new_window(struct ws *w, const char *url)
{
    /* A window takes seconds to appear (WebKit has three processes to
     * start), so the reader clicks again, and again. One is enough:
     * asking twice within a second and a half means the first was not
     * seen yet. It does not mean two were wanted. */
    if (w) {
        uint32_t t = now();
        if (w->new_at && (int32_t)(t - w->new_at) < 150)
            return;
        w->new_at = t;
        w->want_icon = 1;                             /* the task puts its icon up */
    }
    char *words[3] = { (char *)BROWSER_IN_ROOT, NULL, NULL };
    if (url && *url)
        words[1] = (char *)url;
    wwin_wperun(words);
}

static void open_downloads(struct ws *w)
{
    mkdir(DOWNLOADS_HOST, 0755);
    wwin_cli(w, "Filer_OpenDir %s", DOWNLOADS_ROS);
}

/* ---- the toolbar ---------------------------------------------------------------- */

static void bar_icon(struct ws *w, struct xwin *x, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     uint32_t flags, char *text, uint32_t size)
{
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)x->bar;
    b[1] = x0, b[2] = y0, b[3] = x1, b[4] = y1;
    b[5] = (int32_t)(flags | NEEDS_HELP);
    b[6] = (int32_t)A(text), b[7] = -1, b[8] = (int32_t)size;
    uint32_t r[8] = { 0, A(w->block) };
    swi(XWimp_CreateIcon, r);
}

/* a button wide enough for its word in the desktop font, whatever that is */
static int32_t button_width(const char *label)
{
    uint32_t r[8] = { 1, A(label), 0 };                   /* Wimp_TextOp 1: its width */
    int32_t wd = swi(XWimp_TextOp, r) ? (int32_t)(16 * strlen(label)) : (int32_t)r[0];
    return wd + 28;                                       /* the borders and a gap each side */
}

/* The toolbar laid out for a window this wide: the buttons at the left,
 * Downloads at the right, the address field all that is between them, and
 * the status line along the width below.  Done again whenever the width
 * changes, so the address grows with the window. */
static void bar_layout(struct ws *w, struct xwin *x, int32_t width)
{
    char *const label[] = { w->t_back, w->t_fwd, w->t_reload, w->t_stop, w->t_home };
    if (!x->bar || width <= 0 || width == x->bar_w)
        return;
    x->bar_w = width;
    int32_t at = 8;
    for (int i = B_BACK; i <= B_HOME; i++) {
        int32_t wd = button_width(label[i]);
        uint32_t r[8] = { x->bar, (uint32_t)i, (uint32_t)at, BAR_ROW1_Y0,
                          (uint32_t)(at + wd), BAR_ROW1_Y1 };
        swi(XWimp_ResizeIcon, r);
        at += wd + 4;
    }
    int32_t dlw = button_width(w->t_dl);
    int32_t dl0 = width - 8 - dlw, dl1 = width - 8;
    if (dl0 < at + 48)
        dl0 = at + 48, dl1 = dl0 + dlw;
    uint32_t u[8] = { x->bar, B_URL, (uint32_t)(at + 8), BAR_ROW1_Y0,
                      (uint32_t)(dl0 - 8), BAR_ROW1_Y1 };
    swi(XWimp_ResizeIcon, u);
    uint32_t d[8] = { x->bar, B_DOWNLOADS, (uint32_t)dl0, BAR_ROW1_Y0, (uint32_t)dl1, BAR_ROW1_Y1 };
    swi(XWimp_ResizeIcon, d);
    uint32_t s[8] = { x->bar, B_STATUS, 8, BAR_ROW2_Y0, (uint32_t)(width - 8), BAR_ROW2_Y1 };
    swi(XWimp_ResizeIcon, s);
    uint32_t f[8] = { x->bar, 0, (uint32_t)-TOOLBAR, (uint32_t)width, 0 };
    swi(XWimp_ForceRedraw, f);
}

/* The toolbar: redrawn by the Wimp. It is nested and is not flagged a pane
 * (in a nested pane a click never gives a writable icon the caret: !PDF's) */
void browser_bar_create(struct ws *w, struct xwin *x)
{
    int32_t *b = (int32_t *)w->block;
    memset(w->block, 0, sizeof w->block);
    b[6] = -1;
    b[7] = (int32_t)(1u << 31 | 1u << 4);        /* new format, auto-redraw */
    b[8] = 7 | 2 << 8 | 7 << 16 | 1 << 24;       /* the work area grey */
    b[9] = 3 | 1 << 8 | 12 << 16;
    b[10] = 0, b[11] = -TOOLBAR, b[12] = 8192, b[13] = 0;
    b[15] = 3 << 12;
    b[16] = 1;
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_CreateWindow, r))
        return;
    x->bar = r[0];
    /* in the order of the B_ enum: the handles are 0 upwards */
    bar_icon(w, x, 8, BAR_ROW1_Y0, 88, BAR_ROW1_Y1, BUTTON, w->t_back, sizeof w->t_back);
    bar_icon(w, x, 92, BAR_ROW1_Y0, 220, BAR_ROW1_Y1, BUTTON, w->t_fwd, sizeof w->t_fwd);
    bar_icon(w, x, 224, BAR_ROW1_Y0, 336, BAR_ROW1_Y1, BUTTON, w->t_reload, sizeof w->t_reload);
    bar_icon(w, x, 340, BAR_ROW1_Y0, 420, BAR_ROW1_Y1, BUTTON, w->t_stop, sizeof w->t_stop);
    bar_icon(w, x, 424, BAR_ROW1_Y0, 504, BAR_ROW1_Y1, BUTTON, w->t_home, sizeof w->t_home);
    bar_icon(w, x, 512, BAR_ROW1_Y0, 1300, BAR_ROW1_Y1, WRITABLE, x->url, URL_LEN);
    bar_icon(w, x, 1308, BAR_ROW1_Y0, 1468, BAR_ROW1_Y1, BUTTON, w->t_dl, sizeof w->t_dl);
    bar_icon(w, x, 8, BAR_ROW2_Y0, 1468, BAR_ROW2_Y1, DISPLAY, x->status, STATUS_LEN);
}

/* the toolbar along the top of the window's visible area: its left and
 * right edges on the window's, its top on its top, a pixel short of the
 * page so that the outline drawn below it is not on the page */
void browser_bar_open(struct ws *w, struct xwin *x)
{
    if (!x->bar)
        return;
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)x->handle;
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_GetWindowState, r) || !(w->block[8] & (1u << 16)))
        return;
    int32_t x0 = b[1], x1 = b[3], y1 = b[4];
    bar_layout(w, x, x1 - x0);
    b[0] = (int32_t)x->bar;
    b[1] = x0, b[2] = y1 - TOOLBAR + (1 << w->yeig), b[3] = x1, b[4] = y1;
    b[5] = 0, b[6] = 0, b[7] = -1;
    uint32_t o[8] = { 0, A(w->block), TASK_WORD, x->handle, 1u << 16 | 2u << 18 | 2u << 20 | 2u << 22 };
    swi(XWimp_OpenWindow, o);
}

/* an icon's text changed (or its shading): drawn again */
static void bar_set(struct ws *w, struct xwin *x, int icon, uint32_t eor, uint32_t clear)
{
    uint32_t *b = w->block;
    b[0] = x->bar, b[1] = (uint32_t)icon, b[2] = eor, b[3] = clear;
    uint32_t r[8] = { 0, A(w->block) };
    swi(XWimp_SetIconState, r);
}

/* The status line: what is under the pointer if anything is, else what
 * the browser last said (loading, a download).  A link is worth more to
 * the reader than "Loading 97%". */
static void status_show(struct ws *w, struct xwin *x)
{
    const char *s = x->link[0] ? x->link : x->image[0] ? x->image : x->msg;
    if (!strcmp(s, x->status))
        return;
    snprintf(x->status, sizeof x->status, "%s", s);
    bar_set(w, x, B_STATUS, 0, 0);
}

/* What the browser last said. News worth keeping is sticky: a download, a
 * page that would not come, a word not found. The progress ticks that
 * follow do not write over it, and only the next page clears it. (It was
 * spelt as strncmp against "Saved" and "Downloading" before, which both
 * missed the new messages and could never be got right.) */
static void say_msg(struct ws *w, struct xwin *x, int sticky, const char *fmt, const char *arg)
{
    snprintf(x->msg, sizeof x->msg, fmt, arg);
    x->msg_sticky = sticky;
    status_show(w, x);
}

/* whether the caret is in the toolbar (someone typing an address) */
static int bar_has_caret(struct ws *w, struct xwin *x)
{
    uint32_t r[8] = { 0, A(w->block) };
    return !swi(XWimp_GetCaretPosition, r) && w->block[0] == x->bar;
}

static void caret_to_page(struct ws *w, struct xwin *x)
{
    (void)w;
    uint32_t r[8] = { x->handle, 0xFFFFFFFFu, 0, 0, (1u << 25) | 40u, 0xFFFFFFFFu };
    swi(XWimp_SetCaretPosition, r);
}

static void caret_to_url(struct ws *w, struct xwin *x)
{
    (void)w;
    uint32_t r[8] = { x->bar, B_URL, 0, 0, 0xFFFFFFFFu, (uint32_t)(int32_t)strlen(x->url) };
    swi(XWimp_SetCaretPosition, r);
}

/* The page's place and height, as the browser says: the work area is the
 * toolbar and the page, the scroll offset the page's.  Just after a drag
 * of the scroll bar the page's reports, behind the bar, move the extent
 * but not the bar. */
static void page_scrolled(struct ws *w, struct xwin *x, int32_t y, int32_t h)
{
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)x->handle;
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_GetWindowState, r) || !(w->block[8] & (1u << 16)))
        return;
    int32_t vis_h = b[4] - b[2], ext_h = (h << w->yeig) + x->inset;
    if (ext_h < vis_h)
        ext_h = vis_h;
    if (h != x->doc_h) {
        x->doc_h = h;
        x->extent_new = 1;
        int32_t *e = (int32_t *)w->block + 16;
        e[0] = 0, e[1] = -ext_h, e[2] = w->screen_w > b[3] - b[1] ? w->screen_w : b[3] - b[1], e[3] = 0;
        uint32_t s[8] = { x->handle, A(e) };
        swi(XWimp_SetExtent, s);
    }
    x->scroll_y = y;
    int32_t scy = -(y << w->yeig);
    if ((int32_t)(now() - x->dragged_at) < 30)
        scy = b[6];                             /* the bar's, being dragged */
    if (b[6] == scy && h == x->doc_h && !x->extent_new)
        return;
    /* opened again, so the scroll bar is drawn for the new extent: the
     * Wimp draws a window's furniture when it is opened */
    x->extent_new = 0;
    b[0] = (int32_t)x->handle;
    b[6] = scy;
    swi(XWimp_OpenWindow, r);
    browser_bar_open(w, x);
}

/* the Wimp's scroll bar moved the page (an open request on the window) */
void browser_scrolled(struct ws *w, struct xwin *x, int32_t y)
{
    (void)w;
    if (y == x->scroll_y)
        return;
    x->scroll_y = y;
    x->dragged_at = now();
    browser_tell(x, "scrollto %d", y);
}

/* ---- what the browser says ------------------------------------------------------ */

static void bline(struct ws *w, struct xwin *x, char *l)
{
    if (!strncmp(l, "url ", 4)) {
        if (!bar_has_caret(w, x)) {
            snprintf(x->url, sizeof x->url, "%s", l + 4);
            bar_set(w, x, B_URL, 0, 0);
        }
    } else if (!strncmp(l, "nav ", 4)) {
        int back = 0, fwd = 0;
        sscanf(l + 4, "%d %d", &back, &fwd);
        bar_set(w, x, B_BACK, back ? 0 : SHADED, SHADED);
        bar_set(w, x, B_FWD, fwd ? 0 : SHADED, SHADED);
    } else if (!strncmp(l, "progress ", 9)) {
        int p = atoi(l + 9);
        if (p < 100 && !x->msg_sticky) {
            char pc[8];
            snprintf(pc, sizeof pc, "%d", p);
            say_msg(w, x, 0, "Loading %s%%", pc);
        }
    } else if (!strcmp(l, "load started")) {
        bar_set(w, x, B_STOP, 0, SHADED);
        say_msg(w, x, 0, "%s", "");             /* a new page: the last news is done with */
    } else if (!strcmp(l, "load finished")) {
        bar_set(w, x, B_STOP, SHADED, SHADED);
        if (!x->msg_sticky)
            say_msg(w, x, 0, "%s", "");
    } else if (!strncmp(l, "scroll ", 7)) {
        int y = 0, h = 0, vh = 0;
        if (sscanf(l + 7, "%d %d %d", &y, &h, &vh) >= 2)
            page_scrolled(w, x, y, h);
    } else if (!strncmp(l, "zoom ", 5)) {
        x->zoom = atoi(l + 5);
    } else if (!strncmp(l, "hit ", 4)) {
        /* a new hit test: what was under the pointer is no longer */
        x->link[0] = 0;
        x->image[0] = 0;
        status_show(w, x);
    } else if (!strncmp(l, "hitlink ", 8)) {
        snprintf(x->link, sizeof x->link, "%s", l + 8);
        status_show(w, x);
    } else if (!strncmp(l, "hitimage ", 9)) {
        snprintf(x->image, sizeof x->image, "%s", l + 9);
        status_show(w, x);
    } else if (!strcmp(l, "find found")) {
        say_msg(w, x, 0, "%s", "");
    } else if (!strcmp(l, "find notfound")) {
        say_msg(w, x, 1, "Not found: %s", w->find);
    } else if (!strncmp(l, "error ", 6)) {
        say_msg(w, x, 1, "%s", l + 6);
    } else if (!strncmp(l, "download started ", 17)) {
        say_msg(w, x, 1, "Downloading %s", l + 17);
    } else if (!strncmp(l, "download done ", 14)) {
        say_msg(w, x, 1, "Saved %s in Downloads", l + 14);
    } else if (!strncmp(l, "download failed ", 16)) {
        say_msg(w, x, 1, "Download failed: %s", l + 16);
    }
}

void browser_receive(struct ws *w, struct xwin *x)
{
    for (;;) {
        ssize_t n = recv(x->bfd, x->bin + x->binlen, sizeof x->bin - 1 - x->binlen, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            close(x->bfd);
            x->bfd = -1;
            return;
        }
        if (n < 0)
            return;
        x->binlen += (uint32_t)n;
        char *start = x->bin, *nl;
        while ((nl = memchr(start, '\n', x->binlen - (uint32_t)(start - x->bin))) != NULL) {
            *nl = 0;
            bline(w, x, start);
            start = nl + 1;
        }
        x->binlen -= (uint32_t)(start - x->bin);
        memmove(x->bin, start, x->binlen);
        if (x->binlen == sizeof x->bin - 1)
            x->binlen = 0;
    }
}

void browser_closed(struct ws *w, struct xwin *x)
{
    if (w->menu_window == x->handle)
        w->menu_window = 0;
    if (x->bfd >= 0) {
        close(x->bfd);
        x->bfd = -1;
    }
}

/* ---- a click on the toolbar ----------------------------------------------------- */

void browser_bar_click(struct ws *w, struct xwin *x)
{
    switch ((int32_t)w->poll[4]) {
    case B_BACK: browser_tell(x, "back"); break;
    case B_FWD: browser_tell(x, "forward"); break;
    case B_RELOAD: browser_tell(x, "reload"); break;
    case B_STOP: browser_tell(x, "stop"); break;
    case B_HOME: browser_tell(x, "go %s", START_PAGE); break;
    case B_DOWNLOADS: open_downloads(w); break;
    default: break;
    }
}

/* ---- the icon bar --------------------------------------------------------------- */

/* The icon belongs to the browser and not to the box. It comes when a
 * browser is started and goes when Quit is chosen, as an application's
 * does. (It used to be put up at boot and never taken down. That left
 * Quit with nothing to mean, because WaylandWindows is the window manager
 * for every Wayland program, and quitting that is not what Quit on a
 * browser's icon should do.)
 *
 * icon_up says whether there is one. icon_handle does not, because 0 is a
 * perfectly good icon handle, and reading it as "none" is how a second
 * icon gets made. */
/* ONLY from WaylandWindows' own task. A Wimp icon belongs to the task
 * that creates it, and a *command runs in whatever task issued it. An
 * icon made there dies with that task, leaving one on the bar that answers
 * nobody. A command sets want_icon and the poll loop comes here. */
void browser_started(struct ws *w)
{
    if (!w || w->icon_up || !browser_present())
        return;
    w->want_icon = 0;
    strcpy(w->icon_sprite, "!browser");
    wwin_cli(w, "IconSprites %s", SPRITES);
    uint32_t sz[8] = { 40, 0, A(w->icon_sprite) };    /* Wimp_SpriteOp 40: its size */
    if (swi(XWimp_SpriteOp, sz)) {
        ros_console_printf("rosgd: browser: the Wimp's pool has no '%s' sprite\n",
                           w->icon_sprite);
        return;
    }
    uint32_t *b = w->block;
    b[0] = 0xFFFFFFFFu;                               /* -1: the right-hand side */
    b[1] = 0, b[2] = 0;
    b[3] = sz[3] << w->xeig, b[4] = sz[4] << w->yeig;
    b[5] = 0x0700319Au;                               /* sprite, centred, indirected, click,
                                                         and it answers help requests */
    b[6] = A(w->icon_sprite), b[7] = 1, b[8] = 12;    /* the Wimp's sprite pool */
    uint32_t r[8] = { 0, A(b) };
    if (swi(XWimp_CreateIcon, r)) {
        ros_console_printf("rosgd: browser: the Wimp would not make the icon bar's icon\n");
        return;
    }
    w->icon_handle = (int32_t)r[0];
    w->icon_up = 1;
}

/* A mode change: the icon's size is the sprite's in the new mode, so it is
 * made again, but only if it was there. The flag is cleared before the
 * new one is made, so a failure leaves none rather than two. */
void browser_mode_change(struct ws *w)
{
    if (!w->icon_up)
        return;
    browser_icon_gone(w);
    browser_started(w);
}

static int file_address(uint32_t type, const char *name, int dropped, char *out, size_t max);

/* *BrowserOpen's half: a window of its own for `what`. It is an address
 * when type is 0, else the file of that type. `what` is this program's own
 * memory and never reaches compiled code, which may be given arena
 * addresses only (ros_addr aborts on anything else). */
int browser_open(const char *what, uint32_t type)
{
    char url[URL_LEN];
    if (!type) {
        browser_new_window(NULL, what);
        return 1;
    }
    if (!file_address(type, what, 1, url, sizeof url))
        return 0;
    browser_new_window(NULL, url);
    return 1;
}

/* whether a name is an address rather than a file */
int browser_is_url(const char *what)
{
    return strstr(what, "://") != NULL || !strncasecmp(what, "mailto:", 7) ||
           !strncasecmp(what, "about:", 6);
}

/* The variables the desktop opens web things through, as RISC OS 5.30's
 * browser sets them (NetSurf's !Boot):
 *
 *   Alias$URLOpen_<scheme>   the ANT protocol's load step. StrongHelp's
 *                            manuals and PipeDream's help look for it.
 *                            Neither found it in this box before, so not
 *                            one http link anywhere in it worked.
 *   Alias$Open_URI_<scheme>  the Acorn protocol's last resort, for when a
 *                            URI module is there to broadcast first.
 *   Alias$@RunType_FAF, _B28 the Filer's fallback. It sends
 *                            Message_DataOpen first (which this task
 *                            claims) and comes here only if that bounces,
 *                            from the command line or with no desktop.
 *                            It does not use &F91, because the
 *                            specification says an application must not
 *                            claim the URI type.
 *   File$Type_*              the names, which the kernel's own table
 *                            stops short of.
 *
 * All of this is behind the same test as the icon, so a box with no WebKit
 * in it does not say it can open a web page. */
void browser_aliases(struct ws *w)
{
    if (!browser_present())
        return;
    wwin_cli(w, "Set File$Type_FAF HTML");
    wwin_cli(w, "Set File$Type_B28 URL");
    wwin_cli(w, "Set File$Type_F91 URI");
    wwin_cli(w, "Set File$Type_F79 CSS");
    static const char *const scheme[] = { "http", "https", "file", NULL };
    for (int i = 0; scheme[i]; i++) {
        wwin_cli(w, "Set Alias$URLOpen_%s BrowserOpen %%*0", scheme[i]);
        wwin_cli(w, "Set Alias$Open_URI_%s BrowserOpen", scheme[i]);
    }
    wwin_cli(w, "Set Alias$@RunType_FAF BrowserOpen %%*0");
    wwin_cli(w, "Set Alias$@RunType_B28 BrowserOpen %%*0");
}

void browser_icon_gone(struct ws *w)
{
    if (!w->icon_up)
        return;
    uint32_t *b = w->block;
    b[0] = ICONBAR;
    b[1] = (uint32_t)w->icon_handle;
    uint32_t r[8] = { 0, A(b) };
    if (swi(XWimp_DeleteIcon, r))
        ros_console_printf("rosgd: browser: the Wimp would not delete the icon bar's icon\n");
    w->icon_up = 0;                                   /* gone either way: never two */
    w->icon_handle = 0;
}

/* Quit: every window this browser opened is asked to close, its programs
 * go with them, and the icon leaves the bar. */
static void browser_quit(struct ws *w)
{
    for (int i = 0; i < WINDOWS; i++)
        if (w->win[i].id && w->win[i].bfd >= 0)
            wwin_say(w, "close %u", w->win[i].id);
    browser_icon_gone(w);
}

/* a click on the icon: a window of its own */
int browser_icon_click(struct ws *w)
{
    if (!w->icon_up || (int32_t)w->poll[3] != -2 || (int32_t)w->poll[4] != w->icon_handle)
        return 0;
    if (w->poll[2] & BUTTON_MENU)
        browser_icon_menu(w);
    else
        browser_new_window(w, NULL);
    return 1;
}

/* ---- the menus ------------------------------------------------------------------ */

enum { M_BACK, M_FWD, M_RELOAD, M_STOP, M_HOME, M_LINK, M_IMAGE, M_SAVE, M_ADDRESS,
       M_FIND, M_ZOOM, M_NEW, M_DOWNLOADS, M_ITEMS };
enum { ML_NEW, ML_SAVE, ML_ADDRESS };
enum { MF_TEXT, MF_NEXT, MF_PREV };
enum { MB_NEW, MB_DOWNLOADS, MB_QUIT };
static const int zooms[] = { 50, 75, 100, 125, 150, 200 };

static void menu_head(uint32_t *m, const char *title, int32_t width, int items)
{
    memset(m, 0, MENU_HEADER + (uint32_t)items * MENU_ITEM);
    memcpy(m, title, strlen(title) < 12 ? strlen(title) : 12);
    uint8_t *c = (uint8_t *)m;
    c[12] = 7, c[13] = 2, c[14] = 7, c[15] = 0;
    m[4] = (uint32_t)width;
    m[5] = 44;
    m[6] = 0;
}

static uint32_t *item_of(uint32_t *m, int k)
{
    return &m[(MENU_HEADER + (uint32_t)k * MENU_ITEM) / 4];
}

static void menu_item(uint32_t *m, int k, const char *text, uint32_t mflags, uint32_t iflags)
{
    uint32_t *it = item_of(m, k);
    it[0] = mflags;
    it[1] = 0xFFFFFFFFu;
    it[2] = 0x07000021u | iflags;
    memset(&it[3], 0, 12);
    memcpy(&it[3], text, strlen(text) < 12 ? strlen(text) : 12);
}

/* a writable item: the Wimp gives it the caret and a writable button type */
static void menu_write(uint32_t *m, int k, char *buf, uint32_t len, char *val, uint32_t mflags)
{
    uint32_t *it = item_of(m, k);
    it[0] = mflags | MF_WRITABLE;
    it[1] = 0xFFFFFFFFu;
    it[2] = 0x07000121u;                              /* text, indirected, filled */
    it[3] = A(buf), it[4] = A(val), it[5] = len;
}

static void menu_sub(uint32_t *m, int k, const uint32_t *sub)
{
    item_of(m, k)[1] = A(sub);
}

/* The page's menu, as the page is now: Back and Forward as the browser
 * last said, Link and Image live only when the pointer is on one. */
static void menu_build(struct ws *w, struct xwin *x)
{
    menu_head(w->menu_link, "Link", 208, 3);
    menu_item(w->menu_link, ML_NEW, "New window", 0, 0);
    menu_item(w->menu_link, ML_SAVE, "Save link", 0, 0);
    menu_item(w->menu_link, ML_ADDRESS, "Save address", MF_LAST, 0);
    menu_head(w->menu_image, "Image", 192, 2);
    menu_item(w->menu_image, ML_NEW, "New window", 0, 0);
    menu_item(w->menu_image, ML_SAVE, "Save image", MF_LAST, 0);
    menu_head(w->menu_find, "Find", 272, 3);
    strcpy(w->find_val, "Ktar");
    menu_write(w->menu_find, MF_TEXT, w->find, sizeof w->find, w->find_val, MF_LINE);
    menu_item(w->menu_find, MF_NEXT, "Next", 0, 0);
    menu_item(w->menu_find, MF_PREV, "Previous", MF_LAST, 0);
    menu_head(w->menu_zoom, "Zoom", 112, 6);
    for (int i = 0; i < 6; i++) {
        char t[8];
        snprintf(t, sizeof t, "%d%%", zooms[i]);
        menu_item(w->menu_zoom, i, t, (zooms[i] == x->zoom ? MF_TICK : 0) | (i == 5 ? MF_LAST : 0), 0);
    }

    menu_head(w->menu, "Browser", 208, M_ITEMS);
    menu_item(w->menu, M_BACK, "Back", 0, 0);
    menu_item(w->menu, M_FWD, "Forward", 0, 0);
    menu_item(w->menu, M_RELOAD, "Reload", 0, 0);
    menu_item(w->menu, M_STOP, "Stop", 0, 0);
    menu_item(w->menu, M_HOME, "Home", MF_LINE, 0);
    menu_item(w->menu, M_LINK, "Link", 0, w->hit_link[0] ? 0 : SHADED);
    menu_item(w->menu, M_IMAGE, "Image", 0, w->hit_image[0] ? 0 : SHADED);
    menu_item(w->menu, M_SAVE, "Save page", 0, 0);
    menu_item(w->menu, M_ADDRESS, "Save address", MF_LINE, 0);
    menu_item(w->menu, M_FIND, "Find", 0, 0);
    menu_item(w->menu, M_ZOOM, "Zoom", MF_LINE, 0);
    menu_item(w->menu, M_NEW, "New window", 0, 0);
    menu_item(w->menu, M_DOWNLOADS, "Downloads", MF_LAST, 0);
    menu_sub(w->menu, M_LINK, w->menu_link);
    menu_sub(w->menu, M_IMAGE, w->menu_image);
    menu_sub(w->menu, M_FIND, w->menu_find);
    menu_sub(w->menu, M_ZOOM, w->menu_zoom);
    /* Back and Forward as the toolbar has them */
    uint32_t *b = w->block;
    b[0] = x->bar, b[1] = B_BACK;
    uint32_t r[8] = { 0, A(b) };
    if (!swi(XWimp_GetIconState, r) && (b[6] & SHADED))
        item_of(w->menu, M_BACK)[2] |= SHADED;
    b[0] = x->bar, b[1] = B_FWD;
    if (!swi(XWimp_GetIconState, r) && (b[6] & SHADED))
        item_of(w->menu, M_FWD)[2] |= SHADED;
}

static void menu_open(struct ws *w, const uint32_t *m, int32_t mx, int32_t my)
{
    w->menu_x = mx, w->menu_y = my;
    uint32_t r[8] = { 0, A(m), (uint32_t)mx, (uint32_t)my };
    swi(XWimp_CreateMenu, r);
}

void browser_menu(struct ws *w, struct xwin *x)
{
    /* The link and the image are latched here. The pointer leaves the
     * page the moment the menu is up, and the page is told so, which ends
     * the hover. What the menu offers must be what the pointer was on
     * when MENU went down. */
    snprintf(w->hit_link, sizeof w->hit_link, "%s", x->link);
    snprintf(w->hit_image, sizeof w->hit_image, "%s", x->image);
    menu_build(w, x);
    w->menu_window = x->handle;
    menu_open(w, w->menu, (int32_t)w->poll[0] - 64, (int32_t)w->poll[1]);
}

_Static_assert(sizeof ((struct ws *)0)->menu_bar >= MENU_HEADER + 3 * MENU_ITEM,
               "the icon bar menu has three items");

void browser_icon_menu(struct ws *w)
{
    menu_head(w->menu_bar, "Browser", 208, 3);
    menu_item(w->menu_bar, MB_NEW, "New window", 0, 0);
    menu_item(w->menu_bar, MB_DOWNLOADS, "Downloads", MF_LINE, 0);
    menu_item(w->menu_bar, MB_QUIT, "Quit", MF_LAST, 0);
    w->menu_window = 0;
    menu_open(w, w->menu_bar, (int32_t)w->poll[0] - 64, 96 + 3 * 44);
}

/* ---- opening a file or an address ----------------------------------------------- */

/* a path as a URL: file:// and the characters a URL may not hold escaped */
static void file_url(const char *path, char *out, size_t max)
{
    static const char keep[] = "-_.~!$&'()*+,;=:@/";
    size_t n = (size_t)snprintf(out, max, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p && n + 4 < max; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            strchr(keep, *p))
            out[n++] = (char)*p;
        else
            n += (size_t)snprintf(out + n, max - n, "%%%02X", *p);
    }
    out[n] = 0;
}

/* The address inside a URL file (&B28: the address itself) or a URI file
 * (&F91: "URI<tab>100", the base, then the address).  The first thing in
 * it that looks like one. */
static int url_in_file(const char *host, char *out, size_t max)
{
    int fd = open(host, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    for (char *l = buf, *nl; l && *l; l = nl) {
        nl = strpbrk(l, "\r\n");
        if (nl)
            *nl++ = 0;
        while (*l == ' ' || *l == '\t')
            l++;
        if (!strncasecmp(l, "URI", 3) && (l[3] == '\t' || l[3] == ' '))
            continue;                                 /* the URI file's first line */
        if (strstr(l, "://") || !strncasecmp(l, "mailto:", 7) || !strncasecmp(l, "www.", 4)) {
            snprintf(out, max, "%s", l);
            return 1;
        }
    }
    return 0;
}

/* A file the desktop has handed us, dropped or double-clicked in a Filer
 * window, as an address. A URL file's is inside it. Any other file's is
 * where it is. 0 if it is not one we show. */
static int file_address(uint32_t type, const char *name, int dropped, char *out, size_t max)
{
    char host[1024];
    if (ros_hostfs_linux_path(name, host, sizeof host))
        return 0;                                     /* not on HostFS: we cannot show it */
    if (type == T_URL || type == T_URI)
        return url_in_file(host, out, max);
    switch (type) {
    case T_HTML:
        break;
    /* A text file or a picture is !Edit's and !Paint's when the Filer
     * opens it (Message_DataOpen is a broadcast: we claim only what
     * nothing else in the desktop shows).  Dropped on the browser, where
     * the reader meant us, it is a page. */
    case T_TEXT: case T_PNG: case T_JPEG: case T_GIF: case T_SVG:
        if (!dropped)
            return 0;
        break;
    default:
        return 0;
    }
    file_url(host, out, max);
    return 1;
}

/* ---- the save box ---------------------------------------------------------------- */

/* An address dragged out to a directory, as a URL file (&B28). This is the
 * drag to the Filer. Saved, it is a page that opens when it is
 * double-clicked, because Browser claims a URL file's Message_DataOpen
 * (above). The browser has no copy of a link's page, so the thing a link
 * stands for is still a download (Save link). It is the address that is
 * dragged. */

enum { S_SPRITE, S_NAME, S_OK, S_CANCEL };
#define SAVE_W 520
#define SAVE_H 200
#define SPRITE_ICON 0x1700311Au        /* sprite, centred, indirected; click/drag */
#define OK_BUTTON   0x1700313Du

static void save_icon(struct ws *w, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      uint32_t flags, char *text, char *val, uint32_t size)
{
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)w->save_win;
    b[1] = x0, b[2] = y0, b[3] = x1, b[4] = y1;
    b[5] = (int32_t)flags;
    b[6] = (int32_t)A(text), b[7] = val ? (int32_t)A(val) : -1, b[8] = (int32_t)size;
    uint32_t r[8] = { 0, A(w->block) };
    swi(XWimp_CreateIcon, r);
}

static void save_make(struct ws *w)
{
    if (w->save_win)
        return;
    strcpy(w->save_title, "Save address");
    strcpy(w->save_sprite, "file_b28");
    int32_t *b = (int32_t *)w->block;
    uint8_t *c = (uint8_t *)w->block;
    memset(w->block, 0, sizeof w->block);
    b[6] = -1;
    b[7] = (int32_t)(1u << 31 | 1u << 4 | 1u << 1 | 1u << 25 | 1u << 26);
    c[32] = 7, c[33] = 2, c[34] = 7, c[35] = 1;
    c[36] = 3, c[37] = 1, c[38] = 12, c[39] = 0;
    b[10] = 0, b[11] = -SAVE_H, b[12] = SAVE_W, b[13] = 0;
    b[14] = 0x27000119;                          /* the title: text, centred, indirected */
    b[16] = 1;                                   /* the Wimp's sprites */
    b[18] = (int32_t)A(w->save_title), b[19] = -1, b[20] = (int32_t)sizeof w->save_title;
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_CreateWindow, r))
        return;
    w->save_win = r[0];
    /* in the order of the S_ enum */
    save_icon(w, SAVE_W / 2 - 34, -84, SAVE_W / 2 + 34, -16, SPRITE_ICON,
              w->save_sprite, NULL, 12);
    save_icon(w, 16, -140, SAVE_W - 16, -96, WRITABLE | NEEDS_HELP,
              w->save_name, w->find_val, (uint32_t)sizeof w->save_name);
    save_icon(w, SAVE_W - 148, -192, SAVE_W - 16, -148, OK_BUTTON, w->t_ok, NULL,
              (uint32_t)sizeof w->t_ok);
    save_icon(w, 16, -192, 148, -148, OK_BUTTON, w->t_cancel, NULL,
              (uint32_t)sizeof w->t_cancel);
}

/* a leafname for an address: the last part of its path, or its host */
static void leaf_of_url(const char *url, char *out, size_t max)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *end = p + strcspn(p, "?#");
    const char *leaf = p;
    for (const char *q = p; q < end; q++)
        if (*q == '/' && q + 1 < end)
            leaf = q + 1;
    size_t n = 0;
    for (const char *q = leaf; q < end && n < max - 1 && n < 20; q++) {
        unsigned char ch = (unsigned char)*q;
        if (ch == '.')
            out[n++] = '/';                      /* RISC OS keeps "." for itself */
        else if (ch > ' ' && ch < 0x7F && !strchr(":*#$&@^%\"|/\\", ch))
            out[n++] = (char)ch;
        else if (n && out[n - 1] != '_')
            out[n++] = '_';
    }
    while (n && (out[n - 1] == '_' || out[n - 1] == '/'))
        n--;
    out[n] = 0;
    if (!n)
        snprintf(out, max, "Address");
}

static void save_open(struct ws *w, struct xwin *x, const char *url)
{
    if (!url || !*url)
        return;
    save_make(w);
    if (!w->save_win)
        return;
    snprintf(w->save_url, sizeof w->save_url, "%s", url);
    leaf_of_url(url, w->save_name, sizeof w->save_name);
    w->save_from = x->handle;
    /* under the pointer, as a menu's dialogue box opens */
    uint32_t pi[8] = { 0, A(w->block) };
    int32_t px = 640, py = 512;
    if (!swi(XWimp_GetPointerInfo, pi))
        px = (int32_t)w->block[0], py = (int32_t)w->block[1];
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)w->save_win;
    b[1] = px - 64, b[2] = py - SAVE_H - 40, b[3] = px - 64 + SAVE_W, b[4] = py - 40;
    b[5] = 0, b[6] = 0, b[7] = -1;
    uint32_t r[8] = { 0, A(w->block) };
    swi(XWimp_OpenWindow, r);
    uint32_t caret[8] = { w->save_win, S_NAME, 0, 0, 0xFFFFFFFFu,
                          (uint32_t)(int32_t)strlen(w->save_name) };
    swi(XWimp_SetCaretPosition, caret);
}

static void save_close(struct ws *w)
{
    if (!w->save_win)
        return;
    uint32_t r[8] = { 0, A(w->block) };
    w->block[0] = w->save_win;
    swi(XWimp_CloseWindow, r);
    w->save_from = 0;
}

/* the file written where the receiver says, and typed &B28 */
static os_error *save_write(struct ws *w, const char *path)
{
    size_t n = (size_t)snprintf(w->save_data, sizeof w->save_data, "%s\n", w->save_url);
    uint32_t r[8] = { 10, A(path), 0xB28u, 0, A(w->save_data), A(w->save_data) + (uint32_t)n };
    return swi(XOS_File, r);
}

static void report(struct ws *w, os_error *e)
{
    uint32_t r[8] = { A(e), 1, A(w->task_name) };
    swi(XWimp_ReportError, r);
}

/* The sprite dragged: DragASprite if the desktop is set to it, a dashed
 * box if not.  Every block a SWI is given lives in the workspace, which
 * is in the arena: a block on this C stack is nowhere RISC OS can read. */
static void save_drag(struct ws *w)
{
    int32_t *b = (int32_t *)w->block;
    b[0] = (int32_t)w->save_win;
    uint32_t st[8] = { 0, A(w->block) };
    if (swi(XWimp_GetWindowState, st))
        return;
    int32_t wx = b[1], wy = b[4];
    int32_t x0 = wx + SAVE_W / 2 - 34, y0 = wy - 84, x1 = wx + SAVE_W / 2 + 34, y1 = wy - 16;
    int32_t px = x0, py = y0;
    uint32_t pi[8] = { 0, A(&w->block[16]) };
    if (!swi(XWimp_GetPointerInfo, pi))
        px = (int32_t)w->block[16], py = (int32_t)w->block[17];
    /* the parent box: the screen, grown so the sprite can reach every edge */
    int32_t p0 = -(px - x0), q0 = -(py - y0);
    int32_t p1 = w->screen_w + (x1 - px), q1 = w->screen_h + (y1 - py);
    int32_t *box = (int32_t *)&w->block[24], *parent = (int32_t *)&w->block[28];
    box[0] = x0, box[1] = y0, box[2] = x1, box[3] = y1;
    parent[0] = p0, parent[1] = q0, parent[2] = p1, parent[3] = q1;
    w->save_sprited = 0;
    uint32_t cmos[8] = { 161, 28 };
    if (!swi(XOS_Byte, cmos) && (cmos[2] & 2)) {
        uint32_t d[8] = { 0xC5, 1, A(w->save_sprite), A(box), A(parent) };
        if (!swi(XDragASprite_Start, d))
            w->save_sprited = 1;
    }
    if (!w->save_sprited) {
        int32_t *blk = (int32_t *)&w->block[32];
        blk[0] = (int32_t)w->save_win, blk[1] = 5;
        blk[2] = x0, blk[3] = y0, blk[4] = x1, blk[5] = y1;
        blk[6] = p0, blk[7] = q0, blk[8] = p1, blk[9] = q1;
        uint32_t d[8] = { 0, A(blk) };
        swi(XWimp_DragBox, d);
    }
    w->save_dragging = 1;
}

/* the drag let go: Message_DataSave to whatever is under the pointer */
int browser_drag_end(struct ws *w)
{
    if (!w->save_dragging)
        return 0;
    w->save_dragging = 0;
    if (w->save_sprited) {
        uint32_t stop[8] = { 0 };
        swi(XDragASprite_Stop, stop);
        w->save_sprited = 0;
    }
    uint32_t pi[8] = { 0, A(w->block) };
    if (swi(XWimp_GetPointerInfo, pi) || w->block[3] == w->save_win)
        return 1;                                /* dropped on the box itself: nothing */
    uint32_t win = w->block[3], icon = w->block[4];
    int32_t px = (int32_t)w->block[0], py = (int32_t)w->block[1];
    char leaf[64];
    snprintf(leaf, sizeof leaf, "%s", w->save_name);
    memset(w->poll, 0, sizeof w->poll);
    size_t n = strlen(leaf) + 1;
    memcpy((char *)w->poll + 44, leaf, n);
    w->poll[0] = (uint32_t)((44 + n + 3) & ~(size_t)3);
    w->poll[4] = MESSAGE_DATASAVE;
    w->poll[5] = win, w->poll[6] = icon;
    w->poll[7] = (uint32_t)px, w->poll[8] = (uint32_t)py;
    w->poll[9] = (uint32_t)strlen(w->save_url) + 1;
    w->poll[10] = 0xB28u;
    uint32_t r[8] = { 18, A(w->poll), win, icon };
    if (swi(XWimp_SendMessage, r))
        return 1;
    w->save_ref = w->poll[2];
    return 1;
}

/* OK, with a full path typed in: saved there and no one told */
static void save_now(struct ws *w)
{
    if (!strchr(w->save_name, '.')) {
        report(w, ros_error(0xB0, "To save, drag the icon to a directory display"));
        return;
    }
    os_error *e = save_write(w, w->save_name);
    if (e)
        report(w, e);
    else
        save_close(w);
}

/* the save box's own close icon */
int browser_save_close(struct ws *w, uint32_t handle)
{
    if (!w->save_win || handle != w->save_win)
        return 0;
    save_close(w);
    return 1;
}

int browser_save_click(struct ws *w)
{
    if (!w->save_win || w->poll[3] != w->save_win)
        return 0;
    switch ((int32_t)w->poll[4]) {
    case S_SPRITE: save_drag(w); break;
    case S_OK: save_now(w); break;
    case S_CANCEL: save_close(w); break;
    default: break;
    }
    return 1;
}

int browser_save_key(struct ws *w)
{
    if (!w->save_win || w->poll[0] != w->save_win)
        return 0;
    if (w->poll[6] == 13)
        save_now(w);
    else if (w->poll[6] == 27)
        save_close(w);
    else {
        uint32_t r[8] = { w->poll[6] };
        swi(XWimp_ProcessKey, r);
    }
    return 1;
}

/* ---- the menu's selections ------------------------------------------------------ */

static void selection_page(struct ws *w, struct xwin *x, int32_t item, int32_t sub)
{
    switch (item) {
    case M_BACK: browser_tell(x, "back"); break;
    case M_FWD: browser_tell(x, "forward"); break;
    case M_RELOAD: browser_tell(x, "reload"); break;
    case M_STOP: browser_tell(x, "stop"); break;
    case M_HOME: browser_tell(x, "go %s", START_PAGE); break;
    case M_LINK:
        if (!w->hit_link[0])
            break;
        if (sub == ML_NEW)
            browser_new_window(w, w->hit_link);
        else if (sub == ML_SAVE)
            browser_tell(x, "save %s", w->hit_link);
        else if (sub == ML_ADDRESS)
            save_open(w, x, w->hit_link);
        break;
    case M_IMAGE:
        if (!w->hit_image[0])
            break;
        if (sub == ML_NEW)
            browser_new_window(w, w->hit_image);
        else if (sub == ML_SAVE)
            browser_tell(x, "save %s", w->hit_image);
        break;
    case M_SAVE: browser_tell(x, "save"); break;
    case M_ADDRESS: save_open(w, x, x->url); break;
    case M_FIND:
        /* Nothing to look for: the last search ends, and its highlighting
         * with it. This is not done when the menu shuts. The menu shuts
         * after every use, and a reader wants to see what was found. */
        if (sub == MF_TEXT && !w->find[0])
            browser_tell(x, "findstop");
        else if (sub == MF_TEXT)
            browser_tell(x, "find %s", w->find);
        else if (sub == MF_NEXT)
            browser_tell(x, "findnext");
        else if (sub == MF_PREV)
            browser_tell(x, "findprev");
        break;
    case M_ZOOM:
        /* taken here as well as sent, so that a menu kept up with ADJUST
         * shows the tick on the size just chosen */
        if (sub >= 0 && sub < 6) {
            x->zoom = zooms[sub];
            browser_tell(x, "zoom %d", zooms[sub]);
        }
        break;
    case M_NEW: browser_new_window(w, NULL); break;
    case M_DOWNLOADS: open_downloads(w); break;
    default: break;
    }
}

void browser_selection(struct ws *w)
{
    int32_t item = (int32_t)w->poll[0], sub = (int32_t)w->poll[1];
    struct xwin *x = w->menu_window ? wwin_by_handle(w, w->menu_window) : NULL;
    if (!w->menu_window) {
        if (item == MB_NEW)
            browser_new_window(w, NULL);
        else if (item == MB_DOWNLOADS)
            open_downloads(w);
        else if (item == MB_QUIT) {
            browser_quit(w);
            return;                                   /* no menu to keep up */
        }
    } else if (x && x->bfd >= 0) {
        selection_page(w, x, item, sub);
    }
    /* Adjust: the menu stays up, with what has changed in it */
    uint32_t r[8] = { 0, A(w->block) };
    if (swi(XWimp_GetPointerInfo, r) || !(w->block[2] & BUTTON_ADJUST))
        return;
    if (!w->menu_window)
        browser_icon_menu(w);
    else if (x) {
        menu_build(w, x);
        menu_open(w, w->menu, w->menu_x, w->menu_y);
    }
}

/* ---- the keys ------------------------------------------------------------------- */

/* a key over the page: the browser's few, and the page has the rest */
int browser_key(struct ws *w, struct xwin *x, uint32_t code)
{
    switch (code) {
    case 27:                                                    /* Escape */
        browser_tell(x, "stop");
        browser_tell(x, "findstop");                            /* and what was found */
        return 1;
    case 0x183: browser_tell(x, "save"); return 1;              /* F3 */
    case 0x185: browser_tell(x, "reload"); return 1;            /* F5 */
    case 12: caret_to_url(w, x); return 1;                      /* Ctrl-L */
    case 14: browser_new_window(w, NULL); return 1;                     /* Ctrl-N */
    default: return 0;
    }
}

/* a key in the address field */
int browser_bar_key(struct ws *w, struct xwin *x, uint32_t code)
{
    if (code == 13) {                                           /* Return: go there */
        browser_tell(x, "go %s", x->url);
        caret_to_page(w, x);
        return 1;
    }
    if (code == 27) {                                           /* Escape: as it was */
        browser_tell(x, "state");
        caret_to_page(w, x);
        return 1;
    }
    return 0;
}

/* ---- the Wimp's messages -------------------------------------------------------- */

static void reply_ack(struct ws *w, uint32_t action)
{
    w->poll[3] = w->poll[2];                                    /* your_ref: theirs */
    w->poll[4] = action;
    uint32_t r[8] = { 17, A(w->poll), w->poll[1] };
    swi(XWimp_SendMessage, r);
}

static void help_reply(struct ws *w, const char *text)
{
    snprintf(w->help, sizeof w->help, "%s", text);
    size_t n = strlen(w->help) + 1;
    if (n > sizeof w->poll - 24)
        n = sizeof w->poll - 24, w->help[n - 1] = 0;
    memcpy((char *)w->poll + 20, w->help, n);
    w->poll[0] = (uint32_t)((20 + n + 3) & ~(size_t)3);
    w->poll[3] = w->poll[2];
    w->poll[4] = MESSAGE_HELPREPLY;
    uint32_t r[8] = { 17, A(w->poll), w->poll[1] };
    swi(XWimp_SendMessage, r);
}

static const char *bar_help(int32_t icon)
{
    switch (icon) {
    case B_BACK: return "This is the Back button.|MClick SELECT to go back a page.";
    case B_FWD: return "This is the Forward button.|MClick SELECT to go forward a page.";
    case B_RELOAD: return "This is the Reload button.|MClick SELECT to fetch this page again.";
    case B_STOP: return "This is the Stop button.|MClick SELECT to give up fetching this page.";
    case B_HOME: return "This is the Home button.|MClick SELECT to go to the start page.";
    case B_URL: return "This is the address field.|MType an address, or words to search for, "
                       "and press RETURN.";
    case B_DOWNLOADS: return "This is the Downloads button.|MClick SELECT to open the directory "
                             "that files you save go into.";
    case B_STATUS: return "This is the status line.|MIt shows what the pointer is over, and how "
                          "the page is getting on.";
    default: return NULL;
    }
}

/* a message of ours: 1 if we dealt with it */
int browser_message(struct ws *w)
{
    switch (w->poll[4]) {
    case MESSAGE_DATASAVEACK: {
        /* the receiver has named a file: written there, and it is told */
        if (!w->save_ref || w->poll[3] != w->save_ref)
            return 0;
        w->save_ref = 0;
        const char *path = (const char *)&w->poll[11];
        os_error *e = save_write(w, path);
        if (e) {
            report(w, e);
            return 1;
        }
        w->poll[3] = w->poll[2];
        w->poll[4] = MESSAGE_DATALOAD;
        w->poll[9] = (uint32_t)strlen(w->save_url) + 1;
        uint32_t r[8] = { 18, A(w->poll), w->poll[1] };
        swi(XWimp_SendMessage, r);
        save_close(w);
        return 1;
    }
    case MESSAGE_DATALOADACK:
        return w->save_win != 0;                 /* the receiver has it: nothing to do */
    case MESSAGE_DATALOAD: {
        /* a file dropped on a browser window or on the icon bar */
        struct xwin *x = wwin_by_handle(w, w->poll[5]);
        if (!x)
            x = wwin_by_bar(w, w->poll[5]);
        int ours = (x && x->bfd >= 0) ||
                   ((int32_t)w->poll[5] == -2 && w->icon_handle &&
                    (int32_t)w->poll[6] == w->icon_handle);
        if (!ours)
            return 0;
        char url[URL_LEN];
        if (!file_address(w->poll[10], (const char *)&w->poll[11], 1, url, sizeof url))
            return 0;
        reply_ack(w, MESSAGE_DATALOADACK);
        if (x && x->bfd >= 0)
            browser_tell(x, "go %s", url);
        else
            browser_new_window(w, url);
        return 1;
    }
    case MESSAGE_DATAOPEN: {
        /* a page double-clicked in a Filer window: nothing else in the
         * WPE edition shows one, so we claim it */
        char url[URL_LEN];
        if (!browser_present() ||
            !file_address(w->poll[10], (const char *)&w->poll[11], 0, url, sizeof url))
            return 0;
        reply_ack(w, MESSAGE_DATALOADACK);
        browser_new_window(w, url);
        return 1;
    }
    case MESSAGE_HELPREQUEST: {
        uint32_t handle = w->poll[5];
        int32_t icon = (int32_t)w->poll[6];
        if ((int32_t)handle == -2 && w->icon_handle && icon == w->icon_handle) {
            help_reply(w, "This is the Browser icon.|MClick SELECT to open a browser window."
                          "|MClick MENU for the menu.|MDrag a page here to see it.");
            return 1;
        }
        struct xwin *x = wwin_by_bar(w, handle);
        if (x) {
            const char *h = bar_help(icon);
            if (h)
                help_reply(w, h);
            return h != NULL;
        }
        x = wwin_by_handle(w, handle);
        if (x && x->bfd >= 0) {
            help_reply(w, "This is a web page.|MClick MENU for what you can do with it, "
                          "and with the link or picture under the pointer.");
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}
