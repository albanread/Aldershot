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
 * This file is a reimplementation in C of RISC OS Open's SpriteUtils module
 * (Sources/Video/Render/SpriteUtil: s.SpriteUtil, s.MsgCode).
 */

/* spriteutils.c: SpriteUtils, rewritten for ROSGD as a native module.
 *
 * This module provides the system sprite area's star commands. They follow
 * the behaviour of the RISC OS 5 module (Video/Render/SpriteUtil 1.13),
 * checked against 5.30 on the farm. It is new code written from that
 * module's documentation (s/SpriteUtil and s/MsgCode). It is not a
 * translation.
 *
 * Each command is one OS_SpriteOp on the system area (a reason code below
 * 256). It is called as a SWI, so it goes through SpriteV to whoever owns
 * it. The sprite code belongs to the VDU drivers (runtime/vdu) and none is
 * here. The command's tail is the sprite name or file name, as the
 * original passes it. *SInfo and *SList print from the module's Messages
 * file.
 *
 * The module's only state is the descriptor of that Messages file. It is
 * kept in the RMA through the private word and is claimed when first
 * needed. Nothing is held in C statics (module.h).
 */
#include <string.h>

#include "spriteutils.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

/* OS_SpriteOp's reasons, on the system area (hdr/Sprite) */
enum {
    SCREEN_SAVE = 2, SCREEN_LOAD = 3, READ_AREA = 8, CLEAR = 9, LOAD = 10, MERGE = 11,
    SAVE = 12, RETURN_NAME = 13, GET = 14, SELECT = 24, DELETE = 25, RENAME = 26, COPY = 27,
    FLIP_X = 33, FLIP_Y = 47,
};

#define GSLOOKUP_SIZE 512u              /* the original's temporary block */

static const char messages_file[] = "Resources:$.Resources.SpriteUtil.Messages";

struct workspace {
    uint32_t desc[4];           /* the Messages file's MessageTrans descriptor */
    char file[44];              /* its name, which MessageTrans keeps */
};

/* A SWI, X form, on registers r[0..9] */
static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static os_error *sprite_op(uint32_t reason, uint32_t r2, uint32_t r3)
{
    uint32_t r[10] = { reason, 0, r2, r3 };
    return swi(OS_SpriteOp, r);
}

/* ---- messages -------------------------------------------------------------- */

/* The workspace, and the Messages file open in it: claimed the first time */
static os_error *messages(struct workspace **out)
{
    uint32_t pw = spriteutils_module.private_word, a = ros_ld32(pw);
    if (a) {
        *out = ros_ptr(a);
        return NULL;
    }
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(w->file, messages_file, sizeof messages_file);
    uint32_t r[10] = { ros_addr(w->desc), ros_addr(w->file), 0 };
    os_error *e = swi(MessageTrans_OpenFile, r);
    if (e) {
        ros_rma_free(w);
        return e;
    }
    ros_st32(pw, ros_addr(w));
    *out = w;
    return NULL;
}

/* Looks up a token's text with MessageTrans_GSLookup, giving %0 to %2 as
 * the parameters p0 to p2, and writes the result out */
static os_error *print(const char *token, const char *p0, const char *p1, const char *p2)
{
    struct workspace *w;
    os_error *e = messages(&w);
    if (e)
        return e;
    char *b = ros_rma_alloc(GSLOOKUP_SIZE + 64);
    if (!b)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    char *t = b + GSLOOKUP_SIZE, *a[3] = { t + 8, t + 24, t + 40 };
    strcpy(t, token);
    const char *given[3] = { p0, p1, p2 };
    uint32_t r[10] = { ros_addr(w->desc), ros_addr(t), ros_addr(b), GSLOOKUP_SIZE };
    for (int i = 0; i < 3; i++) {
        if (given[i]) {
            strcpy(a[i], given[i]);
            r[4 + i] = ros_addr(a[i]);
        }
    }
    e = swi(MessageTrans_GSLookup, r);
    if (!e) {
        uint32_t n[10] = { r[2], r[3] };
        e = swi(OS_WriteN, n);
    }
    ros_rma_free(b);
    return e;
}

/* ---- the commands ----------------------------------------------------------- */

static os_error *cmd_schoose(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(SELECT, tail, 0);
}

static os_error *cmd_sget(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(GET, tail, 0);             /* no palette */
}

static os_error *cmd_sflipx(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(FLIP_X, tail, 0);
}

static os_error *cmd_sflipy(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(FLIP_Y, tail, 0);
}

static os_error *cmd_sload(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(LOAD, tail, 0);
}

static os_error *cmd_smerge(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(MERGE, tail, 0);
}

static os_error *cmd_ssave(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(SAVE, tail, 0);
}

static os_error *cmd_snew(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(CLEAR, tail, 0);
}

static os_error *cmd_screensave(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(SCREEN_SAVE, tail, 1);     /* with the palette */
}

static os_error *cmd_screenload(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(SCREEN_LOAD, tail, 1);
}

