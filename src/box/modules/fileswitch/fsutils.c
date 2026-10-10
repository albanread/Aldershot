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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/FileSys/FileSwitch: s.FSCommands, s.CtrlUtils, s.FSUtils,
 * s.FSUtils2, s.FSUtils3, s.FSControl).
 */

/* fsutils.c -- FileSwitch's utilities and its *commands.
 *
 * Written from FileSwitch's own sources (FSControl, CtrlUtils, FSUtils,
 * FSCommands).
 *
 *   - The listings are *Cat, *Ex, *LCat, *LEx, *Info and *FileInfo.  Each
 *     has a title block, then items laid out in columns across an
 *     80-column line.  Each item is built from the format that FileSwitch's
 *     Messages give: "01" for Cat, "0123" for Ex, "0145" for FileInfo.  The
 *     fields are name, attributes, type and date (or load and exec), and
 *     size.
 *   - *Access has its attribute letters.
 *   - *Copy, *Wipe and *Count have their options (Copy$Options,
 *     Wipe$Options, Count$Options, then the command's own) and their rules.
 *     A wildcard destination leaf must be "*".  An existing file is
 *     overwritten only with F or when confirmed, and never onto a
 *     directory.  N copies only what is newer.  Locked files are skipped
 *     without F.  C asks, Q stops asking, and A abandons.
 *   - Running a file goes through Run$Path, then Alias$@RunType_xxx with the
 *     canonical name and the rest of the line.  An Absolute file is run if
 *     it is a C application, and otherwise by the ARM container.  A Utility
 *     file is run by the ARM container.  The code of an untyped file, at its
 *     load address, is not run: ROSGD runs no ARM code it has not compiled.
 *
 * The screen is taken as 80 columns until there is a VDU to ask.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsw.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/armbox.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/task.h"

#define SCREEN_WIDTH 80u
#define MIN_NAME_WIDTH 12u

/* The options (FSUtils's util_ bits) */
#define U_RECURSE    (1u << 0)
#define U_FORCE      (1u << 1)
#define U_DATELIMIT  (1u << 2)
#define U_CONFIRM    (1u << 3)
#define U_VERBOSE    (1u << 4)
#define U_QUICK      (1u << 5)
#define U_PROMPT     (1u << 6)
#define U_DELETESRC  (1u << 7)
#define U_PRINTOK    (1u << 8)
#define U_NOATTR     (1u << 9)
#define U_RESTAMP    (1u << 10)
#define U_STRUCTURE  (1u << 11)
#define U_NEWER      (1u << 12)
#define U_USERBUF    (1u << 13)
#define U_PEEKDEST   (1u << 14)

/* ---- small things -------------------------------------------------------------- */

static os_error *nl(void)
{
    return fsw_write_s("\n\r");
}

static os_error *swi_error(const struct ros_cpu *c)
{
    return c->v ? (os_error *)ros_ptr(c->r[0]) : NULL;
}

/* A number as text through one of the kernel's conversions */
static void convert(uint32_t swi, uint32_t value, char *out, size_t max)
{
    uint32_t b = ros_addr(ros_rma_alloc(64));
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = value, c.r[1] = b, c.r[2] = 60;
    ros_swi(&c, swi);
    snprintf(out, max, "%s", c.v ? "" : (const char *)ros_ptr(b));
    ros_rma_free(ros_ptr(b));
}

/* A load and exec's date through OS_ConvertDateAndTime */
static void date_text(uint32_t load, uint32_t exec, const char *fmt, char *out, size_t max)
{
    uint32_t b = ros_addr(ros_rma_alloc(512));
    ros_st32(b, exec);
    ros_st8(b + 4, load & 0xFF);
    strcpy(ros_ptr(b + 8), fmt);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = b, c.r[1] = b + 256, c.r[2] = 250, c.r[3] = b + 8;
    ros_swi(&c, XOS_ConvertDateAndTime);
    snprintf(out, max, "%s", c.v ? "" : (const char *)ros_ptr(b + 256));
    ros_rma_free(ros_ptr(b));
}

static void append(char *out, size_t max, const char *s)
{
    size_t n = strlen(out);
    snprintf(out + n, max - n, "%s", s);
}

/* s padded with spaces to width; truncated to it when trunc */
static void append_padded(char *out, size_t max, const char *s, unsigned width, int trunc)
{
    size_t n = strlen(out);
    if (trunc)
        snprintf(out + n, max - n, "%-*.*s", (int)width, (int)width, s);
    else
        snprintf(out + n, max - n, "%-*s", (int)width, s);
}

static void attr_string(uint32_t type, uint32_t a, char out[8])
{
    int k = 0;
    if (type & OBJ_DIR)                 /* an image too (int_StuffAttsIntoBuffer's TST) */
        out[k++] = 'D';
    if (a & ATTR_L)
        out[k++] = 'L';
    if (a & ATTR_W)
        out[k++] = 'W';
    if (a & ATTR_R)
        out[k++] = 'R';
    out[k++] = '/';
    if (a & ATTR_PW)
        out[k++] = 'W';
    if (a & ATTR_PR)
        out[k++] = 'R';
    out[k] = 0;
}

/* ---- the listings --------------------------------------------------------------- */

enum { FMT_CAT, FMT_EX, FMT_FILEINFO };

static const char *const formats[] = { "01", "0123", "0145" };
static const unsigned columns[] = { 21, 63, 68 };

static void load_exec(char *out, size_t max, const struct fs_info *i, int exact)
{
    char type[16], date[80];
    if (i->type & OBJ_DIR)              /* an image too (int_GenStuffLoadExecIntoBuffer) */
        snprintf(type, sizeof type, "Directory");
    else if (fsw_is_typed(i->load))
        fsw_type_name(i->load >> 8 & 0xFFF, type);
    else
        type[0] = 0;
    append_padded(out, max, type, 9, 0);
    append(out, max, " ");
    if (fsw_is_typed(i->load)) {
        date_text(i->load, i->exec,
                  exact ? "%24:%MI:%SE.%CS %DY-%M3-%CE%YR" : "%24:%MI:%SE %DY-%M3-%CE%YR", date,
                  sizeof date);
    } else {
        snprintf(date, sizeof date, exact ? "      %08X %08X" : "   %08X %08X", i->load, i->exec);
    }
    append(out, max, date);
}

