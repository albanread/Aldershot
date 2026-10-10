/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* smbserver.c -- SMBServer, a native module: the box's directories shared
 * over SMB3, by ksmbd.
 *
 * RISC OS shares directories with Acorn Access (ShareFS). That is a closed
 * protocol that only RISC OS speaks. ROSGD shares them over SMB2/3, which
 * Macs, Windows and Linux all speak. It does so through ksmbd, which is
 * Linux's in-kernel SMB server, and its user-space half, ksmbd.mountd.
 * Access's commands keep their shape:
 *
 *   *Share <directory> [<name>] [-readonly] [-guest [-write]]
 *        the directory, any HostFS (or LanMan) name, as share <name> (its
 *        leaf if none given); -guest lets anyone in without a password, to
 *        read only unless -write says they may write too
 *   *UnShare <name>
 *   *Shares       the shares, and where to find them
 *   *SMBUser <user> [<password>]
 *        who may connect, and with what password; none removes the user
 *
 * ksmbd.mountd starts with the first share and is told of each change
 * (ksmbd.control --reload). Its configuration, /etc/ksmbd/ksmbd.conf, and
 * its users, ksmbdpwd.db beside it, are written from here. The shares'
 * files are the box's, as root owns them.
 */
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "smbserver.h"

#define ERR_SMB 0xC0110u                /* ROSGD's, in the application range */
#define TOOLS "/usr/sbin/ksmbd.tools"
#define CONF_DIR "/etc/ksmbd"
#define CONF CONF_DIR "/ksmbd.conf"
#define PWDDB CONF_DIR "/ksmbdpwd.db"
#define MAX_SHARES 32

extern char **environ;

static struct share {
    char name[64], riscos[256], linux_path[PATH_MAX];
    int readonly, guest, used;
} shares[MAX_SHARES];

static pid_t mountd;
static int started_guests;              /* whether any share was for guests as mountd started */

/* ---- ksmbd-tools --------------------------------------------------------------- */

/* Runs ksmbd.tools under one of its names. Returns its pid, or -errno. With
 * a terminal's name, the tool reads and writes that terminal in place of
 * /dev/null and the log */
