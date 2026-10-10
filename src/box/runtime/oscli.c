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
 * (Sources/Kernel: s.Oscli, s.Utility, s.MoreComms, s.SysComms, s.Arthur3,
 * s.MoreSWIs, HelpStrs).
 */

/* oscli.c: OS_CLI and the kernel's own *commands, reimplemented.
 *
 * Written from the kernel's Kernel/s/Oscli, Utility, MoreComms, SysComms,
 * Arthur3 and MoreSWIs.  OS_CLI goes through CLIV, whose default owner is
 * this file.  It does what the kernel does, in the kernel's order:
 *
 *   1. Leading spaces and *s are skipped.  An empty line or a | comment
 *      is nothing.  "%" skips alias expansion.  Lines of 1024 characters
 *      or more give "Too long".
 *   2. "Title:command" runs the command from that module only.
 *   3. "/name" runs a file. "." is Alias$. or *Cat. Both belong to
 *      FileSwitch.
 *   4. An alias, Alias$<command>, which may be abbreviated with ".", is
 *      expanded with OS_SubstituteArgs32. Each of its lines is run
 *      through OS_CLI.
 *   5. The command is looked for in each module's command table, the
 *      kernel's own first. "*FX0" and its kind come first, in the
 *      kernel's fudge table, where a number may follow the name directly.
 *      Names may be abbreviated with ".". The parameters are counted, and
 *      the ones that the table asks for are GSTransed. A wrong count is
 *      answered with the command's syntax.
 *   6. Then comes Service_UKCommand, then FileSwitch, to run the line as
 *      a file.
 *
 * The kernel's own commands here are the ones that need no filing system:
 * *Configure, *Echo, *Error, *Eval, *FX, *Help, *IF, *Key, *Modules, *Set,
 * *SetEval, *SetMacro, *Show, *Status, *Time, *TV and *Unset.  *Configure
 * and *Status handle the modules' keywords only. The kernel's own options
 * are still to come.  The file commands are *Append, *Build, *Close,
 * *Create, *Delete, *Dump, *Exec, *List, *Load, *Opt, *Print, *Remove,
 * *Save, *Spool, *SpoolOn and *Type, and are in runtime/filecmds.c.
 * FileSwitch's commands (*Dir, *Cat, *Copy ...) belong to FileSwitch
 * (modules/fileswitch).
 *
 * Other points:
 *   - Redirection, "{ > file }", "{ >> file }" and "{ < file }", is done
 *     here.  The files are opened and the braces taken out before the
 *     command runs, and the redirection is undone after it. The streams
 *     are in runtime/streams.c.
 *   - A command that nothing knows goes to FileSwitch, to run as a file.
 *     It gives "File 'x' not found" when there is none.
 *   - *Help follows the kernel's Help_Code. It uses Service_Help and
 *     paged mode. A compiled module's help goes through OS_PrettyPrint.
 *     Its International_Help tokens go through its messages. A module's
 *     title gives its summary. There are two differences. Help that is
 *     code (Help_Is_Code) prints its header only. *Help alone gives a
 *     line of its own, and does not give the help on *Help.
 *
 * ARM shadows (#147): a line runs as the kind of the code that made the
 * OS_CLI.  Each module's commands are looked for in the module that this
 * kind reaches. An ARM caller reaches a shadow, and a native caller
 * reaches the chain's own module. *Help works the same way.  The kernel's
 * own work for a line is SWIs made as the same kind. That work is an
 * alias's lines, RMEnsure's command, *Obey's lines, and the OS_Module
 * calls of RMLoad, RMKill, RMReInit, RMEnsure and RMClear.  call() makes
 * them as the kind of the innermost SWI, which is the OS_CLI's own while a
 * line runs.  A module's command handler that makes SWIs of its own makes
 * them as its own code's kind, as always.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rosgd/api.h"
#include "rosgd/armbox.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/rom.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/sysvars.h"
#include "rosgd/task.h"

/* BBC BASIC's inline assembler: which cross targets this build carries
 * (the Makefile's BASICASM; see modules/basicvfp/asmlib). */
#ifndef BASICASM_HAVE_X64
#define BASICASM_HAVE_X64 0
#endif
#ifndef BASICASM_HAVE_A64
#define BASICASM_HAVE_A64 0
#endif
#include "rosgd/platform.h"
#include "rosgd/vector.h"

#define CLIV 0x05u
#define LONG_CLI 1024u              /* LongCLISize and OscliBuffSize */
#define NESTING 16u                 /* OscliNoBuffs: the kernel's circular buffers */

#define SERVICE_UKCOMMAND 0x04u
#define FSCONTROL_STAR_MINUS 3u
#define FSCONTROL_RUN 4u
#define FSCONTROL_CAT 5u
#define FSCONTROL_RESTORE_CURRENT 19u

#define GS_NO_QUOTE (1u << 31)
#define GS_SPC_TERM (1u << 29)

#define ERR_SYNTAX         0x0DCu   /* also BadNoParms, TooManyParms */
#define ERR_BAD_KEY        0x0FBu
#define ERR_BAD_PARM_STR   0x0FDu
#define ERR_BAD_COMMAND    0x0FEu
#define ERR_LONG_LINE      0x1E0u
#define ERR_TOO_HARD       0x1E1u
#define ERR_BUFF_OVERFLOW  0x1E4u

/* ---- small things --------------------------------------------------------------- */

static int is_end(uint32_t c)
{
    return c == 13 || c == 10 || c == 0;
}

static uint32_t upper(uint32_t c)
{
    return c >= 'a' && c <= 'z' ? c - 0x20 : c;
}

/* The kernel's Up_ItAndTerm_Check_Table: a command's characters upper
 * cased, and 0 for those that end a command's name. */
static uint32_t up_it(uint32_t c)
{
    if (c >= 0x80)
        return c;
    if (c <= ' ' || c == 0x7F || strchr("\"$%&,:<>\\^|", (int)c))
        return 0;
    return upper(c);
}

static os_error *err(uint32_t n, const char *text)
{
    return ros_error(n, "%s", text);
}

static uint32_t scratch(void)
{
    uint8_t *b = ros_rma_alloc(LONG_CLI + 4);
    return b ? ros_addr(b) : 0;
}

static void release(uint32_t b)
{
    if (b)
        ros_rma_free(ros_ptr(b));
}

/* A SWI from here: the registers in and out, V back.  It is made as the
 * kind of the SWI that this is the work of (the OS_CLI's, for a line), so
 * an ARM caller's line makes ARM calls to the end. */
static int call(uint32_t swi, uint32_t r[6])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 6 * sizeof r[0]);
    ros_swi_as(&c, swi, ros_caller_kind());
    memcpy(r, c.r, 6 * sizeof r[0]);
    return (int)c.v;
}

static os_error *error_in(const uint32_t r[6])
{
    return ros_ptr(r[0]);
}

static os_error *write_c(uint32_t c)
{
    return xos_write_c((uint8_t)c);
}

static os_error *write_s(const char *s)
{
    for (; *s; s++) {
        os_error *e = *s == '\n' ? xos_new_line() : write_c((uint8_t)*s);
        if (e)
            return e;
    }
    return NULL;
}

/* A module's text, as OS_PrettyPrint would put it: CR a new line, 27 0 the
 * keyword, TAB to the next column of eight.  No wrapping. */
static os_error *pretty(const char *s, const char *keyword)
{
    unsigned col = 0;
    os_error *e = NULL;
    for (; *s && !e; s++) {
        uint8_t c = (uint8_t)*s;
        if (c == 13) {
            e = xos_new_line();
            col = 0;
        } else if (c == 27 && s[1] == 0) {
            e = write_s(keyword);
            col += (unsigned)strlen(keyword);
            s++;
        } else if (c == 9) {
            do
                e = write_c(' ');
            while (!e && ++col % 8);
        } else {
            e = write_c(c == 31 ? ' ' : c);
            col++;
        }
    }
    return e;
}

/* OS_FSControl, which FileSwitch owns: without it (no filing system has
 * claimed FSCV), the command is simply one nothing knows. */
static os_error *fs_control(uint32_t reason, uint32_t arg)
{
    uint32_t r[6] = { reason, arg, 0, 0, 0, 0 };
    if (!call(XOS_FSControl, r))
        return NULL;
    os_error *e = error_in(r);
    if (e->errnum == ROS_ERR_NO_SUCH_SWI || e->errnum == 0x40Bu)
        return err(ERR_BAD_COMMAND, "Command not recognised");
    return e;
}

/* ---- command tables ---------------------------------------------------------------- */

/* One entry of a module's table, compiled or native. */
struct entry {
    const char *name;
    uint32_t info;
    const char *syntax, *help;
    uint32_t exec;                  /* compiled: code address */
    uint32_t at;                    /* compiled: its code offset's word in the table */
    const struct ros_command *native;
};

/* The nth entry, or 0 at the end. */
static int entry_at(const struct ros_module *m, uint32_t *cursor, struct entry *e)
{
    memset(e, 0, sizeof *e);
    if (m->commands) {
        const struct ros_command *c = &m->commands[*cursor];
        if (!c->name)
            return 0;
        (*cursor)++;
        e->name = c->name, e->info = c->info, e->syntax = c->syntax, e->help = c->help;
        e->native = c;
        return 1;
    }
    if (!m->command_table)
        return 0;
    uint32_t p = *cursor ? *cursor : m->command_table;
    if (!ros_ld8(p))
        return 0;
    e->name = ros_ptr(p);
    uint32_t q = (p + (uint32_t)strlen(e->name) + 1 + 3) & ~3u;
    uint32_t exec = ros_ld32(q), syntax = ros_ld32(q + 8), help = ros_ld32(q + 12);
    e->info = ros_ld32(q + 4);
    e->at = q;
    e->exec = exec ? m->base + exec : 0;
    e->syntax = syntax ? (const char *)ros_ptr(m->base + syntax) : NULL;
    e->help = help ? (const char *)ros_ptr(m->base + help) : NULL;
    *cursor = q + 16;
    return 1;
}

/* The kernel's FindItem match: how much of cmd names this entry, or -1.
 * The amount is its length, with a "." after an abbreviation.  Messy
 * matching (the fudge table) lets a name be followed by anything but a
 * letter. */
static int match(uint32_t cmd, const char *name, int messy)
{
    for (int i = 0;; i++) {
        uint32_t a = up_it(ros_ld8(cmd + (uint32_t)i)), b = (uint8_t)name[i];
        if (a <= 32 && b <= 32)
            return i;
        b = upper(b);
        if (a == b)
            continue;
        if (a == '.')
            return b <= 32 ? -1 : i + 1;
        if (!messy || b > 32 || (a >= 'A' && a <= 'Z'))
            return -1;
        return i;
    }
}

static int is_command(const struct entry *e, uint32_t wanted)
{
    return ((e->info & 0xC0000000u) ^ wanted) == 0 && (e->exec || (e->native && e->native->run));
}

/* ---- International_Help: help and syntax as tokens in the module's messages -------- */

/* The messages file a compiled module's header names (+&2C), open in the
 * descriptor at desc, as the kernel opens it for an International_Help
 * command: 1 if it is; 0, and the Global messages to be used, if the
 * module names none or it will not open. */
static int open_module_messages(const struct ros_module *m, uint32_t desc)
{
    uint32_t off = m->base ? ros_ld32(m->base + 0x2C) : 0;
    if (!off || off & 0xC0000003u)
        return 0;
    for (unsigned i = 0; i < ros_rom_image_count; i++)
        if (*ros_rom_images[i].base == m->base && off >= *ros_rom_images[i].size)
            return 0;
    uint32_t r[6] = { desc, m->base + off, 0, 0, 0, 0 };
    return !call(XMessageTrans_OpenFile, r);
}

