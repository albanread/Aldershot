/* cmdwin.c -- the command window, where a program that is not a Wimp
 * task writes text in the desktop.
 *
 * The window is dormant, pending or active.  Giving it a title makes it
 * pending, and WrchV is claimed.  The first character written then opens
 * the window.  It is drawn at once, because no Wimp_Poll will follow, and
 * a text window is set inside it.  Closing an active window asks for
 * SPACE first.
 *
 * With the GPU presenter the screen is never shown, only the windows'
 * surfaces (design 37, Sprint 6).  The program is given a virtual display
 * of its own instead, shown in a display window: at its first character,
 * in the screen's own mode (what it read of the screen before printing
 * stays true), or at its first mode change, in that mode, which leaves the
 * desktop's mode alone.  Below, "display windows". */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/platform.h"
#include "rosgd/screen.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "wimp.h"

#define WRCHV 3u
#define IF_INDIRECT (1u << 8)

static struct wimp_err *er(void)
{
    return &wimp_ws()->err;
}

static void wr(uint8_t *b, unsigned off, uint32_t v)
{
    memcpy(b + off, &v, 4);
}

/* Writes VDU bytes through the workspace, as OS_WriteN needs arena memory */
static void vdu(const uint8_t *b, uint32_t n)
{
    uint8_t *buf = wimp_ws()->cmd_vdu;
    if (n > sizeof wimp_ws()->cmd_vdu)
        n = sizeof wimp_ws()->cmd_vdu;
    memcpy(buf, b, n);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(buf), c.r[1] = n;
    ros_swi(&c, XOS_WriteN);
}

/* ---- the window ----------------------------------------------- */

void wimp_cmdwin_make(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w->cmd_handle && wimp_window(w->cmd_handle))
        return;
    uint8_t def[88];
    if (!wimp_template_load("command", def))
        return;
    os_error *e = NULL;
    struct wimp_window *win = wimp_create_system(def, &e);
    if (win)
        w->cmd_handle = win->handle;
}

static void text_size(int32_t *tw, int32_t *th)
{
    /* TCharSpaceX and Y, in OS units (readvduvars2) */
    struct ros_cpu c;
    struct wimp_ws *w = wimp_ws();
    uint32_t *in = (uint32_t *)w->cmd_vdu, *out = in + 4;
    in[0] = 0xA9u, in[1] = 0xAAu, in[2] = 0xFFFFFFFFu, out[0] = out[1] = 8;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(in), c.r[1] = ros_addr(out);
    ros_swi(&c, XOS_ReadVduVariables);
    *tw = (int32_t)(out[0] << w->xeig);
    *th = (int32_t)(out[1] << w->yeig);
}

/* resizecommandwindow */
static void resize(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    int32_t tw, th;
    text_size(&tw, &th);
    int32_t H = w->screen_h, W = w->screen_w;
    int32_t Hr = (H + th) & ~th;
    int32_t h = ((Hr >> 1) - (Hr >> 4) + (Hr >> 6) - (Hr >> 8)) / th;
    h = (h & ~1) * th;
    if ((uint32_t)h > 1024)
        h = 1024;
    int32_t Wr = W & ~th;
    int32_t wd = (Wr - (Wr >> 2)) / tw;
    wd = (wd & ~1) * tw;
    if ((uint32_t)wd > 1280)
        wd = 1280;
    wr(win->def, 48, (uint32_t)wd);
    wr(win->def, 44, (uint32_t)(-12 - h));
    int32_t cy = Hr >> 1, cx = Wr >> 1;
    struct wimp_box v = { cx - (wd >> 1), cy - (h >> 1), cx + (wd >> 1), cy + (h >> 1) + 12 };
    win->s[REQ].vis = v;
    wr(win->def, 0, (uint32_t)v.x0);
    wr(win->def, 4, (uint32_t)v.y0);
    wr(win->def, 8, (uint32_t)v.x1);
    wr(win->def, 12, (uint32_t)v.y1);
}

/* commandtextwindow: a text window (VDU 28) inside the visible area,
 * then VDU 4, the cursor turned off, and scroll protection */
