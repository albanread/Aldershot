/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "Licence").
 * You may not use this file except in compliance with the Licence.
 *
 * You can obtain a copy of the licence at
 * cddl/RiscOS/Sources/Networking/Fetchers/AcornSSL/LICENCE.
 * See the Licence for the specific language governing permissions
 * and limitations under the Licence.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the Licence file. If applicable, add the
 * following below this CDDL HEADER, with the fields enclosed by
 * brackets "[]" replaced with your own identifying information:
 * Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2018, RISC OS Open Ltd.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is a reimplementation of RISC OS Open's AcornSSL
 * (Sources/Networking/Fetchers/AcornSSL) over OpenSSL. The Licence file
 * beside it applies.
 */

/* acornssl.c -- AcornSSL, rewritten for ROSGD as a native module over
 * OpenSSL.
 *
 * RISC OS 5's AcornSSL (1.09, Networking/Fetchers/AcornSSL, doc/AcornSSL)
 * is a BSD-like socket interface to TLS. It has sixteen SWIs at &50F80 that
 * mirror the Internet module's, on an opaque "ssl handle" instead of a
 * socket number. It is mbedTLS 2.28 underneath, which stops at TLS 1.2.
 * Here the library is OpenSSL 3. It offers TLS 1.3, and 1.2 for servers that
 * have no better. TLS 1.0 and 1.1, which AcornSSL's own documentation marks
 * for withdrawal, are not offered.
 *
 * What the original's clients depend on, this keeps:
 *
 *   - The registers of every SWI, as sslmod.c's dispatcher reads them. Where
 *     doc/AcornSSL differs, the code wins. (Getsockopt's R4 is a pointer.)
 *   - A handle is a word the client cannot interpret. Here it is an RMA
 *     block's address, as the original's is its heap block's.
 *   - The socket underneath is the Internet module's. AcornSSL_Creat makes
 *     one with Socket_Creat. AcornSSL_CreateSession adopts the caller's, and
 *     then Close leaves it open. Connect, Shutdown, Ioctl, the options, the
 *     names and Stat go to the Internet module's SWIs on it, so their errors
 *     are that module's. The TLS runs on the Linux descriptor behind it
 *     (modules/internet: ros_internet_fd).
 *   - The handshake happens on the first Send or Recv, as the original's
 *     does. On a blocking socket it waits, with the personality lock let go
 *     meanwhile, as the Internet module's calls do. On a non-blocking
 *     socket an unfinished handshake is "Handshake error (state -1)"
 *     numbered as ENOTCONN, and a read or write that would wait is
 *     EWOULDBLOCK. FIONBIO through AcornSSL_Ioctl, and MSG_DONTWAIT and
 *     MSG_WAITALL, decide which, as there.
 *   - MSG_PEEK, through a buffer of peeked data, as the original's.
 *   - SO_ACORNSSL_HOSTNAME (&11E0) is the name the certificate must be
 *     issued to. It is sent as SNI. SO_ACORNSSL_PROMPTTIME (&11E1) is how
 *     long a flagged certificate chain may wait for the user. 0 rejects at
 *     once.
 *   - Certificates are checked against InetDBase:CertData (Mozilla's roots
 *     as curl publishes them). ROSGD's ROM carries a copy, used when there is
 *     no !Internet to set InetDBase$Path. A chain that fails is offered to
 *     UpCall_CertificateConfirm (26) in the original's layout, with its flags
 *     as mbedTLS's BADCERT bits. A claimant may accept it, once or for good.
 *     The claimant would be AcornSSL's desktop task, which ROSGD does not
 *     have yet. If it is accepted for good, the peer certificate's MD5 goes
 *     into Choices:WWW.AcornSSL.CertExcept, as there. Unclaimed, the chain
 *     is rejected. Exceptions already listed are accepted without asking.
 *   - Errors are &813F20 + n, "Handshake error (state %0)" and the rest, from
 *     the original's Messages. Those that it gives a Unix meaning are
 *     numbered &20E00 + errno, as the Internet module's are.
 *   - Service_URLModule_SSL is started (0) on a callback after the module
 *     starts, and dying (1) as it goes. It is started again when a protocol
 *     module (AcornHTTP) announces itself, as in the original, so that HTTP
 *     knows https is there.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "acornssl.h"
#include "internet.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"

#define VERSION        109          /* AcornSSL 1.09: the interface this is */
#define ERROR_BASE     0x813F20u    /* ErrorBase_HTTPS */
#define DCI4_BASE      0x20E00u     /* DCI4ERRORBLOCK */
#define R_EWOULDBLOCK  35
#define R_EINVAL       22
#define R_ENOTCONN     57
#define R_ECONNRESET   54
#define R_MSG_PEEK     0x2
#define R_MSG_WAITALL  0x40
#define R_MSG_DONTWAIT 0x80
#define R_SOL_SOCKET   0xFFFFu
#define R_FIONBIO      0x8004667Eu  /* _IOW('f', 126, int) */

#define SO_ACORNSSL_HOSTNAME   0x11E0u
#define SO_ACORNSSL_PROMPTTIME 0x11E1u
#define CREATESESSION_NEW       0
#define CREATESESSION_REUSEAUTH 1
#define PROMPT_DEFAULT          3000u   /* UPCALL_MAX_TIME, centiseconds */
#define MAX_DEPTH               10      /* MBEDTLS_X509_MAX_INTERMEDIATE_CA */

#define SERVICE_URLMODULE_PROTOCOL 0x83E01u  /* R0 0: a protocol module started, R3 its SWI chunk */
#define SERVICE_URLMODULE_SSL      0x83E02u  /* R0 0 started, 1 dying. R2 is the version */
#define HTTP_CHUNK          0x83F80u    /* HTTP_GetData & ~&3F */
#define UPCALL_CONFIRM      26u         /* UpCall_CertificateConfirm */
#define UPCALL_CONFIRM_END  27u

