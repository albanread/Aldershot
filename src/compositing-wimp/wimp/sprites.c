/* sprites.c: the Wimp's sprite pool. This covers the ROM area, the RAM
 * area, precedence, Wimp_SpriteOp, Wimp_BaseOfSprites and *IconSprites.
 *
 * The ROM area is WindowManager:Sprites with the mode's suffix, loaded
 * into the RMA. 5.30 uses the file where it lies in ResourceFS, but a task
 * sees the same sprites either way. The RAM area is a dynamic area that
 * grows as files are merged into it. A name is looked for in the high
 * area and then in the low one. This finds the same sprite as 5.30's
 * sorted list does. The sorted list itself, and the deletions that ROM
 * precedence makes, are not done. They wait for *Configure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "vicon.h"
#include "wimp.h"

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

/* ---- the ROM area ------------------------------------------------------------ */

#define LOAD_MAX (64u << 20)                    /* the largest sprite or icon file loaded */

static uint32_t load_file(const char *name)
{
    struct wimp_ws *w = wimp_ws();
    char *n = (char *)w->scratch;
    snprintf(n, 250, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17;
    c.r[1] = ros_addr(n);
    if (!call(XOS_File, &c) || c.r[0] != 1)
        return 0;
    if (c.r[4] > LOAD_MAX)                      /* a length near 4G would wrap the claim below */
        return 0;
    uint32_t size = c.r[4] + 4;
    void *mem;
    if (xos_module_claim(size, &mem))
        return 0;
    uint32_t a = ros_addr(mem);
    ros_st32(a, size);                          /* the area's size, then the file */
    ros_cpu_enter(&c);
    c.r[0] = 16;
    c.r[1] = ros_addr(n);
    c.r[2] = a + 4;
    c.r[3] = 0;
    if (!call(XOS_File, &c)) {
        xos_module_free(mem);
        return 0;
    }
    return a;
}

static uint32_t mode_var(uint32_t mode, uint32_t var, uint32_t dflt)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = mode, c.r[1] = var;
    ros_swi(&c, XOS_ReadModeVariable);
    return c.v || c.c ? dflt : c.r[2];         /* C set: an invalid mode or variable */
}

static int file_exists(const char *name)
{
    struct wimp_ws *w = wimp_ws();
    char *n = (char *)w->scratch;
    snprintf(n, 250, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17, c.r[1] = ros_addr(n);
    ros_swi(&c, XOS_File);
    return !c.v && c.r[0] == 1;
}

/* As getromsprites. The suffix comes from the Wimp's mode, or from the
 * screen's mode before the Wimp has one. The suffixes are then tried in
 * 5.30's order, the alpha form (A<y>) first in a mode of square pixels.
 * The name of the first file that exists is returned in *name. */
static int rom_chain(const char *base, char *name, size_t size);

/* ROSGD (design 35): a theme's Sprites take the ROM area's place, as RISC
 * OS 5's !ThemeDefs puts them over Resources.Wimp.Sprites in ResourceFS,
 * found through the same suffixes; the ROM's own if the theme has none. */
static void rom_name(char *name, size_t size)
{
    struct wimp_ws *w = wimp_ws();
    if (w->theme_sprites[0] && rom_chain(w->theme_sprites, name, size))
        return;
    if (!rom_chain("WindowManager:Sprites", name, size))
        snprintf(name, size, "WindowManager:Sprites");
}

/* The first of base<x><y>, down the suffix chain, then base, that exists:
 * 1 and its name in *name, or 0 */
static int rom_chain(const char *base, char *name, size_t size)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t mode = w->mode;
    unsigned x = 1u << mode_var(mode, 4, w->xeig), y = 1u << mode_var(mode, 5, w->yeig);
    if (x == 2 && y == 2 && mode_var(mode, 9, 1) == 0)
        y = 3;                                  /* a 1 bpp 2 x 2 mode: 23 */
    w->rom_suffix[0] = (char)('0' + x), w->rom_suffix[1] = (char)('0' + y), w->rom_suffix[2] = 0;
    /* square pixels: base"A"<y> first, icons with alpha masks (SpriteExtend
     * blends them), as 5.30 tries SpritesA2 before Sprites22 */
    if (x == y) {
        snprintf(name, size, "%sA%u", base, y);
        if (file_exists(name))
            return 1;
    }
    for (;;) {
        snprintf(name, size, "%s%u%u", base, x, y);
        if (file_exists(name))
            return 1;
        if (y == 3)
            y = 2;
        else if (x != y) {
            if (x < y) x <<= 1;
            else y <<= 1;
        } else if (x == 1) {
            x = y = 2;
        } else {
            break;
        }
    }
    snprintf(name, size, "%s", base);
    return file_exists(name);
}