static void text_window(struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    int32_t tw, th;
    text_size(&tw, &th);
    struct wimp_box v = win->s[REQ].vis;
    int32_t H = w->screen_h;
    uint8_t b[5 + 1 + 10 + 10];
    b[0] = 28;
    b[1] = (uint8_t)((v.x0 + tw - 1) / tw);
    b[2] = (uint8_t)((H - 1 - v.y0 - (th - 1)) / th);
    b[3] = (uint8_t)((v.x1 - (tw - 1)) / tw);
    b[4] = (uint8_t)((H - 1 - v.y1 + th - 1) / th);
    static const uint8_t tail[21] = { 4, 23, 1, 0, 0, 0, 0, 0, 0, 0, 0, 23, 16, 1, 0xFE, 0, 0, 0, 0, 0, 0 };
    memcpy(b + 5, tail, sizeof tail);
    w->err.commandflag |= CF_WIMPVDU;
    vdu(b, sizeof b);
    w->err.commandflag &= ~CF_WIMPVDU;
}

/* ---- the keys (restorekeys_withescape, resetkeys_withescape) ---------------------- */

static void fx(uint32_t a, uint32_t b)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = a, c.r[1] = b, c.r[2] = 0;
    ros_swi(&c, XOS_Byte);
}

/* resetkeycodes and restorekeycodes.  The Escape setting (*FX 229) is
 * left alone while a single-tasking program exists.  Only the command
 * window's "withescape" calls change it then (s/Wimp01:5851-5875,
 * s/Wimp02:2715). */
static void keys_fx(int restore, int escape)
{
    struct wimp_ws *w = wimp_ws();
    static const uint8_t codes[13] = { 4, 219, 221, 222, 223, 224, 225, 226, 227, 228, 9, 10, 229 };
    static const uint8_t wimp[13] = { 2, 0x8A, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 1 };
    for (int i = 0; i < 13; i++)
        if (i < 12 || escape)
            fx(codes[i], restore ? w->oldfx[i] : wimp[i]);
}

static void keys(int restore)
{
    keys_fx(restore, 1);
}

static int mywrch(struct ros_cpu *s, uint32_t r12);
static int dispwin_text(void);
static void dispwin_close(int prompt);
static void basic_text_in_graphtask(void);

/* releasewrchvpremodechange: on Service_ModeChanging, while the window
 * is pending and not suspended, WrchV is released and the window becomes
 * active without being drawn */
void wimp_cmdwin_mode_changing(void)
{
    struct wimp_err *e = er();
    if (e->commandflag & CF_SUSPENDED)
        return;
    if (e->commandflag != CF_PENDING)
        return;
    ros_vector_release_native(WRCHV, mywrch, 0);
    e->commandflag = CF_ACTIVE;
}

void wimp_keys_restore(void)
{
    keys_fx(1, wimp_ws()->singletask < 0);
}

/* ---- WrchV: the window opens on the first character ---------------------------------------------------------------------------- */

static int mywrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    if (e->commandflag & (CF_SUSPENDED | CF_WIMPVDU))
        return ROS_VECTOR_PASS;
    if (ros_streams_own())
        return ROS_VECTOR_PASS;                 /* SSH output goes to its terminal (#164) */
    ros_vector_release_native(WRCHV, mywrch, 0);
    e->commandflag = CF_ACTIVE;
    uint32_t ch = s->r[0] & 0xFFu;
    if (ch == 22)
        return ROS_VECTOR_PASS;                 /* a display window comes with the mode, if any */
    if (w->disp_armed && dispwin_text())
        return ROS_VECTOR_PASS;                 /* the character goes to the program's display */
    if (ch == 16)
        return ROS_VECTOR_PASS;
    struct wimp_window *win = wimp_window(w->cmd_handle);
    if (!win)
        return ROS_VECTOR_PASS;
    struct ros_cpu save = *s;
    e->commandflag |= CF_WIMPVDU;
    uint8_t b[32];
    wr(b, 0, win->handle);
    memcpy(b + 4, &win->s[REQ].vis, 16);
    wr(b, 20, (uint32_t)win->s[REQ].scx);
    wr(b, 24, (uint32_t)win->s[REQ].scy);
    wr(b, 28, 0xFFFFFFFFu);
    wimp_open_own(win, b);
    wimp_flush();
    wimp_redraw_now(win);
    e->commandflag &= ~CF_WIMPVDU;
    text_window(win);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = win->def[34] & 0x7Fu;
    ros_swi(&c, XWimp_TextColour);
    ros_cpu_enter(&c);
    c.r[0] = win->def[35] | 0x80u;
    ros_swi(&c, XWimp_TextColour);
    static const uint8_t ff[2] = { 12, 15 };
    vdu(ff, 2);
    *s = save;
    return ROS_VECTOR_PASS;                     /* the character passed on */
}

