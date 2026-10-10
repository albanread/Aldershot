/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* fileswitch.h -- FileSwitch, reimplemented, and the filing systems under it.
 *
 * RISC OS's FileSwitch (FileSys/FileSwitch) turns names into objects on a
 * filing system and runs streams over them.  The filing systems answer
 * FileSwitch's entry points.  This FileSwitch is native, and so are its two
 * filing systems.  HostFS works over Linux's files, with one disc per
 * mount.  ResourceFS works over files in memory.  So the entry points are
 * this C interface, not FileSwitch's register-level one.
 *
 * Paths reach a filing system already resolved: a disc name (HostFS) and
 * the path from $, "a.b.c", or "" for $ itself.
 */
#ifndef ROSGD_FILESWITCH_H
#define ROSGD_FILESWITCH_H

#include <stdint.h>

#include "rosgd/error.h"

#define OBJ_NOTHING 0u
#define OBJ_FILE 1u
#define OBJ_DIR 2u
#define OBJ_IMAGE 3u                /* a file an image filing system claims: both */

/* Attribute bits (OS_File 4) */
#define ATTR_R 0x01u                /* owner read */
#define ATTR_W 0x02u                /* owner write */
#define ATTR_L 0x08u                /* locked */
#define ATTR_PR 0x10u               /* public read */
#define ATTR_PW 0x20u               /* public write */

struct fs_info {
    uint32_t type;                  /* OBJ_ */
    uint32_t load, exec, length, attr;
};

struct fs_entry {
    char name[256];                 /* the leafname, as RISC OS spells it */
    struct fs_info info;
};

struct mfs;                         /* a filing system a module added (modfs.c) */

struct fs {
    const char *name;
    uint32_t number;
    int has_discs;                  /* names carry ":disc." */
    int read_only;                  /* fsinfo_readonly: it refuses every write itself, of
                                       nothing too (OS_File 6 on a name not there) */
    uint32_t info;                  /* the rest of its information word (FileSwitch's
                                       hdr/LowFSI): open files in bits 8-15, the fsinfo_
                                       flags above; the number and fsinfo_readonly are
                                       the two fields before */

    const char *(*boot_disc)(void);
    int (*disc_exists)(const char *disc);
    os_error *(*stat)(const char *disc, const char *path, struct fs_info *info);
    /* Open an existing file: for update if write, else for reading.  The
     * handle is the filing system's own. */
    os_error *(*open)(const char *disc, const char *path, int write, uint32_t *handle);
    os_error *(*read)(uint32_t h, uint32_t pos, void *buf, uint32_t n, uint32_t *got);
    os_error *(*write)(uint32_t h, uint32_t pos, const void *buf, uint32_t n);
    os_error *(*set_extent)(uint32_t h, uint32_t extent);
    uint32_t (*extent)(uint32_t h);
    os_error *(*close)(uint32_t h);
    /* OS_GBPB 9-12's R4 after a call that read the directory's last entries.
     * HostFS gives 0 and -1 at once, as the team's does on RISC OS 5.30.
     * ResourceFS gives 1 and the next index, and -1 only from a call that
     * read none.  That is s/ResourceFS's ReadDirEntries ("MOVLE R4, #-1 ; no
     * files read => all done").  The Desktop's PumpBootROMApps relies on it.
     * It reads Resources:$.Apps one entry at a time and stops at -1, before
     * it boots the entry read with it. */
    int end_when_none;
    /* The index-th entry of a directory, in the order RISC OS lists it
     * (upper-case folded); 1 found, 0 past the end. */
    int (*readdir)(const char *disc, const char *path, uint32_t index, struct fs_entry *e,
                   os_error **err);
    os_error *(*create)(const char *disc, const char *path, uint32_t load, uint32_t exec,
                        uint32_t length);
    os_error *(*mkdir)(const char *disc, const char *path);
    os_error *(*remove)(const char *disc, const char *path);
    /* Reason as OS_File 1-4: 1 all, 2 load, 3 exec, 4 attributes */
    os_error *(*setinfo)(const char *disc, const char *path, int reason, uint32_t load,
                         uint32_t exec, uint32_t attr);
    os_error *(*rename)(const char *disc, const char *from, const char *to);
    os_error *(*free_space)(const char *disc, uint64_t *free_bytes, uint64_t *biggest,
                            uint64_t *size);
    /* A module's filing system (OS_FSControl 12): the calls above are its
     * entries', through modfs.c, and these pointers 0 */
    struct mfs *mfs;
};

