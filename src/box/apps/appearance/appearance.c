/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* appearance.c: !Appearance, the desktop's look, chosen in one window.
 *
 * The application puts an icon on the icon bar.  A click on the icon opens
 * the window, with the pages listed down its left side:
 *
 *   Themes      A card for each theme on Themes$Path, and one for Classic.
 *               A card is a small picture of a desktop in the theme's
 *               colours, with one of the theme's own icons.  A click on a
 *               card runs *WimpTheme and the desktop changes at once.
 *   Windows     The settings of the painted themes: title alignment, window
 *               buttons, scroll bars, edges and title bar height.  Each is
 *               set with *WimpThemeSet.
 *   Wallpaper   The desktop's colour, a tile chosen from the Wallpapers
 *               folders (Wallpapers$Path), and a picture dragged to the
 *               window (a sprite, JPEG or PNG file).  The picture is scaled,
 *               tiled or centred.  All of these use *Backdrop, which the
 *               Pinboard provides.  The Composite switch tiles a one-colour
 *               sprite that is tagged for the Metal display's backdrop layer,
 *               so that the host's layer shows through the desktop.  The
 *               switch is offered only where the window has that layer: the
 *               launcher says so with the system variable BOX$HostBackdrop.
 *   Icons       The icon set of the current theme.  Each set on Themes$Path
 *               that has a SpritesV file is shown with a few of its icons,
 *               drawn from the file by *VIconSet.  A click runs
 *               *WimpThemeSet Theme.Vectors and the icons change at once.
 *
 * Each choice is written as it is made to <Choices$Write>.Appearance, an
 * Obey file of the commands above.  !Boot runs that file as the desktop
 * starts, and this program reads it back.  Revert puts back what was chosen
 * when the program started.
 *
 * The window is made and drawn here.  There is no Templates file and no
 * Toolbox, as in !GraphTask.  Text uses the outline fonts, or the desktop
 * font if they are missing.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "kernel.h"
#include "swis.h"

typedef _kernel_oserror os_error;

#define MSG_QUIT          0
#define MSG_DATASAVE      1
#define MSG_DATALOAD      3
#define MSG_DATALOADACK   4
#define MSG_MODECHANGE    0x400C1
#define MSG_THEMECHANGED  0xC0240       /* sent by the Wimp when *WimpTheme changes the theme */

#define TASK_WORD 0x4B534154

enum {
    EV_NULL = 0, EV_REDRAW = 1, EV_OPEN = 2, EV_CLOSE = 3, EV_CLICK = 6, EV_KEY = 8, EV_MENU = 9,
    EV_MESSAGE = 17, EV_RECORDED = 18, EV_ACK = 19
};

/* ---- the look of the window ------------------------------------------------------------------ */

#define C_SIDEBAR   0xEEECE800u         /* &BBGGRR00 */
#define C_PANE      0xF6F7F800u
#define C_DIVIDER   0xCFD6DA00u
#define C_TEXT      0x1F222400u
#define C_SUBTEXT   0x6B727700u
#define C_DIM       0xA3ABB000u
#define C_ACCENT    0xA86E3B00u         /* #3B6EA8 */
#define C_ACCENTHI  0xF4E8DD00u
#define C_WHITE     0xFFFFFF00u
#define C_CARD      0xFFFFFF00u
#define C_CARDEDGE  0xD0D8DD00u
#define C_SHADOW    0xDDE2E500u
#define C_SEGBG     0xE1E6E900u

#define WIN_W       1776
#define SIDEBAR_W   320
#define PANE_X      360                 /* the pages' left edge */
#define HEAD_Y      (-176)              /* below the heading */
#define CARD_W      256
#define CARD_H      232
#define CARD_GAP    24
#define COLUMNS     5
#define FOOTER_H    96

/* the Wallpaper page's tiles and the Composite switch, below the picture */
#define WALL_HEAD_Y (-740)              /* "Wallpapers" baseline */
#define WALL_TOP    (-764)              /* the first row of thumbnails */
#define WALL_SIZE   112
#define WALL_PITCH  140
#define WALL_ROW_H  152
#define WALL_COLS   10
#define MAX_WALL    30
#define COMPOSITE_H 168                 /* the switch and the lines beneath it */
#define DEFAULT_TILE "Resources:$.Resources.Pinboard.Tile"

/* the Icons page: a row for each set */
#define SET_H       184
#define SET_PITCH   204
#define SET_TEXT_W  440                 /* the name and its two lines, then the samples */
#define SAMPLE_PITCH 170

/* ---- themes ---------------------------------------------------------------------------------- */

#define MAX_THEMES 40
#define NO_COLOUR  0xFFFFFFFFu

struct theme {
    char leaf[32];                      /* what *WimpTheme takes */
    char name[40];                      /* what is shown */
    char dir[200];
    int painted;
    unsigned focus[2], focus_text, unfocus[2], unfocus_text;
    int focus_stripes, unfocus_kind;    /* the fills' kinds: 0 plain, 1 stripes, 2 across, 3 ridges */
    unsigned focus_light, unfocus_light;
    char icons_name[40];                /* [Theme] Icons: the theme whose icons are these */
    unsigned track, thumb, outline, menu_bg, bar_bg, desk, bevel_light, bevel_dark;
    int bevel, align_left, buttons, scroll, title_h, font, round, shadow;
    int *icons;                         /* a sprite area of a few of its icons, or NULL */
    int frame, resize_bar;              /* Window.Frame, Window.ResizeBar: pixels */
    unsigned frame_face, button_face, button_colour;
    int buttons_right, hide_unfocused;  /* Buttons.Layout right, Buttons.HideUnfocused */
    char vectors_name[40];              /* [Theme] Vectors: the icon set it names */
    int own_vectors;                    /* its directory has a SpritesV of its own */
};

static struct theme themes[MAX_THEMES];
static int nthemes;

/* an image in memory: the Wallpaper page's picture, and each tile's thumbnail */
struct img {
    char *data;                         /* a sprite area (the file after its size word), or a JPEG */
    int type, size, w, h;               /* its size in OS units */
    char *table;                        /* a sprite's colour translation, or NULL */
};

static struct img pic;

/* the tiles on Wallpapers$Path */
static struct wall {
    char file[240];                     /* what *Backdrop takes */
    char name[24];                      /* what is shown */
    struct img im;
} walls[MAX_WALL];
static int nwalls;

/* The settings of the Windows page: -1 is the theme's own */
enum { SET_ALIGN, SET_FONT, SET_BUTTONS, SET_SCROLL, SET_BEVEL, SET_CORNERS, SET_SHADOW, SET_TITLEH, NSETTINGS };

static const struct setting {
    const char *label, *key;
    int n;
    const char *names[3], *values[3];
} settings[NSETTINGS] = {
    { "Title text", "Title.Align", 2, { "Left", "Centre" }, { "left", "centre" } },
    { "Title font", "Title.Font", 3, { "Desktop", "Sans", "Serif" },
      { "desktop", "Homerton.Bold 12", "Trinity.Bold 12" } },
    { "Window buttons", "Buttons.Set", 3, { "Glyphs", "Dots", "Boxes" }, { "glyphs", "dots", "boxes" } },
    { "Scroll bars", "Scroll.Style", 3, { "Classic", "Slim", "Channel" }, { "classic", "slim", "channel" } },
    { "Edges", "Chrome.Bevel", 2, { "Flat", "Bevelled" }, { "0", "2" } },
    { "Corners", "Chrome.Radius", 2, { "Square", "Round" }, { "0", "12" } },
    { "Shadow", "Chrome.Shadow", 2, { "None", "Soft" }, { "0", "24 110" } },
    { "Title bar", "Chrome.TitleHeight", 3, { "Compact", "Standard", "Tall" }, { "36", "44", "52" } },
};

enum { FIT_SCALE, FIT_TILE, FIT_CENTRE };
static const char *const fit_names[3] = { "Scale", "Tile", "Centre" };
static const char *const fit_switch[3] = { "-Scale", "-Tile", "-Centre" };

static const unsigned swatches[] = {
    0xBBBBBB00u, 0xA59B8C00u, 0x8A7A5E00u, 0x58482F00u, 0x1A171600u,
    0xD5E1E700u, 0xB7C0B400u, 0x6B8E6B00u, 0x2D52A000u, 0x8E5C7A00u,
};
#define NSWATCHES ((int)(sizeof swatches / sizeof swatches[0]))

/* What is chosen: kept in the Choices file */
struct choice {
    char theme[32];                     /* "" is Classic */
    int set[NSETTINGS];
    unsigned desk;                      /* NO_COLOUR: not chosen */
    char picture[220];
    int fit;
    int composite;                      /* set when the tagged tile is the wallpaper */
    char vectors[24];                   /* the icon set chosen by hand; "" is the theme's own */
};

enum { PAGE_THEMES, PAGE_WINDOWS, PAGE_WALLPAPER, PAGE_ICONS, NPAGES };
static const char *const page_names[NPAGES] = { "Themes", "Windows", "Wallpaper", "Icons" };
static const char *const page_heads[NPAGES] = { "Themes", "Windows", "Wallpaper", "Icons" };
static const char *const page_subs[NPAGES] = {
    "Click a theme: the desktop changes at once.",
    "How a painted theme draws its windows.",
    "The desktop's colour, and a picture behind your windows.",
    "The icons this theme draws with.",
};

static struct {
    int task, window, icon, open, page;
    int poll[64];
    int menu[32];
    int font_head, font_body, font_bold, font_small;
    int win_h;
    struct choice now, start;
    int quitting;
    int host_backdrop;                  /* the window has a backdrop layer of its own (BOX$HostBackdrop) */
} g;

/* ---- small things ---------------------------------------------------------------------------- */

static void report(const os_error *e)
{
    if (e)
        _swix(Wimp_ReportError, _INR(0, 2), e, 1 | 16, "Appearance");
}

static os_error *cli(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static os_error *cli(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    return _swix(OS_CLI, _IN(0), line);
}

static int read_var(const char *name, char *buf, int size)
{
    int len = 0;
    if (_swix(OS_ReadVarVal, _INR(0, 4) | _OUT(2), name, buf, size - 1, 0, 3, &len) || len <= 0) {
        buf[0] = 0;
        return 0;
    }
    buf[len] = 0;
    return len;
}

static int object_type(const char *name)
{
    int type = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0), 17, name, &type))
        return 0;
    return type;
}

/* Reads a whole file into a malloc'd block with a zero byte after it.
 * Returns NULL if that fails. */
static char *load_file(const char *name, int *size)
{
    int type = 0, len = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(4), 17, name, &type, &len) || type != 1 || len < 0 ||
        len > 16 * 1024 * 1024)
        return NULL;
    char *b = malloc((size_t)len + 1);
    if (!b)
        return NULL;
    if (_swix(OS_File, _INR(0, 3), 16, name, b, 0)) {
        free(b);
        return NULL;
    }
    b[len] = 0;
    if (size)
        *size = len;
    return b;
}

static int same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* ---- drawing --------------------------------------------------------------------------------- */

static int ox, oy;                      /* the window's work area origin on the screen */

static void gcol(unsigned c)
{
    _swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), c, 0, 0);
}

static void plot(int code, int x, int y)
{
    _swix(OS_Plot, _INR(0, 2), code, x, y);
}

/* a rectangle of the work area, x0 <= x < x1, y0 <= y < y1 */
static void rect(int x0, int y0, int x1, int y1, unsigned c)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    gcol(c);
    plot(4, ox + x0, oy + y0);
    plot(101, ox + x1 - 1, oy + y1 - 1);
}

static void disc(int cx, int cy, int r, unsigned c)
{
    gcol(c);
    plot(4, ox + cx, oy + cy);
    plot(157, ox + cx + r, oy + cy);
}

