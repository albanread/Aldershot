/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* ipp.c -- the Internet Printing Protocol, enough of it to print.
 *
 * A driverless printer is asked what it takes and then sent it, over HTTP.
 * IPP Everywhere and AirPrint printers are of this kind, which is every
 * printer sold for a decade. There is no driver, no PPD and no CUPS. The
 * kernel gives sockets, the box gives libcurl (modules/acornhttp uses it
 * too), and this file is the rest.
 *
 * IPP (RFC 8010) is a binary message. It has two version octets, the
 * operation, the request id, then groups of attributes, and an end tag.
 * Each attribute is a value tag, the name and the value. A Print-Job
 * carries the document after the end tag, in the same POST. The reply has
 * the same shape, with a status code where the operation was.
 *
 * What the box asks for:
 *   Get-Printer-Attributes  what it is, what it takes, how it is
 *   Print-Job               the job, with its format
 *   Get-Job-Attributes      how a job it took is getting on
 *
 * One thing is worth knowing. IPP Everywhere requires `image/pwg-raster`
 * of every printer and only recommends `application/pdf`. The Brother this
 * was written against takes no PDF at all. So the box reads
 * document-format-supported and rasters the job itself when it must
 * (pdfsvc's "format pwg").
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "rosgd/api.h"
#include "rosgd/error.h"
#include "ipp.h"

/* ---- building a message ------------------------------------------------------- */

struct buf {
    uint8_t *p;
    size_t n, cap;
    int bad;
};

static void put(struct buf *b, const void *data, size_t n)
{
    if (b->bad)
        return;
    if (b->n + n > b->cap) {
        size_t cap = (b->n + n) * 2 + 256;
        uint8_t *p = realloc(b->p, cap);
        if (!p) {
            b->bad = 1;
            return;
        }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void put8(struct buf *b, uint8_t v) { put(b, &v, 1); }

static void put16(struct buf *b, uint16_t v)
{
    uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(b, x, 2);
}

static void put32(struct buf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(b, x, 4);
}

/* One attribute: its tag, its name, its text */
static void attr(struct buf *b, uint8_t tag, const char *name, const char *value)
{
    put8(b, tag);
    put16(b, (uint16_t)strlen(name));
    put(b, name, strlen(name));
    put16(b, (uint16_t)strlen(value));
    put(b, value, strlen(value));
}

static void attr_int(struct buf *b, const char *name, uint32_t value)
{
    put8(b, IPP_TAG_INTEGER);
    put16(b, (uint16_t)strlen(name));
    put(b, name, strlen(name));
    put16(b, 4);
    put32(b, value);
}

/* The head of a request: version, operation, id, and the three operation
 * attributes every one must carry, in the order IPP wants them. */
static void request(struct buf *b, uint16_t op, const char *uri, const char *user)
{
    put8(b, 2), put8(b, 0);                     /* IPP/2.0 */
    put16(b, op);
    put32(b, 1);                                /* request id */
    put8(b, IPP_TAG_OPERATION);
    attr(b, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    attr(b, IPP_TAG_LANGUAGE, "attributes-natural-language", "en");
    attr(b, IPP_TAG_URI, "printer-uri", uri);
    if (user)
        attr(b, IPP_TAG_NAME, "requesting-user-name", user);
}

/* ---- reading a reply ----------------------------------------------------------- */

struct reader {
    const uint8_t *p;
    size_t n, at;
};

static int get8(struct reader *r, uint8_t *v)
{
    if (r->at + 1 > r->n)
        return 0;
    *v = r->p[r->at++];
    return 1;
}

static int get16(struct reader *r, uint16_t *v)
{
    if (r->at + 2 > r->n)
        return 0;
    *v = (uint16_t)(r->p[r->at] << 8 | r->p[r->at + 1]);
    r->at += 2;
    return 1;
}

static int get32(struct reader *r, uint32_t *v)
{
    if (r->at + 4 > r->n)
        return 0;
    *v = (uint32_t)r->p[r->at] << 24 | (uint32_t)r->p[r->at + 1] << 16 |
         (uint32_t)r->p[r->at + 2] << 8 | r->p[r->at + 3];
    r->at += 4;
    return 1;
}

/* Walk a reply's attributes, calling back with each one: the name is the
 * last one named (IPP repeats a value with an empty name), the tag and the
 * value as bytes. */
static int walk(const uint8_t *msg, size_t n, uint16_t *status,
                void (*each)(void *, const char *, uint8_t, const uint8_t *, uint16_t),
                void *arg)
{
    struct reader r = { msg, n, 0 };
    uint8_t v1, v2;
    uint32_t id;
    if (!get8(&r, &v1) || !get8(&r, &v2) || !get16(&r, status) || !get32(&r, &id))
        return 0;
    char name[128] = "";
    for (;;) {
        uint8_t tag;
        if (!get8(&r, &tag))
            return 0;
        if (tag == IPP_TAG_END)
            return 1;
        if (tag < 0x10)                         /* a group: the next attribute follows */
            continue;
        uint16_t nlen, vlen;
        if (!get16(&r, &nlen))
            return 0;
        if (nlen) {
            if (r.at + nlen > r.n)
                return 0;
            size_t k = nlen < sizeof name - 1 ? nlen : sizeof name - 1;
            memcpy(name, r.p + r.at, k);
            name[k] = 0;
            r.at += nlen;
        }
        if (!get16(&r, &vlen) || r.at + vlen > r.n)
            return 0;
        each(arg, name, tag, r.p + r.at, vlen);
        r.at += vlen;
    }
}

/* ---- the HTTP of it ------------------------------------------------------------ */

#define MAX_REPLY (8u * 1024u * 1024u)

struct answer {
    uint8_t *p;
    size_t n, cap;
};

static size_t collect(char *data, size_t size, size_t nmemb, void *arg)
{
    struct answer *a = arg;
    size_t n = size * nmemb;
    if (a->n + n > MAX_REPLY)               /* a printer's answer is small; no more than this is kept */
        return 0;
    if (a->n + n > a->cap) {
        size_t cap = (a->n + n) * 2 + 1024;
        uint8_t *p = realloc(a->p, cap);
        if (!p)
            return 0;
        a->p = p, a->cap = cap;
    }
    memcpy(a->p + a->n, data, n);
    a->n += n;
    return n;
}

/* ipp:// is HTTP on 631; ipps:// is HTTPS. */
static void http_of(const char *uri, char *out, size_t max)
{
    if (!strncasecmp(uri, "ipps://", 7))
        snprintf(out, max, "https://%s", uri + 7);
    else if (!strncasecmp(uri, "ipp://", 6))
        snprintf(out, max, "http://%s", uri + 6);
    else
        snprintf(out, max, "%s", uri);
    /* a port is wanted: IPP's is 631, which http:// would otherwise make 80 */
    char *host = strstr(out, "://");
    if (!host)
        return;
    host += 3;
    char *slash = strchr(host, '/');
    char *colon = strchr(host, ':');
    if (!colon || (slash && colon > slash)) {
        char tail[512];
        snprintf(tail, sizeof tail, "%s", slash ? slash : "/ipp/print");
        if (slash)
            *slash = 0;
        size_t n = strlen(out);
        snprintf(out + n, max - n, ":631%s", tail);
    }
}

/* Send one message (and, for Print-Job, the document after it) and read the
 * reply.  The reply's bytes are the caller's to free. */
static os_error *exchange(const char *uri, const struct buf *msg, const void *doc, size_t doclen,
                          uint8_t **reply, size_t *reply_len, unsigned seconds)
{
    char url[1024];
    http_of(uri, url, sizeof url);
    CURL *c = curl_easy_init();
    if (!c)
        return ros_error(IPP_ERR, "The printer could not be reached");

    uint8_t *body = malloc(msg->n + doclen);
    if (!body) {
        curl_easy_cleanup(c);
        return ros_error(IPP_ERR, "There is not enough memory to print");
    }
    memcpy(body, msg->p, msg->n);
    if (doclen)
        memcpy(body + msg->n, doc, doclen);

    struct answer a = { 0 };
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/ipp");
    h = curl_slist_append(h, "Expect:");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)(msg->n + doclen));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &a);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)seconds);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);    /* printers' certificates are their own */
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    free(body);

    if (rc != CURLE_OK) {
        free(a.p);
        return ros_error(IPP_ERR, "The printer did not answer: %s", curl_easy_strerror(rc));
    }
    if (code != 200) {
        free(a.p);
        return ros_error(IPP_ERR, "The printer answered HTTP %ld", code);
    }
    *reply = a.p, *reply_len = a.n;
    return NULL;
}

