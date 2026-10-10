/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* relay.c -- riscos-cli: root's login shell in the box, for sshd.
 *
 * The RISC OS personality is /init, one process. An SSH session is another
 * process, sshd's. So the login shell is /init under the name riscos-cli (a
 * link the box makes). It does nothing of RISC OS's itself. It joins the
 * session's terminal to /init's socket and relays bytes both ways until
 * /init says the command line has ended, and with what status (session.c
 * has the protocol). "riscos-cli -c <command>" is sshd's way of running
 * "ssh box <command>". It runs the one command.
 *
 * The terminal is raw while it runs. Each key goes to RISC OS as it is
 * typed, and ReadLine does the editing, as it does on the console.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include "sshd.h"

static struct termios saved;
static int raw;

static void restore(void)
{
    if (raw)
        tcsetattr(0, TCSANOW, &saved);
}

static int write_all(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w, n -= (size_t)w;
    }
    return 0;
}

int sshd_relay(int argc, char **argv)
{
    const char *command = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            command = argv[++i];

    const char *path = getenv("ROSGD_CLI_SOCKET");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path ? path : SSHD_SOCKET);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "riscos-cli: RISC OS is not listening (%s): %s\n", a.sun_path, strerror(errno));
        return 1;
    }

    struct winsize ws = { .ws_col = 80, .ws_row = 24 };
    int tty = isatty(0);
    if (tty) {
        ioctl(0, TIOCGWINSZ, &ws);
        if (tcgetattr(0, &saved) == 0) {
            struct termios t = saved;
            cfmakeraw(&t);
            raw = tcsetattr(0, TCSANOW, &t) == 0;
        }
    }
    char head[1200];
    int n = command ? snprintf(head, sizeof head, "ROSGD-CLI 1 C %d %d\n%s\n", ws.ws_col, ws.ws_row, command)
                    : snprintf(head, sizeof head, "ROSGD-CLI 1 I %d %d\n", ws.ws_col, ws.ws_row);
    if (n < 0 || (size_t)n >= sizeof head || write_all(fd, head, (size_t)n) < 0) {
        restore();
        fprintf(stderr, "riscos-cli: cannot start the command line\n");
        return 1;
    }

    int status = 1, in_open = 1, end = 0;
    char b[4096];
    for (;;) {
        struct pollfd p[2] = { { .fd = fd, .events = POLLIN }, { .fd = 0, .events = POLLIN } };
        if (poll(p, in_open ? 2 : 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (p[0].revents) {
            ssize_t r = read(fd, b, sizeof b);
            if (r <= 0)
                break;
            for (ssize_t i = 0; i < r; i++) {
                if (end) {              /* &FF, then the status */
                    status = (unsigned char)b[i];
                    end = 2;
                    break;
                }
                if ((unsigned char)b[i] == 0xFF) {
                    write_all(1, b, (size_t)i);
                    end = 1;
                    if (i + 1 < r) {
                        status = (unsigned char)b[i + 1];
                        end = 2;
                    }
                    break;
                }
            }
            if (!end)
                write_all(1, b, (size_t)r);
            if (end == 2)
                break;
        }
        if (in_open && p[1].revents) {
            ssize_t r = read(0, b, sizeof b);
            if (r <= 0) {
                in_open = 0;
                shutdown(fd, SHUT_WR);  /* the end of the keys: the command line ends */
            } else if (write_all(fd, b, (size_t)r) < 0) {
                break;
            }
        }
    }
    restore();
    return status;
}
