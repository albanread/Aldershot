/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_imagefs.c: filing systems added by modules, and image filing
 * systems, through FileSwitch's SWIs (modules/fileswitch/modfs.c).
 *
 * Two filing systems of the test's own, their entries native code at
 * addresses ros_call reaches (ros_native_entry), registered as a module
 * registers its own, with OS_FSControl 12 and 35, a base of 0 and the entries'
 * addresses as their offsets. FileSwitch calls them with the register
 * contracts of PRM 2 and FileSwitch's s/LowLevel:
 *
 *   - TestFS (number &F5), a filing system in memory: buffered (256-byte
 *     buffers) for most files, unbuffered with an OS_GBPB entry for a file
 *     whose name begins with U or u;
 *   - an image filing system for file type &1A0, whose images are a simple
 *     archive ("TIMG", then each object's path, load, exec, attributes,
 *     kind and length, then its data), read through the FileSwitch handle
 *     NewImage gives it and written back through it when the image closes.
 *
 * Each FSEntry and ImageEntry is reached, and what the SWIs give is
 * checked against RISC OS's contracts: object type 3 for an image file,
 * paths through it, its listing, its files read and written, *Copy out of
 * it, the image closed before its file is opened as a file, and both
 * filing systems removed.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check

#define TFS_NUMBER 0xF5u
#define IMG_TYPE   0x1A0u

/* ---- an object tree in memory --------------------------------------------------- */

struct tnode {
    char name[64];
    uint32_t load, exec, attr;
    int dir;
    uint8_t *data;
    uint32_t len, cap;
    struct tnode *kid[24];
    int nkids;
    struct tnode *parent;
};

static struct tnode *tnew(struct tnode *parent, const char *name, int dir)
{
    if (parent && parent->nkids == 24)
        return NULL;
    struct tnode *n = calloc(1, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name);
    n->dir = dir;
    n->attr = dir ? 0 : 0x13;
    n->load = 0xFFFFFD00u, n->exec = 0x12345678u;
    n->parent = parent;
    if (parent)
        parent->kid[parent->nkids++] = n;
    return n;
}

static void tfree(struct tnode *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->nkids; i++)
        tfree(n->kid[i]);
    free(n->data);
    free(n);
}

static void tunlink(struct tnode *n)
{
    struct tnode *p = n->parent;
    for (int i = 0; p && i < p->nkids; i++)
        if (p->kid[i] == n) {
            memmove(&p->kid[i], &p->kid[i + 1], (size_t)(p->nkids - i - 1) * sizeof p->kid[0]);
            p->nkids--;
            break;
        }
    n->parent = NULL;
}

static void tsetdata(struct tnode *n, const void *d, uint32_t len)
{
    free(n->data);
    n->data = malloc(len ? len : 1);
    memcpy(n->data, d, len);
    n->len = n->cap = len;
}

static void tgrow(struct tnode *n, uint32_t cap)
{
    if (cap <= n->cap)
        return;
    n->data = realloc(n->data, cap);
    memset(n->data + n->cap, 0, cap - n->cap);
    n->cap = cap;
}

/* "a.b.c" from n; "" is n.  With parent, the last part's parent and leaf. */
static struct tnode *tfind(struct tnode *n, const char *path)
{
    char comp[64];
    while (n && *path) {
        size_t k = strcspn(path, ".");
        if (k >= sizeof comp)
            return NULL;
        memcpy(comp, path, k);
        comp[k] = 0;
        struct tnode *next = NULL;
        for (int i = 0; i < n->nkids; i++)
            if (!strcasecmp(n->kid[i]->name, comp))
                next = n->kid[i];
        n = next;
        path += k;
        if (*path == '.')
            path++;
    }
    return n;
}

static struct tnode *tparent(struct tnode *root, const char *path, const char **leaf)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        *leaf = path;
        return root;
    }
    char head[512];
    snprintf(head, sizeof head, "%.*s", (int)(dot - path), path);
    *leaf = dot + 1;
    struct tnode *p = tfind(root, head);
    return p && p->dir ? p : NULL;
}

/* ---- the two filing systems' state ------------------------------------------------ */

static struct tnode *fsroot;            /* TestFS's */

#define NIMAGES 4
static struct {
    int used, dirty;
    uint32_t swh;                       /* the image file's FileSwitch handle */
    struct tnode *root;
    char name[256];                     /* OS_Args 7 of it, at NewImage */
} imgs[NIMAGES];

#define NOPEN 16
static struct {
    int used, unbuf;
    struct tnode *n;
    uint32_t ptr;
    int img;                            /* -1 TestFS */
} opens[NOPEN];

static struct {
    unsigned file, open, get, put, args, close, func, gbpb;
    unsigned newimage, imageclosing, bootup;
    unsigned getbytes_bytes, flushes, ensures, zeroes, setexts;
    uint32_t last_swh, last_close_load;
    int newimage_handle_ok;
} calls;

static uint32_t errblocks;              /* RMA: error blocks to hand back */

static void ret(struct ros_cpu *s)
{
    s->r[15] = s->r[14];
}

static void fail_with(struct ros_cpu *s, uint32_t num, const char *msg)
{
    static unsigned next;
    uint32_t b = errblocks + 256 * (next++ % 4);
    ros_st32(b, num);
    snprintf(ros_ptr(b + 4), 250, "%s", msg);
    s->r[0] = b;
    s->v = 1;
    ret(s);
}

static void cstr(uint32_t a, char *out, size_t max)
{
    size_t k = 0;
    if (a)
        while (k + 1 < max && ros_ld8(a + k) >= ' ')
            out[k] = (char)ros_ld8(a + k), k++;
    out[k] = 0;
}

/* The path an entry is given: TestFS's "$.a.b" or "$"; an image's "a.b" */
static struct tnode *root_and_path(struct ros_cpu *s, int image, uint32_t addr, char *path,
                                   size_t max)
{
    char raw[512];
    cstr(addr, raw, sizeof raw);
    if (image) {
        uint32_t ih = s->r[6];
        snprintf(path, max, "%s", raw);
        return ih >= 1 && ih <= NIMAGES && imgs[ih - 1].used ? imgs[ih - 1].root : NULL;
    }
    const char *p = raw;
    if (p[0] == '$')
        p += p[1] == '.' ? 2 : 1;
    snprintf(path, max, "%s", p);
    return fsroot;
}

