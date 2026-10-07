/* pgrosc.c -- Paige's machine layer for RISC OS on the Box (x32 and A64X32).
 *
 * A translation of HERMES's PGMODERN.CPP (the portable layer over the
 * pgNative* calls) into C, with the natives written directly against
 * the Box: fonts through the Font Manager (FindFont, StringWidth,
 * Paint, SetFontColours, CharBBox), colour through ColourTrans, rects
 * and the caret through OS_Plot, regions as plain rectangle lists.
 *
 * Coordinates: one Paige unit is one point, as the Haiku platform has
 * it (device resolution 72); the Font Manager's millipoints convert by
 * /1000 and the app's page rectangles come in points.
 *
 * Drawing has a target only when the app has set one
 * (rosc_set_target): with none, every draw is a safe no-op, which lets
 * the engine format and measure headlessly.
 *
 * The caret and the selection are the application's: it puts the Wimp's
 * caret where pgCaretPosition says and inverts what pgGetHiliteRgn gives
 * after each redraw, so the cursor and hilite procs here only keep
 * Paige's flags.
 *
 * Files: a pg_file_unit is a RISC OS file handle (OS_Find), and the
 * pgOS read and write procs take one as their filemap. */
#include <stddef.h>
#include <string.h>

#include "PAIGE.H"
#include "DEFPROCS.H"
#include "MACHINE.H"
#include "PGERRORS.H"
#include "PGEXCEPS.H"
#include "PGOSUTL.H"
#include "PGREGION.H"
#include "PGSELECT.H"
#include "PGSHAPES.H"
#include "PGTRAPS.H"
#include "PGUTILS.H"
#include "PGDEFSTL.H"
#include "PGIO.H"

#include "kernel.h"
#include "swis.h"

/* ---- the drawing target ------------------------------------------------------- */

struct rosc_target {
    int w;                      /* window handle, 0 = none */
    int xoff, yoff;             /* doc-to-window offset, OS units */
};

static struct rosc_target rosc_t;   /* set by the app before a redraw */

void rosc_set_target(int w, int xoff, int yoff)
{
    rosc_t.w = w;
    rosc_t.xoff = xoff;
    rosc_t.yoff = yoff;
}

/* doc space (points, y down) to screen OS units (y up): the app sets
 * xoff/yoff so that doc (0,0) is the window's work-area top-left --
 * yoff is that corner's OS-unit y, and doc y grows downward from it */
static long rosc_osx(long docx)
{
    return docx * 5 / 2 + rosc_t.xoff;     /* 180/72 = 2.5 the point */
}

static long rosc_osy(long docy)
{
    return rosc_t.yoff - docy * 5 / 2;
}

static int rosc_has_target(void)
{
    return rosc_t.w != 0;
}

/* ---- little helpers ------------------------------------------------------------ */

static short rosc_char_width(style_info_ptr style);
static short rosc_point(style_info_ptr style);

static void rosc_plot_rect(long x0, long y0, long x1, long y1, unsigned oscol)
{
    if (!rosc_has_target())
        return;
    _swix(ColourTrans_SetGCOL, _INR(0, 2), (int)oscol, 0, 0);
    /* absolute move, then rectangle fill by opposite corner */
    _swix(OS_Plot, _INR(0, 2), 4, (int)rosc_osx(x0), (int)rosc_osy(y0));
    _swix(OS_Plot, _INR(0, 2), 100, (int)rosc_osx(x1), (int)rosc_osy(y1));
}

static void rosc_hline(long x0, long x1, long y, unsigned oscol)
{
    if (!rosc_has_target())
        return;
    _swix(ColourTrans_SetGCOL, _INR(0, 2), (int)oscol, 0, 0);
    _swix(OS_Plot, _INR(0, 2), 4, (int)rosc_osx(x0), (int)rosc_osy(y));
    _swix(OS_Plot, _INR(0, 2), 5, (int)rosc_osx(x1), (int)rosc_osy(y));
}

/* a Paige colour to RISC OS's &BBGGRR00 */
static unsigned rosc_oscol(const color_value_ptr c)
{
    if (!c)
        return 0;
    return ((unsigned)(c->blue >> 8) << 24) | ((unsigned)(c->green >> 8) << 16)
         | ((unsigned)(c->red >> 8) << 8);
}

/* ---- regions: rectangle lists --------------------------------------------------- */

struct rosc_region {
    rectangle rects[64];
    int n;
};

pg_region pgCreateRgn(void)
{
    return (pg_region)rosc_alloc_clear(sizeof(struct rosc_region));
}

void pgDisposeRgn(pg_region rgn)
{
    if (rgn)
        rosc_free(rgn);
}

pg_boolean pgEmptyRgn(pg_region rgn)
{
    struct rosc_region *r = (struct rosc_region *)rgn;
    return (pg_boolean)(!r || r->n == 0);
}

void pgSetEmptyRgn(pg_region rgn)
{
    struct rosc_region *r = (struct rosc_region *)rgn;
    if (r)
        r->n = 0;
}

void pgCopyRgn(pg_region src, pg_region dst)
{
    struct rosc_region *s = (struct rosc_region *)src;
    struct rosc_region *d = (struct rosc_region *)dst;
    if (!d)
        return;
    if (!s)
        d->n = 0;
    else {
        d->n = s->n;
        memcpy(d->rects, s->rects, sizeof(rectangle) * (size_t)s->n);
    }
}

void pgRectToRgn(pg_region rgn, Rect PG_FAR *r)
{
    struct rosc_region *d = (struct rosc_region *)rgn;
    if (!d)
        return;
    d->n = 0;
    if (r && r->left < r->right && r->top < r->bottom) {
        d->rects[0].top_left.h = (short)r->left;
        d->rects[0].top_left.v = (short)r->top;
        d->rects[0].bot_right.h = (short)r->right;
        d->rects[0].bot_right.v = (short)r->bottom;
        d->n = 1;
    }
}

void pgOffsetRgn(pg_region rgn, short h, short v)
{
    struct rosc_region *r = (struct rosc_region *)rgn;
    int i;
    if (!r)
        return;
    for (i = 0; i < r->n; i++) {
        r->rects[i].top_left.h += h;
        r->rects[i].bot_right.h += h;
        r->rects[i].top_left.v += v;
        r->rects[i].bot_right.v += v;
    }
}

void pgInsetRgn(pg_region rgn, short dh, short dv)
{
    struct rosc_region *r = (struct rosc_region *)rgn;
    int i, w = 0;
    if (!r)
        return;
    for (i = 0; i < r->n; i++) {
        r->rects[i].top_left.h += dh;
        r->rects[i].bot_right.h -= dh;
        r->rects[i].top_left.v += dv;
        r->rects[i].bot_right.v -= dv;
        if (r->rects[i].top_left.h < r->rects[i].bot_right.h
        &&  r->rects[i].top_left.v < r->rects[i].bot_right.v)
            r->rects[w++] = r->rects[i];
    }
    r->n = w;
}

void pgSectRgn(pg_region a, pg_region b, pg_region out)
{
    struct rosc_region *l = (struct rosc_region *)a, *r = (struct rosc_region *)b;
    struct rosc_region *o = (struct rosc_region *)out;
    int i, j;
    if (!o)
        return;
    o->n = 0;
    if (!l || !r)
        return;
    for (i = 0; i < l->n; i++)
        for (j = 0; j < r->n; j++) {
            long x0 = l->rects[i].top_left.h > r->rects[j].top_left.h
                      ? l->rects[i].top_left.h : r->rects[j].top_left.h;
            long y0 = l->rects[i].top_left.v > r->rects[j].top_left.v
                      ? l->rects[i].top_left.v : r->rects[j].top_left.v;
            long x1 = l->rects[i].bot_right.h < r->rects[j].bot_right.h
                      ? l->rects[i].bot_right.h : r->rects[j].bot_right.h;
            long y1 = l->rects[i].bot_right.v < r->rects[j].bot_right.v
                      ? l->rects[i].bot_right.v : r->rects[j].bot_right.v;
            if (x0 < x1 && y0 < y1 && o->n < 64) {
                o->rects[o->n].top_left.h = x0;
                o->rects[o->n].top_left.v = y0;
                o->rects[o->n].bot_right.h = x1;
                o->rects[o->n].bot_right.v = y1;
                o->n++;
            }
        }
}

