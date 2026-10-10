/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* input_evdev.c -- keys and the pointer, from evdev: virtio-input's keyboard
 * and tablet, or anything else the kernel finds.
 *
 * Each device is watched by the runtime's pump (background.h). When one has
 * input, its events are read at the next safe point and translated into RISC
 * OS's terms (platform.h). Keys and mouse buttons become RISC OS's
 * low-level key numbers, by the table below, which does for Linux's key
 * codes what the USB driver's mapping_table does for HID usages
 * (HWSupport/USB/USBDriver/build/c/usbkboard). An absolute axis becomes a
 * fraction of the device's range. The Input module takes it from there.
 *
 * Devices come and go. A PC's USB keyboard appears a second or two after
 * /init starts, and can be unplugged. /dev/input is watched (inotify) for
 * new event devices, and one that goes away is closed. (Under rosgd-vz,
 * the Apple Silicon box's host, VZ's USB keyboard and digitizer appear
 * 0.5-0.8 s after /init starts, and the same watch finds them.)
 *
 * On AArch64 the power button is an input too. rosgd-vz's polite stop (a
 * window closed, SIGTERM, --seconds) presses the power key of a PL061
 * gpio-keys node, which the kernel gives as KEY_POWER from the evdev
 * device named "gpio-keys". Only that device's counts, because VZ's USB
 * keyboard advertises KEY_POWER as well. It is delivered as
 * ROS_INPUT_POWER.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/input.h>

#include "rosgd/background.h"
#include "rosgd/platform.h"

#define MAX_DEVICES 32              /* a laptop has a dozen: lid, buttons, ... */

static struct {
    int fd;                             /* -1: a slot free */
    char node[16];                      /* "eventN" */
    char name[64];
    struct input_absinfo abs[2];        /* X and Y, for a tablet */
} devices[MAX_DEVICES];
static int ndevices;                    /* slots used, free ones included */
static int notify = -1;                 /* /dev/input, watched for new devices */
static void (*deliver)(const struct ros_input_event *);

static void ready(void *arg, uint32_t revents);

/* ---- key numbers (hdr/Keyboard) ------------------------------------------ */