/* ---- what a printer says it is ------------------------------------------------- */

struct found {
    struct ipp_printer *out;
};

static void note(void *arg, const char *name, uint8_t tag, const uint8_t *v, uint16_t n)
{
    struct ipp_printer *p = ((struct found *)arg)->out;
    char text[256];
    size_t k = n < sizeof text - 1 ? n : sizeof text - 1;
    memcpy(text, v, k);
    text[k] = 0;
    for (size_t i = 0; i < k; i++)          /* shown on the screen: no VDU codes from the network */
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] == 127)
            text[i] = '?';

    if (!strcmp(name, "document-format-supported")) {
        if (!strcasecmp(text, "application/pdf"))
            p->formats |= IPP_FMT_PDF;
        else if (!strcasecmp(text, "image/pwg-raster"))
            p->formats |= IPP_FMT_PWG;
        else if (!strcasecmp(text, "image/urf"))
            p->formats |= IPP_FMT_URF;
        else if (!strcasecmp(text, "image/jpeg"))
            p->formats |= IPP_FMT_JPEG;
    } else if (!strcmp(name, "printer-make-and-model")) {
        snprintf(p->model, sizeof p->model, "%s", text);
    } else if (!strcmp(name, "printer-state") && tag == IPP_TAG_ENUM && n == 4) {
        p->state = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
    } else if (!strcmp(name, "media-default")) {
        snprintf(p->media, sizeof p->media, "%s", text);
    } else if ((!strcmp(name, "pwg-raster-document-resolution-supported") ||
                !strcmp(name, "printer-resolution-default")) &&
               tag == IPP_TAG_RESOLUTION && n == 9) {
        /* cross feed, feed, then the unit: 3 is dots per inch */
        uint32_t x = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
        if (x >= 72 && x <= 2400 &&         /* the rasterising writer streams a page of this size */
            (!p->dpi || !strcmp(name, "pwg-raster-document-resolution-supported")))
            p->dpi = x;
    } else if (!strcmp(name, "pwg-raster-document-type-supported")) {
        if (!strcasecmp(text, "srgb_8"))
            p->colour |= IPP_COLOUR_SRGB;
        else if (!strcasecmp(text, "sgray_8"))
            p->colour |= IPP_COLOUR_SGRAY;
    }
}

