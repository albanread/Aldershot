/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a translation into C of RISC OS Open's DisplayManager source
 * (Sources/Video/UserI/Display: s.Front, s.Icon, s.Menu, s.Message, s.Mode,
 * s.Module, s.Mouse, s.MsgTrans, s.Window and s.Errors). */

/* display.c: DisplayManager, the screen mode chooser, in C.
 *
 * This is a hand conversion of RISC OS 5.31's DisplayManager
 * (Sources/Video/UserI/Display, an AAsm module). Every routine in its nine
 * ObjAsm sources is a function here, with the same shape and the same
 * behaviour. They run from Front's workspace and constants to Mode's
 * FindSubClass. This is a native module, following the pattern of
 * HostFSFiler. The module is C. The task that it becomes at
 * *Desktop_DisplayManager is C too, and it polls the Wimp as the ObjAsm's
 * Mod_Start does.
 *
 * On RISC OS this is the monitor icon at the right of the icon bar.
 * Select opens the Display window, which has colours, resolution, frame
 * rate, OK and Cancel. Menu opens the same choices as a menu tree. The
 * tree holds Info and the Mode dialogue, where a mode string can be typed.
 * The window's buttons pop up their menus. A choice changes the mode
 * through *WimpMode, and OS_ScreenMode 14 writes the string that command
 * is given. The modes come from OS_ScreenMode 2, which is ScreenModes'
 * list. Here that list is the runtime's own (runtime/vdu/modes.c).
 *
 * Everything that the Wimp is given by address is in the workspace, in
 * the RMA (module.h). The ObjAsm keeps these in its claimed workspace in
 * the same way. The mode tables have the ObjAsm's layout, descriptor for
 * descriptor.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

/* ---- Front: constants --------------------------------------------------- */

#define TASK_WORD        0x4B534154u        /* "TASK" */
#define ICONBAR          0xFFFFFFFEu        /* -2, the icon bar's window */

/* icon bar and window icons (Icon.s) */
#define IC_DISPLAY_COLOURS     4u
#define IC_DISPLAY_COLBUTTON   3u
#define IC_DISPLAY_RESOLUTION  5u
#define IC_DISPLAY_RESBUTTON   7u
#define IC_DISPLAY_CANCEL      6u
#define IC_DISPLAY_OK          1u
#define IC_DISPLAY_RATE        9u
#define IC_DISPLAY_RATEBUTTON  10u
#define IC_MODE_MODE           0u
#define IC_MODE_OK             1u
#define IC_INFO_VERSION        3u

/* menu items (Menu.s) */
#define MO_IC_INFO        0u
#define MO_IC_MODE        1u
#define MO_CO_MONO        0u
#define MO_CO_GREY4       1u
#define MO_CO_GREY16      2u
#define MO_CO_COLOUR16    3u
#define MO_CO_GREY256     4u
#define MO_CO_COLOUR256   5u
#define MO_CO_4K          6u
#define MO_CO_32K         7u
#define MO_CO_64K         8u
#define MO_CO_16M         9u
#define COLOURS_COUNT     10u

/* flags (Front.s) */
#define F_GREYLEVEL     0x01u      /* the current mode is grey level */
#define F_MESSAGESOPEN  0x02u
#define F_RATEMENUVALID 0x04u

/* poll word flags (Front.s) */
#define PF_REFRESHICONS 0x01u
#define PF_MODEINIT     0x02u
#define PF_SETTITLE     0x04u

#define FLAGS_SQUAREPIXEL 0x80000000u       /* a descriptor's flags, bit 31 */

/* poll mask: no null events, no pointer entering or leaving, the poll
 * word at high priority (Front.s poll_mask) */
#define POLL_MASK (0x1u | 0x30u | (3u << 22))

/* Wimp reason codes */
#define OPEN_WINDOW       2u
#define CLOSE_WINDOW      3u
#define MOUSE_CLICK       6u
#define KEY_PRESSED       8u
#define MENU_SELECTION    9u
#define POLLWORD_NONZERO  13u
#define USER_MESSAGE      17u
#define USER_MESSAGE_RECORDED 18u

/* messages (Hdr/Messages) */
#define MESSAGE_QUIT            0u
#define MESSAGE_DATASAVE        1u
#define MESSAGE_DATASAVEACK     2u
#define MESSAGE_DATALOAD        3u
#define MESSAGE_DATALOADACK     4u
#define MESSAGE_PALETTECHANGE   9u
#define MESSAGE_HELPREQUEST     0x502u
#define MESSAGE_HELPREPLY       0x503u
#define MESSAGE_MENUWARNING     0x400C0u
#define MESSAGE_MENUSDELETED    0x400C9u

/* services (Hdr/Services) */
#define SERVICE_RESET             0x27u
#define SERVICE_MODECHANGE        0x46u
#define SERVICE_STARTWIMP         0x49u
#define SERVICE_STARTEDWIMP       0x4Au
#define SERVICE_CALIBRATIONCHANGED 0x5Bu
#define SERVICE_WIMPPALETTE       0x5Du
#define SERVICE_MODEFILECHANGED   0x94u

/* Wimp menu and window layout (Hdr/WimpSpace) */
#define M_HEADERSIZE 28u
#define MI_SIZE      24u
#define MI_IT_TICK      0x01u
#define MI_IT_DOTTED    0x02u
#define MI_IT_WARNING   0x08u
#define MI_IT_LASTITEM  0x80u
#define MENU_ICONFLAGS  0x07000021u          /* filled text, fg 7, bg 0 */
#define MENU_RESFLAGS   (MENU_ICONFLAGS | 0x100u)   /* and indirected */
#define IS_SHADED       0x00400000u
#define W_TITLE         72u                  /* a window block's fields */
#define W_NICONS        84u
#define W_ICONS         88u
#define I_FLAGS         16u
#define I_DATA          20u

/* buttons */
#define BUTTON_LEFT   4u
#define BUTTON_MIDDLE 2u
#define BUTTON_RIGHT  1u

/* errors (Errors.s: ErrorBase_Modes, &C00 up; the message file's tokens) */
#define ERR_CANTSTART     0xC00u    /* UseDesk */
#define ERR_NOWIMP        0xC01u    /* NoWimp */
#define ERR_NOTEMPLATE    0xC02u    /* E00 */
#define ERR_UNSUPPORTED   0xC03u    /* E01 */
#define ERR_CANTENUMERATE 0xC04u    /* E02 */
#define ERR_NOSCRAP       0xC05u    /* E03 */
#define ERR_INVALIDMODE   0xC06u    /* E04 */

/* A mode descriptor, in either format (Front.s). Bit 0 of the flags means
 * valid. Bit 1 means the descriptor holds a pixel format. The name follows,
 * word aligned. */
struct desc {
    uint32_t blocksize, flags, xres, yres;
    uint32_t w[4];        /* format 0: depth, rate; format 1: the format */
    char name[];          /* at w[2] (format 0) or w[4] (format 1) */
};
static int desc_is_new(const struct desc *d) { return (d->flags & 2u) != 0; }
static uint32_t desc_rate(const struct desc *d)
{
    return desc_is_new(d) ? d->w[3] : d->w[1];
}
static const char *desc_name(const struct desc *d)
{
    return desc_is_new(d) ? (const char *)&d->w[4] : (const char *)&d->w[2];
}

/* A pixel format, as GraphicsV defines it (Hdr/GraphicsV). */
struct pixelformat {
    uint32_t ncolour, modeflags, log2bpp;
};

/* colours_to_pixelformat (Mode.s): the pixel format of each colours menu item. */
static const struct pixelformat colours_pf[COLOURS_COUNT] = {
    { 1, 0, 0 }, { 3, 0, 1 }, { 15, 0, 2 }, { 15, 0, 2 },
    { 255, 0x80u, 3 }, { 255, 0x80u, 3 }, { 4095, 0, 4 }, { 65535, 0, 4 },
    { 65535, 0x80u, 4 }, { 0xFFFFFFFFu, 0, 5 },
};

/* ---- the workspace (Front.s) ---------------------------------------------
 *
 * task_handle is first because it is the first word of the ObjAsm's
 * workspace. */
struct ws {
    uint32_t task_handle;
    uint32_t pollword;

    int32_t icon_handle;
    int32_t info_handle, mode_handle, display_handle;
    uint32_t menu_handle;            /* the menu shown, or 0 */
    uint32_t menu_x, menu_y;         /* where it was shown */

    uint32_t indirected_data;        /* RMA: the templates' indirected data */
    uint32_t mode_indirect, mode_size;
    uint32_t resolution_indirect, resolution_size;
    uint32_t colours_indirect, colours_size;
    uint32_t rate_indirect, rate_size;
    uint32_t title_indirect, title_size;

    /* one RMA block, carved as Front.s carves it */
    uint32_t mode_space;
    uint32_t mode_sortedlist;        /* descriptors, sorted */
    uint32_t mode_classlist;         /* pointers into mode_sortedlist */
    uint32_t mode_menulist;          /* pointers into mode_classlist */
    uint32_t mode_table;             /* the descriptors as enumerated */
    uint32_t m_resolutionmenu;
    uint32_t m_resolutionsize;
    uint32_t m_ratemenu;
    uint32_t m_ratesize;

    uint32_t mode_count;
    uint32_t selected_subclass;      /* into mode_sortedlist */
    uint32_t selected_mode;          /* a descriptor, or -1 */
    int32_t scrap_ref;

    uint32_t message_file_block[4];
    uint8_t wimp_palette[16];

    uint8_t flags;
    uint8_t selected_colours;
    uint8_t selected_class;
    uint8_t resolution_count;
    uint8_t class_count;
    uint8_t rate_count;
    uint8_t menu_tick;

    /* The menus that MakeMenus makes, the poll block and the Wimp's scratch
     * space. The state block is also used for GetWindowInfo, which needs
     * 244 bytes with the icons. */
    uint32_t iconbarmenu[(M_HEADERSIZE + 2 * MI_SIZE) / 4];
    uint32_t coloursmenu[(M_HEADERSIZE + COLOURS_COUNT * MI_SIZE) / 4];
    uint32_t poll[64];
    uint32_t messages[6];
    uint32_t state[64];             /* Wimp_GetIconState / Wimp_GetWindowState */
    uint32_t menu_state[3];          /* Wimp_GetMenuState */
    uint32_t decode[2];              /* Wimp_DecodeMenu's selection in */
    uint32_t palette_message[5];     /* Message_PaletteChange, broadcast */
    uint32_t palette[256];           /* Mode_TestPalette's reads */
    struct {                         /* Mode_BuildSpecifier's selector */
        uint32_t flags, xres, yres, depth, rate;
        uint32_t vars[5];
    } selector;

    char tok[16];                    /* a message token, for a lookup */
    char messages_name[28];          /* "DisplayManager:Messages" */
    char templates_name[28];         /* "DisplayManager:Templates" */
    char sprite_name[8];             /* "display" */
    char delete_scrap[28];           /* "%Delete <Wimp$Scrap>" */
    uint8_t iconbar_def[48];         /* the menu definitions, copied in */
    uint8_t colours_def[176];
    uint8_t resolution_def[32];
    uint8_t rate_def[32];
    char command[24];                /* "Desktop_DisplayManager", the claim */
    char task_name[60];              /* the banner, Messages' "Title" */
    char cli[104];                   /* the *WimpMode command, and *LoadModeFile */
    os_error error_block;            /* a token error, translated, ours to show */
};