static void close_module_messages(int open, uint32_t desc)
{
    uint32_t r[6] = { desc, 0, 0, 0, 0, 0 };
    if (open)
        call(XMessageTrans_CloseFile, r);
}

/* A token's message, in place in the file (R2 = 0), and MessageTrans'
 * dictionary: the kernel's lookup for help and syntax.  Where the token
 * is not found, the error's text stands in for it, as in the kernel. */
static uint32_t intl_message(int open, uint32_t desc, uint32_t token, uint32_t *dict)
{
    uint32_t r[6] = { open ? desc : 0, token, 0, 0, 0, 0 };
    uint32_t text = call(XMessageTrans_Lookup, r) ? r[0] + 4 : r[2];
    uint32_t d[6] = { 0 };
    *dict = call(XMessageTrans_Dictionary, d) ? 0 : d[0];
    return text;
}

/* The kernel's expandsyntaxmessage.  27 0 stands for the command's name,
 * and 27 n for the dictionary's nth entry, which may be nested.  It runs
 * to the message's end. */
static void expand_syntax(uint32_t p, uint32_t dict, const char *name, char *out, size_t *n,
                          size_t size, int depth)
{
    for (uint32_t c; (c = ros_ld8(p++)) != 0 && c != 10 && c != 13 && *n < size - 1;) {
        if (c != 27) {
            out[(*n)++] = (char)c;
            continue;
        }
        uint32_t k = ros_ld8(p++);
        if (k == 0) {
            for (const char *q = name; *q > ' ' && *n < size - 1; q++)
                out[(*n)++] = *q;
        } else if (dict && depth < 8) {
            uint32_t t = dict;
            while (--k && ros_ld8(t))
                t += ros_ld8(t);
            if (ros_ld8(t))
                expand_syntax(t + 1, dict, name, out, n, size, depth + 1);
        }
    }
}

/* A command's syntax message, in which \x1B\x00 stands for its name.  A
 * compiled command's International_Help syntax is its token's message.
 * It is first offered to Service_SyntaxError, then looked up in the
 * module's messages (the kernel's Oscli, ModCommsLookUp). */
static os_error *syntax_error(const struct ros_module *m, const struct entry *e)
{
    char text[252];
    size_t n = 0;
    if (!e->native && e->syntax && e->info & ROS_CMD_INTL_HELP) {
        uint32_t r[6] = { 0, 0x8Cu, e->at, m->base, 0, 0 };     /* Service_SyntaxError */
        call(XOS_ServiceCall, r);
        if (r[1] == 0)
            return error_in(r);
        uint32_t desc = scratch(), dict;
        if (!desc)
            return err(ERR_LONG_LINE, "Too long");
        int open = open_module_messages(m, desc);
        uint32_t msg = intl_message(open, desc, ros_addr(e->syntax), &dict);
        expand_syntax(msg, dict, e->name, text, &n, sizeof text, 0);
        close_module_messages(open, desc);
        release(desc);
        text[n] = 0;
        return ros_error(ERR_SYNTAX, "%s", text);
    }
    const char *s = e->syntax ? e->syntax : "Invalid number of parameters";
    for (; *s && n < sizeof text - 1; s++) {
        if (*s == 27 && s[1] == 0) {
            for (const char *k = e->name; *k > ' ' && n < sizeof text - 1; k++)
                text[n++] = *k;
            s++;
        } else {
            text[n++] = *s == 13 ? ' ' : *s;
        }
    }
    text[n] = 0;
    return ros_error(ERR_SYNTAX, "%s", text);
}

/* The kernel's ModCommsLookUp once an entry matched len characters of cmd:
 * count the parameters, GSTrans those the table asks for, check the count,
 * and call it. */
static os_error *run_entry(struct ros_module *m, const struct entry *e, uint32_t cmd, int len)
{
    uint32_t p = cmd + (uint32_t)len, c;
    do
        c = ros_ld8(p++);
    while (c == ' ');
    uint32_t tail = p - 1, argc = 0, map = (e->info >> 8) & 0xFF, buf = 0, used = 0;
    os_error *x = NULL;
    if (map) {
        if (!(buf = scratch()))
            return err(ERR_LONG_LINE, "Too long");
        tail = buf;
    }
#define ADD(ch)                                                             \
    do {                                                                    \
        if (map) {                                                          \
            if (used >= LONG_CLI) {                                         \
                x = err(ERR_LONG_LINE, "Too long");                         \
                goto out;                                                   \
            }                                                               \
            ros_st8(buf + used++, (ch));                                    \
        }                                                                   \
    } while (0)
    for (uint32_t bits = map; !is_end(c);) {
        int gstrans = bits & 1;
        bits >>= 1;
        if (gstrans) {
            uint32_t r[6] = { p - 1, buf + used, (LONG_CLI - used) | GS_SPC_TERM, 0, 0, 0 };
            struct ros_cpu g;
            ros_cpu_enter(&g);
            memcpy(g.r, r, sizeof r);
            ros_thunk_OS_GSTrans(&g);
            if (g.c) {
                x = err(ERR_LONG_LINE, "Too long");
                goto out;
            }
            if (g.v || g.r[2] == 0) {
                x = err(ERR_BAD_PARM_STR, "Parameter expansion contains unrecognised characters");
                goto out;
            }
            for (uint32_t i = 0; i < g.r[2]; i++) {
                uint32_t ch = ros_ld8(buf + used + i);
                if (ch <= ' ' || ch == 0x7F) {
                    x = err(ERR_BAD_PARM_STR,
                            "Parameter expansion contains unrecognised characters");
                    goto out;
                }
            }
            used += g.r[2];
            p = g.r[0];
        } else {
            int quoted = c == '"', in = quoted;
            for (;;) {
                ADD(c);
                c = ros_ld8(p++);
                if (c == '"' && quoted)
                    in = !in;
                if ((c == ' ' && !in) || is_end(c))
                    break;
            }
        }
        ADD(' ');
        p--;
        do
            c = ros_ld8(p++);
        while (c == ' ');
        argc++;
    }
    ADD(c);
    uint32_t min = e->info & 0xFF, max = (e->info >> 16) & 0xFF;
    if (argc < min || argc > max) {
        x = syntax_error(m, e);
        goto out;
    }
    if (e->native) {
        x = e->native->run(m, tail, argc);
    } else {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = tail, s.r[1] = argc, s.r[12] = m->private_word;
        s.r[14] = ROS_RETURN_TO_NATIVE;
        ros_call(&s, e->exec);
        if (s.r[15] != ROS_RETURN_TO_NATIVE)
            ros_bad_return(&s, ROS_RETURN_TO_NATIVE);
        if (s.v)
            x = ros_ptr(s.r[0]);
    }
out:
#undef ADD
    release(buf);
    return x;
}

/* A command in one module's table: 1 if found (and run, with *x its error), 0 if not. */
static int in_module(struct ros_module *m, uint32_t cmd, uint32_t wanted, int messy,
                     const struct ros_command *table, os_error **x)
{
    struct ros_module view = *m;
    if (table)
        view.commands = table;
    struct entry e;
    uint32_t cursor = 0;
    while (entry_at(&view, &cursor, &e)) {
        int len = match(cmd, e.name, messy);
        if (len >= 0 && is_command(&e, wanted)) {
            *x = run_entry(m, &e, cmd, len);
            return 1;
        }
    }
    return 0;
}

/* ---- aliases -------------------------------------------------------------------------- */

/* Alias$<cmd>, exactly or abbreviated: 1 and its name and how much of cmd
 * it took, or 0.
 *
 * A command that is not abbreviated is one lookup, as the kernel's
 * Oscli_QuickAliases is (Kernel/s/Oscli, oqa_loop1, and ChocolateOscli is
 * TRUE in RISC OS 5's own build).  The lookup is "Alias$" and the
 * command's name, upper cased and ended as Up_ItAndTerm_Check_Table does
 * it, found by binary chop (VarFindIt_QA).  The name it would have is
 * known, so there is nothing to search for and nothing to compare here.
 * If it is not found there is no alias, and the command goes on to the
 * tables.
 *
 * Only a command whose name ends in "." goes round the whole of Alias$*
 * comparing each. Such a name is an abbreviation, and so is not one name.
 * This is the kernel's own slow path ("oqa_treacletime").  Every call of
 * it is a length probe that comes back with "Buffer overflow".  That is
 * the expected answer and not a fault. The kernel says as much where it
 * turns error translation off for these calls: "We are about to get lots
 * of buffer overflow errors".  Before the quick way was added here, every
 * command went round that loop. In one run of the hosted self-test that
 * was 1.6 million calls and as many errors, a fifth of every SWI the box
 * made. */
static int find_alias(uint32_t cmd, uint32_t *name, uint32_t *len)
{
    uint32_t pattern = scratch(), ctx = 0;
    if (!pattern)
        return 0;
    char *p = ros_ptr(pattern);
    memcpy(p, "Alias$", 6);
    uint32_t n = 0, c;
    while (n < LONG_CLI - 8 && (c = up_it(ros_ld8(cmd + n))) != 0)
        p[6 + n++] = (char)c;
    p[6 + n] = 0;
    if (n < LONG_CLI - 8 && (n == 0 || p[6 + n - 1] != '.')) {
        uint32_t node = ros_sysvars_find_quick(pattern);
        release(pattern);
        if (!node)
            return 0;
        *name = node, *len = n;
        return 1;
    }
    strcpy(p, "Alias$*");
    for (;;) {
        uint32_t r[6] = { pattern, 0, (uint32_t)-1, ctx, 0, 0 };
        call(XOS_ReadVarVal, r);
        if (r[2] == 0)
            break;                      /* no more */
        ctx = r[3];
        uint32_t a = ctx + 6;
        for (uint32_t i = 0;; i++) {
            uint32_t c4 = up_it(ros_ld8(cmd + i)), c5 = ros_ld8(a + i);
            if (c4 <= ' ' && c5 <= ' ') {
                *name = ctx, *len = i;
                release(pattern);
                return 1;
            }
            c5 = upper(c5);
            if (c4 == c5)
                continue;
            if (i && c5 > ' ' && c4 == '.') {
                *name = ctx, *len = i + 1;
                release(pattern);
                return 1;
            }
            break;
        }
    }
    release(pattern);
    return 0;
}

/* The kernel's circular buffers (Kernel/s/Oscli: GetOscliBuffer,
 * ReleaseBuff, CheckUID), which an alias's lines are expanded into.
 * There are NESTING buffers of LONG_CLI bytes. They are taken in turn and
 * a taking is never refused. Each taking has a number, its UID. Only the
 * last NESTING - 1 taken are still their takers' (the kernel moves botUID
 * on as the ring comes round). Giving back the last one taken lets the
 * next taking have it again.
 *
 * So an expansion whose command never comes back holds nothing, and its
 * buffer is taken in its turn.  This happens when an application that it
 * started never returns, when an exit or error handler goes back past it,
 * and when a Wimp task ends.  An expansion that finds its buffer gone
 * when a line comes back fails with "Expansion too complex" (FailInAlias).
 * Fifteen levels deep is the most that comes back whole, as on RISC OS
 * 5.30, where sixteen runs the innermost line and then fails.
 *
 * The value is read into a buffer of its own first (AliasExpansionBuffer).
 * All of it is in the RMA, claimed once. */
static uint32_t ring, ring_at, ring_top, ring_bot, alias_value;

static int ring_made(void)
{
    if (!ring) {
        uint8_t *b = ros_rma_alloc(NESTING * LONG_CLI + LONG_CLI + 4);
        if (!b)
            return 0;
        ring = ros_addr(b);
        alias_value = ring + NESTING * LONG_CLI;
    }
    return 1;
}

