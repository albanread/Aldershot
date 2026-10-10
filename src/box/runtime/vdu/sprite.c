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
 * This file is a reimplementation in C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.vdu.vdugrafg, s.vdu.vdugrafh, s.vdu.vdugrafi,
 * s.vdu.vdugrafj).
 */
/* sprite.c -- OS_SpriteOp: SpriteV, the dispatcher, and the operations on
 * sprite areas.
 *
 * This follows the kernel's Kernel/s/vdu/vdugrafg, vdugrafh, vdugrafi and
 * vdugrafj.
 *
 *   - OS_SpriteOp calls SpriteV. The kernel's code here is the vector's
 *     default owner, as on RISC OS. SpriteExtend claims the vector for its
 *     reasons (17, 35-38, 50-58, 65), and a host renderer can claim it
 *     later. Nothing reaches the sprite code except through the vector.
 *   - R0 &000+n names a sprite in the system sprite area (dynamic area 3).
 *     &100+n names a sprite in a user area. &200+n gives a user area and
 *     a sprite pointer. The kernel's group checks and errors are made in
 *     its order.
 *   - Names are up to 12 characters, ended by any control character or
 *     space. A-Z is lower-cased. A name is compared as three words against
 *     the stored name.
 *   - The area operations are read, clear, load, merge, save, names,
 *     create, select, delete, rename, copy, masks, flips, pixels, size and
 *     left-hand wastage. They leave an area byte for byte as the kernel
 *     does, quirks and all.
 *
 * Plotting and capture are in sprplot.c. Switching output is in sprout.c.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "vduws.h"
#include "sprite.h"

#define SPRITEV 0x1Fu

/* ---- errors ---------------------------------------------------------------------- */

os_error *spr_err(uint32_t n)
{
    switch (n) {
    case ERR_SPR_NOWORKSPACE:
        return ros_error(n, "No memory is reserved for the system sprite area. Use the Task "
                            "Manager to make some space for the system sprites.");
    case ERR_SPR_NOTGRAPHICS: return ros_error(n, "Not a graphics mode");
    case ERR_SPR_NOROOM: return ros_error(n, "Not enough memory to create sprite");
    case ERR_SPR_NOSPRITES: return ros_error(n, "No sprites");
    case ERR_SPR_NOTENOUGHROOM: return ros_error(n, "Not enough memory in sprite area");
    case ERR_SPR_DOESNTEXIST: return ros_error(n, "Sprite doesn't exist");
    case ERR_SPR_BADFILE: return ros_error(n, "Bad sprite file");
    case ERR_SPR_NOROOMTOMERGE: return ros_error(n, "Not enough memory to add sprite");
    case ERR_SPR_ROWCOL: return ros_error(n, "Invalid row or column");
    case ERR_SPR_NOROOMTOINSERT:
        return ros_error(n, "Not enough memory to insert row or column");
    case ERR_SPR_EXISTS: return ros_error(n, "Sprite already exists");
    case ERR_SPR_BADMODE: return ros_error(n, "Invalid sprite mode");
    case ERR_SPR_BADREASON: return ros_error(n, "Bad sprite reason code");
    case ERR_SPR_BADSAVEAREA: return ros_error(n, "Invalid save area");
    case ERR_SPR_ISDEST: return ros_error(n, "Sprite is current destination");
    case ERR_SPR_NOMASK:
        return ros_error(n, "Mask or Palette operations not supported in this display depth");
    case ERR_SPR_BADDPI: return ros_error(n, "Illegal XDPI or YDPI in sprite");
    default: return ros_error(n, "Sprite error");
    }
}

/* ---- the system area and SpChoose ------------------------------------------------ */

void ros_vdu_sprite_area(uint32_t area)
{
    vdu.sp_area = area;
}

void vdu_sprite_init(void)
{
    vdu.dest_select = 0x23C;
    vdu.sp_choose = 0;
    memset(vdu.sp_choose_name, 0, 12);
    vdu.sp_choose_name[12] = 13;
}

static void kill_choose(void)
{
    vdu.sp_choose = 0;
}

/* ---- memory moves, keeping the output sprite where it went ------------------------- */

void spr_move(uint32_t to, uint32_t from, uint32_t bytes)
{
    if (!bytes || to == from)
        return;
    memmove(ros_ptr(to), ros_ptr(from), bytes);
    vdu_sprite_moved(from, to, bytes, (int32_t)(to - from));
}

/* The kernel's CopyDown and CopyUp adjustment. The output sprite inside
 * the moved block moves with it. */
void vdu_sprite_moved(uint32_t from, uint32_t to, uint32_t bytes, int32_t by)
{
    (void)to;
    uint32_t v = vdu.dest_sprite;
    if (!v || v < from || v > from + bytes - 1)
        return;
    vdu.dest_sprite += (uint32_t)by;
    vdu.screen_start += (uint32_t)by;
    vdu.screen = ros_ptr(vdu.screen_start);
}

/* ---- names ---------------------------------------------------------------------------- */

void spr_get_name(uint32_t p, uint8_t key[12])
{
    int i;
    memset(key, 0, 12);
    for (i = 0; i < 12; i++) {
        uint8_t c = (uint8_t)ros_ld8(p + (uint32_t)i);
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c <= 32)
            break;
        key[i] = c;
    }
}

uint32_t spr_find_key(uint32_t area, const uint8_t key[12])
{
    uint32_t n = ros_ld32(area + SA_NUMBER), p = area + ros_ld32(area + SA_FIRST);
    for (; n; n--, p += ros_ld32(p + SP_NEXT))
        if (!memcmp(ros_ptr(p + SP_NAME), key, 12))
            return p;
    return 0;
}