static uint32_t A(const void *p)
{
    return ros_addr(p);
}

/* Calls a SWI in its X form, with R0 to R7 in and out. Returns its error, or NULL. */
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

static void menu_show(struct ws *w, uint32_t menu, uint32_t x, uint32_t y);
static void mode_set_window_icons(struct ws *w, uint32_t xres, uint32_t yres, int32_t rate);
static void window_set_title(struct ws *w);
static void shutdown(struct ros_module *m, struct ws *w);

/* Mod_CopyString (Module.s): copies into a fixed buffer and always terminates it. */
static void copy_string(const char *src, char *dst, uint32_t size)
{
    if (!size)
        return;
    uint32_t n = 0;
    while (n + 1 < size && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

/* The length that copy_string would need, including the terminator. */
static uint32_t copy_string_len(const char *src, uint32_t size)
{
    uint32_t n = 0;
    while (n + 1 < size && src[n])
        n++;
    return n + 1;
}

/* ---- MsgTrans (MsgTrans.s) ------------------------------------------------ */

static os_error *msgtrans_open_file(struct ws *w)
{
    if (w->flags & F_MESSAGESOPEN)
        return NULL;
    uint32_t r[8] = { A(w->message_file_block), A(w->messages_name), 0 };
    os_error *e = swi(XMessageTrans_OpenFile, r);
    if (!e)
        w->flags |= F_MESSAGESOPEN;
    return e;
}

/* Writes a token into the workspace, so that a SWI can be given its address. */
static uint32_t ws_token(struct ws *w, const char *token)
{
    copy_string(token, w->tok, sizeof w->tok);
    return A(w->tok);
}

/* MsgTrans_Lookup: the token's text in the buffer, or the token itself. */
static const char *lookup(struct ws *w, const char *token, char *buffer, uint32_t size)
{
    if (msgtrans_open_file(w))
        return token;
    uint32_t r[8] = { A(w->message_file_block), ws_token(w, token), A(buffer), size };
    if (swi(XMessageTrans_Lookup, r))
        return token;
    return buffer;
}

/* MsgTrans_ErrorLookup: the token error translated, in w->error_block.
 * R1 is the message file that the workspace has open. If the lookup fails,
 * the error text is the token looked up as an ordinary message. */
static os_error *error_lookup(struct ws *w, uint32_t errnum, const char *token)
{
    w->error_block.errnum = errnum;
    snprintf(w->error_block.errmess, sizeof w->error_block.errmess, "%s", token);
    uint32_t r[8] = { A(&w->error_block), A(w->message_file_block), 0 };
    if (msgtrans_open_file(w) || swi(XMessageTrans_ErrorLookup, r)) {
        w->error_block.errnum = errnum;
        snprintf(w->error_block.errmess, sizeof w->error_block.errmess, "%s",
                 lookup(w, token, w->task_name, sizeof w->task_name));
        return &w->error_block;
    }
    os_error *out = ros_ptr(r[0]);
    w->error_block.errnum = out->errnum;
    snprintf(w->error_block.errmess, sizeof w->error_block.errmess, "%s", out->errmess);
    return &w->error_block;
}

/* Mod_ReportError (Module.s): reports the error with the banner as its title. */
static void report_error(struct ws *w, os_error *e, uint32_t buttons)
{
    lookup(w, "Title", w->task_name, sizeof w->task_name);
    uint32_t r[8] = { A(e), buttons, A(w->task_name) };
    swi(XWimp_ReportError, r);
}

/* ---- Icon (Icon.s) --------------------------------------------------------- */

/* Icon_Init: puts the "display" sprite on the right of the icon bar, at the
 * mode chooser's priority (Hdr/Wimp: WimpPriority_ModeChooser, &20000000). */
static os_error *icon_init(struct ws *w)
{
    uint32_t r[8] = { 40, 0, A(w->sprite_name) };  /* Wimp_SpriteOp 40: size */
    uint32_t width = 34, height = 34, xeig = 1, yeig = 1;
    if (!swi(XWimp_SpriteOp, r)) {
        uint32_t mode = r[6];
        width = r[3], height = r[4];
        uint32_t v[8] = { mode, 4 };            /* XEigFactor */
        if (!swi(XOS_ReadModeVariable, v))
            xeig = v[2];
        uint32_t h[8] = { mode, 5 };            /* YEigFactor */
        if (!swi(XOS_ReadModeVariable, h))
            yeig = h[2];
    }
    uint32_t *b = w->state;
    b[0] = 0xFFFFFFF8u;                         /* -8: the right, from the right */
    b[1] = 0;                                   /* x0 */
    b[2] = 0;                                   /* y0 */
    b[3] = width << xeig;                       /* x1 */
    b[4] = height << yeig;                      /* y1 */
    b[5] = 0x3002u;                             /* a sprite icon, menu clicks */
    memset(&b[6], 0, 12);
    memcpy(&b[6], w->sprite_name, 8);            /* the sprite's name */
    uint32_t c[8] = { 0x20000000u, A(b) };
    os_error *e = swi(XWimp_CreateIcon, c);
    w->icon_handle = e ? 0 : (int32_t)c[0];
    return e;
}

/* Icon_Refresh (through Icon_SetState): makes the Wimp redraw the display
 * window's indirected icons. A SetIconState call with nothing to change
 * does this. The Wimp reads the window, icon, EOR and clear words from the
 * block that R1 points at (Wimp04 SetIconState). The other registers carry
 * nothing. */
static void icon_refresh(struct ws *w)
{
    static const uint32_t icons[] = { IC_DISPLAY_COLOURS, IC_DISPLAY_RESOLUTION,
                                      IC_DISPLAY_RATE };
    for (unsigned i = 0; i < sizeof icons / sizeof icons[0]; i++) {
        uint32_t *b = w->state;
        b[0] = (uint32_t)w->display_handle, b[1] = icons[i];
        b[2] = 0, b[3] = 0;
        uint32_t r[8] = { 0, A(b) };
        if (swi(XWimp_SetIconState, r))
            return;
    }
}

/* ---- Window (Window.s) ------------------------------------------------------ */

/* load_template, sizing: R1 = -1 asks the Wimp how much room the template
 * and its indirected data need (Window.s add_template). R6 out of 0 means
 * the template was not found. */
static os_error *size_template(struct ws *w, const char *name, uint32_t *need,
                               uint32_t *indirected_need)
{
    uint32_t r[8] = { 0, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu, ws_token(w, name), 0 };
    os_error *e = swi(XWimp_LoadTemplate, r);
    if (!e && r[6] == 0)
        e = error_lookup(w, ERR_NOTEMPLATE, "E00");
    if (e)
        return e;
    *need = r[1];
    *indirected_need = r[2];
    return NULL;
}

/* load_template, loading: loads the template into the buffer in R1. The
 * Wimp lays the template out there, and that layout is the definition to
 * make the window from. The template's indirected data goes at
 * *indirected. The Wimp advances that to the next free byte, which it
 * returns in R2 (Window.s load_template). R6 out of 0 means the template
 * was not found. */
static os_error *load_template(struct ws *w, const char *name, uint32_t buffer,
                               uint32_t *indirected, uint32_t end)
{
    uint32_t r[8] = { 0, buffer, *indirected, end, 0xFFFFFFFFu, ws_token(w, name), 0 };
    os_error *e = swi(XWimp_LoadTemplate, r);
    if (!e && r[6] == 0)
        e = error_lookup(w, ERR_NOTEMPLATE, "E00");
    if (e)
        return e;
    *indirected = r[2];
    return NULL;
}

static os_error *create_window(uint32_t def, int32_t *handle)
{
    uint32_t r[8] = { 0, def };
    os_error *e = swi(XWimp_CreateWindow, r);
    *handle = e ? 0 : (int32_t)r[0];
    return e;
}

/* patch_info_version: sets the text of the Info window's version icon from the token "_Version". */
static void patch_info_version(struct ws *w, uint32_t def)
{
    uint32_t at = def + W_ICONS + 32 * IC_INFO_VERSION + I_DATA;
    lookup(w, "_Version", ros_ptr(ros_ld32(at)), ros_ld32(at + 8));
}

/* Window_Init: creates the Display, Mode and Info windows from the
 * templates. Their indirected data is in one block. The fields that this
 * module writes are kept. */
static os_error *window_init(struct ws *w)
{
    static const char names[3][8] = { "Display", "Mode", "Info" };
    uint32_t r[8] = { 0, A(w->templates_name) };        /* R0 = flags, R1 = the name */
    os_error *e = swi(XWimp_OpenTemplate, r);
    uint32_t scratch = 0, indirected_size = 0;
    void *scratch_block = NULL, *indirected_block = NULL;

    for (int i = 0; !e && i < 3; i++) {         /* add_template: the room */
        uint32_t need, ineed;
        e = size_template(w, names[i], &need, &ineed);
        /* (the name is copied through, into the workspace, by size_template) */
        if (!e && need > scratch)
            scratch = need;
        if (!e)
            indirected_size += ineed;
    }
    if (!e && scratch && indirected_size) {
        if (xos_module_claim(scratch, &scratch_block))
            e = ros_error(0x101, "No room in RMA");
        if (!e && xos_module_claim(indirected_size, &indirected_block))
            e = ros_error(0x101, "No room in RMA");
    }
    uint32_t at = A(indirected_block);
    uint32_t end = at + indirected_size;
    if (!e && scratch_block && indirected_block) {
        w->indirected_data = at;
        for (int i = 0; !e && i < 3; i++) {
            uint32_t buffer = A(scratch_block);
            e = load_template(w, names[i], buffer, &at, end);
            if (e)
                break;
            uint32_t def = buffer;              /* the Wimp laid it out there */
            if (i == 0) {                       /* the Display window's fields */
                uint32_t icons = def + W_ICONS;
                w->resolution_indirect = ros_ld32(icons + 32 * IC_DISPLAY_RESOLUTION + I_DATA);
                w->resolution_size = ros_ld32(icons + 32 * IC_DISPLAY_RESOLUTION + I_DATA + 8);
                w->colours_indirect = ros_ld32(icons + 32 * IC_DISPLAY_COLOURS + I_DATA);
                w->colours_size = ros_ld32(icons + 32 * IC_DISPLAY_COLOURS + I_DATA + 8);
                w->rate_indirect = ros_ld32(icons + 32 * IC_DISPLAY_RATE + I_DATA);
                w->rate_size = ros_ld32(icons + 32 * IC_DISPLAY_RATE + I_DATA + 8);
                w->title_indirect = ros_ld32(def + W_TITLE);
                w->title_size = ros_ld32(def + W_TITLE + 8);
                e = create_window(def, &w->display_handle);
                if (!e)
                    window_set_title(w);        /* the monitor's name (Window_Init) */
            } else if (i == 1) {                /* the Mode dialogue */
                w->mode_indirect = ros_ld32(def + W_ICONS + 32 * IC_MODE_MODE + I_DATA);
                w->mode_size = ros_ld32(def + W_ICONS + 32 * IC_MODE_MODE + I_DATA + 8);
                e = create_window(def, &w->mode_handle);
            } else {                            /* the Info window */
                patch_info_version(w, def);
                e = create_window(def, &w->info_handle);
            }
        }
    }

    uint32_t c[8] = { 0 };
    swi(XWimp_CloseTemplate, c);
    if (scratch_block)
        xos_module_free(scratch_block);
    return e;
}

/* Window_Open and Window_Close: the Wimp's requests are passed straight on. */
static void window_open(struct ws *w)
{
    uint32_t r[8] = { 0, A(w->poll) };
    swi(XWimp_OpenWindow, r);
}

static void window_close(struct ws *w)
{
    uint32_t r[8] = { 0, A(w->poll) };
    swi(XWimp_CloseWindow, r);
}

static void close_window(struct ws *w, int32_t handle)
{
    w->state[0] = (uint32_t)handle;
    uint32_t r[8] = { 0, A(w->state) };
    swi(XWimp_CloseWindow, r);
}

/* Window_OpenBehind: opens the window with its least corner at (x, y),
 * behind the window "behind". Its size and scrolling are kept. */
static void window_open_behind(struct ws *w, uint32_t window, uint32_t behind,
                               uint32_t x, uint32_t y)
{
    uint32_t *b = w->state;
    b[0] = window;
    uint32_t r[8] = { 0, A(b) };
    if (swi(XWimp_GetWindowState, r))
        return;
    /* +4 to +16 hold the visible area. LDMIB and STMIB step past the handle at +0. */
    uint32_t width = b[3] - b[1], height = b[4] - b[2];
    b[1] = x;
    b[2] = y;
    b[3] = x + width;
    b[4] = y + height;
    b[7] = behind;                              /* +28: the window behind */
    uint32_t o[8] = { 0, A(b) };
    swi(XWimp_OpenWindow, o);
}

/* Window_SetTitle: sets the title to the monitor's name, which is
 * ScreenModes_ReadInfo's answer. If that fails, the title is the banner.
 * The title bar is then redrawn. */
static void window_set_title(struct ws *w)
{
    copy_string(lookup(w, "Title", w->task_name, sizeof w->task_name),
                ros_ptr(w->title_indirect), w->title_size);
    {
        uint32_t r[8] = { 0 };                  /* ScreenModes_ReadInfo 0 */
        if (!swi(0x487C0 | ROS_X_BIT, r))
            copy_string(ros_ptr(r[0]), ros_ptr(w->title_indirect), w->title_size);
    }
    w->state[0] = (uint32_t)w->display_handle;
    uint32_t g[8] = { 0, A(w->state) };
    if (!swi(XWimp_GetWindowOutline, g)) {
        int32_t x1 = (int32_t)g[3], y1 = (int32_t)g[4];
        uint32_t f[8] = { 0xFFFFFFFFu, (uint32_t)x1, (uint32_t)(y1 - 44),
                          (uint32_t)x1, (uint32_t)y1 };
        swi(XWimp_ForceRedraw, f);
    }
}

/* ---- Mode (Mode.s): the mode table ------------------------------------------
 *
 * The table is OS_ScreenMode 2's descriptors in the RMA. The sorted list,
 * the class list and the menu list are Front.s's block, which is carved
 * from one claim. */

/* Mode_GetCurrent: OS_ScreenMode 1's specifier, which is a mode number or a selector. */
static uint32_t mode_get_current(void)
{
    uint32_t r[8] = { 1 };
    swi(XOS_ScreenMode, r);
    return r[1];
}

/* NColour of a depth. The ObjAsm does this with RSB r4, lr, lr, LSL r4.
 * On ARM a shift by 32 gives 0, so 32 bpp comes out as &FFFFFFFF. In C,
 * 1u << 32 is undefined. The x86-64 and AArch64 builds both made it 0.
 * That gave 16 million colours an NColour of 0, and OS_ScreenMode 14
 * refused the selector ("Bad parameters"). *WimpMode then had no mode to
 * change to. */
static uint32_t ncolour_of(uint32_t log2bpp)
{
    uint32_t bpp = 1u << log2bpp;
    return bpp >= 32 ? 0xFFFFFFFFu : (1u << bpp) - 1;
}

/* Mode_PixelFormatFromLog2BPP */
static void pixelformat_from_log2bpp(uint32_t depth, struct pixelformat *pf)
{
    pf->log2bpp = depth;
    pf->modeflags = depth == 3 ? 0x80u : 0;     /* ModeFlag_FullPalette */
    pf->ncolour = ncolour_of(depth);
}

/* Mode_IsOldPixelFormat */
static int is_old_pixelformat(const struct pixelformat *pf)
{
    if (pf->log2bpp > 5)
        return 0;
    if (pf->modeflags != (pf->log2bpp == 3 ? 0x80u : 0))
        return 0;
    return pf->ncolour == ncolour_of(pf->log2bpp);
}

/* The mode flags that the menus distinguish. */
#define PF_CARE (0x80u | 0x4000u | 0x200u | 0xF000u)

/* Mode_GetInfo: finds a specifier's size, pixel format and frame rate. For
 * a mode number the mode variables are read. For a selector the values
 * come from its variable pairs. */
static os_error *mode_get_info(uint32_t mode, uint32_t *xres, uint32_t *yres,
                               struct pixelformat *pf, int32_t *rate)
{
    if (mode < 256) {
        static const uint8_t vars[3] = { 10, 11, 12 };  /* XWind, YWind, Log2BPP */
        uint32_t r[8] = { mode, 0 };
        for (int i = 0; i < 3; i++) {
            r[1] = vars[i];
            if (swi(XOS_ReadModeVariable, r))
                return ros_error(0x1ED, "Screen mode not available");
            if (i == 0)
                *xres = r[2] + 1;
            else if (i == 1)
                *yres = r[2] + 1;
            else
                pf->log2bpp = r[2];
        }
        r[1] = 3;                               /* NColour */
        if (swi(XOS_ReadModeVariable, r))
            return ros_error(0x1ED, "Screen mode not available");
        pf->ncolour = r[2];
        r[1] = 0;                               /* ModeFlags */
        if (swi(XOS_ReadModeVariable, r))
            return ros_error(0x1ED, "Screen mode not available");
        pf->modeflags = r[2] & PF_CARE;
        *rate = -1;
        return NULL;
    }
    if (!ros_arena_readable(mode, mode + 20))
        return ros_error(0x19, "Bad MODE");
    pixelformat_from_log2bpp(ros_ld32(mode + 12), pf);
    for (uint32_t at = mode + 20; ros_ld32(at) != 0xFFFFFFFFu; at += 8) {
        uint32_t index = ros_ld32(at), value = ros_ld32(at + 4);
        if (index == 3)                         /* NColour */
            pf->ncolour = value;
        else if (index == 0)                    /* ModeFlags */
            pf->modeflags = value & PF_CARE;
    }
    *xres = ros_ld32(mode + 4);
    *yres = ros_ld32(mode + 8);
    *rate = (int32_t)ros_ld32(mode + 16);
    return NULL;
}

/* Mode_TestPalette: decides whether the palette is grey. An entry counts
 * as grey when its red and green values are equal. The blue value is not
 * compared. Modes above 8 bpp are always colour. The entries are read
 * with OS_ReadPalette one at a time. The ObjAsm also has a block read,
 * which is only an optimisation, and this is the path that it falls back to. */
static uint32_t mode_test_palette(struct ws *w, const struct pixelformat *pf)
{
    if (pf->log2bpp > 3) {
        w->flags &= ~F_GREYLEVEL;
        return w->flags;
    }
    w->flags |= F_GREYLEVEL;
    uint32_t entries = 1u << (1u << pf->log2bpp);
    if (entries > 256)
        entries = 256;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t r[8] = { i, 17 };
        if (swi(XOS_ReadPalette, r))
            break;                              /* the ObjAsm gives up too */
        uint32_t entry = r[2];
        if (((entry ^ (entry << 8)) & 0x00FF0000u) != 0) {
            w->flags &= ~F_GREYLEVEL;
            break;
        }
    }
    return w->flags;
}

/* Mode_PixelFormatToMenuItem: finds the colours menu item for a format. At
 * 16 bpp, NColour and the 64k flag tell 4K, 32K and 64K apart. */
static uint8_t pixelformat_to_menuitem(struct ws *w, const struct pixelformat *pf)
{
    static const uint8_t grey[6] = { MO_CO_MONO, MO_CO_GREY4, MO_CO_GREY16,
                                     MO_CO_GREY256, MO_CO_32K, MO_CO_16M };
    static const uint8_t colour[6] = { MO_CO_MONO, MO_CO_GREY4, MO_CO_COLOUR16,
                                       MO_CO_COLOUR256, MO_CO_32K, MO_CO_16M };
    uint8_t item = (w->flags & F_GREYLEVEL ? grey : colour)
        [pf->log2bpp < 6 ? pf->log2bpp : 5];
    if (!(w->flags & F_GREYLEVEL) && pf->log2bpp == 4) {
        if (pf->ncolour == 4095)
            item = MO_CO_4K;
        else if (pf->modeflags & 0x4000u)       /* ModeFlag_64k */
            item = MO_CO_64K;
    }
    return item;
}

/* Mode_DescriptorToColourMenuItem */
static uint8_t descriptor_to_menuitem(struct ws *w, const struct desc *d)
{
    struct pixelformat pf;
    if (desc_is_new(d)) {
        pf.ncolour = d->w[0];
        pf.modeflags = d->w[1];
        pf.log2bpp = d->w[2];
    } else {
        pixelformat_from_log2bpp(d->w[0], &pf);
    }
    return pixelformat_to_menuitem(w, &pf);
}

/* Mode_SortList: builds the sorted list and the classes by resolution.
 * The menu list is filled in later, by menu_resolution. The sort is on X,
 * then Y, then the colours menu item, then the frame rate, then the byte
 * order. Duplicates are dropped. These are modes that look the same to the
 * menus and differ only in RGB order or alpha. */
static void mode_sort_list(struct ws *w)
{
    uint32_t *sorted = ros_ptr(w->mode_sortedlist);
    uint32_t n = 0;
    for (uint32_t at = w->mode_table, i = 0; i < w->mode_count; i++) {
        struct desc *d = ros_ptr(at);
        uint32_t valid = d->flags & 0xFFu;
        int keep = valid == 1;
        if (valid == 3) {
            keep = d->w[2] <= 5 &&              /* above 32 bpp is not for the menus */
                    (d->w[1] & (3u << 12)) == 0; /* the RGB family only */
        }
        if (keep)
            sorted[n++] = at;
        at += d->blocksize;
    }
    sorted[n] = 0;

    for (int swapped = 1; swapped;) {           /* the ObjAsm's bubble sort */
        swapped = 0;
        for (uint32_t i = 0; i + 1 < n; i++) {
            struct desc *a = ros_ptr(sorted[i]), *b = ros_ptr(sorted[i + 1]);
            uint32_t fa = 0, fb = 0;
            int swap;
            if (a->xres != b->xres)
                swap = b->xres < a->xres;
            else if (a->yres != b->yres)
                swap = b->yres < a->yres;
            else if (descriptor_to_menuitem(w, a) != descriptor_to_menuitem(w, b))
                swap = descriptor_to_menuitem(w, b) < descriptor_to_menuitem(w, a);
            else if (desc_rate(a) != desc_rate(b))
                swap = desc_rate(b) < desc_rate(a);
            else {
                if (desc_is_new(a))
                    fa = a->w[1];
                if (desc_is_new(b))
                    fb = b->w[1];
                swap = (fb & 0xC000u) < (fa & 0xC000u);
            }
            if (swap) {
                uint32_t t = sorted[i];
                sorted[i] = sorted[i + 1];
                sorted[i + 1] = t;
                swapped = 1;
            }
        }
    }

    /* Drop the modes that a user cannot tell apart (Mode_CheckIdentical). */
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (out) {
            struct desc *a = ros_ptr(sorted[out - 1]), *b = ros_ptr(sorted[i]);
            int same = a->xres == b->xres && a->yres == b->yres &&
                       descriptor_to_menuitem(w, a) == descriptor_to_menuitem(w, b) &&
                       desc_rate(a) == desc_rate(b) &&
                       strcmp(desc_name(a), desc_name(b)) == 0;
            if (same)
                continue;
        }
        sorted[out++] = sorted[i];
    }
    sorted[out] = 0;

    /* The classes. Each class is one run of a single resolution. */
    uint32_t *classlist = ros_ptr(w->mode_classlist);
    uint32_t nclasses = 0;
    for (uint32_t i = 0; i < out; i++) {
        struct desc *d = ros_ptr(sorted[i]);
        struct desc *prev = i ? ros_ptr(sorted[i - 1]) : NULL;
        if (!prev || prev->xres != d->xres || prev->yres != d->yres)
            classlist[nclasses++] = w->mode_sortedlist + 4 * i;
    }
    classlist[nclasses] = 0;
    w->class_count = (uint8_t)nclasses;
}

