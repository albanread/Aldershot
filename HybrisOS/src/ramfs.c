/* ramfs.c. This file holds the box's own file tree: a file system in
 * memory, like Linux's initramfs.
 *
 * At start it holds the contents of the ROM's cpio image (/init, the box's
 * programs and headers, /etc, /dev and so on). The HAL reads each file's
 * data in place in the image until something writes to the file. From then
 * on, and for every file created later, a memfd object (vm.c) holds the
 * data. The object is paged, and grows as the file is written. The box
 * writes /etc/hosts and /etc/resolv.conf here, and makes scratch discs
 * under /tmp, as it does under Linux.
 *
 * The file system supports directories, regular files and symbolic links,
 * with names of up to 255 bytes. It keeps modes, times and extended
 * attributes. Each node is an entry in a single table, and each directory
 * keeps its children in a list. If the box unlinks a node while it is open,
 * the node lasts until the box closes it. */
#include "hal.h"

#define EPERM   1
#define ENOENT  2
#define EBADF   9
#define ENOMEM  12
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOTEMPTY 39
#define ELOOP   40
#define ENODATA 61

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000

#define NODES 16384
struct node {
    uint32_t mode;                              /* 0 if the entry is free */
    int parent, child, next;                    /* -1 if there is none */
    int opens, unlinked;
    int obj;                                    /* memfd object holding the data, or -1 */
    const uint8_t *ro;                          /* else data in the cpio image, until written */
    uint64_t size;
    uint64_t t[6];                              /* atime, mtime, ctime: seconds, nanoseconds */
    int xattr;                                  /* first extended attribute, or -1 */
    char name[256];
};
static struct node *nodes;
#define ROOT 0

static void stamp(struct node *n, int which)    /* bit 0 atime, bit 1 mtime, bit 2 ctime */
{
    uint64_t s, ns;
    hal_now(&s, &ns);
    for (int i = 0; i < 3; i++)
        if (which & 1 << i)
            n->t[2 * i] = s, n->t[2 * i + 1] = ns;
}

static int node_new(int parent, const char *name, size_t len, uint32_t mode)
{
    static int hint = 1;
    for (int k = 0; k < NODES - 1; k++) {
        int i = 1 + (hint - 1 + k) % (NODES - 1);
        struct node *n = &nodes[i];
        if (n->mode)
            continue;
        hint = i + 1;
        memset(n, 0, sizeof *n);
        n->mode = mode;
        n->parent = parent;
        n->child = -1;
        n->obj = -1;
        n->xattr = -1;
        memcpy(n->name, name, len);
        n->name[len] = 0;
        n->next = nodes[parent].child;
        nodes[parent].child = i;
        stamp(n, 7);
        stamp(&nodes[parent], 6);
        return i;
    }
    return -ENOSPC;
}

static void detach(int i)
{
    struct node *n = &nodes[i];
    int *p = &nodes[n->parent].child;
    while (*p >= 0 && *p != i)
        p = &nodes[*p].next;
    if (*p == i)
        *p = n->next;
    stamp(&nodes[n->parent], 6);
}

/* ---- Extended attributes ---------------------------------------------- */

#define XATTRS 8192
static struct xattr {
    int used, next;
    uint16_t len;
    char name[64];
    uint8_t value[192];
} xattrs[XATTRS];

static void node_free(int i)
{
    struct node *n = &nodes[i];
    if (n->obj >= 0)
        memfd_obj_trim(n->obj, 0, 1);
    for (int x = n->xattr; x >= 0; x = xattrs[x].next)
        xattrs[x].used = 0;
    n->mode = 0;
}

static void drop(int i)                         /* unlink; free now if not open */
{
    detach(i);
    nodes[i].unlinked = 1;
    if (!nodes[i].opens)
        node_free(i);
}

static int find(int dir, const char *name, size_t len)
{
    for (int c = nodes[dir].child; c >= 0; c = nodes[c].next)
        if (!strncmp(nodes[c].name, name, len) && !nodes[c].name[len])
            return c;
    return -1;
}

/* ---- Paths ------------------------------------------------------------- */

