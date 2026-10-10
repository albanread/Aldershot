/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* input.c -- Input, the keyboard and pointer driver for ROSGD, as a native
 * module.
 *
 * On a Pi, keys and the mouse reach RISC OS through the USB driver's HID
 * half (HWSupport/USB/USBDriver/build/c/usbkboard, usbmouse), which turns
 * HID reports into calls on two vectors that the kernel's keyboard and
 * pointer code listen to. Under ROSGD the reports come from Linux's evdev,
 * already in RISC OS's terms (platform/input_evdev.c), and this module makes
 * the same calls the USB driver makes:
 *
 *   KeyV 0, KeyboardPresent: once, as a PC keyboard with no debounce;
 *   KeyV 2 / 1, KeyDown / KeyUp: every key and mouse button, by its
 *     low-level key number. Linux's autorepeat is dropped, as RISC OS
 *     repeats keys itself;
 *   PointerV 3, Report: movement (dx, dy, R4 0), or, from a tablet, the
 *     position in OS units with Y upwards and R4 "Abso";
 *   PointerV 9, WheelChange: the scroll wheels.
 *
 * A Mac keyboard has no Break (a PC's Pause), so Ctrl-Cmd-Delete stands in
 * for it. Delete (Backspace or forward Delete) pressed with Ctrl and Cmd
 * down goes as Break down, and its release as Break up. Ctrl being down
 * makes it Ctrl-Break, a reset under *FX 247's default, as on 5.30.
 * A Mac's mouse has one button, or two. The left button pressed with
 * Ctrl down goes as Menu, and with Cmd down as Adjust, and its release goes
 * as the same button. These are ROSGD's decisions, not the USB driver's.
 *
 * On the Apple Silicon box the machine's power button (rosgd-vz's polite
 * stop: ROS_INPUT_POWER) turns the box off as the Task Manager's Shutdown
 * does when the platform can: OS_Reset with R0 "&OFF", which sends
 * Service_PreReset to every module and then powers off.
 *
 * As the pointer device RISC OS selects, the module also answers PointerV.
 * Request returns the movement since the last, Identify returns its record,
 * and Selected turns its reports on or off. It is new code, written from
 * the calls that the USB driver makes (README.md), not a translation. Its
 * state is in the RMA.
 */
#include <string.h>

#include "drmvideo.h"
#include "input.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"

#define KEYV     0x13u
#define POINTERV 0x26u

enum { KEYV_KEYBOARD_PRESENT = 0, KEYV_KEY_UP = 1, KEYV_KEY_DOWN = 2 };
enum { POINTER_REQUEST = 0, POINTER_IDENTIFY = 1, POINTER_SELECTED = 2, POINTER_REPORT = 3,
       POINTER_WHEEL = 9 };
#define KEYBOARD_ID_PC    2u
#define MAGIC_NO_DEBOUNCE 0x4E6F4B64u       /* the USB driver's, "dKoN" */
#define ABSO              0x6F736241u       /* "Abso": R4 for an absolute report */

/* The pointer device type this driver answers as.  RISC OS selects one
 * device type (OS_Pointer, the MouseType CMOS byte); the USB driver's is
 * 7, and it is what a Pi's configuration already names. */
#define POINTER_TYPE 7u

struct workspace {
    int32_t relx, rely;             /* movement since the last Request */
    int32_t dx, dy;                 /* movement in the report being read */
    int32_t wheel_x, wheel_y;
    uint32_t abs_x, abs_y;          /* the tablet's position, 0-65535 */
    uint32_t abs_seen;              /* the report being read has one */
    uint32_t enabled;               /* reports on: the selected device */
    uint32_t announced;
    uint32_t record;                /* our Identify record, in the RMA */
    uint32_t log;                   /* rosgd.inputlog: report each call */
    uint32_t held;                  /* Ctrl and Cmd keys down: bits 0-1 Ctrl, 2-3 Cmd */
    uint32_t broken;                /* the Delete key standing in for Break, while down */
    uint32_t clicked;               /* the button a Ctrl- or Cmd-click went as, while down */
};

struct ros_module input_module;
static int devices;             /* found at init: the platform's, not state */

int input_device_count(void)
{
    return devices;
}

static struct workspace *ws(void)
{
    uint32_t w = input_module.private_word ? ros_ld32(input_module.private_word) : 0;
    return w ? ros_ptr(w) : NULL;
}