void pgUnionRgn(pg_region a, pg_region b, pg_region out)
{
    struct rosc_region *l = (struct rosc_region *)a, *r = (struct rosc_region *)b;
    struct rosc_region *o = (struct rosc_region *)out;
    int i;
    if (!o)
        return;
    o->n = 0;
    for (i = 0; l && i < l->n; i++)
        o->rects[o->n++] = l->rects[i];
    for (i = 0; r && i < r->n; i++)
        if (o->n < 64)
            o->rects[o->n++] = r->rects[i];
}

void pgSubRgn(pg_region a, pg_region b, pg_region out)
{
    struct rosc_region *l = (struct rosc_region *)a, *r = (struct rosc_region *)b;
    struct rosc_region *o = (struct rosc_region *)out;
    int i, j;
    if (!o)
        return;
    o->n = 0;
    if (!l)
        return;
    for (i = 0; i < l->n; i++) {
        int hit = 0;
        for (j = 0; r && j < r->n; j++)
            if (l->rects[i].top_left.h < r->rects[j].bot_right.h
            &&  l->rects[i].bot_right.h > r->rects[j].top_left.h
            &&  l->rects[i].top_left.v < r->rects[j].bot_right.v
            &&  l->rects[i].bot_right.v > r->rects[j].top_left.v) {
                hit = 1;
                break;
            }
        if (!hit && o->n < 64)
            o->rects[o->n++] = l->rects[i];
    }
}

/* ---- fonts: Font Manager handles in style->machine_var -------------------------- */

/* other systems' font families (an imported RTF's, say) as the Font
 * Manager's: the serifs Trinity, the sans serifs Homerton, the fixed
 * pitches Corpus.  A name is matched by its start, case ignored, up to a
 * '-' or ',' ("Times-Roman", "Arial, sans-serif"). */
static const struct { const char *from, *to; } rosc_families[] = {
    { "times", "Trinity" }, { "georgia", "Trinity" }, { "garamond", "Trinity" },
    { "palatino", "Trinity" }, { "book antiqua", "Trinity" }, { "cambria", "Trinity" },
    { "new york", "Trinity" }, { "serif", "Trinity" }, { "roman", "Trinity" },
    { "helvetica", "Homerton" }, { "arial", "Homerton" }, { "verdana", "Homerton" },
    { "calibri", "Homerton" }, { "tahoma", "Homerton" }, { "geneva", "Homerton" },
    { "lucida grande", "Homerton" }, { "trebuchet", "Homerton" }, { "segoe", "Homerton" },
    { "sans", "Homerton" }, { "swiss", "Homerton" },
    { "courier", "Corpus" }, { "monaco", "Corpus" }, { "menlo", "Corpus" },
    { "consolas", "Corpus" }, { "lucida console", "Corpus" }, { "mono", "Corpus" },
};

static void rosc_map_family(char *name, size_t cap)
{
    char lc[64];
    size_t i, n;
    for (n = 0; name[n] && name[n] != '-' && name[n] != ',' && n < sizeof lc - 1; n++)
        lc[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
    lc[n] = 0;
    for (i = 0; i < sizeof rosc_families / sizeof rosc_families[0]; i++)
        if (!strncmp(lc, rosc_families[i].from, strlen(rosc_families[i].from))) {
            strncpy(name, rosc_families[i].to, cap - 1);
            name[cap - 1] = 0;
            return;
        }
    name[n] = 0;                        /* the family, without a "-Bold" */
}

static void rosc_font_name(font_info_ptr font, style_info_ptr style, char *out, size_t cap)
{
    size_t n = 0, i;
    out[0] = 0;
    if (font && font->name[0]) {
        n = (size_t)font->name[0];
        if (n > cap - 16)               /* room for the face */
            n = cap - 16;
        for (i = 0; i < n; i++)
            out[i] = (char)font->name[i + 1];
        out[n] = 0;
    }
    if (!out[0])
        strcpy(out, "Trinity");
    if (!strchr(out, '.')) {            /* a family without a face: the style's */
        int bold = style && style->styles[bold_var], italic = style && style->styles[italic_var];
        const char *slant;
        rosc_map_family(out, cap - 16);
        slant = !strcmp(out, "Trinity") ? ".Italic" : ".Oblique";  /* Trinity's slant is Italic */
        strcat(out, bold ? ".Bold" : ".Medium");
        if (italic)
            strcat(out, slant);
    }
}

static int rosc_find_font(const char *name, short point16)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    r.r[1] = (int)(uintptr_t)name;
    r.r[2] = point16;
    r.r[3] = point16;
    r.r[4] = 0;
    r.r[5] = 0;
    if (_kernel_swi(Font_FindFont, &r, &r))
        return 0;
    return (int)r.r[0];
}

/* the Font Manager handle for this style, kept in its machine_var with,
 * in machine_var2, a key of the font asked for (its full name and size):
 * a style whose font, face or size has changed since -- or one the engine
 * copied from another, handle and all -- finds its own.  Handles are never
 * lost: the engine copies style records without telling the machine
 * layer, so a handle may be in several, and a FindFont of a font already
 * claimed only counts it again. */
static unsigned rosc_font_key(const char *name, short point16)
{
    unsigned h = 2166136261u;
    while (*name)
        h = (h ^ (unsigned char)*name++) * 16777619u;
    return (h ^ (unsigned short)point16) | 1u;     /* never 0 */
}

static int rosc_font_handle(style_info_ptr style, font_info_ptr font)
{
    char name[64];
    short point16;
    unsigned key;
    int handle;

    if (!style)
        return 0;
    rosc_font_name(font, style, name, sizeof name);
    point16 = (short)((style->point >> 16) * 16);
    if (point16 <= 0)
        point16 = 12 * 16;
    key = rosc_font_key(name, point16);
    if (style->machine_var && (unsigned)style->machine_var2 == key)
        return (int)style->machine_var;
    handle = rosc_find_font(name, point16);
    if (!handle)
        handle = rosc_find_font("Trinity.Medium", point16);
    style->machine_var = handle;
    style->machine_var2 = handle ? (long)key : 0;
    return handle;
}

/* a run of UTF-16 Paige chars to a Latin-1 buffer, tabs to spaces */
static long rosc_to_latin(pg_char_ptr data, long length, char *buf, long cap)
{
    long i, n = 0;
    for (i = 0; i < length && n < cap - 1; i++) {
        unsigned ch = data[i];
        if (ch == '\t') {
            int t;
            for (t = 0; t < 4 && n < cap - 1; t++)
                buf[n++] = ' ';
        } else if (ch >= 0x20 && ch < 0x100)
            buf[n++] = (char)ch;
        else if (ch == 0x2019 || ch == 0x2018)
            buf[n++] = '\'';
        else if (ch == 0x201C || ch == 0x201D)
            buf[n++] = '"';
        else if (ch == 0x2013 || ch == 0x2014)
            buf[n++] = '-';
        else if (ch >= 0x100)
            buf[n++] = '?';
        /* controls below 0x20 other than tab: dropped */
    }
    buf[n] = 0;
    return n;
}

/* each character's width in a font, millipoints: Font_StringWidth of the
 * one character, kept for the handles in use (a handle's widths are
 * dropped when the handle is lost) */
#define ROSC_WIDTHS 16
static struct rosc_widths {
    int handle;
    long w[256];
} rosc_wt[ROSC_WIDTHS];
static int rosc_wt_next;

static long rosc_string_width(int handle, const char *s);

static const long *rosc_widths(int handle)
{
    struct rosc_widths *t;
    int i;
    for (i = 0; i < ROSC_WIDTHS; i++)
        if (rosc_wt[i].handle == handle)
            return rosc_wt[i].w;
    t = &rosc_wt[rosc_wt_next];
    rosc_wt_next = (rosc_wt_next + 1) % ROSC_WIDTHS;
    t->handle = handle;
    for (i = 0; i < 256; i++) {
        char c[2];
        c[0] = (char)(i < 32 ? ' ' : i);
        c[1] = 0;
        t->w[i] = rosc_string_width(handle, c);
    }
    return t->w;
}

/* one Paige character's Latin-1 code, as rosc_to_latin makes it */
static unsigned rosc_latin(unsigned ch)
{
    if (ch < 0x100)
        return ch;
    if (ch == 0x2019 || ch == 0x2018)
        return '\'';
    if (ch == 0x201C || ch == 0x201D)
        return '"';
    if (ch == 0x2013 || ch == 0x2014)
        return '-';
    return '?';
}

