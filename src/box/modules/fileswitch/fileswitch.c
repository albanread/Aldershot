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
 * (Sources/FileSys/FileSwitch: s.FileSwitch, s.FileSwBody, s.FSControl,
 * s.FSCtrl2, s.FSPath, s.Canonical, s.OSArgs, s.OSBGetBPut, s.OSFile,
 * s.OSFind, s.OSGBPB, s.StreamBits, s.TopPath, s.SysVars, s.FileTypes,
 * s.DirStore).
 */

/* fileswitch.c -- FileSwitch, reimplemented over native filing systems.
 *
 * RISC OS's FileSwitch (FileSys/FileSwitch) owns the file vectors, and so
 * the file SWIs: OS_File, OS_Args, OS_BGet, OS_BPut, OS_GBPB, OS_Find and
 * OS_FSControl (runtime/files.c calls their vectors).  This version is
 * written afresh. It keeps the SWIs' contracts and FileSwitch's error
 * numbers and texts. It runs over two filing systems that are native too
 * (fileswitch.h): HostFS, which is Linux's files, and ResourceFS, which is
 * the ROM's.
 *
 * Names are FileSwitch's:
 *
 *   [-fs-|fs[#special]:|path:][:disc.][$|&|@|%|\][.a.b^.c]
 *
 * The first part is a filing system's name, or a path variable (X$Path)
 * whose places are tried in turn. Then come a disc (HostFS has one for
 * each mount), the root, URD, CSD, Lib or previous directory (else the
 * CSD), and the components, where "^" is the parent.  A reading call
 * matches wildcards to the first object that fits, with # for one
 * character and * for any run.  A writing call refuses them.  Each filing
 * system has its own CSD, Lib, URD and previous directory, as FileSwitch
 * keeps them, and one filing system is the current one.
 *
 * Streams are handles 255 down to 1.  Reading at the end sets C and marks
 * the stream.  Reading again gives "End of file".  Writing past the end
 * extends the file.  Moving the pointer past the end extends it too, when
 * the stream may write, and gives "Outside file" when it may not.
 *
 * There is no buffering: each byte is a read or write of the filing
 * system.  The utilities and the *commands are in fsutils.c. They are the
 * Cat, Ex and Info listings, Access, Copy, Wipe, Count, and running files.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "fileswitch.h"
#include "fsw.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"

#define FILEV 0x08u
#define ARGSV 0x09u
#define BGETV 0x0Au
#define BPUTV 0x0Bu
#define GBPBV 0x0Cu
#define FINDV 0x0Du
#define FSCV  0x0Fu

struct fsw_dirs fsw_dirs[FSW_MAXFS];
int fsw_current = -1;
int fsw_temp = -1;                      /* OS_CLI's "fs:" prefix, until 19 */

#define NSTREAMS 256
struct stream {
    int used;
    int opening;                        /* its handle taken, the file not yet open: not a
                                           stream yet (stream_of), and no one else's */
    int fsi;
    struct fsw_file *f;                 /* the file, on its filing system (modfs.c) */
    uint32_t ptr;
    int rd, wr, eof_pending, modified;
    char name[1100];                    /* canonical, for OS_Args 7 */
};

static struct stream streams[NSTREAMS];



/* ---- small things -------------------------------------------------------------- */

static int fail(struct ros_cpu *s, const os_error *e)
{
    ros_swi_fail(s, e);
    return ROS_VECTOR_CLAIM;
}

const struct fs *fsw_fs_of(const struct fsw_loc *l)
{
    return fsw_filing_systems[l->fsi];
}

uint64_t fsw_now_cs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return ((uint64_t)t.tv_sec + 2208988800ull) * 100 + (uint64_t)t.tv_nsec / 10000000;
}

uint32_t fsw_stamp_load(uint32_t type, uint64_t cs)
{
    return 0xFFF00000u | (type & 0xFFF) << 8 | (uint32_t)(cs >> 32 & 0xFF);
}

int fsw_is_typed(uint32_t load)
{
    return (load >> 20) == 0xFFF;
}

/* A name from the caller, as RISC OS's FileSwitch takes one (TopPath).
 * It goes through OS_GSTrans, which expands <Var>s, leaves | as it is, and
 * stops at a space or a control character.  There are no spaces round it. */
os_error *fsw_arg_name(uint32_t addr, char *out, size_t max)
{
    if (!addr || !ros_arena_readable(addr, addr + 1))
        return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
    const unsigned char *p = ros_ptr(addr);
    size_t n = 0, k = 0;
    while (p[n] == ' ')
        n++;
    while (p[n + k] > ' ' && k + 1 < max)
        k++;
    if (!memchr(p + n, '<', k) && !memchr(p + n, '"', k)) {
        memcpy(out, p + n, k);                  /* nothing to translate */
        out[k] = 0;
        return NULL;
    }
    uint32_t buf = ros_addr(ros_rma_alloc((uint32_t)max + 4));
    if (!buf)
        return ros_error(0x182, "Not enough memory");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = addr, c.r[1] = buf, c.r[2] = ((uint32_t)max - 1) | 1u << 30 | 1u << 29;
    ros_swi(&c, XOS_GSTrans);
    os_error *e = c.v ? ros_ptr(c.r[0]) : NULL;
    if (!e && c.c)
        e = ros_error(0x1E4, "Buffer overflow");
    if (!e) {
        const char *t = ros_ptr(buf);
        size_t len = c.r[2];
        while (len && t[len - 1] == ' ')
            len--;
        while (len && *t == ' ')
            t++, len--;
        memcpy(out, t, len);
        out[len] = 0;
    }
    ros_rma_free(ros_ptr(buf));
    return e;
}

/* PoliceName's classes (FileSwitch s/FSCtrl2): IsBad, IsAbsolute */
static int bad_char(unsigned char c)
{
    return c <= ' ' || c == 127 || c == ':' || c == '|' || c == '"';
}

static int absolute_char(unsigned char c)
{
    return c == '$' || c == '&' || c == '@' || c == '%' || c == '\\';
}

static int alnum_char(unsigned char c)
{
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* FSPath_FindFSPrefix: past a leading fs: or -fs-, a #special allowed in
 * it (LookForAGoodTerm) */
static const char *past_fs_prefix(const char *name)
{
    const unsigned char *q = (const unsigned char *)name, *r;
    unsigned char term = ':';
    if (*q == '-')
        term = '-', q++;
    else if (!alnum_char(*q) && *q != '#')
        return name;
    for (r = q;;) {
        unsigned char c = *r++;
        if (c == term)
            break;
        if (c == '#') {
            do {
                c = *r++;
                if (c <= ' ' || c == ',')
                    return name;
            } while (c != term);
            break;
        }
        if (!alnum_char(c))
            return name;
    }
    return r - q < 2 ? name : (const char *)r;          /* not the term at once */
}

/* PoliceName: a device, components, absolute and parent symbols in their
 * places; 0 if the name conforms */
static int police_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    unsigned char c = *p++;
    if (c == 0)
        return 0;
    if (c == ':') {                                     /* :<device spec> */
        c = *p++;
        if (c == 0 || c == '.' || bad_char(c))
            return -1;
        do {
            c = *p++;
            if (c == 0)
                return 0;
        } while (c != '.' && !bad_char(c));
        if (c != '.')
            return -1;
        c = *p++;
    }
    int symbol = absolute_char(c);                      /* $ & @ % \ only first */
    for (;;) {
        if (symbol || c == '^') {                       /* then a dot, or the end */
            c = *p++;
            if (c != '.')
                return c == 0 ? 0 : -1;
            c = *p++;
            symbol = 0;
            continue;
        }
        if (c == 0 || c == '.' || absolute_char(c) || bad_char(c))
            return -1;                                  /* a component's first */
        do {
            c = *p++;
            if (c == 0)
                return 0;
        } while (c != '.' && c != '$' && !bad_char(c));
        if (c != '.')
            return -1;
        c = *p++;
    }
}

/* A path from the caller, as TopPath takes one: the name, translated, and
 * policed.  Policing refuses spaces, quotes and |, and gives the form
 * PoliceName wants.  (Wildcard patterns, filing system names and variable
 * names are not policed, as on RISC OS.) */
os_error *fsw_arg_path(uint32_t addr, char *out, size_t max)
{
    os_error *e = fsw_arg_name(addr, out, max);
    if (!e && (strpbrk(out, " \"|") || police_name(past_fs_prefix(out))))
        e = ros_error(0xCC, "File name '%s' not recognised", out);
    return e;
}

/* A system variable's value, expanded; 0 if it is unset. */
int fsw_read_var(const char *name, char *out, size_t max)
{
    uint32_t nlen = (uint32_t)strlen(name) + 1;
    uint32_t buf = ros_addr(ros_rma_alloc(nlen + (uint32_t)max));
    if (!buf)
        return 0;
    strcpy(ros_ptr(buf), name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = buf, c.r[1] = buf + nlen, c.r[2] = (uint32_t)max - 1, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    int ok = !c.v;
    if (ok) {
        memcpy(out, ros_ptr(buf + nlen), c.r[2]);
        out[c.r[2]] = 0;
    }
    ros_rma_free(ros_ptr(buf));
    return ok;
}

os_error *fsw_write_s(const char *s)
{
    for (; *s; s++) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = (uint8_t)*s;
        ros_swi(&c, XOS_WriteC);
        if (c.v)
            return ros_ptr(c.r[0]);
    }
    return NULL;
}

os_error *fsw_printf_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
os_error *fsw_printf_out(const char *fmt, ...)
{
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return fsw_write_s(buf);
}

/* ---- wildcards --------------------------------------------------------------- */

int fsw_has_wild(const char *s)
{
    return strpbrk(s, "#*") != NULL;
}

