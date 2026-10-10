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
 * This file is a reimplementation in C of RISC OS Open's kernel source
 * (Sources/Kernel: s.SysComms).
 */

/* filecmds.c: the kernel's own file commands, reimplemented.
 *
 * Written from the kernel's Kernel/s/SysComms.  The commands are *Close,
 * *Create, *Delete, *Dump, *List, *Load, *Opt, *Print, *Remove, *Save and
 * *Type.  They belong to the UtilityModule (runtime/oscli.c keeps the
 * table).  They do their work through the file SWIs, so FileSwitch answers
 * them.
 *
 * What they keep:
 *   - Addresses are in hex, read as the kernel reads them (OS_ReadUnsigned,
 *     base 16).  *Save takes "<start> <end>" or "<start> +<length>", then
 *     an optional exec and load address.  *Load loads at the file's own
 *     address when none is given.
 *   - *Type and *List print in GSREAD format: |@ for control characters,
 *     |? for delete, |! before top-bit characters, and || and |".  They
 *     treat CR, LF, CR LF and LF CR each as one line end.  *List numbers
 *     the lines, right-justified in four columns.  *Type -TabExpand
 *     expands tabs to eight spaces.  *Print sends the bytes as they are.
 *   - *Dump shows 16 bytes a line in an 80-column window, with the title
 *     every 16 lines.  The display address is the file's load address when
 *     it is not typed.  An offset and a start address are optional.
 *   - *Delete complains about a missing file. *Remove does not.
 *
 *   - *Spool, *SpoolOn and *Exec read the old handle through OS_Byte 199
 *     or 198, which also clears it, and close it. Then they open the new
 *     file and set its handle. The open is OpenOut, OpenUp at its end, or
 *     OpenIn. With no name, they only close.
 *   - *Build and *Append read lines with OS_ReadLine32. Each line is
 *     numbered as *List numbers them, GSTransed and written with a CR,
 *     until Escape ends it. *Build makes the file anew. *Append adds to
 *     its end.
 *
 * The configured GS format (CMOS) is the default one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/platform.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/task.h"

#define ERR_OUTSIDE_FILE 0xB7u

#define WINDOW_WIDTH 80u

/* A SWI for a command.  It is made as the kind of caller of the OS_CLI that
 * is running the command, so an ARM caller's *Obey runs its lines as ARM. */
static os_error *swi(uint32_t n, struct ros_cpu *c)
{
    ros_swi_as(c, n, ros_caller_kind());
    return c->v ? ros_ptr(c->r[0]) : NULL;
}

static os_error *write_c(uint32_t ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ch;
    return swi(XOS_WriteC, &c);
}

static os_error *write_s(const char *s)
{
    os_error *e = NULL;
    for (; *s && !e; s++)
        e = write_c((uint8_t)*s);
    return e;
}

static os_error *new_line(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    return swi(XOS_NewLine, &c);
}

static os_error *bad_address(void)
{
    return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
}

/* The word after the one at p */
static uint32_t skip_word(uint32_t p)
{
    while (ros_ld8(p) > ' ')
        p++;
    while (ros_ld8(p) == ' ')
        p++;
    return p;
}

static uint32_t skip_spaces(uint32_t p)
{
    while (ros_ld8(p) == ' ')
        p++;
    return p;
}

/* A hex number at *p, as ReadAtMost8Hex reads one.  *p moves past it. */
static os_error *read_hex(uint32_t *p, uint32_t *v)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 16, c.r[1] = *p;
    os_error *e = swi(XOS_ReadUnsigned, &c);
    if (e)
        return e;
    *p = c.r[1];
    *v = c.r[2];
    return NULL;
}

/* Up to two more numbers: the first into *a, the second into *b (ReadOptionalLoadAndExec) */
static os_error *read_optional(uint32_t *p, uint32_t *a, uint32_t *b)
{
    *p = skip_spaces(*p);
    if (ros_ld8(*p) < ' ')
        return NULL;
    os_error *e = read_hex(p, a);
    if (e)
        return e;
    *p = skip_spaces(*p);
    if (ros_ld8(*p) < ' ')
        return NULL;
    if ((e = read_hex(p, b)) != NULL)
        return e;
    *p = skip_spaces(*p);
    return ros_ld8(*p) >= ' ' ? bad_address() : NULL;
}