/* The original's messages (Resources.UK.Messages), by number */
enum { E_BAD_SESSION, E_BAD_CTX, E_NO_INIT, E_NO_VERIFY, E_NO_ROOTCA, E_MALLOC, E_SOCKET, E_HANDSHAKE,
       E_PARAMETER, E_NO_EXCEPTIONS };
static const char *const messages[] = {
    "Bad session handle", "Bad context handle", "Security libraries unable to initialise (reason %s)",
    "Unable to initialise certificate verification procedures (reason %s)",
    "Can't locate root certificate store", "Failed to allocate memory", "Socket error (code %s)",
    "Handshake error (state %s)", "Parameter error (code %s)", "Can't locate certificate exceptions",
};

/* mbedTLS's X.509 verify flags, which the UpCall's claimant reads */
enum { BADCERT_EXPIRED = 0x01, BADCERT_REVOKED = 0x02, BADCERT_CN_MISMATCH = 0x04, BADCERT_NOT_TRUSTED = 0x08,
       BADCERT_OTHER = 0x0100, BADCERT_FUTURE = 0x0200, BADCERT_KEY_USAGE = 0x0800,
       BADCERT_EXT_KEY_USAGE = 0x1000, BADCERT_BAD_MD = 0x4000, BADCERT_BAD_PK = 0x8000,
       BADCERT_BAD_KEY = 0x010000 };
#define X509_CERT_VERIFY_FAILED "9984"  /* -MBEDTLS_ERR_X509_CERT_VERIFY_FAILED */

enum { CONFIRM_UNNECESSARY = -2, CONFIRM_REJECT = -1, CONFIRM_PENDING = 0, CONFIRM_ACCEPT = 1,
       CONFIRM_ACCEPT_SAVE = 2 };

struct session {
    uint32_t handle;                /* its RMA block: the client's word */
    uint32_t socket;                /* the Internet module's socket number */
    int client_socket;              /* CreateSession's: Close leaves it open */
    int nbio;                       /* FIONBIO, as set through AcornSSL_Ioctl */
    SSL *ssl;
    char *hostname;                 /* SO_ACORNSSL_HOSTNAME's, and its RMA copy */
    uint32_t hostname_addr;
    uint32_t prompt_max;            /* SO_ACORNSSL_PROMPTTIME */
    int handshaken;
    int confirm;                    /* CONFIRM_* */
    uint32_t confirm_time;          /* when asking began, centiseconds */
    uint32_t confirm_iterations;
    uint32_t chain;                 /* the UpCall's chain, in the RMA */
    uint32_t depth_flags[MAX_DEPTH];/* verify flags, by depth, from the callback */
    int depths;
    uint8_t *peek;                  /* peeked data, [peek_from, peek_to) */
    size_t peek_from, peek_to;
    SSL_SESSION *saved;             /* for CreateSession_ReuseAuth */
    struct session *next;
};

static struct session *sessions;
static SSL_CTX *ctx;
static int ctx_tried;
static char *exceptions;            /* CertExcept's lines, or NULL */
static int announced;

/* ---- errors --------------------------------------------------------------- */

static os_error *error(int n, const char *extra)
{
    return ros_error(ERROR_BASE + (uint32_t)n, messages[n], extra ? extra : "");
}

static os_error *error_errno(int n, const char *extra, int risc_os_errno)
{
    os_error *e = error(n, extra);
    e->errnum = DCI4_BASE + (uint32_t)(risc_os_errno & 0x7F);
    return e;
}

static const char *integer(long v)
{
    static char text[24];
    snprintf(text, sizeof text, "%ld", v);
    return text;
}

/* ---- the Internet module's SWIs ---------------------------------------------- */

static os_error *socket_swi(uint32_t swi, uint32_t r[6])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    for (int i = 0; i < 6; i++)
        s.r[i] = r[i];
    ros_swi(&s, ROS_X_BIT | swi);
    for (int i = 0; i < 6; i++)
        r[i] = s.r[i];
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

enum { SOCKET_CREAT = 0x41200, SOCKET_CONNECT = 0x41204, SOCKET_SHUTDOWN = 0x4120B, SOCKET_SETSOCKOPT = 0x4120C,
       SOCKET_GETSOCKOPT = 0x4120D, SOCKET_GETPEERNAME = 0x4120E, SOCKET_GETSOCKNAME = 0x4120F,
       SOCKET_CLOSE = 0x41210, SOCKET_IOCTL = 0x41212, SOCKET_STAT = 0x41215 };

/* ---- the library, the roots and the exceptions ------------------------------------- */

static char *load_file(const char *name, size_t *size)
{
    size_t nl = strlen(name) + 1;
    char *b = ros_rma_alloc(nl);
    if (!b)
        return NULL;
    memcpy(b, name, nl);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 17, s.r[1] = ros_addr(b);              /* OS_File 17: read the catalogue */
    ros_swi(&s, ROS_X_BIT | 0x08);
    if (s.v || s.r[0] != 1) {
        ros_rma_free(b);
        return NULL;
    }
    uint32_t len = s.r[4];
    uint8_t *buf = ros_rma_alloc(len + 1);
    char *copy = buf ? malloc(len + 1) : NULL;
    if (copy) {
        ros_cpu_enter(&s);
        s.r[0] = 16, s.r[1] = ros_addr(b), s.r[2] = ros_addr(buf), s.r[3] = 0;   /* OS_File 16: load */
        ros_swi(&s, ROS_X_BIT | 0x08);
        if (s.v) {
            free(copy);
            copy = NULL;
        } else {
            memcpy(copy, buf, len);
            copy[len] = 0;
            *size = len;
        }
    }
    if (buf)
        ros_rma_free(buf);
    ros_rma_free(b);
    return copy;
}

