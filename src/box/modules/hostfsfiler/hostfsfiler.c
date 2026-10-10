/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* hostfsfiler.c -- HostFSFiler, a native module: each HostFS disc on the
 * icon bar, its root opened in the Filer.
 *
 * ROSGD's HostFS has one disc per Linux mount (modules/fileswitch). This
 * module gives each one an icon, as the team's HostFSFiler 2.00 gives the
 * emulator's one HostFS disc its icon, and does what that one does for it.
 * Its behaviour was checked against RISC OS 5.30 by
 * tests/desktop/hostfsfiler. It is new code, not a translation.
 *
 * The shape is RAMFSFiler's (FileSys/RAMFS/RAMFSFiler), as every filer's:
 *
 *   - There is no workspace until the Filer asks for filers with
 *     Service_StartFiler. Then the module claims the workspace, keeps the
 *     Filer's task handle, and claims the service with the command that
 *     starts it, *Desktop_HostFSFiler, which the Filer starts with
 *     Wimp_StartTask.
 *   - That command enters the module as the application (OS_Module 2). The
 *     start entry is the task, in C. Its SWIs are the outermost, so each
 *     Wimp_Poll switches tasks, with this thread parked inside the SWI
 *     until the Wimp comes back to it (runtime/callback.c).
 *   - The task ends with filer_exit: Wimp_CloseDown, the workspace freed,
 *     OS_Exit.
 *
 * The private word is 0 before the Filer starts it, the workspace while it
 * exists, and -1 after a start that gave up. The -1 stops the Filer, which
 * issues Service_StartFiler until nobody claims it, from starting again a
 * task that cannot run. Service_StartedFiler clears the -1.
 *
 * Everything that the Wimp and the Filer are given by address is in the
 * workspace, in the RMA. That is the poll block, the messages, the menu,
 * the icons' text and the command. The task's own state is its C locals.
 */
#include <stdio.h>
#include <string.h>

#include "fileswitch.h"
#include "hostfsfiler.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"

#define TASK_WORD        0x4B534154u        /* "TASK" */
#define HOSTFS_FS        220u
#define ICON_SPRITE      "harddisc"
/* Just right of the hard discs on the icon bar (Desktop/Wimp/hdr/Wimp:
 * WimpPriority_HardDiscs is &70000000, WimpPriority_FloppyDiscs
 * &60000000), as the team's HostFSFiler */
#define ICON_PRIORITY    0x6F000000u
#define ICONBAR          0xFFFFFFFEu        /* -2, the icon bar's window */
#define MAX_ICONS        8                  /* HostFS's discs, at most */
#define TEXT_MAX         15                 /* an icon's text (RAMFSFiler's rule) */
#define ROOT_MAX         48                 /* "HostFS::<disc>.$" */

#define SERVICE_RESET        0x27u
#define SERVICE_STARTFILER   0x4Bu
#define SERVICE_STARTEDFILER 0x4Cu
#define SERVICE_FILERDYING   0x4Fu

#define MOUSE_CLICK           6u
#define MENU_SELECTION        9u
#define USER_MESSAGE          17u
#define USER_MESSAGE_RECORDED 18u

#define MESSAGE_QUIT            0u
#define MESSAGE_DATASAVE        1u
#define MESSAGE_DATASAVEACK     2u
#define MESSAGE_DATALOAD        3u
#define MESSAGE_DATALOADACK     4u
#define MESSAGE_FILEROPENDIR    0x400u
#define MESSAGE_FILERDEVICEPATH 0x408u
#define MESSAGE_HELPREQUEST     0x502u
#define MESSAGE_HELPREPLY       0x503u

#define MENU_OPEN  0u
#define MENU_FREE  1u
#define MENU_QUIT  2u
#define MENU_ITEMS 3u

static const char use_desktop[] = "Use *Desktop to start HostFSFiler";