static void rrect(int x0, int y0, int x1, int y1, int r, unsigned c)
{
    if (r * 2 > x1 - x0)
        r = (x1 - x0) / 2;
    if (r * 2 > y1 - y0)
        r = (y1 - y0) / 2;
    if (r < 2) {
        rect(x0, y0, x1, y1, c);
        return;
    }
    rect(x0 + r, y0, x1 - r, y1, c);
    rect(x0, y0 + r, x0 + r, y1 - r, c);
    rect(x1 - r, y0 + r, x1, y1 - r, c);
    disc(x0 + r, y0 + r, r, c);
    disc(x1 - r - 2, y0 + r, r, c);
    disc(x0 + r, y1 - r - 2, r, c);
    disc(x1 - r - 2, y1 - r - 2, r, c);
}

/* a rounded rectangle with an edge of width e */
static void rbox(int x0, int y0, int x1, int y1, int r, int e, unsigned fill, unsigned edge)
{
    rrect(x0, y0, x1, y1, r, edge);
    rrect(x0 + e, y0 + e, x1 - e, y1 - e, r > e ? r - e : 0, fill);
}

static unsigned mix(unsigned a, unsigned b, int k, int n)
{
    unsigned out = 0;
    for (int s = 8; s < 32; s += 8) {
        int ca = (int)(a >> s) & 255, cb = (int)(b >> s) & 255;
        out |= (unsigned)(ca + (cb - ca) * k / n) << s;
    }
    return out;
}

/* a fill as the Wimp paints it: a gradient from top to bottom, or stripes */
static void fill(int x0, int y0, int x1, int y1, const unsigned f[2], int stripes, unsigned light)
{
    int h = y1 - y0;
    if (stripes == 2) {                 /* across: left to right */
        int w = x1 - x0, n = w / 4;
        for (int i = 0; i < n; i++)
            rect(x0 + i * w / n, y0, x0 + (i + 1) * w / n, y1, mix(f[0], f[1], i, n - 1 ? n - 1 : 1));
    } else if (stripes == 3) {          /* ridges: dark and light rows in a band */
        rect(x0, y0, x1, y1, f[0]);
        for (int y = y1 - h / 4 - 2, k = 0; y > y0 + h / 4; y -= 2, k++)
            rect(x0 + 4, y, x1 - 4, y + 2, (k & 1) ? light : f[1]);
    } else if (stripes) {
        rect(x0, y0, x1, y1, f[0]);
        for (int y = y1 - 4; y > y0; y -= 4)
            rect(x0, y, x1, y + 2, f[1]);
    } else if (f[0] == f[1] || h < 4) {
        rect(x0, y0, x1, y1, f[0]);
    } else {
        int n = h / 2;
        for (int i = 0; i < n; i++)
            rect(x0, y1 - (i + 1) * h / n, x1, y1 - i * h / n, mix(f[0], f[1], i, n - 1 ? n - 1 : 1));
    }
}

static int dark(unsigned c)
{
    int r = (int)(c >> 8) & 255, g2 = (int)(c >> 16) & 255, b = (int)(c >> 24) & 255;
    return r * 3 + g2 * 6 + b < 1280;
}

enum { LEFT, CENTRE, RIGHT };

static int text_width(int font, const char *s)
{
    if (font > 0) {
        int w = 0;
        if (!_swix(Font_ScanString, _INR(0, 4) | _OUT(3), font, s, 0x100, 0x7FFFFFFF, 0x7FFFFFFF, &w))
            return w / 400;
    }
    int w = 0;
    _swix(Wimp_TextOp, _INR(0, 2) | _OUT(0), 1, s, 0, &w);
    return w;
}

/* text with its baseline at y */
static void text(int font, const char *s, int x, int y, int align, unsigned fg, unsigned bg)
{
    if (align != LEFT) {
        int w = text_width(font, s);
        x -= align == CENTRE ? w / 2 : w;
    }
    if (font > 0) {
        _swix(ColourTrans_SetFontColours, _INR(0, 3), font, bg, fg, 14);
        _swix(Font_Paint, _INR(0, 4), font, s, 0x110, ox + x, oy + y);
    } else {
        _swix(Wimp_TextOp, _INR(0, 2), 0, fg, bg);
        _swix(Wimp_TextOp, _INR(0, 5), 2, s, -1, -1, ox + x, oy + y);
    }
}

static int find_font(const char *name, int points)
{
    int h = 0;
    if (_swix(Font_FindFont, _INR(1, 5) | _OUT(0), name, points * 16, points * 16, 0, 0, &h))
        return 0;
    return h;
}

static void fonts(void)
{
    g.font_head = find_font("Homerton.Bold", 18);
    g.font_bold = find_font("Homerton.Bold", 12);
    g.font_body = find_font("Homerton.Medium", 12);
    g.font_small = find_font("Homerton.Medium", 10);
}

static void lose_fonts(void)
{
    int *f[] = { &g.font_head, &g.font_bold, &g.font_body, &g.font_small };
    for (int i = 0; i < 4; i++) {
        if (*f[i] > 0)
            _swix(Font_LoseFont, _IN(0), *f[i]);
        *f[i] = 0;
    }
}

/* Plots one of a theme's icons with Wimp_PlotIcon, which scales and colours it. */
static void sprite(const int *area, const char *name, int x0, int y0, int x1, int y1)
{
    int b[8];
    b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1;
    b[4] = 0x1700011A;                  /* sprite, centred, indirected */
    b[5] = (int)(size_t)name, b[6] = (int)(size_t)area, b[7] = (int)strlen(name);
    _swix(Wimp_PlotIcon, _IN(1), b);
}

/* ---- the themes: what they are and how they look --------------------------------------------- */

static unsigned parse_colour(const char *v)
{
    while (*v == ' ')
        v++;
    if (*v == '&' || *v == '#')
        v++;
    unsigned rgb = (unsigned)strtoul(v, NULL, 16);
    return ((rgb & 0xFF) << 24) | (((rgb >> 8) & 0xFF) << 16) | (((rgb >> 16) & 0xFF) << 8);
}

/* Reads a fill as the Wimp does: "&c", "gradient &a &b", "stripes &a &b",
 * "across &a &b" or "ridges &bar &dark &light".  *kind is set to 0 for plain,
 * 1 for stripes, 2 for across and 3 for ridges. */
static void parse_fill(const char *v, unsigned f[2], int *kind, unsigned *light)
{
    static const char *const words[] = { "gradient", "stripes", "across", "ridges" };
    while (*v == ' ')
        v++;
    int k = -1;
    for (int i = 0; i < 4; i++)
        if (!strncmp(v, words[i], strlen(words[i])))
            k = i;
    if (k >= 0) {
        v += strlen(words[k]);
        unsigned c[3] = { 0, 0, 0 };
        for (int i = 0; i < (k == 3 ? 3 : 2); i++) {
            while (*v == ' ')
                v++;
            c[i] = parse_colour(v);
            while (*v && *v != ' ')
                v++;
        }
        f[0] = c[0], f[1] = c[1];
        if (light)
            *light = c[2];
    } else {
        f[0] = f[1] = parse_colour(v);
    }
    if (kind)
        *kind = k <= 0 ? 0 : k;
}

static void theme_defaults(struct theme *t)
{
    t->painted = 0;
    t->focus[0] = t->focus[1] = 0xBBFFFF00u;        /* Wimp colour 12, cream */
    t->unfocus[0] = t->unfocus[1] = 0xDDDDDD00u;
    t->focus_text = 0, t->unfocus_text = 0;
    t->focus_stripes = 0;
    t->track = 0xBBBBBB00u, t->thumb = 0xDDDDDD00u;
    t->outline = 0;
    t->menu_bg = C_WHITE;
    t->bar_bg = 0xDDDDDD00u;
    t->desk = 0x77777700u;              /* the desktop's own grey, Wimp colour 4 */
    t->bevel = 0, t->bevel_light = C_WHITE, t->bevel_dark = 0x55555500u;
    t->align_left = 0, t->buttons = 0, t->scroll = 0, t->title_h = 44;
    t->frame = t->resize_bar = t->buttons_right = t->hide_unfocused = 0;
    t->frame_face = 0xDDDDDD00u, t->button_face = 0xDDDDDD00u, t->button_colour = NO_COLOUR;
}

static void apply_key(struct theme *t, const char *sec, const char *key, const char *v, char *base, int basesize)
{
    if (same(sec, "Theme") && same(key, "Name"))
        snprintf(t->name, sizeof t->name, "%s", v);
    else if (same(sec, "Theme") && same(key, "Icons"))
        snprintf(t->icons_name, sizeof t->icons_name, "%s", v);
    else if (same(sec, "Theme") && same(key, "Vectors"))
        snprintf(t->vectors_name, sizeof t->vectors_name, "%s", v);
    else if (same(sec, "Theme") && same(key, "Base"))
        snprintf(base, (size_t)basesize, "%s", v);
    else if (same(sec, "Chrome") && same(key, "Style"))
        t->painted = same(v, "painted");
    else if (same(sec, "Chrome") && same(key, "TitleHeight"))
        t->title_h = atoi(v);
    else if (same(sec, "Chrome") && same(key, "Radius"))
        t->round = atoi(v) > 0;
    else if (same(sec, "Chrome") && same(key, "Shadow"))
        t->shadow = atoi(v) > 0;
    else if (same(sec, "Chrome") && same(key, "Bevel"))
        t->bevel = atoi(v);
    else if (same(sec, "Chrome") && same(key, "BevelLight"))
        t->bevel_light = parse_colour(v);
    else if (same(sec, "Chrome") && same(key, "BevelDark"))
        t->bevel_dark = parse_colour(v);
    else if (same(sec, "Title") && same(key, "Font"))
        t->font = !strncmp(v, "Homerton", 8) ? 1 : !strncmp(v, "Trinity", 7) ? 2 : 0;
    else if (same(sec, "Title") && same(key, "Align"))
        t->align_left = same(v, "left");
    else if (same(sec, "Title.Focus") && same(key, "Fill"))
        parse_fill(v, t->focus, &t->focus_stripes, &t->focus_light);
    else if (same(sec, "Title.Focus") && same(key, "Text"))
        t->focus_text = parse_colour(v);
    else if (same(sec, "Title.Unfocused") && same(key, "Fill"))
        parse_fill(v, t->unfocus, &t->unfocus_kind, &t->unfocus_light);
    else if (same(sec, "Title.Unfocused") && same(key, "Text"))
        t->unfocus_text = parse_colour(v);
    else if (same(sec, "Buttons") && same(key, "Set"))
        t->buttons = same(v, "dots") ? 1 : same(v, "boxes") ? 2 : same(v, "platinum") ? 3 : 0;
    else if (same(sec, "Buttons") && same(key, "Layout"))
        t->buttons_right = tolower((unsigned char)v[0]) == 'r';
    else if (same(sec, "Buttons") && same(key, "HideUnfocused"))
        t->hide_unfocused = tolower((unsigned char)v[0]) == 'y';
    else if (same(sec, "Buttons") && same(key, "Face"))
        t->button_face = parse_colour(v);
    else if (same(sec, "Buttons") && same(key, "Colour"))
        t->button_colour = parse_colour(v);
    else if (same(sec, "Window") && same(key, "ResizeBar"))
        t->resize_bar = atoi(v);
    else if (same(sec, "Window") && same(key, "Frame")) {
        t->frame = atoi(v);
        const char *c = strchr(v, '&');
        if (c)
            t->frame_face = parse_colour(c);
    } else if (same(sec, "Scroll") && same(key, "Style"))
        t->scroll = same(v, "slim") ? 1 : same(v, "channel") ? 2 : 0;
    else if (same(sec, "Scroll") && same(key, "Track"))
        t->track = parse_colour(v);
    else if (same(sec, "Scroll") && same(key, "Thumb"))
        t->thumb = parse_colour(v);
    else if (same(sec, "Window") && same(key, "Outline"))
        t->outline = parse_colour(v);
    else if (same(sec, "Menus") && same(key, "Background"))
        t->menu_bg = parse_colour(v);
    else if (same(sec, "Iconbar") && same(key, "Background"))
        t->bar_bg = parse_colour(v);
    else if (same(sec, "Desktop") && same(key, "Colour"))
        t->desk = parse_colour(v);
}

