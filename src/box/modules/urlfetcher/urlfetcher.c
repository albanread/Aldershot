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
 * This file is a reimplementation of RISC OS Open's URL fetcher
 * (Sources/Networking/Fetchers/URL).
 */

/* urlfetcher.c -- URL_Fetcher, rewritten for ROSGD as a native module.
 *
 * RISC OS 5's URL module (0.58, Networking/Fetchers/URL, Docs/APISpec) is
 * the one door to the fetchers. A client registers a session, asks for a
 * URL, and polls Status and ReadData. A protocol module does the work behind
 * its own SWIs: AcornHTTP for http: and https:, FileFetcher for file:, and
 * so on. It is a router, and it is one here, with the original's rules:
 *
 *   - A session identifier is the address of a word in the RMA. That word is
 *     the client's poll word, as the original's is. A session carries one
 *     fetch, so GetURL again is "Already connected to a protocol module".
 *   - GetURL canonicalises the URL (URL_ParseURL's own resolver, parseurl.c,
 *     ported unchanged). It looks for a proxy in this order: the client's
 *     no-proxy list, the client's proxies, the global no-proxy list, and the
 *     global proxies. It finds the protocol module registered for the scheme
 *     (of the proxy, if proxied) and calls its SWI base + 0 with the
 *     client's registers. R3 is the canonical URL. For a proxy, R7 is its
 *     URL and R0 bit 31 is set. The client's R3 comes back as it was. (The
 *     original's third step searched the global proxy list again, so global
 *     no-proxy entries never took effect. The specification's order is kept
 *     here.)
 *   - Status, ReadData and Stop go to base + 1, + 2 and + 3 with the
 *     client's registers. Stop forgets the protocol, and Deregister stops a
 *     fetch still going.
 *   - URL_ProtocolRegister and ProtocolDeregister keep the list of schemes,
 *     with their flags and default ports (the original's table for the
 *     schemes it knows). They announce each change with
 *     Service_URLModule_ProtocolModule (&83E01). Service_URLModule (&83E00)
 *     says the module has started (on a callback, so protocol modules can
 *     register at once) or is dying.
 *   - SetProxy and EnumerateProxies, EnumerateSchemes, ParseURL, and
 *     *URLProtoShow are as there. Errors are &80DE00 + n, the original's.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "parseurl.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "urlfetcher.h"

#define VERSION        58              /* URL 0.58: the interface this is */
#define ERROR_BASE     0x80DE00u
#define URL_CHUNK      0x83E00u
#define SERVICE_URLMODULE          0x83E00u
#define SERVICE_URLMODULE_PROTOCOL 0x83E01u

static const char *const messages[] = {
    "Client ID not found", "URL could not free enough memory", "No fetcher service found",
    "SWI not found (URL Module)", "Already connected to a protocol module", "Not connected to protocol module",
    "URL method already exists", "No fetch currently in progress for this client ID",
    "Token not found in messages file", "Session is not active", "Unable to parse URL",
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

/* A string in the RMA, for a pointer handed to a client */
static uint32_t rma_string(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s) + 1;
    char *b = ros_rma_alloc(n);
    if (!b)
        return 0;
    memcpy(b, s, n);
    return ros_addr(b);
}

static void rma_free(uint32_t a)
{
    if (a)
        ros_rma_free(ros_ptr(a));
}

/* ---- protocol modules ------------------------------------------------------ */

struct protocol {
    uint32_t scheme;                /* "http:", lower case, in the RMA */
    uint32_t swi_base;
    uint32_t version;
    uint32_t port;
    int flags;
    uint32_t info;                  /* up to 50 characters, in the RMA */
    struct protocol *next;
};

static struct protocol *protocols;