/* The workspace: from Service_StartFiler until the task ends */
struct ws {
    uint32_t filer_task;            /* the Filer's, from Service_StartFiler */
    uint32_t task;                  /* ours, once Wimp_Initialise has given it */
    uint32_t nicons;
    uint32_t menu_icon;             /* the icon whose menu was opened last */
    uint32_t poll[64];              /* Wimp_Poll's block; messages arrive here */
    uint32_t message[64];           /* messages we send */
    uint32_t menu[7 + MENU_ITEMS * 6];  /* 28-byte header, 24 bytes an item */
    uint32_t pointer[5];            /* Wimp_GetPointerInfo */
    uint32_t state[16];             /* Wimp_GetMenuState */
    uint32_t messages[4];           /* the messages we ask the Wimp for */
    uint32_t block[9];              /* Wimp_CreateIcon's */
    char command[24];               /* what Service_StartFiler is answered with */
    char task_name[16];
    char sprite[12];
    char cli[80];                   /* *ShowFree's command line */
    os_error error;
    struct {
        uint32_t handle;
        char text[TEXT_MAX + 1];    /* the disc's name, as the icon shows it */
        char validation[12];
        char disc[32];              /* the disc's name */
    } icons[MAX_ICONS];
};

static uint32_t A(const void *p)
{
    return ros_addr(p);
}

/* The X form of a SWI, R0-R7 in and out: its error, or NULL */
static os_error *swi(uint32_t number, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, number | ROS_X_BIT);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

static struct ws *workspace(struct ros_module *m)
{
    uint32_t pw = ros_ld32(m->private_word);
    return (int32_t)pw > 0 ? ros_ptr(pw) : NULL;
}

/* word-aligned size of a message whose last byte is at end */
static uint32_t message_size(const uint32_t *block, const char *end)
{
    return ((uint32_t)(end - (const char *)block) + 1 + 3) & ~3u;
}

/* "HostFS::<disc>.$": the root of icon i's disc; its length */
static size_t root_of(const struct ws *w, uint32_t i, char *out)
{
    return (size_t)snprintf(out, ROOT_MAX, "HostFS::%s.$", w->icons[i].disc);
}

/* ---- leaving -------------------------------------------------------------- */

/* Close the task down, if it is one, and free the workspace */
static void free_workspace(struct ros_module *m, struct ws *w)
{
    if (w->task) {
        uint32_t r[8] = { w->task, TASK_WORD };
        swi(XWimp_CloseDown, r);            /* errors ignored */
    }
    ros_st32(m->private_word, 0);
    xos_module_free(w);
}

/* Leave the task for good: closed down, the workspace freed, the private
 * word set to 0 (or to -1, so that the Filer does not start it again), and
 * OS_Exit, which does not return. */
static void filer_exit(struct ros_module *m, struct ws *w, int do_not_restart)
{
    free_workspace(m, w);
    if (do_not_restart)
        ros_st32(m->private_word, 0xFFFFFFFFu);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, OS_Exit);
}

/* An error in the standard box, OK only, titled with the task's name */
static void report(struct ws *w, const os_error *e)
{
    uint32_t r[8] = { A(e), 1, A(w->task_name) };
    swi(XWimp_ReportError, r);
}

/* ---- the icons ------------------------------------------------------------ */

/* One icon a disc, each the harddisc sprite with the disc's name below
 * it, sized to the sprite in this mode (RAMFSFiler's way) and to the text
 * (16 OS units a character in the system font) */