/* Reads a Theme file's settings into t.  *base is set to its Base, if it names one. */
static void parse_theme(struct theme *t, const char *dir, char *base, int basesize, int depth)
{
    char file[220];
    snprintf(file, sizeof file, "%s.Theme", dir);
    char *text = load_file(file, NULL);
    if (!text)
        return;
    char sec[40] = "", mybase[40] = "";
    for (char *p = text; *p;) {
        char *line = p;
        while (*p && *p != '\n' && *p != '\r')
            p++;
        if (*p)
            *p++ = 0;
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '|' || *line == '#' || !*line)
            continue;
        if (*line == '[') {
            char *e = strchr(line, ']');
            if (e) {
                *e = 0;
                snprintf(sec, sizeof sec, "%s", line + 1);
            }
            continue;
        }
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *v = colon + 1, *k = line, *e = colon;
        while (e > k && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); )
            *--e = 0;
        if (same(sec, "Theme") && same(k, "Base") && depth < 4) {
            /* the base's settings first, then this file's over them */
            snprintf(mybase, sizeof mybase, "%s", v);
            char bdir[200], dummy[40];
            snprintf(bdir, sizeof bdir, "Themes:%s", v);
            parse_theme(t, bdir, dummy, sizeof dummy, depth + 1);
            continue;
        }
        apply_key(t, sec, k, v, base, basesize);
    }
    free(text);
    if (mybase[0])
        snprintf(base, (size_t)basesize, "%s", mybase);
}

/* The few icons a card shows, copied out of a theme's Sprites */
static int *icons_from(const char *file)
{
    static const char *const want[] = { "directory", "harddisc", "application" };
    int size = 0;
    char *b = load_file(file, &size);
    if (!b || size < 12) {
        free(b);
        return NULL;
    }
    int count = ((int *)b)[0], off = ((int *)b)[1] - 4;
    int total = 16, n = 0;
    int at[3] = { -1, -1, -1 }, len[3] = { 0 };
    for (int i = 0; i < count && off > 0 && off + 16 <= size; i++) {
        int next = *(int *)(b + off);
        if (next < 44 || next > size - off)    /* (not off + next: that could overflow) */
            break;
        char nm[13];
        memcpy(nm, b + off + 4, 12);
        nm[12] = 0;
        for (int k = 0; k < 3; k++)
            if (at[k] < 0 && same(nm, want[k])) {
                at[k] = off, len[k] = next, total += next, n++;
            }
        off += next;
    }
    int *area = NULL;
    if (n) {
        area = malloc((size_t)total);
        if (area) {
            area[0] = total, area[1] = n, area[2] = 16, area[3] = total;
            char *o = (char *)area + 16;
            for (int k = 0; k < 3; k++)
                if (at[k] >= 0) {
                    memcpy(o, b + at[k], (size_t)len[k]);
                    o += len[k];
                }
        }
    }
    free(b);
    return area;
}

static int *theme_icons(const char *dir, const char *base, const char *icons)
{
    static const char *const files[] = { "SpritesA2", "Sprites22", "Sprites" };
    char f[240];
    for (int pass = 0; pass < 3; pass++) {
        const char *d = dir;
        char bdir[200];
        if (pass == 0 && icons[0]) {    /* [Theme] Icons: another theme's set, as the Wimp takes it */
            snprintf(bdir, sizeof bdir, "Themes:%s", icons);
            d = bdir;
        } else if (pass == 0) {
            continue;
        } else if (pass == 2) {
            if (!base[0])
                break;
            snprintf(bdir, sizeof bdir, "Themes:%s", base);
            d = bdir;
        }
        for (int i = 0; i < 3; i++) {
            snprintf(f, sizeof f, "%s.%s", d, files[i]);
            if (object_type(f) == 1)
                return icons_from(f);
        }
    }
    return NULL;
}

static int by_name(const void *a, const void *b)
{
    const struct theme *x = a, *y = b;
    if (!x->leaf[0] || !y->leaf[0])
        return x->leaf[0] ? 1 : y->leaf[0] ? -1 : 0;   /* Classic first */
    const char *p = x->name, *q = y->name;
    for (; *p && tolower((unsigned char)*p) == tolower((unsigned char)*q); p++, q++)
        ;
    return tolower((unsigned char)*p) - tolower((unsigned char)*q);
}

static int known(const char *leaf)
{
    for (int i = 0; i < nthemes; i++)
        if (same(themes[i].leaf, leaf))
            return 1;
    return 0;
}

static void add_theme(const char *dir, const char *leaf)
{
    if (nthemes >= MAX_THEMES || known(leaf))
        return;
    char f[240];
    int ok = 0;
    static const char *const any[] = { "Theme", "Tools", "Sprites", "Sprites22" };
    for (int i = 0; i < 4 && !ok; i++) {
        snprintf(f, sizeof f, "%s.%s", dir, any[i]);
        ok = object_type(f) == 1;
    }
    if (!ok)
        return;
    struct theme *t = &themes[nthemes++];
    memset(t, 0, sizeof *t);
    theme_defaults(t);
    snprintf(t->leaf, sizeof t->leaf, "%s", leaf);
    snprintf(t->name, sizeof t->name, "%s", leaf);
    snprintf(t->dir, sizeof t->dir, "%s", dir);
    char base[40] = "";
    parse_theme(t, dir, base, sizeof base, 0);
    t->icons = theme_icons(dir, base, t->icons_name);
    snprintf(f, sizeof f, "%s.SpritesV", dir);
    t->own_vectors = object_type(f) == 1;
}

static void scan_themes(void)
{
    for (int i = 0; i < nthemes; i++)
        free(themes[i].icons);
    nthemes = 0;
    struct theme *c = &themes[nthemes++];
    memset(c, 0, sizeof *c);
    theme_defaults(c);
    snprintf(c->name, sizeof c->name, "Classic");
    c->icons = icons_from("Resources:$.Resources.Wimp.Sprites22");
    if (!c->icons)
        c->icons = icons_from("Resources:$.Resources.Wimp.Sprites");

    char path[400];
    read_var("Themes$Path", path, sizeof path);
    for (char *el = path; *el;) {
        char *end = strchr(el, ',');
        if (end)
            *end = 0;
        char dir[200];
        snprintf(dir, sizeof dir, "%s", el);
        size_t n = strlen(dir);
        while (n && dir[n - 1] == '.')
            dir[--n] = 0;
        if (n && object_type(dir) == 2) {
            int offset = 0;
            char buf[1024];
            while (offset != -1) {
                int read = 0;
                if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 10, dir, buf, 32, offset, (int)sizeof buf, 0,
                          &read, &offset))
                    break;
                char *r = buf;
                for (int i = 0; i < read; i++) {
                    int type = ((int *)r)[4];
                    char *leaf = r + 20;
                    if (type == 2) {
                        char sub[240];
                        snprintf(sub, sizeof sub, "%s.%s", dir, leaf);
                        add_theme(sub, leaf);
                    }
                    r = (char *)(((size_t)(leaf + strlen(leaf) + 1) + 3) & ~(size_t)3);
                }
            }
        }
        if (!end)
            break;
        el = end + 1;
    }
    qsort(themes, (size_t)nthemes, sizeof themes[0], by_name);
}

static struct theme *current_theme(void)
{
    for (int i = 0; i < nthemes; i++)
        if (same(themes[i].leaf, g.now.theme))
            return &themes[i];
    return &themes[0];
}

/* Reads the Wimp's current theme from Wimp$IconTheme.  The value is "Morris4." for example,
 * and empty for Classic. */
static void read_current(void)
{
    char v[40];
    read_var("Wimp$IconTheme", v, sizeof v);
    size_t n = strlen(v);
    while (n && v[n - 1] == '.')
        v[--n] = 0;
    snprintf(g.now.theme, sizeof g.now.theme, "%s", v);
}

static int choices_name(char *buf, int size);

/* ---- the icon sets --------------------------------------------------------------------------- */

#define MAX_SETS     8
#define SAMPLES      5
#define SAMPLE_PX    128                /* a sample's size on the screen, in OS units */

/* The icons shown for each set.  Edit, Paint, Draw, the discs, folders and
 * standard file types are drawn the same in every set, so none of those is here. */
static const struct { const char *name, *caption; } samples[SAMPLES] = {
    { "romapps", "Apps" }, { "discs", "Discs" }, { "file_f7a", "BASIC" }, { "file_fb1", "Sound" },
    { "file_c85", "Picture" },
};

static struct iconset {
    char leaf[32];                      /* what Theme.Vectors takes */
    char file[240];                     /* its SpritesV */
    int *area;                          /* the sample icons drawn from it, or NULL */
} sets[MAX_SETS];
static int nsets;

static const struct { const char *leaf, *label, *line1, *line2; } set_text[] = {
    { "Icons", "BOX", "The BOX set. Pages seen in", "perspective, drawn from open art." },
    { "Win2K", "Win2K", "Flat pages on a label band,", "with yellow folders." },
    { "Pinstripe", "Pinstripe", "Win2K's pages with pale lilac", "folders and cool grey hardware." },
    { "Cube", "Cube", "The BOX art on a dark granite", "plinth, the name on a light bar." },
};

static const char *set_label(const struct iconset *c, const char **l1, const char **l2)
{
    for (unsigned i = 0; i < sizeof set_text / sizeof set_text[0]; i++)
        if (same(c->leaf, set_text[i].leaf)) {
            *l1 = set_text[i].line1, *l2 = set_text[i].line2;
            return set_text[i].label;
        }
    *l1 = "An icon set found on", *l2 = "Themes$Path.";
    return c->leaf;
}

static int set_order(const void *a, const void *b)
{
    const struct iconset *x = a, *y = b;
    if (same(x->leaf, "Icons") || same(y->leaf, "Icons"))
        return same(y->leaf, "Icons") - same(x->leaf, "Icons");
    const char *p = x->leaf, *q = y->leaf;
    for (; *p && tolower((unsigned char)*p) == tolower((unsigned char)*q); p++, q++)
        ;
    return tolower((unsigned char)*p) - tolower((unsigned char)*q);
}

static int set_known(const char *leaf)
{
    for (int i = 0; i < nsets; i++)
        if (same(sets[i].leaf, leaf))
            return 1;
    return 0;
}

/* Draws the sample icons of each set into a sprite area, with *VIconSet, which uses the
 * same drawing as the Wimp.  The file is a scrap file, taken away again. */
static void make_samples(void)
{
    char scrap[220], dir[200];
    if (!read_var("Wimp$Scrap", scrap, sizeof scrap)) {
        if (!read_var("Choices$Write", dir, sizeof dir))
            return;
        snprintf(scrap, sizeof scrap, "%s.IconScrap", dir);
    }
    int ex = 1;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &ex);
    int px = SAMPLE_PX >> (ex > 3 ? 3 : ex);
    for (int i = 0; i < nsets; i++) {
        free(sets[i].area);
        sets[i].area = NULL;
        char names[200] = "";
        for (int k = 0; k < SAMPLES; k++)
            snprintf(names + strlen(names), sizeof names - strlen(names), " %s", samples[k].name);
        if (cli("VIconSet %s %d %s%s", sets[i].file, px, scrap, names))
            continue;
        int len = 0;
        char *b = load_file(scrap, &len);
        _swix(OS_File, _INR(0, 1), 6, scrap);
        if (!b || len < 12) {
            free(b);
            continue;
        }
        int *area = malloc((size_t)len + 4);
        if (area) {
            area[0] = len + 4;
            memcpy(area + 1, b, (size_t)len);
            sets[i].area = area;
        }
        free(b);
    }
}

