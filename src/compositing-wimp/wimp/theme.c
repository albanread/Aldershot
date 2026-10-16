/* theme.c -- the desktop's theme, held by the Wimp and changed while it
 * runs (RISCOSGrandDesign design 35, phase P1).
 *
 * A theme is a directory, found on Themes: (Themes$Path, which the disc's
 * !Boot sets: the user's Choices.Themes, then the disc's Resources.Themes;
 * the Wimp's default is the ROM's Resources:$.Resources.Themes).  In it, a
 * Tools file of tool sprites and a Sprites file of icon sprites (with
 * Sprites22 and the rest), as RISC OS 5's theme folders have them.  Both
 * are optional.  "Classic" is the ROM's own tools and icons.
 *
 * *WimpTheme <name> switches to a theme on the running desktop:
 *
 *   - the tools come from the theme's Tools (else WindowManager:Tools) and
 *     the tool list is built again, so the furniture's sizes are right;
 *   - the theme's Sprites (Sprites22 and the rest, by the screen's pixel
 *     shape) become the ROM sprite area, as RISC OS 5's !ThemeDefs puts
 *     them over Resources.Wimp.Sprites; the RAM pool, where applications'
 *     own icons are, is not touched, and its sprites still come first;
 *   - Wimp$IconTheme is set to "<name>.", as RISC OS 5's ThemeSetup sets
 *     it, so applications with icons for each theme load the right ones
 *     (*IconSprites puts it into their file names); Classic unsets it;
 *   - with the desktop running, every task's window is reopened (the
 *     furniture may be a new size), Message_WimpThemeChanged is broadcast,
 *     and the whole screen is redrawn.
 *
 * Nothing is changed until the theme has been found, so a mistyped name is
 * an error and not a broken desktop.
 *
 * *ToolSprites <file> (RISC OS's command, which BOX's Wimp did not honour
 * before) comes here too, through wimp_tools_use.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

/* OS_File 17's object type for a name: 0 none, 1 file, 2 directory */
static int object_type(const char *name)
{
    struct wimp_ws *w = wimp_ws();
    char *n = (char *)w->scratch + 256;
    snprintf(n, 250, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17;
    c.r[1] = ros_addr(n);
    ros_swi(&c, XOS_File);
    return c.v ? 0 : (int)c.r[0];
}

static os_error *write_line(const char *text)
{
    struct wimp_ws *w = wimp_ws();
    snprintf((char *)w->scratch, sizeof w->scratch, "%s", text);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(w->scratch);
    ros_swi(&c, XOS_Write0);
    if (c.v)
        return ros_ptr(c.r[0]);
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_NewLine);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* After the tools or the pool have changed: the tool list built again and,
 * with the desktop running, every window reopened and the screen redrawn */
static void refresh(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w->tools)
        xos_module_free(ros_ptr(w->tools));
    w->tools = 0;
    wimp_tools_refresh();
    wimp_tiles_forget();
    if (w->ntasks) {
        wimp_reopen_requests();
        wimp_invalidate_box(wimp_screen_box());
    }
}

os_error *wimp_tools_use(const char *file)
{
    struct wimp_ws *w = wimp_ws();
    if (file[0] && object_type(file) != 1)
        return ros_error(0xD6, "File '%s' not found", file);
    snprintf(w->tools_file, sizeof w->tools_file, "%s", file);
    refresh();
    return NULL;
}

/* ---- the Theme file (design 35 P2): painted chrome ------------------------------------ */

static int lower(int ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
}

static int word_is(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b))
            return 0;
    return !*a && !*b;
}

