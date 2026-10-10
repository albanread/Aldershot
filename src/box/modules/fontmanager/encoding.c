/* Copyright 1996 Acorn Computers Ltd
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
 * This file is a reimplementation in C of RISC OS Open's ARM assembler source
 * (Sources/Video/Render/Fonts/FontManager: s.Encoding).
 */

/* encoding.c: encodings, which map a character code to a glyph
 * (s/Encoding).
 *
 * A font's target encoding maps codes to glyph names. It is the \E field,
 * or the alphabet for a language font. Its base encoding maps the names to
 * the glyphs in its files. The base encoding is /Base<n> for an
 * IntMetric<n> font, or /Default, or the font's own Encoding file.
 *
 * Both files are read as the original reads them. Comments start with %
 * or #. The forms [hh;]hhhh;name and /name are accepted, and so are the
 * names uniXXXX and uXXXX. Anything the reader cannot read ends the file.
 *
 * The mapping is kept as the original keeps it. It is a list of runs of
 * consecutive codes. Each run has either a constant offset or a table of
 * offsets. The runs are split where the original splits them, because
 * Font_EnumerateCharacters shows where the runs begin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/international.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fm.h"

/* ---- the files' characters --------------------------------------------------------------------- */

/* isalphanumeric: PostScript's name characters, except ';'.  Codes from &80
 * up are not name characters. */
static int alnum(uint8_t c)
{
    static const uint32_t tab[4] = { 0, 0xA7FF7CDE, 0xD7FFFFFF, 0xD7FFFFFF };
    return c < 128 && (tab[c >> 5] >> (c & 31) & 1);
}

/* getcharfromR2: gets the next character.  A comment (from % or # to the
 * end of its line) is read as the control character that ends it.  The
 * original keeps the number from a %%RISCOS_Alphabet line, but nothing
 * reads it. */
static uint8_t getc_enc(const uint8_t **pp)
{
    uint8_t c = *(*pp)++;
    if (c != '%' && c != '#')
        return c;
    if (c == '%')
        for (const char *s = "%RISCOS_Alphabet "; *s; s++)
            if ((c = *(*pp)++) != (uint8_t)*s)
                break;
    while (c >= 32)
        c = *(*pp)++;
    return c;
}

/* readhex: reads a number with OS_ReadUnsigned in base 16, at most &FFFF,
 * and then a ';'.  It returns the number of characters read before the
 * ';', or -1 if there is an error or no ';'. */
static int readhex(const uint8_t *p, uint32_t *v, const uint8_t **after)
{
    uint32_t r[10] = { 16 | 1u << 29, ros_addr(p), 0xFFFF };
    os_error *e;
    fm_swi(XOS_ReadUnsigned, r, &e);
    if (e)
        return -1;
    const uint8_t *q = ros_ptr(r[1]);
    if (*q != ';')
        return -1;
    *v = r[2];
    *after = q + 1;
    return (int)(q - p);
}

static int hexdigit(uint8_t c)                  /* readhexdigitforuni: accepts upper case only */
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* findidentifier: sets *pp to the next identifier.  *code is its explicit
 * code, or -1.  *uni is its Unicode value, from a uniXXXX name or a name
 * from uXXXX to uXXXXXXXX, or -1.  The result is 0 at the end of the file,
 * or at what the original reports as an error.  Every caller takes that
 * as the end of the file. */
