/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* lanmanfs.c -- LanManFS, as a native module: SMB2 and SMB3 shares through
 * libsmb2.
 *
 * RISC OS 5's LanManFS (Networking/Omni/Protocols/OmniLanManFS, Acorn's,
 * Apache 2.0) speaks SMB1 over NetBIOS itself, with LMv2 at best. Modern
 * Samba and Windows refuse that by default. ROSGD's speaks SMB2 and SMB3,
 * with signing and encryption as the server asks, through libsmb2 (Ronnie
 * Sahlberg's, LGPL 2.1; deps/build-libsmb2.sh). It is a client in user
 * space, so it works the same over Linux, over the HAL (HybrisOS), which has
 * no SMB client in its kernel, and hosted on the Mac. It used to mount the
 * share with Linux's cifs client.
 *
 * A share is a HostFS disc whose calls are this module's (struct
 * ros_hostio): HostFS maps names, types and dates onto the share's files
 * as it does onto Linux's. The filing system name LanMan (number 102)
 * reaches the same discs, so LanMan::Name.$ paths and Obey files keep
 * working.
 *
 *   *LMConnect <name> <server> <share> [<user>|- [<password>]]
 *        the share as disc <name>; the user and password *LMLogon's if
 *        not given, a guest's if none
 *   *LMDisconnect <name>
 *   *LMLogon <workgroup> <user> [<password>], *LMLogoff
 *   *LMInfo       the connections
 *   *LanMan       LanMan the current filing system
 *
 * The server is a host name or an address, looked up as the Resolver looks
 * names up. NetBIOS names and browsing (*ListFS, *LMServer, *LMPrinters,
 * *Configure LMNameServer) are not carried, nor are OmniClient's SWIs
 * (&49240), because the protocol they rest on is SMB1's. Errors are
 * LanManFS's own, &16600 + n with its messages.
 *
 * SMB keeps no RISC OS attributes. A file is locked when the server has it
 * read-only, and locking or unlocking it sets that. A connection the server
 * has dropped (an idle one, say) is made again at the next call. Files open
 * on it are lost.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>                     /* before libsmb2's headers, which need it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

#include "fileswitch.h"
#include "lanmanfs.h"
#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define FS_NUMBER 102
#define ERROR_BASE (0x10000u | (FS_NUMBER << 8))
#define MAX_CONN 16
#define MAX_FILES 256                   /* HostFS's own limit on open files */
#define TIMEOUT 30                      /* seconds for a request to be answered */

/* LanManFS's errors (Resources/UK/Messages, E00-E39) that this module gives */
enum {
    E_INTERNAL = 0, E_BADPARAM = 1, E_NOCONN = 2, E_LINKFAILED = 4, E_LINKEXISTS = 7,
    E_TIMEOUT = 8, E_CANTFINDNAME = 10, E_NOACCESS = 20, E_BADPASSWD = 22, E_BADNAME = 23,
    E_BADDRV = 24, E_NOTIMPLEMENTED = 29, E_NOSHARE = 35,
};

static const char *const messages[] = {
    [E_INTERNAL] = "LanManFS internal error",
    [E_BADPARAM] = "Bad parameters",
    [E_NOCONN] = "No connection to server",
    [E_LINKFAILED] = "Connection to server failed",
    [E_LINKEXISTS] = "Connection already exists",
    [E_TIMEOUT] = "Timeout error",
    [E_CANTFINDNAME] = "Cannot find given server",
    [E_NOACCESS] = "Access denied",
    [E_BADPASSWD] = "Incorrect password",
    [E_BADNAME] = "Illegal name",
    [E_BADDRV] = "No such connection",
    [E_NOTIMPLEMENTED] = "Function not implemented",
    [E_NOSHARE] = "Share name does not exist",
};

static os_error *lm_error(int which)
{
    return ros_error(ERROR_BASE + (uint32_t)which, "%s", messages[which]);
}

/* ---- state ------------------------------------------------------------------- */

static struct {
    char workgroup[64], user[64], password[128];
} logon;

static struct conn {
    char name[32], server[128], share[128], user[64], password[128], domain[64];
    int used;
    struct smb2_context *smb2;
    struct smb2fh *files[MAX_FILES];    /* HostFS's handles: an index here */
} conns[MAX_CONN];

static struct conn *find(const char *name)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].used && !strcasecmp(conns[i].name, name))
            return &conns[i];
    return NULL;
}

