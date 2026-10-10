/* Copyright (c) 2014, RISC OS Open Ltd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of RISC OS Open Ltd nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is a reimplementation in C of RISC OS Open's MimeMap module
 * (Sources/Networking/MimeMap: c.mime, h.mime, hdr.MimeMap, cmhg.MimeMapHdr).
 */

/* mimemap.c -- MimeMap, a native module. It is RISC OS's MimeMap 0.19
 * (RISC OS Open's Networking/MimeMap, c/mime, which is 5.30's ROM module),
 * rewritten for ROSGD (#106).
 *
 * Its one SWI, MimeMap_Translate (&50B00), converts between RISC OS file
 * types (as a number or a name), MIME content types and file extensions.
 * It uses the MIME mappings file, <Inet$MimeMappings>. *MimeMap shows a
 * mapping. *ReadMimeMap reads the file again when it has changed.
 *
 * What a caller sees is c/mime's behaviour.
 * - The file is read line by line, as c/mime's fgets() and strtok() read
 *   it. A line is read in chunks of at most MAX_LINE - 1 (255) bytes, and
 *   has at most MAX_EXTS (16) extensions.
 * - The first entry that matches wins.
 * - Each output format has its own checks. A MIME answer skips wildcarded
 *   entries. An extension answer skips entries with no extensions. A file
 *   type answer skips entries with "*" or with a type that FileSwitch does
 *   not know.
 * - The wildcard file type ("*", application/riscos) answers for any type
 *   that comes after it.
 * - The errors are &B00000 Cannot open MIME mapping file, &B00001
 *   Unrecognised parameter and &B00002 No MIME mapping found. Their texts
 *   come from the module's Messages file. The ROM carries that file as
 *   5.30's does (Resources:$.Resources.MimeMap.Messages).
 * - The output formats it has no answer for (MMM_TYPE_MAC, and anything
 *   above 5) write nothing and give no error, as c/mime's switch does.
 *
 * The box differs in one way. 5.30's ROM module finds no file until !Boot
 * sets Inet$MimeMappings (Utils.BootRun) and !Internet sets InetDBase$Path.
 * The box has no !Boot to do this. So the ROM carries the file, as
 * Resources:$.Resources.Internet.files.MimeMap. It is RISC OS 5.30's own
 * file, with the extensions that FileSwitch's typemap.txt knows and it does
 * not added after it. The module sets
 * InetDBase$Path to that directory and Inet$MimeMappings to
 * InetDBase:MimeMap as it starts. It sets each one only if nothing has set
 * it already. A !Boot that sets them later is obeyed.
 *
 * All the state is in the RMA (see rosgd/module.h). There is the
 * workspace, and there is the table. The table is one block. Its entries,
 * their extension arrays and their strings are all at arena addresses. So
 * MMM_TYPE_DOT_EXTNS hands out an array of pointers that a RISC OS program
 * can read, as c/mime hands out its own. Reading the file again frees the
 * old table. Pointers handed out from it become invalid with it, as on
 * RISC OS. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mimemap.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define MAX_LINE  256u              /* h/mime: fgets()'s buffer */
#define MAX_EXTS  16u               /* h/mime: the most extensions on a line */

#define FILETYPE_WILDCARDED 0xFFFFFFFFu  /* "*" */
#define FILETYPE_INVALID    0xFFFFFFFEu  /* a type FileSwitch does not know */

/* The checks made by mime_flags_ok(), chosen by the output format */
#define HAS_NO_CHECKS   0u
#define HAS_VALID_RISCOS 1u
#define HAS_VALID_MIME   2u
#define HAS_VALID_DOT_EXTN 4u

#define ERR_BASE          0xB00000u /* h/mime's ErrorBase_ANTMimeMap */
#define ERR_BAD_FILE      0u
#define ERR_BAD_REASON    1u
#define ERR_LOOKUP_FAILED 2u