uint32_t spr_find(uint32_t area, uint32_t name)
{
    uint8_t key[12];
    spr_get_name(name, key);
    return spr_find_key(area, key);
}

/* ---- the sprite's mode (SetupSprModeData) ---------------------------------------------- */

static uint32_t field_ones(uint32_t l2)
{
    uint32_t bits = 1u << l2;
    return bits >= 32 ? ~0u : (1u << bits) - 1;
}

os_error *spr_mode(uint32_t m, struct spr_mode *d)
{
    uint32_t mv[MV_COUNT];
    if (m >= 256 && !(m & 1))
        return spr_err(ERR_SPR_BADMODE);
    os_error *e = vdu_mode_vars(m, mv);
    if (e)
        return m < 256 ? spr_err(ERR_SPR_BADMODE) : e;
    if (mv[MV_FLAGS] & MF_NONGRAPHIC)
        return spr_err(ERR_SPR_BADMODE);
    d->flags = mv[MV_FLAGS];
    d->l2bpc = mv[MV_LOG2BPC], d->l2bpp = mv[MV_LOG2BPP];
    d->xshft = 5 - d->l2bpc;
    d->npix = (1u << d->xshft) - 1;
    d->bpc = 1u << d->l2bpc;
    d->read_nc = field_ones(d->l2bpp);
    d->write_nc = field_ones(d->l2bpc);
    d->ncolour = mv[MV_NCOLOUR];
    d->xeig = mv[MV_XEIG], d->yeig = mv[MV_YEIG];
    return NULL;
}

/* Gives Log2BPP by a mode word's type (NSM_bpptable). */
uint32_t spr_type_bpp(uint32_t type)
{
    static const uint8_t t[19] = { 0, 0, 1, 2, 3, 4, 5, 5, 5, 5, 4, 5, 5, 5, 5, 5, 4, 5, 5 };
    return type < 19 ? t[type] : 5;
}

/* Gives the type field, which is bits 27-30, or bits 20-26 when that is
 * 15. */
uint32_t spr_type(uint32_t m)
{
    uint32_t t = (m >> 27) & 15;
    return t == 15 ? (m >> 20) & 127 : t;
}

/* Whether a sprite's header describes an image that lies inside the
 * sprite. A sprite is the program's memory and its header can say
 * anything. The drawing code works from the header, so a header that
 * overstates the sprite is refused before it is used. */
int spr_geometry_ok(uint32_t sp)
{
    uint32_t next = ros_ld32(sp + SP_NEXT), image = ros_ld32(sp + SP_IMAGE);
    uint32_t trans = ros_ld32(sp + SP_TRANS), w = ros_ld32(sp + SP_WIDTH);
    uint32_t h = ros_ld32(sp + SP_HEIGHT);
    if (image < 44 || image > next || trans < 44 || trans > next)
        return 0;
    if (w > 0x0FFFFFFFu || h > 0x0FFFFFFFu || ros_ld32(sp + SP_LBIT) > 31 ||
        ros_ld32(sp + SP_RBIT) > 31)
        return 0;
    return (uint64_t)(w + 1u) * (h + 1u) * 4 <= next - image;
}

/* Whether w words in each of rows rows from p lie inside the arena. */
static int row_fits(uint32_t p, uint32_t w, uint32_t rows)
{
    return (uint64_t)p + 4ull * w * rows <= 0x100000000ull;
}

/* GetMaskspWidth. This gives a new-format mask's words per row, and its
 * last bit. */
void spr_mask_width(uint32_t sp, uint32_t *words, uint32_t *lastbit)
{
    uint32_t m = ros_ld32(sp + SP_MODE), l2 = spr_type_bpp(spr_type(m));
    uint32_t px = (ros_ld32(sp + SP_WIDTH) << (5 - l2)) + ((ros_ld32(sp + SP_RBIT) + 1) >> l2);
    uint32_t bits = m & 0x80000000u ? px * 8 : px;
    *words = (bits + 31) / 32;
    *lastbit = ((bits & 31) - 1) & 31;
}

/* DecideMaskSize, in words. */
static uint32_t mask_words(uint32_t sp)
{
    uint32_t m = ros_ld32(sp + SP_MODE);
    if (!(m >> 27))
        return (ros_ld32(sp + SP_NEXT) - ros_ld32(sp + SP_TRANS)) / 4;
    uint32_t w, lb;
    spr_mask_width(sp, &w, &lb);
    return w * (ros_ld32(sp + SP_HEIGHT) + 1);
}

/* ---- growing and shrinking a sprite ------------------------------------------------------- */

/* ExtendSprite. This adds n words at the end of the sprite. The gap is not
 * cleared. */
os_error *spr_extend(uint32_t area, uint32_t sp, uint32_t n)
{
    uint32_t bytes = 4 * n, end = ros_ld32(area + SA_END), free = ros_ld32(area + SA_FREE);
    if (end - bytes < free || bytes > end)
        return spr_err(ERR_SPR_NOROOMTOINSERT);
    uint32_t tail = sp + ros_ld32(sp + SP_NEXT);
    spr_move(tail + bytes, tail, area + free - tail);
    ros_st32(area + SA_FREE, free + bytes);
    ros_st32(sp + SP_NEXT, ros_ld32(sp + SP_NEXT) + bytes);
    return NULL;
}