/* Walk the path, following symbolic links (it follows the last component
 * only if follow is set). On return *node is the node the path names. If
 * only the last name is missing, *node is -1, the function returns -ENOENT,
 * and *parent and leaf say where that name would be. If canon is given, it
 * receives the path with every link followed, and with any names that do
 * not exist left as they are. The HAL uses this canonical path to choose
 * between its other trees (the share and /proc). */
static long walk(const char *path, int follow, int *node, int *parent, char *leaf, char *canon)
{
    static char buf[2][1024];
    int b = 0, links = 0, cur = ROOT, missing = 0;
    size_t cl = 0;
    if (strlen(path) >= sizeof buf[0])
        return -ENAMETOOLONG;
    memcpy(buf[0], path, strlen(path) + 1);
    const char *p = buf[0];
    long err = 0;
    if (canon)
        canon[0] = 0;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t l = (size_t)(p - s);
        const char *q = p;
        while (*q == '/')
            q++;
        int last = !*q;
        if (l > 255)
            return -ENAMETOOLONG;
        if (l == 1 && s[0] == '.')
            continue;
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (missing) {
                missing--;
            } else {
                cur = nodes[cur].parent;
            }
            while (cl && canon[cl - 1] != '/')
                cl--;
            if (cl)
                cl--;
            if (canon)
                canon[cl] = 0;
            continue;
        }
        int child = missing ? -1 : find(cur, s, l);
        if (child >= 0 && (nodes[child].mode & S_IFMT) == S_IFLNK && (!last || follow)) {
            if (++links > 40)
                return -ELOOP;
            /* Build the link's target followed by the rest of the path */
            struct node *ln = &nodes[child];
            char *nb = buf[!b];
            size_t tl = ln->size, rl = strlen(p);
            if (tl + rl + 2 >= sizeof buf[0])
                return -ENAMETOOLONG;
            if (ln->ro)
                memcpy(nb, ln->ro, tl);
            else
                memfd_obj_copy_hal(ln->obj, 0, nb, tl, 1);
            memcpy(nb + tl, p, rl + 1);
            if (nb[0] == '/') {
                cur = ROOT, cl = 0;
                if (canon)
                    canon[0] = 0;
            }
            b = !b;
            p = nb;
            continue;
        }
        if (canon) {
            if (cl + l + 2 >= 1024)
                return -ENAMETOOLONG;
            canon[cl++] = '/';
            memcpy(canon + cl, s, l);
            cl += l;
            canon[cl] = 0;
        }
        if (child < 0) {
            if (last && !missing) {
                if (parent)
                    *parent = cur;
                if (leaf) {
                    memcpy(leaf, s, l);
                    leaf[l] = 0;
                }
                err = -ENOENT;
                cur = -1;
            } else {
                if (!err)
                    err = missing || (nodes[cur].mode & S_IFMT) == S_IFDIR ? -ENOENT : -ENOTDIR;
                missing++;
            }
            continue;
        }
        if (!last && (nodes[child].mode & S_IFMT) != S_IFDIR) {
            err = -ENOTDIR;
            missing++;
            continue;
        }
        cur = child;
        if (last) {
            if (parent)
                *parent = nodes[child].parent;
            if (leaf) {
                memcpy(leaf, s, l);
                leaf[l] = 0;
            }
        }
    }
    if (canon && !cl)
        canon[0] = '/', canon[1] = 0;
    if (err) {
        if (node)
            *node = -1;
        return err == -ENOENT && cur == -1 ? -ENOENT : err == -ENOENT ? -ENOENT : err;
    }
    if (node)
        *node = cur;
    if (cur == ROOT && parent) {
        *parent = ROOT;
        if (leaf)
            leaf[0] = 0;
    }
    return 0;
}

/* Return the path with every link in this tree followed (the last one only
 * if follow is set). The result is where the path leads, which may be
 * outside this tree. */
long ram_resolve(const char *path, char *out, int follow)
{
    long e = walk(path, follow, 0, 0, 0, out);
    return e == -ELOOP || e == -ENAMETOOLONG ? e : 0;
}