/* ---- Wimp_CommandWindow ------------------------------------------------------------------ */

static void flush_kbd_mouse(uint32_t *buttons)
{
    fx(15, 1);
    fx(21, 9);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_Mouse);
    *buttons = c.r[2];
}

/* waitforkey: waits for a key or a new button press */
static void wait_key(void)
{
    uint32_t old;
    flush_kbd_mouse(&old);
    for (;;) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_Mouse);
        if (c.r[2] & ~old)
            return;
        old = c.r[2];
        ros_cpu_enter(&c);
        c.r[0] = 0x81, c.r[1] = 0, c.r[2] = 0;
        ros_swi(&c, XOS_Byte);
        if (c.r[1] != 0xFFu || !c.c) {          /* pollforkey: R1 = &FF means no key */
            if (c.c)
                fx(124, 0);                     /* Escape is cleared */
            return;
        }
        ros_idle(2);
    }
}

os_error *wimp_command_window(uint32_t r0)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_err *e = er();
    struct wimp_window *win = w->cmd_handle ? wimp_window(w->cmd_handle) : NULL;
    if (!win)
        return wimp_error(E_BAD_HANDLE);
    if (r0 == 0 || r0 == 0xFFFFFFFFu) {         /* 0 or -1: close the window */
        uint32_t was = e->commandflag;
        if (was == CF_DORMANT)
            return NULL;
        e->commandflag = CF_DORMANT;
        w->disp_armed = 0;
        if (w->disp_vd) {
            keys(0);
            dispwin_close(r0 == 0 && was == CF_ACTIVE);
            return NULL;
        }
        char *cmd = (char *)w->scratch + 448;
        strcpy(cmd, "Exec");
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(cmd);
        ros_swi(&c, XOS_CLI);
        keys(0);
        ros_vector_release_native(WRCHV, mywrch, 0);
        if (was == CF_ACTIVE) {
            if (r0 == 0) {
                ros_cpu_enter(&c);
                ros_swi(&c, XOS_NewLine);
                uint32_t text = wimp_message("Space");
                for (uint32_t p = text; p && ros_ld8(p) >= 32; p++) {
                    ros_cpu_enter(&c);
                    c.r[0] = ros_ld8(p);
                    ros_swi(&c, XOS_WriteC);
                }
                ros_cpu_enter(&c);
                ros_swi(&c, XOS_NewLine);
                wait_key();
            }
            static const uint8_t five = 5;
            vdu(&five, 1);                      /* VDU 5 puts the text cursor away */
        }
        wimp_close_system(win);
        return NULL;
    }
    if (r0 == 1) {                              /* 1: active, and nothing drawn */
        ros_vector_release_native(WRCHV, mywrch, 0);
        keys(1);
        e->commandflag = CF_ACTIVE;
        return NULL;
    }
    /* Otherwise R0 points to a title, which is used by reference */
    wr(win->def, 56, ros_ld32(ros_addr(win->def + 56)) | IF_INDIRECT);
    wr(win->def, 72, r0);
    wr(win->def, 76, 0xFFFFFFFFu);
    wr(win->def, 80, 1);
    resize(win);
    text_window(win);
    ros_vector_claim_native(WRCHV, mywrch, 0);
    keys(1);
    e->commandflag = CF_PENDING;
    return NULL;
}

void wimp_swi_CommandWindow(struct ros_cpu *s)
{
    wimp_damage_full();                         /* block v2: VDU output the Wimp will not see */
    os_error *e = wimp_command_window(s->r[0]);
    if (e)
        wimp_fail(s, e);
    else
        s->v = 0;
}

