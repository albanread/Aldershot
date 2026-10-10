/* Copyright RISC OS Open Ltd and others
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
 * This file is derived from RISC OS Open's source and from other code.
 * The Apache licence of RISC OS Open's source applies to it.
 */

/* gz.c: ZLib's gzip file calls (ZLib_GZOpen to ZLib_GZEOF) over
 * FileSwitch.
 *
 * RISC OS's ZLib module calls zlib's gzopen, gzread and the rest in
 * RISC OS zlib 1.2.13 (Sources/Lib/zlib). Its files are C library
 * streams, so they are RISC OS files. The host's zlib would open host
 * files. So this file writes that code's behaviour again over OS_Find,
 * OS_GBPB and OS_Args. It has the same states (looking, copying,
 * gunzipping), the same buffers (8K in, 16K out), the same errors and
 * messages, and the same seeks.
 *
 * RISC OS's zlib has two additions of its own. The first is the header
 * that ZLib's 'R' mode reads and writes. Reading is gzgetheader, which
 * parses the header and then reads on as raw deflate, leaving the trailer
 * unchecked. Writing is gzputheader. The second is that every header
 * written names the operating system as RISC OS's zlib does, with 13.
 *
 * A file's state is an RMA block, and so are its buffers. The handle a
 * program gets is the address of the block.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zlib.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "zlibmod.h"

#define GZ_MAGIC  0x4C465A47u           /* "GZFL" */
#define GZ_NONE   0
#define GZ_READ   7247
#define GZ_WRITE  31153
#define GZ_APPEND 1
#define LOOK 0
#define COPY 1
#define GZIP 2
#define GZBUFSIZE 8192u
#define OS_CODE_RISCOS 13

struct gz {
    uint32_t magic;
    uint32_t next;                      /* the next open file, or 0 */
    uint32_t fh;                        /* its FileSwitch handle */
    int mode;
    unsigned want, size;                /* buffer size wanted; 0 until allocated */
    unsigned char *in, *out;
    int direct;                         /* 0 if processing gzip, 1 if transparent */
    int how;                            /* reading: LOOK, COPY or GZIP */
    int64_t start;                      /* where the gzip data started, reading */
    int eof;                            /* reading: the end of the input file */
    int past;                           /* reading: read past the end */
    int level, strategy;
    int reset;                          /* writing: a new member is due */
    int64_t skip;                       /* a seek's amount, pending */
    int seek;                           /* ... whether there is one */
    int err;
    int has_msg;
    struct { unsigned have; unsigned char *next; int64_t pos; } x;
    gz_header head;                     /* writing: the header deflate writes */
    unsigned char extra[32];            /* ... its 'AC' field ('R' mode) */
    char path[256];
    char msg[300];                      /* "path: message" */
    z_stream strm;
};

/* ---- FileSwitch, as the C library's streams use it -------------------------------- */

static int fs_read(struct gz *g, unsigned char *buf, unsigned len)
{
    uint32_t r[8] = { 4, g->fh, ros_addr(buf), len };
    if (zm_swi(XOS_GBPB, r))
        return -1;
    return (int)(len - r[3]);
}

static int fs_write(struct gz *g, const unsigned char *buf, unsigned len)
{
    uint32_t r[8] = { 2, g->fh, ros_addr(buf), len };
    if (zm_swi(XOS_GBPB, r))
        return -1;
    return (int)(len - r[3]);
}

/* lseek: whence 0 is from the start, 1 from here and 2 from the end.
 * Returns the new position, or -1. */
static int64_t fs_seek(struct gz *g, int64_t off, int whence)
{
    uint32_t r[8] = { whence == 2 ? 2u : 0u, g->fh };
    if (whence != 0 && zm_swi(XOS_Args, r))
        return -1;
    int64_t to = whence == 0 ? off : (int64_t)r[2] + off;
    if (to < 0 || to > 0xFFFFFFFFll)
        return -1;
    uint32_t w[8] = { 1, g->fh, (uint32_t)to };
    if (zm_swi(XOS_Args, w))
        return -1;
    return to;
}

static int fs_close(uint32_t fh)
{
    uint32_t r[8] = { 0, fh };
    return zm_swi(XOS_Find, r) ? -1 : 0;
}