/* ---- arguments --------------------------------------------------------------- */

/* Up to max words of a command tail, "quoted" kept whole */
static int words(uint32_t tail, char *buf, size_t size, char *argv[], int max)
{
    const char *t = ros_ptr(tail);
    int argc = 0;
    size_t n = 0;
    while ((uint8_t)*t >= ' ' && argc < max && n < size - 1) {
        while (*t == ' ')
            t++;
        if ((uint8_t)*t < ' ')
            break;
        argv[argc++] = buf + n;
        int quoted = *t == '"';
        if (quoted)
            t++;
        while ((uint8_t)*t >= ' ' && (quoted ? *t != '"' : *t != ' ') && n < size - 1)
            buf[n++] = *t++;
        if (quoted && *t == '"')
            t++;
        buf[n++] = 0;
    }
    return argc;
}

/* A name RISC OS can use as a disc: no path or wildcard characters */
static int good_name(const char *s)
{
    if (!*s || strlen(s) >= sizeof conns[0].name)
        return 0;
    for (; *s; s++)
        if (strchr(".:$&@^%\\#*\"| /", *s) || (uint8_t)*s < ' ')
            return 0;
    return 1;
}

/* ---- the connection ---------------------------------------------------------- */

/* A session and the share on it, made with the connection's details.
 * Returns 0, or -errno with the server's status in *nt */