#define FSCONTROL_READ_FILE_TYPE        18u
#define FSCONTROL_FILE_TYPE_FROM_STRING 31u
#define OSFILE_READ_INFO                5u
#define OSFILE_LOAD                     255u

static const char messages_file[] = "Resources:$.Resources.MimeMap.Messages";
static const char mapping_file[] = "<Inet$MimeMappings>";

/* ---- the state, all in the RMA -------------------------------------------- */

/* An entry of the table. The fields are arena addresses, as the RMA block
 * holds them. */
struct entry {
    uint32_t major, minor;          /* the MIME type's two halves */
    uint32_t filetype;              /* or FILETYPE_WILDCARDED or FILETYPE_INVALID */
    uint32_t num_exts;
    uint32_t exts;                  /* -> num_exts string addresses; 0 if none */
};

struct table {
    uint32_t count;
    struct entry e[];
};

struct workspace {
    uint32_t table;                 /* the table's block; 0 if none is read */
    uint32_t last_load, last_exec;  /* the file's load and exec addresses when last read */
    uint32_t desc[4];               /* the Messages file's descriptor */
    char file[sizeof messages_file];
    uint32_t errblk[3];             /* an error's number and token */
    char scratch[MAX_LINE + 4];     /* strings handed to SWIs */
    char text[48];                  /* intl_lookup()'s result */
};

static struct workspace *ws(void)
{
    uint32_t pw = mimemap_module.private_word, w = pw ? ros_ld32(pw) : 0;
    return w ? ros_ptr(w) : NULL;
}

static struct table *table(struct workspace *w)
{
    return w->table ? ros_ptr(w->table) : NULL;
}

static const char *str(uint32_t a)
{
    return ros_ptr(a);
}

/* Calls a SWI, in its X form, with registers r[0..9]. */
static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* Copies s into the workspace's scratch buffer, for a SWI, and returns its
 * address. */
static uint32_t scratch(struct workspace *w, const char *s)
{
    snprintf(w->scratch, sizeof w->scratch, "%s", s);
    return ros_addr(w->scratch);
}

/* ---- International support (c/mime's) -------------------------------------- */

/* Returns a token's text, or "???" if it cannot be had. */
static const char *intl_lookup(struct workspace *w, const char *token)
{
    uint32_t r[10] = { ros_addr(w->desc), scratch(w, token), ros_addr(w->text), sizeof w->text };
    if (swi(XMessageTrans_Lookup, r))
        return "???";
    size_t n = r[3] < sizeof w->text ? r[3] : sizeof w->text - 1;
    w->text[n] = 0;
    return w->text;
}

/* Returns the error with token "E<which>" and number ErrorBase_ANTMimeMap +
 * which. MessageTrans builds it in its own buffer. */
static os_error *intl_error(struct workspace *w, uint32_t which)
{
    w->errblk[0] = ERR_BASE + which;
    snprintf((char *)&w->errblk[1], 8, "E%02x", which);
    uint32_t r[10] = { ros_addr(w->errblk), ros_addr(w->desc) };
    os_error *e = swi(XMessageTrans_ErrorLookup, r);
    return e ? e : ros_error(ERR_BASE + which, "MimeMap error %u", which);
}

/* ---- the file ------------------------------------------------------------------ */

static void free_table(struct workspace *w)
{
    if (w->table)
        ros_rma_free(ros_ptr(w->table));
    w->table = 0;
}

/* OS_FSControl 31: gets a file type from a string (its name, or a hex
 * number). */
static int type_from_string(struct workspace *w, const char *s, uint32_t *type)
{
    uint32_t r[10] = { FSCONTROL_FILE_TYPE_FROM_STRING, scratch(w, s) };
    if (swi(XOS_FSControl, r))
        return 0;
    *type = r[2];
    return 1;
}

/* A line as c/mime reads it, before it is packed into the RMA. */
struct line {
    char *major, *minor;
    uint32_t filetype;
    unsigned num_exts;
    char *exts[MAX_EXTS];
};