static os_error *make_icons(struct ws *w)
{
    uint32_t r[8] = { 40, 0, A(w->sprite) };  /* Wimp_SpriteOp 40: ReadSpriteSize */
    uint32_t width = 34, height = 17, xeig = 1, yeig = 1;
    if (!swi(XWimp_SpriteOp, r)) {
        uint32_t mode = r[6];
        width = r[3], height = r[4];
        uint32_t v[8] = { mode, 4 };        /* XEigFactor */
        if (!swi(XOS_ReadModeVariable, v))
            xeig = v[2];
        uint32_t h[8] = { mode, 5 };        /* YEigFactor */
        if (!swi(XOS_ReadModeVariable, h))
            yeig = h[2];
    }
    width <<= xeig;
    height <<= yeig;
    w->nicons = 0;
    for (unsigned i = 0; i < MAX_ICONS; i++) {
        const char *disc = ros_hostfs_disc(i);
        if (!disc)
            break;
        snprintf(w->icons[i].disc, sizeof w->icons[i].disc, "%s", disc);
        snprintf(w->icons[i].text, sizeof w->icons[i].text, "%s", disc);
        snprintf(w->icons[i].validation, sizeof w->icons[i].validation, "S%s", ICON_SPRITE);
        uint32_t text_width = (uint32_t)strlen(w->icons[i].text) * 16;
        uint32_t *b = w->block;
        b[0] = 0xFFFFFFFAu;                 /* -6: icon bar, left, with priority */
        b[1] = 0;
        b[2] = 0xFFFFFFF0u;                 /* -16: the text's baseline */
        b[3] = width > text_width ? width : text_width;
        b[4] = 20 + height;                 /* the sprite above the text */
        b[5] = 0x1700310Bu;                 /* sprite and text, h-centred, indirected,
                                               click, fg 7, bg 1 */
        b[6] = A(w->icons[i].text);
        b[7] = A(w->icons[i].validation);
        b[8] = sizeof w->icons[i].text;
        uint32_t c[8] = { ICON_PRIORITY, A(b) };
        os_error *e = swi(XWimp_CreateIcon, c);
        if (e)
            return e;
        w->icons[i].handle = c[0];
        w->nicons = i + 1;
    }
    return NULL;
}

/* Which of our icons a window and icon handle are, or -1 */
static int our_icon(const struct ws *w, uint32_t window, uint32_t icon)
{
    if (window != ICONBAR)
        return -1;
    for (uint32_t i = 0; i < w->nicons; i++)
        if (w->icons[i].handle == icon)
            return (int)i;
    return -1;
}

/* ---- opening a root -------------------------------------------------------- */

/* Message_FilerOpenDir to the Filer (Desktop/Filer/s/MsgsIn): data +0 the
 * filing system's number, +4 flags, +8 the directory's full name */
static void open_root(struct ws *w, uint32_t i)
{
    uint32_t *m = w->message;
    m[1] = m[2] = m[3] = 0;
    m[4] = MESSAGE_FILEROPENDIR;
    m[5] = HOSTFS_FS;
    m[6] = 0;
    char *name = (char *)&m[7];
    size_t n = root_of(w, i, name);
    m[0] = message_size(m, name + n);
    uint32_t r[8] = { USER_MESSAGE_RECORDED, A(m), w->filer_task };
    swi(XWimp_SendMessage, r);
}

/* ---- the menu -------------------------------------------------------------- */

static void menu_item(struct ws *w, uint32_t k, const char *text, int last)
{
    uint32_t *item = &w->menu[7 + k * 6];
    item[0] = last ? 0x80u : 0;             /* menu flags: bit 7, the last item */
    item[1] = 0xFFFFFFFFu;                  /* no submenu */
    item[2] = 0x07000021u;                  /* text, filled, fg 7, bg 0 */
    memset(&item[3], 0, 12);
    memcpy(&item[3], text, strlen(text));
}

/* The menu, titled with icon i's disc: Open, Free, Quit */
static void build_menu(struct ws *w, uint32_t i)
{
    char *title = (char *)w->menu;
    size_t n = strlen(w->icons[i].disc);
    if (n > 12)
        n = 12;
    memset(title, 0, 12);
    memcpy(title, w->icons[i].disc, n);
    title[12] = 7;                          /* title foreground */
    title[13] = 2;                          /* title background */
    title[14] = 7;                          /* work area foreground */
    title[15] = 0;                          /* work area background */
    /* the widest text, "Open"'s items' or the title, and a margin */
    w->menu[4] = (uint32_t)(n > 7 ? n : 7) * 16 + 16;
    w->menu[5] = 44;                        /* item height */
    w->menu[6] = 0;                         /* gap */
    menu_item(w, MENU_OPEN, "Open", 0);
    menu_item(w, MENU_FREE, "Free", 0);
    menu_item(w, MENU_QUIT, "Quit", 1);
}

