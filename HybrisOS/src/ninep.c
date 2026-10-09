/* ninep.c. This file provides the share: a folder on the Mac, reached by
 * 9P2000.L over virtio-9p, which appears as the box's /host.
 *
 * Under Linux the box mounts the share at /host, and HostFS reaches it with
 * ordinary file calls (modules/fileswitch/hostfs.c). Here the HAL handles
 * those calls itself for every path under /host. Each call becomes one or
 * more 9P requests. The HAL sends them one at a time and waits for each
 * reply (the device replies in a few hundred microseconds). The HAL caches
 * nothing, so the box sees changes made on the Mac at once, and anything the
 * box writes is on the Mac's disc when the call returns.
 *
 * The HAL walks each path from the share's root every time. 9P does not
 * follow symbolic links as it walks. This file follows a link that is the
 * last part of a path (up to eight levels deep), but does not follow a link
 * in the middle of a path.
 *
 * QEMU's 9P server uses Linux's numbers for errors and open flags, on a Mac
 * as on Linux. AArch64's own open flags differ from those in four bits, and
 * this file translates them. */
#include "hal.h"

#define EPERM   1
#define ENOENT  2
#define EIO     5
#define EBADF   9
#define ENOMEM  12
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EINVAL  22
#define EMFILE  24
#define ERANGE  34
#define ENAMETOOLONG 36

#define MSIZE   (64 * 1024)
#define NOFID   0xFFFFFFFFu
#define ROOT    0u

enum {
    Rlerror = 7, Tstatfs = 8, Tlopen = 12, Tlcreate = 14, Treadlink = 22, Tgetattr = 24,
    Tsetattr = 26, Txattrwalk = 30, Txattrcreate = 32, Treaddir = 40, Tfsync = 50,
    Tmkdir = 72, Trenameat = 74, Tunlinkat = 76, Tversion = 100, Tattach = 104,
    Twalk = 110, Tread = 116, Twrite = 118, Tclunk = 120,
};

static uint64_t base;                           /* the transport's base; 0 if no share */
static struct vq vq;
static uint8_t *tb, *rb;                        /* request buffer and reply buffer */
static uint64_t tb_pa, rb_pa;
static uint32_t tlen, rpos, rlen;

/* ---- Messages ----------------------------------------------------------- */

static void put8(uint8_t v) { tb[tlen++] = v; }
static void put16(uint16_t v) { put8((uint8_t)v), put8((uint8_t)(v >> 8)); }
static void put32(uint32_t v) { put16((uint16_t)v), put16((uint16_t)(v >> 16)); }
static void put64(uint64_t v) { put32((uint32_t)v), put32((uint32_t)(v >> 32)); }
static void putstr(const char *s, size_t n)
{
    put16((uint16_t)n);
    memcpy(tb + tlen, s, n);
    tlen += (uint32_t)n;
}
static void puts0(const char *s) { putstr(s, strlen(s)); }

static uint8_t get8(void) { return rpos < rlen ? rb[rpos++] : 0; }
static uint16_t get16(void) { uint16_t v = get8(); return (uint16_t)(v | get8() << 8); }
static uint32_t get32(void) { uint32_t v = get16(); return v | (uint32_t)get16() << 16; }
static uint64_t get64(void) { uint64_t v = get32(); return v | (uint64_t)get32() << 32; }

static void begin(uint8_t type)
{
    tlen = 4;                                   /* rpc fills in the size */
    put8(type);
    put16(type == Tversion ? 0xFFFF : 1);       /* tag; one request at a time */
}

/* Send the request and wait for the reply. Returns 0, or -errno (the error
 * that Rlerror gives). The caller then reads the reply's body with the get*
 * functions, starting after its tag. */