static void mark_dirty(struct ros_cpu *s, int image)
{
    if (image && s->r[6] >= 1 && s->r[6] <= NIMAGES)
        imgs[s->r[6] - 1].dirty = 1;
}

/* FSEntry_File */
static void file_entry(struct ros_cpu *s, int image)
{
    calls.file++;
    char path[512];
    const char *leaf;
    struct tnode *root = root_and_path(s, image, s->r[1], path, sizeof path);
    if (!root)
        return fail_with(s, 0x1F5C8, "Bad image");
    struct tnode *n = tfind(root, path), *p;
    switch (s->r[0]) {
    case 5:                                             /* read info */
        s->r[0] = n ? (n->dir ? 2 : 1) : 0;
        if (n)
            s->r[2] = n->load, s->r[3] = n->exec, s->r[4] = n->dir ? 0 : n->len, s->r[5] = n->attr;
        break;
    case 255:                                           /* load at R2 */
        if (!n || n->dir)
            return fail_with(s, 0x1F5D6, "Not found");
        memcpy(ros_ptr(s->r[2]), n->data, n->len);
        s->r[2] = n->load, s->r[3] = n->exec, s->r[4] = n->len, s->r[5] = n->attr;
        break;
    case 0:                                             /* save R4-R5 */
    case 7:                                             /* create, R5-R4 long */
        if (n && n->dir)
            return fail_with(s, 0x1F5A8, "Is a directory");
        if (!n) {
            if (!(p = tparent(root, path, &leaf)) || !(n = tnew(p, leaf, 0)))
                return fail_with(s, 0x1F5D6, "Not found");
        }
        n->load = s->r[2], n->exec = s->r[3];
        if (s->r[0] == 0) {
            tsetdata(n, ros_ptr(s->r[4]), s->r[5] - s->r[4]);
        } else {
            free(n->data);
            n->data = calloc(1, s->r[5] - s->r[4] + 1);
            n->len = n->cap = s->r[5] - s->r[4];
        }
        mark_dirty(s, image);
        break;
    case 8:                                             /* directory */
        if (n)
            break;
        if (!(p = tparent(root, path, &leaf)) || !tnew(p, leaf, 1))
            return fail_with(s, 0x1F5D6, "Not found");
        mark_dirty(s, image);
        break;
    case 6:                                             /* delete */
        s->r[0] = n ? (n->dir ? 2 : 1) : 0;
        if (n) {
            s->r[2] = n->load, s->r[3] = n->exec, s->r[4] = n->len, s->r[5] = n->attr;
            tunlink(n);
            tfree(n);
            mark_dirty(s, image);
        }
        break;
    case 1:                                             /* write info */
        if (!n)
            return fail_with(s, 0x1F5D6, "Not found");
        n->load = s->r[2], n->exec = s->r[3], n->attr = s->r[5];
        mark_dirty(s, image);
        break;
    default:
        return fail_with(s, 0x1F5A5, "Bad file reason");
    }
    s->v = 0;
    ret(s);
}

/* FSEntry_Open: R0 is 0 for read or 2 for update, R1 the path, R3 the FileSwitch handle */
static void open_entry(struct ros_cpu *s, int image)
{
    calls.open++;
    calls.last_swh = s->r[3];
    char path[512];
    struct tnode *root = root_and_path(s, image, s->r[1], path, sizeof path);
    struct tnode *n = root ? tfind(root, path) : NULL;
    if (!n || n->dir) {
        s->r[1] = 0;                                    /* not found */
        s->v = 0;
        return ret(s);
    }
    int h = 0;
    while (h < NOPEN && opens[h].used)
        h++;
    if (h == NOPEN)
        return fail_with(s, 0x1F5C0, "Too many open");
    opens[h].used = 1;
    opens[h].n = n;
    opens[h].ptr = 0;
    opens[h].img = image ? (int)s->r[6] - 1 : -1;
    opens[h].unbuf = !image && (n->name[0] == 'U' || n->name[0] == 'u');
    uint32_t wr = s->r[0] ? 0x80000000u : 0;
    s->r[0] = wr | 0x40000000u | (opens[h].unbuf ? 0x10000000u : 0);
    s->r[1] = (uint32_t)h + 1;
    s->r[2] = opens[h].unbuf ? 0 : 256;
    s->r[3] = n->len;
    s->r[4] = n->cap;
    s->v = 0;
    ret(s);
}

static int handle(struct ros_cpu *s, uint32_t h)
{
    if (h < 1 || h > NOPEN || !opens[h - 1].used) {
        fail_with(s, 0x1F5DE, "Bad handle");
        return -1;
    }
    return (int)h - 1;
}

static void dirty_of(int h)
{
    if (opens[h].img >= 0)
        imgs[opens[h].img].dirty = 1;
}

/* FSEntry_GetBytes: buffered files use R2, R3 and R4.  Unbuffered files give a byte in R0 and C at the end */
static void get_entry(struct ros_cpu *s, int image)
{
    (void)image;
    calls.get++;
    int h = handle(s, s->r[1]);
    if (h < 0)
        return;
    struct tnode *n = opens[h].n;
    if (opens[h].unbuf) {
        s->c = opens[h].ptr >= n->len;
        s->r[0] = s->c ? 0 : n->data[opens[h].ptr++];
    } else {
        uint8_t *d = ros_ptr(s->r[2]);
        for (uint32_t k = 0; k < s->r[3]; k++) {
            uint32_t at = s->r[4] + k;
            d[k] = at < n->cap ? n->data[at] : 0;
        }
        calls.getbytes_bytes += s->r[3];
    }
    s->v = 0;
    ret(s);
}

static void put_entry(struct ros_cpu *s, int image)
{
    (void)image;
    calls.put++;
    int h = handle(s, s->r[1]);
    if (h < 0)
        return;
    struct tnode *n = opens[h].n;
    if (opens[h].unbuf) {
        tgrow(n, opens[h].ptr + 1);
        n->data[opens[h].ptr++] = (uint8_t)s->r[0];
        if (opens[h].ptr > n->len)
            n->len = opens[h].ptr;
    } else {
        tgrow(n, s->r[4] + s->r[3]);
        memcpy(n->data + s->r[4], ros_ptr(s->r[2]), s->r[3]);
    }
    dirty_of(h);
    s->v = 0;
    ret(s);
}