static os_error *os_file(struct ros_cpu *c)
{
    return swi(XOS_File, c);
}

/* ---- *Close, *Delete, *Remove, *Load, *Save, *Create, *Opt ------------------------ */

os_error *ros_cmd_close(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0;
    return swi(XOS_Find, &c);
}

static os_error *delete(uint32_t tail, int winge)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 6, c.r[1] = tail;
    os_error *e = os_file(&c);
    if (e || !winge || c.r[0] != 0)
        return e;
    ros_cpu_enter(&c);
    c.r[0] = 19, c.r[1] = tail, c.r[2] = 0;
    return os_file(&c);
}

os_error *ros_cmd_delete(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return delete(tail, 1);
}

os_error *ros_cmd_remove(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return delete(tail, 0);
}

os_error *ros_cmd_load(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 255, c.r[1] = tail, c.r[3] = 0xFF;
    if (argc > 1) {
        uint32_t p = skip_word(tail), addr;
        os_error *e = read_hex(&p, &addr);
        if (e)
            return e;
        if (ros_ld8(skip_spaces(p)) >= ' ')
            return bad_address();
        c.r[2] = addr, c.r[3] = 0;
    }
    return os_file(&c);
}

/* *Save <file> <start> <end>|+<length> [<exec> [<load>]] */
os_error *ros_cmd_save(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t p = skip_word(tail), start, end;
    os_error *e = read_hex(&p, &start);
    if (e)
        return e;
    p = skip_spaces(p);
    int plus = ros_ld8(p) == '+';
    if (plus)
        p = skip_spaces(p + 1);
    if ((e = read_hex(&p, &end)) != NULL)
        return e;
    if (plus)
        end += start;
    uint32_t exec = start, load = start;
    if ((e = read_optional(&p, &exec, &load)) != NULL)
        return e;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = tail, c.r[2] = load, c.r[3] = exec, c.r[4] = start, c.r[5] = end;
    return os_file(&c);
}

/* *Create <file> [<length> [<exec> [<load>]]].  Without addresses, it makes
 * a Data file stamped now. */
os_error *ros_cmd_create(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    uint32_t length = 0, exec = 0, load = 0;
    uint32_t p = skip_word(tail);
    os_error *e;
    if (argc > 1 && (e = read_hex(&p, &length)) != NULL)
        return e;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = tail, c.r[4] = 0, c.r[5] = length;
    if (argc < 3) {
        c.r[0] = 11, c.r[2] = 0xFFD;
    } else {
        if ((e = read_optional(&p, &exec, &load)) != NULL)
            return e;
        c.r[0] = 7, c.r[2] = load, c.r[3] = exec;
    }
    return os_file(&c);
}

/* *Opt [<n> [[,] <m>]]: OS_FSControl 10 */
os_error *ros_cmd_opt(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    uint32_t a = 0, b = 0;
    if (argc) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 10, c.r[1] = tail;
        os_error *e = swi(XOS_ReadUnsigned, &c);
        if (e)
            return e;
        a = c.r[2];
        uint32_t p = skip_spaces(c.r[1]);
        if (ros_ld8(p) == ',')
            p = skip_spaces(p + 1);
        if (ros_ld8(p) > ' ') {
            ros_cpu_enter(&c);
            c.r[0] = 10 | 1u << 31, c.r[1] = p;
            if ((e = swi(XOS_ReadUnsigned, &c)) != NULL)
                return e;
            b = c.r[2];
        }
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = a, c.r[2] = b;
    return swi(XOS_FSControl, &c);
}

/* ---- *Type, *List, *Print ------------------------------------------------------- */