static void show_menu(struct ws *w, uint32_t x)
{
    build_menu(w, w->menu_icon);
    uint32_t r[8] = { 0, A(w->menu), x - 64, 96 + MENU_ITEMS * 44 };  /* above the icon bar */
    swi(XWimp_CreateMenu, r);
}

/* The Free module's window, as RAMFSFiler's Free entry opens for RAM:
 * *ShowFree -FS <filing system> <device> */
static void show_free(struct ws *w, uint32_t i)
{
    snprintf(w->cli, sizeof w->cli, "ShowFree -FS HostFS %s", w->icons[i].disc);
    uint32_t r[8] = { A(w->cli) };
    os_error *e = swi(XOS_CLI, r);
    if (e)
        report(w, e);
}

/* A choice from the menu: 1 if it was Quit, and the task has gone */
static int menu_selection(struct ros_module *m, struct ws *w)
{
    switch (w->poll[0]) {
    case MENU_OPEN:
        open_root(w, w->menu_icon);
        break;
    case MENU_FREE:
        show_free(w, w->menu_icon);
        break;
    case MENU_QUIT:
        filer_exit(m, w, 0);                /* HostFS stays; only the icons go */
        return 1;
    default:
        return 0;
    }
    /* ADJUST keeps the menu up */
    uint32_t r[8] = { 0, A(w->pointer) };
    if (!swi(XWimp_GetPointerInfo, r) && (w->pointer[2] & 1))
        show_menu(w, w->pointer[0]);
    return 0;
}

/* ---- messages -------------------------------------------------------------- */

static const char help_icon[] = "This is the HostFS icon: the host computer's shared directory."
                                "|MClick SELECT to open it.|MClick MENU for more options.";
static const char help_quit[] = "Removes the HostFS icon from the icon bar."
                                "|MHostFS itself stays: HostFS: still reaches the share.";
static const char *const help_item[MENU_ITEMS] = {
    "Opens the root directory of the host's shared directory.",
    "Shows how much space is free on the host.",
    help_quit,
};

/* Interactive help, for an icon and for each item of the menu */
static void help_request(struct ws *w)
{
    uint32_t *m = w->poll;
    uint32_t window = m[8], icon = m[9];
    const char *text;
    if (our_icon(w, window, icon) >= 0) {
        text = help_icon;
    } else {
        uint32_t r[8] = { 1, A(w->state), window, icon };   /* the item under the pointer */
        if (swi(XWimp_GetMenuState, r) || w->state[1] != 0xFFFFFFFFu)
            return;                         /* not one of our menus */
        if (w->state[0] >= MENU_ITEMS)
            return;
        text = help_item[w->state[0]];
    }
    m[3] = m[2];                            /* your ref: their my ref */
    m[4] = MESSAGE_HELPREPLY;
    char *reply = (char *)&m[5];
    size_t n = strlen(text);
    memcpy(reply, text, n + 1);
    m[0] = message_size(m, reply + n);
    uint32_t r[8] = { USER_MESSAGE, A(m), m[1] };   /* back to the sender */
    swi(XWimp_SendMessage, r);
}

/* A file saved to icon i: tell the saver where it goes, the root.  The
 * proposed leaf is at +44 (PRM 3-253); the path goes over it and has to
 * fit the 256-byte block, so a leaf too long for it is cut short, as the
 * Filer does, rather than written past the end. */