/* "&RRGGBB" as &BBGGRR00; 1 if it was one */
static int colour(const char *v, uint32_t *out)
{
    while (*v == ' ')
        v++;
    if (*v != '&' && *v != '#')
        return 0;
    uint32_t n = 0;
    int digits = 0;
    for (v++; *v; v++, digits++) {
        int ch = lower(*v);
        if (ch >= '0' && ch <= '9') n = n << 4 | (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') n = n << 4 | (uint32_t)(ch - 'a' + 10);
        else break;
    }
    if (digits != 6)
        return 0;
    *out = (n >> 16 & 0xFFu) << 8 | (n >> 8 & 0xFFu) << 16 | (n & 0xFFu) << 24;
    return 1;
}

/* A fill: a colour; "gradient &top &bottom"; "stripes &a &b" (rows
 * alternating, as a pinstriped title bar); "across &left &right" (a gradient
 * left to right); "ridges &bar &dark &light" (engraved ridges, a dark row
 * then a light, in a band across the bar).  *kind says which (FILL_...),
 * *light the ridges' light colour.  Nothing is changed unless it reads. */
static int fill(const char *v, uint32_t f[2], int *kind, uint32_t *light)
{
    while (*v == ' ')
        v++;
    static const struct { const char *word; int kind, colours; } kinds[] = {
        { "gradient", FILL_PLAIN, 2 }, { "stripes", FILL_STRIPES, 2 },
        { "across", FILL_ACROSS, 2 }, { "ridges", FILL_RIDGES, 3 },
    };
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        size_t n = strlen(kinds[k].word);
        int match = 1;
        for (size_t i = 0; i < n && match; i++)
            match = lower(v[i]) == kinds[k].word[i];
        if (!match || (v[n] != ' ' && v[n] != '\t'))
            continue;
        uint32_t c[3];
        const char *p = v + n;
        for (int i = 0; i < kinds[k].colours; i++) {
            if (!colour(p, &c[i]))
                return 0;
            while (*p == ' ')
                p++;
            while (*p && *p != ' ')
                p++;
        }
        f[0] = c[0], f[1] = c[1];
        if (kinds[k].colours == 3 && light)
            *light = c[2];
        *kind = kinds[k].kind;
        return 1;
    }
    uint32_t c;
    if (!colour(v, &c))
        return 0;
    f[0] = f[1] = c;
    *kind = FILL_PLAIN;
    return 1;
}

static int32_t number(const char *v)
{
    int32_t n = 0;
    while (*v == ' ')
        v++;
    while (*v >= '0' && *v <= '9') {
        if (n < 100000)                         /* no overflow, whatever the file says */
            n = n * 10 + (*v - '0');
        v++;
    }
    return n;
}

/* A furniture size, OS units: 0 (the tools' own), or 16 to 160, a whole
 * number of pixels */
static int32_t furniture_size(const char *v)
{
    const struct wimp_ws *w = wimp_ws();
    int32_t n = number(v);
    if (n == 0)
        return 0;
    if (n < 16)
        n = 16;
    if (n > 160)
        n = 160;
    int32_t px = w->dx > w->dy ? w->dx : w->dy;
    return px > 1 ? n / px * px : n;
}

static void style_defaults(struct wimp_style *st)
{
    wimp_title_font_forget(st);
    memset(st, 0, sizeof *st);
    st->outline_w = 1;
    st->focus_fill[0] = st->focus_fill[1] = 0xDDDDDD00u;
    st->unfocus_fill[0] = st->unfocus_fill[1] = 0xDDDDDD00u;
    st->focus_text = 0;
    st->unfocus_text = 0x55555500u;
    st->button_colour = 0xFFFFFFFFu;
    st->track = 0xCCCCCC00u;
    st->track2 = 0xFFFFFFFFu;
    st->arrow_face = 0xFFFFFFFFu;
    st->ctl_face = 0xFFFFFF00u, st->ctl_edge = 0x80808000u, st->ctl_accent = 0xF57B3D00u, st->ctl_mark = 0;
    st->fld_face = 0xFFFFFF00u, st->fld_edge = 0x80808000u, st->fld_focus = 0xFFFFFFFFu;
    st->btn_edge = 0x40404000u, st->btn_ring = st->btn_default = st->btn_default_text = 0xFFFFFFFFu;
    st->frame_face = 0xDDDDDD00u;
    st->thumb = 0x88888800u;
    st->outline = 0;
    st->bevel_light = 0xFFFFFF00u;
    st->bevel_dark = 0x55555500u;
    st->button_face = 0xDDDDDD00u;
    st->menu_bg = 0xFFFFFF00u;
    st->menu_text = 0;
    st->menu_hl = 0;
    st->menu_hl_text = 0xFFFFFF00u;
    st->menu_sep = 0;
    st->menu_title[0] = st->menu_title[1] = 0xDDDDDD00u;
    st->menu_title_text = 0;
    st->button_text = 0xFFFFFFFFu;
    st->iconbar_bg = 0xFFFFFFFFu;
    st->iconbar_text = 0xFFFFFFFFu;
}