/* Mode_GetTable: reads OS_ScreenMode 2's list into the RMA and carves up
 * the block. The layout is Front.s's: sorted list, class list, menu list,
 * table, then the menus. */
static os_error *mode_get_table(struct ws *w)
{
    if (w->mode_space) {
        xos_module_free(ros_ptr(w->mode_space));
        w->mode_space = w->mode_sortedlist = w->mode_classlist = w->mode_menulist = 0;
        w->mode_table = w->m_resolutionmenu = w->m_ratemenu = 0;
    }
    uint32_t r[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };
    if (swi(XOS_ScreenMode, r))
        return error_lookup(w, ERR_CANTENUMERATE, "E02");
    uint32_t count = (uint32_t)-(int32_t)r[2];
    w->mode_count = count;
    if (count == 0)
        return NULL;
    uint32_t table_size = ((-r[7]) + 3u) & ~3u;
    uint32_t menu_size = M_HEADERSIZE + MI_SIZE * count;
    w->m_resolutionsize = menu_size;
    w->m_ratesize = menu_size;
    void *block;
    if (xos_module_claim(3 * (count + 1) * 4 + table_size + 2 * menu_size, &block))
        return NULL;                            /* the ObjAsm leaves no list */
    memset(block, 0, 3 * (count + 1) * 4 + table_size + 2 * menu_size);
    w->mode_space = A(block);
    w->mode_sortedlist = w->mode_space;
    w->mode_classlist = w->mode_space + 4 * (count + 1);
    w->mode_menulist = w->mode_classlist + 4 * (count + 1);
    w->mode_table = w->mode_menulist + 4 * (count + 1);
    w->m_resolutionmenu = w->mode_table + table_size;
    w->m_ratemenu = w->m_resolutionmenu + menu_size;

    r[0] = 2, r[2] = 0, r[6] = w->mode_table, r[7] = table_size;
    if (swi(XOS_ScreenMode, r) || r[1] == 0) {
        xos_module_free(block);
        w->mode_space = w->mode_sortedlist = w->mode_classlist = w->mode_menulist = 0;
        w->mode_table = w->m_resolutionmenu = w->m_ratemenu = 0;
        return error_lookup(w, ERR_CANTENUMERATE, "E02");
    }
    mode_sort_list(w);
    return NULL;
}