static int open_session(struct conn *c, uint32_t *nt)
{
    struct smb2_context *s = smb2_init_context();
    if (!s)
        return -ENOMEM;
    smb2_set_timeout(s, TIMEOUT);
    smb2_set_security_mode(s, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_user(s, c->user[0] ? c->user : "guest");
    smb2_set_password(s, c->password);
    if (c->domain[0])
        smb2_set_domain(s, c->domain);
    int r;
    ROS_BLOCKING(r = smb2_connect_share(s, c->server, c->share, NULL));
    if (r < 0) {
        *nt = (uint32_t)smb2_get_nterror(s);
        smb2_destroy_context(s);
        return r;
    }
    c->smb2 = s;
    return 0;
}

static void close_session(struct conn *c)
{
    if (!c->smb2)
        return;
    for (int i = 0; i < MAX_FILES; i++)
        if (c->files[i]) {
            smb2_close(c->smb2, c->files[i]);
            c->files[i] = NULL;
        }
    smb2_disconnect_share(c->smb2);
    smb2_destroy_context(c->smb2);
    c->smb2 = NULL;
}

/* After a call has failed. This works out whether the connection was lost,
 * and makes it again if so. The call is then worth repeating. The files
 * open on it are gone. */
static int again(struct conn *c, int r)
{
    if (r >= 0 || (c->smb2 && smb2_get_fd(c->smb2) >= 0))
        return 0;
    if (c->smb2) {
        smb2_destroy_context(c->smb2);
        c->smb2 = NULL;
        memset(c->files, 0, sizeof c->files);
    }
    uint32_t nt;
    return open_session(c, &nt) == 0;
}

/* A request of libsmb2's asynchronous kind, waited for. The status is the
 * first callback's that failed, for a compound. It is -errno from libsmb2's
 * own calls, and an NT status from a raw request's. The record outlives a
 * wait that gives up, as the callback may still come. */
struct pending {
    int calls, raw, status, abandoned;
    void *data;
    struct smb2_stat_64 st;             /* fstat's answer, copied from the reply */
};

static void wait_cb(struct smb2_context *s, int status, void *data, void *priv)
{
    (void)s;
    struct pending *w = priv;
    if (status && !w->status)
        w->status = status;
    if (!w->data)
        w->data = data;
    if (--w->calls == 0 && w->abandoned)
        free(w);
}

static int wait_for(struct smb2_context *s, struct pending *w)
{
    time_t start = time(NULL);
    while (w->calls > 0) {
        struct pollfd p = { .fd = smb2_get_fd(s), .events = (short)smb2_which_events(s) };
        if (p.fd < 0 || poll(&p, 1, 1000) < 0 || time(NULL) - start > TIMEOUT + 5) {
            w->abandoned = 1;
            return -EIO;
        }
        if (smb2_service(s, p.revents) < 0) {    /* which also times requests out */
            w->abandoned = 1;
            return -EIO;
        }
    }
    return 0;
}

static struct pending *new_wait(int calls, int raw)
{
    struct pending *w = calloc(1, sizeof *w);
    if (w)
        w->calls = calls, w->raw = raw;
    return w;
}

/* The status of a finished wait, as -errno. The record is freed */
static int wait_done(struct pending *w, int r)
{
    if (r < 0)
        return r;                       /* abandoned: the callback frees it */
    int st = w->status, raw = w->raw;
    free(w);
    if (raw && st)
        return -nterror_to_errno((uint32_t)st);
    return st;
}

/* ---- the calls HostFS makes ----------------------------------------------------- */

static void to_stat(const struct smb2_stat_64 *s, struct stat *st)
{
    memset(st, 0, sizeof *st);
    int dir = s->smb2_type == SMB2_TYPE_DIRECTORY;
    st->st_mode = dir ? S_IFDIR | 0755 : S_IFREG | 0644;
    if (!dir && (s->smb2_attributes & SMB2_FILE_ATTRIBUTE_READONLY))
        st->st_mode &= ~(mode_t)0222;
    st->st_size = (off_t)s->smb2_size;
    st->st_ino = (ino_t)s->smb2_ino;
    st->st_nlink = (nlink_t)s->smb2_nlink;
#ifdef __APPLE__
    st->st_mtimespec.tv_sec = (time_t)s->smb2_mtime;
    st->st_mtimespec.tv_nsec = (long)s->smb2_mtime_nsec;
#else
    st->st_mtim.tv_sec = (time_t)s->smb2_mtime;
    st->st_mtim.tv_nsec = (long)s->smb2_mtime_nsec;
#endif
}

static struct smb2fh *file(struct conn *c, int h)
{
    return h >= 0 && h < MAX_FILES && c->smb2 ? c->files[h] : NULL;
}

static int s_stat(void *ctx, const char *path, struct stat *st)
{
    struct conn *c = ctx;
    struct smb2_stat_64 s;
    int r = c->smb2 ? smb2_stat(c->smb2, path, &s) : -ENOTCONN;
    if (r < 0 && again(c, r))
        r = smb2_stat(c->smb2, path, &s);
    if (r == 0)
        to_stat(&s, st);
    return r;
}

static int s_open(void *ctx, const char *path, int flags)
{
    struct conn *c = ctx;
    int h = 0;
    while (h < MAX_FILES && c->files[h])
        h++;
    if (h == MAX_FILES)
        return -EMFILE;
    for (int tries = 0; tries < 2; tries++) {
        struct pending *w = new_wait(1, 0);
        if (!w)
            return -ENOMEM;
        int r = c->smb2 ? smb2_open_async(c->smb2, path, flags, wait_cb, w) : -ENOTCONN;
        if (r < 0) {
            free(w);
        } else {
            r = wait_for(c->smb2, w);
            struct smb2fh *fh = r == 0 ? w->data : NULL;
            r = wait_done(w, r);
            if (r == 0 && fh) {
                c->files[h] = fh;
                return h;
            }
            if (r == 0)
                r = -EIO;
        }
        if (!again(c, r))
            return r;
    }
    return -EIO;
}

static int64_t s_pread(void *ctx, int h, void *buf, uint32_t n, uint64_t pos)
{
    struct conn *c = ctx;
    struct smb2fh *fh = file(c, h);
    if (!fh)
        return -EBADF;
    uint32_t max = smb2_get_max_read_size(c->smb2), done = 0;
    if (!max)
        max = 65536;
    while (done < n) {
        uint32_t k = n - done < max ? n - done : max;
        int r = smb2_pread(c->smb2, fh, (uint8_t *)buf + done, k, pos + done);
        if (r < 0)
            return done ? done : r;
        if (r == 0)
            break;
        done += (uint32_t)r;
    }
    return done;
}

static int64_t s_pwrite(void *ctx, int h, const void *buf, uint32_t n, uint64_t pos)
{
    struct conn *c = ctx;
    struct smb2fh *fh = file(c, h);
    if (!fh)
        return -EBADF;
    uint32_t max = smb2_get_max_write_size(c->smb2), done = 0;
    if (!max)
        max = 65536;
    while (done < n) {
        uint32_t k = n - done < max ? n - done : max;
        int r = smb2_pwrite(c->smb2, fh, (const uint8_t *)buf + done, k, pos + done);
        if (r <= 0)
            return done ? done : r < 0 ? r : -EIO;
        done += (uint32_t)r;
    }
    return done;
}

static int s_ftruncate(void *ctx, int h, uint64_t length)
{
    struct conn *c = ctx;
    struct smb2fh *fh = file(c, h);
    return fh ? smb2_ftruncate(c->smb2, fh, length) : -EBADF;
}

/* fstat's reply, copied out before libsmb2 frees it */
static void fstat_cb(struct smb2_context *s, int status, void *data, void *priv)
{
    struct pending *w = priv;
    struct smb2_query_info_reply *rep = data;
    if (status == 0 && rep && rep->output_buffer) {
        struct smb2_file_all_info *fs = rep->output_buffer;
        int dir = (fs->basic.file_attributes & SMB2_FILE_ATTRIBUTE_DIRECTORY) != 0;
        w->st.smb2_type = dir ? SMB2_TYPE_DIRECTORY : SMB2_TYPE_FILE;
        w->st.smb2_attributes = fs->basic.file_attributes;
        w->st.smb2_nlink = fs->standard.number_of_links;
        w->st.smb2_ino = fs->index_number;
        w->st.smb2_size = fs->standard.end_of_file;
        w->st.smb2_mtime = (uint64_t)fs->basic.last_write_time.tv_sec;
        w->st.smb2_mtime_nsec = (uint64_t)fs->basic.last_write_time.tv_usec * 1000;
        smb2_free_data(s, fs);
    }
    wait_cb(s, status, NULL, priv);
}

/* fstat: one QUERY_INFO for FileAllInformation on the handle. libsmb2's
 * smb2_fstat sends two in a compound, the second taking the handle from
 * the first. ksmbd refuses that after anything but a CREATE. The second
 * then fails, and HostFS's extent of the file would go stale */
static int s_fstat(void *ctx, int h, struct stat *st)
{
    struct conn *c = ctx;
    struct smb2fh *fh = file(c, h);
    if (!fh)
        return -EBADF;
    struct pending *w = new_wait(1, 1);
    if (!w)
        return -ENOMEM;
    struct smb2_query_info_request req;
    memset(&req, 0, sizeof req);
    req.info_type = SMB2_0_INFO_FILE;
    req.file_info_class = SMB2_FILE_ALL_INFORMATION;
    req.output_buffer_length = 65535;
    memcpy(req.file_id, *smb2_get_file_id(fh), SMB2_FD_SIZE);
    struct smb2_pdu *pdu = smb2_cmd_query_info_async(c->smb2, &req, fstat_cb, w);
    if (!pdu) {
        free(w);
        return -ENOMEM;
    }
    smb2_queue_pdu(c->smb2, pdu);
    int r = wait_for(c->smb2, w);
    if (r == 0)
        to_stat(&w->st, st);
    return wait_done(w, r);
}

static int s_close(void *ctx, int h)
{
    struct conn *c = ctx;
    struct smb2fh *fh = file(c, h);
    if (!fh)
        return -EBADF;
    c->files[h] = NULL;
    return smb2_close(c->smb2, fh);
}

static void *s_opendir(void *ctx, const char *path, int *err)
{
    struct conn *c = ctx;
    for (int tries = 0; tries < 2; tries++) {
        struct pending *w = new_wait(1, 0);
        if (!w) {
            *err = -ENOMEM;
            return NULL;
        }
        int r = c->smb2 ? smb2_opendir_async(c->smb2, path, wait_cb, w) : -ENOTCONN;
        if (r < 0) {
            free(w);
        } else {
            r = wait_for(c->smb2, w);
            void *dir = r == 0 ? w->data : NULL;
            r = wait_done(w, r);
            if (r == 0 && dir)
                return dir;
            if (r == 0)
                r = -EIO;
        }
        if (!again(c, r)) {
            *err = r;
            return NULL;
        }
    }
    *err = -EIO;
    return NULL;
}

static const char *s_readdir(void *ctx, void *dir, struct stat *st)
{
    struct conn *c = ctx;
    struct smb2dirent *e = smb2_readdir(c->smb2, dir);
    if (!e)
        return NULL;
    to_stat(&e->st, st);
    return e->name;
}

static void s_closedir(void *ctx, void *dir)
{
    struct conn *c = ctx;
    smb2_closedir(c->smb2, dir);
}

/* The path calls, repeated once on a connection made again */
#define PATH_CALL(call)                                                     \
    do {                                                                    \
        struct conn *c = ctx;                                               \
        int r = c->smb2 ? (call) : -ENOTCONN;                               \
        if (r < 0 && again(c, r))                                           \
            r = (call);                                                     \
        return r;                                                           \
    } while (0)

static int s_mkdir(void *ctx, const char *path) { PATH_CALL(smb2_mkdir(c->smb2, path)); }
static int s_rmdir(void *ctx, const char *path) { PATH_CALL(smb2_rmdir(c->smb2, path)); }
static int s_unlink(void *ctx, const char *path) { PATH_CALL(smb2_unlink(c->smb2, path)); }
static int s_rename(void *ctx, const char *from, const char *to)
{
    PATH_CALL(smb2_rename(c->smb2, from, to));
}

/* FileBasicInformation set by name. This is a compound of CREATE (for
 * attributes only, which a read-only file allows), SET_INFO and CLOSE, as
 * libsmb2's truncate is made. Times left at zero, and attributes at zero,
 * are left alone by the server. */
static int set_basic(struct conn *c, const char *path, struct smb2_file_basic_info *bi)
{
    struct pending *w = new_wait(3, 1);
    if (!w)
        return -ENOMEM;
    struct smb2_create_request cr;
    memset(&cr, 0, sizeof cr);
    cr.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    cr.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    cr.desired_access = SMB2_FILE_READ_ATTRIBUTES | SMB2_FILE_WRITE_ATTRIBUTES;
    cr.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
    cr.create_disposition = SMB2_FILE_OPEN;
    cr.name = path;
    struct smb2_pdu *pdu = smb2_cmd_create_async(c->smb2, &cr, wait_cb, w), *next;
    if (!pdu) {
        free(w);
        return -ENOMEM;
    }
    struct smb2_set_info_request si;
    memset(&si, 0, sizeof si);
    si.info_type = SMB2_0_INFO_FILE;
    si.file_info_class = SMB2_FILE_BASIC_INFORMATION;
    memcpy(si.file_id, compound_file_id, SMB2_FD_SIZE);
    si.input_data = bi;
    if (!(next = smb2_cmd_set_info_async(c->smb2, &si, wait_cb, w))) {
        smb2_free_pdu(c->smb2, pdu);
        free(w);
        return -ENOMEM;
    }
    smb2_add_compound_pdu(c->smb2, pdu, next);
    struct smb2_close_request cl;
    memset(&cl, 0, sizeof cl);
    memcpy(cl.file_id, compound_file_id, SMB2_FD_SIZE);
    if (!(next = smb2_cmd_close_async(c->smb2, &cl, wait_cb, w))) {
        smb2_free_pdu(c->smb2, pdu);
        free(w);
        return -ENOMEM;
    }
    smb2_add_compound_pdu(c->smb2, pdu, next);
    smb2_queue_pdu(c->smb2, pdu);
    return wait_done(w, wait_for(c->smb2, w));
}

static int s_setinfo(void *ctx, const char *path, const int64_t *mtime, int readonly)
{
    struct conn *c = ctx;
    struct smb2_stat_64 s;
    int r = c->smb2 ? smb2_stat(c->smb2, path, &s) : -ENOTCONN;
    if (r < 0 && again(c, r))
        r = smb2_stat(c->smb2, path, &s);
    if (r < 0)
        return r;
    struct smb2_file_basic_info bi;
    memset(&bi, 0, sizeof bi);
    if (mtime) {
        bi.last_write_time.tv_sec = (time_t)mtime[0];
        bi.last_write_time.tv_usec = (long)(mtime[1] / 1000);
    }
    if (readonly >= 0) {
        uint32_t a = s.smb2_attributes & ~(uint32_t)SMB2_FILE_ATTRIBUTE_READONLY;
        if (readonly)
            a |= SMB2_FILE_ATTRIBUTE_READONLY;
        if (a == s.smb2_attributes && !mtime)
            return 0;
        bi.file_attributes = a ? a : SMB2_FILE_ATTRIBUTE_NORMAL;
    }
    return set_basic(c, path, &bi);
}

static int s_free_space(void *ctx, uint64_t *free_bytes, uint64_t *size)
{
    struct conn *c = ctx;
    struct smb2_statvfs v;
    int r = c->smb2 ? smb2_statvfs(c->smb2, "", &v) : -ENOTCONN;
    if (r < 0 && again(c, r))
        r = smb2_statvfs(c->smb2, "", &v);
    if (r < 0)
        return r;
    *free_bytes = v.f_bavail * v.f_frsize;
    *size = v.f_blocks * v.f_frsize;
    return 0;
}

static const struct ros_hostio smb_io = {
    .stat = s_stat,
    .open = s_open,
    .pread = s_pread,
    .pwrite = s_pwrite,
    .ftruncate = s_ftruncate,
    .fstat = s_fstat,
    .close = s_close,
    .opendir = s_opendir,
    .readdir = s_readdir,
    .closedir = s_closedir,
    .mkdir = s_mkdir,
    .rmdir = s_rmdir,
    .unlink = s_unlink,
    .rename = s_rename,
    .setinfo = s_setinfo,
    .free_space = s_free_space,
};

/* ---- connecting ------------------------------------------------------------------ */

os_error *lanman_connect(const char *name, const char *server, const char *share, const char *user,
                         const char *password)
{
    if (!good_name(name) || !*server || !*share || strchr(share, '/') || strchr(share, '\\'))
        return lm_error(E_BADPARAM);
    if (find(name) || ros_hostfs.disc_exists(name))    /* not another disc's name */
        return lm_error(E_LINKEXISTS);
    struct conn *c = NULL;
    for (int i = 0; i < MAX_CONN && !c; i++)
        if (!conns[i].used)
            c = &conns[i];
    if (!c)
        return ros_error(ERROR_BASE + 34, "Connection limit has been reached");

    if (!user && logon.user[0])
        user = logon.user, password = password ? password : logon.password;
    memset(c, 0, sizeof *c);
    snprintf(c->server, sizeof c->server, "%s", server);
    snprintf(c->share, sizeof c->share, "%s", share);
    snprintf(c->user, sizeof c->user, "%s", user && *user ? user : "");
    snprintf(c->password, sizeof c->password, "%s", user && *user && password ? password : "");
    snprintf(c->domain, sizeof c->domain, "%s", user && *user ? logon.workgroup : "");
    uint32_t nt = 0;
    int r = open_session(c, &nt);
    if (r < 0) {
        memset(c, 0, sizeof *c);
        switch (nt) {
        case SMB2_STATUS_LOGON_FAILURE: case SMB2_STATUS_WRONG_PASSWORD:
            return lm_error(E_BADPASSWD);
        case SMB2_STATUS_BAD_NETWORK_NAME:
            return lm_error(E_NOSHARE);
        case SMB2_STATUS_ACCESS_DENIED: case SMB2_STATUS_NETWORK_ACCESS_DENIED:
        case SMB2_STATUS_ACCOUNT_DISABLED: case SMB2_STATUS_ACCOUNT_RESTRICTION:
            return lm_error(E_NOACCESS);
        }
        switch (-r) {
        case ENOENT: return lm_error(E_CANTFINDNAME);   /* the name not found */
        case ETIMEDOUT: return lm_error(E_TIMEOUT);
        case EHOSTUNREACH: case ENETUNREACH: case ECONNREFUSED: case EHOSTDOWN:
            return lm_error(E_LINKFAILED);
        default:
            return ros_error(ERROR_BASE + E_LINKFAILED, "Connection to server failed: %s",
                             strerror(-r));
        }
    }
    if (ros_hostfs_mount_io(name, &smb_io, c) != 0) {
        close_session(c);
        memset(c, 0, sizeof *c);
        return ros_error(ERROR_BASE + 34, "Connection limit has been reached");
    }
    snprintf(c->name, sizeof c->name, "%s", name);
    c->used = 1;
    return NULL;
}

os_error *lanman_disconnect(const char *name)
{
    struct conn *c = find(name);
    if (!c)
        return lm_error(E_BADDRV);
    for (int i = 0; i < MAX_FILES; i++)
        if (c->files[i])
            return ros_error(ERROR_BASE + 33, "File sharing violation: files are open on %s", c->name);
    ros_hostfs_unmount(c->name);
    close_session(c);
    memset(c, 0, sizeof *c);
    return NULL;
}

int lanman_connection_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONN; i++)
        n += conns[i].used;
    return n;
}