static uint32_t load_rom_area(void)
{
    struct wimp_ws *w = wimp_ws();
    rom_name(w->rom_name, sizeof w->rom_name);
    return load_file(w->rom_name);
}

/* The Wimp's own sprites (WindowManager:Sprites, by the mode's suffix), for
 * Wimp_SpriteOp 36 when a theme's set cannot give the pointer: an alpha
 * set's sprites are 32 bpp, which SetPointerShape refuses as RISC OS does
 * (pointers are 8 bpp or less), and a theme may have no ptr_ sprites at
 * all.  Then *Pointer, the Wimp's pointer_on after every mode change, set
 * nothing and selected nothing, and the pointer stayed off. */
static uint32_t ptr_area(void)
{
    struct wimp_ws *w = wimp_ws();
    char name[200];
    if (!rom_chain("WindowManager:Sprites", name, sizeof name))
        snprintf(name, sizeof name, "WindowManager:Sprites");
    if (w->ptr_sprites && !strcmp(name, w->ptr_name))
        return w->ptr_sprites;
    uint32_t a = load_file(name);
    if (!a)
        return w->ptr_sprites;              /* the one we had, if any */
    if (w->ptr_sprites)
        xos_module_free(ros_ptr(w->ptr_sprites));
    w->ptr_sprites = a;
    strcpy(w->ptr_name, name);
    return a;
}

static os_error *vector_load(const char *name, int remember);
static void pool_changed(void);
static void own_vectors(void);

/* Choose the ROM area again, after a mode change or at
 * Service_ResourceFSStarted. A different file is loaded in place of the
 * old one. If it cannot be loaded, the area is left as it was. */
void wimp_sprites_choose(void)
{
    struct wimp_ws *w = wimp_ws();
    char name[200];
    rom_name(name, sizeof name);
    if (w->rom_sprites && w->rom_sprites != ros_addr(w->empty_area) && !strcmp(name, w->rom_name)) {
        own_vectors();
        return;
    }
    uint32_t a = load_file(name);
    if (!a) {
        own_vectors();
        return;
    }
    if (w->rom_sprites && w->rom_sprites != ros_addr(w->empty_area))
        xos_module_free(ros_ptr(w->rom_sprites));
    w->rom_sprites = a;
    strcpy(w->rom_name, name);
    own_vectors();
}

/* As loseromsprites, at Service_ResourceFSDying. The ROM area becomes an
 * empty one. */
void wimp_sprites_lose(void)
{
    struct wimp_ws *w = wimp_ws();
    uint8_t *e = w->empty_area;
    uint32_t v[4] = { 16, 0, 16, 16 };
    memcpy(e, v, 16);
    if (w->rom_sprites && w->rom_sprites != ros_addr(e))
        xos_module_free(ros_ptr(w->rom_sprites));
    w->rom_sprites = ros_addr(e);
    w->rom_name[0] = 0;
    if (w->ptr_sprites)
        xos_module_free(ros_ptr(w->ptr_sprites));
    w->ptr_sprites = 0;
    w->ptr_name[0] = 0;
}

/* ---- the RAM area -------------------------------------------------------------- */