static long rpc(void)
{
    tb[0] = (uint8_t)tlen, tb[1] = (uint8_t)(tlen >> 8), tb[2] = (uint8_t)(tlen >> 16), tb[3] = 0;
    vq.desc[0] = (struct vdesc){ tb_pa, tlen, VDESC_NEXT, 1 };
    vq.desc[1] = (struct vdesc){ rb_pa, MSIZE, VDESC_WRITE, 0 };
    uint16_t want = (uint16_t)(vq.used_seen + 1);
    virtio_offer(&vq, 0);
    virtio_notify(base, 0);
    while (vq.used[1] != want)
        __asm__ volatile("yield" ::: "memory");
    vq.used_seen = want;
    __asm__ volatile("dmb ish" ::: "memory");
    mmio_w32(base + 0x064, mmio_r32(base + 0x060));     /* acknowledge the interrupt */
    rlen = (uint32_t)rb[0] | (uint32_t)rb[1] << 8 | (uint32_t)rb[2] << 16;
    counts.share_calls++, counts.share_bytes += tlen + rlen;
    if (rlen > MSIZE || rlen < 7)
        return -EIO;
    rpos = 7;
    if (rb[4] == Rlerror)
        return -(long)get32();
    return rb[4] == tb[4] + 1 ? 0 : -EIO;
}

/* ---- Fids (9P's file identifiers) ---------------------------------------- */

#define FIDS 4096
static uint8_t fid_used[FIDS];

static uint32_t fid_new(void)
{
    for (uint32_t i = 1; i < FIDS; i++)
        if (!fid_used[i]) {
            fid_used[i] = 1;
            return i;
        }
    return NOFID;
}

static void clunk(uint32_t fid)
{
    if (fid == NOFID || fid == ROOT)
        return;
    begin(Tclunk);
    put32(fid);
    rpc();
    fid_used[fid] = 0;
}

/* ---- Paths --------------------------------------------------------------- */

/* Return the part of path within the share ("" for its root), or 0 if path
 * is not under /host */
static const char *in_share(const char *path)
{
    if (strncmp(path, "/host", 5) || (path[5] && path[5] != '/'))
        return 0;
    return path + 5;
}

int share_has(const char *path)
{
    return base && in_share(path);
}

#define MAXNAMES 64
struct names {
    int n;
    const char *name[MAXNAMES];
    uint16_t len[MAXNAMES];
};

/* Split rel into its components. Drop each "." and let each ".." remove the
 * component before it. */
static int split(const char *rel, struct names *nm)
{
    nm->n = 0;
    for (const char *p = rel; *p;) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t l = (size_t)(p - s);
        if (!l || (l == 1 && s[0] == '.'))
            continue;
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (nm->n)
                nm->n--;
            continue;
        }
        if (nm->n == MAXNAMES || l > 255)
            return -ENAMETOOLONG;
        nm->name[nm->n] = s, nm->len[nm->n] = (uint16_t)l, nm->n++;
    }
    return 0;
}

/* Walk to the first k names of nm and return a new fid for the result, or
 * -errno. *qtype receives the qid type of the last name (0x80 for a
 * directory, 0x02 for a link). */
static long walk_names(const struct names *nm, int k, uint8_t *qtype)
{
    uint32_t fid = fid_new();
    if (fid == NOFID)
        return -EMFILE;
    uint32_t from = ROOT;
    uint8_t qt = 0x80;
    int i = 0;
    do {
        int c = k - i > 16 ? 16 : k - i;
        begin(Twalk);
        put32(from), put32(fid), put16((uint16_t)c);
        for (int j = 0; j < c; j++)
            putstr(nm->name[i + j], nm->len[i + j]);
        long e = rpc();
        uint16_t got = e ? 0 : get16();
        for (int j = 0; j < got; j++) {
            qt = get8();
            get32(), get64();
        }
        if (e || got < c) {
            if (from == fid)
                clunk(fid);
            else
                fid_used[fid] = 0;
            return e ? e : -ENOENT;
        }
        from = fid;
        i += c;
    } while (i < k);
    if (qtype)
        *qtype = qt;
    return fid;
}

static long readlink_fid(uint32_t fid, char *out, size_t max)
{
    begin(Treadlink);
    put32(fid);
    long e = rpc();
    if (e)
        return e;
    uint16_t l = get16();
    if (l >= max)
        return -ENAMETOOLONG;
    memcpy(out, rb + rpos, l);
    out[l] = 0;
    return l;
}

/* Return a fid for path (a full /host path). If follow is set, this follows
 * a link at the end of the path. */