/* One item of a listing */
static void item(char *out, size_t max, int fmt, const char *name, const struct fs_info *i,
                 unsigned width)
{
    out[0] = 0;
    for (const char *c = formats[fmt]; *c; c++) {
        char tmp[64];
        if (c != formats[fmt])
            append(out, max, " ");
        switch (*c) {
        case '0':
            append_padded(out, max, name, width, 1);
            break;
        case '1':
            attr_string(i->type, i->attr, tmp);
            append_padded(out, max, tmp, 7, 0);
            break;
        case '2':
        case '4':
            load_exec(out, max, i, *c == '4');
            break;
        case '3':
            convert(XOS_ConvertFixedFileSize, i->length, tmp, sizeof tmp);
            append(out, max, tmp);
            break;
        case '5':
            snprintf(tmp, sizeof tmp, "%08X", i->length);
            append(out, max, tmp);
            break;
        }
    }
}

/* The width names get: the longest, within the screen, FileSwitch$NameWidth
 * and at least 12 */
static unsigned name_width(unsigned longest, int fmt)
{
    char v[16];
    unsigned limit = 255;
    if (fsw_read_var("FileSwitch$NameWidth", v, sizeof v)) {
        limit = (unsigned)strtoul(v, NULL, 10);
        if (limit > 255)
            limit = 255;
    }
    unsigned w = longest;
    if (w > SCREEN_WIDTH - (columns[fmt] - 12))
        w = SCREEN_WIDTH - (columns[fmt] - 12);
    if (w > limit)
        w = limit;
    return w < MIN_NAME_WIDTH ? MIN_NAME_WIDTH : w;
}

/* The objects of a directory that match a wildcard, laid out in columns */
static os_error *body(const struct fsw_loc *dir, const char *pattern, int fmt)
{
    struct fs_entry e;
    os_error *err = NULL;
    unsigned longest = 0;
    for (uint32_t i = 0; fsw_readdir(dir, i, &e, &err); i++)
        if (fsw_wild_match(pattern, e.name) && strlen(e.name) > longest)
            longest = (unsigned)strlen(e.name);
    if (err)
        return err;
    unsigned width = name_width(longest, fmt);
    unsigned col = columns[fmt] + width - 12;
    unsigned lpos = 0, pregap = 0;
    for (uint32_t i = 0; fsw_readdir(dir, i, &e, &err); i++) {
        if (!fsw_wild_match(pattern, e.name))
            continue;
        char s[400];
        item(s, sizeof s, fmt, e.name, &e.info, width);
        unsigned len = (unsigned)strlen(s);
        unsigned spacing = (lpos + pregap + col - 1) / col * col - lpos;
        if (pregap && spacing + len + lpos > SCREEN_WIDTH) {
            if ((err = nl()) != NULL)
                return err;
            lpos = pregap = spacing = 0;
        }
        lpos += spacing + len;
        for (unsigned k = 0; k < spacing; k++)
            if ((err = fsw_write_s(" ")) != NULL)
                return err;
        if ((err = fsw_write_s(s)) != NULL)
            return err;
        pregap = 1;
    }
    if (err)
        return err;
    return pregap ? nl() : NULL;
}

static os_error *title_line(const char *label, const struct fsw_loc *l, int set)
{
    char canon[1100];
    if (!set)
        return fsw_printf_out("%s\"Unset\"\n\r", label);
    fsw_canonical(l, canon, sizeof canon);
    return fsw_printf_out("%s%s\n\r", label, canon);
}

/* OS_FSControl 5-8: *Cat, *Ex (of the CSD by default), *LCat, *LEx (the
 * library) */
os_error *fsw_catex(uint32_t reason, uint32_t tail)
{
    struct fsw_loc l;
    os_error *e = fsw_dir_arg(tail, &l, reason == 5 || reason == 6 ? "@" : "%");
    if (e)
        return e;
    struct fsw_dirs *d = fsw_dirs_of(l.fsi);
    char canon[1100];
    fsw_canonical(&l, canon, sizeof canon);
    if ((e = fsw_printf_out("Dir. %s Option 00 (Off)\n\r", canon)) != NULL ||
        (e = title_line("CSD  ", &d->csd, 1)) != NULL ||
        (e = title_line("Lib. ", &d->lib, d->lib_set)) != NULL ||
        (e = title_line("URD  ", &d->urd, d->urd_set)) != NULL)
        return e;
    return body(&l, "*", reason == 5 || reason == 7 ? FMT_CAT : FMT_EX);
}

/* A name whose leaf may be wild: the directory it is in, and the leaf as
 * a pattern.  *wild says whether it was. */
static os_error *split_wild(const char *name, struct fsw_loc *dir, char *pattern, size_t max,
                            int *wild)
{
    const char *colon = strrchr(name, ':');
    const char *dot = strrchr(name, '.');
    const char *start = colon && (!dot || colon > dot) ? colon + 1 : dot ? dot + 1 : name;
    *wild = fsw_has_wild(start);
    if (!*wild || !*start) {
        struct fsw_loc l;
        os_error *e = fsw_resolve(name, R_READ, &l, 0);
        if (e)
            return e;
        *dir = l;
        snprintf(pattern, max, "%s", fsw_leaf_of(&l));
        char *d = strrchr(dir->path, '.');
        if (d)
            *d = 0;
        else
            dir->path[0] = 0;
        *wild = 0;
        return NULL;
    }
    char parent[1100];
    if (start == name)
        snprintf(parent, sizeof parent, "@");
    else if (start[-1] == ':')
        snprintf(parent, sizeof parent, "%.*s", (int)(start - name), name);
    else
        snprintf(parent, sizeof parent, "%.*s", (int)(start - name - 1), name);
    snprintf(pattern, max, "%s", start);
    return fsw_resolve(parent, R_READ, dir, 0);
}

/* The place of the object called leaf in dir */
static void child(const struct fsw_loc *dir, const char *leaf, struct fsw_loc *out)
{
    *out = *dir;
    size_t n = strlen(out->path);
    snprintf(out->path + n, sizeof out->path - n, "%s%s", n ? "." : "", leaf);
}

/* OS_FSControl 9 and 32: *Info and *FileInfo */
os_error *fsw_info(uint32_t reason, uint32_t addr)
{
    char name[1024], pattern[256];
    struct fsw_loc dir, l;
    int wild;
    os_error *e = fsw_arg_path(addr, name, sizeof name);
    if (!e)
        e = split_wild(name, &dir, pattern, sizeof pattern, &wild);
    if (e)
        return e;
    int fmt = reason == 9 ? FMT_EX : FMT_FILEINFO;
    if (wild)
        return body(&dir, pattern, fmt);
    struct fs_info info;
    child(&dir, pattern, &l);
    if (!dir.path[0] && !pattern[0])
        l = dir;
    if ((e = fsw_stat_loc(&l, &info)) != NULL)
        return e;
    if (info.type == OBJ_NOTHING)
        return fsw_err_not_found(name);
    char s[400];
    item(s, sizeof s, fmt, fsw_leaf_of(&l), &info, name_width((unsigned)strlen(fsw_leaf_of(&l)), fmt));
    if ((e = fsw_write_s(s)) != NULL)
        return e;
    return nl();
}