/* Mode_BuildSpecifier: builds the descriptor's selector in the workspace.
 * It sets the greyscale flag when the menus chose grey (Mode_ChangeMode's r1). */
static uint32_t mode_build_specifier(struct ws *w, const struct desc *d, int grey)
{
    w->selector.flags = 1;
    w->selector.xres = d->xres;
    w->selector.yres = d->yres;
    uint32_t ncolour, modeflags, depth;
    if (desc_is_new(d)) {
        ncolour = d->w[0], modeflags = d->w[1], depth = d->w[2];
    } else {
        depth = d->w[0];
        struct pixelformat pf;
        pixelformat_from_log2bpp(depth, &pf);
        ncolour = pf.ncolour, modeflags = pf.modeflags;
    }
    w->selector.depth = depth;
    w->selector.rate = desc_rate(d);
    if (grey)
        modeflags |= 0x200u;                    /* ModeFlag_GreyscalePalette */
    w->selector.vars[0] = 3, w->selector.vars[1] = ncolour;     /* NColour */
    w->selector.vars[2] = 0, w->selector.vars[3] = modeflags;   /* ModeFlags */
    w->selector.vars[4] = 0xFFFFFFFFu;
    return A(&w->selector);
}

/* Mode_FindSubClass: searches for the menus' choice of colours and class.
 * It steps to a different choice as far as the flags allow. Bit 0 allows
 * the class to change, bit 1 allows the colours to change and bit 2 allows
 * modes that are not on the menus. Out: the subclass (a run in the sorted
 * list), the colours, the class and the mode. */
static os_error *mode_find_subclass(struct ws *w, uint32_t find_flags, uint8_t *io3,
                                    uint8_t *io4, int32_t rate, uint32_t *subclass,
                                    uint32_t *mode)
{
    uint32_t r3 = *io3, r4 = *io4, start_r3 = *io3, start_r4 = *io4;
    *subclass = 0xFFFFFFFFu;
    *mode = 0xFFFFFFFFu;
    if (r3 == 0xFF || r4 == 0xFF)
        return error_lookup(w, ERR_UNSUPPORTED, "E01");
    /* This check is not in the ObjAsm, which trusts its callers. Without it,
     * a class past the end of the list would be read as a class, because
     * the list ends with a null class. */
    if (r3 >= COLOURS_COUNT || r4 >= w->class_count)
        return error_lookup(w, ERR_UNSUPPORTED, "E01");

    uint32_t *classlist = ros_ptr(w->mode_classlist);
    /* If the class that we start from is not on the menus, allow modes that are not. */
    if (!(find_flags & 4u) && classlist[r4]) {
        uint32_t *class = ros_ptr(classlist[r4]);
        if (class[0] && desc_name(ros_ptr(class[0]))[0] == 0)
            find_flags |= 4u;
    }

    int step = -1;                              /* down first, then up */
    for (;;) {
        const struct pixelformat *pf = &colours_pf[r3];
        uint32_t *class = ros_ptr(classlist[r4]);
        uint32_t stop = classlist[r4 + 1] ? ros_ld32(classlist[r4 + 1]) : 0;
        for (uint32_t *p = class; *p; p++) {
            if (stop && *p == stop)
                break;                          /* the next class begins */
            struct desc *d = ros_ptr(*p);
            if (!(find_flags & 4u) && desc_name(d)[0] == 0)
                continue;                       /* not on the menus */
            int match;
            if (desc_is_new(d)) {
                /* The match is loose. The menus do not choose the
                 * transparency mode or the byte order (Mode.s's comment). */
                match = d->w[0] == pf->ncolour &&
                        ((d->w[1] ^ pf->modeflags) & (0x80u | 0x4000u)) == 0 &&
                        d->w[2] == pf->log2bpp;
            } else {
                struct pixelformat dpf;
                pixelformat_from_log2bpp(d->w[0], &dpf);
                match = is_old_pixelformat(&dpf) && dpf.log2bpp == pf->log2bpp;
            }
            if (!match)
                continue;
            if (*subclass == 0xFFFFFFFFu)
                *subclass = A(p);
            if (rate < 0 || (int32_t)desc_rate(d) == rate) {
                *mode = *p;
                *io3 = (uint8_t)r3;
                *io4 = (uint8_t)r4;
                return NULL;
            }
        }
        if (!(find_flags & 3u))
            return error_lookup(w, ERR_UNSUPPORTED, "E01");
        if (find_flags & 2u) {                  /* step the colours */
            r3 = (uint32_t)((int32_t)r3 + step);
            if (r3 == MO_CO_COLOUR16 || r3 == MO_CO_GREY256)
                r3 = (uint32_t)((int32_t)r3 + step);
            if ((int32_t)r3 < 0) {
                r3 = start_r3;
                step = 1;
                r3 = (uint32_t)((int32_t)r3 + step);
                if (r3 == MO_CO_COLOUR16 || r3 == MO_CO_GREY256)
                    r3 = (uint32_t)((int32_t)r3 + step);
            }
            if (r3 >= COLOURS_COUNT)
                return error_lookup(w, ERR_UNSUPPORTED, "E01");
        } else {                                /* step the class, wrapping round */
            r4 = (uint32_t)((int32_t)r4 + step);
            if ((int32_t)r4 < 0)
                r4 = w->class_count ? (uint32_t)w->class_count - 1 : 0;
            if (r4 == start_r4 || r4 >= w->class_count)
                return error_lookup(w, ERR_UNSUPPORTED, "E01");
        }
    }
}

/* Mode_SetSelection: finds the current mode's place in the menus. */
static void mode_set_selection(struct ws *w, uint32_t flags, uint32_t xres, uint32_t yres,
                               const struct pixelformat *pf, int32_t rate)
{
    uint8_t class = 0xFF;
    uint32_t *classlist = ros_ptr(w->mode_classlist);
    for (uint32_t c = 0; classlist[c]; c++) {
        struct desc *d = ros_ptr(ros_ld32(classlist[c]));
        if (d->xres == xres && d->yres == yres) {
            class = (uint8_t)c;
            break;
        }
    }
    w->flags = (uint8_t)flags;
    uint8_t colours = pixelformat_to_menuitem(w, pf);
    uint8_t c3 = colours, c4 = class;
    uint32_t subclass, mode;
    mode_find_subclass(w, 4u, &c3, &c4, rate, &subclass, &mode);
    w->selected_subclass = subclass;
    w->selected_colours = colours;
    w->selected_class = class;
    w->selected_mode = mode;
}

/* Mode_Init: reads the current mode, tests the palette for grey, makes the
 * selection and sets the window's icons. Any open menu is reopened. */
