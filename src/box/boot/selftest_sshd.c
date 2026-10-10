/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_sshd.c: SSHD (modules/sshd): RISC OS command lines for SSH
 * sessions.
 *
 * Everywhere: a client on /init's socket, as riscos-cli is, runs one command
 * and gets its status. An error gives its status. An interactive command
 * line is checked with its prompt, a variable set and read back, VDU codes
 * as ANSI, UTF-8 typed as Latin-1, Escape and Ctrl-D. The test is the
 * console's task, so it lets the sessions run as the console does while it
 * waits (ros_streams_idle).
 *
 * In the box: sshd itself, on port 2022 with keys of the test's own in
 * /tmp, and OpenSSH's ssh logging in to it and running a command. That
 * runs ssh, sshd, riscos-cli and the command line end to end.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "pty.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "selftest.h"
#include "sshd.h"

#define check ros_check

static char sock[108];

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* A moment for background work and the sessions, as the console gives */
static void tick(void)
{
    ROS_BLOCKING(usleep(5000));
    ros_streams_idle();
}

static int client(const char *head)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    send(fd, head, strlen(head), 0);
    return fd;
}

static void sends(int fd, const char *s)
{
    send(fd, s, strlen(s), 0);
}

/* What the session writes, until want appears (or it ends): 1 if it did */
static int until(int fd, char *got, size_t size, size_t *n, const char *want, int *status)
{
    uint64_t end = now_ms() + 5000;
    while (now_ms() < end) {
        tick();
        ssize_t r = recv(fd, got + *n, size - 1 - *n, 0);
        if (r > 0) {
            *n += (size_t)r;
            got[*n] = 0;
            char *ff = memchr(got, 0xFF, *n);
            if (ff && ff + 1 < got + *n) {
                *status = (unsigned char)ff[1];
                *ff = 0;
                return !want || strstr(got, want);
            }
        } else if (r == 0) {
            return 0;
        }
        if (want && strstr(got, want))
            return 1;
    }
    return 0;
}

static void socket_sessions(void)
{
    char got[4096];
    size_t n = 0;
    int st = -1;
    int fd = client("ROSGD-CLI 1 C 80 24\nEcho hello there\n");
    int ok = fd >= 0 && until(fd, got, sizeof got, &n, NULL, &st);
    check(ok && st == 0 && !strcmp(got, "hello there\n\r"),
          "SSHD session -- one command (*Echo), its output, status 0", "status %d, \"%.40s\"", st, got);
    if (fd >= 0)
        close(fd);

    n = 0, st = -1, got[0] = 0;
    fd = client("ROSGD-CLI 1 C 80 24\nNoSuchCommandHere\n");
    ok = fd >= 0 && until(fd, got, sizeof got, &n, NULL, &st);
    check(ok && st == 1 && strlen(got) > 4, "SSHD session -- a command's error: its message, status 1",
          "status %d, \"%.60s\"", st, got);
    if (fd >= 0)
        close(fd);

    n = 0, st = -1, got[0] = 0;
    fd = client("ROSGD-CLI 1 I 80 24\n");
    ok = fd >= 0 && until(fd, got, sizeof got, &n, "\n\r*", &st);
    check(ok && strstr(got, "RISC OS on ROSGD"), "SSHD session -- an interactive command line: banner, prompt",
          "\"%.60s\"", got);
    sends(fd, "Set SSHDTest$Var yes\r");
    ok = until(fd, got, sizeof got, &n, "yes\n\r*", &st);
    n = 0, got[0] = 0;
    sends(fd, "Echo <SSHDTest$Var>\r");
    ok = ok && until(fd, got, sizeof got, &n, ">\n\ryes\n\r*", &st);
    check(ok, "SSHD session -- a line read (echoed), *Set, then the variable back", "\"%.80s\"",
          n > 80 ? got + n - 80 : got);
    n = 0, got[0] = 0;
    sends(fd, "Echo |_|E|B\xC3\xA9|G\r");        /* TAB(5,2), e acute, a bell */
    ok = until(fd, got, sizeof got, &n, "\033[3;6H\xC3\xA9\a", &st);
    check(ok, "SSHD session -- VDU 31 as ANSI's cursor address, Latin-1 as UTF-8 both ways, VDU 7",
          "\"%.40s\"", got);
    n = 0, got[0] = 0;
    sends(fd, "abc\033");
    ok = until(fd, got, sizeof got, &n, "Escape\n\r*", &st);
    sends(fd, "\x04");
    int ended = until(fd, got, sizeof got, &n, NULL, &st);
    check(ok && ended && st == 0, "SSHD session -- ESC is Escape at the prompt; Ctrl-D ends it",
          "\"%.40s\", status %d", got, st);
    if (fd >= 0)
        close(fd);
    for (int i = 0; i < 20 && sshd_session_count(); i++)
        tick();
    check(sshd_session_count() == 0, "SSHD session -- every session's task gone at its end", "%d open",
          sshd_session_count());

    /* A UTF-8 lead byte sent alone, then a buffer's worth of text: the keys
     * must not run past the end of the session's input buffer (visible
     * under the address sanitizer) */
    n = 0, st = -1, got[0] = 0;
    fd = client("ROSGD-CLI 1 I 80 24\n");
    ok = fd >= 0 && until(fd, got, sizeof got, &n, "\n\r*", &st);
    sends(fd, "\xC2");
    for (int i = 0; i < 4; i++)
        tick();
    char flood[297];
    memset(flood, 'a', 296);
    flood[296] = 0;
    sends(fd, flood);
    for (int i = 0; i < 4; i++)
        tick();
    sends(fd, "\r");
    n = 0, got[0] = 0;
    ok = ok && until(fd, got, sizeof got, &n, "\n\r*", &st);
    check(ok, "SSHD session -- a UTF-8 lead byte, then 296 characters, are kept within the input buffer", "\"%.40s\"",
          got);
    sends(fd, "\x04");
    until(fd, got, sizeof got, &n, NULL, &st);
    if (fd >= 0)
        close(fd);
    for (int i = 0; i < 20 && sshd_session_count(); i++)
        tick();

    /* A header that never ends is cut off, not kept for ever */
    char huge[6000];
    memset(huge, 'x', sizeof huge - 1);
    huge[sizeof huge - 1] = 0;
    fd = client("ROSGD-CLI 1 C 80 24\n");
    if (fd >= 0)
        sends(fd, huge);
    for (int i = 0; i < 20 && sshd_session_count(); i++)
        tick();
    check(fd >= 0 && sshd_session_count() == 0, "SSHD session -- a command line that never ends is dropped",
          "%d open", sshd_session_count());
    if (fd >= 0)
        close(fd);
}

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