/* Whether a window is the icon bar, and the theme has an [Iconbar] */
int wimp_iconbar_themed(const struct wimp_window *win)
{
    struct wimp_ws *w = wimp_ws();
    return w->style.iconbar && win && win->handle == w->iconbar;
}

/* Whether a window is a menu the theme draws: a painted theme with a
 * [Menus] section, and a level of the open menu tree */
int wimp_menu_themed(const struct wimp_window *win)
{
    const struct wimp_style *st = &wimp_ws()->style;
    return st->painted && st->menus && win && wimp_menu_level(win->handle) >= 0;
}

/* One setting, Section.Key value, into the style: 1 if it was known */
static int style_set(struct wimp_style *st, const char *section, const char *key, const char *v)
{
    char sk[96];
    snprintf(sk, sizeof sk, "%s.%s", section, key);
    if (word_is(sk, "Chrome.Style")) { st->painted = lower(v[0]) == 'p'; return 1; }
    if (word_is(sk, "Chrome.TitleHeight")) { st->title_h = furniture_size(v); return 1; }
    if (word_is(sk, "Chrome.ScrollWidth")) { st->scroll_w = furniture_size(v); return 1; }
    if (word_is(sk, "Chrome.BorderWidth")) {
        int32_t n = number(v);
        st->outline_w = n < 1 ? 1 : n > 4 ? 4 : (uint32_t)n;
        return 1;
    }
    if (word_is(sk, "Title.Focus.Fill")) return fill(v, st->focus_fill, &st->focus_stripes, &st->focus_light);
    if (word_is(sk, "Title.Focus.Text")) return colour(v, &st->focus_text);
    if (word_is(sk, "Title.Unfocused.Fill")) return fill(v, st->unfocus_fill, &st->unfocus_stripes, &st->unfocus_light);
    if (word_is(sk, "Title.Unfocused.Text")) return colour(v, &st->unfocus_text);
    if (word_is(sk, "Title.Font") || word_is(sk, "Menus.Font")) {
        /* "Homerton.Bold 12": a font and its size in points; "desktop": the desktop font */
        int menu = lower(sk[0]) == 'm';
        char *nm = menu ? st->menu_font : st->title_font;
        uint32_t *size = menu ? &st->menu_font_size : &st->title_font_size;
        char name[40];
        unsigned n = 0;
        while (*v == ' ')
            v++;
        while (v[n] > ' ' && n < sizeof name - 1)
            name[n] = v[n], n++;
        name[n] = 0;
        int32_t pt = number(v + n);
        wimp_title_font_forget(st);
        if (!name[0] || word_is(name, "desktop")) {
            nm[0] = 0;
            return 1;
        }
        snprintf(nm, 40, "%s", name);
        *size = (uint32_t)((pt > 4 && pt < 64 ? pt : 12) * 16);
        return 1;
    }
    if (word_is(sk, "Title.Align") || word_is(sk, "Title.Focus.Align")) {
        st->align_left = lower(v[0]) == 'l';
        return 1;
    }
    if (word_is(sk, "Buttons.Set")) {
        st->buttons = lower(v[0]) == 'd' ? STYLE_BUTTONS_DOTS
                    : lower(v[0]) == 'b' ? STYLE_BUTTONS_BOXES
                    : lower(v[0]) == 'p' ? STYLE_BUTTONS_PLATINUM : STYLE_BUTTONS_GLYPHS;
        return 1;
    }
    if (word_is(sk, "Buttons.Layout")) { st->buttons_right = lower(v[0]) == 'r'; return 1; }
    if (word_is(sk, "Buttons.HideUnfocused")) { st->hide_unfocused = lower(v[0]) == 'y' || v[0] == '1'; return 1; }
    if (word_is(sk, "Scroll.Grip")) { st->grip = lower(v[0]) == 'y' || v[0] == '1'; return 1; }
    if (word_is(sk, "Chrome.Focus")) { st->focus_front = lower(v[0]) == 'f'; return 1; }
    if (word_is(sk, "Theme.Icons") || word_is(sk, "Theme.Vectors")) {
        char *to = word_is(sk, "Theme.Icons") ? st->icons : st->vectors;
        snprintf(to, sizeof st->icons, "%s", v);
        size_t n = strlen(to);
        while (n && to[n - 1] <= ' ')
            to[--n] = 0;
        return 1;
    }
    if (word_is(sk, "Buttons.Face")) return colour(v, &st->button_face);
    if (word_is(sk, "Chrome.Radius")) {
        int32_t n = number(v);
        st->radius = n < 0 ? 0 : n > 64 ? 64 : n;
        return 1;
    }
    if (word_is(sk, "Chrome.Shadow")) {
        /* "16 90": its extent, OS units, and its strength at the edge, 0-255 */
        int32_t n = number(v);
        while (*v == ' ')
            v++;
        while (*v >= '0' && *v <= '9')
            v++;
        int32_t a = number(v);
        st->shadow_size = n < 0 ? 0 : n > 64 ? 64 : n;
        st->shadow_alpha = (uint32_t)(a > 0 && a < 256 ? a : 90);
        return 1;
    }
    if (word_is(sk, "Chrome.RoundBottom")) {
        st->square_bottom = lower(v[0]) == 'n' || v[0] == '0';
        return 1;
    }
    if (word_is(sk, "Chrome.Bevel")) {
        int32_t n = number(v);
        st->bevel = n < 0 ? 0 : n > 4 ? 4 : (uint32_t)n;
        return 1;
    }
    if (word_is(sk, "Chrome.BevelLight")) return colour(v, &st->bevel_light);
    if (word_is(sk, "Chrome.BevelDark")) return colour(v, &st->bevel_dark);
    if (word_is(sk, "Buttons.Colour")) return colour(v, &st->button_colour);
    if (word_is(sk, "Scroll.Style")) {
        st->scroll = lower(v[0]) == 's' ? STYLE_SCROLL_SLIM
                   : lower(v[0]) == 'c' && lower(v[1]) == 'h' ? STYLE_SCROLL_CHANNEL : STYLE_SCROLL_CLASSIC;
        return 1;
    }
    if (word_is(sk, "Scroll.Track")) {
        /* a colour, or "checker &a &b": pixels of the two alternating, as a
         * Windows scroll bar's track */
        const char *p = v;
        while (*p == ' ')
            p++;
        if (lower(p[0]) == 'c' && lower(p[1]) == 'h') {
            uint32_t a, b;
            while (*p && *p != ' ')
                p++;
            if (!colour(p, &a))
                return 0;
            while (*p == ' ')
                p++;
            while (*p && *p != ' ')
                p++;
            if (!colour(p, &b))
                return 0;
            st->track = a, st->track2 = b;
            return 1;
        }
        if (!colour(v, &st->track))
            return 0;
        st->track2 = 0xFFFFFFFFu;
        return 1;
    }
    if (word_is(sk, "Scroll.Thumb")) return colour(v, &st->thumb);
    if (word_is(sk, "Scroll.ArrowFace")) return colour(v, &st->arrow_face);
    if (word_is(sk, "Window.Outline")) return colour(v, &st->outline);
    if (word_is(sk, "Window.ResizeBar")) {
        int32_t n = number(v);
        st->resize_bar = n < 0 ? 0 : n > 16 ? 16 : n;
        return 1;
    }
    if (word_is(sk, "Window.Frame")) {
        /* "4 &DDDDDD": a raised frame that many pixels wide, in that colour; 0 none */
        int32_t n = number(v);
        while (*v == ' ')
            v++;
        while (*v >= '0' && *v <= '9')
            v++;
        uint32_t c = 0xDDDDDD00u;
        while (*v == ' ')
            v++;
        if (*v && !colour(v, &c))
            return 0;
        st->frame = n < 0 ? 0 : n > 8 ? 8 : n;
        st->frame_face = c;
        return 1;
    }
    if (word_is(section, "Buttons.Action")) {
        /* the dialogue buttons (action and default, R5 and R6): the colours
         * *WimpVisualFlags sets, undone at the next *WimpTheme */
        struct wimp_draw *d = &wimp_ws()->dr;
        uint32_t c;
        if (word_is(key, "Text")) return colour(v, &st->button_text);
        /* their shape, drawn by the theme (draw.c, pbutton_icon) */
        if (word_is(key, "Shape")) {
            st->action_shape = lower(v[0]) == 'r' && lower(v[1]) == 'a' ? BUTTONS_RAISED : lower(v[0]) == 'f' ? BUTTONS_FLAT
                        : lower(v[0]) == 'r' ? BUTTONS_ROUNDED : BUTTONS_ACORN;
            return 1;
        }
        if (word_is(key, "Edge")) return colour(v, &st->btn_edge);
        if (word_is(key, "Ring")) return colour(v, &st->btn_ring);
        if (word_is(key, "DefaultFill")) return colour(v, &st->btn_default);
        if (word_is(key, "DefaultText")) return colour(v, &st->btn_default_text);
        if (!colour(v, &c))
            return 0;
        if (word_is(key, "Face")) d->theme[TH_BC] = c;
        else if (word_is(key, "Default")) d->theme[TH_BWC] = c;
        else if (word_is(key, "Pressed")) d->theme[TH_BHC] = c;
        else if (word_is(key, "BorderLight")) d->theme[TH_BBFC] = c;
        else if (word_is(key, "BorderDark")) d->theme[TH_BBOC] = c;
        else if (word_is(key, "Shallow")) d->theme[TH_BBSC] = c;
        else return 0;
        return 1;
    }
    if (word_is(section, "Iconbar")) {
        int ok = 0;
        if (word_is(key, "Background")) ok = colour(v, &st->iconbar_bg);
        else if (word_is(key, "Text")) ok = colour(v, &st->iconbar_text);
        if (ok)
            st->iconbar = 1;
        return ok;
    }
    if (word_is(section, "Fields")) {
        /* the writable icons, drawn by the theme (draw.c, pfield) */
        if (word_is(key, "Style")) {
            st->fields = lower(v[0]) == 's' ? FIELDS_SUNKEN : lower(v[0]) == 'f' ? FIELDS_FLAT
                       : lower(v[0]) == 'o' ? FIELDS_OUTLINE : FIELDS_ACORN;
            return 1;
        }
        if (word_is(key, "Face")) return colour(v, &st->fld_face);
        if (word_is(key, "Edge")) return colour(v, &st->fld_edge);
        if (word_is(key, "Focus")) return colour(v, &st->fld_focus);
        return 0;
    }
    if (word_is(section, "Controls")) {
        /* the option and radio icons, drawn by the theme (draw.c, pcontrol) */
        if (word_is(key, "Style")) {
            st->controls = lower(v[0]) == 'f' ? CONTROLS_FLAT : lower(v[0]) == 's' && lower(v[1]) == 'u' ? CONTROLS_SUNKEN
                         : lower(v[0]) == 'r' ? CONTROLS_RAISED : lower(v[0]) == 'o' ? CONTROLS_OUTLINE : CONTROLS_SPRITES;
            return 1;
        }
        if (word_is(key, "Mark") && (lower(v[0]) == 't' || lower(v[0]) == 'c')) {
            st->ctl_cross = lower(v[0]) == 'c';
            return 1;
        }
        if (word_is(key, "Face")) return colour(v, &st->ctl_face);
        if (word_is(key, "Edge")) return colour(v, &st->ctl_edge);
        if (word_is(key, "Accent")) return colour(v, &st->ctl_accent);
        if (word_is(key, "MarkColour")) return colour(v, &st->ctl_mark);
        return 0;
    }
    if (word_is(sk, "Buttons.Iconise")) { st->iconise_square = lower(v[0]) == 's'; return 1; }
    if (word_is(section, "Menus")) {
        int ok = 0;
        if (word_is(key, "Background")) ok = colour(v, &st->menu_bg);
        else if (word_is(key, "Text")) ok = colour(v, &st->menu_text);
        else if (word_is(key, "Highlight")) ok = colour(v, &st->menu_hl);
        else if (word_is(key, "HighlightText")) ok = colour(v, &st->menu_hl_text);
        else if (word_is(key, "Separator")) ok = colour(v, &st->menu_sep);
        else if (word_is(key, "TitleFill")) ok = fill(v, st->menu_title, &st->menu_title_stripes, &st->menu_title_light);
        else if (word_is(key, "TitleText")) ok = colour(v, &st->menu_title_text);
        else if (word_is(key, "Cells")) {
            st->menu_cells = lower(v[0]) == 'y' || v[0] == '1';
            ok = 1;
        }
        if (ok)
            st->menus = 1;
        return ok;
    }
    if (word_is(sk, "Window.VisualFlags")) {
        struct wimp_ws *w = wimp_ws();
        snprintf((char *)w->scratch, sizeof w->scratch, "WimpVisualFlags %s", v);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(w->scratch);
        ros_swi(&c, XOS_CLI);
        return 1;
    }
    if (word_is(section, "Theme"))
        return 1;                               /* Name, Author, Version, Base, Accent: read elsewhere */
    return 0;
}