static int fold(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

int fsw_wild_match(const char *pat, const char *s)
{
    /* One place to go back to, the last "*": the run it matches is made one
     * character longer each time the rest of the pattern fails.  Trying
     * every split of a pattern with many stars would take exponential time. */
    const char *star = NULL, *mark = NULL;
    for (;;) {
        if (*pat == '*') {
            while (*pat == '*')
                pat++;
            if (!*pat)
                return 1;
            star = pat, mark = s;
            continue;
        }
        if (!*pat && !*s)
            return 1;
        if (*pat && *s && (*pat == '#' || fold((unsigned char)*pat) == fold((unsigned char)*s))) {
            pat++, s++;
            continue;
        }
        if (!star || !*mark)
            return 0;
        pat = star, s = ++mark;
    }
}

/* ---- places ------------------------------------------------------------------ */

struct fsw_dirs *fsw_dirs_of(int fsi)
{
    struct fsw_dirs *d = &fsw_dirs[fsi];
    const struct fs *fs = fsw_filing_systems[fsi];
    if (!d->ready || (fs->has_discs && !fs->disc_exists(d->csd.disc))) {
        memset(d, 0, sizeof *d);
        d->csd.fsi = fsi;
        snprintf(d->csd.disc, sizeof d->csd.disc, "%s", fs->has_discs ? fs->boot_disc() : "");
        d->lib = d->urd = d->psd = d->csd;
        d->ready = fs->has_discs ? d->csd.disc[0] != 0 : 1;
    }
    return d;
}

void fsw_canonical(const struct fsw_loc *l, char *out, size_t max)
{
    const struct fs *fs = fsw_fs_of(l);
    if (fs->has_discs)
        snprintf(out, max, "%s::%s.$%s%s", fs->name, l->disc, l->path[0] ? "." : "", l->path);
    else
        snprintf(out, max, "%s:$%s%s", fs->name, l->path[0] ? "." : "", l->path);
}

const char *fsw_leaf_of(const struct fsw_loc *l)
{
    const char *dot = strrchr(l->path, '.');
    return dot ? dot + 1 : l->path[0] ? l->path : "$";
}

/* A filing system's information word, as FileSwitch keeps it (fscb_info):
 * its number, then open files and the fsinfo_ flags (hdr/LowFSI) */
#define FSINFO_READONLY (1u << 16)
#define FSINFO_DONTUSESAVE (1u << 19)

uint32_t fsw_info_word(int fsi)
{
    const struct fs *fs = fsw_filing_systems[fsi];
    return (fs->number & 0xFFu) | fs->info | (fs->read_only ? FSINFO_READONLY : 0);
}

/* What OS_FSControl 13 gives for a filing system, where RISC OS gives its
 * control block: a block of FileSwitch's own in the RMA, FSCB bytes each.
 * The word at +0 is the filing system's number (Free's *ShowFree and
 * *Configure FileSystem only test R2 for 0); the word at +32 is where
 * FileSwitch's control block keeps the information word (fscb_info,
 * s/FSCtrl2), which the Filer reads there (fscb_infoword, s/CacheDir) for
 * each viewer's filing system: its number to match UpCall_ModifyingFile's
 * R8, and fsinfo_readonly to shade its menus. */
#define FSCB 64u
static uint32_t fs_blocks;

static int fs_blocks_stale = 1;

void fsw_fs_blocks_changed(void)
{
    fs_blocks_stale = 1;
}

static uint32_t fs_block(int fsi)
{
    if (!fs_blocks) {
        fs_blocks = ros_addr(ros_rma_alloc(FSCB * FSW_MAXFS));
        if (!fs_blocks)
            return 0;
        fs_blocks_stale = 1;
    }
    if (fs_blocks_stale) {
        memset(ros_ptr(fs_blocks), 0, FSCB * FSW_MAXFS);
        for (int i = 0; i < FSW_MAXFS; i++)
            if (fsw_filing_systems[i]) {
                ros_st32(fs_blocks + FSCB * (uint32_t)i, fsw_filing_systems[i]->number);
                ros_st32(fs_blocks + FSCB * (uint32_t)i + 32, fsw_info_word(i));
            }
        fs_blocks_stale = 0;
    }
    return fs_blocks + FSCB * (uint32_t)fsi;
}

/* ---- UpCall_ModifyingFile ------------------------------------------------------- */

/* FileSwitch tells the machine before each call that changes a filing
 * system.  It does this with OS_UpCall 3, UpCall_ModifyingFile, from
 * DoUpCallModifyingFile (s/LowLevel).  CallFSFile, CallFSFunc (rename,
 * access), CallFSOpen (for update), CallFSArgs (ensure) and CallFSClose (a
 * modified stream) all call that routine.
 *
 * R9 says what happened (hdr/UpCall, the upfs_ codes, below).  R8 is the
 * filing system's information word.  R1 is the object's path without the
 * filing system and special field (":Host.$.a.b" on HostFS, "$.a.b" on
 * ResourceFS).  For a rename, R2 is the new path.  For a close or an
 * ensure, R1 is the stream's handle instead.  R6 and R7 are the special
 * field, which is 0 here.  R2-R5 are otherwise what FileSwitch hands the
 * filing system.
 *
 * The Filer sits on UpCallV (Filer s/UpCall) and marks the viewers that the
 * change touches to be read again.  Without this call its windows never
 * showed a change. */
static void upcall_modifying(uint32_t reason, int fsi, const uint32_t r[6])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 3;                             /* UpCall_ModifyingFile */
    for (int i = 1; i <= 5; i++)
        c.r[i] = r[i];
    c.r[6] = 0, c.r[7] = 0;
    c.r[8] = fsw_info_word(fsi);
    c.r[9] = reason;
    ros_swi(&c, XOS_UpCall);
}

/* The path FileSwitch passes a filing system: the canonical name past its
 * "fs:" */
static uint32_t upcall_path(const struct fsw_loc *l)
{
    char full[1100];
    fsw_canonical(l, full, sizeof full);
    const char *p = strchr(full, ':');
    p = p ? p + 1 : full;
    size_t n = strlen(p) + 1;
    uint32_t a = ros_addr(ros_rma_alloc((uint32_t)n));
    if (a)
        memcpy(ros_ptr(a), p, n);
    return a;
}

void fsw_modifying(uint32_t reason, const struct fsw_loc *l, uint32_t r2, uint32_t r3,
                   uint32_t r4, uint32_t r5)
{
    uint32_t r[6] = { 0, upcall_path(l), r2, r3, r4, r5 };
    if (!r[1])
        return;
    upcall_modifying(reason, l->fsi, r);
    ros_rma_free(ros_ptr(r[1]));
}

/* The changes, each told first, as FileSwitch makes them */
os_error *fsw_create(const struct fsw_loc *l, uint32_t load, uint32_t exec, uint32_t start,
                     uint32_t end)
{
    fsw_modifying(UPFS_CREATE, l, load, exec, start, end);
    return fsw_fs_create(l, load, exec, end - start);
}

os_error *fsw_mkdir(const struct fsw_loc *l)
{
    fsw_modifying(UPFS_CREATE_DIR, l, 0, 0, 0, 0);
    return fsw_fs_mkdir(l);
}

os_error *fsw_remove(const struct fsw_loc *l, const struct fs_info *i)
{
    fsw_modifying(UPFS_DELETE, l, i->load, i->exec, i->length, i->attr);
    return fsw_fs_remove(l);
}

/* OS_File 1-4's reasons.  FileSwitch hands every one of them to the filing
 * system as WriteInfo, the other two values as they were (s/OSFile,
 * WriteInfoFilePresent), and so the UpCall says WriteInfo. */
os_error *fsw_setinfo(const struct fsw_loc *l, int reason, uint32_t load, uint32_t exec,
                      uint32_t attr)
{
    struct fs_info i = { 0 };
    fsw_stat_loc(l, &i);
    fsw_modifying(UPFS_WRITE_INFO, l, reason == 1 || reason == 2 ? load : i.load,
                  reason == 1 || reason == 3 ? exec : i.exec, i.length,
                  reason == 1 || reason == 4 ? attr : i.attr);
    return fsw_fs_setinfo(l, reason, load, exec, attr);
}

static os_error *rename_object(const struct fsw_loc *from, const struct fsw_loc *to)
{
    uint32_t r[6] = { 0, upcall_path(from), upcall_path(to) };
    if (r[1] && r[2])
        upcall_modifying(UPFS_RENAME, from->fsi, r);
    if (r[1])
        ros_rma_free(ros_ptr(r[1]));
    if (r[2])
        ros_rma_free(ros_ptr(r[2]));
    return fsw_fs_rename(from, to);
}

static int fs_by_name(const char *name, size_t n)
{
    for (int i = 0; i < FSW_MAXFS; i++)
        if (fsw_filing_systems[i] && strlen(fsw_filing_systems[i]->name) == n &&
            strncasecmp(fsw_filing_systems[i]->name, name, n) == 0)
            return i;
    /* ResourceFS answers to its module's name too */
    if (n == 10 && strncasecmp(name, "ResourceFS", 10) == 0)
        return 1;
    return -1;
}

static int fs_by_number(uint32_t n)
{
    for (int i = 0; i < FSW_MAXFS; i++)
        if (fsw_filing_systems[i] && fsw_filing_systems[i]->number == n)
            return i;
    return -1;
}

os_error *fsw_err_bad_name(const char *name)
{
    return ros_error(E_BAD_FILE_NAME, "File name '%s' not recognised", name);
}

/* Append a component; "^" goes up. */
static os_error *step(struct fsw_loc *l, const char *comp, const char *name)
{
    if (strcmp(comp, "^") == 0) {
        char *dot = strrchr(l->path, '.');
        if (dot)
            *dot = 0;
        else
            l->path[0] = 0;
        return NULL;
    }
    size_t n = strlen(l->path);
    if (n + 1 + strlen(comp) + 1 > sizeof l->path)
        return fsw_err_bad_name(name);
    if (n)
        l->path[n++] = '.';
    strcpy(l->path + n, comp);
    return NULL;
}

/* The first object in l's directory that fits the wildcard, as its name. */
static void match_wild(const struct fsw_loc *l, const char *pat, char *out, size_t max)
{
    struct fs_entry e;
    os_error *err;
    for (uint32_t i = 0; fsw_readdir(l, i, &e, &err); i++)
        if (fsw_wild_match(pat, e.name)) {
            snprintf(out, max, "%s", e.name);
            return;
        }
    snprintf(out, max, "%s", pat);
}

os_error *fsw_resolve(const char *name, int mode, struct fsw_loc *out, int depth);

/* Each place in a comma-separated list, the rest of the name after it:
 * the first that holds an object, or for a write, the first.  With no
 * rest ("Boot:"), a place is the directory itself, without its "." */
static os_error *resolve_list(const char *list, const char *rest, int mode, struct fsw_loc *out,
                              int depth)
{
    os_error *first_err = NULL;
    int have = 0;
    struct fsw_loc first;
    for (const char *p = list;;) {
        while (*p == ' ')
            p++;
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!*rest && len > 1 && p[len - 1] == '.')
            len--;
        char candidate[1100];
        struct fsw_loc l;
        snprintf(candidate, sizeof candidate, "%.*s%s", (int)len, p, rest);
        os_error *e = fsw_resolve(candidate, mode, &l, depth + 1);
        if (e) {
            if (!first_err)
                first_err = e;
        } else {
            struct fs_info info;
            if (mode == R_WRITE || (!fsw_stat_loc(&l, &info) && info.type != OBJ_NOTHING)) {
                *out = l;
                return NULL;
            }
            if (!have)
                first = l, have = 1;
        }
        if (!end)
            break;
        p = end + 1;
    }
    if (have) {
        *out = first;
        return NULL;
    }
    return first_err ? first_err : fsw_err_bad_name(rest);
}