static int find_identifier(const uint8_t **pp, int32_t *code, int32_t *uni)
{
    *code = -1, *uni = -1;
    uint8_t c;
    do
        c = getc_enc(pp);
    while (c != '/' && c != 0 && c <= ' ');
    if (c == 0)
        return 0;
    if (c != '/') {
        uint32_t v;
        const uint8_t *after;
        int n = readhex(*pp - 1, &v, &after);
        if (n == 4) {                           /* hhhh;name: the name is not checked for Unicode */
            *code = (int32_t)v, *pp = after;
            return 1;
        }
        if (n != 2 || readhex(after, &v, &after) != 4)
            return 0;                           /* "should be preceded by '/'" */
        *code = (int32_t)v, *pp = after;
    }
    const uint8_t *p = *pp;
    if (p[0] != 'u')
        return 1;
    const uint8_t *q = p[1] == 'n' && p[2] == 'i' ? p + 3 : p + 1;
    uint32_t v = 0;
    for (int d; (d = hexdigit(*q)) >= 0; q++)
        v = v << 4 | (uint32_t)d;
    long k = q - p;                             /* the digits, plus 3 for "uni" or plus 1 for "u" */
    if (p[1] == 'n' ? k != 4 + 3 : k > 8 + 1 || k < 4 + 1 || (k != 4 + 1 && p[1] == '0'))
        return 1;
    if (*q && !alnum(*q))
        *uni = (int32_t)v;
    return 1;
}

/* findnextidentifier: moves past this identifier to the next one.  After a
 * ';' it also skips the rest of the line. */
static int next_identifier(const uint8_t **pp, int32_t *code, int32_t *uni)
{
    const uint8_t *back;
    uint8_t c;
    do {
        back = *pp;
        c = getc_enc(pp);
    } while (alnum(c));
    if (c == 0)
        return 0;                               /* "Not enough identifiers" */
    if (c == ';')
        while (**pp >= 32)
            (*pp)++;
    else
        *pp = back;
    return find_identifier(pp, code, uni);
}

static unsigned idhash(const uint8_t *p)       /* getidentifierhash */
{
    unsigned h = 0;
    uint8_t c;
    while (alnum(c = getc_enc(&p)))
        h = ((h << 1) ^ (h >> 5)) ^ c;
    return h & 63;
}

