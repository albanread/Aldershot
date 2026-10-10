/* wimp.c -- the native Window Manager's module: its workspace, its errors,
 * its SWI table, and the SWIs too small for a file of their own.
 *
 * It is the box's Window Manager.  rosgd.wimp=translated boots the
 * translated 5.30 one instead (runtime/rom.c).  A SWI with no thunk would
 * answer "Wimp_<name> is not in the native Wimp yet".  Every SWI now has
 * one. */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/environment.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"
#include "wimp.h"

static const char *const swi_names[] = {
    "Initialise", "CreateWindow", "CreateIcon", "DeleteWindow", "DeleteIcon", "OpenWindow",
    "CloseWindow", "Poll", "RedrawWindow", "UpdateWindow", "GetRectangle", "GetWindowState",
    "GetWindowInfo", "SetIconState", "GetIconState", "GetPointerInfo", "DragBox", "ForceRedraw",
    "SetCaretPosition", "GetCaretPosition", "CreateMenu", "DecodeMenu", "WhichIcon", "SetExtent",
    "SetPointerShape", "OpenTemplate", "CloseTemplate", "LoadTemplate", "ProcessKey", "CloseDown",
    "StartTask", "ReportError", "GetWindowOutline", "PollIdle", "PlotIcon", "SetMode",
    "SetPalette", "ReadPalette", "SetColour", "SendMessage", "CreateSubMenu", "SpriteOp",
    "BaseOfSprites", "BlockCopy", "SlotSize", "ReadPixTrans", "ClaimFreeMemory", "CommandWindow",
    "TextColour", "TransferBlock", "ReadSysInfo", "SetFontColours", "GetMenuState",
    "RegisterFilter", "AddMessages", "RemoveMessages", "SetColourMapping", "TextOp",
    "SetWatchdogState", "Extend", "ResizeIcon", "AutoScroll", NULL,
};
#define WIMP_SWIS 62u

/* ---- the workspace --------------------------------------------------------- */

struct wimp_ws *wimp_ws(void)
{
    uint32_t pw = wimp_module.private_word ? ros_ld32(wimp_module.private_word) : 0;
    return pw ? ros_ptr(pw) : NULL;
}

/* ---- errors ----------------------------------------------------------------- */

os_error *wimp_error(uint32_t num)
{
    const char *text;
    switch (num) {
    case E_BAD_OP:      text = "Invalid Wimp operation in this context"; break;
    case E_BAD_HANDLE:  text = "Illegal window handle"; break;
    case E_BAD_VERSION: text = "Bad version number passed to Wimp_Initialise"; break;
    case E_BAD_MESSAGE: text = "Message block is too big / not a multiple of 4"; break;
    case E_BAD_REASON:  text = "Illegal reason code given to SendMessage"; break;
    case E_BAD_TASK:    text = "Illegal task handle"; break;
    case E_BAD_SYSINFO: text = "Bad parameter passed to Wimp in R0"; break;
    case E_BAD_PTR_R1:  text = "Bad pointer passed to Wimp in R1"; break;
    case E_BAD_R3:      text = "Illegal Wimp_Poll pointer in R3"; break;
    default:            text = "Wimp error"; break;
    }
    return ros_error(num, "%s", text);
}

void wimp_fail(struct ros_cpu *s, os_error *e)
{
    ros_swi_fail(s, e);
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m;
    if (offset < WIMP_SWIS)
        return ros_error(E_BAD_OP, "Wimp_%s is not in the native Wimp yet", swi_names[offset]);
    return wimp_error(E_BAD_OP);                /* &400FE, &400FF: not Wimp SWIs */
}

/* ---- Wimp_ReadSysInfo -------------------------------------------------------- */

static uint8_t cmos(uint32_t addr)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 161, c.r[1] = addr;
    ros_swi(&c, XOS_Byte);
    return (uint8_t)c.r[2];
}