static uint32_t take_buffer(uint32_t *uid)
{
    ring_at = (ring_at + 1) % NESTING;
    *uid = ++ring_top;
    if (ring_top - ring_bot >= NESTING)
        ring_bot++;
    return ring + ring_at * LONG_CLI;
}

static int still_mine(uint32_t uid)
{
    return (int32_t)(uid - ring_bot) > 0;
}

static void give_back(uint32_t uid)
{
    if (ring_top == ring_bot || uid != ring_top)
        return;
    ring_top--;
    ring_at = (ring_at + NESTING - 1) % NESTING;
}

/* The expansions in progress on this thread, innermost last.  Each records
 * run_alias's frame, and its buffer's UID and place.
 *
 * On RISC OS an application that an expansion's line starts replaces
 * everything below it for good.  Nothing comes back to the expansion, and
 * the ring hands its buffer to the next taker.  Here the application
 * comes back to the command that started it (module.c,
 * ros_module_run_as_application), and the expansion goes on with its next
 * line.  So as an application starts, each expansion under it whose
 * buffer is still its own keeps a copy of its lines
 * (ros_oscli_application_starts).  An expansion may come back to find its
 * buffer taken by the application's own expansions.  Those have the ring
 * to themselves, as on RISC OS, so fifteen levels come back whole however
 * deep the expansions that started it were.  Such an expansion takes a
 * buffer again and goes on from the copy.  With no application started
 * under it, a buffer found gone is "Expansion too complex", as on 5.30.
 *
 * An expansion is not in progress if its frame is below the one starting
 * now, or below a longjmp's target (ros_oscli_unwind_below, from
 * ros_resume_unwind).  A thread that ends takes its list with it (the
 * key's destructor).  The copies are in the host heap and are reached only
 * from here. */
struct expansion {
    uintptr_t frame;
    uint32_t uid, lines;
    void *copy;                         /* LONG_CLI bytes, or NULL */
};
struct expansions {
    unsigned n, max;
    struct expansion v[];
};
static _Thread_local struct expansions *exps;
static pthread_key_t exps_key;
static pthread_once_t exps_once = PTHREAD_ONCE_INIT;

static void exps_free(void *p)
{
    struct expansions *e = p;
    for (unsigned i = 0; i < e->n; i++)
        free(e->v[i].copy);
    free(e);
}

static void exps_key_make(void)
{
    pthread_key_create(&exps_key, exps_free);
}

/* Those whose frames are below frame: gone */
static void exps_drop_below(uintptr_t frame)
{
    while (exps && exps->n && exps->v[exps->n - 1].frame < frame)
        free(exps->v[--exps->n].copy);
}

void ros_oscli_unwind_below(const void *to)
{
    exps_drop_below((uintptr_t)to);
}

/* An expansion starting at frame: its index, or -1 if there is no room
 * to note it (it then fails as RISC OS's would, if it comes back) */
static int exps_push(uintptr_t frame, uint32_t uid, uint32_t lines)
{
    exps_drop_below(frame + 1);
    if (!exps || exps->n == exps->max) {
        unsigned max = exps ? exps->max * 2 : 16;
        struct expansions *e = realloc(exps, sizeof *e + max * sizeof e->v[0]);
        if (!e)
            return -1;
        if (!exps) {
            e->n = 0;
            pthread_once(&exps_once, exps_key_make);
        }
        e->max = max;
        exps = e;
        pthread_setspecific(exps_key, e);
    }
    exps->v[exps->n] = (struct expansion){ frame, uid, lines, NULL };
    return (int)exps->n++;
}

static void exps_pop(uintptr_t frame)
{
    exps_drop_below(frame);
    if (exps && exps->n && exps->v[exps->n - 1].frame == frame)
        free(exps->v[--exps->n].copy);
    if (exps && !exps->n && exps->max > 64) {   /* an alias that expanded itself: done */
        free(exps);
        exps = NULL;
        pthread_setspecific(exps_key, NULL);
    }
}

void ros_oscli_application_starts(void)
{
    exps_drop_below((uintptr_t)__builtin_frame_address(0));
    for (unsigned i = 0; exps && i < exps->n; i++) {
        struct expansion *x = &exps->v[i];
        if (!x->copy && still_mine(x->uid) && (x->copy = malloc(LONG_CLI)) != NULL)
            memcpy(x->copy, ros_ptr(x->lines), LONG_CLI);
    }
}

/* Expansion me, at frame, has come back from an application to find its
 * buffer taken.  This gives it a buffer again, with its lines from the
 * copy.  It returns 0 if there is no copy. */
static uint32_t exps_again(int me, uintptr_t frame, uint32_t *uid)
{
    if (me < 0 || !exps || (unsigned)me >= exps->n || exps->v[me].frame != frame ||
        !exps->v[me].copy)
        return 0;
    struct expansion *x = &exps->v[me];
    x->lines = take_buffer(&x->uid);
    memcpy(ros_ptr(x->lines), x->copy, LONG_CLI);
    free(x->copy);
    x->copy = NULL;
    *uid = x->uid;
    return x->lines;
}

/* Run an alias: its value expanded, the arguments substituted into the
 * next of the kernel's buffers, and each line through OS_CLI. */
static os_error *run_alias(uint32_t name, uint32_t args)
{
    /* Nothing but the stack stops an alias expanding itself for ever.  On
     * RISC OS 5.30 the result is a data abort.  Here the room on both
     * stacks is checked, and "Expansion too complex" is given without it
     * (task.h). */
    if (!ros_stack_room(ROS_STACK_ALIAS))
        return err(ERR_TOO_HARD, "Expansion too complex");
    if (!ring_made())
        return err(ERR_LONG_LINE, "Too long");
    uint32_t r[6] = { name, alias_value, LONG_CLI, 0, 3, 0 };
    if (call(XOS_ReadVarVal, r))
        return err(ERR_LONG_LINE, "Too long");
    ros_st8(alias_value + r[2], 13);
    uint32_t uid, lines = take_buffer(&uid);
    uint32_t s[6] = { args, lines, LONG_CLI, alias_value, r[2], 0 };
    if (call(XOS_SubstituteArgs32, s)) {
        give_back(uid);
        return err(ERR_LONG_LINE, "Too long");
    }
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    int me = exps_push(here, uid, lines);
    os_error *x = NULL;
    for (uint32_t p = lines, end = lines + s[2]; p < end && !x;) {
        uint32_t c[6] = { p, 0, 0, 0, 0, 0 };
        if (call(XOS_CLI, c))
            x = error_in(c);
        else if (!still_mine(uid)) {
            uint32_t again = exps_again(me, here, &uid);
            if (again) {                /* an application came back: on, from the copy */
                p = again + (p - lines);
                end = again + (end - lines);
                lines = again;
            } else
                x = err(ERR_TOO_HARD, "Expansion too complex");
        }
        while (!is_end(ros_ld8(p++)))
            ;
    }
    exps_pop(here);
    if (still_mine(uid))
        give_back(uid);
    return x;
}

/* ---- the kernel's own commands ----------------------------------------------------------- */

/* Set, SetMacro, SetEval: the name to its space, then the value. */
static os_error *set_type(uint32_t tail, uint32_t type)
{
    uint32_t v = tail;
    while (ros_ld8(v) != ' ')
        v++;
    while (ros_ld8(v) == ' ')
        v++;
    uint32_t r[6] = { tail, v, 1, 0, type, 0 };
    return call(XOS_SetVarVal, r) ? error_in(r) : NULL;
}

static os_error *cmd_set(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return set_type(tail, 0);
}

static os_error *cmd_setmacro(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return set_type(tail, 2);
}

static os_error *cmd_seteval(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return set_type(tail, 3);
}

static os_error *cmd_unset(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    /* Each deletion hands back where to go on from: a code variable, which
     * this does not delete, is stepped over. */
    for (uint32_t ctx = 0;;) {
        uint32_t r[6] = { tail, 0, (uint32_t)-1, ctx, 0, 0 };
        if (call(XOS_SetVarVal, r))
            return NULL;                /* none left: never an error */
        ctx = r[3];
    }
}

/* A value as *Show and *Eval write it: control characters as |X, DEL as
 * |?, and |, " and < escaped where GSTrans would read them. */
static os_error *write_value(uint32_t p, uint32_t n, int escape_specials)
{
    os_error *e = NULL;
    for (uint32_t i = 0; i < n && !e; i++) {
        uint32_t c = ros_ld8(p + i);
        if (c == 0x7F)
            c = (uint32_t)-1;
        if ((int32_t)c <= 31) {
            c += '@';
            e = write_c('|');
        }
        if (!e && escape_specials && (c == '|' || c == '"' || c == '<'))
            e = write_c('|');
        if (!e)
            e = write_c(c);
    }
    return e;
}

static os_error *cmd_show(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    uint32_t pattern = tail, star = 0, buf = scratch();
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    if (argc == 0) {
        star = scratch();
        strcpy(ros_ptr(star), "*");
        pattern = star;
    }
    uint32_t b[6] = { 117, 0, 0, 0, 0, 0 };        /* paged mode on while it lists */
    call(XOS_Byte, b);
    os_error *e = write_c(14);
    for (uint32_t ctx = 0; !e;) {
        uint32_t r[6] = { pattern, buf, LONG_CLI, ctx, 0, 0 };
        if (call(XOS_ReadVarVal, r)) {
            if (error_in(r)->errnum != ERR_BUFF_OVERFLOW)
                break;                  /* not found: the end */
        }
        ctx = r[3];
        uint32_t type = r[4], n = r[2];
        e = write_s(ros_ptr(ctx));
        if (!e && type == 1) {          /* a number, in decimal */
            n = (uint32_t)snprintf(ros_ptr(buf), LONG_CLI, "%d", (int32_t)ros_ld32(buf));
            e = write_s("(Number)");
        } else if (!e && type == 2) {
            e = write_s("(Macro)");
        }
        if (!e)
            e = write_s(" : ");
        if (!e)
            e = write_value(buf, n, type != 2);
        if (!e)
            e = xos_new_line();
    }
    if (!(b[1] & 5)) {
        os_error *off = write_c(15);
        if (!e)
            e = off;
    }
    release(buf);
    release(star);
    return e;
}

static os_error *cmd_echo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct ros_cpu g;
    ros_cpu_enter(&g);
    g.r[0] = tail, g.r[2] = GS_NO_QUOTE;
    ros_thunk_OS_GSInit(&g);
    for (;;) {
        ros_thunk_OS_GSRead(&g);
        if (g.v)
            return ros_ptr(g.r[0]);
        if (g.c)
            break;
        os_error *e = write_c(g.r[1]);
        if (e)
            return e;
    }
    return xos_new_line();
}

/* *Error [number] text: the text GSTransed. */
static os_error *cmd_error(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t r[6] = { 0, tail, 0, 0, 0, 0 };
    call(XOS_ReadUnsigned, r);          /* no number: 0, and the text from the start */
    uint32_t number = r[2], buf = scratch();
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    uint32_t g[6] = { r[1], buf, 252, 0, 0, 0 };
    call(XOS_GSTrans, g);
    uint32_t n = g[2] > 251 ? 251 : g[2];
    ros_st8(buf + n, 0);
    os_error *e = ros_error(number, "%s", (const char *)ros_ptr(buf));
    release(buf);
    return e;
}

static os_error *cmd_eval(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t buf = scratch();
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    uint32_t r[6] = { tail, buf, LONG_CLI, 0, 0, 0 };
    os_error *e = NULL;
    if (call(XOS_EvaluateExpression, r)) {
        e = error_in(r);
    } else {
        uint32_t n = r[2];
        if (r[1] == 0)
            n = (uint32_t)snprintf(ros_ptr(buf), 256, "%d", (int32_t)r[2]);
        e = write_s(r[1] == 0 ? "Result is an integer, value : " : "Result is a string, value : ");
        if (!e)
            e = write_value(buf, n, 1);
        if (!e)
            e = xos_new_line();
    }
    release(buf);
    return e;
}