static long walk(const char *path, int follow, uint8_t *qtype)
{
    static char buf[2][1024];
    const char *rel = in_share(path);
    if (!rel)
        return -ENOENT;
    for (int depth = 0;; depth++) {
        struct names nm;
        long e = split(rel, &nm);
        if (e)
            return e;
        uint8_t qt = 0;
        long fid = walk_names(&nm, nm.n, &qt);
        if (fid < 0 || !follow || !(qt & 0x02) || depth == 8) {
            if (fid >= 0 && qtype)
                *qtype = qt;
            return fid;
        }
        /* It is a link. Read its target, and resolve it from the link's
         * directory (or from the share's root, if the target is absolute). */
        char *target = buf[depth & 1], *next = buf[!(depth & 1)];
        long l = readlink_fid((uint32_t)fid, target, 512);
        clunk((uint32_t)fid);
        if (l < 0)
            return l;
        size_t at = 0;
        if (target[0] != '/') {
            for (int j = 0; j + 1 < nm.n; j++) {
                if (at + nm.len[j] + 2 >= 512)
                    return -ENAMETOOLONG;
                next[at++] = '/';
                memcpy(next + at, nm.name[j], nm.len[j]);
                at += nm.len[j];
            }
            next[at++] = '/';
        }
        memcpy(next + at, target, (size_t)l + 1);
        rel = next;
    }
}

/* Return a fid for the directory that holds path, and copy the last name
 * in path to name */
static long walk_parent(const char *path, char *name)
{
    name[0] = 0;
    const char *rel = in_share(path);
    if (!rel)
        return -ENOENT;
    struct names nm;
    long e = split(rel, &nm);
    if (e)
        return e;
    if (!nm.n)
        return -EINVAL;                         /* path is the share itself */
    memcpy(name, nm.name[nm.n - 1], nm.len[nm.n - 1]);
    name[nm.len[nm.n - 1]] = 0;
    return walk_names(&nm, nm.n - 1, 0);
}

/* ---- Open files ---------------------------------------------------------- */

#define FILES 2048
static struct file {
    int refs;
    uint32_t fid;
    int dir, opened;
    uint64_t pos;                               /* for a directory, 9P's offset */
    char path[512];
} files[FILES];

static struct file *file(int obj)
{
    return obj >= 0 && obj < FILES && files[obj].refs ? &files[obj] : 0;
}

/* Translate AArch64's open flags into 9P2000.L's (which are x86-64 Linux's) */
static uint32_t dotl_flags(long flags)
{
    uint32_t f = (uint32_t)flags & (03 | 01000 | 02000);   /* access mode, O_TRUNC, O_APPEND */
    if (flags & 040000)
        f |= 0200000;                           /* O_DIRECTORY */
    return f;
}

long share_open(const char *path, long flags, long mode)
{
    int obj = -1;
    for (int i = 0; i < FILES; i++)
        if (!files[i].refs) {
            obj = i;
            break;
        }
    if (obj < 0)
        return -EMFILE;
    struct file *f = &files[obj];
    if (strlen(path) >= sizeof f->path)
        return -ENAMETOOLONG;
    uint8_t qt = 0;
    long fid = walk(path, !(flags & 0100000), &qt);         /* O_NOFOLLOW */
    if (fid >= 0 && (flags & 0300) == 0300) {               /* O_CREAT | O_EXCL */
        clunk((uint32_t)fid);
        return -EEXIST;
    }
    if (fid >= 0 && (flags & 040000) && !(qt & 0x80)) {
        clunk((uint32_t)fid);
        return -ENOTDIR;
    }
    int opened = 0;
    if (fid == -ENOENT && (flags & 0100)) {                 /* O_CREAT */
        char name[256];
        fid = walk_parent(path, name);
        if (fid < 0)
            return fid;
        begin(Tlcreate);
        put32((uint32_t)fid), puts0(name), put32(dotl_flags(flags) & ~01000u), put32((uint32_t)mode & 07777), put32(0);
        long e = rpc();
        if (e) {
            clunk((uint32_t)fid);
            return e;
        }
        opened = 1, qt = 0;
    } else if (fid < 0) {
        return fid;
    } else if (!(flags & 010000000)) {                      /* not O_PATH, so open it */
        begin(Tlopen);
        put32((uint32_t)fid), put32(dotl_flags(flags));
        long e = rpc();
        if (e) {
            clunk((uint32_t)fid);
            return e;
        }
        opened = 1;
    }
    memset(f, 0, sizeof *f);
    f->refs = 1;
    f->fid = (uint32_t)fid;
    f->dir = (qt & 0x80) != 0;
    f->opened = opened;
    memcpy(f->path, path, strlen(path) + 1);
    return obj;
}

