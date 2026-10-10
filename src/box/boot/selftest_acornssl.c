/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_acornssl.c: AcornSSL over OpenSSL (modules/acornssl), through
 * its SWIs, against RISC OS 5's AcornSSL contract (doc/AcornSSL, sslmod.c).
 *
 * On loopback, so it needs no network and runs the same hosted and in the
 * box: a TLS server of the test's own on a thread, with a self-signed
 * certificate for "rosgd.test" made afresh.  The test claims UpCallV as
 * AcornSSL's desktop task would, to see the certificate chain offered by
 * UpCall_CertificateConfirm and to accept or reject it.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

enum { CREAT = 0x50F80, IOCTL, CONNECT, SHUTDOWN, CLOSE, GETSOCKOPT, WRITE, RECV, CREATESESSION, GETPEERNAME,
       GETSOCKNAME, SETSOCKOPT, STAT, VERSION, READ, SEND };
#define SO_HOSTNAME   0x11E0
#define SO_PROMPTTIME 0x11E1
#define FIONBIO       0x8004667Eu

/* ---- the server ------------------------------------------------------------- */

static struct {
    int s;
    uint16_t port;
    SSL_CTX *ctx;
    int connections;
    char version[3][16];            /* each connection's protocol */
    char got[3][16];                /* what each client sent */
} srv;

static SSL_CTX *server_ctx(void)
{
    EVP_PKEY *key = EVP_EC_gen("P-256");
    X509 *x = X509_new();
    if (!key || !x)
        return NULL;
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 0x2605);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"ROSGD", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"rosgd.test", -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509_set_pubkey(x, key);
    X509_EXTENSION *san = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, "DNS:rosgd.test");
    X509_add_ext(x, san, -1);
    X509_EXTENSION_free(san);
    X509_sign(x, key, EVP_sha256());
    SSL_CTX *c = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(c, x);
    SSL_CTX_use_PrivateKey(c, key);
    X509_free(x);
    EVP_PKEY_free(key);
    return c;
}

static void *server(void *arg)
{
    (void)arg;
    for (int i = 0; i < srv.connections; i++) {
        struct pollfd p = { .fd = srv.s, .events = POLLIN };
        if (poll(&p, 1, 10000) <= 0)
            break;
        int c = accept(srv.s, NULL, NULL);
        if (c < 0)
            break;
        SSL *ssl = SSL_new(srv.ctx);
        SSL_set_fd(ssl, c);
        if (SSL_accept(ssl) == 1) {
            snprintf(srv.version[i], sizeof srv.version[i], "%s", SSL_get_version(ssl));
            char buf[16] = "";
            int n = SSL_read(ssl, buf, 5);
            if (n > 0)
                memcpy(srv.got[i], buf, (size_t)n);
            SSL_write(ssl, "world", 5);
            SSL_shutdown(ssl);
        }
        SSL_free(ssl);
        close(c);
    }
    return NULL;
}

/* ---- the client, through the SWIs ------------------------------------------------ */

static uint32_t call(uint32_t swi, uint32_t r[6])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    for (int i = 0; i < 6; i++)
        s.r[i] = r[i];
    ros_swi(&s, ROS_X_BIT | swi);
    for (int i = 0; i < 6; i++)
        r[i] = s.r[i];
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static const char *errmess(uint32_t r0)
{
    return ((os_error *)ros_ptr(r0))->errmess;
}

/* The UpCall's claimant: what it was offered, and its answer */
static struct {
    int calls, ends;
    uint32_t flags;
    char subject[80], serial[16];
    uint32_t version, answer;
} up;

static int upcall(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == 26) {                            /* UpCall_CertificateConfirm */
        up.calls++;
        const uint32_t *e = ros_ptr(s->r[1]);
        up.flags = e[2];
        for (const uint8_t *t = (const uint8_t *)(e + 5); t[2]; t += 8) {
            uint32_t obj = *(const uint32_t *)(t + 4);
            if (t[2] == 2)                          /* CertTag_Subject */
                snprintf(up.subject, sizeof up.subject, "%s", (const char *)ros_ptr(obj));
            if (t[2] == 5)                          /* CertTag_Serial */
                snprintf(up.serial, sizeof up.serial, "%s", (const char *)ros_ptr(obj));
            if (t[2] == 6)                          /* CertTag_Version */
                up.version = ros_ld32(obj);
        }
        s->r[0] = up.answer;
        return ROS_VECTOR_CLAIM;
    }
    if (s->r[0] == 27)
        up.ends++;
    return ROS_VECTOR_PASS;
}

