/* Copyright 1998 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation of RISC OS Open's AcornHTTP
 * (Sources/Networking/Fetchers/HTTP).
 */

/* acornhttp.c -- AcornHTTP, rewritten for ROSGD as a native module over
 * libcurl.
 *
 * RISC OS 5's AcornHTTP (1.09, Networking/Fetchers/HTTP, Docs/SWIs and
 * ImpDetails) is the http: and https: protocol module behind URL_Fetcher.
 * HTTP_GetData starts a fetch. The client polls it through URL_Status and
 * URL_ReadData, and receives the response as an HTTP/1.0 message, headers
 * then body. The original does the protocol itself, over the Internet
 * module's sockets and AcornSSL. Here libcurl does it, over Linux's sockets
 * and OpenSSL, on a thread of the module's own. A transfer goes on whether
 * or not the client polls, and ReadData hands over what has arrived. What
 * its clients depend on, this keeps:
 *
 *   - The SWIs: GetData, Status, ReadData and Stop at &83F80, and the same
 *     four for https: at &83F90 (Secure*). RegisterMethod and
 *     DeregisterMethod (&83FBB, &83FBC). The cookie SWIs AddCookie,
 *     ConsumeCookie and EnumerateCookies (&83FBD-&83FBF). It registers http:
 *     and https: with URL_Fetcher, and again whenever URL_Fetcher
 *     (re)starts.
 *   - A session is the client's poll word, R1. The status is written there,
 *     and in R0: 0 not yet connected, 7 request sent, 15 waiting for the
 *     body, 31 reading, 32 all received, 64 aborted.
 *   - The request is built as start.c builds it. The client's headers are
 *     R4, then its body. R5 is the body's length, or the data wanted. Host,
 *     Connection, User-Agent, Cookie2 and Accept-Encoding are removed. GET,
 *     HEAD and methods registered as GET-like send no body, Content-Type or
 *     Content-Length. POST, PUT and their like get Content-Type
 *     (x-www-form-urlencoded) and Content-Length added. The cookie jar's
 *     Cookie header is added unless R0 bit 30 is set. The User-Agent is the
 *     client's (R6) with "Acorn_HTTP/1.09" after it, or that alone. Accept
 *     for any type is added when the client gave none. The methods are
 *     1 GET, 2 HEAD, 3 OPTIONS, 4 POST, 5 TRACE, 8 PUT, 12 DELETE, and those
 *     registered. What is wanted is in R2 bits 8-15 when R0 bit 1 is set,
 *     and in R5 otherwise: 0 the body, 1 the head, 2 both. A proxy is R7,
 *     with R0 bit 31.
 *   - The response is shaped as header.c shapes it. The status line becomes
 *     HTTP/1.0. Content-Length is taken out, and put back at the end when it
 *     is known and not 0. Transfer-Encoding: chunked is taken out, with the
 *     chunking (libcurl dechunks). Connection is taken out, with the headers
 *     it names. The rest stay in order, with continuation lines joined.
 *     Set-Cookie goes to the cookie jar. Interim (1xx) responses are not
 *     passed on. Redirections are the client's, as they were.
 *   - ReadData: R4 is the bytes given, and R5 is what is left of the body
 *     (-1 if not known, 0 at the end). R2 and R3 are preserved. Status: R2 is
 *     the server's code, R3 the body so far, and R4 its length or -1.
 *   - A fetch that fails is an error from Status and ReadData, the
 *     original's: "Remote host not found" (&80DE20), "Unable to connect to
 *     remote host" (&80DE25), and the rest.
 *
 * https: certificates are checked against InetDBase:CertData, or the ROM's
 * copy, as AcornSSL's are. The worker thread touches nothing of the
 * runtime. It fills each fetch's buffer under the module's lock, and the
 * SWIs, on the runtime's thread, empty it.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "acornhttp.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"

#define VERSION        109
#define ERROR_BASE     0x80DE20u
#define HTTP_CHUNK     0x83F80u
#define HTTPS_BASE     0x83F90u
#define URL_PROTOCOL_REGISTER   0x83E20u
#define URL_PROTOCOL_DEREGISTER 0x83E21u
#define SERVICE_URLMODULE       0x83E00u
#define SERVICE_URLMODULE_SSL   0x83E02u
#define OTHER_METHODS  128
#define HIGH_WATER     (1024 * 1024)        /* buffered: libcurl pauses above this */
#define LOW_WATER      (256 * 1024)         /* and resumes below this */

enum { status_NOT_YET_CONNECTED = 0, status_WAIT_INITIAL_RESPONSE = 7, status_WAIT_FOR_BODY = 15,
       status_READING_REPLY = 31, status_ALL_DATA_RECEIVED = 32, status_ABORTED = 64 };
enum { method_GET = 1, method_HEAD = 2, method_OPTIONS = 3, method_POST = 4, method_TRACE = 5, method_PUT = 8,
       method_DELETE = 12 };
enum { TYPE_USER = 0, TYPE_POST = 1, TYPE_GET = 2, TYPE_MASK = 3 };