/* ---- *Access ----------------------------------------------------------------- */

os_error *fsw_access(uint32_t addr, uint32_t access)
{
    char name[1024], a[64] = "", pattern[256];
    os_error *e = fsw_arg_path(addr, name, sizeof name);
    if (!e && access)
        e = fsw_arg_name(access, a, sizeof a);
    if (e)
        return e;
    uint32_t attr = 0;
    int pub = 0;
    for (const char *p = a; *p; p++) {
        char c = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        if (c == '/' && !pub)
            pub = 1;
        else if (c == 'L' && !pub)
            attr |= ATTR_L;
        else if (c == 'W')
            attr |= pub ? ATTR_PW : ATTR_W;
        else if (c == 'R')
            attr |= pub ? ATTR_PR : ATTR_R;
        else
            return ros_error(E_BAD_PARAMETERS, "Access attributes '%s' not recognised", a);
    }
    struct fsw_loc dir, l;
    int wild;
    if ((e = split_wild(name, &dir, pattern, sizeof pattern, &wild)) != NULL)
        return e;
    if (!wild) {
        struct fs_info info;
        child(&dir, pattern, &l);
        if ((e = fsw_stat_loc(&l, &info)) != NULL)
            return e;
        if (info.type == OBJ_NOTHING)
            return fsw_err_not_found(name);
        return fsw_setinfo(&l, 4, 0, 0, attr);
    }
    struct fs_entry ent;
    os_error *err = NULL;
    for (uint32_t i = 0; fsw_readdir(&dir, i, &ent, &err); i++) {
        if (!fsw_wild_match(pattern, ent.name))
            continue;
        child(&dir, ent.name, &l);
        if ((e = fsw_setinfo(&l, 4, 0, 0, attr)) != NULL)
            return e;
    }
    return err;
}

/* ---- confirming ---------------------------------------------------------------- */

struct util {
    uint32_t flags;
    uint64_t from, to;                  /* the date range, with U_DATELIMIT */
    unsigned done, skipped;
    uint64_t bytes;
    int abandoned;
};

/* Ask: "<question> (Y/N/Quiet/Abandon) ? ", 1 to go ahead */
static int confirm(struct util *u, const char *question, os_error **err)
{
    if (!(u->flags & U_CONFIRM))
        return 1;
    if ((*err = fsw_printf_out("%s (Y/N/Quiet/Abandon) ? ", question)) != NULL)
        return 0;
    for (;;) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_ReadC);
        if (c.v) {
            *err = ros_ptr(c.r[0]);
            return 0;
        }
        char k = (char)(c.r[0] >= 'a' && c.r[0] <= 'z' ? c.r[0] - 32 : c.r[0]);
        const char *answer = k == 'Y' ? "Y" : k == 'N' ? "N" : k == 'Q' ? "Q" : k == 'A' ? "A"
                             : c.c ? "A" : NULL;
        if (!answer)
            continue;
        if ((*err = fsw_printf_out("%s\n\r", answer)) != NULL)
            return 0;
        if (k == 'Q')
            u->flags &= ~U_CONFIRM;
        if (*answer == 'A') {
            u->abandoned = 1;
            *err = fsw_write_s("Abandoned\n\r");
            return 0;
        }
        return *answer != 'N';
    }
}

static void size_text(uint64_t n, char *out, size_t max)
{
    convert(XOS_ConvertFileSize, n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n, out, max);
}

/* A total: the exact count, punctuated as the territory punctuates it
 * (Util_FilesDone's ConvertToPunctCardinal, s/FSUtils3), not a file size
 * rounded to "43 kbytes".  OS_ConvertVariform type 6, eight bytes. */
static void count_text(uint64_t n, char *out, size_t max)
{
    uint8_t *mem = ros_rma_alloc(80);
    uint32_t b = ros_addr(mem), v = b + 64;
    ros_st32(v, (uint32_t)n);
    ros_st32(v + 4, (uint32_t)(n >> 32));
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = v, c.r[1] = b, c.r[2] = 60, c.r[3] = 8, c.r[4] = 6;
    ros_swi(&c, XOS_ConvertVariform);
    snprintf(out, max, "%s", c.v ? "" : (const char *)ros_ptr(b));
    ros_rma_free(mem);
}

static uint64_t date_of(const struct fs_info *i)
{
    return (uint64_t)(i->load & 0xFF) << 32 | i->exec;
}

/* The options: letters, and the bit each sets; 'A' in Copy's is the
 * inverse of its bit */
static os_error *parse_options(const char *s, const char *letters, const uint32_t *bits,
                               uint32_t *flags)
{
    for (const char *p = s; *p;) {
        while (*p == ' ')
            p++;
        if (!*p || (unsigned char)*p < ' ')
            break;
        int neg = 0;
        if (*p == '~')
            neg = 1, p++;
        char c = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        const char *at = c ? strchr(letters, c) : NULL;
        if (!at)
            return ros_error(E_BAD_OPTION, "Command option not known");
        uint32_t bit = bits[at - letters];
        if (c == 'a')
            neg = !neg;
        if (neg)
            *flags &= ~bit;
        else
            *flags |= bit;
        p++;
    }
    return NULL;
}

/* ---- *Copy --------------------------------------------------------------------- */

static os_error *copy_object(struct util *u, const struct fsw_loc *src, const struct fsw_loc *dst,
                             const struct fs_info *si, int top);

static os_error *copy_data(const struct fsw_loc *src, const struct fsw_loc *dst,
                           const struct fs_info *si, uint32_t load, uint32_t exec)
{
    struct fsw_file *sh, *dh;
    os_error *e = fsw_file_open(src, 0, 0, &sh);
    if (e)
        return e;
    e = fsw_create(dst, load, exec, 0, 0);
    if (!e) {
        fsw_modifying(UPFS_OPEN_UPDATE, dst, 0, 0, 0, 0);
        e = fsw_file_open(dst, 1, 0, &dh);
    }
    if (e) {
        fsw_file_close(sh, 0);
        return e;
    }
    /* (static, and so the copy reentrant only between calls: an image
     * filing system's reads do not copy) */
    static uint8_t buf[65536];
    for (uint32_t pos = 0; !e && pos < si->length;) {
        uint32_t got;
        e = fsw_file_read(sh, pos, buf, sizeof buf, &got);
        if (!e && got == 0)
            break;
        if (!e)
            e = fsw_file_write(dh, pos, buf, got);
        pos += got;
    }
    fsw_file_close(sh, 0);
    os_error *e2 = fsw_file_close(dh, 0);
    return e ? e : e2;
}