void share_hold(int obj)
{
    if (file(obj))
        files[obj].refs++;
}

void share_close(int obj)
{
    struct file *f = file(obj);
    if (f && --f->refs == 0)
        clunk(f->fid);
}

const char *share_path(int obj)
{
    struct file *f = file(obj);
    return f ? f->path : "";
}

/* Read into the box's buf from off (or from the file's position, if off < 0) */
long share_read(int obj, uint64_t buf, uint64_t n, int64_t off)
{
    struct file *f = file(obj);
    if (!f || !f->opened)
        return -EBADF;
    uint64_t at = off < 0 ? f->pos : (uint64_t)off, done = 0;
    while (done < n) {
        uint32_t c = n - done > MSIZE - 64 ? MSIZE - 64 : (uint32_t)(n - done);
        begin(Tread);
        put32(f->fid), put64(at + done), put32(c);
        long e = rpc();
        if (e)
            return done ? (long)done : e;
        uint32_t got = get32();
        if (got > c)
            got = c;
        if (copy_to_box(buf + done, rb + rpos, got))
            return done ? (long)done : -EFAULT;
        done += got;
        if (got < c)
            break;
    }
    if (off < 0)
        f->pos += done;
    return (long)done;
}

long share_write(int obj, uint64_t buf, uint64_t n, int64_t off)
{
    struct file *f = file(obj);
    if (!f || !f->opened)
        return -EBADF;
    uint64_t at = off < 0 ? f->pos : (uint64_t)off, done = 0;
    while (done < n) {
        uint32_t c = n - done > MSIZE - 64 ? MSIZE - 64 : (uint32_t)(n - done);
        begin(Twrite);
        put32(f->fid), put64(at + done), put32(c);
        if (copy_from_box(tb + tlen, buf + done, c))
            return done ? (long)done : -EFAULT;
        tlen += c;
        long e = rpc();
        if (e)
            return done ? (long)done : e;
        uint32_t put = get32();
        done += put;
        if (put < c)
            break;
    }
    if (off < 0)
        f->pos += done;
    return (long)done;
}

/* ---- Attributes ---------------------------------------------------------- */

struct attr {
    uint32_t mode, uid, gid;
    uint64_t ino, nlink, size, blksize, blocks;
    uint64_t t[6];                              /* atime, mtime, ctime: seconds, nanoseconds */
};

static long getattr(uint32_t fid, struct attr *a)
{
    begin(Tgetattr);
    put32(fid), put64(0x7FF);                   /* P9_GETATTR_BASIC */
    long e = rpc();
    if (e)
        return e;
    get64();                                    /* valid */
    get8(), get32();
    a->ino = get64();                           /* the qid's path field */
    a->mode = get32(), a->uid = get32(), a->gid = get32();
    a->nlink = get64();
    get64();                                    /* rdev */
    a->size = get64(), a->blksize = get64(), a->blocks = get64();
    for (int i = 0; i < 6; i++)
        a->t[i] = get64();
    return 0;
}

/* Copy a Linux struct stat for AArch64 (128 bytes) into the box */
static long put_stat(const struct attr *a, uint64_t st)
{
    uint64_t s[16];
    memset(s, 0, sizeof s);
    s[0] = 0x9000;                              /* st_dev: a number of the share's own */
    s[1] = a->ino;
    s[2] = (uint64_t)a->mode | (uint64_t)(a->nlink ? a->nlink : 1) << 32;
    s[3] = (uint64_t)a->uid | (uint64_t)a->gid << 32;
    s[6] = a->size;
    s[7] = a->blksize ? a->blksize : 4096;
    s[8] = a->blocks;
    s[9] = a->t[0], s[10] = a->t[1], s[11] = a->t[2], s[12] = a->t[3], s[13] = a->t[4], s[14] = a->t[5];
    return copy_to_box(st, s, 128) ? -EFAULT : 0;
}