static int lookup(const char *path, int follow, long *err)
{
    int n;
    *err = walk(path, follow, &n, 0, 0, 0);
    return *err ? -1 : n;
}

/* ---- Data -------------------------------------------------------------- */

/* Make the node's data writable. This gives the node a memfd object and
 * copies any data from the image into it. */
static long writable(struct node *n)
{
    if (n->obj >= 0)
        return 0;
    int obj = memfd_obj_new();
    if (obj < 0)
        return obj;
    if (n->ro && n->size && memfd_obj_copy_hal(obj, 0, (void *)n->ro, n->size, 0)) {
        memfd_obj_trim(obj, 0, 1);
        return -ENOMEM;
    }
    n->obj = obj;
    n->ro = 0;
    return 0;
}

static long set_size(struct node *n, uint64_t size)
{
    long e = writable(n);
    if (e)
        return e;
    if (size < n->size)
        memfd_obj_trim(n->obj, size, 0);
    /* If the page holding the new end is kept, the part of it beyond the
     * new end must read as zeros. */
    if (size < n->size && (size & PAGE_MASK)) {
        static unsigned char zero[PAGE_SIZE];
        uint64_t c = PAGE_SIZE - (size & PAGE_MASK);
        if (c > n->size - size)
            c = n->size - size;
        memfd_obj_copy_hal(n->obj, size, zero, c, 0);
    }
    n->size = size;
    stamp(n, 6);
    return 0;
}

/* ---- Open files -------------------------------------------------------- */

#define FILES 2048
static struct rfile {
    int refs, node, append;
    uint64_t pos;
    char path[512];
} files[FILES];

static struct rfile *rf(int obj)
{
    return obj >= 0 && obj < FILES && files[obj].refs ? &files[obj] : 0;
}

long ram_open(const char *path, long flags, long mode)
{
    int obj = -1;
    for (int i = 0; i < FILES; i++)
        if (!files[i].refs) {
            obj = i;
            break;
        }
    if (obj < 0)
        return -EMFILE;
    if (strlen(path) >= sizeof files[0].path)
        return -ENAMETOOLONG;
    int n, parent;
    char leaf[256];
    long e = walk(path, !(flags & 0100000), &n, &parent, leaf, 0);   /* O_NOFOLLOW */
    if (e == -ENOENT && n < 0 && (flags & 0100) && leaf[0] && parent >= 0 && nodes[parent].mode) {
        if ((nodes[parent].mode & S_IFMT) != S_IFDIR)
            return -ENOTDIR;
        int c = node_new(parent, leaf, strlen(leaf), S_IFREG | ((uint32_t)mode & 07777));
        if (c < 0)
            return c;
        n = c;
    } else if (e) {
        return e;
    } else if ((flags & 0300) == 0300) {        /* O_CREAT | O_EXCL */
        return -EEXIST;
    }
    struct node *nd = &nodes[n];
    uint32_t type = nd->mode & S_IFMT;
    if ((flags & 040000) && type != S_IFDIR)    /* O_DIRECTORY */
        return -ENOTDIR;
    if (type == S_IFDIR && (flags & 3))
        return -EISDIR;
    if (type == S_IFLNK && !(flags & 010000000))
        return -ELOOP;                          /* a link opened with O_NOFOLLOW */
    if ((flags & 01000) && (flags & 3) && type == S_IFREG && (e = set_size(nd, 0)))
        return e;
    struct rfile *f = &files[obj];
    memset(f, 0, sizeof *f);
    f->refs = 1;
    f->node = n;
    f->append = (flags & 02000) != 0;
    memcpy(f->path, path, strlen(path) + 1);
    nd->opens++;
    return obj;
}

void ram_hold(int obj)
{
    if (rf(obj))
        files[obj].refs++;
}

void ram_close(int obj)
{
    struct rfile *f = rf(obj);
    if (!f || --f->refs)
        return;
    struct node *n = &nodes[f->node];
    if (--n->opens == 0 && n->unlinked)
        node_free(f->node);
}