/* RemoveWords. This removes n words at offset off from the image. */
void spr_remove_words(uint32_t area, uint32_t sp, uint32_t n, uint32_t off)
{
    uint32_t at = sp + ros_ld32(sp + SP_IMAGE) + off, bytes = 4 * n;
    uint32_t free = area + ros_ld32(area + SA_FREE);
    spr_move(at, at + bytes, free - (at + bytes));
    ros_st32(area + SA_FREE, free - bytes - area);
    ros_st32(sp + SP_NEXT, ros_ld32(sp + SP_NEXT) - bytes);
}

/* ---- the header: SanitizeSGetMode, PreCreateHeader, CreateHeader ------------------------- */

os_error *spr_sanitize_mode(uint32_t *mode)
{
    uint32_t m = *mode, mv[MV_COUNT];
    if (m >= 256) {
        if (m & 1)
            return NULL;
        os_error *e = vdu_mode_vars(m, mv);
        if (e)
            return e;
    } else {
        if (vdu_mode_vars(m, mv))
            return ros_error(0x19, "Not enough memory to change to this screen mode");
        if (mv[MV_LOG2BPP] < 4)
            return NULL;
    }
    if (mv[MV_LOG2BPC] != mv[MV_LOG2BPP])
        return ros_error(0x19, "Not enough memory to change to this screen mode");
    uint32_t flags = mv[MV_FLAGS], n = mv[MV_NCOLOUR] + 1, type;
    if (n == 0) type = 6;
    else if (n == 1u << 24) type = 8;
    else if (n == 4096) type = 16;
    else if (n == 2) type = 1;
    else if (n == 4) type = 2;
    else if (n == 16) type = 3;
    else if (n == 256 || n == 64) type = 4;
    else if (n == 65536) type = flags & MF_FULLPALETTE ? 10 : 5;
    else return ros_error(0x19, "Not enough memory to change to this screen mode");
    uint32_t df = flags & 0xF000;
    if (df || type >= 16)
        *mode = 15u << 27 | type << 20 | df | 1 | mv[MV_XEIG] << 4 | mv[MV_YEIG] << 6;
    else
        *mode = type << 27 | 1 | (180u >> mv[MV_XEIG]) << 1 | (180u >> mv[MV_YEIG]) << 14;
    return NULL;
}

/* PreCreateHeader. This works out the geometry of a sprite of the
 * rectangle l,b..r,t in the sprite's own pixels. For GetSprite it also
 * works out the sprite's margins against the window. */
os_error *spr_pre_header(struct spr_get *g, uint32_t pal, int32_t l, int32_t b, int32_t r,
                         int32_t t)
{
    uint32_t tm = g->mode & ~0x80000000u;
    uint32_t tt = tm < (15u << 27) ? tm >> 7 : tm & (127u << 20);
    if (tt >= 5u << 20)
        pal = 0;
    struct spr_mode d;
    os_error *e = spr_mode(g->mode, &d);
    if (e)
        return e;
    uint32_t nc = d.read_nc;
    if (!(vdu.mv[MV_FLAGS] & MF_FULLPALETTE))
        nc &= 63;
    uint32_t entries = (pal ? 0x3FFu : 0) & (nc + 1);
    g->pal = pal;
    g->image = 44 + 8 * entries;
    g->height = t - b;
    g->width = (r >> d.xshft) - (l >> d.xshft);
    g->lbit = ((uint32_t)l & d.npix) << d.l2bpc;
    g->rbit = ((((uint32_t)r & d.npix) + 1) << d.l2bpc) - 1;
    if (g->width >= 0 && g->height >= 0 &&
        ((uint64_t)g->width + 1) * ((uint64_t)g->height + 1) * 4 + g->image > 0xFFFFFFFFu)
        return spr_err(ERR_SPR_NOROOM);
    g->size = g->image + 4 * (uint32_t)(g->width + 1) * (uint32_t)(g->height + 1);
    g->d = d;

    /* The margins against the graphics window, for GetSprite. */
    int32_t rows = g->height + 1, words = g->width + 1;
    if (t > vdu.gwt) {
        g->top_margin = t - vdu.gwt < rows ? t - vdu.gwt : rows;
        g->top_row = vdu.gwt;
    } else {
        g->top_margin = 0;
        g->top_row = t;
    }
    int32_t bm = vdu.gwb - b > 0 ? vdu.gwb - b : 0;
    g->bot_margin = bm < rows ? bm : rows;
    int32_t cl = l > vdu.gwl ? l : vdu.gwl, cr = r < vdu.gwr ? r : vdu.gwr;
    int32_t lw = (cl >> d.xshft) - (l >> d.xshft), rw = (r >> d.xshft) - (cr >> d.xshft);
    g->lw_margin = lw < words ? lw : words;
    g->lb_margin = ((uint32_t)cl & d.npix) << d.l2bpc;
    g->rw_margin = rw < words ? rw : words;
    g->rb_margin = ((((uint32_t)cr & d.npix) + 1) << d.l2bpc) - 1;
    g->clip_l = cl;
    return NULL;
}

/* WritePaletteToSprite. This writes the current palette, as OS_ReadPalette
 * gives it. */
void spr_write_palette(uint32_t sp, const struct spr_mode *d)
{
    uint32_t n = d->read_nc;
    if (!(vdu.mv[MV_FLAGS] & MF_FULLPALETTE))
        n &= 63;
    for (uint32_t i = n + 1; i-- > 0;) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = i, s.r[1] = 16;
        ros_swi(&s, XOS_ReadPalette);
        ros_st32(sp + 44 + 8 * i, s.r[2]);
        ros_st32(sp + 44 + 8 * i + 4, s.r[3]);
    }
}