os_error *fsw_resolve(const char *name, int mode, struct fsw_loc *out, int depth)
{
    if (depth > 8)
        return ros_error(E_RECURSIVE_PATH, "Path variable '%s' refers to itself", name);
    const char *p = name;
    int fsi = -1;
    char special[32] = "";              /* fs#special: the field, for a filing system
                                           with no discs (SystemDevices' source#zero:) */
    if (*p == '-') {
        const char *q = strchr(p + 1, '-');
        if (!q)
            return fsw_err_bad_name(name);
        fsi = fs_by_name(p + 1, (size_t)(q - p - 1));
        if (fsi < 0)
            return ros_error(E_UNKNOWN_FS, "Filing system %.*s: not present", (int)(q - p - 1),
                             p + 1);
        p = q + 1;
    } else {
        const char *colon = strchr(p, ':');
        const char *dot = strchr(p, '.');
        if (colon && colon > p && (!dot || colon < dot)) {
            size_t n = (size_t)(colon - p);
            const char *hash = memchr(p, '#', n);
            fsi = fs_by_name(p, hash ? (size_t)(hash - p) : n);
            if (hash && fsi >= 0 && (size_t)(colon - hash - 1) < sizeof special)
                memcpy(special, hash + 1, (size_t)(colon - hash - 1));
            if (fsi < 0) {
                if (hash)
                    return ros_error(E_UNKNOWN_FS, "Filing system %.*s: not present",
                                     (int)(hash - p), p);
                char var[300], value[1024];
                snprintf(var, sizeof var, "%.*s$Path", (int)n, p);
                if (!fsw_read_var(var, value, sizeof value))
                    return ros_error(E_UNKNOWN_FS, "Filing system or path %.*s: not present",
                                     (int)n, p);
                return resolve_list(value, colon + 1, mode, out, depth);
            }
            p = colon + 1;
        }
    }
    if (fsi < 0) {
        fsi = fsw_temp >= 0 ? fsw_temp : fsw_current;
        if (fsi < 0)
            return ros_error(E_NO_SELECTED_FS, "No selected filing system");
    }
    const struct fs *fs = fsw_filing_systems[fsi];
    struct fsw_dirs *d = fsw_dirs_of(fsi);
    struct fsw_loc l;
    int anchored = 1;
    if (*p == ':' && fs->has_discs) {
        const char *e = ++p;
        while (*e && *e != '.')
            e++;
        if (e == p || (size_t)(e - p) >= sizeof l.disc)
            return fsw_err_bad_name(name);
        memset(&l, 0, sizeof l);
        l.fsi = fsi;
        memcpy(l.disc, p, (size_t)(e - p));
        if (!fs->disc_exists(l.disc))
            return ros_error(0x10000u | fs->number << 8 | 0xD3u, "Disc '%s' not found", l.disc);
        p = e;
        if (*p == '.') {
            p++;
            if (*p != '$')
                return ros_error(E_BAD_FILE_NAME, "Disc was specified, but absolute wasn't $");
        }
        if (*p == '$')
            p++;
    } else if (*p == '$') {
        l = d->csd;
        l.path[0] = 0;
        p++;
    } else if (*p == '&') {
        l = d->urd, p++;
    } else if (*p == '@') {
        l = d->csd, p++;
    } else if (*p == '%') {
        l = d->lib, p++;
    } else if (*p == '\\') {
        l = d->psd, p++;
    } else {
        l = d->csd;
        anchored = 0;
    }
    if (fs->has_discs && !l.disc[0])
        return ros_error(E_NO_SELECTED_FS, "No selected filing system");
    if (!fs->has_discs)                 /* no disc to name: the special field goes there */
        snprintf(l.disc, sizeof l.disc, "%s", special);
    if (anchored) {
        if (*p == '.')
            p++;
        else if (*p)
            return fsw_err_bad_name(name);
    }
    while (*p) {
        const char *e = strchr(p, '.');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char comp[256];
        if (n == 0 || n >= sizeof comp || (e && !e[1]))
            return fsw_err_bad_name(name);
        memcpy(comp, p, n);
        comp[n] = 0;
        if (fsw_has_wild(comp)) {
            if (mode == R_WRITE)
                return ros_error(E_WILDCARDS, "'%s' contains wild cards", name);
            char found[256];
            match_wild(&l, comp, found, sizeof found);
            snprintf(comp, sizeof comp, "%s", found);
        }
        os_error *err = step(&l, comp, name);
        if (err)
            return err;
        p = e ? e + 1 : p + n;
    }
    *out = l;
    return NULL;
}

/* A name read through a list of places, which is a path string or a
 * variable's value, when it is relative.  When it is not relative, the name
 * is used as it stands. */
static int is_relative(const char *name)
{
    if (strchr("$&@%\\-:", name[0]))
        return 0;
    const char *colon = strchr(name, ':'), *dot = strchr(name, '.');
    return !(colon && (!dot || colon < dot));
}

os_error *fsw_resolve_via(const char *name, const char *list, int mode, struct fsw_loc *out)
{
    if (!name[0])
        return fsw_err_bad_name(name);
    if (list && list[0] && is_relative(name))
        return resolve_list(list, name, mode, out, 0);
    return fsw_resolve(name, mode, out, 0);
}

/* The places for a call's path argument: kind 0 File$Path, 1 a path string
 * at arg, 2 a path variable named at arg, 3 none. */
os_error *fsw_path_list(int kind, uint32_t arg, char *out, size_t max)
{
    char var[260];
    out[0] = 0;
    switch (kind) {
    case 0:
        if (!fsw_read_var("File$Path", out, max))
            out[0] = 0;
        return NULL;
    case 1:
        if (!arg)
            return NULL;
        if (!ros_arena_readable(arg, arg + 1))
            return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
        snprintf(out, max, "%s", (const char *)ros_ptr(arg));
        for (char *p = out; *p; p++)
            if ((unsigned char)*p < ' ') {
                *p = 0;
                break;
            }
        return NULL;
    case 2: {
        char name[250];
        os_error *e = fsw_arg_name(arg, name, sizeof name);
        if (e)
            return e;
        snprintf(var, sizeof var, "%s$Path", name);
        if (!fsw_read_var(var, out, max))
            return ros_error(E_UNKNOWN_FS, "Filing system or path %s: not present", name);
        return NULL;
    }
    default:
        return NULL;
    }
}

os_error *fsw_err_not_found(const char *name)
{
    return ros_error(E_FILE_NOT_FOUND, "File '%s' not found", name);
}

os_error *fsw_err_is_dir(const char *name)
{
    return ros_error(E_IS_A_DIRECTORY, "'%s' is a directory", name);
}

/* The file type FileSwitch gives an object (OS_FSControl 38) */
uint32_t fsw_file_type_of(const char *leaf, const struct fs_info *i)
{
    if (i->type == OBJ_NOTHING)         /* neither a file nor a directory: -1 */
        return 0xFFFFFFFFu;
    if (i->type & OBJ_DIR)              /* an image too (InfoToFileType) */
        return leaf[0] == '!' ? 0x2000 : 0x1000;
    /* an untyped file is -1, as InfoToFileType's MOVNE r2,#-1 makes it */
    return fsw_is_typed(i->load) ? (i->load >> 8 & 0xFFF) : 0xFFFFFFFFu;
}

/* ---- streams ----------------------------------------------------------------- */

static struct stream *stream_of(uint32_t h)
{
    return h > 0 && h < NSTREAMS && streams[h].used && !streams[h].opening ? &streams[h] : NULL;
}

/* FileSwitch's ErrorBlock_Channel: its token, Channel, looked up in the
 * Global messages as MakeInternatErrorBlock's errors are */
static os_error *err_channel(void)
{
    return ros_error(E_CHANNEL, "Handle is either illegal or has been closed");
}

static uint32_t extent_of(const struct stream *st)
{
    return fsw_file_extent(st->f);
}

/* The UpCall for a stream (a close, an ensure): R1 its handle, still open,
 * for the client to read its name (the Filer's UpCallNeedsPathname) */
static void upcall_stream(uint32_t reason, struct stream *st, uint32_t r2, uint32_t r3)
{
    uint32_t r[6] = { 0, (uint32_t)(st - streams), r2, r3 };
    upcall_modifying(reason, st->fsi, r);
}

/* FlushAndCloseStream: an image's file closes what is open inside it
 * first, then tells its image filing system */
static os_error *close_stream(struct stream *st)
{
    uint32_t h = (uint32_t)(st - streams);
    int img = fsw_image_of_stream(h);
    if (img >= 0) {
        for (int k = 1; k < NSTREAMS; k++)
            if (streams[k].used && !streams[k].opening && fsw_file_image(streams[k].f) == img)
                close_stream(&streams[k]);
        fsw_image_closing(img);
    }
    if (st->modified)                   /* FileSwitch's CallFSClose: modified only */
        upcall_stream(UPFS_CLOSE, st, 0, 0);
    os_error *e = fsw_file_close(st->f, st->modified);
    st->used = 0;
    st->f = NULL;
    return e;
}

/* Every stream on filing system fsi (all of them for -1): CloseAllFilesOnThisFS */
void fsw_close_all_on(int fsi, os_error **first)
{
    for (int h = 1; h < NSTREAMS; h++)
        if (streams[h].used && !streams[h].opening && (fsi < 0 || streams[h].fsi == fsi)) {
            os_error *e = close_stream(&streams[h]);
            if (e && first && !*first)
                *first = e;
        }
}

os_error *fsw_close_all(void)
{
    os_error *first = NULL;
    fsw_close_all_on(-1, &first);
    return first;
}

/* An image's file, opened as FileSwitch opens one (OpenMultiFSFile): a
 * stream of its own, for update (or for reading) */
os_error *fsw_stream_open(const struct fsw_loc *l, int update, uint32_t *out)
{
    int h = NSTREAMS - 1;
    while (h > 0 && streams[h].used)
        h--;
    if (h == 0)
        return ros_error(E_TOO_MANY_OPEN, "Too many open files");
    struct stream *st = &streams[h];
    memset(st, 0, sizeof *st);
    st->used = st->opening = 1;
    if (update)
        fsw_modifying(UPFS_OPEN_UPDATE, l, (uint32_t)h, (uint32_t)h, 0, 0);
    os_error *e = fsw_file_open(l, update, (uint32_t)h, &st->f);
    if (e) {
        st->used = st->opening = 0;
        return e;
    }
    st->opening = 0;
    st->fsi = l->fsi;
    st->rd = 1;
    st->wr = update;
    fsw_canonical(l, st->name, sizeof st->name);
    *out = (uint32_t)h;
    return NULL;
}

os_error *fsw_stream_close(uint32_t h)
{
    struct stream *st = stream_of(h);
    return st ? close_stream(st) : NULL;
}

uint32_t fsw_stream_bufsize(uint32_t h)
{
    struct stream *st = stream_of(h);
    return st ? fsw_file_bufsize(st->f) : 0;
}

int fsw_stream_files_inside(int img)
{
    int n = 0;
    for (int k = 1; k < NSTREAMS; k++)
        if (streams[k].used && !streams[k].opening && fsw_file_image(streams[k].f) == img &&
            fsw_image_of_stream((uint32_t)k) < 0)
            n++;
    return n;
}

