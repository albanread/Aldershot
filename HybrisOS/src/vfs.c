/* vfs.c. This file decides which file system handles a request. The share
 * (ninep.c) handles /host and everything below it; the RAM file system
 * (ramfs.c) handles the rest of the tree.
 *
 * The RAM file system first makes a path canonical, following every symbolic
 * link in it (it follows the last component only if the call asks for that).
 * Such a link may lead into the share, or to one of the HAL's own files in
 * /proc, which syscall.c handles before it calls this file. A handle stands
 * for an open file. The handle is the file system's object number, with
 * H_SHARE added if the object belongs to the share. */
#include "hal.h"

#define H_SHARE 0x10000
#define IS_SHARE(h) ((h) & H_SHARE)
#define OBJ(h) ((int)((h) & 0xFFFF))

long vfs_resolve(const char *path, char *canon, int follow)
{
    return ram_resolve(path, canon, follow);
}

long vfs_open(const char *canon, long flags, long mode)
{
    if (share_has(canon)) {
        long o = share_open(canon, flags, mode);
        return o < 0 ? o : o | H_SHARE;
    }
    return ram_open(canon, flags, mode);
}

void vfs_hold(long h) { IS_SHARE(h) ? share_hold(OBJ(h)) : ram_hold(OBJ(h)); }
void vfs_close(long h) { IS_SHARE(h) ? share_close(OBJ(h)) : ram_close(OBJ(h)); }
const char *vfs_path(long h) { return IS_SHARE(h) ? share_path(OBJ(h)) : ram_path(OBJ(h)); }

long vfs_read(long h, uint64_t buf, uint64_t n, int64_t off)
{
    return IS_SHARE(h) ? share_read(OBJ(h), buf, n, off) : ram_read(OBJ(h), buf, n, off);
}

long vfs_write(long h, uint64_t buf, uint64_t n, int64_t off)
{
    return IS_SHARE(h) ? share_write(OBJ(h), buf, n, off) : ram_write(OBJ(h), buf, n, off);
}

long vfs_seek(long h, int64_t off, int whence)
{
    return IS_SHARE(h) ? share_seek(OBJ(h), off, whence) : ram_seek(OBJ(h), off, whence);
}

long vfs_fstat(long h, uint64_t st)
{
    return IS_SHARE(h) ? share_fstat(OBJ(h), st) : ram_fstat(OBJ(h), st);
}

long vfs_truncate(long h, uint64_t size)
{
    return IS_SHARE(h) ? share_truncate(OBJ(h), size) : ram_truncate(OBJ(h), size);
}

long vfs_fsync(long h)
{
    return IS_SHARE(h) ? share_fsync(OBJ(h)) : 0;
}

long vfs_getdents(long h, uint64_t buf, uint64_t n)
{
    return IS_SHARE(h) ? share_getdents(OBJ(h), buf, n) : ram_getdents(OBJ(h), buf, n);
}

/* ---- Calls that take a canonical path ---------------------------------- */

long vfs_stat(const char *p, uint64_t st, int follow)
{
    return share_has(p) ? share_stat(p, st, follow) : ram_stat(p, st, follow);
}

long vfs_statfs(const char *p, uint64_t out)
{
    return share_has(p) ? share_statfs(p, out) : ram_statfs(out);
}

long vfs_chmod(const char *p, uint32_t mode)
{
    return share_has(p) ? share_chmod(p, mode) : ram_chmod(p, mode);
}

long vfs_utimens(const char *p, uint64_t times, int follow)
{
    return share_has(p) ? share_utimens(p, times, follow) : ram_utimens(p, times, follow);
}

long vfs_access(const char *p)
{
    return share_has(p) ? share_access(p) : ram_access(p);
}

long vfs_mkdir(const char *p, uint32_t mode)
{
    if (!strcmp(p, "/host") && share_has(p))
        return -17;                             /* EEXIST: this is the share's root */
    return share_has(p) ? share_mkdir(p, mode) : ram_mkdir(p, mode);
}

long vfs_unlink(const char *p, long flags)
{
    return share_has(p) ? share_unlink(p, flags) : ram_unlink(p, flags);
}

long vfs_rename(const char *from, const char *to)
{
    if (share_has(from) != share_has(to))
        return -18;                             /* EXDEV: across file systems */
    return share_has(from) ? share_rename(from, to) : ram_rename(from, to);
}

long vfs_readlink(const char *p, uint64_t buf, uint64_t n)
{
    return share_has(p) ? share_readlink(p, buf, n) : ram_readlink(p, buf, n);
}

long vfs_symlink(const char *target, const char *p)
{
    return share_has(p) ? -1 : ram_symlink(target, p);         /* EPERM: no links on the share */
}

long vfs_getxattr(const char *p, const char *name, uint64_t buf, uint64_t n, int follow)
{
    return share_has(p) ? share_getxattr(p, name, buf, n, follow) : ram_getxattr(p, name, buf, n, follow);
}

long vfs_setxattr(const char *p, const char *name, uint64_t val, uint64_t n, long flags, int follow)
{
    return share_has(p) ? share_setxattr(p, name, val, n, flags, follow)
                        : ram_setxattr(p, name, val, n, flags, follow);
}

/* Read from a file into the HAL's own memory. The loader uses this to read
 * program headers. */
long vfs_pread_hal(long h, void *buf, uint64_t n, uint64_t off)
{
    return IS_SHARE(h) ? share_pread_hal(OBJ(h), buf, n, off) : ram_pread_hal(OBJ(h), buf, n, off);
}
