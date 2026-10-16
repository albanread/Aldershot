/* templates.c: template files.  This holds Wimp_OpenTemplate,
 * Wimp_CloseTemplate and Wimp_LoadTemplate.
 *
 * One template file is open for the whole desktop.  It is read whole into
 * the RMA.  5.30 reads a ResourceFS file in place and any other file
 * whole, but the difference cannot be seen.  LoadTemplate copies an entry
 * out of the file.  It binds the entry's fonts and places its indirected
 * data. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

#define E_NO_TEMPLATE 0x28Cu
#define E_TEMPLATE_EOF 0x2A4u
#define E_BAD_FONTS 0x28Eu

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

static void close_file(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w->tfile)
        xos_module_free(ros_ptr(w->tfile));
    w->tfile = 0;
    w->tsize = 0;
}

void wimp_swi_OpenTemplate(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    memset(w->tfonts, 0, sizeof w->tfonts);    /* clear the font bindings */
    close_file();                               /* close any open file */
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17, c.r[1] = s->r[1];
    if (!call(XOS_File, &c)) {
        wimp_fail(s, ros_ptr(c.r[0]));
        return;
    }
    if (c.r[0] != 1) {
        uint32_t type = c.r[0];
        ros_cpu_enter(&c);
        c.r[0] = 19, c.r[1] = s->r[1], c.r[2] = type;
        ros_swi(&c, XOS_File);
        wimp_fail(s, c.v ? ros_ptr(c.r[0]) : wimp_error(E_NO_TEMPLATE));
        return;
    }
    uint32_t size = c.r[4];
    if (size < 16) {
        wimp_fail(s, ros_error(E_TEMPLATE_EOF, "End of file found while reading template file"));
        return;
    }
    void *mem;
    os_error *e = xos_module_claim(size, &mem);
    if (e) {
        wimp_fail(s, e);
        return;
    }
    ros_cpu_enter(&c);
    c.r[0] = 16, c.r[1] = s->r[1], c.r[2] = ros_addr(mem), c.r[3] = 0;
    if (!call(XOS_File, &c)) {
        xos_module_free(mem);
        wimp_fail(s, ros_ptr(c.r[0]));
        return;
    }
    w->tfile = ros_addr(mem);
    w->tsize = size;
    s->v = 0;
}

void wimp_swi_CloseTemplate(struct ros_cpu *s)
{
    close_file();
    s->v = 0;
}

/* Match a template name.  Case is ignored (bit 5 of each character).  #
 * matches one character and * matches any run of characters. */
static int match(uint32_t pat, const uint8_t *name)
{
    uint32_t p = 0, n = 0, star_p = 0xFFFFFFFFu, star_n = 0;
    for (;;) {
        uint32_t pc = ros_ld8(pat + p);
        uint32_t nc = n < 12 ? name[n] : 0;
        if (nc < 32)
            nc = 0;
        if (pc < 32) {
            if (nc == 0)
                return 1;
        } else if (pc == '*') {
            star_p = ++p, star_n = n;
            continue;
        } else if (nc != 0 && (pc == '#' || (pc | 0x20u) == (nc | 0x20u))) {
            p++, n++;
            continue;
        }
        if (star_p == 0xFFFFFFFFu || star_n >= 12 || name[star_n] < 32)
            return 0;
        p = star_p, n = ++star_n;
    }
}

static uint32_t strlen_ctrl(uint32_t a, uint32_t end)
{
    uint32_t n = 0;
    while (a + n < end && ros_ld8(a + n) >= 32)
        n++;
    return n + 1;                               /* and the control terminator */
}

/* Place the indirected data of one icon, or of the title.  The data is
 * the three words at data. */
static os_error *indirect(uint32_t flags, uint32_t data, uint32_t entry, uint32_t esize,
                          uint32_t *r2, uint32_t r3, int load, uint32_t *need)
{
    if (!(flags & (1u << 8)))
        return NULL;
    int32_t len = (int32_t)ros_ld32(data + 8);
    uint32_t a = ros_ld32(data), v = ros_ld32(data + 4), end = entry + esize;
    if (len > 0) {
        if (!load) {
            *need += (uint32_t)len;
        } else {
            if (a >= esize)
                return ros_error(E_TEMPLATE_EOF, "End of file found while reading template file");
            if (*r2 + (uint32_t)len > r3)
                return ros_error(E_TOO_BIG, "Not enough memory to create this window or menu");
            uint32_t n = strlen_ctrl(entry + a, end);
            if (n > (uint32_t)len)
                n = (uint32_t)len;
            memcpy(ros_ptr(*r2), ros_ptr(entry + a), n);
            ros_st32(data, *r2);
            *r2 += (uint32_t)len;
        }
    }
    if ((int32_t)v > 87) {
        if (v >= esize)
            return ros_error(E_TEMPLATE_EOF, "End of file found while reading template file");
        uint32_t n = strlen_ctrl(entry + v, end);
        if (!load) {
            *need += n;
        } else {
            if (*r2 + n > r3)
                return ros_error(E_TOO_BIG, "Not enough memory to create this window or menu");
            memcpy(ros_ptr(*r2), ros_ptr(entry + v), n);
            ros_st32(data + 4, *r2);
            *r2 += n;
        }
    }
    return NULL;
}