/* Lists the folders of Themes$Path that hold a SpritesV, the icon sets */
static void scan_sets(void)
{
    for (int i = 0; i < nsets; i++)
        free(sets[i].area);
    nsets = 0;
    char path[400];
    read_var("Themes$Path", path, sizeof path);
    for (char *el = path; *el;) {
        char *end = strchr(el, ',');
        if (end)
            *end = 0;
        char dir[200];
        snprintf(dir, sizeof dir, "%s", el);
        size_t n = strlen(dir);
        while (n && dir[n - 1] == '.')
            dir[--n] = 0;
        if (n && object_type(dir) == 2) {
            int offset = 0;
            char buf[1024];
            while (offset != -1) {
                int read = 0;
                if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 10, dir, buf, 32, offset, (int)sizeof buf, 0,
                          &read, &offset))
                    break;
                char *r = buf;
                for (int i = 0; i < read; i++) {
                    int type = ((int *)r)[4];
                    char *leaf = r + 20;
                    char f[240];
                    snprintf(f, sizeof f, "%s.%s.SpritesV", dir, leaf);
                    if (type == 2 && nsets < MAX_SETS && !set_known(leaf) && object_type(f) == 1) {
                        struct iconset *c = &sets[nsets++];
                        memset(c, 0, sizeof *c);
                        snprintf(c->leaf, sizeof c->leaf, "%s", leaf);
                        snprintf(c->file, sizeof c->file, "%s", f);
                    }
                    r = (char *)(((size_t)(leaf + strlen(leaf) + 1) + 3) & ~(size_t)3);
                }
            }
        }
        if (!end)
            break;
        el = end + 1;
    }
    qsort(sets, (size_t)nsets, sizeof sets[0], set_order);
    make_samples();
}

/* The set the current theme draws with when none is chosen by hand: its own SpritesV, else
 * the one it names, else none (it draws the bitmap icons) */
static const char *native_set(const struct theme *t)
{
    if (t->own_vectors)
        return t->leaf;
    if (t->vectors_name[0])
        return t->vectors_name;
    return t->icons_name;
}

static const char *current_set(void)
{
    return g.now.vectors[0] ? g.now.vectors : native_set(current_theme());
}

/* Finds the tagged tile that Composite uses, on the disc with the Pinboard's pictures.
 * Returns 0 if it is missing. */
static int tile_file(char *buf, int size)
{
    char v[200];
    if (!read_var("BoxDisc$Dir", v, sizeof v))
        return 0;
    snprintf(buf, (size_t)size, "%s.Resources.Pinboard.BackTile", v);
    return object_type(buf) == 1;
}

/* ---- the Choices file ------------------------------------------------------------------------ */

static int choices_name(char *buf, int size)
{
    char v[200];
    if (!read_var("Choices$Write", v, sizeof v))
        return 0;
    snprintf(buf, (size_t)size, "%s.Appearance", v);
    return 1;
}

static void choice_clear(struct choice *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < NSETTINGS; i++)
        c->set[i] = -1;
    c->desk = NO_COLOUR;
}

/* whether a is v's first word ("Homerton.Bold" of "Homerton.Bold 12") */
static int first_word(const char *a, const char *v)
{
    char w[40];
    unsigned n = 0;
    while (v[n] && v[n] != ' ' && n < sizeof w - 1)
        w[n] = v[n], n++;
    w[n] = 0;
    return same(a, w);
}

/* Reads the -Colour switch and the fit switch (-Scale, -Tile or -Centre) with its file
 * from a *Backdrop line. */
static void parse_backdrop(const char *line)
{
    const char *c = strstr(line, "-Colour &");
    if (c)
        g.now.desk = (unsigned)strtoul(c + 9, NULL, 16);
    for (int k = 0; k < 3; k++) {
        const char *s = strstr(line, fit_switch[k]);
        if (s) {
            g.now.fit = k;
            s += strlen(fit_switch[k]);
            while (*s == ' ')
                s++;
            snprintf(g.now.picture, sizeof g.now.picture, "%s", s);
            char *e = g.now.picture + strlen(g.now.picture);
            while (e > g.now.picture && (e[-1] == ' ' || e[-1] == '\r'))
                *--e = 0;
        }
    }
}

static void load_choices(void)
{
    char name[220];
    char *text = choices_name(name, sizeof name) ? load_file(name, NULL) : NULL;
    if (!text)
        return;
    for (char *p = text; *p;) {
        char *line = p;
        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        char a[40] = "", b[40] = "";
        if (sscanf(line, "WimpThemeSet %39s %39s", a, b) == 2) {
            if (same(a, "Theme.Vectors"))
                snprintf(g.now.vectors, sizeof g.now.vectors, "%s", b);
            for (int i = 0; i < NSETTINGS; i++)
                if (same(a, settings[i].key))
                    for (int k = 0; k < settings[i].n; k++)
                        if (first_word(b, settings[i].values[k]))
                            g.now.set[i] = k;
        } else if (!strncmp(line, "| Composite on", 14)) {
            g.now.composite = 1;        /* the Backdrop line below is the tagged tile: not a wallpaper */
        } else if (!strncmp(line, "| Before ", 9)) {
            parse_backdrop(line + 9);   /* what Composite's Off puts back */
        } else if (!strncmp(line, "Backdrop ", 9) && !g.now.composite) {
            parse_backdrop(line + 9);
        }
    }
    free(text);
}

static void save_choices(void)
{
    char name[220];
    if (!choices_name(name, sizeof name))
        return;
    FILE *f = fopen(name, "w");
    if (!f)
        return;
    fprintf(f, "| The desktop's look, as !Appearance last left it; !Appearance's !Boot\n"
               "| runs this as the desktop starts.\n");
    fprintf(f, "WimpTheme %s\n", g.now.theme[0] ? g.now.theme : "Classic");
    for (int i = 0; i < NSETTINGS; i++)
        if (g.now.set[i] >= 0)
            fprintf(f, "WimpThemeSet %s %s\n", settings[i].key, settings[i].values[g.now.set[i]]);
    if (g.now.vectors[0])
        fprintf(f, "WimpThemeSet Theme.Vectors %s\n", g.now.vectors);
    char tile[260];
    if (g.now.composite && tile_file(tile, sizeof tile)) {
        /* The comments are what Off puts back.
         * The Backdrop line is what the desktop starts with. */
        fprintf(f, "| Composite on\n");
        if (g.now.desk != NO_COLOUR || g.now.picture[0]) {
            fprintf(f, "| Before");
            if (g.now.desk != NO_COLOUR)
                fprintf(f, " -Colour &%08X", g.now.desk);
            if (g.now.picture[0])
                fprintf(f, " %s %s", fit_switch[g.now.fit], g.now.picture);
            fprintf(f, "\n");
        }
        fprintf(f, "Backdrop -Tile %s\n", tile);
    } else if (g.now.desk != NO_COLOUR || g.now.picture[0]) {
        fprintf(f, "Backdrop");
        if (g.now.desk != NO_COLOUR)
            fprintf(f, " -Colour &%08X", g.now.desk);
        if (g.now.picture[0])
            fprintf(f, " %s %s", fit_switch[g.now.fit], g.now.picture);
        fprintf(f, "\n");
    }
    fclose(f);
    _swix(OS_File, _INR(0, 2), 18, name, 0xFEB);
}

/* ---- the window ------------------------------------------------------------------------------ */

static int wall_rows(void)
{
    int rows = (nwalls + WALL_COLS - 1) / WALL_COLS;
    return rows < 1 ? 1 : rows;
}

/* where the Composite switch's block begins, below the thumbnails */
static int composite_top(void)
{
    return WALL_TOP - wall_rows() * WALL_ROW_H - 8;
}

static int content_height(void)
{
    int rows = (nthemes + COLUMNS - 1) / COLUMNS;
    int h = -HEAD_Y + rows * (CARD_H + CARD_GAP) + FOOTER_H + 24;
    int w = -(composite_top() - COMPOSITE_H) + FOOTER_H + 16;      /* the Wallpaper page */
    if (w > h)
        h = w;
    w = -HEAD_Y + (nsets > 4 ? nsets : 4) * SET_PITCH + FOOTER_H + 40;      /* the Icons page */
    if (w > h)
        h = w;
    return h < 1040 ? 1040 : h;
}

static void redraw_all(void)
{
    if (g.window)
        _swix(Wimp_ForceRedraw, _INR(0, 4), g.window, 0, -0x10000, 0x10000, 0);
}

static int screen_h(void)
{
    int sh = 0, ey = 1;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &sh);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &ey);
    return (sh + 1) << ey;
}

static os_error *create_window(void)
{
    int def[23];
    memset(def, 0, sizeof def);
    g.win_h = content_height();
    def[0] = 200, def[1] = 200, def[2] = 200 + WIN_W, def[3] = 200 + g.win_h;
    def[6] = -1;
    def[7] = (int)0x87000002u;          /* moveable, back, close, title, new format */
    if (g.win_h > screen_h() - 240)
        def[7] |= 1 << 28;              /* and a scroll bar if it is taller than the screen */
    unsigned char *c = (unsigned char *)&def[8];
    c[0] = 7, c[1] = 2, c[2] = 7, c[3] = 0xFF, c[4] = 3, c[5] = 1, c[6] = 12, c[7] = 0;
    def[10] = 0, def[11] = -g.win_h, def[12] = WIN_W, def[13] = 0;
    def[14] = 0x3D;                     /* title: text, border, centred, filled */
    def[15] = 3 << 12;                  /* the work area: click */
    def[16] = 1;
    def[17] = 0;
    strncpy((char *)&def[18], "Appearance", 12);
    def[21] = 0;
    return _swix(Wimp_CreateWindow, _IN(1) | _OUT(0), def, &g.window);
}

static void open_window(void)
{
    int sw = 0, sh = 0, ex = 1, ey = 1;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &sw);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &sh);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &ex);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &ey);
    sw = (sw + 1) << ex, sh = (sh + 1) << ey;
    int b[10];
    b[0] = g.window;
    if (g.open) {
        _swix(Wimp_GetWindowState, _IN(1), b);
    } else {
        int h = g.win_h;
        if (h > sh - 240)
            h = sh - 240;
        b[1] = (sw - WIN_W) / 2;
        b[3] = b[1] + WIN_W;
        b[4] = sh - (sh - h) / 2 + 40;
        b[2] = b[4] - h;
        b[5] = 0, b[6] = 0;
    }
    b[7] = -1;
    _swix(Wimp_OpenWindow, _IN(1), b);
    g.open = 1;
}

/* ---- layout: where things are on each page, for drawing and for clicks ----------------------- */

struct box { int x0, y0, x1, y1; };

static struct box sidebar_item(int i)
{
    struct box b = { 24, -152 - (i + 1) * 64, SIDEBAR_W - 24, -152 - i * 64 };
    b.y1 -= 8;
    return b;
}

static struct box card(int i)
{
    int col = i % COLUMNS, row = i / COLUMNS;
    struct box b;
    b.x0 = PANE_X + col * (CARD_W + CARD_GAP);
    b.x1 = b.x0 + CARD_W;
    b.y1 = HEAD_Y - row * (CARD_H + CARD_GAP);
    b.y0 = b.y1 - CARD_H;
    return b;
}

#define SEG_X   (PANE_X + 300)
#define SEG_W   180

static struct box segment(int row, int k, int n)
{
    struct box b;
    b.y1 = HEAD_Y - 24 - row * 80;
    b.y0 = b.y1 - 60;
    b.x0 = SEG_X + k * SEG_W;
    b.x1 = b.x0 + SEG_W;
    (void)n;
    return b;
}

static struct box swatch(int i)
{
    struct box b;
    b.x0 = PANE_X + i * 88;
    b.x1 = b.x0 + 64;
    b.y1 = HEAD_Y - 64;
    b.y0 = b.y1 - 64;
    return b;
}