static void args_entry(struct ros_cpu *s, int image)
{
    (void)image;
    calls.args++;
    int h = handle(s, s->r[1]);
    if (h < 0)
        return;
    struct tnode *n = opens[h].n;
    switch (s->r[0]) {
    case 0: s->r[2] = opens[h].ptr; break;
    case 1: opens[h].ptr = s->r[2]; break;
    case 2: s->r[2] = n->len; break;
    case 3:
        calls.setexts++;
        tgrow(n, s->r[2]);          /* what PutBytes put past the extent stays */
        n->len = s->r[2];
        dirty_of(h);
        break;
    case 4: s->r[2] = n->cap; break;
    case 5: s->r[2] = opens[h].ptr >= n->len ? 0xFFFFFFFFu : 0; break;
    case 6: calls.flushes++; break;
    case 7: calls.ensures++; tgrow(n, s->r[2]); s->r[2] = n->cap; break;
    case 8:
        calls.zeroes++;
        tgrow(n, s->r[2] + s->r[3]);
        memset(n->data + s->r[2], 0, s->r[3]);
        break;
    case 9: s->r[2] = n->load, s->r[3] = n->exec; break;
    default: return fail_with(s, 0x1F5A6, "Bad args reason");
    }
    s->v = 0;
    ret(s);
}

static void close_entry(struct ros_cpu *s, int image)
{
    (void)image;
    calls.close++;
    int h = handle(s, s->r[1]);
    if (h < 0)
        return;
    calls.last_close_load = s->r[2];
    if (s->r[2] || s->r[3]) {
        opens[h].n->load = s->r[2], opens[h].n->exec = s->r[3];
        dirty_of(h);
    }
    opens[h].used = 0;
    s->v = 0;
    ret(s);
}

/* FSEntry_GBPB, unbuffered files: R0 1-4, R1, R2 the memory, R3 the
 * count, R4 the pointer (1 and 3) */
static void gbpb_entry(struct ros_cpu *s)
{
    calls.gbpb++;
    int h = handle(s, s->r[1]);
    if (h < 0)
        return;
    struct tnode *n = opens[h].n;
    if (s->r[0] == 1 || s->r[0] == 3)
        opens[h].ptr = s->r[4];
    uint32_t p = opens[h].ptr, k = s->r[3];
    if (s->r[0] <= 2) {
        tgrow(n, p + k);
        memcpy(n->data + p, ros_ptr(s->r[2]), k);
        if (p + k > n->len)
            n->len = p + k;
    } else {
        k = p >= n->len ? 0 : n->len - p < k ? n->len - p : k;
        memcpy(ros_ptr(s->r[2]), n->data + p, k);
    }
    opens[h].ptr = p + k;
    s->r[2] += k;
    s->r[3] -= k;
    s->r[4] = opens[h].ptr;
    s->v = 0;
    ret(s);
}

/* ---- images: reading and writing the archive through FileSwitch -------------------- */

static int xswi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 8 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 8 * sizeof r[0]);
    return c.v ? 1 : c.c ? 2 : 0;
}

struct rec {                            /* one object in an archive */
    char path[64];
    uint32_t load, exec, attr, dir, len;
};

static void load_archive(struct tnode *root, const uint8_t *b, uint32_t len)
{
    if (len < 8 || memcmp(b, "TIMG", 4))
        return;
    uint32_t count, at = 8;
    memcpy(&count, b + 4, 4);
    for (uint32_t i = 0; i < count && at + sizeof(struct rec) <= len; i++) {
        struct rec r;
        memcpy(&r, b + at, sizeof r);
        at += sizeof r;
        const char *leaf;
        struct tnode *p = tparent(root, r.path, &leaf);
        struct tnode *n = p ? tnew(p, leaf, (int)r.dir) : NULL;
        if (n) {
            n->load = r.load, n->exec = r.exec, n->attr = r.attr;
            if (!r.dir)
                tsetdata(n, b + at, r.len);
        }
        at += r.dir ? 0 : r.len;
    }
}

static void save_node(const struct tnode *n, const char *prefix, uint8_t **b, uint32_t *len,
                      uint32_t *count)
{
    for (int i = 0; i < n->nkids; i++) {
        const struct tnode *k = n->kid[i];
        struct rec r;
        memset(&r, 0, sizeof r);
        snprintf(r.path, sizeof r.path, "%s%s%s", prefix, prefix[0] ? "." : "", k->name);
        r.load = k->load, r.exec = k->exec, r.attr = k->attr, r.dir = (uint32_t)k->dir;
        r.len = k->dir ? 0 : k->len;
        *b = realloc(*b, *len + sizeof r + r.len);
        memcpy(*b + *len, &r, sizeof r);
        if (r.len)
            memcpy(*b + *len + sizeof r, k->data, r.len);
        *len += (uint32_t)sizeof r + r.len;
        (*count)++;
        if (k->dir)
            save_node(k, r.path, b, len, count);
    }
}

static uint8_t *make_archive(const struct tnode *root, uint32_t *len)
{
    uint8_t *b = malloc(8);
    uint32_t count = 0;
    *len = 8;
    memcpy(b, "TIMG", 4);
    save_node(root, "", &b, len, &count);
    memcpy(b + 4, &count, 4);
    return b;
}

/* NewImage: R1 the FileSwitch handle, R2 the buffer size; R1 out the
 * image's handle.  The archive is read through the handle. */
static void new_image(struct ros_cpu *s)
{
    calls.newimage++;
    int i = 0;
    while (i < NIMAGES && imgs[i].used)
        i++;
    if (i == NIMAGES)
        return fail_with(s, 0x1F5C0, "Too many images");
    uint32_t swh = s->r[1];
    uint32_t r[8] = { 2, swh };                         /* OS_Args 2: its extent */
    if (xswi(XOS_Args, r) == 1)
        return fail_with(s, 0x1F5DE, "Image handle not valid");
    uint32_t len = r[2];
    uint32_t buf = ros_addr(ros_rma_alloc(len + 4));
    uint32_t g[8] = { 3, swh, buf, len, 0 };
    int ok = xswi(XOS_GBPB, g) != 1;
    imgs[i].root = tnew(NULL, "$", 1);
    if (ok)
        load_archive(imgs[i].root, ros_ptr(buf), len);
    ros_rma_free(ros_ptr(buf));
    uint32_t a[8] = { 7, swh, errblocks + 1024, 0, 0, 256 };   /* OS_Args 7: its name */
    xswi(XOS_Args, a);
    cstr(errblocks + 1024, imgs[i].name, sizeof imgs[i].name);
    calls.newimage_handle_ok = ok && strstr(imgs[i].name, "Img") != NULL;
    imgs[i].used = 1;
    imgs[i].dirty = 0;
    imgs[i].swh = swh;
    s->r[1] = (uint32_t)i + 1;
    s->v = 0;
    ret(s);
}