/* The original's table (c.protocol) for the schemes it knows */
static const struct {
    const char *scheme;
    int flags;
    unsigned port;
} defaults[] = {
    { "mailto:", proto_HAS_NO_NETLOC, 25 },
    { "telnet:", proto_DOES_NOT_PARSE, 23 },
    { "finger:", proto_DOES_NOT_PARSE, 79 },
    { "file:", proto_HOST_ALLOW_HASH | proto_HAS_NO_NETLOC, 0 },
    { "filer_", proto_HOST_ALLOW_HASH | proto_HAS_NO_NETLOC, 0 },
    { "local:", proto_HOST_ALLOW_HASH | proto_HAS_NO_NETLOC, 0 },
    { "gopher:", proto_PATH_NOT_UNIX, 70 },
    { "ftp:", proto_HAS_USER, 21 },
    { "http:", proto_STRIP_DOT_DOT, 80 },
    { "https:", proto_STRIP_DOT_DOT, 443 },
    { "whois:", proto_DOES_NOT_PARSE, 43 },
    { "ldap:", proto_PATH_NOT_UNIX, 389 },
    { "data:", proto_DOES_NOT_PARSE, 0 },
    { NULL, 0, 0 },
};

static int default_entry(const char *url)
{
    const char *colon = strchr(url, ':');
    if (!colon)
        colon = strchr(url, '\0') - 1;
    size_t length = (size_t)(colon + 1 - url);
    if (length < 2)
        return -1;
    int i;
    for (i = 0; defaults[i].scheme; i++)
        if (!strncmp(defaults[i].scheme, url, length))
            return i;
    return i;                                       /* the terminator: flags 0, port 0 */
}

static const struct protocol *locate(const char *url)
{
    size_t lenurl = strlen(url);
    for (const struct protocol *p = protocols; p; p = p->next) {
        const char *scheme = ros_ptr(p->scheme);
        size_t protolen = strlen(scheme);
        if (protolen == lenurl + 1)
            protolen--;
        if (!strncasecmp(scheme, url, protolen))
            return p;
    }
    return NULL;
}

int protocol_get_flags(const char *url)
{
    const struct protocol *p = locate(url);
    if (p)
        return p->flags;
    int i = default_entry(url);
    return i < 0 ? 0 : defaults[i].flags;
}

unsigned int protocol_get_default_port(const char *url)
{
    const struct protocol *p = locate(url);
    if (p && p->port)
        return p->port;
    int i = default_entry(url);
    return i < 0 ? 0 : defaults[i].port;
}

static void protocol_service(uint32_t reason, const struct protocol *p)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = reason, s.r[1] = SERVICE_URLMODULE_PROTOCOL;
    s.r[2] = p->scheme, s.r[3] = p->swi_base, s.r[4] = p->info;
    ros_service_call(&s);
}

/* ---- sessions and proxies -------------------------------------------------------- */

struct proxy {
    uint32_t base;                  /* the URL (or scheme) to proxy, in the RMA */
    uint32_t proxy;                 /* where to, in the RMA. "" means not proxied */
    struct proxy *next;
};

struct client {
    uint32_t id;                    /* its poll word's address */
    uint32_t protocol;              /* the fetch's SWI base, or 0 */
    struct proxy *proxy, *no_proxy;
    struct client *next;
};

static struct client *clients;
static struct proxy *global_proxy, *global_no_proxy;
static int starting;

static struct client *find_client(uint32_t id)
{
    for (struct client *c = clients; c; c = c->next)
        if (c->id == id)
            return c;
    return NULL;
}

static void free_proxies(struct proxy *p)
{
    while (p) {
        struct proxy *next = p->next;
        rma_free(p->base);
        rma_free(p->proxy);
        free(p);
        p = next;
    }
}

/* The proxy for a request from a list: 1 and *to if one matches */
static int proxied(const char *request, const struct proxy *p, const char **to)
{
    for (; p; p = p->next) {
        const char *base = ros_ptr(p->base);
        if (!strncmp(request, base, strlen(base))) {
            *to = ros_ptr(p->proxy);
            return 1;
        }
    }
    return 0;
}

/* A protocol module's SWI with the client's registers, which come back */
static os_error *call_protocol(struct ros_cpu *s, uint32_t swi)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    for (int i = 0; i < 10; i++)
        c.r[i] = s->r[i];
    ros_swi(&c, ROS_X_BIT | swi);
    if (c.v)
        return ros_ptr(c.r[0]);
    for (int i = 0; i < 10; i++)
        s->r[i] = c.r[i];
    return NULL;
}

#define DONE(e)                  \
    do {                         \
        os_error *e_ = (e);      \
        if (e_)                  \
            ros_swi_fail(s, e_); \
        else                     \
            s->v = 0;            \
        return;                  \
    } while (0)