static os_error *open_in(uint32_t name, uint32_t *h)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x4C, c.r[1] = name;       /* OpenIn, must exist, not a directory */
    os_error *e = swi(XOS_Find, &c);
    *h = c.r[0];
    return e;
}

static void close_h(uint32_t h)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = h;
    swi(XOS_Find, &c);
}

static os_error *bput_(uint32_t h, uint32_t ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ch, c.r[1] = h;
    return swi(XOS_BPut, &c);
}

/* A byte from h: 0 and *eof at the end */
static os_error *bget(uint32_t h, uint32_t *ch, int *eof)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = h;
    os_error *e = swi(XOS_BGet, &c);
    *ch = c.r[0] & 0xFF;
    *eof = !e && c.c;
    return e;
}

/* One character in GSREAD format (the kernel's PrintCharInGSFormat, format 0) */
static os_error *gs_char(uint32_t ch)
{
    os_error *e = NULL;
    if (ch >= ' ' && ch <= 0x7E && ch != '|' && ch != '"' && ch != '<')
        return write_c(ch);
    if (ch >= 0x80) {
        if ((e = write_s("|!")) != NULL)
            return e;
        ch &= 0x7F;
    }
    if (ch == 0x7F)
        return write_s("|?");
    if (ch == '"' || ch == '|')
        return (e = write_c('|')) ? e : write_c(ch);
    if (ch <= 0x1F)
        return (e = write_c('|')) ? e : write_c(ch + '@');
    return write_c(ch);
}

enum { RAW, GS, NUMBERED };

static os_error *print_file(uint32_t name, int mode, int tabs)
{
    uint32_t h, ch, last = 0, line = 0;
    int eof;
    os_error *e = open_in(name, &h);
    if (e)
        return e;
    for (;;) {
        if ((e = bget(h, &ch, &eof)) != NULL || eof)
            break;
        if (mode == NUMBERED) {
            char n[16];
            snprintf(n, sizeof n, "%4u ", ++line);
            if ((e = write_s(n)) != NULL)
                break;
        }
        for (;;) {
            if (mode == RAW) {
                e = write_c(ch);
            } else if (ch == 13 || ch == 10) {
                if (ch == last) {
                    e = new_line();         /* LF LF: a blank line */
                    if (!e)
                        goto next_line;
                } else if (last == 13 || last == 10) {
                    last = 0;               /* CR LF or LF CR: one end */
                } else {
                    last = ch;
                    e = new_line();
                    if (!e)
                        goto next_line;
                }
            } else if (ch == 9 && tabs) {
                e = write_s("        ");
            } else {
                last = ch;
                e = (ch == '"' || ch == '<') ? write_c(ch) : gs_char(ch);
            }
            if (e)
                break;
            if ((e = bget(h, &ch, &eof)) != NULL || eof)
                break;
        }
        if (!e && mode != RAW)
            e = new_line();
        break;
    next_line:
        continue;
    }
    close_h(h);
    return e;
}

os_error *ros_cmd_print(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return print_file(tail, RAW, 0);
}

os_error *ros_cmd_list(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    int tabs = 0;
    uint32_t name = tail;
    if (argc > 1) {
        /* "-File fred -TabExpand", in either order */
        for (uint32_t p = tail; ros_ld8(p) >= ' '; p = skip_word(p)) {
            char w[16];
            unsigned n = 0;
            while (ros_ld8(p + n) > ' ' && n < sizeof w - 1)
                w[n] = (char)ros_ld8(p + n), n++;
            w[n] = 0;
            if (!strcasecmp(w, "-TabExpand"))
                tabs = 1;
            else if (!strcasecmp(w, "-File"))
                name = skip_word(p);
            else if (w[0] != '-' && name == tail)
                name = p;
        }
    }
    return print_file(name, NUMBERED, tabs);
}