long share_stat(const char *path, uint64_t st, int follow)
{
    long fid = walk(path, follow, 0);
    if (fid < 0)
        return fid;
    struct attr a;
    long e = getattr((uint32_t)fid, &a);
    clunk((uint32_t)fid);
    return e ? e : put_stat(&a, st);
}

long share_fstat(int obj, uint64_t st)
{
    struct file *f = file(obj);
    if (!f)
        return -EBADF;
    struct attr a;
    long e = getattr(f->fid, &a);
    return e ? e : put_stat(&a, st);
}

long share_seek(int obj, int64_t off, int whence)
{
    struct file *f = file(obj);
    if (!f)
        return -EBADF;
    int64_t from = 0;
    if (whence == 1) {
        from = (int64_t)f->pos;
    } else if (whence == 2) {
        struct attr a;
        long e = getattr(f->fid, &a);
        if (e)
            return e;
        from = (int64_t)a.size;
    } else if (whence != 0) {
        return -EINVAL;
    }
    if (from + off < 0)
        return -EINVAL;
    f->pos = (uint64_t)(from + off);
    return (long)f->pos;
}

/* Send Tsetattr for fid, with the bits of valid and the values they select */
static long setattr(uint32_t fid, uint32_t valid, uint32_t mode, uint64_t size, const uint64_t t[4])
{
    begin(Tsetattr);
    put32(fid), put32(valid), put32(mode), put32(0), put32(0), put64(size);
    for (int i = 0; i < 4; i++)
        put64(t ? t[i] : 0);
    return rpc();
}

long share_truncate(int obj, uint64_t size)
{
    struct file *f = file(obj);
    return f ? setattr(f->fid, 0x8, 0, size, 0) : -EBADF;         /* P9_ATTR_SIZE */
}

long share_chmod(const char *path, uint32_t mode)
{
    long fid = walk(path, 1, 0);
    if (fid < 0)
        return fid;
    long e = setattr((uint32_t)fid, 0x1, mode & 07777, 0, 0);     /* P9_ATTR_MODE */
    clunk((uint32_t)fid);
    return e;
}

/* Set utimensat's two times from the box (if times is 0, set both to now) */
long share_utimens(const char *path, uint64_t times, int follow)
{
    uint64_t ts[4] = { 0 };
    uint32_t valid = 0x10 | 0x20;               /* ATIME, MTIME: now, unless given */
    if (times) {
        if (copy_from_box(ts, times, 32))
            return -EFAULT;
        valid = 0;
        for (int i = 0; i < 2; i++) {
            if (ts[2 * i + 1] == 0x3ffffffe)    /* UTIME_OMIT */
                continue;
            valid |= i ? 0x20 : 0x10;
            if (ts[2 * i + 1] != 0x3fffffff)    /* not UTIME_NOW: use this time */
                valid |= i ? 0x100 : 0x80;
        }
        if (!valid)
            return 0;
    }
    long fid = walk(path, follow, 0);
    if (fid < 0)
        return fid;
    long e = setattr((uint32_t)fid, valid, 0, 0, ts);
    clunk((uint32_t)fid);
    return e;
}

long share_fsync(int obj)
{
    struct file *f = file(obj);
    if (!f)
        return -EBADF;
    begin(Tfsync);
    put32(f->fid), put32(0);
    return rpc();
}

long share_access(const char *path)
{
    long fid = walk(path, 1, 0);
    if (fid < 0)
        return fid;
    clunk((uint32_t)fid);
    return 0;
}

/* Copy statfs's structure for AArch64 (120 bytes) into the box. This
 * reports tmpfs's type rather than 9P's. HostFS flushes a file on a 9P disc
 * when it closes the file, because Linux caches what is written; the HAL
 * caches nothing, so the flush is not needed. */
