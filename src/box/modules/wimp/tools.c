/* tools.c -- the tool sprites: the tool list, the furniture's sizes and the
 * parameters every tool is plotted with.
 *
 * The tool set is WindowManager:Tools, loaded into the RMA.  The list has
 * 53 slots.  Each holds a sprite's address with bit 0 set if it has a
 * mask, or 0.  Building the list measures each tool for the furniture's
 * sizes and finds the custom translation tables (table_0 to table_15).
 * It also works out the plot action, the scale factors and the
 * translation table, as maketoollist does.  Tinting is left out.  It
 * applies only to a title whose colours are not the default grey on
 * black. */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

/* The slots: name, pressed form, width metric, height metric */
enum { M_NONE, M_TITLE_H, M_UP_H, M_DOWN_H, M_HSCROLL_H, M_VSCROLL_W, M_BACK_W, M_CLOSE_W,
       M_ICONISE_W, M_LEFT_W, M_RIGHT_W, M_TITLE_L, M_TITLE_R, M_TITLE_SECT, M_TITLE_TOP,
       M_TITLE_BOT, M_VS_TOP, M_VS_TOPFILL, M_VS_BLOBTOP, M_VS_BLOBFILL, M_VS_BLOBBOT,
       M_VS_BOTFILL, M_VS_BOT, M_HS_LEFT, M_HS_LEFTFILL, M_HS_BLOBLEFT, M_HS_BLOB,
       M_HS_BLOBRIGHT, M_HS_RIGHTFILL, M_HS_RIGHT, M_HS_BLIPW, M_VS_BLIPH, M_COUNT };

static const struct slot {
    const char *name;
    int pressed;
    uint8_t wm, hm;
} slots[] = {
    { "bicon", 1, M_BACK_W, M_TITLE_H },        { "cicon", 1, M_CLOSE_W, M_TITLE_H },
    { "ticon", 1, M_VSCROLL_W, M_TITLE_H },     { "ticon1", 1, M_VSCROLL_W, M_TITLE_H },
    { "sicon", 1, M_VSCROLL_W, M_HSCROLL_H },   { "iicon", 1, M_ICONISE_W, M_TITLE_H },
    { "uicon", 1, M_VSCROLL_W, M_UP_H },        { "dicon", 1, M_VSCROLL_W, M_DOWN_H },
    { "ricon", 1, M_RIGHT_W, M_HSCROLL_H },     { "licon", 1, M_LEFT_W, M_HSCROLL_H },
    { "tbarlcap", 1, M_TITLE_L, M_TITLE_H },    { "tbarmidt", 1, M_TITLE_SECT, M_TITLE_TOP },
    { "tbarmidb", 1, M_TITLE_SECT, M_TITLE_BOT },{ "tbarrcap", 1, M_TITLE_R, M_TITLE_H },
    { "vwelltcap", 0, M_VSCROLL_W, M_VS_TOP },  { "vwellt", 0, M_VSCROLL_W, M_VS_TOPFILL },
    { "vbart", 1, M_VSCROLL_W, M_VS_BLOBTOP },  { "vbarmid", 1, M_VSCROLL_W, M_VS_BLOBFILL },
    { "vbarb", 1, M_VSCROLL_W, M_VS_BLOBBOT },  { "vwellb", 0, M_VSCROLL_W, M_VS_BOTFILL },
    { "vwellbcap", 0, M_VSCROLL_W, M_VS_BOT },  { "hwelllcap", 0, M_HS_LEFT, M_HSCROLL_H },
    { "hwelll", 0, M_HS_LEFTFILL, M_HSCROLL_H },{ "hbarl", 1, M_HS_BLOBLEFT, M_HSCROLL_H },
    { "hbarmid", 1, M_HS_BLOB, M_HSCROLL_H },   { "hbarr", 1, M_HS_BLOBRIGHT, M_HSCROLL_H },
    { "hwellr", 0, M_HS_RIGHTFILL, M_HSCROLL_H },{ "hwellrcap", 0, M_HS_RIGHT, M_HSCROLL_H },
    { "hblip", 1, M_HS_BLIPW, M_HSCROLL_H },    { "vblip", 1, M_VSCROLL_W, M_VS_BLIPH },
    { "blicon", 0, M_VSCROLL_W, M_HSCROLL_H },
};

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

