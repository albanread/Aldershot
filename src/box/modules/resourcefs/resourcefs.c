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
 * This file is a reimplementation in C of RISC OS Open's ResourceFS
 * (Sources/FileSys/ResourceFS/ResourceFS: s.ResourceFS, s.MsgCode, hdr.ResourceFS).
 */

/* resourcefs.c: ResourceFS and Messages, which hold the ROM's files in
 * memory.
 *
 * RISC OS's ResourceFS (FileSys/ResourceFS) is a filing system over files
 * that modules register in memory. FileSwitch reaches it as "Resources:".
 * This file provides these parts.
 *
 *   - ResourceFS_RegisterFiles and DeregisterFiles. They take the kernel's
 *     block format, which tools/mkresources.py describes, and they give the
 *     kernel's errors. The module issues Service_ResourceFSStarting when it
 *     starts, so that modules register their files. It issues
 *     Service_ResourceFSStarted after a change, on a callback. It issues
 *     Service_ResourceFSDying when it goes. It issues UpCall_ModifyingFile
 *     for each set of files registered (a create) or deregistered (a
 *     delete), as RISC OS's modifyingfiles does. The Filer's viewers then
 *     look again.
 *   - ros_resourcefs_find, the lookup that MessageTrans needs. It returns
 *     the word before a file's data where the data lies in memory.
 *     FileSwitch's ReadFSHandle gives the data. Path variables are
 *     followed as FileSwitch follows them. Names match without regard to
 *     case. A file registered later hides one of the same name. The
 *     exception is the files of an ARM module that shadows a native one.
 *     Those go under every other file.
 *   - ros_resourcefs_fs, the filing system that FileSwitch
 *     (modules/fileswitch) calls. It is read-only. Every write is refused
 *     as ResourceFS refuses it, with "The filing system Resources: is read
 *     only" (&113). A delete is refused even when there is nothing to
 *     delete. Its directories are the ones that the names imply. A
 *     directory is dated as its first file is. Its length and attributes
 *     are 0, but when it is listed it shows that file's length and
 *     attributes too. The root, $, has 0 and attributes R L r. A file's
 *     handle is the address of its data, and the length word is before it.
 *     FSControl 21 gives the handle, and ITable reads its table there.
 *
 * Messages, as RISC OS's Messages module does, registers the ROM's own
 * files with ResourceFS. The files are in resources/, packed by
 * tools/mkresources.py.
 *
 * All state is in the RMA. It is a list of registered blocks, newest first.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fileswitch.h"
#include "resourcefs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"

#define SERVICE_RESOURCEFS_STARTED  0x59u
#define SERVICE_RESOURCEFS_DYING    0x5Au
#define SERVICE_RESOURCEFS_STARTING 0x60u

#define ERR_RFS_REG  0x12E00u
#define ERR_RFS_DREG 0x12E01u

#define UPCALL_MODIFYING_FILE 3u
#define UPFS_DELETE 6u                  /* hdr/UpCall's upfsfile_ reasons */
#define UPFS_CREATE 7u
#define RFS_INFO_WORD 0x0001002Eu       /* ResourceFS's fsinfoword: fsnumber_resourcefs, read only */

/* The workspace. Each link in the list of registered blocks is two words in
 * the RMA. Word [0] is the next link and word [4] is the registered block. */
struct workspace {
    uint32_t links;                 /* The first link, or 0 */
    uint32_t started_pending;       /* A Service_ResourceFSStarted callback is queued */
    uint32_t svc_register;          /* The entry Service_ResourceFSStarting offers */
};

static struct workspace *ws(void)
{
    uint32_t w = resourcefs_module.private_word ? ros_ld32(resourcefs_module.private_word) : 0;
    return w ? ros_ptr(w) : NULL;
}

/* ---- registration ------------------------------------------------------------ */

static uint32_t find_link(uint32_t files, uint32_t *prev)
{
    struct workspace *w = ws();
    uint32_t before = 0;
    for (uint32_t l = w ? w->links : 0; l; before = l, l = ros_ld32(l))
        if (ros_ld32(l + 4) == files) {
            if (prev)
                *prev = before;
            return l;
        }
    return 0;
}