static os_error *copy_file(struct util *u, const struct fsw_loc *src, const struct fsw_loc *dst,
                           const struct fs_info *si)
{
    char sname[1100], dname[1100], q[2300], size[32];
    fsw_canonical(src, sname, sizeof sname);
    fsw_canonical(dst, dname, sizeof dname);
    if ((u->flags & U_DATELIMIT) && fsw_is_typed(si->load) &&
        (date_of(si) < u->from || date_of(si) > u->to))
        return NULL;
    if (u->flags & U_STRUCTURE)
        return NULL;
    struct fs_info di;
    os_error *e = fsw_stat_loc(dst, &di);
    if (e)
        return e;
    /* onto a directory, never; onto an image, only an image (FSUtils) */
    if (di.type == OBJ_DIR || (di.type == OBJ_IMAGE && si->type != OBJ_IMAGE))
        return ros_error(E_TYPES_DONT_MATCH, "File cannot be copied - a directory with that "
                                             "destination name already exists");
    snprintf(q, sizeof q, "%s file %s as %s", u->flags & U_DELETESRC ? "Move" : "Copy", sname,
             dname);
    if (!confirm(u, q, &e))
        return e;
    if (di.type & OBJ_FILE) {
        if ((u->flags & U_NEWER) && fsw_is_typed(si->load) && fsw_is_typed(di.load) &&
            date_of(si) <= date_of(&di)) {
            u->skipped++;
            if (u->flags & U_VERBOSE)
                return fsw_printf_out("File %s has %s %s\n\r", sname,
                                      date_of(si) == date_of(&di) ? "the same datestamp as"
                                                                 : "an earlier datestamp than",
                                      dname);
            return NULL;
        }
        if (!(u->flags & U_FORCE)) {
            int locked = (di.attr & ATTR_L) != 0;
            if (!(u->flags & U_VERBOSE)) {
                u->skipped++;
                return NULL;
            }
            u->skipped++;
            return fsw_printf_out("File %s already exists%s.\n\r", dname,
                                  locked ? " and is locked" : "");
        }
        if ((di.attr & ATTR_L) || (di.attr & (ATTR_R | ATTR_W)) != (ATTR_R | ATTR_W))
            if ((e = fsw_setinfo(dst, 4, 0, 0, ATTR_R | ATTR_W)) != NULL)
                return e;
    }
    uint32_t load = si->load, exec = si->exec;
    if ((u->flags & U_RESTAMP) && fsw_is_typed(load)) {
        uint64_t now = fsw_now_cs();
        load = fsw_stamp_load(load >> 8 & 0xFFF, now);
        exec = (uint32_t)now;
    }
    if ((e = copy_data(src, dst, si, load, exec)) != NULL)
        return e;
    e = fsw_setinfo(dst, u->flags & U_NOATTR ? 2 : 1, load, exec, si->attr);
    if (!e && (u->flags & U_NOATTR))
        e = fsw_setinfo(dst, 3, load, exec, 0);
    if (e)
        return e;
    if (u->flags & U_DELETESRC)
        if ((e = fsw_remove(src, si)) != NULL)
            return e;
    u->done++;
    u->bytes += si->length;
    if (u->flags & U_VERBOSE) {
        size_text(si->length, size, sizeof size);
        return fsw_printf_out("File %s %s as %s, %s\n\r", sname,
                              u->flags & U_DELETESRC ? "moved" : "copied", dname, size);
    }
    return NULL;
}

/* A directory's leaves, read before anything changes it */
static char **leaves(const struct fsw_loc *dir, unsigned *n, os_error **err)
{
    struct fs_entry e;
    char **list = NULL;
    unsigned cap = 0;
    *n = 0;
    *err = NULL;
    for (uint32_t i = 0; fsw_readdir(dir, i, &e, err); i++) {
        if (*n == cap) {
            cap = cap ? cap * 2 : 16;
            char **m = realloc(list, cap * sizeof *m);
            if (!m)
                break;
            list = m;
        }
        list[(*n)++] = strdup(e.name);
    }
    return list;
}

