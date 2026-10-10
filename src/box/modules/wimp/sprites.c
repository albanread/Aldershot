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
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

/* ---- the ROM area ------------------------------------------------------------ */

static uint32_t load_file(const char *name)
{
    struct wimp_ws *w = wimp_ws();
    char *n = (char *)w->scratch;
    snprintf(n, 128, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17;
    c.r[1] = ros_addr(n);
    if (!call(XOS_File, &c) || c.r[0] != 1)
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
    snprintf(n, 128, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17, c.r[1] = ros_addr(n);
    ros_swi(&c, XOS_File);
    return !c.v && c.r[0] == 1;
}

/* As getromsprites. The suffix comes from the Wimp's mode, or from the
 * screen's mode before the Wimp has one. The suffixes are then tried in
 * 5.30's order, leaving out the alpha forms because the box's files have
 * none. The name of the first file that exists is returned in *name. */
static void rom_name(char *name, size_t size)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t mode = w->mode;
    unsigned x = 1u << mode_var(mode, 4, w->xeig), y = 1u << mode_var(mode, 5, w->yeig);
    if (x == 2 && y == 2 && mode_var(mode, 9, 1) == 0)
        y = 3;                                  /* a 1 bpp 2 x 2 mode: 23 */
    w->rom_suffix[0] = (char)('0' + x), w->rom_suffix[1] = (char)('0' + y), w->rom_suffix[2] = 0;
    for (;;) {
        snprintf(name, size, "WindowManager:Sprites%u%u", x, y);
        if (file_exists(name))
            return;
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
    snprintf(name, size, "WindowManager:Sprites");
}

static uint32_t load_rom_area(void)
{
    struct wimp_ws *w = wimp_ws();
    rom_name(w->rom_name, sizeof w->rom_name);
    return load_file(w->rom_name);
}

/* Choose the ROM area again, after a mode change or at
 * Service_ResourceFSStarted. A different file is loaded in place of the
 * old one. If it cannot be loaded, the area is left as it was. */
void wimp_sprites_choose(void)
{
    struct wimp_ws *w = wimp_ws();
    char name[48];
    rom_name(name, sizeof name);
    if (w->rom_sprites && w->rom_sprites != ros_addr(w->empty_area) && !strcmp(name, w->rom_name))
        return;
    uint32_t a = load_file(name);
    if (!a)
        return;
    if (w->rom_sprites && w->rom_sprites != ros_addr(w->empty_area))
        xos_module_free(ros_ptr(w->rom_sprites));
    w->rom_sprites = a;
    strcpy(w->rom_name, name);
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
    if (!sprite) {
        wimp_fail(s, ros_error(0x86, "Sprite doesn't exist"));
        return;
    }
    if (reason == 99) {                         /* a fast existence test */
        s->v = 0;
        return;
    }
    if (reason == 36 && !(s->r[3] & 0x20u))
        wimp_mouse_palette(0);                  /* colours 1-3 from the Wimp's palette */
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
        unsigned x = 1u << w->xeig, y = 1u << w->yeig;
        for (;;) {
            char *f = (char *)w->scratch;
            snprintf(f, 250, "%s%u%u", file, x, y);
            if (!(last = merge(ros_addr(f))))
                return NULL;
            if (y == 3) y = 2;
            else if (x != y) { if (x < y) x <<= 1; else y <<= 1; }
            else if (x == 1) x = y = 2;
            else break;
        }
        char *f = (char *)w->scratch;
        snprintf(f, 250, "%s", file);
        if (!(last = merge(ros_addr(f))))
            return NULL;
    }
    return last;
}