/* the positions of a run, as Paige's measure procs give them: length + 1
 * of them, [0] where the run starts (the caller's, kept) and [i + 1] the
 * right edge of character i, from there.  Cumulative millipoints, rounded
 * once each, so the rounding never accumulates; slop is spread over the
 * spaces.  The result is the run's width. */
static long rosc_measure(style_info_ptr style, font_info_ptr font, pg_char_ptr data,
                         long length, long slop, long num_spaces, long *positions_l,
                         pg_text_int PG_FAR *positions_t)
{
    int handle = rosc_font_handle(style, font);
    const long *w = handle ? rosc_widths(handle) : 0;
    long tabw = (long)rosc_char_width(style) * 4 * 1000;
    long mp = 0, extra = 0, spaces_seen = 0, i, pos = 0;
    long bias_l = positions_l ? positions_l[0] : 0;
    if (positions_t)
        positions_t[0] = 0;
    for (i = 0; i < length; i++) {
        unsigned ch = data ? data[i] : ' ';
        if (ch == '\t')
            mp += tabw;
        else if (ch < 32)
            ;                               /* controls: no width */
        else if (w)
            mp += w[rosc_latin(ch) & 0xFF];
        else
            mp += (long)rosc_char_width(style) * 1000;
        if (ch == ' ' && slop && num_spaces) {
            spaces_seen++;
            extra = slop * spaces_seen / num_spaces;
        }
        pos = (mp + 500) / 1000 + extra;
        if (positions_l)
            positions_l[i + 1] = bias_l + pos;
        if (positions_t)
            positions_t[i + 1] = (pg_text_int)pos;
    }
    return pos;
}

static long rosc_string_width(int handle, const char *s)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    r.r[0] = handle;
    r.r[1] = (int)(uintptr_t)s;
    r.r[2] = 0x7FFFFFFF;            /* no limit across (millipoints) */
    r.r[3] = 0x7FFFFFFF;            /* nor down */
    r.r[4] = -1;                    /* no split character */
    r.r[5] = 0x7FFFFFFF;            /* the whole string */
    if (_kernel_swi(Font_StringWidth, &r, &r))
        return 0;
    return (long)r.r[2];        /* millipoints */
}

/* ---- the machine surface ---------------------------------------------------------- */

static void rosc_init_char(pg_char_ptr gc, pg_short_t v)
{
    gc[0] = (pg_char)v;
    gc[1] = 0;
    gc[2] = 0;
    gc[3] = 0;
}

static short rosc_point(style_info_ptr style)
{
    long p;
    if (!style || !style->point)
        return (short)DEF_POINT_SIZE;
    p = style->point >> 16;
    return (short)(p > 1 ? p : 1);
}

static short rosc_char_width(style_info_ptr style)
{
    short point = rosc_point(style);
    long w = (style && style->char_width) ? (style->char_width >> 16) : 0;
    if (w <= 0)
        w = point / 2;
    return (short)(w > 1 ? w : 1);
}

static void rosc_make_device(const pg_globals_ptr globals, const generic_var port,
        size_t machine_ref, graf_device_ptr device)
{
    if (!device)
        return;
    memset((void *)device, 0, sizeof(graf_device));
    device->machine_var = port;
    device->machine_ref = machine_ref;
    device->clip_rgn = pgCreateRgn();
    device->scroll_rgn = pgCreateRgn();
    device->resolution = (72L << 16) | 72L;
    device->scale.scale = 0;            /* none (a scale is num<<16 | denom) */
    device->bk_color.red = device->bk_color.green = device->bk_color.blue = 0xFFFF;
    if (globals)
        device->graf_stack = MemoryAlloc(globals->mem_globals, sizeof(port_preserve), 0, 4);
}

static int rosc_word_break(pg_char ch, pg_globals_ptr g)
{
    return ch == ' ' || ch == '\t' || ch == (unsigned)g->hyphen_char[0]
        || ch == g->line_wrap_char || ch == g->soft_line_char;
}

PG_PASCAL (void) pgMachineInit(pg_globals_ptr globals)
{
    pg_globals_ptr g = globals;

    g->max_offscreen = MAX_OFFSCREEN;
    g->max_block_size = MAX_TEXTBLOCK;
    g->def_tab_space = DEF_TAB_SPACE;
    g->minimum_line_width = DEF_MIN_WIDTH;
    g->line_wrap_char = CR_CHAR;
    g->soft_line_char = SOFT_CR_CHAR;
    g->tab_char = TAB_CHAR;
    g->soft_hyphen_char = SOFT_HYPHEN;
    g->bs_char = DELETE_CHAR;
    g->ff_char = FF_CHAR;
    g->container_brk_char = CONTAINER_BRK_CHAR;
    g->left_arrow_char = LEFT_ARROW;
    g->right_arrow_char = RIGHT_ARROW;
    g->up_arrow_char = UP_ARROW;
    g->down_arrow_char = DOWN_ARROW;
    g->text_brk_char = TEXT_BRK_CHAR;
    g->fwd_delete_char = FWD_DELETE_CHAR;
    g->machine_specific = 0;

    rosc_init_char(g->hyphen_char, HYPHEN_CHAR);
    rosc_init_char(g->decimal_char, DECIMAL_CHAR);
    rosc_init_char(g->cr_invis_symbol, INVIS_CR);
    rosc_init_char(g->lf_invis_symbol, INVIS_LF);
    rosc_init_char(g->tab_invis_symbol, INVIS_TAB);
    rosc_init_char(g->end_invis_symbol, INVIS_END);
    rosc_init_char(g->cont_invis_symbol, INVIS_BREAK);
    rosc_init_char(g->pbrk_invis_symbol, INVIS_PBREAK);
    rosc_init_char(g->space_invis_symbol, INVIS_SPACE);
    rosc_init_char(g->elipse_symbol, ELIPSE_SYMBOL);
    rosc_init_char(g->flat_single_quote, APOSTROPHE);
    rosc_init_char(g->flat_double_quote, FLAT_QUOTE_CHAR);
    rosc_init_char(g->left_single_quote, LEFT_SINGLE_QUOTE);
    rosc_init_char(g->right_single_quote, RIGHT_SINGLE_QUOTE);
    rosc_init_char(g->left_double_quote, LEFT_DOUBLE_QUOTE);
    rosc_init_char(g->right_double_quote, RIGHT_DOUBLE_QUOTE);
    rosc_init_char(g->unknown_char, UNSUPPORTED_CHAR);
    rosc_init_char(g->bullet_char, BULLET_CHAR);

    pgInitDefaultFont(g, &g->def_font);
    pgInitDefaultStyle(g, &g->def_style, &g->def_font);
    pgInitDefaultPar(g, &g->def_par);

    g->def_bk_color.red = g->def_bk_color.green = g->def_bk_color.blue = 0xFFFF;
    g->trans_color = g->def_bk_color;
    g->system_version = PAIGE_OS;
    g->color_enable = TRUE;
    g->bullet_size = 5;
    g->alpha_widths = MemoryAlloc(g->mem_globals, sizeof(pg_text_int), 0, 128);

    rosc_make_device(g, 0, 0, &g->offscreen_port);
    g->offscreen_enable = FALSE;
    g->offscreen_exclusion = pgRectToShape(g->mem_globals, 0);
}

PG_PASCAL (void) pgMachineShutdown(const pg_globals_ptr globals)
{
    if (!globals)
        return;
    pgCloseDevice(globals, &globals->offscreen_port);
    DisposeNonNilMemory(globals->offscreen_exclusion);
    DisposeNonNilMemory(globals->alpha_widths);
    DisposeNonNilMemory(globals->pg_list);
}

PG_PASCAL (void) pgInitDefaultDevice(const pg_globals_ptr globals, graf_device_ptr device)
{
    pgInitDevice(globals, 0, 0, device);
}

PG_PASCAL (void) pgInitDefaultFont(const pg_globals_ptr globals, font_info_ptr font)
{
    static const char name[] = "Homerton";
    size_t i;
    (void)globals;
    if (!font)
        return;
    font->environs = FONT_GOOD;
    font->platform = PAIGE_GRAPHICS;
    font->char_type = 0;
    font->code_page = 0;
    font->language = 0;
    if (!font->name[0]) {
        font->name[0] = (pg_char)(sizeof(name) - 1);
        for (i = 0; i < sizeof(name) - 1; i++)
            font->name[i + 1] = (pg_char)name[i];
        font->name[sizeof(name)] = 0;
    }
}

