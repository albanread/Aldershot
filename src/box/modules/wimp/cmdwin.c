/* cmdwin.c -- the command window, where a program that is not a Wimp
 * task writes text in the desktop.
 *
 * The window is dormant, pending or active.  Giving it a title makes it
 * pending, and WrchV is claimed.  The first character written then opens
 * the window.  It is drawn at once, because no Wimp_Poll will follow, and
 * a text window is set inside it.  Closing an active window asks for
 * SPACE first. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
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
    if (ch == 22 || ch == 16)
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
    os_error *e = wimp_command_window(s->r[0]);
    if (e)
        wimp_fail(s, e);
    else
        s->v = 0;
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
}