static uint32_t make_ram_area(void)
{
    struct wimp_ws *w = wimp_ws();
    /* The name goes past the file name that merge() was given.
     * *IconSprites builds that at the start of scratch, and merge() makes
     * the area before it reads it. */
    char *name = (char *)w->scratch + 448;
    strcpy(name, "WSpr");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0;                                 /* OS_DynamicArea 0: create */
    c.r[1] = 0xFFFFFFFFu;
    c.r[2] = 4096;
    c.r[3] = 0xFFFFFFFFu;
    c.r[4] = 0x80u;                             /* not draggable */
    c.r[5] = 16u << 20;
    c.r[6] = c.r[7] = 0;
    c.r[8] = ros_addr(name);
    if (!call(XOS_DynamicArea, &c))
        return 0;
    w->ram_number = c.r[1];
    uint32_t a = c.r[3];
    ros_st32(a, 4096);
    ros_st32(a + 4, 0);
    ros_st32(a + 8, 16);
    ros_st32(a + 12, 16);
    return a;
}


void wimp_sprites_start(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w->rom_sprites)
        w->rom_sprites = load_rom_area();
    if (!w->ram_sprites)
        w->ram_sprites = make_ram_area();
    own_vectors();
}

/* ---- finding a name in the pool ----------------------------------------------------- */

static uint32_t high_area(void)
{
    return wimp_ws()->ram_sprites;              /* RAM precedence, the default */
}

static uint32_t low_area(void)
{
    return wimp_ws()->rom_sprites;
}

/* Look for the sprite named, in the high area and then the low one.
 * Returns its address and the area it is in, or 0 if neither has it. */
uint32_t wimp_pool_find(uint32_t name, uint32_t *area)
{
    uint32_t areas[2] = { high_area(), low_area() };
    for (int i = 0; i < 2; i++) {
        if (!areas[i])
            continue;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0x118;                         /* SelectSprite, by name */
        c.r[1] = areas[i];
        c.r[2] = name;
        if (call(XOS_SpriteOp, &c)) {
            *area = areas[i];
            return c.r[2];
        }
    }
    return 0;
}

/* ---- Wimp_SpriteOp and Wimp_BaseOfSprites ----------------------------------------- */

static int allowed(uint32_t r)
{
    static const uint8_t ok[] = { 3, 4, 5, 6, 7, 8, 11, 12, 13, 17, 18, 19, 20, 21, 22, 23, 24,
                                  28, 34, 36, 37, 39, 40, 41, 43, 48, 49, 50, 51, 52, 53, 55,
                                  56, 59, 62, 65, 99 };
    for (unsigned i = 0; i < sizeof ok; i++)
        if (ok[i] == r)
            return 1;
    return 0;
}

static os_error *merge(uint32_t file)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17;
    c.r[1] = file;
    ros_swi(&c, XOS_File);
    if (c.v)
        return ros_ptr(c.r[0]);
    if (c.r[0] != 1) {
        ros_cpu_enter(&c);
        c.r[0] = 19;                            /* "not found", as OS_File gives it */
        c.r[1] = file;
        c.r[2] = 0;
        ros_swi(&c, XOS_File);
        return c.v ? ros_ptr(c.r[0]) : ros_error(0xD6, "File not found");
    }
    if (((c.r[2] >> 8) & 0xFFFu) == 0xFCAu)
        return ros_error(0x2A9, "Squashed sprite files cannot be merged");
    uint32_t length = c.r[4];
    if (!w->ram_sprites && !(w->ram_sprites = make_ram_area()))
        return ros_error(0x2A7, "Unable to create the Wimp sprite area");
    /* Grow the area by the file's length, merge, then shrink it to what
     * is used. */
    ros_cpu_enter(&c);
    c.r[0] = w->ram_number;
    c.r[1] = length + 16;
    ros_swi(&c, XOS_ChangeDynamicArea);
    if (c.v)
        return ros_ptr(c.r[0]);
    ros_st32(w->ram_sprites, ros_ld32(w->ram_sprites) + c.r[1]);
    ros_cpu_enter(&c);
    c.r[0] = 0x10B;                             /* MergeSpriteFile */
    c.r[1] = w->ram_sprites;
    c.r[2] = file;
    ros_swi(&c, XOS_SpriteOp);
    os_error *err = c.v ? ros_ptr(c.r[0]) : NULL;
    uint32_t used = ros_ld32(w->ram_sprites + 12), size = ros_ld32(w->ram_sprites);
    uint32_t spare = (size - used) & ~4095u;
    if (spare) {
        ros_cpu_enter(&c);
        c.r[0] = w->ram_number;
        c.r[1] = (uint32_t)-(int32_t)spare;
        ros_swi(&c, XOS_ChangeDynamicArea);
        if (!c.v)
            ros_st32(w->ram_sprites, size - c.r[1]);
    }
    wimp_tiles_forget();                        /* the window tiles may have changed */
    if (!err) {
        ros_cpu_enter(&c);                      /* Service_WimpSpritesMoved */
        c.r[1] = 0x85;
        c.r[2] = w->rom_sprites;
        c.r[3] = w->ram_sprites;
        ros_service_call(&c);
    }
    return err;
}

