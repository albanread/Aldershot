/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_smb.c: LanManFS (modules/lanmanfs) and SMBServer
 * (modules/smbserver): SMB shares, both ways.
 *
 * Everywhere: the LanMan filing system name reaching HostFS's discs, and
 * LanManFS's commands and errors. In the box there is a directory of the
 * test's own in /tmp, shared by *Share over ksmbd, and a user made for it by
 * *SMBUser. LanManFS connects to it over the loopback through libsmb2. A file
 * that the directory holds is read through LanMan::. A file saved through it
 * appears in the directory. A wrong password is refused. Then the rest of
 * what HostFS asks of the share is checked: a directory, a file's type and
 * date, a listing, a rename, and a lock.
 */
#include <dirent.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fileswitch.h"
#include "lanmanfs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"
#include "smbserver.h"

#define check ros_check

static int cli(const char *line)
{
    size_t n = strlen(line);
    char *c = ros_rma_alloc((uint32_t)n + 1);
    memcpy(c, line, n);
    c[n] = '\r';
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(c);
    ros_swi(&s, XOS_CLI);
    ros_rma_free(c);
    return s.v ? (int)((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

/* A file's contents through FileSwitch (OS_File 16 into a buffer), or -1 */
static int load(const char *name, char *out, int max)
{
    size_t n = strlen(name);
    char *nm = ros_rma_alloc((uint32_t)n + 1), *buf = ros_rma_alloc((uint32_t)max);
    memcpy(nm, name, n + 1);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 5, s.r[1] = ros_addr(nm);            /* OS_File 5: its length */
    ros_swi(&s, XOS_File);
    int len = -1;
    if (!s.v && s.r[0] == 1 && (int)s.r[4] < max) {
        len = (int)s.r[4];
        ros_cpu_enter(&s);
        s.r[0] = 16, s.r[1] = ros_addr(nm), s.r[2] = ros_addr(buf), s.r[3] = 0;
        ros_swi(&s, XOS_File);
        if (s.v)
            len = -1;
        else
            memcpy(out, buf, (size_t)len), out[len] = 0;
    }
    ros_rma_free(nm);
    ros_rma_free(buf);
    return len;
}

/* Save text as a Text file (OS_File 10) */
static int save(const char *name, const char *text)
{
    size_t n = strlen(name), t = strlen(text);
    char *nm = ros_rma_alloc((uint32_t)n + 1), *buf = ros_rma_alloc((uint32_t)t + 1);
    memcpy(nm, name, n + 1);
    memcpy(buf, text, t);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 10, s.r[1] = ros_addr(nm), s.r[2] = 0xFFF;
    s.r[4] = ros_addr(buf), s.r[5] = ros_addr(buf) + (uint32_t)t;
    ros_swi(&s, XOS_File);
    ros_rma_free(nm);
    ros_rma_free(buf);
    return s.v ? (int)((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

/* A file's extent with it open (OS_Find 0x40, OS_Args 2), or -1 */
static int extent_open(const char *name)
{
    size_t n = strlen(name);
    char *nm = ros_rma_alloc((uint32_t)n + 1);
    memcpy(nm, name, n + 1);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0x4F, s.r[1] = ros_addr(nm);      /* OS_Find: open to read, errors if absent */
    ros_swi(&s, XOS_Find);
    ros_rma_free(nm);
    if (s.v || !s.r[0])
        return -1;
    uint32_t h = s.r[0];
    ros_cpu_enter(&s);
    s.r[0] = 2, s.r[1] = h;
    ros_swi(&s, XOS_Args);
    int ext = s.v ? -1 : (int)s.r[2];
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = h;
    ros_swi(&s, XOS_Find);
    return ext;
}

/* OS_File 1: a file's load and exec addresses and attributes */
static int set_info(const char *name, uint32_t load, uint32_t exec, uint32_t attr)
{
    size_t n = strlen(name);
    char *nm = ros_rma_alloc((uint32_t)n + 1);
    memcpy(nm, name, n + 1);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 1, s.r[1] = ros_addr(nm), s.r[2] = load, s.r[3] = exec, s.r[5] = attr;
    ros_swi(&s, XOS_File);
    ros_rma_free(nm);
    return s.v ? (int)((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

/* OS_File 17: an object's type, load address and attributes; -1 for an error */
static int get_info(const char *name, uint32_t *load, uint32_t *attr)
{
    size_t n = strlen(name);
    char *nm = ros_rma_alloc((uint32_t)n + 1);
    memcpy(nm, name, n + 1);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 17, s.r[1] = ros_addr(nm);
    ros_swi(&s, XOS_File);
    ros_rma_free(nm);
    *load = s.r[2], *attr = s.r[5];
    return s.v ? -1 : (int)s.r[0];
}

/* OS_GBPB 10: the first name in a directory and how many it holds, or -1 */
static int first_entry(const char *dir, char *out, size_t max)
{
    size_t n = strlen(dir);
    char *nm = ros_rma_alloc((uint32_t)n + 1), *buf = ros_rma_alloc(256);
    memcpy(nm, dir, n + 1);
    int count = 0;
    out[0] = 0;
    for (uint32_t offset = 0; offset != 0xFFFFFFFFu;) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 10, s.r[1] = ros_addr(nm), s.r[2] = ros_addr(buf), s.r[3] = 1;
        s.r[4] = offset, s.r[5] = 256, s.r[6] = 0;
        ros_swi(&s, XOS_GBPB);
        if (s.v) {
            count = -1;
            break;
        }
        if (s.r[3] && !count++)
            snprintf(out, max, "%s", buf + 20);
        offset = s.r[4];
    }
    ros_rma_free(nm);
    ros_rma_free(buf);
    return count;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int read_file(const char *path, char *out, size_t max)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(out, 1, max - 1, f);
    out[n] = 0;
    fclose(f);
    return 1;
}

/* ---- a disc of calls (struct ros_hostio) over a Linux directory ---------- */

/* What LanManFS gives HostFS, made of the POSIX calls: so HostFS's way
 * through a disc's calls, and a lock kept as a read-only file, can be tried
 * where there is no SMB server that keeps one */
static char io_root[256];

static void io_path(const char *rel, char *out, size_t max)
{
    snprintf(out, max, "%s/%s", io_root, rel);
}

static int t_ret(int r)
{
    return r < 0 ? -errno : r;
}

static int t_stat(void *ctx, const char *p, struct stat *st)
{
    char f[512];
    (void)ctx;
    io_path(p, f, sizeof f);
    return t_ret(stat(f, st));
}

static int t_open(void *ctx, const char *p, int flags)
{
    char f[512];
    (void)ctx;
    io_path(p, f, sizeof f);
    return t_ret(open(f, flags, 0644));
}

static int64_t t_pread(void *ctx, int h, void *buf, uint32_t n, uint64_t pos)
{
    (void)ctx;
    ssize_t r = pread(h, buf, n, (off_t)pos);
    return r < 0 ? -errno : r;
}

static int64_t t_pwrite(void *ctx, int h, const void *buf, uint32_t n, uint64_t pos)
{
    (void)ctx;
    ssize_t r = pwrite(h, buf, n, (off_t)pos);
    return r < 0 ? -errno : r;
}

static int t_ftruncate(void *ctx, int h, uint64_t length)
{
    (void)ctx;
    return t_ret(ftruncate(h, (off_t)length));
}

static int t_fstat(void *ctx, int h, struct stat *st)
{
    (void)ctx;
    return t_ret(fstat(h, st));
}

static int t_close(void *ctx, int h)
{
    (void)ctx;
    return t_ret(close(h));
}

static void *t_opendir(void *ctx, const char *p, int *err)
{
    char f[512];
    (void)ctx;
    io_path(p, f, sizeof f);
    DIR *d = opendir(f);
    if (!d)
        *err = -errno;
    return d;
}

static const char *t_readdir(void *ctx, void *dir, struct stat *st)
{
    (void)ctx;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        char f[512];
        snprintf(f, sizeof f, "%s/%s", io_root, e->d_name);
        /* the listing is of the disc's root only, in this test */
        if (stat(f, st) == 0)
            return e->d_name;
    }
    return NULL;
}

static void t_closedir(void *ctx, void *dir)
{
    (void)ctx;
    closedir(dir);
}

static int t_mkdir(void *ctx, const char *p)
{
    char f[512];
    (void)ctx;
    io_path(p, f, sizeof f);
    return t_ret(mkdir(f, 0755));
}

static int t_rmdir(void *ctx, const char *p)
{
    char f[512];
    (void)ctx;
    io_path(p, f, sizeof f);
    return t_ret(rmdir(f));
}

static int t_unlink(void *ctx, const char *p)
{
    char f[512];
    (void)ctx;
    io_path(p, f, sizeof f);
    return t_ret(unlink(f));
}

static int t_rename(void *ctx, const char *from, const char *to)
{
    char f[512], t[512];
    (void)ctx;
    io_path(from, f, sizeof f);
    io_path(to, t, sizeof t);
    return t_ret(rename(f, t));
}

static int t_setinfo(void *ctx, const char *p, const int64_t *mtime, int readonly)
{
    char f[512];
    (void)ctx;
    io_path(p, f, sizeof f);
    if (mtime) {
        struct timespec t[2] = { { (time_t)mtime[0], (long)mtime[1] }, { (time_t)mtime[0], (long)mtime[1] } };
        if (utimensat(AT_FDCWD, f, t, 0) != 0)
            return -errno;
    }
    return readonly < 0 ? 0 : t_ret(chmod(f, readonly ? 0444 : 0644));
}

static int t_free_space(void *ctx, uint64_t *free_bytes, uint64_t *size)
{
    struct statvfs v;
    (void)ctx;
    if (statvfs(io_root, &v) != 0)
        return -errno;
    *free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
    *size = (uint64_t)v.f_blocks * v.f_frsize;
    return 0;
}

static const struct ros_hostio t_io = {
    .stat = t_stat, .open = t_open, .pread = t_pread, .pwrite = t_pwrite,
    .ftruncate = t_ftruncate, .fstat = t_fstat, .close = t_close, .opendir = t_opendir,
    .readdir = t_readdir, .closedir = t_closedir, .mkdir = t_mkdir, .rmdir = t_rmdir,
    .unlink = t_unlink, .rename = t_rename, .setinfo = t_setinfo, .free_space = t_free_space,
};

static void a_disc_of_calls(const char *dir)
{
    char path[512], got[64] = "", first[64] = "";
    uint32_t ld = 0, attr = 0, attr2 = 0;
    struct stat st;
    snprintf(io_root, sizeof io_root, "%s", dir);
    mkdir(dir, 0755);
    int m = ros_hostfs_mount_io("IOTest", &t_io, NULL);
    int sv = save("HostFS::IOTest.$.Note", "kept");
    snprintf(path, sizeof path, "%s/Note", dir);
    int there = stat(path, &st) == 0 && st.st_size == 4;
    int n = load("HostFS::IOTest.$.Note", got, sizeof got);
    int ents = first_entry("HostFS::IOTest.$", first, sizeof first);
    int lk = cli("Access HostFS::IOTest.$.Note LR");
    int ro = stat(path, &st) == 0 && !(st.st_mode & 0222);
    int li = get_info("HostFS::IOTest.$.Note", &ld, &attr);
    int refused = cli("Delete HostFS::IOTest.$.Note");
    int ul = cli("Access HostFS::IOTest.$.Note WR");
    int ui = get_info("HostFS::IOTest.$.Note", &ld, &attr2);
    int dl = cli("Delete HostFS::IOTest.$.Note");
    int gone = stat(path, &st) != 0;
    check(!m && !sv && there && n == 4 && !strcmp(got, "kept") && ents == 1 &&
              !strcmp(first, "Note") && !lk && ro && li == 1 && (attr & 8) &&
              (refused & 0xFF) == 0xC3 && !ul && ui == 1 && !(attr2 & 8) && (attr2 & 2) && !dl &&
              gone,
          "HostFS -- a disc of calls (LanManFS's way): a file saved, read and listed; locked as a "
          "read-only file, which a delete is refused; unlocked, deleted",
          "%d &%X %s %d \"%s\" %d \"%s\", &%X %s %d attr &%X &%X, &%X %d attr &%X &%X %s", m, sv,
          there ? "there" : "not there", n, got, ents, first, lk, ro ? "read-only" : "writable", li,
          attr, refused, ul, ui, attr2, dl, gone ? "gone" : "there");
    ros_hostfs_unmount("IOTest");
    unlink(path);
    rmdir(dir);
}

static void everywhere(const char *dir)
{
    char path[512], got[64] = "";
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/note", dir);
    write_file(path, "a note");
    ros_hostfs_mount("LMAlias", dir);
    int n = load("LanMan::LMAlias.$.note", got, sizeof got);
    check(n == 6 && !strcmp(got, "a note"), "LanMan:: -- LanManFS's name reaches HostFS's discs",
          "%d \"%s\"", n, got);
    ros_hostfs_unmount("LMAlias");
    unlink(path);
    rmdir(dir);

    int e0 = cli("LMConnect OnlyTwo args");     /* the command's own minimum: Syntax */
    int e1 = cli("LMConnect Bad.Name server share");
    int e2 = cli("LMDisconnect NotConnected");
    int e3 = cli("LMLogon WORKGROUP someone secret");
    int e4 = cli("LMInfo");
    int e5 = cli("LMLogoff");
    check(e0 == 0xDC && e1 == 0x16601 && e2 == 0x16618 && !e3 && !e4 && !e5,
          "*LMConnect, *LMDisconnect, *LMLogon, *LMInfo -- LanManFS's errors, &16600 + n",
          "&%X &%X &%X %d %d %d", e0, e1, e2, e3, e4, e5);
}

/* What the server at ADDR says about signing: the SecurityMode byte of its
 * SMB2 NEGOTIATE reply (bit 0 signing enabled, bit 1 required), asked for
 * with a bare dialect 3.0.2 request over a plain socket.  -1 if it does not
 * answer, or -2 if it refuses the connection. */
static int negotiate_mode(const char *addr)
{
    unsigned char req[4 + 64 + 38] = { 0 }, rsp[256];
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(445) };
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -2;
    }
    req[3] = 64 + 38;                           /* the NetBIOS length */
    memcpy(req + 4, "\xFE" "SMB", 4);
    req[8] = 64;                                /* header size */
    req[18] = 1;                                /* credits requested */
    req[4 + 64] = 36;                           /* NEGOTIATE: structure size */
    req[4 + 64 + 2] = 1;                        /* one dialect */
    req[4 + 64 + 4] = 1;                        /* the client enables signing */
    req[4 + 64 + 36] = 0x02, req[4 + 64 + 37] = 0x03;       /* 3.0.2 */
    int mode = -1;
    if (write(fd, req, sizeof req) == (ssize_t)sizeof req) {
        ssize_t n = read(fd, rsp, sizeof rsp);
        if (n > 4 + 64 + 4 && !memcmp(rsp + 4, "\xFE" "SMB", 4))
            mode = rsp[4 + 64 + 2];
    }
    close(fd);
    return mode;
}

static void in_the_box(void)
{
    const char *dir = "/tmp/smb-share";
    char path[512], got[128] = "";
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/greeting", dir);
    write_file(path, "hello over SMB");
    ros_hostfs_mount("SMBTest", dir);

    int u = cli("SMBUser rosgdtest not-a-real-password");
    int sh = cli("Share HostFS::SMBTest.$ Test");
    check(!u && !sh && smb_running(), "*SMBUser, *Share -- a directory shared by ksmbd",
          "&%X &%X, ksmbd %s (see /run/ksmbd.log)", u, sh, smb_running() ? "running" : "not running");

    int sig = negotiate_mode("127.0.0.1");
    check(sig >= 0 && (sig & 3) == 3, "SMBServer -- the server requires SMB signing (NEGOTIATE reply)",
          "SecurityMode %d", sig);

    int bad = cli("LMConnect Wrong 127.0.0.1 Test rosgdtest wrong-password");
    int c = cli("LMConnect T 127.0.0.1 Test rosgdtest not-a-real-password");
    int n = c ? -1 : load("LanMan::T.$.greeting", got, sizeof got);
    int ext = c ? -1 : extent_open("LanMan::T.$.greeting");
    check(bad == 0x16616 && !c && n == 14 && !strcmp(got, "hello over SMB") && ext == 14,
          "*LMConnect -- the share over SMB3 (libsmb2, ksmbd), a file read, its extent while open; "
          "a wrong password refused", "&%X &%X %d \"%s\" extent %d", bad, c, n, got, ext);

    int w = c ? -1 : save("LanMan::T.$.Written", "saved from RISC OS");
    char back[128] = "";
    snprintf(path, sizeof path, "%s/Written", dir);   /* Text: HostFS needs no suffix */
    int there = read_file(path, back, sizeof back);
    check(!w && there && !strcmp(back, "saved from RISC OS"),
          "LanMan:: -- a Text file saved through the share lands in the directory",
          "&%X, %s: \"%s\"", w, there ? "there" : "not there", back);

    /* The rest of HostFS's calls, over SMB.  The date: &5A12345678
     * centiseconds since 1900, 14:12:45 on 3 August 2022, set with the
     * type BASIC (&FFB), which the share keeps as ",ffb" */
    const uint64_t cs = 0x5A12345678ull;
    const long long secs = (long long)(cs / 100) - 2208988800LL;
    struct stat st = { 0 };
    char first[64] = "";
    uint32_t ld = 0, attr = 0;
    int md = c ? -1 : cli("CDir LanMan::T.$.Dir");
    int sv = c ? -1 : save("LanMan::T.$.Dir.Prog", "10 PRINT");
    int si = c ? -1 : set_info("LanMan::T.$.Dir.Prog", 0xFFFFFB00u | (uint32_t)(cs >> 32),
                               (uint32_t)cs, 3);
    snprintf(path, sizeof path, "%s/Dir/Prog,ffb", dir);
    int typed = stat(path, &st) == 0 && (long long)st.st_mtime == secs;
    int ents = c ? -1 : first_entry("LanMan::T.$.Dir", first, sizeof first);
    int ti = c ? -1 : get_info("LanMan::T.$.Dir.Prog", &ld, &attr);
    int rn = c ? -1 : cli("Rename LanMan::T.$.Dir.Prog LanMan::T.$.Dir.Renamed");
    snprintf(path, sizeof path, "%s/Dir/Renamed,ffb", dir);
    int moved = stat(path, &st) == 0;
    /* (A lock is the server's read-only attribute, which *SMBServer's
     * shares do not keep because of "store dos attributes = no". So locks
     * are tried in everywhere(), on a disc of calls that keeps them.) */
    int dl = c ? -1 : cli("Delete LanMan::T.$.Dir.Renamed");
    int dd = c ? -1 : cli("Delete LanMan::T.$.Dir");
    snprintf(path, sizeof path, "%s/Dir", dir);
    int gone = stat(path, &st) != 0;
    check(!md && !sv && !si && typed && ents == 1 && !strcmp(first, "Prog") && ti == 1 &&
              ld == (0xFFFFFB00u | (uint32_t)(cs >> 32)) && !rn && moved && !dl && !dd && gone,
          "LanMan:: -- over SMB, a directory made, a type and a date set (\"Prog,ffb\", its mtime), "
          "a listing, a rename, the deletes",
          "&%X &%X &%X %s, %d \"%s\", %d &%08X, &%X %s, &%X &%X %s", md, sv, si,
          typed ? "typed" : "not typed", ents, first, ti, ld, rn, moved ? "moved" : "not moved",
          dl, dd, gone ? "gone" : "there");

    int d = cli("LMDisconnect T");
    int us = cli("UnShare Test");
    cli("SMBUser rosgdtest");                   /* the test's user gone again */
    check(!d && !us && lanman_connection_count() == 0, "*LMDisconnect, *UnShare", "&%X &%X", d, us);
    ros_hostfs_unmount("SMBTest");
    unlink(path);
    snprintf(path, sizeof path, "%s/greeting", dir);
    unlink(path);
    rmdir(dir);
}

/* The interfaces ksmbd is told to use: the loopback and the box's other
 * interfaces that are up by default, the ones SMBServer$Interfaces names
 * if it is set, and only that. (The box reaches its own addresses through
 * the loopback, so what a client on the network sees is tried from outside:
 * tests/smb/mac.sh) */
static void the_interfaces(void)
{
    const char *dir = "/tmp/smb-if";
    char conf[2048] = "", up[160] = "";
    mkdir(dir, 0755);
    ros_hostfs_mount("SMBIf", dir);
    struct ifaddrs *list, *a;
    if (getifaddrs(&list) == 0) {
        for (a = list; a; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && (a->ifa_flags & IFF_UP) &&
                (a->ifa_flags & IFF_RUNNING) && !(a->ifa_flags & IFF_LOOPBACK) && !strstr(up, a->ifa_name))
                snprintf(up + strlen(up), sizeof up - strlen(up), " %s", a->ifa_name);
        freeifaddrs(list);
    }
    int sh = cli("Share HostFS::SMBIf.$ If");
    int rd = read_file("/etc/ksmbd/ksmbd.conf", conf, sizeof conf);
    int all = 1;
    for (const char *p = up; *p;) {
        char name[32];
        p++;
        size_t n = strcspn(p, " ");
        snprintf(name, sizeof name, "%.*s", (int)n, p);
        all &= strstr(conf, name) != NULL;
        p += n;
    }
    int lo = negotiate_mode("127.0.0.1");
    check(!sh && rd && smb_running() && lo >= 0 && strstr(conf, "\tinterfaces = lo") &&
              strstr(conf, "\tbind interfaces only = yes") && all,
          "*Share -- by default the server is bound to the loopback and the interfaces that are up, "
          "not to every address", "&%X %d \"%s\" up:%s", sh, lo, conf, up);

    cli("UnShare If");                          /* ksmbd reads the interfaces as it starts */
    int set = cli("Set SMBServer$Interfaces lo");
    sh = cli("Share HostFS::SMBIf.$ If");
    conf[0] = 0;
    rd = read_file("/etc/ksmbd/ksmbd.conf", conf, sizeof conf);
    lo = negotiate_mode("127.0.0.1");
    check(!set && !sh && rd && lo >= 0 && strstr(conf, "\tinterfaces = lo\n"),
          "SMBServer$Interfaces -- names the interfaces to use, and only those", "&%X &%X %d \"%s\"", set, sh,
          lo, conf);

    cli("UnShare If");
    set = cli("Set SMBServer$Interfaces bad/name;x");
    sh = cli("Share HostFS::SMBIf.$ If");
    conf[0] = 0;
    rd = read_file("/etc/ksmbd/ksmbd.conf", conf, sizeof conf);
    check(!set && !sh && rd && !strstr(conf, "bad") && strstr(conf, "\tinterfaces = lo"),
          "SMBServer$Interfaces -- a name that is not an interface name is never written to ksmbd.conf",
          "&%X &%X \"%s\"", set, sh, conf);
    cli("UnShare If");
    cli("Unset SMBServer$Interfaces");
    ros_hostfs_unmount("SMBIf");
    rmdir(dir);
}

/* (What a guest can do over the wire is tried from outside, with macOS's
 * client, tests/smb/mac-vz.sh: LanManFS's libsmb2 cannot be a guest of a
 * server that requires signing, as a guest's session has no key to sign
 * with.) */
/* Guests: a share for them is read only unless -write is given, an unknown
 * user or a wrong password cannot reach a share that is not for guests,
 * and the command refuses what is not clear */
static void the_guests(void)
{
    const char *dir = "/tmp/smb-guest", *priv = "/tmp/smb-private";
    char path[512], conf[2048] = "", got[64] = "";
    mkdir(dir, 0755);
    mkdir(priv, 0755);
    snprintf(path, sizeof path, "%s/pub", dir);
    write_file(path, "for anyone");
    snprintf(path, sizeof path, "%s/secret", priv);
    write_file(path, "for users");
    snprintf(path, sizeof path, "%s/.ssh", priv);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.ssh/authorized_keys", priv);
    write_file(path, "a key");
    snprintf(path, sizeof path, "%s/.other", priv);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/.other/f", priv);
    write_file(path, "kept");
    ros_hostfs_mount("SMBGuest", dir);
    ros_hostfs_mount("SMBPriv", priv);

    int u = cli("SMBUser rosgdtest not-a-real-password");
    int p = cli("Share HostFS::SMBPriv.$ Priv");
    int g = cli("Share HostFS::SMBGuest.$ Pub -guest");
    int rd = read_file("/etc/ksmbd/ksmbd.conf", conf, sizeof conf);
    const char *sec = strstr(conf, "[Pub]");
    int ro = sec && strstr(sec, "read only = yes") && strstr(sec, "guest ok = yes");
    check(!u && !p && !g && rd && ro, "*Share -guest -- read only unless -write is given",
          "&%X &%X &%X \"%s\"", u, p, g, conf);

    struct stat cs = { 0 }, ds = { 0 }, ps = { 0 };
    int st1 = stat("/etc/ksmbd/ksmbd.conf", &cs), st2 = stat("/etc/ksmbd", &ds),
        st3 = stat("/etc/ksmbd/ksmbdpwd.db", &ps);
    check(!st1 && !st2 && !st3 && (cs.st_mode & 0777) == 0600 && (ds.st_mode & 0777) == 0700 &&
              (ps.st_mode & 0777) == 0600,
          "SMBServer -- ksmbd.conf, the password database and their directory are root's alone",
          "conf %o, directory %o, database %o", cs.st_mode & 0777, ds.st_mode & 0777, ps.st_mode & 0777);

    /* What cannot be written into ksmbd.conf is refused: a directory with ';'
     * in its name (the reader would stop there, and share the one above),
     * one that ends in a space, and a share name with '#' */
    const char *odd = "/tmp/smb-odd;dir", *sp = "/tmp/smb-odd ";
    mkdir(odd, 0755);
    mkdir(sp, 0755);
    ros_hostfs_mount("SMBOdd", odd);
    ros_hostfs_mount("SMBSp", sp);
    int o1 = cli("Share HostFS::SMBOdd.$ Odd");
    int o2 = cli("Share HostFS::SMBSp.$ Sp");
    int o3 = cli("Share HostFS::SMBPriv.$ a#b");
    int o4 = cli("Share HostFS::SMBPriv.$ \"a b \"");
    int o5 = cli("Share HostFS::SMBPriv.$ ..");
    conf[0] = 0;
    read_file("/etc/ksmbd/ksmbd.conf", conf, sizeof conf);
    check(o1 && o2 && o3 && o4 && o5 && !strstr(conf, "[Odd]") && !strstr(conf, "[Sp]"),
          "*Share -- a directory or a share name that ksmbd.conf cannot hold is refused",
          "&%X &%X &%X &%X &%X", o1, o2, o3, o4, o5);
    ros_hostfs_unmount("SMBOdd");
    ros_hostfs_unmount("SMBSp");
    rmdir(odd);
    rmdir(sp);

    int pu = cli("LMConnect UP 127.0.0.1 Priv nosuchuser not-a-real-password");
    int pw = cli("LMConnect WP 127.0.0.1 Priv rosgdtest wrong-password");
    int pk = cli("LMConnect KP 127.0.0.1 Priv rosgdtest not-a-real-password");
    int pn = pk ? -1 : load("LanMan::KP.$.secret", got, sizeof got);
    check(pu != 0 && pw != 0 && !pk && pn == 9,
          "LanMan:: -- a share that is not for guests: an unknown user and a wrong password are "
          "refused, a user gets in", "&%X &%X &%X %d", pu, pw, pk, pn);
    /* The files of the .ssh directory are not served: a share of the host
     * share's top would hand over the keys (a name that begins with a dot is
     * its RISC OS name with a slash) */
    char one[64] = "", two[64] = "";
    int kt = pk ? -1 : load("LanMan::KP.$./other.f", one, sizeof one);
    int ks = pk ? -1 : load("LanMan::KP.$./ssh.authorized_keys", two, sizeof two);
    check(kt == 4 && !strcmp(one, "kept") && ks == -1,
          "LanMan:: -- over SMB, .ssh and what is in it are not served, another dot directory is",
          "%d \"%s\" %d \"%s\"", kt, one, ks, two);
    cli("LMDisconnect KP");
    cli("LMDisconnect UP");
    cli("LMDisconnect WP");

    /* Passwords are typed to ksmbd.adduser on a terminal of its own: every
     * printable ASCII character but the double quote (which ends a quoted
     * word) and a name with a space, the DEL it cannot take, and the empty
     * and overlong ones refused */
    char pwall[128], cmd[512], cmd2[512];
    size_t k = 0;
    for (int ch = 0x20; ch < 0x7F; ch++)
        if (ch != '"')
            pwall[k++] = (char)ch;
    pwall[k] = 0;
    snprintf(cmd, sizeof cmd, "SMBUser rosgdpw \"%s\"", pwall);
    snprintf(cmd2, sizeof cmd2, "LMConnect PW 127.0.0.1 Priv rosgdpw \"%s\"", pwall);
    int sa = cli(cmd);
    int ca = sa ? -1 : cli(cmd2);
    int na = ca ? -1 : load("LanMan::PW.$.secret", got, sizeof got);
    cli("LMDisconnect PW");
    snprintf(cmd, sizeof cmd, "SMBUser rosgdpw \"%s\"", "\xC3\xA9t\xC3\xA9 \xE2\x82\xAC;#$%^&*()");
    snprintf(cmd2, sizeof cmd2, "LMConnect PW 127.0.0.1 Priv rosgdpw \"%s\"", "\xC3\xA9t\xC3\xA9 \xE2\x82\xAC;#$%^&*()");
    int sb = cli(cmd);
    int cb = sb ? -1 : cli(cmd2);
    int nb = cb ? -1 : load("LanMan::PW.$.secret", got, sizeof got);
    cli("LMDisconnect PW");
    int s_empty = cli("SMBUser rosgdpw \"\"");
    char longpw[200];
    memset(longpw, 'x', 129);
    longpw[129] = 0;
    snprintf(cmd, sizeof cmd, "SMBUser rosgdpw %s", longpw);
    int s_long = cli(cmd);
    int s_del = cli("SMBUser rosgdpw a\x7F" "b");
    int cc = cli("LMConnect PW 127.0.0.1 Priv rosgdpw wrong");
    cli("LMDisconnect PW");
    cli("SMBUser rosgdpw");
    check(!sa && !ca && na == 9 && !sb && !cb && nb == 9 && s_empty && s_long && s_del && cc,
          "*SMBUser -- the password reaches ksmbd.adduser whole, 94 ASCII characters and UTF-8 ones; "
          "an empty, an overlong and a DEL password are refused",
          "&%X &%X %d &%X &%X %d &%X &%X &%X &%X", sa, ca, na, sb, cb, nb, s_empty, s_long,
          s_del, cc);

    int e1 = cli("Share HostFS::SMBGuest.$ Pub -guest -readonly");
    int e2 = cli("Share HostFS::SMBGuest.$ Pub -write");
    int e3 = cli("Share HostFS::SMBGuest.$ Pub -guest -write -readonly");
    os_error *e4 = smb_share(dir, "Pub", 0, 1, 0);
    check(!e1 && e2 && e3 && e4, "*Share -- -write needs -guest and not -readonly; a guest share that "
          "is not read only is refused without it", "&%X &%X &%X %s", e1, e2, e3, e4 ? e4->errmess : "accepted");

    int ew = cli("Share HostFS::SMBGuest.$ Pub -guest -write");
    conf[0] = 0;
    rd = read_file("/etc/ksmbd/ksmbd.conf", conf, sizeof conf);
    sec = strstr(conf, "[Pub]");
    check(!ew && rd && sec && strstr(sec, "read only = no") && strstr(sec, "guest ok = yes"),
          "*Share -guest -write -- the guest share is writable", "&%X \"%s\"", ew, conf);

    cli("UnShare Pub");
    cli("UnShare Priv");
    cli("SMBUser rosgdtest");
    ros_hostfs_unmount("SMBGuest");
    ros_hostfs_unmount("SMBPriv");
    snprintf(path, sizeof path, "%s/pub", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/secret", priv);
    unlink(path);
    snprintf(path, sizeof path, "%s/.ssh/authorized_keys", priv);
    unlink(path);
    snprintf(path, sizeof path, "%s/.ssh", priv);
    rmdir(path);
    snprintf(path, sizeof path, "%s/.other/f", priv);
    unlink(path);
    snprintf(path, sizeof path, "%s/.other", priv);
    rmdir(path);
    rmdir(dir);
    rmdir(priv);
}

void ros_selftest_smb(void)
{
    char dir[256];
    const char *t = getpid() == 1 ? "/tmp" : getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rosgd-lmalias-%d", t ? t : "/tmp", (int)getpid());
    everywhere(dir);
    snprintf(dir, sizeof dir, "%s/rosgd-iodisc-%d", t ? t : "/tmp", (int)getpid());
    a_disc_of_calls(dir);
    if (getpid() == 1)
    {
        in_the_box();
        the_interfaces();
        the_guests();
    }
}