static const uint8_t keys[KEY_MAX + 1] = {
    [KEY_ESC] = 0x00, [KEY_F1] = 0x01, [KEY_F2] = 0x02, [KEY_F3] = 0x03, [KEY_F4] = 0x04,
    [KEY_F5] = 0x05, [KEY_F6] = 0x06, [KEY_F7] = 0x07, [KEY_F8] = 0x08, [KEY_F9] = 0x09,
    [KEY_F10] = 0x0A, [KEY_F11] = 0x0B, [KEY_F12] = 0x0C, [KEY_SYSRQ] = 0x0D,
    [KEY_SCROLLLOCK] = 0x0E, [KEY_PAUSE] = 0x0F,
    [KEY_GRAVE] = 0x10, [KEY_1] = 0x11, [KEY_2] = 0x12, [KEY_3] = 0x13, [KEY_4] = 0x14,
    [KEY_5] = 0x15, [KEY_6] = 0x16, [KEY_7] = 0x17, [KEY_8] = 0x18, [KEY_9] = 0x19,
    [KEY_0] = 0x1A, [KEY_MINUS] = 0x1B, [KEY_EQUAL] = 0x1C, [KEY_BACKSPACE] = 0x1E,
    [KEY_INSERT] = 0x1F, [KEY_HOME] = 0x20, [KEY_PAGEUP] = 0x21, [KEY_NUMLOCK] = 0x22,
    [KEY_KPSLASH] = 0x23, [KEY_KPASTERISK] = 0x24,
    [KEY_TAB] = 0x26, [KEY_Q] = 0x27, [KEY_W] = 0x28, [KEY_E] = 0x29, [KEY_R] = 0x2A,
    [KEY_T] = 0x2B, [KEY_Y] = 0x2C, [KEY_U] = 0x2D, [KEY_I] = 0x2E, [KEY_O] = 0x2F,
    [KEY_P] = 0x30, [KEY_LEFTBRACE] = 0x31, [KEY_RIGHTBRACE] = 0x32, [KEY_BACKSLASH] = 0x33,
    [KEY_DELETE] = 0x34, [KEY_END] = 0x35, [KEY_PAGEDOWN] = 0x36, [KEY_KP7] = 0x37,
    [KEY_KP8] = 0x38, [KEY_KP9] = 0x39, [KEY_KPMINUS] = 0x3A,
    [KEY_LEFTCTRL] = 0x3B, [KEY_A] = 0x3C, [KEY_S] = 0x3D, [KEY_D] = 0x3E, [KEY_F] = 0x3F,
    [KEY_G] = 0x40, [KEY_H] = 0x41, [KEY_J] = 0x42, [KEY_K] = 0x43, [KEY_L] = 0x44,
    [KEY_SEMICOLON] = 0x45, [KEY_APOSTROPHE] = 0x46, [KEY_ENTER] = 0x47, [KEY_KP4] = 0x48,
    [KEY_KP5] = 0x49, [KEY_KP6] = 0x4A, [KEY_KPPLUS] = 0x4B,
    [KEY_LEFTSHIFT] = 0x4C, [KEY_102ND] = 0x4D, [KEY_Z] = 0x4E, [KEY_X] = 0x4F,
    [KEY_C] = 0x50, [KEY_V] = 0x51, [KEY_B] = 0x52, [KEY_N] = 0x53, [KEY_M] = 0x54,
    [KEY_COMMA] = 0x55, [KEY_DOT] = 0x56, [KEY_SLASH] = 0x57, [KEY_RIGHTSHIFT] = 0x58,
    [KEY_UP] = 0x59, [KEY_KP1] = 0x5A, [KEY_KP2] = 0x5B, [KEY_KP3] = 0x5C,
    [KEY_CAPSLOCK] = 0x5D, [KEY_LEFTALT] = 0x5E, [KEY_SPACE] = 0x5F, [KEY_RIGHTALT] = 0x60,
    [KEY_RIGHTCTRL] = 0x61, [KEY_LEFT] = 0x62, [KEY_DOWN] = 0x63, [KEY_RIGHT] = 0x64,
    [KEY_KP0] = 0x65, [KEY_KPDOT] = 0x66, [KEY_KPENTER] = 0x67,
    [KEY_LEFTMETA] = 0x68, [KEY_RIGHTMETA] = 0x69, [KEY_COMPOSE] = 0x6A,
    [KEY_MUHENKAN] = 0x6B, [KEY_HENKAN] = 0x6C, [KEY_KATAKANAHIRAGANA] = 0x6D,
    [KEY_RO] = 0x6E,
    [BTN_LEFT] = 0x70, [BTN_MIDDLE] = 0x71, [BTN_RIGHT] = 0x72, [BTN_SIDE] = 0x73,
    [BTN_EXTRA] = 0x74,
};

int ros_input_key_number(unsigned code)
{
    /* 0 in the table is "none", but KEY_ESC is key number 0. */
    if (code == KEY_ESC)
        return 0;
    return code <= KEY_MAX && keys[code] ? keys[code] : -1;
}

/* ---- devices -------------------------------------------------------------- */

/* Open /dev/input/<name>, if it is an event device: its slot, or -1 */
static int open_device(const char *name)
{
    if (strncmp(name, "event", 5) != 0 || strlen(name) >= sizeof devices[0].node)
        return -1;
    int i;
    for (i = 0; i < ndevices; i++)      /* the scan and inotify can both see one */
        if (devices[i].fd >= 0 && strcmp(devices[i].node, name) == 0)
            return -1;
    for (i = 0; i < ndevices && devices[i].fd >= 0; i++)
        ;
    if (i == MAX_DEVICES)
        return -1;
    char path[300];
    snprintf(path, sizeof path, "/dev/input/%s", name);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    memset(&devices[i], 0, sizeof devices[i]);
    devices[i].fd = fd;
    snprintf(devices[i].node, sizeof devices[i].node, "%s", name);
    if (ioctl(fd, EVIOCGNAME(sizeof devices[i].name), devices[i].name) < 0)
        snprintf(devices[i].name, sizeof devices[i].name, "%s", name);
    ioctl(fd, EVIOCGABS(ABS_X), &devices[i].abs[0]);
    ioctl(fd, EVIOCGABS(ABS_Y), &devices[i].abs[1]);
    if (i == ndevices)
        ndevices++;
    return i;
}

static void close_device(int i)
{
    ros_unwatch(devices[i].fd);
    close(devices[i].fd);
    devices[i].fd = -1;
}