/* Move the pointer: past the end extends a stream that may write. */
static os_error *set_ptr(struct stream *st, uint32_t ptr)
{
    uint32_t ext = extent_of(st);
    if (ptr > ext) {
        if (!st->wr)
            return ros_error(E_OUTSIDE_FILE, "Outside file");
        os_error *e = fsw_file_set_extent(st->f, ptr);
        if (e)
            return e;
    }
    st->ptr = ptr;
    st->eof_pending = 0;
    return NULL;
}

static os_error *stream_write(struct stream *st, const void *buf, uint32_t n)
{
    if (!st->wr)
        return ros_error(E_NOT_UPDATE, "Not open for update");
    os_error *e = fsw_file_write(st->f, st->ptr, buf, n);
    if (e)
        return e;
    st->ptr += n;
    st->eof_pending = 0;
    st->modified = 1;
    return NULL;
}

static os_error *stream_read(struct stream *st, void *buf, uint32_t n, uint32_t *got)
{
    if (!st->rd)
        return ros_error(E_NOT_READING, "Not open for reading");
    os_error *e = fsw_file_read(st->f, st->ptr, buf, n, got);
    if (e)
        return e;
    st->ptr += *got;
    return NULL;
}

/* ---- OS_Find ------------------------------------------------------------------- */

static int find_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t r0 = s->r[0];
    if ((r0 & 0xC0) == 0) {
        if (s->r[1] == 0) {
            os_error *e = fsw_close_all();
            return e ? fail(s, e) : ROS_VECTOR_CLAIM;
        }
        struct stream *st = stream_of(s->r[1]);
        if (!st)
            return fail(s, err_channel());
        os_error *e = close_stream(st);
        return e ? fail(s, e) : ROS_VECTOR_CLAIM;
    }
    if (r0 & 0x30)
        return fail(s, ros_error(E_BAD_FIND_MODE, "Bad mode for OSFind"));
    char name[1024], list[1024];
    struct fsw_loc l;
    struct fs_info info;
    int out = (r0 & 0xC0) == 0x80, up = (r0 & 0xC0) == 0xC0;
    os_error *e = fsw_arg_path(s->r[1], name, sizeof name);
    if (!e)
        e = fsw_path_list(r0 & 3, s->r[2], list, sizeof list);
    if (!e)
        e = fsw_resolve_via(name, list, out ? R_WRITE : R_READ, &l);
    if (!e)
        e = fsw_stat_loc(&l, &info);
    if (e)
        return fail(s, e);
    if (info.type == OBJ_DIR)
        return fail(s, fsw_err_is_dir(name));
    int existed = info.type != OBJ_NOTHING;
    if (out && !existed) {
        /* FileSwitch creates only what is not there (TryToOpenFile); what
         * is, it opens for update with the extent 0, its load and exec
         * addresses and attributes kept (below) */
        uint64_t cs = fsw_now_cs();
        e = fsw_create(&l, fsw_stamp_load(0xFFD, cs), (uint32_t)cs, 0, 0);
        if (e)
            return fail(s, e);
    } else if (!out && !existed) {
        if (r0 & 0x08)
            return fail(s, fsw_err_not_found(name));
        s->r[0] = 0;
        return ROS_VECTOR_CLAIM;
    }
    int h = NSTREAMS - 1;
    while (h > 0 && streams[h].used)
        h--;
    if (h == 0)
        return fail(s, ros_error(E_TOO_MANY_OPEN, "Too many open files"));
    /* The handle is taken before the UpCall, as FileSwitch allocates the
     * stream first (DoTheOpen_Common: AllocateStream, then TryToOpenFile):
     * a client that opens a file during the UpCall gets another */
    struct stream *st = &streams[h];
    memset(st, 0, sizeof *st);
    st->used = st->opening = 1;
    /* OpenOut and OpenUp are both opens for update to the filing system */
    if (out || up)
        fsw_modifying(UPFS_OPEN_UPDATE, &l, (uint32_t)h, (uint32_t)h, 0, 0);
    struct fsw_file *f;
    e = fsw_file_open(&l, out || up, (uint32_t)h, &f);
    if (!e && out && existed) {
        e = fsw_file_set_extent(f, 0);
        if (e)
            fsw_file_close(f, 0);
    }
    if (e) {
        st->used = st->opening = 0;
        return fail(s, e);
    }
    st->opening = 0;
    st->fsi = l.fsi;
    st->f = f;
    st->rd = 1;
    st->wr = out || up;
    st->modified = out;                 /* OpenOut: stamped, and told, at the close */
    fsw_canonical(&l, st->name, sizeof st->name);
    s->r[0] = (uint32_t)h;
    return ROS_VECTOR_CLAIM;
}

/* ---- OS_BGet, OS_BPut ------------------------------------------------------------ */

static int bget_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct stream *st = stream_of(s->r[1]);
    if (!st)
        return fail(s, err_channel());
    if (st->eof_pending)
        return fail(s, ros_error(E_END_OF_FILE, "End of file"));
    uint8_t b;
    uint32_t got;
    os_error *e = stream_read(st, &b, 1, &got);
    if (e)
        return fail(s, e);
    if (got == 0) {
        st->eof_pending = 1;
        s->r[0] = 0xFFFFFFFFu;
        s->c = 1;
    } else {
        s->r[0] = b;
        s->c = 0;
    }
    return ROS_VECTOR_CLAIM;
}

static int bput_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct stream *st = stream_of(s->r[1]);
    if (!st)
        return fail(s, err_channel());
    uint8_t b = (uint8_t)s->r[0];
    os_error *e = stream_write(st, &b, 1);
    return e ? fail(s, e) : ROS_VECTOR_CLAIM;
}

/* ---- OS_Args ------------------------------------------------------------------- */

#define SCB_READ 0x40u
#define SCB_WRITE 0x80u
#define SCB_MODIFIED 0x100u
#define SCB_EOF_PENDING 0x200u
#define SCB_UNBUFFERED 0x400u
#define SCB_UNALLOCATED 0x800u

static int args_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t reason = s->r[0];
    if (reason == 0 && s->r[1] == 0) {
        s->r[0] = fsw_current < 0 ? 0 : fsw_filing_systems[fsw_current]->number;
        return ROS_VECTOR_CLAIM;
    }
    if (reason == 0xFF) {                   /* ensure: nothing held back */
        os_error *e = NULL;
        if (s->r[1] && !stream_of(s->r[1]))
            return fail(s, err_channel());
        for (int h = s->r[1] ? (int)s->r[1] : 1; h < NSTREAMS; h++) {
            if (streams[h].used && !streams[h].opening && !e)
                e = fsw_file_flush(streams[h].f);
            if (s->r[1])
                break;
        }
        return e ? fail(s, e) : ROS_VECTOR_CLAIM;
    }
    struct stream *st = stream_of(s->r[1]);
    if (reason == 0xFE) {
        if (!st) {
            s->r[0] = SCB_UNALLOCATED;
            s->r[2] = 0;
        } else {
            s->r[0] = (st->rd ? SCB_READ : 0) | (st->wr ? SCB_WRITE : 0) |
                      (st->modified ? SCB_MODIFIED : 0) | (st->eof_pending ? SCB_EOF_PENDING : 0) |
                      SCB_UNBUFFERED;
            s->r[2] = fsw_filing_systems[st->fsi] ? fsw_filing_systems[st->fsi]->number : 0;
        }
        return ROS_VECTOR_CLAIM;
    }
    if (reason > 9)
        return fail(s, ros_error(E_BAD_OSARGS, "Bad OSArgs call"));
    if (!st)
        return fail(s, err_channel());
    os_error *e = NULL;
    uint32_t ext;
    switch (reason) {
    case 0:
        s->r[2] = st->ptr;
        break;
    case 1:
        e = set_ptr(st, s->r[2]);
        break;
    case 2:
        s->r[2] = extent_of(st);
        break;
    case 3:
        if (!st->wr) {
            e = ros_error(E_NOT_UPDATE, "Not open for update");
            break;
        }
        e = fsw_file_set_extent(st->f, s->r[2]);
        if (!e && st->ptr > s->r[2])
            st->ptr = s->r[2];
        if (!e)                         /* not modified: FileSwitch sets that on writes
                                           only (an unbuffered stream's SetEXT is the
                                           FS's, s/OSArgs), so a close says nothing */
            st->eof_pending = 0;
        break;
    case 4:
        s->r[2] = extent_of(st);
        break;
    case 5:
        s->r[2] = st->ptr == extent_of(st) ? 0xFFFFFFFFu : 0;
        break;
    case 6:                             /* an unbuffered stream's ensure: the FS's */
        upcall_stream(UPFS_ENSURE_SIZE, st, s->r[2], 0);
        ext = extent_of(st);
        if (s->r[2] < ext)
            s->r[2] = ext;
        break;
    case 7: {
        uint32_t len = (uint32_t)strlen(st->name) + 1;
        if (s->r[2] && s->r[5] >= len) {
            if (!ros_arena_valid(s->r[2], s->r[2] + len)) {
                e = ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
                break;
            }
            memcpy(ros_ptr(s->r[2]), st->name, len);
        }
        s->r[5] -= len;
        break;
    }
    case 8:
        break;
    case 9:
        e = ros_error(E_UNSUPPORTED, "Filing system does not support this operation");
        break;
    }
    return e ? fail(s, e) : ROS_VECTOR_CLAIM;
}

/* ---- OS_File ------------------------------------------------------------------- */

static os_error *err_file_on_dir(const char *name)
{
    return ros_error(E_TYPES_DONT_MATCH,
                     "'%s' cannot be created - a directory with that name already exists", name);
}

/* An object's catalogue information in R0, R2-R5.  When there is nothing
 * there, R0 is 0 and R2-R5 are not what the caller passed in.  RISC OS
 * 5.30's FileSwitch gives back whatever its filing system left (code and
 * stack addresses, R5 0: OS_File 5, 17 on HostFS and ResourceFS on the
 * farm).  Callers read them.  Filer_Action's Newer test
 * (test_add_to_read_list) takes a missing destination's R2 and R3 as its
 * date, so with its own source's date left in the block it skipped every
 * file copied anywhere new.  Here they are 0: no date stamp, nothing. */
static void set_info_regs(struct ros_cpu *s, const struct fs_info *i)
{
    s->r[0] = i->type;
    if (i->type == OBJ_NOTHING) {
        s->r[2] = s->r[3] = s->r[4] = s->r[5] = 0;
        return;
    }
    s->r[2] = i->load;
    s->r[3] = i->exec;
    s->r[4] = i->length;
    s->r[5] = i->attr;
}

/* Create a file and write its data: OS_File 0 and 10. */
static os_error *save(const struct fsw_loc *l, uint32_t load, uint32_t exec, uint32_t start,
                      uint32_t end)
{
    if (end < start)
        return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
    if (end > start && !ros_arena_readable(start, end))
        return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
    /* FileSwitch's int_DoSaveFile: a filing system that opts out of saves
     * (HostFS), or an image's, is given a create and an open for update.
     * The stream is closed unmodified, and the filing system is not told.
     * Any other filing system is given a save */
    if (fsw_info_word(l->fsi) & FSINFO_DONTUSESAVE) {
        fsw_modifying(UPFS_CREATE, l, load, exec, start, end);
        fsw_modifying(UPFS_OPEN_UPDATE, l, 0, 0, 0, 0);
    } else {
        fsw_modifying(UPFS_SAVE, l, load, exec, start, end);
    }
    return fsw_fs_save(l, load, exec, start, end);
}