static void swi_ReadSysInfo(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_task *t = wimp_current();
    switch (s->r[0]) {
    case 0:                                     /* tasks active */
        s->r[0] = w->ntasks;
        break;
    case 2: {                                   /* the icon sprite suffix: romspr_suffix */
        uint8_t *sfx = w->scratch + 480;
        memcpy(sfx, w->rom_suffix, 3);
        if (!sfx[0])
            sfx[0] = (uint8_t)('0' + (1 << w->xeig)), sfx[1] = (uint8_t)('0' + (1 << w->yeig)), sfx[2] = 0;
        s->r[0] = ros_addr(sfx);
        break;
    }
    case 3:                                     /* the desktop: 1 while a task exists */
        s->r[0] = w->ntasks && w->err.commandflag != 2 ? 1 : 0;   /* 0 if commandflag is exactly 2, active */
        break;
    case 4:                                     /* the write direction (*WimpWriteDir) */
        s->r[0] = 0;
        break;
    case 5:                                     /* the current task, its version */
        if (t && t->live) {
            s->r[0] = t->handle;
            s->r[1] = t->version;
        } else {
            s->r[0] = 0;
        }
        break;
    case 7:                                     /* the Wimp's version */
        s->r[0] = WIMP_VERSION;
        break;
    case 8:                                     /* the desktop and symbol fonts: 0, the
                                                   system font, for both */
        s->r[0] = 0;
        s->r[1] = 0;
        break;
    case 9:                                     /* the tool sprite area */
        s->r[0] = w->tools;
        break;
    case 11:                                    /* application space when the Wimp started */
        s->r[0] = w->start_app_space;
        break;
    case 13:                                    /* 5.30's answer, the pointer dropped */
        s->r[0] = 13;
        break;
    case 21:                                    /* text selection, as 5.30 */
        s->r[0] = 0xFFFFFFFFu;
        s->r[1] = 0xFFFFFFFFu;
        s->r[2] = 0x5Bu;
        break;
    case 29:                                    /* no alpha-masked sprites */
        s->r[0] = 0;
        break;
    case 1:                                     /* the Wimp's mode */
        s->r[0] = w->mode;
        break;
    case 16:                                    /* the low and high sprite pools */
        s->r[0] = w->rom_sprites, s->r[1] = w->ram_sprites;
        break;
    case 17: {                                  /* the autoscroll pause, cs */
        uint8_t dd = cmos(0xDD), de = cmos(0xDE);
        s->r[0] = (((uint32_t)dd >> 4) ^ 5u) * ((de & 2u) ? 10u : 1u) * 10u;
        break;
    }
    case 23:                                    /* the drag limits */
        s->r[0] = w->in.drag_move, s->r[1] = w->in.drag_time;
        break;
    case 24:                                    /* the double-click limits */
        s->r[0] = w->in.dclick_move, s->r[1] = w->in.dclick_time;
        break;
    case 25:                                    /* the menu delays */
        s->r[0] = w->mn.timelimit, s->r[1] = w->mn.dragdelay;
        break;
    case 26: {                                  /* the iconbar's scroll speed and acceleration */
        static const uint32_t log[8] = { 0, 20, 50, 100, 200, 500, 1000, 2000 };
        s->r[0] = log[((cmos(0x17) >> 5) ^ 4u) & 7u];
        s->r[1] = log[((cmos(0x1B) >> 5) ^ 3u) & 7u];
        break;
    }
    case 28:                                    /* ThreeDFlags */
        s->r[0] = w->dr.threed;
        break;
    default:                                    /* 6, 10, 12, 14, 15, 18-20, 22, 27, >= 30 */
        wimp_fail(s, wimp_error(E_BAD_SYSINFO));
        return;
    }
    s->v = 0;
}

/* ---- Wimp_Extend -------------------------------------------------------------- */

