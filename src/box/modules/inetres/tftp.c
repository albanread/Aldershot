/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (C) 2008 Edwin Groothuis. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's tftp(1) as
 * ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/tftp: c.main, c.tftp, c.tftp-file,
 * c.tftp-io, c.tftp-options, c.tftp-transfer, c.tftp-utils).
 */

/* tftp.c -- *Tftp (InetRes/Sources/tftp, FreeBSD's tftp(1)), over Linux's
 * UDP sockets.
 *
 * The trivial file transfer program. It has a "tftp> " prompt and these
 * commands: connect, get, put, mode, binary, ascii, status, verbose, trace,
 * debug, rexmt, timeout, blocksize, blocksize2, rollover, windowsize,
 * options, packetdrop, help and quit. A tftp://host/file[;mode=...] URI
 * can be given instead, to fetch at once. The protocol is FreeBSD's
 * client: RFC 1350, with RFC 2347 options (tsize, timeout, blksize,
 * windowsize, and the non-RFC blksize2 and rollover), netascii conversion,
 * windowed sends with partial ACKs, and FreeBSD's retries and messages.
 *
 * Local files are RISC OS names, through FileSwitch (files.c). Escape
 * interrupts a transfer, or a prompt, back to "tftp> ", as SIGINT does.
 * FreeBSD's client logs to its standard output, and so does this. RISC OS's
 * port had no syslog and dropped those lines. Sys$ReturnCode on quit is
 * FreeBSD's txrx_error.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "inetres.h"
#include "rosgd/background.h"

/* ---- the protocol's numbers (RFC 1350, 2347; arpa/tftp.h) ---------------- */

enum { RRQ = 1, WRQ, DATA, ACK, ERROR, OACK };
enum { EUNDEF, ENOTFOUND, EACCESS, ENOSPACE, EBADOP, EBADID, EEXISTS, ENOUSER, EOPTNEG };

#define SEGSIZE        512
#define TIMEOUT        5
#define MAX_TIMEOUTS   5
#define MIN_TIMEOUTS   3
#define MAXSEGSIZE     65464
#define MAXPKTSIZE     (MAXSEGSIZE + 4)
#define BLKSIZE_MIN    8
#define BLKSIZE_MAX    MAXSEGSIZE
#define TIMEOUT_MIN    0
#define TIMEOUT_MAX    255
#define WINDOWSIZE_MIN 1
#define WINDOWSIZE_MAX 65535
#define MAXDGRAM       65507        /* Linux's largest UDP payload: its net.inet.udp.maxdgram */
#define TFTP_PORT      69           /* the box has no /etc/services */

enum { RP_NONE = 0, RP_RECVFROM = -1, RP_TOOSMALL = -2, RP_ERROR = -3, RP_WRONGSOURCE = -4, RP_TIMEOUT = -5,
       RP_TOOBIG = -6, RP_ESCAPE = -7 };

enum { DEBUG_NONE = 0, DEBUG_PACKETS = 1, DEBUG_SIMPLE = 2, DEBUG_OPTIONS = 4, DEBUG_ACCESS = 8 };
enum { LOG_INFO, LOG_WARNING, LOG_DEBUG, LOG_ERR };

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

/* ---- the program's state, as tftp's statics ------------------------------------- */

enum { OPT_TSIZE, OPT_TIMEOUT, OPT_BLKSIZE, OPT_BLKSIZE2, OPT_ROLLOVER, OPT_WINDOWSIZE, OPT_COUNT };

struct option {
    const char *type;
    char request[16], reply[16];    /* "" for none */
    int has_request, has_reply;
    int (*handler)(int peer);
    int rfc;
};

static int option_timeout(int peer);
static int option_blksize(int peer);
static int option_blksize2(int peer);
static int option_rollover(int peer);
static int option_windowsize(int peer);

static struct option options[OPT_COUNT] = {
    { "tsize", "", "", 0, 0, NULL, 1 },
    { "timeout", "", "", 0, 0, option_timeout, 1 },
    { "blksize", "", "", 0, 0, option_blksize, 1 },
    { "blksize2", "", "", 0, 0, option_blksize2, 0 },
    { "rollover", "", "", 0, 0, option_rollover, 0 },
    { "windowsize", "", "", 0, 0, option_windowsize, 1 },
};

static int options_rfc_enabled, options_extra_enabled;
static int timeoutpacket, timeoutnetwork, maxtimeouts;
static uint16_t segsize, pktsize, windowsize;
static int debug, packetdroppercentage, verbose, txrx_error, connected;
static char mode[32], hostname[256], port[16];
static int peer = -1;
static struct sockaddr_storage peer_sock;
static socklen_t peer_len;
static int escaped;                 /* Escape: back to the prompt */

static const struct { int value; const char *name, *desc; } debugs[] = {
    { DEBUG_PACKETS, "packet", "Packet debugging" }, { DEBUG_SIMPLE, "simple", "Simple debugging" },
    { DEBUG_OPTIONS, "options", "Options debugging" }, { DEBUG_ACCESS, "access", "TCPd access debugging" },
    { DEBUG_NONE, NULL, "No debugging" },
};

static void set_option(int i, const char *value)
{
    snprintf(options[i].request, sizeof options[i].request, "%s", value);
    options[i].has_request = 1;
}

static void reset(void)
{
    for (int i = 0; i < OPT_COUNT; i++)
        options[i].has_request = options[i].has_reply = 0;
    set_option(OPT_ROLLOVER, "0");                  /* init_options() */
    options_rfc_enabled = options_extra_enabled = 1;
    timeoutpacket = TIMEOUT, timeoutnetwork = MAX_TIMEOUTS * TIMEOUT, maxtimeouts = MAX_TIMEOUTS;
    segsize = SEGSIZE, pktsize = SEGSIZE + 4, windowsize = 1;
    debug = packetdroppercentage = verbose = txrx_error = connected = 0;
    snprintf(mode, sizeof mode, "octet");
    hostname[0] = port[0] = 0;
    peer = -1;
    escaped = 0;
}

static os_error *out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static os_error *out(const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    return inet_printf("%s", text);
}

/* tftp_log: FreeBSD's client logs a line to its standard output */
static void tftp_log(int priority, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tftp_log(int priority, const char *fmt, ...)
{
    (void)priority;
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    inet_printf("%s\n", text);
}

static const char *debug_show(int d)
{
    static char s[100];
    s[0] = 0;
    for (int i = 0; debugs[i].name; i++)
        if (d & debugs[i].value) {
            if (s[0])
                strcat(s, " ");
            strcat(s, debugs[i].name);
        }
    return s[0] ? s : "none";
}

static int debug_find(const char *s)
{
    int i = 0;
    while (debugs[i].name && strcasecmp(debugs[i].name, s))
        i++;
    return debugs[i].value;
}

static const char *packettype(int type)
{
    static const char *const names[] = { NULL, "RRQ", "WRQ", "DATA", "ACK", "ERROR", "OACK" };
    static char failed[40];
    if (type >= 1 && type <= 6)
        return names[type];
    snprintf(failed, sizeof failed, "unknown (type: %d)", type);
    return failed;
}

static const char *rp_strerror(int error)
{
    static char s[40];
    switch (error) {
    case RP_TIMEOUT: return "Network timeout";
    case RP_TOOSMALL: return "Not enough data bytes";
    case RP_WRONGSOURCE: return "Invalid IP address of UDP port";
    case RP_ERROR: return "Error packet";
    case RP_RECVFROM: return "recvfrom() complained";
    case RP_TOOBIG: return "Too many data bytes";
    case RP_ESCAPE: return "Escape";
    default:
        snprintf(s, sizeof s, "unknown (error=%d)", error);
        return s;
    }
}

static int settimeouts(int packet, int network)
{
    if (packet >= network)
        return 0;
    int i = packet;
    maxtimeouts = 0;
    while (i < network || maxtimeouts < MIN_TIMEOUTS) {
        maxtimeouts++;
        i += 5;
    }
    timeoutpacket = packet;
    timeoutnetwork = i;
    return 1;
}

static int dropped(const char *where)
{
    if (packetdroppercentage && rand() % 100 < packetdroppercentage) {
        tftp_log(LOG_DEBUG, "Artificial packet drop in %s", where);
        return 1;
    }
    return 0;
}

/* A pause that sleeps: in a task window the desktop goes on */
static void pause_seconds(int t)
{
    struct timespec end, now;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += t;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (end.tv_sec - now.tv_sec) * 1000L + (end.tv_nsec - now.tv_nsec) / 1000000L;
        os_error *slept;
        if (ms <= 0 || ros_sleep_fd(-1, 0, (unsigned)ms, &slept) < 0)
            break;
    }
}