static os_error *mode_init(struct ws *w)
{
    uint32_t mode = mode_get_current();
    uint32_t xres = 0, yres = 0;          /* set unless mode_get_info errors */
    struct pixelformat pf = { 0, 0, 0 };
    int32_t rate = -1;
    os_error *e = mode_get_info(mode, &xres, &yres, &pf, &rate);
    if (e)
        return e;
    uint32_t flags = mode_test_palette(w, &pf);
    mode_set_selection(w, flags, xres, yres, &pf, rate);
    mode_set_window_icons(w, xres, yres, rate);
    if (w->menu_handle)
        menu_show(w, w->menu_handle, w->menu_x, w->menu_y);
    return NULL;
}

/* Mode_SetModeString: OS_ScreenMode 14 writes the dialogue's string. A mode
 * number is written as its own digits. */
static void mode_set_modestring(struct ws *w, uint32_t mode)
{
    char *buffer = ros_ptr(w->mode_indirect);
    uint32_t size = w->mode_size;
    if (mode < 256) {
        uint32_t r[8] = { mode, A(buffer), size };
        if (!swi(XOS_ConvertCardinal4, r) && r[2] < size)
            buffer[r[2]] = 0;
        return;
    }
    uint32_t r[8] = { 14, mode, A(buffer), size };
    if (swi(XOS_ScreenMode, r) || (int32_t)r[3] < 0)
        buffer[0] = 0;
}

/* Mode_WimpCommand: gives the dialogue's string to *WimpMode. */
static os_error *mode_wimp_command(struct ws *w)
{
    snprintf(w->cli, sizeof w->cli, "WimpMode ");
    copy_string(ros_ptr(w->mode_indirect), w->cli + 9, sizeof w->cli - 9);
    uint32_t r[8] = { A(w->cli) };
    return swi(XOS_CLI, r) ? error_lookup(w, ERR_INVALIDMODE, "E04") : NULL;
}

/* Mode_ChangeMode: changes to the mode that the menus chose. It goes
 * through the specifier's string, as the ObjAsm does. It builds the
 * specifier, writes the dialogue and runs *WimpMode. */
static os_error *mode_change_mode(struct ws *w)
{
    uint32_t mode = w->selected_mode;
    if (mode == 0xFFFFFFFFu) {
        uint32_t subclass;
        uint8_t c3 = w->selected_colours, c4 = w->selected_class;
        if (mode_find_subclass(w, 4u, &c3, &c4, -1, &subclass, &mode))
            return error_lookup(w, ERR_UNSUPPORTED, "E01");
        w->selected_colours = c3;
        w->selected_class = c4;
        w->selected_mode = mode;
    }
    int grey = w->selected_colours == MO_CO_GREY16 || w->selected_colours == MO_CO_GREY256;
    mode_set_modestring(w, mode_build_specifier(w, ros_ptr(mode), grey));
    return mode_wimp_command(w);
}

/* Mode_KeyPressed: Return in the Mode dialogue acts as its OK button. */
static void mode_key_pressed(struct ws *w)
{
    if (w->poll[0] != (uint32_t)w->mode_handle || w->poll[6] != 13) {
        uint32_t r[8] = { w->poll[6] };
        swi(XWimp_ProcessKey, r);
        return;
    }
    w->menu_handle = 0;
    uint32_t r[8] = { 0, 0xFFFFFFFFu };         /* close the menu and dialogue */
    swi(XWimp_CreateMenu, r);
    os_error *e = mode_wimp_command(w);
    if (e)
        report_error(w, e, 1);
}

/* ---- Menu (Menu.s) ---------------------------------------------------------- */

/* The menus' definitions, as the Menu and Item macros in Hdr/MsgMenus lay
 * them out. Each is a byte stream. It holds the title's token and its 0,
 * the four colours, and the item height and gap. Then comes each item's
 * token and 0, padded to a word boundary, followed by its flags, its
 * submenu's offset and its icon flags. A 0 byte ends the list, and
 * MakeMenus walks the stream in exactly this form. The submenus of the
 * icon bar's items are the Info and Mode windows. They are patched into
 * the built menu afterwards. The colours menu takes Menu.s's order, which
 * is M11, M12, M13, M14, M15, M16, M19, M17, M20 and M18, with M18 last. */
/* The words of an item: its flags, its submenu's offset and its icon flags. */
#define MENU_WORDS(iflags, sub, icon) \
    (iflags) & 0xFF, ((iflags) >> 8) & 0xFF, ((iflags) >> 16) & 0xFF, ((iflags) >> 24) & 0xFF, \
    (sub) & 0xFF, ((sub) >> 8) & 0xFF, ((sub) >> 16) & 0xFF, ((sub) >> 24) & 0xFF, \
    (icon) & 0xFF, ((icon) >> 8) & 0xFF, ((icon) >> 16) & 0xFF, ((icon) >> 24) & 0xFF

static const uint8_t rom_iconbar_menu[] = {
    'T', '0', '0', 0, 7, 2, 7, 0, 44, 0,
    'M', '0', '1', 0, 0, 0,                /* the header is 10 bytes, so this is padding */
    MENU_WORDS(0, 0, MENU_ICONFLAGS),
    'M', '0', '2', 0,
    MENU_WORDS(MI_IT_WARNING | MI_IT_LASTITEM, 0, MENU_ICONFLAGS),
    0, 0, 0, 0,
};
static const uint8_t rom_colours_menu[] = {
    'T', '0', '1', 0, 7, 2, 7, 0, 44, 0,
    'M', '1', '1', 0, 0, 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '2', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '3', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '4', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '5', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '6', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '9', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '7', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '2', '0', 0,
    MENU_WORDS(0, 0, MENU_ICONFLAGS | IS_SHADED),
    'M', '1', '8', 0,
    MENU_WORDS(MI_IT_LASTITEM, 0, MENU_ICONFLAGS | IS_SHADED),
    0, 0, 0, 0,
};
static const uint8_t rom_resolution_menu[] = {
    'T', '0', '2', 0, 7, 2, 7, 0, 44, 0,
    'M', 'X', '1', 0, 0, 0,
    MENU_WORDS(MI_IT_LASTITEM, 0, MENU_RESFLAGS),
    0, 0, 0, 0,
};
static const uint8_t rom_rate_menu[] = {
    'T', '0', '3', 0, 7, 2, 7, 0, 44, 0,
    'M', 'X', '1', 0, 0, 0,
    MENU_WORDS(MI_IT_LASTITEM, 0, MENU_ICONFLAGS),
    0, 0, 0, 0,
};

/* Menu_Resolution: builds the resolution menu from the class list. Each
 * class is a menu item, named by its descriptor. A dotted line separates
 * square pixel shapes from non-square ones. That never happens here,
 * because the display's pixels are square. */
static os_error *menu_resolution(struct ws *w)
{
    uint8_t count = 0;
    os_error *e = mode_get_table(w);
    if (!e)
        e = msgtrans_open_file(w);
    if (e || !w->m_resolutionmenu)
        goto done;

    {
        uint32_t r[8] = { A(w->message_file_block), A(w->resolution_def),
                          w->m_resolutionmenu, w->m_resolutionsize };
        e = swi(XMessageTrans_MakeMenus, r);
        if (e)
            goto done;
    }

    uint32_t *classlist = ros_ptr(w->mode_classlist);
    uint32_t *menulist = ros_ptr(w->mode_menulist);
    if (!classlist[0])
        goto done;
    uint32_t *at = ros_ptr(w->m_resolutionmenu + M_HEADERSIZE);
    uint32_t previous_shape = 0;
    for (uint32_t c = 0; classlist[c]; c++) {
        uint32_t *class = ros_ptr(classlist[c]);
        struct desc *d = ros_ptr(class[0]);
        if (desc_name(d)[0] == 0)
            continue;                           /* not on the menus */
        uint32_t shape = d->flags & FLAGS_SQUAREPIXEL;
        if (count && shape != previous_shape)
            *(at - MI_SIZE / 4) |= MI_IT_DOTTED;   /* a separator. MI_SIZE is
                                                      unsigned, so -MI_SIZE
                                                      would wrap round */
        previous_shape = shape;
        menulist[count] = w->mode_classlist + 4 * c;  /* points into the class
                                                  list, which is how
                                                  Menu_ClassToResolution and
                                                  Menu_ResolutionToClass read it */
        at[0] = classlist[c + 1] ? 0 : MI_IT_LASTITEM;
        at[1] = 0;                              /* no submenu */
        at[2] = MENU_RESFLAGS;
        at[3] = A(desc_name(d));                /* the descriptor's name */
        at[4] = 0;                              /* no validation */
        at[5] = 128;                            /* the text's room */
        at += MI_SIZE / 4;
        count++;
    }
    menulist[count] = 0;
    if (count)
        *(at - MI_SIZE / 4) |= MI_IT_LASTITEM;
    else
        w->m_resolutionmenu = 0;
done:
    w->resolution_count = count;
    return e;
}

/* Menu_Rate: builds the rate menu from the selected subclass's frame rates.
 * It takes the descriptors that have the same colours and resolution. */
static void menu_rate(struct ws *w)
{
    w->flags &= ~F_RATEMENUVALID;
    w->rate_count = 0;
    if (w->selected_subclass == 0xFFFFFFFFu || !w->m_ratemenu)
        return;
    if (msgtrans_open_file(w))
        return;
    {
        uint32_t r[8] = { A(w->message_file_block), A(w->rate_def), w->m_ratemenu,
                          w->m_ratesize };
        if (swi(XMessageTrans_MakeMenus, r))
            return;
    }
    uint32_t *subclass = ros_ptr(w->selected_subclass);
    struct desc *first = ros_ptr(subclass[0]);
    uint8_t item = descriptor_to_menuitem(w, first);
    uint32_t xres = first->xres, yres = first->yres;
    uint32_t *at = ros_ptr(w->m_ratemenu + M_HEADERSIZE);
    for (uint32_t *p = subclass; *p; p++) {
        struct desc *d = ros_ptr(*p);
        if (descriptor_to_menuitem(w, d) != item || d->xres != xres || d->yres != yres)
            break;
        char hz[16];
        snprintf(hz, sizeof hz, "%uHz", desc_rate(d));
        at[0] = p[1] ? 0 : MI_IT_LASTITEM;
        at[1] = 0;
        at[2] = MENU_ICONFLAGS;
        memset(&at[3], 0, 12);
        memcpy(&at[3], hz, strlen(hz) + 1);
        w->rate_count++;
        if (!p[1])
            break;
        at += MI_SIZE / 4;
    }
    w->flags |= F_RATEMENUVALID;
    if (w->menu_handle == w->m_ratemenu)
        w->menu_tick = 0xFF;                    /* rebuilt, so there is no tick to move */
}

/* Menu_Init: makes the menus and patches in the dialogue pointers. It also
 * builds the resolution menu and unshades the colours that the modes have. */