static void data_save(struct ws *w, uint32_t i)
{
    uint32_t *m = w->poll;
    char root[ROOT_MAX], leaf[212];
    size_t n = root_of(w, i, root);
    size_t max = 256 - 44 - 1 - (n + 1), k;
    const char *p = (const char *)m + 44;
    for (k = 0; k < max && k < sizeof leaf - 1 && (uint8_t)p[k] >= ' '; k++)
        leaf[k] = p[k];
    leaf[k] = 0;
    char *path = (char *)m + 44;
    int len = snprintf(path, 256 - 44, "%s.%s", root, leaf);
    m[0] = message_size(m, path + len);
    m[3] = m[2];
    m[4] = MESSAGE_DATASAVEACK;
    m[9] = 0xFFFFFFFFu;                     /* estimated size: unsafe, not ours */
    uint32_t r[8] = { USER_MESSAGE, A(m), m[1] };
    swi(XWimp_SendMessage, r);
}

/* Files dropped on icon i from a Filer window (your ref 0): the device
 * path back, and the Filer copies them into the root.  Otherwise the end
 * of a save to the icon: acknowledged, and the root opened to show it. */
static void data_load(struct ws *w, uint32_t i)
{
    uint32_t *m = w->poll;
    if (m[3] == 0) {
        uint32_t sender = m[1];
        m[3] = 0;
        m[4] = MESSAGE_FILERDEVICEPATH;
        char *path = (char *)&m[5];
        size_t n = root_of(w, i, path);
        m[0] = message_size(m, path + n);
        uint32_t r[8] = { USER_MESSAGE, A(m), sender };
        swi(XWimp_SendMessage, r);
        return;
    }
    m[3] = m[2];
    m[4] = MESSAGE_DATALOADACK;
    uint32_t r[8] = { USER_MESSAGE, A(m), m[1] };
    swi(XWimp_SendMessage, r);
    open_root(w, i);
}

/* A message: 1 if it was Quit, and the task has gone */
static int user_message(struct ros_module *m, struct ws *w)
{
    int i;
    switch (w->poll[4]) {
    case MESSAGE_QUIT:
        filer_exit(m, w, 0);
        return 1;
    case MESSAGE_HELPREQUEST:
        help_request(w);
        break;
    case MESSAGE_DATASAVE:
        if ((i = our_icon(w, w->poll[5], w->poll[6])) >= 0)
            data_save(w, (uint32_t)i);
        break;
    case MESSAGE_DATALOAD:
        if ((i = our_icon(w, w->poll[5], w->poll[6])) >= 0)
            data_load(w, (uint32_t)i);
        break;
    }
    return 0;
}

/* ---- the task -------------------------------------------------------------- */

static void run(struct ros_module *m, struct ws *w)
{
    /* No HostFS disc, no icon: quietly, and for good (RAMFSFiler does the
     * same with no RAM disc) */
    if (!ros_hostfs_disc(0)) {
        filer_exit(m, w, 1);
        return;
    }
    w->messages[0] = MESSAGE_DATASAVE;
    w->messages[1] = MESSAGE_DATALOAD;
    w->messages[2] = MESSAGE_HELPREQUEST;
    w->messages[3] = MESSAGE_QUIT;          /* always delivered; ends the list */
    uint32_t r[8] = { 310, TASK_WORD, A(w->task_name), A(w->messages) };
    os_error *e = swi(XWimp_Initialise, r);
    if (!e) {
        w->task = r[1];
        e = make_icons(w);
    }
    if (e) {
        report(w, e);
        filer_exit(m, w, 1);
        return;
    }
    for (;;) {
        uint32_t p[8] = { 0x31, A(w->poll) };   /* no null events, no pointer
                                                   entering or leaving */
        if ((e = swi(XWimp_Poll, p))) {
            report(w, e);
            continue;
        }
        int i;
        switch (p[0]) {
        case MOUSE_CLICK:
            if ((i = our_icon(w, w->poll[3], w->poll[4])) < 0)
                break;
            if (w->poll[2] & 5) {           /* SELECT or ADJUST */
                open_root(w, (uint32_t)i);
            } else if (w->poll[2] & 2) {    /* MENU */
                w->menu_icon = (uint32_t)i;
                show_menu(w, w->poll[0]);
            }
            break;
        case MENU_SELECTION:
            if (menu_selection(m, w))
                return;
            break;
        case USER_MESSAGE:
        case USER_MESSAGE_RECORDED:
            if (user_message(m, w))
                return;
            break;
        }
    }
}

