/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* printerfs.c -- "printer:", where a print job goes.
 *
 * RISC OS applications print by opening a file and writing the job to it.
 * The file they open is `printer:` unless the user says otherwise (Draw's
 * `<Draw$PrintFile>`).  On RISC OS that is a DeviceFS device.  The box has
 * no DeviceFS, and FileSwitch is its own (modules/fileswitch). So here it
 * is a small filing system.  Everything written to it is one job, and
 * closing it sends the job to the destination.
 *
 * What a job is depends on who wrote it.  From the printer driver
 * (pdriver.c) it is a PDF, so the destination file is typed &ADF and a
 * double-click opens it in !PDF.  `*Copy file printer:` works too, and
 * sends whatever the file held.
 *
 * The destination (dest.c) is a file or a printer.  A file is either the
 * one `*PrintTo file` named, or a dated name in `Printer$Out`, which is
 * `HostFS:$.PrintOut` unless it is set.  A printer on the network is sent
 * the job over IPP, with the same two calls.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fileswitch.h"
#include "pdriver.h"

#define FS_NUMBER  200u                 /* ours; Global/FSNumbers allocates to 195 */
#define ERR(code)  (0x10000u | FS_NUMBER << 8 | (code))

#define MAX_JOBS 4

struct job_file {
    int used;
    int fd;
    char host[1024];                    /* where it is being written */
    char shown[256];                    /* what to call it in a message */
    uint32_t extent;
};

static struct job_file files[MAX_JOBS];

static os_error *refuse(const char *what)
{
    return ros_error(ERR(0xBDu), "%s", what);
}

/* Nothing is ever there: an open for output creates, which is what a job
 * is.  The root is a directory so that "printer:" can be set as a path and
 * catalogued without an error, but opening it for output is opening a job
 * (fsw's open with write), which is what RISC OS's device does. */
static os_error *pfs_stat(const char *disc, const char *path, struct fs_info *info)
{
    (void)disc;
    memset(info, 0, sizeof *info);
    info->type = OBJ_NOTHING;
    info->attr = ATTR_W | ATTR_PW;
    (void)path;
    return NULL;
}

static const char *boot_disc(void) { return ""; }
static int no_disc(const char *disc) { (void)disc; return 0; }

static int pfs_readdir(const char *disc, const char *path, uint32_t index,
                       struct fs_entry *e, os_error **err)
{
    (void)disc, (void)path, (void)index, (void)e, (void)err;
    return 0;                           /* a job queue is not a directory */
}

static os_error *pfs_create(const char *disc, const char *path, uint32_t load,
                            uint32_t exec, uint32_t length)
{
    (void)disc, (void)path, (void)load, (void)exec, (void)length;
    return NULL;                        /* the job is made when it is opened */
}

static os_error *pfs_open(const char *disc, const char *path, int write, uint32_t *handle)
{
    (void)disc;
    if (!write)
        return refuse("There is nothing to read from printer:");
    int i = 0;
    while (i < MAX_JOBS && files[i].used)
        i++;
    if (i == MAX_JOBS)
        return refuse("Too many print jobs at once");
    struct job_file *f = &files[i];
    memset(f, 0, sizeof *f);
    os_error *e = pdriver_dest_open(path, f->host, sizeof f->host, f->shown, sizeof f->shown);
    if (e)
        return e;
    f->fd = open(f->host, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f->fd < 0)
        return ros_error(ERR(0xBDu), "The print job could not be started: %s", strerror(errno));
    f->used = 1;
    *handle = (uint32_t)(i + 1);
    return NULL;
}

static struct job_file *of(uint32_t h)
{
    return h >= 1 && h <= MAX_JOBS && files[h - 1].used ? &files[h - 1] : NULL;
}

static os_error *pfs_read(uint32_t h, uint32_t pos, void *buf, uint32_t n, uint32_t *got)
{
    (void)h, (void)pos, (void)buf, (void)n;
    *got = 0;
    return refuse("There is nothing to read from printer:");
}

static os_error *pfs_write(uint32_t h, uint32_t pos, const void *buf, uint32_t n)
{
    struct job_file *f = of(h);
    if (!f)
        return refuse("That print job is not open");
    const char *p = buf;
    while (n) {
        ssize_t w = pwrite(f->fd, p, n, (off_t)pos);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return ros_error(ERR(0xBDu), "The print job could not be written: %s", strerror(errno));
        }
        p += w, n -= (uint32_t)w, pos += (uint32_t)w;
    }
    if (pos > f->extent)
        f->extent = pos;
    return NULL;
}

static os_error *pfs_set_extent(uint32_t h, uint32_t extent)
{
    struct job_file *f = of(h);
    if (!f)
        return refuse("That print job is not open");
    if (ftruncate(f->fd, (off_t)extent) < 0)
        return ros_error(ERR(0xBDu), "The print job could not be written: %s", strerror(errno));
    f->extent = extent;
    return NULL;
}

static uint32_t pfs_extent(uint32_t h)
{
    struct job_file *f = of(h);
    return f ? f->extent : 0;
}

static os_error *pfs_close(uint32_t h)
{
    struct job_file *f = of(h);
    if (!f)
        return refuse("That print job is not open");
    close(f->fd);
    f->used = 0;
    return pdriver_dest_done(f->host, f->shown, f->extent);
}

static os_error *pfs_mkdir(const char *disc, const char *path)
{
    (void)disc, (void)path;
    return refuse("printer: has no directories");
}

static os_error *pfs_remove(const char *disc, const char *path)
{
    (void)disc, (void)path;
    return refuse("A print job cannot be deleted");
}

static os_error *pfs_setinfo(const char *disc, const char *path, int reason, uint32_t load,
                             uint32_t exec, uint32_t attr)
{
    (void)disc, (void)path, (void)reason, (void)load, (void)exec, (void)attr;
    return NULL;                        /* a job's type and date are the driver's */
}

static os_error *pfs_rename(const char *disc, const char *from, const char *to)
{
    (void)disc, (void)from, (void)to;
    return refuse("A print job cannot be renamed");
}

static os_error *pfs_free(const char *disc, uint64_t *free_bytes, uint64_t *biggest, uint64_t *size)
{
    (void)disc;
    *free_bytes = *biggest = *size = 0;
    return NULL;
}

const struct fs ros_printerfs = {
    .name = "Printer",
    .number = FS_NUMBER,
    .has_discs = 0,
    .read_only = 0,
    .end_when_none = 1,
    .boot_disc = boot_disc,
    .disc_exists = no_disc,
    .stat = pfs_stat,
    .open = pfs_open,
    .read = pfs_read,
    .write = pfs_write,
    .set_extent = pfs_set_extent,
    .extent = pfs_extent,
    .close = pfs_close,
    .readdir = pfs_readdir,
    .create = pfs_create,
    .mkdir = pfs_mkdir,
    .remove = pfs_remove,
    .setinfo = pfs_setinfo,
    .rename = pfs_rename,
    .free_space = pfs_free,
};