/* Parses one line of the file. The line is a chunk of up to MAX_LINE - 1
 * bytes, as fgets() gives it. It follows c/mime's mime_read_file step for
 * step. It fills in *l and returns 1, or returns 0 when the line is a
 * comment, is blank or has too few columns. */
static int parse_line(struct workspace *w, char *buffer, struct line *l)
{
    char *p = buffer, *save = NULL;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '#')
        return 0;
    char *major = strtok_r(p, "/", &save);
    char *minor = strtok_r(NULL, " \t", &save);
    char *type = strtok_r(NULL, " \t\n", &save);
    char *ext = strtok_r(NULL, " \t\n", &save);
    if (!major || !minor || !type)
        return 0;
    l->major = strdup(major);
    l->minor = strdup(minor);
    if (type[0] == '*') {
        l->filetype = FILETYPE_WILDCARDED;
    } else {
        uint32_t value = 0;
        int ok = type_from_string(w, type, &value);
        if (!ok && ext && ext[0] != '.') {
            /* Try the next column, if it is not a dotted extension. */
            ok = type_from_string(w, ext, &value);
            ext = NULL;
        }
        l->filetype = ok ? value : FILETYPE_INVALID;
    }
    unsigned i = 0;
    do {
        if (ext && ext[0] == '.')
            l->exts[i++] = strdup(ext + 1);
        ext = strtok_r(NULL, " \t\n", &save);
    } while (i < MAX_EXTS && ext);
    l->num_exts = i;
    return 1;
}

static void free_lines(struct line *lines, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        free(lines[i].major);
        free(lines[i].minor);
        for (unsigned j = 0; j < lines[i].num_exts; j++)
            free(lines[i].exts[j]);
    }
    free(lines);
}

/* Copies s to *at, moves *at past it, and returns the copy's arena address. */
static uint32_t put(char **at, const char *s)
{
    char *p = *at;
    size_t n = strlen(s) + 1;
    memcpy(p, s, n);
    *at = p + n;
    return ros_addr(p);
}

/* Packs the lines into one RMA block holding the table, its extension
 * arrays and its strings. Returns the block's address, or 0 if the RMA has
 * no room. */
static uint32_t pack(const struct line *lines, unsigned n)
{
    uint32_t size = sizeof(struct table) + n * sizeof(struct entry), words = 0, chars = 0;
    for (unsigned i = 0; i < n; i++) {
        chars += (uint32_t)(strlen(lines[i].major) + strlen(lines[i].minor) + 2);
        words += lines[i].num_exts;
        for (unsigned j = 0; j < lines[i].num_exts; j++)
            chars += (uint32_t)strlen(lines[i].exts[j]) + 1;
    }
    size += words * 4 + chars;
    struct table *t = ros_rma_alloc(size);
    if (!t)
        return 0;
    t->count = n;
    uint32_t *arrays = (uint32_t *)&t->e[n];
    char *strings = (char *)(arrays + words);
    for (unsigned i = 0; i < n; i++) {
        struct entry *e = &t->e[i];
        e->major = put(&strings, lines[i].major);
        e->minor = put(&strings, lines[i].minor);
        e->filetype = lines[i].filetype;
        e->num_exts = lines[i].num_exts;
        e->exts = lines[i].num_exts ? ros_addr(arrays) : 0;
        for (unsigned j = 0; j < lines[i].num_exts; j++)
            *arrays++ = put(&strings, lines[i].exts[j]);
    }
    return ros_addr(t);
}

/* mime_read_file: reads the file. The old table is dropped once the file is
 * known to be there. If it is not, the error is "Cannot open MIME mapping
 * file". */