/* ---- the commands -------------------------------------------------------------------- */

static os_error *cmd_lmconnect(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char buf[512], *argv[5] = { 0 };
    int n = words(tail, buf, sizeof buf, argv, 5);
    if (n < 3)
        return lm_error(E_BADPARAM);
    const char *user = n > 3 && strcmp(argv[3], "-") ? argv[3] : NULL;
    return lanman_connect(argv[0], argv[1], argv[2], user, n > 4 ? argv[4] : NULL);
}

static os_error *cmd_lmdisconnect(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char buf[128], *argv[1];
    if (words(tail, buf, sizeof buf, argv, 1) < 1)
        return lm_error(E_BADPARAM);
    return lanman_disconnect(argv[0]);
}

static os_error *cmd_lmlogon(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char buf[300], *argv[3] = { 0 };
    int n = words(tail, buf, sizeof buf, argv, 3);
    if (n < 2)
        return lm_error(E_BADPARAM);
    snprintf(logon.workgroup, sizeof logon.workgroup, "%s", argv[0]);
    snprintf(logon.user, sizeof logon.user, "%s", argv[1]);
    snprintf(logon.password, sizeof logon.password, "%s", n > 2 ? argv[2] : "");
    return NULL;
}

static os_error *cmd_lmlogoff(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    memset(&logon, 0, sizeof logon);
    return NULL;
}