const char *ram_path(int obj)
{
    struct rfile *f = rf(obj);
    return f ? f->path : "";
}

long ram_read(int obj, uint64_t buf, uint64_t len, int64_t off)
{
    struct rfile *f = rf(obj);
    if (!f)
        return -EBADF;
    struct node *n = &nodes[f->node];
    if ((n->mode & S_IFMT) == S_IFDIR)
        return -EISDIR;
    uint64_t at = off < 0 ? f->pos : (uint64_t)off;
    if (at >= n->size)
        return 0;
    if (len > n->size - at)
        len = n->size - at;
    long e = n->ro ? (copy_to_box(buf, n->ro + at, len) ? -EFAULT : 0)
                   : memfd_obj_copy(n->obj, at, buf, len, 1);
    if (e)
        return e;
    if (off < 0)
        f->pos += len;
    return (long)len;
}

long ram_write(int obj, uint64_t buf, uint64_t len, int64_t off)
{
    struct rfile *f = rf(obj);
    if (!f)
        return -EBADF;
    struct node *n = &nodes[f->node];
    long e = writable(n);
    if (e)
        return e;
    uint64_t at = off < 0 ? (f->append ? n->size : f->pos) : (uint64_t)off;
    if ((e = memfd_obj_copy(n->obj, at, buf, len, 0)))
        return e;
    if (at + len > n->size)
        n->size = at + len;
    stamp(n, 6);
    if (off < 0)
        f->pos = at + len;
    return (long)len;
}

long ram_seek(int obj, int64_t off, int whence)
{
    struct rfile *f = rf(obj);
    if (!f)
        return -EBADF;
    int64_t from = whence == 0 ? 0 : whence == 1 ? (int64_t)f->pos
                 : whence == 2 ? (int64_t)nodes[f->node].size : -1;
    if (from < 0 || from + off < 0)
        return -EINVAL;
    f->pos = (uint64_t)(from + off);
    return (long)f->pos;
}

long ram_truncate(int obj, uint64_t size)
{
    struct rfile *f = rf(obj);
    if (!f)
        return -EBADF;
    if ((nodes[f->node].mode & S_IFMT) != S_IFREG)
        return -EINVAL;
    return set_size(&nodes[f->node], size);
}

/* ---- Attributes -------------------------------------------------------- */

static long put_stat(int i, uint64_t st)
{
    const struct node *n = &nodes[i];
    uint64_t s[16];
    memset(s, 0, sizeof s);
    s[0] = 1;                                   /* st_dev */
    s[1] = (uint64_t)i + 1;
    s[2] = (uint64_t)n->mode | 1UL << 32;       /* st_nlink is 1 */
    s[6] = n->size;
    s[7] = 4096;
    s[8] = (n->size + 511) / 512;
    for (int k = 0; k < 6; k++)
        s[9 + k] = n->t[k];
    return copy_to_box(st, s, 128) ? -EFAULT : 0;
}

long ram_stat(const char *path, uint64_t st, int follow)
{
    long e;
    int n = lookup(path, follow, &e);
    return n < 0 ? e : put_stat(n, st);
}

long ram_fstat(int obj, uint64_t st)
{
    struct rfile *f = rf(obj);
    return f ? put_stat(f->node, st) : -EBADF;
}

long ram_chmod(const char *path, uint32_t mode)
{
    long e;
    int n = lookup(path, 1, &e);
    if (n < 0)
        return e;
    nodes[n].mode = (nodes[n].mode & S_IFMT) | (mode & 07777);
    stamp(&nodes[n], 4);
    return 0;
}

long ram_utimens(const char *path, uint64_t times, int follow)
{
    long e;
    int n = lookup(path, follow, &e);
    if (n < 0)
        return e;
    uint64_t ts[4], s, ns;
    hal_now(&s, &ns);
    if (!times) {
        ts[0] = ts[2] = s, ts[1] = ts[3] = ns;
    } else if (copy_from_box(ts, times, 32)) {
        return -EFAULT;
    }
    for (int i = 0; i < 2; i++) {
        if (ts[2 * i + 1] == 0x3ffffffe)        /* UTIME_OMIT */
            continue;
        if (ts[2 * i + 1] == 0x3fffffff)        /* UTIME_NOW */
            ts[2 * i] = s, ts[2 * i + 1] = ns;
        nodes[n].t[2 * i] = ts[2 * i], nodes[n].t[2 * i + 1] = ts[2 * i + 1];
    }
    stamp(&nodes[n], 4);
    return 0;
}