static void free_leaves(char **list, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

static os_error *copy_dir(struct util *u, const struct fsw_loc *src, const struct fsw_loc *dst,
                          const struct fs_info *si, int top)
{
    char sname[1100], dname[1100], q[2300];
    fsw_canonical(src, sname, sizeof sname);
    fsw_canonical(dst, dname, sizeof dname);
    struct fs_info di;
    os_error *e = fsw_stat_loc(dst, &di);
    if (e)
        return e;
    if (di.type & OBJ_FILE)
        return ros_error(E_TYPES_DONT_MATCH, "Directory cannot be copied - a file with that "
                                             "destination name already exists");
    snprintf(q, sizeof q, "%s directory %s as %s", u->flags & U_DELETESRC ? "Move" : "Copy",
             sname, dname);
    if (!confirm(u, q, &e))
        return e;
    if (di.type == OBJ_NOTHING) {
        if ((e = fsw_mkdir(dst)) != NULL)
            return e;
        if ((u->flags & U_VERBOSE) && (e = fsw_printf_out("Created directory %s\n\r", dname)))
            return e;
    }
    if (!(u->flags & U_NOATTR))
        fsw_setinfo(dst, 4, 0, 0, si->attr);
    if (!top && !(u->flags & U_RECURSE))
        return NULL;
    unsigned n;
    uint64_t before = u->bytes;         /* what this directory itself copies */
    char **list = leaves(src, &n, &e);
    for (unsigned i = 0; !e && !u->abandoned && i < n; i++) {
        struct fsw_loc s2, d2;
        struct fs_info i2;
        child(src, list[i], &s2);
        child(dst, list[i], &d2);
        e = fsw_stat_loc(&s2, &i2);
        if (!e && i2.type != OBJ_NOTHING)
            e = copy_object(u, &s2, &d2, &i2, 0);
    }
    free_leaves(list, n);
    if (e || u->abandoned)
        return e;
    /* CopyDirectory_Verbose (s/FSUtils, CC17/CC18): the directory's own
     * line, once its contents are copied, with what they came to */
    if (u->flags & U_VERBOSE) {
        char size[32];
        size_text(u->bytes - before, size, sizeof size);
        if ((e = fsw_printf_out("Directory %s %s as %s, %s\n\r", sname,
                                u->flags & U_DELETESRC ? "moved" : "copied", dname, size)) != NULL)
            return e;
    }
    if (u->flags & U_DELETESRC)
        if ((e = fsw_remove(src, si)) != NULL)
            return e;
    return NULL;
}

static os_error *copy_object(struct util *u, const struct fsw_loc *src, const struct fsw_loc *dst,
                             const struct fs_info *si, int top)
{
    if (si->type == OBJ_DIR) {
        /* Into itself would never end */
        size_t n = strlen(src->path);
        if (src->fsi == dst->fsi && !strcasecmp(src->disc, dst->disc) &&
            !strncasecmp(dst->path, src->path, n) && (dst->path[n] == '.' || !dst->path[n]))
            return ros_error(E_BAD_COPY, "Inappropriate use of wildcard characters in "
                                         "destination name");
        if (!top && !(u->flags & U_RECURSE))
            return NULL;
        return copy_dir(u, src, dst, si, top);
    }
    return copy_file(u, src, dst, si);
}

static os_error *summary(struct util *u, const char *verb)
{
    char total[48];                     /* "%0 bytes", "%0 byte" (bytetags) */
    count_text(u->bytes, total, sizeof total);
    return fsw_printf_out("%u file%s %s, total %s byte%s\n\r", u->done, u->done == 1 ? "" : "s",
                          verb, total, u->bytes == 1 ? "" : "s");
}

/* OS_FSControl 26: R1 source, R2 destination, R3 options, R4-R7 the dates */
os_error *fsw_copy(struct ros_cpu *s)
{
    char sname[1024], dname[1024], spat[256];
    struct util u = { s->r[3], (uint64_t)(s->r[5] & 0xFF) << 32 | s->r[4],
                      (uint64_t)(s->r[7] & 0xFF) << 32 | s->r[6], 0, 0, 0, 0 };
    struct fsw_loc sdir, ddir, sl, dl;
    int swild;
    os_error *e = fsw_arg_path(s->r[1], sname, sizeof sname);
    if (!e)
        e = fsw_arg_path(s->r[2], dname, sizeof dname);
    if (!e)
        e = split_wild(sname, &sdir, spat, sizeof spat, &swild);
    if (e)
        return e;
    /* The destination: its leaf may only be wild as "*" */
    const char *dot = strrchr(dname, '.'), *colon = strrchr(dname, ':');
    const char *dstart = colon && (!dot || colon > dot) ? colon + 1 : dot ? dot + 1 : dname;
    int dwild = fsw_has_wild(dstart);
    if (dwild && strcmp(dstart, "*") != 0)
        return ros_error(E_BAD_COPY, "Inappropriate use of wildcard characters in destination "
                                     "name");
    if (dwild) {
        char parent[1100];
        if (dstart == dname)
            snprintf(parent, sizeof parent, "@");
        else
            snprintf(parent, sizeof parent, "%.*s", (int)(dstart - dname - (dstart[-1] == '.')),
                     dname);
        if ((e = fsw_resolve(parent, R_WRITE, &ddir, 0)) != NULL)
            return e;
    } else {
        if ((e = fsw_resolve(dname, R_WRITE, &dl, 0)) != NULL)
            return e;
    }
    struct fs_info si;
    if (!swild || !dwild) {
        /* One object; a wild source is its first match */
        child(&sdir, spat, &sl);
        if (swild) {
            if ((e = fsw_resolve(sname, R_READ, &sl, 0)) != NULL)
                return e;
        }
        if ((e = fsw_stat_loc(&sl, &si)) != NULL)
            return e;
        if (si.type == OBJ_NOTHING)
            return fsw_err_not_found(sname);
        if (dwild)
            child(&ddir, fsw_leaf_of(&sl), &dl);
        e = copy_object(&u, &sl, &dl, &si, 1);
    } else {
        unsigned n, matched = 0;
        char **list = leaves(&sdir, &n, &e);
        for (unsigned i = 0; !e && !u.abandoned && i < n; i++) {
            if (!fsw_wild_match(spat, list[i]))
                continue;
            matched++;
            child(&sdir, list[i], &sl);
            child(&ddir, list[i], &dl);
            e = fsw_stat_loc(&sl, &si);
            if (!e)
                e = copy_object(&u, &sl, &dl, &si, 0);
        }
        free_leaves(list, n);
        if (!e && !matched)
            e = ros_error(E_NOTHING_TO_COPY, "Nothing to copy");
    }
    if (e)
        return e;
    if ((u.flags & U_VERBOSE) && !u.abandoned)
        return summary(&u, u.flags & U_DELETESRC ? "moved" : "copied");
    return NULL;
}

/* ---- *Wipe and *Count ------------------------------------------------------------ */

static os_error *wipe_object(struct util *u, const struct fsw_loc *l, const struct fs_info *i)
{
    char name[1100], q[1200];
    fsw_canonical(l, name, sizeof name);
    os_error *e = NULL;
    if (i->attr & ATTR_L) {
        if (!(u->flags & U_FORCE)) {
            u->skipped++;
            return u->flags & U_VERBOSE ? fsw_printf_out("%s is locked\n\r", name) : NULL;
        }
    }
    if (i->type == OBJ_DIR) {
        struct fs_entry first;
        if (!(u->flags & U_RECURSE) && fsw_readdir(l, 0, &first, &e))
            return e;                   /* not empty, and not recursing: left */
        snprintf(q, sizeof q, "Delete directory %s", name);
        if (!confirm(u, q, &e))
            return e;
        unsigned n;
        char **list = leaves(l, &n, &e);
        for (unsigned k = 0; !e && !u->abandoned && k < n; k++) {
            struct fsw_loc c;
            struct fs_info ci;
            child(l, list[k], &c);
            e = fsw_stat_loc(&c, &ci);
            if (!e && ci.type != OBJ_NOTHING)
                e = wipe_object(u, &c, &ci);
        }
        free_leaves(list, n);
        if (e || u->abandoned)
            return e;
    } else {
        snprintf(q, sizeof q, "Delete file %s", name);
        if (!confirm(u, q, &e))
            return e;
    }
    if (i->attr & ATTR_L)
        if ((e = fsw_setinfo(l, 4, 0, 0, i->attr & ~ATTR_L)) != NULL)
            return e;
    if ((e = fsw_remove(l, i)) != NULL)
        return e;
    if (i->type != OBJ_DIR)             /* files, and images */
        u->done++;
    if (u->flags & U_VERBOSE)
        return fsw_printf_out("%s %s deleted\n\r", i->type == OBJ_DIR ? "Directory" : "File",
                              name);
    return NULL;
}

static os_error *count_object(struct util *u, const struct fsw_loc *l, const struct fs_info *i)
{
    char name[1100], size[32];
    os_error *e = NULL;
    fsw_canonical(l, name, sizeof name);
    if (i->type == OBJ_DIR) {
        if (!(u->flags & U_RECURSE))
            return NULL;
        unsigned n;
        char **list = leaves(l, &n, &e);
        for (unsigned k = 0; !e && k < n; k++) {
            struct fsw_loc c;
            struct fs_info ci;
            child(l, list[k], &c);
            e = fsw_stat_loc(&c, &ci);
            if (!e && ci.type != OBJ_NOTHING)
                e = count_object(u, &c, &ci);
        }
        free_leaves(list, n);
        return e;
    }
    u->done++;
    u->bytes += i->length;
    if (u->flags & U_VERBOSE) {
        size_text(i->length, size, sizeof size);
        return fsw_printf_out("File %s counted, %s\n\r", name, size);
    }
    return NULL;
}

/* Each object a possibly-wild name matches: through fn */
static os_error *each(struct util *u, uint32_t addr, int wipe,
                      os_error *(*fn)(struct util *, const struct fsw_loc *, const struct fs_info *))
{
    char name[1024], pattern[256];
    struct fsw_loc dir, l;
    int wild;
    os_error *e = fsw_arg_path(addr, name, sizeof name);
    if (!e)
        e = split_wild(name, &dir, pattern, sizeof pattern, &wild);
    if (e)
        return e;
    struct fs_info info;
    if (!wild) {
        child(&dir, pattern, &l);
        if ((e = fsw_stat_loc(&l, &info)) != NULL)
            return e;
        if (info.type == OBJ_NOTHING)
            return wipe ? ros_error(E_NOTHING_TO_DELETE, "Nothing to delete")
                        : fsw_err_not_found(name);
        return fn(u, &l, &info);
    }
    unsigned n, matched = 0;
    char **list = leaves(&dir, &n, &e);
    for (unsigned k = 0; !e && !u->abandoned && k < n; k++) {
        if (!fsw_wild_match(pattern, list[k]))
            continue;
        matched++;
        child(&dir, list[k], &l);
        e = fsw_stat_loc(&l, &info);
        if (!e && info.type != OBJ_NOTHING)
            e = fn(u, &l, &info);
    }
    free_leaves(list, n);
    if (!e && !matched && wipe)
        e = ros_error(E_NOTHING_TO_DELETE, "Nothing to delete");
    return e;
}

/* OS_FSControl 27: R1 the name, R3 options */
os_error *fsw_wipe(struct ros_cpu *s)
{
    struct util u = { s->r[3], 0, 0, 0, 0, 0, 0 };
    os_error *e = each(&u, s->r[1], 1, wipe_object);
    if (!e && (u.flags & U_VERBOSE) && !u.abandoned)
        e = fsw_printf_out("%u file%s deleted\n\r", u.done, u.done == 1 ? "" : "s");
    return e;
}

/* OS_FSControl 28: R1 the name, R3 options; R2 the bytes, R3 the files */
os_error *fsw_count(struct ros_cpu *s)
{
    struct util u = { s->r[3], 0, 0, 0, 0, 0, 0 };
    os_error *e = each(&u, s->r[1], 0, count_object);
    if (e)
        return e;
    if (u.flags & U_PRINTOK)
        if ((e = summary(&u, "counted")) != NULL)
            return e;
    s->r[2] = u.bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)u.bytes;
    s->r[3] = u.done;
    return NULL;
}