PG_PASCAL (void) pgInitDevice(const pg_globals_ptr globals, const generic_var the_port,
        size_t machine_ref, graf_device_ptr device)
{
    (void)globals;
    rosc_make_device(globals, the_port, machine_ref, device);
}

PG_PASCAL (void) pgCloseDevice(const pg_globals_ptr globals, const graf_device_ptr device)
{
    (void)globals;
    if (!device)
        return;
    DisposeNonNilMemory(device->graf_stack);
    if (device->clip_rgn)
        pgDisposeRgn(device->clip_rgn);
    if (device->scroll_rgn)
        pgDisposeRgn(device->scroll_rgn);
    ((graf_device_ptr)device)->graf_stack = MEM_NULL;
    ((graf_device_ptr)device)->clip_rgn = REGION_NULL;
    ((graf_device_ptr)device)->scroll_rgn = REGION_NULL;
}

PG_PASCAL (short) pgInsertQuery(paige_rec_ptr pg, pg_char_ptr the_char, short charsize)
{
    (void)pg; (void)the_char; (void)charsize;
    return (short)key_insert_mode;
}

PG_PASCAL (pg_word) pgCharClassProc(paige_rec_ptr pg, pg_char_ptr the_char, short charsize,
        style_info_ptr style, font_info_ptr font)
{
    (void)pg; (void)charsize; (void)style; (void)font;
    return 0;
}

PG_PASCAL (void) pgDrawHiliteProc(paige_rec_ptr pg, shape_ref rgn)
{
    (void)pg; (void)rgn;        /* the application inverts the selection */
}

PG_PASCAL (void) pgDrawCursorProc(paige_rec_ptr pg, t_select_ptr select, short verb)
{
    (void)select;               /* the Wimp's caret is the application's */
    if (!pg)
        return;
    switch (verb) {
    case hide_cursor:
    case deactivate_cursor:
    case dont_draw_cursor:
        pg->flags &= ~CARET_BIT;
        break;
    case show_cursor:
    case toggle_cursor:
    case restore_cursor:
    case update_cursor:
        if (!(pg->flags & NO_CARET_BIT))
            pg->flags |= CARET_BIT;
        break;
    default:
        break;
    }
}

PG_PASCAL (void) pgInitFont(paige_rec_ptr pg, font_info_ptr info)
{
    if (!info || !info->name[0])
        return;
    if (!info->platform)
        info->platform = PAIGE_GRAPHICS;
    info->environs &= ~(FONT_USES_ALTERNATE | FONT_NOT_AVAIL | FONT_BEST_GUESS);
    pgFixFontName(info);
    if (!pg || pgIsRealFont(pg->globals, info, FALSE))
        info->environs |= FONT_GOOD;
    else
        info->environs |= FONT_NOT_AVAIL | FONT_BEST_GUESS;
}

PG_PASCAL (void) pgStyleInitProc(paige_rec_ptr pg, style_info_ptr style, font_info_ptr font)
{
    int handle;
    (void)font;
    if (!style)
        return;
    /* defaults first, as the portable layer does, then the Font Manager's */
    {
        short point = rosc_point(style);
        style->ascent = (short)((point * 3) / 4);
        style->descent = (short)(point - style->ascent > 1 ? point - style->ascent : 1);
        style->leading = (pg && (pg->flags & NO_DEFAULT_LEADING)) ? 0 : 1;
        style->char_width = (pg_fixed)(rosc_char_width(style)) << 16;
        style->right_overhang = 0;
        style->left_overhang = 0;
    }
    handle = rosc_font_handle(style, font);
    if (handle) {
        /* ascent/descent from CharBBox of 'd' and 'g': millipoints -> points */
        {
            long up = 0, down = 0, i;
            const char ups[] = "Hd", downs[] = "gp";
            _kernel_swi_regs r;
            for (i = 0; i < 2; i++) {
                memset(&r, 0, sizeof r);
                r.r[0] = handle;
                r.r[1] = (int)(unsigned char)ups[i];
                if (!_kernel_swi(Font_CharBBox, &r, &r) && r.r[4] > up)
                    up = r.r[4];
                memset(&r, 0, sizeof r);
                r.r[0] = handle;
                r.r[1] = (int)(unsigned char)downs[i];
                if (!_kernel_swi(Font_CharBBox, &r, &r) && r.r[2] < down)
                    down = r.r[2];
            }
            if (up > 0)
                style->ascent = (short)((up + 500) / 1000);
            if (down < 0)
                style->descent = (short)((-down + 500) / 1000);
        }
    }
}

PG_PASCAL (void) pgInstallFont(paige_rec_ptr pg, style_info_ptr the_style,
        font_info_ptr the_font, style_info_ptr composite_style, short style_overlay,
        pg_boolean include_offscreen)
{
    style_info imposed;
    font_info imposed_font;
    style_info_ptr use = the_style;
    font_info_ptr use_font = the_font;
    (void)include_offscreen;
    if (pg && the_style && style_overlay) {
        pgStyleSuperImpose(pg, the_style, &imposed, &imposed_font, style_overlay);
        use = &imposed;
        use_font = &imposed_font;
    }
    if (use) {
        rosc_font_handle(use, use_font);
        pgStyleInitProc(pg, use, use_font);
    }
    if (composite_style && the_style) {
        pgBlockMove(use ? use : the_style, composite_style, sizeof(style_info));
        composite_style->machine_var = 0;
        composite_style->machine_var2 = 0;
    }
}

PG_PASCAL (void) pgDeleteStyleProc(paige_rec_ptr pg, pg_globals_ptr globals,
        short reason_verb, format_ref all_styles, style_info_ptr style)
{
    (void)pg; (void)globals; (void)reason_verb; (void)all_styles;
    if (style) {                        /* the handle stays claimed (above) */
        style->machine_var = 0;
        style->machine_var2 = 0;
    }
}

PG_PASCAL (void) pgDupStyleProc(paige_rec_ptr src_pg, paige_rec_ptr target_pg,
        short reason_verb, format_ref all_styles, style_info_ptr style)
{
    (void)src_pg; (void)target_pg; (void)reason_verb; (void)all_styles;
    if (style) {
        style->machine_var = 0;         /* the handle is not shared */
        style->machine_var2 = 0;
    }
}

PG_PASCAL (void) pgSaveStyleProc(paige_rec_ptr pg, style_info_ptr style_to_save)
{
    (void)pg;
    if (style_to_save) {
        style_to_save->machine_var = 0;
        style_to_save->machine_var2 = 0;
    }
}

PG_PASCAL (void) pgMeasureProc(paige_rec_ptr pg, style_walk_ptr walker,
        pg_char_ptr data, size_t length, pg_short_t slop, long PG_FAR *positions,
        short PG_FAR *types, short measure_verb, size_t current_offset,
        pg_boolean scale_widths, short call_order)
{
    style_info_ptr style;
    font_info_ptr font;
    long num_spaces = 0, i;

    (void)measure_verb; (void)current_offset; (void)scale_widths; (void)call_order;

    style = walker ? walker->cur_style : (pg ? &pg->globals->def_style : 0);
    font = walker ? walker->cur_font : (pg ? &pg->globals->def_font : 0);
    if (data)
        for (i = 0; (size_t)i < length; i++)
            if (data[i] == ' ')
                num_spaces++;
    rosc_measure(style, font, data, (long)length, (long)slop, num_spaces, positions, 0);

    if (types)
        for (i = 0; (size_t)i < length; i++) {
            long info = pgCharInfoProc(pg, walker, data, 0, 0, length, (size_t)i, ALL_INFO_BITS);
            types[i] = (short)(info & (CTL_CHAR_BITS | WORD_BREAK_BIT | WORD_SEL_BIT | BLANK_BIT));
        }
}