/* ---- packets (tftp-io.c) ------------------------------------------------------------ */

static const char *errtomsg(int error)
{
    static const char *const msgs[] = { "Undefined error code", "File not found", "Access violation",
                                        "Disk full or allocation exceeded", "Illegal TFTP operation",
                                        "Unknown transfer ID", "File already exists", "No such user",
                                        "Option negotiation" };
    return error >= 0 && error <= EOPTNEG ? msgs[error] : NULL;
}

static int send_to_peer(const uint8_t *buf, size_t n)
{
    return sendto(peer, buf, n, 0, (struct sockaddr *)&peer_sock, peer_len) == (ssize_t)n;
}

/* An ERROR: a TFTP code, or errno + 100 */
static void send_error(int error)
{
    uint8_t buf[600];
    if (debug & DEBUG_PACKETS)
        tftp_log(LOG_DEBUG, "Sending ERROR %d", error);
    if (dropped("send_error"))
        return;
    const char *msg = errtomsg(error);
    int code = error;
    if (!msg)
        msg = strerror(error - 100), code = EUNDEF;
    put16(buf, ERROR), put16(buf + 2, (unsigned)code);
    size_t len = strlen(msg) < 512 ? strlen(msg) : 511;
    memcpy(buf + 4, msg, len);
    buf[4 + len] = 0;
    if (debug & DEBUG_PACKETS)
        tftp_log(LOG_DEBUG, "Sending ERROR %d: %s", error, (char *)buf + 4);
    if (!send_to_peer(buf, len + 5))
        tftp_log(LOG_ERR, "send_error: %s", strerror(errno));
}

/* The options, "name\0value\0" each, after a request */
static size_t make_options(uint8_t *p, size_t size)
{
    size_t total = 0;
    if (!options_rfc_enabled)
        return 0;
    for (int i = 0; i < OPT_COUNT; i++) {
        if (!options[i].rfc && !options_extra_enabled)
            continue;
        if (!options[i].has_request)
            continue;
        size_t length = strlen(options[i].type) + strlen(options[i].request) + 2;
        if (size <= length) {
            tftp_log(LOG_ERR, "Running out of option space for option '%s' with value '%s': needed %d bytes, "
                     "got %d bytes", options[i].type, options[i].request, (int)size, (int)length);
            continue;
        }
        memcpy(p, options[i].type, strlen(options[i].type) + 1);
        memcpy(p + strlen(options[i].type) + 1, options[i].request, strlen(options[i].request) + 1);
        p += length, size -= length, total += length;
    }
    return total;
}

static int send_request(int op, const char *filename, const char *m)
{
    static uint8_t buf[MAXPKTSIZE];
    if (debug & DEBUG_PACKETS)
        tftp_log(LOG_DEBUG, "Sending %s: filename: '%s', mode '%s'", op == RRQ ? "RRQ" : "WRQ", filename, m);
    if (dropped(op == RRQ ? "send_rrq" : "send_wrq"))
        return 0;
    put16(buf, (unsigned)op);
    size_t size = 2, fl = strlen(filename) + 1, ml = strlen(m) + 1;
    if (fl + ml + 2 > sizeof buf)
        return 1;
    memcpy(buf + size, filename, fl), size += fl;
    memcpy(buf + size, m, ml), size += ml;
    if (options_rfc_enabled) {
        if (op == RRQ)
            set_option(OPT_TSIZE, "0");
        size += make_options(buf + size, sizeof buf - size);
    }
    if (!send_to_peer(buf, size)) {
        tftp_log(LOG_ERR, "send_%s: %s", op == RRQ ? "rrq" : "wrq", strerror(errno));
        return 1;
    }
    return 0;
}

static int send_ack(uint16_t block)
{
    uint8_t buf[4];
    if (debug & DEBUG_PACKETS)
        tftp_log(LOG_DEBUG, "Sending ACK for block %d", block);
    if (dropped("send_ack"))
        return 0;
    put16(buf, ACK), put16(buf + 2, block);
    if (!send_to_peer(buf, 4)) {
        tftp_log(LOG_INFO, "send_ack: %s", strerror(errno));
        return 1;
    }
    return 0;
}

static int send_data(uint16_t block, const uint8_t *data, int size)
{
    static uint8_t buf[MAXPKTSIZE];
    if (debug & DEBUG_PACKETS)
        tftp_log(LOG_DEBUG, "Sending DATA packet %d of %d bytes", block, size);
    if (dropped("send_data"))
        return 0;
    put16(buf, DATA), put16(buf + 2, block);
    memcpy(buf + 4, data, (size_t)size);
    int t = 1;
    for (int i = 0; i < 12; i++) {                  /* send_packet() */
        if (dropped("send_packet"))
            return 0;
        if (send_to_peer(buf, (size_t)size + 4)) {
            if (i)
                tftp_log(LOG_ERR, "%s block %d, attempt %d successful", packettype(DATA), block, i);
            return 0;
        }
        tftp_log(LOG_ERR, "%s block %d, attempt %d failed (Error %d: %s)", packettype(DATA), block, i, errno,
                 strerror(errno));
        pause_seconds(t);
        if (t < 32)
            t <<= 1;
    }
    tftp_log(LOG_ERR, "send_packet: %s", strerror(errno));
    return 1;
}

/* A packet, its opcode and block in host order: the bytes after the
 * header, or an RP_ code.  from, if given, gets the sender. */