static os_error *library(void)
{
    if (ctx)
        return NULL;
    if (ctx_tried)
        return error(E_NO_ROOTCA, NULL);
    ctx_tried = 1;
    size_t size = 0;
    char *pem = load_file("InetDBase:CertData", &size);
    if (!pem)
        pem = load_file("Resources:$.Resources.URL.AcornSSL.CertData", &size);
    if (!pem)
        return error(E_NO_ROOTCA, NULL);
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (!c) {
        free(pem);
        return error(E_NO_INIT, integer((long)ERR_get_error()));
    }
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    X509_STORE *store = SSL_CTX_get_cert_store(c);
    BIO *bio = BIO_new_mem_buf(pem, (int)size);
    int roots = 0;
    for (X509 *x; (x = PEM_read_bio_X509(bio, NULL, NULL, NULL)); X509_free(x))
        roots += X509_STORE_add_cert(store, x) == 1;
    BIO_free(bio);
    free(pem);
    ERR_clear_error();                              /* the end of the PEM */
    if (!roots) {
        SSL_CTX_free(c);
        return error(E_NO_VERIFY, "0");
    }
    ctx = c;
    return NULL;
}

static void read_exceptions(void)
{
    if (exceptions)
        return;
    size_t size;
    exceptions = load_file("Choices:WWW.AcornSSL.CertExcept", &size);
}

static void md5_hex(const uint8_t *der, size_t n, char out[33])
{
    uint8_t hash[16];
    unsigned len = 0;
    EVP_Digest(der, n, hash, &len, EVP_md5(), NULL);
    for (int i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", hash[i]);
}

static int is_exception(const uint8_t *der, size_t n)
{
    if (!exceptions)
        return 0;
    char match[33];
    md5_hex(der, n, match);
    for (const char *line = exceptions; line && *line;) {
        if (!strncmp(line, match, 32))              /* a short last line is not a match */
            return 1;
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return 0;
}

static void add_exception(const uint8_t *der, size_t n)
{
    if (is_exception(der, n))
        return;
    char match[33];
    md5_hex(der, n, match);
    size_t old = exceptions ? strlen(exceptions) : 0;
    char *grown = realloc(exceptions, old + 34);
    if (!grown)
        return;
    exceptions = grown;
    memcpy(exceptions + old, match, 32);
    exceptions[old + 32] = '\n', exceptions[old + 33] = 0;
    /* <Choices$Write>.WWW.AcornSSL.CertExcept, a Text file, as the original */
    static const char *const dirs[] = { "<Choices$Write>.WWW", "<Choices$Write>.WWW.AcornSSL" };
    char *b = ros_rma_alloc(64 + old + 34);
    if (!b)
        return;
    struct ros_cpu s;
    for (int i = 0; i < 2; i++) {
        strcpy(b, dirs[i]);
        ros_cpu_enter(&s);
        s.r[0] = 8, s.r[1] = ros_addr(b), s.r[4] = 0;          /* OS_File 8: create a directory */
        ros_swi(&s, ROS_X_BIT | 0x08);
    }
    strcpy(b, "<Choices$Write>.WWW.AcornSSL.CertExcept");
    char *data = b + 64;
    memcpy(data, exceptions, old + 33);
    ros_cpu_enter(&s);
    s.r[0] = 10, s.r[1] = ros_addr(b), s.r[2] = 0xFFF;          /* OS_File 10: save, stamped, Text */
    s.r[4] = ros_addr(data), s.r[5] = ros_addr(data + old + 33);
    ros_swi(&s, ROS_X_BIT | 0x08);
    ros_rma_free(b);
}

/* ---- sessions ----------------------------------------------------------------- */

static struct session *find(uint32_t handle)
{
    for (struct session *h = sessions; h; h = h->next)
        if (h->handle == handle)
            return h;
    return NULL;
}

/* OpenSSL's verify callback, in the handshake. It records, by depth, what
 * was wrong, as mbedTLS's flags. It always goes on, as
 * MBEDTLS_SSL_VERIFY_OPTIONAL does, and the chain is judged when the
 * handshake is over. Nothing of the runtime is touched here, so the lock
 * may be let go. */
static int ex_index = -1;

static uint32_t badcert(int err)
{
    switch (err) {
    case X509_V_OK: return 0;
    case X509_V_ERR_CERT_HAS_EXPIRED: return BADCERT_EXPIRED;
    case X509_V_ERR_CERT_NOT_YET_VALID: return BADCERT_FUTURE;
    case X509_V_ERR_CERT_REVOKED: return BADCERT_REVOKED;
    case X509_V_ERR_HOSTNAME_MISMATCH:
    case X509_V_ERR_IP_ADDRESS_MISMATCH: return BADCERT_CN_MISMATCH;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_CERT_UNTRUSTED:
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:
    case X509_V_ERR_INVALID_CA: return BADCERT_NOT_TRUSTED;
    case X509_V_ERR_INVALID_PURPOSE: return BADCERT_EXT_KEY_USAGE;
    case X509_V_ERR_KEYUSAGE_NO_CERTSIGN:
    case X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE: return BADCERT_KEY_USAGE;
    case X509_V_ERR_CA_MD_TOO_WEAK: return BADCERT_BAD_MD;
    case X509_V_ERR_CA_KEY_TOO_SMALL:
    case X509_V_ERR_EE_KEY_TOO_SMALL: return BADCERT_BAD_KEY;
    default: return BADCERT_OTHER;
    }
}

static int verify(int ok, X509_STORE_CTX *store)
{
    SSL *ssl = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());
    struct session *h = ssl ? SSL_get_ex_data(ssl, ex_index) : NULL;
    int depth = X509_STORE_CTX_get_error_depth(store);
    if (h && depth >= 0 && depth < MAX_DEPTH) {
        if (!ok)
            h->depth_flags[depth] |= badcert(X509_STORE_CTX_get_error(store));
        if (depth + 1 > h->depths)
            h->depths = depth + 1;
    }
    return 1;
}

static os_error *new_session(uint32_t sock, int client_socket, struct session **out)
{
    os_error *e = library();
    if (e)
        return e;
    read_exceptions();
    struct session *h = calloc(1, sizeof *h);
    uint8_t *block = h ? ros_rma_alloc(16) : NULL;
    if (!block) {
        free(h);
        return error(E_MALLOC, NULL);
    }
    memset(block, 0, 16);
    h->handle = ros_addr(block);
    h->socket = sock;
    h->client_socket = client_socket;
    h->prompt_max = PROMPT_DEFAULT;
    h->confirm = CONFIRM_UNNECESSARY;
    h->ssl = SSL_new(ctx);
    if (!h->ssl) {
        ros_rma_free(block);
        free(h);
        return error(E_NO_INIT, integer((long)ERR_get_error()));
    }
    SSL_set_ex_data(h->ssl, ex_index, h);
    SSL_set_verify(h->ssl, SSL_VERIFY_PEER, verify);
    SSL_set_connect_state(h->ssl);
    int fd = ros_internet_fd(sock);
    h->nbio = fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
    h->next = sessions;
    sessions = h;
    *out = h;
    return NULL;
}

static void free_chain(struct session *h);

static void free_session(struct session *h)
{
    for (struct session **p = &sessions; *p; p = &(*p)->next)
        if (*p == h) {
            *p = h->next;
            break;
        }
    free_chain(h);
    SSL_free(h->ssl);
    if (h->saved)
        SSL_SESSION_free(h->saved);
    free(h->hostname);
    if (h->hostname_addr)
        ros_rma_free(ros_ptr(h->hostname_addr));
    free(h->peek);
    ros_rma_free(ros_ptr(h->handle));
    free(h);
}

/* A socket BIO that sends with MSG_NOSIGNAL. OpenSSL's own socket BIO writes
 * with write(), and a peer that had gone would then raise SIGPIPE instead of
 * giving EPIPE. (On macOS the Internet module sets SO_NOSIGPIPE on its
 * sockets instead.) */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int bio_fd(BIO *b)
{
    return (int)(intptr_t)BIO_get_data(b) - 1;
}

static int bio_write(BIO *b, const char *p, int n)
{
    ssize_t r = send(bio_fd(b), p, (size_t)n, MSG_NOSIGNAL);
    BIO_clear_retry_flags(b);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        BIO_set_retry_write(b);
    return (int)r;
}

static int bio_read(BIO *b, char *p, int n)
{
    ssize_t r = recv(bio_fd(b), p, (size_t)n, 0);
    BIO_clear_retry_flags(b);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        BIO_set_retry_read(b);
    return (int)r;
}

static long bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    switch (cmd) {
    case BIO_C_SET_FD:                              /* the descriptor, never closed here */
        BIO_set_data(b, (void *)(intptr_t)(*(int *)ptr + 1));
        BIO_set_init(b, 1);
        return 1;
    case BIO_C_GET_FD:
        if (ptr)
            *(int *)ptr = bio_fd(b);
        return bio_fd(b);
    case BIO_CTRL_FLUSH:
        return 1;
    default:
        (void)num;
        return 0;
    }
}