/* ---- running a file ------------------------------------------------------------- */

/* Past a name as the line has it.  The name ends at a space or a control
 * character, and a quoted part is taken whole.  These are the name's own
 * characters, however long its GSTransed form is (a <Var> in it) */
static uint32_t past_name(uint32_t p)
{
    int quoted = 0;
    for (;; p++) {
        uint8_t c = ros_ld8(p);
        if (c < ' ' || (c == ' ' && !quoted))
            return p;
        if (c == '"')
            quoted = !quoted;
    }
}

/* OS_FSControl 4: R1 -> the file's name, then the rest of the line.  That
 * is the command tail, which starts after the name as written, not as
 * translated: %Run <TaskWindow$Server> gives !Edit's !Run no arguments */
os_error *fsw_run(uint32_t line)
{
    /* A file whose run action runs it again, such as an Obey file naming
     * itself, goes as deep as the stacks allow.  Then it fails as FileSwitch
     * does when it has too little SVC stack to call a filing system (task.h) */
    if (!ros_stack_room(ROS_STACK_FILE))
        return ros_error(E_NO_STACK, "Not enough stack to call filing system");
    char name[1024], list[1024], tail[1024], canon[1100];
    uint32_t p = line;
    while (ros_ld8(p) == ' ')
        p++;
    if (ros_ld8(p) < ' ')
        return ros_error(E_BAD_COMMAND, "Command not recognised");
    uint32_t command = p;                   /* the line from the name: an application's GetEnv */
    os_error *e = fsw_arg_path(p, name, sizeof name);
    if (e)
        return e;
    p = past_name(p);
    while (ros_ld8(p) == ' ')
        p++;
    uint32_t tail_at = p;
    size_t n = 0;
    while (ros_ld8(p + n) >= ' ' && n + 1 < sizeof tail)
        n++;
    memcpy(tail, ros_ptr(p), n);
    tail[n] = 0;
    if (!fsw_read_var("Run$Path", list, sizeof list))
        snprintf(list, sizeof list, ",%%.");
    struct fsw_loc l;
    struct fs_info info;
    if ((e = fsw_resolve_via(name, list, R_READ, &l)) != NULL ||
        (e = fsw_stat_loc(&l, &info)) != NULL)
        return e;
    if (info.type == OBJ_NOTHING)
        return fsw_err_not_found(name);
    if (info.type == OBJ_DIR) {
        /* A directory runs its !Run, as FileSwitch's TopPath_TryPlingRun
         * does: an application directory, *Run !Chars or a double-click */
        char run[1100];
        struct fsw_loc rl;
        struct fs_info ri;
        fsw_canonical(&l, canon, sizeof canon);
        snprintf(run, sizeof run, "%s.!Run", canon);
        if (fsw_resolve_via(run, ",", R_READ, &rl) || fsw_stat_loc(&rl, &ri) ||
            ri.type != OBJ_FILE)
            return fsw_err_is_dir(name);
        l = rl, info = ri;
    }
    uint32_t load = info.load;
    if ((load == 0 || load == 0xFFFFFFFFu) && info.exec == 0xFFFFFFFFu)
        load = 0xFFFFFEFFu;                 /* an old command file: Command */
    uint32_t type = load >> 8 & 0xFFF;
    if (fsw_is_typed(load) && type == 0xFF8) {  /* an Absolute: a C application's image? */
        int is_capp;
        fsw_canonical(&l, canon, sizeof canon);
        e = ros_capp_run(canon, command, &is_capp);
        if (is_capp || e)
            return e;
        /* not one: ARM code, which the ARM container runs */
        return ros_armrun_run(canon, command);
    }
    if (fsw_is_typed(load) && type == 0xFFC) {  /* a utility: ARM code, which the ARM container runs */
        fsw_canonical(&l, canon, sizeof canon);
        return ros_armrun_utility(canon, command, tail_at);
    }
    if (!fsw_is_typed(load))
        return ros_error(ROS_ERR_UNIMPLEMENTED, "'%s' is ARM code, which ROSGD does not run",
                         name);
    char var[40], action[512];
    snprintf(var, sizeof var, "Alias$@RunType_%03X", type);
    if (!fsw_read_var(var, action, sizeof action))
        return ros_error(E_UNKNOWN_ACTION, "An application that loads a file of this type has "
                                           "not been found by the Filer. Open a directory "
                                           "display containing the required application and "
                                           "try again.");
    fsw_canonical(&l, canon, sizeof canon);
    /* The command goes on the SVC stack, below the SWIs in progress, and not
     * in the RMA.  What it starts may never come back here. That is an
     * application directory's !Run, whose last line starts the program.  The
     * program's start flattens that stack */
    int len = snprintf(NULL, 0, "@RunType_%03X %s%s%s\r", type, canon, tail[0] ? " " : "",
                       tail);
    uint32_t outer = ros_svc_sp, cmd = (outer - (uint32_t)len - 8) & ~7u;
    snprintf(ros_ptr(cmd), (size_t)len + 1, "@RunType_%03X %s%s%s\r", type, canon,
             tail[0] ? " " : "", tail);
    ros_svc_sp = cmd;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = cmd;
    ros_swi_as(&c, XOS_CLI, ros_caller_kind());    /* as the *Run's kind */
    ros_svc_sp = outer;
    return swi_error(&c);
}