/* A word at p, such as "THEN" or "ELSE", in any case, followed by a space
 * or the end. */
static int word_at(uint32_t p, const char *w, int end_ok)
{
    for (; *w; w++, p++)
        if (upper(ros_ld8(p)) != (uint8_t)*w)
            return 0;
    uint32_t c = ros_ld8(p);
    return c == ' ' || (end_ok && is_end(c));
}

/* *IF expression THEN command [ELSE command].  The kernel's IF_Code holds
 * nothing while the command runs.  The THEN part is in its
 * GeneralMOSBuffer, and the ELSE part is where it was typed.  An
 * application that the command starts may never come back here.  Two cases
 * are a Wimp task's end and a TaskObey file whose last line runs itself
 * again.  So the expression is read in the RMA and given back before the
 * command runs.  The THEN part runs from the SVC stack, as FileSwitch's
 * run action's command does, and the application's start flattens that
 * stack.  Nothing is left behind. */
static os_error *cmd_if(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t buf = scratch(), p = tail, n = 0, c;
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    for (;;) {
        c = ros_ld8(p++);
        ros_st8(buf + n++, c);
        if (is_end(c) || n >= LONG_CLI) {
            release(buf);
            return err(ERR_SYNTAX, "There is no THEN");
        }
        if (c == ' ' && word_at(p, "THEN", 1))
            break;
    }
    ros_st8(buf + n - 1, 13);
    p += 4;                             /* after THEN */
    uint32_t r[6] = { buf, buf, (uint32_t)-1, 0, 0, 0 };
    os_error *e = NULL;
    if (call(XOS_EvaluateExpression, r))
        e = r[1] ? err(ERR_SYNTAX, "Expression is a string") : error_in(r);
    release(buf);
    if (e)
        return e;
    if (r[2]) {                         /* THEN: up to any ELSE */
        n = 0;
        for (uint32_t q = p;;) {
            c = ros_ld8(q++);
            n++;
            if (is_end(c) || n >= LONG_CLI || (c == ' ' && word_at(q, "ELSE", 0)))
                break;
        }
        uint32_t outer = ros_svc_sp, line = (outer - n - 8) & ~7u;
        for (uint32_t i = 0; i + 1 < n; i++)
            ros_st8(line + i, ros_ld8(p + i));
        ros_st8(line + n - 1, 13);
        ros_svc_sp = line;
        uint32_t x[6] = { line, 0, 0, 0, 0, 0 };
        if (call(XOS_CLI, x))
            e = error_in(x);
        ros_svc_sp = outer;
    } else {                            /* ELSE, if there is one */
        for (;;) {
            c = ros_ld8(p++);
            if (is_end(c))
                return NULL;
            if (c == ' ' && word_at(p, "ELSE", 0))
                break;
        }
        uint32_t x[6] = { p + 4, 0, 0, 0, 0, 0 };
        if (call(XOS_CLI, x))
            e = error_in(x);
    }
    return e;
}

/* ---- *Configure and *Status ----------------------------------------------------- */

/* The kernel's (Kernel/s/Arthur3, Configure_Code and Status_Code), for the
 * modules' keywords: the kernel's own options (Baud, Mode, Language ...)
 * are not here yet, so a list shows the modules' alone. */

#define SERVICE_UKCONFIG 0x28u
#define SERVICE_UKSTATUS 0x29u

os_error *ros_configure_error(uint32_t which)
{
    switch (which) {
    case 0:
        return err(ERR_SYNTAX, "Configure option not recognised");
    case 1:
        return err(ERR_SYNTAX, "Numeric parameter needed");
    case 2:
        return err(ERR_SYNTAX, "Configure parameter too big");
    default:
        return err(ERR_SYNTAX, "Too many parameters");
    }
}

/* A module's keyword, entered as the kernel enters it.  R0 is 0 to list
 * its syntax, 1 to print its status, and otherwise points to its
 * parameters.  On return, R0 = 0 to 3 with V set means one of the generic
 * errors. */