static os_error *menu_init(struct ws *w)
{
    w->menu_handle = 0;
    os_error *e = msgtrans_open_file(w);
    if (!e) {
        uint32_t r[8] = { A(w->message_file_block), A(w->iconbar_def),
                          A(w->iconbarmenu), sizeof w->iconbarmenu };
        e = swi(XMessageTrans_MakeMenus, r);
    }
    if (!e) {
        uint32_t r[8] = { A(w->message_file_block), A(w->colours_def),
                          A(w->coloursmenu), sizeof w->coloursmenu };
        e = swi(XMessageTrans_MakeMenus, r);
    }
    if (e)
        return e;
    w->iconbarmenu[(M_HEADERSIZE + MO_IC_INFO * MI_SIZE) / 4 + 1] = (uint32_t)w->info_handle;
    w->iconbarmenu[(M_HEADERSIZE + MO_IC_MODE * MI_SIZE) / 4 + 1] = (uint32_t)w->mode_handle;

    if ((e = menu_resolution(w)))
        return e;
    if (!w->mode_menulist)
        return NULL;

    /* Unshade every colour that the modes offer. In Menu_Init's 16/256 rule,
     * the grey item at 16 or 256 colours is unshaded whenever the colour item
     * at that depth is. */
    uint32_t found = 0;
    uint32_t want = (1u << MO_CO_MONO) | (1u << MO_CO_GREY4) | (1u << MO_CO_COLOUR16) |
                    (1u << MO_CO_COLOUR256) | (1u << MO_CO_4K) | (1u << MO_CO_32K) |
                    (1u << MO_CO_64K) | (1u << MO_CO_16M);
    uint32_t *menulist = ros_ptr(w->mode_menulist);
    for (uint32_t m = 0; menulist[m] && found != want; m++) {
        uint32_t *class = ros_ptr(ros_ld32(menulist[m]));
        for (uint32_t *p = class; *p; p++) {
            found |= 1u << descriptor_to_menuitem(w, ros_ptr(*p));
            if (found == want)
                break;
        }
    }
    found |= (found & ((1u << MO_CO_COLOUR16) | (1u << MO_CO_COLOUR256))) >> 1;
    for (uint32_t item = 0; item < COLOURS_COUNT; item++)
        if (found & (1u << item))
            w->coloursmenu[(M_HEADERSIZE + item * MI_SIZE) / 4 + 2] &= ~IS_SHADED;
    return NULL;
}

/* Menu_ChangeTick / Menu_RemoveTick */
static void menu_change_tick(uint32_t menu, uint8_t item)
{
    if (item == 0xFF)
        return;
    uint32_t *flags = &((uint32_t *)ros_ptr(menu))[(M_HEADERSIZE + item * MI_SIZE) / 4];
    *flags ^= MI_IT_TICK;
}

static void menu_remove_tick(struct ws *w)
{
    if (w->menu_handle)
        menu_change_tick(w->menu_handle, w->menu_tick);
}

/* Menu_ClassToResolution / Menu_ResolutionToClass */
static uint8_t class_to_resolution(struct ws *w, uint8_t class)
{
    uint32_t entry = w->mode_classlist + 4u * class;
    uint32_t *menulist = ros_ptr(w->mode_menulist);
    for (int i = (int)w->resolution_count - 1; i >= 0; i--)
        if (menulist[i] == entry)
            return (uint8_t)i;
    return 0xFF;
}

static uint8_t resolution_to_class(struct ws *w, uint8_t resolution)
{
    uint32_t *menulist = ros_ptr(w->mode_menulist);
    return (uint8_t)((menulist[resolution] - w->mode_classlist) / 4);
}

/* Menu_Show: ticks the menu with the selection and shows it. */
static void menu_show(struct ws *w, uint32_t menu, uint32_t x, uint32_t y)
{
    menu_remove_tick(w);
    uint8_t tick = 0xFF;
    if (menu == A(w->coloursmenu)) {
        if (w->selected_colours < COLOURS_COUNT)
            tick = w->selected_colours;
    } else if (menu == w->m_resolutionmenu) {
        uint8_t item = class_to_resolution(w, w->selected_class);
        if (item < w->resolution_count)
            tick = item;
    } else if (menu == w->m_ratemenu && w->selected_subclass != 0xFFFFFFFFu) {
        for (uint8_t i = 0; i < w->rate_count; i++)
            if (ros_ld32(w->selected_subclass + 4u * i) == w->selected_mode) {
                tick = i;
                break;
            }
    }
    menu_change_tick(menu, tick);
    w->menu_tick = tick;
    w->menu_handle = menu;
    w->menu_x = x, w->menu_y = y;
    uint32_t r[8] = { 0, menu, x, y };
    swi(XWimp_CreateMenu, r);
}

/* Menu_Decode: works out what a menu choice means. A choice from the
 * colours menu sets the colours and keeps the class. A choice from the
 * resolution menu sets the class and keeps the colours. A choice from the
 * rate menu sets the mode to the descriptor with that rate. The search
 * first allows no stepping to other colours or classes. If it finds
 * nothing, it runs again with modes that are not on the menus allowed. */
static os_error *menu_decode(struct ws *w, uint32_t menu)
{
    const uint32_t *selection = w->poll;
    if (menu == A(w->iconbarmenu))
        return NULL;
    if (menu == w->m_ratemenu) {
        uint32_t *subclass = ros_ptr(w->selected_subclass);
        uint32_t mode = subclass[selection[0]];
        if (mode == w->selected_mode)
            return NULL;
        w->selected_mode = mode;
    } else {
        int from_colours = menu == A(w->coloursmenu);
        uint8_t chosen_colours = from_colours ? (uint8_t)selection[0] : w->selected_colours;
        uint8_t chosen_class = from_colours ?
            w->selected_class : resolution_to_class(w, (uint8_t)selection[0]);
        if (chosen_class == 0xFF)
            chosen_class = 0;                   /* search from the bottom up */
        uint32_t subclass, mode;
        if (mode_find_subclass(w, 0, &chosen_colours, &chosen_class, -1, &subclass,
                               &mode)) {
            /* Search once more, this time allowing modes that are not on the menus. */
            chosen_colours = from_colours ? (uint8_t)selection[0] : w->selected_colours;
            chosen_class = from_colours ?
                w->selected_class : resolution_to_class(w, (uint8_t)selection[0]);
            if (chosen_class == 0xFF)
                chosen_class = 0;
            if (mode_find_subclass(w, 4u, &chosen_colours, &chosen_class, -1, &subclass,
                                   &mode))
                return error_lookup(w, ERR_UNSUPPORTED, "E01");
        }
        w->selected_subclass = subclass;
        w->selected_colours = chosen_colours;
        w->selected_class = chosen_class;
        w->selected_mode = mode;
    }
    mode_set_window_icons(w, 0, 0, -1);        /* a mode is chosen, so use its own values */
    icon_refresh(w);
    return NULL;
}

/* Menu_Selection: handles a click on a menu item. ADJUST keeps the menu open. */
static void menu_selection(struct ws *w)
{
    uint32_t menu = w->menu_handle;
    os_error *e = menu_decode(w, menu);
    if (e) {
        report_error(w, e, 1);
        return;
    }
    uint32_t r[8] = { 0, A(w->state) };
    if (!swi(XWimp_GetPointerInfo, r) && (w->state[2] & BUTTON_RIGHT) &&
        w->menu_handle == menu) {
        menu_show(w, menu, w->menu_x, w->menu_y);   /* ADJUST: again, Sam! */
        return;
    }
    menu_remove_tick(w);
    w->menu_handle = 0;
}

/* ---- the display window's icons (Mode_SetWindowIcons) ------------------------ */

/* The colours icon shows the colours menu's item text (Wimp_DecodeMenu). */
static void set_colours_icon(struct ws *w)
{
    w->decode[0] = w->selected_colours, w->decode[1] = 0xFFFFFFFFu;  /* -1 ends
                                                                   the selection */
    uint32_t r[8] = { 0, A(w->coloursmenu), A(w->decode),
                      w->colours_indirect, w->colours_size, 0 };
    if (swi(XWimp_DecodeMenu, r))
        ros_st8(w->colours_indirect, 0);
}

/* The resolution icon shows the descriptor's name. If the current mode is
 * not in the list, it shows a string such as "1024 x 768" built from the
 * mode's own size (Mode_SetWindowIcons' r3 and r4, Mode_Init's). */
static void set_resolution_icon(struct ws *w, uint32_t xres, uint32_t yres)
{
    char *buffer = ros_ptr(w->resolution_indirect);
    uint32_t size = w->resolution_size;
    uint32_t mode = w->selected_mode;
    if (mode != 0xFFFFFFFFu) {
        struct desc *d = ros_ptr(mode);
        if (desc_name(d)[0]) {
            copy_string(desc_name(d), buffer, size);
            return;
        }
        xres = d->xres, yres = d->yres;
    }
    snprintf(buffer, size, "%u x %u", xres, yres);
}

/* The rate icon shows the rate in Hz, or "Unknown". If the current mode is
 * not in the list, it shows the current mode's own rate (r6). */
static void set_rate_icon(struct ws *w, int32_t rate)
{
    char *buffer = ros_ptr(w->rate_indirect);
    uint32_t size = w->rate_size;
    uint32_t mode = w->selected_mode;
    if (mode != 0xFFFFFFFFu)
        rate = (int32_t)desc_rate(ros_ptr(mode));
    if (rate >= 0)
        snprintf(buffer, size, "%uHz", (unsigned)rate);
    else
        copy_string(lookup(w, "Unknown", w->task_name, sizeof w->task_name), buffer, size);
}

void mode_set_window_icons(struct ws *w, uint32_t xres, uint32_t yres, int32_t rate)
{
    set_colours_icon(w);
    set_resolution_icon(w, xres, yres);
    set_rate_icon(w, rate);
}

/* ---- Mouse (Mouse.s) ---------------------------------------------------------- */

/* set_icon_xy: finds the place for a pop-up menu, at the icon's top right.
 * The icon state's bounding box is at +8. The window's state is at +4,
 * after the word of flags that GetWindowInfo returns first. */
static void set_icon_xy(struct ws *w, uint32_t window, uint32_t icon, uint32_t *x,
                        uint32_t *y)
{
    uint32_t *b = w->state;
    b[0] = window, b[1] = icon;
    uint32_t r[8] = { 0, A(b) };
    if (swi(XWimp_GetIconState, r))
        return;
    uint32_t ix1 = b[4], iy1 = b[5];            /* the icon's bounding box */
    uint32_t q[8] = { 0, A(b) };                /* b[0] is still the window handle
                                                   that GetIconState left there */
    if (swi(XWimp_GetWindowInfo, q))
        return;
    int32_t x0 = (int32_t)b[1], y1 = (int32_t)b[4];
    *x = (uint32_t)(x0 + (int32_t)ix1);
    *y = (uint32_t)(y1 + (int32_t)iy1);
}

static void show_menu_at(struct ws *w, uint32_t menu, uint32_t window, uint32_t icon)
{
    uint32_t x = 0, y = 0;
    set_icon_xy(w, window, icon, &x, &y);
    menu_show(w, menu, x, y);
}