/* The tool area, loaded afresh: 0 if there is no tools file */
static uint32_t load(void)
{
    struct wimp_ws *w = wimp_ws();
    char *name = (char *)w->scratch;
    strcpy(name, "WindowManager:Tools");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17;
    c.r[1] = ros_addr(name);
    if (!call(XOS_File, &c) || c.r[0] != 1)
        return 0;
    uint32_t size = c.r[4] + 16;
    void *area;
    if (xos_module_claim(size, &area))
        return 0;
    uint32_t a = ros_addr(area);
    ros_st32(a, size);
    ros_st32(a + 8, 16);
    ros_st32(a + 12, 16);
    ros_cpu_enter(&c);
    c.r[0] = 0x109, c.r[1] = a;
    int ok = call(XOS_SpriteOp, &c);
    ros_cpu_enter(&c);
    c.r[0] = 0x10A, c.r[1] = a, c.r[2] = ros_addr(name);
    if (!ok || !call(XOS_SpriteOp, &c)) {
        xos_module_free(area);
        return 0;
    }
    return a;
}

/* A sprite in the tool area by name: its address, or 0 */
static uint32_t select(const char *name)
{
    struct wimp_ws *w = wimp_ws();
    char *n = (char *)w->scratch + 64;
    snprintf(n, 16, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x118, c.r[1] = w->tools, c.r[2] = ros_addr(n);
    return call(XOS_SpriteOp, &c) ? c.r[2] : 0;
}

/* A tool by name, through the suffix chain.  The name is tried with the
 * suffix "<x><y>" for the screen's pixel shape, from its eigen factors.
 * Then the larger of x and y is halved until they are equal, trying each.
 * At 11 the suffix 22 is tried too.  Last comes the plain name. */
static uint32_t find(const char *base)
{
    struct wimp_ws *w = wimp_ws();
    unsigned x = 1u << w->xeig, y = 1u << w->yeig;
    char n[16];
    uint32_t s;
    snprintf(n, sizeof n, "%s%u%u", base, x, y);
    if ((s = select(n)))
        return s;
    while (x != y) {
        if (x > y) x >>= 1;
        else y >>= 1;
        snprintf(n, sizeof n, "%s%u%u", base, x, y);
        if ((s = select(n)))
            return s;
    }
    if (x == 1 && y == 1) {
        snprintf(n, sizeof n, "%s22", base);
        if ((s = select(n)))
            return s;
    }
    return select(base);
}

static int32_t eig(uint32_t mode, uint32_t var)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = mode, c.r[1] = var;
    ros_swi(&c, XOS_ReadModeVariable);
    return (c.v || c.c) ? -1 : (int32_t)c.r[2];
}

/* The scale factors from sprite pixels to screen pixels */
static void factors(uint32_t *blk, int32_t exs, int32_t eys, int32_t exd, int32_t eyd)
{
    blk[0] = exs > exd ? (1u << exs) >> exd : 1u;
    blk[2] = exs < exd ? (1u << exd) >> exs : 1u;
    blk[1] = eys > eyd ? (1u << eys) >> eyd : 1u;
    blk[3] = eys < eyd ? (1u << eyd) >> eys : 1u;
}

/* mastertoactive: the custom tables for this screen.  A standard 32 bpp
 * screen (&00BBGGRR, with no RGB or alpha sub-format) uses the masters as
 * they are.  For any other screen, each master entry is put through
 * ColourTrans_ReturnColourNumber into a block in the RMA.  Each entry
 * takes E bytes: 1 below 16 bpp, 2 at 16 bpp and 4 at 32 bpp. */
