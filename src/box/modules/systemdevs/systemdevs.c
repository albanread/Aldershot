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
 * This file is a reimplementation in C of RISC OS Open's SystemDevices module
 * (Sources/HWSupport/SystemDevs: s.SystemDevs).
 */

/* systemdevs.c -- SystemDevices, a native module: RISC OS 5.30's module's
 * name and version, and its filing systems (#136).
 *
 * Stock !Run files check for it before they start. OvationPro's does:
 *
 *     RMEnsure SystemDevices  1.13 Error Ovation Pro needs SystemDevices Module
 *
 * which fails with no module of that name. On RISC OS 5.30 it is a ROM
 * module (HWSupport/SystemDevs, 1.34), and with this one in the ROM the
 * line passes, as FPEmulator's do (#114).
 *
 * The title is "SystemDevices". The help string is 5.30's: "System
 * Devices", a tab, and "1.34 (22 Oct 2022)". The version that RMEnsure
 * compares is 1.34. There are no commands or SWIs, as 5.30's has none.
 *
 * Its filing systems are 5.30's (s/SystemDevs). Each is a FileSwitch
 * filing system with no directories, whose name alone is the whole of a
 * file's name ("vdu:"):
 *
 *     null:     output discarded, input at its end at once
 *     vdu:      output to the screen through OS_WriteC, with characters that
 *               would not print shown as GSRead shows them (|M, |! ...),
 *               and CR and LF made one new line; no input
 *     rawvdu:   output to the screen through OS_WriteC as it is; no input
 *     kbd:      input a line at a time from OS_ReadLine, ending in CR, with
 *               Ctrl-D as its end; one stream at a time; no output
 *     rawkbd:   input a character at a time from OS_ReadC, from the
 *               keyboard; no output
 *     source:   input only, by its special field. source#zero: gives
 *               zeros, and source#random: and source#urandom: give bytes
 *               from C's rand() (ISO 9899's), seeded 1. zero$Path,
 *               random$Path and urandom$Path name them, so zero: and the
 *               rest work
 *
 * The screen ones write with output to the VDU alone (OS_Byte 3, as
 * EnableJustVdu). rawkbd: reads with input from the keyboard (OS_Byte 2).
 * printer: is the printer driver's (modules/pdriver/printerfs.c) and not
 * this module's. Using one the wrong way round, such as reading vdu: or
 * writing kbd:, gives 5.30's error &112, "Bad operation on filing system
 * vdu:". */
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fileswitch.h"
#include "systemdevs.h"

#define E_BAD_FS_OP 0x112u              /* BadFSOp: "Bad operation on filing system %0:" */
#define E_ESCAPE    0x11u

enum { DEV_NULL, DEV_VDU, DEV_RAWVDU, DEV_KBD, DEV_RAWKBD, DEV_SOURCE };
static const char *const dev_names[] = { "null", "vdu", "rawvdu", "kbd", "rawkbd", "source" };

static os_error *bad_op(int dev)
{
    return ros_error(E_BAD_FS_OP, "Bad operation on filing system %s:", dev_names[dev]);
}

static uint32_t swi(uint32_t n, uint32_t r0, uint32_t r1, uint32_t r2, struct ros_cpu *out)
{
    ros_cpu_enter(out);
    out->r[0] = r0, out->r[1] = r1, out->r[2] = r2;
    ros_swi(out, n);
    return out->r[0];
}

/* OS_Byte 3 or 2 to v, the old value back */
static uint32_t select_stream(uint32_t byte, uint32_t v)
{
    struct ros_cpu c;
    swi(XOS_Byte, byte, v, 0, &c);
    return c.r[1];
}

/* ---- the streams ---------------------------------------------------------------- */

#define NHANDLES 64
static struct {
    int used, dev, source;              /* source: 1 zero, 2 random, 3 urandom */
} handles[NHANDLES];

static int kbd_open;                    /* kbd: has one stream at a time */
static uint8_t kbd_line[256];
static int kbd_len, kbd_index = -1;     /* -1: no line held */
static char gs_last;                    /* vdu:'s last CR or LF */
static uint32_t rand_seed = 1;

static int dev_of(uint32_t h, int *source)
{
    if (h < 1 || h > NHANDLES || !handles[h - 1].used)
        return -1;
    if (source)
        *source = handles[h - 1].source;
    return handles[h - 1].dev;
}