static uint32_t tree(uint32_t reason, uint32_t handle, int *bad)
{
    if (handle == 0xFFFFFFFFu) {                /* the top level */
        struct wimp_window *x = wimp_stack_front(NO_WINDOW, REQ), *last = NULL;
        if (reason == 7)
            return x ? x->handle : 0xFFFFFFFFu;
        if (reason == 8) {
            while (x)
                last = x, x = wimp_window_below(x, REQ);
            return last ? last->handle : 0xFFFFFFFFu;
        }
        return 0xFFFFFFFFu;
    }
    struct wimp_window *win = wimp_window(handle);
    if (!win) {
        *bad = 1;
        return 0;
    }
    struct wimp_window *x = wimp_stack_front(win->handle, REQ), *last = NULL;
    switch (reason) {
    case 6:
        return win->s[REQ].parent;
    case 7:
        return x ? x->handle : 0xFFFFFFFFu;
    case 8:
        while (x)
            last = x, x = wimp_window_below(x, REQ);
        return last ? last->handle : 0xFFFFFFFFu;
    case 9:                                     /* the sibling in front */
        if (!win->s[REQ].open)
            return 0xFFFFFFFFu;
        for (x = wimp_stack_front(win->s[REQ].parent, REQ); x && x != win;
             x = wimp_window_below(x, REQ))
            last = x;
        return last ? last->handle : 0xFFFFFFFFu;
    default:                                    /* 10: the sibling behind */
        if (!win->s[REQ].open)
            return 0xFFFFFFFFu;
        x = wimp_window_below(win, REQ);
        return x ? x->handle : 0xFFFFFFFFu;
    }
}

/* Jump table entry 0, getspriteaddr: finds the name at reason 4's word
 * in the sprite pools.  On exit R2 is the sprite with V clear, or R2 = 0
 * with V set. */
static void ext_getspriteaddr(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t p = w->ext_name;
    char *name = (char *)w->scratch + 496;
    unsigned n = 0;
    for (; n < 12 && p; n++) {
        uint32_t c = ros_ld8(p + n);
        if (c <= 32)
            break;
        name[n] = (char)c;
    }
    name[n] = 0;
    uint32_t area, sprite = n ? wimp_pool_find(ros_addr(name), &area) : 0;
    s->r[2] = sprite;
    s->v = sprite == 0;
    s->r[15] = s->r[14];
}

/* Jump table entries 1-7 have no callers.  They give BadOp. */
static void ext_dropped(struct ros_cpu *s)
{
    s->r[0] = ros_addr(wimp_error(E_BAD_OP));
    s->v = 1;
    s->r[15] = s->r[14];
}

