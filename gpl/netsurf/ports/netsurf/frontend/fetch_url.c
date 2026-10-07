/* fetch_url.c -- NetSurf's http: and https: through RISC OS's URL_Fetcher
 * and AcornHTTP (design 22, section 11).  ROSGD's own; MIT licence.
 *
 * NetSurf's own fetcher for these is libcurl's multi interface, compiled in,
 * which moves only while NetSurf has control.  A Wimp task is paged out
 * between polls, so here the transfers are the system's: URL_GetURL starts
 * one in AcornHTTP, whose libcurl runs on a thread of its own, and NetSurf's
 * poll of its fetchers (every 10 ms while any is active) takes what has
 * arrived with URL_ReadData.  NetSurf has no curl, no OpenSSL and no
 * sockets.
 *
 * The request is the one NetSurf's curl fetcher makes:
 *   - GET, or POST for a form, url-encoded or multipart;
 *   - NetSurf's headers (Referer, the cache's conditions), and
 *     Accept-Language, Accept-Charset and DNT from its options;
 *   - its cookies and its credentials (Basic) from urldb, read when the
 *     fetch starts;
 *   - its User-Agent, in R6.
 * AcornHTTP's own cookie jar is left out of the request (R0 bit 30):
 * NetSurf's is the one that counts, as it is with curl.
 *
 * The response comes as AcornHTTP shapes it: the head as HTTP/1.0, then the
 * body (R2 bits 8-15 = 2, "both").  Each line of the head goes to NetSurf
 * as curl's header callback gives it, the status line first; Set-Cookie
 * also goes to NetSurf's jar.  Then NetSurf's rules for 304, 3xx, 401 and
 * only_2xx, as its curl fetcher applies them, and the body as data.
 *
 * NetSurf may abort a fetch from inside one of its callbacks.  While this
 * fetcher is calling out, an abort only marks the fetch; the fetcher
 * finishes it, and frees it, when the callback has returned.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <kernel.h>
#include <swis.h>
#include <libwapcaplet/libwapcaplet.h>
#include <nsutils/time.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/string.h"
#include "utils/useragent.h"
#include "netsurf/fetch.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"
#include "desktop/gui_internal.h"

#include "rosgd/fetch_url.h"

/* URL_GetURL (AcornHTTP's Docs/SWIs) */
#define METHOD_GET      1u
#define METHOD_POST     4u
#define WANT_BOTH       (2u << 8)       /* R2 bits 8-15: the head, then the body */
#define F_AGENT_IN_R6   1u
#define F_LENGTH_IN_R5  2u              /* R4's length in R5; what is wanted in R2 */
#define F_NO_COOKIES    (1u << 30)      /* NetSurf's jar, not AcornHTTP's */
/* AcornHTTP's errors that mean a timeout */
#define ERR_CONNECT_TIMEOUT 0x80DE21
#define ERR_READ_TIMEOUT    0x80DE22

#define READ_CHUNK      (64 * 1024)     /* a URL_ReadData */
#define POLL_BUDGET     (1024 * 1024)   /* a fetch's bytes a poll, so the desktop keeps moving */
#define HEAD_MAX        (256 * 1024)    /* a response head bigger than this is refused */
#define PROGRESS_MS     500

struct buf {
    char *p;
    size_t len, cap;
    bool failed;
};

struct url_fetch {
    struct fetch *fetch;            /* NetSurf's, which owns this */
    nsurl *url;
    lwc_string *scheme;
    bool only_2xx;
    struct buf headers;             /* NetSurf's, and those from its options */
    struct buf body;                /* a POST's */
    char *content_type;             /* the body's */
    bool post;
    struct buf request;             /* headers, a blank line, the body: GetURL's R4 */
    uint32_t session;               /* URL_Register's; 0 when none */
    bool started, aborted, finished;
    char error[256];                /* a start that failed, reported at the next poll */
    struct buf head;                /* the response head, until its blank line */
    bool head_done;
    long code;
    char *location, *realm;
    unsigned long long received, length;    /* the body's; length 0 if not known */
    uint64_t last_progress;
    struct url_fetch *next;
};

static struct url_fetch *fetches;   /* every fetch set up and not yet freed */
static bool busy;                   /* calling out to NetSurf: aborts wait */
static char *chunk;                 /* URL_ReadData's buffer */
static int users;                   /* schemes initialised */

/* ---- buffers ------------------------------------------------------------- */