/* Writes a header for g at sp, and its palette if it has one. */
void spr_write_header(uint32_t sp, const struct spr_get *g, const uint8_t key[12])
{
    ros_st32(sp + SP_NEXT, g->size);
    memcpy(ros_ptr(sp + SP_NAME), key, 12);
    ros_st32(sp + SP_WIDTH, (uint32_t)g->width);
    ros_st32(sp + SP_HEIGHT, (uint32_t)g->height);
    ros_st32(sp + SP_LBIT, g->lbit);
    ros_st32(sp + SP_RBIT, g->rbit);
    ros_st32(sp + SP_IMAGE, g->image);
    ros_st32(sp + SP_TRANS, g->image);
    ros_st32(sp + SP_MODE, g->mode);
    uint32_t tm = g->mode & ~0x80000000u;
    uint32_t tt = tm < (15u << 27) ? tm >> 7 : tm & (127u << 20);
    if (g->pal && tt < 5u << 20)
        spr_write_palette(sp, &g->d);
}

/* CreateHeader at the area's free space. It gives the NoRoom error if the
 * header will not fit. */
os_error *spr_create_header(uint32_t area, const struct spr_get *g, const uint8_t key[12],
                            uint32_t *sp)
{
    uint32_t end = ros_ld32(area + SA_END), free = ros_ld32(area + SA_FREE);
    if (end - free < g->size || free > end)
        return spr_err(ERR_SPR_NOROOM);
    *sp = area + free;
    spr_write_header(*sp, g, key);
    return NULL;
}

/* DeleteSpriteByName and DeleteSprite. */
void spr_delete(uint32_t area, uint32_t sp)
{
    uint32_t next = sp + ros_ld32(sp + SP_NEXT), free = area + ros_ld32(area + SA_FREE);
    spr_move(sp, next, free - next);
    ros_st32(area + SA_NUMBER, ros_ld32(area + SA_NUMBER) - 1);
    ros_st32(area + SA_FREE, sp - area + (free - next));
    kill_choose();
}

/* ---- the operations ------------------------------------------------------------------------- */