static void *gz_alloc(unsigned n)
{
    void *b;
    return xos_module_claim(n, &b) ? NULL : b;
}

static void gz_free(void *p)
{
    if (p)
        xos_module_free(p);
}

/* ---- zlib's gzlib.c ----------------------------------------------------------------- */

static void gz_error(struct gz *g, int err, const char *msg)
{
    g->has_msg = 0;
    if (err != Z_OK && err != Z_BUF_ERROR)
        g->x.have = 0;
    g->err = err;
    if (msg == NULL || err == Z_MEM_ERROR)
        return;
    snprintf(g->msg, sizeof g->msg, "%s: %s", g->path, msg);
    g->has_msg = 1;
}

static void gz_reset(struct gz *g)
{
    g->x.have = 0;
    if (g->mode == GZ_READ) {
        g->eof = 0;
        g->past = 0;
        g->how = LOOK;
    } else {
        g->reset = 0;
    }
    g->seek = 0;
    gz_error(g, Z_OK, NULL);
    g->x.pos = 0;
    g->strm.avail_in = 0;
}

/* The file a handle names, or NULL */
static struct gz *gz_of(uint32_t h)
{
    if ((h & 3) || !zm_valid(h, sizeof(struct gz)))
        return NULL;
    struct gz *g = ros_ptr(h);
    return g->magic == GZ_MAGIC ? g : NULL;
}

static void gz_link(struct gz *g)
{
    struct zm_workspace *w = zm_ws();
    g->next = w->files;
    w->files = ros_addr(g);
}

static void gz_unlink_free(struct gz *g)
{
    struct zm_workspace *w = zm_ws();
    uint32_t a = ros_addr(g), *p = &w->files;
    while (*p && *p != a)
        p = &((struct gz *)ros_ptr(*p))->next;
    if (*p)
        *p = g->next;
    g->magic = 0;
    gz_free(g);
}

/* gz_open: NULL if the mode or the file will not do */
static struct gz *gz_open(const char *path, const char *mode)
{
    struct gz *g = gz_alloc(sizeof *g);
    if (!g)
        return NULL;
    memset(g, 0, sizeof *g);
    g->want = GZBUFSIZE;
    g->mode = GZ_NONE;
    g->level = Z_DEFAULT_COMPRESSION;
    g->strategy = Z_DEFAULT_STRATEGY;
    for (; *mode; mode++) {
        if (*mode >= '0' && *mode <= '9') {
            g->level = *mode - '0';
            continue;
        }
        switch (*mode) {
        case 'r': g->mode = GZ_READ; break;
        case 'w': g->mode = GZ_WRITE; break;
        case 'a': g->mode = GZ_APPEND; break;
        case '+': gz_free(g); return NULL;      /* can't read and write at the same time */
        /* 'x' (exclusive) is O_EXCL to open(). RISC OS's zlib opens C
         * streams and has no such flag, so it ignores 'x' and so does this. */
        case 'f': g->strategy = Z_FILTERED; break;
        case 'h': g->strategy = Z_HUFFMAN_ONLY; break;
        case 'R': g->strategy = Z_RLE; break;
        case 'F': g->strategy = Z_FIXED; break;
        case 'T': g->direct = 1; break;
        default: break;                         /* 'b', 'e' and the rest */
        }
    }
    if (g->mode == GZ_NONE || (g->mode == GZ_READ && g->direct)) {
        gz_free(g);
        return NULL;
    }
    if (g->mode == GZ_READ)
        g->direct = 1;                          /* for empty file */
    snprintf(g->path, sizeof g->path, "%s", path);