/* ImageClosing: R1 the image's handle.  A changed archive is written back
 * through the FileSwitch handle, still open. */
static void image_closing(struct ros_cpu *s)
{
    calls.imageclosing++;
    uint32_t ih = s->r[1];
    if (ih < 1 || ih > NIMAGES || !imgs[ih - 1].used)
        return fail_with(s, 0x1F5C8, "Bad image");
    int i = (int)ih - 1;
    if (imgs[i].dirty) {
        uint32_t len;
        uint8_t *b = make_archive(imgs[i].root, &len);
        uint32_t buf = ros_addr(ros_rma_alloc(len));
        memcpy(ros_ptr(buf), b, len);
        free(b);
        uint32_t w[8] = { 1, imgs[i].swh, buf, len, 0 };
        xswi(XOS_GBPB, w);
        uint32_t x[8] = { 3, imgs[i].swh, len };
        xswi(XOS_Args, x);
        ros_rma_free(ros_ptr(buf));
    }
    tfree(imgs[i].root);
    imgs[i].used = 0;
    s->v = 0;
    ret(s);
}

/* FSEntry_Func: 8 rename, 10 boot, 15 read entries with information, 21
 * and 22 the images */
static void func_entry(struct ros_cpu *s, int image)
{
    calls.func++;
    char path[512], path2[512];
    switch (s->r[0]) {
    case 21:
        return new_image(s);
    case 22:
        return image_closing(s);
    case 10:
        calls.bootup++;
        break;
    case 8: {
        struct tnode *root = root_and_path(s, image, s->r[1], path, sizeof path);
        root_and_path(s, image, s->r[2], path2, sizeof path2);
        struct tnode *n = root ? tfind(root, path) : NULL;
        const char *leaf;
        struct tnode *p = root ? tparent(root, path2, &leaf) : NULL;
        if (!n || !p || tfind(root, path2)) {
            s->r[1] = 1;                                /* invalid */
            break;
        }
        tunlink(n);
        snprintf(n->name, sizeof n->name, "%s", leaf);
        n->parent = p;
        p->kid[p->nkids++] = n;
        mark_dirty(s, image);
        s->r[1] = 0;
        break;
    }
    case 15: {
        struct tnode *root = root_and_path(s, image, s->r[1], path, sizeof path);
        struct tnode *d = root ? tfind(root, path) : NULL;
        if (!d || !d->dir)
            return fail_with(s, 0x1F5D6, "Not found");
        uint32_t at = s->r[2], room = s->r[5], got = 0, i = s->r[4];
        for (; i < (uint32_t)d->nkids && got < s->r[3]; i++) {
            struct tnode *k = d->kid[i];
            uint32_t size = (20 + (uint32_t)strlen(k->name) + 1 + 3) & ~3u;
            if (size > room)
                break;
            ros_st32(at, k->load), ros_st32(at + 4, k->exec);
            ros_st32(at + 8, k->dir ? 0 : k->len), ros_st32(at + 12, k->attr);
            ros_st32(at + 16, k->dir ? 2 : 1);
            strcpy(ros_ptr(at + 20), k->name);
            at += size, room -= size, got++;
        }
        s->r[3] = got;
        s->r[4] = i >= (uint32_t)d->nkids ? 0xFFFFFFFFu : i;
        break;
    }
    default:
        return fail_with(s, 0x1F5A7, "Bad func reason");
    }
    s->v = 0;
    ret(s);
}

static void fs_open(struct ros_cpu *s) { open_entry(s, 0); }
static void fs_get(struct ros_cpu *s) { get_entry(s, 0); }
static void fs_put(struct ros_cpu *s) { put_entry(s, 0); }
static void fs_args(struct ros_cpu *s) { args_entry(s, 0); }
static void fs_close(struct ros_cpu *s) { close_entry(s, 0); }
static void fs_file(struct ros_cpu *s) { file_entry(s, 0); }
static void fs_func(struct ros_cpu *s) { func_entry(s, 0); }
static void fs_gbpb(struct ros_cpu *s) { gbpb_entry(s); }
static void im_open(struct ros_cpu *s) { open_entry(s, 1); }
static void im_get(struct ros_cpu *s) { get_entry(s, 1); }
static void im_put(struct ros_cpu *s) { put_entry(s, 1); }
static void im_args(struct ros_cpu *s) { args_entry(s, 1); }
static void im_close(struct ros_cpu *s) { close_entry(s, 1); }
static void im_file(struct ros_cpu *s) { file_entry(s, 1); }
static void im_func(struct ros_cpu *s) { func_entry(s, 1); }

/* ---- the checks ------------------------------------------------------------------- */

static uint32_t text;                   /* RMA scratch: names at +0, +512; data at +1024 */
static char root[512];

static int swi(uint32_t n, uint32_t r[8])
{
    return xswi(n, r);
}

static const char *errmess(const uint32_t r[8])
{
    return ((os_error *)ros_ptr(r[0]))->errmess;
}

static uint32_t errnum(const uint32_t r[8])
{
    return ((os_error *)ros_ptr(r[0]))->errnum;
}

static uint32_t name(const char *s)
{
    strcpy(ros_ptr(text), s);
    return text;
}

static uint32_t name2(const char *s)
{
    strcpy(ros_ptr(text + 512), s);
    return text + 512;
}

static uint32_t info(const char *n, uint32_t r[8])
{
    uint32_t q[8] = { 5, name(n) };
    int v = swi(XOS_File, q);
    memcpy(r, q, sizeof q);
    return v == 1 ? 0xFFFFFFFFu : q[0];
}