static int receive_packet(uint8_t *data, size_t size, struct sockaddr_storage *from, int thistimeout)
{
    if (debug & DEBUG_PACKETS)
        tftp_log(LOG_DEBUG, "Waiting %d seconds for packet", timeoutpacket);
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (inet_escape()) {
            escaped = 1;
            return RP_ESCAPE;
        }
        gettimeofday(&now, NULL);
        long left = thistimeout * 1000L - ((now.tv_sec - t0.tv_sec) * 1000L + (now.tv_usec - t0.tv_usec) / 1000);
        if (left <= 0) {
            tftp_log(LOG_ERR, "receive_packet: timeout");
            return RP_TIMEOUT;
        }
        os_error *slept;
        int r = ros_sleep_fd(peer, POLLIN, left > 100 ? 100 : (unsigned)left, &slept);
        if (r < 0) {                                /* ended by Escape, in a task window */
            inet_escape();                          /* acknowledged, as a pressed Escape is */
            escaped = 1;
            return RP_ESCAPE;
        }
        if (r > 0)
            break;
    }
    struct sockaddr_storage local, *pfrom = from ? from : &local;
    socklen_t fromlen = sizeof *pfrom;
    ssize_t n = recvfrom(peer, data, size, 0, (struct sockaddr *)pfrom, &fromlen);
    if (dropped("receive_packet"))
        return RP_TIMEOUT;
    if (n < 0) {
        tftp_log(LOG_ERR, "receive_packet: timeout");
        return RP_TIMEOUT;
    }
    if (n < 4) {
        tftp_log(LOG_ERR, "receive_packet: packet too small (%d bytes)", (int)n);
        return RP_TOOSMALL;
    }
    int op = get16(data);
    if (op == DATA && n > pktsize) {
        tftp_log(LOG_ERR, "receive_packet: packet too big");
        return RP_TOOBIG;
    }
    int same = pfrom->ss_family == peer_sock.ss_family &&
               (pfrom->ss_family == AF_INET
                    ? ((struct sockaddr_in *)pfrom)->sin_addr.s_addr ==
                          ((struct sockaddr_in *)&peer_sock)->sin_addr.s_addr
                    : !memcmp(&((struct sockaddr_in6 *)pfrom)->sin6_addr,
                              &((struct sockaddr_in6 *)&peer_sock)->sin6_addr, 16));
    if (!same) {
        tftp_log(LOG_ERR, "receive_packet: received packet from wrong source");
        return RP_WRONGSOURCE;
    }
    if (op == ERROR) {
        data[n < (ssize_t)size ? n : (ssize_t)size - 1] = 0;
        tftp_log(get16(data + 2) == EUNDEF ? LOG_DEBUG : LOG_ERR, "Got ERROR packet: %s", (char *)data + 4);
        return RP_ERROR;
    }
    if (debug & DEBUG_PACKETS)
        tftp_log(LOG_DEBUG, "Received %d bytes in a %s packet", (int)n, packettype(op));
    return (int)n - 4;
}

/* Flush what is queued for us: the two sides may be out of step */
static int synchnet(void)
{
    static uint8_t rbuf[MAXPKTSIZE];
    int j = 0, i;
    while (ioctl(peer, FIONREAD, &i) == 0 && i) {
        j++;
        recv(peer, rbuf, sizeof rbuf, 0);
    }
    return j;
}

static void set_peer_port(unsigned p)
{
    if (peer_sock.ss_family == AF_INET6)
        ((struct sockaddr_in6 *)&peer_sock)->sin6_port = htons((uint16_t)p);
    else
        ((struct sockaddr_in *)&peer_sock)->sin_port = htons((uint16_t)p);
}

static unsigned port_of(const struct sockaddr_storage *s)
{
    return ntohs(s->ss_family == AF_INET6 ? ((const struct sockaddr_in6 *)s)->sin6_port
                                          : ((const struct sockaddr_in *)s)->sin_port);
}

/* ---- options (tftp-options.c), for a client ------------------------------------- */

static void reply(int i, const char *value)
{
    snprintf(options[i].reply, sizeof options[i].reply, "%s", value);
    options[i].has_reply = 1;
}

static int option_timeout(int p)
{
    (void)p;
    if (!options[OPT_TIMEOUT].has_request)
        return 0;
    int to = atoi(options[OPT_TIMEOUT].request);
    if (to < TIMEOUT_MIN || to > TIMEOUT_MAX) {
        tftp_log(LOG_ERR, "Received bad value for timeout. Should be between %d and %d, received %d", TIMEOUT_MIN,
                 TIMEOUT_MAX, to);
        send_error(EBADOP);
        return 1;
    }
    timeoutpacket = to;
    reply(OPT_TIMEOUT, options[OPT_TIMEOUT].request);
    settimeouts(timeoutpacket, timeoutnetwork);
    if (debug & DEBUG_OPTIONS)
        tftp_log(LOG_DEBUG, "Setting timeout to '%s'", options[OPT_TIMEOUT].reply);
    return 0;
}

static int option_rollover(int p)
{
    (void)p;
    if (!options[OPT_ROLLOVER].has_request)
        return 0;
    if (strcmp(options[OPT_ROLLOVER].request, "0") && strcmp(options[OPT_ROLLOVER].request, "1")) {
        tftp_log(LOG_ERR, "Bad value for rollover, should be either 0 or 1, received '%s', ignoring request",
                 options[OPT_ROLLOVER].request);
        send_error(EBADOP);
        return 1;
    }
    reply(OPT_ROLLOVER, options[OPT_ROLLOVER].request);
    if (debug & DEBUG_OPTIONS)
        tftp_log(LOG_DEBUG, "Setting rollover to '%s'", options[OPT_ROLLOVER].reply);
    return 0;
}

static int option_blksize(int p)
{
    (void)p;
    if (!options[OPT_BLKSIZE].has_request)
        return 0;
    int size = atoi(options[OPT_BLKSIZE].request);
    if (size < BLKSIZE_MIN || size > BLKSIZE_MAX) {
        tftp_log(LOG_ERR, "Invalid blocksize (%d bytes), aborting", size);
        send_error(EBADOP);
        return 1;
    }
    if (size > MAXDGRAM) {
        tftp_log(LOG_ERR, "Invalid blocksize (%d bytes), net.inet.udp.maxdgram sysctl limits it to %ld bytes.\n",
                 size, (long)MAXDGRAM);
        send_error(EBADOP);
        return 1;
    }
    char v[16];
    snprintf(v, sizeof v, "%d", size);
    reply(OPT_BLKSIZE, v);
    segsize = (uint16_t)size;
    pktsize = (uint16_t)(size + 4);
    if (debug & DEBUG_OPTIONS)
        tftp_log(LOG_DEBUG, "Setting blksize to '%s'", options[OPT_BLKSIZE].reply);
    return 0;
}

static const int sizes2[] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 0 };

static int option_blksize2(int p)
{
    (void)p;
    if (!options[OPT_BLKSIZE2].has_request)
        return 0;
    int size = atoi(options[OPT_BLKSIZE2].request), i;
    for (i = 0; sizes2[i] && sizes2[i] != size; i++)
        ;
    if (!sizes2[i]) {
        tftp_log(LOG_INFO, "Invalid blocksize2 (%d bytes), ignoring request", size);
        return 1;
    }
    char v[16];
    snprintf(v, sizeof v, "%d", size);
    reply(OPT_BLKSIZE2, v);
    segsize = (uint16_t)size;
    pktsize = (uint16_t)(size + 4);
    if (debug & DEBUG_OPTIONS)
        tftp_log(LOG_DEBUG, "Setting blksize2 to '%s'", options[OPT_BLKSIZE2].reply);
    return 0;
}

static int option_windowsize(int p)
{
    (void)p;
    if (!options[OPT_WINDOWSIZE].has_request)
        return 0;
    int size = atoi(options[OPT_WINDOWSIZE].request);
    if (size < WINDOWSIZE_MIN || size > WINDOWSIZE_MAX) {
        tftp_log(LOG_ERR, "Invalid windowsize (%d blocks), aborting", size);
        send_error(EBADOP);
        return 1;
    }
    char v[16];
    snprintf(v, sizeof v, "%d", size);
    reply(OPT_WINDOWSIZE, v);
    windowsize = (uint16_t)size;
    if (debug & DEBUG_OPTIONS)
        tftp_log(LOG_DEBUG, "Setting windowsize to '%s'", options[OPT_WINDOWSIZE].reply);
    return 0;
}