    /* the file, as fopen opens it: in, out, or up at its end */
    uint32_t pa = ros_addr(g->path);
    uint32_t r[8] = { 0 };
    os_error *e = NULL;
    if (g->mode == GZ_READ) {
        r[0] = 0x4C, r[1] = pa;                 /* OpenIn: error if absent or a directory */
        e = zm_swi(XOS_Find, r);
    } else {
        if (g->mode == GZ_APPEND) {
            r[0] = 0xCC, r[1] = pa;             /* OpenUp */
            if (zm_swi(XOS_Find, r) || r[0] == 0) {
                uint32_t o[8] = { 0x80, pa };
                e = zm_swi(XOS_Find, o);
                r[0] = o[0];
            }
        } else {
            r[0] = 0x80, r[1] = pa;             /* OpenOut */
            e = zm_swi(XOS_Find, r);
        }
    }
    if (e || r[0] == 0) {
        gz_free(g);
        return NULL;
    }
    g->fh = r[0];
    if (g->mode == GZ_APPEND) {
        fs_seek(g, 0, 2);
        g->mode = GZ_WRITE;
    }
    if (g->mode == GZ_READ) {
        g->start = fs_seek(g, 0, 1);
        if (g->start == -1)
            g->start = 0;
    }
    g->magic = GZ_MAGIC;
    g->head.os = OS_CODE_RISCOS;
    gz_reset(g);
    gz_link(g);
    return g;
}

static int gz_rewind(struct gz *g)
{
    if (g->mode != GZ_READ || (g->err != Z_OK && g->err != Z_BUF_ERROR))
        return -1;
    if (fs_seek(g, g->start, 0) == -1)
        return -1;
    gz_reset(g);
    return 0;
}

/* ---- zlib's gzread.c ------------------------------------------------------------------ */

static int gz_load(struct gz *g, unsigned char *buf, unsigned len, unsigned *have)
{
    int ret = 0;
    *have = 0;
    do {
        ret = fs_read(g, buf + *have, len - *have);
        if (ret <= 0)
            break;
        *have += (unsigned)ret;
    } while (*have < len);
    if (ret < 0) {
        gz_error(g, Z_ERRNO, "Input/output error");
        return -1;
    }
    if (ret == 0)
        g->eof = 1;
    return 0;
}

static int gz_avail(struct gz *g)
{
    z_stream *strm = &g->strm;
    if (g->err != Z_OK && g->err != Z_BUF_ERROR)
        return -1;
    if (g->eof == 0) {
        if (strm->avail_in)
            memmove(g->in, strm->next_in, strm->avail_in);
        unsigned got;
        if (gz_load(g, g->in + strm->avail_in, g->size - strm->avail_in, &got) == -1)
            return -1;
        strm->avail_in += got;
        strm->next_in = g->in;
    }
    return 0;
}

/* RISC OS's gz_look. With a header to fill, it reads the header with
 * inflate's Z_BLOCK and then reads what follows as raw deflate. */
static int gz_look(struct gz *g, gz_header *head)
{
    z_stream *strm = &g->strm;
    if (g->size == 0) {
        g->in = gz_alloc(g->want);
        g->out = gz_alloc(g->want << 1);
        if (g->in == NULL || g->out == NULL) {
            gz_free(g->out);
            gz_free(g->in);
            g->in = g->out = NULL;
            gz_error(g, Z_MEM_ERROR, "out of memory");
            return -1;
        }
        g->size = g->want;
        strm->zalloc = zm_zalloc;
        strm->zfree = zm_zfree;
        strm->opaque = Z_NULL;
        strm->avail_in = 0;
        strm->next_in = Z_NULL;
        if (inflateInit2(strm, 15 + 16) != Z_OK) {
            gz_free(g->out);
            gz_free(g->in);
            g->in = g->out = NULL;
            g->size = 0;
            gz_error(g, Z_MEM_ERROR, "out of memory");
            return -1;
        }
    }
    if (strm->avail_in < 2) {
        if (gz_avail(g) == -1)
            return -1;
        if (strm->avail_in == 0)
            return 0;
    }
    if (strm->avail_in > 1 && strm->next_in[0] == 31 && strm->next_in[1] == 139) {
        if (head != NULL) {
            int ret;
            inflateGetHeader(strm, head);
            strm->next_out = g->out;
            strm->avail_out = 0;
            do {
                ret = inflate(strm, Z_BLOCK);
                if (ret == Z_BUF_ERROR &&
                    (gz_avail(g) == -1 || (g->eof && strm->avail_in == 0))) {
                    gz_error(g, Z_DATA_ERROR, "EOF in middle of gzip header");
                    return -1;
                }
            } while (ret == Z_BUF_ERROR);
            if (ret == Z_STREAM_ERROR || ret == Z_NEED_DICT) {
                gz_error(g, Z_STREAM_ERROR, "internal error: inflate stream corrupt");
                return -1;
            }
            if (ret == Z_MEM_ERROR) {
                gz_error(g, Z_MEM_ERROR, "out of memory");
                return -1;
            }
            if (ret == Z_DATA_ERROR) {
                gz_error(g, Z_DATA_ERROR, strm->msg == NULL ? "compressed data error" : strm->msg);
                return -1;
            }
        }
        inflateReset2(strm, head ? -15 : 15 + 16);
        g->how = GZIP;
        g->direct = 0;
        return 0;
    }
    if (g->direct == 0) {                       /* trailing garbage: done */
        strm->avail_in = 0;
        g->eof = 1;
        g->x.have = 0;
        return 0;
    }
    g->x.next = g->out;
    memcpy(g->x.next, strm->next_in, strm->avail_in);
    g->x.have = strm->avail_in;
    strm->avail_in = 0;
    g->how = COPY;
    g->direct = 1;
    return 0;
}

