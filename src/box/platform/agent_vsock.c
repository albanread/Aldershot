/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* agent_vsock.c -- the test harness's agent in the Apple Silicon box.
 *
 * The Intel box is driven through QEMU's QMP: input-send-event for the
 * pointer and keys, and memsave for the screen (tests/lib/deskdrive.py).
 * VZ has neither, so under rosgd-vz the box does it itself. With
 * rosgd.agent on the kernel command line, this listens on vsock port 7700,
 * and rosgd-vz relays its --control socket there (vz/Sources/Control.swift).
 * The agent checks nobody who connects. Anything that can reach the port
 * can read the box's memory and press its keys, so it starts only with
 * rosgd.agent.
 *
 * The protocol is lines of text. Each answer is "ok N\n" and N bytes, or
 * "err TEXT\n".
 *
 *   ping                  ok, no bytes
 *   read ADDR LEN         LEN bytes of the arena at ADDR (hex), as memsave
 *                         gives them; err where nothing is mapped
 *   maps                  /init's /proc/self/maps, for debugging
 *   screen                the arena address of the screen block
 *                         (include/rosgd/screen.h), as text: the last 4 KB
 *                         of the mapping at ROS_SCREEN_BASE, with the magic
 *                         checked
 *   key CODE VALUE        a key or mouse button by its Linux code (KEY_*,
 *                         BTN_LEFT ...): VALUE 1 down, 0 up
 *   abs X Y               the pointer to X, Y: 0-65535 across the mode's
 *                         screen, as a tablet reports it
 *   rel DX DY             the pointer moved by DX, DY (Y down)
 *   wheel N               the wheel, N notches (positive away)
 *   sound                 the sound system as it stands, a line of text:
 *                         the time (CLOCK_MONOTONIC, ns); the audio
 *                         chain's frames made, those not silent, those
 *                         at full scale and the peak (ros_audio_meter);
 *                         the queue's clock (beats), BEAT, bar, tempo,
 *                         events waiting, queued and fired
 *                         (ros_soundq_state); the notes the voices were
 *                         given, and those gated on. A test checks music
 *                         by this and never by hearing it
 *
 * What the host sends goes in at the same place evdev's events do. Each
 * command's events are queued as background work (ros_post) and handed to
 * the Input module at a safe point, holding the lock, through KeyV and
 * PointerV as a USB keyboard and tablet would. An absolute position is
 * the mode's and not the display's, so a screenshot's pixel is where the
 * pointer goes.
 *
 * The listening thread never runs module code and never takes the lock.
 * Memory is read through /proc/self/mem, which answers EIO for a page
 * that is not there rather than faulting /init. One client is served at
 * a time.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/vm_sockets.h>

#include "input.h"
#include "sound.h"
#include "soundsched.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/platform.h"
#include "rosgd/screen.h"

#define AGENT_PORT 7700u
#define MAX_READ   (64u << 20)

struct batch {
    unsigned n;
    struct ros_input_event ev[3];
};

static void deliver(void *arg, uint32_t info)
{
    (void)info;
    struct batch *b = arg;
    for (unsigned i = 0; i < b->n; i++)
        input_deliver(&b->ev[i]);
    free(b);
}

static int post(unsigned n, const struct ros_input_event *ev)
{
    struct batch *b = malloc(sizeof *b);
    if (!b)
        return -1;
    b->n = n;
    memcpy(b->ev, ev, n * sizeof *ev);
    if (ros_post(deliver, b, 0) < 0) {
        free(b);
        return -1;
    }
    return 0;
}

static int send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        c += w, n -= (size_t)w;
    }
    return 0;
}

static int answer(int fd, const void *p, size_t n)
{
    char head[32];
    int k = snprintf(head, sizeof head, "ok %zu\n", n);
    return send_all(fd, head, (size_t)k) || (n && send_all(fd, p, n)) ? -1 : 0;
}

static int refuse(int fd, const char *why)
{
    char line[160];
    int k = snprintf(line, sizeof line, "err %s\n", why);
    return send_all(fd, line, (size_t)k);
}

/* Whether [lo, hi) is wholly in readable mappings of /init's */
static int readable(uint64_t lo, uint64_t hi)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;
    char line[512];
    uint64_t at = lo;
    while (at < hi && fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perms) != 3 || perms[0] != 'r')
            continue;
        if (a <= at && at < b)
            at = b;
    }
    fclose(f);
    return at >= hi;
}