static struct box well(void)
{
    struct box b = { PANE_X, HEAD_Y - 520, PANE_X + 720, HEAD_Y - 224 };
    return b;
}

static struct box fit_button(int k)
{
    struct box b;
    b.x0 = PANE_X + 760 + k * 150;
    b.x1 = b.x0 + 150;
    b.y1 = HEAD_Y - 264;
    b.y0 = b.y1 - 60;
    return b;
}

static struct box wall_box(int i)
{
    struct box b;
    b.x0 = PANE_X + (i % WALL_COLS) * WALL_PITCH;
    b.x1 = b.x0 + WALL_SIZE;
    b.y1 = WALL_TOP - (i / WALL_COLS) * WALL_ROW_H;
    b.y0 = b.y1 - WALL_SIZE;
    return b;
}

/* the Composite switch, and the label beside it, which also takes clicks */
static struct box composite_switch(void)
{
    int top = composite_top();
    struct box b = { PANE_X, top - 52, PANE_X + 96, top - 4 };
    return b;
}

static struct box composite_hit(void)
{
    struct box b = composite_switch();
    b.x1 = PANE_X + 400, b.y0 -= 4, b.y1 += 4;
    return b;
}

static struct box set_row(int i)
{
    struct box b = { PANE_X, HEAD_Y - i * SET_PITCH - SET_H, WIN_W - 40, HEAD_Y - i * SET_PITCH };
    return b;
}

static struct box remove_button(void)
{
    struct box b = { PANE_X + 760, HEAD_Y - 420, PANE_X + 1060, HEAD_Y - 360 };
    return b;
}

static struct box revert_button(void)
{
    struct box b = { WIN_W - 40 - 200, 0, WIN_W - 40, 0 };
    b.y0 = -g.win_h + 22, b.y1 = b.y0 + 56;
    return b;
}

static int inside(struct box b, int x, int y)
{
    return x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1;
}

/* ---- the pages ------------------------------------------------------------------------------- */

static void button(struct box b, const char *label, int on, int enabled)
{
    unsigned face = on ? C_ACCENT : C_WHITE, ink = on ? C_WHITE : enabled ? C_TEXT : C_DIM;
    rbox(b.x0, b.y0, b.x1, b.y1, 12, 2, face, on ? C_ACCENT : C_CARDEDGE);
    text(g.font_body, label, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2 - 10, CENTRE, ink, face);
}

/* a title bar button in miniature, in the theme's set */
static void mini_button(const struct theme *t, int cx, int cy, unsigned ink, int k)
{
    static const unsigned dots[] = { 0xEFAF6100u, 0x756CE000u, 0x7BC0E500u, 0x79C39800u };
    switch (t->buttons) {
    case 1:                             /* dots */
        disc(cx, cy, 4, dots[k & 3]);
        break;
    case 2:                             /* boxes: a raised face */
        rect(cx - 6, cy - 6, cx + 6, cy + 6, t->bevel_dark);
        rect(cx - 6, cy - 4, cx + 4, cy + 6, t->bevel_light);
        rect(cx - 4, cy - 4, cx + 4, cy + 4, t->button_face);
        rect(cx - 2, cy - 2, cx + 2, cy + 2, t->button_colour != NO_COLOUR ? t->button_colour : ink);
        break;
    case 3:                             /* platinum: an outlined square */
        rect(cx - 6, cy - 6, cx + 6, cy + 6, t->outline);
        rect(cx - 4, cy - 4, cx + 4, cy + 4, t->button_face);
        break;
    default:                            /* glyphs */
        rect(cx - 3, cy - 1, cx + 3, cy + 1, ink);
        break;
    }
}

/* a window in miniature: title bar, work area, scroll bar */
static void mini_window(const struct theme *t, int x0, int y0, int x1, int y1, int focus, unsigned work)
{
    int th = 20, sb = t->scroll == 1 ? 8 : 12;
    if (t->painted && t->resize_bar) {  /* NeXTSTEP's resize bar, below: three raised parts */
        int r0 = y0 - 12, r1 = y0 - 2;
        rect(x0 - 2, r0, x1 + 2, r1, t->outline);
        rect(x0, r0 + 2, x1, r1, t->frame_face);
        rect(x0, r1 - 2, x1, r1, t->bevel_light);
        rect(x0 + 14, r0 + 2, x0 + 16, r1, t->outline);
        rect(x1 - 16, r0 + 2, x1 - 14, r1, t->outline);
        y0 = r1 + 2;
    }
    if (t->painted && t->frame) {       /* Window.Frame: a raised band round it all */
        int f = 4;
        rect(x0 - 2 - f - 2, y0 - 2 - f - 2, x1 + 2 + f + 2, y1 + 2 + f + 2, t->outline);
        rect(x0 - 2 - f, y0 - 2 - f, x1 + 2 + f, y1 + 2 + f, t->frame_face);
        rect(x0 - 2 - f, y1 + 2 + f - 2, x1 + 2 + f, y1 + 2 + f, t->bevel_light);
        rect(x0 - 2 - f, y0 - 2 - f, x0 - f, y1 + 2 + f, t->bevel_light);
        rect(x0 - 2 - f, y0 - 2 - f, x1 + 2 + f, y0 - f, t->bevel_dark);
    }
    rect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, t->outline);
    const unsigned *f = focus ? t->focus : t->unfocus;
    int kind = focus ? t->focus_stripes : t->unfocus_kind;
    fill(x0, y1 - th, x1, y1, f, kind, focus ? t->focus_light : t->unfocus_light);
    if (t->bevel) {
        rect(x0, y1 - 2, x1, y1, t->bevel_light);
        rect(x0, y1 - th, x1, y1 - th + 2, t->bevel_dark);
    }
    unsigned ink = focus ? t->focus_text : t->unfocus_text;
    if (t->painted && !(t->hide_unfocused && !focus)) {
        /* the buttons where the theme puts them: one at each end of the bar,
         * or three together at the right */
        int cy = y1 - th / 2, k = 0;
        if (t->buttons_right) {
            for (int i = 0; i < 3; i++)
                mini_button(t, x1 - 10 - i * 16 - (i ? 4 : 0), cy, ink, k++);
        } else {
            mini_button(t, x0 + 10, cy, ink, k++);
            mini_button(t, x1 - 10, cy, ink, k++);
        }
    }
    int tw = (x1 - x0) / 3;
    int tx = t->align_left ? x0 + 24 : (x0 + x1 - tw) / 2;
    if (kind != 1 && kind != 3) {
        rect(tx, y1 - th / 2 - 2, tx + tw, y1 - th / 2 + 2, ink);
    } else {
        rect(tx - 6, y1 - th + 2, tx + tw + 6, y1 - 2, f[0]);
        rect(tx, y1 - th / 2 - 2, tx + tw, y1 - th / 2 + 2, ink);
    }
    rect(x0, y0, x1 - sb, y1 - th, work);
    rect(x1 - sb, y0, x1, y1 - th, t->track);
    int h = y1 - th - y0;
    rect(x1 - sb + (t->scroll == 2 ? 3 : 1), y0 + h / 2, x1 - (t->scroll == 2 ? 3 : 1), y1 - th - 6, t->thumb);
    if (t->bevel)
        rect(x1 - sb, y0, x1 - sb + 2, y1 - th, t->bevel_light);
    if (work != C_WHITE || !t->painted) {
        unsigned line = mix(work, 0, 1, 4);
        for (int k = 0; k < 3 && y1 - th - 14 - k * 14 > y0 + 6; k++)
            rect(x0 + 10, y1 - th - 14 - k * 14, x1 - sb - 18 - (k == 2) * 30, y1 - th - 10 - k * 14, line);
    }
}

static void draw_card(int i)
{
    const struct theme *t = &themes[i];
    struct box b = card(i);
    int cur = t == current_theme();
    rrect(b.x0, b.y0 - 4, b.x1, b.y1 - 4, 18, C_SHADOW);
    rbox(b.x0, b.y0, b.x1, b.y1, 18, cur ? 4 : 2, C_CARD, cur ? C_ACCENT : C_CARDEDGE);

    /* the picture: a desktop with two windows and the icon bar */
    int px0 = b.x0 + 12, px1 = b.x1 - 12, py1 = b.y1 - 12, py0 = py1 - 148;
    rrect(px0, py0, px1, py1, 10, g.now.desk != NO_COLOUR ? g.now.desk : t->desk);
    rect(px0, py0, px1, py0 + 22, t->bar_bg);
    for (int k = 0; k < 3; k++)
        rect(px0 + 14 + k * 26, py0 + 6, px0 + 30 + k * 26, py0 + 16, mix(t->bar_bg, 0, 1, 3));
    unsigned work = t->painted ? t->menu_bg : C_WHITE;
    mini_window(t, px0 + 96, py0 + 62, px1 - 34, py1 - 10, 0, work);
    if (t->shadow) {                    /* a soft shadow under the front window */
        unsigned desk = g.now.desk != NO_COLOUR ? g.now.desk : t->desk;
        for (int k = 3; k >= 1; k--)
            rect(px0 + 74 + 2 * k, py0 + 34 - 4 - 2 * k, px1 - 14 + 2 * k, py1 - 42 - 4, mix(desk, 0, 4 - k, 10));
    }
    mini_window(t, px0 + 74, py0 + 34, px1 - 14, py1 - 42, 1, work);
    const int *icons = t->icons ? t->icons : themes[0].icons;
    if (icons)
        sprite(icons, "directory", px0 + 6, py1 - 78, px0 + 74, py1 - 10);

    text(g.font_bold, t->name, b.x0 + 18, b.y0 + 44, LEFT, C_TEXT, C_CARD);
    text(g.font_small, t->leaf[0] ? (t->painted ? "Painted" : "Sprites") : "The ROM's own",
         b.x0 + 18, b.y0 + 14, LEFT, C_SUBTEXT, C_CARD);
    if (cur) {
        disc(b.x1 - 34, b.y0 + 34, 16, C_ACCENT);
        /* a tick */
        gcol(C_WHITE);
        int cx = ox + b.x1 - 34, cy = oy + b.y0 + 34;
        for (int k = -1; k <= 1; k++) {
            plot(4, cx - 9, cy + 1 + k);
            plot(5, cx - 3, cy - 6 + k);
            plot(5, cx + 9, cy + 7 + k);
        }
    }
}

static void draw_themes(void)
{
    for (int i = 0; i < nthemes; i++)
        draw_card(i);
}

static int setting_value(int i)
{
    if (g.now.set[i] >= 0)
        return g.now.set[i];
    const struct theme *t = current_theme();
    switch (i) {
    case SET_ALIGN:
        return t->align_left ? 0 : 1;
    case SET_BUTTONS:
        return t->buttons;
    case SET_SCROLL:
        return t->scroll;
    case SET_BEVEL:
        return t->bevel ? 1 : 0;
    case SET_TITLEH:
        return t->title_h <= 38 ? 0 : t->title_h >= 50 ? 2 : 1;
    case SET_FONT:
        return t->font;
    case SET_CORNERS:
        return t->round;
    case SET_SHADOW:
        return t->shadow;
    }
    return 0;
}