/* A theme's Theme file, loaded into the RMA with a terminator: NULL if
 * it has none */
static char *load_text(const char *dir)
{
    struct wimp_ws *w = wimp_ws();
    char *n = (char *)w->scratch + 256;
    snprintf(n, 250, "%s.Theme", dir);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 17, c.r[1] = ros_addr(n);
    ros_swi(&c, XOS_File);
    if (c.v || c.r[0] != 1 || c.r[4] > 64 * 1024)
        return NULL;
    uint32_t size = c.r[4];
    void *mem;
    if (xos_module_claim(size + 1, &mem))
        return NULL;
    ros_cpu_enter(&c);
    c.r[0] = 16, c.r[1] = ros_addr(n), c.r[2] = ros_addr(mem), c.r[3] = 0;
    ros_swi(&c, XOS_File);
    if (c.v) {
        xos_module_free(mem);
        return NULL;
    }
    ((char *)mem)[size] = 0;
    return mem;
}

/* The settings of a Theme file's text.  apply 0: only its [Theme] Base is
 * read, into base; apply 1: every setting goes into the style. */
static void parse(const char *text, int apply, char *base, size_t base_size)
{
    char section[48] = "";
    const char *p = text;
    while (*p) {
        char line[256];
        unsigned k = 0;
        while (*p && *p != '\n' && *p != '\r') {
            if (k < sizeof line - 1)
                line[k++] = *p;
            p++;
        }
        while (*p == '\n' || *p == '\r')
            p++;
        line[k] = 0;
        char *bar = strchr(line, '|');
        if (bar)
            *bar = 0;
        char *l = line;
        while (*l == ' ' || *l == '\t')
            l++;
        size_t len = strlen(l);
        while (len && (l[len - 1] == ' ' || l[len - 1] == '\t'))
            l[--len] = 0;
        if (!*l)
            continue;
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (e) {
                *e = 0;
                snprintf(section, sizeof section, "%s", l + 1);
            }
            continue;
        }
        char *colon = strchr(l, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *key = l, *val = colon + 1;
        size_t kl = strlen(key);
        while (kl && key[kl - 1] == ' ')
            key[--kl] = 0;
        while (*val == ' ' || *val == '\t')
            val++;
        if (!apply) {
            if (word_is(section, "Theme") && word_is(key, "Base"))
                snprintf(base, base_size, "%s", val);
        } else {
            style_set(&wimp_ws()->style, section, key, val);
        }
    }
}