void ros_thunk_URL_Register(struct ros_cpu *s)
{
    struct client *c = calloc(1, sizeof *c);
    uint32_t *word = c ? ros_rma_alloc(4) : NULL;
    if (!word) {
        free(c);
        DONE(make_error(url_ERROR_MEMORY_EXHAUSTED, 0));
    }
    *word = 0;
    c->id = ros_addr(word);
    struct client **end = &clients;                 /* at the end, as the original */
    while (*end)
        end = &(*end)->next;
    *end = c;
    s->r[0] = 0;
    s->r[1] = c->id;
    DONE(NULL);
}

void ros_thunk_URL_GetURL(struct ros_cpu *s)
{
    struct client *c = find_client(s->r[1]);
    if (!c)
        DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
    if (c->protocol)
        DONE(make_error(url_ERROR_ALREADY_CONNECTED, 0));
    char *request = URL_canonicalise(parseurlflags_APPLY_HEX_ENCODE, ros_ptr(s->r[3]));
    if (!request)
        DONE(make_error(url_ERROR_NO_PARSE_URL, 0));
    const char *to = NULL;
    int done = proxied(request, c->no_proxy, &to) || proxied(request, c->proxy, &to) ||
               proxied(request, global_no_proxy, &to) || proxied(request, global_proxy, &to);
    if (done && to && !*to)
        to = NULL;                                  /* a no-proxy entry */
    const struct protocol *p = locate(to ? to : request);
    if (!p) {
        free(request);
        DONE(make_error(url_ERROR_NO_FETCHER_SERVICE, 0));
    }
    uint32_t client_r3 = s->r[3];
    uint32_t canonical = rma_string(request), proxy = to ? rma_string(to) : 0;
    free(request);
    if (!canonical || (to && !proxy)) {
        rma_free(canonical);
        DONE(make_error(url_ERROR_MEMORY_EXHAUSTED, 0));
    }
    c->protocol = p->swi_base;
    s->r[3] = canonical;
    if (proxy)
        s->r[7] = proxy, s->r[0] |= 1u << 31;
    os_error *e = call_protocol(s, p->swi_base + 0);
    if (e)
        c->protocol = 0;
    else if (proxy)
        s->r[0] &= ~(1u << 31);
    s->r[3] = client_r3;
    rma_free(canonical);
    rma_free(proxy);
    DONE(e);
}

void ros_thunk_URL_Status(struct ros_cpu *s)
{
    struct client *c = find_client(s->r[1]);
    if (!c)
        DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
    if (!c->protocol) {
        s->r[0] = s->r[1] = s->r[2] = 0;
        DONE(NULL);
    }
    DONE(call_protocol(s, c->protocol + 1));
}

static os_error *not_in_progress(uint32_t id)
{
    os_error *e = make_error(url_ERROR_NOT_IN_PROGRESS, 0);
    size_t n = strlen(e->errmess);
    snprintf(e->errmess + n, sizeof e->errmess - n, " (%08x)", id);
    return e;
}

void ros_thunk_URL_ReadData(struct ros_cpu *s)
{
    struct client *c = find_client(s->r[1]);
    if (!c)
        DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
    if (!c->protocol)
        DONE(not_in_progress(s->r[1]));
    DONE(call_protocol(s, c->protocol + 2));
}

static os_error *stop(struct ros_cpu *s, struct client *c)
{
    if (!c->protocol)
        return not_in_progress(s->r[1]);
    call_protocol(s, c->protocol + 3);              /* its error is not the client's */
    c->protocol = 0;
    return NULL;
}

void ros_thunk_URL_Stop(struct ros_cpu *s)
{
    struct client *c = find_client(s->r[1]);
    if (!c)
        DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
    DONE(stop(s, c));
}