/* The names a directory lists, OS_GBPB 10, as "a,b,c", each with its
 * object type after a colon */
static int list(const char *dir, char *out, size_t max)
{
    out[0] = 0;
    uint32_t buf = text + 1024, at = 0xFFFFFFFFu, next = 0;
    for (int rounds = 0; rounds < 50 && next != 0xFFFFFFFFu; rounds++) {
        uint32_t r[8] = { 10, name(dir), buf, 1, next, 512, 0 };
        if (swi(XOS_GBPB, r) == 1) {
            snprintf(out, max, "error: %s", errmess(r));
            return 0;
        }
        for (uint32_t k = 0, p = buf; k < r[3]; k++) {
            size_t n = strlen(out);
            snprintf(out + n, max - n, "%s%s:%u", n ? "," : "", (char *)ros_ptr(p + 20),
                     ros_ld32(p + 16));
            p += (20 + (uint32_t)strlen(ros_ptr(p + 20)) + 1 + 3) & ~3u;
        }
        next = r[4];
        (void)at;
    }
    return 1;
}

static int load_text(const char *n, char *out, size_t max)
{
    uint32_t r[8] = { 255, name(n), text + 1024, 0 };
    if (swi(XOS_File, r) == 1) {
        snprintf(out, max, "error: %s", errmess(r));
        return 0;
    }
    snprintf(out, max, "%.*s", (int)r[4], (char *)ros_ptr(text + 1024));
    return 1;
}

static uint32_t fs_block, ifs_block;

static void add_fs(void)
{
    static uint32_t entries[8];
    if (!entries[0]) {
        entries[0] = ros_native_entry(fs_open, "testfs:open");
        entries[1] = ros_native_entry(fs_get, "testfs:get");
        entries[2] = ros_native_entry(fs_put, "testfs:put");
        entries[3] = ros_native_entry(fs_args, "testfs:args");
        entries[4] = ros_native_entry(fs_close, "testfs:close");
        entries[5] = ros_native_entry(fs_file, "testfs:file");
        entries[6] = ros_native_entry(fs_func, "testfs:func");
        entries[7] = ros_native_entry(fs_gbpb, "testfs:gbpb");
    }
    uint32_t b = fs_block;
    strcpy(ros_ptr(b + 64), "TestFS");
    strcpy(ros_ptr(b + 80), "Test filing system");
    ros_st32(b + 0, b + 64);                    /* base 0: offsets are addresses */
    ros_st32(b + 4, b + 80);
    for (int i = 0; i < 6; i++)
        ros_st32(b + 8 + 4 * (uint32_t)i, entries[i]);
    ros_st32(b + 32, TFS_NUMBER);               /* info: its number, buffered */
    ros_st32(b + 36, entries[6]);
    ros_st32(b + 40, entries[7]);
}

static void add_ifs_block(void)
{
    static uint32_t entries[7];
    if (!entries[0]) {
        entries[0] = ros_native_entry(im_open, "testimg:open");
        entries[1] = ros_native_entry(im_get, "testimg:get");
        entries[2] = ros_native_entry(im_put, "testimg:put");
        entries[3] = ros_native_entry(im_args, "testimg:args");
        entries[4] = ros_native_entry(im_close, "testimg:close");
        entries[5] = ros_native_entry(im_file, "testimg:file");
        entries[6] = ros_native_entry(im_func, "testimg:func");
    }
    uint32_t b = ifs_block;
    ros_st32(b + 0, 1u << 27);                  /* flush notify */
    ros_st32(b + 4, IMG_TYPE);
    for (int i = 0; i < 7; i++)
        ros_st32(b + 8 + 4 * (uint32_t)i, entries[i]);
}