/* Reason 11: the furniture's sizes, for a window, or in general (handle 0) */
static os_error *furniture_sizes(uint32_t b)
{
    struct wimp_ws *w = wimp_ws();
    const struct wimp_furniture *fu = &w->furn;
    if (b < ROS_APP_BASE)
        return wimp_error(E_BAD_PTR_R1);
    uint32_t h = ros_ld32(b), f;
    struct wimp_window *win = NULL;
    struct wimp_box v = { 0, 0, 0, 0 };
    int32_t top, right, bottom;
    int toplevel;
    if (h) {
        win = wimp_window_ib(h);
        if (!win)
            return wimp_error(E_BAD_HANDLE);
        v = win->s[APP].vis;
        struct wimp_box o = win->s[APP].outline;
        ros_st32(b + 4, (uint32_t)(v.x0 - o.x0));
        ros_st32(b + 8, (uint32_t)(v.y0 - o.y0));
        ros_st32(b + 12, (uint32_t)(o.x1 - v.x1));
        ros_st32(b + 16, (uint32_t)(o.y1 - v.y1));
        top = bottom = o.x1 - o.x0;
        right = o.y1 - o.y0;
        f = win->s[APP].flags;
        toplevel = win->s[APP].parent == NO_WINDOW;
    } else {
        ros_st32(b + 4, (uint32_t)w->dx);
        ros_st32(b + 8, (uint32_t)(fu->H + w->dy));
        ros_st32(b + 12, (uint32_t)(fu->V + w->dx));
        ros_st32(b + 16, (uint32_t)(fu->T + w->dy));
        top = right = bottom = 0x200;
        f = 0x7F000000u;                        /* every furniture flag */
        toplevel = 1;
    }
    uint32_t back = (f & (1u << 24)) ? (uint32_t)fu->B : 0, close = (f & (1u << 25)) ? (uint32_t)fu->C : 0;
    uint32_t iconise = close && toplevel ? (uint32_t)fu->I : 0;
    top -= (int32_t)(back + close + iconise);
    ros_st32(b + 20, back), ros_st32(b + 24, close), ros_st32(b + 40, iconise);
    uint32_t togw = (f & (1u << 27)) ? (uint32_t)fu->V : 0, togh = (f & (1u << 27)) ? (uint32_t)fu->T : 0;
    top -= (int32_t)togw, right -= (int32_t)togh;
    ros_st32(b + 44, togw), ros_st32(b + 48, togh);
    ros_st32(b + 32, (f & (1u << 26)) ? (uint32_t)top : 0);
    ros_st32(b + 28, 0), ros_st32(b + 36, 0);
    /* adjust-size: both scroll bars, or the size icon */
    int adjust = ((f & (1u << 28)) && (f & (1u << 30))) || (f & (1u << 29));
    uint32_t adjh = adjust ? (uint32_t)fu->H : 0, adjw = adjust ? (uint32_t)fu->V : 0;
    right -= (int32_t)adjh, bottom -= (int32_t)adjw;
    ros_st32(b + 72, adjh), ros_st32(b + 76, adjw);
    for (int vert = 1; vert >= 0; vert--) {
        uint32_t at = vert ? 52 : 80;           /* gap1, near arrow, well, far arrow, gap0 */
        if (!(f & (vert ? 1u << 28 : 1u << 30))) {
            for (int k = 0; k < 5; k++)
                ros_st32(b + at + 4 * (uint32_t)k, 0);
            continue;
        }
        int32_t g0 = 0, g1 = 0;
        if (win) {
            struct wimp_box bar, well, sausage;
            wimp_scroll_geom(win, vert, &bar, &well, &sausage);
            if (vert)
                g0 = bar.y0 - v.y0, g1 = v.y1 - bar.y1;
            else
                g0 = bar.x0 - v.x0, g1 = v.x1 - bar.x1;
        }
        int32_t len = (vert ? right : bottom) - g0 - g1, a0, a1;   /* a0 down/left, a1 up/right */
        if (len <= 0) {
            a0 = a1 = 0;
        } else {
            a0 = vert ? fu->D : fu->L, a1 = vert ? fu->U : fu->R;
            if (a0 + a1 > len)
                a0 = len >> 1, a1 = len - a0;
        }
        ros_st32(b + at, (uint32_t)g1);
        ros_st32(b + at + 4, (uint32_t)a1);
        ros_st32(b + at + 8, (uint32_t)(len - a0 - a1));
        ros_st32(b + at + 12, (uint32_t)a0);
        ros_st32(b + at + 16, (uint32_t)g0);
    }
    return NULL;
}