os_error *ros_cmd_type(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    int tabs = 0;
    uint32_t name = tail;
    if (argc > 1) {
        uint32_t file = 0;
        for (uint32_t p = tail; ros_ld8(p) >= ' '; p = skip_word(p)) {
            char w[16];
            unsigned n = 0;
            while (ros_ld8(p + n) > ' ' && n < sizeof w - 1)
                w[n] = (char)ros_ld8(p + n), n++;
            w[n] = 0;
            if (!strcasecmp(w, "-TabExpand"))
                tabs = 1;
            else if (!strcasecmp(w, "-File"))
                file = skip_word(p), p = file;
            else if (!file)
                file = p;
        }
        if (file)
            name = file;
    }
    return print_file(name, GS, tabs);
}

/* ---- *Spool, *SpoolOn, *Exec -------------------------------------------------------- */

static os_error *open_winge(uint32_t mode, uint32_t name, uint32_t *h)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = mode | 0x08 | 0x04, c.r[1] = name;
    os_error *e = swi(XOS_Find, &c);
    *h = c.r[0];
    return e;
}

static os_error *to_end(uint32_t h)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 2, c.r[1] = h;
    os_error *e = swi(XOS_Args, &c);
    if (e)
        return e;
    uint32_t ext = c.r[2];
    ros_cpu_enter(&c);
    c.r[0] = 1, c.r[1] = h, c.r[2] = ext;
    return swi(XOS_Args, &c);
}

/* The kernel's shared body: byte 198 or 199, open mode */
static os_error *exec_spool(uint32_t byte, uint32_t mode, uint32_t tail, uint32_t argc)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = byte, c.r[1] = 0, c.r[2] = 0;
    swi(XOS_Byte, &c);                  /* the old handle, and 0 in its place */
    if (c.r[1])
        close_h(c.r[1]);
    if (!argc)
        return NULL;
    uint32_t h;
    os_error *e = open_winge(mode, tail, &h);
    if (e)
        return e;
    if (mode == 0xC0 && (e = to_end(h)) != NULL) {
        close_h(h);
        return e;
    }
    ros_cpu_enter(&c);
    c.r[0] = byte, c.r[1] = h, c.r[2] = 0;
    return swi(XOS_Byte, &c);
}

os_error *ros_cmd_exec(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    return exec_spool(198, 0x40, tail, argc);
}

os_error *ros_cmd_spool(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    return exec_spool(199, 0x80, tail, argc);
}

os_error *ros_cmd_spoolon(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    return exec_spool(199, 0xC0, tail, argc);
}

/* ---- *Build, *Append ---------------------------------------------------------- */

static os_error *build(uint32_t mode, uint32_t name)
{
    uint32_t h;
    os_error *e = open_winge(mode, name, &h);
    if (e)
        return e;
    if ((e = to_end(h)) != NULL) {
        close_h(h);
        return e;
    }
    uint32_t buf = ros_addr(ros_rma_alloc(256));
    if (!buf) {
        close_h(h);
        return ros_error(0x182, "Not enough memory");
    }
    for (uint32_t line = 1;; line++) {
        char n[16];
        snprintf(n, sizeof n, "%4u ", line);
        if ((e = write_s(n)) != NULL)
            break;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = buf, c.r[1] = 255, c.r[2] = ' ', c.r[3] = 0xFF, c.r[4] = 0;
        if ((e = swi(XOS_ReadLine32, &c)) != NULL)
            break;
        int escape = (int)c.c;
        ros_st8(buf + c.r[1], 13);
        if (escape) {
            ros_cpu_enter(&c);
            c.r[0] = 126;               /* acknowledge it */
            swi(XOS_Byte, &c);
        }
        /* Leading spaces as they are, then the rest GSTransed */
        uint32_t p = buf;
        while (!e && ros_ld8(p) == ' ')
            e = bput_(h, ros_ld8(p++));
        struct ros_cpu g;
        ros_cpu_enter(&g);
        g.r[0] = p, g.r[2] = 1u << 31;
        if (!e)
            e = swi(XOS_GSInit, &g);
        while (!e) {
            e = swi(XOS_GSRead, &g);
            if (e || g.c)
                break;
            e = bput_(h, g.r[1] & 0xFF);
        }
        if (e)
            break;
        if (escape) {
            e = new_line();
            break;
        }
        if ((e = bput_(h, 13)) != NULL)
            break;
    }
    ros_rma_free(ros_ptr(buf));
    close_h(h);
    return e;
}