static BIO_METHOD *nosignal_method(void)
{
    static BIO_METHOD *m;
    if (!m) {
        m = BIO_meth_new(BIO_TYPE_SOCKET, "socket, MSG_NOSIGNAL");
        BIO_meth_set_write(m, bio_write);
        BIO_meth_set_read(m, bio_read);
        BIO_meth_set_ctrl(m, bio_ctrl);
    }
    return m;
}

static void set_nonblocking(int fd, int nb)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, nb ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
}

/* ---- the certificate chain, for UpCall_CertificateConfirm ---------------------- */

/* One certificate's entry: next, format, flags, der, dersize, then eight tags
 * of { type, pad, tag, object }. This is AcornSSL's certchain_t, 32-bit */
#define CHAIN_ENTRY (20 + 8 * 8)
enum { TAG_END, TAG_ISSUER, TAG_SUBJECT, TAG_VALIDFROM, TAG_VALIDTO, TAG_SERIAL, TAG_VERSION, TAG_SIGNATURE };
enum { TYPE_STRING, TYPE_INTEGER, TYPE_CARDINAL, TYPE_UNIXTIME };

static void free_chain(struct session *h)
{
    if (h->chain)
        ros_rma_free(ros_ptr(h->chain));
    h->chain = 0;
}

/* "C=GB, O=Org, CN=name", as mbedtls_x509_dn_gets writes it */
static size_t dn(X509_NAME *name, char *out, size_t max)
{
    BIO *b = BIO_new(BIO_s_mem());
    X509_NAME_print_ex(b, name, 0, XN_FLAG_SEP_CPLUS_SPC | ASN1_STRFLGS_UTF8_CONVERT);
    char *p;
    long n = BIO_get_mem_data(b, &p);
    size_t len = (size_t)n < max - 1 ? (size_t)n : max - 1;
    memcpy(out, p, len);
    out[len] = 0;
    BIO_free(b);
    return len;
}

/* "04:A3:...", as mbedtls_x509_serial_gets */
static size_t serial(X509 *x, char *out, size_t max)
{
    const ASN1_INTEGER *s = X509_get0_serialNumber(x);
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < s->length && n + 4 < max; i++)
        n += (size_t)snprintf(out + n, max - n, i ? ":%02X" : "%02X", s->data[i]);
    return n;
}