/* ---- *commands ------------------------------------------------------------------ */

static os_error *fsc(uint32_t reason, uint32_t r1, uint32_t r2)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = reason, c.r[1] = r1, c.r[2] = r2;
    ros_swi(&c, XOS_FSControl);
    return swi_error(&c);
}

/* The word after the one at p, or 0 */
static uint32_t next_word(uint32_t p)
{
    while (ros_ld8(p) > ' ')
        p++;
    while (ros_ld8(p) == ' ')
        p++;
    return ros_ld8(p) < ' ' ? 0 : p;
}

#define CMD(fn) static os_error *fn(struct ros_module *m, uint32_t tail, uint32_t argc)
#define UNUSED (void)m, (void)tail, (void)argc

CMD(cmd_access) { UNUSED; return fsc(24, tail, argc > 1 ? next_word(tail) : 0); }
CMD(cmd_back)   { UNUSED; return fsc(40, 0, 0); }
CMD(cmd_cat)    { UNUSED; return fsc(5, argc ? tail : 0, 0); }
CMD(cmd_dir)    { UNUSED; return fsc(0, argc ? tail : 0, 0); }
CMD(cmd_ex)     { UNUSED; return fsc(6, argc ? tail : 0, 0); }
CMD(cmd_fileinfo) { UNUSED; return fsc(32, tail, 0); }
CMD(cmd_info)   { UNUSED; return fsc(9, tail, 0); }
CMD(cmd_lcat)   { UNUSED; return fsc(7, argc ? tail : 0, 0); }
CMD(cmd_lex)    { UNUSED; return fsc(8, argc ? tail : 0, 0); }
CMD(cmd_lib)    { UNUSED; return fsc(1, argc ? tail : 0, 0); }
CMD(cmd_nodir)  { UNUSED; return fsc(43, 0, 0); }
CMD(cmd_nolib)  { UNUSED; return fsc(45, 0, 0); }
CMD(cmd_nourd)  { UNUSED; return fsc(44, 0, 0); }
CMD(cmd_rename) { UNUSED; return fsc(25, tail, next_word(tail)); }
CMD(cmd_run)    { UNUSED; return fsc(4, tail, 0); }
CMD(cmd_shut)   { UNUSED; return fsc(22, 0, 0); }
CMD(cmd_shutdown) { UNUSED; return fsc(23, 0, 0); }
CMD(cmd_urd)    { UNUSED; return fsc(39, argc ? tail : 0, 0); }
CMD(cmd_hostfs) { UNUSED; return fsw_select_fs(0); }
CMD(cmd_resourcefs) { UNUSED; return fsw_select_fs(1); }

CMD(cmd_cdir)
{
    UNUSED;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 8, c.r[1] = tail, c.r[4] = 0;
    ros_swi(&c, XOS_File);
    return swi_error(&c);
}

CMD(cmd_stamp)
{
    UNUSED;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 9, c.r[1] = tail;
    ros_swi(&c, XOS_File);
    return swi_error(&c);
}

/* *SetType <file> <type>: the type a number or a name, which may have
 * spaces in it */
CMD(cmd_settype)
{
    UNUSED;
    uint32_t t = next_word(tail);
    uint32_t buf = ros_addr(ros_rma_alloc(64));
    size_t n = 0;
    while (ros_ld8(t + n) >= ' ' && n < 60)
        n++;
    while (n && ros_ld8(t + n - 1) == ' ')
        n--;
    memcpy(ros_ptr(buf), ros_ptr(t), n);
    ros_st8(buf + n, 0);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 31, c.r[1] = buf;
    ros_swi(&c, XOS_FSControl);
    ros_rma_free(ros_ptr(buf));
    if (c.v)
        return swi_error(&c);
    uint32_t type = c.r[2];
    ros_cpu_enter(&c);
    c.r[0] = 18, c.r[1] = tail, c.r[2] = type;
    ros_swi(&c, XOS_File);
    return swi_error(&c);
}

/* *Up [n]: the parent, n times */
CMD(cmd_up)
{
    UNUSED;
    unsigned n = 1;
    if (argc)
        n = (unsigned)strtoul(ros_ptr(tail), NULL, 10);
    if (n == 0)
        return NULL;
    uint32_t buf = ros_addr(ros_rma_alloc(2 * n + 2));
    char *p = ros_ptr(buf);
    for (unsigned k = 0; k < n; k++) {
        *p++ = '^';
        *p++ = k + 1 < n ? '.' : 0;
    }
    os_error *e = fsc(0, buf, 0);
    ros_rma_free(ros_ptr(buf));
    return e;
}