static os_error *dev_open(int dev, const char *special, int write, uint32_t *h)
{
    int source = 0;
    if (dev == DEV_SOURCE) {
        static const char *const kinds[] = { "zero", "random", "urandom" };
        for (int i = 0; i < 3 && !source; i++)
            if (!strcasecmp(special, kinds[i]))         /* as source_Open, case folded */
                source = i + 1;
        if (!source)
            return bad_op(dev);
    }
    int input = dev == DEV_KBD || dev == DEV_RAWKBD || dev == DEV_SOURCE;
    int output = dev == DEV_VDU || dev == DEV_RAWVDU;
    if ((write && input) || (!write && output))
        return bad_op(dev);
    if (dev == DEV_KBD) {
        if (kbd_open)
            return ros_error(0x10000u | 19u << 8 | 0xC2u, "kbd: is already open");
        kbd_open = 1;
        kbd_index = -1;
    }
    for (int i = 0; i < NHANDLES; i++)
        if (!handles[i].used) {
            handles[i].used = 1, handles[i].dev = dev, handles[i].source = source;
            *h = (uint32_t)i + 1;
            return NULL;
        }
    return ros_error(0x10000u | 0xC0u, "Too many open files");
}

static os_error *dev_close(uint32_t h)
{
    int dev = dev_of(h, NULL);
    if (dev < 0)
        return NULL;
    if (dev == DEV_KBD)
        kbd_open = 0;
    handles[h - 1].used = 0;
    return NULL;
}

/* vdu:'s character: as s/SystemDevs' vdu_wrch with GSFormat 0 (BBC
 * GSRead form): CR and LF a new line each, but the second of a pair */
static os_error *vdu_wrch(uint8_t ch)
{
    struct ros_cpu c;
    if (ch == '\n' || ch == '\r') {
        if (gs_last && gs_last != (char)ch) {
            gs_last = 0;                /* CR LF or LF CR: one new line */
            return NULL;
        }
        gs_last = (char)ch;
        swi(XOS_NewLine, 0, 0, 0, &c);
        return c.v ? ros_ptr(c.r[0]) : NULL;
    }
    gs_last = 0;
    char out[4];
    int n = 0;
    if (ch == '"' || ch == '<' || ch == 12)
        out[n++] = (char)ch;            /* quotes, angle brackets, CLS as they are */
    else if (ch >= ' ' && ch <= '~' && ch != '|')
        out[n++] = (char)ch;
    else {
        if (ch >= 0x80) {
            out[n++] = '|', out[n++] = '!';
            ch &= 0x7F;
        }
        if (ch == 0x7F)
            out[n++] = '|', out[n++] = '?';
        else if (ch == '|')
            out[n++] = '|', out[n++] = '|';
        else if (ch < ' ')
            out[n++] = '|', out[n++] = (char)(ch + '@');
        else
            out[n++] = (char)ch;
    }
    for (int i = 0; i < n; i++) {
        swi(XOS_WriteC, (uint8_t)out[i], 0, 0, &c);
        if (c.v)
            return ros_ptr(c.r[0]);
    }
    return NULL;
}

static os_error *dev_write(uint32_t h, uint32_t pos, const void *buf, uint32_t n)
{
    (void)pos;
    int dev = dev_of(h, NULL);
    if (dev < 0)
        return bad_op(DEV_NULL);
    if (dev == DEV_NULL)
        return NULL;
    if (dev != DEV_VDU && dev != DEV_RAWVDU)
        return bad_op(dev);
    const uint8_t *p = buf;
    uint32_t old = select_stream(3, 0x14);      /* the VDU alone */
    os_error *e = NULL;
    for (uint32_t i = 0; i < n && !e; i++) {
        if (dev == DEV_VDU)
            e = vdu_wrch(p[i]);
        else {
            struct ros_cpu c;
            swi(XOS_WriteC, p[i], 0, 0, &c);
            if (c.v)
                e = ros_ptr(c.r[0]);
        }
    }
    select_stream(3, old);
    return e;
}

/* An Escape read: acknowledged, and the error */
static os_error *escape(void)
{
    struct ros_cpu c;
    swi(XOS_Byte, 126, 0, 0, &c);
    return ros_error(E_ESCAPE, "Escape");
}

