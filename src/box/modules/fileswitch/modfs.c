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
 * (Sources/FileSys/FileSwitch: s.LowLevel, s.Ensure, s.FSCommon, s.FSShared,
 * hdr.LowFSI).
 */

/* modfs.c -- filing systems added by modules, image filing systems, and
 * the calls FileSwitch makes of any filing system, through them.
 *
 * RISC OS's FileSwitch knows its filing systems by the blocks modules give
 * it (OS_FSControl 12, AddFS; 35, AddImageFS).  It calls their entries with
 * the register contracts of PRM 2 ("Writing a filing system") and
 * FileSwitch's own s/LowLevel: FSEntry_Open, _GetBytes, _PutBytes, _Args,
 * _Close, _File, _Func and _GBPB.  An image filing system has ImageEntry_
 * entries, which are the same with R6 the image's handle.  Here those
 * entries are module code.  That is compiled C in an x32 or A64X32 module,
 * or anything else that ros_call reaches.  They are entered with R12 the
 * value the module gave.
 *
 * An image filing system claims a file type.  A file of that type is an
 * image.  OS_File 5 says it is a file and a directory (object type 3).  A
 * path through it, such as HostFS::Host.$.a/zip.dir.file, reaches the
 * objects inside, which the image filing system keeps.
 *
 * FileSwitch opens the image file as a stream of its own when a path first
 * goes into it (s/Ensure's EnsureCanonicalObject and OpenMultiFSFile).  It
 * tells the image filing system (FSEntry_Func 21, NewImage, R1 the stream's
 * handle).  From then on it passes the image filing system paths relative
 * to the image's root, with R6 its handle.
 *
 * The image stays open until its stream is closed.  That happens by
 * OS_Find 0, by *Shut, or by Service_CloseFile (&68) naming it or a
 * directory above it.  The image filing system itself issues the service
 * when it is done with an archive.  FileSwitch issues it before it opens,
 * deletes, creates, saves, loads or renames an object (TryGetFileClosed).
 * Each time, it first tells the image filing system (FSEntry_Func 22,
 * ImageClosing).
 *
 * Every call FileSwitch makes of a filing system goes through here, as a
 * "target".  A target is the native HostFS, ResourceFS or LanMan
 * (fileswitch.h's struct fs), a module's filing system (its path "$.a.b"),
 * or an image (its path "a.b", "" for the root).  Open files are struct
 * fsw_file.  The handle is the native filing system's or the module's.
 * FileSwitch buffers the file as RISC OS does when the open gives a buffer
 * size (one buffer a file).  Otherwise it goes byte by byte through
 * GetBytes and PutBytes, or by OS_GBPB's entry when the open says it has
 * one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsw.h"
#include "pdriver.h"
#include "systemdevs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

/* hdr/LowFSI: the info word's bits, the open's, the entries' reasons */
#define FSINFO_SPECIAL       (1u << 31)
#define FSINFO_ALWAYSOPEN    (1u << 28)
#define FSINFO_FLUSHNOTIFY   (1u << 27)
#define FSINFO_EXTRAINFO     (1u << 17)
#define FSINFO_READONLY      (1u << 16)
#define FSINFO_DONTUSELOAD   (1u << 20)
#define FSINFO_DONTUSESAVE   (1u << 19)
#define FSINFO_NOTFORMULTIFS (~FSINFO_FLUSHNOTIFY)

#define FSOPEN_WRITE        (1u << 31)
#define FSOPEN_READ         (1u << 30)
#define FSOPEN_DIR          (1u << 29)
#define FSOPEN_UNBUFFGBPB   (1u << 28)

#define FSFILE_LOAD    0xFFu
#define FSFILE_SAVE    0u
#define FSFILE_WRITEINFO 1u
#define FSFILE_READINFO  5u
#define FSFILE_DELETE  6u
#define FSFILE_CREATE  7u
#define FSFILE_CDIR    8u

#define FSARGS_READPTR   0u
#define FSARGS_SETPTR    1u
#define FSARGS_READEXT   2u
#define FSARGS_SETEXT    3u
#define FSARGS_FLUSH     6u
#define FSARGS_ENSURE    7u
#define FSARGS_ZEROES    8u
#define FSARGS_LOADEXEC  9u

#define FSFUNC_RENAME     8u
#define FSFUNC_BOOTUP     10u
#define FSFUNC_READINFO   15u
#define FSFUNC_SHUTDOWN   16u
#define FSFUNC_NEWIMAGE   21u
#define FSFUNC_IMAGECLOSE 22u
#define FSFUNC_FREESPACE  30u

#define MAXBUF 4096u                    /* hdr/LowFSI's Max_BuffSize */

/* ---- the filing systems modules add ------------------------------------------------ */

struct mfs {
    struct fs fs;                       /* in fsw_filing_systems (module FS only) */
    int used, image;
    char name[64];
    uint32_t base, r12, info, extra, filetype;
    /* entries, absolute; 0 where the block gives none */
    uint32_t open, get, put, args, close, file, func, gbpb;
};

#define NMFS 16
#define NIFS 16
static struct mfs mods[NMFS];           /* OS_FSControl 12 */
static struct mfs imfs[NIFS];           /* OS_FSControl 35 */

const struct fs *fsw_filing_systems[FSW_MAXFS] = { &ros_hostfs, &ros_resourcefs_fs, &ros_lanmanfs,
                                                   &ros_printerfs, &ros_nullfs, &ros_vdufs,
                                                   &ros_rawvdufs, &ros_kbdfs, &ros_rawkbdfs,
                                                   &ros_sourcefs };

static struct mfs *mfs_of(const struct fs *fs)
{
    return fs ? fs->mfs : NULL;
}

static const char *no_disc(void)
{
    return "";
}

/* ---- images ---------------------------------------------------------------------- */

struct image {
    int used;
    int opening;                        /* scb_partitionbad: NewImage not yet back */
    struct fsw_loc loc;                 /* the image file */
    struct mfs *ifs;
    uint32_t handle;                    /* the file's FileSwitch stream */
    uint32_t ih;                        /* the image filing system's handle */
};

#define NIMG 32
static struct image images[NIMG];