/* An OACK's options: how many were refused */
static int parse_options(uint8_t *p, int size)
{
    int failed = 0;
    if (!options_rfc_enabled)
        return 0;
    uint8_t *end = p + size;
    while (p < end) {
        char *option = (char *)p;
        uint8_t *z = memchr(p, 0, (size_t)(end - p));
        if (!z) {
            tftp_log(LOG_ERR, "Bad option - no trailing \\0 found");
            send_error(EBADOP);
            return failed + 1;
        }
        char *value = (char *)z + 1;
        uint8_t *z2 = (uint8_t *)value < end ? memchr(value, 0, (size_t)(end - (uint8_t *)value)) : NULL;
        if (!*option)
            break;
        if (!z2) {
            tftp_log(LOG_ERR, "Bad option - no trailing \\0 found");
            send_error(EBADOP);
            return failed + 1;
        }
        p = z2 + 1;
        if (debug & DEBUG_OPTIONS)
            tftp_log(LOG_DEBUG, "option: '%s' value: '%s'", option, value);
        for (char *c = option; *c; c++)
            *c = (char)tolower((unsigned char)*c);
        int i;
        for (i = 0; i < OPT_COUNT; i++)
            if (!strcmp(option, options[i].type)) {
                if (!options_extra_enabled && !options[i].rfc) {
                    tftp_log(LOG_INFO, "Option '%s' with value '%s' found but it is not an RFC option", option,
                             value);
                    break;
                }
                if (options[i].handler)
                    failed += options[i].handler(peer);
                break;
            }
        if (i == OPT_COUNT)
            tftp_log(LOG_WARNING, "Unknown option: '%s'", option);
    }
    return failed;
}

/* ---- the local file, and netascii (tftp-file.c) ---------------------------------- */

static struct inet_file *file;
static int convert, gotcr, newline;
static size_t conv_n, conv_in;
static uint8_t convbuffer[66000];

static long write_file(uint8_t *buf, int count)
{
    if (!convert)
        return inet_fwrite(file, buf, (size_t)count);
    size_t n = 0;                                   /* CR LF -> LF, CR NUL -> CR */
    for (int i = 0; i < count; i++) {
        if (!gotcr) {
            convbuffer[n++] = buf[i];
            gotcr = buf[i] == '\r';
            continue;
        }
        if (buf[i] == 0) {
            gotcr = 0;
            continue;
        }
        if (buf[i] == '\n') {
            if (n == 0) {
                long at = inet_ftell(file);         /* the CR ended the last block */
                if (at > 0 && inet_fseek(file, at - 1) == 0)
                    convbuffer[n++] = '\n';
            } else {
                convbuffer[n - 1] = '\n';
            }
            gotcr = 0;
            continue;
        }
        convbuffer[n++] = buf[i];
        gotcr = buf[i] == '\r';
    }
    return n ? inet_fwrite(file, convbuffer, n) : 0; /* the bytes written, as fwrite() counts */
}

static long read_file(uint8_t *buf, int count)
{
    if (!convert)
        return inet_fread(file, buf, (size_t)count);
    int i = 0;                                      /* LF -> CR LF, CR -> CR NUL */
    if (newline != -1) {
        buf[i++] = (uint8_t)newline;
        newline = -1;
    }
    while (i < count) {
        if (conv_n == conv_in) {
            long got = inet_fread(file, convbuffer, (size_t)count);
            if (got <= 0)
                break;
            conv_in = (size_t)got, conv_n = 0;
        }
        uint8_t c = convbuffer[conv_n++];
        if (c == '\r' || c == '\n') {
            buf[i++] = '\r';
            if (i < count)
                buf[i++] = c == '\r' ? 0 : '\n';
            else
                newline = c == '\r' ? 0 : '\n';     /* the pair's second byte, next time */
            continue;
        }
        buf[i++] = c;
    }
    return i;
}

static void file_init(struct inet_file *f, const char *m)
{
    file = f;
    convert = !strcmp(m, "netascii");
    gotcr = 0, newline = -1, conv_n = conv_in = 0;
}

/* ---- transfers (tftp-transfer.c, tftp.c) ------------------------------------------ */

struct stats {
    size_t amount;
    int rollovers, retries;
    uint32_t blocks;
    struct timeval tstart, tstop;
};

static void stats_init(struct stats *ts)
{
    memset(ts, 0, sizeof *ts);
    gettimeofday(&ts->tstart, NULL);
}

static void printstats(const char *direction, struct stats *ts)
{
    double delta = (ts->tstop.tv_sec * 10. + ts->tstop.tv_usec / 100000) -
                   (ts->tstart.tv_sec * 10. + ts->tstart.tv_usec / 100000);
    delta /= 10.;
    out("%s %zu bytes during %.1f seconds in %u blocks", direction, ts->amount, delta, ts->blocks);
    if (ts->rollovers)
        out(" with %d rollover%s", ts->rollovers, ts->rollovers != 1 ? "s" : "");
    if (verbose)
        out(" [%.0f bits/sec]", ts->amount * 8. / delta);
    out("\n");
}

static uint16_t next_block(uint16_t block, struct stats *ts)
{
    uint16_t next = (uint16_t)(block + 1);
    if (next < block) {
        next = options[OPT_ROLLOVER].has_request ? (uint16_t)atoi(options[OPT_ROLLOVER].request) : 0;
        ts->rollovers++;
    }
    return next;
}

struct window_block {
    long offset;
    uint16_t block;
    int size;
};

static void tftp_send(uint16_t *block, struct stats *ts)
{
    static uint8_t sendbuffer[MAXPKTSIZE], recvbuffer[MAXPKTSIZE];
    static struct window_block window[WINDOWSIZE_MAX];
    int size, acktry = 0;
    unsigned windowblock = 0;
    *block = 1;
    ts->amount = 0;
    do {
    read_block:
        if (debug & DEBUG_SIMPLE)
            tftp_log(LOG_DEBUG, "Sending block %d (window block %d)", *block, windowblock);
        window[windowblock].offset = inet_ftell(file);
        window[windowblock].block = *block;
        size = (int)read_file(sendbuffer, segsize);
        if (size < 0) {
            tftp_log(LOG_ERR, "read_file returned %d", size);
            send_error(EIO + 100);
            return;
        }
        window[windowblock].size = size;
        windowblock++;

        for (int sendtry = 0;; sendtry++) {
            if (send_data(*block, sendbuffer, size) == 0)
                break;
            if (sendtry == maxtimeouts) {
                tftp_log(LOG_ERR, "Cannot send DATA packet #%d, giving up", *block);
                return;
            }
            tftp_log(LOG_ERR, "Cannot send DATA packet #%d, trying again", *block);
        }

        if (windowblock == windowsize || size != segsize) {    /* the ACK for the window's last */
            int n_ack = receive_packet(recvbuffer, MAXPKTSIZE, NULL, timeoutpacket);
            if (n_ack < 0) {
                if (n_ack == RP_TIMEOUT) {
                    if (acktry == maxtimeouts) {
                        tftp_log(LOG_ERR, "Timeout #%d send ACK %d giving up", acktry, *block);
                        return;
                    }
                    tftp_log(LOG_WARNING, "Timeout #%d on ACK %d", acktry, *block);
                    acktry++;
                    ts->retries++;
                    if (inet_fseek(file, window[0].offset)) {
                        tftp_log(LOG_ERR, "seek_file failed: %s", strerror(EIO));
                        send_error(EIO + 100);
                        return;
                    }
                    *block = window[0].block;
                    windowblock = 0;
                    goto read_block;
                }
                if (debug & DEBUG_SIMPLE)
                    tftp_log(LOG_ERR, "Aborting: %s", rp_strerror(n_ack));
                return;
            }
            {
                /* A packet that is not an ACK is treated as an ACK for no
                 * block of the window, so the window is sent again. If it
                 * were ignored, windowblock would never be reset and would
                 * run past the end of window[]. */
                int is_ack = get16(recvbuffer) == ACK;
                uint16_t acked = get16(recvbuffer + 2);
                unsigned i;
                for (i = 0; i < windowblock && (!is_ack || acked != window[i].block); i++)
                    ;
                if (i == windowblock) {
                    if (debug & DEBUG_SIMPLE)
                        tftp_log(LOG_DEBUG, "ACK %d out of window", acked);
                    synchnet();
                    ts->retries++;
                    if (inet_fseek(file, window[0].offset)) {
                        tftp_log(LOG_ERR, "seek_file failed: %s", strerror(EIO));
                        send_error(EIO + 100);
                        return;
                    }
                    *block = window[0].block;
                    windowblock = 0;
                    goto read_block;
                }
                acktry = 0;
                for (unsigned j = 0; j <= i; j++) {
                    if (debug & DEBUG_SIMPLE)
                        tftp_log(LOG_DEBUG, "ACKed block %d", window[j].block);
                    ts->blocks++;
                    ts->amount += (size_t)window[j].size;
                }
                if (i + 1 != windowblock) {         /* a partial ACK: from the first un-ACKed */
                    if (debug & DEBUG_SIMPLE)
                        tftp_log(LOG_DEBUG, "Partial ACK");
                    if (inet_fseek(file, window[i + 1].offset)) {
                        tftp_log(LOG_ERR, "seek_file failed: %s", strerror(EIO));
                        send_error(EIO + 100);
                        return;
                    }
                    *block = window[i + 1].block;
                    windowblock = 0;
                    ts->retries++;
                    goto read_block;
                }
                windowblock = 0;
            }
        }
        *block = next_block(*block, ts);
        gettimeofday(&ts->tstop, NULL);
    } while (size == segsize);
}

