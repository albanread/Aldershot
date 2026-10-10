/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* sshd.c -- SSHD, a native module: an SSH server for the box, whose logins
 * are RISC OS command lines.
 *
 * RISC OS has no SSH server. ROSGD's is OpenSSH's sshd, a Linux daemon that
 * /init starts and stops (*SSHD), with RISC OS behind it. Root's login
 * shell is riscos-cli (relay.c). It joins the session to a command line of
 * its own inside the personality (session.c). That is a second CLI, a task
 * beside the console's. "ssh -p 2222 root@127.0.0.1" from the Mac, through
 * QEMU's forwarded port, gives a "*" prompt. "ssh ... <command>" runs one
 * command. sftp and scp reach the box's files through sshd's own sftp.
 *
 *   *SSHD [start] [-p <port>]   sshd on the port (22). Its host key is in
 *                               ~/.ssh, which is the host share's .ssh, so
 *                               it stays the same from boot to boot.
 *                               ~/.ssh/authorized_keys holds the keys that
 *                               may log in, as root. There are no passwords.
 *   *SSHD stop                  sshd ended; sessions open run on
 *   *SSHD status
 *
 * The host key is made the first time, by ssh-keygen. *SSHD prints its
 * fingerprint, for the first connection's question.
 */
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "sshd.h"

#define ERR_SSHD 0xC0100u               /* ROSGD's, in the application range, as PTY's are */

extern char **environ;

static pid_t daemon_pid;
static int daemon_port;

/* ---- programs ------------------------------------------------------------------ */

/* Runs a program with its standard streams on /dev/null. Returns its pid, or -errno */
static pid_t spawn(char *const argv[])
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    int e = posix_spawn(&pid, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    return e ? -e : pid;
}

static int run_wait(char *const argv[])
{
    pid_t pid = spawn(argv);
    if (pid < 0)
        return -1;
    int st = 0;
    ROS_BLOCKING(waitpid(pid, &st, 0));
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* SHA256:<base64>, as ssh-keygen -l prints it, of a .pub file's key */
static int fingerprint(const char *pub, char *out, size_t size)
{
    char line[2048];
    FILE *f = fopen(pub, "r");
    if (!f)
        return -1;
    int got = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    char *b64 = got ? strchr(line, ' ') : NULL;
    if (!b64)
        return -1;
    b64++;
    size_t n = strcspn(b64, " \r\n");
    unsigned char blob[1536], md[32], enc[64];
    if (n < 4 || n > 2040 || n % 4)
        return -1;
    int len = EVP_DecodeBlock(blob, (unsigned char *)b64, (int)n);
    if (len < 0)
        return -1;
    len -= (b64[n - 1] == '=') + (b64[n - 2] == '=');
    unsigned int mdlen = 0;
    EVP_Digest(blob, (size_t)len, md, &mdlen, EVP_sha256(), NULL);
    int el = EVP_EncodeBlock(enc, md, (int)mdlen);
    while (el > 0 && enc[el - 1] == '=')
        el--;
    snprintf(out, size, "SHA256:%.*s", el, enc);
    return 0;
}

static int count_keys(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof line, f))
        n += line[0] != '#' && line[0] != '\n' && line[0] != 0;
    fclose(f);
    return n;
}

/* ---- the daemon ------------------------------------------------------------------ */

int sshd_running(void)
{
    if (daemon_pid > 0 && waitpid(daemon_pid, NULL, WNOHANG) == daemon_pid)
        daemon_pid = 0, daemon_port = 0;
    return daemon_pid > 0 ? daemon_port : 0;
}