static void module_fs(void)
{
    char t[512];
    uint32_t r[8];
    fsroot = tnew(NULL, "$", 1);
    struct tnode *hello = tnew(fsroot, "Hello", 0);
    tsetdata(hello, "Hello world\n", 12);
    hello->load = 0xFFFFFF00u;                  /* Text */
    struct tnode *dir = tnew(fsroot, "Dir", 1);
    struct tnode *inner = tnew(dir, "Inner", 0);
    char big[700];
    for (int i = 0; i < 700; i++)
        big[i] = (char)('a' + i % 26);
    tsetdata(inner, big, 700);
    struct tnode *u = tnew(fsroot, "UFile", 0);
    tsetdata(u, "unbuffered", 10);

    add_fs();
    uint32_t a[8] = { 12, 0, fs_block, 0x1234 };
    int v = swi(XOS_FSControl, a);
    check(!v, "Module FS -- OS_FSControl 12 adds TestFS", "AddFS: %s", v ? errmess(a) : "ok");

    uint32_t l[8] = { 13, name("TestFS"), 1 };
    swi(XOS_FSControl, l);
    uint32_t nm[8] = { 33, TFS_NUMBER, text + 1024, 32 };
    swi(XOS_FSControl, nm);
    check(l[1] == TFS_NUMBER && l[2] != 0 && ros_ld32(l[2]) == TFS_NUMBER &&
              !strcmp(ros_ptr(text + 1024), "TestFS"),
          "Module FS -- OS_FSControl 13 finds it by name, its number &F5; 33 names &F5 TestFS",
          "13: %X %X; 33: %s", l[1], l[2], (char *)ros_ptr(text + 1024));

    uint32_t i5 = info("TestFS:$.Hello", r);
    check(i5 == 1 && r[2] == 0xFFFFFF00u && r[3] == 0x12345678u && r[4] == 12 && r[5] == 0x13,
          "Module FS -- OS_File 5 through FSEntry_File 5: a file's load, exec, length, attributes",
          "%u %08X %08X %u %X", i5, r[2], r[3], r[4], r[5]);
    uint32_t ir = info("TestFS:$", r), id = info("TestFS:$.Dir", r);
    uint32_t in = info("TestFS:$.Nothing", r);
    check(ir == 2 && id == 2 && in == 0, "Module FS -- $ and Dir are directories, Nothing nothing",
          "$ %u, Dir %u, Nothing %u", ir, id, in);

    unsigned files0 = calls.file;
    check(load_text("TestFS:$.Hello", t, sizeof t) && !strcmp(t, "Hello world\n") &&
              calls.file > files0,
          "Module FS -- OS_File 255 loads through FSEntry_File 255", "%s", t);

    /* selected: relative names, OS_Args 0 */
    uint32_t sel[8] = { 14, name("TestFS") };
    v = swi(XOS_FSControl, sel);
    uint32_t a0[8] = { 0, 0 };
    swi(XOS_Args, a0);
    uint32_t irel = info("Dir.Inner", r);
    check(!v && a0[0] == TFS_NUMBER && irel == 1 && r[4] == 700,
          "Module FS -- *TestFS selected (OS_FSControl 14 by name): OS_Args 0 &F5, Dir.Inner relative",
          "%d %X %u %u", v, a0[0], irel, r[4]);

    /* a buffered stream: 700 bytes in 256-byte buffers */
    uint32_t o[8] = { 0x4C, name("TestFS:$.Dir.Inner") };
    v = swi(XOS_Find, o);
    uint32_t h = o[0];
    int ok = !v && h;
    uint32_t g[8] = { 3, h, text + 1024, 300, 250 };
    if (ok)
        swi(XOS_GBPB, g);
    ok = ok && g[3] == 0 && g[4] == 550 && !memcmp(ros_ptr(text + 1024), big + 250, 300);
    uint32_t b1[8] = { 0, h };
    swi(XOS_BGet, b1);
    ok = ok && b1[0] == (uint32_t)big[550];
    uint32_t e2[8] = { 2, h };
    swi(XOS_Args, e2);
    uint32_t g2[8] = { 4, h, text + 1024, 400 };
    int c2 = swi(XOS_GBPB, g2);
    uint32_t fh[8] = { 21, h };
    swi(XOS_FSControl, fh);
    check(ok && e2[2] == 700 && c2 == 2 && g2[3] == 251 && fh[1] >= 1 && fh[1] <= NOPEN &&
              calls.last_swh == h,
          "Module FS -- a buffered stream: OS_GBPB 3 across 256-byte buffers, OS_BGet, the "
          "extent 700, a read past the end with C, OS_FSControl 21's handle, FSEntry_Open's R3",
          "%d %u %u %u fh %u swh %u", ok, e2[2], c2, g2[3], fh[1], calls.last_swh);
    uint32_t cl[8] = { 0, h };
    swi(XOS_Find, cl);

    /* OpenOut of a new file, written across buffers, then closed: stamped */
    unsigned ens = calls.ensures, sx = calls.setexts;
    uint32_t oo[8] = { 0x8C, name("TestFS:$.New") };
    v = swi(XOS_Find, oo);
    h = oo[0];
    for (int i = 0; i < 600; i++)
        ros_st8(text + 1024 + (uint32_t)i, (uint32_t)(i * 7));
    uint32_t w[8] = { 2, h, text + 1024, 600 };
    if (!v)
        swi(XOS_GBPB, w);
    uint32_t bp[8] = { 'Z', h };
    swi(XOS_BPut, bp);
    uint32_t cl2[8] = { 0, h };
    swi(XOS_Find, cl2);
    uint32_t inew = info("TestFS:$.New", r);
    struct tnode *nn = tfind(fsroot, "New");
    int same = nn && nn->len == 601;
    for (int i = 0; same && i < 600; i++)
        same = nn->data[i] == (uint8_t)(i * 7);
    same = same && nn->data[600] == 'Z';
    check(!v && inew == 1 && r[4] == 601 && (r[2] >> 8) == 0xFFFFFD && same &&
              calls.ensures > ens && calls.setexts > sx && (calls.last_close_load >> 8) == 0xFFFFFD,
          "Module FS -- OpenOut made it (FSEntry_File 7), 601 bytes written through PutBytes, "
          "EnsureSize and SetEXT, closed with a Data stamp",
          "%d %u len %u load %08X same %d ens %u setext %u close %08X", v, inew, r[4], r[2], same,
          calls.ensures - ens, calls.setexts - sx, calls.last_close_load);

    /* an unbuffered file, through FSEntry_GBPB, and OS_BGet's GetBytes */
    unsigned gb = calls.gbpb;
    uint32_t ou[8] = { 0xCC, name("TestFS:$.UFile") };
    swi(XOS_Find, ou);
    h = ou[0];
    uint32_t ug[8] = { 3, h, text + 1024, 4, 2 };
    swi(XOS_GBPB, ug);
    int uok = !memcmp(ros_ptr(text + 1024), "buff", 4);
    uint32_t ub[8] = { 0, h };
    swi(XOS_BGet, ub);
    uint32_t uw[8] = { 1, h, name("!!"), 2, 0 };
    swi(XOS_GBPB, uw);
    uint32_t cl3[8] = { 0, h };
    swi(XOS_Find, cl3);
    check(uok && ub[0] == 'e' && calls.gbpb >= gb + 2 && u->len == 10 && !memcmp(u->data, "!!buffered", 10),
          "Module FS -- an unbuffered file: OS_GBPB 3 and 1 through FSEntry_GBPB, OS_BGet through GetBytes",
          "%d %c gbpb %u %.*s", uok, ub[0], calls.gbpb - gb, (int)u->len, (char *)u->data);

    /* OS_File 10: FSEntry_File 0, the file saved whole */
    uint32_t sv[8] = { 10, name("TestFS:$.Saved"), 0xFFF, 0, name2("saved data"), name2("saved data") + 10 };
    v = swi(XOS_File, sv);
    check(!v && load_text("TestFS:$.Saved", t, sizeof t) && !strcmp(t, "saved data") &&
              info("TestFS:$.Saved", r) == 1 && (r[2] >> 8 & 0xFFF) == 0xFFF,
          "Module FS -- OS_File 10 saves through FSEntry_File 0, typed Text", "%d %s", v, t);

    /* a listing through FSEntry_Func 15 */
    list("TestFS:$", t, sizeof t);
    check(!strcmp(t, "Hello:1,Dir:2,UFile:1,New:1,Saved:1"),
          "Module FS -- OS_GBPB 10 lists $ through FSEntry_Func 15", "%s", t);

    /* rename (FSEntry_Func 8), a directory made (File 8), delete (File 6) */
    uint32_t rn[8] = { 25, name("TestFS:$.Saved"), name2("TestFS:$.Dir.Moved") };
    int vr = swi(XOS_FSControl, rn);
    uint32_t cd[8] = { 8, name("TestFS:$.NewDir") };
    int vc = swi(XOS_File, cd);
    uint32_t dl[8] = { 6, name("TestFS:$.New") };
    int vd = swi(XOS_File, dl);
    list("TestFS:$", t, sizeof t);
    char t2[256];
    list("TestFS:$.Dir", t2, sizeof t2);
    check(!vr && !vc && !vd && dl[0] == 1 && !strcmp(t, "Hello:1,Dir:2,UFile:1,NewDir:2") &&
              !strcmp(t2, "Inner:1,Moved:1"),
          "Module FS -- *Rename (FSEntry_Func 8), *CDir (File 8), *Delete (File 6)",
          "%d %d %d; %s; %s", vr, vc, vd, t, t2);

    /* *Copy from HostFS's disc into TestFS and back */
    uint32_t cp[8] = { 26, name("TestFS:$.Hello"), name2("HostFS::Test.$.FromTFS"), 0x2 };
    int v1 = swi(XOS_FSControl, cp);
    uint32_t cp2[8] = { 26, name("HostFS::Test.$.FromTFS"), name2("TestFS:$.Back"), 0x2 };
    int v2 = swi(XOS_FSControl, cp2);
    check(!v1 && !v2 && load_text("TestFS:$.Back", t, sizeof t) && !strcmp(t, "Hello world\n"),
          "Module FS -- *Copy out to HostFS and back in", "%d %d %s", v1, v2, t);

    /* boot, read module base */
    unsigned bu = calls.bootup;
    uint32_t bo[8] = { 15 };
    swi(XOS_FSControl, bo);
    uint32_t mb[8] = { 20 };
    swi(XOS_FSControl, mb);
    check(calls.bootup == bu + 1 && mb[1] == 0 && mb[2] == 0x1234,
          "Module FS -- OS_FSControl 15 calls FSEntry_Func 10; 20 gives its base and R12",
          "boot %u, %X %X", calls.bootup - bu, mb[1], mb[2]);

    /* removal: by number is refused.  By name works, and its open files are closed */
    uint32_t keep[8] = { 0x4C, name("TestFS:$.Hello") };
    swi(XOS_Find, keep);
    uint32_t rnum[8] = { 16, TFS_NUMBER };
    int vn = swi(XOS_FSControl, rnum);
    uint32_t rm[8] = { 16, name("TestFS") };
    int vm = swi(XOS_FSControl, rm);
    uint32_t st[8] = { 254, keep[0] };
    swi(XOS_Args, st);
    uint32_t q[8] = { 5, name("TestFS:$.Hello") };
    int vq = swi(XOS_File, q);
    check(vn == 1 && !vm && st[0] == 0x800 && vq == 1 && errnum(q) == 0xF8,
          "Module FS -- OS_FSControl 16: by number refused; by name it goes, its files closed, "
          "then TestFS: is not present",
          "%d %d %X %d %s", vn, vm, st[0], vq, vq == 1 ? errmess(q) : "-");
    uint32_t back[8] = { 14, 220 };
    swi(XOS_FSControl, back);
    tfree(fsroot);
    fsroot = NULL;
}