static const char *const messages[] = {
    "Remote host not found. Please check the URL", "Connection timed out. Please retry in a while",
    "Timeout reading data", "An error occurred whilst fetching an HTTP URL", "Bad session pointer passed to HTTP module",
    "Unable to connect to remote host", "Method not supported by HTTP", "Unable to initiate fetch",
    "Unable to parse URL", "Remote proxy not found. Please check the proxy settings",
    "Security support module not present", "Invalid parameter passed to SWI",
    "Insufficient internal resources to handle SWI",
};

_kernel_oserror *make_error(int which, int unused)
{
    (void)unused;
    return ros_error(ERROR_BASE + (uint32_t)which, "%s", messages[which]);
}

char *Strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

int Strcmp_ci(const char *a, const char *b)
{
    return strcasecmp(a, b);
}

int Strncmp_ci(const char *a, const char *b, size_t n)
{
    return strncasecmp(a, b, n);
}

/* A system variable, expanded, as RISC OS's C library's getenv reads one */
char *http_getenv(const char *name)
{
    static char value[256];
    size_t nl = strlen(name) + 1;
    char *b = ros_rma_alloc(nl + 256);
    if (!b)
        return NULL;
    memcpy(b, name, nl);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(b), s.r[1] = ros_addr(b + nl), s.r[2] = 255, s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, ROS_X_BIT | 0x23);                  /* OS_ReadVarVal, expanded */
    char *result = NULL;
    if (!s.v) {
        snprintf(value, sizeof value, "%.*s", (int)s.r[2], b + nl);
        result = value;
    }
    ros_rma_free(b);
    return result;
}

/* ---- header lists (c.header) ------------------------------------------------ */

http_header *http_add_header(http_header **list, const char *header, const char *value)
{
    http_header *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->header = strdup(header);
    h->value = strdup(value ? value : "");
    while (*list)
        list = &(*list)->next;
    *list = h;
    return h;
}

http_header *http_find_header(http_header *list, const char *header)
{
    for (; list; list = list->next)
        if (!strcasecmp(list->header, header))
            return list;
    return NULL;
}

void http_delete_header(http_header **list, http_header *which)
{
    for (; *list; list = &(*list)->next)
        if (*list == which) {
            *list = which->next;
            free(which->header), free(which->value), free(which);
            return;
        }
}

void http_free_headers(http_header **list)
{
    while (*list)
        http_delete_header(list, *list);
}

static void delete_all(http_header **list, const char *name)
{
    http_header *h;
    while ((h = http_find_header(*list, name)))
        http_delete_header(list, h);
}

/* ---- methods --------------------------------------------------------------------- */

static struct {
    char *name;
    uint32_t flags;
    unsigned uses;
} others[OTHER_METHODS];

static const char *method_text(int method)
{
    switch (method) {
    case method_GET: return "GET";
    case method_HEAD: return "HEAD";
    case method_OPTIONS: return "OPTIONS";
    case method_POST: return "POST";
    case method_TRACE: return "TRACE";
    case method_PUT: return "PUT";
    case method_DELETE: return "DELETE";
    default: return method > 0 && method < OTHER_METHODS ? others[method].name : NULL;
    }
}

static int method_type(int method)
{
    if (method == method_GET || method == method_HEAD)
        return TYPE_GET;
    if (method == method_POST || method == method_PUT)
        return TYPE_POST;
    if (method > 0 && method < OTHER_METHODS && others[method].name)
        return (int)(others[method].flags & TYPE_MASK);
    return TYPE_USER;
}

static void methods_init(void)
{
    memset(others, 0, sizeof others);
    static const int fixed[] = { 0, method_GET, method_HEAD, method_OPTIONS, method_POST, method_TRACE, method_PUT,
                                 method_DELETE };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        others[fixed[i]].uses = 1;                  /* never given out */
}

/* ---- fetches --------------------------------------------------------------------- */