static void tftp_receive(uint16_t *block, struct stats *ts, uint8_t *firstblock, int fb_size)
{
    static uint8_t recvbuffer[MAXPKTSIZE];
    int n_data = 0, windowblock = 0;
    ts->amount = 0;
    if (firstblock) {
        long w = write_file(firstblock + 4, fb_size);
        ts->amount += w > 0 ? (size_t)w : 0;
        ts->blocks++;
        windowblock++;
        if (windowsize == 1 || fb_size != segsize) {
            for (int i = 0;; i++) {
                if (send_ack(*block) == 0)
                    break;
                if (i == maxtimeouts) {
                    tftp_log(LOG_ERR, "Cannot send ACK packet #%d, giving up", *block);
                    return;
                }
                tftp_log(LOG_ERR, "Cannot send ACK packet #%d, trying again", *block);
            }
        }
        if (fb_size != segsize) {
            gettimeofday(&ts->tstop, NULL);
            return;
        }
    }
    do {
        uint16_t oldblock = *block;
        *block = next_block(*block, ts);
        for (int retry = 0;; retry++) {
            if (debug & DEBUG_SIMPLE)
                tftp_log(LOG_DEBUG, "Receiving DATA block %d (window block %d)", *block, windowblock);
            n_data = receive_packet(recvbuffer, MAXPKTSIZE, NULL, timeoutpacket);
            if (n_data < 0) {
                if (retry == maxtimeouts) {
                    tftp_log(LOG_ERR, "Timeout #%d on DATA block %d, giving up", retry, *block);
                    return;
                }
                if (n_data == RP_TIMEOUT) {
                    tftp_log(LOG_WARNING, "Timeout #%d on DATA block %d", retry, *block);
                    send_ack(oldblock);
                    windowblock = 0;
                    continue;
                }
                if (debug & DEBUG_SIMPLE)
                    tftp_log(LOG_DEBUG, "Aborting: %s", rp_strerror(n_data));
                return;
            }
            if (get16(recvbuffer) == DATA) {
                uint16_t got = get16(recvbuffer + 2);
                ts->blocks++;
                if (got == *block)
                    break;
                uint16_t windowstart = *block > windowsize ? (uint16_t)(*block - windowsize) : 0;
                if (got > windowstart && got < *block) {
                    if (debug & DEBUG_SIMPLE)
                        tftp_log(LOG_DEBUG, "Ignoring duplicate DATA block %d", got);
                    windowblock++;
                    retry = 0;
                    continue;
                }
                tftp_log(LOG_WARNING, "Expected DATA block %d, got block %d", *block, got);
                synchnet();
                tftp_log(LOG_INFO, "Trying to sync");
                *block = oldblock;
                ts->retries++;
                goto send_ack;
            } else {
                tftp_log(LOG_WARNING, "Expected DATA block, got %s block", packettype(get16(recvbuffer)));
            }
        }
        if (n_data > 0) {
            long w = write_file(recvbuffer + 4, n_data);
            if (w < 0 || (w == 0 && !convert)) {    /* netascii may leave nothing to write */
                tftp_log(LOG_ERR, "write_file returned %ld", w);
                send_error(w < 0 ? EIO + 100 : ENOSPACE);
                return;
            }
            ts->amount += (size_t)w;
        }
        windowblock++;
        if (windowblock < windowsize && n_data == segsize)
            continue;
    send_ack:
        for (int i = 0;; i++) {
            if (send_ack(*block) == 0) {
                if (debug & DEBUG_SIMPLE)
                    tftp_log(LOG_DEBUG, "Sent ACK for %d", *block);
                windowblock = 0;
                break;
            }
            if (i == maxtimeouts) {
                tftp_log(LOG_ERR, "Cannot send ACK packet #%d, giving up", *block);
                return;
            }
            tftp_log(LOG_ERR, "Cannot send ACK packet #%d, trying again", *block);
        }
        gettimeofday(&ts->tstop, NULL);
    } while (n_data == segsize);
}

/* The request, retried; the first answer's port is the transfer's.  The
 * answer's length after its header, or <0 */
static int request(int op, const char *name, uint8_t *rbuf)
{
    set_peer_port(port[0] ? (unsigned)atoi(port) : TFTP_PORT);
    int n = RP_TIMEOUT, i;
    for (i = 0; i < 12; i++) {
        if (debug & DEBUG_SIMPLE)
            out("%s %s\n", op == RRQ ? "Requesting" : "Sending", name);
        if (send_request(op, name, mode)) {
            out("Cannot send %s packet\n", op == RRQ ? "RRQ" : "WRQ");
            return RP_RECVFROM;
        }
        struct sockaddr_storage from;
        n = receive_packet(rbuf, MAXPKTSIZE, &from, timeoutpacket);
        if (n >= 0 || n == RP_ERROR) {
            set_peer_port(port_of(&from));
            break;
        }
        if (n == RP_TIMEOUT) {
            out("Try %d, didn't receive answer from remote.\n", i + 1);
            continue;
        }
        break;
    }
    if (i == 12) {
        out("Transfer timed out.\n");
        return RP_TIMEOUT;
    }
    return n;
}