static void through_sshd(void)
{
    const char *dir = "/tmp/sshd-test";
    mkdir(dir, 0700);
    unlink("/tmp/sshd-test/id"), unlink("/tmp/sshd-test/id.pub");
    int e = cli("SSH-KeyGen -q -t ed25519 -N \"\" -C selftest -f /tmp/sshd-test/id");
    FILE *in = fopen("/tmp/sshd-test/id.pub", "r"), *out = fopen("/tmp/sshd-test/authorized_keys", "w");
    char line[512] = "";
    if (in && fgets(line, sizeof line, in) && out)
        fputs(line, out);
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    char report[512] = "";
    struct sshd_options o = { 2022, dir, "/tmp/sshd-test/authorized_keys" };
    os_error *se = e ? NULL : sshd_start(&o, report, sizeof report);
    check(!e && !se && sshd_running() == 2022 && strstr(report, "SHA256:") &&
              strstr(report, "1 key may log in"),
          "*SSHD -- sshd on a port, its host key made, its fingerprint, the keys that may log in",
          "%s", se ? se->errmess : report);

    struct pty *p = NULL;
    os_error *pe = se ? se : pty_open("ssh -i /tmp/sshd-test/id -p 2022 -o StrictHostKeyChecking=no "
                                      "-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "
                                      "root@127.0.0.1 Echo from RISC OS, over SSH",
                                      80, 24, &p);
    char got[2048] = "";
    size_t n = 0;
    int status = -1;
    uint64_t end = now_ms() + 20000;
    while (!pe && now_ms() < end) {
        tick();
        long r = pty_read(p, got + n, sizeof got - 1 - n);
        n += r > 0 ? (size_t)r : 0;
        got[n] = 0;
        if (pty_ended(p, &status))
            break;
    }
    if (p)
        pty_close(p);
    char last[200] = "";                /* sshd's say, if it went wrong */
    FILE *log = fopen("/run/sshd.2022.log", "r");
    for (char l[200]; log && fgets(l, sizeof l, log);)
        snprintf(last, sizeof last, "%.*s", (int)strcspn(l, "\r\n"), l);
    if (log)
        fclose(log);
    check(!pe && status == 0 && strstr(got, "from RISC OS, over SSH"),
          "SSHD -- OpenSSH's ssh logs in with a key, riscos-cli runs the command in RISC OS",
          "%s, status %d, \"%.80s\"; sshd: %s", pe ? pe->errmess : "", status, got, last);
    if (!se)
        sshd_stop();
    check(!sshd_running(), "*SSHD stop -- sshd ended", "");
}

void ros_selftest_sshd(void)
{
    if (getpid() == 1) {
        snprintf(sock, sizeof sock, "%s", SSHD_SOCKET);
        mkdir("/run", 0755);
    } else {
        const char *t = getenv("TMPDIR");
        snprintf(sock, sizeof sock, "%s/rosgd-cli-%d.sock", t ? t : "/tmp", (int)getpid());
    }
    int e = sshd_listen(sock);
    check(!e, "SSHD -- /init listens for riscos-cli", "%s: %s", sock, strerror(-e));
    if (e)
        return;
    socket_sessions();
    if (getpid() == 1)
        through_sshd();
    else
        sshd_unlisten();
}