static pid_t tool_on(const char *as, char *const args[], const char *tty)
{
    char *argv[16];
    int n = 0;
    argv[n++] = (char *)as;
    for (int i = 0; args && args[i] && n < 15; i++)
        argv[n++] = args[i];
    argv[n] = NULL;
    mkdir("/run", 0755);                /* its log, and ksmbd's lock */
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    if (tty) {
        posix_spawn_file_actions_addopen(&fa, 0, tty, O_RDWR | O_NOCTTY, 0);
        posix_spawn_file_actions_adddup2(&fa, 0, 1);
        posix_spawn_file_actions_addopen(&fa, 2, "/run/ksmbd.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/run/ksmbd.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    int e = posix_spawn(&pid, TOOLS, &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    return e ? -e : pid;
}

static pid_t tool(const char *as, char *const args[])
{
    return tool_on(as, args, NULL);
}

static int tool_wait(const char *as, char *const args[])
{
    pid_t pid = tool(as, args);
    if (pid < 0)
        return -1;
    int st = 0;
    pid_t r;
    do
        ROS_BLOCKING(r = waitpid(pid, &st, 0));
    while (r < 0 && errno == EINTR);
    return r == pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The interfaces the server listens on, as ksmbd.conf names them. By
 * default these are the loopback and every other interface that is up and
 * running, which are the ones the box can be reached on. The system
 * variable SMBServer$Interfaces, a list of interface names, replaces that.
 * ksmbd binds a named interface when it comes up, so a name need not be up
 * yet. */
static char ifaces[160];

static int ifname_ok(const char *s, size_t n)
{
    if (n == 0 || n >= IFNAMSIZ)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z') || s[i] == '.' || s[i] == '-' || s[i] == '_'))
            return 0;
    return 1;
}

/* A string system variable, GSTrans'd: 1 if it is set */
static int read_var(const char *name, char *out, size_t max)
{
    char *b = ros_rma_alloc(1100);
    if (!b)
        return 0;
    snprintf(b + 1024, 76, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b + 1024), c.r[1] = ros_addr(b), c.r[2] = 1023, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    int got = !c.v && c.r[2] < 1023;
    if (got)
        snprintf(out, max, "%.*s", (int)c.r[2], b);
    ros_rma_free(b);
    return got && out[0];
}

/* Whether the name is one of the chosen interfaces */
static int iface_listed(const char *name, size_t n)
{
    for (const char *p = ifaces; *p;) {
        size_t k = strcspn(p, " ");
        if (k == n && !strncmp(p, name, n))
            return 1;
        p += k + (p[k] != 0);
    }
    return 0;
}

static void add_iface(const char *name, size_t n)
{
    size_t have = strlen(ifaces);
    if (!ifname_ok(name, n) || have + n + 2 > sizeof ifaces || iface_listed(name, n))
        return;
    snprintf(ifaces + have, sizeof ifaces - have, "%s%.*s", have ? " " : "", (int)n, name);
}

static void choose_interfaces(void)
{
    char var[sizeof ifaces];
    ifaces[0] = 0;
    if (read_var("SMBServer$Interfaces", var, sizeof var)) {
        for (const char *p = var; *p;) {
            size_t n = strcspn(p, " ,");
            if (n)
                add_iface(p, n);
            p += n + (p[n] != 0);
        }
        if (ifaces[0])
            return;                     /* none of the names was usable: the default */
    }
    add_iface("lo", 2);
    struct ifaddrs *list, *a;
    if (getifaddrs(&list) != 0)
        return;
    for (a = list; a; a = a->ifa_next)
        if (a->ifa_addr && (a->ifa_flags & IFF_UP) && (a->ifa_flags & IFF_RUNNING) &&
            !(a->ifa_flags & IFF_LOOPBACK) && (a->ifa_addr->sa_family == AF_INET || a->ifa_addr->sa_family == AF_INET6))
            add_iface(a->ifa_name, strlen(a->ifa_name));
    freeifaddrs(list);
}

/* ksmbd.adduser with a password. The password is not an argument, where
 * every process could read it in /proc. ksmbd.adduser asks for it on its
 * terminal, twice, so it is given a pseudo-terminal of its own and the
 * password is typed to it as soon as the prompt appears. Returns the tool's
 * exit status, or -1 */
static int adduser_with_password(char *const args[], const char *password)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    char name[64];
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0 || ptsname_r(m, name, sizeof name) != 0) {
        if (m >= 0)
            close(m);
        return -1;
    }
    pid_t pid = tool_on("ksmbd.adduser", args, name);
    if (pid < 0) {
        close(m);
        return -1;
    }
    char typed[2 * 129 + 4], seen[256];
    int len = snprintf(typed, sizeof typed, "%s\n%s\n", password, password);
    size_t have = 0;
    int sent = 0, st = 0, done = 0;
    for (int i = 0; i < 200 && !done; i++) {            /* up to 20 s */
        struct pollfd p = { .fd = m, .events = POLLIN };
        if (!sent && poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(m, seen + have, sizeof seen - 1 - have);
            if (n > 0)
                seen[have += (size_t)n] = 0;
            /* The tool clears what is waiting to be read as it switches
             * the terminal's echo off, just before it asks. Typing only
             * after "password: " cannot be lost. */
            if (strstr(seen, "password: ") && write(m, typed, (size_t)len) == len)
                sent = 1;
            else if (have >= sizeof seen - 1)
                have = 0;
        } else if (sent) {
            ROS_BLOCKING(usleep(100000));
        }
        done = waitpid(pid, &st, WNOHANG) == pid;
    }
    memset(typed, 0, sizeof typed);
    close(m);
    if (!done) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Whether ksmbd answers on port 445 yet. It starts listening only once
 * mountd has told the kernel of the configuration. It is asked on the
 * loopback. The box reaches its own addresses through the loopback too, so
 * the other interfaces cannot be asked from here. If the loopback is not
 * one of the chosen interfaces, there is nothing to ask and a short wait
 * stands in for it. */
static int listening(void)
{
    if (!iface_listed("lo", 2)) {
        ROS_BLOCKING(usleep(1000000));
        return 1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(445),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int ok = fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    if (fd >= 0)
        close(fd);
    return ok;
}

static int running(void)
{
    if (mountd > 0 && waitpid(mountd, NULL, WNOHANG) == mountd)
        mountd = 0;
    return mountd > 0;
}

static int any_guest(void)
{
    int guests = 0;
    for (int i = 0; i < MAX_SHARES; i++)
        guests |= shares[i].used && shares[i].guest;
    return guests;
}

static void stop(void);

static os_error *write_conf(void)
{
    mkdir("/etc", 0755);
    mkdir(CONF_DIR, 0700);
    chmod(CONF_DIR, 0700);              /* it holds the users' password hashes */
    int cfd = open(CONF, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f = cfd >= 0 ? fdopen(cfd, "w") : NULL;
    if (cfd >= 0 && !f)
        close(cfd);
    if (!f)
        return ros_error(ERR_SMB, "SMBServer cannot write %s: %s", CONF, strerror(errno));
    int guests = any_guest();
    if (!running())
        choose_interfaces();            /* ksmbd reads them only as it starts */
    fprintf(f, "; Written by SMBServer: the box's shares (*Share, *UnShare)\n"
               "[global]\n"
               "\tnetbios name = ROSGD\n"
               "\tserver string = RISC OS on ROSGD\n"
               "\tworkgroup = WORKGROUP\n"
               "\tguest account = nobody\n"
               "\t; ksmbd signs only when the client asks, unless told it must\n"
               "\tserver signing = mandatory\n"
               "\t; only the interfaces chosen, not every address the box has\n"
               "\tinterfaces = %s\n"
               "\tbind interfaces only = yes\n"
               "\tmap to guest = %s\n",
            ifaces, guests ? "bad user" : "never");
    for (int i = 0; i < MAX_SHARES; i++) {
        if (!shares[i].used)
            continue;
        fprintf(f, "\n[%s]\n"
                   "\tcomment = %s\n"
                   "\tpath = %s\n"
                   "\tread only = %s\n"
                   "\tguest ok = %s\n"
                   "\t; the host share's files cannot all carry DOS attributes\n"
                   "\tstore dos attributes = no\n"
                   "\t; the box's files are root's, as all of RISC OS is: who may\n"
                   "\t; connect is *SMBUser's to say, not Linux's. The box's own\n"
                   "\t; directories are root's alone to write, so an unprivileged\n"
                   "\t; user could not save in a share of them\n"
                   "\tforce user = root\n"
                   "\tforce group = root\n"
                   "\t; a link is never followed out of the share\n"
                   "\tfollow symlinks = no\n"
                   "\t; the SSH keys: a share of the host share's top holds them, and\n"
                   "\t; a writer of authorized_keys would be root on the box\n"
                   "\tveto files = /*.ssh*/\n",
                shares[i].name, shares[i].riscos, shares[i].linux_path,
                shares[i].readonly ? "yes" : "no", shares[i].guest ? "yes" : "no");
    }
    fclose(f);
    if (access(PWDDB, F_OK) != 0) {
        int fd = open(PWDDB, O_WRONLY | O_CREAT, 0600);
        if (fd >= 0)
            close(fd);
    }
    chmod(CONF, 0600);
    chmod(PWDDB, 0600);
    return NULL;
}

/* The server told of the shares: started, or reloaded */
static os_error *apply(void)
{
    /* ksmbd reads "map to guest" as it starts: a guest could not connect to
     * a share added after the first, which had none for guests */
    if (running() && any_guest() != started_guests)
        stop();
    os_error *e = write_conf();
    if (e)
        return e;
    if (running()) {
        char *args[] = { "--reload", NULL };
        return tool_wait("ksmbd.control", args) == 0
                   ? NULL
                   : ros_error(ERR_SMB, "SMBServer: ksmbd did not take the new shares");
    }
    char *args[] = { "-n", NULL };      /* in the foreground: /init's child */
    pid_t pid = tool("ksmbd.mountd", args);
    if (pid < 0)
        return ros_error(ERR_SMB, "SMBServer cannot run ksmbd.mountd: %s", strerror((int)-pid));
    for (int i = 0; i < 50 && !listening(); i++) {         /* up to 5 s */
        ROS_BLOCKING(usleep(100000));
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return ros_error(ERR_SMB, "SMBServer: ksmbd.mountd stopped (see /run/ksmbd.log)");
    }
    mountd = pid;
    started_guests = any_guest();
    return NULL;
}

static void stop(void)
{
    if (!running())
        return;
    char *args[] = { "--shutdown", NULL };
    tool_wait("ksmbd.control", args);
    kill(mountd, SIGTERM);
    ROS_BLOCKING(waitpid(mountd, NULL, 0));
    mountd = 0;
}

/* ---- from C ------------------------------------------------------------------------ */

static struct share *find(const char *name)
{
    for (int i = 0; i < MAX_SHARES; i++)
        if (shares[i].used && !strcasecmp(shares[i].name, name))
            return &shares[i];
    return NULL;
}

/* Whether a path can be written into ksmbd.conf as it stands. A control
 * character would end the line, and ksmbd.mountd ends a value at ';' or '#',
 * which would share the directory above in its place. Spaces at either end
 * are trimmed by the reader. */
static int conf_safe(const char *s)
{
    if (!*s || *s == ' ' || s[strlen(s) - 1] == ' ')
        return 0;
    for (; *s; s++)
        if ((unsigned char)*s < ' ' || *s == 0x7F || *s == ';' || *s == '#')
            return 0;
    return 1;
}

static int good_name(const char *s)
{
    if (!*s || strlen(s) >= sizeof shares[0].name || *s == ' ' || s[strlen(s) - 1] == ' ')
        return 0;
    if (!strcasecmp(s, "global") || !strcasecmp(s, "ipc$"))  /* ksmbd.conf's own sections */
        return 0;
    if (!strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    for (; *s; s++)
        if (strchr("[]\\/:;#|=,+*?<>\"", *s) || (unsigned char)*s < ' ' || *s == 0x7F)
            return 0;
    return 1;
}

os_error *smb_share(const char *dir, const char *name, int readonly, int guest, int guest_write)
{
    if (guest && !readonly && !guest_write)
        return ros_error(ERR_SMB, "A share for guests must be read only, unless -write is given");
    if (getpid() != 1)
        return ros_error(ERR_SMB, "Sharing runs in the box: it needs ksmbd");
    char path[PATH_MAX];
    os_error *e = ros_hostfs_linux_path(dir, path, sizeof path);
    if (e)
        return e;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return ros_error(ERR_SMB, "%s is not a directory", dir);
    if (strlen(dir) >= sizeof shares[0].riscos)
        return ros_error(ERR_SMB, "The directory's name is too long");
    if (!conf_safe(path) || !conf_safe(dir))
        return ros_error(ERR_SMB, "%s has a character in its name that a share cannot have", dir);
    char leaf[64];
    if (!name) {                        /* the directory's own name */
        const char *l = strrchr(dir, '.');
        l = l ? l + 1 : dir;
        if (*l == '$' || !*l)
            l = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        snprintf(leaf, sizeof leaf, "%s", *l ? l : "Box");
        name = leaf;
    }
    if (!good_name(name))
        return ros_error(ERR_SMB, "Bad share name '%s'", name);
    struct share *s = find(name);
    for (int i = 0; i < MAX_SHARES && !s; i++)
        if (!shares[i].used)
            s = &shares[i];
    if (!s)
        return ros_error(ERR_SMB, "Too many shares");
    struct share keep = *s;
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->riscos, sizeof s->riscos, "%s", dir);
    snprintf(s->linux_path, sizeof s->linux_path, "%s", path);
    s->readonly = readonly, s->guest = guest, s->used = 1;
    e = apply();
    if (e) {
        *s = keep;
        write_conf();                   /* the file as the table is again */
    }
    return e;
}

os_error *smb_unshare(const char *name)
{
    struct share *s = find(name);
    if (!s)
        return ros_error(ERR_SMB, "No share called '%s'", name);
    s->used = 0;
    int any = 0;
    for (int i = 0; i < MAX_SHARES; i++)
        any |= shares[i].used;
    if (!any) {                         /* nothing left to serve */
        stop();
        return write_conf();
    }
    return running() ? apply() : NULL;
}

os_error *smb_user(const char *user, const char *password)
{
    if (getpid() != 1)
        return ros_error(ERR_SMB, "Sharing runs in the box: it needs ksmbd");
    if (!*user || *user == '-' || strlen(user) > 32 || strpbrk(user, ":\\/ "))
        return ros_error(ERR_SMB, "Bad user name '%s'", user);
    if (password && (!*password || strlen(password) > 128 || strchr(password, 0x7F)))
        return ros_error(ERR_SMB, "Bad password: 1 to 128 characters, none of them DEL");
    os_error *e = write_conf();         /* the database's directory */
    if (e)
        return e;
    int r;
    if (password) {
        char *add[] = { "-P", PWDDB, "-a", (char *)user, NULL };
        char *upd[] = { "-P", PWDDB, "-u", (char *)user, NULL };
        r = adduser_with_password(upd, password);
        if (r != 0)
            r = adduser_with_password(add, password);
    } else {
        char *del[] = { "-P", PWDDB, "-d", (char *)user, NULL };
        r = tool_wait("ksmbd.adduser", del);
    }
    chmod(PWDDB, 0600);                 /* the tool may have written a new file */
    if (r != 0)
        return ros_error(ERR_SMB, "SMBServer could not %s the user %s", password ? "set" : "remove", user);
    if (running()) {
        char *args[] = { "--reload", NULL };
        tool_wait("ksmbd.control", args);
    }
    return NULL;
}

int smb_running(void)
{
    return running();
}

/* ---- the commands -------------------------------------------------------------------- */

static int words(uint32_t tail, char *buf, size_t size, char *argv[], int max)
{
    const char *t = ros_ptr(tail);
    int argc = 0;
    size_t n = 0;
    while ((unsigned char)*t >= ' ' && argc < max && n < size - 1) {
        while (*t == ' ')
            t++;
        if ((unsigned char)*t < ' ')
            break;
        argv[argc++] = buf + n;
        int quoted = *t == '"';
        if (quoted)
            t++;
        while ((unsigned char)*t >= ' ' && (quoted ? *t != '"' : *t != ' ') && n < size - 1)
            buf[n++] = *t++;
        if (quoted && *t == '"')
            t++;
        buf[n++] = 0;
    }
    return argc;
}

static os_error *cmd_share(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char buf[512], *argv[6];
    int n = words(tail, buf, sizeof buf, argv, 6), ro = 0, guest = 0, write = 0;
    const char *dir = NULL, *name = NULL;
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(argv[i], "-readonly"))
            ro = 1;
        else if (!strcasecmp(argv[i], "-guest"))
            guest = 1;
        else if (!strcasecmp(argv[i], "-write"))
            write = 1;
        else if (argv[i][0] == '-')
            return ros_error(ERR_SMB, "Syntax: *Share <directory> [<name>] [-readonly] [-guest [-write]]");
        else if (!dir)
            dir = argv[i];
        else if (!name)
            name = argv[i];
    }
    if (!dir || (write && ro) || (write && !guest))
        return ros_error(ERR_SMB, "Syntax: *Share <directory> [<name>] [-readonly] [-guest [-write]]");
    if (guest && !write)
        ro = 1;                         /* anyone may read, only -write lets them write */
    return smb_share(dir, name, ro, guest, write);
}

static os_error *cmd_unshare(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char buf[128], *argv[1];
    if (words(tail, buf, sizeof buf, argv, 1) < 1)
        return ros_error(ERR_SMB, "Syntax: *UnShare <name>");
    return smb_unshare(argv[0]);
}

static os_error *cmd_shares(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    char line[1200];
    int any = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (!shares[i].used)
            continue;
        snprintf(line, sizeof line, "%-16s %s%s%s\r\n", shares[i].name, shares[i].riscos,
                 shares[i].readonly ? " (read only)" : "", shares[i].guest ? " (guests)" : "");
        xos_write0(line, NULL);
        any = 1;
    }
    xos_write0(any ? (running() ? "Served by ksmbd, on port 445\r\n" : "ksmbd is not running\r\n")
                   : "No shares\r\n",
               NULL);
    return NULL;
}

static os_error *cmd_smbuser(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char buf[256], *argv[2];
    int n = words(tail, buf, sizeof buf, argv, 2);
    if (n < 1)
        return ros_error(ERR_SMB, "Syntax: *SMBUser <user> [<password>]");
    return smb_user(argv[0], n > 1 ? argv[1] : NULL);
}

#define C(n, lo, hi, syntax, help, fn) { n, ROS_CMD_INFO(lo, hi, 0, 0), "Syntax: *" syntax, help, fn }

static const struct ros_command commands[] = {
    C("Share", 1, 5, "Share <directory> [<name>] [-readonly] [-guest [-write]]",
      "*Share shares a directory over SMB (ksmbd), as <name> or its own name; -readonly lets no one "
      "write; -guest lets anyone in without a password, to read only unless -write is given as well.",
      cmd_share),
    C("Shares", 0, 0, "Shares", "*Shares lists the directories shared.", cmd_shares),
    C("SMBUser", 1, 2, "SMBUser <user> [<password>]",
      "*SMBUser lets a user connect to the shares with a password, or with no password given, "
      "no longer.", cmd_smbuser),
    C("UnShare", 1, 1, "UnShare <name>", "*UnShare stops sharing a directory.", cmd_unshare),
    { 0 },
};

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    stop();
    return NULL;
}

struct ros_module smbserver_module = {
    .title = "SMBServer",
    .help = "SMBServer\t1.00 (26 Sep 2026) ROSGD native: directories shared over SMB3, by ksmbd",
    .final = final,
    .commands = commands,
};