static void swi_Extend(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    switch (s->r[0]) {
    case 1:                                     /* the value for R12 */
        s->r[0] = ros_addr(w);
        break;
    case 2:                                     /* the jump table */
        /* BL_Wimp jumps to the table's address + 4k.  The table is eight
         * code addresses in a row, given out as native entries. */
        if (!w->ext_jump[0]) {
            w->ext_jump[0] = ros_native_entry(ext_getspriteaddr, "wimp:getspriteaddr");
            for (int k = 1; k < 8; k++)
                w->ext_jump[k] = ros_native_entry(ext_dropped, "wimp:extend-dropped");
        }
        s->r[0] = w->ext_jump[0];
        break;
    case 4:                                     /* the name word and the may-call word */
        w->ext_ok = 1;
        s->r[0] = ros_addr(&w->ext_name);
        s->r[1] = ros_addr(&w->ext_ok);
        break;
    case 6: case 7: case 8: case 9: case 10: {  /* the window tree */
        int bad = 0;
        uint32_t r = tree(s->r[0], s->r[1], &bad);
        if (bad) {
            wimp_fail(s, wimp_error(E_BAD_HANDLE));
            return;
        }
        s->r[1] = r;
        break;
    }
    case 11: {                                  /* furniture sizes */
        os_error *e = furniture_sizes(s->r[1]);
        if (e) {
            wimp_fail(s, e);
            return;
        }
        break;
    }
    case 14: {                                  /* an icon's validation string */
        struct wimp_window *win = wimp_window(s->r[1]);
        if (!win) {
            wimp_fail(s, wimp_error(E_BAD_HANDLE));
            return;
        }
        const uint8_t *icon = wimp_icon(win, s->r[2]);
        if (!icon) {
            wimp_fail(s, ros_error(0x2A1, "Illegal icon handle"));
            return;
        }
        uint32_t f, v;
        memcpy(&f, icon + 16, 4);
        memcpy(&v, icon + 24, 4);
        if ((f & 0x101u) != 0x101u || (int32_t)v <= 0) {
            s->r[4] = 0;
            break;
        }
        /* The string is in the owner's memory.  It is paged in and copied
         * through the Wimp's buffer, so it can reach R3 in any memory. */
        struct wimp_task *ot = (int32_t)win->owner > 0 ? wimp_task_by_handle(win->owner, 0) : wimp_menu_owner();
        uint8_t *tmp = w->scratch + 256;
        const struct ros_slot *was = ros_task_page_in(ot ? ot->rt : NULL);
        uint32_t len = 0;
        while (len < 255 && ros_ld8(v + len) >= ' ') {
            tmp[len] = (uint8_t)ros_ld8(v + len);
            len++;
        }
        ros_task_page_back(was);
        len++;
        if (s->r[4] >= len) {
            for (uint32_t i = 0; i + 1 < len; i++)
                ros_st8(s->r[3] + i, tmp[i]);
            ros_st8(s->r[3] + len - 1, 0);
        }
        s->r[4] -= len;
        break;
    }
    case 15:                                    /* slot sizes in pages (task.c) */
        wimp_extend_read_slot_size(s);
        break;
    case WIMP_SURFACE_REASON: case WIMP_SURFACE_REASON + 1:
    case WIMP_SURFACE_REASON + 2: case WIMP_SURFACE_REASON + 3:
        wimp_extend_surface(s);                 /* ROSGD: surface windows (surface.c) */
        return;
    default:                                    /* unknown: everything as it was */
        break;
    }
    s->v = 0;
}