static os_error *load_file(const struct fsw_loc *l, const char *name, uint32_t addr,
                           const struct fs_info *i)
{
    if (i->length && !ros_arena_valid(addr, addr + i->length))
        return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
    (void)name;
    return fsw_fs_load(l, addr, i->length);
}

static int file_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t reason = s->r[0];
    char name[1024], list[1024];
    struct fsw_loc l;
    struct fs_info info;
    os_error *e;
    if (reason > 24 && reason != 255)
        return fail(s, ros_error(E_BAD_OSFILE, "Bad OSFile call"));

    /* The path each reason reads through, and whether it writes */
    int kind = 3, mode = R_READ;
    switch (reason) {
    case 5: case 20: case 255: kind = 0; break;
    case 12: case 13: case 21: kind = 1; break;
    case 14: case 15: case 22: kind = 2; break;
    case 0: case 7: case 8: case 10: case 11: mode = R_WRITE; break;
    case 1: case 2: case 3: case 4: case 6: case 9: case 18: mode = R_WRITE; break;
    }
    if (reason == 19) {                                 /* FileSwitch's MakeErrorOp */
        /* The error names the object as the caller gave it, to a space or
         * a control character, not translated (strlenTS): the sprite
         * code's "File '<Dir>.X' not found" */
        uint32_t a = s->r[1];
        if (!a || !ros_arena_readable(a, a + 1))
            return fail(s, ros_error(ROS_ERR_BAD_ADDRESS, "Bad address"));
        size_t n = 0;
        while (n + 1 < sizeof name && ros_arena_readable(a + n, a + n + 1)) {
            uint8_t ch = *(const uint8_t *)ros_ptr(a + n);
            if (ch <= ' ')
                break;
            name[n++] = (char)ch;
        }
        name[n] = 0;
        switch (s->r[2]) {
        case OBJ_NOTHING: return fail(s, fsw_err_not_found(name));
        case OBJ_NOTHING + 0x100:
            return fail(s, ros_error(E_FILE_NOT_FOUND, "Directory '%s' not found", name));
        case OBJ_FILE: return fail(s, ros_error(E_IS_A_FILE, "'%s' is a file", name));
        case OBJ_DIR: case OBJ_DIR | OBJ_FILE: return fail(s, fsw_err_is_dir(name));
        default: return ROS_VECTOR_CLAIM;               /* no error for any other */
        }
    }
    if ((e = fsw_arg_path(s->r[1], name, sizeof name)) != NULL)
        return fail(s, e);
    if (reason == 24) {
        s->r[2] = 4096;
        return ROS_VECTOR_CLAIM;
    }
    if ((e = fsw_path_list(kind, s->r[4], list, sizeof list)) != NULL)
        return fail(s, e);
    if ((e = fsw_resolve_via(name, list, mode, &l)) != NULL)
        return fail(s, e);
    if ((e = fsw_stat_loc(&l, &info)) != NULL)
        return fail(s, e);
    uint64_t cs = fsw_now_cs();

    switch (reason) {
    case 0:                                             /* save */
        if (info.type == OBJ_DIR)
            return fail(s, err_file_on_dir(name));
        e = save(&l, s->r[2], s->r[3], s->r[4], s->r[5]);
        break;
    case 10:                                            /* save, typed */
        if (info.type == OBJ_DIR)
            return fail(s, err_file_on_dir(name));
        e = save(&l, fsw_stamp_load(s->r[2], cs), (uint32_t)cs, s->r[4], s->r[5]);
        break;
    case 7:                                             /* create */
    case 11:                                            /* create, typed */
        if (info.type == OBJ_DIR)
            return fail(s, err_file_on_dir(name));
        if (s->r[5] < s->r[4])
            return fail(s, ros_error(ROS_ERR_BAD_ADDRESS, "Bad address"));
        e = reason == 7 ? fsw_create(&l, s->r[2], s->r[3], s->r[4], s->r[5])
                        : fsw_create(&l, fsw_stamp_load(s->r[2], cs), (uint32_t)cs, s->r[4],
                                     s->r[5]);
        break;
    case 8:                                             /* directory */
        if (info.type == OBJ_FILE)
            return fail(s, ros_error(E_TYPES_DONT_MATCH, "'%s' cannot be created - a file with "
                                                         "that name already exists", name));
        /* One already there is left, the filing system not called
         * (int_DeleteCreateFileOp) */
        e = info.type == OBJ_NOTHING ? fsw_mkdir(&l) : fsw_fs_mkdir(&l);
        break;
    case 1: case 2: case 3: case 4:                     /* write catalogue information */
        if (info.type == OBJ_NOTHING)
            return fail(s, fsw_err_not_found(name));
        e = fsw_setinfo(&l, (int)reason, s->r[2], s->r[3], s->r[5]);
        break;
    case 9:                                             /* stamp */
        if (info.type == OBJ_NOTHING)
            return fail(s, fsw_err_not_found(name));
        e = fsw_setinfo(&l, 1, fsw_stamp_load(fsw_is_typed(info.load) ? info.load >> 8 : 0xFFD, cs),
                        (uint32_t)cs, info.attr);
        break;
    case 18:                                            /* set type */
        if (info.type == OBJ_NOTHING)
            return fail(s, fsw_err_not_found(name));
        if (info.type == OBJ_DIR)
            break;
        e = fsw_is_typed(info.load)
                ? fsw_setinfo(&l, 2, fsw_stamp_load(s->r[2], (uint64_t)(info.load & 0xFF) << 32),
                              info.exec, info.attr)
                : fsw_setinfo(&l, 1, fsw_stamp_load(s->r[2], cs), (uint32_t)cs, info.attr);
        break;
    case 6:                                             /* delete */
        if (info.type != OBJ_NOTHING)                   /* nothing: nothing done, or told */
            e = fsw_remove(&l, &info);
        else if (fsw_read_only(&l))
            e = fsw_fs_remove(&l);
        if (!e)
            set_info_regs(s, &info);
        break;
    case 5: case 13: case 15: case 17:                  /* read catalogue information */
        set_info_regs(s, &info);
        break;
    case 20: case 21: case 22: case 23:                 /* ... and the file type */
        set_info_regs(s, &info);
        s->r[6] = fsw_file_type_of(fsw_leaf_of(&l), &info);     /* -1 for nothing */
        break;
    case 12: case 14: case 16: case 255:                /* load */
        if (info.type == OBJ_NOTHING)
            return fail(s, fsw_err_not_found(name));
        if (info.type == OBJ_DIR)
            return fail(s, fsw_err_is_dir(name));
        e = load_file(&l, name, (s->r[3] & 0xFF) ? info.load : s->r[2], &info);
        if (!e)
            set_info_regs(s, &info);
        break;
    }
    return e ? fail(s, e) : ROS_VECTOR_CLAIM;
}

/* ---- OS_GBPB ------------------------------------------------------------------- */

/* 1 if it would not fit */
static int put_bytes(uint32_t *at, uint32_t *room, const void *p, uint32_t n)
{
    if (n > *room)
        return 1;
    memcpy(ros_ptr(*at), p, n);
    *at += n, *room -= n;
    return 0;
}

static int read_entries(struct ros_cpu *s, uint32_t reason)
{
    char name[1024], pat[256];
    struct fsw_loc l;
    os_error *e;
    if (reason == 8) {
        l = fsw_dirs_of(fsw_current < 0 ? 0 : fsw_current)->csd;
        if (fsw_current < 0)
            return fail(s, ros_error(E_NO_SELECTED_FS, "No selected filing system"));
    } else {
        e = fsw_arg_path(s->r[1], name, sizeof name);
        if (!e)
            e = fsw_resolve(name[0] ? name : "@", R_READ, &l, 0);
        if (e)
            return fail(s, e);
        struct fs_info info;
        if ((e = fsw_stat_loc(&l, &info)) != NULL)
            return fail(s, e);
        if (info.type == OBJ_NOTHING)                   /* TopPath_EnsureThingIsDirectory */
            return fail(s, ros_error(E_FILE_NOT_FOUND, "Directory '%s' not found", name));
        if (info.type == OBJ_FILE)
            return fail(s, ros_error(E_IS_A_FILE, "'%s' is a file", name));
    }
    snprintf(pat, sizeof pat, "*");
    if (reason != 8 && s->r[6] && (e = fsw_arg_name(s->r[6], pat, sizeof pat)) != NULL)
        return fail(s, e);
    uint32_t at = s->r[2];
    uint32_t room = reason == 8 ? 0xFFFFFFFFu : s->r[5];
    uint32_t want = s->r[3], got = 0, index = s->r[4];
    if (want && !ros_arena_valid(at, at + (room > 0x100000 ? 0x100 : room)))
        return fail(s, ros_error(ROS_ERR_BAD_ADDRESS, "Bad address"));
    struct fs_entry ent;
    int more = 1;
    while (got < want) {
        more = fsw_readdir(&l, index, &ent, &e);
        if (e)
            return fail(s, e);
        if (!more)
            break;
        if (!fsw_wild_match(pat, ent.name)) {
            index++;
            continue;
        }
        uint32_t save_at = at, save_room = room;
        uint32_t nlen = (uint32_t)strlen(ent.name);
        uint32_t words[6] = { ent.info.load, ent.info.exec, ent.info.length, ent.info.attr,
                              ent.info.type, 0 };
        int full = 0;
        if (reason == 8) {
            uint8_t b = (uint8_t)nlen;
            full = put_bytes(&at, &room, &b, 1) || put_bytes(&at, &room, ent.name, nlen);
        } else if (reason == 9) {
            full = put_bytes(&at, &room, ent.name, nlen + 1);
        } else {
            uint32_t head = reason == 10 ? 20 : reason == 11 ? 29 : 24;
            uint32_t total = (head + nlen + 1 + 3) & ~3u;
            if (total > room) {
                full = 1;
            } else {
                uint8_t rec[320];
                memset(rec, 0, sizeof rec);
                memcpy(rec, words, 20);
                if (reason == 11) {
                    if (fsw_is_typed(ent.info.load)) {  /* SIN 0, then the five-byte date */
                        memcpy(rec + 24, &ent.info.exec, 4);
                        rec[28] = (uint8_t)ent.info.load;
                    }
                } else if (reason == 12) {
                    uint32_t ft = fsw_file_type_of(ent.name, &ent.info);
                    memcpy(rec + 20, &ft, 4);
                }
                memcpy(rec + head, ent.name, nlen + 1);
                put_bytes(&at, &room, rec, total);
            }
        }
        if (full) {
            at = save_at, room = save_room;
            break;
        }
        got++;
        index++;
    }
    int end_when_none = fsw_end_when_none(&l);
    if (got == want && more && !end_when_none) {  /* is there anything after? */
        struct fs_entry probe;
        more = fsw_readdir(&l, index, &probe, &e);
    }
    if (end_when_none && got)                     /* -1 only when none was read */
        more = 1;
    if (reason == 8) {
        s->r[3] = want - got;
        s->c = got == 0;
    } else {
        s->r[3] = got;
    }
    s->r[4] = more ? index : 0xFFFFFFFFu;
    s->r[2] = reason == 8 ? at : s->r[2];
    return ROS_VECTOR_CLAIM;
}