static void master_to_active(struct wimp_tools *t)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t mf = (uint32_t)eig(0xFFFFFFFFu, 0);     /* the screen's mode flags */
    unsigned c = 0;
    for (int n = 0; n < 16; n++)
        if (t->master[n])
            c++;
    if (w->log2bpp == 5 && !(mf & 0xC000u)) {
        for (int n = 0; n < 16; n++)
            t->active[n] = t->master[n];
        return;
    }
    uint32_t E = w->log2bpp < 4 ? 1 : w->log2bpp == 4 ? 2 : 4;
    uint32_t block = c ? wimp_table_space(&t->act_at, &t->act_size, 256 * E * c) : 0;
    for (int n = 15; n >= 0; n--) {
        t->active[n] = 0;
        if (!t->master[n] || !block)
            continue;
        uint32_t out = block;
        block += 256 * E;
        for (int i = 255; i >= 0; i--) {
            uint32_t m = ros_ld32(t->master[n] + 4u * (uint32_t)i);
            struct ros_cpu r;
            ros_cpu_enter(&r);
            r.r[0] = m << 8 | m >> 24;          /* &00BBGGRR rotated to &BBGGRR00 */
            ros_swi(&r, XColourTrans_ReturnColourNumber);
            uint32_t v = r.v ? 0 : r.r[0];
            for (uint32_t b = 0; b < E; b++)
                ros_st8(out + E * (uint32_t)i + b, (uint8_t)(v >> (8 * b)));
        }
        t->active[n] = out;
    }
}

/* An old-style first task gets no mode set, so maketoollist is not run.
 * The tool list is empty and the furniture's sizes are 0 until a border
 * is drawn (s/Wimp02:519-531, 2046-2048, 8361-8370). */
void wimp_tools_unlist(void)
{
    struct wimp_ws *w = wimp_ws();
    memset(w->tl.list, 0, sizeof w->tl.list);
    memset(&w->furn, 0, sizeof w->furn);
    w->tl.built = 0;
    w->tl.unlisted = 1;
}

/* The tool list built: the list, the furniture's sizes and the plot
 * parameters */
