/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* wwin.h -- WaylandWindows' two halves: the window manager (waylandwin.c)
 * and the browser's RISC OS side (browser.c). It declares their workspace,
 * their windows and what each half asks of the other.
 */
#ifndef ROSGD_WWIN_H
#define ROSGD_WWIN_H

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"

/* A close-on-exec, non-blocking AF_UNIX stream socket: the compositor's
 * own and a browser's are both made this way.  Linux takes the two flags
 * in socket()'s type; a host that has not got them (macOS, where the
 * hosted build compiles this module although nothing in it runs there)
 * wants fcntl afterwards. */
static inline int wwin_unix_socket(void)
{
#ifdef SOCK_CLOEXEC
    return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int fl;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        (fl = fcntl(fd, F_GETFL, 0)) < 0 ||
        fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    return fd;
#endif
}

/* ---- constants ------------------------------------------------------------- */

#define TASK_WORD   0x4B534154u        /* "TASK" */
#define XWimp_Extend 0x600FBu
#define SURFACE_BIND 0x5200u
#define SURFACE_EXTERNAL 2u

#define NULL_REASON      0u
#define OPEN_REQUEST     2u
#define CLOSE_REQUEST    3u
#define POINTER_LEAVING  4u
#define POINTER_ENTERING 5u
#define MOUSE_CLICK      6u
#define KEY_PRESSED      8u
#define MENU_SELECTION   9u
#define SCROLL_REQUEST   10u
#define LOSE_CARET       11u
#define GAIN_CARET       12u
#define USER_DRAG_BOX    7u
#define USER_MESSAGE     17u
#define USER_MESSAGE_RECORDED 18u

#define MESSAGE_QUIT        0u
#define MESSAGE_DATASAVE    1u
#define MESSAGE_DATASAVEACK 2u
#define MESSAGE_DATALOAD    3u
#define MESSAGE_DATALOADACK 4u
#define MESSAGE_DATAOPEN    5u
#define MESSAGE_HELPREQUEST 0x502u
#define MESSAGE_HELPREPLY   0x503u

#define BUTTON_SELECT 4u
#define BUTTON_MENU   2u
#define BUTTON_ADJUST 1u

/* Linux's buttons (linux/input-event-codes.h) */
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

#define WINDOWS   16
#define TITLE_LEN 128

/* A browser's window (rosgd-browser, ports/browser): a toolbar along the
 * top, a window nested in it, and the page below it.  Two rows: the
 * buttons and the address above, what the page has to say below. */
#define TOOLBAR    108                 /* OS units */
#define BAR_ROW1_Y0 (-60)
#define BAR_ROW1_Y1 (-8)
#define BAR_ROW2_Y0 (-102)
#define BAR_ROW2_Y1 (-64)
#define URL_LEN    1024
#define STATUS_LEN 128
enum { B_BACK, B_FWD, B_RELOAD, B_STOP, B_HOME, B_URL, B_DOWNLOADS, B_STATUS, B_ICONS };
#define BUTTON   0x1700313Du           /* text, border, centred, filled, indirected; click */
#define WRITABLE 0x0700F135u           /* text, border, v-centred, filled, indirected; writable */
#define DISPLAY  0x17000131u           /* text, v-centred, filled, indirected */
#define NEEDS_HELP (1u << 7)
#define SHADED   (1u << 22)
#define BROWSER_SOCKS "/wpe/run/rosgd-browser"   /* rosgd-browser's, seen from /init */
#define BROWSER_PROG  "/wpe/usr/bin/rosgd-browser"

/* the menus: a block is a header and 24 bytes an item (PRM 3-212) */
#define MENU_HEADER 28u
#define MENU_ITEM   24u
#define MENU_WORDS(items) ((MENU_HEADER + (items) * MENU_ITEM) / 4)
#define MF_TICK     1u
#define MF_LINE     2u                 /* a dotted line below */
#define MF_WRITABLE 4u
#define MF_LAST     0x80u

/* ---- the workspace --------------------------------------------------------- */

struct xwin {
    uint32_t id;                       /* the compositor's; 0 a free slot */
    uint32_t handle;                   /* the Wimp's */
    int32_t cfg_w, cfg_h;              /* the size, pixels, last agreed */
    char title[TITLE_LEN];
    /* a browser's (0 and -1 for any other program's) */
    int32_t inset;                     /* OS units above the page: the toolbar's */
    uint32_t bar;                      /* the toolbar, nested */
    int32_t bar_w;                     /* the width its icons are laid out for */
    int bfd;                           /* rosgd-browser's control socket */
    char url[URL_LEN];
    char status[STATUS_LEN];           /* the status line, as it shows */
    char msg[STATUS_LEN];              /* what the browser last said: loading, a download */
    int msg_sticky;                    /* and whether progress may write over it */
    char link[URL_LEN];                /* under the pointer: the link, the image */
    char image[URL_LEN];
    int zoom;                          /* per cent */
    char bin[4096];
    uint32_t binlen;
    int32_t scroll_y, doc_h;           /* the page's, pixels: the scroll bar's */
    int extent_new;                    /* set since the window was last opened */
    uint32_t dragged_at;               /* when the scroll bar last moved the page */
};