static os_error *kbd_byte(uint8_t *b, int *end)
{
    if (kbd_index < 0) {
        /* OS_ReadLine32: the RMA's addresses have bit 30 set, which
         * OS_ReadLine would read as a flag */
        uint8_t *buf = ros_rma_alloc(sizeof kbd_line);
        if (!buf)
            return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(buf), c.r[1] = sizeof kbd_line - 1, c.r[2] = 0, c.r[3] = 0xFF, c.r[4] = 0;
        ros_swi(&c, XOS_ReadLine32);
        if (c.v) {
            ros_rma_free(buf);
            return ros_ptr(c.r[0]);
        }
        if (c.c) {
            ros_rma_free(buf);
            return escape();
        }
        kbd_len = (int)c.r[1];
        memcpy(kbd_line, buf, (size_t)kbd_len);
        ros_rma_free(buf);
        kbd_line[kbd_len++] = '\r';     /* the line's end is part of it */
        kbd_index = 0;
    }
    *b = kbd_line[kbd_index];
    *end = *b == 4;                     /* Ctrl-D: the end, and it stays */
    if (!*end && ++kbd_index == kbd_len)
        kbd_index = -1;
    return NULL;
}

static uint8_t random_byte(void)
{
    rand_seed = rand_seed * 1103515245u + 12345u;       /* ISO 9899:1999 7.20.2.2 */
    return (uint8_t)(rand_seed >> 24);
}

static os_error *dev_read(uint32_t h, uint32_t pos, void *buf, uint32_t n, uint32_t *got)
{
    (void)pos;
    int source, dev = dev_of(h, &source);
    *got = 0;
    if (dev < 0)
        return bad_op(DEV_NULL);
    uint8_t *p = buf;
    switch (dev) {
    case DEV_NULL:
        return NULL;                    /* at the end at once */
    case DEV_SOURCE:
        for (uint32_t i = 0; i < n; i++)
            p[i] = source == 1 ? 0 : random_byte();
        *got = n;
        return NULL;
    case DEV_KBD:
        while (*got < n) {
            int end;
            os_error *e = kbd_byte(&p[*got], &end);
            if (e)
                return e;
            if (end)
                break;
            if (p[(*got)++] == '\r')
                break;                  /* a line at a time */
        }
        return NULL;
    case DEV_RAWKBD:
        if (n) {
            uint32_t old = select_stream(2, 0);       /* the keyboard */
            struct ros_cpu c;
            swi(XOS_ReadC, 0, 0, 0, &c);
            select_stream(2, old);
            if (c.v)
                return ros_ptr(c.r[0]);
            if (c.c)
                return escape();
            p[0] = (uint8_t)c.r[0];
            *got = 1;
        }
        return NULL;
    default:
        return bad_op(dev);
    }
}

/* ---- the filing systems ----------------------------------------------------------- */

/* Every name is the device: a file that is there, read or written as the
 * device allows (so OPENIN finds kbd:, and OPENOUT of vdu: needs no
 * create) */
static int dev_stat_kind(int dev, struct fs_info *info)
{
    memset(info, 0, sizeof *info);
    info->type = OBJ_FILE;
    info->load = 0xFFFFFF00u, info->exec = 0;
    info->attr = (dev == DEV_VDU || dev == DEV_RAWVDU || dev == DEV_NULL ? ATTR_W : 0) |
                 (dev == DEV_KBD || dev == DEV_RAWKBD || dev == DEV_SOURCE || dev == DEV_NULL ? ATTR_R : 0);
    return 0;
}

static const char *boot_disc(void) { return ""; }
static int no_disc(const char *disc) { (void)disc; return 0; }
static os_error *set_extent(uint32_t h, uint32_t e) { (void)h, (void)e; return NULL; }
static int no_entries(const char *disc, const char *path, uint32_t index, struct fs_entry *e, os_error **err)
{
    (void)disc, (void)path, (void)index, (void)e, (void)err;
    return 0;
}