os_error *ros_cmd_build(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return build(0x80, tail);
}

os_error *ros_cmd_append(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return build(0xC0, tail);
}

/* ---- *Dump ---------------------------------------------------------------------- */

static os_error *hex(uint32_t v, int digits)
{
    char b[12];
    snprintf(b, sizeof b, "%0*X", digits, v);
    return write_s(b);
}

/* *Dump <file> [<offset> [<start address>]] */
os_error *ros_cmd_dump(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    uint32_t h;
    os_error *e = open_in(tail, &h);
    if (e)
        return e;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 20, c.r[1] = tail;
    if ((e = os_file(&c)) != NULL) {
        close_h(h);
        return e;
    }
    uint32_t extent = c.r[0] ? c.r[4] : 0xFFFFFFFFu;
    int untyped = c.r[0] && (c.r[2] >> 20) != 0xFFF;
    uint32_t base = untyped ? c.r[2] : 0, offset = 0;
    if (argc > 1) {
        uint32_t p = skip_word(tail);
        if ((e = read_optional(&p, &offset, &base)) != NULL) {
            close_h(h);
            return e;
        }
    }
    uint32_t display = base + offset;
    if (offset > extent) {
        close_h(h);
        return ros_error(ERR_OUTSIDE_FILE, "Outside file");
    }
    ros_cpu_enter(&c);
    c.r[0] = 1, c.r[1] = h, c.r[2] = offset;
    if ((e = swi(XOS_Args, &c)) != NULL) {
        close_h(h);
        return e;
    }
    /* Bytes a line.  This is the window's width less the address and
     * separators, at four columns a byte, rounded down to its top two bits. */
    uint32_t per = (WINDOW_WIDTH - 13) >> 2, bit = 1u << 31;
    while (bit && !(bit & per))
        bit >>= 1;
    per = bit ? (per & (bit >> 1)) | bit : 1;
    uint8_t buf[256];
    for (uint32_t lines = 0;; lines++) {
        uint32_t n = 0, ch;
        int eof = 0;
        while (n < per && !(e = bget(h, &ch, &eof)) && !eof)
            buf[n++] = (uint8_t)ch;
        if (e || n == 0)
            break;
        if (lines % 16 == 0) {
            if ((e = new_line()) || (e = write_s("Address  :")))
                break;
            for (uint32_t k = 0; k < per && !e; k++)
                if (!(e = write_c(' ')))
                    e = hex((display + k) & 0xFF, 2);
            if (!e && per >= 11) {
                e = write_s(" : ");
                for (uint32_t k = 0; k <= (per - 10) / 2 && !e; k++)
                    e = write_c(' ');
                if (!e)
                    e = write_s("ASCII data");
            }
            if (e || (e = new_line()) || (e = new_line()))
                break;
        }
        if ((e = hex(display, 8)) || (e = write_s(" :")))
            break;
        for (uint32_t k = 0; k < per && !e; k++) {
            e = write_c(' ');
            if (!e)
                e = k < n ? hex(buf[k], 2) : write_s("  ");
        }
        if (!e)
            e = write_s(" : ");
        for (uint32_t k = 0; k < n && !e; k++)
            e = write_c(buf[k] >= ' ' && buf[k] <= 0x7E ? buf[k] : '.');
        if (e || (e = new_line()))
            break;
        display += per;
        if (n < per)
            break;
    }
    close_h(h);
    return e;
}