static void draw_windows(void)
{
    const struct theme *t = current_theme();
    int enabled = t->painted;
    for (int i = 0; i < NSETTINGS; i++) {
        const struct setting *s = &settings[i];
        struct box first = segment(i, 0, s->n);
        text(g.font_bold, s->label, PANE_X, first.y0 + 20, LEFT, enabled ? C_TEXT : C_DIM, C_PANE);
        struct box all = { first.x0, first.y0, first.x0 + s->n * SEG_W, first.y1 };
        rbox(all.x0, all.y0, all.x1, all.y1, 14, 2, C_SEGBG, C_SEGBG);
        int v = setting_value(i);
        for (int k = 0; k < s->n; k++) {
            struct box b = segment(i, k, s->n);
            if (k == v) {
                rbox(b.x0 + 4, b.y0 + 4, b.x1 - 4, b.y1 - 4, 10, 2, enabled ? C_WHITE : C_SEGBG,
                     enabled ? C_CARDEDGE : C_SEGBG);
            } else if (k) {
                rect(b.x0 - 1, b.y0 + 16, b.x0 + 1, b.y1 - 16, C_CARDEDGE);
            }
            text(k == v ? g.font_bold : g.font_body, s->names[k], (b.x0 + b.x1) / 2, b.y0 + 20, CENTRE,
                 enabled ? (k == v ? C_TEXT : C_SUBTEXT) : C_DIM, k == v && enabled ? C_WHITE : C_SEGBG);
        }
    }
    struct box last = segment(NSETTINGS - 1, 0, 1);
    char line[120];
    if (enabled)
        snprintf(line, sizeof line, "These change %s for now; choosing another theme starts again from its own.",
                 t->name);
    else
        snprintf(line, sizeof line, "%s draws its windows from sprites. Choose a painted theme to change these.",
                 t->name);
    text(g.font_body, line, PANE_X, last.y0 - 70, LEFT, C_SUBTEXT, C_PANE);
}

static const char *leafname(const char *path)
{
    const char *l = path;
    for (const char *p = path; *p; p++)
        if (*p == '.' || *p == ':')
            l = p + 1;
    return l;
}

/* ---- the pictures: the Wallpaper page's picture and the tiles' thumbnails -------------------- */

static void img_forget(struct img *im)
{
    free(im->data);
    free(im->table);
    memset(im, 0, sizeof *im);
}

/* the screen's bits per pixel */
static int screen_bpp(void)
{
    int l = 3;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 9, &l);
    return 1 << l;
}

/* the colour translation from a sprite's palette to the screen's, made again when the mode changes */
static void img_table(struct img *im)
{
    free(im->table);
    im->table = NULL;
    if (!im->data || im->type != 0xFF9)
        return;
    int *area = (int *)im->data;
    char *sp = im->data + area[2];
    int mode = 0, sbpp = 0, bpp = 0;
    _swix(OS_SpriteOp, _INR(0, 2) | _OUT(6), 512 + 40, area, sp, &mode);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), mode, 9, &sbpp);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 9, &bpp);
    if (sbpp <= 3 && bpp >= 4 && *(int *)(sp + 32) >= 44 + 8 * (1 << (1 << sbpp))) {
        /* a palette sprite on a 16 or 32 bpp screen: a wide table, the screen's colour for each
         * palette entry, as the sprite's own palette gives it (plot action bit 5) */
        int n = 1 << (1 << sbpp);
        const unsigned *pal = (const unsigned *)(sp + 44);
        im->table = malloc((size_t)n * 4);
        for (int i = 0; im->table && i < n; i++) {
            unsigned c = 0;
            if (_swix(ColourTrans_ReturnColourNumber, _IN(0) | _OUT(0), pal[2 * i], &c)) {
                free(im->table);
                im->table = NULL;
            } else if (bpp == 4) {
                ((unsigned short *)im->table)[i] = (unsigned short)c;
            } else {
                ((unsigned *)im->table)[i] = c;
            }
        }
    } else if (bpp <= 3) {              /* a palette on the screen: a table of bytes */
        int size = 0;
        if (!_swix(ColourTrans_SelectTable, _INR(0, 5) | _OUT(4), area, sp, -1, -1, 0, 1, &size) && size > 0 &&
            size <= 65536 && (im->table = malloc((size_t)size)) != NULL)
            _swix(ColourTrans_SelectTable, _INR(0, 5), area, sp, -1, -1, im->table, 1);
    }
}

/* a sprite file, or a JPEG when jpeg is set, up to limit bytes */
static void img_load(struct img *im, const char *name, int jpeg, int limit)
{
    img_forget(im);
    int obj = 0, load = 0, len = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(2) | _OUT(4), 17, name, &obj, &load, &len) || obj != 1 ||
        len < 16 || len > limit || ((unsigned)load >> 20) != 0xFFFu)
        return;
    int type = (load >> 8) & 0xFFF;
    if (type != 0xFF9 && !(jpeg && type == 0xC85))
        return;
    char *b = malloc((size_t)len + 4);
    if (!b)
        return;
    if (_swix(OS_File, _INR(0, 3), 16, name, b + (type == 0xFF9 ? 4 : 0), 0)) {
        free(b);
        return;
    }
    im->data = b, im->type = type, im->size = len;
    if (type == 0xFF9) {
        int *area = (int *)b;
        area[0] = len + 4;
        /* check that the first sprite's header and size lie within the file
         * before the OS reads them */
        if (area[1] < 1 || area[2] < 16 || area[2] > len + 4 - 44 || *(int *)(b + area[2]) < 44 ||
            *(int *)(b + area[2]) > len + 4 - area[2]) {
            img_forget(im);
            return;
        }
        char *sp = b + area[2];
        int w = 0, h = 0, mode = 0, ex = 1, ey = 1;
        if (_swix(OS_SpriteOp, _INR(0, 2) | _OUT(3) | _OUT(4) | _OUT(6), 512 + 40, area, sp, &w, &h, &mode)) {
            img_forget(im);
            return;
        }
        _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), mode, 4, &ex);
        _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), mode, 5, &ey);
        im->w = w << ex, im->h = h << ey;
        img_table(im);
    } else {
        int w = 0, h = 0, xd = 90, yd = 90;
        if (_swix(JPEG_Info, _INR(0, 2) | _OUTR(2, 5), 1, b, len, &w, &h, &xd, &yd) || w <= 0 || h <= 0) {
            img_forget(im);
            return;
        }
        im->w = w * 2, im->h = h * 2;   /* a pixel to 2 OS units, as the desktop's */
    }
}

static void forget_picture(void)
{
    img_forget(&pic);
}

static void load_picture(const char *name)
{
    img_load(&pic, name, 1, 6 * 1024 * 1024);
}

/* an image fitted into a box, kept to its shape */
static void draw_img(const struct img *im, struct box b)
{
    if (!im->data || im->w <= 0 || im->h <= 0)
        return;
    int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    int w = bw, h = im->h * bw / im->w;
    if (h > bh)
        h = bh, w = im->w * bh / im->h;
    int x = ox + b.x0 + (bw - w) / 2, y = oy + b.y0 + (bh - h) / 2;
    int scale[4] = { w, h, im->w, im->h };
    if (im->type == 0xFF9) {
        int *area = (int *)im->data;
        /* the table's entries are words on a 16 or 32 bpp screen: plot action bit 5 */
        int action = 8 | (im->table && screen_bpp() >= 16 ? 32 : 0);
        _swix(OS_SpriteOp, _INR(0, 7), 512 + 52, area, im->data + area[2], x, y, action, scale, im->table);
    } else {
        _swix(JPEG_PlotScaled, _INR(0, 5), im->data, x, y, scale, im->size, 1);
    }
}

static void draw_picture(struct box b)
{
    draw_img(&pic, b);
}

/* ---- the tiles ------------------------------------------------------------------------------- */

static int wall_known(const char *name)
{
    for (int i = 0; i < nwalls; i++)
        if (same(walls[i].name, name))
            return 1;
    return 0;
}

static int wall_by_name(const void *a, const void *b)
{
    const struct wall *x = a, *y = b;
    const char *p = x->name, *q = y->name;
    for (; *p && tolower((unsigned char)*p) == tolower((unsigned char)*q); p++, q++)
        ;
    return tolower((unsigned char)*p) - tolower((unsigned char)*q);
}

/* Lists the sprite files in each folder of Wallpapers$Path, the user's first.  A name in an
 * earlier folder hides the same name in a later one.  If the variable is not set, it uses
 * the disc's folder. */
static void scan_walls(void)
{
    for (int i = 0; i < nwalls; i++)
        img_forget(&walls[i].im);
    nwalls = 0;
    char path[600], disc[200];
    read_var("Wallpapers$Path", path, sizeof path);
    if (!path[0] && read_var("BoxDisc$Dir", disc, sizeof disc))
        snprintf(path, sizeof path, "%s.Resources.Wallpapers.", disc);
    for (char *el = path; *el;) {
        char *end = strchr(el, ',');
        if (end)
            *end = 0;
        char dir[200];
        snprintf(dir, sizeof dir, "%s", el);
        size_t n = strlen(dir);
        while (n && dir[n - 1] == '.')
            dir[--n] = 0;
        int first = nwalls;
        if (n && object_type(dir) == 2) {
            int offset = 0;
            char buf[1024];
            while (offset != -1 && nwalls < MAX_WALL) {
                int read = 0;
                if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 10, dir, buf, 32, offset, (int)sizeof buf, 0,
                          &read, &offset))
                    break;
                char *r = buf;
                for (int i = 0; i < read && nwalls < MAX_WALL; i++) {
                    int type = ((int *)r)[4];
                    char *leaf = r + 20;
                    if (type == 1 && !wall_known(leaf)) {
                        struct wall *w = &walls[nwalls];
                        snprintf(w->file, sizeof w->file, "%s.%s", dir, leaf);
                        snprintf(w->name, sizeof w->name, "%s", leaf);
                        img_load(&w->im, w->file, 0, 512 * 1024);
                        if (w->im.data)
                            nwalls++;
                    }
                    r = (char *)(((size_t)(leaf + strlen(leaf) + 1) + 3) & ~(size_t)3);
                }
            }
        }
        qsort(&walls[first], (size_t)(nwalls - first), sizeof walls[0], wall_by_name);
        if (!end)
            break;
        el = end + 1;
    }
}

static void walls_retable(void)
{
    img_table(&pic);
    for (int i = 0; i < nwalls; i++)
        img_table(&walls[i].im);
}

static void draw_walls(void)
{
    text(g.font_bold, "Wallpapers", PANE_X, WALL_HEAD_Y, LEFT, C_TEXT, C_PANE);
    text(g.font_small, "Click one to tile it. Your own go in Choices.Wallpapers.", PANE_X + 220, WALL_HEAD_Y, LEFT,
         C_SUBTEXT, C_PANE);
    for (int i = 0; i < nwalls; i++) {
        struct box b = wall_box(i);
        int on = !g.now.composite && g.now.fit == FIT_TILE && same(g.now.picture, walls[i].file);
        if (on)
            rrect(b.x0 - 8, b.y0 - 8, b.x1 + 8, b.y1 + 8, 18, C_ACCENT);
        rbox(b.x0 - (on ? 3 : 0), b.y0 - (on ? 3 : 0), b.x1 + (on ? 3 : 0), b.y1 + (on ? 3 : 0), 14, on ? 3 : 2,
             C_WHITE, on ? C_WHITE : C_CARDEDGE);
        struct box in = { b.x0 + 6, b.y0 + 6, b.x1 - 6, b.y1 - 6 };
        draw_img(&walls[i].im, in);
        char label[16];
        snprintf(label, sizeof label, "%s", walls[i].name);
        text(g.font_small, label, (b.x0 + b.x1) / 2, b.y0 - 28, CENTRE, on ? C_ACCENT : C_SUBTEXT, C_PANE);
    }
}