os_error *ipp_ask(const char *uri, struct ipp_printer *out)
{
    memset(out, 0, sizeof *out);
    struct buf b = { 0 };
    request(&b, IPP_OP_GET_PRINTER_ATTRIBUTES, uri, NULL);
    attr(&b, IPP_TAG_KEYWORD, "requested-attributes", "all");
    put8(&b, IPP_TAG_END);
    if (b.bad) {
        free(b.p);
        return ros_error(IPP_ERR, "There is not enough memory to print");
    }
    uint8_t *reply = NULL;
    size_t n = 0;
    os_error *e = exchange(uri, &b, NULL, 0, &reply, &n, 30);
    free(b.p);
    if (e)
        return e;
    uint16_t status = 0;
    struct found f = { out };
    int ok = walk(reply, n, &status, note, &f);
    free(reply);
    if (!ok)
        return ros_error(IPP_ERR, "The printer's answer could not be read");
    if (status >= 0x0100)
        return ros_error(IPP_ERR, "The printer refused the enquiry (IPP status &%X)", status);
    return NULL;
}

/* ---- sending a job ------------------------------------------------------------- */

static void job_note(void *arg, const char *name, uint8_t tag, const uint8_t *v, uint16_t n)
{
    uint32_t *id = arg;
    if (!strcmp(name, "job-id") && tag == IPP_TAG_INTEGER && n == 4)
        *id = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
}

os_error *ipp_print(const char *uri, const char *job_name, const char *format,
                    const void *doc, size_t doclen, uint32_t copies, uint32_t *job_id)
{
    struct buf b = { 0 };
    request(&b, IPP_OP_PRINT_JOB, uri, "ROSGD");
    attr(&b, IPP_TAG_NAME, "job-name", job_name && *job_name ? job_name : "RISC OS print job");
    attr(&b, IPP_TAG_MIMETYPE, "document-format", format);
    put8(&b, IPP_TAG_JOB);
    attr_int(&b, "copies", copies ? copies : 1);
    put8(&b, IPP_TAG_END);
    if (b.bad) {
        free(b.p);
        return ros_error(IPP_ERR, "There is not enough memory to print");
    }
    uint8_t *reply = NULL;
    size_t n = 0;
    os_error *e = exchange(uri, &b, doc, doclen, &reply, &n, 300);
    free(b.p);
    if (e)
        return e;
    uint16_t status = 0;
    uint32_t id = 0;
    int ok = walk(reply, n, &status, job_note, &id);
    free(reply);
    if (!ok)
        return ros_error(IPP_ERR, "The printer's answer could not be read");
    if (status >= 0x0100)
        return ros_error(IPP_ERR, "The printer refused the job (IPP status &%X)", status);
    if (job_id)
        *job_id = id;
    return NULL;
}

const char *ipp_format_name(uint32_t format)
{
    switch (format) {
    case IPP_FMT_PDF: return "application/pdf";
    case IPP_FMT_PWG: return "image/pwg-raster";
    case IPP_FMT_URF: return "image/urf";
    case IPP_FMT_JPEG: return "image/jpeg";
    default: return "application/octet-stream";
    }
}