int ros_input_init(void)
{
    /* Watched before it is read, so no device slips between the two; made
     * if no device has made it yet (a PC whose only keyboard is USB) */
    mkdir("/dev/input", 0755);
    notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (notify >= 0 && inotify_add_watch(notify, "/dev/input", IN_CREATE) < 0) {
        close(notify);
        notify = -1;
    }
    DIR *d = opendir("/dev/input");
    if (!d)
        return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)))
        n += open_device(e->d_name) >= 0;
    closedir(d);
    return n;
}

const char *ros_input_device_name(uint32_t device)
{
    return device < (uint32_t)ndevices ? devices[device].name : "";
}

static void emit(enum ros_input_kind kind, uint32_t key, uint32_t axis, int32_t value,
                 uint32_t device)
{
    struct ros_input_event ev = { kind, key, axis, value, device };
    deliver(&ev);
}

/* A device has input: read it all, translate, deliver; watch it again. */
static void ready(void *arg, uint32_t revents)
{
    (void)revents;
    uint32_t i = (uint32_t)(uintptr_t)arg;
    struct input_event ev[64];
    ssize_t n;
    if (devices[i].fd < 0)
        return;
    while ((n = read(devices[i].fd, ev, sizeof ev)) > 0) {
        for (size_t k = 0; k < (size_t)n / sizeof ev[0]; k++) {
            const struct input_event *e = &ev[k];
            int key;
            if (e->type == EV_KEY && (key = ros_input_key_number(e->code)) >= 0) {
                emit(ROS_INPUT_KEY, (uint32_t)key, 0, e->value, i);
            } else if (e->type == EV_REL && (e->code == REL_X || e->code == REL_Y)) {
                emit(ROS_INPUT_MOVE, 0, e->code == REL_X ? ROS_AXIS_X : ROS_AXIS_Y, e->value, i);
            } else if (e->type == EV_REL && (e->code == REL_WHEEL || e->code == REL_HWHEEL)) {
                emit(ROS_INPUT_WHEEL, 0, e->code == REL_HWHEEL ? ROS_AXIS_X : ROS_AXIS_Y,
                     e->value, i);
            } else if (e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_Y)) {
                const struct input_absinfo *a = &devices[i].abs[e->code == ABS_Y];
                int64_t span = (int64_t)a->maximum - a->minimum;
                int32_t f = span > 0 ? (int32_t)(((int64_t)e->value - a->minimum) * 65535 / span)
                                     : 0;
                uint32_t axis = e->code == ABS_X ? ROS_AXIS_X : ROS_AXIS_Y;
                emit(ROS_INPUT_POSITION, 0, axis, ros_display_map_position(axis, f), i);
            } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                emit(ROS_INPUT_SYNC, 0, 0, 0, i);
            }
#if defined(__aarch64__)
            else if (e->type == EV_KEY && e->code == KEY_POWER && e->value == 1 &&
                     !strcmp(devices[i].name, "gpio-keys")) {
                emit(ROS_INPUT_POWER, 0, 0, 0, i);
            }
#endif
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
        close_device((int)i);           /* unplugged (ENODEV) */
        return;
    }
    ros_watch(devices[i].fd, POLLIN, ready, arg);
}

/* /dev/input has new entries: open and watch the event devices among them */
static void arrived(void *arg, uint32_t revents)
{
    (void)arg, (void)revents;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;
    while ((n = read(notify, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            int i;
            if (ev->len && (i = open_device(ev->name)) >= 0 &&
                ros_watch(devices[i].fd, POLLIN, ready, (void *)(uintptr_t)i) < 0)
                close_device(i);
#if defined(__aarch64__)
            /* rosgd-vz's USB keyboard and digitizer come here, late */
            else if (ev->len && i >= 0 && ros_cmdline_has("rosgd.inputlog"))
                ros_console_printf("Input: %s (%s) arrived\n", devices[i].name, devices[i].node);
#endif
            p += sizeof *ev + ev->len;
        }
    ros_watch(notify, POLLIN, arrived, NULL);
}

int ros_input_start(void (*fn)(const struct ros_input_event *ev))
{
    deliver = fn;
    for (int i = 0; i < ndevices; i++)
        if (devices[i].fd >= 0 &&
            ros_watch(devices[i].fd, POLLIN, ready, (void *)(uintptr_t)i) < 0)
            return -ENOSPC;
    if (notify >= 0 && ros_watch(notify, POLLIN, arrived, NULL) < 0)
        return -ENOSPC;
    return 0;
}
