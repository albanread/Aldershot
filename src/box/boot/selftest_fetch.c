/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_fetch.c: URL_Fetcher and AcornHTTP (modules/urlfetcher,
 * modules/acornhttp), through URL_Fetcher's SWIs as a browser uses them,
 * against RISC OS 5's contracts (URL Docs/APISpec, AcornHTTP Docs/SWIs and
 * ImpDetails).
 *
 * The test runs on loopback.  It starts an HTTP server of its own on a
 * thread.  The server answers each request by its path and keeps what it
 * was sent.  The box's own checks exercise https: against a real server.
 * Here http: is used, as it carries the same code.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check

enum { URL_REGISTER = 0x83E00, URL_GETURL, URL_STATUS, URL_READDATA, URL_SETPROXY, URL_STOP, URL_DEREGISTER,
       URL_PARSEURL, URL_ENUMSCHEMES, URL_ENUMPROXIES };
enum { HTTP_REGISTERMETHOD = 0x83FBB, HTTP_DEREGISTERMETHOD, HTTP_ADDCOOKIE, HTTP_CONSUMECOOKIE,
       HTTP_ENUMCOOKIES };

static uint32_t call(uint32_t swi, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    for (int i = 0; i < 10; i++)
        s.r[i] = r[i];
    ros_swi(&s, ROS_X_BIT | swi);
    for (int i = 0; i < 10; i++)
        r[i] = s.r[i];
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static uint32_t text(const char *s)
{
    char *b = ros_rma_alloc(strlen(s) + 1);
    strcpy(b, s);
    return ros_addr(b);
}

/* ---- the server ------------------------------------------------------------ */

/* The requests the server answers: chunked, length, post, other, moved,
 * HEAD, and, in the box, length again for a read into bad memory */
#ifdef ROS_ARENA_HOSTED
#define REQUESTS 8
#else
#define REQUESTS 9
#endif

static struct {
    int s, requests;
    uint16_t port;
    char got[REQUESTS][1024];       /* each request, as it came */
} srv;

static void reply(int c, const char *request)
{
    char path[64] = "";
    sscanf(request, "%*s %63s", path);
    const char *r;
    static char cookies[9600];
    if (!strcmp(path, "/cookies")) {
        /* A big cookie, attributes with no value, a tab in a value, an empty path */
        size_t n = (size_t)snprintf(cookies, sizeof cookies,
                                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n"
                                    "Set-Cookie: big=");
        memset(cookies + n, 'A', 9000);
        n += 9000;
        snprintf(cookies + n, sizeof cookies - n,
                 "; path=/\r\nSet-Cookie: bare=1; domain; path; max-age; expires; lastaccess\r\n"
                 "Set-Cookie: tab=a\tb; path=/\r\nSet-Cookie: emptypath=1; path=\r\n\r\nok");
        r = cookies;
    } else if (!strcmp(path, "/chunked"))
        r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close, X-Hop\r\nX-Hop: gone\r\n"
            "Content-Type: text/plain\r\nSet-Cookie: flavour=oat; path=/\r\n\r\n"
            "5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n";
    else if (!strcmp(path, "/length"))
        r = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nhello again";
    else if (!strcmp(path, "/moved"))
        r = "HTTP/1.1 302 Found\r\nLocation: /length\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    else
        r = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    send(c, r, strlen(r), 0);
}

static void *server(void *arg)
{
    (void)arg;
    for (int i = 0; i < REQUESTS; i++) {
        struct pollfd p = { .fd = srv.s, .events = POLLIN };
        if (poll(&p, 1, 10000) <= 0)
            break;
        int c = accept(srv.s, NULL, NULL);
        if (c < 0)
            break;
        char buf[16384];
        size_t n = 0;
        for (;;) {                                  /* the head, and a body of Content-Length */
            struct pollfd q = { .fd = c, .events = POLLIN };
            if (poll(&q, 1, 5000) <= 0)
                break;
            ssize_t r = recv(c, buf + n, sizeof buf - 1 - n, 0);
            if (r <= 0)
                break;
            n += (size_t)r;
            buf[n] = 0;
            char *end = strstr(buf, "\r\n\r\n");
            if (end) {
                const char *cl = strcasestr(buf, "\r\nContent-Length:");
                size_t want = cl && cl < end ? (size_t)atoi(cl + 17) : 0;
                if (n >= (size_t)(end + 4 - buf) + want)
                    break;
            }
        }
        snprintf(srv.got[i], sizeof srv.got[i], "%s", buf);
        srv.requests++;
        reply(c, buf);
        close(c);
    }
    return NULL;
}

/* ---- a fetch, as a browser makes one ---------------------------------------------- */

struct result {
    uint32_t error;
    char errmess[128];
    char data[2048];
    size_t len;
    uint32_t status, code;
};

static void fetch(uint32_t method, const char *url, const char *extra, uint32_t wanted, struct result *res)
{
    memset(res, 0, sizeof *res);
    uint32_t r[10] = { 0 };
    call(URL_REGISTER, r);
    uint32_t id = r[1];
    uint32_t u = text(url), x = extra ? text(extra) : 0;
    uint32_t g[10] = { 0, id, method, u, x, wanted };
    res->error = call(URL_GETURL, g);
    uint8_t *buf = ros_rma_alloc(512);
    for (int i = 0; i < 4000 && !res->error; i++) {
        uint32_t d[10] = { 0, id, ros_addr(buf), 512 };
        uint32_t e = call(URL_READDATA, d);
        if (e) {
            res->error = e;
            snprintf(res->errmess, sizeof res->errmess, "%s", ((os_error *)ros_ptr(d[0]))->errmess);
            break;
        }
        if (d[4] && res->len + d[4] < sizeof res->data) {
            memcpy(res->data + res->len, buf, d[4]);
            res->len += d[4];
        }
        res->status = d[0];
        if (d[5] == 0 || (d[0] & 32))
            break;
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        ROS_BLOCKING(nanosleep(&ts, NULL));
    }
    res->data[res->len] = 0;
    uint32_t st[10] = { 0, id };
    if (!call(URL_STATUS, st))
        res->code = st[2];
    uint32_t dr[10] = { 0, id };
    call(URL_DEREGISTER, dr);
    ros_rma_free(buf);
    ros_rma_free(ros_ptr(u));
    if (x)
        ros_rma_free(ros_ptr(x));
}

/* ---- URL_ParseURL -------------------------------------------------------------- */

static int quick(const char *base, const char *rel, const char *want, char *got, size_t max)
{
    uint32_t b = text(base), rr = rel ? text(rel) : 0;
    uint8_t *buf = ros_rma_alloc(256);
    uint32_t r[10] = { 0, 3, b, rr, ros_addr(buf), 256 };
    uint32_t e = call(URL_PARSEURL, r);
    snprintf(got, max, "%s", e ? "(error)" : (char *)buf);
    ros_rma_free(buf), ros_rma_free(ros_ptr(b));
    if (rr)
        ros_rma_free(ros_ptr(rr));
    return !e && !strcmp(got, want);
}

static void parse(void)
{
    char got[256];
    /* Alone, a URL is canonicalised and its ".." stays, as URL 0.58's does */
    int ok = quick("HTTP://WWW.Acorn.COM:80/a/b/../c?x#y", NULL, "http://www.acorn.com/a/b/../c?x#y", got, sizeof got);
    check(ok, "URL_ParseURL 3 -- canonical: scheme and host lower case, the default port gone",
          "\"%s\"", got);
    ok = quick("http://h/a/b/c", "../d", "http://h/a/d", got, sizeof got) &&
         quick("http://h/a/b/c", "/e?q", "http://h/e?q", got, sizeof got) &&
         quick("http://h/a/b/c", "g#s", "http://h/a/b/g#s", got, sizeof got) &&
         quick("http://h:8080/", "ftp://f/x", "ftp://f/x", got, sizeof got);
    check(ok, "URL_ParseURL 3 -- relative URLs resolved (RFC 1808)", "\"%s\"", got);

    uint32_t url = text("https://user:pw@Example.org:8443/p/q.html?a=1#top");
    uint32_t *block = ros_rma_alloc(40);
    uint32_t r[10] = { 0, 0, url, 0, ros_addr(block) };
    uint32_t e = call(URL_PARSEURL, r);
    uint32_t lens[10];
    memcpy(lens, block, sizeof lens);
    char *bufs = ros_rma_alloc(512);
    size_t at = 0;
    for (int i = 0; i < 10; i++) {
        block[i] = lens[i] ? ros_addr(bufs + at) : 0;
        at += lens[i];
    }
    uint32_t r2[10] = { 0, 1, url, 0, ros_addr(block) };
    uint32_t e2 = call(URL_PARSEURL, r2);
    const char *f[10];
    for (int i = 0; i < 10; i++)
        f[i] = block[i] ? (const char *)ros_ptr(block[i]) : "";
    ok = !e && !e2 && lens[1] == 6 && !strcmp(f[0], "https://user:pw@example.org:8443/p/q.html?a=1#top") &&
         !strcmp(f[1], "https") && !strcmp(f[2], "example.org") && !strcmp(f[3], "8443") &&
         !strcmp(f[4], "user") && !strcmp(f[5], "pw") && !strcmp(f[7], "p/q.html") && !strcmp(f[8], "a=1") &&
         !strcmp(f[9], "top");
    check(ok, "URL_ParseURL 0 and 1 -- the lengths, then the ten fields", "&%X &%X \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"",
          e, e2, f[0], f[2], f[3], f[7], f[8]);
    ros_rma_free(bufs), ros_rma_free(block), ros_rma_free(ros_ptr(url));

    /* no URL at all: an error, not a fault */
    uint8_t *nb = ros_rma_alloc(256);
    uint32_t nr[10] = { 0, 3, 0, 0, ros_addr(nb), 256 };
    uint32_t ne = call(URL_PARSEURL, nr);
    check(ne != 0, "URL_ParseURL 3 with no URL -- an error, not a fault", "&%X", ne);
    ros_rma_free(nb);
}

void ros_selftest_fetch(void)
{
    parse();

    /* the schemes AcornHTTP registered */
    int http = 0, https = 0;
    for (uint32_t ctx = 0; ctx != (uint32_t)-1;) {
        uint32_t r[10] = { 0, ctx };
        if (call(URL_ENUMSCHEMES, r))
            break;
        ctx = r[1];
        if (ctx != (uint32_t)-1) {
            http |= !strcmp(ros_ptr(r[2]), "http:") && r[4] == 0x83F80;
            https |= !strcmp(ros_ptr(r[2]), "https:") && r[4] == 0x83F90;
        }
    }
    check(http && https, "URL_EnumerateSchemes -- AcornHTTP's http: (&83F80) and https: (&83F90)", " ");

    /* a proxy, set and read back, then cleared */
    uint32_t pb = text("http://proxy.test:3128/"), ps = text("ftp:");
    uint32_t sp[10] = { 0, 0, pb, ps, 0 };
    uint32_t ep[10] = { 0, 0, 0 };
    int pok = !call(URL_SETPROXY, sp) && !call(URL_ENUMPROXIES, ep) && ep[2] == 1 &&
              !strcmp(ros_ptr(ep[3]), "ftp:") && !strcmp(ros_ptr(ep[4]), "http://proxy.test:3128/");
    uint32_t cp[10] = { 0, 0, 0 };
    call(URL_SETPROXY, cp);
    uint32_t ep2[10] = { 0, 0, 0 };
    pok = pok && !call(URL_ENUMPROXIES, ep2) && ep2[2] == (uint32_t)-1;
    check(pok, "URL_SetProxy, URL_EnumerateProxies -- a global proxy for ftp:, then none", " ");
    ros_rma_free(ros_ptr(pb)), ros_rma_free(ros_ptr(ps));

    /* the server */
    struct sigaction ignore = { .sa_handler = SIG_IGN }, before;
    sigaction(SIGPIPE, &ignore, &before);
    srv.s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t ml = sizeof me;
    pthread_t th;
    if (srv.s < 0 || bind(srv.s, (struct sockaddr *)&me, sizeof me) < 0 || listen(srv.s, 8) < 0 ||
        getsockname(srv.s, (struct sockaddr *)&me, &ml) < 0 || pthread_create(&th, NULL, server, NULL)) {
        check(0, "AcornHTTP: an HTTP server on loopback", "none");
        sigaction(SIGPIPE, &before, NULL);
        return;
    }
    srv.port = ntohs(me.sin_port);
    char base[64], url[128];
    snprintf(base, sizeof base, "http://127.0.0.1:%u", srv.port);
    struct result res;

    /* 1: chunked, hop-by-hop headers, a cookie */
    snprintf(url, sizeof url, "%s/chunked", base);
    fetch(1, url, NULL, 2, &res);
    int ok = !res.error && res.code == 200 && !strncmp(res.data, "HTTP/1.0 200 OK\r\n", 17) &&
             !strstr(res.data, "Transfer-Encoding") && !strstr(res.data, "Connection") &&
             !strstr(res.data, "X-Hop") && !strstr(res.data, "Content-Length") &&
             strstr(res.data, "Content-Type: text/plain\r\n") && strstr(res.data, "\r\n\r\nhello, world") &&
             res.status == 32 && strstr(srv.got[0], "User-Agent: Acorn_HTTP/1.09\r\n") &&
             strstr(srv.got[0], "Accept: */*\r\n") && strstr(srv.got[0], "GET /chunked HTTP/1.1\r\n");
    check(ok, "AcornHTTP: GET, chunked -- HTTP/1.0 to the client, dechunked, Connection and what it names gone",
          "&%X %s code %u status %u \"%.200s\" / \"%.200s\"", res.error, res.errmess, res.code, res.status, res.data,
          srv.got[0]);

    /* the cookie is queued.  Once accepted, it goes with the next request. */
    uint32_t en[10] = { 0, 0 };
    int cok = !call(HTTP_ENUMCOOKIES, en) && en[1] && !strcmp(ros_ptr(en[5]), "flavour") &&
              !strcmp(ros_ptr(en[6]), "oat");
    uint32_t cons[10] = { 1, 0, en[1] };
    cok = cok && !call(HTTP_CONSUMECOOKIE, cons);

    /* 2: Content-Length, the head only wanted, and the cookie sent */
    snprintf(url, sizeof url, "%s/length", base);
    fetch(1, url, NULL, 2, &res);
    ok = !res.error && strstr(res.data, "Content-Length: 11\r\n\r\nhello again") &&
         strstr(srv.got[1], "Cookie: flavour=oat");
    check(ok && cok, "AcornHTTP: Content-Length put back last; Set-Cookie queued, consumed, then sent",
          "cookie %d \"%.200s\" / \"%.300s\"", cok, res.data, srv.got[1]);

    /* 3: POST, the client's headers and body */
    snprintf(url, sizeof url, "%s/post", base);
    fetch(4, url, "X-Client: yes\r\nHost: wrong.example\r\nUser-Agent: nope\r\n\r\nname=value", 0, &res);
    ok = !res.error && !strcmp(res.data, "ok") && strstr(srv.got[2], "POST /post HTTP/1.1\r\n") &&
         strstr(srv.got[2], "X-Client: yes\r\n") && !strstr(srv.got[2], "wrong.example") &&
         !strstr(srv.got[2], "nope") &&
         strstr(srv.got[2], "content-type: application/x-www-form-urlencoded\r\n") &&
         strstr(srv.got[2], "\r\n\r\nname=value");
    check(ok, "AcornHTTP: POST -- the client's headers less Host and User-Agent; its body; body only wanted",
          "\"%.100s\" / \"%.400s\"", res.data, srv.got[2]);

    /* 4: a registered method, and a redirection left to the client */
    uint32_t name = text("REPORT");
    uint32_t rm[10] = { name, 2 };
    uint32_t rme = call(HTTP_REGISTERMETHOD, rm);
    snprintf(url, sizeof url, "%s/other", base);
    fetch(rm[2], url, NULL, 2, &res);
    ok = !rme && rm[2] > 0 && strstr(srv.got[3], "REPORT /other HTTP/1.1\r\n") && !res.error;
    uint32_t dm[10] = { name, 2 };
    ok = ok && !call(HTTP_DEREGISTERMETHOD, dm);
    snprintf(url, sizeof url, "%s/moved", base);
    fetch(1, url, NULL, 2, &res);
    ok = ok && res.code == 302 && strstr(res.data, "HTTP/1.0 302 Found\r\n") && strstr(res.data, "Location: /length");
    check(ok, "AcornHTTP: HTTP_RegisterMethod (REPORT); a 302 given to the client, not followed",
          "&%X method %u \"%.200s\"", rme, rm[2], res.data);
    ros_rma_free(ros_ptr(name));

    /* 5: HEAD */
    snprintf(url, sizeof url, "%s/length", base);
    fetch(2, url, NULL, 1, &res);
    ok = !res.error && strstr(srv.got[5], "HEAD /length HTTP/1.1\r\n") && strstr(res.data, "HTTP/1.0 200 OK") &&
         !strstr(res.data, "hello again");
    check(ok, "AcornHTTP: HEAD -- the head alone", "&%X \"%.200s\"", res.error, res.data);

    /* 6, 7: Set-Cookie as a hostile server sends it.  A 9000-byte value must
     * not overflow the Cookie header's buffer, an attribute with no value
     * ("domain;") must not crash the parser, a tab in a value must not
     * become a field of the saved cookie file, and an empty path must not
     * be read before its string.  The cookies are accepted, then sent. */
    snprintf(url, sizeof url, "%s/cookies", base);
    fetch(1, url, NULL, 2, &res);
    int queued = 0, tab_queued = 0;
    for (int i = 0; i < 20; i++) {
        uint32_t q[10] = { 0, 0 };
        if (call(HTTP_ENUMCOOKIES, q) || !q[1])
            break;
        queued++;
        tab_queued |= !strcmp(ros_ptr(q[5]), "tab");
        uint32_t c[10] = { 1, 0, q[1] };
        call(HTTP_CONSUMECOOKIE, c);
    }
    int big_ok = 0, bare_ok = 0;
    for (uint32_t h = 0, i = 0; i < 50; i++) {
        uint32_t d[10] = { 1, h };
        if (call(HTTP_ENUMCOOKIES, d) || !d[1])
            break;
        h = d[1];
        big_ok |= !strcmp(ros_ptr(d[5]), "big") && strlen(ros_ptr(d[6])) == 9000;
        bare_ok |= !strcmp(ros_ptr(d[5]), "bare") && !strcmp(ros_ptr(d[6]), "1");
    }
    snprintf(url, sizeof url, "%s/after", base);
    fetch(1, url, NULL, 2, &res);
    check(!res.error && queued == 3 && !tab_queued && big_ok && bare_ok && strstr(srv.got[7], "Cookie: ") &&
              strstr(srv.got[7], "AAAAAAAA"),
          "AcornHTTP: a 9000-byte cookie, valueless attributes, a tab in a value, an empty path -- kept safely",
          "&%X queued %d tab %d big %d bare %d \"%.100s\"", res.error, queued, tab_queued, big_ok, bare_ok,
          srv.got[7]);

#ifndef ROS_ARENA_HOSTED
    /* 6 (the box, whose runtime turns a SWI's fault into the caller's
     * error): URL_ReadData into memory that is not there.  A data abort is
     * raised to the caller before AcornHTTP holds its lock, and nothing
     * taken, so the fetch goes on: the next reads, into good memory, get
     * all of it */
    snprintf(url, sizeof url, "%s/length", base);
    uint32_t rr[10] = { 0 };
    call(URL_REGISTER, rr);
    uint32_t id = rr[1], u = text(url);
    uint32_t g[10] = { 0, id, 1, u, 0, 0 };
    uint32_t ge = call(URL_GETURL, g);
    const os_error *caught = NULL;
    for (int i = 0; i < 4000 && !ge && !caught; i++) {
        uint32_t d[10] = { 0, id, ROS_DA_LIMIT, 512 };
        struct ros_handler h;
        if (ROS_TRY(&h)) {
            uint32_t e = call(URL_READDATA, d);
            ros_handler_pop(&h);
            if (e || d[4] || d[5] == 0 || (d[0] & 32))
                break;                  /* (an error, or bytes given, or the end: wrong) */
            struct timespec ts = { 0, 2 * 1000 * 1000 };
            ROS_BLOCKING(nanosleep(&ts, NULL));
        } else {
            caught = h.error;
        }
    }
    uint32_t abort_num = caught ? caught->errnum : 0;
    char got[64] = "";
    size_t len = 0;
    uint8_t *good = ros_rma_alloc(512);
    for (int i = 0; i < 4000 && caught && good; i++) {
        uint32_t d[10] = { 0, id, ros_addr(good), 512 };
        if (call(URL_READDATA, d))
            break;
        if (d[4] && len + d[4] < sizeof got) {
            memcpy(got + len, good, d[4]);
            len += d[4];
            got[len] = 0;
        }
        if (d[5] == 0 || (d[0] & 32))
            break;
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        ROS_BLOCKING(nanosleep(&ts, NULL));
    }
    uint32_t dr[10] = { 0, id };
    call(URL_DEREGISTER, dr);
    ros_rma_free(good);
    ros_rma_free(ros_ptr(u));
    check(!ge && abort_num == 0x80000002u && !strcmp(got, "hello again"),
          "AcornHTTP: URL_ReadData into memory that is not there -- the caller's data abort, taken "
          "before the module's lock; the fetch goes on, the next reads get all of it",
          "&%X %s; then \"%s\"", abort_num, caught ? caught->errmess : "no abort", got);
#endif

    ROS_BLOCKING(pthread_join(th, NULL));
    close(srv.s);
    sigaction(SIGPIPE, &before, NULL);

    /* failures: the original's errors */
    fetch(1, "http://127.0.0.1:1/", NULL, 2, &res);
    ok = res.error == 0x80DE25 && !strcmp(res.errmess, "Unable to connect to remote host");
    check(ok, "AcornHTTP: connection refused -- &80DE25, Unable to connect to remote host", "&%X \"%s\"", res.error,
          res.errmess);
    fetch(1, "http://nosuch.invalid/", NULL, 2, &res);
    ok = res.error == 0x80DE20 && !strcmp(res.errmess, "Remote host not found. Please check the URL");
    check(ok, "AcornHTTP: no such host -- &80DE20, Remote host not found", "&%X \"%s\"", res.error, res.errmess);
}