long share_statfs(const char *path, uint64_t out)
{
    long fid = path ? walk(path, 1, 0) : ROOT;
    if (fid < 0)
        return fid;
    begin(Tstatfs);
    put32((uint32_t)fid);
    long e = rpc();
    uint64_t s[15];
    memset(s, 0, sizeof s);
    if (!e) {                                   /* read before the clunk's reply overwrites it */
        get32();                                /* the server's type (ignored) */
        s[0] = 0x01021994;                      /* TMPFS_MAGIC */
        s[1] = get32();                         /* bsize */
        s[2] = get64(), s[3] = get64(), s[4] = get64(), s[5] = get64(), s[6] = get64();
        s[7] = get64();                         /* fsid */
        s[8] = get32();                         /* namelen */
        s[9] = s[1];                            /* frsize */
    }
    if (fid != ROOT)
        clunk((uint32_t)fid);
    if (e)
        return e;
    return copy_to_box(out, s, 120) ? -EFAULT : 0;
}

/* ---- Names --------------------------------------------------------------- */

long share_mkdir(const char *path, uint32_t mode)
{
    char name[256];
    long fid = walk_parent(path, name);
    if (fid < 0)
        return fid == -EINVAL ? -EEXIST : fid;
    begin(Tmkdir);
    put32((uint32_t)fid), puts0(name), put32(mode & 07777), put32(0);
    long e = rpc();
    clunk((uint32_t)fid);
    return e;
}

long share_unlink(const char *path, long flags)
{
    char name[256];
    long fid = walk_parent(path, name);
    if (fid < 0)
        return fid == -EINVAL ? -EPERM : fid;
    begin(Tunlinkat);
    put32((uint32_t)fid), puts0(name), put32((uint32_t)flags & 0x200);    /* AT_REMOVEDIR */
    long e = rpc();
    clunk((uint32_t)fid);
    return e;
}

long share_rename(const char *from, const char *to)
{
    char a[256], b[256];
    long fa = walk_parent(from, a);
    if (fa < 0)
        return fa;
    long fb = walk_parent(to, b);
    if (fb < 0) {
        clunk((uint32_t)fa);
        return fb;
    }
    begin(Trenameat);
    put32((uint32_t)fa), puts0(a), put32((uint32_t)fb), puts0(b);
    long e = rpc();
    clunk((uint32_t)fa);
    clunk((uint32_t)fb);
    return e;
}

/* readlink. Copy the link's target into the box. Returns -EINVAL if path
 * is not a link. */
long share_readlink(const char *path, uint64_t buf, uint64_t n)
{
    uint8_t qt;
    long fid = walk(path, 0, &qt);
    if (fid < 0)
        return fid;
    static char target[1024];
    long l = qt & 0x02 ? readlink_fid((uint32_t)fid, target, sizeof target) : -EINVAL;
    clunk((uint32_t)fid);
    if (l < 0)
        return l;
    if ((uint64_t)l > n)
        l = (long)n;
    return copy_to_box(buf, target, (size_t)l) ? -EFAULT : l;
}

/* getdents64 for a directory opened on the share */
long share_getdents(int obj, uint64_t buf, uint64_t n)
{
    struct file *f = file(obj);
    if (!f || !f->opened)
        return -EBADF;
    if (!f->dir)
        return -ENOTDIR;
    /* A 9P entry is 24 bytes plus its name. A Linux entry is 19 bytes plus
     * the name, its terminator, and padding to a multiple of 8. Ask the
     * server for only as much as will fit after conversion. */
    uint32_t ask = (uint32_t)(n / 2 > MSIZE - 64 ? MSIZE - 64 : n / 2);
    if (ask < 300)
        return -EINVAL;
    begin(Treaddir);
    put32(f->fid), put64(f->pos), put32(ask);
    long e = rpc();
    if (e)
        return e;
    uint32_t count = get32(), end = rpos + count;
    if (end > rlen)
        end = rlen;
    uint64_t done = 0;
    static unsigned char rec[512];
    while (rpos < end) {
        get8(), get32();
        uint64_t ino = get64(), off = get64();
        uint8_t type = get8();
        uint16_t l = get16();
        if (rpos + l > end || l > 255)
            break;
        uint16_t reclen = (uint16_t)((19 + l + 1 + 7) & ~7u);
        if (done + reclen > n)
            break;
        memset(rec, 0, reclen);
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &off, 8);
        memcpy(rec + 16, &reclen, 2);
        rec[18] = type;
        memcpy(rec + 19, rb + rpos, l);
        rpos += l;
        if (copy_to_box(buf + done, rec, reclen))
            return -EFAULT;
        done += reclen;
        f->pos = off;
    }
    return (long)done;
}