/* OS_GBPB 5-7: a counted name */
static int put_counted(struct ros_cpu *s, int lead_zero, const char *text, int trail)
{
    uint32_t n = (uint32_t)strlen(text), at = s->r[2];
    if (!ros_arena_valid(at, at + n + 3))
        return fail(s, ros_error(ROS_ERR_BAD_ADDRESS, "Bad address"));
    uint8_t *p = ros_ptr(at);
    if (lead_zero)
        *p++ = 0;
    *p++ = (uint8_t)n;
    memcpy(p, text, n);
    p[n] = (uint8_t)trail;
    return ROS_VECTOR_CLAIM;
}

static int gbpb_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t reason = s->r[0];
    if (reason >= 1 && reason <= 4) {
        struct stream *st = stream_of(s->r[1]);
        if (!st)
            return fail(s, err_channel());
        uint32_t n = s->r[3];
        if (n && !ros_arena_valid(s->r[2], s->r[2] + n))
            return fail(s, ros_error(ROS_ERR_BAD_ADDRESS, "Bad address"));
        os_error *e = NULL;
        if (reason == 1 || reason == 3) {             /* at R4, not the pointer */
            e = set_ptr(st, s->r[4]);
            if (e)
                return fail(s, e);
        }
        if (reason <= 2) {
            e = stream_write(st, ros_ptr(s->r[2]), n);
            if (e)
                return fail(s, e);
            s->r[2] += n;
            s->r[3] = 0;
        } else {
            uint32_t got = 0;
            e = n ? stream_read(st, ros_ptr(s->r[2]), n, &got) : NULL;
            if (e)
                return fail(s, e);
            s->r[2] += got;
            s->r[3] = n - got;
            s->c = s->r[3] != 0;
        }
        s->r[4] = st->ptr;
        return ROS_VECTOR_CLAIM;
    }
    if (reason >= 5 && reason <= 7) {
        if (fsw_current < 0)
            return fail(s, ros_error(E_NO_SELECTED_FS, "No selected filing system"));
        struct fsw_dirs *d = fsw_dirs_of(fsw_current);
        if (reason == 5)
            return put_counted(s, 0, d->csd.disc[0] ? d->csd.disc : "Resources", 0);
        return put_counted(s, 1, fsw_leaf_of(reason == 6 ? &d->csd : &d->lib), 0);
    }
    if (reason >= 8 && reason <= 12)
        return read_entries(s, reason);
    return fail(s, ros_error(E_BAD_OSGBPB, "Bad OSGBPB call"));
}

/* ---- OS_FSControl ---------------------------------------------------------------- */

os_error *fsw_dir_arg(uint32_t addr, struct fsw_loc *l, const char *dflt)
{
    char name[1024];
    os_error *e = NULL;
    if (addr)
        e = fsw_arg_path(addr, name, sizeof name);
    else
        name[0] = 0;
    if (e)
        return e;
    if (!name[0])
        snprintf(name, sizeof name, "%s", dflt);
    if ((e = fsw_resolve(name, R_READ, l, 0)) != NULL)
        return e;
    struct fs_info info;
    if ((e = fsw_stat_loc(l, &info)) != NULL)
        return e;
    if (info.type == OBJ_NOTHING)
        return ros_error(0x10000u | fsw_fs_of(l)->number << 8 | 0xD6u, "Not found");
    if (info.type == OBJ_FILE)
        return ros_error(E_IS_A_FILE, "'%s' is a file", name);
    return NULL;
}

os_error *fsw_select_fs(int fsi)
{
    fsw_current = fsi;
    fsw_dirs_of(fsi);
    return NULL;
}

/* *Dir: the CSD of the directory's filing system, which becomes current;
 * the old CSD is the previous directory. */
static os_error *set_csd(uint32_t addr)
{
    struct fsw_loc l;
    os_error *e = fsw_dir_arg(addr, &l, "&");
    if (e)
        return e;
    struct fsw_dirs *d = fsw_dirs_of(l.fsi);
    d->psd = d->csd;
    d->csd = l;
    return fsw_select_fs(l.fsi);
}

/* A file type's name, "&xxx" if it has none: 8 characters */
void fsw_type_name(uint32_t type, char out[9])
{
    char var[32], value[64];
    snprintf(var, sizeof var, "File$Type_%03X", type & 0xFFF);
    if (!fsw_read_var(var, value, sizeof value))
        snprintf(value, sizeof value, "&%03X", type & 0xFFF);
    snprintf(out, 9, "%-8.8s", value);
}

/* The next File$Type_* variable after the one at `ctx` (0 for the first).
 * It returns the variable's name, with its value in the arena buffer `buf`:
 * the name first, then room for the value from buf + 16.  It returns 0 at
 * the end.  FileTypeFromStringEntry (FSCtrl2) reads them the same way: the
 * wildcarded name, and the context back in R3.  Reading them is "quicker
 * than doing all 4096, he claims", which is Acorn's comment there.  It is
 * also the difference between one error and four thousand.  A type with no
 * variable is a name that is not there, which is an error built and
 * returned, and *SWIStats counts every one. */
static uint32_t next_type_var(uint32_t buf, uint32_t ctx, size_t max)
{
    strcpy(ros_ptr(buf), "File$Type_*");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = buf, c.r[1] = buf + 16, c.r[2] = (uint32_t)max - 1, c.r[3] = ctx, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    if (c.v)
        return 0;
    ros_st8(buf + 16 + c.r[2], 0);
    return c.r[3];
}

/* The three hex digits after "File$Type_" in a variable's name: its type,
 * or 0x1000 if the name does not end in three of them (FileSwitch reads
 * them with OS_ReadUnsigned, base 16, restricted to 000..FFF). */
static uint32_t type_of_var(uint32_t name)
{
    uint32_t t = 0;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t c = ros_ld8(name + 10 + i) | 0x20;
        if (c >= '0' && c <= '9')
            t = t * 16 + c - '0';
        else if (c >= 'a' && c <= 'f')
            t = t * 16 + c - 'a' + 10;
        else
            return 0x1000;
    }
    return ros_ld8(name + 13) == 0 ? t : 0x1000;
}

/* A file type from a name or a number, as FileSwitch's
 * FileTypeFromStringEntry does it: the File$Type_* variables in turn, each
 * value's trailing spaces off and compared without case (FSCommon's
 * strncmp), and the digits in the name that matched; a number if none
 * matched. */
static os_error *type_from_string(const char *s, uint32_t *type)
{
    if (s[0] == '&') {
        char *end;
        unsigned long v = strtoul(s + 1, &end, 16);
        if (end != s + 1 && !*end && v <= 0xFFF) {
            *type = (uint32_t)v;
            return NULL;
        }
    }
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    size_t n = strlen(s), max = 256;     /* FileSwitch's fts_result */
    while (n && s[n - 1] == ' ')
        n--;
    uint8_t *block = ros_rma_alloc(16 + (uint32_t)max);
    if (block) {
        uint32_t buf = ros_addr(block);
        for (uint32_t ctx = 0; (ctx = next_type_var(buf, ctx, max));) {
            const char *value = ros_ptr(buf + 16);
            size_t m = strlen(value);
            while (m && value[m - 1] == ' ')
                m--;
            if (m != n || strncasecmp(value, s, n) != 0)
                continue;
            uint32_t t = type_of_var(ctx);
            if (t <= 0xFFF) {
                ros_rma_free(block);
                *type = t;
                return NULL;
            }
        }
        ros_rma_free(block);
    }
    if (s[0] && !*end && v <= 0xFFF) {
        *type = (uint32_t)v;
        return NULL;
    }
    return ros_error(E_BAD_FILE_TYPE, "File type is unrecognised");
}

/* OS_FSControl 2, StartApplication (FileSwitch's StartUpTheApplication):
 * R1 -> the command tail, R2 the CAO, R3 -> the command's name.  The
 * command line and the time are set for OS_GetEnv.  UpCall 256 and
 * Service_NewApplication are offered, and either may refuse it.  Handlers
 * left in application space are put back to the defaults, unless the exit
 * handler is there too (the application is a shell).  The CAO is set, and
 * the temporary filing system goes.
 *
 * First, the stacks' room.  An application started here runs inside the
 * one that started it, which it comes back to (runtime/module.c,
 * ros_module_run_as_application).  A program that starts itself again and
 * again nests without end.  An example is BASIC's OSCLI "BASICVFP -quit"
 * naming its own file.  RISC OS 5.30 replaces the application each time and
 * goes round for ever.  Here it goes as deep as the stacks allow, then
 * fails as FileSwitch does when it has too little stack to call a filing
 * system (task.h, ros_stack_room). */
static os_error *start_application(struct ros_cpu *s)
{
    if (!ros_stack_room(ROS_STACK_FILE))
        return ros_error(E_NO_STACK, "Not enough stack to call filing system");
    uint32_t buf = ros_addr(ros_rma_alloc(1100));
    if (!buf)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    uint32_t n = 0;
    for (uint32_t p = s->r[3]; p && ros_ld8(p) >= ' ' && n < 1000; p++)
        ros_st8(buf + n++, ros_ld8(p));
    if (s->r[1] && ros_ld8(s->r[1]) >= ' ') {
        ros_st8(buf + n++, ' ');
        for (uint32_t p = s->r[1]; ros_ld8(p) >= ' ' && n < 1090; p++)
            ros_st8(buf + n++, ros_ld8(p));
    }
    ros_st8(buf + n, 0);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_st8(buf + 1092, 3);                 /* OS_Word 14 3: the time, five bytes */
    c.r[0] = 14, c.r[1] = buf + 1092;
    ros_swi(&c, XOS_Word);
    ros_cpu_enter(&c);
    c.r[0] = buf, c.r[1] = buf + 1092;
    ros_swi(&c, XOS_WriteEnv);
    ros_rma_free(ros_ptr(buf));

    ros_cpu_enter(&c);
    c.r[0] = 256;                           /* UpCall_NewApplication */
    ros_swi(&c, XOS_UpCall);
    int refused = !c.v && c.r[0] == 0;
    if (!refused) {
        ros_cpu_enter(&c);
        c.r[1] = 0x2A;                      /* Service_NewApplication */
        ros_service_call(&c);
        refused = c.r[1] == 0;
    }
    if (refused)
        return ros_error(0x600u, "Unable to start application");

    /* Handlers below the memory limit (a signed compare, as FileSwitch's) */
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0, c.r[2] = 0, c.r[3] = 0;
    ros_swi(&c, XOS_ChangeEnvironment);
    int32_t limit = (int32_t)c.r[1];
    ros_cpu_enter(&c);
    c.r[0] = 11, c.r[1] = 0, c.r[2] = 0, c.r[3] = 0;
    ros_swi(&c, XOS_ChangeEnvironment);
    if ((int32_t)c.r[1] >= limit)
        for (uint32_t h = 1; h <= 16; h = h == 12 ? 16 : h + 1) {
            ros_cpu_enter(&c);
            c.r[0] = h, c.r[1] = 0, c.r[2] = 0, c.r[3] = 0;
            ros_swi(&c, XOS_ChangeEnvironment);
            if ((int32_t)c.r[1] < limit) {
                ros_cpu_enter(&c);
                c.r[0] = h;
                ros_swi(&c, XOS_ReadDefaultHandler);
                c.r[0] = h;
                ros_swi(&c, XOS_ChangeEnvironment);
            }
        }
    ros_cpu_enter(&c);
    c.r[0] = 15, c.r[1] = s->r[2], c.r[2] = 0, c.r[3] = 0;   /* CAO */
    ros_swi(&c, XOS_ChangeEnvironment);
    fsw_temp = -1;
    return NULL;
}