/* *Obey [[-v][-c][-m] [<filename> [<parameters>]]], as RISC OS's Obey module
 * does it (Sources/Programmer/Obey).
 *
 * Obey$Dir is set to the name as given, up to its last '.' ("@" if it has
 * none), GSTrans'd, as the module sets it.
 *
 * Each line ends at any control character or at the end of the file, and
 * tabs are made spaces.  The parameters are substituted into the line
 * (OS_SubstituteArgs32, with the unused ones not appended).  The line then
 * goes to OS_CLI, which takes | lines as comments.
 *
 * The options are two characters, each followed by '-' or a space.
 * -v echoes "Obey: <line>" first.  -c reads the file into memory first,
 * which runs the same lines.  -m means that the "filename" is the address,
 * in hexadecimal, of the text, 0-terminated.
 *
 * Escape between lines is an error.  *Obey alone ends every Obey file in
 * progress after its current line.
 *
 * The last line is a tail call.  Before the line runs, the file is closed
 * and its level and its memory are given back. The line itself runs from
 * the SVC stack, which the application it starts flattens. RISC OS's Obey
 * likewise drops an exhausted file when the application on its last line
 * starts (s.Obey, Service_NewApplication). An application started there
 * (an !Run's "Run <Obey$Dir>.!RunImage", the ROM !Edit's
 * "Desktop_Edit %*0") never comes back to this frame, and every start
 * would leave its RMA behind.
 *
 * Otherwise files nest as deep as the stacks allow.  On RISC OS the limit
 * is reached when FileSwitch finds too little SVC stack to open the next
 * file, and gives "Not enough stack to call filing system".  That also
 * ends an Obey file whose last line obeys itself.  Here the limit is
 * reached when *Obey finds too little room on the native stack or the SVC
 * stack, and it gives the same error (task.h, ros_stack_room).
 *
 * A file that is open while its lines run holds a handle at each level.
 * So a file obeyed again from a line before its last reaches FileSwitch's
 * limit of 255 handles first here, with "Too many open files".  On 5.30
 * the small SVC stack gives out before that (task.h). */
static int obey_depth, obey_stop;
/* This thread's Obey files with lines still to run (a task is a thread).
 * An application started by one of their lines comes back to the file when
 * it exits (ros_obey_catch_exit).  This does not apply to the last line,
 * whose file is closed first. */
static _Thread_local int obey_open;

#define OBEY_BUF 1024u                  /* the module's LongCLISize */
#define ERR_BUFF_OVERFLOW 0x1E4u
#define ERR_ESCAPE 0x11u
#define ERR_NO_STACK 0x414u             /* FileSwitch's NotEnoughStackForFSEntry */

struct obey_src {
    uint32_t h;                         /* the file, or 0: the text at mem */
    uint32_t mem, ptr, ext;
    int own_mem;                        /* mem is -c's copy, the RMA's */
    uint32_t val;                       /* Obey$Dir's value and the parameters, the RMA's */
};

/* The file's memory given back: at its last line, or after it */
static void obey_free(struct obey_src *o)
{
    if (o->own_mem)
        ros_rma_free(ros_ptr(o->mem)), o->own_mem = 0;
    if (o->val)
        ros_rma_free(ros_ptr(o->val)), o->val = 0;
}

static os_error *obey_getc(struct obey_src *o, uint32_t *ch, int *eof)
{
    if (o->h)
        return bget(o->h, ch, eof);
    *eof = o->ptr >= o->ext;
    *ch = *eof ? 0 : ros_ld8(o->mem + o->ptr++);
    return NULL;
}