static ros_swi_thunk *const thunks[64] = {
    [0x00] = wimp_swi_Initialise,
    [0x01] = wimp_swi_CreateWindow,
    [0x02] = wimp_swi_CreateIcon,
    [0x03] = wimp_swi_DeleteWindow,
    [0x04] = wimp_swi_DeleteIcon,
    [0x05] = wimp_swi_OpenWindow,
    [0x06] = wimp_swi_CloseWindow,
    [0x07] = wimp_swi_Poll,
    [0x08] = wimp_swi_RedrawWindow,
    [0x09] = wimp_swi_UpdateWindow,
    [0x0A] = wimp_swi_GetRectangle,
    [0x0B] = wimp_swi_GetWindowState,
    [0x0C] = wimp_swi_GetWindowInfo,
    [0x0D] = wimp_swi_SetIconState,
    [0x0E] = wimp_swi_GetIconState,
    [0x0F] = wimp_swi_GetPointerInfo,
    [0x10] = wimp_swi_DragBox,
    [0x11] = wimp_swi_ForceRedraw,
    [0x12] = wimp_swi_SetCaretPosition,
    [0x13] = wimp_swi_GetCaretPosition,
    [0x14] = wimp_swi_CreateMenu,
    [0x15] = wimp_swi_DecodeMenu,
    [0x16] = wimp_swi_WhichIcon,
    [0x17] = wimp_swi_SetExtent,
    [0x18] = wimp_swi_SetPointerShape,
    [0x19] = wimp_swi_OpenTemplate,
    [0x1A] = wimp_swi_CloseTemplate,
    [0x1B] = wimp_swi_LoadTemplate,
    [0x1C] = wimp_swi_ProcessKey,
    [0x1D] = wimp_swi_CloseDown,
    [0x1E] = wimp_swi_StartTask,
    [0x1F] = wimp_swi_ReportError,
    [0x22] = wimp_swi_PlotIcon,
    [0x23] = wimp_swi_SetMode,
    [0x20] = wimp_swi_GetWindowOutline,
    [0x21] = wimp_swi_PollIdle,
    [0x24] = wimp_swi_SetPalette,
    [0x25] = wimp_swi_ReadPalette,
    [0x26] = wimp_swi_SetColour,
    [0x27] = wimp_swi_SendMessage,
    [0x29] = wimp_swi_SpriteOp,
    [0x2A] = wimp_swi_BaseOfSprites,
    [0x28] = wimp_swi_CreateSubMenu,
    [0x2B] = wimp_swi_BlockCopy,
    [0x2C] = wimp_swi_SlotSize,
    [0x2D] = wimp_swi_ReadPixTrans,
    [0x2E] = wimp_swi_ClaimFreeMemory,
    [0x2F] = wimp_swi_CommandWindow,
    [0x30] = wimp_swi_TextColour,
    [0x31] = wimp_swi_TransferBlock,
    [0x32] = swi_ReadSysInfo,
    [0x33] = wimp_swi_SetFontColours,
    [0x34] = wimp_swi_GetMenuState,
    [0x3A] = wimp_swi_SetWatchdogState,
    [0x3B] = swi_Extend,
    [0x35] = wimp_swi_RegisterFilter,
    [0x36] = wimp_swi_AddMessages,
    [0x38] = wimp_swi_SetColourMapping,
    [0x39] = wimp_swi_TextOp,
    [0x37] = wimp_swi_RemoveMessages,
    [0x3C] = wimp_swi_ResizeIcon,
    [0x3D] = wimp_swi_AutoScroll,
};

static const struct ros_command commands[] = {
    { "WimpTask", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *WimpTask <*command>",
      "*WimpTask starts up a new task with the given command line.", wimp_cmd_wimptask },
    { "Pointer", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *Pointer [0|1]",
      "*Pointer turns the mouse pointer on or off.", wimp_cmd_pointer },
    { "ToolSprites", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *ToolSprites [<filename>]",
      "*ToolSprites loads the sprites used for the window borders.", wimp_cmd_toolsprites },
    { "WimpMode", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *WimpMode <mode>",
      "*WimpMode changes the screen mode the desktop uses.", wimp_cmd_wimpmode },
    { "WimpWriteDir", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *WimpWriteDir 0|1",
      "*WimpWriteDir sets the direction of text in writable icons.", wimp_cmd_wimpwritedir },
    { "WimpKillSprite", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *WimpKillSprite <spritename>",
      "*WimpKillSprite deletes a sprite from the Wimp's RAM sprite pool.", wimp_cmd_wimpkillsprite },
    { "WimpVisualFlags", ROS_CMD_INFO(0, 255, 0, 0), "Syntax: *WimpVisualFlags [options]",
      "*WimpVisualFlags sets how the Wimp draws its 3D effects.", wimp_cmd_wimpvisualflags },
    { "IconSprites", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *IconSprites <filename>",
      "*IconSprites merges the sprites in a file with those in the Wimp's sprite pool.",
      wimp_cmd_iconsprites },
    { "WimpSlot", ROS_CMD_INFO(0, 6, 0, 0),
      "Syntax: *WimpSlot [-min] <size>[K|M|G] [-max <size>[K|M|G]] [-next <size>[K|M|G]]",
      "*WimpSlot changes the memory allocation of the current task, and of the next task "
      "to start.", wimp_cmd_wimpslot },
    { "WimpStats", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *WimpStats [-reset]",
      "*WimpStats lists each task's null events and how many of them were paced: held back "
      "a few milliseconds because the task takes them back to back (Wimp$NullPace, in ms "
      "or Hz, 0 off). -reset zeroes the counts.", wimp_cmd_wimpstats },
    { 0 },
};