static os_error *swi_call(uint32_t n, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

/* LoadFile. This loads a sprite file's contents at addr, in the area. */
static os_error *load_file(uint32_t area, uint32_t name, uint32_t addr)
{
    uint32_t free = area + ros_ld32(area + SA_END) - addr;
    uint32_t r[10] = { 5, name };
    os_error *e = swi_call(XOS_File, r);
    if (e)
        return e;
    if (r[0] != 1) {
        uint32_t m[10] = { 19, name, r[0] };
        e = swi_call(XOS_File, m);
        return e ? e : spr_err(ERR_SPR_BADFILE);
    }
    uint32_t length = r[4];
    if (free < length)
        return spr_err(ERR_SPR_NOTENOUGHROOM);
    uint32_t l[10] = { 255, name, addr, 0 };
    if ((e = swi_call(XOS_File, l)) != NULL)
        return e;
    if (ros_ld32(addr + 4) < length)
        return NULL;
    ros_st32(addr, 0), ros_st32(addr + 4, 16), ros_st32(addr + 8, 16);
    return spr_err(ERR_SPR_BADFILE);
}

/* AddSprite. This adds a copy of src at the end of the area. */
static os_error *add_sprite(uint32_t area, uint32_t src)
{
    uint32_t size = ros_ld32(src + SP_NEXT), free = ros_ld32(area + SA_FREE);
    if (ros_ld32(area + SA_END) - free < size)
        return spr_err(ERR_SPR_NOROOMTOMERGE);
    spr_move(area + free, src, size);
    ros_st32(area + SA_NUMBER, ros_ld32(area + SA_NUMBER) + 1);
    ros_st32(area + SA_FREE, free + size);
    return NULL;
}

static os_error *merge_areas(uint32_t dest, uint32_t src)
{
    uint32_t read = dest + ros_ld32(dest + SA_FIRST), write = read;
    uint32_t count = ros_ld32(dest + SA_NUMBER);
    for (uint32_t n = ros_ld32(dest + SA_NUMBER); n; n--) {
        uint32_t size = ros_ld32(read + SP_NEXT);
        if (spr_find_key(src, ros_ptr(read + SP_NAME))) {
            count--;
            read += size;
        } else if (read == write) {
            read += size, write += size;
        } else {
            spr_move(write, read, size);
            read += size, write += size;
        }
    }
    uint32_t nfree = write - dest;
    if (nfree != ros_ld32(dest + SA_FREE)) {
        ros_st32(dest + SA_NUMBER, count);
        ros_st32(dest + SA_FREE, nfree);
        kill_choose();
    }
    /* The source sits in the free space above. Appending copies downwards,
     * so each source sprite is read before it is overwritten. */
    uint32_t p = src + ros_ld32(src + SA_FIRST);
    for (uint32_t n = ros_ld32(src + SA_NUMBER); n; n--) {
        uint32_t size = ros_ld32(p + SP_NEXT);
        os_error *e = add_sprite(dest, p);
        if (e)
            return e;
        p += size;
    }
    return NULL;
}

/* Reverses a row of w words at p in pixels of bpp bits. */
static uint32_t reverse_word(uint32_t v, uint32_t bpp)
{
    if (bpp >= 32)
        return v;
    uint32_t out = 0, m = (1u << bpp) - 1;
    for (uint32_t i = 0; i < 32; i += bpp)
        out |= ((v >> i) & m) << (32 - bpp - i);
    return out;
}

static void reverse_rows(uint32_t p, uint32_t w, uint32_t rows, uint32_t bpp)
{
    if (!row_fits(p, w, rows))
        return;
    for (uint32_t r = 0; r < rows; r++, p += 4 * w) {
        uint32_t *row = ros_ptr(p);
        for (uint32_t i = 0, j = w - 1; i <= j && j < w; i++, j--) {
            uint32_t a = reverse_word(row[i], bpp), b = reverse_word(row[j], bpp);
            row[i] = b, row[j] = a;
            if (j == 0)
                break;
        }
    }
}

static void shift_rows_left(uint32_t p, uint32_t w, uint32_t rows, uint32_t l)
{
    if (!row_fits(p, w, rows))
        return;
    for (uint32_t r = 0; r < rows; r++, p += 4 * w) {
        uint32_t *row = ros_ptr(p);
        for (uint32_t j = 0; j + 1 < w; j++)
            row[j] = row[j] >> l | row[j + 1] << (32 - l);
        row[w - 1] >>= l;
    }
}

static void flip_x_rows(uint32_t p, uint32_t w, uint32_t rows)
{
    if (!row_fits(p, w, rows))
        return;
    uint32_t *a = ros_ptr(p);
    for (uint32_t i = 0, j = rows - 1; i < j; i++, j--)
        for (uint32_t k = 0; k < w; k++) {
            uint32_t t = a[i * w + k];
            a[i * w + k] = a[j * w + k], a[j * w + k] = t;
        }
}

/* SpriteGenAddr. This gives the word and bit of pixel (x, y). It returns
 * -1 if the pixel is outside the sprite. */
static int gen_addr(uint32_t sp, const struct spr_mode *d, int32_t x, int32_t y, int mask,
                    uint32_t *addr, uint32_t *bit)
{
    uint32_t h = ros_ld32(sp + SP_HEIGHT), w = ros_ld32(sp + SP_WIDTH), rb = ros_ld32(sp + SP_RBIT);
    uint32_t l2 = d->l2bpc, xs = d->xshft, np = d->npix, base = ros_ld32(sp + SP_IMAGE);
    uint32_t m = ros_ld32(sp + SP_MODE);
    if (mask) {
        if (m >> 27) {
            spr_mask_width(sp, &w, &rb);
            w -= 1;
            if (m & 0x80000000u)
                l2 = 3, xs = 2, np = 3;
            else
                l2 = 0, xs = 5, np = 31;
        }
        base = ros_ld32(sp + SP_TRANS);
    }
    if ((uint32_t)y > h)
        return -1;
    uint32_t r = h - (uint32_t)y;
    uint32_t b = (((uint32_t)x & np) << l2) + ros_ld32(sp + SP_LBIT);
    uint32_t word = (uint32_t)(x >> xs) + (b >> 5);
    b &= 31;
    if (word > w || (word == w && b > rb))
        return -1;
    *addr = sp + base + 4 * (r * (w + 1) + word);
    *bit = b;
    return 0;
}

static os_error *op_pixels(struct ros_cpu *s, uint32_t reason, uint32_t sp,
                           const struct spr_mode *d)
{
    uint32_t addr, bit;
    int has_mask = ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS);
    uint32_t m = ros_ld32(sp + SP_MODE);
    if (reason == 43 && !has_mask) {
        s->r[5] = m & 0x80000000u ? 255 : 1;
        return NULL;
    }
    if (reason == 44 && !has_mask)
        return NULL;
    if (gen_addr(sp, d, (int32_t)s->r[3], (int32_t)s->r[4], reason >= 43, &addr, &bit))
        return spr_err(ERR_SPR_ROWCOL);
    uint32_t w = ros_ld32(addr);
    switch (reason) {
    case 41: {
        uint32_t p = (w >> bit) & d->read_nc;
        if (d->read_nc != 255 || ros_ld32(sp + SP_IMAGE) - 44 == 2048) {
            s->r[5] = p, s->r[6] = 0;
        } else {
            s->r[6] = (p & 3) << 6;
            s->r[5] = (p >> 7 & 1) << 5 | (p >> 3 & 1) << 4 | (p >> 6 & 1) << 3 |
                      (p >> 5 & 1) << 2 | (p >> 4 & 1) << 1 | (p >> 2 & 1);
        }
        return NULL;
    }
    case 42: {
        uint32_t c = s->r[5] & d->read_nc, v;
        if (d->read_nc == 255) {
            uint32_t ps = ros_ld32(sp + SP_IMAGE) < ros_ld32(sp + SP_TRANS) ?
                          ros_ld32(sp + SP_IMAGE) : ros_ld32(sp + SP_TRANS);
            uint32_t byte;
            if (ps - 44 == 2048)
                byte = c;
            else
                byte = ((s->r[6] >> 6) & 3) | (c & 1) << 2 | (c & 0x20) << 2 | (c & 0x10) >> 1 |
                       (c & 0x0E) << 3;
            v = byte * 0x01010101u;
        } else if (d->read_nc < 255) {
            uint32_t full = d->read_nc == 1 ? 0xFF * c : d->read_nc == 3 ? 0x55 * c : 0x11 * c;
            v = full * 0x01010101u;
        } else {
            v = c;
        }
        uint32_t f = d->write_nc << bit;
        ros_st32(addr, (w & ~f) | ((v & d->write_nc) << bit));
        return NULL;
    }
    case 43:
        if (m & 0x80000000u)
            s->r[5] = (w >> bit) & 255;
        else if (m >> 27)
            s->r[5] = (w >> bit) & 1;
        else
            s->r[5] = (w >> bit) & d->read_nc ? 1 : 0;
        return NULL;
    default: {                                              /* 44 */
        uint32_t v, field;
        if (m & 0x80000000u)
            v = s->r[5], field = 255;
        else if (m >> 27)
            v = s->r[5] ? 1 : 0, field = 1;
        else
            field = d->write_nc, v = s->r[5] ? field : 0;
        ros_st32(addr, (w & ~(field << bit)) | (v << bit));
        return NULL;
    }
    }
}