PG_PASCAL (long) pgCharInfoProc(paige_rec_ptr pg, style_walk_ptr style_walker,
        pg_char_ptr data, size_t block_offset, size_t offset_begin, size_t offset_end,
        size_t char_offset, long mask_bits)
{
    pg_char ch;
    long result = 0;

    (void)block_offset; (void)offset_begin; (void)offset_end;

    if (!pg || !data)
        return 0;
    if (style_walker && style_walker->cur_style && (pg->flags & NO_HIDDEN_TEXT_BIT)
            && style_walker->cur_style->styles[hidden_text_var])
        return 0;

    ch = data[char_offset];
    if (ch == ' ')
        result |= BLANK_BIT | WORD_BREAK_BIT | WORD_SEL_BIT;
    if (rosc_word_break(ch, pg->globals))
        result |= WORD_BREAK_BIT | WORD_SEL_BIT;
    if (ch == pg->globals->line_wrap_char)
        result |= CTL_BIT | PAR_SEL_BIT;
    if (ch == pg->globals->soft_line_char)
        result |= CTL_BIT | LINE_SEL_BIT;
    if (ch == pg->globals->tab_char)
        result |= CTL_BIT | TAB_BIT | WORD_BREAK_BIT | WORD_SEL_BIT;
    if (ch == pg->globals->container_brk_char)
        result |= CTL_BIT | CONTAINER_BRK_BIT;
    if (ch == pg->globals->ff_char)
        result |= CTL_BIT | PAGE_BRK_BIT;
    if (ch >= '0' && ch <= '9')
        result |= NUMBER_BIT;
    if (ch == (unsigned)pg->globals->decimal_char[0])
        result |= DECIMAL_CHAR_BIT;
    if (ch >= 'A' && ch <= 'Z')
        result |= UPPER_CASE_BIT | EUROPEAN_BIT;
    if (ch >= 'a' && ch <= 'z')
        result |= LOWER_CASE_BIT | EUROPEAN_BIT;
    if (ch == APOSTROPHE)
        result |= SINGLE_QUOTE_BIT | FLAT_QUOTE_BIT;
    if (ch == FLAT_QUOTE_CHAR)
        result |= FLAT_QUOTE_BIT;
    if (ch > 0x7F)
        result |= NON_ROMAN_BIT;
    return result & mask_bits;
}

PG_PASCAL (void) pgDrawProc(paige_rec_ptr pg, style_walk_ptr walker, pg_char_ptr data,
        pg_short_t offset, pg_short_t length, draw_points_ptr draw_position,
        long extra, short draw_mode)
{
    style_info_ptr style;
    font_info_ptr font;
    int handle;
    char buf[512];
    long n, width, spaces = 0, i;

    (void)pg; (void)draw_mode;
    if (!walker || !data || !draw_position || length <= 0)
        return;
    style = walker->cur_style;
    font = walker->cur_font;
    handle = rosc_font_handle(style, font);
    for (i = offset; i < offset + length; i++)
        if (data[i] == ' ')
            spaces++;
    width = rosc_measure(style, font, data + offset, length, extra, spaces, 0, 0);

    if (rosc_has_target() && handle) {
        long x = rosc_osx(draw_position->from.h);
        long y = rosc_osy(draw_position->from.v);
        unsigned fg = rosc_oscol(style ? &style->fg_color : 0);
        unsigned bg = 0xFFFFFF00u;
        n = rosc_to_latin(data + offset, length, buf, (long)sizeof buf);
        if (n > 0) {
            _swix(ColourTrans_SetFontColours, _INR(0, 3), handle, (int)bg, (int)fg, 14);
            if (spaces && extra) {
                /* justified: the extra spread over the spaces (Font_Paint's
                 * R5 block: space and letter offsets, millipoints) */
                int block[4];
                block[0] = (int)(extra * 1000 / spaces);
                block[1] = 0;
                block[2] = 0;
                block[3] = 0;
                _swix(Font_Paint, _INR(0, 5), handle, buf, 0x10 | 0x20 | 0x100,
                      (int)x, (int)y, block);
            } else
                _swix(Font_Paint, _INR(0, 4), handle, buf, 0x10 | 0x100, (int)x, (int)y);
        }
        if (style && (style->styles[underline_var] || style->styles[word_underline_var]))
            rosc_hline(draw_position->from.h, draw_position->from.h + width,
                       draw_position->from.v + 2, fg);
        if (style && style->styles[dbl_underline_var]) {
            rosc_hline(draw_position->from.h, draw_position->from.h + width,
                       draw_position->from.v + 2, fg);
            rosc_hline(draw_position->from.h, draw_position->from.h + width,
                       draw_position->from.v + 4, fg);
        }
        if (style && style->styles[strikeout_var])
            rosc_hline(draw_position->from.h, draw_position->from.h + width,
                       draw_position->from.v - rosc_point(style) / 3, fg);
    }
    /* where the pen ended, as the Windows proc says it: from stays */
    draw_position->to.h = draw_position->from.h + width;
    draw_position->to.v = draw_position->from.v;
}

PG_PASCAL (void) pgSpecialCharProc(paige_rec_ptr pg, style_walk_ptr walker, pg_char_ptr data,
        pg_short_t offset, pg_short_t length, draw_points_ptr draw_position,
        long extra, short draw_mode)
{
    pgDrawProc(pg, walker, data, offset, length, draw_position, extra, draw_mode);
}

PG_PASCAL (void) pgTabDrawProc(paige_rec_ptr pg, style_walk_ptr walker, tab_stop_ptr tab,
        draw_points_ptr draw_position)
{
    (void)pg; (void)tab; (void)walker; (void)draw_position;
    /* a tab draws nothing: the engine places what follows it */
}

PG_PASCAL (void) pgIdleProc(paige_rec_ptr pg, short verb)
{
    (void)pg; (void)verb;
}

PG_PASCAL (void) pgSetGrafDevice(paige_rec_ptr pg, short verb, graf_device_ptr device,
        color_value_ptr bk_color)
{
    (void)bk_color;
    if (!pg || !device)
        return;
    if (verb == set_pg_device) {
        port_preserve *preserve;
        if (!device->graf_stack)
            device->graf_stack = MemoryAlloc(pg->globals->mem_globals, sizeof(port_preserve), 0, 4);
        preserve = (port_preserve *)AppendMemory(device->graf_stack, 1, TRUE);
        if (preserve) {
            preserve->last_device = pg->globals->current_port;
            preserve->clip_rgn = device->clip_rgn;
            UnuseMemory(device->graf_stack);
        }
        pg->globals->current_port = device;
        ++device->access_ctr;
    } else {
        if (device->graf_stack && GetMemorySize(device->graf_stack)) {
            size_t last = GetMemorySize(device->graf_stack) - 1;
            port_preserve *preserve =
                (port_preserve *)UseMemoryRecord(device->graf_stack, last, 1, FALSE);
            pg->globals->current_port = preserve ? preserve->last_device : 0;
            UnuseMemory(device->graf_stack);
            DeleteMemory(device->graf_stack, last, 1);
        } else
            pg->globals->current_port = 0;
    }
}

PG_PASCAL (generic_var) pgGetPlatformDevice(graf_device_ptr the_device)
{
    return the_device ? the_device->machine_var : 0;
}

PG_PASCAL (void) pgReleasePlatformDevice(graf_device_ptr the_device)
{
    (void)the_device;
}

PG_PASCAL (void) pgClipGrafDevice(paige_rec_ptr pg, short clip_verb, shape_ref alternate_vis)
{
    (void)pg; (void)clip_verb; (void)alternate_vis;
    /* the Wimp's redraw rectangle is the clip: our redraw loop asks for
     * each rectangle and the VDU graphics window is set as it comes */
}

PG_PASCAL (void) pgSetMeasureDevice(paige_rec_ptr pg) { (void)pg; }
PG_PASCAL (void) pgUnsetMeasureDevice(paige_rec_ptr pg) { (void)pg; }
PG_PASCAL (void) pgPrintDeviceChanged(paige_rec_ptr pg) { (void)pg; }

PG_PASCAL (void) pgPrepareOffscreen(paige_rec_ptr pg, rectangle_ptr target_area,
        rectangle_ptr real_bits_target, co_ordinate_ptr offset_adjust,
        long text_offset, point_start_ptr line_start, short draw_mode)
{
    (void)pg; (void)text_offset; (void)line_start; (void)draw_mode;
    if (real_bits_target && target_area)
        *real_bits_target = *target_area;
    if (offset_adjust)
        offset_adjust->h = offset_adjust->v = 0;
}

PG_PASCAL (pg_boolean) pgFinishOffscreen(paige_rec_ptr pg, long text_offset,
        point_start_ptr line_start, co_ordinate_ptr new_offset,
        rectangle_ptr new_target, short draw_mode)
{
    (void)pg; (void)text_offset; (void)line_start; (void)new_offset; (void)new_target;
    (void)draw_mode;
    return FALSE;
}

PG_PASCAL (void) pgScaleGrafDevice(paige_rec_ptr pg) { (void)pg; }