/* With the presenter, a BASIC text file run from the desktop (a double
 * click on one of the Examples) runs in a GraphTask window: a task window
 * with a virtual display, so the desktop goes on, the window can be moved
 * and closed, and the keys go to the program while it has the caret.  As a
 * single-tasking program in a display window it held the desktop until it
 * ended (16 Oct: Mandel, its first quarter drawn and then nothing to be
 * done but wait).  !GraphTask's !Run is run, not *GraphTask: that is
 * WimpTask, and a program the Wimp started is not a Wimp task to start
 * another.  It becomes GraphTask, or hands the command to the one that is
 * running.  FileSwitch's own alias is replaced, never one set since. */
static void basic_text_in_graphtask(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    static const char var[] = "Alias$@RunType_FD1", was[] = "Basic -quit \"%0\" %*1";
    static const char now[] = "Run Resources:$.Apps.!GraphTask.!Run -cmd Basic -quit \"%0\" %*1";
    struct wimp_ws *w = wimp_ws();
    char *name = (char *)w->scratch + 400;
    memcpy(name, var, sizeof var);
    uint8_t *val = w->disp_title;               /* free until a display window has a title */
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(name), c.r[1] = ros_addr(val), c.r[2] = sizeof w->disp_title - 1;
    c.r[3] = 0, c.r[4] = 3;                     /* expanded */
    ros_swi(&c, XOS_ReadVarVal);
    if (c.v || c.r[2] != sizeof was - 1 || memcmp(val, was, sizeof was - 1))
        return;
    memcpy(val, now, sizeof now);
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(name), c.r[1] = ros_addr(val), c.r[2] = sizeof now - 1;
    c.r[3] = 0, c.r[4] = 4;                     /* a literal string */
    ros_swi(&c, XOS_SetVarVal);
}

/* Wimp_StartTask makes the window pending, with the command as its title */
void wimp_command_pending(uint32_t command)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t n = 0;
    while (n < sizeof w->cmd_title - 1 && ros_ld8(command + n) >= 32)
        n++;
    for (uint32_t i = 0; i < n; i++)
        w->cmd_title[i] = (uint8_t)ros_ld8(command + i);
    w->cmd_title[n] = 0;
    wimp_command_window(ros_addr(w->cmd_title));
    w->disp_armed = ros_display_v2_shown() && !ros_streams_own();
    if (w->disp_armed)
        basic_text_in_graphtask();
}

/* ---- display windows (design 37, Sprint 6) -----------------------------------------------
 *
 * A program the desktop started that is not a Wimp task, on a desktop the
 * presenter composites, gets a virtual display (runtime/vdu/vdisplay.c)
 * and the Wimp shows it in a window of its own, bound to the display as
 * GraphTask's are (surface.c).  No Wimp_Poll comes while the program runs:
 * the display's changes are shown and published at the VSync
 * (vsync_present), and the window is opened and sized again at the
 * program's next SWI (the foreground hook), where nothing is under way.
 * The keys and the mouse go to the program, as to any single-tasking one.
 * At its end the window's title asks for SPACE, then the window closes and
 * the display goes.  The desktop's own mode is never changed. */

int wimp_dispwin_on(void)
{
    struct wimp_ws *w = wimp_ws();
    return w && w->disp_vd;
}

/* Whether the program running now may have a display window */
static int dispwin_wanted(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w || !w->disp_armed || w->disp_vd || !w->ntasks || w->singletask < 0)
        return 0;
    if (w->err.commandflag & (CF_SUSPENDED | CF_WIMPVDU))
        return 0;
    if (wimp_current() || ros_streams_own() || ros_in_background())
        return 0;                               /* a Wimp task, an SSH session's, or an interrupt */
    struct ros_task *t = ros_task_current();
    return t && !*ros_task_vdu_real(t) && !ros_task_vdisplay(t);
}

/* A display in the mode, attached to the program and live.  The window
 * follows at its next SWI. */