/* ---- vector icons (RISCOSGrandDesign design 36) ------------------------------------ */

/* An icon file (VecIcons, &A6F: ports/vicon/vicon.h) holds Haiku's HVIF
 * icons by name.  *IconSprites loads <file>V as well as the sprite files:
 * each icon is drawn into an alpha sprite at its size in the screen's
 * pixels (its width at 90 dpi, twice that at 180), and the sprite put in
 * the RAM area in place of any of its name, so it is found before a bitmap
 * one.  The files are remembered, and drawn again when a mode change gives
 * pixels of another size. */
#define VECTOR_FILES 32
static char vector_file[VECTOR_FILES][200];
static uint32_t vector_files, vector_xeig, vector_yeig;

/* Puts the sprite (its bytes, from its "next" word) into the RAM area, in
 * place of one of its name */
static os_error *pool_put(const uint8_t *sprite, uint32_t length)
{
    struct wimp_ws *w = wimp_ws();
    if (!w->ram_sprites && !(w->ram_sprites = make_ram_area()))
        return ros_error(0x2A7, "Unable to create the Wimp sprite area");
    char *name = (char *)w->scratch + 256;
    memcpy(name, sprite + 4, 12);
    name[12] = 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x119;                             /* DeleteSprite, by name: not there is no matter */
    c.r[1] = w->ram_sprites;
    c.r[2] = ros_addr(name);
    ros_swi(&c, XOS_SpriteOp);
    uint32_t size = ros_ld32(w->ram_sprites), used = ros_ld32(w->ram_sprites + 12);
    if (size - used < length) {
        ros_cpu_enter(&c);
        c.r[0] = w->ram_number;
        c.r[1] = length - (size - used);
        ros_swi(&c, XOS_ChangeDynamicArea);
        if (c.v)
            return ros_ptr(c.r[0]);
        size += c.r[1];
        ros_st32(w->ram_sprites, size);
        if (size - used < length)               /* it grew by less than was asked */
            return ros_error(0x2A7, "No room in the Wimp sprite area");
    }
    memcpy(ros_ptr(w->ram_sprites + used), sprite, length);
    ros_st32(w->ram_sprites + 4, ros_ld32(w->ram_sprites + 4) + 1);
    ros_st32(w->ram_sprites + 12, used + length);
    return NULL;
}

/* Draws each icon of the icon file name into the RAM area, at the
 * screen's pixel size; NULL, or why not */