os_error *sshd_start(const struct sshd_options *o, char *report, size_t size)
{
    if (getpid() != 1)
        return ros_error(ERR_SSHD, "The SSH server runs in the box: it needs /init's sshd");
    if (sshd_running())
        return ros_error(ERR_SSHD, "SSHD is running already, on port %d", daemon_port);
    mkdir("/run", 0755);
    int e = sshd_listen(SSHD_SOCKET);
    if (e)
        return ros_error(ERR_SSHD, "SSHD cannot listen at %s: %s", SSHD_SOCKET, strerror(-e));
    mkdir("/var", 0755);
    mkdir("/var/empty", 0755);               /* sshd-auth's chroot */
    mkdir("/etc/ssh", 0755);
    mkdir(o->keydir, 0700);

    char key[512], pub[520], cfg[64], log[64];
    snprintf(key, sizeof key, "%s/ssh_host_ed25519_key", o->keydir);
    snprintf(pub, sizeof pub, "%s.pub", key);
    /* A key that is there but empty is no key. The box can lose its power
     * between ssh-keygen making the file and the disc taking its contents.
     * That leaves an empty key, and sshd then exits ("no hostkeys
     * available"). On a machine reached only over SSH, that means it cannot
     * be reached at all. So the file must have something in it, or it is
     * made again. */
    struct stat ks;
    if (access(key, R_OK) != 0 || stat(key, &ks) != 0 || ks.st_size < 64) {
        unlink(key);
        unlink(pub);
        char *kg[] = { "/usr/bin/ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "rosgd box", "-f", key, NULL };
        if (run_wait(kg) != 0)
            return ros_error(ERR_SSHD, "SSHD cannot make its host key, %s", key);
    }
    snprintf(cfg, sizeof cfg, "/etc/ssh/sshd_config.%d", o->port);
    snprintf(log, sizeof log, "/run/sshd.%d.log", o->port);
    FILE *f = fopen(cfg, "w");
    if (!f)
        return ros_error(ERR_SSHD, "SSHD cannot write %s: %s", cfg, strerror(errno));
    fprintf(f,
            "# Written by *SSHD: sshd for ROSGD's box, whose logins are RISC OS command lines\n"
            "Port %d\n"
            "HostKey %s\n"
            "AuthorizedKeysFile %s\n"
            "PermitRootLogin prohibit-password\n"
            "PasswordAuthentication no\n"
            "KbdInteractiveAuthentication no\n"
            "# the share's files are the host's user's, not root's\n"
            "StrictModes no\n"
            "PidFile none\n"
            "PrintMotd no\n"
            "Subsystem sftp internal-sftp\n",
            o->port, key, o->authorized);
    fclose(f);

    char *argv[] = { "/usr/sbin/sshd", "-D", "-f", cfg, "-E", log, NULL };
    unlink(log);
    pid_t pid = spawn(argv);
    if (pid < 0)
        return ros_error(ERR_SSHD, "SSHD cannot run sshd: %s", strerror((int)-pid));
    ROS_BLOCKING(usleep(300000));       /* a bad configuration ends it at once */
    int st;
    if (waitpid(pid, &st, WNOHANG) == pid) {
        char last[200] = "no reason given";
        FILE *l = fopen(log, "r");
        for (char line[200]; l && fgets(line, sizeof line, l);)
            snprintf(last, sizeof last, "%.*s", (int)strcspn(line, "\r\n"), line);
        if (l)
            fclose(l);
        return ros_error(ERR_SSHD, "sshd stopped: %s", last);
    }
    daemon_pid = pid, daemon_port = o->port;
    char fp[80] = "(none)";
    fingerprint(pub, fp, sizeof fp);
    int keys = count_keys(o->authorized);
    snprintf(report, size,
             "SSHD: sshd listening on port %d\r\n"
             "  host key %s\r\n"
             "  %d key%s may log in as root, from %s\r\n",
             o->port, fp, keys, keys == 1 ? "" : "s", o->authorized);
    return NULL;
}

os_error *sshd_stop(void)
{
    if (!sshd_running())
        return ros_error(ERR_SSHD, "SSHD is not running");
    kill(daemon_pid, SIGTERM);
    ROS_BLOCKING(waitpid(daemon_pid, NULL, 0));
    daemon_pid = 0, daemon_port = 0;
    return NULL;
}

/* ---- the command ------------------------------------------------------------------ */

static const char *home(void)
{
    struct passwd *pw = getpwuid(0);
    return pw && pw->pw_dir ? pw->pw_dir : "/root";
}

static void say(const char *s)
{
    xos_write0(s, NULL);
}

static os_error *cmd_sshd(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char words[256];
    const char *t = ros_ptr(tail);
    size_t n = 0;
    while ((uint8_t)t[n] >= ' ' && n < sizeof words - 1)
        words[n] = t[n], n++;
    words[n] = 0;
    char *action = "start", *save = NULL;
    int port = 22;
    for (char *w = strtok_r(words, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (!strcmp(w, "-p")) {
            char *v = strtok_r(NULL, " ", &save);
            port = v ? atoi(v) : 0;
            if (port < 1 || port > 65535)
                return ros_error(ERR_SSHD, "Syntax: *SSHD [start | stop | status] [-p <port>]");
        } else if (!strcasecmp(w, "start") || !strcasecmp(w, "stop") || !strcasecmp(w, "status")) {
            action = w;
        } else {
            return ros_error(ERR_SSHD, "Syntax: *SSHD [start | stop | status] [-p <port>]");
        }
    }
    if (!strcasecmp(action, "stop")) {
        os_error *e = sshd_stop();
        if (!e)
            say("SSHD: sshd stopped\r\n");
        return e;
    }
    if (!strcasecmp(action, "status")) {
        char line[160];
        int p = sshd_running();
        if (p)
            snprintf(line, sizeof line, "SSHD: sshd listening on port %d; %d session%s open\r\n", p,
                     sshd_session_count(), sshd_session_count() == 1 ? "" : "s");
        else
            snprintf(line, sizeof line, "SSHD: not running\r\n");
        say(line);
        return NULL;
    }
    char keydir[512], authorized[560], report[1024];
    snprintf(keydir, sizeof keydir, "%s/.ssh", home());
    snprintf(authorized, sizeof authorized, "%s/authorized_keys", keydir);
    struct sshd_options o = { port, keydir, authorized };
    os_error *e = sshd_start(&o, report, sizeof report);
    if (!e)
        say(report);
    return e;
}

static const struct ros_command commands[] = {
    { "SSHD", ROS_CMD_INFO(0, 3, 0, 0), "Syntax: *SSHD [start | stop | status] [-p <port>]",
      "*SSHD starts OpenSSH's sshd in the box, on port 22 (or -p's); its logins, as root with a key "
      "from ~/.ssh/authorized_keys, are RISC OS command lines. *SSHD stop ends it; *SSHD status "
      "says whether it runs.", cmd_sshd },
    { 0 },
};

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    if (sshd_running())
        sshd_stop();
    sshd_unlisten();
    return NULL;
}

struct ros_module sshd_module = {
    .title = "SSHD",
    .help = "SSHD\t1.00 (26 Sep 2026) ROSGD native: OpenSSH's sshd, its logins RISC OS command lines",
    .final = final,
    .commands = commands,
};