static int fsc_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t reason = s->r[0];
    os_error *e = NULL;
    char name[1024], name2[1024];
    struct fsw_loc l, l2;
    switch (reason) {
    case 0:                                             /* *Dir */
        e = set_csd(s->r[1]);
        break;
    case 1:                                             /* *Lib */
        if (!(e = fsw_dir_arg(s->r[1], &l, "&"))) {
            fsw_dirs_of(l.fsi)->lib = l;
            fsw_dirs_of(l.fsi)->lib_set = 1;
        }
        break;
    case 2:                                             /* start an application */
        e = start_application(s);
        break;
    case 4:                                             /* run */
        e = fsw_run(s->r[1]);
        break;
    case 5: case 6: case 7: case 8:                     /* *Cat, *Ex, *LCat, *LEx */
        e = fsw_catex(reason, s->r[1]);
        break;
    case 9: case 32:                                    /* *Info, *FileInfo */
        e = fsw_info(reason, s->r[1]);
        break;
    case 10:                                            /* *Opt: nothing to set */
        break;
    case 24:                                            /* *Access */
        e = fsw_access(s->r[1], s->r[2]);
        break;
    case 26:                                            /* *Copy */
        e = fsw_copy(s);
        break;
    case 27:                                            /* *Wipe */
        e = fsw_wipe(s);
        break;
    case 28:                                            /* *Count */
        e = fsw_count(s);
        break;
    case 13: {                                          /* look up a filing system */
        /* R1 is a number (below 256) or points to a name, which a control
         * character ends.  With R2 0, '#', ':' and '-' end it too.  Out: R1
         * is its number and R2 its block, or R2 is 0 if there is none
         * (LookupFSEntry) */
        int fsi = -1;
        if (s->r[1] < 256) {
            fsi = fs_by_number(s->r[1]);
        } else {
            uint32_t p = s->r[1], n = 0;
            for (;; n++) {
                uint32_t c = ros_ld8(p + n);
                if (c < ' ' || (s->r[2] == 0 && (c == '#' || c == ':' || c == '-')) || n == 64)
                    break;
            }
            for (int i = 0; i < FSW_MAXFS; i++)
                if (fsw_filing_systems[i] && strlen(fsw_filing_systems[i]->name) == n &&
                    strncasecmp(fsw_filing_systems[i]->name, (const char *)ros_ptr(p), n) == 0)
                    fsi = i;
        }
        if (fsi < 0) {
            s->r[2] = 0;
            break;
        }
        s->r[1] = fsw_filing_systems[fsi]->number;
        s->r[2] = fs_block(fsi);
        break;
    }
    case 14: {                                          /* select a filing system */
        /* SelectFSEntry: by number (0: none selected; a number no filing
         * system has: nothing, and no error), or by name */
        int fsi = -1;
        if (s->r[1] == 0) {
            fsw_current = -1;
            fsw_temp = -1;
            break;
        }
        if (s->r[1] < 256) {
            fsi = fs_by_number(s->r[1]);
            if (fsi < 0)
                break;
        } else if (!(e = fsw_arg_name(s->r[1], name, sizeof name))) {
            size_t n = strcspn(name, ":#-");
            fsi = fs_by_name(name, n);
        }
        if (!e && fsi < 0)
            e = ros_error(E_UNKNOWN_FS, "Filing system name not recognised");
        if (!e)
            e = fsw_select_fs(fsi);
        break;
    }
    case 12:                                            /* add a filing system */
        e = fsw_add_fs(s->r[1], s->r[2], s->r[3]);
        break;
    case 15:                                            /* boot the current one */
        e = fsw_bootup_fs(fsw_current);
        break;
    case 16: {                                          /* remove a filing system */
        if (s->r[1] < 256) {
            e = ros_error(0x8E, "Filing system cannot be removed by number");
            break;
        }
        uint32_t p = s->r[1], n = 0;
        while (n < 64 && ros_ld8(p + n) > ' ' && ros_ld8(p + n) != ':' && ros_ld8(p + n) != '#' &&
               ros_ld8(p + n) != '-')
            n++;
        int fsi = fs_by_name((const char *)ros_ptr(p), n);
        if (fsi < 0)
            e = ros_error(E_UNKNOWN_FS, "Filing system %.*s: not present", (int)n,
                          (const char *)ros_ptr(p));
        else
            e = fsw_remove_fs(fsi);
        break;
    }
    case 17:                                            /* add a secondary module */
        break;
    case 35:                                            /* add an image filing system */
        e = fsw_add_image_fs(s->r[1], s->r[2], s->r[3]);
        break;
    case 36:                                            /* remove one */
        e = fsw_remove_image_fs(s->r[1]);
        break;
    case 18: {                                          /* a file type's name */
        char n8[9];
        fsw_type_name(s->r[2], n8);
        uint32_t w[2];
        memcpy(w, n8, 8);
        s->r[2] = w[0], s->r[3] = w[1];
        break;
    }
    case 3: {                                           /* a filing system prefix */
        /* R1 -> "fs[#special]:" or "-fs[#special]-": R1 past it, R2 the
         * filing system's number (-1 if there is none), R3 -> the special
         * field or 0; the filing system is the temporary one until 19 */
        uint32_t p = s->r[1];
        s->r[2] = 0xFFFFFFFFu, s->r[3] = 0;
        int dash = ros_ld8(p) == '-';
        uint32_t q = p + (uint32_t)dash, special = 0;
        while (ros_ld8(q) > ' ' && ros_ld8(q) != ':' && ros_ld8(q) != '#' &&
               ros_ld8(q) != '-' && ros_ld8(q) != '.')
            q++;
        uint32_t n = q - p - (uint32_t)dash;
        if (ros_ld8(q) == '#') {
            special = q + 1;
            while (ros_ld8(q) > ' ' && ros_ld8(q) != (dash ? '-' : ':'))
                q++;
        }
        if (n == 0 || n >= sizeof name || ros_ld8(q) != (dash ? '-' : ':'))
            break;
        memcpy(name, ros_ptr(p + (uint32_t)dash), n);
        int fsi = fs_by_name(name, n);
        if (fsi < 0)
            break;
        fsw_temp = fsi;
        fsw_dirs_of(fsi);
        s->r[1] = q + 1;
        s->r[2] = fsw_filing_systems[fsi]->number;
        s->r[3] = special;
        break;
    }
    case 19:                                            /* restore the current FS */
        fsw_temp = -1;
        break;
    case 20: {                                          /* the temporary FS's module */
        uint32_t base = 0, r12 = 0;
        fsw_module_of(fsw_temp >= 0 ? fsw_temp : fsw_current, &base, &r12);
        s->r[1] = base, s->r[2] = r12;
        break;
    }
    case 21: {                                          /* a stream's FS handle */
        struct stream *st = stream_of(s->r[1]);
        if (!st)
            e = err_channel();
        else
            s->r[1] = fsw_file_fh(st->f), s->r[2] = fsw_filing_systems[st->fsi]->number;
        break;
    }
    case 22: case 23:                                   /* shut, shut down */
        e = fsw_close_all();
        if (reason == 23)
            fsw_shutdown_fs();
        break;
    case 25: {                                          /* rename */
        struct fs_info i1, i2;
        e = fsw_arg_path(s->r[1], name, sizeof name);
        if (!e)
            e = fsw_arg_path(s->r[2], name2, sizeof name2);
        if (!e)
            e = fsw_resolve(name, R_WRITE, &l, 0);
        if (!e)
            e = fsw_resolve(name2, R_WRITE, &l2, 0);
        if (!e && (l.fsi != l2.fsi || strcasecmp(l.disc, l2.disc) != 0))
            e = ros_error(E_BAD_RENAME, "Bad rename");
        if (!e)
            e = fsw_stat_loc(&l, &i1);
        if (!e && i1.type == OBJ_NOTHING)
            e = fsw_err_not_found(name);
        if (!e)
            e = fsw_stat_loc(&l2, &i2);
        if (!e && i2.type != OBJ_NOTHING && strcasecmp(l.path, l2.path) != 0)
            e = ros_error(E_ALREADY_EXISTS,
                          "Item cannot be renamed - '%s' already exists", name2);
        if (!e && i1.type == OBJ_DIR && strncasecmp(l2.path, l.path, strlen(l.path)) == 0 &&
            l2.path[strlen(l.path)] == '.')
            e = ros_error(E_BAD_RENAME, "Bad rename");
        if (!e)
            e = rename_object(&l, &l2);
        break;
    }
    case 31: {                                          /* file type from a string */
        uint32_t type;
        e = fsw_arg_name(s->r[1], name, sizeof name);
        if (!e)
            e = type_from_string(name, &type);
        if (!e)
            s->r[2] = type;
        break;
    }
    case 33: {                                          /* a filing system's name */
        int fsi = fs_by_number(s->r[1] & 0xFF);
        const struct fs *fs = fsi >= 0 ? fsw_filing_systems[fsi] : NULL;
        const char *n = fs ? fs->name : "";
        uint32_t len = (uint32_t)strlen(n) + 1;
        if (s->r[3] < len)
            e = ros_error(0x1E4u, "Buffer overflow");
        else if (!ros_arena_valid(s->r[2], s->r[2] + len))
            e = ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
        else
            memcpy(ros_ptr(s->r[2]), n, len);
        break;
    }
    case 37: {                                          /* canonicalise */
        char list[1024], canon[1100];
        e = fsw_arg_name(s->r[1], name, sizeof name);
        if (!e) {
            if (s->r[3])
                e = fsw_path_list(2, s->r[3], list, sizeof list);
            else if (s->r[4])
                e = fsw_path_list(1, s->r[4], list, sizeof list);
            else
                list[0] = 0;
        }
        if (!e)
            e = fsw_resolve_via(name, list, R_READ, &l);
        if (e)
            break;
        fsw_canonical(&l, canon, sizeof canon);
        uint32_t len = (uint32_t)strlen(canon) + 1;
        if (s->r[2] && s->r[5] >= len) {
            if (!ros_arena_valid(s->r[2], s->r[2] + len)) {
                e = ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
                break;
            }
            memcpy(ros_ptr(s->r[2]), canon, len);
        }
        s->r[5] -= len;
        break;
    }
    case 38: {                                          /* information to file type */
        struct fs_info i = { s->r[6], s->r[2], s->r[3], s->r[4], s->r[5] };
        name[0] = 0;
        if (s->r[1])
            e = fsw_arg_name(s->r[1], name, sizeof name);
        if (!e) {
            const char *dot = strrchr(name, '.');
            s->r[2] = fsw_file_type_of(dot ? dot + 1 : name, &i);
        }
        break;
    }
    case 39:                                            /* *URD */
        if (!(e = fsw_dir_arg(s->r[1], &l, "$"))) {
            fsw_dirs_of(l.fsi)->urd = l;
            fsw_dirs_of(l.fsi)->urd_set = 1;
        }
        break;
    case 40: {                                          /* *Back */
        if (fsw_current < 0) {
            e = ros_error(E_NO_SELECTED_FS, "No selected filing system");
            break;
        }
        struct fsw_dirs *d = fsw_dirs_of(fsw_current);
        struct fsw_loc t = d->csd;
        d->csd = d->psd, d->psd = t;
        break;
    }
    case 43: case 44: case 45: {                        /* *NoDir, *NoURD, *NoLib */
        if (fsw_current < 0)
            break;
        struct fsw_dirs *d = fsw_dirs_of(fsw_current);
        struct fsw_loc *which = reason == 43 ? &d->csd : reason == 44 ? &d->urd : &d->lib;
        *which = d->csd;
        which->path[0] = 0;
        if (reason == 44)
            d->urd_set = 0;
        if (reason == 45)
            d->lib_set = 0;
        break;
    }
    case 47:                                            /* read the boot option */
        s->r[2] = 0;
        break;
    case 49: case 55: {                                 /* free space */
        uint64_t fr, big, size;
        name[0] = 0;
        if (s->r[1])
            e = fsw_arg_path(s->r[1], name, sizeof name);
        if (!e)
            e = fsw_resolve(name[0] ? name : "@", R_READ, &l, 0);
        if (!e)
            e = fsw_fs_free(&l, &fr, &big, &size);
        if (e)
            break;
        if (reason == 49) {
            s->r[0] = fr > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)fr;
            s->r[1] = big > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)big;
            s->r[2] = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
        } else {
            s->r[0] = (uint32_t)fr, s->r[1] = (uint32_t)(fr >> 32);
            s->r[2] = big > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)big;
            s->r[3] = (uint32_t)size, s->r[4] = (uint32_t)(size >> 32);
        }
        break;
    }
    default:
        e = ros_error(E_BAD_FSCONTROL, "Bad FSControl call");
        break;
    }
    return e ? fail(s, e) : ROS_VECTOR_CLAIM;
}