/* The end of the mapping at ROS_SCREEN_BASE (the buffer: the rest of the
 * screen's reservation is PROT_NONE, a mapping of its own), or 0 */
static uint64_t screen_end(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;
    char line[512];
    uint64_t end = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) == 3 && lo == (uintptr_t)ros_ptr(ROS_SCREEN_BASE) &&
            perms[0] == 'r') {
            end = hi - (uintptr_t)ros_ptr(0);    /* as an arena address */
            break;
        }
    }
    fclose(f);
    return end;
}

static int command(int fd, int mem, char *line)
{
    char word[16];
    unsigned long long a = 0, b = 0;
    int n = sscanf(line, "%15s %llx %llx", word, &a, &b);
    if (n < 1)
        return refuse(fd, "empty");
    if (!strcmp(word, "ping"))
        return answer(fd, NULL, 0);
    if (!strcmp(word, "read")) {
        if (n != 3 || b > MAX_READ || a > 0x100000000ull || b > 0x100000000ull - a)
            return refuse(fd, "read ADDR LEN (hex), within the arena");
        char *buf = malloc(b ? b : 1);
        if (!buf)
            return refuse(fd, "no memory");
        size_t done = 0;
        while (done < b) {
            ssize_t r = pread(mem, buf + done, b - done, (off_t)(uintptr_t)ros_ptr((uint32_t)(a + done)));
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            done += (size_t)r;
        }
        /* /proc/self/mem cannot read a device's pages (the DRM dumb
         * buffer, VM_PFNMAP), so those are read directly, having checked
         * that they are mapped. (A mode change in between could unmap them.
         * The harness reads the screen between its own actions.) */
        uint64_t host = (uintptr_t)ros_ptr((uint32_t)(a + done));
        if (done < b && readable(host, host + (b - done))) {
            memcpy(buf + done, (const void *)(uintptr_t)host, b - done);
            done = b;
        }
        int e = done == b ? answer(fd, buf, b) : refuse(fd, "not mapped");
        free(buf);
        return e;
    }
    if (!strcmp(word, "maps")) {         /* for debugging the harness */
        int m = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        static char text[1 << 16];
        ssize_t got = m < 0 ? -1 : read(m, text, sizeof text);
        size_t total = got > 0 ? (size_t)got : 0;
        while (got > 0 && total < sizeof text && (got = read(m, text + total, sizeof text - total)) > 0)
            total += (size_t)got;
        if (m >= 0)
            close(m);
        return answer(fd, text, total);
    }
    if (!strcmp(word, "screen")) {
        uint64_t end = screen_end();
        uint32_t magic = 0;
        if (end)
            memcpy(&magic, ros_ptr((uint32_t)(end - ROS_SCREEN_BLOCK)), 4);
        if (!end || magic != ROS_SCREEN_MAGIC)
            return refuse(fd, "no screen block");
        char text[32];
        int k = snprintf(text, sizeof text, "%llx", (unsigned long long)(end - ROS_SCREEN_BLOCK));
        return answer(fd, text, (size_t)k);
    }
    if (!strcmp(word, "sound")) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        struct ros_audio_meter m;
        ros_audio_meter(&m);
        struct ros_soundq_state q;
        ros_soundq_state(&q);
        uint32_t in, on;
        ros_sound_state(&in, &on);
        char text[400];
        int k = snprintf(text, sizeof text,
                         "t=%llu frames=%llu loud=%llu full=%llu peak=%u beat=%u qbeat=%u bar=%u tempo=%u depth=%u "
                         "scheduled=%u fired_notes=%u fired_other=%u notes=%u notes_on=%u",
                         (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec,
                         (unsigned long long)m.frames, (unsigned long long)m.loud,
                         (unsigned long long)m.full, m.peak,
                         q.beat, q.qbeat, q.bar, q.tempo, q.depth, q.scheduled, q.fired_notes,
                         q.fired_other, in, on);
        return answer(fd, text, (size_t)k);
    }
    /* input: decimal arguments */
    long x = 0, y = 0;
    n = sscanf(line, "%15s %ld %ld", word, &x, &y);
    struct ros_input_event ev[3];
    memset(ev, 0, sizeof ev);
    unsigned count;
    if (!strcmp(word, "key") && n == 3) {
        int key = ros_input_key_number((unsigned)x);
        if (key < 0)
            return refuse(fd, "no RISC OS key number for that code");
        ev[0] = (struct ros_input_event){ ROS_INPUT_KEY, (uint32_t)key, 0, (int32_t)y, 0 };
        count = 1;
    } else if (!strcmp(word, "abs") && n == 3) {
        if (x < 0 || x > 65535 || y < 0 || y > 65535)
            return refuse(fd, "abs X Y, 0-65535");
        ev[0] = (struct ros_input_event){ ROS_INPUT_POSITION, 0, ROS_AXIS_X, (int32_t)x, 0 };
        ev[1] = (struct ros_input_event){ ROS_INPUT_POSITION, 0, ROS_AXIS_Y, (int32_t)y, 0 };
        ev[2] = (struct ros_input_event){ ROS_INPUT_SYNC, 0, 0, 0, 0 };
        count = 3;
    } else if (!strcmp(word, "rel") && n == 3) {
        ev[0] = (struct ros_input_event){ ROS_INPUT_MOVE, 0, ROS_AXIS_X, (int32_t)x, 0 };
        ev[1] = (struct ros_input_event){ ROS_INPUT_MOVE, 0, ROS_AXIS_Y, (int32_t)y, 0 };
        ev[2] = (struct ros_input_event){ ROS_INPUT_SYNC, 0, 0, 0, 0 };
        count = 3;
    } else if (!strcmp(word, "wheel") && n >= 2) {
        ev[0] = (struct ros_input_event){ ROS_INPUT_WHEEL, 0, ROS_AXIS_Y, (int32_t)x, 0 };
        ev[1] = (struct ros_input_event){ ROS_INPUT_SYNC, 0, 0, 0, 0 };
        count = 2;
    } else {
        return refuse(fd, "unknown command");
    }
    return post(count, ev) ? refuse(fd, "cannot queue") : answer(fd, NULL, 0);
}