static os_error *call_keyword(struct ros_module *m, const struct entry *e, uint32_t arg)
{
    if (e->native)
        return e->native->run(m, arg, 0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = arg, s.r[1] = m->base, s.r[12] = m->private_word;
    ros_call(&s, e->exec);
    if (s.r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(&s, ROS_RETURN_TO_NATIVE);
    if (!s.v)
        return NULL;
    return s.r[0] <= 3 ? ros_configure_error(s.r[0]) : ros_ptr(s.r[0]);
}

/* The keyword that the tail starts with, in a module's table.  It returns
 * 1 and where its parameters start, or 0 if there is none. */
static int find_keyword(uint32_t tail, struct ros_module **mod, struct entry *e, uint32_t *params)
{
    for (struct ros_module *x = ros_module_first(); x; x = x->next) {
        uint32_t cursor = 0;
        while (entry_at(x, &cursor, e)) {
            int len = match(tail, e->name, 0);
            if (len >= 0 && is_command(e, ROS_CMD_CONFIGURE)) {
                uint32_t p = tail + (uint32_t)len;
                while (ros_ld8(p) == ' ')
                    p++;
                *mod = x, *params = p;
                return 1;
            }
        }
    }
    return 0;
}

/* *Configure and *Status alone: every module's keyword, then the service
 * call. */
static os_error *list_keywords(uint32_t arg, uint32_t service, const char *title,
                               const char *tail)
{
    uint32_t b[6] = { 117, 0, 0, 0, 0, 0 };
    call(XOS_Byte, b);
    os_error *e = write_c(14);
    if (!e)
        e = write_s(title);
    for (struct ros_module *x = ros_module_first(); x && !e; x = x->next) {
        struct entry en;
        uint32_t cursor = 0;
        while (!e && entry_at(x, &cursor, &en))
            if (is_command(&en, ROS_CMD_CONFIGURE))
                e = call_keyword(x, &en, arg);
    }
    if (!e) {
        uint32_t r[6] = { 0, service, 0, 0, 0, 0 };
        call(XOS_ServiceCall, r);
        e = write_s(tail);
    }
    if (!(b[1] & 5)) {
        os_error *off = write_c(15);
        if (!e)
            e = off;
    }
    return e;
}

static os_error *cmd_configure(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    if (argc == 0)
        return list_keywords(0, SERVICE_UKCONFIG, "Configuration options:\n\n",
                             "\nWhere:\nD is a decimal number, a hexadecimal number preceded by &,\n"
                             "or the base followed by underscore, followed\n"
                             "by digits in the given base.\nItems within [ ] are optional.\n"
                             "Use *Status to display the current settings.\n");
    struct ros_module *x;
    struct entry e;
    uint32_t params;
    if (find_keyword(tail, &x, &e, &params))
        return call_keyword(x, &e, params);
    uint32_t r[6] = { tail, SERVICE_UKCONFIG, 0, 0, 0, 0 };
    call(XOS_ServiceCall, r);
    if (r[1] != 0)
        return ros_configure_error(0);
    if ((int32_t)r[0] < 0)
        return NULL;
    return r[0] <= 3 ? ros_configure_error(r[0]) : ros_ptr(r[0]);
}

static os_error *cmd_status(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    if (argc == 0)
        return list_keywords(1, SERVICE_UKSTATUS, "Configuration status:\n\n",
                             "\nUse *Configure to set the options.\n");
    struct ros_module *x;
    struct entry e;
    uint32_t params;
    if (find_keyword(tail, &x, &e, &params))
        return call_keyword(x, &e, 1);
    uint32_t r[6] = { tail, SERVICE_UKSTATUS, 0, 0, 0, 0 };
    call(XOS_ServiceCall, r);
    return r[1] == 0 ? NULL : err(ERR_SYNTAX, "Status option not recognised");
}

static os_error *cmd_time(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    uint32_t buf = scratch();
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    ros_st8(buf, 0);
    uint32_t r[6] = { 14, buf, 0, 0, 0, 0 };
    os_error *e = call(XOS_Word, r) ? error_in(r) : NULL;
    for (uint32_t p = buf; !e && ros_ld8(p) != 13; p++)
        e = write_c(ros_ld8(p));
    if (!e)
        e = xos_new_line();
    release(buf);
    return e;
}

/* *FX and *TV: up to three numbers, commas optional; then OS_Byte. */
static os_error *fx(uint32_t p, uint32_t args[3], int first)
{
    for (int i = first; i < 4; i++) {
        uint32_t c;
        do
            c = ros_ld8(p++);
        while (c == ' ');
        if (c != ',')
            p--;
        if (is_end(c)) {
            uint32_t r[6] = { args[0], args[1], args[2], 0, 0, 0 };
            return call(XOS_Byte, r) ? error_in(r) : NULL;
        }
        if (i == 3)
            return err(ERR_SYNTAX, "Too many parameters");
        uint32_t r[6] = { 10, p, 0, 0, 0, 0 };
        if (call(XOS_ReadUnsigned, r))
            return error_in(r);
        args[i] = r[2];
        p = r[1];
    }
    return NULL;
}

static os_error *cmd_fx(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t args[3] = { 0, 0, 0 };
    uint32_t r[6] = { 10, tail, 0, 0, 0, 0 };
    if (call(XOS_ReadUnsigned, r))
        return error_in(r);
    args[0] = r[2];
    return fx(r[1], args, 1);
}

static os_error *cmd_tv(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t args[3] = { 144, 0, 0 };
    uint32_t r[6] = { 10, tail, 0, 0, 0, 0 };
    if (call(XOS_ReadUnsigned, r))
        return error_in(r);             /* the kernel's TV reads one at once */
    args[1] = r[2];
    return fx(r[1], args, 2);
}

/* *Key n value: Key$n, a string. */
static os_error *cmd_key(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t r[6] = { 10 | 1u << 29, tail, 15, 0, 0, 0 };
    if (call(XOS_ReadUnsigned, r))
        return err(ERR_BAD_KEY, "Key number must be in the range 0-15");
    uint32_t name = scratch();
    if (!name)
        return err(ERR_LONG_LINE, "Too long");
    snprintf(ros_ptr(name), 8, "Key$%u", r[2]);
    uint32_t s[6] = { name, r[1], 1, 0, 0, 0 };
    os_error *e = call(XOS_SetVarVal, s) ? error_in(s) : NULL;
    release(name);
    return e;
}

/* *WimpSlot.  In the desktop, with Wimp tasks running, it is the Wimp's own
 * (Wimp08's WimpSlot_Code, run as WindowManager:WimpSlot).  The running
 * task's slot is grown to -min or cut to -max through Wimp_SlotSize, and
 * the next slot is set by -next.  A !Run file's *WimpSlot sizes its
 * application in this way.
 *
 * Outside the desktop, Wimp_SlotSize has no task, and the Wimp's command
 * changes nothing. Here the console's task has no application space until
 * a language asks for one. So the size is taken as the next slot's, which
 * is the room that a language entered from the console is given. An Obey
 * file that will run BASIC gives it room first. */
static os_error *cmd_wimpslot(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t tasks[6] = { 0, 0, 0, 0, 0, 0 };  /* Wimp_ReadSysInfo 0: tasks active */
    if (!call(XWimp_ReadSysInfo, tasks) && tasks[0]) {
        uint32_t line = scratch();
        if (!line)
            return err(ERR_LONG_LINE, "Too long");
        static const char prefix[] = "WindowManager:WimpSlot ";
        uint32_t n = sizeof prefix - 1;
        memcpy(ros_ptr(line), prefix, n);
        for (uint32_t q = tail; ros_ld8(q) >= ' ' && n < LONG_CLI - 1; q++)
            ros_st8(line + n++, ros_ld8(q));
        ros_st8(line + n, 13);
        uint32_t r[6] = { line, 0, 0, 0, 0, 0 };
        os_error *e = call(XOS_CLI, r) ? error_in(r) : NULL;
        release(line);
        return e;
    }
    uint32_t p = tail;
    for (;;) {
        while (ros_ld8(p) == ' ')
            p++;
        uint32_t c = ros_ld8(p);
        if (c < ' ')
            return NULL;                    /* a query: nothing to report */
        if (c == '-') {
            while (ros_ld8(p) > ' ')
                p++;
            /* -min and -max take a size of their own: skip it */
            uint32_t q = p;
            while (ros_ld8(q) == ' ')
                q++;
            if (ros_ld8(q) == '-' || ros_ld8(q) < ' ')
                continue;
            p = q;
            continue;
        }
        uint32_t size = 0, digits = 0;
        while ((c = ros_ld8(p)) >= '0' && c <= '9') {
            size = size * 10 + c - '0', p++, digits = 1;
        }
        if (!digits)
            return err(ERR_SYNTAX, "Bad size");
        if (ros_ld8(p) == 'K' || ros_ld8(p) == 'k')
            p++;
        else if (ros_ld8(p) == 'M' || ros_ld8(p) == 'm')
            size <<= 10, p++;
        else if (ros_ld8(p) == 'G' || ros_ld8(p) == 'g')
            size <<= 20, p++;
        else
            size >>= 10;                     /* a bare count is K */
        ros_task_slot_next(size << 10);
        return NULL;
    }
}

/* *BasicAsmCPU [X64|A64|ARM] sets the CPU that BASIC's '[' inline
 * assembler assembles for (modules/basicvfp/asmlib), for this task.  By
 * default it is the CPU the ROM runs on, x86-64 or AArch64, unless a
 * program writes ARM, which is recognised (basicasm.c).  A CPU that is
 * named is used as it is, and ARM programs are not recognised.  With no
 * argument, the default is restored.  Only the CPUs that this build
 * carries can be chosen. */
static os_error *cmd_basicasmcpu(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char word[16];
    unsigned k = 0;
    uint32_t p = tail;
    while (ros_ld8(p) == ' ')
        p++;
    while (ros_ld8(p) != ' ' && ros_ld8(p) != 13 && k < sizeof word - 1) {
        char ch = (char)ros_ld8(p++);
        word[k++] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    }
    word[k] = 0;
    extern void basicasm_set_cpu(int);
    extern void basicasm_default(void);
    if (!word[0])
        basicasm_default();                 /* the machine's: ARM programs recognised */
    else if (!strcmp(word, "ARM"))
        basicasm_set_cpu(0);
#if BASICASM_HAVE_X64
    else if (!strcmp(word, "X64"))
        basicasm_set_cpu(1);
#endif
#if BASICASM_HAVE_A64
    else if (!strcmp(word, "A64"))
        basicasm_set_cpu(2);
#endif
    else
        return err(ERR_SYNTAX, "Syntax: *BasicAsmCPU ["
#if BASICASM_HAVE_X64
                   "X64|"
#endif
#if BASICASM_HAVE_A64
                   "A64|"
#endif
                   "ARM]");
    return NULL;
}

/* *Modules: each incarnation through OS_Module 12, as the kernel lists
 * them.  There is a postfix only where a module has more than one. */
static os_error *cmd_modules(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    os_error *e = write_s("No. Position Workspace Name\n");
    uint32_t module = 0, inc = 0, n = 0;
    while (!e) {
        uint32_t r[6] = { 12, module, inc, 0, 0, 0 };
        if (call(XOS_Module, r))
            break;                      /* no more modules */
        char line[160], num[8] = "   ";
        if (inc == 0)
            snprintf(num, sizeof num, "%3u", ++n);
        const char *title = r[3] ? (const char *)ros_ptr(r[3] + ros_ld32(r[3] + 0x10)) : "";
        int many = inc != 0 || r[2] != 0;
        snprintf(line, sizeof line, "%s %08X %08X  %s%s%s\n", num, r[3], r[4], title,
                 many ? "%" : "", many ? (const char *)ros_ptr(r[5]) : "");
        e = write_s(line);
        module = r[1], inc = r[2];
    }
    return e;
}

/* *NModules: *Modules with each module's kind after its number.  The kind
 * is A for ARM code that the ARM container runs (a RISC OS module loaded
 * from a file, or the built-in SharedCLibrary).  It is N for native code
 * (the ROM's, compiled or native C, or an x32 or A64X32 C module).  This
 * is ROSGD's own command. *Modules keeps RISC OS 5.30's format.
 *
 * It walks the chain itself and does not go through OS_Module 12, so that
 * it can show everything.  An ARM shadow is an unnumbered line straight
 * after its native twin. It is marked A and "(shadow)", or
 * "(shadow, preferred)" when native callers are sent to it too. */
static os_error *nmodules_line(const char *num, const struct ros_module *x, uint32_t node,
                               const char *mark)
{
    int many = node != x->incarnations || ros_ld32(node) != 0;
    char line[200];
    snprintf(line, sizeof line, "%s  %c  %08X %08X  %s%s%s%s\n", num, x->arm ? 'A' : 'N', x->base,
             ros_ld32(node + 4), x->title, many ? "%" : "", many ? (const char *)ros_ptr(node + 8) : "",
             mark);
    return write_s(line);
}

static os_error *cmd_nmodules(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    os_error *e = write_s("No. A/N Position Workspace Name\n");
    unsigned n = 0;
    for (struct ros_module *c = ros_module_first(); c && !e; c = c->next) {
        char num[8];
        snprintf(num, sizeof num, "%3u", ++n);
        for (uint32_t node = c->incarnations; node && !e; node = ros_ld32(node)) {
            e = nmodules_line(num, c, node, "");
            strcpy(num, "   ");
        }
        const struct ros_module *sh = c->shadow;
        for (uint32_t node = sh ? sh->incarnations : 0; node && !e; node = ros_ld32(node))
            e = nmodules_line("   ", sh, node, c->prefer_arm ? " (shadow, preferred)" : " (shadow)");
    }
    return e;
}

/* *ARMPrefer [<title> [on|off]] is an escape hatch, for testing.  When it
 * is on, native callers of the title reach its ARM shadow too, for SWIs,
 * *commands and OS_Module lookups.  The native module stays loaded and
 * keeps its service calls, so turning it off returns to the native module
 * as it was.  The title is remembered. A shadow loaded later, or a native
 * module joining later, is preferred from then on (*NModules shows
 * "(shadow, preferred)").  With no argument, it lists the preferred
 * titles.  It is refused for SharedCLibrary, whose ARM shadow native
 * programs cannot use.  rosgd.armprefer=Title[,Title] does the same at
 * boot (runtime/rom.c). */
static os_error *cmd_armprefer(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    char word[2][40] = { "", "" };
    const char *p = (const char *)ros_ptr(tail);
    for (unsigned w = 0; w < 2; w++) {
        while (*p == ' ')
            p++;
        size_t n = 0;
        while ((uint8_t)*p > ' ' && n < sizeof word[w] - 1)
            word[w][n++] = *p++;
        word[w][n] = 0;
    }
    if (argc == 0) {
        const char *t = ros_module_armpreferred(0);
        if (!t)
            return write_s("No title is ARM preferred.\n");
        os_error *e = write_s("ARM preferred:\n");
        for (unsigned i = 0; !e && (t = ros_module_armpreferred(i)); i++) {
            const struct ros_module *c = ros_module_first();
            while (c && (c->arm || strcasecmp(c->title, t)))
                c = c->next;
            char line[96];
            snprintf(line, sizeof line, "  %s%s\n", t,
                     c && c->shadow ? " (its shadow serves native callers)"
                                    : " (no shadow loaded: native callers get the native module)");
            e = write_s(line);
        }
        return e;
    }
    int on = 1;
    if (word[1][0] && !strcasecmp(word[1], "off"))
        on = 0;
    else if (word[1][0] && strcasecmp(word[1], "on"))
        return ros_error(ERR_SYNTAX, "Syntax: *ARMPrefer [<title> [on|off]]");
    return ros_module_armprefer(word[0], on);
}

/* *SWITrace [<n>], *SWIStats [-all|-reset]: the SWI ring and counts
 * (switrace.h), a line at a time through the VDU. */
static int emit_line(const char *line, void *ctx)
{
    os_error **e = ctx;
    *e = write_s(line);
    if (!*e)
        *e = xos_new_line();
    return *e != NULL;
}

static os_error *cmd_switrace(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    unsigned n = argc ? (unsigned)strtoul((const char *)ros_ptr(tail), NULL, 10) : 40;
    os_error *e = NULL;
    ros_switrace_recent(n ? n : 40, emit_line, &e);
    return e;
}

static os_error *cmd_swistats(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    const char *t = argc ? (const char *)ros_ptr(tail) : "";
    if (!strncasecmp(t, "-reset", 6)) {
        ros_switrace_reset();
        return NULL;
    }
    os_error *e = NULL;
    ros_switrace_stats(strncasecmp(t, "-all", 4) ? 40 : 0, emit_line, &e);
    return e;
}

/* *ARMStats [-reset]: what the ARM container has cost (runtime/armrun/box.c) */
static os_error *cmd_armstats(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    const char *t = argc ? (const char *)ros_ptr(tail) : "";
    os_error *e = NULL;
    ros_armrun_stats(!strncasecmp(t, "-reset", 6), emit_line, &e);
    return e;
}

/* *RMKill, *RMReInit, *RMClear, *RMTidy: OS_Module with the tail. */
static os_error *rm(uint32_t reason, uint32_t tail)
{
    uint32_t r[6] = { reason, tail, 0, 0, 0, 0 };
    return call(XOS_Module, r) ? error_in(r) : NULL;
}

static os_error *cmd_rmkill(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return rm(4, tail);
}

static os_error *cmd_rmreinit(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return rm(3, tail);
}

static os_error *cmd_rmload(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return rm(1, tail);
}

static os_error *cmd_rmrun(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return rm(0, tail);
}

static os_error *cmd_rmclear(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return rm(9, tail);
}

static os_error *cmd_rmtidy(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return rm(8, tail);
}

/* A version as the kernel reads one: "1.23" is &00012300. */
static uint32_t read_version(uint32_t p)
{
    uint32_t whole = 0, frac = 0, c;
    while (ros_ld8(p) == ' ')
        p++;
    while ((c = ros_ld8(p)) >= '0' && c <= '9')
        whole = whole << 4 | (c - '0'), p++;
    if (ros_ld8(p) == '.')
        for (int shift = 12; shift >= 0 && (c = ros_ld8(++p)) >= '0' && c <= '9'; shift -= 4)
            frac |= (c - '0') << shift;
    return whole << 16 | (frac & 0xFFFF);
}

/* *RMEnsure title version [command]: the command, or an error, if the
 * module is missing or older. */
static os_error *cmd_rmensure(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    uint32_t r[6] = { 18, tail, 0, 0, 0, 0 };
    os_error *missing = call(XOS_Module, r) ? error_in(r) : NULL;
    const char *title = NULL;
    if (!missing) {
        uint32_t base = r[3], p = tail;
        while (ros_ld8(p) != ' ')
            p++;
        uint32_t want = read_version(p);
        uint32_t help = base ? ros_ld32(base + 0x14) : 0;
        if (help) {
            /* The module that the lookup found. It is the chain's, or a
             * shadow for an ARM caller (inherited through call). */
            const struct ros_module *found = NULL;
            for (struct ros_module *x = ros_module_first(); x && !found; x = x->next)
                if (x->base == base)
                    found = x;
                else if (x->shadow && x->shadow->base == base)
                    found = x->shadow;
            if (found && ros_module_version(found) >= want)
                return NULL;
            /* A native caller's module is too old, but its shadow is new
             * enough.  This counts as satisfied.  A !Run that RMLoads its
             * own newer ARM copy, which becomes the shadow, can then
             * start its program. */
            if (found && found->shadow && found->shadow->base &&
                ros_module_version(found->shadow) >= want)
                return NULL;
        }
        title = ros_ptr(base + ros_ld32(base + 0x10));
    }
    if (argc == 2)
        return missing ? missing : ros_error(0x10F, "Module %s too old", title);
    uint32_t p = tail;                  /* the command: after the title and version */
    while (ros_ld8(p) != ' ')
        p++;
    while (ros_ld8(p) == ' ')
        p++;
    while (ros_ld8(p) != ' ')
        p++;
    uint32_t c[6] = { p, 0, 0, 0, 0, 0 };
    return call(XOS_CLI, c) ? error_in(c) : NULL;
}

/* *ROMModules: the image's modules, from OS_Module 20. */
static os_error *cmd_rommodules(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    os_error *e = write_s("No. Position    Module Name             Version Status\n");
    for (uint32_t i = 0; !e;) {
        uint32_t r[7] = { 20, i, (uint32_t)-1, 0, 0, 0, 0 };
        struct ros_cpu c;
        ros_cpu_enter(&c);
        memcpy(c.r, r, sizeof r);
        ros_swi(&c, XOS_Module);
        if (c.v)
            break;
        i = c.r[1];
        uint32_t v = c.r[6];
        char version[16], line[160];
        snprintf(version, sizeof version, "%X.%02X", v >> 16, (v >> 8) & 0xFF);
        if (v & 0xFF)
            snprintf(version + strlen(version), 4, "%02X", v & 0xFF);
        snprintf(line, sizeof line, "%3u System ROM  %-24s%-8s%s\n", i,
                 (const char *)ros_ptr(c.r[3]), version,
                 (int32_t)c.r[4] < 0 ? "Unplugged" : c.r[4] ? "Active" : "Dormant");
        e = write_s(line);
    }
    return e;
}

/* OS_PrettyPrint: text in the arena, a dictionary (0 the kernel's), and
 * what 27 0 stands for. */
static os_error *pretty_print(uint32_t text, uint32_t dict, uint32_t special)
{
    uint32_t r[6] = { text, dict, special, 0, 0, 0 };
    return call(XOS_PrettyPrint, r) ? error_in(r) : NULL;
}

/* The kernel's PrintMatch: CR, then the header (the keyword up to its
 * terminator), and a new line. */
static os_error *print_match(const char *keyword)
{
    os_error *e = write_s("\r==> Help on keyword ");
    for (; !e && (uint8_t)*keyword >= ' '; keyword++)
        e = write_c((uint8_t)*keyword);
    return e ? e : xos_new_line();
}

/* Whether keyword k names s: to its end, or abbreviated with "." */
static int help_match(uint32_t k, const char *s)
{
    for (int i = 0;; i++) {
        uint32_t a = ros_ld8(k + (uint32_t)i), b = (uint8_t)s[i];
        if (a <= 32 && b <= 32)
            return 1;
        if (upper(a) == upper(b))
            continue;
        return b > 32 && a == '.';
    }
}

/* A native command's text, pretty printed as a compiled one's is.  The
 * text is copied to the arena, and 27 0 does not end it. The keyword
 * follows it, as the kernel's dictionary for its other tokens. Then a new
 * line is written. */
static os_error *pretty_native(const char *s, const char *keyword)
{
    uint32_t buf = scratch();
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    size_t n = 0, k = strlen(keyword);
    while (s[n] || (n && s[n - 1] == 27))
        n++;
    if (n + k + 2 > LONG_CLI)
        n = k = 0;
    memcpy(ros_ptr(buf), s, n);
    ros_st8(buf + (uint32_t)n, 0);
    memcpy(ros_ptr(buf + (uint32_t)n + 1), keyword, k + 1);
    os_error *e = pretty_print(buf, 0, buf + (uint32_t)n + 1);
    release(buf);
    return e ? e : xos_new_line();
}

/* A help entry that is code (Help_Is_Code_Flag).  It is entered as the
 * kernel's CallHelpKeywordCode enters it: R0 is a buffer of HelpBufferSize
 * (512) bytes, R1 is its size, and R12 is the private word.  What it gives
 * back in R0, if not 0, is pretty printed. Its error is returned. */
static os_error *help_code(const struct ros_module *x, const struct entry *en)
{
    if (en->native || !en->help)
        return NULL;
    uint32_t buf = scratch();
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = buf, s.r[1] = 512, s.r[12] = x->private_word;
    s.r[14] = ROS_RETURN_TO_NATIVE;
    ros_call(&s, ros_addr(en->help));
    if (s.r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(&s, ROS_RETURN_TO_NATIVE);
    os_error *e = NULL;
    if (s.v)
        e = ros_ptr(s.r[0]);
    else if (s.r[0]) {
        e = pretty_print(s.r[0], 0, 0);
        if (!e)
            e = xos_new_line();
    }
    release(buf);
    return e;
}

/* One command's help, as the kernel's ShowHelpInModule gives it.  A
 * compiled International_Help command's help and syntax are looked up in
 * the module's messages and pretty printed with MessageTrans' dictionary.
 * A compiled command's text is pretty printed, and so is a native one's. */
static os_error *show_help(const struct ros_module *x, const struct entry *en)
{
    os_error *e = print_match(en->name);
    if (e)
        return e;
    if (en->info & ROS_CMD_HELP_CODE)
        return help_code(x, en);
    if (en->native) {
        e = pretty_native(en->help, en->name);
        if (!e && en->syntax)
            e = pretty_native(en->syntax, en->name);
        return e;
    }
    uint32_t keyword = ros_addr(en->name);
    if (!(en->info & ROS_CMD_INTL_HELP)) {
        e = pretty_print(ros_addr(en->help), 0, keyword);
        return e ? e : xos_new_line();
    }
    uint32_t desc = scratch(), dict;
    if (!desc)
        return err(ERR_LONG_LINE, "Too long");
    int open = open_module_messages(x, desc);
    const char *texts[2] = { en->help, en->syntax };
    for (int i = 0; i < 2 && !e && texts[i]; i++) {
        uint32_t msg = intl_message(open, desc, ros_addr(texts[i]), &dict);
        e = pretty_print(msg, dict, keyword);
        if (!e)
            e = xos_new_line();
    }
    close_module_messages(open, desc);
    release(desc);
    return e;
}

#define HELP_BUFFER 512u            /* the kernel's HelpBufferSize */

/* The kernel's OneModuleK: a module's entries of one kind, their title
 * first, then their names a tab apart.  They are pretty printed, so they
 * come out in columns.  The kinds are commands (flags 0), filing system
 * commands, *Configure keywords, and (help only) the entries with help and
 * no code. */
static os_error *module_keywords(const struct ros_module *x, uint32_t kind, int help_only,
                                 const char *title)
{
    uint32_t buf = scratch(), used = 0, cursor = 0;
    if (!buf)
        return err(ERR_LONG_LINE, "Too long");
    os_error *e = NULL;
    struct entry en;
    int first = 1;
    while (!e && entry_at(x, &cursor, &en)) {
        int has_code = en.exec || (en.native && en.native->run);
        if (help_only ? has_code
                      : !has_code || (en.info & 0xCF000000u) != kind)
            continue;
        if (first) {
            first = 0;
            e = xos_new_line();
            if (!e)
                e = xos_new_line();
            if (!e)
                e = write_s(title);
        }
        size_t n = strlen(en.name);
        if (!e && used && HELP_BUFFER - used < n + 1) {
            ros_st8(buf + used - 1, 0);
            e = pretty_print(buf, 0, 0);
            if (!e)
                e = xos_new_line();
            used = 0;
        }
        if (e || n + 1 > HELP_BUFFER)
            continue;
        memcpy(ros_ptr(buf + used), en.name, n);
        ros_st8(buf + used + (uint32_t)n, 9);
        used += (uint32_t)n + 1;
    }
    if (!e && used) {
        ros_st8(buf + used - 1, 0);
        e = pretty_print(buf, 0, 0);
    }
    release(buf);
    return e;
}

/* The kernel's ModuleJackanory: a module's title and help string, from
 * its header (a native module's is built in the RMA), and its keywords.
 * "Module is:" is printed only if the header has a help string. */
static os_error *module_help(const struct ros_module *x)
{
    uint32_t title = x->base ? ros_ld32(x->base + 0x10) : 0;
    uint32_t help = x->base ? ros_ld32(x->base + 0x14) : 0;
    os_error *e = print_match(title ? (const char *)ros_ptr(x->base + title) : x->title);
    if (!e && (help || (!x->base && x->help))) {
        e = write_s("Module is: ");
        if (!e)
            e = help ? pretty_print(x->base + help, 0, 0) : pretty(x->help, "");
    }
    if (!e)
        e = module_keywords(x, 0, 0, "Commands provided:\n");
    if (!e)
        e = module_keywords(x, ROS_CMD_FS, 0, "Filing system commands:\n");
    if (!e)
        e = module_keywords(x, ROS_CMD_CONFIGURE, 0, "Configuration keywords:\n");
    if (!e)
        e = module_keywords(x, 0, 1, "It has help on:\n");
    return e ? e : xos_new_line();
}

/* *Help.  Service_Help comes first, which a module may claim.  Then each
 * keyword is matched against each module's commands, and then against the
 * modules' titles, once for each incarnation (the kernel's Help_Code, in
 * SysComms).  Paged mode is on while it prints, as there.  The next
 * keyword starts after a space, or straight after an abbreviation's "."
 * ("*Help Desk.WindowU."). */
static os_error *cmd_help(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    int kind = ros_caller_kind();       /* each module as this caller reaches it */
    os_error *e = NULL;
    int found = 0;
    uint32_t sv[6] = { tail, 0x09u, argc, 0, 0, 0 };                /* Service_Help */
    call(XOS_ServiceCall, sv);
    if (sv[1] == 0)
        return NULL;
    if (argc == 0)
        return write_s("*Help <subjects> attempts to give useful information on the selected "
                       "topics.\n");
    uint32_t b[6] = { 117, 0, 0, 0, 0, 0 };
    call(XOS_Byte, b);
    e = write_c(14);
    uint32_t p = tail;
    while (ros_ld8(p) == ' ')
        p++;
    while (!e && ros_ld8(p) >= ' ') {
        for (struct ros_module *c = ros_module_first(); c && !e; c = c->next) {
            const struct ros_module *x = ros_module_route_existing(c, kind);
            struct entry en;
            uint32_t cursor = 0;
            while (!e && entry_at(x, &cursor, &en)) {
                if (!en.help || !help_match(p, en.name))
                    continue;
                found = 1;
                e = show_help(x, &en);
            }
        }
        for (struct ros_module *c = ros_module_first(); c && !e; c = c->next) {
            const struct ros_module *x = ros_module_route_existing(c, kind);
            uint32_t title = x->base ? ros_ld32(x->base + 0x10) : 0;
            const char *name = title ? (const char *)ros_ptr(x->base + title) : x->title;
            if (!name || !help_match(p, name))
                continue;
            found = 1;
            uint32_t node = x->incarnations;
            do
                e = module_help(x);
            while (!e && node && (node = ros_ld32(node)) != 0);
        }
        uint32_t c;
        while ((c = ros_ld8(p)) > ' ' && c != '.')
            p++;
        if (c == '.')
            p++;
        while (ros_ld8(p) == ' ')
            p++;
    }
    if (!e && !found)
        e = write_s("No help found.\n");
    if (!(b[1] & 5)) {
        os_error *off = write_c(15);
        if (!e)
            e = off;
    }
    return e;
}

/* The kernel's file commands (runtime/filecmds.c) */
os_error *ros_cmd_append(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_build(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_close(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_exec(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_spool(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_spoolon(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_create(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_delete(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_dump(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_list(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_load(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_opt(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_print(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_remove(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_save(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_type(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *ros_cmd_obey(struct ros_module *m, uint32_t tail, uint32_t argc);

/* The kernel's help and syntax (Kernel/HelpStrs), 27 0 standing for the
 * command's name. */
#define N "\x1B\x00"
static const struct ros_command utility_commands[] = {
    { "FX", ROS_CMD_INFO(1, 5, 0, 0), N " needs 1 to 3 numeric parameters.",
      "*" N " r0 [[,] r1 [[,] r2]] calls OS_Byte.", cmd_fx },
    { "Help", ROS_CMD_INFO(0, 255, 0, 0), NULL,
      "*" N " <subjects> attempts to give useful information on the selected topics.",
      cmd_help },
    { "Key", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *" N " <keynumber> [<value>]",
      "*" N " sets the function keys.", cmd_key },
    { "TV", ROS_CMD_INFO(0, 3, 0, 0), "*" N " [<vertical position> [[,] <interlace>]]",
      "*" N " controls interlacing and sets the position of the display on the screen.",
      cmd_tv },
    { "BasicAsmCPU", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [X64|A64|ARM]",
      "*" N " selects the CPU BASIC's inline assembler assembles, for this task: x86-64, "
      "AArch64 or ARM. It starts as the CPU the machine is; with no argument, it goes back "
      "to it.", cmd_basicasmcpu },
    { "WimpSlot", ROS_CMD_INFO(0, 255, 0, 0),
      "Syntax: *" N " [[-min] <size>[K|M|G]] [[-max] <size>[K|M|G]] [[-next] <size>[K|M|G]]",
      "Change the size of application space, or the amount of application space allocated "
      "to the next task to run.",
      cmd_wimpslot },
    { "Configure", ROS_CMD_INFO(0, 255, 0, 0), "Syntax: *" N " [<keyword> [<value>]]",
      "*" N " sets the configuration held in CMOS RAM: with no keyword, it lists "
      "the keywords.", cmd_configure },
    { "Echo", ROS_CMD_INFO(0, 255, 0, 0), "Syntax: *" N " <string>",
      "*" N " sends a string to the VDU, after transformation by GSRead.", cmd_echo },
    { "Error", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *" N " [<number>] <text>",
      "*" N " generates an error with the given number and text.", cmd_error },
    { "Eval", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *" N " <expression>",
      "*" N " evaluates an integer or string expression.", cmd_eval },
    { "IF", ROS_CMD_INFO(2, 255, 0, 0),
      "Syntax: *" N " <expression> THEN <command> [ELSE <command>]",
      "*" N " conditionally executes another command depending on the value of an "
      "expression.", cmd_if },
    { "Modules", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *" N,
      "*" N " lists the modules.", cmd_modules },
    { "NModules", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *" N,
      "*" N " lists the modules as *Modules does, with each one's kind after its number: A, ARM "
      "code the ARM container runs, or N, native.", cmd_nmodules },
    { "ARMPrefer", ROS_CMD_INFO(0, 2, 0, 0), "Syntax: *" N " [<title> [on|off]]",
      "*" N " sends native callers of a module to its ARM shadow too, for testing (on), or "
      "back to the native module (off). With no argument it lists the titles so preferred.",
      cmd_armprefer },
    { "RMClear", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *" N,
      "*" N " deletes all relocatable modules from the RMA.", cmd_rmclear },
    { "RMEnsure", ROS_CMD_INFO(2, 255, 0, 0),
      "Syntax: *" N " <moduletitle> <version number> [<*command>]",
      "*" N " checks that a module is present and is the given version, or a more modern "
      "one. The command is executed if this is not the case.", cmd_rmensure },
    { "RMKill", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *" N " <moduletitle>",
      "*" N " kills and deletes a relocatable module.", cmd_rmkill },
    { "RMLoad", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *" N " <filename>",
      "*" N " loads and initialises a relocatable module.", cmd_rmload },
    { "RMReInit", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *" N " <moduletitle>",
      "*" N " reinitialises a relocatable module.", cmd_rmreinit },
    { "RMRun", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *" N " <filename>",
      "*" N " runs a relocatable module.", cmd_rmrun },
    { "RMTidy", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *" N,
      "*" N " compacts the RMA and reinitialises all the modules.", cmd_rmtidy },
    { "ROMModules", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *" N,
      "*" N " lists the relocatable modules currently in ROM, along with their status.",
      cmd_rommodules },
    { "Set", ROS_CMD_INFO(2, 255, 0, 0), "Syntax: *" N " <varname> <value>",
      "*" N " assigns a string value to a system variable. Other types of value can be "
      "assigned with *" N "Eval and *" N "Macro.", cmd_set },
    { "SetEval", ROS_CMD_INFO(2, 255, 0, 0), "Syntax: *" N " <varname> <expression>",
      "*" N " evaluates an expression and assigns it to a system variable.", cmd_seteval },
    { "SetMacro", ROS_CMD_INFO(2, 255, 0, 0), "Syntax: *" N " <varname> <value>",
      "*" N " assigns a macro value to a system variable.", cmd_setmacro },
    { "Show", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [<variablespec>]",
      "*" N " lists system variables matching the name given, or all system variables if "
      "no name is specified.", cmd_show },
    { "Status", ROS_CMD_INFO(0, 255, 0, 0), "Syntax: *" N " [<keyword>]",
      "*" N " shows the configuration held in CMOS RAM.", cmd_status },
    { "SWIStats", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [-all|-reset]",
      "*" N " lists the SWIs called since the start, or since *" N " -reset, by the time "
      "spent in them: calls, errors, total and self time, mean and longest call. -all "
      "lists every SWI, not the top 40.", cmd_swistats },
    { "ARMStats", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [-reset]",
      "*" N " gives what the ARM container has cost since the start, or since *" N " -reset: "
      "the time in the engine, in native code it called and in the FPA, instructions, SWIs, "
      "and where the ARM code's time went, from samples of the PC.", cmd_armstats },
    { "SWITrace", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [<n>]",
      "*" N " lists the last n SWIs called (default 40), oldest first: when, how long, the "
      "task, R0-R3 and the result.", cmd_switrace },
    { "Time", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *" N, "*" N " displays the time and date.",
      cmd_time },
    { "Unset", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *" N " <varname>",
      "*" N " deletes a system variable.", cmd_unset },
    { "Append", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *" N " <filename>",
      "*" N " opens an existing file and subsequent lines of keyboard input are appended "
      "to it, input being terminated by ESCAPE.", ros_cmd_append },
    { "Build", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *" N " <filename>",
      "*" N " opens a new file and subsequent lines of keyboard input are directed to it, "
      "input being terminated by ESCAPE.", ros_cmd_build },
    { "Close", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *" N,
      "*" N " closes all files on the current filing system.", ros_cmd_close },
    { "Create", ROS_CMD_INFO(1, 4, 0, 0),
      "Syntax: *" N " <filename> [<length> [<exec addr> [<load addr>]]]",
      "*" N " reserves space for the named file, optionally giving it load and execution "
      "addresses. No data is transferred to the file. Length and addresses are in hex.",
      ros_cmd_create },
    { "Delete", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *" N " <object>",
      "*" N " tries to delete the object, and gives an error if it does not exist.",
      ros_cmd_delete },
    { "Exec", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [<filename>]",
      "*" N " <filename> directs the operating system to take its input from the file. *" N
      " alone closes the exec file.", ros_cmd_exec },
    { "Dump", ROS_CMD_INFO(1, 3, 0, 0),
      "Syntax: *" N " <filename> [<file offset> [<start address>]]",
      "*" N " displays the contents of the file as a hex and ASCII dump.", ros_cmd_dump },
    { "Load", ROS_CMD_INFO(1, 2, 0, 0), "Syntax: *" N " <filename> [<load addr>]",
      "*" N " with no specified address loads the named file at its own load address. "
      "Otherwise it loads the file at the given address (in hex).", ros_cmd_load },
    { "List", ROS_CMD_INFO(1, 3, 0, 0), "Syntax: *" N " [-File] <filename> [-TabExpand]",
      "*" N " displays the contents of the file in the configured GSRead format, each line "
      "preceded by a line number.", ros_cmd_list },
    { "Opt", ROS_CMD_INFO(0, 2, 0, 0), "Syntax: *" N " [<x> [[,] <y>]]",
      "*" N " controls various filing system actions.", ros_cmd_opt },
    { "Print", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *" N " <filename>",
      "*" N " displays the contents of a file by sending each byte to the VDU.",
      ros_cmd_print },
    { "Remove", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *" N " <object>",
      "*" N " tries to delete the object, and gives no error if it does not exist.",
      ros_cmd_remove },
    { "Save", ROS_CMD_INFO(2, 6, 0, 0),
      "Syntax: *" N " <filename> <start addr> <end addr>|+<length> [<exec addr> "
      "[<load addr>]]",
      "*" N " copies the given area of memory to the named file. Addresses are in hex.",
      ros_cmd_save },
    { "Spool", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [<filename>]",
      "*" N " <filename> opens a new file and causes subsequent VDU output to be sent to it. "
      "*" N " alone closes the spool file.", ros_cmd_spool },
    { "SpoolOn", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *" N " [<filename>]",
      "*" N " <filename> opens an existing file and causes subsequent VDU output to be "
      "appended to it. *Spool alone closes the spool file.", ros_cmd_spoolon },
    { "Type", ROS_CMD_INFO(1, 3, 0, 0), "Syntax: *" N " [-File] <filename> [-TabExpand]",
      "*" N " displays the contents of the file in the configured GSRead format.",
      ros_cmd_type },
    { "Obey", ROS_CMD_INFO(0, 255, 0, 0),
      "Syntax: *" N " [[-v][-c][-m] [<filename> [<parameters>]]]",
      "*" N " executes a file of *commands, performing argument substitution on each line. "
      "Prefixing the filename with -v causes each line to be echoed before execution, -c "
      "causes the file to be cached and executed from memory, -m causes the lines to be read "
      "from the specified memory address", ros_cmd_obey },
    { NULL, 0, NULL, NULL, NULL },
};

/* The kernel's SHC_fudgeulike: FX and its kind, a number straight after. */
static const struct ros_command fudge_table[] = {
    { "FX", ROS_CMD_INFO(1, 5, 0, 0), N " needs 1 to 3 numeric parameters.", NULL, cmd_fx },
    { "Key", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *" N " <keynumber> [<value>]", NULL,
      cmd_key },
    { "TV", ROS_CMD_INFO(0, 3, 0, 0), "*" N " [<vertical position> [[,] <interlace>]]", NULL,
      cmd_tv },
    { NULL, 0, NULL, NULL, NULL },
};
#undef N

/* ---- OS_CLI ------------------------------------------------------------------------ */

/* "{ > file }" and its kind, outside quotes: the kernel's ParseRDNSpec.
 * The position of the "{", or 0; *end past the "}". */
static uint32_t redirection(uint32_t p, uint32_t *end)
{
    int quoted = 0;
    for (uint32_t c; !is_end(c = ros_ld8(p)); p++) {
        if (c == '"')
            quoted = !quoted;
        if (c != '{' || quoted || ros_ld8(p + 1) != ' ')
            continue;
        uint32_t q = p + 1;
        int specs = 0;
        for (;;) {
            while (ros_ld8(q) == ' ')
                q++;
            c = ros_ld8(q);
            if (c != '<' && c != '>')
                break;
            q++;
            if (c == '>' && ros_ld8(q) == '>')
                q++;
            if (ros_ld8(q++) != ' ')
                break;
            while (ros_ld8(q) == ' ')
                q++;
            while (ros_ld8(q) > ' ')
                q++;
            if (ros_ld8(q) != ' ')
                break;
            specs++;
        }
        if (specs && ros_ld8(q) == '}') {
            *end = q + 1;
            return p;
        }
    }
    return 0;
}

/* Set up each redirection between the braces (the kernel's doredirect) */
static os_error *redirect(uint32_t brace)
{
    for (uint32_t q = brace + 1;;) {
        while (ros_ld8(q) == ' ')
            q++;
        uint32_t c = ros_ld8(q), mode = 0x40;
        if (c != '<' && c != '>')
            return NULL;
        q++;
        if (c == '>') {
            mode = 0x80;
            if (ros_ld8(q) == '>')
                mode = 0xC0, q++;
        }
        while (ros_ld8(q) == ' ')
            q++;
        os_error *e = ros_redirect(mode, q);
        if (e)
            return e;
        while (ros_ld8(q) > ' ')
            q++;
    }
}

/* "Title:command": the module named, and the command after it. */
static struct ros_module *module_prefix(uint32_t *cmd)
{
    uint32_t p = *cmd;
    const char *hash = "MODULE#";
    uint32_t i = 0;
    while (hash[i] && upper(ros_ld8(p + i)) == (uint8_t)hash[i])
        i++;
    if (!hash[i])
        p += i;
    char title[64];
    unsigned n = 0;
    for (uint32_t c;; p++) {
        c = ros_ld8(p);
        if (c == '.' || c <= ' ' || n == sizeof title - 1)
            return NULL;
        if (c == ':')
            break;
        title[n++] = (char)c;
    }
    title[n] = 0;
    for (struct ros_module *m = ros_module_first(); m; m = m->next) {
        const char *t = m->title;
        unsigned k = 0;
        while (k < n && t[k] && upper((uint8_t)t[k]) == upper((uint8_t)title[k]))
            k++;
        if (k == n && !t[k]) {
            *cmd = p + 1;
            return ros_module_route_existing(m, ros_caller_kind());
        }
    }
    return NULL;
}

/* The kernel's fudge, as in "*FX0" and "*Key1": a number within the first
 * few characters after the name's start. */
static int fudged(uint32_t cmd)
{
    for (uint32_t i = 2; i <= 4; i++) {
        uint32_t c = ros_ld8(cmd + i);
        if (c <= ' ')
            return 0;
        if (c == '&' || (c >= '0' && c <= '9'))
            return 1;
    }
    return 0;
}

static int abbreviated(uint32_t cmd)
{
    for (uint32_t c; (c = up_it(ros_ld8(cmd))) != 0; cmd++)
        if (c == '.')
            return 1;
    return 0;
}

static os_error *command(uint32_t cmd);
static os_error *unredirected(uint32_t start);

static os_error *cli(uint32_t line)
{
    uint32_t p = line, c;
    do
        c = ros_ld8(p++);
    while (c == ' ' || c == '*');
    if (c == '%')
        c = ros_ld8(p);
    if (is_end(c) || c == '|')
        return NULL;
    uint32_t start = p - 1;
    for (uint32_t n = 0; !is_end(ros_ld8(start + n)); n++)
        if (n + 1 >= LONG_CLI)
            return err(ERR_LONG_LINE, "Too long");
    /* Redirection.  The files are opened, the command is run without the
     * braces, and the redirection is undone after it (the kernel's
     * OscliTidy). */
    uint32_t end, brace = redirection(start, &end);
    if (brace) {
        uint32_t line2 = scratch();
        if (!line2)
            return err(ERR_LONG_LINE, "Too long");
        os_error *e = redirect(brace);
        if (!e) {
            uint32_t n = 0;
            for (uint32_t q = start; q < brace; q++)
                ros_st8(line2 + n++, ros_ld8(q));
            while (ros_ld8(end) == ' ')
                end++;
            for (uint32_t q = end; !is_end(ros_ld8(q)) && n < LONG_CLI; q++)
                ros_st8(line2 + n++, ros_ld8(q));
            ros_st8(line2 + n, 13);
            e = unredirected(line2);
        }
        ros_redirect_tidy();
        release(line2);
        return e;
    }
    return unredirected(start);
}

/* A line after its redirection: a filing system prefix, then the command. */
static os_error *unredirected(uint32_t start)
{
    /* A filing system as a prefix, "fs:" or "-fs-", is the temporary one
     * while the rest runs (OS_FSControl 3, then 19).  With a special
     * field, the whole line is a file to run. */
    uint32_t r[6] = { FSCONTROL_STAR_MINUS, start, 0, 0, 0, 0 };
    if (call(XOS_FSControl, r) || r[2] == (uint32_t)-1)
        return command(start);
    os_error *e = r[3] ? fs_control(FSCONTROL_RUN, start) : command(r[1]);
    uint32_t q[6] = { FSCONTROL_RESTORE_CURRENT, 0, 0, 0, 0, 0 };
    call(XOS_FSControl, q);
    return e;
}

/* The command after any prefix: module, "/", aliases, tables, the
 * service, and then FileSwitch to run it as a file. */
static os_error *command(uint32_t cmd)
{
    uint32_t c;
    struct ros_module *only = module_prefix(&cmd);
    while (ros_ld8(cmd) == ' ')
        cmd++;
    c = ros_ld8(cmd);
    os_error *x = NULL;
    if (only) {
        if (in_module(only, cmd, 0, 0, NULL, &x))
            return x;
        return err(ERR_BAD_COMMAND, "Command not recognised");
    }
    if (c == '/')
        return fs_control(FSCONTROL_RUN, cmd + 1);

    uint32_t name, len;
    if (c == '%') {
        cmd++;
    } else if (c == '.') {
        uint32_t dot = scratch();
        if (!dot)
            return err(ERR_LONG_LINE, "Too long");
        strcpy(ros_ptr(dot), "Alias$.");
        uint32_t r[6] = { dot, 0, (uint32_t)-1, 0, 0, 0 };
        call(XOS_ReadVarVal, r);
        release(dot);
        if (r[2] != 0)
            return run_alias(r[3], cmd + 1);
    } else if (find_alias(cmd, &name, &len)) {
        return run_alias(name, cmd + len);
    }

    if (ros_ld8(cmd) == '.')
        return fs_control(FSCONTROL_CAT, cmd + 1);

    if (!abbreviated(cmd) && fudged(cmd) &&
        in_module(&ros_utility_module, cmd, 0, 1, fudge_table, &x))
        return x;
    /* Each module is taken as this line's kind reaches it.  An ARM caller
     * reaches a shadow. A native caller reaches the chain's own module,
     * and never a shadow's own commands. */
    int kind = ros_caller_kind();
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (in_module(ros_module_route_existing(m, kind), cmd, 0, 0, NULL, &x))
            return x;

    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = cmd, s.r[1] = SERVICE_UKCOMMAND;
    ros_service_call(&s);
    if (s.r[1] == 0)
        return s.r[0] ? (os_error *)ros_ptr(s.r[0]) : NULL;
    return fs_control(FSCONTROL_RUN, cmd);
}

static void default_cli(struct ros_cpu *s)
{
    os_error *e = cli(s->r[0]);
    if (e)
        ros_swi_fail(s, e);
}

void ros_thunk_OS_CLI(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2];
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    s->v = 0;
    if (!ros_vector_call(CLIV, s))
        default_cli(s);
    ros_svc_sp = outer_sp;
    if (!s->v)
        s->r[0] = r0;
    s->r[1] = r1, s->r[2] = r2;
    s->r[10] = r10, s->r[11] = r11, s->r[12] = r12;
}

/* ---- OS_SubstituteArgs ------------------------------------------------------------------ */

/* The kernel's SubstituteArgs32.  %0-%9 are the arguments, %*n is the rest
 * of the line from n, and %% is a %.  Arguments not used are appended
 * unless R5's top bit says not.  R2 returns the number of characters out,
 * counting the terminator. */
void ros_thunk_OS_SubstituteArgs32(struct ros_cpu *s)
{
    uint32_t p = s->r[0], out = s->r[1], size = s->r[2], in = s->r[3];
    uint32_t in_end = in + s->r[4], flags = s->r[5];
    uint32_t start[11], end[11];
    for (int i = 0; i < 11; i++) {
        uint32_t c;
        do
            c = ros_ld8(p++);
        while (c == ' ');
        int delim = c == '"' ? '"' : ' ';
        if (i == 10)
            delim = -1;                 /* the rest of the line */
        p--;
        start[i] = p;
        if (c == '"')
            p++;
        for (;;) {
            c = ros_ld8(p++);
            if (is_end(c) || (int)c == delim) {
                if (c == '"' && ros_ld8(p) == '"') {
                    p++;
                    continue;           /* "" inside quotes */
                }
                break;
            }
        }
        if (delim != '"')
            p--;
        end[i] = p;
    }

    uint32_t n = 0, highest = 0, c;
    int full = 0;
#define ADD(ch)                                                             \
    do {                                                                    \
        if (++n == size) {                                                  \
            full = 1;                                                       \
            goto done;                                                      \
        }                                                                   \
        ros_st8(out++, (ch));                                               \
    } while (0)
    while (in != in_end) {
        c = ros_ld8(in++);
        if (c != '%') {
            ADD(c);
            continue;
        }
        if (in == in_end) {
            ADD('%');
            break;
        }
        c = ros_ld8(in++);
        if (c == '%') {
            ADD('%');
        } else if (c == '*') {
            if (in == in_end) {
                ADD('%');
                ADD('*');
                break;
            }
            c = ros_ld8(in++);
            if (c >= '0' && c <= '9') {
                highest = 11;
                for (uint32_t q = start[c - '0']; q < end[10]; q++)
                    ADD(ros_ld8(q));
            } else {
                ADD('%');
                ADD('*');
                ADD(c);
            }
        } else if (c >= '0' && c <= '9') {
            uint32_t k = c - '0';
            if (k + 1 > highest)
                highest = k + 1;
            for (uint32_t q = start[k]; q < end[k]; q++)
                ADD(ros_ld8(q));
        } else {
            ADD('%');
            ADD(c);
        }
    }
    {
        uint32_t q = highest == 11 || (flags & 0x80000000u) ? end[10] : start[highest];
        do {
            c = ros_ld8(q++);
            ADD(c);
        } while (!is_end(c));
    }
done:
#undef ADD
    if (full) {
        ros_swi_fail(s, err(ERR_BUFF_OVERFLOW, "Buffer overflow"));
        return;
    }
    s->r[2] = n;
    s->v = 0;
}

void ros_thunk_OS_SubstituteArgs(struct ros_cpu *s)
{
    uint32_t r5 = s->r[5];
    s->r[5] = 0;
    ros_thunk_OS_SubstituteArgs32(s);
    s->r[5] = r5;
}

/* The kernel's own module: first in the chain, as the kernel keeps it.
 * Its help is the kernel's "MOS Utilities", a tab, and $VersionNo.  That
 * is the OS's version and date, as OS_Byte 0 gives them.  So a !Run's
 * "RMEnsure UtilityModule 3.10" (the OS itself) passes as it does on
 * RISC OS 5.30. */
struct ros_module ros_utility_module = {
    .title = "UtilityModule",
    .help = "MOS Utilities\t5.30 (15 May 2024) ROSGD native",
    .commands = utility_commands,
};