static int id_equal(const uint8_t *a, const uint8_t *b)       /* compareidentifiers */
{
    for (;; a++, b++) {
        uint8_t x = alnum(*a) ? *a : 0, y = alnum(*b) ? *b : 0;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/* ---- the mappings ---------------------------------------------------------------------------- */

struct run {                                    /* lkent */
    int32_t min;
    uint32_t n;
    uint16_t off;                               /* a constant run's offset */
    uint16_t *tab;                              /* a table run's offsets (lktab), or NULL */
};

struct fm_map {
    struct fm_map *next;
    uint8_t target[12];
    int32_t base;
    struct fm_font *master;                     /* a private base's font, else NULL */
    uint32_t nruns;
    struct run *runs;
};
static struct fm_map *maps;

static void free_map(struct fm_map *m)
{
    for (uint32_t i = 0; i < m->nruns; i++)
        free(m->runs[i].tab);
    free(m->runs);
    free(m);
}

void fm_forget_maps(struct fm_font *master)
{
    for (struct fm_map **pm = &maps; *pm;)
        if ((*pm)->master == master) {
            struct fm_map *m = *pm;
            *pm = m->next;
            free_map(m);
        } else {
            pm = &(*pm)->next;
        }
}

struct pair { int32_t from; uint32_t to; };    /* srcdst */
#define DST_IS_UNI 0x80000000u

static int cmp_pair(const void *a, const void *b)                /* compare_srcdst */
{
    const struct pair *x = a, *y = b;
    if (x->from != y->from)
        return x->from < y->from ? -1 : 1;
    int32_t p = (int32_t)x->to, q = (int32_t)y->to;
    return p < q ? -1 : p > q;
}

static void append(struct run *r, uint16_t off)
{
    r->tab = realloc(r->tab, sizeof *r->tab * (r->n + 1));
    r->tab[r->n++] = off;
}

/* The runs, made from the sorted pairs as Encoding's first pass finds
 * them. A new constant run starts at a gap, when the run is close to &8000
 * long, or when the offset changes after four or more codes. If the offset
 * changes sooner, the run becomes a table. When four equal offsets end a
 * table, they are split off as a constant run. */
static void build_runs(struct fm_map *m, const struct pair *v, size_t n)
{
    struct run *runs = calloc(n + 1, sizeof *runs), *cur = NULL;
    uint32_t nr = 0;
    int32_t start = -1, next = -1;
    uint16_t last = 0;
    /* same is 0 for a constant run. For a table it counts the equal offsets
     * at the table's end. */
    int same = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t from = v[i].from;
        uint16_t off = (uint16_t)(v[i].to - (uint32_t)from);
        if (from < 0 || from < next)            /* (no real encoding has negative codes) */
            continue;
        int fresh = from > next || ((uint32_t)(next - start) + 2) & 0x8000;
        if (!fresh && same) {
            append(cur, off);
            if (off != last) {
                last = off, same = 1;
            } else if (++same == 4) {
                cur->n -= 4;
                cur = &runs[nr++];
                cur->min = start = from - 3, cur->n = 4, cur->off = off;
                same = 0;
            }
        } else if (!fresh && off == last) {
            cur->n++;
        } else if (!fresh && next - start < 4) {
            uint32_t k = cur->n;
            cur->tab = malloc(sizeof *cur->tab * (k + 1));
            for (uint32_t j = 0; j < k; j++)
                cur->tab[j] = cur->off;
            cur->tab[k] = off;
            cur->n = k + 1;
            last = off, same = 1;
        } else {
            cur = &runs[nr++];
            cur->min = start = from, cur->n = 1, cur->off = off;
            last = off, same = 0;
        }
        next = from + 1;
    }
    m->runs = runs, m->nruns = nr;
}

static uint32_t run_offset(const struct run *r, uint32_t d)
{
    return r->tab ? r->tab[d] : r->off;
}

/* The binary chop that the lookups share.  It returns the run that c may
 * be in, or -1 if there are no runs. */
static int32_t chop(const struct fm_map *m, int32_t c)
{
    int32_t lo = 0, hi = (int32_t)m->nruns - 1;
    if (hi < 0)
        return -1;
    while (lo < hi) {
        int32_t mid = (lo + hi + 1) >> 1;
        if (c >= m->runs[mid].min) {
            lo = mid;
            if (c == m->runs[mid].min)
                break;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

/* ---- the files -------------------------------------------------------------------------------- */

/* getencodingfilelength and loadencodingfile: loads the file into the RMA,
 * zero-terminated, because OS_ReadUnsigned reads it there */
static os_error *load_file(const char *path, uint8_t **data)
{
    uint32_t name = ros_addr(ros_rma_alloc((uint32_t)strlen(path) + 1));
    strcpy(ros_ptr(name), path);
    uint32_t r[10] = { 17, name };
    os_error *e;
    fm_swi(XOS_File, r, &e);
    uint32_t len = !e && r[0] == 1 ? r[4] : 0;
    uint8_t *buf = NULL;
    if (len > 0x7FFFFFFFu)
        e = fm_err(FE_NOROOM, NULL, NULL);
    if (!e && !(buf = ros_rma_alloc(len + 1)))
        e = fm_err(FE_NOROOM, NULL, NULL);
    if (!e) {
        uint32_t q[10] = { 16, name, ros_addr(buf), 0 };
        fm_swi(XOS_File, q, &e);
    }
    ros_rma_free(ros_ptr(name));
    if (e) {
        if (buf)
            ros_rma_free(buf);
        return e;
    }
    buf[len] = 0;
    *data = buf;
    return NULL;
}

/* getencodingpath */
static void encoding_path(const struct fm_block *b, char *out, size_t size)
{
    snprintf(out, size, "%sEncodings.%s", b->prefix->prefix, b->id);
}

static const char *font_dir(const struct fm_font *f, int outlines)
{
    return outlines && f->pathname2 ? f->pathname2 : f->pathname;
}

/* getbaseencodingpath: the base the master's files use */
static os_error *base_path(const struct fm_font *master, const char *target, char *out,
                           size_t size)
{
    char id[16];
    if (master->base == BASE_PRIVATE) {
        snprintf(out, size, "%sEncoding", font_dir(master, 0));
        return NULL;
    }
    if (master->base >= 0) {
        snprintf(id, sizeof id, "/Base%d", (int)master->base);
    } else if (master->base == BASE_DEFAULT) {
        strcpy(id, "/Default");
    } else {
        char name[256];
        fm_name_from_id(target, 1, name, sizeof name);
        return fm_err(FE_NOBASEENC, name, NULL);
    }
    struct fm_block *b;
    os_error *e = fm_find_block(id, 1, &b);
    if (!e)
        encoding_path(b, out, size);
    return e;
}

/* ---- getmapping_fromR6 ------------------------------------------------------------------------- */

struct entry { const uint8_t *id; int32_t code; int referenced; int hnext; };

static os_error *new_map(struct fm_font *f, struct fm_font *master, struct fm_map **out)
{
    char target[13], tpath[320], bpath[320];
    memcpy(target, f->encoding, 12), target[12] = 0;
    struct fm_block *tb;
    uint8_t *tdata = NULL, *bdata = NULL;
    os_error *e = fm_find_block(target, 1, &tb);
    if (!e)
        encoding_path(tb, tpath, sizeof tpath), e = load_file(tpath, &tdata);
    int is_unicode = !memcmp(f->encoding, "utf8", 4);
    if (!e && !(e = base_path(master, target, bpath, sizeof bpath)))
        e = load_file(bpath, &bdata);
    if (e) {
        if (tdata)
            ros_rma_free(tdata);
        return e;
    }

    /* the base: from names (or Unicode values) to glyphs */
    size_t ne = 0, np = 0, ecap = 64, pcap = 64;
    struct entry *ents = malloc(ecap * sizeof *ents);
    struct pair *v = malloc(pcap * sizeof *v);
    int hash[64];
    for (int h = 0; h < 64; h++)
        hash[h] = -1;
#define ADD(f_, t_) do { if (np == pcap) v = realloc(v, (pcap *= 2) * sizeof *v); \
                         v[np].from = (f_), v[np].to = (t_), np++; } while (0)
    const uint8_t *p = bdata;
    int32_t counter = 0, code, uni;
    for (int ok = find_identifier(&p, &code, &uni); ok; ok = next_identifier(&p, &code, &uni)) {
        int32_t glyph = code == -1 ? counter++ : code;
        if (*p == '.')                          /* .notdef */
            continue;
        if (uni != -1 && is_unicode) {
            ADD(uni, (uint32_t)glyph | DST_IS_UNI);
            continue;
        }
        if (ne == ecap)
            ents = realloc(ents, (ecap *= 2) * sizeof *ents);
        unsigned h = idhash(p);
        ents[ne] = (struct entry){ p, glyph, 0, hash[h] };
        hash[h] = (int)ne++;
    }

    /* the target: from codes to names, and so to glyphs */
    p = tdata, counter = 0;
    for (int ok = find_identifier(&p, &code, &uni); ok; ok = next_identifier(&p, &code, &uni)) {
        int32_t ext = code < 0 ? counter++ : code;
        if (*p == '.')
            continue;
        int i = hash[idhash(p)];
        while (i >= 0 && !id_equal(ents[i].id, p))
            i = ents[i].hnext;
        if (i < 0)
            continue;
        ents[i].referenced = 1;
        ADD(ext, (uint32_t)ents[i].code);
    }

    /* Unicode: the glyphs that nothing named, numbered from &E000 */
    if (is_unicode) {
        int32_t u = 0xE000;
        for (size_t i = 0; i < ne && u < 0xF000; i++)
            if (!ents[i].referenced)
                ADD(u++, (uint32_t)ents[i].code);
    }
#undef ADD

    /* Sort, and keep the first of each code.  That is the uniXXXX name's, or
     * else the lowest glyph. */
    qsort(v, np, sizeof *v, cmp_pair);
    size_t k = 0;
    int32_t prev = -1;
    for (size_t i = 0; i < np; i++)
        if (v[i].from != prev) {
            prev = v[i].from;
            v[k].from = prev, v[k].to = v[i].to & ~DST_IS_UNI, k++;
        }

    struct fm_map *m = calloc(1, sizeof *m);
    memcpy(m->target, f->encoding, 12);
    m->base = master->base;
    m->master = master->base == BASE_PRIVATE ? master : NULL;
    build_runs(m, v, k);
    m->next = maps, maps = m;
    free(v), free(ents);
    ros_rma_free(tdata), ros_rma_free(bdata);
    *out = m;
    return NULL;
}

/* f's mapping.  It is NULL when the font is used directly.  That means
 * there is no encoding, and f->base is then BASE_NONE. */
static os_error *get_mapping(struct fm_font *f, struct fm_map **out)
{
    *out = NULL;
    if (!memcmp(f->encoding, "\0\0\0\0", 4)) {
        f->base = BASE_NONE;
        return NULL;
    }
    struct fm_font *master = f;
    if (f->masterfont) {
        os_error *e = fm_font_ptr(f->masterfont, &master);
        if (e)
            return e;
    }
    f->base = master->base;
    for (struct fm_map *m = maps; m; m = m->next)
        if (!memcmp(m->target, f->encoding, 12) && m->base == master->base &&
            m->master == (master->base == BASE_PRIVATE ? master : NULL)) {
            *out = m;
            return NULL;
        }
    return new_map(f, master, out);
}

uint32_t fm_map_r14;

/* mapchar: the glyph for code c in f, -1 if none */
os_error *fm_map_char(struct fm_font *f, int32_t c, int32_t *out)
{
    struct fm_map *m;
    os_error *e = get_mapping(f, &m);
    if (e)
        return e;
    fm_map_r14 = 0;                     /* (what getmapping leaves in R14 is not modelled) */
    *out = c;
    if (!m)
        return NULL;
    *out = -1;
    int32_t lo = chop(m, c);
    if (lo < 0)
        return NULL;
    const struct run *r = &m->runs[lo];
    uint32_t d = (uint32_t)c - (uint32_t)r->min;
    fm_map_r14 = d;
    if ((uint32_t)c >= (uint32_t)r->min && d < r->n) {
        *out = (int32_t)(((uint32_t)c + run_offset(r, d)) & 0xFFFF);
        if (r->tab)
            fm_map_r14 = r->tab[d] >> 8;
    }
    return NULL;
}

/* ---- GetFontBaseEncoding ---------------------------------------------------------------------- */

/* The base that the master's files use.  IntMetric<n> gives /Base<n>.
 * With IntMetrics, the base is the font's own Encoding file if it has one,
 * and otherwise /Default. */
os_error *fm_base_encoding(struct fm_font *f)
{
    struct fm_font *m = f;
    os_error *e;
    if (f->masterfont && (e = fm_font_ptr(f->masterfont, &m)))
        return e;
    if (m->base != BASE_UNKNOWN) {
        f->base = m->base;
        return NULL;
    }
    const char *dir = m->pathname;
    while (*dir == ' ')
        dir++;
    size_t n = strlen(dir);
    uint32_t dname = ros_addr(ros_rma_alloc((uint32_t)n + 1));
    uint32_t buf = ros_addr(ros_rma_alloc(64)), wild = ros_addr(ros_rma_alloc(8));
    memcpy(ros_ptr(dname), dir, n);
    ((char *)ros_ptr(dname))[n ? n - 1 : 0] = 0;           /* without its final '.' */
    strcpy(ros_ptr(wild), "IntMet*");
    int32_t base = BASE_NONE;
    uint32_t r[10] = { 0 };
    do {
        r[0] = 9, r[1] = dname, r[2] = buf, r[3] = 1, r[5] = 64, r[6] = wild;
        fm_swi(XOS_GBPB, r, &e);
    } while (!e && r[3] == 0 && r[4] != 0xFFFFFFFFu);
    if (!e && r[3]) {
        const char *q = (const char *)ros_ptr(buf) + 1;
        while (*q && (*q < '0' || *q > '9'))
            q++;
        if (*q)
            base = (int32_t)strtoul(q, NULL, 10);
    }
    ros_rma_free(ros_ptr(dname)), ros_rma_free(ros_ptr(buf)), ros_rma_free(ros_ptr(wild));
    if (e)
        return e;
    if (base < 0) {
        char path[320];
        const char *pp = path;
        snprintf(path, sizeof path, "%sEncoding", m->pathname);
        while (*pp == ' ')
            pp++;
        uint32_t name = ros_addr(ros_rma_alloc((uint32_t)strlen(pp) + 1));
        strcpy(ros_ptr(name), pp);
        uint32_t q[10] = { 17, name };
        fm_swi(XOS_File, q, &e);
        ros_rma_free(ros_ptr(name));
        if (e)
            return e;
        base = q[0] == 1 ? BASE_PRIVATE : BASE_DEFAULT;
    }
    m->base = f->base = base;
    return NULL;
}

/* ---- getencodingid ------------------------------------------------------------------------------ */

/* getalphabet_R1: gets the alphabet number from OS_Byte 71 and its name
 * from Service_International 3, which the International module answers.
 * If either fails, the name is "Latin1". */
static void alphabet(char out[17])
{
    strcpy(out, "Latin1");
    uint32_t r[10] = { 71, 127 };
    os_error *e;
    fm_swi(XOS_Byte, r, &e);
    if (e || !r[1])
        return;
    uint32_t buf = ros_addr(ros_rma_alloc(16));
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_INTERNATIONAL, s.r[2] = INTER_ANO_TO_ANA, s.r[3] = r[1], s.r[4] = buf, s.r[5] = 16;
    ros_service_call(&s);
    if (s.r[1] == 0) {
        uint32_t n = s.r[5] < 16 ? s.r[5] : 16;
        memcpy(out, ros_ptr(buf), n);
        out[n] = 0;
    }
    ros_rma_free(ros_ptr(buf));
}

/* The encoding that a font string asks for.  It is the string's \E field.
 * If there is none, it is the alphabet for a language font, or UTF8 for any
 * font if UTF8 is the alphabet.  Otherwise there is no encoding. */
os_error *fm_encoding_id(const char *s, uint8_t enc[12])
{
    memset(enc, 0, 12);
    const char *p = fm_find_field(s, 'E');
    char alpha[17];
    if (!p) {
        struct fm_block *b;
        os_error *e = fm_find_block_field(s, 0, &b);
        if (e)
            return e;
        alphabet(alpha);
        if (!(b->flags & LFFF_LANGFONT) && strcmp(alpha, "UTF8"))
            return NULL;
        p = alpha;
    }
    for (int i = 0; i < 12; i++) {
        uint8_t c = (uint8_t)*p;
        if (c == '\\' || c <= ' ')
            continue;                           /* (zero-padded) */
        p++;
        enc[i] = fm_lower(c);
    }
    if (!memcmp(enc, "glyph\0\0\0", 8)) {
        memset(enc, 0, 12);
        return NULL;
    }
    char id[13];
    memcpy(id, enc, 12), id[12] = 0;
    struct fm_block *b;
    return fm_find_block(id, 1, &b);
}

/* ---- the SWIs ------------------------------------------------------------------------------------ */

/* Font_ReadEncodingFilename: gives the target encoding's file.  For a font
 * without an encoding it gives the Encoding file beside its outlines. */
void fm_thunk_ReadEncodingFilename(struct ros_cpu *s)
{
    uint32_t keep = fm_currentfont;
    fm_currentfont = s->r[0] & 255;
    struct fm_font *f, *m;
    char path[330];
    os_error *e = fm_font_ptr(s->r[0], &f);
    if (!e && !memcmp(f->encoding, "\0\0\0\0", 4)) {
        m = f;
        if (f->masterfont)
            e = fm_font_ptr(f->masterfont, &m);
        if (!e)
            snprintf(path, sizeof path, "%sEncoding", font_dir(m, 1));
    } else if (!e) {
        char id[13];
        memcpy(id, f->encoding, 12), id[12] = 0;
        struct fm_block *b;
        if (!(e = fm_find_block(id, 1, &b)))
            encoding_path(b, path, sizeof path);
    }
    fm_currentfont = keep;
    if (e) {
        ros_swi_fail(s, e);
        return;
    }
    const char *p = path;                       /* copytobuffer_skipspaces */
    while (*p == ' ')
        p++;
    uint32_t n = (uint32_t)strlen(p) + 1;
    if (n > s->r[2]) {
        memcpy(ros_ptr(s->r[1]), p, s->r[2]);
        ros_swi_fail(s, fm_err(FE_BUFFOVERFLOW, NULL, NULL));
        return;
    }
    memcpy(ros_ptr(s->r[1]), p, n);
    s->r[0] = s->r[1];
    s->r[1] += n - 1;
    s->r[2] -= n - 1;
    s->v = 0;
}

/* Font_EnumerateCharacters: from code R1, returns the next code that is
 * mapped and R1's glyph.  R0 is left as the code for mapped fonts, or as
 * the master's handle. */
void fm_thunk_EnumerateCharacters(struct ros_cpu *s)
{
    uint32_t c = s->r[1];
    if (c > 0x80000000u) {
        ros_swi_fail(s, fm_err(FE_BADCHARCODE, NULL, NULL));
        return;
    }
    uint32_t keep = fm_currentfont, h = s->r[0];
    if (h)
        fm_currentfont = h & 255;
    else
        h = keep;
    struct fm_font *f, *mf;
    struct fm_map *m = NULL;
    os_error *e = fm_font_ptr(h, &f);
    if (!e)
        e = get_mapping(f, &m);
    int overflow = 0;
    if (!e && !m) {                             /* direct: the characters of the master's chunks */
        s->r[0] = f->masterfont;
        if (!(e = fm_font_ptr(f->masterfont, &mf))) {
            uint32_t max = mf->leaf1.nchunks * 32 - 1;          /* (if none is known, all are taken) */
            s->r[2] = c <= max ? c : 0xFFFFFFFFu;
            s->r[1] = c < max ? c + 1 : 0xFFFFFFFFu;
            /* Its CMP code,max sets V when that overflows.  The SWI returns
             * with V set, and R0 is the master's handle, as for an error. */
            int64_t d = (int64_t)(int32_t)c - (int32_t)max;
            overflow = d < INT32_MIN || d > INT32_MAX;
        }
    } else if (!e) {
        s->r[0] = c;
        s->r[1] = s->r[2] = 0xFFFFFFFFu;
        int32_t lo = chop(m, (int32_t)c);
        if (lo >= 0) {
            const struct run *r = &m->runs[lo];
            uint32_t d = c - (uint32_t)r->min, nxt = c == 0 ? (uint32_t)lo : (uint32_t)lo + 1;
            if (c >= (uint32_t)r->min && d < r->n)
                s->r[2] = (c + run_offset(r, d)) & 0xFFFF, s->r[1] = c + 1;
            else if (nxt < m->nruns)
                s->r[1] = (uint32_t)m->runs[nxt].min;
        }
    }
    fm_currentfont = keep;
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = (uint32_t)overflow;
}