static os_error *dispwin_give(uint32_t mode)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = mode;
    ros_swi(&c, XVDisplay_Create);
    if (c.v)
        return ros_ptr(c.r[0]);
    uint32_t vd = c.r[0];
    ros_cpu_enter(&c);
    c.r[0] = vd, c.r[1] = 0;
    ros_swi(&c, XVDisplay_Attach);
    if (c.v) {
        os_error *e = ros_ptr(c.r[0]);
        ros_cpu_enter(&c);
        c.r[0] = vd;
        ros_swi(&c, XVDisplay_Destroy);
        return e;
    }
    ros_cpu_enter(&c);                          /* the keys go to it (ros_vdu_keys_blocked) */
    c.r[0] = vd, c.r[1] = 1, c.r[2] = 0, c.r[3] = 0;
    ros_swi(&c, XVDisplay_SetFocus);
    w->disp_vd = vd;
    w->disp_armed = 0;
    w->err.commandflag = CF_ACTIVE;
    ros_vector_release_native(WRCHV, mywrch, 0);
    ros_vdisplay_foreground_want();             /* the window, at the next SWI */
    return NULL;
}

/* The program's first character, not a mode change: a display in the
 * screen's mode.  The program may have read the screen's size already --
 * Mandel did, and drew a picture four times its mode 28 display, a quarter
 * of it seen (16 Oct).  The selector is copied: the VDU's own block is
 * every context's, and the display's mode change rewrites it. */
static int dispwin_text(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!dispwin_wanted())
        return 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 1;
    ros_swi(&c, XOS_ScreenMode);
    uint32_t mode = c.v ? 28u : c.r[1];
    if (mode >= 256) {
        uint32_t n = 5;
        while (n + 1 < sizeof w->disp_sel / 4 && ros_ld32(mode + 4 * n) != 0xFFFFFFFFu)
            n += 2;
        for (uint32_t i = 0; i < n; i++)
            w->disp_sel[i] = ros_ld32(mode + 4 * i);
        w->disp_sel[n] = 0xFFFFFFFFu;
        mode = ros_addr(w->disp_sel);
    }
    if (dispwin_give(mode) == NULL)
        return 1;
    return dispwin_give(28) == NULL;            /* the screen's mode refused: mode 28 */
}

/* VDU 22 or OS_ScreenMode 0 on the real display, by the program */
static int dispwin_mode(uint32_t mode, os_error **e)
{
    if (!dispwin_wanted())
        return 0;
    *e = dispwin_give(mode);
    return 1;
}

/* The window's place and scale for the display's size: 1:1 where it fits
 * in most of the screen, otherwise fitted, its shape kept; centred */
static void dispwin_place(struct wimp_box *vis, uint32_t *scale)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = w->disp_vd;
    ros_swi(&c, XVDisplay_Info);
    int32_t dw = c.v ? 1280 : (int32_t)(c.r[3] << c.r[5]);
    int32_t dh = c.v ? 960 : (int32_t)(c.r[4] << c.r[6]);
    int32_t maxw = w->screen_w * 9 / 10, maxh = w->screen_h * 8 / 10;
    *scale = 1;
    if (dw > maxw || dh > maxh) {
        *scale = 0;
        if ((int64_t)dw * maxh > (int64_t)dh * maxw)
            dh = (int32_t)((int64_t)dh * maxw / dw), dw = maxw;
        else
            dw = (int32_t)((int64_t)dw * maxh / dh), dh = maxh;
    }
    int32_t px = ~((1 << w->xeig) - 1), py = ~((1 << w->yeig) - 1);
    dw &= px, dh &= py;
    vis->x0 = ((w->screen_w - dw) / 2) & px;
    vis->y0 = ((w->screen_h - dh) / 2) & py;
    vis->x1 = vis->x0 + dw;
    vis->y1 = vis->y0 + dh;
}

/* The window opened, or opened again at the display's new size, drawn and
 * published; and the mouse mapped onto it */