static void draw_composite(void)
{
    struct box b = composite_switch();
    int top = composite_top(), on = g.now.composite;
    int usable = g.host_backdrop || on;         /* an unavailable switch can still be turned off */
    rrect(b.x0, b.y0, b.x1, b.y1, 24, on ? (usable ? C_ACCENT : C_DIM) : g.host_backdrop ? C_DIM : C_SEGBG);
    disc(on ? b.x1 - 24 : b.x0 + 24, (b.y0 + b.y1) / 2, 19, usable ? C_WHITE : C_CARDEDGE);
    text(g.font_bold, "Composite", b.x1 + 28, b.y0 + 14, LEFT, g.host_backdrop ? C_TEXT : C_DIM, C_PANE);
    text(g.font_small, on ? "On" : "Off", b.x1 + 28 + text_width(g.font_bold, "Composite") + 16, b.y0 + 14, LEFT,
         on && g.host_backdrop ? C_ACCENT : g.host_backdrop ? C_SUBTEXT : C_DIM, C_PANE);
    text(g.font_small, "Lets the Mac's window show its own backdrop behind your windows.", PANE_X, top - 88, LEFT,
         g.host_backdrop ? C_SUBTEXT : C_DIM, C_PANE);
    if (!g.host_backdrop) {
        text(g.font_small, "Not available: the layer comes from BOX's Metal window, and this display is not it.",
             PANE_X, top - 122, LEFT, C_ACCENT, C_PANE);
        return;
    }
    text(g.font_small, "Needs a 32 bpp mode and BOX's Metal window.", PANE_X, top - 122, LEFT, C_SUBTEXT, C_PANE);
    int bpp = screen_bpp();
    if (bpp != 32) {
        char line[120];
        snprintf(line, sizeof line, "This desktop is in %s %d bpp mode: the host's layer will not show through.",
                 bpp == 8 ? "an" : "a", bpp);
        text(g.font_small, line, PANE_X, top - 156, LEFT, C_ACCENT, C_PANE);
    }
}

static void draw_wallpaper(void)
{
    text(g.font_bold, "Colour", PANE_X, HEAD_Y - 36, LEFT, C_TEXT, C_PANE);
    for (int i = 0; i < NSWATCHES; i++) {
        struct box b = swatch(i);
        int on = g.now.desk == swatches[i];
        if (on)
            rrect(b.x0 - 8, b.y0 - 8, b.x1 + 8, b.y1 + 8, 20, C_ACCENT);
        rbox(b.x0 - (on ? 3 : 0), b.y0 - (on ? 3 : 0), b.x1 + (on ? 3 : 0), b.y1 + (on ? 3 : 0), 16, on ? 3 : 2,
             swatches[i], on ? C_WHITE : mix(swatches[i], 0, 1, 4));
    }

    text(g.font_bold, "Picture", PANE_X, HEAD_Y - 196, LEFT, C_TEXT, C_PANE);
    if (g.now.composite)
        text(g.font_small, "Composite is on: this comes back when it is off.", PANE_X + 130, HEAD_Y - 196, LEFT,
             C_SUBTEXT, C_PANE);
    struct box w = well();
    if (g.now.picture[0]) {
        rbox(w.x0, w.y0, w.x1, w.y1, 18, 2, g.now.desk != NO_COLOUR ? g.now.desk : 0x77777700u, C_CARDEDGE);
        unsigned desk = g.now.desk != NO_COLOUR ? g.now.desk : 0x77777700u;
        struct box in = { w.x0 + 24, w.y0 + 64, w.x1 - 24, w.y1 - 24 };
        if (pic.data) {
            draw_picture(in);
        } else {
            rbox(in.x0 + 16, in.y0, in.x1 - 16, in.y1, 12, 2, C_WHITE, C_CARDEDGE);
            text(g.font_bold, leafname(g.now.picture), (w.x0 + w.x1) / 2, (in.y0 + in.y1) / 2, CENTRE, C_TEXT,
                 C_WHITE);
        }
        rect(w.x0 + 2, w.y0 + 2, w.x1 - 2, w.y0 + 58, desk);
        text(g.font_small, g.now.picture, (w.x0 + w.x1) / 2, w.y0 + 22, CENTRE, dark(desk) ? C_WHITE : C_TEXT,
             desk);
    } else {
        rbox(w.x0, w.y0, w.x1, w.y1, 18, 2, C_WHITE, C_CARDEDGE);
        /* a dashed edge inside, the drop target */
        for (int x = w.x0 + 30; x < w.x1 - 30; x += 24) {
            rect(x, w.y0 + 24, x + 12, w.y0 + 26, C_DIM);
            rect(x, w.y1 - 26, x + 12, w.y1 - 24, C_DIM);
        }
        for (int y = w.y0 + 30; y < w.y1 - 30; y += 24) {
            rect(w.x0 + 24, y, w.x0 + 26, y + 12, C_DIM);
            rect(w.x1 - 26, y, w.x1 - 24, y + 12, C_DIM);
        }
        text(g.font_bold, "Drag a sprite or a JPEG here", (w.x0 + w.x1) / 2, (w.y0 + w.y1) / 2 + 10, CENTRE,
             C_TEXT, C_WHITE);
        text(g.font_small, "It goes behind your windows, on the Pinboard", (w.x0 + w.x1) / 2,
             (w.y0 + w.y1) / 2 - 34, CENTRE, C_SUBTEXT, C_WHITE);
    }
    int has = g.now.picture[0] != 0;
    text(g.font_bold, "Fit", fit_button(0).x0, fit_button(0).y1 + 18, LEFT, has ? C_TEXT : C_DIM, C_PANE);
    for (int k = 0; k < 3; k++)
        button(fit_button(k), fit_names[k], has && g.now.fit == k, has);
    button(remove_button(), "Remove picture", 0, has);
    draw_walls();
    draw_composite();
}

static void draw_icons(void)
{
    const struct theme *t = current_theme();
    int enabled = t->painted;
    const char *now = current_set();
    for (int i = 0; i < nsets; i++) {
        const struct iconset *c = &sets[i];
        struct box b = set_row(i);
        int cur = enabled && now[0] && same(now, c->leaf);
        rrect(b.x0, b.y0 - 4, b.x1, b.y1 - 4, 18, C_SHADOW);
        rbox(b.x0, b.y0, b.x1, b.y1, 18, cur ? 4 : 2, C_CARD, cur ? C_ACCENT : C_CARDEDGE);
        const char *l1, *l2, *label = set_label(c, &l1, &l2);
        text(g.font_bold, label, b.x0 + 28, b.y1 - 62, LEFT, enabled ? C_TEXT : C_DIM, C_CARD);
        text(g.font_small, l1, b.x0 + 28, b.y1 - 106, LEFT, enabled ? C_SUBTEXT : C_DIM, C_CARD);
        text(g.font_small, l2, b.x0 + 28, b.y1 - 138, LEFT, enabled ? C_SUBTEXT : C_DIM, C_CARD);
        for (int k = 0; k < SAMPLES; k++) {
            int x0 = b.x0 + SET_TEXT_W + k * SAMPLE_PITCH, y0 = b.y0 + 40;
            if (c->area)
                sprite(c->area, samples[k].name, x0, y0, x0 + SAMPLE_PX, y0 + SAMPLE_PX);
            text(g.font_small, samples[k].caption, x0 + SAMPLE_PX / 2, b.y0 + 10, CENTRE, C_SUBTEXT, C_CARD);
        }
        if (cur) {
            disc(b.x1 - 40, b.y1 - 40, 16, C_ACCENT);
            gcol(C_WHITE);
            int cx = ox + b.x1 - 40, cy = oy + b.y1 - 40;
            for (int j = -1; j <= 1; j++) {
                plot(4, cx - 9, cy + 1 + j);
                plot(5, cx - 3, cy - 6 + j);
                plot(5, cx + 9, cy + 7 + j);
            }
        }
    }
    struct box last = set_row(nsets > 4 ? nsets - 1 : 3);
    char line[160];
    if (!enabled)
        snprintf(line, sizeof line, "%s draws its icons from sprites. Choose a painted theme to pick a set.",
                 t->name);
    else if (!now[0])
        snprintf(line, sizeof line, "%s draws the bitmap icons. Choose a set to use vector icons instead.",
                 t->name);
    else
        snprintf(line, sizeof line, "This changes %s for now; choosing another theme starts again from its own.",
                 t->name);
    text(g.font_body, line, PANE_X, last.y0 - 50, LEFT, C_SUBTEXT, C_PANE);
    text(g.font_small, "Edit, Paint, Draw, folders, discs and the standard file types look the same in every set.",
         PANE_X, last.y0 - 90, LEFT, C_SUBTEXT, C_PANE);
}

static void draw(void)
{
    rect(0, -g.win_h, SIDEBAR_W, 0, C_SIDEBAR);
    rect(SIDEBAR_W, -g.win_h, SIDEBAR_W + 2, 0, C_DIVIDER);
    rect(SIDEBAR_W + 2, -g.win_h, WIN_W, 0, C_PANE);

    /* the sidebar: the app's name, then the pages */
    disc(64, -84, 26, C_ACCENT);
    disc(56, -76, 10, 0x6FC8F400u);
    disc(76, -92, 8, 0x5E9C4A00u);
    text(g.font_bold, "Appearance", 104, -96, LEFT, C_TEXT, C_SIDEBAR);
    for (int i = 0; i < NPAGES; i++) {
        struct box b = sidebar_item(i);
        int on = i == g.page;
        if (on)
            rrect(b.x0, b.y0, b.x1, b.y1, 14, C_ACCENT);
        unsigned ink = on ? C_WHITE : C_TEXT, bg = on ? C_ACCENT : C_SIDEBAR;
        /* a small mark for each page */
        int mx = b.x0 + 34, my = (b.y0 + b.y1) / 2;
        if (i == PAGE_THEMES) {
            rect(mx - 12, my - 12, mx - 2, my - 2, ink), rect(mx + 2, my - 12, mx + 12, my - 2, ink);
            rect(mx - 12, my + 2, mx - 2, my + 12, ink), rect(mx + 2, my + 2, mx + 12, my + 12, ink);
        } else if (i == PAGE_WINDOWS) {
            rect(mx - 14, my - 12, mx + 14, my + 12, ink);
            rect(mx - 11, my - 9, mx + 11, my + 4, bg);
        } else if (i == PAGE_WALLPAPER) {
            rect(mx - 14, my - 12, mx + 14, my + 12, ink);
            rect(mx - 11, my - 9, mx + 11, my + 9, bg);
            disc(mx + 4, my + 3, 4, ink);
            rect(mx - 9, my - 7, mx + 9, my - 3, ink);
        } else {                        /* a folder */
            rect(mx - 14, my - 10, mx + 14, my + 8, ink);
            rect(mx - 14, my + 8, mx - 2, my + 12, ink);
            rect(mx - 11, my - 7, mx + 11, my + 5, bg);
        }
        text(on ? g.font_bold : g.font_body, page_names[i], b.x0 + 70, my - 10, LEFT, ink, bg);
    }

    text(g.font_head, page_heads[g.page], PANE_X, -84, LEFT, C_TEXT, C_PANE);
    text(g.font_body, page_subs[g.page], PANE_X, -128, LEFT, C_SUBTEXT, C_PANE);
    switch (g.page) {
    case PAGE_THEMES:
        draw_themes();
        break;
    case PAGE_WINDOWS:
        draw_windows();
        break;
    case PAGE_WALLPAPER:
        draw_wallpaper();
        break;
    case PAGE_ICONS:
        draw_icons();
        break;
    }

    /* the foot of the window: where the choice is kept, and Revert */
    rect(SIDEBAR_W + 2, -g.win_h + FOOTER_H, WIN_W, -g.win_h + FOOTER_H + 2, C_DIVIDER);
    char where[220];
    text(g.font_small, choices_name(where, sizeof where) ? "Kept in Choices: used each time the desktop starts"
                                                        : "Not kept: Choices$Write is not set",
         PANE_X, -g.win_h + 40, LEFT, C_SUBTEXT, C_PANE);
    int changed = memcmp(&g.now, &g.start, sizeof g.now) != 0;
    button(revert_button(), "Revert", 0, changed);
}

static void redraw(int *b)
{
    int more = 0;
    _swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
    while (more) {
        ox = b[1] - b[5], oy = b[4] - b[6];
        draw();
        _swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
    }
}

/* ---- doing what is chosen -------------------------------------------------------------------- */