static int gz_decomp(struct gz *g)
{
    int ret = Z_OK;
    z_stream *strm = &g->strm;
    unsigned had = strm->avail_out;
    do {
        if (strm->avail_in == 0 && gz_avail(g) == -1)
            return -1;
        if (strm->avail_in == 0) {
            gz_error(g, Z_BUF_ERROR, "unexpected end of file");
            break;
        }
        ret = inflate(strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_NEED_DICT) {
            gz_error(g, Z_STREAM_ERROR, "internal error: inflate stream corrupt");
            return -1;
        }
        if (ret == Z_MEM_ERROR) {
            gz_error(g, Z_MEM_ERROR, "out of memory");
            return -1;
        }
        if (ret == Z_DATA_ERROR) {
            gz_error(g, Z_DATA_ERROR, strm->msg == NULL ? "compressed data error" : strm->msg);
            return -1;
        }
    } while (strm->avail_out && ret != Z_STREAM_END);
    g->x.have = had - strm->avail_out;
    g->x.next = strm->next_out - g->x.have;
    if (ret == Z_STREAM_END)
        g->how = LOOK;
    return 0;
}

static int gz_fetch(struct gz *g)
{
    z_stream *strm = &g->strm;
    do {
        switch (g->how) {
        case LOOK:
            if (gz_look(g, NULL) == -1)
                return -1;
            if (g->how == LOOK)
                return 0;
            break;
        case COPY:
            if (gz_load(g, g->out, g->size << 1, &g->x.have) == -1)
                return -1;
            g->x.next = g->out;
            return 0;
        case GZIP:
            strm->avail_out = g->size << 1;
            strm->next_out = g->out;
            if (gz_decomp(g) == -1)
                return -1;
        }
    } while (g->x.have == 0 && (!g->eof || strm->avail_in));
    return 0;
}

static int gz_skip(struct gz *g, int64_t len)
{
    while (len) {
        if (g->x.have) {
            unsigned n = (int64_t)g->x.have > len ? (unsigned)len : g->x.have;
            g->x.have -= n;
            g->x.next += n;
            g->x.pos += n;
            len -= n;
        } else if (g->eof && g->strm.avail_in == 0) {
            break;
        } else if (gz_fetch(g) == -1) {
            return -1;
        }
    }
    return 0;
}

static unsigned gz_read(struct gz *g, unsigned char *buf, unsigned len)
{
    if (len == 0)
        return 0;
    if (g->seek) {
        g->seek = 0;
        if (gz_skip(g, g->skip) == -1)
            return 0;
    }
    unsigned got = 0;
    do {
        unsigned n = len;
        if (g->x.have) {
            if (g->x.have < n)
                n = g->x.have;
            memcpy(buf, g->x.next, n);
            g->x.next += n;
            g->x.have -= n;
        } else if (g->eof && g->strm.avail_in == 0) {
            g->past = 1;
            break;
        } else if (g->how == LOOK || n < (g->size << 1)) {
            if (gz_fetch(g) == -1)
                return 0;
            continue;
        } else if (g->how == COPY) {
            if (gz_load(g, buf, n, &n) == -1)
                return 0;
        } else {
            g->strm.avail_out = n;
            g->strm.next_out = buf;
            if (gz_decomp(g) == -1)
                return 0;
            n = g->x.have;
            g->x.have = 0;
        }
        len -= n;
        buf += n;
        got += n;
        g->x.pos += n;
    } while (len);
    return got;
}