static void dispwin_show(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = w->disp_handle ? wimp_window(w->disp_handle) : NULL;
    if (!win) {
        uint8_t def[88];
        if (!wimp_template_load("command", def))
            return;
        os_error *e = NULL;
        if (!(win = wimp_create_system(def, &e)))
            return;
        w->disp_handle = win->handle;
        wr(win->def, 56, ros_ld32(ros_addr(win->def + 56)) | IF_INDIRECT);
        wr(win->def, 72, ros_addr(w->disp_title));
        wr(win->def, 76, 0xFFFFFFFFu);
        wr(win->def, 80, sizeof w->disp_title);
    }
    memcpy(w->disp_title, w->cmd_title, sizeof w->disp_title);
    w->disp_title[sizeof w->disp_title - 1] = 0;
    struct wimp_box v;
    uint32_t scale;
    dispwin_place(&v, &scale);
    int32_t ext[4] = { 0, -(v.y1 - v.y0), v.x1 - v.x0, 0 };
    wimp_set_extent(win, ext);
    if (wimp_surface_bind(win, WS_VDISPLAY, w->disp_vd, 0, scale))
        return;
    uint8_t b[32];
    wr(b, 0, win->handle);
    memcpy(b + 4, &v, 16);
    wr(b, 20, 0);
    wr(b, 24, 0);
    wr(b, 28, 0xFFFFFFFFu);
    wimp_open_own(win, b);
    wimp_flush();
    wimp_surface_poll();                        /* its backing, made */
    wimp_redraw_now(win);
    wimp_surface_poll();                        /* and published */
    struct wimp_box a = win->s[APP].vis;
    int32_t r[4] = { a.x0, a.y0, a.x1, a.y1 };
    if (scale) {                                /* 1:1: the display's size from the top left */
        int32_t ext2[4];
        memcpy(ext2, ros_ptr(ros_addr(win->def + 40)), sizeof ext2);
        r[1] = a.y1 + ext2[1], r[2] = a.x0 + ext2[2];
    }
    int32_t *rr = (int32_t *)w->cmd_vdu;
    memcpy(rr, r, sizeof r);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = w->disp_vd, c.r[1] = 1, c.r[2] = win->handle, c.r[3] = ros_addr(rr);
    ros_swi(&c, XVDisplay_SetFocus);
}

/* At the program's next SWI after the display came or changed its mode */
static void dispwin_foreground(void)
{
    if (wimp_dispwin_on())
        dispwin_show();
}

/* The program has ended, or become a Wimp task.  The title asks for SPACE
 * if it printed, then the window closes and the display goes. */
static void dispwin_close(int prompt)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_window *win = w->disp_handle ? wimp_window(w->disp_handle) : NULL;
    if (prompt) {
        /* the prompt in the display itself, at the text cursor, as the
         * command window prints it on the screen: Mandel clears its
         * screen as it ends, and with the prompt only in the title the
         * window was blank and looked hung (16 Oct).  The program's own
         * context is live here, so this is drawn in its display */
        struct ros_cpu c;
        static const uint8_t vdu4[3] = { 4, 13, 10 };
        for (int i = 0; i < 3; i++) {
            ros_cpu_enter(&c);
            c.r[0] = vdu4[i];
            ros_swi(&c, XOS_WriteC);
        }
        for (uint32_t p = wimp_message("Space"); p && ros_ld8(p) >= 32; p++) {
            ros_cpu_enter(&c);
            c.r[0] = ros_ld8(p);
            ros_swi(&c, XOS_WriteC);
        }
    }
    int real = ros_vdu_real_enter();            /* the window's drawing: the desktop's */
    if (prompt) {
        if (!win || !win->s[APP].open)
            dispwin_show();                     /* it ended before its next SWI */
        win = w->disp_handle ? wimp_window(w->disp_handle) : NULL;
        uint32_t text = wimp_message("Space");
        uint32_t n = 0;
        while (n < sizeof w->disp_title - 8 && w->disp_title[n])
            n++;
        if (text && n) {
            memcpy(w->disp_title + n, " -- ", 4);
            n += 4;
        }
        for (uint32_t p = text; p && ros_ld8(p) >= 32 && n < sizeof w->disp_title - 1; p++)
            w->disp_title[n++] = ros_ld8(p);
        w->disp_title[n] = 0;
        if (win && win->s[APP].open) {
            wimp_redraw_now(win);
            wimp_surface_poll();
        }
        wait_key();
    }
    if (win && win->s[APP].open) {
        wimp_close_system(win);
        wimp_flush();
        wimp_surface_poll();                    /* gone from the list at once */
    }
    ros_vdu_real_leave(real);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0;
    ros_swi(&c, XVDisplay_Attach);              /* the program's thread: detached */
    ros_cpu_enter(&c);
    c.r[0] = w->disp_vd;
    ros_swi(&c, XVDisplay_Destroy);
    w->disp_vd = 0;
}

void wimp_dispwin_start(void)
{
    ros_vdisplay_mode_hook = dispwin_mode;
    ros_vdisplay_foreground_hook = dispwin_foreground;
}