static void issue_started(void *unused)
{
    (void)unused;
    struct workspace *w = ws();
    if (w)
        w->started_pending = 0;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_RESOURCEFS_STARTED;
    ros_service_call(&s);
}

/* The kernel's Issue_Service_ResourceFSStarted. It is issued once, on a
 * callback. */
static void started_later(void)
{
    struct workspace *w = ws();
    if (w && !w->started_pending) {
        w->started_pending = 1;
        ros_callback_add_native(issue_started, NULL);
    }
}

/* modifyingfiles: issue UpCall_ModifyingFile for "". That means every
 * directory, because ResourceFS cannot say which directories the files make
 * or unmake. R9 is the reason, R8 is the information word and R6 is 0 (no
 * special field). The Filer's viewers of Resources: then look again. Errors
 * are ignored, as RISC OS ignores them (CLRV). */
static void modifying_files(uint32_t reason)
{
    static uint32_t null;                 /* An arena copy of "" */
    if (!null)
        null = ros_addr(ros_rma_alloc(4));
    if (!null)
        return;
    ros_st32(null, 0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = UPCALL_MODIFYING_FILE, s.r[1] = null, s.r[6] = 0, s.r[8] = RFS_INFO_WORD;
    s.r[9] = reason;
    ros_swi(&s, XOS_UpCall);
}

static os_error *register_files(uint32_t files)
{
    struct workspace *w = ws();
    if (!w)
        return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module ResourceFS");
    if (find_link(files, NULL))
        return ros_error(ERR_RFS_REG, "ResourceFS files already registered");
    uint32_t *l = ros_rma_alloc(8);
    if (!l)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    l[1] = files;
    if (ros_module_shadow_at(files)) {
        /* The files of an ARM module that shadows a native one go at the
         * old end of the list. Names that are already registered keep their
         * files, including the native twin's Messages. The shadow's new
         * names are added. Its block is in its image, so the address says
         * whose it is. */
        uint32_t *p = &w->links;
        while (*p)
            p = (uint32_t *)ros_ptr(*p);
        l[0] = 0;
        *p = ros_addr(l);
    } else {
        l[0] = w->links;
        w->links = ros_addr(l);
    }
    modifying_files(UPFS_CREATE);
    return NULL;
}

os_error *xresourcefs_register_files(void *files)
{
    os_error *e = register_files(ros_addr(files));
    if (!e)
        started_later();
    return e;
}

os_error *xresourcefs_deregister_files(void *files)
{
    uint32_t prev, l = find_link(ros_addr(files), &prev);
    if (!l)
        return ros_error(ERR_RFS_DREG, "ResourceFS files not registered");
    modifying_files(UPFS_DELETE);       /* The files are going */
    if (prev)
        ros_st32(prev, ros_ld32(l));
    else
        ws()->links = ros_ld32(l);
    ros_rma_free(ros_ptr(l));
    started_later();
    return NULL;
}

/* The routine that Service_ResourceFSStarting offers in R2. It registers
 * the files in R0. R3 is the workspace. */
static void svc_register(struct ros_cpu *s)
{
    os_error *e = register_files(s->r[0]);
    if (e) {
        s->v = 1;
        s->r[0] = ros_addr(e);
    } else {
        s->v = 0;
    }
    s->r[15] = s->r[14];
}

/* ---- finding a file ----------------------------------------------------------- */

/* Find the registered file named path ("Resources.X.Messages", counted
 * from $), looking at the newest registration first. Returns the address of
 * the file's length word, or 0. */
static uint32_t find_file(const char *path, size_t n)
{
    struct workspace *w = ws();
    for (uint32_t l = w ? w->links : 0; l; l = ros_ld32(l)) {
        uint32_t e = ros_ld32(l + 4);
        for (uint32_t next; (next = ros_ld32(e)) != 0; e += next) {
            const char *name = ros_ptr(e + 20);
            if (strlen(name) == n && strncasecmp(name, path, n) == 0)
                return (e + 20 + (uint32_t)n + 1 + 3) & ~3u;
        }
    }
    return 0;
}

static uint32_t find_path(const char *name, int depth)
{
    const char *colon = strchr(name, ':');
    if (!colon || depth > 8)
        return 0;
    size_t plen = (size_t)(colon - name);
    const char *rest = colon + 1;
    size_t n = 0;
    while ((uint8_t)rest[n] > ' ')
        n++;
    if (plen == 9 && strncasecmp(name, "Resources", 9) == 0) {
        if (n >= 2 && rest[0] == '$' && rest[1] == '.')
            rest += 2, n -= 2;
        return find_file(rest, n);
    }
    /* A path variable. Try each of its places, in order. */
    char var[80], value[256];
    if (plen + 6 > sizeof var)
        return 0;
    memcpy(var, name, plen);
    memcpy(var + plen, "$Path", 6);
    uint32_t buf = ros_addr(ros_rma_alloc(sizeof var + sizeof value));
    if (!buf)
        return 0;
    strcpy(ros_ptr(buf), var);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = buf, c.r[1] = buf + sizeof var, c.r[2] = sizeof value - 1, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    int ok = !c.v;
    if (ok) {
        memcpy(value, ros_ptr(buf + sizeof var), c.r[2]);
        value[c.r[2]] = 0;
    }
    ros_rma_free(ros_ptr(buf));
    if (!ok)
        return 0;
    for (char *p = value; *p;) {
        while (*p == ' ')
            p++;
        char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char candidate[512];
        if (len + n + 1 <= sizeof candidate) {
            memcpy(candidate, p, len);
            memcpy(candidate + len, rest, n);
            candidate[len + n] = 0;
            uint32_t f = find_path(candidate, depth + 1);
            if (f)
                return f;
        }
        if (!end)
            break;
        p = end + 1;
    }
    return 0;
}

uint32_t ros_resourcefs_find(const char *name)
{
    return find_path(name, 0);
}

/* ---- the filing system ------------------------------------------------------------ */

#define FS_NUMBER 0x2Eu

/* ErrorBlock_FilingSystemReadOnly, as ResourceFS gives it. */
static os_error *read_only(void)
{
    return ros_error(0x113, "The filing system Resources: is read only");
}

/* Loop over each registered file, newest first. The variable e is the
 * address of the file's entry. */
#define EACH_FILE(e)                                                                    \
    for (uint32_t l_ = ws() ? ws()->links : 0; l_; l_ = ros_ld32(l_))                   \
        for (uint32_t e = ros_ld32(l_ + 4), n_; (n_ = ros_ld32(e)) != 0; e += n_)

static void file_info(uint32_t e, struct fs_info *info)
{
    info->type = OBJ_FILE;
    info->load = ros_ld32(e + 4);
    info->exec = ros_ld32(e + 8);
    info->length = ros_ld32(e + 12);
    info->attr = ros_ld32(e + 16);
}

/* A directory, as RISC OS's ResourceFS answers for one (s/ResourceFS,
 * FindFileOrDirectory). The names imply the directory. It takes the load
 * and exec addresses of the first file found in it, and its length and
 * attributes are 0. For example, the farm's Apps viewer dates !Chars as
 * its !Help. The root, $, has load and exec addresses of 0, and its
 * attributes are read, locked and public read. When a directory is listed,
 * it also takes that file's length and attributes (see rfs_readdir). */
static void dir_info(struct fs_info *info, uint32_t first)
{
    info->type = OBJ_DIR;
    info->load = first ? ros_ld32(first + 4) : 0;
    info->exec = first ? ros_ld32(first + 8) : 0;
    info->length = 0;
    info->attr = first ? 0 : ATTR_R | ATTR_L | ATTR_PR;
}

static int no_disc(const char *disc)
{
    return disc[0] == 0;
}

static const char *rfs_boot_disc(void)
{
    return "";
}

static os_error *rfs_stat(const char *disc, const char *path, struct fs_info *info)
{
    (void)disc;
    size_t n = strlen(path);
    if (n == 0) {
        dir_info(info, 0);
        return NULL;
    }
    uint32_t f = find_file(path, n);
    if (f) {
        /* find_file answers with the length word address. The entry is before it. */
        EACH_FILE(e) {
            if ((e + 20 + (uint32_t)n + 1 + 3 & ~3u) == f) {
                file_info(e, info);
                return NULL;
            }
        }
    }
    EACH_FILE(e) {
        const char *name = ros_ptr(e + 20);
        if (strncasecmp(name, path, n) == 0 && name[n] == '.') {
            dir_info(info, e);
            return NULL;
        }
    }
    info->type = OBJ_NOTHING;
    return NULL;
}

static os_error *rfs_open(const char *disc, const char *path, int write, uint32_t *handle)
{
    (void)disc;
    if (write)
        return read_only();
    uint32_t f = find_file(path, strlen(path));
    if (!f)
        return ros_error(0x10000u | FS_NUMBER << 8 | 0xD6u, "Not found");
    *handle = f + 4;                    /* The data, as ResourceFS's handles are */
    return NULL;
}

static uint32_t rfs_extent(uint32_t h)
{
    return ros_ld32(h - 4) - 4;
}

static os_error *rfs_read(uint32_t h, uint32_t pos, void *buf, uint32_t n, uint32_t *got)
{
    uint32_t ext = rfs_extent(h);
    uint32_t k = pos >= ext ? 0 : ext - pos < n ? ext - pos : n;
    memcpy(buf, ros_ptr(h + pos), k);
    *got = k;
    return NULL;
}

static os_error *rfs_write(uint32_t h, uint32_t pos, const void *buf, uint32_t n)
{
    (void)h, (void)pos, (void)buf, (void)n;
    return read_only();
}

static os_error *rfs_set_extent(uint32_t h, uint32_t extent)
{
    (void)h, (void)extent;
    return read_only();
}

static os_error *rfs_close(uint32_t h)
{
    (void)h;
    return NULL;
}

/* Compare two leaf names without regard to case. This gives the order in
 * which a directory's children are listed. A directory's entries are the
 * leaves of the names under it, each listed once. */
static int child_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    for (size_t i = 0; i < an && i < bn; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'a' && ca <= 'z')
            ca -= 32;
        if (cb >= 'a' && cb <= 'z')
            cb -= 32;
        if (ca != cb)
            return ca - cb;
    }
    return (int)an - (int)bn;
}