static os_error *obey_lines(struct obey_src *o, uint32_t params, int verbose, int *exhausted)
{
    uint32_t in = ros_addr(ros_rma_alloc(2 * OBEY_BUF));
    if (!in)
        return ros_error(0x182, "Not enough memory to obey");
    uint32_t outb = in + OBEY_BUF;
    os_error *e = NULL;
    for (int live = 1; live && !e && !obey_stop;) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        swi(XOS_ReadEscapeState, &c);
        if (c.c) {
            ros_cpu_enter(&c);
            c.r[0] = 126;
            swi(XOS_Byte, &c);
            e = ros_error(ERR_ESCAPE, "Escape");
            break;
        }
        uint32_t n = 0, ch;
        int eof = 0;
        for (;;) {
            if ((e = obey_getc(o, &ch, &eof)) != NULL)
                break;
            if (eof)
                ch = 0;
            if (++n >= OBEY_BUF) {
                e = ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow");
                break;
            }
            if (ch == 9)
                ch = ' ';
            if (ch < ' ') {
                n--;
                break;
            }
            ros_st8(in + n - 1, ch);
        }
        if (e)
            break;
        /* the end of the file: this is the last line */
        if (eof)
            live = 0;
        else if (o->h) {
            ros_cpu_enter(&c);
            c.r[0] = 5, c.r[1] = o->h;          /* OS_Args 5: EOF? */
            if ((e = swi(XOS_Args, &c)) != NULL)
                break;
            live = c.r[2] == 0;
        } else
            live = o->ptr < o->ext;
        ros_cpu_enter(&c);
        c.r[0] = params, c.r[1] = outb, c.r[2] = OBEY_BUF, c.r[3] = in, c.r[4] = n;
        c.r[5] = 0x80000000u;                   /* no unused arguments appended */
        if ((e = swi(XOS_SubstituteArgs32, &c)) != NULL)
            break;
        uint32_t len = c.r[2];
        ros_st8(outb + len - 1, 0);
        if (verbose && ((e = write_s("Obey: ")) || (e = write_s(ros_ptr(outb))) ||
                        (e = new_line())))
            break;
        if (!live) {                            /* the last line: done with the file */
            if (o->h)
                close_h(o->h), o->h = 0;
            if (--obey_depth == 0)
                obey_stop = 0;
            obey_open--;
            *exhausted = 1;
            uint32_t outer = ros_svc_sp, line = (outer - len - 8) & ~7u;
            memcpy(ros_ptr(line), ros_ptr(outb), len);
            ros_rma_free(ros_ptr(in));
            obey_free(o);
            ros_svc_sp = line;
            ros_cpu_enter(&c);
            c.r[0] = line;
            e = swi(XOS_CLI, &c);
            ros_svc_sp = outer;
            return e;
        }
        ros_cpu_enter(&c);
        c.r[0] = outb;
        e = swi(XOS_CLI, &c);
    }
    ros_rma_free(ros_ptr(in));
    return e;
}

/* An application started by a line of an Obey file that has more lines to
 * run comes back to the file when it exits.  RISC OS's Obey module puts
 * its own exit handler in front of the application's starter's. It sets
 * the handler in Service_NewApplication, and MyExitHandler goes on with
 * the next line (s.Obey).  So an !Run's "Run <Obey$Dir>.!ResFind App"
 * goes on to the !Run's next line when the application ends. The
 * application there is BASIC, and its END is an OS_Exit. This holds in a
 * Wimp task too, where the handler underneath is the Wimp's, which would
 * end the task.
 *
 * Here the application already comes back to the command that started it
 * (ros_module_run_as_application), so the handler is that return. It is
 * set as the application starts, after the environment it restores is
 * saved. It is set only while an Obey file of this thread has lines to
 * come.  On the last line the file is closed first, and the starter's
 * handler stays, as RISC OS's Obey drops an exhausted file when the
 * application starts.
 *
 * An error is not caught.  Obey's error handler closes its files and
 * passes the error on, as the box's does by unwinding. */
static void obey_exit(struct ros_cpu *s)
{
    (void)s;
    ros_module_app_exit();
}

int ros_obey_lines_left(void)
{
    return obey_open > 0;
}

void ros_obey_catch_exit(void)
{
    static uint32_t entry;
    if (!obey_open)
        return;
    if (!entry)
        entry = ros_native_entry(obey_exit, "Obey:ExitHandler");
    uint32_t r1 = entry, r2 = 0, r3 = 0;
    (void)ros_env_change(ROS_ENV_EXIT, &r1, &r2, &r3);
}

