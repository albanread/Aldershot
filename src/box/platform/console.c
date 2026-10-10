/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* console.c -- the serial console: where the runtime reports, and until
 * the VDU drivers are compiled, where the VDU stream goes. */
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

#include "rosgd/platform.h"

void ros_console_init(void)
{
    /* OS_ReadC wants each key as it is typed: no line editing, no echo.
     * Output processing stays on, so "\n" still reaches the terminal as
     * CR LF. */
    struct termios t;
    if (isatty(0) && tcgetattr(0, &t) == 0) {
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
    }
}

void ros_console_write(const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(1, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

void ros_console_putc(int ch)
{
    char c = (char)ch;
    ros_console_write(&c, 1);
}

void ros_console_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        ros_console_write(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

int ros_console_getc(int timeout_ms)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0)
        return -1;
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -2;
}