/* mbedtls_oid_get_sig_alg_desc's names, for the usual ones */
static const char *signature(X509 *x)
{
    switch (X509_get_signature_nid(x)) {
    case NID_sha1WithRSAEncryption: return "RSA with SHA1";
    case NID_sha224WithRSAEncryption: return "RSA with SHA-224";
    case NID_sha256WithRSAEncryption: return "RSA with SHA-256";
    case NID_sha384WithRSAEncryption: return "RSA with SHA-384";
    case NID_sha512WithRSAEncryption: return "RSA with SHA-512";
    case NID_ecdsa_with_SHA1: return "ECDSA with SHA1";
    case NID_ecdsa_with_SHA224: return "ECDSA with SHA224";
    case NID_ecdsa_with_SHA256: return "ECDSA with SHA256";
    case NID_ecdsa_with_SHA384: return "ECDSA with SHA384";
    case NID_ecdsa_with_SHA512: return "ECDSA with SHA512";
    case NID_rsassaPss: return "RSASSA-PSS";
    default: return OBJ_nid2ln(X509_get_signature_nid(x));
    }
}

static uint32_t unix_time(const ASN1_TIME *t)
{
    struct tm tm;
    if (!t || ASN1_TIME_to_tm(t, &tm) != 1)
        return 0;
    return (uint32_t)timegm(&tm);
}

/* The peer's chain, leaf first, as the original builds it for the UpCall */
static int build_chain(struct session *h, STACK_OF(X509) *certs)
{
    int n = sk_X509_num(certs);
    if (n > MAX_DEPTH)
        n = MAX_DEPTH;
    size_t space = (size_t)n * CHAIN_ENTRY;
    for (int i = 0; i < n; i++)
        space += (size_t)i2d_X509(sk_X509_value(certs, i), NULL) + 3 * 1024;
    uint8_t *base = ros_rma_alloc(space);
    if (!base)
        return -1;
    memset(base, 0, space);
    uint8_t *heap = base + (size_t)n * CHAIN_ENTRY;
    for (int i = 0; i < n; i++) {
        X509 *x = sk_X509_value(certs, i);
        uint8_t *e = base + (size_t)i * CHAIN_ENTRY;
        uint32_t *w = (uint32_t *)e;
        w[0] = i + 1 < n ? ros_addr(e + CHAIN_ENTRY) : 0;
        w[1] = 0;                                   /* UPCALL_FORMAT */
        w[2] = h->depth_flags[i];
        unsigned char *der = heap;
        int dl = i2d_X509(x, &der);
        w[3] = ros_addr(heap), w[4] = (uint32_t)dl;
        heap += (dl + 3) & ~3;
        uint8_t *tags = e + 20;
        int t = 0;
#define TAG(tagno, typ, len)                                                         \
    do {                                                                             \
        tags[8 * t] = (typ), tags[8 * t + 1] = 0;                                    \
        tags[8 * t + 2] = (uint8_t)(tagno), tags[8 * t + 3] = 0;                     \
        *(uint32_t *)(tags + 8 * t + 4) = ros_addr(heap);                            \
        heap += ((len) + 3) & ~3u, t++;                                              \
    } while (0)
        size_t l = dn(X509_get_subject_name(x), (char *)heap, 1024);
        TAG(TAG_SUBJECT, TYPE_STRING, l + 1);
        l = dn(X509_get_issuer_name(x), (char *)heap, 1024);
        TAG(TAG_ISSUER, TYPE_STRING, l + 1);
        *(uint32_t *)heap = (uint32_t)X509_get_version(x) + 1;
        TAG(TAG_VERSION, TYPE_INTEGER, 4u);
        l = serial(x, (char *)heap, 256);
        TAG(TAG_SERIAL, TYPE_STRING, l + 1);
        l = strlen(strcpy((char *)heap, signature(x)));
        TAG(TAG_SIGNATURE, TYPE_STRING, l + 1);
        *(uint32_t *)heap = unix_time(X509_get0_notBefore(x));
        TAG(TAG_VALIDFROM, TYPE_UNIXTIME, 4u);
        *(uint32_t *)heap = unix_time(X509_get0_notAfter(x));
        TAG(TAG_VALIDTO, TYPE_UNIXTIME, 4u);
#undef TAG
        tags[8 * t + 2] = TAG_END;
    }
    h->chain = ros_addr(base);
    return 0;
}

static uint32_t monotonic(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, ROS_X_BIT | 0x42);                  /* OS_ReadMonotonicTime */
    return s.r[0];
}

static int upcall(uint32_t reason, uint32_t r1, uint32_t r2)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = reason, s.r[1] = r1, s.r[2] = r2;
    ros_swi(&s, ROS_X_BIT | 0x33);                  /* OS_UpCall */
    return (int)s.r[0];
}

static void ask(struct session *h)
{
    int claimed = upcall(UPCALL_CONFIRM, h->chain, h->confirm_iterations);
    h->confirm = claimed == CONFIRM_PENDING || claimed == CONFIRM_ACCEPT || claimed == CONFIRM_ACCEPT_SAVE
                     ? claimed
                     : CONFIRM_REJECT;              /* rejected, or nobody asked */
    if (h->confirm != CONFIRM_PENDING) {
        upcall(UPCALL_CONFIRM_END, h->chain, (uint32_t)h->confirm);
        free_chain(h);
    }
}