static void xmitfile(struct inet_file *f, const char *name)
{
    static uint8_t rbuf[MAXPKTSIZE];
    struct stats ts;
    stats_init(&ts);
    int n = request(WRQ, name, rbuf);
    if (n == RP_ERROR) {
        out("Got ERROR, aborted\n");
        return;
    }
    if (n < 0)
        return;
    if (get16(rbuf) == OACK) {
        if (!options_rfc_enabled) {
            out("Got OACK while options are not enabled!\n");
            send_error(EBADOP);
            return;
        }
        parse_options(rbuf + 2, n + 2);
    }
    file_init(f, mode);
    uint16_t block = 1;
    tftp_send(&block, &ts);
    if (ts.amount > 0 && !escaped)
        printstats("Sent", &ts);
    txrx_error = 1;
}

static void recvfile(struct inet_file *f, const char *name)
{
    static uint8_t rbuf[MAXPKTSIZE];
    struct stats ts;
    stats_init(&ts);
    int n = request(RRQ, name, rbuf);
    if (n == RP_ERROR) {
        tftp_log(LOG_ERR, "Error code %d: %s", get16(rbuf + 2), (char *)rbuf + 4);
        return;
    }
    if (n < 0)
        return;
    file_init(f, mode);
    uint16_t block;
    if (get16(rbuf) == OACK) {
        if (!options_rfc_enabled) {
            out("Got OACK while options are not enabled!\n");
            send_error(EBADOP);
            return;
        }
        parse_options(rbuf + 2, n + 2);
        if (send_ack(0)) {
            out("Cannot send ACK on OACK.\n");
            return;
        }
        block = 0;
        tftp_receive(&block, &ts, NULL, 0);
    } else {
        block = 1;
        tftp_receive(&block, &ts, rbuf, n);
    }
    if (ts.amount > 0 && !escaped)
        printstats("Received", &ts);
}

/* ---- the commands (main.c) ------------------------------------------------------- */

#define MAXLINE 1024
#define MAX_MARGV 20

static int margc;
static char *margv[MAX_MARGV];
static char line[MAXLINE];

static void makeargv(char *l)
{
    char *cp = l;
    margc = 0;
    char *nl = strchr(l, '\n');
    if (nl)
        *nl = 0;
    while (margc < MAX_MARGV - 1 && *cp) {
        while (isspace((unsigned char)*cp))
            cp++;
        if (!*cp)
            break;
        margv[margc++] = cp;
        while (*cp && !isspace((unsigned char)*cp))
            cp++;
        if (!*cp)
            break;
        *cp++ = 0;
    }
    margv[margc] = NULL;
}

/* "(to) " and the like: the line, prefixed, split; 0 on Escape */
static int prompt_for(const char *prefix, const char *prompt)
{
    out("%s", prompt);
    snprintf(line, sizeof line, "%s", prefix);
    size_t l = strlen(line);
    if (inet_readline(line + l, sizeof line - l) < 0) {
        escaped = 1;
        return 0;
    }
    makeargv(line);
    return 1;
}

static void setpeer0(const char *host, const char *lport)
{
    if (connected) {
        close(peer);
        peer = -1;
    }
    connected = 0;
    struct addrinfo hints = { .ai_family = PF_UNSPEC, .ai_socktype = SOCK_DGRAM, .ai_protocol = IPPROTO_UDP,
                              .ai_flags = AI_CANONNAME },
                    *res0, *res;
    char p[16];
    snprintf(p, sizeof p, "%s", lport ? lport : "69");
    int error = getaddrinfo(host, p, &hints, &res0);
    if (error) {
        out("tftp: %s\n", gai_strerror(error));
        return;
    }
    const char *cause = "unknown";
    for (res = res0; res; res = res->ai_next) {
        if (res->ai_addrlen > sizeof peer_sock)
            continue;
        peer = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (peer < 0) {
            cause = "socket";
            continue;
        }
        struct sockaddr_storage any;
        memset(&any, 0, sizeof any);
        any.ss_family = (sa_family_t)res->ai_family;
#ifdef __APPLE__
        any.ss_len = (uint8_t)res->ai_addrlen;
#endif
        if (bind(peer, (struct sockaddr *)&any, res->ai_addrlen) < 0) {
            cause = "bind";
            close(peer);
            peer = -1;
            continue;
        }
        break;
    }
    if (peer < 0) {
        out("tftp: %s: %s\n", cause, strerror(errno));
    } else {
        memcpy(&peer_sock, res->ai_addr, res->ai_addrlen);
        peer_len = res->ai_addrlen;
        snprintf(hostname, sizeof hostname, "%s", res->ai_canonname ? res->ai_canonname : host);
        connected = 1;
    }
    freeaddrinfo(res0);
}

static void setpeer(int argc, char **argv)
{
    if (argc < 2) {
        if (!prompt_for("Connect ", "(to) "))
            return;
        argc = margc, argv = margv;
    }
    if (argc < 2 || argc > 3) {
        out("usage: %s [host [port]]\n", argv[0]);
        return;
    }
    if (argc == 3) {
        snprintf(port, sizeof port, "%s", argv[2]);
        setpeer0(argv[1], argv[2]);
    } else {
        setpeer0(argv[1], NULL);
    }
}

static const struct { const char *name, *mode; } modes[] = {
    { "ascii", "netascii" }, { "netascii", "netascii" }, { "binary", "octet" },
    { "image", "octet" },    { "octet", "octet" },       { NULL, NULL },
};

static void settftpmode(const char *m)
{
    snprintf(mode, sizeof mode, "%s", m);
    if (verbose)
        out("mode set to %s\n", mode);
}

static void modecmd(int argc, char **argv)
{
    if (argc < 2) {
        out("Using %s mode to transfer files.\n", mode);
        return;
    }
    if (argc == 2) {
        for (int i = 0; modes[i].name; i++)
            if (!strcmp(argv[1], modes[i].name)) {
                settftpmode(modes[i].mode);
                return;
            }
        out("%s: unknown mode\n", argv[1]);
    }
    out("usage: %s [", argv[0]);
    const char *sep = " ";
    for (int i = 0; modes[i].name; i++) {
        out("%s%s", sep, modes[i].name);
        if (*sep == ' ')
            sep = " | ";
    }
    out(" ]\n");
}

static void setbinary(int argc, char **argv)
{
    (void)argc, (void)argv;
    settftpmode("octet");
}

static void setascii(int argc, char **argv)
{
    (void)argc, (void)argv;
    settftpmode("netascii");
}

/* The last element of a remote path */
static char *tail(char *filename)
{
    while (*filename) {
        char *s = strrchr(filename, '/');
        if (!s)
            break;
        if (s[1])
            return s + 1;
        *s = 0;
    }
    return filename;
}

static void putusage(const char *s)
{
    out("usage: %s file [remotename]\n", s);
    out("       %s file host:remotename\n", s);
    out("       %s file1 file2 ... fileN [[host:]remote-directory]\n", s);
}

static void getusage(const char *s)
{
    out("usage: %s file [localname]\n", s);
    out("       %s [host:]file [localname]\n", s);
    out("       %s [host1:]file1 [host2:]file2 ... [hostN:]fileN\n", s);
}

static void put_one(const char *local, const char *targ)
{
    struct inet_file f;
    if (inet_fopen(&f, local, 0)) {
        out("Could not open %s for reading\n", local);
        return;
    }
    char v[16];
    snprintf(v, sizeof v, "%ld", inet_fsize(local));
    set_option(OPT_TSIZE, v);
    if (verbose)
        out("putting %s to %s:%s [%s]\n", local, hostname, targ, mode);
    xmitfile(&f, targ);
    inet_fclose(&f);
}