/* ChangeEnvCode: a handler that is set (and not only read) is marked in
 * handlerword, so that setdefaulthandlers leaves it alone */
static int changeenv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct wimp_ws *w = wimp_ws();
    if (w && s->r[1] && s->r[0] < 32)
        w->handlerword |= 1u << s->r[0];
    return ROS_VECTOR_PASS;
}

/* Wimp$State (CommandWindow_var in s/Wimp01) is a code variable.  Writing
 * to it has no effect.  Reading it gives "desktop" or "commands", as
 * Wimp_ReadSysInfo 3 says. */
static void state_write(struct ros_cpu *s)
{
    s->v = 0;
    s->r[15] = s->r[14];
}

static void state_read(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 3;
    ros_swi(&c, XWimp_ReadSysInfo);
    uint32_t text = w->statevar + 16 + (c.r[0] ? 12 : 0);
    s->r[0] = text;
    s->r[2] = (uint32_t)strlen(ros_ptr(text));
    s->v = 0;
    s->r[15] = s->r[14];
}

static void state_var(struct wimp_ws *w, int make)
{
    struct ros_cpu c;
    memcpy(w->scratch, "Wimp$State", 11);
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(w->scratch), c.r[1] = w->statevar, c.r[2] = make ? 16 : 0xFFFFFFFFu;
    c.r[3] = 0, c.r[4] = 16;                    /* VarType_Code: needed to delete one too */
    ros_swi(&c, XOS_SetVarVal);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    void *block;
    os_error *e = xos_module_claim(sizeof(struct wimp_ws), &block);
    if (e)
        return e;
    memset(block, 0, sizeof(struct wimp_ws));
    struct wimp_ws *w = block;
    struct ros_cpu c0;
    w->singletask = -1;
    w->poller = -1;
    w->mode = 0xFFFFFFFFu;                      /* currentmode: none until the first task */
    w->dr.threed = 0x48u;                       /* RemoveIconBackgrounds | NoFontBlending */
    memcpy(w->dr.theme, wimp_theme_defaults, sizeof w->dr.theme);
    w->dr.tc[TC_FG] = 0xFFFFFFFFu;
    w->generation = 1;
    ros_st32(m->private_word, ros_addr(block));
    wimp_colourmap_reset();                     /* the default colour mappings */
    wimp_surface_start();                       /* VDisplay tells it of changes */
    ros_task_on_end(wimp_task_thread_ended);    /* tasks whose threads end without polling (#164) */
    ros_cpu_enter(&c0);                         /* save_context: a save area for */
    c0.r[0] = 62, c0.r[2] = 0;                  /* Wimp_ReportError's switch to the screen */
    ros_swi(&c0, XOS_SpriteOp);
    void *sc;
    if (!c0.v && !xos_module_claim(c0.r[3], &sc)) {
        memset(sc, 0, 4);
        w->save_context = ros_addr(sc);
    }
    /* Set WindowManager$Path, for its Templates, Tools and Messages, if
     * no one has set it */
    static const char set[] = "If \"<WindowManager$Path>\" = \"\" Then Set WindowManager$Path "
                              "Resources:$.Resources.Wimp.";
    memcpy(w->scratch, set, sizeof set);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(w->scratch);
    ros_swi(&c, XOS_CLI);
    ros_vector_claim_native(0x1Eu, changeenv, 0);   /* ChangeEnvironmentV (initptrs) */
    void *sv;
    if (!xos_module_claim(40, &sv)) {           /* two LDR PC, [PC, #0], the entries, and the answers */
        uint32_t *e = sv;
        e[0] = e[1] = 0xE59FF000u;
        e[2] = ros_native_entry(state_write, "wimp:Wimp$State");
        e[3] = ros_native_entry(state_read, "wimp:Wimp$State");
        memcpy((char *)sv + 16, "commands", 9);
        memcpy((char *)sv + 28, "desktop", 8);
        w->statevar = ros_addr(sv);
        state_var(w, 1);
    }
    return NULL;
}