/* The chain judged, once the handshake is over: NULL to go on */
static os_error *judge(struct session *h)
{
    if (h->confirm == CONFIRM_UNNECESSARY) {
        uint32_t flags = 0;
        for (int i = 0; i < h->depths; i++)
            flags |= h->depth_flags[i];
        X509 *leaf = SSL_get0_peer_certificate(h->ssl);
        uint8_t *der = NULL;
        int dl = leaf ? i2d_X509(leaf, &der) : 0;
        if (!leaf)
            flags |= 0x40;                          /* BADCERT_MISSING */
        if (dl > 0 && is_exception(der, (size_t)dl))
            flags = 0;
        OPENSSL_free(der);
        if (!flags)
            return NULL;
        if (h->prompt_max == 0) {
            h->confirm = CONFIRM_REJECT;
            return error(E_HANDSHAKE, X509_CERT_VERIFY_FAILED);
        }
        STACK_OF(X509) *chain = SSL_get_peer_cert_chain(h->ssl);
        if (!chain || build_chain(h, chain) < 0) {
            h->confirm = CONFIRM_REJECT;
            return error(E_MALLOC, NULL);
        }
        h->confirm_time = monotonic();
        h->confirm_iterations = 0;
        ask(h);
    }
    for (;;) {
        switch (h->confirm) {
        case CONFIRM_ACCEPT:
            return NULL;
        case CONFIRM_ACCEPT_SAVE: {
            X509 *leaf = SSL_get0_peer_certificate(h->ssl);
            uint8_t *der = NULL;
            int dl = leaf ? i2d_X509(leaf, &der) : 0;
            if (dl > 0)
                add_exception(der, (size_t)dl);
            OPENSSL_free(der);
            h->confirm = CONFIRM_ACCEPT;
            return NULL;
        }
        case CONFIRM_PENDING:
            if (monotonic() - h->confirm_time >= h->prompt_max) {
                h->confirm = CONFIRM_REJECT;
                upcall(UPCALL_CONFIRM_END, h->chain, (uint32_t)CONFIRM_REJECT);
                free_chain(h);
                break;
            }
            if (h->nbio)
                return error_errno(E_HANDSHAKE, "-1", R_ENOTCONN);
            {
                struct timespec ts = { 0, 10 * 1000 * 1000 };
                ROS_BLOCKING(nanosleep(&ts, NULL));
            }
            h->confirm_iterations++;
            ask(h);
            continue;
        default:
            break;
        }
        return error(E_HANDSHAKE, X509_CERT_VERIFY_FAILED);
    }
}

/* ---- the handshake, reading and writing ---------------------------------------- */

/* Whether the socket is connected. This asks for its peer's name, through
 * the Internet module. If the socket is not connected, that module's error
 * (ENOTCONN) is the answer */
static os_error *connected(struct session *h)
{
    uint8_t *b = ros_rma_alloc(64);
    if (!b)
        return error(E_MALLOC, NULL);
    uint32_t *len = (uint32_t *)(b + 48);
    *len = 44;
    uint32_t r[6] = { h->socket, ros_addr(b), ros_addr(len) };
    os_error *e = socket_swi(SOCKET_GETPEERNAME, r);
    ros_rma_free(b);
    return e;
}