struct fetch {
    /* the runtime's thread's */
    uint32_t id;                    /* the poll word */
    int method, wanted;
    Session c;                      /* for the cookie jar */
    struct curl_slist *headers;
    char *body;
    size_t body_len;
    char *proxy;
    CURL *easy;
    long long given;                /* bytes given to the client: head, then body */
    struct fetch *next;
    /* the worker's, shared under the lock */
    int state, code, done, failed_connect;
    CURLcode result;
    long long size, received;
    char *raw;                      /* the header block so far */
    size_t raw_len;
    int interim, head_done;
    size_t head_len;                /* the shaped head's bytes, at the start of what is given */
    char *out;                      /* for the client: the shaped head, then the body */
    size_t out_len, out_pos, out_cap;
    char **cookies;                 /* Set-Cookie values, for the jar */
    int ncookies;
    int paused, unpause, stopped;
    struct fetch *queue;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static int worker_running;
static CURLM *multi;
static struct fetch *fetches;               /* the runtime's list */
static struct fetch *to_add, *to_remove;    /* for the worker, under the lock */
static char *ca_pem;
static size_t ca_len;
static int registered_http, registered_https;

static struct fetch *find_fetch(uint32_t id)
{
    for (struct fetch *f = fetches; f; f = f->next)
        if (f->id == id)
            return f;
    return NULL;
}

static void append(struct fetch *f, const char *p, size_t n)
{
    if (f->out_len + n > f->out_cap) {
        if (f->out_pos) {                           /* close the gap first */
            memmove(f->out, f->out + f->out_pos, f->out_len - f->out_pos);
            f->out_len -= f->out_pos, f->out_pos = 0;
        }
        if (f->out_len + n > f->out_cap) {
            size_t cap = f->out_cap ? f->out_cap : 16384;
            while (cap < f->out_len + n)
                cap *= 2;
            char *grown = realloc(f->out, cap);
            if (!grown)
                return;
            f->out = grown, f->out_cap = cap;
        }
    }
    memcpy(f->out + f->out_len, p, n);
    f->out_len += n;
}

/* The response's head, shaped as header.c shapes it (the worker's) */
static void shape_head(struct fetch *f)
{
    http_header *list = NULL;
    char *text = f->raw ? f->raw : (char *)"";
    int chunked = 0, first = 1;
    long long size = -1;
    /* join continuation lines */
    for (char *p = text; (p = strchr(p, '\n'));) {
        if (p[1] == ' ' || p[1] == '\t') {
            char *q = p;
            if (q > text && q[-1] == '\r')
                q[-1] = ' ';
            *q = ' ';
        }
        p++;
    }
    for (char *line = text, *next; *line; line = next) {
        next = line + strcspn(line, "\r\n");
        if (*next == '\r')
            *next++ = 0;
        if (*next == '\n')
            *next++ = 0;
        char *end = line + strlen(line);
        while (end > line && isspace((unsigned char)end[-1]))
            *--end = 0;
        line += strspn(line, " \t");
        if (!*line)
            continue;
        char *header = line, *value = line + strcspn(line, " :\t");
        if (!*value)
            continue;
        *value++ = 0;
        value += strspn(value, " :\t");
        if (first) {
            first = 0;
            http_add_header(&list, "HTTP/1.0", value);
            continue;
        }
        if (!strcasecmp(header, "Content-Length")) {
            size = strtoll(value, NULL, 10);
            continue;
        }
        if (!strcasecmp(header, "Transfer-Encoding") && !strcasecmp(value, "chunked")) {
            chunked = 1;
            continue;
        }
        if (!strcasecmp(header, "Set-Cookie")) {
            char **grown = realloc(f->cookies, (size_t)(f->ncookies + 1) * sizeof *grown);
            if (grown) {
                f->cookies = grown;
                f->cookies[f->ncookies++] = strdup(value);
            }
        }
        http_add_header(&list, header, value);
    }
    http_header *conn;
    while ((conn = http_find_header(list, "Connection"))) {
        char *tokens = strdup(conn->value);
        for (char *t = strtok(tokens, ", \t"); t; t = strtok(NULL, ", \t"))
            if (strcasecmp(t, "connection"))
                delete_all(&list, t);
        free(tokens);
        http_delete_header(&list, conn);
    }
    if (chunked)
        size = -1;
    if (size > 0) {
        char n[24];
        snprintf(n, sizeof n, "%lld", size);
        http_add_header(&list, "Content-Length", n);
    }
    f->size = size;
    if (f->wanted > 0) {
        size_t before = f->out_len - f->out_pos;
        for (http_header *h = list; h; h = h->next) {
            char line[2048];
            int n = snprintf(line, sizeof line, "%s%s %s\r\n", h->header, h == list ? "" : ":", h->value);
            append(f, line, n < (int)sizeof line ? (size_t)n : sizeof line - 1);
        }
        append(f, "\r\n", 2);
        f->head_len = f->out_len - f->out_pos - before;
    }
    http_free_headers(&list);
}

static size_t on_header(char *p, size_t size, size_t n, void *arg)
{
    struct fetch *f = arg;
    size_t len = size * n;
    pthread_mutex_lock(&lock);
    int blank = len <= 2 && (len == 0 || p[0] == '\r' || p[0] == '\n');
    if (len > 5 && !strncmp(p, "HTTP/", 5)) {        /* a response begins */
        const char *sp = memchr(p, ' ', len);
        int code = sp ? atoi(sp + 1) : 0;
        f->interim = code >= 100 && code < 200;
        if (!f->interim) {
            f->code = code;
            f->raw_len = 0;
            f->head_done = 0;
        }
    }
    if (f->interim) {
        if (blank)
            f->interim = 0;
    } else if (blank) {
        if (!f->head_done) {
            f->head_done = 1;
            shape_head(f);
            f->state = status_WAIT_FOR_BODY;
        }
    } else {
        char *grown = realloc(f->raw, f->raw_len + len + 1);
        if (grown) {
            f->raw = grown;
            memcpy(f->raw + f->raw_len, p, len);
            f->raw_len += len;
            f->raw[f->raw_len] = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return len;
}

static size_t on_body(char *p, size_t size, size_t n, void *arg)
{
    struct fetch *f = arg;
    size_t len = size * n;
    pthread_mutex_lock(&lock);
    if (f->stopped) {
        pthread_mutex_unlock(&lock);
        return 0;                                   /* abandoned: end the transfer */
    }
    if (f->wanted != 1 && f->out_len - f->out_pos > HIGH_WATER) {
        f->paused = 1;
        pthread_mutex_unlock(&lock);
        return CURL_WRITEFUNC_PAUSE;
    }
    f->received += (long long)len;
    if (f->wanted != 1)
        append(f, p, len);
    f->state = status_READING_REPLY;
    pthread_mutex_unlock(&lock);
    return len;
}

/* Connected, the request about to go */
static int on_prereq(void *arg, char *conn_primary_ip, char *conn_local_ip, int conn_primary_port,
                     int conn_local_port)
{
    (void)conn_primary_ip, (void)conn_local_ip, (void)conn_primary_port, (void)conn_local_port;
    struct fetch *f = arg;
    pthread_mutex_lock(&lock);
    if (f->state < status_WAIT_INITIAL_RESPONSE)
        f->state = status_WAIT_INITIAL_RESPONSE;
    pthread_mutex_unlock(&lock);
    return CURL_PREREQFUNC_OK;
}

static void free_fetch_worker(struct fetch *f)
{
    if (f->easy)
        curl_easy_cleanup(f->easy);
    curl_slist_free_all(f->headers);
    free(f->body), free(f->proxy), free(f->raw), free(f->out);
    for (int i = 0; i < f->ncookies; i++)
        free(f->cookies[i]);
    free(f->cookies);
    free(f->c.host), free(f->c.endhost), free(f->c.url), free(f->c.uri);
    http_free_headers(&f->c.headers);
    free(f);
}

static void *work(void *arg)
{
    ros_thread_name("http");      /* named for /proc: what uses the time */
    (void)arg;
    ros_thread_signal_stack();
    for (;;) {
        struct fetch *resume[64];
        int nresume = 0;
        pthread_mutex_lock(&lock);
        if (!worker_running) {
            pthread_mutex_unlock(&lock);
            break;
        }
        while (to_add) {
            struct fetch *f = to_add;
            to_add = f->queue;
            curl_multi_add_handle(multi, f->easy);
        }
        while (to_remove) {
            struct fetch *f = to_remove;
            to_remove = f->queue;
            curl_multi_remove_handle(multi, f->easy);
            free_fetch_worker(f);
        }
        for (struct fetch *f = fetches; f && nresume < 64; f = f->next)
            if (f->paused && f->unpause) {
                f->paused = f->unpause = 0;
                resume[nresume++] = f;
            }
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < nresume; i++)           /* the write callback may run: no lock held */
            curl_easy_pause(resume[i]->easy, CURLPAUSE_CONT);
        int running;
        curl_multi_poll(multi, NULL, 0, 1000, NULL);
        curl_multi_perform(multi, &running);
        CURLMsg *m;
        int left;
        while ((m = curl_multi_info_read(multi, &left))) {
            if (m->msg != CURLMSG_DONE)
                continue;
            struct fetch *f = NULL;
            curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&f);
            if (!f)
                continue;
            long connect_code = 0;
            double connect_time = 0;
            curl_easy_getinfo(m->easy_handle, CURLINFO_CONNECT_TIME, &connect_time);
            curl_easy_getinfo(m->easy_handle, CURLINFO_HTTP_CONNECTCODE, &connect_code);
            pthread_mutex_lock(&lock);
            f->done = 1;
            f->result = m->data.result;
            f->failed_connect = connect_time <= 0;
            if (f->result == CURLE_OK && !f->head_done) {   /* HEAD, or a head with no end */
                f->head_done = 1;
                shape_head(f);
            }
            pthread_mutex_unlock(&lock);
        }
    }
    return NULL;
}

/* ---- the root certificates ------------------------------------------------------- */

static char *load_file(const char *name, size_t *size)
{
    size_t nl = strlen(name) + 1;
    char *b = ros_rma_alloc(nl);
    if (!b)
        return NULL;
    memcpy(b, name, nl);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 17, s.r[1] = ros_addr(b);
    ros_swi(&s, ROS_X_BIT | 0x08);                  /* OS_File 17 */
    char *copy = NULL;
    if (!s.v && s.r[0] == 1) {
        uint32_t len = s.r[4];
        uint8_t *buf = ros_rma_alloc(len + 1);
        if (buf) {
            ros_cpu_enter(&s);
            s.r[0] = 16, s.r[1] = ros_addr(b), s.r[2] = ros_addr(buf), s.r[3] = 0;
            ros_swi(&s, ROS_X_BIT | 0x08);          /* OS_File 16: load */
            if (!s.v && (copy = malloc(len + 1))) {
                memcpy(copy, buf, len);
                copy[len] = 0;
                *size = len;
            }
            ros_rma_free(buf);
        }
    }
    ros_rma_free(b);
    return copy;
}

static void load_roots(void)
{
    if (ca_pem)
        return;
    ca_pem = load_file("InetDBase:CertData", &ca_len);
    if (!ca_pem)
        ca_pem = load_file("Resources:$.Resources.URL.AcornSSL.CertData", &ca_len);
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

static void set_status(struct fetch *f, int state, struct ros_cpu *s)
{
    s->r[0] = (uint32_t)state;
    ros_st32(f->id, (uint32_t)state);
}

/* The client's R4 data: headers, a blank line, the body */
static void parse_client_data(struct fetch *f, const char *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && data[eol] != '\n' && data[eol] != '\r')
            eol++;
        size_t next = eol;
        if (next < len && data[next] == '\r')
            next++;
        if (next < len && data[next] == '\n')
            next++;
        if (eol == i) {                             /* the blank line: the body follows */
            i = next;
            break;
        }
        char line[2048];
        size_t n = eol - i < sizeof line - 1 ? eol - i : sizeof line - 1;
        memcpy(line, data + i, n);
        line[n] = 0;
        char *header = line + strspn(line, " \t"), *value = header + strcspn(header, " :\t");
        if (*value) {
            *value++ = 0;
            value += strspn(value, " :\t");
            if (strcasecmp(header, "host") && strcasecmp(header, "connection") && strcasecmp(header, "user-agent"))
                http_add_header(&f->c.headers, header, value);
        }
        i = next;
        if (i >= len)
            break;
    }
    if (i < len) {
        f->body_len = len - i;
        f->body = malloc(f->body_len + 1);
        if (f->body) {
            memcpy(f->body, data + i, f->body_len);
            f->body[f->body_len] = 0;
        } else {
            f->body_len = 0;
        }
    }
}