static os_error *read_file(struct workspace *w)
{
    uint32_t r[10] = { OSFILE_READ_INFO, scratch(w, mapping_file) };
    if (swi(XOS_File, r) || (r[0] != 1 && r[0] != 3))
        return intl_error(w, ERR_BAD_FILE);
    uint32_t length = r[4];
    if (length >= 0x7FFFFFF0u)          /* length + 1 below would wrap to nothing */
        return intl_error(w, ERR_BAD_FILE);
    char *buf = ros_rma_alloc(length + 1);
    if (!buf)
        return intl_error(w, ERR_BAD_FILE);
    uint32_t l[10] = { OSFILE_LOAD, scratch(w, mapping_file), ros_addr(buf), 0 };
    if (swi(XOS_File, l)) {
        ros_rma_free(buf);
        return intl_error(w, ERR_BAD_FILE);
    }
    buf[length] = 0;

    free_table(w);
    struct line *lines = NULL;
    unsigned n = 0, cap = 0;
    for (uint32_t at = 0; at < length;) {
        /* As fgets() does: up to MAX_LINE - 1 bytes, to a newline and including it. */
        char chunk[MAX_LINE];
        uint32_t k = 0;
        while (at < length && k < MAX_LINE - 1) {
            char c = buf[at++];
            chunk[k++] = c;
            if (c == '\n')
                break;
        }
        chunk[k] = 0;
        if (n == cap) {
            struct line *more = realloc(lines, (cap = cap ? cap * 2 : 64) * sizeof *lines);
            if (!more)
                break;
            lines = more;
        }
        memset(&lines[n], 0, sizeof lines[n]);
        if (parse_line(w, chunk, &lines[n]))
            n++;
    }
    ros_rma_free(buf);
    w->table = n ? pack(lines, n) : 0;
    free_lines(lines, n);
    return NULL;
}

/* mime_read_file_if_changed: reads the file again if its datestamp (load
 * and exec addresses) is not the one last read. If it is not a file, the
 * error is "Cannot open MIME mapping file". */
static os_error *read_file_if_changed(struct workspace *w)
{
    uint32_t r[10] = { OSFILE_READ_INFO, scratch(w, mapping_file) };
    if (swi(XOS_File, r) || r[0] != 1)
        return intl_error(w, ERR_BAD_FILE);
    if (r[2] != w->last_load || r[3] != w->last_exec) {
        w->last_load = r[2];
        w->last_exec = r[3];
        return read_file(w);
    }
    return NULL;
}

static void read_file_if_not_inited(struct workspace *w)
{
    if (!w->table)
        read_file(w);
}

/* ---- the lookups ---------------------------------------------------------------- */

/* What a lookup found. This is the entry, with the file type asked for in
 * place of a wildcard's. It corresponds to c/mime's static result. */
struct found {
    const struct entry *e;
    uint32_t filetype;
};

static int flags_ok(const struct entry *e, uint32_t flags)
{
    if ((flags & HAS_VALID_MIME) && (str(e->major)[0] == '*' || str(e->minor)[0] == '*'))
        return 0;
    if ((flags & HAS_VALID_DOT_EXTN) && e->num_exts == 0)
        return 0;
    if ((flags & HAS_VALID_RISCOS) &&
        (e->filetype == FILETYPE_WILDCARDED || e->filetype == FILETYPE_INVALID))
        return 0;
    return 1;
}

/* As c/mime's stricmp(): true if the strings have the same letters,
 * ignoring case, and the same length. */
static int same(const char *a, const char *b)
{
    while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b))
        a++, b++;
    return *a == *b;
}

static int by_mime(struct workspace *w, const char *mime, uint32_t flags, struct found *f)
{
    char buffer[MAX_LINE], *save = NULL;
    snprintf(buffer, sizeof buffer, "%s", mime);
    char *major = strtok_r(buffer, "/", &save);
    char *minor = strtok_r(NULL, " \t\r\n", &save);
    struct table *t = table(w);
    if (!major || !minor || !t)
        return 0;
    for (uint32_t i = 0; i < t->count; i++) {
        const struct entry *e = &t->e[i];
        if (!flags_ok(e, flags))
            continue;
        /* A wildcarded major type is the catch-all and matches anything. */
        if (str(e->major)[0] == '*' ||
            (same(major, str(e->major)) && (str(e->minor)[0] == '*' || same(minor, str(e->minor))))) {
            *f = (struct found){ e, e->filetype };
            return 1;
        }
    }
    return 0;
}