static void apply_settings(void)
{
    for (int i = 0; i < NSETTINGS; i++)
        if (g.now.set[i] >= 0)
            report(cli("WimpThemeSet %s %s", settings[i].key, settings[i].values[g.now.set[i]]));
    if (g.now.vectors[0])
        report(cli("WimpThemeSet Theme.Vectors %s", g.now.vectors));
}

static void apply_backdrop(void)
{
    char tile[260];
    if (g.now.composite && tile_file(tile, sizeof tile)) {
        report(cli("Backdrop -Tile %s", tile));
        return;
    }
    if (g.now.desk != NO_COLOUR)
        report(cli("Backdrop -Colour &%08X", g.now.desk));
    if (g.now.picture[0])
        report(cli("Backdrop %s %s", fit_switch[g.now.fit], g.now.picture));
}

/* Puts the chosen wallpaper back on the Pinboard after Composite's tile has covered it.
 * If none is chosen, it puts back the desktop's own tile. */
static void restore_wallpaper(void)
{
    if (!g.now.picture[0] && g.now.desk == NO_COLOUR) {
        report(cli("Backdrop -Tile %s", DEFAULT_TILE));
        return;
    }
    if (!g.now.picture[0])
        cli("Backdrop -Remove");
    apply_backdrop();
}

static void composite_set(int on)
{
    if (on) {
        char tile[260];
        if (!tile_file(tile, sizeof tile)) {
            os_error e = { 0, "Composite needs Resources.Pinboard.BackTile on the disc." };
            report(&e);
            return;
        }
        os_error *err = cli("Backdrop -Tile %s", tile);
        if (err) {
            report(err);
            return;
        }
        g.now.composite = 1;
    } else {
        g.now.composite = 0;
        restore_wallpaper();
    }
    save_choices();
    redraw_all();
}

static void choose_theme(const struct theme *t)
{
    os_error *e = cli("WimpTheme %s", t->leaf[0] ? t->leaf : "Classic");
    if (e) {
        report(e);
        return;
    }
    snprintf(g.now.theme, sizeof g.now.theme, "%s", t->leaf);
    for (int i = 0; i < NSETTINGS; i++)
        g.now.set[i] = -1;
    g.now.vectors[0] = 0;
    save_choices();
}

static void revert(void)
{
    g.now = g.start;
    if (g.now.picture[0])
        load_picture(g.now.picture);
    else
        forget_picture();
    report(cli("WimpTheme %s", g.now.theme[0] ? g.now.theme : "Classic"));
    apply_settings();
    if (g.now.picture[0] || g.now.desk != NO_COLOUR || g.now.composite)
        apply_backdrop();
    else
        report(cli("Backdrop -Remove"));
    save_choices();
}

static void click(const int *b)
{
    if (b[3] == -2) {                   /* the icon bar */
        if (b[2] == 2) {
            int x = b[0] - 64;
            _swix(Wimp_CreateMenu, _INR(1, 3), g.menu, x, 96 + 2 * 44);  /* above the icon bar */
        } else {
            open_window();
        }
        return;
    }
    if (b[3] != g.window || (b[2] & 2))
        return;
    int s[10];
    s[0] = g.window;
    _swix(Wimp_GetWindowState, _IN(1), s);
    int x = b[0] - (s[1] - s[5]), y = b[1] - (s[4] - s[6]);

    for (int i = 0; i < NPAGES; i++)
        if (inside(sidebar_item(i), x, y)) {
            g.page = i;
            redraw_all();
            return;
        }
    if (inside(revert_button(), x, y)) {
        if (memcmp(&g.now, &g.start, sizeof g.now))
            revert();
        redraw_all();
        return;
    }
    switch (g.page) {
    case PAGE_THEMES:
        for (int i = 0; i < nthemes; i++)
            if (inside(card(i), x, y)) {
                choose_theme(&themes[i]);
                redraw_all();
                return;
            }
        break;
    case PAGE_WINDOWS:
        if (!current_theme()->painted)
            return;
        for (int i = 0; i < NSETTINGS; i++)
            for (int k = 0; k < settings[i].n; k++)
                if (inside(segment(i, k, settings[i].n), x, y)) {
                    os_error *e = cli("WimpThemeSet %s %s", settings[i].key, settings[i].values[k]);
                    if (e) {
                        report(e);
                        return;
                    }
                    g.now.set[i] = k;
                    save_choices();
                    redraw_all();
                    return;
                }
        break;
    case PAGE_ICONS:
        if (!current_theme()->painted)
            return;
        for (int i = 0; i < nsets; i++)
            if (inside(set_row(i), x, y)) {
                if (same(current_set(), sets[i].leaf))
                    return;
                os_error *e = cli("WimpThemeSet Theme.Vectors %s", sets[i].leaf);
                if (e) {
                    report(e);
                    return;
                }
                snprintf(g.now.vectors, sizeof g.now.vectors, "%s", sets[i].leaf);
                save_choices();
                redraw_all();
                return;
            }
        break;
    case PAGE_WALLPAPER:
        for (int i = 0; i < NSWATCHES; i++)
            if (inside(swatch(i), x, y)) {
                if (g.now.composite) {      /* the colour replaces the tagged tile */
                    g.now.composite = 0;
                    g.now.desk = swatches[i];
                    restore_wallpaper();
                    save_choices();
                    redraw_all();
                    return;
                }
                os_error *e = cli("Backdrop -Colour &%08X", swatches[i]);
                if (e) {
                    report(e);
                    return;
                }
                g.now.desk = swatches[i];
                save_choices();
                redraw_all();
                return;
            }
        for (int i = 0; i < nwalls; i++)
            if (inside(wall_box(i), x, y)) {
                os_error *e = cli("Backdrop -Tile %s", walls[i].file);
                if (e) {
                    report(e);
                    return;
                }
                g.now.composite = 0;
                g.now.fit = FIT_TILE;
                snprintf(g.now.picture, sizeof g.now.picture, "%s", walls[i].file);
                load_picture(g.now.picture);
                save_choices();
                redraw_all();
                return;
            }
        if (inside(composite_hit(), x, y)) {
            if (g.host_backdrop || g.now.composite)     /* with no layer, it can only be turned off */
                composite_set(!g.now.composite);
            return;
        }
        if (g.now.picture[0]) {
            for (int k = 0; k < 3; k++)
                if (inside(fit_button(k), x, y)) {
                    os_error *e = cli("Backdrop %s %s", fit_switch[k], g.now.picture);
                    if (e) {
                        report(e);
                        return;
                    }
                    g.now.fit = k;
                    g.now.composite = 0;
                    save_choices();
                    redraw_all();
                    return;
                }
            if (inside(remove_button(), x, y)) {
                report(cli("Backdrop -Remove"));
                g.now.picture[0] = 0;
                g.now.composite = 0;
                forget_picture();
                if (g.now.desk != NO_COLOUR)
                    cli("Backdrop -Colour &%08X", g.now.desk);
                save_choices();
                redraw_all();
            }
        }
        break;
    }
}

/* A file dropped on the window: a picture for the wallpaper */
static void data_load(int *b)
{
    int type = b[10];
    ((char *)b)[255] = 0;               /* the name ends within the block, whatever the sender sent */
    const char *name = (const char *)&b[11];
    if (b[5] != g.window)
        return;
    if (type != 0xFF9 && type != 0xC85 && type != 0xB60) {
        os_error e = { 0, "Drag a sprite, JPEG or PNG file here for the desktop's picture." };
        report(&e);
        return;
    }
    os_error *err = cli("Backdrop %s %s", fit_switch[g.now.fit], name);
    if (err) {
        report(err);
        return;
    }
    g.now.composite = 0;
    snprintf(g.now.picture, sizeof g.now.picture, "%s", name);
    load_picture(g.now.picture);
    b[3] = b[2];
    b[4] = MSG_DATALOADACK;
    _swix(Wimp_SendMessage, _INR(0, 2), EV_MESSAGE, b, b[1]);
    g.page = PAGE_WALLPAPER;
    save_choices();
    redraw_all();
}

static void message(int *b)
{
    switch (b[4]) {
    case MSG_QUIT:
        g.quitting = 1;
        break;
    case MSG_DATALOAD:
        data_load(b);
        break;
    case MSG_THEMECHANGED: {
        /* by us, or by someone at the command line */
        char was[32];
        snprintf(was, sizeof was, "%s", g.now.theme);
        read_current();
        if (!same(was, g.now.theme)) {
            for (int i = 0; i < NSETTINGS; i++)
                g.now.set[i] = -1;
            g.now.vectors[0] = 0;
        }
        redraw_all();
        break;
    }
    case MSG_MODECHANGE:
        make_samples();
        walls_retable();
        redraw_all();
        break;
    }
}

static void menus_init(void)
{
    memset(g.menu, 0, sizeof g.menu);
    strncpy((char *)&g.menu[0], "Appearance", 12);
    unsigned char *c = (unsigned char *)&g.menu[3];
    c[0] = 7, c[1] = 2, c[2] = 7, c[3] = 0;
    g.menu[4] = 200, g.menu[5] = 44, g.menu[6] = 0;
    int *it = &g.menu[7];
    it[0] = 0, it[1] = -1, it[2] = 0x07000021;
    strncpy((char *)&it[3], "Open...", 12);
    it += 6;
    it[0] = 0x80, it[1] = -1, it[2] = 0x07000021;
    strncpy((char *)&it[3], "Quit", 12);
}

int main(void)
{
    static const int messages[] = { MSG_DATALOAD, MSG_MODECHANGE, MSG_THEMECHANGED, 0 };
    os_error *e = _swix(Wimp_Initialise, _INR(0, 3) | _OUT(1), 310, TASK_WORD, "Appearance", messages, &g.task);
    if (e) {
        report(e);
        return 1;
    }
    choice_clear(&g.now);
    read_current();
    load_choices();
    read_current();                     /* the Wimp's theme, whatever the file says */
    g.start = g.now;
    if (g.now.picture[0])
        load_picture(g.now.picture);
    scan_themes();
    scan_sets();
    scan_walls();
    char flag[8];
    g.host_backdrop = read_var("BOX$HostBackdrop", flag, sizeof flag) > 0;
    fonts();
    menus_init();
    if ((e = create_window())) {
        report(e);
        _swix(Wimp_CloseDown, _INR(0, 1), g.task, TASK_WORD);
        return 1;
    }
    int ib[9] = { -1, 0, 0, 68, 68, 0x301A, 0, 0, 0 };
    strncpy((char *)&ib[6], "!appearance", 12);
    if ((e = _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, ib, &g.icon))) {
        report(e);
        _swix(Wimp_CloseDown, _INR(0, 1), g.task, TASK_WORD);
        return 1;
    }

    while (!g.quitting) {
        int reason = 0;
        e = _swix(Wimp_Poll, _INR(0, 1) | _OUT(0), 0x3831, g.poll, &reason);
        if (e) {
            report(e);
            continue;
        }
        int *b = g.poll;
        switch (reason) {
        case EV_REDRAW:
            redraw(b);
            break;
        case EV_OPEN:
            _swix(Wimp_OpenWindow, _IN(1), b);
            break;
        case EV_CLOSE:
            _swix(Wimp_CloseWindow, _IN(1), b);
            g.open = 0;
            break;
        case EV_CLICK:
            click(b);
            break;
        case EV_KEY:
            _swix(Wimp_ProcessKey, _IN(0), b[6]);
            break;
        case EV_MENU: {
            int ptr[5];
            _swix(Wimp_GetPointerInfo, _IN(1), ptr);
            if (b[0] == 0)
                open_window();
            else if (b[0] == 1)
                g.quitting = 1;
            if (ptr[2] == 1 && !g.quitting)
                _swix(Wimp_CreateMenu, _IN(1), g.menu);
            break;
        }
        case EV_MESSAGE:
        case EV_RECORDED:
            message(b);
            break;
        default:
            break;
        }
    }
    lose_fonts();
    _swix(Wimp_CloseDown, _INR(0, 1), g.task, TASK_WORD);
    return 0;
}