void wimp_tools_refresh(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_tools *t = &w->tl;
    t->unlisted = 0;
    if (!w->tools)
        w->tools = load();
    int32_t m[M_COUNT];
    memset(m, 0, sizeof m);
    memset(t->list, 0, sizeof t->list);
    memset(t->master, 0, sizeof t->master);
    t->built = 0;
    uint32_t a = w->tools;
    if (a) {
        /* the custom tables */
        int any = 0;
        for (int n = 15; n >= 0; n--) {
            char name[12];
            snprintf(name, sizeof name, "table_%d", n);
            uint32_t s = select(name);
            if (s) {
                t->master[n] = s + ros_ld32(s + 32);    /* the image: 256 &00BBGGRR words */
                any = 1;
            }
        }
        if (any && !t->master[2])
            a = 0;                              /* no table_2: the whole set discarded */
    }
    if (a) {
        unsigned k = 0;
        for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) {
            for (int p = 0; p <= slots[i].pressed; p++, k++) {
                char name[16];
                snprintf(name, sizeof name, "%s%s", p ? "p" : "", slots[i].name);
                uint32_t s = find(name);
                if (!s)
                    continue;
                struct ros_cpu c;
                ros_cpu_enter(&c);
                c.r[0] = 0x228, c.r[1] = a, c.r[2] = s;    /* ReadSpriteSize */
                if (!call(XOS_SpriteOp, &c))
                    continue;
                int32_t wpx = (int32_t)c.r[3] - 1, hpx = (int32_t)c.r[4] - 1;
                if (wpx < 0) wpx = 0;
                if (hpx < 0) hpx = 0;
                uint32_t entry = s | (c.r[5] == 1 ? 1u : 0u);
                int32_t xe = eig(c.r[6], 4), ye = eig(c.r[6], 5);
                if (xe < 0 || ye < 0)
                    continue;
                int32_t W = wpx << xe, H = hpx << ye;
                if (slots[i].wm && W > m[slots[i].wm]) m[slots[i].wm] = W;
                if (slots[i].hm && H > m[slots[i].hm]) m[slots[i].hm] = H;
                t->list[k] = entry;
            }
        }
        t->built = 1;
    }
    /* defaults for the sizes that no tool set */
    int hs[] = { M_TITLE_H, M_UP_H, M_DOWN_H, M_HSCROLL_H };
    int ws[] = { M_VSCROLL_W, M_BACK_W, M_CLOSE_W, M_ICONISE_W, M_LEFT_W, M_RIGHT_W };
    for (unsigned i = 0; i < 4; i++) if (!m[hs[i]]) m[hs[i]] = 40;
    for (unsigned i = 0; i < 6; i++) if (!m[ws[i]]) m[ws[i]] = 40;
    struct wimp_furniture *f = &w->furn;
    f->T = m[M_TITLE_H], f->V = m[M_VSCROLL_W], f->H = m[M_HSCROLL_H];
    f->B = m[M_BACK_W], f->C = m[M_CLOSE_W], f->I = m[M_ICONISE_W];
    f->L = m[M_LEFT_W], f->R = m[M_RIGHT_W], f->U = m[M_UP_H], f->D = m[M_DOWN_H];
    t->title_left = m[M_TITLE_L], t->title_right = m[M_TITLE_R];
    t->title_section = m[M_TITLE_SECT], t->title_top = m[M_TITLE_TOP];
    t->title_bottom = m[M_TITLE_BOT];
    t->vs_top = m[M_VS_TOP], t->vs_topfill = m[M_VS_TOPFILL], t->vs_blobtop = m[M_VS_BLOBTOP];
    t->vs_blobfill = m[M_VS_BLOBFILL], t->vs_blobbot = m[M_VS_BLOBBOT];
    t->vs_botfill = m[M_VS_BOTFILL], t->vs_bot = m[M_VS_BOT];
    t->hs_left = m[M_HS_LEFT], t->hs_leftfill = m[M_HS_LEFTFILL], t->hs_blobleft = m[M_HS_BLOBLEFT];
    t->hs_blob = m[M_HS_BLOB], t->hs_blobright = m[M_HS_BLOBRIGHT];
    t->hs_rightfill = m[M_HS_RIGHTFILL], t->hs_right = m[M_HS_RIGHT];
    t->hblip_w = m[M_HS_BLIPW], t->vblip_h = m[M_VS_BLIPH];
    if (!t->built)
        return;

    /* the translation, from the blank tool */
    uint32_t blank = t->list[TOOL_BLANK] & ~1u;
    uint32_t mode = blank ? ros_ld32(blank + 40) : 0;
    int32_t xe = blank ? eig(mode, 4) : (int32_t)w->xeig, ye = blank ? eig(mode, 5) : (int32_t)w->yeig;
    uint32_t *blk = (uint32_t *)t->scale;
    factors(blk, xe, ye, (int32_t)w->xeig, (int32_t)w->yeig);
    t->table = 0;
    master_to_active(t);
    if (!t->master[2] && blank) {
        /* no custom tables: ColourTrans_GenerateTable from the blank tool */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = w->tools, c.r[1] = blank, c.r[2] = c.r[3] = 0xFFFFFFFFu, c.r[4] = 0;
        c.r[5] = 0x13;
        uint32_t buf;
        if (call(XColourTrans_GenerateTable, &c) &&
            (buf = wimp_table_space(&t->pix_at, &t->pix_size, c.r[4])) != 0) {
            uint32_t size = c.r[4];
            ros_cpu_enter(&c);
            c.r[0] = w->tools, c.r[1] = blank, c.r[2] = c.r[3] = 0xFFFFFFFFu;
            c.r[4] = buf, c.r[5] = 0x13;
            if (call(XColourTrans_GenerateTable, &c)) {
                int identity = size <= 256;
                for (uint32_t i = 0; identity && i < size; i++)
                    identity = ros_ld8(buf + i) == i;
                if (!identity)
                    t->table = buf;
            }
        }
    }
    /* the plot action: 32 (wide table), with 8 added if any tool other
     * than the unpressed title middles has a mask */
    t->action = 32;
    for (unsigned k = 0; k < TOOL_SLOTS; k++) {
        if (k == TOOL_TBARMIDT || k == TOOL_TBARMIDB)
            continue;
        if (t->list[k] & 1u) {
            t->action |= 8;
            break;
        }
    }
}

/* The translation table for a tool drawn in an element of colour c */
uint32_t wimp_tool_table(uint32_t c)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_tools *t = &w->tl;
    if (!t->active[2])
        return t->table;
    for (int n = 15; n >= 0; n--)
        if (w->palette[n] == c && t->active[n])
            return t->active[n];
    return t->active[2];
}