/* Mouse_Click */
static void mouse_click(struct ws *w)
{
    uint32_t x = w->poll[0], buttons = w->poll[2];
    uint32_t window = w->poll[3], icon = w->poll[4];

    if (buttons & (BUTTON_LEFT | BUTTON_RIGHT)) {       /* select or adjust */
        if (window == ICONBAR) {
            mode_init(w);                       /* set the icons for this mode */
            close_window(w, w->display_handle); /* reopen at the top, whatever its state */
            window_open_behind(w, (uint32_t)w->display_handle, 0xFFFFFFFFu, x - 64, 136);
            return;
        }
        if (window != (uint32_t)w->display_handle)
            goto mode_dialogue;
        if (icon == IC_DISPLAY_COLBUTTON || icon == IC_DISPLAY_RESBUTTON ||
            icon == IC_DISPLAY_RATEBUTTON) {
            if (buttons & BUTTON_RIGHT)         /* only SELECT pops these up */
                return;
            uint32_t menu = icon == IC_DISPLAY_COLBUTTON ?
                A(w->coloursmenu) :
                icon == IC_DISPLAY_RESBUTTON ? w->m_resolutionmenu : w->m_ratemenu;
            if (icon == IC_DISPLAY_RATEBUTTON) {
                menu_rate(w);
                if (!(w->flags & F_RATEMENUVALID))
                    return;
            }
            if (menu)
                show_menu_at(w, menu, window, icon);
            return;
        }
        if (icon == IC_DISPLAY_OK) {
            os_error *e = mode_change_mode(w);
            if (e) {
                report_error(w, e, 1);          /* the window stays open */
                return;
            }
            if (!(buttons & BUTTON_RIGHT))
                close_window(w, w->display_handle);
            return;
        }
        if (icon == IC_DISPLAY_CANCEL) {
            if (!(buttons & BUTTON_RIGHT))
                close_window(w, w->display_handle);
            else {
                mode_init(w);                   /* ADJUST restores the icons */
                icon_refresh(w);
            }
            return;
        }
        return;
    }

    if (!(buttons & BUTTON_MIDDLE))
        return;

    /* the menu button */
    if (window == ICONBAR) {
        menu_show(w, A(w->iconbarmenu), x - 64, 96 + 2 * 44);
        return;
    }
    if (window != (uint32_t)w->display_handle)
        return;
    if (icon == IC_DISPLAY_COLBUTTON) {
        show_menu_at(w, A(w->coloursmenu), window, icon);
        return;
    }
    if (icon == IC_DISPLAY_RESBUTTON) {
        show_menu_at(w, w->m_resolutionmenu, window, icon);
        return;
    }
    if (icon == IC_DISPLAY_RATEBUTTON) {
        menu_rate(w);
        if (w->flags & F_RATEMENUVALID)
            show_menu_at(w, w->m_ratemenu, window, icon);
    }
    return;

mode_dialogue:
    if (window == (uint32_t)w->mode_handle && icon == IC_MODE_OK) {
        os_error *e = mode_wimp_command(w);
        if (buttons & BUTTON_RIGHT) {           /* ADJUST shows the menu again */
            if (e)
                report_error(w, e, 1);
            if (w->menu_handle)
                menu_show(w, w->menu_handle, w->menu_x, w->menu_y);
        } else {
            w->menu_handle = 0;
            uint32_t r[8] = { 0, 0xFFFFFFFFu };
            swi(XWimp_CreateMenu, r);           /* close the menu and dialogue */
        }
    }
}

/* ---- Message (Message.s) ------------------------------------------------------- */

/* Sends a message back to its sender, moving its references on. */
static void send_ack(struct ws *w, uint32_t action)
{
    uint32_t *m = w->poll;
    m[3] = m[2];
    m[4] = action;
    uint32_t r[8] = { USER_MESSAGE, A(m), m[1] };
    swi(XWimp_SendMessage, r);
}

/* return_help: looks up the help token "H<letter><item>" and sends the
 * reply over the received message itself (Message_HelpReply's protocol).
 * The text is the message's data, at +20. */
static void return_help(struct ws *w, char letter, uint32_t item)
{
    char token[8];
    snprintf(token, sizeof token, "H%c%02X", letter, item & 0xFFu);
    uint32_t *m = w->poll;
    char *reply = (char *)&m[5];                 /* the reply's data */
    const char *text = lookup(w, token, reply, sizeof w->poll - 20);
    if (text == token)                           /* the lookup failed, so there is no help */
        return;
    m[3] = m[2];                                 /* your ref becomes their my ref */
    m[4] = MESSAGE_HELPREPLY;
    uint32_t n = copy_string_len(text, sizeof w->poll - 20);
    m[0] = (20 + n + 1 + 3) & ~3u;               /* the size of the header and the text */
    uint32_t r[8] = { USER_MESSAGE, A(m), m[1] };
    swi(XWimp_SendMessage, r);
}

/* msg_menuwarning: handles a submenu that is about to open. The icon bar
 * menu's submenus are the Info and Mode windows. The Mode dialogue's string
 * is set first. The message's data is the submenu object, x and y. */
static void msg_menuwarning(struct ws *w)
{
    const uint32_t *warn = w->poll + 5;
    mode_set_modestring(w, mode_get_current());
    uint32_t r[8] = { 0, warn[0], warn[1], warn[2] };
    swi(XWimp_CreateSubMenu, r);
}

static void message_received(struct ros_module *m, struct ws *w)
{
    uint32_t action = w->poll[4];
    switch (action) {
    case MESSAGE_QUIT:
        shutdown(m, w);
        {
            struct ros_cpu s;
            ros_cpu_enter(&s);
            ros_swi(&s, OS_Exit);
        }
        return;
    case MESSAGE_MENUWARNING:
        msg_menuwarning(w);
        return;
    case MESSAGE_MENUSDELETED:
        menu_remove_tick(w);
        w->menu_handle = 0;
        return;
    case MESSAGE_HELPREQUEST: {
        uint32_t window = w->poll[8], icon = w->poll[9];     /* data +12, +16 */
        if (window == ICONBAR)
            return return_help(w, 'B', 0);
        if (window == (uint32_t)w->info_handle)
            return return_help(w, 'I', 0);
        if (window == (uint32_t)w->display_handle)
            return return_help(w, 'D', icon);
        if (window == (uint32_t)w->mode_handle)
            return return_help(w, 'W', icon);
        if (icon == 0xFFFFFFFFu)
            return;                             /* no help off a menu item */
        {
            uint32_t r[8] = { 1, A(w->menu_state), window, icon };
            if (swi(XWimp_GetMenuState, r))
                return;
        }
        uint32_t menu = w->menu_handle;
        if (menu == A(w->iconbarmenu))
            return return_help(w, 'M', w->menu_state[0]);
        if (menu == A(w->coloursmenu))
            return return_help(w, 'C', w->menu_state[0]);
        if (menu == w->m_resolutionmenu)
            return return_help(w, 'R', w->menu_state[0]);
        if (menu == w->m_ratemenu)
            return return_help(w, 'F', w->menu_state[0]);
        return;
    }
    case MESSAGE_DATASAVE: {
        uint32_t *msg = w->poll;
        if (msg[10] != 0xFFF)                   /* FileType_Text only */
            return;
        copy_string("Wimp$Scrap", w->tok, sizeof w->tok);
        uint32_t r[8] = { A(w->tok), 0, 0xFFFFFFFFu, 0, 3 };
        if (swi(XOS_ReadVarVal, r))
            return;
        if (r[2] == 0) {
            report_error(w, error_lookup(w, ERR_NOSCRAP, "E03"), 1);
            return;
        }
        msg[9] = 0xFFFFFFFFu;                   /* any size */
        char *leaf = (char *)&msg[11];
        copy_string("<Wimp$Scrap>", leaf, 256 - 44);
        msg[0] = (A(leaf) + copy_string_len("<Wimp$Scrap>", 256 - 44) - A(msg) + 3) & ~3u;
        w->scrap_ref = (int32_t)msg[2];
        send_ack(w, MESSAGE_DATASAVEACK);
        return;
    }
    case MESSAGE_DATALOAD: {
        uint32_t *msg = w->poll;
        if (msg[10] != 0xFFF && msg[10] != 0xF95)  /* Text or EDID */
            return;
        send_ack(w, MESSAGE_DATALOADACK);
        char *leaf = (char *)&msg[11];
        snprintf(w->cli, sizeof w->cli, "LoadModeFile ");
        copy_string(leaf, w->cli + 13, sizeof w->cli - 13);
        uint32_t r[8] = { A(w->cli) };
        os_error *e = swi(XOS_CLI, r);
        if (e)
            report_error(w, e, 1);
        if ((int32_t)msg[3] == w->scrap_ref) {  /* the reply to our DataSaveAck */
            w->scrap_ref = -1;
            uint32_t d[8] = { A(w->delete_scrap) };
            swi(XOS_CLI, d);
        }
        return;
    }
    default:
        return;
    }
}

/* ---- the task (Module.s Mod_Start) -----------------------------------------------
 *
 * Mod_PollWord: does at once whatever a service asked for. */
static void pollword(struct ros_module *m, struct ws *w)
{
    if (w->pollword & PF_SETTITLE)
        window_set_title(w);
    if (w->pollword & PF_MODEINIT)
        mode_init(w);
    if (w->pollword & PF_REFRESHICONS)
        icon_refresh(w);
    w->pollword = 0;
    (void)m;
}

static void run(struct ros_module *m, struct ws *w)
{
    strcpy(w->messages_name, "DisplayManager:Messages");
    strcpy(w->templates_name, "DisplayManager:Templates");
    strcpy(w->sprite_name, "display");
    strcpy(w->delete_scrap, "%Delete <Wimp$Scrap>");
    memcpy(w->iconbar_def, rom_iconbar_menu, sizeof rom_iconbar_menu);
    memcpy(w->colours_def, rom_colours_menu, sizeof rom_colours_menu);
    memcpy(w->resolution_def, rom_resolution_menu, sizeof rom_resolution_menu);
    memcpy(w->rate_def, rom_rate_menu, sizeof rom_rate_menu);
    lookup(w, "Title", w->task_name, sizeof w->task_name);
    w->messages[0] = MESSAGE_MENUWARNING;
    w->messages[1] = MESSAGE_MENUSDELETED;
    w->messages[2] = MESSAGE_HELPREQUEST;
    w->messages[3] = MESSAGE_DATASAVE;
    w->messages[4] = MESSAGE_DATALOAD;
    w->messages[5] = 0;
    uint32_t r[8] = { 300, TASK_WORD, A(w->task_name), A(w->messages) };
    os_error *e = swi(XWimp_Initialise, r);
    if (!e && r[0] < 300)                       /* the Wimp version that the ObjAsm asks for */
        e = error_lookup(w, ERR_NOWIMP, "NoWimp");
    if (!e) {
        w->task_handle = r[1];
        e = icon_init(w);
    }
    if (!e)
        e = window_init(w);
    if (!e)
        e = menu_init(w);
    if (!e)
        e = mode_init(w);
    w->scrap_ref = -1;
    w->pollword = 0;
    if (e) {
        report_error(w, e, 2);                  /* Cancel only: error_abort */
        shutdown(m, w);
        ros_st32(m->private_word, 0xFFFFFFFFu); /* it must not be started again */
        struct ros_cpu s;
        ros_cpu_enter(&s);
        ros_swi(&s, OS_Exit);
        return;
    }

    for (;;) {
        if (e) {
            report_error(w, e, 1);
            e = NULL;
        }
        uint32_t p[8] = { POLL_MASK, A(w->poll), 0, A(&w->pollword) };
        e = swi(XWimp_Poll, p);
        if (e)
            continue;                           /* poll again, as the ObjAsm does */
        switch (p[0]) {
        case OPEN_WINDOW:
            window_open(w);
            break;
        case CLOSE_WINDOW:
            window_close(w);
            break;
        case MOUSE_CLICK:
            mouse_click(w);
            break;
        case KEY_PRESSED:
            mode_key_pressed(w);
            break;
        case MENU_SELECTION:
            menu_selection(w);
            break;
        case POLLWORD_NONZERO:
            pollword(m, w);
            break;
        case USER_MESSAGE:
        case USER_MESSAGE_RECORDED:
            message_received(m, w);
            break;
        default:
            break;
        }
    }
}