/* RISC OS zlib's gzgetheader */
static int gz_getheader(struct gz *g, gz_header *head)
{
    if (g->mode != GZ_READ) {
        gz_error(g, Z_STREAM_ERROR, "File not opened for read");
        return g->err;
    }
    if (g->how != LOOK) {
        gz_error(g, Z_STREAM_ERROR, "Header already read");
        return g->err;
    }
    gz_look(g, head);
    return g->err;
}

static int gz_close_r(struct gz *g)
{
    if (g->size) {
        inflateEnd(&g->strm);
        gz_free(g->out);
        gz_free(g->in);
    }
    int err = g->err == Z_BUF_ERROR ? Z_BUF_ERROR : Z_OK;
    int ret = fs_close(g->fh);
    gz_unlink_free(g);
    return ret ? Z_ERRNO : err;
}

/* ---- zlib's gzwrite.c ----------------------------------------------------------------- */

static int gz_init(struct gz *g)
{
    z_stream *strm = &g->strm;
    g->in = gz_alloc(g->want << 1);
    if (g->in == NULL) {
        gz_error(g, Z_MEM_ERROR, "out of memory");
        return -1;
    }
    if (!g->direct) {
        g->out = gz_alloc(g->want);
        if (g->out == NULL) {
            gz_free(g->in);
            g->in = NULL;
            gz_error(g, Z_MEM_ERROR, "out of memory");
            return -1;
        }
        strm->zalloc = zm_zalloc;
        strm->zfree = zm_zfree;
        strm->opaque = Z_NULL;
        if (deflateInit2(strm, g->level, Z_DEFLATED, MAX_WBITS + 16, 8, g->strategy) != Z_OK) {
            gz_free(g->out);
            gz_free(g->in);
            g->in = g->out = NULL;
            gz_error(g, Z_MEM_ERROR, "out of memory");
            return -1;
        }
        strm->next_in = NULL;
        deflateSetHeader(strm, &g->head);       /* RISC OS's: OS 13, perhaps 'AC' */
    }
    g->size = g->want;
    if (!g->direct) {
        strm->avail_out = g->size;
        strm->next_out = g->out;
        g->x.next = strm->next_out;
    }
    return 0;
}

static int gz_comp(struct gz *g, int flush)
{
    z_stream *strm = &g->strm;
    if (g->size == 0 && gz_init(g) == -1)
        return -1;
    if (g->direct) {
        while (strm->avail_in) {
            int writ = fs_write(g, strm->next_in, strm->avail_in);
            if (writ < 0) {
                gz_error(g, Z_ERRNO, "Input/output error");
                return -1;
            }
            strm->avail_in -= (unsigned)writ;
            strm->next_in += writ;
        }
        return 0;
    }
    if (g->reset) {
        if (strm->avail_in == 0)
            return 0;
        deflateReset(strm);
        g->reset = 0;
    }
    int ret = Z_OK;
    unsigned have;
    do {
        if (strm->avail_out == 0 ||
            (flush != Z_NO_FLUSH && (flush != Z_FINISH || ret == Z_STREAM_END))) {
            while (strm->next_out > g->x.next) {
                int writ = fs_write(g, g->x.next, (unsigned)(strm->next_out - g->x.next));
                if (writ < 0) {
                    gz_error(g, Z_ERRNO, "Input/output error");
                    return -1;
                }
                g->x.next += writ;
            }
            if (strm->avail_out == 0) {
                strm->avail_out = g->size;
                strm->next_out = g->out;
                g->x.next = g->out;
            }
        }
        have = strm->avail_out;
        ret = deflate(strm, flush);
        if (ret == Z_STREAM_ERROR) {
            gz_error(g, Z_STREAM_ERROR, "internal error: deflate stream corrupt");
            return -1;
        }
        have -= strm->avail_out;
    } while (have);
    if (flush == Z_FINISH)
        g->reset = 1;
    return 0;
}