/* ---- Extended attributes (HostFS keeps RISC OS attributes in one) ------- */

long share_getxattr(const char *path, const char *name, uint64_t buf, uint64_t n, int follow)
{
    long fid = walk(path, follow, 0);
    if (fid < 0)
        return fid;
    uint32_t xf = fid_new();
    if (xf == NOFID) {
        clunk((uint32_t)fid);
        return -EMFILE;
    }
    begin(Txattrwalk);
    put32((uint32_t)fid), put32(xf), puts0(name);
    long e = rpc();
    uint64_t size = e ? 0 : get64();            /* read before the clunk's reply overwrites it */
    clunk((uint32_t)fid);
    if (e) {
        fid_used[xf] = 0;
        return e == -ENOENT ? -61 : e;          /* ENODATA */
    }
    if (n && size > n)
        e = -ERANGE;
    else if (n && size) {
        begin(Tread);
        put32(xf), put64(0), put32((uint32_t)size);
        e = rpc();
        if (!e) {
            uint32_t got = get32();
            e = copy_to_box(buf, rb + rpos, got) ? -EFAULT : (long)got;
        }
    } else {
        e = (long)size;
    }
    clunk(xf);
    return e;
}

long share_setxattr(const char *path, const char *name, uint64_t val, uint64_t n, long flags, int follow)
{
    if (n > MSIZE - 64)
        return -ERANGE;
    long fid = walk(path, follow, 0);
    if (fid < 0)
        return fid;
    begin(Txattrcreate);
    put32((uint32_t)fid), puts0(name), put64(n), put32((uint32_t)flags);
    long e = rpc();
    if (!e && n) {
        begin(Twrite);
        put32((uint32_t)fid), put64(0), put32((uint32_t)n);
        if (copy_from_box(tb + tlen, val, n))
            e = -EFAULT;
        tlen += (uint32_t)n;
        if (!e)
            e = rpc();
    }
    clunk((uint32_t)fid);                       /* the clunk sets the attribute */
    return e;
}

/* ---- The device ---------------------------------------------------------- */

int ninep_attach(uint64_t b)
{
    if (base)
        return -1;                              /* only one share is supported */
    uint64_t t = pages_alloc_run(MSIZE / PAGE_SIZE), r = pages_alloc_run(MSIZE / PAGE_SIZE);
    if (!t || !r || virtio_begin(b, 0) || virtio_queue(b, 0, &vq, 8))
        return -1;
    virtio_go(b);
    base = b;
    tb = pa_to_va(t), rb = pa_to_va(r), tb_pa = t, rb_pa = r;
    begin(Tversion);
    put32(MSIZE), puts0("9P2000.L");
    long e = rpc();
    if (!e) {
        begin(Tattach);
        put32(ROOT), put32(NOFID), puts0(""), puts0(""), put32(0);
        e = rpc();
    }
    if (e) {
        kprintf("HAL: the share did not answer (%ld)\n", e);
        base = 0;
        return -1;
    }
    fid_used[ROOT] = 1;
    kprintf("HAL: share: 9P2000.L at %lx\n", b);
    return 0;
}

long share_pread_hal(int obj, void *buf, uint64_t n, uint64_t off)
{
    struct file *f = file(obj);
    if (!f || !f->opened)
        return -EBADF;
    uint64_t done = 0;
    while (done < n) {
        uint32_t c = n - done > MSIZE - 64 ? MSIZE - 64 : (uint32_t)(n - done);
        begin(Tread);
        put32(f->fid), put64(off + done), put32(c);
        long e = rpc();
        if (e)
            return done ? (long)done : e;
        uint32_t got = get32();
        if (got > c)
            got = c;
        memcpy((char *)buf + done, rb + rpos, got);
        done += got;
        if (got < c)
            break;
    }
    return (long)done;
}