static os_error *cmd_lminfo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    char line[400];
    snprintf(line, sizeof line, "Workgroup: %s, user: %s\r\n", logon.workgroup[0] ? logon.workgroup : "(none)",
             logon.user[0] ? logon.user : "(none)");
    xos_write0(line, NULL);
    if (!lanman_connection_count())
        xos_write0("No connections\r\n", NULL);
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].used) {
            snprintf(line, sizeof line, "  %-12s \\\\%s\\%s as %s\r\n", conns[i].name, conns[i].server,
                     conns[i].share, conns[i].user[0] ? conns[i].user : "(guest)");
            xos_write0(line, NULL);
        }
    return NULL;
}

static os_error *cmd_lanman(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    char *name = ros_rma_alloc(8);
    memcpy(name, "LanMan", 7);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 14, s.r[1] = ros_addr(name);           /* OS_FSControl 14: select it */
    ros_swi(&s, XOS_FSControl);
    ros_rma_free(name);
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

#define C(n, lo, hi, syntax, help, fn) { n, ROS_CMD_INFO(lo, hi, 0, 0), "Syntax: *" syntax, help, fn }

static const struct ros_command commands[] = {
    C("LanMan", 0, 0, "LanMan", "*LanMan selects LanManFS as the current filing system.", cmd_lanman),
    C("LMConnect", 3, 5, "LMConnect <name> <server> <share> [<user>|- [<password>]]",
      "*LMConnect connects to a share on an SMB server -- SMB2 or SMB3 -- as "
      "the disc <name>: LanMan::<name>.$ and HostFS::<name>.$.", cmd_lmconnect),
    C("LMDisconnect", 1, 1, "LMDisconnect <name>", "*LMDisconnect disconnects from a share.",
      cmd_lmdisconnect),
    C("LMLogon", 2, 3, "LMLogon <workgroup> <user> [<password>]",
      "*LMLogon sets the workgroup, user and password *LMConnect uses when it is given none.",
      cmd_lmlogon),
    C("LMLogoff", 0, 0, "LMLogoff", "*LMLogoff clears the workgroup, user and password.", cmd_lmlogoff),
    C("LMInfo", 0, 0, "LMInfo", "*LMInfo lists the connections.", cmd_lminfo),
    { 0 },
};

/* ---- the filing system name -------------------------------------------------------------- */

/* LanMan: HostFS's discs by another name and number. Its $ is the first
 * connection's */
struct fs ros_lanmanfs;

static const char *lm_boot_disc(void)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].used)
            return conns[i].name;
    return "";
}

__attribute__((constructor)) static void make_fs(void)
{
    ros_lanmanfs = ros_hostfs;
    ros_lanmanfs.name = "LanMan";
    ros_lanmanfs.number = FS_NUMBER;
    ros_lanmanfs.boot_disc = lm_boot_disc;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].used)
            lanman_disconnect(conns[i].name);
    return NULL;
}

struct ros_module lanmanfs_module = {
    .title = "LanManFS",
    .help = "LanManFS\t3.10 (10 Oct 2026) ROSGD native: SMB2/3 shares through libsmb2",
    .final = final,
    .commands = commands,
};