/* A TIMG archive on the host, typed &1A0 */
static void write_image(const char *leaf)
{
    struct tnode *r = tnew(NULL, "$", 1);
    struct tnode *rd = tnew(r, "Readme", 0);
    tsetdata(rd, "inside the image\n", 17);
    rd->load = 0xFFFFFF00u;
    struct tnode *sub = tnew(r, "Sub", 1);
    struct tnode *deep = tnew(sub, "Deep", 0);
    tsetdata(deep, "deeper", 6);
    uint32_t len;
    uint8_t *b = make_archive(r, &len);
    char p[1024];
    snprintf(p, sizeof p, "%s/%s,1a0", root, leaf);
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(b, 1, len, f);
        fclose(f);
    }
    free(b);
    tfree(r);
}

static void image_fs(void)
{
    char t[512];
    uint32_t r[8];
    write_image("Img");
    uint32_t before = info("HostFS::Test.$.Img", r);

    add_ifs_block();
    uint32_t a[8] = { 35, 0, ifs_block, 0x5678 };
    int v = swi(XOS_FSControl, a);
    uint32_t after = info("HostFS::Test.$.Img", r);
    uint32_t ft[8] = { 20, name("HostFS::Test.$.Img") };
    swi(XOS_File, ft);
    check(before == 1 && !v && after == 3 && (r[2] >> 8 & 0xFFF) == IMG_TYPE && ft[6] == 0x1000 &&
              calls.newimage == 0,
          "Image FS -- OS_FSControl 35: a file of type &1A0 is then object type 3 (OS_File 5), "
          "OS_File 20's type &1000, and not opened for that",
          "before %u, %d, after %u, type %X, 20: %X, NewImage %u", before, v, after,
          r[2] >> 8 & 0xFFF, ft[6], calls.newimage);

    uint32_t ir = info("HostFS::Test.$.Img.Readme", r);
    check(ir == 1 && r[4] == 17 && calls.newimage == 1 && calls.newimage_handle_ok,
          "Image FS -- a path through it opens it (NewImage, R1 a FileSwitch handle on the file: "
          "OS_Args 2, OS_GBPB 3, OS_Args 7 work on it); Img.Readme is a 17-byte file",
          "%u %u NewImage %u handle %d (%s)", ir, r[4], calls.newimage, calls.newimage_handle_ok,
          imgs[0].name);

    list("HostFS::Test.$.Img", t, sizeof t);
    char t2[512];
    list("HostFS::Test.$.Img.Sub", t2, sizeof t2);
    check(!strcmp(t, "Readme:1,Sub:2") && !strcmp(t2, "Deep:1"),
          "Image FS -- OS_GBPB 10 lists the image's root and a directory in it", "%s; %s", t, t2);

    char t3[2048];
    list("HostFS::Test.$", t3, sizeof t3);
    check(strstr(t3, "Img:3") != NULL, "Image FS -- its parent lists the image as object type 3",
          "%s", t3);

    check(load_text("HostFS::Test.$.Img.Readme", t, sizeof t) && !strcmp(t, "inside the image\n"),
          "Image FS -- OS_File 255 of a file in it: Open, GetBytes, Close", "%s", t);

    uint32_t o[8] = { 0x4C, name("HostFS::Test.$.Img.Sub.Deep") };
    v = swi(XOS_Find, o);
    uint32_t g[8] = { 4, o[0], text + 1024, 6 };
    if (!v)
        swi(XOS_GBPB, g);
    uint32_t c7[8] = { 7, o[0], text + 1536, 0, 0, 256 };
    swi(XOS_Args, c7);
    int ok = !v && !memcmp(ros_ptr(text + 1024), "deeper", 6) &&
             !strcmp(ros_ptr(text + 1536), "HostFS::Test.$.Img.Sub.Deep");
    uint32_t cl[8] = { 0, o[0] };
    swi(XOS_Find, cl);
    check(ok, "Image FS -- a stream on Img.Sub.Deep: its bytes, its canonical name", "%d %s", v,
          (char *)ros_ptr(text + 1536));

    /* *Copy out of the image: the bytes on the host */
    uint32_t cp[8] = { 26, name("HostFS::Test.$.Img.Readme"), name2("HostFS::Test.$.Out"), 0x2 };
    v = swi(XOS_FSControl, cp);
    char p[1024], hb[64] = "";
    snprintf(p, sizeof p, "%s/Out,fff", root);
    FILE *f = fopen(p, "rb");
    if (!f) {
        snprintf(p, sizeof p, "%s/Out", root);
        f = fopen(p, "rb");
    }
    size_t hn = f ? fread(hb, 1, sizeof hb - 1, f) : 0;
    if (f)
        fclose(f);
    hb[hn] = 0;
    check(!v && !strcmp(hb, "inside the image\n"), "Image FS -- *Copy out of it: the bytes on the host",
          "%d %s", v, hb);

    /* writing: OS_File 10 into it, a directory, then read back */
    uint32_t sv[8] = { 10, name("HostFS::Test.$.Img.Written"), 0xFFF, 0, name2("written in"),
                       name2("written in") + 10 };
    int vs = swi(XOS_File, sv);
    uint32_t cd[8] = { 8, name("HostFS::Test.$.Img.Sub.New") };
    int vc = swi(XOS_File, cd);
    list("HostFS::Test.$.Img", t, sizeof t);
    check(!vs && !vc && !strcmp(t, "Readme:1,Sub:2,Written:1") &&
              load_text("HostFS::Test.$.Img.Written", t2, sizeof t2) && !strcmp(t2, "written in"),
          "Image FS -- OS_File 10 into it (create, open, PutBytes, close), *CDir in it",
          "%d %d %s / %s", vs, vc, t, t2);

    /* Service_CloseFile naming the image: ImageClosing, the archive written
     * back through the handle.  The file then reads as the new archive */
    unsigned closing = calls.imageclosing;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = 0x68, c.r[2] = name("HostFS::Test.$.Img"), c.r[3] = 0;
    ros_service_call(&c);
    uint32_t streams[8] = { 254, imgs[0].swh };
    swi(XOS_Args, streams);
    check(calls.imageclosing == closing + 1 && c.r[3] == 1 && streams[0] == 0x800,
          "Image FS -- Service_CloseFile on the image: ImageClosing, its stream closed, R3 counted",
          "closing %u, R3 %u, %X", calls.imageclosing - closing, c.r[3], streams[0]);
    unsigned newi = calls.newimage;
    list("HostFS::Test.$.Img.Sub", t, sizeof t);
    check(calls.newimage == newi + 1 && !strcmp(t, "Deep:1,New:2"),
          "Image FS -- opened again: what was written is in the archive on the host", "%s", t);

    /* the image file opened as a file closes the image first */
    closing = calls.imageclosing;
    uint32_t oi[8] = { 0x4C, name("HostFS::Test.$.Img") };
    v = swi(XOS_Find, oi);
    uint32_t gi[8] = { 4, oi[0], text + 1024, 4 };
    if (!v)
        swi(XOS_GBPB, gi);
    uint32_t cli[8] = { 0, oi[0] };
    swi(XOS_Find, cli);
    check(!v && calls.imageclosing == closing + 1 && !memcmp(ros_ptr(text + 1024), "TIMG", 4),
          "Image FS -- OpenIn of the image file: the image closed first (TryGetFileClosed), "
          "its own bytes read",
          "%d closing %u", v, calls.imageclosing - closing);

    /* *Cat into it; *Shut closes images too */
    info("HostFS::Test.$.Img.Readme", r);
    closing = calls.imageclosing;
    uint32_t sh[8] = { 0, 0 };
    swi(XOS_Find, sh);
    check(calls.imageclosing == closing + 1, "Image FS -- OS_Find 0,0 closes the image too",
          "closing %u", calls.imageclosing - closing);

    /* removed: the file is a file again */
    info("HostFS::Test.$.Img.Readme", r);
    closing = calls.imageclosing;
    uint32_t rm[8] = { 36, IMG_TYPE };
    v = swi(XOS_FSControl, rm);
    uint32_t gone = info("HostFS::Test.$.Img", r);
    uint32_t inside = info("HostFS::Test.$.Img.Readme", r);
    uint32_t rm2[8] = { 36, IMG_TYPE };
    int v2 = swi(XOS_FSControl, rm2);
    check(!v && calls.imageclosing == closing + 1 && gone == 1 && inside == 0 && v2 == 1,
          "Image FS -- OS_FSControl 36: its open image closed, the file a file again, a path "
          "into it nothing; again, an error",
          "%d closing %u, %u %u, %d", v, calls.imageclosing - closing, gone, inside, v2);
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            remove_tree(p);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

void ros_selftest_imagefs(void)
{
    text = ros_addr(ros_rma_alloc(4096));
    errblocks = ros_addr(ros_rma_alloc(2048));
    fs_block = ros_addr(ros_rma_alloc(256));
    ifs_block = ros_addr(ros_rma_alloc(64));
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-imagefs-XXXXXX", tmp);
    if (!mkdtemp(root)) {
        check(0, "Image FS -- no directory for the test disc", "no directory for the test disc");
        return;
    }
    ros_hostfs_mount("Test", root);
    uint32_t r[8] = { 14, 220 };
    swi(XOS_FSControl, r);

    module_fs();
    image_fs();

    uint32_t cl[8] = { 0, 0 };
    swi(XOS_Find, cl);
    ros_hostfs_unmount("Test");
    remove_tree(root);
}