static void call_vector(uint32_t vector, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                        uint32_t r4)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = r0, s.r[1] = r1, s.r[2] = r2, s.r[3] = r3, s.r[4] = r4;
    ros_vector_call(vector, &s);
    if (ws()->log)
        ros_console_printf("Input: %s %u, R1 &%X R2 %d R3 %d%s\n",
                           vector == KEYV ? "KeyV" : "PointerV", r0, r1, (int32_t)r2,
                           (int32_t)r3, r4 == ABSO ? " Abso" : "");
}

/* A variable of the current mode (OS_ReadModeVariable), or dflt */
static uint32_t mode_variable(uint32_t var, uint32_t dflt)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0xFFFFFFFFu, c.r[1] = var;
    ros_swi(&c, XOS_ReadModeVariable);
    return c.v || c.c ? dflt : c.r[2];
}

/* The screen in OS units, as the USB driver asks the VDU drivers: the
 * current mode's pixels shifted by its eig factors, so MODE 12's 640 x 256
 * is 1280 x 1024. Without a display it is DRMVideo's mode, or 1280 x 1024. */
static void screen_os_units(uint32_t *w, uint32_t *h)
{
    const struct ros_display *d = drmvideo_display();
    uint32_t xpix = d ? d->width : 640, ypix = d ? d->height : 512;
    *w = (mode_variable(11, xpix - 1) + 1) << (mode_variable(4, 1) & 31);    /* XWindLimit, XEigFactor */
    *h = (mode_variable(12, ypix - 1) + 1) << (mode_variable(5, 1) & 31);    /* YWindLimit, YEigFactor */
}

/* ---- from the platform ------------------------------------------------------ */

/* A Mac's keyboard and mouse (the top of the file): Ctrl-Cmd-Delete as
 * Break; Ctrl-click as Menu, Cmd-click as Adjust */
#define KEY_BREAK     0x0Fu
#define KEY_BACKSPACE 0x1Eu
#define KEY_DELETE    0x34u
#define KEY_SELECT    0x70u
#define KEY_MENU      0x71u
#define KEY_ADJUST    0x72u
static uint32_t mac_key(struct workspace *w, uint32_t key, int32_t down)
{
    static const uint32_t mods[4] = { 0x3B, 0x61, 0x68, 0x69 };   /* Ctrl l/r, Cmd l/r */
    for (unsigned i = 0; i < 4; i++)
        if (key == mods[i])
            w->held = down ? w->held | 1u << i : w->held & ~(1u << i);
    int ctrl = (w->held & 3) != 0, cmd = (w->held & 12) != 0;
    if (down && (key == KEY_BACKSPACE || key == KEY_DELETE) && ctrl && cmd) {
        w->broken = key;
        return KEY_BREAK;
    }
    if (!down && w->broken && key == w->broken) {
        w->broken = 0;
        return KEY_BREAK;
    }
    if (key == KEY_SELECT) {
        if (down && ctrl != cmd)
            w->clicked = ctrl ? KEY_MENU : KEY_ADJUST;
        uint32_t as = w->clicked ? w->clicked : key;
        if (!down)
            w->clicked = 0;
        return as;
    }
    return key;
}