/* RemoveLeftHandWastage (54). */
void spr_remove_lh_wastage(uint32_t area, uint32_t sp)
{
    uint32_t l = ros_ld32(sp + SP_LBIT);
    if (!l)
        return;
    ros_st32(sp + SP_LBIT, 0);
    uint32_t w = ros_ld32(sp + SP_WIDTH), p = sp + ros_ld32(sp + SP_IMAGE);
    uint32_t end = sp + ros_ld32(sp + SP_NEXT);
    for (; p < end; p += 4 * (w + 1))
        for (uint32_t j = 0; j <= w; j++)
            ros_st32(p + 4 * j, ros_ld32(p + 4 * j) >> l | ros_ld32(p + 4 * (j + 1)) << (32 - l));
    int32_t r = (int32_t)ros_ld32(sp + SP_RBIT) - (int32_t)l;
    if (r >= 0) {
        ros_st32(sp + SP_RBIT, (uint32_t)r);
        return;
    }
    ros_st32(sp + SP_RBIT, (uint32_t)(r + 32));
    int has_mask = ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS);
    uint32_t h = ros_ld32(sp + SP_HEIGHT) + 1, rows = has_mask ? 2 * h : h;
    if (has_mask)
        ros_st32(sp + SP_TRANS, ros_ld32(sp + SP_TRANS) - 4 * h);
    uint32_t src = sp + ros_ld32(sp + SP_IMAGE), dst = src;
    for (uint32_t i = 0; i < rows; i++) {
        memmove(ros_ptr(dst), ros_ptr(src), 4 * w);
        src += 4 * (w + 1), dst += 4 * w;
    }
    /* The freed tail is rows words, removed from the end of the sprite. */
    uint32_t tail_off = ros_ld32(sp + SP_NEXT) - ros_ld32(sp + SP_IMAGE) - 4 * rows;
    spr_remove_words(area, sp, rows, tail_off);
    ros_st32(sp + SP_WIDTH, w - 1);
}

static os_error *flip_y(uint32_t sp, const struct spr_mode *d)
{
    uint32_t w = ros_ld32(sp + SP_WIDTH) + 1, h = ros_ld32(sp + SP_HEIGHT) + 1;
    uint32_t lb = ros_ld32(sp + SP_LBIT), rb = ros_ld32(sp + SP_RBIT), m = ros_ld32(sp + SP_MODE);
    uint32_t nl = 31 - rb, nr = 31 - lb;
    ros_st32(sp + SP_LBIT, nl), ros_st32(sp + SP_RBIT, nr);
    uint32_t img = sp + ros_ld32(sp + SP_IMAGE);
    reverse_rows(img, w, h, d->bpc);
    if ((m >> 27) && nl) {
        shift_rows_left(img, w, h, nl);
        ros_st32(sp + SP_LBIT, 0), ros_st32(sp + SP_RBIT, rb);
    }
    if (ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS)) {
        uint32_t msk = sp + ros_ld32(sp + SP_TRANS);
        if (!(m >> 27)) {
            reverse_rows(msk, w, h, d->bpc);
        } else {
            uint32_t mw, last;
            spr_mask_width(sp, &mw, &last);
            reverse_rows(msk, mw, h, 1);                /* 1-bit, even for alpha masks (a kernel bug) */
            if (31 - last)
                shift_rows_left(msk, mw, h, 31 - last);
        }
    }
    return NULL;
}

/* Reasons 57 and 58, which the kernel's 31, 32, 45 and 46 call, through
 * SpriteV. */
static os_error *adjust_size(uint32_t reason, uint32_t area, uint32_t sp, uint32_t at,
                             int32_t by)
{
    uint32_t r[10] = { 0x200 + reason, area, sp, at, (uint32_t)by };
    return swi_call(XOS_SpriteOp, r);
}

/* ---- the dispatcher ---------------------------------------------------------------------------- */

enum {
    F_SCREEN_NOT_ALLOWED = 1, F_NEEDS_SOMETHING = 2, F_NEEDS_SPRITE = 4, F_NEEDS_MODE = 8,
    F_DANGER = 16, F_DANGER_AREA = 32,
    G1 = 1, G2 = 1 | 2, G3 = 1 | 2 | 4, G4 = 1 | 2 | 4 | 8, G5 = 2 | 4,
};

static const uint8_t op_flags[63] = {
    G1, G1, G1, G1, G1, G1, G1, G1,
    G2, G2 | F_DANGER_AREA, G2 | F_DANGER_AREA, G2 | F_DANGER_AREA, G2, G2, G2, G2,
    G2, G2, G2, G2, G2, G2, G2, G2,
    G3, G3 | F_DANGER, G3, G3, G3, G3, G3 | F_DANGER, G3 | F_DANGER,
    G3 | F_DANGER, G3, G3, G3, G3, G3, G3, G3,
    G4, G4, G4, G4, G4, G4 | F_DANGER, G4 | F_DANGER, G4 | F_DANGER,
    G4, G4, G4, G4, G4, G4, G4, G4,
    G4, G4, G4, G4, G5, G5, G5,
};