/* The services the Wimp acts on.  It claims none. */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (!wimp_ws())
        return;
    struct wimp_ws *w = wimp_ws();
    switch (s->r[1]) {
    case 0x46:                                  /* ModeChange: recalcmodevars */
        wimp_mode_refresh();
        wimp_surface_desktop_changed();
        wimp_sprites_choose();
        wimp_tiles_forget();
        if (w->ntasks)
            wimp_mouse_palette(1);              /* the mode's palette is the kernel's again */
        break;
    case 0x4E: {                                /* MemoryMoved */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 14, c.r[1] = 0, c.r[2] = 0, c.r[3] = 0;
        ros_swi(&c, XOS_ChangeEnvironment);
        if (!c.v && c.r[1] - 0x8000u != w->appspacesize)
            wimp_slot_moved(c.r[1] - 0x8000u);
        break;
    }
    case 0x59:                                  /* ResourceFSStarted: the ROM sprites found again */
        wimp_sprites_choose();
        break;
    case 0x5A:                                  /* ResourceFSDying: lost */
        wimp_sprites_lose();
        break;
    case 0x72:                                  /* SwitchingOutputToSprite */
        w->sprout_current = s->r[4];
        if (s->r[4] == 0)
            wimp_sprout_recache();
        break;
    case 0x82:                                  /* InvalidateCache */
        w->dr.sprite_lastmode = 0xFFFFFFFFu;
        w->tl.regen = 1;
        break;
    case 0x89:                                  /* ModeChanging: the command window */
        wimp_cmdwin_mode_changing();
        break;
    case 0x94:                                  /* ModeFileChanged: outside the desktop */
        if (!w->ntasks)
            wimp_read_mode();
        break;
    case 0x27:                                  /* Reset */
        /* 5.30 discards every window, task and message without a word and
         * initialises its state again.  The kernel issues it at start-up
         * (Kernel s/NewReset), when no task exists.  With tasks it can
         * come only from a program's OS_ServiceCall.  ROSGD's tasks are
         * domains that would outlive it, so the native Wimp acts on it
         * only outside the desktop.  This differs from 5.30 on purpose. */
        if (w->ntasks)
            break;
        wimp_clipboard_reset();                 /* pending again */
        wimp_read_mode();                       /* the configured mode, a *WimpMode forgotten */
        wimp_queue_discard();
        w->err.commandflag = 0;                 /* dormant */
        if (w->tools)                           /* freetoolarea: WindowManager:Tools at the next start */
            xos_module_free(ros_ptr(w->tools));
        w->tools = 0;
        break;
    case 0x2A: {                                /* NewApplication: the handler record starts again */
        w->handlerword = 0;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ROS_ENV_EXIT, c.r[1] = 0, c.r[2] = 0, c.r[3] = 0;
        ros_swi(&c, XOS_ChangeEnvironment);
        w->parentquit = c.v ? 0 : c.r[1];
        break;
    }
    default:                                    /* ResourceFSStarting: ignored on purpose */
        break;
    }
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct wimp_ws *w = wimp_ws();
    if (!w)
        return NULL;
    ros_vector_release_native(0x1Eu, changeenv, 0);
    ros_vdisplay_changed_hook = NULL;
    ros_vdisplay_vsync_hook = NULL;
    if (w->statevar) {
        state_var(w, 0);
        xos_module_free(ros_ptr(w->statevar));
    }
    wimp_queue_discard();
    for (unsigned i = 0; i < WIMP_TASKS; i++)
        if (w->task[i])
            xos_module_free(w->task[i]);
    xos_module_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module wimp_module = {
    .title = "WindowManager",
    .help = "Window Manager\t5.88 (21 Mar 2026) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x400C0,
    .swi_count = WIMP_SWIS,
    .swi_thunks = thunks,
    .swi_names = swi_names,
    .swi_prefix = "Wimp",
    .commands = commands,
    .start = wimp_start,
};