/* Bind a template's font number to a font handle. */
static os_error *font(uint32_t flagsaddr, uint32_t counts)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t flags = ros_ld32(flagsaddr);
    if (!(flags & (1u << 6)))
        return NULL;
    uint32_t n = flags >> 24;
    if (counts == 0xFFFFFFFFu || counts == 0 || n == 0)
        return ros_error(E_BAD_FONTS, "Font handle in template not found or invalid");
    uint32_t h = w->tfonts[n];
    if (!h) {
        int32_t fonts = (int32_t)ros_ld32(w->tfile);
        uint32_t rec = (uint32_t)fonts + 48 * (n - 1);
        if (fonts <= 0 || rec + 48 > w->tsize)
            return ros_error(E_TEMPLATE_EOF, "End of file found while reading template file");
        uint8_t *name = (uint8_t *)w->scratch;
        uint32_t k = 0;
        for (; k < 40 && ros_ld8(w->tfile + rec + 8 + k) >= 32; k++)
            name[k] = (uint8_t)ros_ld8(w->tfile + rec + 8 + k);
        name[k] = 0;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = ros_addr(name), c.r[2] = ros_ld32(w->tfile + rec);
        c.r[3] = ros_ld32(w->tfile + rec + 4), c.r[4] = c.r[5] = 0;
        if (!call(XFont_FindFont, &c))
            return ros_ptr(c.r[0]);
        h = c.r[0] & 0xFFu;
        uint32_t count = ros_ld8(counts + h);
        if (count >= 255)
            return ros_error(E_BAD_FONTS, "Font handle in template not found or invalid");
        ros_st8(counts + h, count + 1);
        w->tfonts[n] = h;
    }
    ros_st32(flagsaddr, (flags & 0x00FFFFFFu) | (h << 24));
    return NULL;
}

void wimp_swi_LoadTemplate(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    if (!w->tfile) {
        wimp_fail(s, ros_error(0xDE, "Handle is either illegal or has been closed"));
        return;
    }
    uint32_t r1 = s->r[1], r5 = s->r[5];
    int query = (int32_t)r1 <= 0;
    uint32_t pos = s->r[6] ? s->r[6] : 16;
    for (;; pos += 24) {
        if (pos + 24 > w->tsize) {
            wimp_fail(s, ros_error(E_TEMPLATE_EOF, "End of file found while reading template file"));
            return;
        }
        uint32_t ix = w->tfile + pos, off = ros_ld32(ix);
        if (off == 0)
            break;
        uint8_t name[12];
        memcpy(name, ros_ptr(ix + 12), 12);
        if (!match(r5, name))
            continue;
        uint32_t esize = ros_ld32(ix + 4), type = ros_ld32(ix + 8);
        if (off + esize > w->tsize) {
            wimp_fail(s, ros_error(E_TEMPLATE_EOF, "End of file found while reading template file"));
            return;
        }
        /* If the name in R5 has a wildcard and R5 is word-aligned, the
         * name found is written back there. */
        int wild = 0;
        for (uint32_t k = 0; k < 12 && ros_ld8(r5 + k) >= 32; k++)
            if (ros_ld8(r5 + k) == '*' || ros_ld8(r5 + k) == '#')
                wild = 1;
        if (wild && !(r5 & 3u))
            memcpy(ros_ptr(r5), name, 12);
        uint32_t entry = w->tfile + off;
        if (query) {                            /* R1 <= 0: sizes only */
            uint32_t need = 0;
            if (type == 1 && esize >= 88) {
                indirect(ros_ld32(entry + 56), entry + 72, entry, esize, NULL, 0, 0, &need);
                uint32_t n = ros_ld32(entry + 84);
                for (uint32_t i = 0; i < n && 88 + 32 * (i + 1) <= esize; i++)
                    indirect(ros_ld32(entry + 88 + 32 * i + 16), entry + 88 + 32 * i + 20, entry, esize,
                             NULL, 0, 0, &need);
            }
            s->r[1] = esize;
            s->r[2] = type == 1 ? need : 0;
            s->r[6] = pos + 24;
            s->v = 0;
            return;
        }
        memcpy(ros_ptr(r1), ros_ptr(entry), esize);  /* copy the entry */
        if (type == 1 && esize >= 88) {
            uint32_t r2 = s->r[2], r3 = s->r[3], r4 = s->r[4], need = 0;
            os_error *e = font(r1 + 56, r4);
            if (!e)
                e = indirect(ros_ld32(r1 + 56), r1 + 72, entry, esize, &r2, r3, 1, &need);
            uint32_t n = ros_ld32(r1 + 84);
            for (uint32_t i = 0; !e && i < n && 88 + 32 * (i + 1) <= esize; i++) {
                uint32_t ic = r1 + 88 + 32 * i;
                e = font(ic + 16, r4);
                if (!e)
                    e = indirect(ros_ld32(ic + 16), ic + 20, entry, esize, &r2, r3, 1, &need);
            }
            if (e) {
                wimp_fail(s, e);
                return;
            }
            s->r[2] = r2;
        }
        ros_st32(r1 + 64, 1);                   /* sprite area 1: the Wimp's */
        s->r[6] = pos + 24;
        s->v = 0;
        return;
    }
    if (!query)
        ros_st32(r1 + 64, 1);
    s->r[6] = 0;
    s->v = 0;
}