PG_PASCAL (pg_region) pgScrollRect(paige_rec_ptr pg, rectangle_ptr rect, long distance_h,
        long distance_v, rectangle_ptr affected_area, short draw_mode)
{
    pg_region result;
    struct rosc_region *r;
    (void)distance_h; (void)distance_v; (void)draw_mode;
    result = pgCreateRgn();
    r = (struct rosc_region *)result;
    if (rect && r) {
        r->rects[0] = *rect;
        r->n = 1;
    }
    if (affected_area && rect)
        *affected_area = *rect;
    return result;
}

PG_PASCAL (void) pgEraseRect(pg_globals_ptr globals, rectangle_ptr rect,
        pg_scale_ptr scaling, co_ordinate_ptr offset_extra)
{
    rectangle converted;
    if (!globals || !rect || pgEmptyRect(rect))
        return;
    pgScaleRectToRect(scaling, rect, &converted, offset_extra);
    rosc_plot_rect(converted.top_left.h, converted.top_left.v,
                   converted.bot_right.h, converted.bot_right.v,
                   rosc_oscol(globals->current_port ? &globals->current_port->bk_color : 0));
}

PG_PASCAL (pg_short_t) pgMeasureText(paige_rec_ptr pg, short measure_verb, pg_char_ptr data,
        long length, long slop, long num_spaces, pg_text_int PG_FAR *positions,
        style_walk_ptr walker)
{
    style_info_ptr style = walker ? walker->cur_style : (pg ? &pg->globals->def_style : 0);
    font_info_ptr font = walker ? walker->cur_font : (pg ? &pg->globals->def_font : 0);
    (void)measure_verb;
    return (pg_short_t)rosc_measure(style, font, data, length, slop, num_spaces, 0, positions);
}

PG_PASCAL (pg_short_t) pgMeasureText32(paige_rec_ptr pg, short measure_verb, pg_char_ptr data,
        long length, long slop, long num_spaces, pg_text_int PG_FAR *positions,
        style_walk_ptr walker)
{
    return pgMeasureText(pg, measure_verb, data, length, slop, num_spaces, positions, walker);
}

PG_PASCAL (pg_short_t) pgMeasureText16(paige_rec_ptr pg, short measure_verb, pg_char_ptr data,
        long length, long slop, long num_spaces, pg_text_int PG_FAR *positions,
        style_walk_ptr walker)
{
    return pgMeasureText(pg, measure_verb, data, length, slop, num_spaces, positions, walker);
}

PG_PASCAL (short) pgScalePointSize(paige_rec_ptr pg, style_walk_ptr walker,
        pg_char_ptr text, long length, pg_boolean PG_FAR *did_scale)
{
    (void)pg; (void)text; (void)length;
    if (did_scale)
        *did_scale = FALSE;
    return rosc_point(walker ? walker->cur_style : 0);
}

PG_PASCAL (pg_boolean) pgIsRealFont(pg_globals_ptr globals, font_info_ptr font,
        pg_boolean use_alternate)
{
    char name[64];
    (void)globals; (void)use_alternate;
    if (!font)
        return FALSE;
    rosc_font_name(font, 0, name, sizeof name);
    return (pg_boolean)(rosc_find_font(name, 12 * 16) != 0);    /* the Font Manager has it */
}

PG_PASCAL (pg_fixed) pgPointsizeToScreen(pg_ref pg, pg_fixed pointsize)
{
    (void)pg;
    return pointsize;           /* one Paige unit the point */
}

PG_PASCAL (pg_fixed) pgScreenToPointsize(pg_ref pg, pg_fixed screensize)
{
    (void)pg;
    return screensize;
}

PG_PASCAL (void) pgOSToPgColor(const pg_plat_color_value PG_FAR *os_color,
        color_value_ptr pg_color)
{
    unsigned color;
    if (!os_color || !pg_color)
        return;
    color = *os_color;
    pg_color->red = (unsigned short)(((color >> 16) & 0xFF) * 257);
    pg_color->green = (unsigned short)(((color >> 8) & 0xFF) * 257);
    pg_color->blue = (unsigned short)((color & 0xFF) * 257);
    pg_color->alpha = (pg_short_t)((color >> 24) & 0xFF);
}

PG_PASCAL (void) pgColorToOS(const color_value_ptr pg_color, pg_plat_color_value PG_FAR *os_color)
{
    if (!pg_color || !os_color)
        return;
    *os_color = ((unsigned)(pg_color->alpha & 0xFF) << 24)
              | ((unsigned)(pg_color->red >> 8) << 16)
              | ((unsigned)(pg_color->green >> 8) << 8)
              | (unsigned)(pg_color->blue >> 8);
}

PG_PASCAL (void) pgTransLiterate(pg_char_ptr text, long length, pg_char_ptr target,
        pg_boolean do_uppercase)
{
    long i;
    if (!text || !target)
        return;
    for (i = 0; i < length; i++) {
        pg_char ch = text[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (pg_char)(do_uppercase ? ch : ch + 32);
        else if (ch >= 'a' && ch <= 'z')
            ch = (pg_char)(do_uppercase ? ch - 32 : ch);
        target[i] = ch;
    }
}

PG_PASCAL (void) pgOpenPrinter(paige_rec_ptr pg_rec, graf_device_ptr print_dev,
        long first_position, rectangle_ptr page_rect)
{
    (void)first_position; (void)page_rect;
    if (pg_rec && print_dev)
        pgInitDevice(pg_rec->globals, 0, 0, print_dev);
}

PG_PASCAL (void) pgClosePrinter(paige_rec_ptr pg_rec, graf_device_ptr print_dev)
{
    if (pg_rec && print_dev)
        pgCloseDevice(pg_rec->globals, print_dev);
}

PG_PASCAL (pg_boolean) pgIsCaretTime(paige_rec_ptr pg)
{
    static unsigned last;
    unsigned now;
    _kernel_swi_regs r;
    static unsigned char buf[8];
    if (!pg)
        return FALSE;
    memset(&r, 0, sizeof r);
    r.r[0] = 1;                     /* OS_Word 1: the monotonic clock */
    r.r[1] = (int)(uintptr_t)buf;
    if (_kernel_swi(OS_Word, &r, &r))
        return TRUE;
    now = (unsigned)buf[0] | ((unsigned)buf[1] << 8) | ((unsigned)buf[2] << 16)
        | ((unsigned)buf[3] << 24);
    if (pg->port.caret_info == now)
        return FALSE;
    pg->port.caret_info = now;
    return TRUE;
}

PG_PASCAL (short) pgGetCharWidth(paige_rec_ptr pg_rec, style_info_ptr style, pg_char the_char)
{
    pg_char c = the_char;
    (void)pg_rec;
    return (short)rosc_measure(style, 0, &c, 1, 0, 0, 0, 0);
}

PG_PASCAL (void) pgDrawSpecialUnderline(paige_rec_ptr pg, Point from_pt,
        short distance, style_info_ptr style, short draw_bits)
{
    (void)pg; (void)from_pt; (void)distance; (void)style; (void)draw_bits;
}

PG_PASCAL (void) SetFontCharWidths(pg_ref pg, style_info_ptr style, int PG_FAR *charwidths)
{
    (void)pg; (void)style; (void)charwidths;
}

/* ---- the engine's smaller externs --------------------------------------------- */

PG_PASCAL (long) PaigeToQDStyle(const style_info_ptr the_style)
{
    long result = 0;
    if (the_style->styles[bold_var])
        result |= 1;
    if (the_style->styles[italic_var])
        result |= 2;
    if (the_style->styles[underline_var])
        result |= 4;
    if (the_style->styles[outline_var])
        result |= 8;
    if (the_style->styles[shadow_var])
        result |= 0x10;
    if (the_style->styles[superscript_var])
        result |= 0x20;
    if (the_style->styles[subscript_var])
        result |= 0x40;
    if (the_style->styles[strikeout_var])
        result |= 0x100;
    return result;
}

PG_PASCAL (void) QDStyleToPaige(long qd_styles, style_info_ptr the_style)
{
    pgFillBlock(the_style->styles, sizeof(the_style->styles), 0);
    if (qd_styles & 1)
        the_style->styles[bold_var] = -1;
    if (qd_styles & 2)
        the_style->styles[italic_var] = -1;
    if (qd_styles & 4)
        the_style->styles[underline_var] = -1;
    if (qd_styles & 8)
        the_style->styles[outline_var] = -1;
    if (qd_styles & 0x10)
        the_style->styles[shadow_var] = -1;
    if (qd_styles & 0x20)
        the_style->styles[superscript_var] = -1;
    if (qd_styles & 0x40)
        the_style->styles[subscript_var] = -1;
    if (qd_styles & 0x100)
        the_style->styles[strikeout_var] = -1;
}

PG_PASCAL (void) RectToRectangle(Rect PG_FAR *the_rect, rectangle_ptr the_rectangle)
{
    the_rectangle->top_left.h = (short)the_rect->left;
    the_rectangle->top_left.v = (short)the_rect->top;
    the_rectangle->bot_right.h = (short)the_rect->right;
    the_rectangle->bot_right.v = (short)the_rect->bottom;
}

PG_PASCAL (void) RectangleToRect(rectangle_ptr the_rectangle, co_ordinate_ptr offset,
        Rect PG_FAR *the_rect)
{
    the_rect->left = the_rectangle->top_left.h;
    the_rect->top = the_rectangle->top_left.v;
    the_rect->right = the_rectangle->bot_right.h;
    the_rect->bottom = the_rectangle->bot_right.v;
    (void)offset;
}

PG_PASCAL (short) pgSystemDirection(pg_globals_ptr globals)
{
    (void)globals;
    return (short)left_right_direction;
}

PG_PASCAL (short) pgComputePointSize(paige_rec_ptr pg, style_info_ptr style)
{
    (void)pg;
    return (pg_short_t)(rosc_point(style));
}

PG_PASCAL (pg_short_t) pgCountCtlChars(text_block_ptr block, pg_short_t ctl_char)
{
    (void)block; (void)ctl_char;
    return 0;
}

/* ---- files: a pg_file_unit is a RISC OS file handle ---------------------------- */

static pg_error rosc_args(int reason, pg_file_unit f, size_t *value)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    r.r[0] = reason;
    r.r[1] = (int)f;
    r.r[2] = value ? (int)*value : 0;
    if (_kernel_swi(OS_Args, &r, &r))
        return IO_ERR;
    if (value)
        *value = (size_t)r.r[2];
    return NO_ERROR;
}