static int gz_zero(struct gz *g, int64_t len)
{
    if (g->strm.avail_in && gz_comp(g, Z_NO_FLUSH) == -1)
        return -1;
    int first = 1;
    while (len) {
        unsigned n = (int64_t)g->size > len ? (unsigned)len : g->size;
        if (first) {
            memset(g->in, 0, n);
            first = 0;
        }
        g->strm.avail_in = n;
        g->strm.next_in = g->in;
        g->x.pos += n;
        if (gz_comp(g, Z_NO_FLUSH) == -1)
            return -1;
        len -= n;
    }
    return 0;
}

static unsigned gz_write(struct gz *g, const unsigned char *buf, unsigned len)
{
    unsigned put = len;
    if (len == 0)
        return 0;
    if (g->size == 0 && gz_init(g) == -1)
        return 0;
    if (g->seek) {
        g->seek = 0;
        if (gz_zero(g, g->skip) == -1)
            return 0;
    }
    if (len < g->size) {
        do {
            if (g->strm.avail_in == 0)
                g->strm.next_in = g->in;
            unsigned have = (unsigned)((g->strm.next_in + g->strm.avail_in) - g->in);
            unsigned copy = g->size - have;
            if (copy > len)
                copy = len;
            memcpy(g->in + have, buf, copy);
            g->strm.avail_in += copy;
            g->x.pos += copy;
            buf += copy;
            len -= copy;
            if (len && gz_comp(g, Z_NO_FLUSH) == -1)
                return 0;
        } while (len);
    } else {
        if (g->strm.avail_in && gz_comp(g, Z_NO_FLUSH) == -1)
            return 0;
        g->strm.next_in = (Bytef *)buf;
        g->strm.avail_in = len;
        g->x.pos += len;
        if (gz_comp(g, Z_NO_FLUSH) == -1)
            return 0;
    }
    return put;
}

/* RISC OS zlib's gzputheader: the header, before anything else */
static int gz_putheader(struct gz *g)
{
    if (g->mode != GZ_WRITE || g->size != 0)
        return Z_STREAM_ERROR;
    if (gz_init(g) == -1)
        return -1;
    return gz_comp(g, Z_NO_FLUSH);
}

static int gz_close_w(struct gz *g)
{
    int ret = Z_OK;
    if (g->seek) {
        g->seek = 0;
        if (gz_zero(g, g->skip) == -1)
            ret = g->err;
    }
    if (gz_comp(g, Z_FINISH) == -1)
        ret = g->err;
    if (g->size) {
        if (!g->direct) {
            deflateEnd(&g->strm);
            gz_free(g->out);
        }
        gz_free(g->in);
    }
    if (fs_close(g->fh) == -1)
        ret = Z_ERRNO;
    gz_unlink_free(g);
    return ret;
}

/* ---- the SWIs (c/cmodule's) -------------------------------------------------------------- */

static void from_arena(uint32_t a, char *out, size_t max)
{
    size_t i = 0;
    for (uint32_t c; i + 1 < max && (c = ros_ld8(a + (uint32_t)i)) != 0; i++)
        out[i] = (char)c;
    out[i] = 0;
}

/* GZOpen: R0 -> the file's name, R1 -> the mode. With 'R' in the mode, the
 * RISC OS 'AC' header field is written from R2-R5 (load, exec, length,
 * attributes), or read into them. Out R0 the handle, or 0. */