/* Skips a name's characters and the spaces after it, to the next parameter */
static uint32_t next_name(uint32_t p)
{
    while (ros_ld8(p) > ' ')
        p++;
    while (ros_ld8(p) == ' ')
        p++;
    return p;
}

static os_error *cmd_srename(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(RENAME, tail, next_name(tail));
}

static os_error *cmd_scopy(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return sprite_op(COPY, tail, next_name(tail));
}

/* Each name in turn, until a control character or an error */
static os_error *cmd_sdelete(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    for (uint32_t p = tail; ros_ld8(p) > ' '; p = next_name(p)) {
        os_error *e = sprite_op(DELETE, p, 0);
        if (e)
            return e;
    }
    return NULL;
}

/* ReadAreaCB: returns 1 and the area's size, sprite count and free offset.
 * Returns 0 if there is no area. */
static int area(uint32_t *size, uint32_t *count, uint32_t *free_at)
{
    uint32_t r[10] = { READ_AREA };
    if (swi(OS_SpriteOp, r))
        return 0;
    *size = r[2], *count = r[3], *free_at = r[5];
    return 1;
}

static void decimal(char *out, uint32_t n)
{
    char d[12];
    int k = 0;
    do
        d[k++] = (char)('0' + n % 10);
    while (n /= 10);
    while (k)
        *out++ = d[--k];
    *out = 0;
}

static os_error *cmd_sinfo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    uint32_t size, count, free_at;
    if (!area(&size, &count, &free_at))
        return print("NoSpMem", NULL, NULL, NULL);
    char kb[12], left[12], n[12];
    decimal(kb, (size + 16) >> 10);
    decimal(left, size - free_at);
    decimal(n, count);
    return print("SInfo", kb, left, n);
}

static os_error *cmd_slist(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    uint32_t size, count, free_at;
    if (!area(&size, &count, &free_at))
        return print("NoSpMem", NULL, NULL, NULL);
    if (!count)
        return print("NoSpDef", NULL, NULL, NULL);
    uint8_t *name = ros_rma_alloc(16);
    if (!name)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    os_error *e = NULL;
    for (uint32_t i = 1; i <= count && !e; i++) {
        uint32_t r[10] = { RETURN_NAME, 0, ros_addr(name), 16, i };
        if (!(e = swi(OS_SpriteOp, r))) {
            uint32_t w[10] = { ros_addr(name) };
            if (!(e = swi(OS_Write0, w)))
                e = xos_new_line();
        }
    }
    ros_rma_free(name);
    return e;
}

/* ---- the module ---------------------------------------------------------- */

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    uint32_t a = ros_ld32(m->private_word);
    if (a) {
        struct workspace *w = ros_ptr(a);
        uint32_t r[10] = { ros_addr(w->desc) };
        swi(MessageTrans_CloseFile, r);
        ros_rma_free(w);
        ros_st32(m->private_word, 0);
    }
    return NULL;
}

/* Help and syntax texts, from the Messages file. \x1B\x00 stands for the
 * command's name. */
#define C(name, min, max, syntax, help, fn) \
    { name, ROS_CMD_INFO(min, max, 0, 0), "Syntax: *\x1B\x00" syntax, "*\x1B\x00 " help, fn }

static const struct ros_command commands[] = {
    C("SChoose", 1, 1, " <name>", "selects a sprite.", cmd_schoose),
    C("SGet", 1, 1, " <name>", "picks up an area of the screen as a sprite.", cmd_sget),
    C("SFlipX", 1, 1, " <name>", "reflects the sprite about the X axis.", cmd_sflipx),
    C("SFlipY", 1, 1, " <name>", "reflects the sprite about the Y axis.", cmd_sflipy),
    C("SDelete", 1, 255, " <name> [<name>]", "deletes sprites.", cmd_sdelete),
    C("SList", 0, 0, "", "lists all sprites.", cmd_slist),
    C("SLoad", 1, 1, " <filename>", "loads a sprite file into memory.", cmd_sload),
    C("SMerge", 1, 1, " <filename>", "appends a sprite file to those in memory.", cmd_smerge),
    C("SNew", 0, 0, "", "clears all sprite definitions.", cmd_snew),
    C("SSave", 1, 1, " <filename>", "saves the sprite memory.", cmd_ssave),
    C("SInfo", 0, 0, "", "prints the size of the sprite memory.", cmd_sinfo),
    C("SRename", 2, 2, " <old name> <new name>", "renames a sprite.", cmd_srename),
    C("SCopy", 2, 2, " <name> <new name>", "makes a copy of a sprite.", cmd_scopy),
    C("ScreenSave", 1, 1, " <filename>", "saves the graphics window.", cmd_screensave),
    C("ScreenLoad", 1, 1, " <filename>", "loads into the graphics window.", cmd_screenload),
    { 0 },
};

struct ros_module spriteutils_module = {
    .title = "SpriteUtils",
    .help = "SpriteUtils\t1.13 (26 Sep 2026) ROSGD native",
    .final = final,
    .commands = commands,
};