void ros_thunk_URL_Deregister(struct ros_cpu *s)
{
    struct client *c = find_client(s->r[1]);
    if (!c)
        DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
    if (c->protocol)
        stop(s, c);
    for (struct client **p = &clients; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    free_proxies(c->proxy);
    free_proxies(c->no_proxy);
    rma_free(c->id);
    free(c);
    DONE(NULL);
}

static struct proxy **proxy_list(uint32_t session, int no_proxy)
{
    if (session) {
        struct client *c = find_client(session);
        if (!c)
            return NULL;
        return no_proxy ? &c->no_proxy : &c->proxy;
    }
    return no_proxy ? &global_no_proxy : &global_proxy;
}

void ros_thunk_URL_SetProxy(struct ros_cpu *s)
{
    if (!s->r[2]) {                                 /* R2 = 0: clear the lists */
        if (s->r[1]) {
            struct client *c = find_client(s->r[1]);
            if (!c)
                DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
            free_proxies(c->proxy), free_proxies(c->no_proxy);
            c->proxy = c->no_proxy = NULL;
        } else {
            free_proxies(global_proxy), free_proxies(global_no_proxy);
            global_proxy = global_no_proxy = NULL;
        }
        DONE(NULL);
    }
    struct proxy **list = proxy_list(s->r[1], s->r[4] == 1);
    if (!list)
        DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
    const char *base = s->r[3] ? ros_ptr(s->r[3]) : "";
    struct proxy **p = list;
    while (*p) {                                    /* a duplicate goes */
        if (!strcmp(ros_ptr((*p)->base), base)) {
            struct proxy *gone = *p;
            *p = gone->next;
            gone->next = NULL;
            free_proxies(gone);
        } else {
            p = &(*p)->next;
        }
    }
    struct proxy *n = calloc(1, sizeof *n);
    if (n) {
        n->proxy = rma_string(ros_ptr(s->r[2]));
        n->base = rma_string(base);
    }
    if (!n || !n->proxy || !n->base) {
        free_proxies(n);
        DONE(make_error(url_ERROR_MEMORY_EXHAUSTED, 0));
    }
    *p = n;                                         /* at the end */
    DONE(NULL);
}

void ros_thunk_URL_EnumerateProxies(struct ros_cpu *s)
{
    struct proxy **list = proxy_list(s->r[1], s->r[0] & 1);
    if (!list)
        DONE(make_error(url_ERROR_CLIENT_ID_NOT_FOUND, 0));
    int32_t ctx = (int32_t)s->r[2];
    struct proxy *p = *list;
    for (int32_t i = 0; ctx >= 0 && i < ctx && p; i++)
        p = p->next;
    if (!p || ctx < 0) {
        s->r[2] = (uint32_t)-1;
    } else {
        s->r[3] = p->base;
        s->r[4] = p->proxy;
        s->r[2] = (uint32_t)ctx + 1;
    }
    DONE(NULL);
}

void ros_thunk_URL_ParseURL(struct ros_cpu *s)
{
    uint32_t r[6] = { s->r[0], s->r[1], s->r[2], s->r[3], s->r[4], s->r[5] };
    os_error *e = parse_url(r);
    if (!e)
        s->r[0] = r[0], s->r[5] = r[5];
    DONE(e);
}

void ros_thunk_URL_EnumerateSchemes(struct ros_cpu *s)
{
    int32_t ctx = (int32_t)s->r[1];
    const struct protocol *p = protocols;
    for (int32_t i = 0; ctx >= 0 && i < ctx && p; i++)
        p = p->next;
    if (ctx < 0 || !p) {
        s->r[1] = (uint32_t)-1;
    } else {
        s->r[1] = (uint32_t)ctx + 1;
        s->r[2] = p->scheme, s->r[3] = p->info, s->r[4] = p->swi_base, s->r[5] = p->version;
    }
    s->r[0] = 0;
    DONE(NULL);
}

void ros_thunk_URL_ProtocolRegister(struct ros_cpu *s)
{
    char scheme[256];
    snprintf(scheme, sizeof scheme, "%s", (const char *)ros_ptr(s->r[2]));
    for (char *c = scheme; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    if (locate(scheme))
        DONE(make_error(url_ERROR_PROTOCOL_EXISTS, 0));
    struct protocol *p = calloc(1, sizeof *p);
    char info[51] = "";
    if (s->r[4])
        snprintf(info, sizeof info, "%s", (const char *)ros_ptr(s->r[4]));
    if (p) {
        p->scheme = rma_string(scheme);
        p->info = rma_string(info);
    }
    if (!p || !p->scheme || !p->info) {
        if (p)
            rma_free(p->scheme), rma_free(p->info);
        free(p);
        DONE(make_error(url_ERROR_MEMORY_EXHAUSTED, 0));
    }
    p->swi_base = s->r[1];
    p->version = s->r[3];
    int i = default_entry(scheme);
    p->flags = (s->r[0] & 1) ? (int)s->r[5] : i < 0 ? 0 : defaults[i].flags;
    p->port = (s->r[0] & 2) ? s->r[6] : i < 0 ? 0 : defaults[i].port;
    p->next = protocols;                            /* at the front, as the original */
    protocols = p;
    protocol_service(0, p);                         /* URLProtocolModuleStarted */
    s->r[0] = 0;
    DONE(NULL);
}

void ros_thunk_URL_ProtocolDeregister(struct ros_cpu *s)
{
    uint32_t base = s->r[1];
    const char *scheme = (s->r[0] & 1) && s->r[2] ? ros_ptr(s->r[2]) : NULL;
    for (struct protocol **pp = &protocols; *pp;) {
        struct protocol *p = *pp;
        if (p->swi_base != base || (scheme && strcasecmp(ros_ptr(p->scheme), scheme))) {
            pp = &p->next;
            continue;
        }
        *pp = p->next;
        protocol_service(1, p);                     /* URLProtocolModuleDying */
        rma_free(p->scheme), rma_free(p->info);
        free(p);
    }
    uint32_t users = 0;
    for (struct client *c = clients; c; c = c->next)
        users += c->protocol == base;
    s->r[0] = 0;
    s->r[1] = users;
    DONE(NULL);
}

/* ---- *URLProtoShow ----------------------------------------------------------------- */

static os_error *print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static os_error *print(const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    for (const char *p = text; *p; p++) {
        os_error *e = *p == '\n' ? xos_new_line() : xos_write_c((uint8_t)*p);
        if (e)
            return e;
    }
    return NULL;
}

static os_error *urlprotoshow(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    if (!protocols)
        return print("No URL style protocol modules are registered\n");
    os_error *e = print("Base URL    SwiBase  Version  Comment\n");
    for (int i = 0; i < 8 && !e; i++)
        e = print("==========");
    if (!e)
        e = print("\n%-10s  0x%5x    %03d    %s\n", " --- ", URL_CHUNK, VERSION,
                  "URL 0.58 (26 Sep 2026) ROSGD native");
    for (const struct protocol *p = protocols; p && !e; p = p->next)
        e = print("%-10s  0x%5x    %03u    %s\n", (const char *)ros_ptr(p->scheme), p->swi_base, p->version,
                  (const char *)ros_ptr(p->info));
    return e;
}

static const struct ros_command commands[] = {
    { "URLProtoShow", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *URLProtoShow",
      "*URLProtoShow shows all the current protocols known and their SWI bases.", urlprotoshow },
    { 0 },
};

/* ---- the module ------------------------------------------------------------------ */

static void announce(uint32_t reason)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = reason, s.r[1] = SERVICE_URLMODULE, s.r[2] = VERSION;
    ros_service_call(&s);
}

static void started(void *arg)
{
    (void)arg;
    if (starting) {
        starting = 0;
        announce(0);                                /* URLModuleStarted: protocols, register */
    }
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    starting = 1;
    ros_callback_add_native(started, NULL);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    while (clients) {
        struct client *c = clients;
        clients = c->next;
        if (c->protocol) {
            struct ros_cpu s;
            ros_cpu_enter(&s);
            s.r[0] = 0, s.r[1] = c->id;
            call_protocol(&s, c->protocol + 3);
        }
        free_proxies(c->proxy), free_proxies(c->no_proxy);
        rma_free(c->id);
        free(c);
    }
    free_proxies(global_proxy), free_proxies(global_no_proxy);
    global_proxy = global_no_proxy = NULL;
    while (protocols) {
        struct protocol *p = protocols;
        protocols = p->next;
        rma_free(p->scheme), rma_free(p->info);
        free(p);
    }
    starting = 0;
    announce(1);                                    /* URLModuleDying */
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module urlfetcher_module = {
    .title = "URL_Fetcher",
    .help = "URL\t0.58 (26 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .commands = commands,
    .swi_chunk = URL_CHUNK,
    .swi_thunks = ros_swi_thunks_URL_Fetcher,
    .swi_names = ros_swi_names_URL_Fetcher,
    .swi_prefix = "URL",
};

__attribute__((constructor)) static void count(void)
{
    urlfetcher_module.swi_count = ros_swi_count_URL_Fetcher;
}