void input_deliver(const struct ros_input_event *ev)
{
    struct workspace *w = ws();
    if (!w)
        return;
    switch (ev->kind) {
    case ROS_INPUT_KEY:
        if (ev->value == 2)
            break;                      /* autorepeat: RISC OS makes its own */
        call_vector(KEYV, ev->value ? KEYV_KEY_DOWN : KEYV_KEY_UP, mac_key(w, ev->key, ev->value),
                    0, 0, 0);
        break;
    case ROS_INPUT_MOVE:
        if (ev->axis == ROS_AXIS_X)
            w->dx += ev->value;
        else
            w->dy -= ev->value;         /* RISC OS's Y counts upwards */
        break;
    case ROS_INPUT_POSITION:
        if (ev->axis == ROS_AXIS_X)
            w->abs_x = (uint32_t)ev->value;
        else
            w->abs_y = (uint32_t)ev->value;
        w->abs_seen = 1;
        break;
    case ROS_INPUT_WHEEL:
        if (ev->axis == ROS_AXIS_X)
            w->wheel_x += ev->value;
        else
            w->wheel_y += ev->value;
        break;
    case ROS_INPUT_SYNC:
        /* One report: movement, a position, the wheels, as usbmouse does. */
        if (w->dx || w->dy) {
            w->relx += w->dx;
            w->rely += w->dy;
            if (w->enabled)
                call_vector(POINTERV, POINTER_REPORT, POINTER_TYPE, (uint32_t)w->dx,
                            (uint32_t)w->dy, 0);
            w->dx = w->dy = 0;
        }
        if (w->abs_seen) {
            uint32_t sw, sh;
            screen_os_units(&sw, &sh);
            uint32_t x = (uint32_t)((uint64_t)w->abs_x * sw / 65536);
            uint32_t y = sh - 1 - (uint32_t)((uint64_t)w->abs_y * sh / 65536);
            if (w->enabled)
                call_vector(POINTERV, POINTER_REPORT, POINTER_TYPE, x, y, ABSO);
            w->abs_seen = 0;
        }
        if (w->wheel_x || w->wheel_y) {
            /* R1 Y, negative away; R2 extra buttons; R3 X */
            if (w->enabled)
                call_vector(POINTERV, POINTER_WHEEL, (uint32_t)-w->wheel_y, 0,
                            (uint32_t)w->wheel_x, 0);
            w->wheel_x = w->wheel_y = 0;
        }
        break;
#if defined(__aarch64__)
    case ROS_INPUT_POWER: {
        struct ros_cpu s;
        ros_console_printf("Input: the power button: the box powers off\n");
        ros_cpu_enter(&s);
        s.r[0] = 0x46464F26u;           /* "&OFF" */
        ros_swi(&s, XOS_Reset);         /* (does not return) */
        break;
    }
#endif
    }
}

/* ---- PointerV, as a pointer device ------------------------------------------ */

static int pointerv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct workspace *w = ws();
    switch (s->r[0]) {
    case POINTER_REQUEST:
        if (s->r[1] != POINTER_TYPE)
            return ROS_VECTOR_PASS;
        s->r[2] = (uint32_t)w->relx;
        s->r[3] = (uint32_t)w->rely;
        w->relx = w->rely = 0;
        return ROS_VECTOR_CLAIM;        /* the PRM says a Request is intercepted */
    case POINTER_IDENTIFY: {
        /* Our record at the head of the list: next, flags, type, name. */
        uint32_t rec = w->record;
        ros_st32(rec, s->r[1]);
        s->r[1] = rec;
        return ROS_VECTOR_PASS;
    }
    case POINTER_SELECTED:
        w->enabled = s->r[1] == POINTER_TYPE;
        if (w->enabled)
            w->relx = w->rely = 0;      /* so the pointer does not jump */
        return ROS_VECTOR_PASS;
    default:
        return ROS_VECTOR_PASS;
    }
}

/* ---- the module ---------------------------------------------------------------- */

static void announce(void *arg)
{
    (void)arg;
    struct workspace *w = ws();
    if (w && !w->announced) {
        w->announced = 1;
        call_vector(KEYV, KEYV_KEYBOARD_PRESENT, KEYBOARD_ID_PC, MAGIC_NO_DEBOUNCE, 0, 0);
    }
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    uint8_t *rec = ros_rma_alloc(4 + 4 + 1 + 32);
    if (!w || !rec)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    memset(rec, 0, 4 + 4 + 1 + 32);
    rec[8] = POINTER_TYPE;
    strcpy((char *)rec + 9, "ROSGD pointer (Linux evdev)");
    w->record = ros_addr(rec);
    /* No kernel pointer code yet to send Selected: reporting starts on. */
    w->enabled = 1;
    w->log = ros_cmdline_has("rosgd.inputlog");
    ros_st32(m->private_word, ros_addr(w));
    os_error *e = ros_vector_claim_native(POINTERV, pointerv, 0);
    if (e)
        return e;
    ros_callback_add_native(announce, NULL);    /* when every module has started */
    devices = ros_input_init();
    ros_input_start(input_deliver);     /* devices that come later too */
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    ros_vector_release_native(POINTERV, pointerv, 0);
    if (w) {
        ros_rma_free(ros_ptr(w->record));
        ros_rma_free(w);
    }
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module input_module = {
    .title = "Input",
    .help = "Input\t\t0.01 (24 Sep 2026) ROSGD native, over Linux evdev",
    .init = init,
    .final = final,
};