static const char *our_agent = Module_Help "/" Module_VersionString;

static void start(struct ros_cpu *s, int secure)
{
    uint32_t flags = s->r[0] & ~(uint32_t)flags_USING_HTTPS;
    if (secure)
        flags |= flags_USING_HTTPS;
    if (find_fetch(s->r[1]))
        DONE(make_error(HTTP_BAD_SESSION_ERROR, 0));
    int method = (int)s->r[2], wanted;
    if (flags & flags_DATA_LENGTH_IN_R5) {
        wanted = (method >> 8) & 0xFF;
        method &= 0xFF;
    } else {
        wanted = (int)(s->r[5] & 0xFF);
    }
    const char *mtext = method_text(method);
    if (!mtext)
        DONE(make_error(HTTP_METHOD_UNSUPPORTED, 0));

    struct fetch *f = calloc(1, sizeof *f);
    if (!f)
        DONE(make_error(HTTP_NO_RESOURCES, 0));
    f->id = s->r[1];
    f->method = method, f->wanted = wanted;
    f->size = -1;
    f->c.flags = (int)flags;
    f->c.url = strdup(ros_ptr(s->r[3]));
    if (flags & (uint32_t)flags_PROXY && s->r[7])
        f->proxy = strdup(ros_ptr(s->r[7]));

    CURLU *u = curl_url();
    char *host = NULL, *path = NULL, *query = NULL;
    if (!u || curl_url_set(u, CURLUPART_URL, f->c.url, CURLU_NON_SUPPORT_SCHEME) ||
        curl_url_get(u, CURLUPART_HOST, &host, 0)) {
        curl_url_cleanup(u);
        free_fetch_worker(f);
        DONE(make_error(HTTP_BAD_URL_PARSE, 0));
    }
    curl_url_get(u, CURLUPART_PATH, &path, 0);
    curl_url_get(u, CURLUPART_QUERY, &query, 0);
    f->c.host = strdup(host);
    size_t ul = (path ? strlen(path) : 1) + (query ? strlen(query) + 1 : 0) + 1;
    f->c.uri = malloc(ul);
    snprintf(f->c.uri, ul, "%s%s%s", path ? path : "/", query ? "?" : "", query ? query : "");
    curl_free(host), curl_free(path), curl_free(query);
    curl_url_cleanup(u);

    /* the client's data: headers, then the body */
    if (s->r[4]) {
        const char *data = ros_ptr(s->r[4]);
        size_t len = (flags & flags_DATA_LENGTH_IN_R5) ? s->r[5] : strlen(data);
        parse_client_data(f, data, len);
    }
    int type = method_type(method);
    http_header *cl = http_find_header(f->c.headers, "content-length");
    if (cl && (size_t)strtoul(cl->value, NULL, 10) > f->body_len) {
        char n[24];
        snprintf(n, sizeof n, "%zu", f->body_len);
        http_delete_header(&f->c.headers, cl);
        http_add_header(&f->c.headers, "content-length", n);
    }
    if (type == TYPE_GET) {
        delete_all(&f->c.headers, "content-type");
        delete_all(&f->c.headers, "content-length");
        delete_all(&f->c.headers, "transfer-encoding");
        free(f->body);
        f->body = NULL, f->body_len = 0;
    }
    if (type == TYPE_POST) {
        if (!http_find_header(f->c.headers, "content-type"))
            http_add_header(&f->c.headers, "content-type", "application/x-www-form-urlencoded");
        if (!http_find_header(f->c.headers, "content-length") &&
            !http_find_header(f->c.headers, "transfer-encoding")) {
            char n[24];
            snprintf(n, sizeof n, "%zu", f->body_len);
            http_add_header(&f->c.headers, "content-length", n);
        }
    }
    delete_all(&f->c.headers, "user-agent");
    delete_all(&f->c.headers, "cookie2");
    delete_all(&f->c.headers, "accept-encoding");
    if (!(flags & flags_NO_COOKIES))
        send_cookies_to_domain(&f->c);              /* adds Cookie */
    char agent[512];
    const char *ua = (flags & flags_USER_AGENT_IN_R6) && s->r[6] ? ros_ptr(s->r[6]) : NULL;
    if (ua && *ua) {
        if (strstr(ua, our_agent))
            snprintf(agent, sizeof agent, "%s", ua);
        else
            snprintf(agent, sizeof agent, "%s %s", ua, our_agent);
        for (char *a = agent; *a; a++)              /* top-bit characters confuse servers */
            if ((unsigned char)*a == 0xDF)
                *a = 'B';
            else if ((unsigned char)*a > 0x7E || !isprint((unsigned char)*a))
                *a = ' ';
    } else {
        snprintf(agent, sizeof agent, "%s", our_agent);
    }
    http_add_header(&f->c.headers, "User-Agent", agent);
    if (!http_find_header(f->c.headers, "Accept"))
        http_add_header(&f->c.headers, "Accept", "*/*");
    for (http_header *h = f->c.headers; h; h = h->next) {
        char line[2048];
        snprintf(line, sizeof line, "%s: %s", h->header, h->value);
        f->headers = curl_slist_append(f->headers, line);
    }
    f->headers = curl_slist_append(f->headers, "Expect:");  /* no 100-continue round trip */

    /* the transfer */
    load_roots();
    CURL *e = curl_easy_init();
    if (!e) {
        free_fetch_worker(f);
        DONE(make_error(HTTP_METHOD_INIT_ERR, 0));
    }
    f->easy = e;
    curl_easy_setopt(e, CURLOPT_URL, f->c.url);
    curl_easy_setopt(e, CURLOPT_PRIVATE, f);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, f->headers);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, f);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(e, CURLOPT_PREREQFUNCTION, on_prereq);
    curl_easy_setopt(e, CURLOPT_PREREQDATA, f);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(e, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(e, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    curl_easy_setopt(e, CURLOPT_HTTP09_ALLOWED, 1L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 120L);
    if (ca_pem) {
        struct curl_blob blob = { ca_pem, ca_len, CURL_BLOB_NOCOPY };
        curl_easy_setopt(e, CURLOPT_CAINFO_BLOB, &blob);
    }
    if (f->proxy)
        curl_easy_setopt(e, CURLOPT_PROXY, f->proxy);
    else
        curl_easy_setopt(e, CURLOPT_PROXY, "");     /* no proxy from the host's environment */
    if (method == method_HEAD) {
        curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
    } else {
        if (f->body || type == TYPE_POST) {
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)f->body_len);
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, f->body ? f->body : "");
        }
        if (method != method_GET && method != method_POST)
            curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, mtext);
        else if (method == method_GET && f->body == NULL)
            curl_easy_setopt(e, CURLOPT_HTTPGET, 1L);
    }

    f->state = status_NOT_YET_CONNECTED;
    pthread_mutex_lock(&lock);
    f->next = fetches;
    fetches = f;
    f->queue = to_add;
    to_add = f;
    pthread_mutex_unlock(&lock);
    curl_multi_wakeup(multi);
    set_status(f, status_NOT_YET_CONNECTED, s);
    DONE(NULL);
}