static int rfs_readdir(const char *disc, const char *path, uint32_t index, struct fs_entry *out,
                       os_error **err)
{
    (void)disc;
    *err = NULL;
    size_t n = strlen(path);
    /* Find the index-th smallest distinct leaf. Walk up from below, one leaf
     * at a time. */
    const char *prev = NULL;
    size_t prevn = 0;
    for (uint32_t i = 0;; i++) {
        const char *best = NULL;
        size_t bestn = 0;
        uint32_t best_e = 0;
        EACH_FILE(e) {
            const char *name = ros_ptr(e + 20);
            if (n && (strncasecmp(name, path, n) != 0 || name[n] != '.'))
                continue;
            const char *leaf = n ? name + n + 1 : name;
            const char *dot = strchr(leaf, '.');
            size_t ln = dot ? (size_t)(dot - leaf) : strlen(leaf);
            if (prev && child_cmp(leaf, ln, prev, prevn) <= 0)
                continue;
            if (!best || child_cmp(leaf, ln, best, bestn) < 0)
                best = leaf, bestn = ln, best_e = e;
        }
        if (!best)
            return 0;
        if (i == index) {
            snprintf(out->name, sizeof out->name, "%.*s", (int)bestn, best);
            /* Use the entry that the name was found in (FindDirEntry's R2).
             * For a file it is the file's own. For a directory it is the
             * first file found in it. Load, exec, length and attributes are
             * all copied, as readdirentriesinfo copies them. */
            file_info(best_e, &out->info);
            if (best[bestn] == '.')
                out->info.type = OBJ_DIR;
            return 1;
        }
        prev = best, prevn = bestn;
    }
}