static void serve(int fd, int mem)
{
    char buf[512];
    size_t have = 0;
    for (;;) {
        ssize_t r = read(fd, buf + have, sizeof buf - 1 - have);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return;
        have += (size_t)r;
        char *nl;
        while ((nl = memchr(buf, '\n', have))) {
            *nl = 0;
            if (command(fd, mem, buf))
                return;
            size_t used = (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, have - used);
            have -= used;
        }
        if (have == sizeof buf - 1)
            return;                     /* a line too long: not a harness */
    }
}

static void *agent(void *unused)
{
    ros_thread_name("agent");      /* named for /proc: what uses the time */
    (void)unused;
    sigset_t all;                       /* the runtime's signals are for its own threads */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    /* /proc is /init's to mount: wait for it, then for the word */
    char cmdline[4096];
    ssize_t n = -1;
    for (int tries = 0; tries < 600 && n < 0; tries++) {
        int f = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
        if (f >= 0) {
            n = read(f, cmdline, sizeof cmdline - 1);
            close(f);
        }
        if (n < 0)
            usleep(50000);
    }
    if (n <= 0)
        return NULL;
    cmdline[n] = 0;
    for (char *p = cmdline; (p = strstr(p, "rosgd.agent"));) {
        int start = p == cmdline || p[-1] == ' ';
        p += 11;
        if (start && (*p == 0 || *p == ' ' || *p == '\n'))
            goto enabled;
    }
    return NULL;
enabled:;
    int s = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_vm addr = { .svm_family = AF_VSOCK, .svm_port = AGENT_PORT,
                                .svm_cid = VMADDR_CID_ANY };
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof addr) || listen(s, 4)) {
        fprintf(stderr, "rosgd: agent: vsock port %u: %s\n", AGENT_PORT, strerror(errno));
        return NULL;
    }
    int mem = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    fprintf(stderr, "rosgd: agent: listening on vsock port %u\n", AGENT_PORT);
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            return NULL;
        }
        serve(c, mem);
        close(c);
    }
}

__attribute__((constructor)) static void agent_start(void)
{
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_create(&t, &at, agent, NULL);
    pthread_attr_destroy(&at);
}