static os_error *dispatch(struct ros_cpu *s, uint32_t reason, uint32_t area, uint32_t sp)
{
    os_error *e;
    struct spr_mode d;
    switch (reason) {
    case 2: return spr_screen_save(s);
    case 3: return spr_screen_load(s);
    case 8:
        s->r[2] = ros_ld32(area + SA_END), s->r[3] = ros_ld32(area + SA_NUMBER);
        s->r[4] = ros_ld32(area + SA_FIRST), s->r[5] = ros_ld32(area + SA_FREE);
        return NULL;
    case 9:
        ros_st32(area + SA_FREE, ros_ld32(area + SA_FIRST));
        ros_st32(area + SA_NUMBER, 0);
        if (s->r[0] < 0x100) {
            vdu.sp_choose = 0;
            memset(vdu.sp_choose_name, 0, 12);
            vdu.sp_choose_name[12] = 13;
        }
        return NULL;
    case 10:
        kill_choose();
        return load_file(area, s->r[2], area + 4);
    case 11: {
        kill_choose();
        uint32_t at = area + ros_ld32(area + SA_FREE);
        if ((e = load_file(area, s->r[2], at)) != NULL)
            return e;
        return merge_areas(area, at - 4);
    }
    case 12: {
        if (!ros_ld32(area + SA_NUMBER))
            return spr_err(ERR_SPR_NOSPRITES);
        uint32_t r[10] = { 10, s->r[2], 0xFF9, 0, area + 4, area + ros_ld32(area + SA_FREE) };
        return swi_call(XOS_File, r);
    }
    case 13: {
        int32_t n = (int32_t)s->r[4];
        if (!(n >= 1 && (int32_t)ros_ld32(area + SA_NUMBER) >= n))
            return spr_err(ERR_SPR_DOESNTEXIST);
        uint32_t p = area + ros_ld32(area + SA_FIRST);
        while (--n)
            p += ros_ld32(p + SP_NEXT);
        uint32_t len = s->r[3], buf = s->r[2];
        if (len == 0) {
            s->r[3] = 0;
            return NULL;
        }
        uint32_t max = len - 1 < 12 ? len - 1 : 12, i = 0;
        for (; i < max; i++) {
            uint8_t c = (uint8_t)ros_ld8(p + SP_NAME + i);
            if (c <= 32)
                break;
            ros_st8(buf + i, c);
        }
        ros_st8(buf + i, 0);
        s->r[3] = i;
        return NULL;
    }
    case 14:
    case 16: return spr_get_sprite(s, reason, area);
    case 15: {
        uint8_t key[12];
        kill_choose();
        spr_get_name(s->r[2], key);
        uint32_t old = spr_find_key(area, key);
        if (old)
            spr_delete(area, old);
        struct spr_get g = { .mode = s->r[6] };
        if ((e = spr_sanitize_mode(&g.mode)) != NULL)
            return e;
        if ((e = spr_pre_header(&g, s->r[3], 0, 0, (int32_t)s->r[4] - 1,
                                (int32_t)s->r[5] - 1)) != NULL)
            return e;
        uint32_t nsp;
        if ((e = spr_create_header(area, &g, key, &nsp)) != NULL)
            return e;
        memset(ros_ptr(nsp + g.image), 0, g.size - g.image);
        ros_st32(area + SA_NUMBER, ros_ld32(area + SA_NUMBER) + 1);
        ros_st32(area + SA_FREE, ros_ld32(area + SA_FREE) + g.size);
        return NULL;
    }
    case 24:
        if (s->r[0] < 0x100) {
            vdu.sp_choose = sp;
            memcpy(vdu.sp_choose_name, ros_ptr(sp + SP_NAME), 12);
        } else {
            s->r[2] = sp;
        }
        return NULL;
    case 25:
        spr_delete(area, sp);
        return NULL;
    case 26:
    case 27: {
        uint8_t key[12];
        spr_get_name(s->r[3], key);
        /* If a sprite of the new name exists, nothing is done and there is
         * no error. The kernel's AlreadyExists tests "is it the same one"
         * and then returns VC either way (vdugrafg 855-858). */
        if (spr_find_key(area, key))
            return NULL;
        if (reason == 26) {
            memcpy(ros_ptr(sp + SP_NAME), key, 12);
            kill_choose();
            return NULL;
        }
        uint32_t at = area + ros_ld32(area + SA_FREE);
        if ((e = add_sprite(area, sp)) != NULL)
            return e;
        memcpy(ros_ptr(at + SP_NAME), key, 12);
        return NULL;
    }
    case 28:
    case 34: return spr_put(s, reason, sp);
    case 29: {
        kill_choose();
        if (!spr_geometry_ok(sp))
            return spr_err(ERR_SPR_BADFILE);
        uint32_t n = mask_words(sp);
        if (ros_ld32(sp + SP_IMAGE) == ros_ld32(sp + SP_TRANS)) {
            uint32_t next = ros_ld32(sp + SP_NEXT);
            if (spr_extend(area, sp, n))
                return spr_err(ERR_SPR_NOTENOUGHROOM);
            ros_st32(sp + SP_TRANS, next);
        }
        if ((uint64_t)sp + ros_ld32(sp + SP_TRANS) + 4ull * n > 0x100000000ull)
            return spr_err(ERR_SPR_NOTENOUGHROOM);
        memset(ros_ptr(sp + ros_ld32(sp + SP_TRANS)), 0xFF, 4 * n);
        return NULL;
    }
    case 30:
        kill_choose();
        if (ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS)) {
            spr_remove_words(area, sp, mask_words(sp),
                             ros_ld32(sp + SP_TRANS) - ros_ld32(sp + SP_IMAGE));
            ros_st32(sp + SP_TRANS, ros_ld32(sp + SP_IMAGE));
        }
        return NULL;
    case 31: return adjust_size(57, area, sp, s->r[3], 1);
    case 32: return adjust_size(57, area, sp, s->r[3], -1);
    case 45: return adjust_size(58, area, sp, s->r[3], 1);
    case 46: return adjust_size(58, area, sp, s->r[3], -1);
    case 33: {
        if (!spr_geometry_ok(sp))
            return spr_err(ERR_SPR_BADFILE);
        uint32_t w = ros_ld32(sp + SP_WIDTH) + 1, h = ros_ld32(sp + SP_HEIGHT) + 1;
        flip_x_rows(sp + ros_ld32(sp + SP_IMAGE), w, h);
        if (ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS)) {
            uint32_t mw = w, last;
            if (ros_ld32(sp + SP_MODE) >> 27)
                spr_mask_width(sp, &mw, &last);
            flip_x_rows(sp + ros_ld32(sp + SP_TRANS), mw, h);
        }
        return NULL;
    }
    case 40:
        if ((e = spr_mode(ros_ld32(sp + SP_MODE), &d)) != NULL)
            return e;
        s->r[3] = ((ros_ld32(sp + SP_WIDTH) + 1) * 32 - ros_ld32(sp + SP_LBIT) - 31 +
                   ros_ld32(sp + SP_RBIT)) >> d.l2bpc;
        s->r[4] = ros_ld32(sp + SP_HEIGHT) + 1;
        s->r[5] = ros_ld32(sp + SP_IMAGE) != ros_ld32(sp + SP_TRANS);
        s->r[6] = ros_ld32(sp + SP_MODE);
        return NULL;
    case 41: case 42: case 43: case 44:
        if ((e = spr_mode(ros_ld32(sp + SP_MODE), &d)) != NULL)
            return e;
        return op_pixels(s, reason, sp, &d);
    case 47:
        if ((e = spr_mode(ros_ld32(sp + SP_MODE), &d)) != NULL)
            return e;
        if (!spr_geometry_ok(sp))
            return spr_err(ERR_SPR_BADFILE);
        return flip_y(sp, &d);
    case 48:
    case 49: return spr_plot_mask(s, reason, sp);
    case 54:
        spr_remove_lh_wastage(area, sp);
        return NULL;
    case 60:
    case 61: return spr_switch_output(s, reason, area, sp);
    case 62:
        s->r[3] = 384;
        return NULL;
    default:
        return spr_err(ERR_SPR_BADREASON);
    }
}