extern const struct fs ros_hostfs;
extern const struct fs ros_resourcefs_fs;
/* LanMan (modules/lanmanfs): HostFS's discs by LanManFS's name and number,
 * so LanMan::Share.$ reaches a connected SMB share */
extern struct fs ros_lanmanfs;

/* A HostFS disc: name, and the Linux directory it is.  The first is the
 * boot disc, what "HostFS:$" means. */
int ros_hostfs_mount(const char *name, const char *root);
int ros_hostfs_unmount(const char *name);
/* The index-th HostFS disc's name, in the order they were mounted; NULL
 * past the last (HostFSFiler puts one icon on the icon bar for each) */
const char *ros_hostfs_disc(unsigned index);

/* A HostFS disc whose files are not Linux's: the calls HostFS makes of
 * them, in place of the POSIX ones, for LanManFS's SMB shares
 * (modules/lanmanfs, over libsmb2).  HostFS still maps names, types, dates
 * and attributes as it does for any disc.  Paths are relative to the
 * disc's root, "/"-separated, "" for the root itself.  Each call returns 0
 * (or a count, or a handle) on success and -errno on failure.  There are no
 * links and no extended attributes: the attributes HostFS keeps are
 * derived from st_mode, and a file without S_IWUSR is read-only. */
struct stat;
struct ros_hostio {
    int (*stat)(void *ctx, const char *path, struct stat *st);
    int (*open)(void *ctx, const char *path, int flags);   /* O_RDONLY, O_RDWR, O_WRONLY|O_CREAT|O_TRUNC */
    int64_t (*pread)(void *ctx, int h, void *buf, uint32_t n, uint64_t pos);
    int64_t (*pwrite)(void *ctx, int h, const void *buf, uint32_t n, uint64_t pos);
    int (*ftruncate)(void *ctx, int h, uint64_t length);
    int (*fstat)(void *ctx, int h, struct stat *st);
    int (*close)(void *ctx, int h);
    /* a listing: opendir's result (NULL and *err set when it fails) passed
     * to readdir, which gives each entry's name and its stat, then NULL */
    void *(*opendir)(void *ctx, const char *path, int *err);
    const char *(*readdir)(void *ctx, void *dir, struct stat *st);
    void (*closedir)(void *ctx, void *dir);
    int (*mkdir)(void *ctx, const char *path);
    int (*rmdir)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
    int (*rename)(void *ctx, const char *from, const char *to);
    /* the modification time, mtime[0] seconds since 1970 and mtime[1]
     * nanoseconds, and whether the file is read-only: either may be left
     * alone (mtime NULL, readonly -1) */
    int (*setinfo)(void *ctx, const char *path, const int64_t *mtime, int readonly);
    int (*free_space)(void *ctx, uint64_t *free_bytes, uint64_t *size);
};
/* Mounts such a disc; ros_hostfs_unmount removes it.  -1 when HostFS has
 * no room for another disc. */
int ros_hostfs_mount_io(const char *name, const struct ros_hostio *io, void *ctx);

/* The Linux path a RISC OS name resolves to, on HostFS (or LanMan) */
os_error *ros_hostfs_linux_path(const char *name, char *out, size_t max);
/* HostFS's entry for the Free module's windows (Free_Register), or, with
 * deregister, not: nothing when there is no Free.  HostFS starts, in
 * FileSwitch, before Free, so FileSwitch registers it once the ROM's
 * modules have all started (Service_PostInit), and as it starts later. */
void ros_hostfs_free_register(int deregister);

/* The module, for the ROM's list. */
extern struct ros_module fileswitch_module;

#endif