/* The failure's error, the original's */
static os_error *failure(struct fetch *f)
{
    switch (f->result) {
    case CURLE_COULDNT_RESOLVE_HOST: return make_error(HTTP_HOST_NOT_FOUND, 0);
    case CURLE_COULDNT_RESOLVE_PROXY: return make_error(HTTP_PROXY_NOT_FOUND, 0);
    case CURLE_COULDNT_CONNECT: return make_error(HTTP_CONNECTION_FAILED, 0);
    case CURLE_OPERATION_TIMEDOUT:
        return make_error(f->failed_connect ? HTTP_HOST_CONNECT_ERROR : HTTP_DATA_READ_ERROR, 0);
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE: return make_error(HTTP_DATA_READ_ERROR, 0);
    default: return make_error(HTTP_GENERAL_ERROR, 0);
    }
}

/* Cookies the worker saw, into the jar (the jar is the runtime's thread's) */
static void take_cookies(struct fetch *f)
{
    char **cookies;
    int n;
    pthread_mutex_lock(&lock);
    cookies = f->cookies, n = f->ncookies;
    f->cookies = NULL, f->ncookies = 0;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) {
        cookie_set_cookie(cookies[i], &f->c);
        free(cookies[i]);
    }
    free(cookies);
}

static void status(struct ros_cpu *s)
{
    struct fetch *f = find_fetch(s->r[1]);
    if (!f)
        DONE(make_error(HTTP_BAD_SESSION_ERROR, 0));
    take_cookies(f);
    pthread_mutex_lock(&lock);
    int state = f->state, done = f->done, code = f->code;
    CURLcode result = f->result;
    long long received = f->received, size = f->size;
    size_t buffered = f->out_len - f->out_pos;
    pthread_mutex_unlock(&lock);
    if (done && result != CURLE_OK) {
        set_status(f, status_ABORTED, s);
        DONE(failure(f));
    }
    if (done && !buffered)
        state = status_ALL_DATA_RECEIVED;
    set_status(f, state, s);
    s->r[2] = (uint32_t)code;
    s->r[3] = (uint32_t)received;
    s->r[4] = (uint32_t)(size >= 0 ? size : -1);
    DONE(NULL);
}