static os_error *rfs_create(const char *disc, const char *path, uint32_t load, uint32_t exec,
                            uint32_t length)
{
    (void)disc, (void)path, (void)load, (void)exec, (void)length;
    return read_only();
}

static os_error *rfs_mkdir(const char *disc, const char *path)
{
    (void)disc, (void)path;
    return read_only();
}

static os_error *rfs_remove(const char *disc, const char *path)
{
    (void)disc, (void)path;
    return read_only();
}

static os_error *rfs_setinfo(const char *disc, const char *path, int reason, uint32_t load,
                             uint32_t exec, uint32_t attr)
{
    (void)disc, (void)path, (void)reason, (void)load, (void)exec, (void)attr;
    return read_only();
}

static os_error *rfs_rename(const char *disc, const char *from, const char *to)
{
    (void)disc, (void)from, (void)to;
    return read_only();
}

static os_error *rfs_free(const char *disc, uint64_t *free_bytes, uint64_t *biggest,
                          uint64_t *size)
{
    (void)disc;
    uint64_t total = 0;
    EACH_FILE(e) {
        total += ros_ld32(e + 12);
    }
    *free_bytes = *biggest = 0;
    *size = total;
    return NULL;
}

const struct fs ros_resourcefs_fs = {
    .name = "Resources",
    .number = FS_NUMBER,
    .has_discs = 0,
    .read_only = 1,
    .end_when_none = 1,
    .boot_disc = rfs_boot_disc,
    .disc_exists = no_disc,
    .stat = rfs_stat,
    .open = rfs_open,
    .read = rfs_read,
    .write = rfs_write,
    .set_extent = rfs_set_extent,
    .extent = rfs_extent,
    .close = rfs_close,
    .readdir = rfs_readdir,
    .create = rfs_create,
    .mkdir = rfs_mkdir,
    .remove = rfs_remove,
    .setinfo = rfs_setinfo,
    .rename = rfs_rename,
    .free_space = rfs_free,
};