PG_C (pg_error) pgSetFilePos(pg_file_unit ref_num, size_t offset)
{
    size_t o = offset, ext = 0;
    if (rosc_args(2, ref_num, &ext))           /* past the end: extend first */
        return IO_ERR;
    if (o > ext && rosc_args(3, ref_num, &o))
        return IO_ERR;
    o = offset;
    return rosc_args(1, ref_num, &o);
}

PG_C (pg_error) pgGetFilePos(pg_file_unit ref_num, size_t PG_FAR *offset_result)
{
    return rosc_args(0, ref_num, offset_result);
}

PG_C (pg_error) pgGetFileEOF(pg_file_unit ref_num, size_t PG_FAR *offset_result)
{
    return rosc_args(2, ref_num, offset_result);
}

PG_C (pg_error) pgSetFileEOF(pg_file_unit ref_num, size_t offset)
{
    size_t o = offset;
    return rosc_args(3, ref_num, &o);
}

PG_C (pg_error) pgReadFileData(pg_file_unit ref_num, size_t byte_size, void PG_FAR *buffer)
{
    _kernel_swi_regs r;
    if (!byte_size)
        return NO_ERROR;
    memset(&r, 0, sizeof r);
    r.r[0] = 4;                     /* OS_GBPB 4: read from the pointer */
    r.r[1] = (int)ref_num;
    r.r[2] = (int)(uintptr_t)buffer;
    r.r[3] = (int)byte_size;
    if (_kernel_swi(OS_GBPB, &r, &r))
        return IO_ERR;
    return r.r[3] ? EOF_ERR : NO_ERROR;
}

PG_C (pg_error) pgWriteFileData(pg_file_unit ref_num, size_t byte_size, const void PG_FAR *buffer)
{
    _kernel_swi_regs r;
    if (!byte_size)
        return NO_ERROR;
    memset(&r, 0, sizeof r);
    r.r[0] = 2;                     /* OS_GBPB 2: write at the pointer */
    r.r[1] = (int)ref_num;
    r.r[2] = (int)(uintptr_t)buffer;
    r.r[3] = (int)byte_size;
    if (_kernel_swi(OS_GBPB, &r, &r))
        return IO_ERR;
    return NO_ERROR;
}

/* the I/O procs, as PGOSUTL.C's: the Standard pair's filemap is a
 * memory_ref holding the file unit, the OS pair's the unit itself */
static pg_error rosc_read(pg_file_unit f_ref, void PG_FAR *data, short verb,
        size_t PG_FAR *position, size_t PG_FAR *data_size)
{
    pg_error error;
    pg_bits8_ptr data_ptr;
    if (verb == io_file_unit) {
        *((pg_file_unit PG_FAR *)data) = f_ref;
        return NO_ERROR;
    }
    if (verb == io_get_eof)
        return pgGetFileEOF(f_ref, (size_t PG_FAR *)data);
    if (verb == io_set_eof)
        return NO_ERROR;
    if ((error = pgSetFilePos(f_ref, *position)) != NO_ERROR)
        return error;
    if (verb == io_set_fpos)
        return NO_ERROR;
    if (verb == io_data_indirect) {
        SetMemorySize((memory_ref)data, *data_size);
        data_ptr = (pg_bits8_ptr)UseMemory((memory_ref)data);
    } else
        data_ptr = (pg_bits8_ptr)data;
    error = pgReadFileData(f_ref, *data_size, data_ptr);
    if (verb == io_data_indirect)
        UnuseMemory((memory_ref)data);
    if (error)
        return error;
    *position += *data_size;
    return NO_ERROR;
}

static pg_error rosc_write(pg_file_unit f_ref, void PG_FAR *data, short verb,
        size_t PG_FAR *position, size_t PG_FAR *data_size)
{
    pg_error error;
    pg_bits8_ptr data_ptr;
    if (verb == io_file_unit) {
        *((pg_file_unit PG_FAR *)data) = f_ref;
        return NO_ERROR;
    }
    if (verb == io_get_eof)
        return pgGetFileEOF(f_ref, (size_t PG_FAR *)data);
    if (verb == io_set_eof)
        return pgSetFileEOF(f_ref, *position);
    if ((error = pgSetFilePos(f_ref, *position)) != NO_ERROR)
        return error;
    if (verb == io_set_fpos)
        return NO_ERROR;
    if (verb == io_data_indirect)
        data_ptr = (pg_bits8_ptr)UseMemory((memory_ref)data);
    else
        data_ptr = (pg_bits8_ptr)data;
    error = pgWriteFileData(f_ref, *data_size, data_ptr);
    if (verb == io_data_indirect)
        UnuseMemory((memory_ref)data);
    if (error)
        return error;
    *position += *data_size;
    return NO_ERROR;
}

PG_PASCAL (pg_error) pgStandardReadProc(void PG_FAR *data, short verb,
        size_t PG_FAR *position, size_t PG_FAR *data_size, file_ref filemap)
{
    pg_file_unit f_ref;
    GetMemoryRecord(filemap, 0, (void PG_FAR *)&f_ref);
    return rosc_read(f_ref, data, verb, position, data_size);
}

PG_PASCAL (pg_error) pgStandardWriteProc(void PG_FAR *data, short verb,
        size_t PG_FAR *position, size_t PG_FAR *data_size, file_ref filemap)
{
    pg_file_unit f_ref;
    GetMemoryRecord(filemap, 0, (void PG_FAR *)&f_ref);
    return rosc_write(f_ref, data, verb, position, data_size);
}

PG_PASCAL (pg_error) pgOSReadProc(void PG_FAR *data, short verb,
        size_t PG_FAR *position, size_t PG_FAR *data_size, file_ref filemap)
{
    return rosc_read((pg_file_unit)(uintptr_t)filemap, data, verb, position, data_size);
}

PG_PASCAL (pg_error) pgOSWriteProc(void PG_FAR *data, short verb,
        size_t PG_FAR *position, size_t PG_FAR *data_size, file_ref filemap)
{
    return rosc_write((pg_file_unit)(uintptr_t)filemap, data, verb, position, data_size);
}

/* the remaining hooks the engine installs by name: functional no-ops */
PG_PASCAL (void) pgBitmapModifyProc(paige_rec_ptr pg, graf_device_ptr bits_port,
        pg_boolean post_call, rectangle_ptr bits_rect, co_ordinate_ptr screen_offset,
        long text_offset)
{
    (void)pg; (void)bits_port; (void)post_call; (void)bits_rect; (void)screen_offset;
    (void)text_offset;
}