static os_error *vector_load(const char *name, int remember)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t a = load_file(name);               /* the area's size word, then the file */
    if (!a)
        return ros_error(0xD6, "File '%s' not found", name);
    const uint8_t *file = ros_ptr(a + 4);
    uint32_t length = ros_ld32(a) - 4;
    int n = vicon_file_count(file, length);
    os_error *e = n < 0 ? ros_error(0x2AA, "'%s' is not an icon file", name) : NULL;
    uint32_t xeig = w->xeig > 3 ? 3 : w->xeig, yeig = w->yeig > 3 ? 3 : w->yeig;
    for (int i = 0; i < n && !e; i++) {
        struct vicon_entry v;
        if (!vicon_file_entry(file, length, (uint32_t)i, &v)) {
            e = ros_error(0x2AA, "The icon file '%s' is damaged", name);
            break;
        }
        uint32_t pw = (v.width * 2) >> xeig, ph = (v.height * 2) >> yeig;
        if (!pw || !ph || pw > VICON_MAX || ph > VICON_MAX)
            continue;
        size_t bytes = vicon_sprite_bytes(pw, ph);
        uint8_t *sp = malloc(bytes);
        if (!sp) {
            e = ros_error(0x101, "No room");
            break;
        }
        if (vicon_sprite_one(v.hvif, v.length, pw, ph, 180u >> xeig, 180u >> yeig, v.name, sp, bytes))
            e = pool_put(sp, (uint32_t)bytes);
        free(sp);
    }
    xos_module_free(ros_ptr(a));
    if (e || !remember)
        return e;
    unsigned k = 0;
    while (k < vector_files && strcmp(vector_file[k], name))
        k++;
    if (k == vector_files && vector_files < VECTOR_FILES)
        snprintf(vector_file[vector_files++], sizeof vector_file[0], "%s", name);
    vector_xeig = w->xeig, vector_yeig = w->yeig;
    return NULL;
}

/* The pool's sprites have changed: what depends on them is told */
static void pool_changed(void)
{
    struct wimp_ws *w = wimp_ws();
    wimp_tiles_forget();
    struct ros_cpu c;
    ros_cpu_enter(&c);                          /* Service_WimpSpritesMoved */
    c.r[1] = 0x85;
    c.r[2] = w->rom_sprites;
    c.r[3] = w->ram_sprites;
    ros_service_call(&c);
}

/* The desktop's vector icons, in layers put into the RAM area in this
 * order, each over the one before (a name in a later layer replaces it):
 *
 *   1. BOX's own (WindowManager:SpritesV, tools/mkvectoricons.py: the Task
 *      Manager's cog and its turnings) -- except under RISC OS 5's sprite
 *      themes (their own Sprites, no painted chrome: Iyonix, Sovereign,
 *      Morris4, Ursula), whose icons are their look;
 *   2. the theme's icon set's (Themes:<[Theme] Icons>.SpritesV);
 *   3. the theme's own (<its directory>.SpritesV).
 *
 * So a theme's partial set falls back to the set it names, and that to
 * the bitmaps.  Called whenever the ROM area is chosen (the desktop's
 * start, a theme, a mode change): when the files or the pixels' size have
 * changed, the old layers' names come out and the new layers go in. */
#define OWN_VECTORS "WindowManager:SpritesV"
#define LAYERS 3
static char layer_now[LAYERS][200];
static uint32_t layer_xeig = 0xFFFFFFFFu, layer_yeig;

/* Takes the icon file's names out of the RAM area */
static void vector_unload(const char *name)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t a = load_file(name);
    if (!a)
        return;
    const uint8_t *file = ros_ptr(a + 4);
    uint32_t length = ros_ld32(a) - 4;
    int n = vicon_file_count(file, length);
    char *sp = (char *)w->scratch + 256;
    for (int i = 0; i < n; i++) {
        struct vicon_entry v;
        if (!vicon_file_entry(file, length, (uint32_t)i, &v))
            break;
        strcpy(sp, v.name);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0x119;                         /* DeleteSprite, by name */
        c.r[1] = w->ram_sprites;
        c.r[2] = ros_addr(sp);
        ros_swi(&c, XOS_SpriteOp);
    }
    xos_module_free(ros_ptr(a));
}