static int by_ft(struct workspace *w, uint32_t filetype, uint32_t flags, struct found *f)
{
    struct table *t = table(w);
    for (uint32_t i = 0; t && i < t->count; i++) {
        const struct entry *e = &t->e[i];
        if (flags != HAS_VALID_RISCOS && !flags_ok(e, flags))
            continue;
        if (e->filetype == filetype) {
            *f = (struct found){ e, e->filetype };
            return 1;
        }
        if (e->filetype == FILETYPE_WILDCARDED) {
            *f = (struct found){ e, filetype };
            return 1;
        }
    }
    return 0;
}

static int by_ftname(struct workspace *w, const char *type, uint32_t flags, struct found *f)
{
    uint32_t value;
    return type_from_string(w, type, &value) && by_ft(w, value, flags, f);
}

static int by_ext(struct workspace *w, const char *ext, uint32_t flags, struct found *f)
{
    if (ext[0] == '.')
        ext++;
    struct table *t = table(w);
    for (uint32_t i = 0; t && i < t->count; i++) {
        const struct entry *e = &t->e[i];
        for (uint32_t j = 0; j < e->num_exts; j++) {
            if (flags_ok(e, flags) && same(ext, str(ros_ld32(e->exts + 4 * j)))) {
                *f = (struct found){ e, e->filetype };
                return 1;
            }
        }
    }
    return 0;
}

/* ---- MimeMap_Translate ---------------------------------------------------------- */

/* Copies a string that a caller gave, from the arena, up to its zero or at
 * most max - 1 characters. */
static void from_arena(uint32_t a, char *out, size_t max)
{
    size_t i = 0;
    for (uint32_t c; i + 1 < max && (c = ros_ld8(a + (uint32_t)i)) != 0; i++)
        out[i] = (char)c;
    out[i] = 0;
}

/* R0 is the input's format, R1 the input, R2 the output's format and R3
 * the output's buffer. On exit, for MMM_TYPE_RISCOS, R3 is the file type.
 * For MMM_TYPE_DOT_EXTNS, the address of the array of extensions is stored
 * at R3 -> and R4 is the number of extensions. */