/* ---- the module ------------------------------------------------------------------ */

static void starting(void *unused)
{
    (void)unused;
    struct workspace *w = ws();
    if (!w)
        return;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_RESOURCEFS_STARTING;
    s.r[2] = w->svc_register;
    s.r[3] = ros_addr(w);
    ros_service_call(&s);
    started_later();
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    static uint32_t entry;
    if (!entry)
        entry = ros_native_entry(svc_register, "ResourceFS:Register");
    w->svc_register = entry;
    ros_st32(m->private_word, ros_addr(w));
    ros_callback_add_native(starting, NULL);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    for (uint32_t l = w->links, next; l; l = next) {
        next = ros_ld32(l);
        w->links = next;
        modifying_files(UPFS_DELETE);   /* Tell the Filer, a block at a time */
        ros_rma_free(ros_ptr(l));
    }
    struct ros_cpu s;                   /* Then tell the others that Resources: is disappearing */
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_RESOURCEFS_DYING;
    ros_service_call(&s);
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module resourcefs_module = {
    .title = "ResourceFS",
    .help = "ResourceFS\t0.26 (25 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x41B40,
    .swi_thunks = ros_swi_thunks_ResourceFS,
    .swi_names = ros_swi_names_ResourceFS,
    .swi_prefix = "ResourceFS",
};

/* ---- Messages: the ROM's own files ------------------------------------------------ */

extern const uint8_t ros_rom_resources[];
extern const uint32_t ros_rom_resources_size;

static uint32_t rom_files(void)
{
    uint32_t w = messages_module.private_word ? ros_ld32(messages_module.private_word) : 0;
    return w;
}

static os_error *messages_init(struct ros_module *m, const char *tail)
{
    (void)tail;
    uint8_t *block = ros_rma_alloc(ros_rom_resources_size);
    if (!block)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(block, ros_rom_resources, ros_rom_resources_size);
    ros_st32(m->private_word, ros_addr(block));
    if (ws())
        xresourcefs_register_files(block);  /* ResourceFS is already running, so register now */
    return NULL;
}

static os_error *messages_final(struct ros_module *m, int fatal)
{
    (void)fatal;
    uint32_t block = rom_files();
    if (ws() && find_link(block, NULL))
        xresourcefs_deregister_files(ros_ptr(block));
    ros_rma_free(ros_ptr(block));
    ros_st32(m->private_word, 0);
    return NULL;
}

/* Service_ResourceFSStarting. Register through the routine that the service
 * offers. */
static void messages_service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    uint32_t block = rom_files();
    if (s->r[1] != SERVICE_RESOURCEFS_STARTING || !block || find_link(block, NULL))
        return;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = block, c.r[3] = s->r[3];
    ros_call(&c, s->r[2]);
}

struct ros_module messages_module = {
    .title = "Messages",
    .help = "Messages\t1.12 (25 Sep 2026) ROSGD's own files",
    .init = messages_init,
    .final = messages_final,
    .service = messages_service,
};

__attribute__((constructor)) static void count(void)
{
    resourcefs_module.swi_count = ros_swi_count_ResourceFS;
}