PG_PASCAL (void) pgBkImageProc(paige_rec_ptr pg, pg_url_image_ptr image,
        generic_var device, short verb, rectangle_ptr target, rectangle_ptr clip,
        co_ordinate_ptr actual_target_offset)
{
    (void)pg; (void)image; (void)device; (void)verb; (void)target; (void)clip;
    (void)actual_target_offset;
}

/* ---- the remaining platform utilities the link asked for ---------------------- */

PG_PASCAL (void) pgInitDefaultStyle(const pg_globals_ptr globals, style_info_ptr style,
        font_info_ptr def_font)
{
    pg_style_hooks procs = style->procs;    /* pgSetStandardProcs's, kept */
    (void)globals;
    pgFillBlock(style, sizeof(style_info), 0);
    style->procs = procs;
    style->point = (long)DEF_POINT_SIZE << 16;
    style->font_index = 0;
    style->fg_color.red = 0;
    style->fg_color.green = 0;
    style->fg_color.blue = 0;
    style->bk_color.red = 0xFFFF;
    style->bk_color.green = 0xFFFF;
    style->bk_color.blue = 0xFFFF;
    pgStyleInitProc(NULL, style, def_font);
}

PG_PASCAL (void) pgInitDefaultPar(const pg_globals_ptr globals, par_info_ptr def_par)
{
    pg_par_hooks procs = def_par->procs;
    (void)globals;
    pgFillBlock(def_par, sizeof(par_info), 0);
    def_par->procs = procs;
}

/* as PGOSUTL.C's: a name given as a C string (NAME_IS_CSTR) becomes a
 * Pascal one, its length in name[0], zero-terminated after */
static void rosc_pascal_name(pg_char *name, short *environs, short flag)
{
    long length;
    if (!(*environs & flag))
        return;
    length = pgCStrLength((pg_c_string_ptr)name);
    if (length >= FONT_SIZE - 1)
        length = FONT_SIZE - 2;
    memmove(&name[1], &name[0], (size_t)length * sizeof(pg_char));
    name[0] = (pg_char)length;
    name[length + 1] = 0;
    *environs &= (short)~flag;
}

PG_PASCAL (void) pgFixFontName(font_info_ptr font)
{
    if (!font)
        return;
    rosc_pascal_name(font->name, &font->environs, NAME_IS_CSTR);
    rosc_pascal_name(font->alternate_name, &font->environs, NAME_ALT_IS_CSTR);
}

PG_PASCAL (pg_boolean) pgTransColor(pg_globals_ptr globals, color_value_ptr color)
{
    if (!globals || !color)
        return FALSE;
    return (pg_boolean)(color->red == globals->trans_color.red
        && color->green == globals->trans_color.green
        && color->blue == globals->trans_color.blue);
}

/* a C string's length in pg_chars: the engine's strings (URLs, font names,
 * pgSetFontByName's) are pg_char wide, as Windows' UNICODE lstrlen counts */
PG_PASCAL (long) pgCStrLength(const pg_c_string_ptr str)
{
    long n = 0;
    if (!str)
        return 0;
    while (((const pg_char *)str)[n])
        n++;
    return n;
}

/* the three conversions as PGOSUTL.C has them: a byte stream is
 * Unicode if it starts with a byte order mark; a NULL output asks only
 * whether it is (and, for Unicode, how many characters); bytes widen in
 * place, from the end */
PG_PASCAL (size_t) pgBytesToUnicode(pg_bits8_ptr input_bytes, pg_short_t PG_FAR *output_chars,
        font_info_ptr font, size_t input_byte_size)
{
    size_t result, index;
    int is_unicode = 0;
    (void)font;
    if (!(result = input_byte_size))
        return 0;
    if (!(input_byte_size & 1)) {
        pg_short_t first = (pg_short_t)(input_bytes[0] | input_bytes[1] << 8);
        is_unicode = first == PG_BOM || first == PG_REVERSE_BOM;
    }
    if (is_unicode) {
        if (output_chars)
            pgBlockMove(input_bytes, output_chars, input_byte_size);
        return result / sizeof(pg_short_t);
    }
    if (!output_chars)
        return FALSE;
    for (index = input_byte_size; index; --index)
        output_chars[index - 1] = (pg_short_t)input_bytes[index - 1];
    return result;
}

PG_PASCAL (size_t) pgUnicodeToBytes(pg_short_t PG_FAR *input_chars, pg_bits8_ptr output_bytes,
        font_info_ptr font, size_t input_char_size)
{
    pg_short_t PG_FAR *input = input_chars;
    pg_short_t bom = *input_chars;
    long index, input_size = (long)input_char_size;
    size_t bytecount = 0;
    (void)font;
    if (bom == PG_BOM || bom == PG_REVERSE_BOM) {
        ++input;
        --input_size;
    }
    for (index = 0; index < input_size; ++index) {
        pg_short_t c = *input++;
        if (bom == PG_REVERSE_BOM)
            c = (pg_short_t)((c >> 8) | (c << 8));
        output_bytes[bytecount++] = (pg_bits8)c;
    }
    return bytecount;
}

PG_PASCAL (size_t) pgUnicodeToUnicode(pg_short_t PG_FAR *the_chars, size_t num_chars,
        pg_boolean force_reverse)
{
    size_t char_count = num_chars, i;
    pg_short_t bom;
    if (!char_count)
        return 0;
    bom = *the_chars;
    if (bom == PG_BOM || bom == PG_REVERSE_BOM) {
        if (--char_count > 0) {
            pgBlockMove(&the_chars[1], the_chars, char_count * sizeof(pg_short_t));
            the_chars[char_count] = 0;
        }
    }
    if (force_reverse || bom == PG_REVERSE_BOM)
        for (i = 0; i < char_count; i++)
            the_chars[i] = (pg_short_t)((the_chars[i] >> 8) | (the_chars[i] << 8));
    return char_count;
}

PG_PASCAL (long) pgDeviceResolution(graf_device_ptr device)
{
    return device ? (long)device->resolution : (long)((72L << 16) | 72L);
}

/* the memory "files" the codecs read and write when given a memory_ref and
 * no file (PGIO.C's, whose POSIX file layer the procs above replace) */
PG_PASCAL (pg_error) pgScrapMemoryWrite(void PG_FAR *data, short verb, size_t PG_FAR *position,
        size_t PG_FAR *data_size, file_ref filemap)
{
    pg_bits8_ptr new_data, source_data;
    size_t ref_size;
    if (verb == io_set_fpos)
        return NO_ERROR;
    if (verb == io_set_eof) {
        SetMemorySize(filemap, *position);
        return NO_ERROR;
    }
    source_data = verb == io_data_indirect ? (pg_bits8_ptr)UseMemory((memory_ref)data) : (pg_bits8_ptr)data;
    ref_size = GetMemorySize(filemap);
    if (ref_size > *position) {
        if (*position + *data_size > ref_size)
            SetMemorySize(filemap, *position + *data_size);
        new_data = (pg_bits8_ptr)UseMemoryRecord(filemap, *position, USE_ALL_RECS, TRUE);
    } else
        new_data = (pg_bits8_ptr)AppendMemory(filemap, *data_size, FALSE);
    pgBlockMove(source_data, new_data, *data_size);
    UnuseMemory(filemap);
    if (verb == io_data_indirect)
        UnuseMemory((memory_ref)data);
    *position += *data_size;
    return NO_ERROR;
}

PG_PASCAL (pg_error) pgScrapMemoryRead(void PG_FAR *data, short verb, size_t PG_FAR *position,
        size_t PG_FAR *data_size, file_ref filemap)
{
    if (verb == io_set_fpos)
        return NO_ERROR;
    if (verb == io_file_unit) {
        *((pg_file_unit PG_FAR *)data) = (pg_file_unit)(uintptr_t)filemap;
        return NO_ERROR;
    }
    if (verb == io_get_eof)
        *(size_t PG_FAR *)data = GetMemorySize(filemap);
    else {
        pg_bits8_ptr the_data = (pg_bits8_ptr)UseMemory(filemap) + *position, target_data;
        if (verb == io_data_indirect) {
            SetMemorySize((memory_ref)data, *data_size);
            target_data = (pg_bits8_ptr)UseMemory((memory_ref)data);
        } else
            target_data = (pg_bits8_ptr)data;
        pgBlockMove(the_data, target_data, *data_size);
        UnuseMemory(filemap);
        if (verb == io_data_indirect)
            UnuseMemory((memory_ref)data);
        *position += *data_size;
    }
    return NO_ERROR;
}