struct ws {
    uint32_t task_handle;
    uint32_t pollword;
    uint8_t mode_changed;
    int fd;                            /* the socket, or -1 */
    uint32_t next_try;                 /* when to try the socket again */
    uint32_t xeig, yeig;
    int32_t screen_w, screen_h;        /* OS units */

    struct xwin win[WINDOWS];
    uint32_t inside;                   /* the window under the pointer, or 0 */
    uint32_t held, held_window;        /* buttons down since a click in it */
    uint32_t caret;                    /* the window with our caret, or 0 */
    int32_t last_x, last_y;            /* the pointer as last sent */
    int cascade;
    int told_full;                     /* the "no room" error has been given once */

    char in[4096];
    uint32_t inlen;

    uint32_t poll[64];
    uint32_t block[64];
    uint32_t messages[8];
    char t_back[8], t_fwd[8], t_reload[8], t_stop[8], t_home[8], t_dl[12];
    char t_ok[8], t_cancel[8];
    char cli[160];                     /* a *command: short ones only (OS_CLI's 1024) */
    char task_name[20];
    char command[20];

    /* the browser's icon on the icon bar, and its menus */
    int icon_up;                       /* whether there is one. Not icon_handle,
                                          which may legitimately be 0 */
    int32_t icon_handle;
    uint32_t new_at;                   /* when a window was last asked for */
    int want_icon;                     /* a *command asked for the icon: the TASK makes it */
    char icon_sprite[12];
    uint32_t menu_window;              /* whose window the open menu is, or 0 */
    char hit_link[URL_LEN];            /* what was under the pointer when it opened: */
    char hit_image[URL_LEN];           /* a menu acts on that, not on where it is now */
    int32_t menu_x, menu_y;            /* where it was opened (Adjust keeps it up) */
    uint32_t menu[MENU_WORDS(14)];
    uint32_t menu_link[MENU_WORDS(3)];
    uint32_t menu_image[MENU_WORDS(2)];
    uint32_t menu_find[MENU_WORDS(3)];
    uint32_t menu_zoom[MENU_WORDS(6)];
    uint32_t menu_bar[MENU_WORDS(3)];
    char find[64];                     /* the Find menu's writable field */
    char find_val[8];
    char help[236];

    /* the save box: an address dragged out to a directory, as a URL file */
    uint32_t save_win;                 /* made when first wanted; 0 until then */
    uint32_t save_from;                /* the browser window it belongs to */
    uint32_t save_ref;                 /* my_ref of the Message_DataSave sent */
    int save_dragging, save_sprited;
    char save_title[20];
    char save_sprite[12];
    char save_name[256];               /* the writable: a leafname, or a path */
    char save_url[URL_LEN];            /* the address it saves */
    char save_data[URL_LEN + 2];       /* what goes in the file */
};

/* ---- what each half asks of the other ---------------------------------------- */

static inline uint32_t A(const void *p)
{
    return ros_addr(p);
}

static inline os_error *swi(uint32_t number, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

static inline uint32_t now(void)
{
    uint32_t r[8] = { 0 };
    return swi(XOS_ReadMonotonicTime, r) ? 0 : r[0];
}

/* waylandwin.c's */
struct xwin *wwin_by_id(struct ws *w, uint32_t id);
struct xwin *wwin_by_handle(struct ws *w, uint32_t handle);
struct xwin *wwin_by_bar(struct ws *w, uint32_t handle);
void wwin_cli(struct ws *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void wwin_say(struct ws *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* a Linux program in the WPE edition's root, on its own (*WaylandRun's):
 * 0, or an errno; 127 if there is no root to run it in */
int wwin_wperun(char *const words[]);

/* browser.c's */
void browser_messages(uint32_t *list, int max);
int browser_present(void);
void browser_started(struct ws *w);    /* a browser is running: its icon belongs on the bar */
void browser_mode_change(struct ws *w);
void browser_aliases(struct ws *w);
int browser_open(const char *what, uint32_t type);
int browser_is_url(const char *what);
void browser_icon_gone(struct ws *w);
int browser_icon_click(struct ws *w);
int browser_connect(struct xwin *x, int pid);
void browser_tell(struct xwin *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void browser_receive(struct ws *w, struct xwin *x);
void browser_closed(struct ws *w, struct xwin *x);
void browser_bar_create(struct ws *w, struct xwin *x);
void browser_bar_open(struct ws *w, struct xwin *x);
void browser_bar_click(struct ws *w, struct xwin *x);
void browser_scrolled(struct ws *w, struct xwin *x, int32_t y);
int browser_key(struct ws *w, struct xwin *x, uint32_t code);
int browser_bar_key(struct ws *w, struct xwin *x, uint32_t code);
void browser_menu(struct ws *w, struct xwin *x);
void browser_icon_menu(struct ws *w);
void browser_new_window(struct ws *w, const char *url);
void browser_selection(struct ws *w);
int browser_message(struct ws *w);
int browser_save_close(struct ws *w, uint32_t handle);
int browser_save_click(struct ws *w);
int browser_save_key(struct ws *w);
int browser_drag_end(struct ws *w);

#endif