os_error *zm_gz_open(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    if (!zm_valid(r[0], 1))
        return zm_abort(r[0]);
    if (!zm_valid(r[1], 1))
        return zm_abort(r[1]);
    char path[256], mode[64], newmode[64];
    from_arena(r[0], path, sizeof path);
    from_arena(r[1], mode, sizeof mode);
    /* This follows do_gzopen in c/cmodule, loop included. An 'R' is taken
     * out of the mode (it asks for the metadata) and the character after
     * it is copied. */
    int ac = 0, reading = 0;
    const char *m = mode;
    char *c = newmode;
    while (*m) {
        if (*m == 'R') {
            ac = 1;
            m++;
        }
        if (*m == 'r')
            reading = 1;
        if (*m)
            *c++ = *m++;
    }
    *c = 0;
    struct gz *g = gz_open(path, newmode);
    r[0] = g ? ros_addr(g) : 0;
    if (!g || !ac)
        return NULL;
    os_error *e = NULL;
    if (reading) {
        unsigned char *extra = gz_alloc(1024);
        if (!extra) {
            gz_close_r(g);
            r[0] = 0;
            return zm_error(ZM_E_NOMEM);
        }
        gz_header head;
        memset(&head, 0, sizeof head);
        head.extra = extra;
        head.extra_max = 1024;
        if (gz_getheader(g, &head) < 0) {
            e = zm_zlib_error(g->err, g->has_msg ? g->msg : "");
            gz_close_r(g);
            gz_free(extra);
            r[0] = 0;
            return e;
        }
        /* zlib stores at most extra_max bytes of the field, but it reports
         * the file's whole length in extra_len, up to 65535. */
        int remain = (int)(head.extra_len < head.extra_max ? head.extra_len : head.extra_max);
        unsigned char *b = extra;
        while (remain >= 4) {
            unsigned char si1 = *b++, si2 = *b++;
            unsigned len = *b++;
            len += (unsigned)*b++ << 8;
            remain -= 4;
            if (si1 == 'A' && si2 == 'C' && len >= 28 && remain >= 16) {
                memcpy(&r[2], b, 16);
                uint32_t t = r[4];
                r[4] = r[5];
                r[5] = t;
                break;
            }
            b += len;
            remain -= (int)len;
        }
        gz_free(extra);
    } else {
        uint32_t field[8] = { 0x001C4341u, r[2], r[3], r[5], r[4], 0, 0, 0 };
        memcpy(g->extra, field, sizeof g->extra);
        g->head.extra = g->extra;
        g->head.extra_len = 32;
        g->head.os = OS_CODE_RISCOS;
        if (gz_putheader(g) < 0) {
            e = zm_zlib_error(g->err, g->has_msg ? g->msg : "");
            gz_close_w(g);
            r[0] = 0;
            return e;
        }
    }
    return NULL;
}

/* GZRead: R0 the handle, R1 -> buffer, R2 its size; out R0 bytes read, or -1 */
os_error *zm_gz_read(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    struct gz *g = gz_of(r[0]);
    if (!g || g->mode != GZ_READ || (g->err != Z_OK && g->err != Z_BUF_ERROR)) {
        r[0] = 0xFFFFFFFFu;
        return NULL;
    }
    if ((int32_t)r[2] < 0) {
        gz_error(g, Z_STREAM_ERROR, "request does not fit in an int");
        r[0] = 0xFFFFFFFFu;
        return NULL;
    }
    if (!zm_valid(r[1], r[2]))
        return zm_abort(r[1]);
    unsigned len = gz_read(g, ros_ptr(r[1]), r[2]);
    if (len == 0 && g->err != Z_OK && g->err != Z_BUF_ERROR)
        r[0] = 0xFFFFFFFFu;
    else
        r[0] = len;
    return NULL;
}

/* GZWrite: R0 the handle, R1 -> data, R2 its size. Out R0 bytes written,
 * or 0 on an error. */
os_error *zm_gz_write(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    struct gz *g = gz_of(r[0]);
    if (!g || g->mode != GZ_WRITE || g->err != Z_OK) {
        r[0] = 0;
        return NULL;
    }
    if ((int32_t)r[2] < 0) {
        gz_error(g, Z_DATA_ERROR, "requested length does not fit in int");
        r[0] = 0;
        return NULL;
    }
    if (!zm_valid(r[1], r[2]))
        return zm_abort(r[1]);
    r[0] = gz_write(g, ros_ptr(r[1]), r[2]);
    return NULL;
}