/* Each page of [buf, buf + n) is written with what it holds. Memory that the
 * caller cannot write then faults here, as its data abort (runtime/fault.c),
 * and not while this module's lock is held. A fault under the lock would
 * leave the fetch worker and every later call waiting for it (issue 3) */
static void touch(uint8_t *buf, size_t n)
{
    volatile uint8_t *b = buf;
    for (size_t i = 0; i < n; i = (i | 4095) + 1)
        b[i] = b[i];
    if (n)
        b[n - 1] = b[n - 1];
}

static void readdata(struct ros_cpu *s)
{
    struct fetch *f = find_fetch(s->r[1]);
    if (!f)
        DONE(make_error(HTTP_BAD_SESSION_ERROR, 0));
    take_cookies(f);
    uint8_t *buf = ros_ptr(s->r[2]);
    size_t room = s->r[3];
    /* What there is to give. Then the caller's memory for it is tried with no
     * lock held. Only this SWI takes from the buffer (under the big lock),
     * so there is at least as much once the lock is taken again */
    pthread_mutex_lock(&lock);
    size_t n = f->out_len - f->out_pos;
    pthread_mutex_unlock(&lock);
    if (n > room)
        n = room;
    touch(buf, n);
    pthread_mutex_lock(&lock);
    size_t buffered = f->out_len - f->out_pos;
    memcpy(buf, f->out + f->out_pos, n);
    f->out_pos += n;
    buffered -= n;
    int wake = 0;
    if (f->paused && buffered < LOW_WATER)
        f->unpause = 1, wake = 1;
    int state = f->state, done = f->done, head_done = f->head_done;
    CURLcode result = f->result;
    long long size = f->size;
    size_t head_len = f->head_len;
    pthread_mutex_unlock(&lock);
    if (wake)
        curl_multi_wakeup(multi);
    f->given += (long long)n;
    s->r[4] = (uint32_t)n;
    if (done && !buffered) {
        if (result != CURLE_OK) {
            set_status(f, status_ABORTED, s);
            if (!n)
                DONE(failure(f));
        } else {
            set_status(f, status_ALL_DATA_RECEIVED, s);
        }
        s->r[5] = 0;
        DONE(NULL);
    }
    set_status(f, state, s);
    s->r[5] = (uint32_t)-1;                         /* not known */
    if (size >= 0 && head_done) {
        long long body = f->given - (long long)head_len;
        long long left = size - (body > 0 ? body : 0);
        if (left > 0)
            s->r[5] = (uint32_t)left;
    }
    DONE(NULL);
}