/* The start entry: the task, once Service_StartFiler has given it its
 * workspace; without, the error the command gives */
static void start(struct ros_module *m, uint32_t tail)
{
    (void)tail;
    struct ws *w = workspace(m);
    if (!w) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = A(ros_error(0, use_desktop));
        ros_swi(&s, OS_GenerateError);      /* to the program's error handler */
        return;
    }
    run(m, w);
}

/* ---- the service calls, the command, finalisation ----------------------------- */

/* Service_StartFiler: R0 the Filer's task handle.  Claimed, with R0 -> the
 * command that starts us, unless we are running, starting, or told not
 * to. */
static void start_filer(struct ros_module *m, struct ros_cpu *s)
{
    if (ros_ld32(m->private_word) != 0)
        return;
    struct ws *w;
    if (xos_module_claim(sizeof *w, (void **)&w)) {
        ros_st32(m->private_word, 0xFFFFFFFFu);     /* no memory: do not ask again */
        return;
    }
    memset(w, 0, sizeof *w);
    w->filer_task = s->r[0];
    strcpy(w->command, "Desktop_HostFSFiler");
    strcpy(w->task_name, "HostFS Filer");
    strcpy(w->sprite, ICON_SPRITE);
    ros_st32(m->private_word, A(w));
    s->r[0] = A(w->command);
    s->r[1] = 0;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    uint32_t pw = ros_ld32(m->private_word);
    switch (s->r[1]) {
    case SERVICE_STARTFILER:
        start_filer(m, s);
        break;
    case SERVICE_STARTEDFILER:
        if ((int32_t)pw < 0)
            ros_st32(m->private_word, 0);   /* the Filer may start us again */
        break;
    case SERVICE_FILERDYING:
    case SERVICE_RESET:
        /* The Filer, or the whole desktop, is going and the task with it.
         * After a reset the Wimp has gone already: the task is only
         * forgotten, not closed down. */
        if ((int32_t)pw < 0) {
            ros_st32(m->private_word, 0);
        } else if (pw) {
            struct ws *w = ros_ptr(pw);
            if (s->r[1] == SERVICE_RESET)
                w->task = 0;
            free_workspace(m, w);
        }
        break;
    }
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    uint32_t pw = ros_ld32(m->private_word);
    if ((int32_t)pw < 0)
        ros_st32(m->private_word, 0);
    else if (pw)
        free_workspace(m, ros_ptr(pw));
    return NULL;                            /* never refuses to die */
}

/* *Desktop_HostFSFiler: the module entered as the application, which runs
 * the task. It does so only once Service_StartFiler has given it a
 * workspace. */
static os_error *cmd_desktop(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)tail, (void)argc;
    if (!workspace(m))
        return ros_error(0, use_desktop);
    uint32_t r[8] = { 2, m->base + ros_ld32(m->base + 0x10), 0 };  /* OS_Module 2: its title */
    return swi(XOS_Module, r);             /* returns only with an error */
}

static const struct ros_command commands[] = {
    { "Desktop_HostFSFiler", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Desktop_HostFSFiler",
      "The HostFS filer puts the host computer's shared directory on the icon bar, and uses "
      "the Filer to show it.\rDo not use *Desktop_HostFSFiler, use *Desktop instead.\r",
      cmd_desktop },
    { 0 },
};

struct ros_module hostfsfiler_module = {
    .title = "HostFSFiler",
    .help = "HostFSFiler\t2.00 (12 Sep 2026) ROSGD native",
    .final = final,
    .service = service,
    .start = start,
    .commands = commands,
};