/* ---- the module: services, the command, finalisation ------------------------- */

/* Mod_ShutDown (Module.s): closes the task and the messages file, frees
 * the blocks and clears the private word. */
static void shutdown(struct ros_module *m, struct ws *w)
{
    if (w->task_handle) {
        uint32_t r[8] = { (uint32_t)w->task_handle, TASK_WORD };
        swi(XWimp_CloseDown, r);
        w->task_handle = 0;
    }
    if (w->flags & F_MESSAGESOPEN) {
        uint32_t r[8] = { A(w->message_file_block) };
        swi(XMessageTrans_CloseFile, r);
        w->flags &= ~F_MESSAGESOPEN;
    }
    if (w->indirected_data) {
        xos_module_free(ros_ptr(w->indirected_data));
        w->indirected_data = 0;
    }
    if (w->mode_space) {
        xos_module_free(ros_ptr(w->mode_space));
        w->mode_space = 0;
    }
    ros_st32(m->private_word, 0);
    xos_module_free(w);
}

/* Mod_Init (Module.s): sets DisplayManager$Path, if nothing else has set
 * it. The ObjAsm writes "$$Path", where "$$" is an escaped "$". */
static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail, (void)m;
    /* These are the strings that a SWI takes by address. They are claimed
     * from the RMA for the life of the module, because there is no workspace
     * until Service_StartWimp. */
    static uint32_t strings;                    /* arena: "name\0default" */
    if (!strings) {
        char *block = ros_rma_alloc(64);
        if (!block)
            return NULL;
        strings = ros_addr(block);
        strcpy(block, "DisplayManager$Path");
        strcpy(block + 24, "Resources:$.Resources.Display.");
    }
    uint32_t r[8] = { strings, 0, 0xFFFFFFFFu, 0, 3 };
    int there = !swi(XOS_ReadVarVal, r) && r[2] != 0;
    if (there)
        return NULL;
    r[0] = strings, r[1] = strings + 24;
    r[2] = 31, r[3] = 0, r[4] = 0;              /* R2 is the length with its
                                               terminator and R4 is
                                               VarType_String */
    swi(XOS_SetVarVal, r);
    return NULL;
}

/* The services (Module.s Mod_Service). */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    struct ws *w = workspace(m);
    switch (s->r[1]) {
    case SERVICE_STARTWIMP:
        if (w)
            return;                             /* already running, or starting */
        {
            void *block;
            if (xos_module_claim(sizeof *w, &block)) {
                s->r[1] = 0;                    /* claim the service without
                                                   providing a command, so
                                                   that the Wimp tries again */
                return;
            }
            w = block;
            memset(w, 0, sizeof *w);
            w->scrap_ref = -1;
            ros_st32(m->private_word, A(w));
            strcpy(w->command, "Desktop_DisplayManager");
            s->r[0] = A(w->command);
            s->r[1] = 0;                        /* claim the service */
        }
        return;
    case SERVICE_STARTEDWIMP:
        if (ros_ld32(m->private_word) == 0xFFFFFFFFu)
            ros_st32(m->private_word, 0);       /* an earlier start gave up */
        return;
    case SERVICE_RESET:
        if (!w) {
            if (ros_ld32(m->private_word) == 0xFFFFFFFFu)
                ros_st32(m->private_word, 0);
            return;
        }
        w->task_handle = 0;                     /* the Wimp has already gone */
        shutdown(m, w);
        return;
    case SERVICE_MODECHANGE:
        if (w && w->task_handle)
            w->pollword |= PF_MODEINIT | PF_REFRESHICONS;
        return;
    case SERVICE_WIMPPALETTE:
    case SERVICE_CALIBRATIONCHANGED: {
        if (!w || !w->task_handle)
            return;
        w->pollword |= PF_MODEINIT | PF_REFRESHICONS;
        uint32_t r[8] = { 0 };
        swi(XColourTrans_InvalidateCache, r);
        /* scanpalette: read the Wimp's palette and look for changes. If it
         * changed, redraw the screen and broadcast Message_PaletteChange. */
        r[0] = 0, r[1] = A(w->palette);
        if (!swi(XWimp_ReadPalette, r)) {
            int changed = 0;
            for (int i = 0; i < 16; i++) {
                uint8_t was = w->wimp_palette[i];
                w->wimp_palette[i] = (uint8_t)w->palette[i];
                if (was != (uint8_t)w->palette[i])
                    changed = 1;
            }
            if (changed) {
                uint32_t f[8] = { 0xFFFFFFFFu, 0xFFFFFFFFu - 0x0FFFFFFFu,
                                  0xFFFFFFFFu - 0x0FFFFFFFu, 0x0FFFFFFFu, 0x0FFFFFFFu };
                swi(XWimp_ForceRedraw, f);
                w->palette_message[0] = 20;
                w->palette_message[4] = MESSAGE_PALETTECHANGE;
                uint32_t q[8] = { USER_MESSAGE, A(w->palette_message), 0 };
                swi(XWimp_SendMessage, q);
            }
        }
        return;
    }
    case SERVICE_MODEFILECHANGED:
        if (!w || !w->task_handle)
            return;
        menu_init(w);
        /* Redraw the icon, in case the 'display' sprite changed. */
        {
            uint32_t *b = w->state;
            b[0] = ICONBAR, b[1] = (uint32_t)w->icon_handle;
            uint32_t r[8] = { 0, A(b) };
            if (!swi(XWimp_GetIconState, r)) {
                uint32_t f[8] = { ICONBAR, b[2], b[3], b[4], b[5] };   /* its bounding box */
                swi(XWimp_ForceRedraw, f);
            }
        }
        w->pollword |= PF_MODEINIT | PF_REFRESHICONS | PF_SETTITLE;
        return;
    default:
        return;
    }
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct ws *w = workspace(m);
    if (w)
        shutdown(m, w);
    return NULL;                                /* the module never refuses to die */
}

/* The start entry (Module.s Mod_Start). It runs the task once
 * Service_StartWimp has given the module its workspace. */
static void start(struct ros_module *m, uint32_t tail)
{
    (void)tail;
    struct ws *w = workspace(m);
    if (!w || w->task_handle) {
        /* Mod_Start's error: there is no workspace, or the task already exists. */
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = A(w ? error_lookup(w, ERR_CANTSTART, "UseDesk")   /* already running: looked up */
                     : ros_error(ERR_CANTSTART, "UseDesk"));        /* no workspace: built in the arena */
        ros_swi(&s, OS_GenerateError);          /* to the program's handler */
        return;
    }
    run(m, w);
}

/* *Desktop_DisplayManager: enters the module as the application, which
 * runs the task (Desktop_DisplayManager_Code). */
static os_error *cmd_desktop(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)tail, (void)argc;
    if (!workspace(m)) {
        return ros_error(ERR_CANTSTART, "UseDesk");
    }
    uint32_t r[8] = { 2, m->base + ros_ld32(m->base + 0x10), 0 };  /* OS_Module 2 */
    return swi(XOS_Module, r);                  /* this returns only with an error */
}

/* This is for the self-test (boot/selftest_display.c). It builds the menus'
 * lists as the task builds them, in a workspace of its own. Then it takes
 * each resolution item as a choice. It checks that the item maps to its
 * class and back. It looks for a mode at 16 million and at 256 colours.
 * It also turns the selector that the Change button builds into a mode
 * string with OS_ScreenMode 14, as *WimpMode needs. It returns 0, or -1
 * with the reason in why. */
int ros_display_check_menus(char *why, size_t size)
{
    void *block;
    if (xos_module_claim(sizeof(struct ws), &block)) {
        snprintf(why, size, "no workspace");
        return -1;
    }
    struct ws *w = block;
    memset(w, 0, sizeof *w);
    strcpy(w->messages_name, "DisplayManager:Messages");
    memcpy(w->resolution_def, rom_resolution_menu, sizeof rom_resolution_menu);
    int bad = 0;
    if (menu_resolution(w) || w->resolution_count == 0) {
        snprintf(why, size, "no resolution menu");
        bad = -1;
    }
    for (uint32_t i = 0; !bad && i < w->resolution_count; i++) {
        uint8_t class = resolution_to_class(w, (uint8_t)i);
        if (class >= w->class_count || class_to_resolution(w, class) != i) {
            snprintf(why, size, "item %u: class %u of %u", i, class, w->class_count);
            bad = -1;
            break;
        }
        static const uint8_t colours[] = { MO_CO_16M, MO_CO_COLOUR256 };
        for (uint32_t k = 0; !bad && k < sizeof colours; k++) {
            uint8_t c3 = colours[k], c4 = class;
            uint32_t subclass, mode;
            if (mode_find_subclass(w, 4u, &c3, &c4, -1, &subclass, &mode) || c4 != class) {
                snprintf(why, size, "item %u colours %u: no mode", i, colours[k]);
                bad = -1;
                break;
            }
            uint32_t r[8] = { 14, mode_build_specifier(w, ros_ptr(mode), 0), A(w->cli),
                              sizeof w->cli };
            struct desc *d = ros_ptr(mode);
            char want[24];
            snprintf(want, sizeof want, "X%u Y%u ", d->xres, d->yres);
            if (swi(XOS_ScreenMode, r) || (int32_t)r[3] < 0 ||
                strncmp(w->cli, want, strlen(want)) != 0) {
                snprintf(why, size, "item %u colours %u: no mode string", i, colours[k]);
                bad = -1;
            }
        }
    }
    if (w->flags & F_MESSAGESOPEN) {
        uint32_t r[8] = { A(w->message_file_block) };
        swi(XMessageTrans_CloseFile, r);
    }
    if (w->mode_space)
        xos_module_free(ros_ptr(w->mode_space));
    xos_module_free(w);
    return bad;
}

static const struct ros_command commands[] = {
    { "Desktop_DisplayManager", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Desktop_DisplayManager",
      "The Display Manager module allows the selection of screen modes.\r"
      "Do not use *Desktop_DisplayManager, use *Desktop instead.\r",
      cmd_desktop },
    { 0 },
};

struct ros_module display_module = {
    .title = "DisplayManager",
    .help = "DisplayManager\t0.44 (29 May 2016) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .start = start,
    .commands = commands,
};