static void stop(struct ros_cpu *s)
{
    struct fetch *f = find_fetch(s->r[1]);
    s->r[0] = 0;
    if (!f)
        DONE(NULL);                                 /* kill_session validates, and ignores */
    take_cookies(f);
    pthread_mutex_lock(&lock);                      /* the worker walks the list */
    for (struct fetch **p = &fetches; *p; p = &(*p)->next)
        if (*p == f) {
            *p = f->next;
            break;
        }
    /* still queued to be added: never given to libcurl */
    int queued = 0;
    for (struct fetch **q = &to_add; *q; q = &(*q)->queue)
        if (*q == f) {
            *q = f->queue;
            queued = 1;
            break;
        }
    if (!queued) {
        f->stopped = 1;
        f->queue = to_remove;
        to_remove = f;
    }
    pthread_mutex_unlock(&lock);
    if (queued)
        free_fetch_worker(f);
    else
        curl_multi_wakeup(multi);
    DONE(NULL);
}

void ros_thunk_HTTP_GetData(struct ros_cpu *s)
{
    start(s, 0);
}

void ros_thunk_HTTP_Status(struct ros_cpu *s)
{
    status(s);
}

void ros_thunk_HTTP_ReadData(struct ros_cpu *s)
{
    uint32_t r2 = s->r[2], r3 = s->r[3];
    s->r[4] = 0, s->r[5] = (uint32_t)-1;
    readdata(s);
    s->r[2] = r2, s->r[3] = r3;                     /* preserved, as http_readdata's wrapper does */
}

void ros_thunk_HTTP_Stop(struct ros_cpu *s)
{
    stop(s);
}

void ros_thunk_HTTP_SecureGetData(struct ros_cpu *s)
{
    start(s, 1);
}

void ros_thunk_HTTP_SecureStatus(struct ros_cpu *s)
{
    status(s);
}

void ros_thunk_HTTP_SecureReadData(struct ros_cpu *s)
{
    ros_thunk_HTTP_ReadData(s);
}

void ros_thunk_HTTP_SecureStop(struct ros_cpu *s)
{
    stop(s);
}

void ros_thunk_HTTP_RegisterMethod(struct ros_cpu *s)
{
    if (!s->r[0] || (s->r[1] & ~(uint32_t)TYPE_MASK))
        DONE(make_error(HTTP_BAD_PARAMETER, 0));
    const char *name = ros_ptr(s->r[0]);
    if (!*name)
        DONE(make_error(HTTP_BAD_PARAMETER, 0));
    int free_slot = -1;
    for (int i = 0; i < OTHER_METHODS; i++) {
        if (free_slot < 0 && !others[i].uses) {
            free_slot = i;
        } else if (others[i].uses && others[i].name && others[i].flags == s->r[1] &&
                   !strcmp(others[i].name, name)) {
            others[i].uses++;
            s->r[2] = (uint32_t)i;
            DONE(NULL);
        }
    }
    if (free_slot < 0 || !(others[free_slot].name = strdup(name)))
        DONE(make_error(HTTP_NO_RESOURCES, 0));
    others[free_slot].uses = 1;
    others[free_slot].flags = s->r[1];
    s->r[2] = (uint32_t)free_slot;
    DONE(NULL);
}