long ram_access(const char *path)
{
    long e;
    return lookup(path, 1, &e) < 0 ? e : 0;
}

long ram_statfs(uint64_t out)
{
    uint64_t s[15];
    memset(s, 0, sizeof s);
    s[0] = 0x01021994;                          /* TMPFS_MAGIC */
    s[1] = 4096;
    s[2] = (boot.ram_size >> 12);
    s[3] = s[4] = pages_free();
    s[5] = NODES;
    for (int i = 0; i < NODES; i++)
        s[6] += !nodes[i].mode;
    s[8] = 255;
    s[9] = 4096;
    return copy_to_box(out, s, 120) ? -EFAULT : 0;
}

/* ---- Names ------------------------------------------------------------- */

static long make(const char *path, uint32_t mode, const char *target)
{
    int n, parent;
    char leaf[256];
    long e = walk(path, 0, &n, &parent, leaf, 0);
    if (!e)
        return -EEXIST;
    if (e != -ENOENT || n >= 0 || !leaf[0])
        return e;
    if ((nodes[parent].mode & S_IFMT) != S_IFDIR)
        return -ENOTDIR;
    int c = node_new(parent, leaf, strlen(leaf), mode);
    if (c < 0)
        return c;
    if (target) {
        size_t l = strlen(target);
        if (writable(&nodes[c]) || memfd_obj_copy_hal(nodes[c].obj, 0, (void *)target, l, 0)) {
            drop(c);
            return -ENOMEM;
        }
        nodes[c].size = l;
    }
    return 0;
}

long ram_mkdir(const char *path, uint32_t mode)
{
    return make(path, S_IFDIR | (mode & 07777), 0);
}

long ram_symlink(const char *target, const char *path)
{
    return make(path, S_IFLNK | 0777, target);
}

long ram_unlink(const char *path, long flags)
{
    long e;
    int n = lookup(path, 0, &e);
    if (n < 0)
        return e;
    if (n == ROOT)
        return -EBUSY;
    int dir = (nodes[n].mode & S_IFMT) == S_IFDIR;
    if (flags & 0x200) {                        /* AT_REMOVEDIR */
        if (!dir)
            return -ENOTDIR;
        if (nodes[n].child >= 0)
            return -ENOTEMPTY;
    } else if (dir) {
        return -EISDIR;
    }
    drop(n);
    return 0;
}

long ram_rename(const char *from, const char *to)
{
    long e;
    int f = lookup(from, 0, &e);
    if (f < 0)
        return e;
    if (f == ROOT)
        return -EBUSY;
    int t, parent;
    char leaf[256];
    e = walk(to, 0, &t, &parent, leaf, 0);
    if (e && !(e == -ENOENT && t < 0 && leaf[0]))
        return e;
    if (!e && t == f)
        return 0;
    int fdir = (nodes[f].mode & S_IFMT) == S_IFDIR;
    if (!e) {
        int tdir = (nodes[t].mode & S_IFMT) == S_IFDIR;
        if (fdir && !tdir)
            return -ENOTDIR;
        if (!fdir && tdir)
            return -EISDIR;
        if (tdir && nodes[t].child >= 0)
            return -ENOTEMPTY;
        parent = nodes[t].parent;
    }
    if ((nodes[parent].mode & S_IFMT) != S_IFDIR)
        return -ENOTDIR;
    for (int a = parent; fdir; a = nodes[a].parent) {   /* not into itself */
        if (a == f)
            return -EINVAL;
        if (a == ROOT)
            break;
    }
    if (!e)
        drop(t);
    detach(f);
    struct node *n = &nodes[f];
    memcpy(n->name, leaf, strlen(leaf) + 1);
    n->parent = parent;
    n->next = nodes[parent].child;
    nodes[parent].child = f;
    stamp(&nodes[parent], 6);
    stamp(n, 4);
    return 0;
}