static const char copy_letters[] = "acdflnpqrstv";
static const uint32_t copy_bits[] = { U_NOATTR, U_CONFIRM, U_DELETESRC, U_FORCE, U_PEEKDEST,
                                      U_NEWER, U_PROMPT, U_QUICK, U_RECURSE, U_RESTAMP,
                                      U_STRUCTURE, U_VERBOSE };
static const char wipe_letters[] = "cfrv";
static const uint32_t wipe_bits[] = { U_CONFIRM, U_FORCE, U_RECURSE, U_VERBOSE };
static const char count_letters[] = "crv";
static const uint32_t count_bits[] = { U_CONFIRM, U_RECURSE, U_VERBOSE };

/* The options: the variable's, then the command's from word p on */
static os_error *options(const char *var, const char *dflt, const char *letters,
                         const uint32_t *bits, uint32_t p, uint32_t *flags)
{
    char v[256], line[256];
    if (!fsw_read_var(var, v, sizeof v))
        snprintf(v, sizeof v, "%s", dflt);
    os_error *e = parse_options(v, letters, bits, flags);
    if (e || !p)
        return e;
    size_t n = 0;
    while (ros_ld8(p + n) >= ' ' && n + 1 < sizeof line)
        n++;
    memcpy(line, ros_ptr(p), n);
    line[n] = 0;
    return parse_options(line, letters, bits, flags);
}

static os_error *utility(uint32_t reason, uint32_t r1, uint32_t r2, uint32_t flags)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = reason, c.r[1] = r1, c.r[2] = r2, c.r[3] = flags;
    c.r[4] = 0, c.r[5] = 0, c.r[6] = 0xFFFFFFFFu, c.r[7] = 0xFF;
    ros_swi(&c, XOS_FSControl);
    return swi_error(&c);
}

CMD(cmd_copy)
{
    UNUSED;
    uint32_t flags = U_PRINTOK | U_CONFIRM | U_VERBOSE;
    uint32_t dst = next_word(tail);
    os_error *e = options("Copy$Options", "A C ~D ~F ~L ~N ~P ~Q ~R ~S ~T V", copy_letters,
                          copy_bits, dst ? next_word(dst) : 0, &flags);
    return e ? e : utility(26, tail, dst, flags);
}

CMD(cmd_wipe)
{
    UNUSED;
    uint32_t flags = U_PRINTOK | U_CONFIRM | U_VERBOSE;
    os_error *e = options("Wipe$Options", "C ~F ~R V", wipe_letters, wipe_bits,
                          next_word(tail), &flags);
    return e ? e : utility(27, tail, 0, flags);
}

CMD(cmd_count)
{
    UNUSED;
    uint32_t flags = U_PRINTOK | U_RECURSE;
    os_error *e = options("Count$Options", "~C R ~V", count_letters, count_bits,
                          next_word(tail), &flags);
    return e ? e : utility(28, tail, 0, flags);
}

#define C(n, min, max, syntax, help, fn) \
    { n, ROS_CMD_INFO(min, max, 0, 0), "Syntax: *" syntax, help, fn }

const struct ros_command fsw_util_commands[] = {
    C("Access", 1, 2, "Access <object> [<access>]",
      "*Access changes the attributes of objects: L lock, W and R the owner's write and "
      "read, and after /, the public's.", cmd_access),
    C("Back", 0, 0, "Back", "*Back swaps the current and previous directories.", cmd_back),
    C("Cat", 0, 1, "Cat [<directory>]",
      "*Cat lists the objects in a directory (default the current directory).", cmd_cat),
    C("CDir", 1, 2, "CDir <directory> [<size in entries>]",
      "*CDir creates a directory.", cmd_cdir),
    C("Copy", 2, 255, "Copy <source spec> <destination spec> [[~]<options>]",
      "*Copy copies objects; options A C D F L N P Q R S T V, and Copy$Options, as "
      "FileSwitch's.", cmd_copy),
    C("Count", 1, 255, "Count <object spec> [[~]<options>]",
      "*Count adds up the sizes of files; options C R V, and Count$Options.", cmd_count),
    C("Dir", 0, 1, "Dir [<directory>]",
      "*Dir selects a directory as the current directory (default the user root "
      "directory).", cmd_dir),
    C("Ex", 0, 1, "Ex [<directory>]",
      "*Ex lists the objects in a directory with their types, dates and sizes.", cmd_ex),
    C("FileInfo", 1, 1, "FileInfo <object spec>",
      "*FileInfo gives the full information on objects.", cmd_fileinfo),
    C("HostFS", 0, 0, "HostFS", "*HostFS selects HostFS as the current filing system.",
      cmd_hostfs),
    C("Info", 1, 1, "Info <object spec>", "*Info gives information on objects.", cmd_info),
    C("LCat", 0, 1, "LCat [<directory>]",
      "*LCat lists the objects in a directory of the library.", cmd_lcat),
    C("LEx", 0, 1, "LEx [<directory>]",
      "*LEx lists the objects in a directory of the library, with their information.",
      cmd_lex),
    C("Lib", 0, 1, "Lib [<directory>]",
      "*Lib selects a directory as the library (default the user root directory).", cmd_lib),
    C("NoDir", 0, 0, "NoDir", "*NoDir unsets the current directory.", cmd_nodir),
    C("NoLib", 0, 0, "NoLib", "*NoLib unsets the library.", cmd_nolib),
    C("NoURD", 0, 0, "NoURD", "*NoURD unsets the user root directory.", cmd_nourd),
    C("Rename", 2, 2, "Rename <object> <new name>", "*Rename renames an object.", cmd_rename),
    C("ResourceFS", 0, 0, "ResourceFS",
      "*ResourceFS selects ResourceFS as the current filing system.", cmd_resourcefs),
    C("Run", 1, 255, "Run <filename> [<parameters>]",
      "*Run runs a file, by its type's Alias$@RunType_xxx.", cmd_run),
    C("SetType", 2, 5, "SetType <filename> <file type>",
      "*SetType sets a file's type, a number or a name.", cmd_settype),
    C("Shut", 0, 0, "Shut", "*Shut closes all open files.", cmd_shut),
    C("ShutDown", 0, 0, "ShutDown", "*ShutDown closes all open files.", cmd_shutdown),
    C("Stamp", 1, 1, "Stamp <filename>", "*Stamp stamps a file with the time now.", cmd_stamp),
    C("Up", 0, 1, "Up [<levels>]", "*Up moves the current directory up the tree.", cmd_up),
    C("URD", 0, 1, "URD [<directory>]", "*URD sets the user root directory.", cmd_urd),
    C("Wipe", 1, 255, "Wipe <object spec> [[~]<options>]",
      "*Wipe deletes objects; options C F R V, and Wipe$Options.", cmd_wipe),
    { NULL, 0, NULL, NULL, NULL },
};