static os_error *handshake(struct session *h)
{
    if (h->handshaken)
        return judge(h);
    if (h->confirm == CONFIRM_REJECT)
        return error(E_HANDSHAKE, X509_CERT_VERIFY_FAILED);
    int fd = ros_internet_fd(h->socket);
    if (fd < 0)
        return error_errno(E_SOCKET, "9", 9);       /* EBADF */
    os_error *e = connected(h);
    if (e)
        return e;
    if (SSL_get_fd(h->ssl) != fd) {
        BIO *bio = BIO_new(nosignal_method());
        if (!bio)
            return error(E_MALLOC, NULL);
        BIO_set_fd(bio, fd, BIO_NOCLOSE);
        SSL_set_bio(h->ssl, bio, bio);
        if (h->hostname) {
            SSL_set_tlsext_host_name(h->ssl, h->hostname);
            SSL_set1_host(h->ssl, h->hostname);
        }
    }
    int r, err;
    set_nonblocking(fd, h->nbio);
    ROS_BLOCKING(r = SSL_do_handshake(h->ssl));
    ros_internet_consumed(h->socket);
    if (r == 1) {
        h->handshaken = 1;
        return judge(h);
    }
    err = SSL_get_error(h->ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return error_errno(E_HANDSHAKE, "-1", R_ENOTCONN);
    unsigned long lib = ERR_peek_last_error();
    ERR_clear_error();
    if (err == SSL_ERROR_SYSCALL || err == SSL_ERROR_ZERO_RETURN)
        return error_errno(E_HANDSHAKE, integer(err), R_ECONNRESET);
    return error(E_HANDSHAKE, integer((long)ERR_GET_REASON(lib)));
}

/* Whether this call is non-blocking. If exactly one of MSG_DONTWAIT and
 * MSG_WAITALL is set it decides, as in the original. Otherwise FIONBIO
 * decides */
static int nonblocking(struct session *h, uint32_t flags)
{
    int dontwait = (flags & R_MSG_DONTWAIT) != 0, waitall = (flags & R_MSG_WAITALL) != 0;
    return dontwait != waitall ? dontwait : h->nbio;
}

/* SSL_read or SSL_write of n bytes. It returns an error, or sets *moved to the count */
static os_error *transfer(struct session *h, int writing, uint8_t *buf, uint32_t len, int nb, uint32_t *moved)
{
    int fd = ros_internet_fd(h->socket);
    set_nonblocking(fd, nb);
    int r;
    size_t done = 0;
    if (writing)
        ROS_BLOCKING(r = SSL_write_ex(h->ssl, buf, len, &done));
    else
        ROS_BLOCKING(r = SSL_read_ex(h->ssl, buf, len, &done));
    int err = r == 1 ? SSL_ERROR_NONE : SSL_get_error(h->ssl, r);
    if (!writing)
        ros_internet_consumed(h->socket);
    set_nonblocking(fd, h->nbio);
    ERR_clear_error();
    switch (err) {
    case SSL_ERROR_NONE:
        *moved = (uint32_t)done;
        return NULL;
    case SSL_ERROR_ZERO_RETURN:                     /* close_notify: the end */
        *moved = 0;
        return NULL;
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        if (nb)
            return error_errno(E_SOCKET, integer(err == SSL_ERROR_WANT_READ ? 26880 : 26752), R_EWOULDBLOCK);
        /* fall through: a blocking descriptor does not ask again */
    default:
        if (err == SSL_ERROR_SYSCALL && !writing && errno == 0) {
            *moved = 0;                             /* the peer went without close_notify */
            return NULL;
        }
        return error(E_SOCKET, integer(err));
    }
}

static os_error *do_send(struct ros_cpu *s, uint32_t flags)
{
    struct session *h = find(s->r[0]);
    if (!h)
        return error(E_BAD_CTX, NULL);
    os_error *e = handshake(h);
    if (e)
        return e;
    uint32_t moved = 0;
    e = transfer(h, 1, ros_ptr(s->r[1]), s->r[2], nonblocking(h, flags), &moved);
    if (!e)
        s->r[0] = moved;
    return e;
}

static os_error *do_recv(struct ros_cpu *s, uint32_t flags)
{
    struct session *h = find(s->r[0]);
    if (!h)
        return error(E_BAD_CTX, NULL);
    os_error *e = handshake(h);
    if (e)
        return e;
    uint8_t *buf = ros_ptr(s->r[1]);
    uint32_t len = s->r[2], moved = 0;
    int nb = nonblocking(h, flags);
    size_t available = h->peek_to - h->peek_from;
    if (flags & R_MSG_PEEK) {
        if (len > available) {                      /* peek more: read into the buffer */
            uint8_t *grown = malloc(len);
            if (!grown)
                return error(E_MALLOC, NULL);
            if (available)
                memcpy(grown, h->peek + h->peek_from, available);
            free(h->peek);
            h->peek = grown, h->peek_from = 0, h->peek_to = available;
            e = transfer(h, 0, grown + available, len - (uint32_t)available, nb, &moved);
            if (e && !available)
                return e;
            if (!e)
                h->peek_to += moved, available += moved;
        }
        uint32_t n = len < available ? len : (uint32_t)available;
        memcpy(buf, h->peek + h->peek_from, n);
        s->r[0] = n;
        return NULL;
    }
    uint32_t copy = 0;
    if (available) {                                /* peeked data first */
        copy = len < available ? len : (uint32_t)available;
        memcpy(buf, h->peek + h->peek_from, copy);
        h->peek_from += copy;
        if (h->peek_from == h->peek_to) {
            free(h->peek);
            h->peek = NULL, h->peek_from = h->peek_to = 0;
        }
        if (copy == len) {
            s->r[0] = copy;
            return NULL;
        }
    }
    e = transfer(h, 0, buf + copy, len - copy, nb, &moved);
    if (e && copy)
        e = NULL, moved = 0;                        /* what was peeked, at least */
    if (!e)
        s->r[0] = copy + moved;
    return e;
}

/* ---- the SWIs ------------------------------------------------------------------ */

#define DONE(e)                  \
    do {                         \
        os_error *e_ = (e);      \
        if (e_)                  \
            ros_swi_fail(s, e_); \
        else                     \
            s->v = 0;            \
        return;                  \
    } while (0)

void ros_thunk_AcornSSL_Creat(struct ros_cpu *s)
{
    if (s->r[0] >> 8) {                             /* flags reserved */
        DONE(error_errno(E_PARAMETER, integer(R_EINVAL), R_EINVAL));
    }
    uint32_t r[6] = { s->r[0], s->r[1], s->r[2] };
    os_error *e = socket_swi(SOCKET_CREAT, r);
    if (e)
        DONE(e);
    struct session *h;
    e = new_session(r[0], 0, &h);
    if (e) {
        uint32_t c[6] = { r[0] };
        socket_swi(SOCKET_CLOSE, c);
        DONE(e);
    }
    s->r[0] = h->handle;
    DONE(NULL);
}

void ros_thunk_AcornSSL_CreateSession(struct ros_cpu *s)
{
    uint32_t cmd = s->r[1] & 0xFF;
    if ((cmd != CREATESESSION_NEW && cmd != CREATESESSION_REUSEAUTH) || (s->r[1] >> 8))
        DONE(error_errno(E_PARAMETER, integer(R_EINVAL), R_EINVAL));
    struct session *src = NULL;
    if (cmd == CREATESESSION_REUSEAUTH && !(src = find(s->r[2])))
        DONE(error(E_BAD_CTX, NULL));
    struct session *h;
    os_error *e = new_session(s->r[0], 1, &h);
    if (e)
        DONE(e);
    if (src) {
        h->confirm = CONFIRM_ACCEPT;                /* already accepted */
        if (!src->saved)
            src->saved = SSL_get1_session(src->ssl);
        if (src->saved && !SSL_set_session(h->ssl, src->saved)) {
            free_session(h);
            DONE(error(E_NO_INIT, integer((long)ERR_get_error())));
        }
    }
    s->r[0] = h->handle;
    DONE(NULL);
}

void ros_thunk_AcornSSL_Close(struct ros_cpu *s)
{
    struct session *h = find(s->r[0]);
    if (!h)
        DONE(error(E_BAD_CTX, NULL));
    if (h->handshaken) {                            /* close_notify, without waiting for one back */
        int fd = ros_internet_fd(h->socket);
        if (fd >= 0) {
            set_nonblocking(fd, 1);
            SSL_shutdown(h->ssl);
            set_nonblocking(fd, h->nbio);
        }
        ERR_clear_error();
    }
    os_error *e = NULL;
    if (!h->client_socket) {
        uint32_t r[6] = { h->socket };
        e = socket_swi(SOCKET_CLOSE, r);
    }
    if (!e)
        free_session(h);
    s->r[0] = 0;
    DONE(e);
}

/* The calls the socket answers. These go to the Internet module's SWIs on the
 * socket number, with the handle's registers otherwise unchanged */
static void forward(struct ros_cpu *s, uint32_t swi)
{
    struct session *h = find(s->r[0]);
    if (!h)
        DONE(error(E_BAD_CTX, NULL));
    uint32_t r[6] = { h->socket, s->r[1], s->r[2], s->r[3], s->r[4], s->r[5] };
    os_error *e = socket_swi(swi, r);
    if (!e)
        s->r[0] = r[0];
    DONE(e);
}

void ros_thunk_AcornSSL_Connect(struct ros_cpu *s)
{
    forward(s, SOCKET_CONNECT);
}

void ros_thunk_AcornSSL_Shutdown(struct ros_cpu *s)
{
    forward(s, SOCKET_SHUTDOWN);
}

void ros_thunk_AcornSSL_Getpeername(struct ros_cpu *s)
{
    forward(s, SOCKET_GETPEERNAME);
}

void ros_thunk_AcornSSL_Getsockname(struct ros_cpu *s)
{
    forward(s, SOCKET_GETSOCKNAME);
}

void ros_thunk_AcornSSL_Stat(struct ros_cpu *s)
{
    forward(s, SOCKET_STAT);
}

void ros_thunk_AcornSSL_Ioctl(struct ros_cpu *s)
{
    struct session *h = find(s->r[0]);
    if (!h)
        DONE(error(E_BAD_CTX, NULL));
    if (s->r[1] == R_FIONBIO)
        h->nbio = ros_ld32(s->r[2]) != 0;
    forward(s, SOCKET_IOCTL);
}

void ros_thunk_AcornSSL_Getsockopt(struct ros_cpu *s)
{
    struct session *h = find(s->r[0]);
    if (!h)
        DONE(error(E_BAD_CTX, NULL));
    if (s->r[1] == R_SOL_SOCKET && (s->r[2] == SO_ACORNSSL_HOSTNAME || s->r[2] == SO_ACORNSSL_PROMPTTIME)) {
        if (ros_ld32(s->r[4]) != 4)
            DONE(error(E_PARAMETER, integer(R_EINVAL)));
        ros_st32(s->r[3], s->r[2] == SO_ACORNSSL_HOSTNAME ? h->hostname_addr : h->prompt_max);
        s->r[0] = 0;
        DONE(NULL);
    }
    forward(s, SOCKET_GETSOCKOPT);
}

void ros_thunk_AcornSSL_Setsockopt(struct ros_cpu *s)
{
    struct session *h = find(s->r[0]);
    if (!h)
        DONE(error(E_BAD_CTX, NULL));
    if (s->r[1] == R_SOL_SOCKET && s->r[2] == SO_ACORNSSL_HOSTNAME) {
        if (s->r[4] != 4)
            DONE(error(E_PARAMETER, integer(R_EINVAL)));
        free(h->hostname);
        h->hostname = NULL;
        if (h->hostname_addr)
            ros_rma_free(ros_ptr(h->hostname_addr));
        h->hostname_addr = 0;
        if (s->r[3]) {                              /* R3 is the name itself */
            const char *name = ros_ptr(s->r[3]);
            size_t n = strnlen(name, 256);
            if (n == 0 || n >= 256)
                DONE(error_errno(E_PARAMETER, integer(R_EINVAL), R_EINVAL));
            h->hostname = strndup(name, n);
            char *copy = ros_rma_alloc(n + 1);
            if (!h->hostname || !copy)
                DONE(error(E_MALLOC, NULL));
            memcpy(copy, name, n + 1);
            h->hostname_addr = ros_addr(copy);
        }
        s->r[0] = 0;
        DONE(NULL);
    }
    if (s->r[1] == R_SOL_SOCKET && s->r[2] == SO_ACORNSSL_PROMPTTIME) {
        if (s->r[4] != 4)
            DONE(error(E_PARAMETER, integer(R_EINVAL)));
        h->prompt_max = ros_ld32(s->r[3]);
        s->r[0] = 0;
        DONE(NULL);
    }
    forward(s, SOCKET_SETSOCKOPT);
}

void ros_thunk_AcornSSL_Write(struct ros_cpu *s)
{
    DONE(do_send(s, 0));
}

void ros_thunk_AcornSSL_Send(struct ros_cpu *s)
{
    DONE(do_send(s, s->r[3]));
}

void ros_thunk_AcornSSL_Read(struct ros_cpu *s)
{
    DONE(do_recv(s, 0));
}

void ros_thunk_AcornSSL_Recv(struct ros_cpu *s)
{
    DONE(do_recv(s, s->r[3]));
}

void ros_thunk_AcornSSL_Version(struct ros_cpu *s)
{
    s->r[0] = VERSION;
    s->v = 0;
}

/* ---- the module ------------------------------------------------------------------ */

static void announce(uint32_t state)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = state, s.r[1] = SERVICE_URLMODULE_SSL, s.r[2] = VERSION;
    ros_service_call(&s);
}