static void put(int argc, char **argv)
{
    if (argc < 2) {
        if (!prompt_for("send ", "(file) "))
            return;
        argc = margc, argv = margv;
    }
    if (argc < 2) {
        putusage(argv[0]);
        return;
    }
    char *targ = argv[argc - 1];
    if (strrchr(argv[argc - 1], ':')) {
        for (int n = 1; n < argc - 1; n++)
            if (strchr(argv[n], ':')) {
                putusage(argv[0]);
                return;
            }
        char *lcp = argv[argc - 1];
        targ = strrchr(lcp, ':');
        *targ++ = 0;
        if (lcp[0] == '[' && lcp[strlen(lcp) - 1] == ']') {
            lcp[strlen(lcp) - 1] = 0;
            lcp++;
        }
        setpeer0(lcp, NULL);
    }
    if (!connected) {
        out("No target machine specified.\n");
        return;
    }
    if (argc < 4) {
        put_one(argc == 2 ? tail(targ) : argv[1], targ);
        return;
    }
    char dir[MAXLINE];                              /* the target is a directory */
    for (int n = 1; n < argc - 1 && !escaped; n++) {
        snprintf(dir, sizeof dir, "%s/%s", targ, tail(argv[n]));
        put_one(argv[n], dir);
    }
}

static void get_one(const char *src, const char *local)
{
    struct inet_file f;
    if (inet_fopen(&f, local, 1)) {
        out("Could not open %s for writing\n", local);
        return;
    }
    if (verbose)
        out("getting from %s:%s to %s [%s]\n", hostname, src, local, mode);
    recvfile(&f, src);
    inet_fclose(&f);
}

static void get(int argc, char **argv)
{
    if (argc < 2) {
        if (!prompt_for("get ", "(files) "))
            return;
        argc = margc, argv = margv;
    }
    if (argc < 2) {
        getusage(argv[0]);
        return;
    }
    if (!connected)
        for (int n = 1; n < argc; n++)
            if (!strrchr(argv[n], ':')) {
                out("No remote host specified and no host given for file '%s'\n", argv[n]);
                getusage(argv[0]);
                return;
            }
    for (int n = 1; n < argc && !escaped; n++) {
        char *src = strrchr(argv[n], ':');
        if (!src) {
            src = argv[n];
        } else {
            *src++ = 0;
            char *lcp = argv[n];
            if (lcp[0] == '[' && lcp[strlen(lcp) - 1] == ']') {
                lcp[strlen(lcp) - 1] = 0;
                lcp++;
            }
            setpeer0(lcp, NULL);
            if (!connected)
                continue;
        }
        if (argc < 4) {
            get_one(src, argc == 3 ? argv[2] : tail(src));
            break;
        }
        get_one(src, tail(src));
    }
}

static void settimeoutpacket(int argc, char **argv)
{
    if (argc < 2) {
        if (!prompt_for("Packet timeout ", "(value) "))
            return;
        argc = margc, argv = margv;
    }
    if (argc != 2) {
        out("usage: %s value\n", argv[0]);
        return;
    }
    int t = atoi(argv[1]);
    if (t < 0) {
        out("%s: bad value\n", argv[1]);
        return;
    }
    settimeouts(t, timeoutnetwork);
}

static void settimeoutnetwork(int argc, char **argv)
{
    if (argc < 2) {
        if (!prompt_for("Network timeout ", "(value) "))
            return;
        argc = margc, argv = margv;
    }
    if (argc != 2) {
        out("usage: %s value\n", argv[0]);
        return;
    }
    int t = atoi(argv[1]);
    if (t < 0) {
        out("%s: bad value\n", argv[1]);
        return;
    }
    settimeouts(timeoutpacket, t);
}

static void showstatus(int argc, char **argv)
{
    (void)argc, (void)argv;
    out("Remote host: %s\n", connected ? hostname : "none specified yet");
    out("RFC2347 Options support: %s\n", options_rfc_enabled ? "enabled" : "disabled");
    out("Non-RFC defined options support: %s\n", options_extra_enabled ? "enabled" : "disabled");
    out("Mode: %s\n", mode);
    out("Verbose: %s\n", verbose ? "on" : "off");
    out("Debug: %s\n", debug_show(debug));
    out("Artificial packetloss: %d in 100 packets\n", packetdroppercentage);
    out("Segment size: %d bytes\n", segsize);
    out("Network timeout: %d seconds\n", timeoutpacket);
    out("Maximum network timeout: %d seconds\n", timeoutnetwork);
    out("Maximum timeouts: %d \n", maxtimeouts);
}

static void setverbose(int argc, char **argv)
{
    (void)argc, (void)argv;
    verbose = !verbose;
    out("Verbose mode %s.\n", verbose ? "on" : "off");
}

static void setoptions(int argc, char **argv)
{
    if (argc == 2) {
        if (!strcasecmp(argv[1], "enable") || !strcasecmp(argv[1], "on"))
            options_extra_enabled = options_rfc_enabled = 1;
        if (!strcasecmp(argv[1], "disable") || !strcasecmp(argv[1], "off"))
            options_extra_enabled = options_rfc_enabled = 0;
        if (!strcasecmp(argv[1], "extra"))
            options_extra_enabled = !options_extra_enabled;
    }
    out("Support for RFC2347 style options are now %s.\n", options_rfc_enabled ? "enabled" : "disabled");
    out("Support for non-RFC defined options are now %s.\n", options_extra_enabled ? "enabled" : "disabled");
    out("\nThe following options are available:\n"
        "\toptions on\t: enable support for RFC2347 style options\n"
        "\toptions off\t: disable support for RFC2347 style options\n"
        "\toptions extra\t: toggle support for non-RFC defined options\n");
}

static void setrollover(int argc, char **argv)
{
    if (argc == 2) {
        if (!strcasecmp(argv[1], "never") || !strcasecmp(argv[1], "none"))
            options[OPT_ROLLOVER].has_request = 0;
        if (!strcmp(argv[1], "1"))
            set_option(OPT_ROLLOVER, "1");
        if (!strcmp(argv[1], "0"))
            set_option(OPT_ROLLOVER, "0");
    }
    out("Support for the rollover options is %s.\n", options[OPT_ROLLOVER].has_request ? "enabled" : "disabled");
    if (options[OPT_ROLLOVER].has_request)
        out("Block rollover will be to block %s.\n", options[OPT_ROLLOVER].request);
    out("\nThe following rollover options are available:\n"
        "\trollover 0\t: rollover to block zero (default)\n"
        "\trollover 1\t: rollover to block one\n"
        "\trollover never\t: do not support the rollover option\n"
        "\trollover none\t: do not support the rollover option\n");
}

static void setdebug(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        debug ^= debug_find(argv[i]);
    out("The following debugging is enabled: %s\n", debug_show(debug));
    out("\nThe following debugs are available:\n");
    for (int i = 0; debugs[i].name; i++)
        out("\t%s\t%s\n", debugs[i].name, debugs[i].desc);
}

static void setblocksize(int argc, char **argv)
{
    if (!options_rfc_enabled)
        out("RFC2347 style options are not enabled (but proceeding anyway)\n");
    if (argc != 1) {
        int size = atoi(argv[1]);
        char v[16];
        if (size < BLKSIZE_MIN || size > BLKSIZE_MAX) {
            out("Blocksize should be between %d and %d bytes.\n", BLKSIZE_MIN, BLKSIZE_MAX);
            return;
        } else if (size > MAXDGRAM - 4) {
            out("Blocksize can't be bigger than %ld bytes due to the net.inet.udp.maxdgram sysctl limitation.\n",
                (long)MAXDGRAM - 4);
            snprintf(v, sizeof v, "%ld", (long)MAXDGRAM - 4);
        } else {
            snprintf(v, sizeof v, "%d", size);
        }
        set_option(OPT_BLKSIZE, v);
    }
    out("Blocksize is now %s bytes.\n", options[OPT_BLKSIZE].has_request ? options[OPT_BLKSIZE].request : "(null)");
}

