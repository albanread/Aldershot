/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* hostfs.c -- HostFS: RISC OS files on Linux's, one disc per mount.
 *
 * The mapping between the two is the team's HostFS's. It was ported from
 * the host side of that module (RISCOSQEMUA72 hw/misc/vmchannel.c), so a
 * share reads the same from ROSGD as from the emulator.
 *
 *   - Names: a RISC OS "/" is a host ".", and a hard space is a space.
 *     Acorn Latin-1 is UTF-8 on the host.  What a host could not store goes
 *     to U+F000 + c.  That is < > " | ? *, a trailing dot or space, a
 *     Windows device name, and a ",xxx" that the name itself ends with.
 *     Lookups are case-insensitive. When the literal name misses, the
 *     directory is scanned.
 *   - Types: a ",xxx" suffix, or the extension through the type table
 *     (typemap.txt).  An executable type is never taken from an extension.
 *     Text needs no decoration, and a name whose extension already says its
 *     type is left alone.  Setting a type renames the host file, and a
 *     handle open on it follows.
 *   - Dates: the host's mtime, in UTC, as the RISC OS five-byte time in the
 *     load and exec words of a typed file.  Every file is typed.
 *   - Attributes: the whole byte is kept in an extended attribute when one
 *     has been set, and derived from the mode when not.  Locked is enforced
 *     here.
 *   - A file that is open is not deleted, renamed or opened again for
 *     writing, and is not opened for reading while it is open for update.
 *     These are FileCore's rules.
 *   - Nothing is reached outside a disc's directory, symlinks included.
 *
 * A disc is a Linux directory, or a set of calls in place of Linux's
 * (struct ros_hostio: LanManFS's SMB shares, over libsmb2).  Every host
 * path HostFS makes starts with its disc's root.  The root of such a disc
 * is "io:<name>", which no Linux path can be, because they are absolute.
 * So the io_ and hf_ functions below choose the calls by the path.
 *
 * Errors are HostFS's: filing system 220, as the team's module uses,
 * &0001DCxx with FileCore's texts.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "fileswitch.h"
#include "fsw.h"
#include "rosgd/heap.h"

#define FS_NUMBER 220u
#define ERR(code) (0x10000u | FS_NUMBER << 8 | (code))
/* FileCore's text for &C3 since RISC OS 3.1 ("Locked" before), which 5.30's
 * HostFS gives too (OS_File 0 onto a file *Access LR/r on the farm) and
 * Filer_Action shows in its window when a copy meets a locked file */
#define E_LOCKED_TEXT "This item is locked to stop changes being made to it"

#define MAX_DISCS 8
#define MAX_OPEN 256

struct disc {
    char name[32];
    char root[PATH_MAX];
    int nine_p;                         /* over 9p: -1 not yet known (nine_p_disc) */
    const struct ros_hostio *io;        /* NULL: Linux's files */
    void *ctx;
};

/* A file open on the host: a descriptor, or a handle of a disc's calls */
struct hf {
    int fd;
    const struct ros_hostio *io;
    void *ctx;
};

static struct disc discs[MAX_DISCS];
static int ndiscs;

/* Open files: the fd and the resolved path, for FileCore's rules */
static struct {
    struct hf f;
    int write;
    int nine_p;                         /* on a 9p share (nine_p_disc) */
    int flush;                          /* so, and written: fsync at the close */
    uint32_t extent;                    /* the last seen, for a file gone */
    char path[PATH_MAX];
} open_files[MAX_OPEN];

extern const struct ros_typemap_entry {
    const char *ext;
    uint16_t type;
} ros_typemap[];
extern const unsigned ros_typemap_count;

static int mount_disc(const char *name, const char *root, const struct ros_hostio *io, void *ctx)
{
    struct disc *d = NULL;
    for (int i = 0; i < ndiscs && !d; i++)
        if (strcasecmp(discs[i].name, name) == 0)
            d = &discs[i];
    if (!d) {
        if (ndiscs == MAX_DISCS)
            return -1;
        d = &discs[ndiscs++];
        snprintf(d->name, sizeof d->name, "%s", name);
    }
    snprintf(d->root, sizeof d->root, "%s", root);
    d->nine_p = -1;
    d->io = io;
    d->ctx = ctx;
    return 0;
}

int ros_hostfs_mount(const char *name, const char *root)
{
    return mount_disc(name, root, NULL, NULL);
}

int ros_hostfs_mount_io(const char *name, const struct ros_hostio *io, void *ctx)
{
    char root[64];
    snprintf(root, sizeof root, "io:%s", name);
    return mount_disc(name, root, io, ctx);
}

int ros_hostfs_unmount(const char *name)
{
    for (int i = 0; i < ndiscs; i++)
        if (strcasecmp(discs[i].name, name) == 0) {
            memmove(&discs[i], &discs[i + 1], (size_t)(ndiscs - i - 1) * sizeof discs[0]);
            ndiscs--;
            return 0;
        }
    return -1;
}

const char *ros_hostfs_disc(unsigned index)
{
    return index < (unsigned)ndiscs ? discs[index].name : NULL;
}

static const struct disc *find_disc(const char *name)
{
    for (int i = 0; i < ndiscs; i++)
        if (strcasecmp(discs[i].name, name) == 0)
            return &discs[i];
    return NULL;
}

static os_error *err_not_found(void)
{
    return ros_error(ERR(0xD6), "Not found");
}

static os_error *err_errno(int e)
{
    switch (e) {
    case ENOENT: case ENOTDIR: return err_not_found();
    case ENOTEMPTY: case EEXIST: return ros_error(ERR(0xB4), "Directory not empty");
    case EACCES: case EPERM: return ros_error(ERR(0xBD), "Access violation");
    case ENOSPC: return ros_error(ERR(0xC6), "Disc full");
    case EROFS: return ros_error(ERR(0xC9), "Disc protected");
    default: return ros_error(ERR(0xC7), "Disc error (%s)", strerror(e));
    }
}

/* ---- the host's calls ------------------------------------------------------- */

/* The disc of calls a host path is on, and the path within it; NULL for a
 * Linux path */
static const struct disc *io_disc(const char *p, const char **rel)
{
    if (p[0] == '/')
        return NULL;
    for (int i = 0; i < ndiscs; i++)
        if (discs[i].io) {
            size_t n = strlen(discs[i].root);
            if (!strncmp(p, discs[i].root, n) && (p[n] == 0 || p[n] == '/')) {
                *rel = p[n] ? p + n + 1 : "";
                return &discs[i];
            }
        }
    return NULL;
}

/* A disc's -errno as a POSIX call's -1 and errno */
static int io_ret(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

static int io_stat(const char *p, struct stat *st)
{
    const char *r;
    const struct disc *d = io_disc(p, &r);
    return d ? io_ret(d->io->stat(d->ctx, r, st)) : stat(p, st);
}

static int io_lstat(const char *p, struct stat *st)
{
    const char *r;
    const struct disc *d = io_disc(p, &r);
    return d ? io_ret(d->io->stat(d->ctx, r, st)) : lstat(p, st);
}

static int io_mkdir(const char *p)
{
    const char *r;
    const struct disc *d = io_disc(p, &r);
    return d ? io_ret(d->io->mkdir(d->ctx, r)) : mkdir(p, 0755);
}

static int io_rmdir(const char *p)
{
    const char *r;
    const struct disc *d = io_disc(p, &r);
    return d ? io_ret(d->io->rmdir(d->ctx, r)) : rmdir(p);
}

static int io_unlink(const char *p)
{
    const char *r;
    const struct disc *d = io_disc(p, &r);
    return d ? io_ret(d->io->unlink(d->ctx, r)) : unlink(p);
}

static int io_rename(const char *from, const char *to)
{
    const char *r1, *r2;
    const struct disc *d1 = io_disc(from, &r1), *d2 = io_disc(to, &r2);
    if (d1 != d2) {
        errno = EXDEV;
        return -1;
    }
    return d1 ? io_ret(d1->io->rename(d1->ctx, r1, r2)) : rename(from, to);
}

/* A listing: Linux's, or a disc's.  "." and ".." are left out; with st,
 * each entry's lstat as well, and an entry that has none is left out. */
struct hdir {
    DIR *d;
    const struct disc *disc;
    void *h;
    char path[PATH_MAX];
};

static struct hdir *io_opendir(const char *p)
{
    struct hdir *hd = malloc(sizeof *hd);
    if (!hd) {
        errno = ENOMEM;
        return NULL;
    }
    const char *r;
    hd->disc = io_disc(p, &r);
    hd->d = NULL, hd->h = NULL;
    snprintf(hd->path, sizeof hd->path, "%s", p);
    int err = 0;
    if (hd->disc ? !(hd->h = hd->disc->io->opendir(hd->disc->ctx, r, &err))
                 : !(hd->d = opendir(p))) {
        if (hd->disc)
            errno = err ? -err : EIO;
        free(hd);
        return NULL;
    }
    return hd;
}

static const char *io_readdir(struct hdir *hd, struct stat *st)
{
    for (;;) {
        const char *name;
        struct stat dst;
        if (hd->disc) {
            name = hd->disc->io->readdir(hd->disc->ctx, hd->h, &dst);
        } else {
            struct dirent *e = readdir(hd->d);
            name = e ? e->d_name : NULL;
        }
        if (!name)
            return NULL;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        if (st && hd->disc) {
            *st = dst;
        } else if (st) {
            char one[PATH_MAX];
            snprintf(one, sizeof one, "%s/%s", hd->path, name);
            if (lstat(one, st) != 0)
                continue;
        }
        return name;
    }
}

static void io_closedir(struct hdir *hd)
{
    if (hd->disc)
        hd->disc->io->closedir(hd->disc->ctx, hd->h);
    else
        closedir(hd->d);
    free(hd);
}

/* A file opened through the disc's calls or Linux's: 0 or -1 and errno */
static int hf_open(const char *p, int flags, struct hf *f)
{
    const char *r;
    const struct disc *d = io_disc(p, &r);
    f->io = d ? d->io : NULL;
    f->ctx = d ? d->ctx : NULL;
    if (d) {
        int h = d->io->open(d->ctx, r, flags);
        f->fd = h;
        return io_ret(h);
    }
    f->fd = open(p, flags, 0644);
    return f->fd < 0 ? -1 : 0;
}

static int hf_ftruncate(const struct hf *f, uint64_t length)
{
    return f->io ? io_ret(f->io->ftruncate(f->ctx, f->fd, length)) : ftruncate(f->fd, (off_t)length);
}

static int hf_fstat(const struct hf *f, struct stat *st)
{
    return f->io ? io_ret(f->io->fstat(f->ctx, f->fd, st)) : fstat(f->fd, st);
}

static void hf_close(const struct hf *f)
{
    if (f->io)
        f->io->close(f->ctx, f->fd);
    else
        close(f->fd);
}

/* ---- names ------------------------------------------------------------------ */

/* The Acorn additions at &80-&9F (Fonts/Encodings/Latin1) */
static const uint32_t acorn_80[32] = {
    0x20AC, 0x0174, 0x0175, 0x0083, 0x0084, 0x0176, 0x0177, 0x0087,
    0x0088, 0x0089, 0x008A, 0x008B, 0x2026, 0x2122, 0x2030, 0x2022,
    0x2018, 0x2019, 0x2039, 0x203A, 0x201C, 0x201D, 0x201E, 0x2013,
    0x2014, 0x2212, 0x0152, 0x0153, 0x2020, 0x2021, 0xFB01, 0xFB02,
};

static size_t put_utf8(char *out, size_t at, size_t max, uint32_t u)
{
    char b[4];
    size_t n;
    if (u < 0x80) {
        b[0] = (char)u, n = 1;
    } else if (u < 0x800) {
        b[0] = (char)(0xC0 | u >> 6), b[1] = (char)(0x80 | (u & 0x3F)), n = 2;
    } else if (u < 0x10000) {
        b[0] = (char)(0xE0 | u >> 12), b[1] = (char)(0x80 | ((u >> 6) & 0x3F));
        b[2] = (char)(0x80 | (u & 0x3F)), n = 3;
    } else {
        b[0] = (char)(0xF0 | u >> 18), b[1] = (char)(0x80 | ((u >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((u >> 6) & 0x3F)), b[3] = (char)(0x80 | (u & 0x3F)), n = 4;
    }
    for (size_t i = 0; i < n && at + 1 < max; i++)
        out[at++] = b[i];
    out[at < max ? at : max - 1] = 0;
    return at;
}

/* One UTF-8 character from p: its code point, and how many bytes; 0xFFFD
 * for a byte that is not UTF-8. */
static uint32_t get_utf8(const unsigned char *p, size_t *len)
{
    uint32_t u;
    size_t n;
    if (p[0] < 0x80)
        u = p[0], n = 1;
    else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
        u = (p[0] & 0x1Fu) << 6 | (p[1] & 0x3Fu), n = 2;
    else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
        u = (p[0] & 0x0Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu), n = 3;
    else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
             (p[3] & 0xC0) == 0x80)
        u = (p[0] & 0x07u) << 18 | (p[1] & 0x3Fu) << 12 | (p[2] & 0x3Fu) << 6 | (p[3] & 0x3Fu),
        n = 4;
    else
        u = 0xFFFD, n = 1;
    *len = n;
    return u;
}

static int win32_reserved(const char *name, size_t len)
{
    static const char *dev[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
                                 "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2",
                                 "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL };
    size_t stem = 0;
    while (stem < len && name[stem] != '.')
        stem++;
    for (int i = 0; dev[i]; i++)
        if (stem == strlen(dev[i]) && strncasecmp(name, dev[i], stem) == 0)
            return 1;
    return 0;
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* A RISC OS leafname, as the host spells it (vmchannel's host_name_of and
 * escape_host_leaf). */
static void host_name_of(const char *guest, char *out, size_t max)
{
    char ideal[1024];
    size_t n = 0;
    ideal[0] = 0;
    for (const unsigned char *p = (const unsigned char *)guest; *p; p++) {
        if (*p == '/')
            n = put_utf8(ideal, n, sizeof ideal, '.');
        else if (*p == 0xA0)
            n = put_utf8(ideal, n, sizeof ideal, ' ');
        else if (*p < 0x80)
            n = put_utf8(ideal, n, sizeof ideal, *p);
        else
            n = put_utf8(ideal, n, sizeof ideal, *p < 0xA0 ? acorn_80[*p - 0x80] : *p);
    }
    size_t len = n, end = len, i = 0, comma = (size_t)-1, o = 0;
    uint32_t trail = 0;
    out[0] = 0;
    if (end > 0 && (ideal[end - 1] == '.' || ideal[end - 1] == ' '))
        trail = (unsigned char)ideal[--end];
    if (end >= 4 && ideal[end - 4] == ',' && is_hex(ideal[end - 3]) && is_hex(ideal[end - 2]) &&
        is_hex(ideal[end - 1]))
        comma = end - 4;
    if (win32_reserved(ideal, end)) {
        o = put_utf8(out, o, max, 0xF000 + (unsigned char)ideal[0]);
        i = 1;
    }
    for (; i < end; i++) {
        unsigned char c = (unsigned char)ideal[i];
        if (i == comma || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
            o = put_utf8(out, o, max, 0xF000 + c);
        else if (o + 1 < max) {
            out[o++] = (char)c;
            out[o] = 0;
        }
    }
    if (trail)
        o = put_utf8(out, o, max, 0xF000 + trail);
}

/* A letter and a combining accent after it, as the one Latin-1 character
 * they make: a host that stores names decomposed (NFD, as macOS may) gives
 * "e" and U+0301 where RISC OS has "\xE9" (from Unicode's decompositions). */
static const struct {
    uint16_t base, mark;
    uint8_t latin1;
} compose[] = {
    { 0x41, 0x0300, 0xC0 }, { 0x41, 0x0301, 0xC1 }, { 0x41, 0x0302, 0xC2 },
    { 0x41, 0x0303, 0xC3 }, { 0x41, 0x0308, 0xC4 }, { 0x41, 0x030A, 0xC5 },
    { 0x43, 0x0327, 0xC7 }, { 0x45, 0x0300, 0xC8 }, { 0x45, 0x0301, 0xC9 },
    { 0x45, 0x0302, 0xCA }, { 0x45, 0x0308, 0xCB }, { 0x49, 0x0300, 0xCC },
    { 0x49, 0x0301, 0xCD }, { 0x49, 0x0302, 0xCE }, { 0x49, 0x0308, 0xCF },
    { 0x4E, 0x0303, 0xD1 }, { 0x4F, 0x0300, 0xD2 }, { 0x4F, 0x0301, 0xD3 },
    { 0x4F, 0x0302, 0xD4 }, { 0x4F, 0x0303, 0xD5 }, { 0x4F, 0x0308, 0xD6 },
    { 0x55, 0x0300, 0xD9 }, { 0x55, 0x0301, 0xDA }, { 0x55, 0x0302, 0xDB },
    { 0x55, 0x0308, 0xDC }, { 0x59, 0x0301, 0xDD }, { 0x61, 0x0300, 0xE0 },
    { 0x61, 0x0301, 0xE1 }, { 0x61, 0x0302, 0xE2 }, { 0x61, 0x0303, 0xE3 },
    { 0x61, 0x0308, 0xE4 }, { 0x61, 0x030A, 0xE5 }, { 0x63, 0x0327, 0xE7 },
    { 0x65, 0x0300, 0xE8 }, { 0x65, 0x0301, 0xE9 }, { 0x65, 0x0302, 0xEA },
    { 0x65, 0x0308, 0xEB }, { 0x69, 0x0300, 0xEC }, { 0x69, 0x0301, 0xED },
    { 0x69, 0x0302, 0xEE }, { 0x69, 0x0308, 0xEF }, { 0x6E, 0x0303, 0xF1 },
    { 0x6F, 0x0300, 0xF2 }, { 0x6F, 0x0301, 0xF3 }, { 0x6F, 0x0302, 0xF4 },
    { 0x6F, 0x0303, 0xF5 }, { 0x6F, 0x0308, 0xF6 }, { 0x75, 0x0300, 0xF9 },
    { 0x75, 0x0301, 0xFA }, { 0x75, 0x0302, 0xFB }, { 0x75, 0x0308, 0xFC },
    { 0x79, 0x0301, 0xFD }, { 0x79, 0x0308, 0xFF },
};

/* The first n bytes of a host leafname, as RISC OS spells them. */
static void guest_name_of(const char *host, size_t n, char *out, size_t max)
{
    size_t o = 0;
    for (size_t i = 0; i < n && host[i] && o + 1 < max;) {
        size_t len;
        uint32_t u = get_utf8((const unsigned char *)host + i, &len);
        i += len;
        if (i < n && host[i]) {
            size_t len2;
            uint32_t mark = get_utf8((const unsigned char *)host + i, &len2);
            if (mark >= 0x0300 && mark <= 0x036F)
                for (size_t k = 0; k < sizeof compose / sizeof compose[0]; k++)
                    if (compose[k].base == u && compose[k].mark == mark) {
                        u = compose[k].latin1;
                        i += len2;
                        break;
                    }
        }
        if (u >= 0xF000 && u <= 0xF0FF)
            u -= 0xF000;
        unsigned char b = '_';
        if (u == '.')
            b = '/';
        else if (u == ' ' || u == 0xA0)
            b = 0xA0;
        else if (u > ' ' && u < 0x7F)
            b = (unsigned char)u;
        else if (u >= 0xA1 && u <= 0xFF)
            b = (unsigned char)u;
        else
            for (int k = 0; k < 32; k++)
                if (acorn_80[k] == u)
                    b = (unsigned char)(0x80 + k);
        out[o++] = (char)b;
    }
    out[o] = 0;
}

static uint32_t type_for_ext(const char *ext)
{
    if (!ext || !*ext)
        return 0xFFF;
    for (unsigned i = 0; i < ros_typemap_count; i++)
        if (strcasecmp(ros_typemap[i].ext, ext) == 0)
            return ros_typemap[i].type;
    return 0xFFF;
}

/* A host leafname as RISC OS sees it, and its type (0 for a directory). */
static void guest_leaf_of(const char *host_leaf, int is_dir, char *out, size_t max,
                          uint32_t *type)
{
    const char *comma = strrchr(host_leaf, ',');
    if (comma && strlen(comma + 1) == 3 && is_hex(comma[1]) && is_hex(comma[2]) &&
        is_hex(comma[3])) {
        *type = is_dir ? 0 : (uint32_t)strtoul(comma + 1, NULL, 16);
        guest_name_of(host_leaf, (size_t)(comma - host_leaf), out, max);
        return;
    }
    guest_name_of(host_leaf, strlen(host_leaf), out, max);
    const char *dot = strrchr(host_leaf, '.');
    *type = is_dir ? 0 : (dot && dot != host_leaf) ? type_for_ext(dot + 1) : 0xFFF;
}

/* A host leafname without its ",xxx". */
static void host_base_of(const char *leaf, char *out, size_t max)
{
    const char *comma = strrchr(leaf, ',');
    size_t n = strlen(leaf);
    if (comma && strlen(comma + 1) == 3 && is_hex(comma[1]) && is_hex(comma[2]) &&
        is_hex(comma[3]))
        n = (size_t)(comma - leaf);
    snprintf(out, max, "%.*s", (int)n, leaf);
}

/* The host leafname for a base and a type: decorated only when the name
 * does not already say the type. */
static void host_leaf_for(const char *base, uint32_t type, int is_dir, char *out, size_t max)
{
    const char *dot = strrchr(base, '.');
    uint32_t implied = (dot && dot != base) ? type_for_ext(dot + 1) : 0xFFF;
    if (is_dir || type == implied)
        snprintf(out, max, "%s", base);
    else
        snprintf(out, max, "%s,%03x", base, type & 0xFFF);
}

static uint32_t type_of_load(uint32_t load)
{
    return (load >> 20) == 0xFFF ? (load >> 8) & 0xFFF : 0xFFFFFFFFu;
}

static uint64_t date_cs(const struct stat *st)
{
#ifdef __APPLE__
    int64_t secs = (int64_t)st->st_mtimespec.tv_sec + 2208988800LL;
    int64_t ns = st->st_mtimespec.tv_nsec;
#else
    int64_t secs = (int64_t)st->st_mtim.tv_sec + 2208988800LL;
    int64_t ns = st->st_mtim.tv_nsec;
#endif
    if (secs < 0)
        return 0;
    return (uint64_t)secs * 100 + (uint64_t)(ns / 10000000);
}

static void set_date(const char *hp, uint64_t cs)
{
    struct timespec t[2];
    t[0].tv_sec = t[1].tv_sec = (time_t)((int64_t)(cs / 100) - 2208988800LL);
    t[0].tv_nsec = t[1].tv_nsec = (long)(cs % 100) * 10000000L;
    const char *r;
    const struct disc *d = io_disc(hp, &r);
    if (d) {
        int64_t mt[2] = { t[1].tv_sec, t[1].tv_nsec };
        d->io->setinfo(d->ctx, r, mt, -1);
        return;
    }
    utimensat(AT_FDCWD, hp, t, 0);
}

/* ---- attributes ---------------------------------------------------------------- */

static int attr_get(const char *hp, uint32_t *attr)
{
    const char *r;
    if (io_disc(hp, &r))                /* none kept: attrs_for derives them */
        return 0;
    uint8_t b;
#ifdef __APPLE__
    ssize_t n = getxattr(hp, "riscos.attr", &b, 1, 0, 0);
#else
    ssize_t n = getxattr(hp, "user.riscos.attr", &b, 1);
#endif
    if (n == 1) {
        *attr = b;
        return 1;
    }
    return 0;
}

static void attr_set(const char *hp, uint32_t attr)
{
    const char *r;
    const struct disc *d = io_disc(hp, &r);
    if (d) {                            /* locked, or not writable: read-only */
        d->io->setinfo(d->ctx, r, NULL, (attr & ATTR_L) || !(attr & ATTR_W));
        return;
    }
    uint8_t b = (uint8_t)(attr & 0x3F);
#ifdef __APPLE__
    setxattr(hp, "riscos.attr", &b, 1, 0, 0);
#else
    setxattr(hp, "user.riscos.attr", &b, 1, 0);
#endif
}

static uint32_t attrs_for(const struct stat *st, const char *hp)
{
    uint32_t a;
    const char *r;
    if (io_disc(hp, &r))                /* a read-only file is locked */
        return st->st_mode & S_IWUSR ? ATTR_R | ATTR_W : ATTR_R | ATTR_L;
    if (attr_get(hp, &a))
        return a;
    a = 0;
    if (st->st_mode & S_IRUSR)
        a |= ATTR_R;
    if (st->st_mode & S_IWUSR)
        a |= ATTR_W;
    return a;
}

static int is_locked(const char *hp)
{
    uint32_t a;
    const char *r;
    struct stat st;
    if (io_disc(hp, &r))
        return io_stat(hp, &st) == 0 && !(st.st_mode & S_IWUSR);
    return attr_get(hp, &a) && (a & ATTR_L);
}

static int real_path(const char *path, char *out);

static unsigned open_count;              /* files open: the check below is free at 0 */

static int path_is_open(const char *hp, int write_only)
{
    char real[PATH_MAX];
    if (open_count == 0)
        return 0;
    if (!real_path(hp, real))
        return 0;
    for (int i = 0; i < MAX_OPEN; i++)
        if (open_files[i].path[0] && strcmp(open_files[i].path, real) == 0 &&
            (!write_only || open_files[i].write))
            return 1;
    return 0;
}

/* ---- paths ------------------------------------------------------------------- */

/* The host leaf in dir that RISC OS would call want: one stat, then the
 * directory forward-mapped entry by entry.  1 found; 0 and the literal
 * translation, for a create. */
static int resolve_leaf_link(const char *dir, const char *want, char *out, size_t max, int *link)
{
    char naive[1024];
    struct stat st;
    host_name_of(want, naive, sizeof naive);
    snprintf(out, max, "%s/%s", dir, naive);
    if (io_lstat(out, &st) == 0) {
        *link = S_ISLNK(st.st_mode);
        return 1;
    }
    struct hdir *d = io_opendir(dir);
    if (d) {
        const char *e;
        while ((e = io_readdir(d, NULL)) != NULL) {
            char one[PATH_MAX], leaf[256];
            uint32_t type;
            /* The name is read first both ways.  As a file, its ",xxx" is a type.
             * As a directory, it has none.  So only a name that could be the
             * one asked for costs a stat.  Scanning a big directory entry by
             * entry is slow over 9p */
            guest_leaf_of(e, 0, leaf, sizeof leaf, &type);
            int maybe = strcasecmp(leaf, want) == 0;
            if (!maybe) {
                guest_leaf_of(e, 1, leaf, sizeof leaf, &type);
                maybe = strcasecmp(leaf, want) == 0;
            }
            if (!maybe)
                continue;
            snprintf(one, sizeof one, "%s/%s", dir, e);
            if (io_lstat(one, &st) != 0)
                continue;
            int islink = S_ISLNK(st.st_mode);
            if (islink && stat(one, &st) != 0)
                continue;
            guest_leaf_of(e, S_ISDIR(st.st_mode), leaf, sizeof leaf, &type);
            if (strcasecmp(leaf, want) == 0) {
                snprintf(out, max, "%s", one);
                *link = islink;
                io_closedir(d);
                return 1;
            }
        }
        io_closedir(d);
    }
    snprintf(out, max, "%s/%s", dir, naive);
    *link = 0;
    return 0;
}

static int resolve_leaf(const char *dir, const char *want, char *out, size_t max)
{
    int link;
    return resolve_leaf_link(dir, want, out, max, &link);
}

/* Where a path really leads, with every link followed.  This takes one
 * open and one query of the descriptor.  realpath() walks the path a
 * component at a time, and over 9p with no cache each of those walks starts
 * from the root */
static int real_path(const char *path, char *out)
{
    const char *r;
    if (io_disc(path, &r)) {            /* no links: the path is the object's */
        snprintf(out, PATH_MAX, "%s", path);
        return 1;
    }
#if defined(__linux__)
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return 0;
    char proc[64];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, out, PATH_MAX - 1);
    close(fd);
    if (n < 0)
        return realpath(path, out) != NULL;
    out[n] = 0;
    return 1;
#elif defined(__APPLE__)
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return realpath(path, out) != NULL;
    int ok = fcntl(fd, F_GETPATH, out) != -1;
    close(fd);
    return ok || realpath(path, out) != NULL;
#else
    return realpath(path, out) != NULL;
#endif
}

static int within_root(const char *path, const char *root)
{
    char rootreal[PATH_MAX], cur[PATH_MAX], real[PATH_MAX];
    if (!real_path(root, rootreal))
        return 0;
    snprintf(cur, sizeof cur, "%s", path);
    size_t rl = strlen(rootreal);
    for (;;) {
        if (real_path(cur, real))
            return strncmp(real, rootreal, rl) == 0 && (real[rl] == 0 || real[rl] == '/');
        /* The path exists but does not resolve: a link whose target is not
         * there.  Opening it for writing would create the target, wherever
         * it is, so it counts as outside. */
        struct stat ls;
        if (lstat(cur, &ls) == 0)
            return 0;
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur)
            return 0;
        *slash = 0;
    }
}

/* disc + "a.b.c" to a host path; every component but the last must exist.
 * 0, or an error. */
static os_error *host_path(const char *disc, const char *path, char *out, size_t max)
{
    const struct disc *d = find_disc(disc);
    if (!d)
        return ros_error(ERR(0xCC), "Bad file name");
    char cur[PATH_MAX];
    int links = 0;
    /* The fast way first: the whole name translated as it stands, with one
     * lstat.  Or its directory translated so, and only the leaf looked for.
     * A path walked a component at a time costs a walk from the root for
     * each component over 9p, so the cost is quadratic in its depth. */
    {
        char naive[PATH_MAX], parent[PATH_MAX] = "";
        size_t n = (size_t)snprintf(naive, sizeof naive, "%s", d->root);
        const char *last = path;
        int ok = 1;
        for (const char *c = path; *c && ok;) {
            const char *e = strchr(c, '.');
            size_t k = e ? (size_t)(e - c) : strlen(c);
            char comp[256], host[1024];
            if (k == 0 || k >= sizeof comp)
                return ros_error(ERR(0xCC), "Bad file name");
            memcpy(comp, c, k);
            comp[k] = 0;
            host_name_of(comp, host, sizeof host);
            snprintf(parent, sizeof parent, "%s", naive);
            last = c;
            n += (size_t)snprintf(naive + n, sizeof naive - n, "/%s", host);
            ok = n < sizeof naive;
            c = e ? e + 1 : c + k;
        }
        struct stat st;
        if (ok && path[0] && io_lstat(naive, &st) == 0) {
            if (!within_root(naive, d->root))
                return ros_error(ERR(0xCC), "Bad file name");
            snprintf(out, max, "%s", naive);
            return NULL;
        }
        if (ok && path[0] && parent[0] && io_lstat(parent, &st) == 0 && S_ISDIR(st.st_mode)) {
            char leaf[256], found[PATH_MAX];
            snprintf(leaf, sizeof leaf, "%s", last);
            resolve_leaf(parent, leaf, found, sizeof found);
            if (!within_root(found, d->root))
                return ros_error(ERR(0xCC), "Bad file name");
            snprintf(out, max, "%s", found);
            return NULL;
        }
    }
    /* The slow way, for a directory whose case differs */
    snprintf(cur, sizeof cur, "%s", d->root);
    for (const char *c = path; *c;) {
        const char *e = strchr(c, '.');
        size_t n = e ? (size_t)(e - c) : strlen(c);
        char comp[256], next[PATH_MAX];
        if (n == 0 || n >= sizeof comp)
            return ros_error(ERR(0xCC), "Bad file name");
        memcpy(comp, c, n);
        comp[n] = 0;
        int link;
        resolve_leaf_link(cur, comp, next, sizeof next, &link);
        links |= link;
        snprintf(cur, sizeof cur, "%s", next);
        c = e ? e + 1 : c + n;
    }
    /* A RISC OS name cannot become "." or ".." (host_name_of escapes
     * them), so only a symlink on the way could lead outside the disc */
    if (links && !within_root(cur, d->root))
        return ros_error(ERR(0xCC), "Bad file name");
    snprintf(out, max, "%s", cur);
    return NULL;
}

static void info_of(const char *hp, const struct stat *st, struct fs_info *info)
{
    char leaf[256];
    const char *slash = strrchr(hp, '/');
    uint32_t type;
    guest_leaf_of(slash ? slash + 1 : hp, S_ISDIR(st->st_mode), leaf, sizeof leaf, &type);
    uint64_t cs = date_cs(st);
    info->type = S_ISDIR(st->st_mode) ? OBJ_DIR : OBJ_FILE;
    info->load = 0xFFF00000u | type << 8 | (uint32_t)((cs >> 32) & 0xFF);
    info->exec = (uint32_t)cs;
    if (S_ISDIR(st->st_mode))           /* directories carry neither, as the team's HostFS
                                           (dde/c/hostfs, FSEntry_File 5 and its catalogue):
                                           5.30's Full info shows 00000000 00000000 */
        info->load = info->exec = 0;
    info->length = S_ISDIR(st->st_mode) ? 0 : st->st_size > 0xFFFFFFFFLL ? 0xFFFFFFFFu
                                                                          : (uint32_t)st->st_size;
    info->attr = attrs_for(st, hp);
}

/* ---- the entry points ------------------------------------------------------------ */

static const char *boot_disc(void)
{
    return ndiscs ? discs[0].name : "";
}

static int disc_exists(const char *disc)
{
    return find_disc(disc) != NULL;
}

static os_error *h_stat(const char *disc, const char *path, struct fs_info *info)
{
    char hp[PATH_MAX];
    struct stat st;
    os_error *e = host_path(disc, path, hp, sizeof hp);
    if (e)
        return e;
    if (io_stat(hp, &st) != 0) {
        info->type = OBJ_NOTHING;
        return NULL;
    }
    info_of(hp, &st, info);
    return NULL;
}

/* Whether a disc is a 9p share (QEMU's, run-qemu.sh, run-x86_64.sh): the
 * box mounts it with write-back caching (boot/main.c), so what is written
 * reaches the host at a close only when it is flushed.  (virtio-fs and a
 * hosted disc write through: no flush, which on the Mac is a disc sync.) */
static int nine_p_disc(struct disc *d)
{
    if (d->nine_p < 0) {
        d->nine_p = 0;
#ifdef __linux__
        if (d->io)
            return 0;
        struct statfs sf;
        if (statfs(d->root, &sf) == 0 && (unsigned long)sf.f_type == 0x01021997UL)   /* V9FS_MAGIC */
            d->nine_p = 1;
#endif
    }
    return d->nine_p;
}

static os_error *h_open(const char *disc, const char *path, int write, uint32_t *handle)
{
    char hp[PATH_MAX];
    os_error *e = host_path(disc, path, hp, sizeof hp);
    if (e)
        return e;
    if (write && is_locked(hp))
        return ros_error(ERR(0xC3), E_LOCKED_TEXT);
    if (path_is_open(hp, !write))
        return ros_error(ERR(0xC2), "File open");
    int slot = -1;
    for (int i = 1; i < MAX_OPEN && slot < 0; i++)
        if (!open_files[i].path[0])
            slot = i;
    if (slot < 0)
        return ros_error(0xC0, "Too many open files");
    if (hf_open(hp, write ? O_RDWR : O_RDONLY, &open_files[slot].f) != 0)
        return err_errno(errno);
    struct stat st;                     /* the extent, until h_extent asks again */
    open_files[slot].extent = hf_fstat(&open_files[slot].f, &st) == 0 ? (uint32_t)st.st_size : 0;
    open_count++;
    open_files[slot].write = write;
    open_files[slot].flush = 0;
    struct disc *dp = (struct disc *)find_disc(disc);
    open_files[slot].nine_p = dp ? nine_p_disc(dp) : 0;
    if (!real_path(hp, open_files[slot].path))
        snprintf(open_files[slot].path, sizeof open_files[slot].path, "%s", hp);
    *handle = (uint32_t)slot;
    return NULL;
}

static void changed(uint32_t h);
static void changed_at(const char *p);

/* The file a handle is open on, still there: a host that deletes or
 * replaces it underneath an open handle (Finder, a script) leaves the
 * descriptor on the old file, and reading or writing that would quietly
 * use data no longer anyone's.  "Not found" instead, as the suite asks. */
static os_error *still_there(uint32_t h)
{
    struct stat fst, pst;
    if (open_files[h].f.io)             /* the server keeps a file open whole */
        return NULL;
    if (fstat(open_files[h].f.fd, &fst) != 0 || stat(open_files[h].path, &pst) != 0 ||
        fst.st_dev != pst.st_dev || fst.st_ino != pst.st_ino)
        return err_not_found();
    return NULL;
}

/* pread and pwrite, through a buffer of the host's own where Linux cannot
 * take the memory directly.  Screen memory in the box is the DRM buffer
 * mapped into the arena, which 9p's zero-copy cannot pin (EFAULT).  An
 * example is OS_File 10 of the screen. */
#define BOUNCE 65536u

static ssize_t pread_any(int fd, void *buf, size_t n, off_t pos)
{
    ssize_t r = pread(fd, buf, n, pos);
    if (r >= 0 || errno != EFAULT)
        return r;
    char *b = malloc(BOUNCE);
    if (!b)
        return -1;                          /* EFAULT still */
    size_t done = 0;
    while (done < n) {
        size_t k = n - done < BOUNCE ? n - done : BOUNCE;
        r = pread(fd, b, k, pos + (off_t)done);
        if (r <= 0)
            break;
        memcpy((char *)buf + done, b, (size_t)r);
        done += (size_t)r;
    }
    free(b);
    return r < 0 && !done ? -1 : (ssize_t)done;
}

static ssize_t pwrite_any(int fd, const void *buf, size_t n, off_t pos)
{
    ssize_t r = pwrite(fd, buf, n, pos);
    if (r >= 0 || errno != EFAULT)
        return r;
    char *b = malloc(BOUNCE);
    if (!b)
        return -1;
    size_t done = 0;
    while (done < n) {
        size_t k = n - done < BOUNCE ? n - done : BOUNCE;
        memcpy(b, (const char *)buf + done, k);
        r = pwrite(fd, b, k, pos + (off_t)done);
        if (r <= 0)
            break;
        done += (size_t)r;
    }
    free(b);
    return r < 0 && !done ? -1 : (ssize_t)done;
}

static os_error *h_read(uint32_t h, uint32_t pos, void *buf, uint32_t n, uint32_t *got)
{
    os_error *e = still_there(h);
    if (e)
        return e;
    const struct hf *f = &open_files[h].f;
    ssize_t r = f->io ? f->io->pread(f->ctx, f->fd, buf, n, pos) : pread_any(f->fd, buf, n, (off_t)pos);
    if (f->io && r < 0)
        errno = (int)-r;
    if (r < 0)
        return err_errno(errno);
    *got = (uint32_t)r;
    return NULL;
}

static os_error *h_write(uint32_t h, uint32_t pos, const void *buf, uint32_t n)
{
    changed(h);
    os_error *e = still_there(h);
    if (e)
        return e;
    const struct hf *f = &open_files[h].f;
    ssize_t r = f->io ? f->io->pwrite(f->ctx, f->fd, buf, n, pos) : pwrite_any(f->fd, buf, n, (off_t)pos);
    if (f->io && r < 0)
        errno = (int)-r;
    if (r < 0 || (uint32_t)r != n)
        return err_errno(r < 0 ? errno : ENOSPC);
    open_files[h].flush = open_files[h].nine_p;
    return NULL;
}

static os_error *h_set_extent(uint32_t h, uint32_t extent)
{
    changed(h);
    return hf_ftruncate(&open_files[h].f, extent) ? err_errno(errno) : NULL;
}

/* A file deleted under the handle may read as empty (over 9p it does):
 * the extent it last had, so that what comes next is "Not found" from the
 * read or write, not "Outside file" from moving the pointer */
static uint32_t h_extent(uint32_t h)
{
    struct stat st;
    if (!still_there(h) && hf_fstat(&open_files[h].f, &st) == 0)
        open_files[h].extent = (uint32_t)st.st_size;
    return open_files[h].extent;
}

static os_error *h_close(uint32_t h)
{
    changed(h);
    /* written over a 9p share: on the host at the close, as a file a
     * program has saved is there for the Mac's own programs at once */
    if (open_files[h].flush)
        fsync(open_files[h].f.fd);
    hf_close(&open_files[h].f);
    open_files[h].path[0] = 0;
    open_count--;
    return NULL;
}

struct listing {
    char leaf[256];
    char host[256];                     /* the host's leafname */
    struct fs_info info;
};

static int fold_cmp(const void *a, const void *b)
{
    const unsigned char *p = (const unsigned char *)((const struct listing *)a)->leaf;
    const unsigned char *q = (const unsigned char *)((const struct listing *)b)->leaf;
    for (; *p && *q; p++, q++) {
        int cp = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
        int cq = *q >= 'a' && *q <= 'z' ? *q - 32 : *q;
        if (cp != cq)
            return cp - cq;
    }
    return (int)*p - (int)*q;
}

/* The last listing, kept while an enumeration walks it: entry k is asked
 * for after entry k-1, and reading and sorting the whole directory for
 * each would make listing a large one quadratic.  It is read again when an
 * enumeration starts over (index 0), when HostFS has changed anything, or
 * when the directory itself has changed on the host. */
static struct {
    char hp[PATH_MAX];
    dev_t dev;
    ino_t ino;
    struct timespec mtime;
    unsigned generation;
    char real[PATH_MAX];                /* hp, all links resolved */
    struct listing *all;
    size_t n;
} cache;
static unsigned generation;

/* Something in host directory dir changed: its cached listing is stale */
static void changed_in(const char *dir)
{
    if (!cache.all)
        return;
    if (!strcmp(dir, cache.hp) || !strcmp(dir, cache.real))
        generation++;
}

/* The object at host path p changed: its directory's listing is stale */
static void changed_at(const char *p)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", p);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    changed_in(dir);
}

static void changed(uint32_t h)
{
    changed_at(open_files[h].path);
}

static struct timespec mtime_of(const struct stat *st)
{
#ifdef __APPLE__
    return st->st_mtimespec;
#else
    return st->st_mtim;
#endif
}

static int h_readdir(const char *disc, const char *path, uint32_t index, struct fs_entry *out,
                     os_error **err)
{
    char hp[PATH_MAX];
    *err = host_path(disc, path, hp, sizeof hp);
    if (*err)
        return 0;
    struct stat dst;
    if (io_stat(hp, &dst) != 0) {
        *err = err_errno(errno);
        return 0;
    }
    struct timespec mt = mtime_of(&dst);
    if (index > 0 && cache.all && cache.generation == generation && !strcmp(cache.hp, hp) &&
        cache.dev == dst.st_dev && cache.ino == dst.st_ino && cache.mtime.tv_sec == mt.tv_sec &&
        cache.mtime.tv_nsec == mt.tv_nsec) {
        if (index >= cache.n)
            return 0;
        snprintf(out->name, sizeof out->name, "%s", cache.all[index].leaf);
        out->info = cache.all[index].info;
        return 1;
    }
    struct hdir *d = io_opendir(hp);
    if (!d) {
        *err = err_errno(errno);
        return 0;
    }
    struct listing *all = NULL;
    size_t n = 0, cap = 0;
    const char *e;
    struct stat st;
    while ((e = io_readdir(d, &st)) != NULL) {
        char one[PATH_MAX];
        snprintf(one, sizeof one, "%s/%s", hp, e);
        if (S_ISLNK(st.st_mode) &&
            (stat(one, &st) != 0 || !within_root(one, find_disc(disc)->root)))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            struct listing *m = realloc(all, cap * sizeof *m);
            if (!m)
                break;
            all = m;
        }
        uint32_t type;
        guest_leaf_of(e, S_ISDIR(st.st_mode), all[n].leaf, sizeof all[n].leaf, &type);
        snprintf(all[n].host, sizeof all[n].host, "%s", e);
        info_of(one, &st, &all[n].info);
        n++;
    }
    io_closedir(d);
    qsort(all, n, sizeof *all, fold_cmp);
    /* Host files that are one RISC OS name ("notes" and "notes,ffb") are
     * one entry: the one a lookup of that name opens */
    size_t w = 0;
    for (size_t r = 0; r < n;) {
        size_t run = r + 1;
        while (run < n && !strcmp(all[run].leaf, all[r].leaf))
            run++;
        size_t keep = r;
        if (run - r > 1) {
            char found[PATH_MAX];
            resolve_leaf(hp, all[r].leaf, found, sizeof found);
            const char *leaf = strrchr(found, '/');
            for (size_t k = r; k < run; k++)
                if (leaf && !strcmp(all[k].host, leaf + 1))
                    keep = k;
        }
        all[w++] = all[keep];
        r = run;
    }
    n = w;
    int found = index < n;
    if (found) {
        snprintf(out->name, sizeof out->name, "%s", all[index].leaf);
        out->info = all[index].info;
    }
    free(cache.all);
    snprintf(cache.hp, sizeof cache.hp, "%s", hp);
    if (!real_path(hp, cache.real))
        cache.real[0] = 0;
    cache.dev = dst.st_dev, cache.ino = dst.st_ino, cache.mtime = mt;
    cache.generation = generation;
    cache.all = all, cache.n = n;
    return found;
}

static os_error *h_create(const char *disc, const char *path, uint32_t load, uint32_t exec,
                          uint32_t length)
{
    char hp[PATH_MAX];
    os_error *e = host_path(disc, path, hp, sizeof hp);
    if (e)
        return e;
    changed_at(hp);
    struct stat st;
    if (io_stat(hp, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return ros_error(ERR(0xA8), "Object is a directory");
        if (is_locked(hp))
            return ros_error(ERR(0xC3), E_LOCKED_TEXT);
        if (path_is_open(hp, 0))
            return ros_error(ERR(0xC2), "File open");
    }
    uint32_t type = type_of_load(load);
    if (type == 0xFFFFFFFFu)
        type = 0xFFD;                   /* an address pair cannot be kept: Data */
    char dir[PATH_MAX], leaf[512], base[512], want[600], dest[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", hp);
    char *slash = strrchr(dir, '/');
    *slash = 0;
    snprintf(leaf, sizeof leaf, "%s", slash + 1);
    host_base_of(leaf, base, sizeof base);
    host_leaf_for(base, type, 0, want, sizeof want);
    snprintf(dest, sizeof dest, "%s/%s", dir, want);
    if (strcmp(dest, hp) != 0 && io_stat(hp, &st) == 0)
        io_unlink(hp);                  /* one RISC OS name, one host file */
    struct hf f;
    if (hf_open(dest, O_WRONLY | O_CREAT | O_TRUNC, &f) != 0)
        return err_errno(errno);
    if (length && hf_ftruncate(&f, length) != 0) {
        int err = errno;
        hf_close(&f);
        return err_errno(err);
    }
    hf_close(&f);
    if (type_of_load(load) != 0xFFFFFFFFu) {
        uint64_t cs = ((uint64_t)(load & 0xFF) << 32) | exec;
        if (cs)
            set_date(dest, cs);
    }
    return NULL;
}

static os_error *h_mkdir(const char *disc, const char *path)
{
    char hp[PATH_MAX];
    os_error *e = host_path(disc, path, hp, sizeof hp);
    if (e)
        return e;
    changed_at(hp);
    if (io_mkdir(hp) == 0)
        return NULL;
    struct stat st;
    if (errno == EEXIST && io_stat(hp, &st) == 0 && S_ISDIR(st.st_mode))
        return NULL;                    /* already there: as the PRM allows */
    if (errno == EEXIST)
        return ros_error(ERR(0xC4), "Already exists");
    return err_errno(errno);
}

static os_error *h_remove(const char *disc, const char *path)
{
    char hp[PATH_MAX];
    struct stat st;
    os_error *e = host_path(disc, path, hp, sizeof hp);
    if (e)
        return e;
    changed_at(hp);
    if (io_stat(hp, &st) != 0)
        return err_not_found();
    if (is_locked(hp))
        return ros_error(ERR(0xC3), E_LOCKED_TEXT);
    if (path_is_open(hp, 0))
        return ros_error(ERR(0xC2), "File open");
    if (S_ISDIR(st.st_mode))
        return io_rmdir(hp) == 0 ? NULL
               : (errno == ENOTEMPTY || errno == EEXIST)
                   ? ros_error(ERR(0xB4), "Directory not empty")
                   : err_errno(errno);
    return io_unlink(hp) == 0 ? NULL : err_errno(errno);
}

static os_error *h_setinfo(const char *disc, const char *path, int reason, uint32_t load,
                           uint32_t exec, uint32_t attr)
{
    char hp[PATH_MAX];
    struct stat st;
    os_error *e = host_path(disc, path, hp, sizeof hp);
    if (e)
        return e;
    changed_at(hp);
    if (io_stat(hp, &st) != 0)
        return err_not_found();
    if (S_ISDIR(st.st_mode))
        return NULL;                    /* a directory holds none of it */
    if (reason == 1 || reason == 4) {
        mode_t m = st.st_mode & ~(mode_t)(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        m |= S_IRUSR | S_IWUSR;
        if (attr & ATTR_PR)
            m |= S_IROTH | S_IRGRP;
        if (attr & ATTR_PW)
            m |= S_IWOTH | S_IWGRP;
        const char *r;
        if (!io_disc(hp, &r) && chmod(hp, m) != 0)
            return err_errno(errno);
        attr_set(hp, attr);
    }
    if (reason >= 1 && reason <= 3) {
        if (reason != 1) {              /* the other word as the file has it */
            struct fs_info cur;
            info_of(hp, &st, &cur);
            if (reason == 2)
                exec = cur.exec;
            else
                load = cur.load;
        }
        uint32_t type = type_of_load(load);
        if (reason != 3 && type != 0xFFFFFFFFu) {
            char dir[PATH_MAX], leaf[512], base[512], want[600];
            snprintf(dir, sizeof dir, "%s", hp);
            char *slash = strrchr(dir, '/');
            *slash = 0;
            snprintf(leaf, sizeof leaf, "%s", slash + 1);
            host_base_of(leaf, base, sizeof base);
            host_leaf_for(base, type, 0, want, sizeof want);
            if (strcmp(want, leaf) != 0) {
                char dest[PATH_MAX], was[PATH_MAX];
                snprintf(dest, sizeof dest, "%s/%s", dir, want);
                if (!real_path(hp, was))
                    snprintf(was, sizeof was, "%s", hp);
                if (io_rename(hp, dest) != 0)
                    return err_errno(errno);
                snprintf(hp, sizeof hp, "%s", dest);
                /* a file open keeps its handle: FileCore sets an open
                 * file's type (Font_MakeBitmap does, before writing) */
                for (int i = 1; i < MAX_OPEN; i++)
                    if (open_files[i].path[0] && !strcmp(open_files[i].path, was) &&
                        !real_path(dest, open_files[i].path))
                        snprintf(open_files[i].path, sizeof open_files[i].path, "%s", dest);
            }
        }
        if (type != 0xFFFFFFFFu) {
            uint64_t cs = ((uint64_t)(load & 0xFF) << 32) | exec;
            if (cs)
                set_date(hp, cs);
        }
    }
    return NULL;
}

static os_error *h_rename(const char *disc, const char *from, const char *to)
{
    char hp1[PATH_MAX], hp2[PATH_MAX];
    struct stat st;
    os_error *e = host_path(disc, from, hp1, sizeof hp1);
    if (!e)
        e = host_path(disc, to, hp2, sizeof hp2);
    if (e)
        return e;
    if (io_stat(hp1, &st) != 0)
        return err_not_found();
    if (is_locked(hp1))
        return ros_error(ERR(0xC3), E_LOCKED_TEXT);
    if (path_is_open(hp1, 0))
        return ros_error(ERR(0xC2), "File open");
    /* The type rides the name: carry it across */
    char leaf[256], dir[PATH_MAX], hleaf[512], base[512], want[600];
    uint32_t type;
    const char *s1 = strrchr(hp1, '/');
    guest_leaf_of(s1 + 1, S_ISDIR(st.st_mode), leaf, sizeof leaf, &type);
    snprintf(dir, sizeof dir, "%s", hp2);
    char *slash = strrchr(dir, '/');
    *slash = 0;
    /* The new leaf as it was asked for, not as a lookup that ignores case
     * found it, which may be the object itself ("lower" to "LOWER") */
    const char *to_leaf = strrchr(to, '.');
    host_name_of(to_leaf ? to_leaf + 1 : to, hleaf, sizeof hleaf);
    host_base_of(hleaf, base, sizeof base);
    host_leaf_for(base, type, S_ISDIR(st.st_mode), want, sizeof want);
    snprintf(hp2, sizeof hp2, "%s/%s", dir, want);
    changed_at(hp1);
    changed_at(hp2);
#if defined(__aarch64__) && !defined(ROS_ARENA_HOSTED)
    /* The Apple Silicon box's share is rosgd-vz's virtio-fs (VZ's).  It takes
     * a rename that only changes case, such as "lower" to "LOWER" on the
     * Mac's case-insensitive APFS, and leaves the old name.  So such a
     * rename goes by way of a name of its own */
    const char *l1 = strrchr(hp1, '/'), *l2 = strrchr(hp2, '/');
    if (l1 - hp1 == l2 - hp2 && !strncmp(hp1, hp2, (size_t)(l1 - hp1)) && strcmp(l1, l2) &&
        !strcasecmp(l1, l2)) {
        char tmp[PATH_MAX + 32];
        snprintf(tmp, sizeof tmp, "%.*s/.rosgd-rename-%d", (int)(l1 - hp1), hp1, (int)getpid());
        if (io_rename(hp1, tmp) != 0)
            return err_errno(errno);
        if (io_rename(tmp, hp2) != 0) {
            int err = errno;
            io_rename(tmp, hp1);
            return err_errno(err);
        }
        return NULL;
    }
#endif
    return io_rename(hp1, hp2) == 0 ? NULL : err_errno(errno);
}

static os_error *h_free(const char *disc, uint64_t *free_bytes, uint64_t *biggest, uint64_t *size)
{
    const struct disc *d = find_disc(disc);
    struct statvfs v;
    if (d && d->io) {
        int r = d->io->free_space(d->ctx, free_bytes, size);
        if (r < 0)
            return err_errno(-r);
        *biggest = *free_bytes;
        return NULL;
    }
    if (!d || statvfs(d->root, &v) != 0)
        return ros_error(ERR(0xCC), "Bad file name");
    *free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
    *biggest = *free_bytes;
    *size = (uint64_t)v.f_blocks * v.f_frsize;
    return NULL;
}

/* ---- the Free module's entry ------------------------------------------------- */

/* The Free module's windows read a filing system's space through an entry
 * that the filing system gives Free with Free_Register
 * (Desktop/Free/Doc/FreeSpace).  The windows are *ShowFree and the Free
 * entry on HostFSFiler's menu, which runs it.
 *
 * Free has entries of its own for ADFS, RAMFS, SCSIFS, NetFS, NFS and
 * PCCardFS.  Every other filing system registers as it starts.  SDFS does
 * (c/module), CDFS does (s/Main), and so does the team's HostFS (dde/s/head,
 * FreeEntry).  That is why *ShowFree -FS HostFS finds HostFS on RISC OS
 * 5.30.  With no entry Free says "Unknown filing system".
 *
 * Free calls the entry with R0 the reason, R1 the filing system, R2 -> a
 * buffer, R3 -> the device, and R12 as registered.  It calls from its task
 * or from *ShowFree.  The return address is on the caller's stack, not in
 * R14 (StartLoop's CallEntry: Push "PC", then LDR PC).  So the entry
 * returns by pulling it, as Free's own entries and SDFS's veneer do.  Every
 * register but a result is kept.  An error is V set with R0 -> it.
 *
 * The device is a disc's name, as HostFSFiler's *ShowFree gives it.  It is
 * found without case, and a ":" before it is allowed.  With no device the
 * boot disc is used.  The boot disc is also used for "HostFS" when no disc
 * has that name.  The team's HostFS has the one disc and ignores the
 * device, and its filer runs *ShowFree -FS HostFS HostFS
 * (riscos-pi4/hostfs/filer/c/hostfsfiler).  A 5.30 script may do the same.
 *
 *   0  nothing
 *   1  [R2] the disc's name as it was mounted, R0 its length with the
 *      terminator.  Free's window is titled with it, and gives it back as
 *      the device from then on.
 *   2  [R2] the disc's size, free space and used space, 32 bits each, as
 *      OS_FSControl 49 gives them (the team's HostFS reads them so)
 *   3  Z set if the file R2 names may be on the device.  One on another
 *      disc (":Other.$...") is not.  One with no disc may be, as RAMFS's
 *      and the team's HostFS's entries answer every name.
 *   4  [R2] size, free and used, 64 bits each, as OS_FSControl 55 gives
 *      them; R0 = 0, which tells Free the reason is known
 */
#define XFREE_REGISTER   0x644C0u
#define XFREE_DEREGISTER 0x644C1u

/* A string in the arena, to a control character or stop: 0 if it is not
 * all readable */
static int arena_string(uint32_t addr, char stop, char *out, size_t max)
{
    size_t n = 0;
    for (; n + 1 < max; n++) {
        if (!ros_arena_readable(addr + (uint32_t)n, addr + (uint32_t)n + 1))
            return 0;
        char c = (char)ros_ld8(addr + (uint32_t)n);
        if ((unsigned char)c < ' ' || c == stop)
            break;
        out[n] = c;
    }
    out[n] = 0;
    return 1;
}

/* The disc the device at addr names; FileSwitch's "Disc not found" when
 * there is none of that name */
static os_error *free_device(uint32_t addr, const struct disc **d)
{
    char name[64] = "";
    if (addr && !arena_string(addr, 0, name, sizeof name))
        return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
    const char *p = name[0] == ':' ? name + 1 : name;
    if (!*p)
        p = boot_disc();
    *d = find_disc(p);
    if (!*d && strcasecmp(p, "HostFS") == 0)
        *d = find_disc(boot_disc());
    return *d ? NULL : ros_error(ERR(0xD3), "Disc '%s' not found", p);
}

static os_error *free_space(uint32_t device, uint32_t buf, int wide)
{
    const struct disc *d;
    uint64_t fr, big, size;
    os_error *e = free_device(device, &d);
    if (!e)
        e = h_free(d->name, &fr, &big, &size);
    if (e)
        return e;
    if (!ros_arena_valid(buf, buf + (wide ? 24u : 12u)))
        return ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
    if (wide) {
        ros_st64(buf, size);
        ros_st64(buf + 8, fr);
        ros_st64(buf + 16, size - fr);
    } else {                            /* OS_FSControl 49's R2 and R0 */
        uint32_t size32 = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
        uint32_t fr32 = fr > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)fr;
        ros_st32(buf, size32);
        ros_st32(buf + 4, fr32);
        ros_st32(buf + 8, size32 - fr32);
    }
    return NULL;
}

static void free_entry(struct ros_cpu *s)
{
    os_error *e = NULL;
    const struct disc *d;
    switch (s->r[0]) {
    case 1:
        e = free_device(s->r[3], &d);
        if (!e) {
            uint32_t n = (uint32_t)strlen(d->name) + 1;
            if (!ros_arena_valid(s->r[2], s->r[2] + n)) {
                e = ros_error(ROS_ERR_BAD_ADDRESS, "Bad address");
                break;
            }
            memcpy(ros_ptr(s->r[2]), d->name, n);
            s->r[0] = n;
        }
        break;
    case 2:
        e = free_space(s->r[3], s->r[2], 0);
        break;
    case 3: {
        char file[64];
        s->z = 1;
        if (arena_string(s->r[2], '.', file, sizeof file) && file[0] == ':' &&
            free_device(s->r[3], &d) == NULL)
            s->z = strcasecmp(file + 1, d->name) == 0;
        break;
    }
    case 4:
        e = free_space(s->r[3], s->r[2], 1);
        if (!e)
            s->r[0] = 0;
        break;
    default:
        break;
    }
    if (e)
        s->r[0] = ros_addr(e);
    s->v = e != NULL;
    s->r[15] = ros_ld32(s->r[13]);      /* Pull "PC" */
    s->r[13] += 4;
}

void ros_hostfs_free_register(int deregister)
{
    static uint32_t entry;              /* a code address, made once for the image */
    if (!entry)
        entry = ros_native_entry(free_entry, "HostFS:FreeEntry");
    if (!ros_module_for_swi(XFREE_REGISTER & ~0x20000u))
        return;                         /* no Free: no reason to refuse */
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = FS_NUMBER, c.r[1] = entry, c.r[2] = 0;
    ros_swi(&c, XFREE_DEREGISTER);      /* never twice in the list */
    if (!deregister) {
        ros_cpu_enter(&c);
        c.r[0] = FS_NUMBER, c.r[1] = entry, c.r[2] = 0;
        ros_swi(&c, XFREE_REGISTER);
    }
}

const struct fs ros_hostfs = {
    .name = "HostFS",
    .number = FS_NUMBER,
    .has_discs = 1,
    /* The team's HostFS's word (riscos-pi4/hostfs/dde/s/head, FSInfoWord):
     * 255 files, fsfilereadinfonolen, multifsextensions, dontuseload,
     * dontusesave */
    .info = 255u << 8 | 1u << 26 | 1u << 23 | 1u << 20 | 1u << 19,
    .boot_disc = boot_disc,
    .disc_exists = disc_exists,
    .stat = h_stat,
    .open = h_open,
    .read = h_read,
    .write = h_write,
    .set_extent = h_set_extent,
    .extent = h_extent,
    .close = h_close,
    .readdir = h_readdir,
    .create = h_create,
    .mkdir = h_mkdir,
    .remove = h_remove,
    .setinfo = h_setinfo,
    .rename = h_rename,
    .free_space = h_free,
};

/* The Linux file or directory a RISC OS name is, for a module that hands
 * paths to a Linux program (SMBServer's shares): a HostFS name, or a
 * LanMan one, whose discs are HostFS's */
os_error *ros_hostfs_linux_path(const char *name, char *out, size_t max)
{
    struct fsw_loc l;
    os_error *e = fsw_resolve(name, R_READ, &l, 0);
    if (e)
        return e;
    const struct disc *d = find_disc(l.disc);
    if (fsw_fs_of(&l)->readdir != ros_hostfs.readdir || (d && d->io))
        return ros_error(ERR(0xCC), "Bad file name");   /* not a Linux file */
    return host_path(l.disc, l.path, out, max);
}