static void started(void *arg)
{
    (void)arg;
    if (!announced) {
        announced = 1;
        announce(0);                                /* Service_URLModule_SSL_Started */
    }
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    if (ex_index < 0)
        ex_index = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    announced = 0;
    ros_callback_add_native(started, NULL);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    while (sessions) {
        struct session *h = sessions;
        if (!h->client_socket) {
            uint32_t r[6] = { h->socket };
            socket_swi(SOCKET_CLOSE, r);
        }
        free_session(h);
    }
    if (announced)
        announce(1);                                /* Service_URLModule_SSL_Dying */
    SSL_CTX_free(ctx);
    ctx = NULL, ctx_tried = 0;
    free(exceptions);
    exceptions = NULL;
    return NULL;
}

/* A protocol module (AcornHTTP) is starting. Tell it that SSL is here, as
 * sslmod.c does, so that its https scheme works */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] == SERVICE_URLMODULE_PROTOCOL && s->r[0] == 0 && s->r[3] == HTTP_CHUNK && announced)
        announce(0);
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module acornssl_module = {
    .title = "AcornSSL",
    .help = "AcornSSL\t1.09 (26 Sep 2026) ROSGD native, OpenSSL " OPENSSL_FULL_VERSION_STR,
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x50F80,
    .swi_thunks = ros_swi_thunks_AcornSSL,
    .swi_names = ros_swi_names_AcornSSL,
    .swi_prefix = "AcornSSL",
};

__attribute__((constructor)) static void count(void)
{
    acornssl_module.swi_count = ros_swi_count_AcornSSL;
}