void ros_thunk_HTTP_DeregisterMethod(struct ros_cpu *s)
{
    if (!s->r[0])
        DONE(make_error(HTTP_BAD_PARAMETER, 0));
    const char *name = ros_ptr(s->r[0]);
    for (int i = 0; i < OTHER_METHODS; i++)
        if (others[i].uses && others[i].name && others[i].flags == s->r[1] && !strcmp(others[i].name, name)) {
            if (!--others[i].uses) {
                free(others[i].name);
                others[i].name = NULL;
                others[i].flags = 0;
            }
            DONE(NULL);
        }
    DONE(make_error(HTTP_METHOD_UNSUPPORTED, 0));
}

static void cookie_swi(struct ros_cpu *s, _kernel_oserror *(*fn)(uint32_t r[8]))
{
    uint32_t r[8];
    for (int i = 0; i < 8; i++)
        r[i] = s->r[i];
    os_error *e = fn(r);
    if (!e)
        for (int i = 0; i < 8; i++)
            s->r[i] = r[i];
    DONE(e);
}

void ros_thunk_HTTP_AddCookie(struct ros_cpu *s)
{
    cookie_swi(s, add_cookie);
}

void ros_thunk_HTTP_ConsumeCookie(struct ros_cpu *s)
{
    cookie_swi(s, consume_cookie);
}

void ros_thunk_HTTP_EnumerateCookies(struct ros_cpu *s)
{
    cookie_swi(s, enumerate_cookies);
}

/* ---- registering with URL_Fetcher -------------------------------------------------- */

static os_error *protocol_register(uint32_t base, const char *scheme, const char *info)
{
    char *b = ros_rma_alloc(128);
    if (!b)
        return make_error(HTTP_NO_RESOURCES, 0);
    strcpy(b, scheme);
    strcpy(b + 16, info);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = base, s.r[2] = ros_addr(b), s.r[3] = VERSION, s.r[4] = ros_addr(b + 16);
    ros_swi(&s, ROS_X_BIT | URL_PROTOCOL_REGISTER);
    os_error *e = s.v ? ros_ptr(s.r[0]) : NULL;
    ros_rma_free(b);
    return e;
}

static void protocol_deregister(uint32_t base)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = base;
    ros_swi(&s, ROS_X_BIT | URL_PROTOCOL_DEREGISTER);
}

/* http: and https: (TLS is libcurl's own, so https: needs no AcornSSL) */
static void try_to_register(void)
{
    if (!registered_http && !protocol_register(HTTP_CHUNK, "http:", "Acorn_HTTP 1.09 ROSGD native, over libcurl"))
        registered_http = 1;
    if (registered_http && !registered_https &&
        !protocol_register(HTTPS_BASE, "https:", "Acorn_HTTP 1.09 ROSGD native, TLS over OpenSSL"))
        registered_https = 1;
}

static void try_to_deregister(void)
{
    if (registered_https)
        protocol_deregister(HTTPS_BASE);
    if (registered_http)
        protocol_deregister(HTTP_CHUNK);
    registered_http = registered_https = 0;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    if (s->r[1] != SERVICE_URLMODULE)
        return;
    if (s->r[0] == 0) {                             /* URL_Fetcher started: (again) */
        registered_http = registered_https = 0;
        try_to_register();
    } else if (s->r[0] == 1) {                      /* URL_Fetcher dying */
        registered_http = registered_https = 0;
    }
}

static void registering(void *arg)
{
    (void)arg;
    try_to_register();
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    if (curl_global_init(CURL_GLOBAL_DEFAULT))
        return make_error(HTTP_METHOD_INIT_ERR, 0);
    multi = curl_multi_init();
    if (!multi)
        return make_error(HTTP_METHOD_INIT_ERR, 0);
    methods_init();
    worker_running = 1;
    if (pthread_create(&worker, NULL, work, NULL)) {
        worker_running = 0;
        curl_multi_cleanup(multi);
        return make_error(HTTP_NO_RESOURCES, 0);
    }
    read_cookie_file();
    ros_callback_add_native(registering, NULL);     /* when URL_Fetcher is there to hear */
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    try_to_deregister();
    while (fetches) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[1] = fetches->id;
        stop(&s);
    }
    pthread_mutex_lock(&lock);
    worker_running = 0;
    pthread_mutex_unlock(&lock);
    curl_multi_wakeup(multi);
    pthread_join(worker, NULL);
    while (to_remove) {
        struct fetch *f = to_remove;
        to_remove = f->queue;
        curl_multi_remove_handle(multi, f->easy);
        free_fetch_worker(f);
    }
    curl_multi_cleanup(multi);
    multi = NULL;
    cookie_final();
    free(ca_pem);
    ca_pem = NULL;
    for (int i = 0; i < OTHER_METHODS; i++)
        free(others[i].name);
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module acornhttp_module = {
    .title = "AcornHTTP",
    .help = "Acorn_HTTP\t1.09 (26 Sep 2026) ROSGD native, over libcurl",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = HTTP_CHUNK,
    .swi_thunks = ros_swi_thunks_AcornHTTP,
    .swi_names = ros_swi_names_AcornHTTP,
    .swi_prefix = "HTTP",
};

__attribute__((constructor)) static void count(void)
{
    acornhttp_module.swi_count = ros_swi_count_AcornHTTP;
}