static int any_ifs(void)
{
    for (int i = 0; i < NIFS; i++)
        if (imfs[i].used)
            return 1;
    return 0;
}

static struct mfs *ifs_for_type(uint32_t type)
{
    for (int i = 0; i < NIFS; i++)
        if (imfs[i].used && imfs[i].filetype == type)
            return &imfs[i];
    return NULL;
}

/* AdjustObjectTypeReMultiFS: a file of a type an image filing system
 * claims is a file and a directory */
static uint32_t adjust_type(uint32_t type, uint32_t load)
{
    if (type == OBJ_FILE && fsw_is_typed(load) && ifs_for_type(load >> 8 & 0xFFF))
        return OBJ_FILE | OBJ_DIR;
    return type;
}

static int same_place(const struct fsw_loc *a, const struct fsw_loc *b)
{
    return a->fsi == b->fsi && !strcasecmp(a->disc, b->disc);
}

/* If path is prefix, then ".rest": the length of prefix; else 0 (an exact
 * match is not "inside") */
static size_t inside(const char *prefix, const char *path)
{
    size_t n = strlen(prefix);
    if (strncasecmp(prefix, path, n) != 0 || path[n] != '.')
        return 0;
    return n;
}

/* The open image l is inside, the deepest; -1 if none */
static int image_holding(const struct fsw_loc *l, size_t *plen)
{
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < NIMG; i++) {
        if (!images[i].used || images[i].opening || !same_place(&images[i].loc, l))
            continue;
        size_t n = inside(images[i].loc.path, l->path);
        if (n && n >= bl)
            best = i, bl = n;
    }
    if (plen)
        *plen = bl;
    return best;
}

/* The open image that is l itself; -1 if none */
static int image_at(const struct fsw_loc *l)
{
    for (int i = 0; i < NIMG; i++)
        if (images[i].used && !images[i].opening && same_place(&images[i].loc, l) &&
            !strcasecmp(images[i].loc.path, l->path))
            return i;
    return -1;
}

/* ---- targets ----------------------------------------------------------------------- */

struct tgt {
    int fsi;
    const struct fs *fs;                /* native, when m is 0 */
    struct mfs *m;                      /* a module's filing system, or an image's */
    int img;                            /* the image, or -1 */
    const char *disc;
    char path[1100];                    /* as the filing system is passed it */
};

static void tgt_base(const struct fsw_loc *l, struct tgt *t)
{
    t->fsi = l->fsi;
    t->fs = fsw_filing_systems[l->fsi];
    t->m = mfs_of(t->fs);
    t->img = -1;
    t->disc = l->disc;
    if (t->m)
        snprintf(t->path, sizeof t->path, "$%s%s", l->path[0] ? "." : "", l->path);
    else
        snprintf(t->path, sizeof t->path, "%s", l->path);
}

static void tgt_image(int img, const char *inner, struct tgt *t)
{
    t->fsi = images[img].loc.fsi;
    t->fs = NULL;
    t->m = images[img].ifs;
    t->img = img;
    t->disc = images[img].loc.disc;
    snprintf(t->path, sizeof t->path, "%s", inner);
}

/* Where l is: inside an open image, or on its own filing system */
static void target_of(const struct fsw_loc *l, struct tgt *t)
{
    size_t n;
    int img = image_holding(l, &n);
    if (img >= 0)
        tgt_image(img, l->path + n + 1, t);
    else
        tgt_base(l, t);
}

/* ---- calling a filing system's entry ------------------------------------------------- */

/* r[0..7] in, R12 the module's value; out the same, and C.  The error, if
 * the entry set V. */