/* A theme's Theme file read into the style: its Base first (to four deep;
 * Classic is the defaults), then its own settings.  The nearest base's
 * directory in base_dir, if base_dir is given and the theme names one. */
static void read_theme(const char *dir, int depth, char *base_dir, size_t base_size)
{
    char *text = load_text(dir);
    if (!text)
        return;
    char base[48] = "";
    parse(text, 0, base, sizeof base);
    if (base[0] && depth < 4 && !word_is(base, "Classic")) {
        char bdir[160];
        snprintf(bdir, sizeof bdir, "Themes:%s", base);
        if (base_dir)
            snprintf(base_dir, base_size, "%s", bdir);
        read_theme(bdir, depth + 1, NULL, 0);
    }
    parse(text, 1, NULL, 0);
    xos_module_free(text);
}

/* ---- *WimpTheme ---------------------------------------------------------------------- */

/* WimpVisualFlags as they were before any theme or setting changed them
 * (Window.VisualFlags, [Buttons.Action]), kept once, so that one theme's
 * do not stay for the next */
static void keep_visual_flags(void)
{
    struct wimp_ws *w = wimp_ws();
    if (w->vf_saved)
        return;
    w->vf_saved = 1;
    w->vf_threed = w->dr.threed;
    memcpy(w->vf_theme, w->dr.theme, sizeof w->vf_theme);
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static void set_icon_theme(const char *leaf)
{
    struct wimp_ws *w = wimp_ws();
    char *var = (char *)w->scratch + 256, *val = (char *)w->scratch + 300;
    strcpy(var, "Wimp$IconTheme");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(var);
    if (leaf) {
        snprintf(val, 200, "%s.", leaf);
        c.r[1] = ros_addr(val);
        c.r[2] = (uint32_t)strlen(val);
    } else {
        c.r[1] = 0;
        c.r[2] = 0xFFFFFFFFu;                   /* delete it */
    }
    c.r[3] = 0;
    c.r[4] = 0;                                 /* a string */
    ros_swi(&c, XOS_SetVarVal);
}

static void announce(const char *name)
{
    uint8_t b[64];
    memset(b, 0, sizeof b);
    size_t len = strlen(name);
    if (len > 39)
        len = 39;
    uint32_t size = 20 + (uint32_t)((len + 1 + 3) & ~3u);
    b[0] = (uint8_t)size;
    b[16] = (uint8_t)(MSG_WIMP_THEME_CHANGED);
    b[17] = (uint8_t)(MSG_WIMP_THEME_CHANGED >> 8);
    b[18] = (uint8_t)(MSG_WIMP_THEME_CHANGED >> 16);
    b[19] = (uint8_t)(MSG_WIMP_THEME_CHANGED >> 24);
    memcpy(b + 20, name, len);                  /* +20: the theme's name, "" for Classic */
    wimp_queue_message(17, b, size, RECV_BROADCAST, 0, 0);
}

os_error *wimp_cmd_wimptheme(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct wimp_ws *w = wimp_ws();
    char name[40] = "";
    if (argc) {
        unsigned n = 0;
        for (uint32_t p = tail; ros_ld8(p) > ' ' && n < sizeof name - 1; p++)
            name[n++] = (char)ros_ld8(p);
        name[n] = 0;
    }
    if (!name[0]) {
        char line[80];
        snprintf(line, sizeof line, "Theme: %s", w->theme[0] ? w->theme : "Classic");
        return write_line(line);
    }
    size_t len = strlen(name);
    while (len && name[len - 1] == '.')         /* Wimp$IconTheme's form, "Acorn." */
        name[--len] = 0;
    int classic = same_name(name, "Classic");
    char dir[160] = "";
    if (!classic) {
        if (strchr(name, '.') || strchr(name, ':'))
            snprintf(dir, sizeof dir, "%s", name);
        else
            snprintf(dir, sizeof dir, "Themes:%s", name);
        if (object_type(dir) != 2)
            return ros_error(0xD6, "Theme '%s' not found", name);
    }
    /* the leaf, for Wimp$IconTheme and the name kept */
    const char *leaf = name;
    for (const char *p = name; *p; p++)
        if (*p == '.' || *p == ':')
            leaf = p + 1;

    keep_visual_flags();
    w->dr.threed = w->vf_threed;
    memcpy(w->dr.theme, w->vf_theme, sizeof w->vf_theme);
    style_defaults(&w->style);
    char file[200], base[160] = "";
    w->tools_file[0] = 0;
    w->theme_sprites[0] = 0;
    w->theme_vectors[0][0] = w->theme_vectors[1][0] = 0;
    if (!classic) {
        read_theme(dir, 0, base, sizeof base);  /* the painted chrome, if it has a Theme file */
        snprintf(file, sizeof file, "%s.Tools", dir);
        if (object_type(file) != 1 && base[0])
            snprintf(file, sizeof file, "%s.Tools", base);
        if (object_type(file) == 1)
            snprintf(w->tools_file, sizeof w->tools_file, "%s", file);
        snprintf(w->theme_sprites, sizeof w->theme_sprites, "%s.Sprites", dir);
        if (base[0] && object_type(w->theme_sprites) != 1)
            snprintf(w->theme_sprites, sizeof w->theme_sprites, "%s.Sprites", base);
        if (w->style.icons[0])                  /* [Theme] Icons: another theme's icons (an alpha set) */
            snprintf(w->theme_sprites, sizeof w->theme_sprites, "Themes:%s.Sprites", w->style.icons);
        /* its vector icons (design 36): the set's, then its own over them */
        /* [Theme] Vectors names a set apart from the bitmaps; else the Icons set's */
        const char *vset = w->style.vectors[0] ? w->style.vectors : w->style.icons;
        if (vset[0])
            snprintf(w->theme_vectors[0], sizeof w->theme_vectors[0], "Themes:%s.SpritesV", vset);
        snprintf(w->theme_vectors[1], sizeof w->theme_vectors[1], "%s.SpritesV", dir);
        if (!strcmp(w->theme_vectors[0], w->theme_vectors[1]))
            w->theme_vectors[1][0] = 0;
    }
    wimp_sprites_choose();                      /* the theme's Sprites, or the ROM's, as the ROM area */
    set_icon_theme(classic ? NULL : leaf);
    snprintf(w->theme, sizeof w->theme, "%s", classic ? "" : leaf);
    refresh();
    if (w->ntasks)
        announce(w->theme);
    return NULL;
}

/* *WimpThemeSet <Section.Key> <value>: one setting of the current theme
 * changed, at once (until the next *WimpTheme) */
os_error *wimp_cmd_wimpthemeset(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char name[96], value[160];
    unsigned n = 0;
    uint32_t p = tail;
    while (ros_ld8(p) == ' ')
        p++;
    while (ros_ld8(p) > ' ' && n < sizeof name - 1)
        name[n++] = (char)ros_ld8(p++);
    name[n] = 0;
    while (ros_ld8(p) == ' ')
        p++;
    n = 0;
    while (ros_ld8(p) >= ' ' && n < sizeof value - 1)
        value[n++] = (char)ros_ld8(p++);
    value[n] = 0;
    char *dot = strrchr(name, '.');
    if (!dot || !value[0])
        return ros_error(0x220, "Syntax: *WimpThemeSet <Section.Key> <value>");
    *dot = 0;
    keep_visual_flags();                        /* before a setting can change them */
    if (!style_set(&wimp_ws()->style, name, dot + 1, value))
        return ros_error(0x220, "Unknown theme setting '%s.%s', or a bad value", name, dot + 1);
    refresh();
    return NULL;
}

/* Themes$Path, if no one has set it: the ROM's themes */
void wimp_theme_init(void)
{
    struct wimp_ws *w = wimp_ws();
    static const char set[] = "If \"<Themes$Path>\" = \"\" Then Set Themes$Path "
                              "Resources:$.Resources.Themes.";
    memcpy(w->scratch, set, sizeof set);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(w->scratch);
    ros_swi(&c, XOS_CLI);
}