#define DEVICE(id)                                                                                \
    static os_error *stat_##id(const char *disc, const char *path, struct fs_info *info)         \
    {                                                                                             \
        (void)disc, (void)path;                                                                   \
        dev_stat_kind(id, info);                                                                  \
        return NULL;                                                                              \
    }                                                                                             \
    static os_error *open_##id(const char *disc, const char *path, int write, uint32_t *h)       \
    {                                                                                             \
        (void)path;                                                                               \
        return dev_open(id, disc, write, h);                                                      \
    }                                                                                             \
    static uint32_t extent_##id(uint32_t h)                                                       \
    {                                                                                             \
        (void)h;                                                                                  \
        return id == DEV_NULL || id == DEV_VDU || id == DEV_RAWVDU ? 0 : 0xFFFFFFFFu;             \
    }                                                                                             \
    static os_error *refuse_##id(const char *disc, const char *path)                              \
    {                                                                                             \
        (void)disc, (void)path;                                                                   \
        return bad_op(id);                                                                        \
    }                                                                                             \
    static os_error *create_##id(const char *disc, const char *path, uint32_t load, uint32_t exec,\
                                 uint32_t length)                                                 \
    {                                                                                             \
        (void)disc, (void)path, (void)load, (void)exec, (void)length;                             \
        return NULL;                                                                              \
    }                                                                                             \
    static os_error *setinfo_##id(const char *disc, const char *path, int reason, uint32_t load,  \
                                  uint32_t exec, uint32_t attr)                                   \
    {                                                                                             \
        (void)disc, (void)path, (void)reason, (void)load, (void)exec, (void)attr;                 \
        return NULL;                                                                              \
    }                                                                                             \
    static os_error *rename_##id(const char *disc, const char *from, const char *to)              \
    {                                                                                             \
        (void)disc, (void)from, (void)to;                                                         \
        return bad_op(id);                                                                        \
    }

DEVICE(DEV_NULL)
DEVICE(DEV_VDU)
DEVICE(DEV_RAWVDU)
DEVICE(DEV_KBD)
DEVICE(DEV_RAWKBD)
DEVICE(DEV_SOURCE)

/* FileSwitch's delete of a device is "nothing there" (CommonFile) */
static os_error *remove_ok(const char *disc, const char *path)
{
    (void)disc, (void)path;
    return NULL;
}

static os_error *no_free(const char *disc, uint64_t *f, uint64_t *b, uint64_t *s)
{
    (void)disc;
    *f = *b = *s = 0;
    return NULL;
}

#define FS(var, title, num, id)                                                                   \
    const struct fs var = {                                                                       \
        .name = title, .number = num, .has_discs = 0, .read_only = 0, .end_when_none = 1,         \
        .boot_disc = boot_disc, .disc_exists = no_disc, .stat = stat_##id, .open = open_##id,     \
        .read = dev_read, .write = dev_write, .set_extent = set_extent, .extent = extent_##id,     \
        .close = dev_close, .readdir = no_entries, .create = create_##id, .mkdir = refuse_##id,   \
        .remove = remove_ok, .setinfo = setinfo_##id, .rename = rename_##id, .free_space = no_free, \
    }

FS(ros_nullfs, "null", 13u, DEV_NULL);
FS(ros_vdufs, "vdu", 17u, DEV_VDU);
FS(ros_rawvdufs, "rawvdu", 18u, DEV_RAWVDU);
FS(ros_kbdfs, "kbd", 19u, DEV_KBD);
FS(ros_rawkbdfs, "rawkbd", 20u, DEV_RAWKBD);
FS(ros_sourcefs, "source", 195u, DEV_SOURCE);

/* ---- the module ------------------------------------------------------------------- */

/* SystemDevices_Init's variables: Set PrinterType$0 null:, zero$Path and
 * the rest */
static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    static const char *const vars[][2] = {
        { "PrinterType$0", "null:" },
        { "zero$Path", "source#zero:" },
        { "random$Path", "source#random:" },
        { "urandom$Path", "source#urandom:" },
    };
    char *buf = ros_rma_alloc(128);
    if (!buf)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        struct ros_cpu c;
        strcpy(buf, vars[i][0]);
        strcpy(buf + 64, vars[i][1]);
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(buf), c.r[1] = ros_addr(buf + 64), c.r[2] = (uint32_t)strlen(vars[i][1]);
        c.r[3] = 0, c.r[4] = 0;
        ros_swi(&c, XOS_SetVarVal);
    }
    ros_rma_free(buf);
    rand_seed = 1;
    return NULL;
}

struct ros_module systemdevs_module = {
    .title = "SystemDevices",
    .help = "System Devices\t1.34 (22 Oct 2022)",
    .init = init,
};