/* ---- the module ---------------------------------------------------------------- */

static const struct {
    uint32_t vector;
    ros_vector_fn *fn;
} owned[] = {
    { FILEV, file_v }, { ARGSV, args_v }, { BGETV, bget_v }, { BPUTV, bput_v },
    { GBPBV, gbpb_v }, { FINDV, find_v }, { FSCV, fsc_v },
};

/* The discs: ROSGD_DISCS, "Name=/dir[,Name=/dir...]", when it is set;
 * otherwise the host share, /host, as Host, and the RISC OS disc /init
 * mounted, /disc, as Disc. */
static void mount_discs(void)
{
    const char *env = getenv("ROSGD_DISCS");
    if (env) {
        char buf[2048];
        snprintf(buf, sizeof buf, "%s", env);
        for (char *p = strtok(buf, ","); p; p = strtok(NULL, ",")) {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                ros_hostfs_mount(p, eq + 1);
            }
        }
        return;
    }
    struct stat st;
    if (stat("/host", &st) == 0 && S_ISDIR(st.st_mode))
        ros_hostfs_mount("Host", "/host");
    if (stat("/disc", &st) == 0 && S_ISDIR(st.st_mode))
        ros_hostfs_mount("Disc", "/disc");
}

/* The variables FileSwitch makes when it starts, each only if it is not
 * set already (FileSwBody, FileSwitch_FirstVariableToCreate on): paths,
 * the utilities' options, run and load actions, and the type names. */
static const char *const default_vars[][2] = {
    { "Run$Path", ",%." },
    { "File$Path", "" },
    { "Copy$Options", "A C ~D ~F ~L ~N ~P ~Q ~R ~S ~T V" },
    { "Wipe$Options", "C ~F ~R V" },
    { "Count$Options", "~C R ~V" },
    { "Alias$@RunType_FD1", "Basic -quit \"%0\" %*1" },
    { "Alias$@RunType_FEA", "Desktop -file %*0" },
    { "Alias$@RunType_FEB", "Obey %0 " },
    { "Alias$@RunType_FED", "WimpPalette %0 " },
    { "Alias$@RunType_FF7", "Print %0 " },
    { "Alias$@RunType_FF9", "ScreenLoad %0 " },
    { "Alias$@RunType_FFA", "RMRun %*0" },
    { "Alias$@RunType_FFB", "Basic -quit \"%0\" %*1" },
    { "Alias$@RunType_FFE", "Exec %0 " },
    { "Alias$@RunType_FFF", "Type %0 " },
    { "Alias$@LoadType_FD1", "Basic -load \"%0\" %*1" },
    { "Alias$@LoadType_FF7", "Print %0 " },
    { "Alias$@LoadType_FF9", "SLoad %0 " },
    { "Alias$@LoadType_FFA", "RMLoad %*0" },
    { "Alias$@LoadType_FFB", "Basic -load \"%0\" %*1" },
    { "File$Type_AFF", "DrawFile" },
    { "File$Type_BBC", "BBC ROM" },
    { "File$Type_F95", "Code" },
    { "File$Type_FD1", "BASICTxt" },
    { "File$Type_FAE", "Resource" },
    { "File$Type_FEA", "Desktop" },
    { "File$Type_FEB", "Obey" },
    { "File$Type_FEC", "Template" },
    { "File$Type_FED", "Palette" },
    { "File$Type_FF2", "Config" },
    { "File$Type_FF4", "Printout" },
    { "File$Type_FF5", "PoScript" },
    { "File$Type_FF6", "Font" },
    { "File$Type_FF7", "BBC font" },
    { "File$Type_FF8", "Absolute" },
    { "File$Type_FF9", "Sprite" },
    { "File$Type_FFA", "Module" },
    { "File$Type_FFB", "BASIC" },
    { "File$Type_FFC", "Utility" },
    { "File$Type_FFD", "Data" },
    { "File$Type_FFE", "Command" },
    { "File$Type_FFF", "Text" },
};

static void make_variables(void)
{
    uint32_t buf = ros_addr(ros_rma_alloc(256));
    if (!buf)
        return;
    for (unsigned i = 0; i < sizeof default_vars / sizeof default_vars[0]; i++) {
        struct ros_cpu c;
        strcpy(ros_ptr(buf), default_vars[i][0]);
        ros_cpu_enter(&c);
        c.r[0] = buf, c.r[1] = 0, c.r[2] = 0x80000000u, c.r[3] = 0, c.r[4] = 0;
        ros_swi(&c, XOS_ReadVarVal);
        if (c.r[2] != 0)
            continue;                   /* there already */
        strcpy(ros_ptr(buf + 64), default_vars[i][1]);
        ros_cpu_enter(&c);
        c.r[0] = buf, c.r[1] = buf + 64, c.r[2] = (uint32_t)strlen(default_vars[i][1]);
        c.r[3] = 0, c.r[4] = 0;
        ros_swi(&c, XOS_SetVarVal);
    }
    ros_rma_free(ros_ptr(buf));
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    static int mounted;
    if (!mounted) {
        mount_discs();
        mounted = 1;
    }
    memset(fsw_dirs, 0, sizeof fsw_dirs);
    memset(streams, 0, sizeof streams);
    fsw_current = ros_hostfs.boot_disc()[0] ? 0 : 1;
    make_variables();
    for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; i++) {
        os_error *e = ros_vector_claim_native(owned[i].vector, owned[i].fn, 0);
        if (e)
            return e;
    }
    ros_hostfs_free_register(0);        /* Free there already: started again (*RMReInit) */
    /* Service_FSRedeclare: the filing systems modules added to the last
     * FileSwitch add themselves again (FileSwBody) */
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = 0x40;
    ros_service_call(&c);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    ros_hostfs_free_register(1);
    fsw_close_all();
    fsw_forget_modules();
    for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; i++)
        ros_vector_release_native(owned[i].vector, owned[i].fn, 0);
    fsw_current = -1;
    if (fs_blocks)
        ros_rma_free(ros_ptr(fs_blocks));
    fs_blocks = 0;
    return NULL;
}

/* Service_StartUpFS (&12), R2 a filing system's number: select it, and
 * claim the call whatever happens (FileSwitch_Service_StartUpFS).  A number
 * that no filing system has leaves the current one.  The service comes from
 * the old OS_Byte 143 way of selecting a filing system.  The Resource Filer
 * asks this way whether ResourceFS is there before it puts its icon on the
 * icon bar.
 *
 * Service_PostInit (&73): every module in the ROM has started.  A filing
 * system registers with Free as it starts.  On RISC OS the team's HostFS
 * starts after Free in the ROM.  ROSGD's starts in FileSwitch, the first
 * module, so it registers here, once.  This is where the team's puts
 * itself in Free's list.  A *RMReInit Free later forgets it, as on RISC OS
 * 5.30, where no filing system registers again (tests/desktop/resfiler). */
#define SERVICE_STARTUPFS 0x12u
#define SERVICE_POSTINIT  0x73u
#define SERVICE_CLOSEFILE 0x68u

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] == SERVICE_POSTINIT) {
        ros_hostfs_free_register(0);
        return;
    }
    if (s->r[1] == SERVICE_CLOSEFILE) {
        /* R2 a path: images at it or below, with only images open in
         * them, closed; R3 counts them (FileSwitch_Service_CloseFile) */
        char name[1024];
        struct fsw_loc l;
        if (!fsw_arg_name(s->r[2], name, sizeof name) && name[0] &&
            !fsw_resolve(name, R_READ, &l, 0))
            s->r[3] += fsw_images_close_at(&l);
        return;
    }
    if (s->r[1] != SERVICE_STARTUPFS)
        return;
    int fsi = fs_by_number(s->r[2] & 0xFFu);
    if (fsi >= 0)
        fsw_select_fs(fsi);
    s->r[1] = 0;
}

struct ros_module fileswitch_module = {
    .title = "FileSwitch",
    .help = "FileSwitch\t2.92 (25 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .commands = fsw_util_commands,
};