long ram_readlink(const char *path, uint64_t buf, uint64_t len)
{
    long e;
    int n = lookup(path, 0, &e);
    if (n < 0)
        return e;
    struct node *nd = &nodes[n];
    if ((nd->mode & S_IFMT) != S_IFLNK)
        return -EINVAL;
    if (len > nd->size)
        len = nd->size;
    e = nd->ro ? (copy_to_box(buf, nd->ro, len) ? -EFAULT : 0) : memfd_obj_copy(nd->obj, 0, buf, len, 1);
    return e ? e : (long)len;
}

/* getdents64. This returns ".", "..", then the children. The file position
 * is the number of entries returned so far. */
long ram_getdents(int obj, uint64_t buf, uint64_t len)
{
    struct rfile *f = rf(obj);
    if (!f)
        return -EBADF;
    struct node *d = &nodes[f->node];
    if ((d->mode & S_IFMT) != S_IFDIR)
        return -ENOTDIR;
    uint64_t done = 0, k;
    int c = d->child;
    if (f->pos < 2) {
        k = f->pos;
    } else {
        for (k = 2; k < f->pos && c >= 0; k++)   /* skip to where the last call stopped */
            c = nodes[c].next;
    }
    static unsigned char rec[280];
    for (;;) {
        const char *name;
        uint64_t ino;
        uint8_t type;
        if (k == 0)
            name = ".", ino = (uint64_t)f->node + 1, type = 4;
        else if (k == 1)
            name = "..", ino = (uint64_t)d->parent + 1, type = 4;
        else if (c >= 0) {
            name = nodes[c].name, ino = (uint64_t)c + 1;
            uint32_t m = nodes[c].mode & S_IFMT;
            type = m == S_IFDIR ? 4 : m == S_IFLNK ? 10 : m == S_IFREG ? 8 : 2;
        } else
            break;
        size_t l = strlen(name);
        uint16_t reclen = (uint16_t)((19 + l + 1 + 7) & ~7UL);
        if (done + reclen > len)
            break;
        memset(rec, 0, reclen);
        int64_t next = (int64_t)k + 1;
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &next, 8);
        memcpy(rec + 16, &reclen, 2);
        rec[18] = type;
        memcpy(rec + 19, name, l);
        if (copy_to_box(buf + done, rec, reclen))
            return -EFAULT;
        done += reclen;
        if (k >= 2)
            c = nodes[c].next;
        k++;
    }
    if (!done && len < 280 && (k < 2 || c >= 0))
        return -EINVAL;                         /* buffer too small for one entry */
    f->pos = k;
    return (long)done;
}

/* ---- Extended attributes ------------------------------------------------ */

long ram_getxattr(const char *path, const char *name, uint64_t buf, uint64_t len, int follow)
{
    long e;
    int n = lookup(path, follow, &e);
    if (n < 0)
        return e;
    for (int x = nodes[n].xattr; x >= 0; x = xattrs[x].next)
        if (!strcmp(xattrs[x].name, name)) {
            if (!len)
                return xattrs[x].len;
            if (len < xattrs[x].len)
                return -ERANGE;
            return copy_to_box(buf, xattrs[x].value, xattrs[x].len) ? -EFAULT : xattrs[x].len;
        }
    return -ENODATA;
}

long ram_setxattr(const char *path, const char *name, uint64_t val, uint64_t len, long flags, int follow)
{
    long e;
    int n = lookup(path, follow, &e);
    if (n < 0)
        return e;
    if (strlen(name) >= sizeof xattrs[0].name || len > sizeof xattrs[0].value)
        return -ERANGE;
    int x;
    for (x = nodes[n].xattr; x >= 0 && strcmp(xattrs[x].name, name); x = xattrs[x].next)
        ;
    if (x >= 0 && (flags & 1))                  /* XATTR_CREATE */
        return -EEXIST;
    if (x < 0 && (flags & 2))                   /* XATTR_REPLACE */
        return -ENODATA;
    if (x < 0) {
        for (x = 0; x < XATTRS && xattrs[x].used; x++)
            ;
        if (x == XATTRS)
            return -ENOSPC;
        xattrs[x].used = 1;
        memcpy(xattrs[x].name, name, strlen(name) + 1);
        xattrs[x].next = nodes[n].xattr;
        nodes[n].xattr = x;
    }
    if (copy_from_box(xattrs[x].value, val, len))
        return -EFAULT;
    xattrs[x].len = (uint16_t)len;
    stamp(&nodes[n], 4);
    return 0;
}