static void own_vectors(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w->ram_sprites)
        return;
    char want[LAYERS][200];
    int sprite_theme = w->theme_sprites[0] && !w->style.painted;
    snprintf(want[0], sizeof want[0], "%s", sprite_theme ? "" : OWN_VECTORS);
    snprintf(want[1], sizeof want[1], "%s", w->theme_vectors[0]);
    snprintf(want[2], sizeof want[2], "%s", w->theme_vectors[1]);
    for (int k = 0; k < LAYERS; k++)
        if (want[k][0] && !file_exists(want[k]))
            want[k][0] = 0;
    if (!memcmp(want, layer_now, sizeof want) && layer_xeig == w->xeig && layer_yeig == w->yeig)
        return;
    for (int k = LAYERS - 1; k >= 0; k--)
        if (layer_now[k][0])
            vector_unload(layer_now[k]);
    for (int k = 0; k < LAYERS; k++)
        if (want[k][0] && vector_load(want[k], 0))
            want[k][0] = 0;
    memcpy(layer_now, want, sizeof want);
    layer_xeig = w->xeig, layer_yeig = w->yeig;
    pool_changed();
}

/* *IconSprites' icon file, loaded after its sprite file */
static os_error *vector_after(const char *name)
{
    os_error *e = vector_load(name, 1);
    pool_changed();
    return e;
}

/* After a mode change: the icon files drawn again if the pixels' size has
 * changed (a file that has gone keeps the sprites it gave) */
void wimp_vectors_again(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!vector_files || (w->xeig == vector_xeig && w->yeig == vector_yeig))
        return;
    for (unsigned k = 0; k < vector_files; k++)
        vector_load(vector_file[k], 1);
    vector_xeig = w->xeig, vector_yeig = w->yeig;
    pool_changed();
}

/* Wimp_SpriteOp 36, SetPointerShape, from the pool's sprite (area, sprite;
 * 0 if the pool has none) -- and, when a theme's sprites are the ROM area
 * and its sprite cannot be a pointer (or it has none), from the Wimp's own
 * sprite of that name, so that a theme never leaves the desktop without a
 * pointer */
static void pointer_op(struct ros_cpu *s, uint32_t area, uint32_t sprite)
{
    struct wimp_ws *w = wimp_ws();
    if (!(s->r[3] & 0x20u))
        wimp_mouse_palette(0);                  /* colours 1-3 from the Wimp's palette */
    struct ros_cpu c = *s;
    int done = 0;                               /* c holds an answer */
    if (sprite) {
        c.r[0] = 0x200u | 36;
        c.r[1] = area;
        c.r[2] = sprite;
        ros_swi(&c, XOS_SpriteOp);
        done = 1;
    }
    uint32_t own;
    if ((!done || c.v) && w->theme_sprites[0] && (!sprite || area != w->ram_sprites) &&
        (own = ptr_area()) != 0) {
        struct ros_cpu f = *s;
        f.r[0] = 0x100u | 36;                   /* by name, in the Wimp's own area */
        f.r[1] = own;
        ros_swi(&f, XOS_SpriteOp);
        if (!f.v || !done)
            c = f, done = 1;
    }
    if (!done) {
        wimp_fail(s, ros_error(0x86, "Sprite doesn't exist"));
        return;
    }
    memcpy(&s->r[1], &c.r[1], 10 * sizeof s->r[0]);
    if (c.v) {
        wimp_fail(s, ros_ptr(c.r[0]));
        return;
    }
    s->v = 0;
}