/* SpriteV's default owner. */
static void sprite_op(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], area = r0 < 0x100 ? vdu.sp_area : s->r[1], sp = s->r[2];
    os_error *e = NULL;
    if (r0 >= 0x300 || (r0 & 0xFF) >= 63) {
        ros_swi_fail(s, spr_err(ERR_SPR_BADREASON));
        return;
    }
    uint32_t reason = r0 & 0xFF, f = op_flags[reason];
    if (!(f & F_SCREEN_NOT_ALLOWED) && sp == 0) {
        area = 0;
        goto go;
    }
    if (!(f & F_NEEDS_SOMETHING))
        goto go;
    if (area == 0) {
        e = spr_err(ERR_SPR_NOWORKSPACE);
        goto out;
    }
    if ((f & F_DANGER_AREA) && vdu.dest_area == area && vdu.dest_sprite) {
        e = spr_err(ERR_SPR_ISDEST);
        goto out;
    }
    if (!(f & F_NEEDS_SPRITE)) {
        if (reason == 15) {
            uint32_t mv[MV_COUNT];
            uint32_t l = vdu_mode_vars(s->r[6], mv) ? 0 : mv[MV_LOG2BPP];
            if (l >= 4 && s->r[3])
                e = spr_err(ERR_SPR_NOMASK);
        } else if ((reason == 14 || reason == 16) && vdu.mv[MV_LOG2BPP] >= 4 && s->r[3]) {
            e = spr_err(ERR_SPR_NOMASK);
        }
        goto check;
    }
    if (sp == 0) {
        e = spr_err(ERR_SPR_DOESNTEXIST);
        goto out;
    }
    if (r0 < 0x200 && (sp = spr_find(area, sp)) == 0) {
        e = spr_err(ERR_SPR_DOESNTEXIST);
        goto out;
    }
    {
        uint32_t m = ros_ld32(sp + SP_MODE), t = (m >> 27) & 15, need;
        if (t == 0) {
            uint32_t mv[MV_COUNT];
            need = vdu_mode_vars(m, mv) ? 1 : mv[MV_LOG2BPP] >= 4;
        } else {
            if (t == 15) {
                t = (m >> 20) & 127;
                if (t == 0)
                    t = 6;
            }
            need = t >= 5;
        }
        if (need) {
            int32_t a = (int32_t)ros_ld32(sp + SP_IMAGE), b = (int32_t)ros_ld32(sp + SP_TRANS);
            if ((a < b ? a : b) - 44 != 0) {
                e = spr_err(ERR_SPR_NOMASK);
                goto out;
            }
        }
    }
    if ((f & F_DANGER) && vdu.dest_sprite == sp) {
        e = spr_err(ERR_SPR_ISDEST);
        goto out;
    }
    if (f & F_NEEDS_MODE) {
        struct spr_mode d;
        if ((e = spr_mode(ros_ld32(sp + SP_MODE), &d)) != NULL)
            goto out;
    }
check:
    if (e)
        goto out;
go:
    e = dispatch(s, reason, area, sp);
out:
    if (e)
        ros_swi_fail(s, e);
}

void ros_thunk_OS_SpriteOp(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    s->v = 0;
    if (!ros_vector_call(SPRITEV, s))
        sprite_op(s);
    ros_svc_sp = outer_sp;
    s->r[10] = r10, s->r[11] = r11, s->r[12] = r12;
}