static void put(struct buf *b, const void *p, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *grown = realloc(b->p, cap);
        if (!grown) {
            b->failed = true;
            return;
        }
        b->p = grown, b->cap = cap;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void puts_(struct buf *b, const char *s)
{
    put(b, s, strlen(s));
}

static void putf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(struct buf *b, const char *fmt, ...)
{
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->failed = true;
        return;
    }
    if ((size_t)n < sizeof small) {
        put(b, small, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big) {
        b->failed = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    put(b, big, (size_t)n);
    free(big);
}

static void drop(struct buf *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* ---- the request ------------------------------------------------------------ */

static void base64(struct buf *b, const char *s)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = (const unsigned char *)s;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        char q[4] = { digits[v >> 18 & 63], digits[v >> 12 & 63], i + 1 < n ? digits[v >> 6 & 63] : '=',
                      i + 2 < n ? digits[v & 63] : '=' };
        put(b, q, 4);
    }
}

/* A multipart field's name or file name, quoted as HTML's form submission
 * quotes it */
static void quoted(struct buf *b, const char *s)
{
    put(b, "\"", 1);
    for (; s && *s; s++)
        if (*s == '"')
            puts_(b, "%22");
        else if (*s == '\r')
            puts_(b, "%0D");
        else if (*s == '\n')
            puts_(b, "%0A");
        else
            put(b, s, 1);
    put(b, "\"", 1);
}

static bool multipart(struct url_fetch *f, const struct fetch_multipart_data *m)
{
    char boundary[64];
    uint64_t now = 0;
    nsu_getmonotonic_ms(&now);
    snprintf(boundary, sizeof boundary, "----NetSurfFormBoundary%08lx%08llx", (unsigned long)(uintptr_t)f,
             (unsigned long long)now);
    for (; m; m = m->next) {
        putf(&f->body, "--%s\r\nContent-Disposition: form-data; name=", boundary);
        quoted(&f->body, m->name);
        if (m->file) {
            const char *type = "application/octet-stream";
            if (m->rawfile && *m->rawfile && guit->fetch->filetype)
                type = guit->fetch->filetype(m->rawfile);
            puts_(&f->body, "; filename=");
            quoted(&f->body, m->value);
            putf(&f->body, "\r\nContent-Type: %s\r\n\r\n", type);
            FILE *in = m->rawfile && *m->rawfile ? fopen(m->rawfile, "rb") : NULL;
            if (in) {
                char block[8192];
                size_t n;
                while ((n = fread(block, 1, sizeof block, in)) > 0)
                    put(&f->body, block, n);
                fclose(in);
            }
        } else {
            puts_(&f->body, "\r\n\r\n");
            puts_(&f->body, m->value ? m->value : "");
        }
        puts_(&f->body, "\r\n");
    }
    putf(&f->body, "--%s--\r\n", boundary);
    size_t n = strlen("multipart/form-data; boundary=") + strlen(boundary) + 1;
    f->content_type = malloc(n);
    if (!f->content_type)
        return false;
    snprintf(f->content_type, n, "multipart/form-data; boundary=%s", boundary);
    return !f->body.failed;
}

static void fu_free(void *vf);

static void *fu_setup(struct fetch *parent, nsurl *url, bool only_2xx, bool downgrade_tls, const char *post_urlenc,
                      const struct fetch_multipart_data *post_multipart, const char **headers)
{
    (void)downgrade_tls;                /* TLS is AcornHTTP's, and the system's */
    struct url_fetch *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->fetch = parent;
    f->url = nsurl_ref(url);
    f->scheme = nsurl_get_component(url, NSURL_SCHEME);
    f->only_2xx = only_2xx;
    f->next = fetches;
    fetches = f;
    if (!f->scheme)
        goto failed;

    const char *lang = nsoption_charp(accept_language), *charset = nsoption_charp(accept_charset);
    if (lang && *lang)
        putf(&f->headers, "Accept-Language: %s, *;q=0.1\r\n", lang);
    if (charset && *charset)
        putf(&f->headers, "Accept-Charset: %s, *;q=0.1\r\n", charset);
    if (nsoption_bool(do_not_track))
        puts_(&f->headers, "DNT: 1\r\n");
    for (int i = 0; headers[i]; i++)
        putf(&f->headers, "%s\r\n", headers[i]);

    if (post_urlenc) {
        f->post = true;
        puts_(&f->body, post_urlenc);
        f->content_type = strdup("application/x-www-form-urlencoded");
        if (!f->content_type)
            goto failed;
    } else if (post_multipart) {
        f->post = true;
        if (!multipart(f, post_multipart))
            goto failed;
    }
    if (f->headers.failed || f->body.failed)
        goto failed;
    NSLOG(netsurf, INFO, "fetch %p, url '%s'", f, nsurl_access(url));
    return f;

failed:
    fu_free(f);
    return NULL;
}

/* The request as the fetch starts: NetSurf's cookies and credentials now,
 * not when it was queued */
static bool compose(struct url_fetch *f)
{
    struct buf *r = &f->request;
    drop(r);
    put(r, f->headers.p ? f->headers.p : "", f->headers.len);
    char *cookie = urldb_get_cookie(f->url, true);
    if (cookie) {
        putf(r, "Cookie: %s\r\n", cookie);
        free(cookie);
    }
    const char *auth = urldb_get_auth_details(f->url, NULL);
    if (auth) {
        puts_(r, "Authorization: Basic ");
        base64(r, auth);
        puts_(r, "\r\n");
    }
    if (f->post) {
        putf(r, "Content-Type: %s\r\nContent-Length: %zu\r\n", f->content_type, f->body.len);
        puts_(r, "\r\n");
        put(r, f->body.p ? f->body.p : "", f->body.len);
    } else {
        puts_(r, "\r\n");
    }
    return !r->failed;
}

static void fail_start(struct url_fetch *f, const char *why)
{
    snprintf(f->error, sizeof f->error, "%s", why);
}

static bool fu_start(void *vf)
{
    struct url_fetch *f = vf;
    f->started = true;
    if (!compose(f)) {
        fail_start(f, messages_get("NoMemory"));
        return true;
    }
    _kernel_oserror *e = _swix(URL_Register, _IN(0) | _OUT(1), 0, &f->session);
    if (e) {
        f->session = 0;
        fail_start(f, e->errmess);
        return true;
    }
    e = _swix(URL_GetURL, _INR(0, 6), F_AGENT_IN_R6 | F_LENGTH_IN_R5 | F_NO_COOKIES, f->session,
              (f->post ? METHOD_POST : METHOD_GET) | WANT_BOTH, nsurl_access(f->url), f->request.p,
              (unsigned)f->request.len, user_agent_string());
    if (e)
        fail_start(f, e->errmess);
    return true;
}

/* ---- the response ------------------------------------------------------------ */

static void tell(struct url_fetch *f, fetch_msg *msg)
{
    fetch_send_callback(msg, f->fetch);
}

/* The end of a fetch: a last message, then the fetcher lets it go */
static void finish(struct url_fetch *f, fetch_msg_type type, const char *text)
{
    fetch_msg msg = { .type = type };
    switch (type) {
    case FETCH_REDIRECT: msg.data.redirect = text; break;
    case FETCH_AUTH: msg.data.auth.realm = text; break;
    case FETCH_ERROR:
    case FETCH_TIMEDOUT: msg.data.error = text; break;
    default: break;
    }
    f->finished = true;
    if (!f->aborted)
        tell(f, &msg);
}

static void progress(struct url_fetch *f)
{
    uint64_t now = 0;
    nsu_getmonotonic_ms(&now);
    if (now - f->last_progress < PROGRESS_MS)
        return;
    f->last_progress = now;
    char text[256];
    if (f->length)
        snprintf(text, sizeof text, messages_get("Progress"), human_friendly_bytesize(f->received),
                 human_friendly_bytesize(f->length));
    else
        snprintf(text, sizeof text, messages_get("ProgressU"), human_friendly_bytesize(f->received));
    fetch_msg msg = { .type = FETCH_PROGRESS, .data.progress = text };
    tell(f, &msg);
}

static void data(struct url_fetch *f, const char *p, size_t n)
{
    if (!n || f->aborted || f->finished)
        return;
    f->received += n;
    fetch_msg msg = { .type = FETCH_DATA, .data.header_or_data = { (const uint8_t *)p, n } };
    tell(f, &msg);
    if (!f->aborted)
        progress(f);
}

static char *value_of(const char *line, size_t name_len)
{
    const char *v = line + name_len;
    v += strspn(v, " \t");
    size_t n = strcspn(v, "\r\n");
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
        n--;
    return strndup(v, n);
}

/* The head, [0, n) of f->head: each line to NetSurf, then its rules for the
 * status */
static void head(struct url_fetch *f, size_t n)
{
    f->head_done = true;
    char *line = f->head.p, *end = f->head.p + n;
    while (line < end && !f->aborted) {
        char *eol = memchr(line, '\n', (size_t)(end - line));
        eol = eol ? eol + 1 : end;
        size_t len = (size_t)(eol - line);
        char save = *eol;
        *eol = 0;                                   /* NetSurf reads the line as a string */
        fetch_msg msg = { .type = FETCH_HEADER, .data.header_or_data = { (const uint8_t *)line, len } };
        tell(f, &msg);
        if (!strncmp(line, "HTTP/", 5)) {
            const char *sp = strchr(line, ' ');
            f->code = sp ? strtol(sp + 1, NULL, 10) : 0;
        } else if (!strncasecmp(line, "Location:", 9)) {
            free(f->location);
            f->location = value_of(line, 9);
        } else if (!strncasecmp(line, "Content-Length:", 15)) {
            f->length = strtoull(line + 15, NULL, 10);
        } else if (!strncasecmp(line, "WWW-Authenticate:", 17)) {
            const char *r = line + 17;
            while (*r && strncasecmp(r, "realm=\"", 7))
                r++;
            if (*r) {
                r += 7;
                free(f->realm);
                f->realm = strndup(r, strcspn(r, "\"\r\n"));
            }
        } else if (!strncasecmp(line, "Set-Cookie:", 11)) {
            char *cookie = value_of(line, 11);
            if (cookie) {
                fetch_set_cookie(f->fetch, cookie);
                free(cookie);
            }
        }
        *eol = save;
        line = eol;
    }
    if (f->aborted)
        return;
    fetch_set_http_code(f->fetch, f->code);
    NSLOG(netsurf, INFO, "fetch %p: HTTP status %ld", f, f->code);
    if (f->code == 304 && !f->post)
        finish(f, FETCH_NOTMODIFIED, NULL);
    else if (f->code >= 300 && f->code < 400 && f->location)
        finish(f, FETCH_REDIRECT, f->location);
    else if (f->code == 401)
        finish(f, FETCH_AUTH, f->realm);
    else if (f->only_2xx && (f->code < 200 || f->code >= 300))
        finish(f, FETCH_ERROR, messages_get("Not2xx"));
}

/* What URL_ReadData gave: the head until its blank line, then the body */
static void arrived(struct url_fetch *f, const char *p, size_t n)
{
    if (f->head_done) {
        data(f, p, n);
        return;
    }
    size_t before = f->head.len;
    put(&f->head, p, n);
    if (f->head.failed || f->head.len > HEAD_MAX) {
        finish(f, FETCH_ERROR, messages_get("NoMemory"));
        return;
    }
    /* the blank line may straddle two reads */
    size_t from = before > 3 ? before - 3 : 0;
    char *blank = NULL;
    for (size_t i = from; i + 4 <= f->head.len; i++)
        if (!memcmp(f->head.p + i, "\r\n\r\n", 4)) {
            blank = f->head.p + i;
            break;
        }
    if (!blank)
        return;
    size_t head_len = (size_t)(blank + 4 - f->head.p);
    head(f, head_len);
    if (!f->aborted && !f->finished)
        data(f, f->head.p + head_len, f->head.len - head_len);
    drop(&f->head);
}

/* All of it has come */
static void complete(struct url_fetch *f)
{
    if (!f->head_done) {
        /* No head: an HTTP/0.9 answer, all body -- or a head cut short */
        if (f->head.len >= 5 && !memcmp(f->head.p, "HTTP/", 5)) {
            finish(f, FETCH_ERROR, messages_get("FetchFailedToFinish"));
            return;
        }
        f->head_done = true;
        f->code = 200;
        fetch_set_http_code(f->fetch, 200);
        data(f, f->head.p, f->head.len);
        drop(&f->head);
        if (f->aborted)
            return;
    }
    if (!f->finished)
        finish(f, FETCH_FINISHED, NULL);
}

static void failed(struct url_fetch *f, const _kernel_oserror *e)
{
    char why[256];
    snprintf(why, sizeof why, "%s", e->errmess);    /* the OS's buffer is not the fetch's */
    bool timeout = e->errnum == ERR_CONNECT_TIMEOUT || e->errnum == ERR_READ_TIMEOUT;
    NSLOG(netsurf, INFO, "fetch %p: &%X %s", f, e->errnum, why);
    finish(f, timeout ? FETCH_TIMEDOUT : FETCH_ERROR, why);
}

/* One fetch's turn: read what has arrived, up to the budget */
static void advance(struct url_fetch *f)
{
    if (f->error[0]) {
        finish(f, FETCH_ERROR, f->error);
        return;
    }
    size_t budget = POLL_BUDGET;
    while (budget && !f->aborted && !f->finished) {
        unsigned status = 0, got = 0, left = 0;
        _kernel_oserror *e = _swix(URL_ReadData, _INR(0, 3) | _OUT(0) | _OUTR(4, 5), 0, f->session, chunk,
                                   READ_CHUNK, &status, &got, &left);
        if (e) {
            failed(f, e);
            return;
        }
        if (got > READ_CHUNK)
            got = READ_CHUNK;
        arrived(f, chunk, got);
        if (f->aborted || f->finished)
            return;
        if (left == 0) {                /* R5 0: the end; -1 or more: not yet */
            complete(f);
            return;
        }
        if (!got)
            return;
        budget = got < budget ? budget - got : 0;
    }
}

/* ---- the fetches' ends ----------------------------------------------------------- */

static void stop(struct url_fetch *f)
{
    if (!f->session)
        return;
    _swix(URL_Stop, _INR(0, 1), 0, f->session);
    _swix(URL_Deregister, _INR(0, 1), 0, f->session);
    f->session = 0;
}

/* Every fetch aborted or finished: its transfer stopped, and NetSurf's
 * fetch freed (which frees this fetcher's).  Freeing calls NetSurf back,
 * which may abort or start others: each round starts from the head again. */
static void reap(void)
{
    bool was = busy;
    busy = true;
    for (;;) {
        struct url_fetch **p = &fetches, *f;
        while ((f = *p) && !f->aborted && !f->finished)
            p = &f->next;
        if (!f)
            break;
        *p = f->next;
        f->next = NULL;
        stop(f);
        fetch_remove_from_queues(f->fetch);
        fetch_free(f->fetch);
    }
    busy = was;
}

static void fu_abort(void *vf)
{
    struct url_fetch *f = vf;
    NSLOG(netsurf, INFO, "fetch %p, url '%s'", f, nsurl_access(f->url));
    f->aborted = true;
    if (!busy)
        reap();
}

static void fu_free(void *vf)
{
    struct url_fetch *f = vf;
    for (struct url_fetch **p = &fetches; *p; p = &(*p)->next)
        if (*p == f) {
            *p = f->next;
            break;
        }
    stop(f);
    if (f->url)
        nsurl_unref(f->url);
    if (f->scheme)
        lwc_string_unref(f->scheme);
    drop(&f->headers);
    drop(&f->body);
    drop(&f->request);
    drop(&f->head);
    free(f->content_type);
    free(f->location);
    free(f->realm);
    free(f);
}

static void fu_poll(lwc_string *scheme)
{
    busy = true;
    for (struct url_fetch *f = fetches; f; f = f->next) {
        bool match = false;
        if (!f->started || f->aborted || f->finished ||
            lwc_string_isequal(f->scheme, scheme, &match) != lwc_error_ok || !match)
            continue;
        advance(f);
    }
    busy = false;
    reap();
}

/* ---- the fetcher ------------------------------------------------------------------- */

static bool fu_initialise(lwc_string *scheme)
{
    (void)scheme;
    if (!chunk && !(chunk = malloc(READ_CHUNK)))
        return false;
    users++;
    return true;
}

static void fu_finalise(lwc_string *scheme)
{
    (void)scheme;
    if (--users == 0) {
        free(chunk);
        chunk = NULL;
    }
}

static bool fu_acceptable(const nsurl *url)
{
    (void)url;
    return true;
}

nserror fetch_url_register(void)
{
    const struct fetcher_operation_table ops = {
        .initialise = fu_initialise,
        .acceptable = fu_acceptable,
        .setup = fu_setup,
        .start = fu_start,
        .abort = fu_abort,
        .free = fu_free,
        .poll = fu_poll,
        .fdset = NULL,                  /* nothing to wait on: AcornHTTP's thread has the sockets */
        .finalise = fu_finalise,
    };
    static const char *const schemes[] = { "http", "https" };
    for (size_t i = 0; i < sizeof schemes / sizeof schemes[0]; i++) {
        lwc_string *s;
        if (lwc_intern_string(schemes[i], strlen(schemes[i]), &s) != lwc_error_ok)
            return NSERROR_NOMEM;
        nserror e = fetcher_add(s, &ops);
        if (e != NSERROR_OK)
            return e;
    }
    return NSERROR_OK;
}