static void setblocksize2(int argc, char **argv)
{
    if (!options_rfc_enabled || !options_extra_enabled)
        out("RFC2347 style or non-RFC defined options are not enabled (but proceeding anyway)\n");
    if (argc != 1) {
        int size = atoi(argv[1]), i;
        for (i = 0; sizes2[i] && sizes2[i] != size; i++)
            ;
        if (!sizes2[i]) {
            out("Blocksize2 should be a power of two between 8 and 32768.\n");
            return;
        }
        char v[16];
        snprintf(v, sizeof v, "%d", size);
        set_option(OPT_BLKSIZE2, v);
    }
    out("Blocksize2 is now %s bytes.\n",
        options[OPT_BLKSIZE2].has_request ? options[OPT_BLKSIZE2].request : "(null)");
}

static void setpacketdrop(int argc, char **argv)
{
    if (argc != 1)
        packetdroppercentage = atoi(argv[1]);
    out("Randomly %d in 100 packets will be dropped\n", packetdroppercentage);
}

static void setwindowsize(int argc, char **argv)
{
    if (!options_rfc_enabled)
        out("RFC2347 style options are not enabled (but proceeding anyway)\n");
    if (argc != 1) {
        int size = atoi(argv[1]);
        if (size < WINDOWSIZE_MIN || size > WINDOWSIZE_MAX) {
            out("Windowsize should be between %d and %d blocks.\n", WINDOWSIZE_MIN, WINDOWSIZE_MAX);
            return;
        }
        char v[16];
        snprintf(v, sizeof v, "%d", size);
        set_option(OPT_WINDOWSIZE, v);
    }
    out("Windowsize is now %s blocks.\n",
        options[OPT_WINDOWSIZE].has_request ? options[OPT_WINDOWSIZE].request : "(null)");
}

static int quitting;

static void quit(int argc, char **argv)
{
    (void)argc, (void)argv;
    quitting = 1;
}

static void help(int argc, char **argv);

static const struct cmd {
    const char *name;
    void (*handler)(int, char **);
    const char *help;
} cmdtab[] = {
    { "connect", setpeer, "connect to remote tftp" },
    { "mode", modecmd, "set file transfer mode" },
    { "put", put, "send file" },
    { "get", get, "receive file" },
    { "quit", quit, "exit tftp" },
    { "verbose", setverbose, "toggle verbose mode" },
    { "status", showstatus, "show current status" },
    { "binary", setbinary, "set mode to octet" },
    { "ascii", setascii, "set mode to netascii" },
    { "rexmt", settimeoutpacket, "set per-packet retransmission timeout[-]" },
    { "timeout", settimeoutnetwork, "set total retransmission timeout" },
    { "trace", setdebug, "enable 'debug packet'[-]" },
    { "debug", setdebug, "enable verbose output" },
    { "blocksize", setblocksize, "set blocksize[*]" },
    { "blocksize2", setblocksize2, "set blocksize as a power of 2[**]" },
    { "rollover", setrollover, "rollover after 64K packets[**]" },
    { "options", setoptions, "enable or disable RFC2347 style options" },
    { "help", help, "print help information" },
    { "packetdrop", setpacketdrop, "artificial packetloss feature" },
    { "windowsize", setwindowsize, "set windowsize[*]" },
    { "?", help, "print help information" },
    { NULL, NULL, NULL },
};

#define AMBIGUOUS ((const struct cmd *)-1)

static const struct cmd *getcmd(const char *name)
{
    const struct cmd *found = NULL;
    int nmatches = 0, longest = 0;
    for (const struct cmd *c = cmdtab; c->name; c++) {
        const char *p = c->name, *q;
        for (q = name; *q == *p++; q++)
            if (!*q)
                return c;                           /* exact */
        if (!*q) {                                  /* a prefix */
            if (q - name > longest) {
                longest = (int)(q - name);
                nmatches = 1;
                found = c;
            } else if (q - name == longest) {
                nmatches++;
            }
        }
    }
    return nmatches > 1 ? AMBIGUOUS : found;
}

static void help(int argc, char **argv)
{
    if (argc == 1) {
        out("Commands may be abbreviated.  Commands are:\n\n");
        for (const struct cmd *c = cmdtab; c->name; c++)
            out("%-*s\t%s\n", (int)sizeof "connect", c->name, c->help);
        out("\n[-] : You shouldn't use these ones anymore.\n");
        out("[*] : RFC2347 options support required.\n");
        out("[**] : Non-standard RFC2347 option.\n");
        return;
    }
    while (--argc > 0) {
        const char *arg = *++argv;
        const struct cmd *c = getcmd(arg);
        if (c == AMBIGUOUS)
            out("?Ambiguous help command: %s\n", arg);
        else if (!c)
            out("?Invalid help command: %s\n", arg);
        else
            out("%s\n", c->help);
    }
}

/* tftp://host/file[;mode=m]: fetched at once */
static int urihandling(const char *uri)
{
    char buf[MAXLINE];
    snprintf(buf, sizeof buf, "%s", uri);
    char *host = buf + 7, *s = strchr(host, '/');
    if (!s) {
        out("Invalid URI: Couldn't find / after hostname\n");
        return 1;
    }
    *s = 0;
    char *path = s + 1;
    if ((s = strchr(path, ';'))) {
        *s = 0;
        char *opts = s + 1;
        if (!strncmp(opts, "mode=", 5)) {
            const char *tmode = opts + 5;
            int i;
            for (i = 0; modes[i].name && strcmp(modes[i].name, tmode); i++)
                ;
            if (!modes[i].name) {
                out("Invalid mode: '%s'\n", mode);
                return 1;
            }
            settftpmode(modes[i].mode);
        }
    } else {
        settftpmode("octet");
    }
    setpeer0(host, NULL);
    snprintf(line, sizeof line, "get %s", path);
    makeargv(line);
    get(margc, margv);
    return txrx_error;
}

os_error *inet_tftp(const struct inet_args *a)
{
    reset();
    int rc = 0;
    if (a->argc > 1) {
        if (!strcmp(a->argv[1], "-help"))
            return out("Usage:   tftp [host] [port]\n"
                       "         trivial file transfer program\n");
        if (!strncmp(a->argv[1], "tftp://", 7)) {
            rc = urihandling(a->argv[1]);
            if (escaped)
                out("\n");
            goto done;
        }
        char *argv[4] = { "connect", a->argv[1], a->argc > 2 ? a->argv[2] : NULL, NULL };
        setpeer(a->argc > 2 ? 3 : 2, argv);
    }
    quitting = 0;
    while (!quitting) {
        if (escaped) {
            out("\n");
            escaped = 0;
        }
        out("tftp> ");
        if (inet_readline(line, sizeof line) < 0) {
            escaped = 1;
            continue;
        }
        if (!line[0])
            continue;
        makeargv(line);
        if (!margc)
            continue;
        const struct cmd *c = getcmd(margv[0]);
        if (c == AMBIGUOUS)
            out("?Ambiguous command\n");
        else if (!c)
            out("?Invalid command\n");
        else
            c->handler(margc, margv);
    }
    rc = txrx_error;
done:
    if (peer >= 0)
        close(peer);
    peer = -1;
    inet_exit(rc);
    return NULL;
}
