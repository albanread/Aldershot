/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_background.c: the personality lock, the event queue and
 * background work, against background.h.
 *
 * Other threads stand in for Linux's pumps.  They post work, or send to a
 * socket, without the lock.  The checks are about when that work runs.  It
 * must not run while module code runs.  It runs at the way out of the
 * outermost SWI, during a blocking wait, and at INKEY and OS_Mouse even
 * inside a SWI.  It does not run while interrupts are off.  The checks also
 * cover the Internet event that FIOASYNC sockets raise through the pump.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/keyboard.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* What a piece of work saw when it ran. */
static struct {
    volatile int ran;
    pthread_t thread;
    uint32_t irqsema;
    int held;
    int order;
} seen;
static volatile int order;

static void note(void *arg, uint32_t info)
{
    (void)arg, (void)info;
    seen.thread = pthread_self();
    seen.irqsema = ros_ld32(ROS_ZP_IRQSEMA);
    seen.held = ros_lock_held();
    seen.order = ++order;
    seen.ran = 1;
}

static void *poster(void *delay)
{
    sleep_ms((unsigned)(uintptr_t)delay);
    ros_post(note, NULL, 0);
    return NULL;
}

/* Post from another thread, and wait for the post.  It does not wait for the work. */
static void post_from_thread(unsigned delay_ms, pthread_t *t)
{
    seen.ran = 0;
    pthread_create(t, NULL, poster, (void *)(uintptr_t)delay_ms);
}

static void swi(uint32_t number)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, number);
}

static int callback_order;
static void callback(void *arg)
{
    (void)arg;
    callback_order = ++order;
}

/* ---- input polled from inside a SWI -------------------------------------- */

#define USERV 0x00u
#define POINTERV 0x26u

/* The keyboard's interrupt: a character into the keyboard buffer. */
static void key_in(void *arg, uint32_t ch)
{
    (void)arg;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 138, c.r[1] = 0, c.r[2] = ch;
    ros_swi(&c, XOS_Byte);
}

/* The mouse's: a movement (PointerV Report, for a device type nobody has
 * selected), then Select down (the keyboard handler's MouseButtonChange). */
static void mouse_in(void *arg, uint32_t d)
{
    (void)arg;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 3, c.r[1] = 0xFE, c.r[2] = (uint32_t)(int8_t)d, c.r[3] = (uint32_t)(int8_t)(d >> 8);
    ros_vector_call(POINTERV, &c);
    ros_key_mouse_buttons(4);
}

static struct ros_cpu byte_c(uint32_t r0, uint32_t r1, uint32_t r2)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = r0, c.r[1] = r1, c.r[2] = r2;
    ros_swi(&c, XOS_Byte);
    return c;
}

/* What Wimp_ReportError's loop saw: INKEY(0) and OS_Mouse, called from a
 * claimant of UserV, entered through OS_CallAVector.  That is inside a SWI, as
 * the Wimp calls them inside Wimp_ReportError. */
static struct {
    uint32_t depth;
    int neg_ran, pos_ran, off_after;
    struct ros_cpu key, mouse;
} poll_in;

static int poll_input(struct ros_cpu *s, uint32_t r12)
{
    (void)s, (void)r12;
    poll_in.depth = ros_call_depth;
    seen.ran = 0;
    swi(XOS_IntOff);
    ros_post(note, NULL, 0);                    /* interrupts off: it waits */
    byte_c(0x81, 0xFF, 0xFF);                   /* a negative INKEY (Shift): they stay off */
    poll_in.neg_ran = seen.ran;
    byte_c(0x81, 0, 0);                         /* INKEY(0) turns them on while it lasts */
    poll_in.pos_ran = seen.ran;
    poll_in.off_after = (int)ros_irq_off_count();
    swi(XOS_IntOn);
    ros_post(key_in, NULL, 'x');                /* a key comes */
    poll_in.key = byte_c(0x81, 0, 0);
    ros_post(mouse_in, NULL, 0x0810);           /* the mouse moves and Select goes down */
    ros_cpu_enter(&poll_in.mouse);
    ros_swi(&poll_in.mouse, XOS_Mouse);
    return ROS_VECTOR_CLAIMED;
}

/* ---- the Internet event -------------------------------------------------- */

static struct {
    volatile int count;
    uint32_t r[4];
} ev;

static int eventv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == 19) {
        memcpy(ev.r, s->r, sizeof ev.r);
        ev.count++;
    }
    return ROS_VECTOR_PASS;
}

/* Wait, releasing the lock, until the event count reaches n or ms pass. */
static int wait_events(int n, unsigned ms)
{
    for (unsigned t = 0; ev.count < n && t < ms; t += 5)
        ROS_BLOCKING(sleep_ms(5));
    return ev.count >= n;
}