void wimp_swi_SpriteOp(struct ros_cpu *s)
{
    uint32_t reason = s->r[0] & 0xFFu;        /* only bits 0-7 are used */
    if (!allowed(reason)) {
        wimp_fail(s, wimp_error(E_BAD_SYSINFO));
        return;
    }
    if (reason == 11) {
        os_error *e = merge(s->r[2]);
        if (e)
            wimp_fail(s, e);
        else
            s->v = 0;
        return;
    }
    uint32_t area;
    uint32_t sprite = wimp_pool_find(s->r[2], &area);
    if (!sprite && reason == 36 && wimp_ws()->theme_sprites[0]) {
        pointer_op(s, 0, 0);                    /* the theme has none: the Wimp's own */
        return;
    }
    if (!sprite) {
        wimp_fail(s, ros_error(0x86, "Sprite doesn't exist"));
        return;
    }
    if (reason == 99) {                         /* a fast existence test */
        s->v = 0;
        return;
    }
    if (reason == 36) {
        pointer_op(s, area, sprite);
        return;
    }
    struct ros_cpu c = *s;
    c.r[0] = 0x200u | reason;
    c.r[1] = area;
    c.r[2] = sprite;
    ros_swi(&c, XOS_SpriteOp);
    memcpy(&s->r[1], &c.r[1], 10 * sizeof s->r[0]);
    if (c.v) {
        wimp_fail(s, ros_ptr(c.r[0]));
        return;
    }
    s->v = 0;
}

void wimp_swi_BaseOfSprites(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    s->r[0] = w->rom_sprites;
    s->r[1] = w->ram_sprites;
    s->v = 0;
}

/* ---- *IconSprites ------------------------------------------------------------------- */

os_error *wimp_cmd_iconsprites(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct wimp_ws *w = wimp_ws();
    char base[200], theme[64] = "";
    unsigned n = 0;
    for (uint32_t p = tail; ros_ld8(p) > ' ' && n < sizeof base - 1; p++)
        base[n++] = (char)ros_ld8(p);
    base[n] = 0;
    /* <Wimp$IconTheme>, after the last "." or ":" */
    char *var = (char *)w->scratch + 256, *val = (char *)w->scratch + 300;
    strcpy(var, "Wimp$IconTheme");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(var);
    c.r[1] = ros_addr(val);
    c.r[2] = 60;
    c.r[3] = 0;
    c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    if (!c.v && c.r[2] < sizeof theme) {
        memcpy(theme, val, c.r[2]);
        theme[c.r[2]] = 0;
    }
    os_error *last = ros_error(0xD6, "File '%s' not found", base);
    for (int pass = theme[0] ? 0 : 1; pass < 2; pass++) {
        char file[300];
        if (pass == 0) {
            char *cut = strrchr(base, '.'), *colon = strrchr(base, ':');
            if (colon > cut)
                cut = colon;
            size_t at = cut ? (size_t)(cut - base) + 1 : 0;
            snprintf(file, sizeof file, "%.*s%s%s", (int)at, base, theme, base + at);
        } else {
            snprintf(file, sizeof file, "%s", base);
        }
        /* <file>V, an icon file of vector icons (design 36): drawn after
         * whatever sprite file of the chain below is merged, so that its
         * icons win, or alone */
        char vfile[300];
        snprintf(vfile, sizeof vfile, "%sV", file);
        int vector = file_exists(vfile);
        unsigned x = 1u << w->xeig, y = 1u << w->yeig;
        if (x == y) {                           /* square pixels: <file>A<y> first, as rom_chain */
            char *f = (char *)w->scratch;
            snprintf(f, 250, "%sA%u", file, y);
            if (!(last = merge(ros_addr(f))))
                return vector ? vector_after(vfile) : NULL;
        }
        for (;;) {
            char *f = (char *)w->scratch;
            snprintf(f, 250, "%s%u%u", file, x, y);
            if (!(last = merge(ros_addr(f))))
                return vector ? vector_after(vfile) : NULL;
            if (y == 3) y = 2;
            else if (x != y) { if (x < y) x <<= 1; else y <<= 1; }
            else if (x == 1) x = y = 2;
            else break;
        }
        char *f = (char *)w->scratch;
        snprintf(f, 250, "%s", file);
        if (!(last = merge(ros_addr(f))))
            return vector ? vector_after(vfile) : NULL;
        if (vector)
            return vector_after(vfile);         /* the icon file alone */
    }
    return last;
}