os_error *ros_cmd_obey(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    if (argc == 0) {
        obey_stop = obey_depth > 0;             /* *Obey alone: stop them all */
        return NULL;
    }
    if (!ros_stack_room(ROS_STACK_FILE))
        return ros_error(ERR_NO_STACK, "Not enough stack to call filing system");
    int verbose = 0, cache = 0, memory = 0;
    uint32_t p = tail;
    while (ros_ld8(p) == ' ')
        p++;
    for (;;) {
        uint32_t o = ros_ld8(p + 1) | 0x20, next = ros_ld8(p + 2);
        if (ros_ld8(p) != '-' || (next != '-' && next != ' ') ||
            (o != 'c' && o != 'v' && o != 'm'))
            break;
        verbose |= o == 'v', cache |= o == 'c', memory |= o == 'm';
        p += 2;
    }
    while (ros_ld8(p) == ' ')
        p++;
    uint32_t name = p, end = p;
    while (ros_ld8(end) > ' ')
        end++;
    if (end == name)
        return ros_error(0xFD, "Syntax: *Obey [[-v][-c][-m] [<filename> [<parameters>]]]");
    struct obey_src src = { 0 };
    os_error *e;
    if (memory) {
        char hex[16] = { 0 };
        for (uint32_t k = 0; k < 15 && name + k < end; k++)
            hex[k] = (char)ros_ld8(name + k);
        src.mem = (uint32_t)strtoul(hex, NULL, 16);
        while (ros_ld8(src.mem + src.ext))
            src.ext++;
    } else if ((e = open_in(name, &src.h)) != NULL)
        return e;
    if (cache && src.h) {                       /* -c: the file into memory first */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 2, c.r[1] = src.h;             /* OS_Args 2: the extent */
        if ((e = swi(XOS_Args, &c)) != NULL) {
            close_h(src.h);
            return e;
        }
        uint32_t ext = c.r[2], blk = ros_addr(ros_rma_alloc(ext + 1));
        if (blk) {
            ros_cpu_enter(&c);
            c.r[0] = 4, c.r[1] = src.h, c.r[2] = blk, c.r[3] = ext;  /* OS_GBPB 4 */
            e = swi(XOS_GBPB, &c);
            close_h(src.h);
            if (e) {
                ros_rma_free(ros_ptr(blk));
                return e;
            }
            src = (struct obey_src){ 0, blk, 0, ext, 1, 0 };
        }
    }
    /* Obey$Dir: the name up to its last '.' */
    uint32_t dot = end;
    while (dot > name && ros_ld8(dot - 1) != '.')
        dot--;
    uint32_t val = ros_addr(ros_rma_alloc(OBEY_BUF + 8));
    uint32_t params = val ? val + 512 : 0;
    if (!val) {
        e = ros_error(0x182, "Not enough memory to obey");
        goto done;
    }
    src.val = val;
    if (dot > name + 1 && dot - 1 - name < 500) {
        memcpy(ros_ptr(val), ros_ptr(name), dot - 1 - name);
        ros_st8(val + (dot - 1 - name), 0);
    } else
        strcpy(ros_ptr(val), "@");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(ros_rma_alloc(12)), c.r[1] = val, c.r[2] = 0, c.r[3] = 0, c.r[4] = 0;
    if (!c.r[0]) {
        e = ros_error(0x182, "Not enough memory to obey");
        goto done;
    }
    strcpy(ros_ptr(c.r[0]), "Obey$Dir");
    uint32_t vname = c.r[0];
    e = swi(XOS_SetVarVal, &c);
    ros_rma_free(ros_ptr(vname));
    if (!e) {
        /* the parameters: from the end of the name to the end of the line */
        uint32_t k = 0;
        while (ros_ld8(end + k) >= ' ' && k < OBEY_BUF - 512 - 1)
            ros_st8(params + k, ros_ld8(end + k)), k++;
        ros_st8(params + k, 0);
        int exhausted = 0;
        obey_depth++;
        obey_open++;
        e = obey_lines(&src, params, verbose, &exhausted);
        if (!exhausted) {
            obey_open--;
            if (--obey_depth == 0)
                obey_stop = 0;
        }
    }
done:
    if (src.h)
        close_h(src.h);
    obey_free(&src);
    return e;
}