static os_error *call_entry(const struct mfs *m, uint32_t addr, uint32_t r[8], int *carry)
{
    if (!addr)
        return ros_error(E_UNSUPPORTED, "Filing system does not support this operation");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    for (int i = 0; i < 8; i++)
        c.r[i] = r[i];
    c.r[12] = m->r12;
    ros_call(&c, addr);
    ros_continue(&c);
    if (c.r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(&c, ROS_RETURN_TO_NATIVE);
    for (int i = 0; i < 8; i++)
        r[i] = c.r[i];
    if (carry)
        *carry = c.c;
    return c.v ? (os_error *)ros_ptr(c.r[0]) : NULL;
}

/* A string the entry can read, in the RMA */
static uint32_t rma_str(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t a = ros_addr(ros_rma_alloc((uint32_t)n));
    if (a)
        memcpy(ros_ptr(a), s, n);
    return a;
}

static void rma_free(uint32_t a)
{
    if (a)
        ros_rma_free(ros_ptr(a));
}

static os_error *no_room(void)
{
    return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
}

/* The image's handle, or a module FS's special field (none: 0) */
static uint32_t special_of(const struct tgt *t)
{
    return t->img >= 0 ? images[t->img].ih : 0;
}

/* FSEntry_File: r[2..5] in and out, the object type in r[0] */
static os_error *t_file(const struct tgt *t, uint32_t reason, uint32_t r[8])
{
    uint32_t path = rma_str(t->path);
    if (!path)
        return no_room();
    r[0] = reason, r[1] = path, r[6] = special_of(t);
    os_error *e = call_entry(t->m, t->m->file, r, NULL);
    rma_free(path);
    return e;
}

/* ComplexAdjustObjectTypeReMultiFS's "not found" errors: &xxD6 in any
 * filing system's range, as FileCore's &108D6 */
static int not_found_error(const os_error *e)
{
    return (e->errnum & ~0xFF00u) == (0x10000u | E_FILE_NOT_FOUND) || e->errnum == E_FILE_NOT_FOUND;
}

static os_error *t_stat(const struct tgt *t, struct fs_info *info)
{
    if (!t->m)
        return t->fs->stat(t->disc, t->path, info);
    uint32_t r[8] = { 0 };
    os_error *e = t_file(t, FSFILE_READINFO, r);
    memset(info, 0, sizeof *info);
    if (e)
        return not_found_error(e) ? NULL : e;
    info->type = r[0];
    info->load = r[2], info->exec = r[3], info->length = r[4], info->attr = r[5];
    if (info->type > OBJ_DIR + OBJ_FILE)
        info->type = OBJ_NOTHING;
    /* the root: "$" that is neither file nor directory is a directory
     * (ComplexAdjustObjectTypeReMultiFS) */
    size_t n = strlen(t->path);
    if (info->type == OBJ_NOTHING && (!n || t->path[n - 1] == '$') && t->img < 0) {
        info->type = OBJ_DIR;
        info->load = 0xFFFFFD00u, info->exec = 0;
    }
    if (t->img >= 0 && !n) {                    /* an image's root is the image file */
        info->type = OBJ_DIR;
    }
    return NULL;
}

/* ---- directories, read through FSEntry_Func 15, kept while they are walked ------------- */

struct listing {
    int valid;
    struct mfs *m;
    uint32_t special;
    char path[1100];
    struct fs_entry *e;
    unsigned n, cap;
};

static struct listing cache;

static os_error *read_listing(const struct tgt *t)
{
    struct listing *L = &cache;
    L->valid = 0;
    L->n = 0;
    uint32_t path = rma_str(t->path), buf = ros_addr(ros_rma_alloc(MAXBUF));
    os_error *e = NULL;
    if (!path || !buf) {
        e = no_room();
        goto out;
    }
    uint32_t offset = 0;
    for (int rounds = 0; rounds < 100000 && offset != 0xFFFFFFFFu; rounds++) {
        uint32_t r[8] = { FSFUNC_READINFO, path, buf, 255, offset, MAXBUF, special_of(t), 0 };
        if ((e = call_entry(t->m, t->m->func, r, NULL)) != NULL)
            goto out;
        uint32_t got = r[3], p = buf;
        for (uint32_t k = 0; k < got; k++) {
            if (L->n == L->cap) {
                unsigned cap = L->cap ? L->cap * 2 : 32;
                struct fs_entry *ne = realloc(L->e, cap * sizeof *ne);
                if (!ne) {
                    e = ros_error(0x182, "Not enough memory");
                    goto out;
                }
                L->e = ne, L->cap = cap;
            }
            struct fs_entry *x = &L->e[L->n++];
            x->info.load = ros_ld32(p), x->info.exec = ros_ld32(p + 4);
            x->info.length = ros_ld32(p + 8), x->info.attr = ros_ld32(p + 12);
            x->info.type = ros_ld32(p + 16);
            size_t k2 = 0;
            const char *s = ros_ptr(p + 20);
            while (k2 + 1 < sizeof x->name && (unsigned char)s[k2] >= ' ')
                k2++;
            memcpy(x->name, s, k2);
            x->name[k2] = 0;
            p += (20 + (uint32_t)strlen(s) + 1 + 3) & ~3u;
        }
        if (got == 0 && r[4] == offset)
            break;                              /* nothing read and nowhere to go */
        offset = r[4];
    }
    L->valid = 1;
    L->m = t->m;
    L->special = special_of(t);
    snprintf(L->path, sizeof L->path, "%s", t->path);
out:
    rma_free(path);
    rma_free(buf);
    return e;
}

static int t_readdir(const struct tgt *t, uint32_t index, struct fs_entry *ent, os_error **err)
{
    *err = NULL;
    if (!t->m)
        return t->fs->readdir(t->disc, t->path, index, ent, err);
    struct listing *L = &cache;
    if (index == 0 || !L->valid || L->m != t->m || L->special != special_of(t) ||
        strcmp(L->path, t->path) != 0)
        if ((*err = read_listing(t)) != NULL)
            return 0;
    if (index >= L->n)
        return 0;
    *ent = L->e[index];
    return 1;
}

static void forget_listing(void)
{
    cache.valid = 0;
}

/* ---- opening an image -------------------------------------------------------------- */

/* OpenMultiFSFile: the image file at l, of image filing system ifs, opened
 * as a stream for update (read only, if it cannot be written) and given to
 * the image filing system */
static os_error *image_open(const struct fsw_loc *l, struct mfs *ifs, int *out)
{
    int i = 0;
    while (i < NIMG && images[i].used)
        i++;
    if (i == NIMG)
        return ros_error(E_TOO_MANY_OPEN, "Too many open files");
    uint32_t h;
    os_error *e = fsw_stream_open(l, 1, &h);
    if (e)
        e = fsw_stream_open(l, 0, &h);
    if (e)
        return e;
    struct image *im = &images[i];
    memset(im, 0, sizeof *im);
    im->used = im->opening = 1;
    im->loc = *l;
    im->ifs = ifs;
    im->handle = h;
    uint32_t r[8] = { FSFUNC_NEWIMAGE, h, fsw_stream_bufsize(h), 0, 0, 0, 0, 0 };
    e = call_entry(ifs, ifs->func, r, NULL);
    if (e) {
        /* the image filing system barfed: close the file, say why */
        im->used = 0;
        fsw_stream_close(h);
        return e;
    }
    im->ih = r[1];
    im->opening = 0;
    *out = i;
    return NULL;
}

int fsw_image_of_stream(uint32_t h)
{
    for (int i = 0; i < NIMG; i++)
        if (images[i].used && images[i].handle == h)
            return i;
    return -1;
}

/* The image's stream is closing (FlushAndCloseStream): the streams inside
 * it are closed first (fileswitch.c), then the image filing system told */
void fsw_image_closing(int img)
{
    struct image *im = &images[img];
    if (!im->used)
        return;
    if (!im->opening) {
        uint32_t r[8] = { FSFUNC_IMAGECLOSE, im->ih, 0, 0, 0, 0, 0, 0 };
        call_entry(im->ifs, im->ifs->func, r, NULL);       /* errors ignored, as RISC OS's */
    }
    im->used = 0;
    forget_listing();
}

/* Service_CloseFile's rule (FileSwitch_Service_CloseFile), and
 * TryGetFileClosed's: each image that is l or below it, with nothing open
 * inside it but other images, is closed.  The number closed. */
unsigned fsw_images_close_at(const struct fsw_loc *l)
{
    unsigned n = 0;
    for (int again = 1; again;) {
        again = 0;
        for (int i = 0; i < NIMG; i++) {
            struct image *im = &images[i];
            if (!im->used || im->opening || !same_place(&im->loc, l))
                continue;
            if (strcasecmp(im->loc.path, l->path) != 0 && !inside(l->path, im->loc.path) &&
                l->path[0])
                continue;
            if (fsw_stream_files_inside(i))
                continue;
            fsw_stream_close(im->handle);
            n++;
            again = 1;
        }
    }
    return n;
}

int fsw_file_image(const struct fsw_file *f);

/* ---- the calls, on a place ------------------------------------------------------------ */

/* EnsureCanonicalObject: what l is.  Where it is not there and an image
 * filing system is registered, the deepest part of its path that is there
 * may be an image file not yet open: then that is opened, and l looked
 * for again inside it. */
os_error *fsw_stat_loc(const struct fsw_loc *l, struct fs_info *info)
{
    for (int tries = 0; tries < 16; tries++) {
        struct tgt t;
        target_of(l, &t);
        os_error *e = t_stat(&t, info);
        if (e)
            return e;
        if (info->type != OBJ_NOTHING) {
            info->type = adjust_type(info->type, info->load);
            return NULL;
        }
        if (!any_ifs())
            return NULL;
        /* peel off leaves until something is there */
        size_t floor = 0;
        int holder = image_holding(l, &floor);
        struct fsw_loc p = *l;
        int opened = 0;
        for (;;) {
            char *dot = strrchr(p.path, '.');
            if (!dot)
                break;
            *dot = 0;
            if (holder >= 0 && strlen(p.path) <= floor)
                break;                          /* the image itself: not inside it */
            struct tgt pt;
            struct fs_info pi;
            target_of(&p, &pt);
            if ((e = t_stat(&pt, &pi)) != NULL)
                return e;
            if (pi.type == OBJ_NOTHING)
                continue;
            struct mfs *ifs = pi.type == OBJ_FILE && fsw_is_typed(pi.load)
                                  ? ifs_for_type(pi.load >> 8 & 0xFFF) : NULL;
            if (!ifs || image_at(&p) >= 0)
                break;                          /* a directory, or a plain file: not there */
            int img;
            if ((e = image_open(&p, ifs, &img)) != NULL)
                return e;
            opened = 1;
            break;
        }
        if (!opened) {
            memset(info, 0, sizeof *info);
            return NULL;
        }
    }
    memset(info, 0, sizeof *info);
    return NULL;
}

/* A directory's index-th entry.  When l is an image file, the entry is the
 * image's, and the image is opened if need be
 * (AssessDestinationForPathTailForDirRead) */
int fsw_readdir(const struct fsw_loc *l, uint32_t index, struct fs_entry *ent, os_error **err)
{
    struct tgt t;
    *err = NULL;
    int img = image_at(l);
    if (img < 0) {
        target_of(l, &t);
        if (any_ifs() && index == 0) {
            struct fs_info info;
            if ((*err = t_stat(&t, &info)) != NULL)
                return 0;
            struct mfs *ifs = info.type == OBJ_FILE && fsw_is_typed(info.load)
                                  ? ifs_for_type(info.load >> 8 & 0xFFF) : NULL;
            if (ifs && (*err = image_open(l, ifs, &img)) != NULL)
                return 0;
        }
    }
    if (img >= 0)
        tgt_image(img, "", &t);
    int more = t_readdir(&t, index, ent, err);
    if (more)
        ent->info.type = adjust_type(ent->info.type, ent->info.load);
    return more;
}

int fsw_end_when_none(const struct fsw_loc *l)
{
    struct tgt t;
    target_of(l, &t);
    return !t.m && image_at(l) < 0 ? t.fs->end_when_none : 0;
}

int fsw_read_only(const struct fsw_loc *l)
{
    return fsw_filing_systems[l->fsi]->read_only;
}

/* Before an object is opened, deleted, created, saved, loaded or renamed:
 * images at it or below closed (TryGetFileClosed) */
static void get_closed(const struct fsw_loc *l)
{
    for (int i = 0; i < NIMG; i++)
        if (images[i].used) {
            fsw_images_close_at(l);
            return;
        }
}

static void stamp_now(uint32_t type, uint32_t *load, uint32_t *exec)
{
    uint64_t cs = fsw_now_cs();
    *load = fsw_stamp_load(type, cs);
    *exec = (uint32_t)cs;
}

os_error *fsw_fs_create(const struct fsw_loc *l, uint32_t load, uint32_t exec, uint32_t length)
{
    get_closed(l);
    forget_listing();
    struct tgt t;
    target_of(l, &t);
    if (!t.m)
        return t.fs->create(t.disc, t.path, load, exec, length);
    uint32_t r[8] = { 0, 0, load, exec, 0, length, 0, 0 };
    return t_file(&t, FSFILE_CREATE, r);
}

os_error *fsw_fs_mkdir(const struct fsw_loc *l)
{
    forget_listing();
    struct tgt t;
    target_of(l, &t);
    if (!t.m)
        return t.fs->mkdir(t.disc, t.path);
    uint32_t r[8] = { 0 };
    stamp_now(0xFFD, &r[2], &r[3]);
    return t_file(&t, FSFILE_CDIR, r);
}

os_error *fsw_fs_remove(const struct fsw_loc *l)
{
    get_closed(l);
    forget_listing();
    struct tgt t;
    target_of(l, &t);
    if (!t.m)
        return t.fs->remove(t.disc, t.path);
    uint32_t r[8] = { 0 };
    return t_file(&t, FSFILE_DELETE, r);
}

/* OS_File 1-4's reason: the native filing systems take it as it is; a
 * module's is given WriteInfo with the other values as they were */
os_error *fsw_fs_setinfo(const struct fsw_loc *l, int reason, uint32_t load, uint32_t exec,
                         uint32_t attr)
{
    forget_listing();
    struct tgt t;
    target_of(l, &t);
    if (!t.m)
        return t.fs->setinfo(t.disc, t.path, reason, load, exec, attr);
    struct fs_info i;
    os_error *e = t_stat(&t, &i);
    if (e)
        return e;
    uint32_t r[8] = { 0, 0, reason == 1 || reason == 2 ? load : i.load,
                      reason == 1 || reason == 3 ? exec : i.exec, 0,
                      reason == 1 || reason == 4 ? attr : i.attr, 0, 0 };
    return t_file(&t, FSFILE_WRITEINFO, r);
}

os_error *fsw_fs_rename(const struct fsw_loc *from, const struct fsw_loc *to)
{
    get_closed(from);
    forget_listing();
    struct tgt a, b;
    target_of(from, &a);
    target_of(to, &b);
    if (a.m != b.m || a.img != b.img)
        return ros_error(E_BAD_RENAME, "Bad rename");
    if (!a.m)
        return a.fs->rename(a.disc, from->path, to->path);
    uint32_t p1 = rma_str(a.path), p2 = rma_str(b.path);
    os_error *e = NULL;
    if (!p1 || !p2) {
        e = no_room();
    } else {
        uint32_t r[8] = { FSFUNC_RENAME, p1, p2, 0, 0, 0, special_of(&a), special_of(&b) };
        e = call_entry(a.m, a.m->func, r, NULL);
        if (!e && r[1] != 0)
            e = ros_error(E_BAD_RENAME, "Bad rename");
    }
    rma_free(p1);
    rma_free(p2);
    return e;
}

os_error *fsw_fs_free(const struct fsw_loc *l, uint64_t *fr, uint64_t *big, uint64_t *size)
{
    struct tgt t;
    target_of(l, &t);
    if (!t.m)
        return t.fs->free_space(t.disc, fr, big, size);
    uint32_t path = rma_str(t.path);
    if (!path)
        return no_room();
    uint32_t r[8] = { FSFUNC_FREESPACE, path, 0, 0, 0, 0, special_of(&t), 0 };
    os_error *e = call_entry(t.m, t.m->func, r, NULL);
    rma_free(path);
    if (!e)
        *fr = r[0], *big = r[1], *size = r[2];
    return e;
}

/* ---- open files -------------------------------------------------------------------- */

struct fsw_file {
    const struct fs *fs;                /* native */
    struct mfs *m;                      /* or a module's, or an image's */
    int img;
    uint32_t fh;
    uint32_t info;                      /* the open's R0 */
    uint32_t bufsize;                   /* 0: unbuffered */
    uint32_t extent, alloc, fs_extent;
    uint32_t buf, bbase;                /* the buffer (RMA) and the offset it holds */
    int bvalid, bdirty;
};

int fsw_file_image(const struct fsw_file *f)
{
    return f->img;
}

uint32_t fsw_file_fh(const struct fsw_file *f)
{
    return f->fh;
}

uint32_t fsw_file_bufsize(const struct fsw_file *f)
{
    return f->bufsize;
}

static os_error *args(struct fsw_file *f, uint32_t reason, uint32_t r2, uint32_t r3, uint32_t *out2)
{
    uint32_t r[8] = { reason, f->fh, r2, r3, 0, 0, 0, 0 };
    os_error *e = call_entry(f->m, f->m->args, r, NULL);
    if (!e && out2)
        *out2 = r[2];
    return e;
}

os_error *fsw_file_open(const struct fsw_loc *l, int write, uint32_t swh, struct fsw_file **out)
{
    get_closed(l);
    struct tgt t;
    target_of(l, &t);
    struct fsw_file *f = calloc(1, sizeof *f);
    if (!f)
        return ros_error(0x182, "Not enough memory");
    f->img = t.img;
    os_error *e;
    if (!t.m) {
        f->fs = t.fs;
        e = t.fs->open(t.disc, t.path, write, &f->fh);
        if (e) {
            free(f);
            return e;
        }
        *out = f;
        return NULL;
    }
    f->m = t.m;
    uint32_t path = rma_str(t.path);
    if (!path) {
        free(f);
        return no_room();
    }
    uint32_t r[8] = { write ? 2u : 0u, path, swh, swh, 0, 0, special_of(&t), 0 };
    e = call_entry(t.m, t.m->open, r, NULL);
    rma_free(path);
    if (!e && r[1] == 0)
        e = fsw_err_not_found(t.path);
    if (!e && r[2] && (r[2] > MAXBUF || r[2] < 64 || (r[2] & (r[2] - 1))))
        e = ros_error(0x41F, "Bad buffer size");
    if (!e && r[2]) {
        f->buf = ros_addr(ros_rma_alloc(r[2]));
        if (!f->buf) {
            uint32_t c[8] = { 0, r[1], 0, 0, 0, 0, 0, 0 };
            call_entry(t.m, t.m->close, c, NULL);
            e = no_room();
        }
    }
    if (e) {
        free(f);
        return e;
    }
    f->info = r[0];
    f->fh = r[1];
    f->bufsize = r[2];
    f->extent = f->fs_extent = r[3];
    f->alloc = r[4];
    *out = f;
    return NULL;
}

/* The buffer: written back if it was changed */
static os_error *flush(struct fsw_file *f)
{
    if (!f->bvalid || !f->bdirty)
        return NULL;
    uint32_t r[8] = { 0, f->fh, f->buf, f->bufsize, f->bbase, 0, 0, 0 };
    os_error *e = call_entry(f->m, f->m->put, r, NULL);
    if (!e)
        f->bdirty = 0;
    return e;
}

/* The block at base in the buffer; what is past the extent is zero */
static os_error *fill(struct fsw_file *f, uint32_t base)
{
    if (f->bvalid && f->bbase == base)
        return NULL;
    os_error *e = flush(f);
    if (e)
        return e;
    f->bvalid = 0;
    memset(ros_ptr(f->buf), 0, f->bufsize);
    if (base < f->extent) {
        uint32_t r[8] = { 0, f->fh, f->buf, f->bufsize, base, 0, 0, 0 };
        if ((e = call_entry(f->m, f->m->get, r, NULL)) != NULL)
            return e;
        if (base + f->bufsize > f->extent && f->extent > base)
            memset((uint8_t *)ros_ptr(f->buf) + (f->extent - base), 0, base + f->bufsize - f->extent);
    }
    f->bvalid = 1;
    f->bbase = base;
    f->bdirty = 0;
    return NULL;
}

uint32_t fsw_file_extent(struct fsw_file *f)
{
    if (f->fs)
        return f->fs->extent(f->fh);
    if (f->bufsize)
        return f->extent;
    uint32_t ext = 0;
    args(f, FSARGS_READEXT, 0, 0, &ext);
    return ext;
}

/* Data to and from the caller's memory: straight there when it is in the
 * arena, else through a block of the RMA */
static os_error *unbuffered_io(struct fsw_file *f, int put, uint32_t pos, void *buf, uint32_t n,
                               uint32_t *done)
{
    *done = 0;
    if (!n)
        return NULL;
    uint32_t a = ros_in_arena(buf) ? ros_addr(buf) : 0, tmp = 0;
    if (!a) {
        tmp = a = ros_addr(ros_rma_alloc(n));
        if (!a)
            return no_room();
        if (put)
            memcpy(ros_ptr(a), buf, n);
    }
    os_error *e = NULL;
    /* OS_BGet and OS_BPut are GetBytes and PutBytes (CallFSBGet), what
     * moves more the GBPB entry where the open offers one */
    if (n > 1 && (f->info & FSOPEN_UNBUFFGBPB) && f->m->gbpb) {
        uint32_t r[8] = { put ? 1u : 3u, f->fh, a, n, pos, 0, 0, 0 };
        e = call_entry(f->m, f->m->gbpb, r, NULL);
        if (!e)
            *done = n - r[3];
    } else {
        /* byte by byte: the pointer, then BGet or BPut */
        e = args(f, FSARGS_SETPTR, pos, 0, NULL);
        for (uint32_t k = 0; !e && k < n; k++) {
            if (put) {
                uint32_t r[8] = { ros_ld8(a + k), f->fh, 0, 0, 0, 0, 0, 0 };
                e = call_entry(f->m, f->m->put, r, NULL);
            } else {
                uint32_t r[8] = { 0, f->fh, 0, 0, 0, 0, 0, 0 };
                int c;
                e = call_entry(f->m, f->m->get, r, &c);
                if (!e && c)
                    break;
                if (!e)
                    ros_st8(a + k, r[0]);
            }
            if (!e)
                (*done)++;
        }
    }
    if (tmp) {
        if (!put && !e)
            memcpy(buf, ros_ptr(tmp), *done);
        rma_free(tmp);
    }
    return e;
}

os_error *fsw_file_read(struct fsw_file *f, uint32_t pos, void *buf, uint32_t n, uint32_t *got)
{
    *got = 0;
    if (f->fs)
        return f->fs->read(f->fh, pos, buf, n, got);
    if (!f->bufsize)
        return unbuffered_io(f, 0, pos, buf, n, got);
    if (pos >= f->extent)
        return NULL;
    if (n > f->extent - pos)
        n = f->extent - pos;
    uint8_t *d = buf;
    uint32_t mask = f->bufsize - 1;
    while (*got < n) {
        uint32_t at = pos + *got, base = at & ~mask, off = at - base;
        uint32_t k = f->bufsize - off;
        if (k > n - *got)
            k = n - *got;
        /* whole blocks the cache does not hold, straight into arena memory */
        if (!off && k == f->bufsize && ros_in_arena(d + *got) && !(f->bvalid && f->bbase == base) &&
            base + f->bufsize <= f->extent) {
            uint32_t r[8] = { 0, f->fh, ros_addr(d + *got), f->bufsize, base, 0, 0, 0 };
            os_error *e = call_entry(f->m, f->m->get, r, NULL);
            if (e)
                return e;
        } else {
            os_error *e = fill(f, base);
            if (e)
                return e;
            memcpy(d + *got, (uint8_t *)ros_ptr(f->buf) + off, k);
        }
        *got += k;
    }
    return NULL;
}

/* The file made at least size long on the filing system's medium */
static os_error *ensure(struct fsw_file *f, uint32_t size)
{
    if (size <= f->alloc)
        return NULL;
    uint32_t got = size;
    os_error *e = args(f, FSARGS_ENSURE, size, 0, &got);
    if (!e)
        f->alloc = got < size ? size : got;
    return e;
}

os_error *fsw_file_write(struct fsw_file *f, uint32_t pos, const void *buf, uint32_t n)
{
    if (f->fs)
        return f->fs->write(f->fh, pos, buf, n);
    if (!n)
        return NULL;
    if (!f->bufsize) {
        uint32_t done;
        return unbuffered_io(f, 1, pos, (void *)buf, n, &done);
    }
    os_error *e = ensure(f, pos + n);
    if (e)
        return e;
    if (pos > f->extent) {                      /* a gap: zeros on the medium */
        if ((e = flush(f)) != NULL)
            return e;
        f->bvalid = 0;
        if ((e = args(f, FSARGS_ZEROES, f->extent, pos - f->extent, NULL)) != NULL)
            return e;
        f->extent = pos;
    }
    const uint8_t *s = buf;
    uint32_t mask = f->bufsize - 1, done = 0;
    while (done < n) {
        uint32_t at = pos + done, base = at & ~mask, off = at - base;
        uint32_t k = f->bufsize - off;
        if (k > n - done)
            k = n - done;
        if ((e = fill(f, base)) != NULL)
            return e;
        memcpy((uint8_t *)ros_ptr(f->buf) + off, s + done, k);
        f->bdirty = 1;
        done += k;
        if (at + k > f->extent)
            f->extent = at + k;
    }
    return NULL;
}

os_error *fsw_file_set_extent(struct fsw_file *f, uint32_t ext)
{
    if (f->fs)
        return f->fs->set_extent(f->fh, ext);
    if (!f->bufsize)
        return args(f, FSARGS_SETEXT, ext, 0, NULL);
    os_error *e = flush(f);
    if (!e)
        e = ensure(f, ext);
    if (!e && ext > f->extent)
        e = args(f, FSARGS_ZEROES, f->extent, ext - f->extent, NULL);
    if (!e)
        e = args(f, FSARGS_SETEXT, ext, 0, NULL);
    if (e)
        return e;
    f->extent = f->fs_extent = ext;
    if (f->bvalid && f->bbase >= ext)
        f->bvalid = 0;
    else if (f->bvalid && f->bbase + f->bufsize > ext)
        memset((uint8_t *)ros_ptr(f->buf) + (ext - f->bbase), 0, f->bbase + f->bufsize - ext);
    return NULL;
}

/* OS_Args 255 and 6: nothing is held back past this */
os_error *fsw_file_flush(struct fsw_file *f)
{
    if (f->fs)
        return NULL;
    os_error *e = flush(f);
    if (!e && f->bufsize && f->extent != f->fs_extent) {
        e = args(f, FSARGS_SETEXT, f->extent, 0, NULL);
        if (!e)
            f->fs_extent = f->extent;
    }
    if (!e && (f->m->info & FSINFO_FLUSHNOTIFY))
        e = args(f, FSARGS_FLUSH, 0, 0, NULL);
    return e;
}

/* FlushAndCloseStream: the buffer written, the real extent set, and a
 * modified file restamped (ReadLoadExec, then its type with the time now);
 * an unmodified one closed with load and exec 0, "do not restamp" */
os_error *fsw_file_close(struct fsw_file *f, int modified)
{
    os_error *e = NULL;
    if (f->fs) {
        e = f->fs->close(f->fh);
        free(f);
        return e;
    }
    e = flush(f);
    if (!e && f->bufsize && f->extent != f->fs_extent)
        e = args(f, FSARGS_SETEXT, f->extent, 0, NULL);
    uint32_t load = 0, exec = 0;
    if (modified && !e) {
        uint32_t r[8] = { FSARGS_LOADEXEC, f->fh, 0, 0, 0, 0, 0, 0 };
        if (!call_entry(f->m, f->m->args, r, NULL))
            stamp_now(r[2] >= 0xFFF00000u ? r[2] >> 8 & 0xFFF : 0xFFD, &load, &exec);
    }
    uint32_t r[8] = { 0, f->fh, load, exec, 0, 0, 0, 0 };
    os_error *e2 = call_entry(f->m, f->m->close, r, NULL);
    rma_free(f->buf);
    free(f);
    forget_listing();
    return e ? e : e2;
}

/* ---- whole files --------------------------------------------------------------------- */

static uint32_t info_word_of(const struct tgt *t)
{
    return t->m ? t->m->info : 0;
}

/* int_DoSaveFile: a module's filing system is given FSEntry_File 0, unless
 * it opts out (fsinfo_dontusesave) or is an image, which are given a create,
 * an open for update, the data, and a close.  The native ones save as
 * HostFS does, the same way. */
os_error *fsw_fs_save(const struct fsw_loc *l, uint32_t load, uint32_t exec, uint32_t start,
                      uint32_t end)
{
    get_closed(l);
    forget_listing();
    struct tgt t;
    target_of(l, &t);
    if (t.m && t.img < 0 && !(info_word_of(&t) & FSINFO_DONTUSESAVE)) {
        uint32_t r[8] = { 0, 0, load, exec, start, end, 0, 0 };
        return t_file(&t, FSFILE_SAVE, r);
    }
    os_error *e = fsw_fs_create(l, load, exec, 0);
    if (e || end == start)
        return e;
    struct fsw_file *f;
    if ((e = fsw_file_open(l, 1, 0, &f)) != NULL)
        return e;
    e = fsw_file_write(f, 0, ros_ptr(start), end - start);
    os_error *e2 = fsw_file_close(f, 0);
    if (e)
        return e;
    if (e2)
        return e2;
    /* writing moved the date: put the one asked for back */
    return fsw_is_typed(load) ? fsw_fs_setinfo(l, 2, load, exec, 0) : NULL;
}

/* int_DoLoadFile: FSEntry_File 255, unless the filing system opts out
 * (fsinfo_dontuseload) or is an image: then an open, the data, a close */
os_error *fsw_fs_load(const struct fsw_loc *l, uint32_t addr, uint32_t length)
{
    get_closed(l);
    struct tgt t;
    target_of(l, &t);
    if (t.m && t.img < 0 && !(info_word_of(&t) & FSINFO_DONTUSELOAD)) {
        uint32_t r[8] = { 0, 0, addr, 0, 0, 0, 0, 0 };
        return t_file(&t, FSFILE_LOAD, r);
    }
    struct fsw_file *f;
    os_error *e = fsw_file_open(l, 0, 0, &f);
    if (e)
        return e;
    uint32_t got;
    e = length ? fsw_file_read(f, 0, ros_ptr(addr), length, &got) : NULL;
    fsw_file_close(f, 0);
    return e;
}

/* ---- OS_FSControl 12, 16, 35, 36 ------------------------------------------------------- */

static uint32_t entry_at(uint32_t base, uint32_t block, unsigned word)
{
    uint32_t off = ros_ld32(block + 4 * word);
    if (!off || ((base + off) & 3))
        return 0;                               /* Lowlevel_UnsupportedFSEntry */
    return base + off;
}

static int name_char(uint32_t c)
{
    return c > ' ' && c != 127;
}

static int slot_of(const struct fs *fs)
{
    for (int i = 0; i < FSW_MAXFS; i++)
        if (fsw_filing_systems[i] == fs)
            return i;
    return -1;
}

/* AddFSEntry: R1 the module's base, R2 its block's offset, R3 R12 for its
 * entries.  A filing system already there by name is updated. */
os_error *fsw_add_fs(uint32_t base, uint32_t offset, uint32_t r12)
{
    uint32_t block = base + offset;
    uint32_t noff = ros_ld32(block);
    if (!noff)
        return ros_error(0xF8, "Bad filing system name");
    uint32_t np = base + noff;
    char name[64];
    size_t n = 0;
    while (n + 1 < sizeof name && name_char(ros_ld8(np + n)))
        name[n] = (char)ros_ld8(np + n), n++;
    name[n] = 0;
    if (!n)
        return ros_error(0xF8, "Bad filing system name");
    struct mfs *m = NULL;
    for (int i = 0; i < NMFS; i++)
        if (mods[i].used && !strcasecmp(mods[i].name, name))
            m = &mods[i];
    int fresh = !m;
    if (fresh) {
        for (int i = 0; i < NMFS && !m; i++)
            if (!mods[i].used)
                m = &mods[i];
        if (!m)
            return ros_error(0x182, "Not enough memory");
    }
    int fsi = fresh ? -1 : slot_of(&m->fs);
    if (fresh) {
        for (int i = 0; i < FSW_MAXFS && fsi < 0; i++)
            if (!fsw_filing_systems[i])
                fsi = i;
        if (fsi < 0)
            return ros_error(0x182, "Not enough memory");
        memset(m, 0, sizeof *m);
    }
    m->used = 1;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base, m->r12 = r12;
    m->open = entry_at(base, block, 2);
    m->get = entry_at(base, block, 3);
    m->put = entry_at(base, block, 4);
    m->args = entry_at(base, block, 5);
    m->close = entry_at(base, block, 6);
    m->file = entry_at(base, block, 7);
    m->info = ros_ld32(block + 32);
    m->func = entry_at(base, block, 9);
    m->gbpb = entry_at(base, block, 10);
    m->extra = m->info & FSINFO_EXTRAINFO ? ros_ld32(block + 44) : 0;
    m->fs.name = m->name;
    m->fs.number = m->info & 0xFF;
    m->fs.read_only = (m->info & FSINFO_READONLY) != 0;
    m->fs.info = m->info & ~(0xFFu | FSINFO_READONLY);
    m->fs.boot_disc = no_disc;
    m->fs.mfs = m;
    if (fresh) {
        fsw_filing_systems[fsi] = &m->fs;
        memset(&fsw_dirs[fsi], 0, sizeof fsw_dirs[fsi]);
    }
    fsw_fs_blocks_changed();
    return NULL;
}

/* RemoveFS: by name only (a number would be an accident: "can't remove by
 * number"); its files are closed, and it is no longer selected */
os_error *fsw_remove_fs(int fsi)
{
    const struct fs *fs = fsw_filing_systems[fsi];
    struct mfs *m = mfs_of(fs);
    if (!m)
        return ros_error(0x8E, "Filing system %s cannot be removed", fs->name);
    fsw_close_all_on(fsi, NULL);
    fsw_filing_systems[fsi] = NULL;
    memset(&fsw_dirs[fsi], 0, sizeof fsw_dirs[fsi]);
    if (fsw_current == fsi)
        fsw_current = -1;
    if (fsw_temp == fsi)
        fsw_temp = -1;
    m->used = 0;
    forget_listing();
    fsw_fs_blocks_changed();
    return NULL;
}

/* AddImageFSEntry: the block is the information word, the file type, then
 * Open, GetBytes, PutBytes, Args, Close, File and Func.  One already there
 * for the type is updated. */
os_error *fsw_add_image_fs(uint32_t base, uint32_t offset, uint32_t r12)
{
    uint32_t block = base + offset;
    uint32_t type = ros_ld32(block + 4);
    struct mfs *m = ifs_for_type(type);
    for (int i = 0; i < NIFS && !m; i++)
        if (!imfs[i].used)
            m = &imfs[i];
    if (!m)
        return ros_error(0x182, "Not enough memory");
    memset(m, 0, sizeof *m);
    m->used = m->image = 1;
    m->base = base, m->r12 = r12;
    m->info = ros_ld32(block) & ~FSINFO_NOTFORMULTIFS;
    m->filetype = type;
    m->open = entry_at(base, block, 2);
    m->get = entry_at(base, block, 3);
    m->put = entry_at(base, block, 4);
    m->args = entry_at(base, block, 5);
    m->close = entry_at(base, block, 6);
    m->file = entry_at(base, block, 7);
    m->func = entry_at(base, block, 8);
    snprintf(m->name, sizeof m->name, "Image &%03X", type & 0xFFF);
    forget_listing();
    return NULL;
}

/* RemoveImageFSEntry: by file type; its images, and what is open in them,
 * closed (CloseAllFilesOnThisFS: the streams used inside it) */
os_error *fsw_remove_image_fs(uint32_t type)
{
    struct mfs *m = ifs_for_type(type);
    if (!m)
        return ros_error(E_UNKNOWN_FS, "Filing system not present");
    for (int i = 0; i < NIMG; i++)
        if (images[i].used && images[i].ifs == m)
            fsw_stream_close(images[i].handle);
    m->used = 0;
    forget_listing();
    return NULL;
}

/* OS_FSControl 20: a module's filing system's base and R12 */
int fsw_module_of(int fsi, uint32_t *base, uint32_t *r12)
{
    struct mfs *m = fsi >= 0 ? mfs_of(fsw_filing_systems[fsi]) : NULL;
    if (!m)
        return 0;
    *base = m->base, *r12 = m->r12;
    return 1;
}

/* OS_FSControl 15: the current filing system's FSEntry_Func 10, Bootup */
os_error *fsw_bootup_fs(int fsi)
{
    struct mfs *m = fsi >= 0 ? mfs_of(fsw_filing_systems[fsi]) : NULL;
    if (!m)
        return NULL;
    uint32_t r[8] = { FSFUNC_BOOTUP, 0, 0, 0, 0, 0, 0, 0 };
    return call_entry(m, m->func, r, NULL);
}

/* *ShutDown's FSEntry_Func 16 for each module's filing system */
void fsw_shutdown_fs(void)
{
    for (int i = 0; i < NMFS; i++)
        if (mods[i].used) {
            uint32_t r[8] = { FSFUNC_SHUTDOWN, 0, 0, 0, 0, 0, 0, 0 };
            call_entry(&mods[i], mods[i].func, r, NULL);
        }
}

/* FileSwitch dies: forget them all (their modules die with it, or tell it
 * again, Service_FSRedeclare) */
void fsw_forget_modules(void)
{
    for (int i = 0; i < FSW_MAXFS; i++)
        if (fsw_filing_systems[i] && mfs_of(fsw_filing_systems[i]))
            fsw_filing_systems[i] = NULL;
    memset(mods, 0, sizeof mods);
    memset(imfs, 0, sizeof imfs);
    memset(images, 0, sizeof images);
    forget_listing();
}