static void sin_loop(uint8_t *p, uint32_t port)
{
    memset(p, 0, 16);
    p[0] = 16, p[1] = 2, p[2] = (uint8_t)(port >> 8), p[3] = (uint8_t)port;
    p[4] = 127, p[7] = 1;
}

/* A host thread sending a datagram, as the network would. */
static uint32_t send_port;
static void *sender(void *delay)
{
    sleep_ms((unsigned)(uintptr_t)delay);
    ros_post(note, NULL, 0);                      /* an interrupt, mid-wait */
    sleep_ms(20);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)send_port) };
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(fd, "late", 4, 0, (struct sockaddr *)&to, sizeof to);
    close(fd);
    return NULL;
}

void ros_selftest_background(void)
{
    pthread_t t;
    check(ros_lock_held() && ros_call_depth == 0,
          "background: /init holds the personality lock, outside any SWI", NULL);

    /* ---- work waits for a safe point ---- */
    post_from_thread(0, &t);
    pthread_join(t, NULL);
    sleep_ms(20);                               /* the background thread wants the lock */
    check(!seen.ran, "background: work posted while module code runs waits", NULL);
    ros_callback_add_native(callback, NULL);
    swi(XOS_ReadMonotonicTime);
    check(seen.ran && pthread_equal(seen.thread, pthread_self()) && seen.held &&
              seen.irqsema != 0,
          "background: it runs on the way out of the outermost SWI, locked, IRQsema set",
          "ran %d, held %d, IRQsema &%X", seen.ran, seen.held, seen.irqsema);
    check(callback_order == seen.order + 1,
          "background: then callbacks, after the background work", "work %d, callback %d",
          seen.order, callback_order);
    check(ros_ld32(ROS_ZP_IRQSEMA) == 0, "background: IRQsema is 0 again afterwards", NULL);

    /* ---- IntOff defers it ---- */
    swi(XOS_IntOff);
    post_from_thread(0, &t);
    pthread_join(t, NULL);
    swi(XOS_ReadMonotonicTime);
    unsigned d = ros_blocking_begin();
    sleep_ms(20);                               /* the background thread may try, too */
    ros_blocking_end(d);
    check(!seen.ran, "background: OS_IntOff defers it, through SWIs and waits", NULL);
    swi(XOS_IntOn);
    check(seen.ran, "background: OS_IntOn runs it", NULL);

    /* ---- a blocking wait lets it run, on the background thread ---- */
    post_from_thread(30, &t);
    ROS_BLOCKING(sleep_ms(150));
    pthread_join(t, NULL);
    check(seen.ran && !pthread_equal(seen.thread, pthread_self()) && seen.held &&
              seen.irqsema != 0,
          "background: during a blocking wait it runs on the background thread", "ran %d",
          seen.ran);

    /* ---- INKEY and OS_Mouse are safe points, inside a SWI too: RISC OS's
     * keyboard and mouse interrupts break into a loop that polls them
     * inside a SWI (Wimp_ReportError's, Wimp_CommandWindow's) ---- */
    struct ros_cpu m0;
    ros_cpu_enter(&m0);
    ros_swi(&m0, XOS_Mouse);
    ros_vector_claim_native(USERV, poll_input, 0);
    struct ros_cpu cav;
    ros_cpu_enter(&cav);
    cav.r[9] = USERV;
    ros_swi(&cav, XOS_CallAVector);
    ros_vector_release_native(USERV, poll_input, 0);
    struct ros_cpu m1;                          /* the mouse, now all the work has run */
    ros_cpu_enter(&m1);
    ros_swi(&m1, XOS_Mouse);
    check(poll_in.depth >= 1 && !poll_in.neg_ran,
          "background: inside a SWI, a negative INKEY with interrupts off leaves work waiting",
          "depth %u, ran %d", poll_in.depth, poll_in.neg_ran);
    check(poll_in.pos_ran && poll_in.off_after == 1,
          "background: INKEY(0) with interrupts off turns them on while it lasts, as the "
          "kernel's RdchInkey (CLI): the work runs, and they are off again after",
          "ran %d, IntOff count after %d", poll_in.pos_ran, poll_in.off_after);
    check(poll_in.key.r[1] == 'x' && poll_in.key.r[2] == 0 && !poll_in.key.c,
          "background: inside a SWI, INKEY(0) takes the key that came -- a safe point",
          "R1 &%X R2 &%X C %u", poll_in.key.r[1], poll_in.key.r[2], poll_in.key.c);
    check(poll_in.mouse.r[2] == 4 && poll_in.mouse.r[0] == m1.r[0] &&
              poll_in.mouse.r[1] == m1.r[1] && m1.r[2] == 4 &&
              (m1.r[0] != m0.r[0] || m1.r[1] != m0.r[1]),
          "background: inside a SWI, OS_Mouse reads the mouse as it moved and clicked -- a "
          "safe point",
          "(%d,%d) buttons %u; before (%d,%d) %u; after (%d,%d) %u", (int)poll_in.mouse.r[0],
          (int)poll_in.mouse.r[1], poll_in.mouse.r[2], (int)m0.r[0], (int)m0.r[1], m0.r[2],
          (int)m1.r[0], (int)m1.r[1], m1.r[2]);
    if (poll_in.key.r[1] != 'x')
        byte_c(21, 0, 0);                       /* the key the check missed */
    ros_key_mouse_buttons(m0.r[2] & 7);         /* Select up, the mouse back (OS_Word 21,3) */
    uint8_t *blk = ros_rma_alloc(8);
    blk[0] = 3, blk[1] = (uint8_t)m0.r[0], blk[2] = (uint8_t)(m0.r[0] >> 8);
    blk[3] = (uint8_t)m0.r[1], blk[4] = (uint8_t)(m0.r[1] >> 8);
    struct ros_cpu w;
    ros_cpu_enter(&w);
    w.r[0] = 21, w.r[1] = ros_addr(blk);
    ros_swi(&w, XOS_Word);
    ros_rma_free(blk);

    /* ---- a raise leaves the SWIs it leaves ---- */
    struct ros_handler h;
    if (ROS_TRY(&h)) {
        swi(0x3FFFFF & ~ROS_X_BIT);             /* no such SWI, non-X: raised */
        ros_handler_pop(&h);
    }
    check(ros_call_depth == 0, "background: a raise out of a SWI restores the depth", "%u",
          ros_call_depth);

    /* ---- the Internet event, through the pump ---- */
    uint8_t *a = ros_rma_alloc(16);
    uint32_t *len = ros_rma_alloc(4), rx = 0, tx = 0, n, v;
    uint8_t *buf = ros_rma_alloc(64);
    ros_vector_claim_native(ROS_EVENTV, eventv, 0);
    ros_event_enable(19, 1);
    os_error *e = xsocket_creat(2, 2, 0, &rx);
    sin_loop(a, 0);
    if (!e)
        e = xsocket_bind(rx, a, 16);
    *len = 16;
    if (!e)
        e = xsocket_getsockname_1(rx, a, len);
    uint32_t port = (uint32_t)a[2] << 8 | a[3];
    v = 1;
    if (!e)
        e = xsocket_ioctl(rx, 0x8004667Du, (uint8_t *)&v);          /* FIOASYNC */
    if (!e)
        e = xsocket_creat(2, 2, 0, &tx);
    ev.count = 0;
    if (!e)
        e = xsocket_sendto(tx, (const uint8_t *)"one", 3, 0, a, 16, &n);
    int got = !e && wait_events(1, 1000);
    check(got && ev.r[1] == 1 && ev.r[2] == rx && ev.r[3] == port,
          "background: FIOASYNC -- input raises Event 19, R1 1, R2 the socket, R3 its port",
          "%s %d events, R1 %u R2 %u R3 %u (socket %u port %u)", e ? e->errmess : "",
          ev.count, ev.r[1], ev.r[2], ev.r[3], rx, port);
    xsocket_sendto(tx, (const uint8_t *)"two", 3, 0, a, 16, &n);
    wait_events(2, 150);
    check(ev.count == 1, "background: no second event until the input is read", "%d events",
          ev.count);
    xsocket_recv(rx, buf, 64, 0, &n);
    check(wait_events(2, 1000) && ev.r[2] == rx,
          "background: reading re-arms it -- the waiting datagram raises the next", "%d events",
          ev.count);
    xsocket_recv(rx, buf, 64, 0, &n);

    /* ---- a blocking Socket call releases the lock for its wait ---- */
    v = 0;
    xsocket_ioctl(rx, 0x8004667Du, (uint8_t *)&v);                    /* FIOASYNC off */
    send_port = port;
    seen.ran = 0;
    pthread_create(&t, NULL, sender, (void *)(uintptr_t)30);
    memset(buf, 0, 64);
    e = xsocket_recv(rx, buf, 64, 0, &n);             /* blocks until "late" comes */
    pthread_join(t, NULL);
    check(!e && n == 4 && memcmp(buf, "late", 4) == 0 && seen.ran &&
              !pthread_equal(seen.thread, pthread_self()),
          "background: work runs while Socket_Recv waits, before it returns", "%s ran %d",
          e ? e->errmess : "", seen.ran);

    xsocket_close(tx);
    xsocket_close(rx);
    ros_event_enable(19, 0);
    ros_vector_release_native(ROS_EVENTV, eventv, 0);
    ros_rma_free(buf), ros_rma_free(len), ros_rma_free(a);
}