/* GZFlush: R0 the handle, R1 the flush type; out R0 the code */
os_error *zm_gz_flush(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    struct gz *g = gz_of(r[0]);
    int flush = (int)r[1];
    if (!g || g->mode != GZ_WRITE || g->err != Z_OK || flush < 0 || flush > Z_FINISH) {
        r[0] = (uint32_t)Z_STREAM_ERROR;
        return NULL;
    }
    if (g->seek) {
        g->seek = 0;
        if (gz_zero(g, g->skip) == -1) {
            r[0] = (uint32_t)g->err;
            return NULL;
        }
    }
    gz_comp(g, flush);
    r[0] = (uint32_t)g->err;
    return NULL;
}

/* GZClose: R0 the handle; out R0 the code */
os_error *zm_gz_close(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    struct gz *g = gz_of(r[0]);
    if (!g)
        r[0] = (uint32_t)Z_STREAM_ERROR;
    else
        r[0] = (uint32_t)(g->mode == GZ_READ ? gz_close_r(g) : gz_close_w(g));
    return NULL;
}

/* GZError: R0 the handle; out R0 -> the message, R1 the code */
os_error *zm_gz_error(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    struct gz *g = gz_of(r[0]);
    if (!g || (g->mode != GZ_READ && g->mode != GZ_WRITE)) {
        r[0] = 0;
        return NULL;
    }
    r[1] = (uint32_t)g->err;
    r[0] = g->err == Z_MEM_ERROR ? zm_string("out of memory")
           : g->has_msg ? ros_addr(g->msg) : zm_string("");
    return NULL;
}

/* GZSeek: R0 the handle, R1 the offset, R2 0 from the start or 1 from
 * here. Out R0 the new position, or -1. */
os_error *zm_gz_seek(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    if (r[2] > 1)
        return zm_error(ZM_E_INVGZSK);
    struct gz *g = gz_of(r[0]);
    int64_t offset = (int32_t)r[1];
    r[0] = 0xFFFFFFFFu;
    if (!g || (g->mode != GZ_READ && g->mode != GZ_WRITE) ||
        (g->err != Z_OK && g->err != Z_BUF_ERROR))
        return NULL;
    if (r[2] == 0)
        offset -= g->x.pos;
    else if (g->seek)
        offset += g->skip;
    g->seek = 0;
    if (g->mode == GZ_READ && g->how == COPY && g->x.pos + offset >= 0) {
        if (fs_seek(g, offset - (int64_t)g->x.have, 1) == -1)
            return NULL;
        g->x.have = 0;
        g->eof = 0;
        g->past = 0;
        g->seek = 0;
        gz_error(g, Z_OK, NULL);
        g->strm.avail_in = 0;
        g->x.pos += offset;
        r[0] = (uint32_t)g->x.pos;
        return NULL;
    }
    if (offset < 0) {
        if (g->mode != GZ_READ)
            return NULL;
        offset += g->x.pos;
        if (offset < 0 || gz_rewind(g) == -1)
            return NULL;
    }
    if (g->mode == GZ_READ) {
        unsigned n = (int64_t)g->x.have > offset ? (unsigned)offset : g->x.have;
        g->x.have -= n;
        g->x.next += n;
        g->x.pos += n;
        offset -= n;
    }
    if (offset) {
        g->seek = 1;
        g->skip = offset;
    }
    r[0] = (uint32_t)(g->x.pos + offset);
    return NULL;
}

/* GZTell: R0 the handle; out R0 the position, or -1 */
os_error *zm_gz_tell(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    struct gz *g = gz_of(r[0]);
    if (!g || (g->mode != GZ_READ && g->mode != GZ_WRITE))
        r[0] = 0xFFFFFFFFu;
    else
        r[0] = (uint32_t)(g->x.pos + (g->seek ? g->skip : 0));
    return NULL;
}

/* GZEOF: R0 the handle; out R0 1 if reading has gone past the end */
os_error *zm_gz_eof(struct ros_cpu *s)
{
    uint32_t *r = s->r;
    struct gz *g = gz_of(r[0]);
    r[0] = g && g->mode == GZ_READ ? (uint32_t)g->past : 0;
    return NULL;
}

void zm_gz_close_all(void)
{
    struct zm_workspace *w = zm_ws();
    while (w->files) {
        struct gz *g = ros_ptr(w->files);
        if (g->mode == GZ_READ)
            gz_close_r(g);
        else
            gz_close_w(g);
    }
}