/* ---- The cpio image ----------------------------------------------------- */

static uint64_t hex8(const char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 4 | (uint64_t)(p[i] <= '9' ? p[i] - '0' : (p[i] | 32) - 'a' + 10);
    return v;
}

/* Return the directory that is the parent of path, creating it if needed.
 * *leaf is set to the last name in the path. */
static int dir_for(const char *path, const char **leaf)
{
    int cur = ROOT;
    const char *p = path;
    for (;;) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        if (!*p) {
            *leaf = s;
            return cur;
        }
        int c = find(cur, s, (size_t)(p - s));
        if (c < 0 && (c = node_new(cur, s, (size_t)(p - s), S_IFDIR | 0755)) < 0)
            return c;
        cur = c;
    }
}

void ram_init(void)
{
    uint64_t pages = (NODES * sizeof(struct node) + PAGE_MASK) >> 12;
    uint64_t pa = pages_alloc_run(pages);
    if (!pa)
        panic("no memory for the RAM file system");
    nodes = pa_to_va(pa);
    nodes[ROOT].mode = S_IFDIR | 0755;
    nodes[ROOT].parent = ROOT;
    nodes[ROOT].child = nodes[ROOT].next = -1;
    nodes[ROOT].obj = nodes[ROOT].xattr = -1;
    stamp(&nodes[ROOT], 7);
    unsigned count = 0;
    const char *p = pa_to_va(boot.initrd_start), *end = pa_to_va(boot.initrd_end);
    while (p + 110 <= end && !memcmp(p, "070701", 6)) {
        uint64_t mode = hex8(p + 14), mtime = hex8(p + 46), size = hex8(p + 54), nsize = hex8(p + 94);
        const char *name = p + 110;
        const uint8_t *data = (const uint8_t *)p + ((110 + nsize + 3) & ~3UL);
        if (!strcmp(name, "TRAILER!!!"))
            break;
        p = (const char *)data + ((size + 3) & ~3UL);
        while (*name == '.' || *name == '/')
            name++;
        if (!*name)
            continue;
        const char *leaf;
        int dir = dir_for(name, &leaf);
        if (dir < 0)
            break;
        int n = find(dir, leaf, strlen(leaf));
        if (n < 0 && (n = node_new(dir, leaf, strlen(leaf), (uint32_t)mode)) < 0)
            break;
        nodes[n].mode = (uint32_t)mode;
        if ((mode & S_IFMT) == S_IFREG || (mode & S_IFMT) == S_IFLNK)
            nodes[n].ro = data, nodes[n].size = size;
        for (int k = 0; k < 3; k++)
            nodes[n].t[2 * k] = mtime, nodes[n].t[2 * k + 1] = 0;
        count++;
    }
    const char *leaf;
    int d = dir_for("tmp/x", &leaf);
    if (d >= 0)
        nodes[d].mode = S_IFDIR | 01777;
    kprintf("HAL: RAM file system: %u entries from the image\n", count);
}

long ram_pread_hal(int obj, void *buf, uint64_t len, uint64_t at)
{
    struct rfile *f = rf(obj);
    if (!f)
        return -EBADF;
    struct node *n = &nodes[f->node];
    if (at >= n->size)
        return 0;
    if (len > n->size - at)
        len = n->size - at;
    if (n->ro)
        memcpy(buf, n->ro + at, len);
    else if (memfd_obj_copy_hal(n->obj, at, buf, len, 1))
        return -EFAULT;
    return (long)len;
}