/* A handle, connected to the server */
static uint32_t open_session(const char *host, uint32_t prompt, uint8_t *mem)
{
    uint32_t r[6] = { 2, 1, 0 };                   /* AF_INET, SOCK_STREAM */
    if (call(CREAT, r))
        return 0;
    uint32_t h = r[0];
    if (host) {
        strcpy((char *)mem, host);
        uint32_t o[6] = { h, 0xFFFF, SO_HOSTNAME, ros_addr(mem), 4 };
        call(SETSOCKOPT, o);
    }
    ros_st32(ros_addr(mem + 64), prompt);
    uint32_t p[6] = { h, 0xFFFF, SO_PROMPTTIME, ros_addr(mem + 64), 4 };
    call(SETSOCKOPT, p);
    uint8_t *sa = mem + 128;                        /* 4.3's sockaddr_in: 127.0.0.1 */
    memset(sa, 0, 16);
    sa[0] = 2, sa[2] = (uint8_t)(srv.port >> 8), sa[3] = (uint8_t)srv.port;
    sa[4] = 127, sa[7] = 1;
    uint32_t c[6] = { h, ros_addr(sa), 16 };
    if (call(CONNECT, c))
        return 0;
    return h;
}

void ros_selftest_acornssl(void)
{
    uint32_t r[6] = { 0 };
    check(!call(VERSION, r) && r[0] == 109, "AcornSSL: Version -- 1.09's interface", "%u", r[0]);
    r[0] = 0x12345678;
    uint32_t e = call(CLOSE, r);
    check(e == 0x813F21, "AcornSSL: a handle it did not give -- &813F21, Bad context handle", "&%X", e);

    /* The test's server writes to sockets the client may have closed */
    struct sigaction ignore = { .sa_handler = SIG_IGN }, before;
    sigaction(SIGPIPE, &ignore, &before);
    srv.ctx = server_ctx();
    srv.s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t ml = sizeof me;
    srv.connections = 3;
    pthread_t th;
    if (!srv.ctx || srv.s < 0 || bind(srv.s, (struct sockaddr *)&me, sizeof me) < 0 || listen(srv.s, 4) < 0 ||
        getsockname(srv.s, (struct sockaddr *)&me, &ml) < 0 || pthread_create(&th, NULL, server, NULL)) {
        check(0, "AcornSSL: a TLS server on loopback", "none");
        sigaction(SIGPIPE, &before, NULL);
        return;
    }
    srv.port = ntohs(me.sin_port);
    ros_vector_claim_native(ROS_UPCALLV, upcall, 0);
    uint8_t *mem = ros_rma_alloc(512);

    /* 1: blocking, the chain offered and accepted; TLS 1.3 */
    up.answer = 1;                                  /* CONFIRM_ACCEPT */
    uint32_t h = open_session("rosgd.test", 3000, mem);
    strcpy((char *)mem + 256, "hello");
    uint32_t w[6] = { h, ros_addr(mem + 256), 5 };
    uint32_t we = call(WRITE, w);
    uint32_t rd[6] = { h, ros_addr(mem + 300), 16 };
    uint32_t re = call(READ, rd);
    int ok = h && !we && w[0] == 5 && !re && rd[0] == 5 && !memcmp(mem + 300, "world", 5) && up.calls == 1 &&
             up.ends == 1 && up.flags == 0x08 && !strcmp(up.subject, "O=ROSGD, CN=rosgd.test") &&
             !strcmp(up.serial, "26:05") && up.version == 3;
    check(ok, "AcornSSL: a self-signed server -- UpCall 26 offers the chain (NOT_TRUSTED), accepted, data both ways",
          "h &%X write &%X %s read &%X n %u; upcalls %d/%d flags &%X \"%s\" \"%s\" v%u", h, we,
          we ? errmess(w[0]) : "", re, rd[0], up.calls, up.ends, up.flags, up.subject, up.serial, up.version);

    uint32_t g[6] = { h, 0xFFFF, SO_HOSTNAME, ros_addr(mem + 400), ros_addr(mem + 404) };
    ros_st32(ros_addr(mem + 404), 4);
    uint32_t ge = call(GETSOCKOPT, g);
    uint32_t pn[6] = { h, ros_addr(mem + 420), ros_addr(mem + 440) };
    ros_st32(ros_addr(mem + 440), 16);
    uint32_t pe = call(GETPEERNAME, pn);
    check(!ge && !strcmp(ros_ptr(ros_ld32(ros_addr(mem + 400))), "rosgd.test") && !pe && mem[424] == 127,
          "AcornSSL: SO_ACORNSSL_HOSTNAME read back; Getpeername through the Internet module", "&%X &%X", ge, pe);
    uint32_t c[6] = { h };
    check(!call(CLOSE, c), "AcornSSL: Close", " ");

    /* 2: PROMPTTIME 0: rejected at once, no UpCall */
    up.calls = up.ends = 0;
    h = open_session("rosgd.test", 0, mem);
    w[0] = h;
    we = call(WRITE, w);
    check(we == 0x813F27 && !strcmp(errmess(w[0]), "Handshake error (state 9984)") && up.calls == 0,
          "AcornSSL: SO_ACORNSSL_PROMPTTIME 0 -- a bad chain fails the handshake, unasked", "&%X \"%s\" %d", we,
          we ? errmess(w[0]) : "", up.calls);
    c[0] = h;
    call(CLOSE, c);

    /* 3: non-blocking, a name that does not match, MSG_PEEK */
    up.calls = up.ends = 0;
    h = open_session("wrong.test", 3000, mem);
    ros_st32(ros_addr(mem + 64), 1);
    uint32_t io[6] = { h, FIONBIO, ros_addr(mem + 64) };
    call(IOCTL, io);
    int waits = 0, bad = 0;
    for (int i = 0; i < 1000; i++) {                /* the handshake, driven by the writes */
        w[0] = h, w[1] = ros_addr(mem + 256), w[2] = 5;
        we = call(WRITE, w);
        if (!we)
            break;
        if (we == 0x20E39 || we == 0x20E23)         /* ENOTCONN, EWOULDBLOCK */
            waits++;
        else {
            bad = (int)we;
            break;
        }
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        ROS_BLOCKING(nanosleep(&ts, NULL));
    }
    uint32_t pk[6], n2 = 0;
    for (int i = 0; i < 1000 && !bad; i++) {
        pk[0] = h, pk[1] = ros_addr(mem + 300), pk[2] = 16, pk[3] = 2;     /* MSG_PEEK */
        re = call(RECV, pk);
        if (!re && pk[0] == 5)
            break;
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        ROS_BLOCKING(nanosleep(&ts, NULL));
    }
    rd[0] = h, rd[1] = ros_addr(mem + 350), rd[2] = 16;
    uint32_t re2 = call(READ, rd);
    n2 = rd[0];
    ok = !we && !bad && !re && pk[0] == 5 && !memcmp(mem + 300, "world", 5) && !re2 && n2 == 5 &&
         !memcmp(mem + 350, "world", 5) && up.calls >= 1 && (up.flags & 0x0C) == 0x0C;
    check(ok, "AcornSSL: non-blocking -- ENOTCONN/EWOULDBLOCK until ready; MSG_PEEK then Read; CN_MISMATCH flagged",
          "write &%X bad &%X waits %d peek &%X %u read &%X %u flags &%X", we, (unsigned)bad, waits, re, pk[0], re2,
          n2, up.flags);
    c[0] = h;
    call(CLOSE, c);

    ROS_BLOCKING(pthread_join(th, NULL));
    check(!strcmp(srv.version[0], "TLSv1.3") && !strcmp(srv.got[0], "hello") && !strcmp(srv.got[2], "hello"),
          "AcornSSL: TLS 1.3 on the wire; the server read what was written", "\"%s\" \"%s\" \"%s\"", srv.version[0],
          srv.got[0], srv.got[2]);
    ros_vector_release_native(ROS_UPCALLV, upcall, 0);
    ros_rma_free(mem);
    close(srv.s);
    SSL_CTX_free(srv.ctx);
    sigaction(SIGPIPE, &before, NULL);
}