void ros_thunk_MimeMap_Translate(struct ros_cpu *s)
{
    struct workspace *w = ws();
    uint32_t *r = s->r;
    read_file_if_not_inited(w);

    uint32_t flags;
    switch (r[2]) {
    case MMM_TYPE_RISCOS:
    case MMM_TYPE_RISCOS_STRING: flags = HAS_VALID_RISCOS; break;
    case MMM_TYPE_MIME:          flags = HAS_VALID_MIME; break;
    case MMM_TYPE_DOT_EXTN:      flags = HAS_VALID_DOT_EXTN; break;
    default:                     flags = HAS_NO_CHECKS; break;
    }

    char in[1024];
    struct found f;
    int found;
    switch (r[0]) {
    case MMM_TYPE_RISCOS:
        found = by_ft(w, r[1], flags, &f);
        break;
    case MMM_TYPE_RISCOS_STRING:
        from_arena(r[1], in, sizeof in);
        found = by_ftname(w, in, flags, &f);
        break;
    case MMM_TYPE_MIME:
        from_arena(r[1], in, sizeof in);
        found = by_mime(w, in, flags, &f);
        break;
    case MMM_TYPE_DOT_EXTN:
        from_arena(r[1], in, sizeof in);
        found = by_ext(w, in, flags, &f);
        break;
    default:                            /* MMM_TYPE_DOT_EXTNS and the rest */
        ros_swi_fail(s, intl_error(w, ERR_BAD_REASON));
        return;
    }
    if (!found) {
        ros_swi_fail(s, intl_error(w, ERR_LOOKUP_FAILED));
        return;
    }

    switch (r[2]) {
    case MMM_TYPE_RISCOS:
        r[3] = f.filetype;
        break;
    case MMM_TYPE_RISCOS_STRING: {
        /* The name up to its first space, or 8 characters. If it has none,
         * the result is &xxx. */
        uint32_t q[10] = { FSCONTROL_READ_FILE_TYPE, 0, f.filetype };
        char name[12];
        if (!swi(XOS_FSControl, q)) {
            memcpy(name, &q[2], 4);
            memcpy(name + 4, &q[3], 4);
            name[8] = ' ';
            *strchr(name, ' ') = 0;
        } else {
            snprintf(name, sizeof name, "&%03X", f.filetype);
        }
        strcpy(ros_ptr(r[3]), name);
        break;
    }
    case MMM_TYPE_MIME:
        sprintf(ros_ptr(r[3]), "%s/%s", str(f.e->major), str(f.e->minor));
        break;
    case MMM_TYPE_DOT_EXTN:
        strcpy(ros_ptr(r[3]), str(ros_ld32(f.e->exts)));
        break;
    case MMM_TYPE_DOT_EXTNS:
        ros_st32(r[3], f.e->exts);
        r[4] = f.e->num_exts;
        break;
    }
    s->v = 0;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6, "SWI value out of range for module MimeMap");
}

/* ---- the *commands -------------------------------------------------------------- */

static os_error *write_s(const char *s)
{
    os_error *e = NULL;
    for (; *s && !e; s++)
        e = xos_write_c((uint8_t)*s);
    return e;
}

/* mime_print_mimeentry_t: prints "MIME type: major/minor, RISC OS file
 * type: &xxx". Then it prints the extensions on a line of their own, each
 * after a tab. */
static os_error *print_entry(struct workspace *w, const struct entry *e, uint32_t filetype)
{
    char line[MAX_LINE * 2];
    snprintf(line, sizeof line, "%s: %s/%s, ", intl_lookup(w, "Mt"), str(e->major), str(e->minor));
    os_error *x = write_s(line);
    if (!x) {
        snprintf(line, sizeof line, "%s: &%03X", intl_lookup(w, "Rt"), filetype);
        x = write_s(line);
    }
    if (!x)
        x = xos_new_line();
    if (!x && e->num_exts && e->exts) {
        snprintf(line, sizeof line, "%s:", intl_lookup(w, "Ex"));
        x = write_s(line);
        if (!x)
            x = xos_new_line();
        for (uint32_t j = 0; !x && j < e->num_exts; j++) {
            snprintf(line, sizeof line, "\t%s", str(ros_ld32(e->exts + 4 * j)));
            x = write_s(line);
        }
        if (!x)
            x = xos_new_line();
    }
    return x;
}

/* *MimeMap [&xxx | .ext | mime/type | Filetype] shows every mapping, or the
 * one that the parameter finds. The parameter is a dotted extension, a hex
 * file type, a MIME type (it has a "/") or otherwise a file type's name.
 * The lookup makes no checks. */
static os_error *cmd_mimemap(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct workspace *w = ws();
    read_file_if_not_inited(w);
    if (argc == 0) {
        struct table *t = table(w);
        for (uint32_t i = 0; t && i < t->count; i++) {
            os_error *e = print_entry(w, &t->e[i], t->e[i].filetype);
            if (e)
                return e;
        }
        return NULL;
    }
    char copy[MAX_LINE], *save = NULL;
    size_t n = 0;
    for (uint32_t c; n + 1 < sizeof copy && (c = ros_ld8(tail + (uint32_t)n)) != 0 && c != 10 &&
                     c != 13; n++)
        copy[n] = (char)c;
    copy[n] = 0;
    struct found f;
    int found = 0;
    char *tok;
    if (copy[0] == '.') {
        if ((tok = strtok_r(&copy[1], " \t\r\n", &save)) != NULL)
            found = by_ext(w, tok, HAS_NO_CHECKS, &f);
    } else if (copy[0] == '&') {
        if ((tok = strtok_r(&copy[1], " \t\r\n", &save)) != NULL)
            found = by_ft(w, (uint32_t)strtoul(tok, NULL, 16), HAS_NO_CHECKS, &f);
    } else if (strchr(copy, '/')) {
        if ((tok = strtok_r(copy, " \t\r\n", &save)) != NULL)
            found = by_mime(w, tok, HAS_NO_CHECKS, &f);
    } else if ((tok = strtok_r(copy, " \t\r\n", &save)) != NULL) {
        found = by_ftname(w, tok, HAS_NO_CHECKS, &f);
    }
    if (!found)
        return intl_error(w, ERR_LOOKUP_FAILED);
    return print_entry(w, f.e, f.filetype);
}

/* *ReadMimeMap: reads the file again if it has changed. */
static os_error *cmd_readmimemap(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    return read_file_if_changed(ws());
}

/* Help and syntax. These are the texts of the CmdHelp tokens, as 5.30's
 * *Help prints them. */
static const struct ros_command commands[] = {
    { "MimeMap", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *\x1B\x00 [&xxx | .ext | mime/type | Filetype]",
      "*MimeMap returns information on the file type specified.", cmd_mimemap },
    { "ReadMimeMap", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *\x1B\x00",
      "*ReadMimeMap rereads the MIME mappings file.", cmd_readmimemap },
    { 0 },
};

/* ---- the module ----------------------------------------------------------------- */

/* Sets a variable to value (a string) when it is not set at all.
 * OS_ReadVarVal with R2 < 0 asks only whether the variable is there. It
 * gives "System variable not found" (&124) when it is not. */
static void default_var(struct workspace *w, const char *name, const char *value)
{
    uint32_t r[10] = { scratch(w, name), 0, 0x80000000u, 0, 0 };
    os_error *e = swi(XOS_ReadVarVal, r);
    if (!e || e->errnum != 0x124u)
        return;
    size_t at = strlen(name) + 1;
    snprintf(w->scratch + at, sizeof w->scratch - at, "%s", value);
    uint32_t s[10] = { ros_addr(w->scratch), ros_addr(w->scratch + at), (uint32_t)strlen(value), 0, 0 };
    swi(XOS_SetVarVal, s);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    memcpy(w->file, messages_file, sizeof messages_file);
    uint32_t r[10] = { ros_addr(w->desc), ros_addr(w->file), 0 };
    os_error *e = swi(XMessageTrans_OpenFile, r);
    if (e) {
        ros_rma_free(w);
        return e;
    }
    ros_st32(m->private_word, ros_addr(w));
    /* Point at the ROM's table, unless something has said otherwise. */
    default_var(w, "InetDBase$Path", MIMEMAP_ROM_DBASE);
    default_var(w, "Inet$MimeMappings", MIMEMAP_DEFAULT);
    read_file_if_changed(w);            /* gives no error if the file is not there */
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    if (w) {
        free_table(w);
        uint32_t r[10] = { ros_addr(w->desc) };
        swi(XMessageTrans_CloseFile, r);
        ros_rma_free(w);
    }
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module mimemap_module = {
    .title = "MimeMap",
    .help = "MimeMap\t\t0.19 (15 Apr 2016)",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .commands = commands,
    .swi_chunk = 0x50B00,
    .swi_thunks = ros_swi_thunks_MimeMap,
    .swi_names = ros_swi_names_MimeMap,
    .swi_prefix = "MimeMap",
};

__attribute__((constructor)) static void count(void)
{
    mimemap_module.swi_count = ros_swi_count_MimeMap;
}
