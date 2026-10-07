/* window.c -- NetSurf's native RISC OS front end: browser windows, their
 * toolbar, the icon bar icon and the menus (design 22, section 11).
 * ROSGD's own; MIT licence.
 *
 * A browser window is a Wimp window with every control; its toolbar is a
 * window nested in it, along the top of its visible area: Back, Forward,
 * Stop, Reload, Home (shaded when they cannot act), the writable URL, and
 * the status line.  The page starts below the toolbar in the work area and
 * scrolls under it.
 *
 * Redraw: each rectangle the Wimp asks for is drawn by NetSurf into the
 * page sprite (plot.c), which is then plotted there.  Select and Adjust
 * are NetSurf's buttons 1 and 2 -- but Adjust over a link downloads it
 * (download.c) -- and Menu opens the window's menu; keys go to
 * the page, else on to the Wimp.  While the pointer is over a page, it is
 * tracked on null events: NetSurf shows a link's address in the status
 * line and sets the pointer's shape (NetSurf's own pointer sprites,
 * Resources.Sprites).
 *
 * The icon bar icon opens a window on Select; its menu has New window and
 * Quit.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel.h>
#include <swis.h>

#include "utils/errors.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "desktop/browser_history.h"
#include "desktop/searchweb.h"

#include "rosgd/rosgd.h"

#define TASK     0x4B534154u        /* "TASK" */
#define TOOLBAR  112                /* the pane's height, OS units */
#define EXTENT   0x3FFFFFF
#define SHADED   (1u << 22)

enum { I_BACK, I_FORWARD, I_STOP, I_RELOAD, I_HOME, I_URL, I_STATUS, BUTTONS = I_URL };

struct gui_window {
    struct browser_window *bw;
    int w, pane;                    /* Wimp window handles */
    char title[256], url[1024], status[256];
    bool loading;
    int track_x, track_y;           /* where the pointer was last seen, page pixels */
    struct gui_window *next;
};

static struct gui_window *windows;
static struct gui_window *hover;        /* the window whose page the pointer is over */
static int iconbar = -1;                /* the icon bar icon */

static struct gui_window *find(int w)
{
    for (struct gui_window *g = windows; g; g = g->next)
        if (g->w == w || g->pane == w)
            return g;
    return NULL;
}

/* Wimp_GetWindowState: [0] handle, [1..4] visible area, [5..6] scroll */
static void state(struct gui_window *g, int32_t s[9])
{
    s[0] = g->w;
    _swix(Wimp_GetWindowState, _IN(1), s);
}

/* The page's origin: floor division, the scroll being negative */
static int shift_down(int v, int e)
{
    return v >= 0 ? v >> e : -((-v + (1 << e) - 1) >> e);
}

/* A point on the screen as page pixels in g */
static void page_xy(struct gui_window *g, int sx, int sy, int *x, int *y)
{
    int32_t s[9];
    state(g, s);
    *x = (sx - s[1] + s[5]) >> ro_eigx;
    *y = shift_down(s[4] - s[6] - sy - TOOLBAR, ro_eigy);
}

static const char *home_page(void)
{
    const char *home = nsoption_charp(homepage_url);
    return home && *home ? home : "about:welcome";
}

/* ---- pointers --------------------------------------------------------------------- */

static uint32_t *pointers;              /* NetSurf's pointer sprites: Resources.Sprites */
static enum gui_pointer_shape pointer = GUI_POINTER_DEFAULT;

/* NetSurf's RISC OS pointers and their active points, as gui_pointer_shape
 * orders them; the default is the Wimp's own, shape 1 */
static const struct { const char *name; int x, y; } shapes[] = {
    { NULL, 0, 0 }, { "ptr_point", 6, 0 }, { "ptr_caret", 4, 9 }, { "ptr_menu", 6, 4 },
    { "ptr_ud", 6, 7 }, { "ptr_ud", 6, 7 }, { "ptr_lr", 7, 6 }, { "ptr_lr", 7, 6 },
    { "ptr_ld", 7, 7 }, { "ptr_ld", 7, 7 }, { "ptr_rd", 7, 7 }, { "ptr_rd", 6, 7 },
    { "ptr_cross", 7, 7 }, { "ptr_move", 8, 0 }, { "ptr_wait", 7, 10 }, { "ptr_help", 0, 0 },
    { "ptr_nodrop", 0, 0 }, { "ptr_nt_allwd", 10, 10 }, { "ptr_progress", 0, 0 },
};

/* A sprite file as a sprite area: the file is the area without its size */
static uint32_t *load_sprites(const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint32_t *area = n > 12 ? malloc((size_t)n + 4) : NULL;
    if (area && fread(area + 1, 1, (size_t)n, f) == (size_t)n) {
        area[0] = (uint32_t)n + 4;
    } else {
        free(area);
        area = NULL;
    }
    fclose(f);
    return area;
}

static void set_pointer(enum gui_pointer_shape shape)
{
    if (shape == pointer)
        return;
    pointer = shape;
    if ((unsigned)shape >= sizeof shapes / sizeof shapes[0] || !shapes[shape].name || !pointers ||
        _swix(OS_SpriteOp, _INR(0, 7), 36 + 256, pointers, shapes[shape].name, 2, shapes[shape].x,
              shapes[shape].y, 0, 0))
        _swix(OS_Byte, _INR(0, 1), 106, 1);         /* the Wimp's pointer */
}

/* ---- opening ------------------------------------------------------------------ */

/* The window where the block says, and the toolbar along its top: nested,
 * its left and right edges on the window's, its top and bottom on its top */
static void open_at(struct gui_window *g, const int32_t *block)
{
    _swix(Wimp_OpenWindow, _IN(1), block);
    int32_t s[9];
    state(g, s);
    int32_t p[8] = { g->pane, s[1], s[4] - TOOLBAR, s[3], s[4], 0, 0, -1 };
    _swix(Wimp_OpenWindow, _INR(1, 4), p, TASK, g->w, 1 << 16 | 2 << 18 | 2 << 20 | 2 << 22);
}

static void update_extent(struct gui_window *g)
{
    int w = 0, h = 0;
    browser_window_get_extents(g->bw, true, &w, &h);
    int32_t s[9];
    state(g, s);
    int ew = w << ro_eigx, eh = (h << ro_eigy) + TOOLBAR;
    if (ew < s[3] - s[1])
        ew = s[3] - s[1];
    if (eh < s[4] - s[2])
        eh = s[4] - s[2];
    int32_t e[4] = { 0, -eh, ew, 0 };
    _swix(Wimp_SetExtent, _INR(0, 1), g->w, e);
}

static int create_window(void *title, int title_len)
{
    int32_t b[22] = {
        0, 0, 0, 0, 0, 0, -1,
        (int32_t)0xFF000002,            /* moveable; back, close, title, toggle, scroll bars, adjust */
        7 | 2 << 8 | 7 << 16 | 0xFF << 24,  /* work area not cleared: the page covers it */
        3 | 1 << 8 | 12 << 16,
        0, -EXTENT, EXTENT, 0,
        0x13D,                          /* title: text, border, centred, filled, indirected */
        3 << 12,                        /* work area: click */
        1, 0,
        (int32_t)(uintptr_t)title, -1, title_len, 0,
    };
    int handle = 0;
    return _swix(Wimp_CreateWindow, _IN(1) | _OUT(0), b, &handle) ? 0 : handle;
}

static int create_pane(void)
{
    int32_t b[22] = {
        0, 0, 0, 0, 0, 0, -1,
        /* redrawn by the Wimp; nested, so not flagged a pane -- in a nested
         * pane a writable icon is never given the caret by a click */
        (int32_t)(1u << 31 | 1 << 4),
        7 | 2 << 8 | 7 << 16 | 1 << 24,
        3 | 1 << 8 | 12 << 16,
        0, -TOOLBAR, 4096, 0,
        0, 3 << 12, 1, 0, 0, 0, 0, 0,   /* work area: click (Menu over the toolbar) */
    };
    int handle = 0;
    return _swix(Wimp_CreateWindow, _IN(1) | _OUT(0), b, &handle) ? 0 : handle;
}

static void icon(int w, int x0, int y0, int x1, int y1, uint32_t flags, const char *text, int size)
{
    int32_t b[9] = { w, x0, y0, x1, y1, (int32_t)flags, (int32_t)(uintptr_t)text, -1, size };
    int handle;
    _swix(Wimp_CreateIcon, _IN(1) | _OUT(0), b, &handle);
}

/* text, border, centred, filled, indirected; click; black on grey */
#define BUTTON 0x1700313Du
/* text, border, v-centred, filled, indirected; writable; black on white */
#define WRITABLE 0x0700F135u
/* text, v-centred, filled, indirected; black on grey */
#define DISPLAY 0x17000131u

static void toolbar(struct gui_window *g)
{
    static const char *const names[BUTTONS] = { "Back", "Forward", "Stop", "Reload", "Home" };
    for (int i = 0; i < BUTTONS; i++)
        icon(g->pane, 8 + i * 124, -56, 124 + i * 124, -4, BUTTON, names[i], (int)strlen(names[i]) + 1);
    icon(g->pane, 8 + BUTTONS * 124, -56, 4088, -4, WRITABLE, g->url, sizeof g->url);
    icon(g->pane, 8, -108, 4088, -60, DISPLAY, g->status, sizeof g->status);
}

static void shade(struct gui_window *g, int i, bool off)
{
    int32_t st[10] = { g->pane, i };
    if (_swix(Wimp_GetIconState, _IN(1), st) || ((uint32_t)st[6] & SHADED) == (off ? SHADED : 0))
        return;
    _swix(Wimp_SetIconState, _IN(1), (int32_t[]){ g->pane, i, off ? (int32_t)SHADED : 0, (int32_t)SHADED });
}

/* Back, Forward and Stop shaded when they cannot act */
static void update_buttons(struct gui_window *g)
{
    shade(g, I_BACK, !browser_window_back_available(g->bw));
    shade(g, I_FORWARD, !browser_window_forward_available(g->bw));
    shade(g, I_STOP, !g->loading);
}

/* ---- NetSurf's window table ------------------------------------------------------- */

static struct gui_window *gw_create(struct browser_window *bw, struct gui_window *existing,
                                    gui_window_create_flags flags)
{
    (void)existing, (void)flags;
    struct gui_window *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->bw = bw;
    strcpy(g->title, "NetSurf");
    g->w = create_window(g->title, sizeof g->title);
    g->pane = create_pane();
    if (!g->w || !g->pane) {
        free(g);
        return NULL;
    }
    toolbar(g);
    g->next = windows;
    windows = g;

    int xl = 0, yl = 0;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &xl);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &yl);
    int sw = (xl + 1) << ro_eigx, sh = (yl + 1) << ro_eigy, n = 0;
    for (struct gui_window *o = windows; o; o = o->next)
        n++;
    int x0 = 96 + (n - 1) * 48, x1 = x0 + 1800 < sw - 32 ? x0 + 1800 : sw - 32;
    int y1 = sh - 96 - (n - 1) * 48, y0 = y1 - 1500 > 180 ? y1 - 1500 : 180;
    int32_t open[8] = { g->w, x0, y0, x1, y1, 0, 0, -1 };
    open_at(g, open);
    update_buttons(g);
    return g;
}

static void gw_destroy(struct gui_window *g)
{
    for (struct gui_window **p = &windows; *p; p = &(*p)->next)
        if (*p == g) {
            *p = g->next;
            break;
        }
    if (hover == g) {
        hover = NULL;
        set_pointer(GUI_POINTER_DEFAULT);
    }
    _swix(Wimp_DeleteWindow, _IN(1), &g->pane);
    _swix(Wimp_DeleteWindow, _IN(1), &g->w);
    free(g);
    if (!windows)
        ro_windows_gone();
}

static nserror gw_invalidate(struct gui_window *g, const struct rect *r)
{
    if (!r)
        _swix(Wimp_ForceRedraw, _INR(0, 4), g->w, 0, -EXTENT, EXTENT, 0);
    else
        _swix(Wimp_ForceRedraw, _INR(0, 4), g->w, r->x0 << ro_eigx, -(TOOLBAR + (r->y1 << ro_eigy)),
              r->x1 << ro_eigx, -(TOOLBAR + (r->y0 << ro_eigy)));
    return NSERROR_OK;
}

static bool gw_get_scroll(struct gui_window *g, int *sx, int *sy)
{
    int32_t s[9];
    state(g, s);
    *sx = s[5] >> ro_eigx;
    *sy = -s[6] >> ro_eigy;
    return true;
}

static nserror gw_set_scroll(struct gui_window *g, const struct rect *r)
{
    int32_t s[9];
    state(g, s);
    s[5] = (r->x0 > 0 ? r->x0 : 0) << ro_eigx;
    s[6] = -((r->y0 > 0 ? r->y0 : 0) << ro_eigy);
    open_at(g, s);
    return NSERROR_OK;
}

static nserror gw_get_dimensions(struct gui_window *g, int *width, int *height)
{
    int32_t s[9];
    state(g, s);
    *width = (s[3] - s[1]) >> ro_eigx;
    *height = (s[4] - s[2] - TOOLBAR) >> ro_eigy;
    return NSERROR_OK;
}

static void set_caret_invisible(struct gui_window *g)
{
    _swix(Wimp_SetCaretPosition, _INR(0, 5), g->w, -1, 0, 0, 1 << 25, -1);
}

static nserror gw_event(struct gui_window *g, enum gui_window_event event)
{
    int32_t caret[6];
    switch (event) {
    case GW_EVENT_UPDATE_EXTENT:
        update_extent(g);
        break;
    case GW_EVENT_REMOVE_CARET:
        if (!_swix(Wimp_GetCaretPosition, _IN(1), caret) && caret[0] == g->w)
            set_caret_invisible(g);
        break;
    case GW_EVENT_START_THROBBER:
        g->loading = true;
        update_buttons(g);
        break;
    case GW_EVENT_STOP_THROBBER:
        g->loading = false;
        update_buttons(g);
        ro_loaded(g);
        break;
    case GW_EVENT_SCROLL_START: {
        struct rect top = { 0, 0, 0, 0 };
        gw_set_scroll(g, &top);
        break;
    }
    default:
        break;
    }
    return NSERROR_OK;
}

static void copy(char *to, size_t size, const char *from)
{
    size_t n = strlen(from);
    if (n >= size)
        n = size - 1;
    memcpy(to, from, n);
    to[n] = 0;
}

static void gw_set_title(struct gui_window *g, const char *title)
{
    copy(g->title, sizeof g->title, title);
    _swix(Wimp_ForceRedraw, _INR(0, 2), g->w, TASK, 3);        /* the title bar */
    ro_log("title %s", title);
}

static nserror gw_set_url(struct gui_window *g, struct nsurl *url)
{
    copy(g->url, sizeof g->url, nsurl_access(url));
    _swix(Wimp_SetIconState, _IN(1), (int32_t[]){ g->pane, I_URL, 0, 0 });
    int32_t caret[6];
    if (!_swix(Wimp_GetCaretPosition, _IN(1), caret) && caret[0] == g->pane && caret[1] == I_URL)
        _swix(Wimp_SetCaretPosition, _INR(0, 5), g->pane, I_URL, -1, -1, -1, (int)strlen(g->url));
    update_buttons(g);
    ro_log("url %s", g->url);
    return NSERROR_OK;
}

static void gw_set_status(struct gui_window *g, const char *text)
{
    if (!strcmp(g->status, text))
        return;
    copy(g->status, sizeof g->status, text);
    _swix(Wimp_SetIconState, _IN(1), (int32_t[]){ g->pane, I_STATUS, 0, 0 });
    ro_log("status %s", text);
}

/* A note in g's status line, if g is still open */
void ro_window_note(struct gui_window *g, const char *text)
{
    for (struct gui_window *o = windows; o; o = o->next)
        if (o == g)
            gw_set_status(g, text);
}

static void gw_set_pointer(struct gui_window *g, enum gui_pointer_shape shape)
{
    if (g == hover)
        set_pointer(shape);
}

static void gw_place_caret(struct gui_window *g, int x, int y, int height, const struct rect *clip)
{
    (void)clip;
    _swix(Wimp_SetCaretPosition, _INR(0, 5), g->w, -1, x << ro_eigx, -(TOOLBAR + ((y + height) << ro_eigy)),
          height << ro_eigy, -1);
}

static void gw_console_log(struct gui_window *g, browser_window_console_source src, const char *msg,
                           size_t len, browser_window_console_flags flags)
{
    (void)g, (void)src, (void)flags;
    ro_log("console %.*s", (int)len, msg);
}

static struct gui_window_table window_table = {
    .create = gw_create,
    .destroy = gw_destroy,
    .invalidate = gw_invalidate,
    .get_scroll = gw_get_scroll,
    .set_scroll = gw_set_scroll,
    .get_dimensions = gw_get_dimensions,
    .event = gw_event,
    .set_title = gw_set_title,
    .set_url = gw_set_url,
    .set_status = gw_set_status,
    .set_pointer = gw_set_pointer,
    .place_caret = gw_place_caret,
    .console_log = gw_console_log,
};
struct gui_window_table *ro_window_table = &window_table;

nserror ro_window_new(const char *text)
{
    nsurl *url;
    struct browser_window *bw;
    nserror e = search_web_omni(text ? text : home_page(), SEARCH_WEB_OMNI_NONE, &url);
    if (e != NSERROR_OK)
        return e;
    e = browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw);
    nsurl_unref(url);
    return e;
}

/* The icon bar icon, NetSurf's own sprite (IconSprites in !Run), and the
 * pointer sprites from the resources */
void ro_window_init(const char *res)
{
    char file[1024];
    snprintf(file, sizeof file, "%sSprites,ff9", res);
    pointers = load_sprites(file);
    if (!pointers) {
        snprintf(file, sizeof file, "%sSprites", res);
        pointers = load_sprites(file);
    }
    /* sprite, centred; click */
    int32_t b[9] = { -1, 0, 0, 68, 68, 0x301A, 0, 0, 0 };
    memcpy(&b[6], "!netsurf", 9);
    _swix(Wimp_CreateIcon, _IN(1) | _OUT(0), b, &iconbar);
}

/* ---- menus --------------------------------------------------------------------------- */

struct menu_item {
    uint32_t flags;             /* bit 7 the last */
    int32_t submenu;
    uint32_t icon;
    char text[12];
};

struct menu {
    char title[12];
    uint8_t colours[4];
    int32_t width, height, gap;
    struct menu_item items[6];
};

enum { M_BACK, M_FORWARD, M_RELOAD, M_STOP, M_HOME, M_NEW };
enum { MI_NEW, MI_QUIT };

#define ITEM(text, last) { (last) ? 0x80u : 0, -1, 0x07000021u, text }

static struct menu window_menu = {
    "NetSurf", { 7, 2, 7, 0 }, 240, 44, 0,
    { ITEM("Back", 0), ITEM("Forward", 0), ITEM("Reload", 0), ITEM("Stop", 0), ITEM("Home", 0),
      ITEM("New window", 1) },
};
static struct menu iconbar_menu = {
    "NetSurf", { 7, 2, 7, 0 }, 240, 44, 0,
    { ITEM("New window", 0), ITEM("Quit", 1) },
};
static struct menu *open_menu;
static struct gui_window *menu_window;

static void item_shade(struct menu *m, int i, bool off)
{
    m->items[i].icon = off ? m->items[i].icon | SHADED : m->items[i].icon & ~SHADED;
}

static void show_menu(struct menu *m, int x, int y)
{
    open_menu = m;
    _swix(Wimp_CreateMenu, _INR(1, 3), m, x, y);
}

/* [0] the selections, -1 at the end */
void ro_window_menu(uint32_t *block)
{
    const int32_t *sel = (const int32_t *)block;
    struct gui_window *g = NULL;
    for (struct gui_window *o = windows; o; o = o->next)     /* still open? */
        if (o == menu_window)
            g = o;
    if (open_menu == &iconbar_menu) {
        if (sel[0] == MI_NEW)
            ro_window_new(NULL);
        else if (sel[0] == MI_QUIT)
            ro_quit();
    } else if (open_menu == &window_menu && g) {
        switch (sel[0]) {
        case M_BACK: browser_window_history_back(g->bw, false); break;
        case M_FORWARD: browser_window_history_forward(g->bw, false); break;
        case M_RELOAD: browser_window_reload(g->bw, true); break;
        case M_STOP: browser_window_stop(g->bw); break;
        case M_HOME: {
            nsurl *url;
            if (search_web_omni(home_page(), SEARCH_WEB_OMNI_NONE, &url) == NSERROR_OK) {
                browser_window_navigate(g->bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
                nsurl_unref(url);
            }
            break;
        }
        case M_NEW: ro_window_new(NULL); break;
        default: break;
        }
    }
    /* Adjust keeps the menu open */
    int32_t p[5];
    if (open_menu && !_swix(Wimp_GetPointerInfo, _IN(1), p) && (p[2] & 1)) {
        if (open_menu == &window_menu && g) {
            item_shade(&window_menu, M_BACK, !browser_window_back_available(g->bw));
            item_shade(&window_menu, M_FORWARD, !browser_window_forward_available(g->bw));
        }
        _swix(Wimp_CreateMenu, _IN(1), open_menu);
    }
}

/* ---- the Wimp's events ---------------------------------------------------------- */

void ro_window_redraw(uint32_t *block)
{
    struct gui_window *g = find((int)block[0]);
    int more = 0;
    if (_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), block, &more))
        return;
    struct redraw_context ctx = { .interactive = true, .background_images = true, .plot = &ro_plotters };
    while (more) {
        const int32_t *b = (const int32_t *)block;
        int vx0 = b[1], vy0 = b[2], vx1 = b[3], vy1 = b[4], scx = b[5], scy = b[6];
        /* the rectangle in the sprite's pixels, whose top left is the visible area's */
        struct rect clip = { (b[7] - vx0) >> ro_eigx, (vy1 - b[10]) >> ro_eigy, (b[9] - vx0) >> ro_eigx,
                             (vy1 - b[8]) >> ro_eigy };
        if (g && ro_canvas_begin((vx1 - vx0) >> ro_eigx, (vy1 - vy0) >> ro_eigy)) {
            static const plot_style_t white = { .fill_type = PLOT_OP_TYPE_SOLID, .fill_colour = 0xFFFFFF };
            ro_plotters.clip(&ctx, &clip);
            ro_plotters.rectangle(&ctx, &white, &clip);
            browser_window_redraw(g->bw, -(scx >> ro_eigx), shift_down(scy + TOOLBAR, ro_eigy), &clip, &ctx);
            ro_canvas_end();
            ro_canvas_put(vx0, vy1, b + 7);     /* within the rectangle (b[7..10]) */
        }
        if (_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), block, &more))
            break;
    }
}

/* After a mode change: the page laid out again at the new resolution (its
 * extent follows, in the new OS units, when the layout is done) and the
 * windows drawn again from it */
void ro_window_mode_changed(void)
{
    for (struct gui_window *g = windows; g; g = g->next) {
        browser_window_schedule_reformat(g->bw);
        update_extent(g);
    }
}

void ro_window_open(uint32_t *block)
{
    struct gui_window *g = find((int)block[0]);
    if (!g || (int)block[0] == g->pane) {
        _swix(Wimp_OpenWindow, _IN(1), block);
        return;
    }
    int32_t s[9];
    state(g, s);
    const int32_t *b = (const int32_t *)block;
    bool resized = b[3] - b[1] != s[3] - s[1] || b[4] - b[2] != s[4] - s[2];
    open_at(g, b);
    if (resized)
        browser_window_schedule_reformat(g->bw);
}

void ro_window_close(uint32_t *block)
{
    struct gui_window *g = find((int)block[0]);
    if (g)
        browser_window_destroy(g->bw);          /* NetSurf calls gw_destroy */
}

static void navigate(struct gui_window *g, const char *text)
{
    nsurl *url;
    if (search_web_omni(text, SEARCH_WEB_OMNI_NONE, &url) != NSERROR_OK)
        return;
    browser_window_navigate(g->bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
    nsurl_unref(url);
}

/* [0..1] where, [2] the buttons (4 Select, 1 Adjust, 2 Menu), [3] window, [4] icon */
void ro_window_click(uint32_t *block)
{
    const int32_t *b = (const int32_t *)block;
    int32_t caret[6] = { 0 };
    _swix(Wimp_GetCaretPosition, _IN(1), caret);
    ro_log("click %d,%d buttons %d window %d icon %d (caret %d,%d)", b[0], b[1], b[2], b[3], b[4], caret[0],
           caret[1]);
    if (b[3] == -2 && b[4] == iconbar) {                /* the icon bar */
        if (b[2] == 2)
            show_menu(&iconbar_menu, b[0] - 64, 96 + 2 * 44);
        else
            ro_window_new(NULL);
        return;
    }
    struct gui_window *g = find(b[3]);
    if (!g)
        return;
    if (b[2] == 2) {                                    /* Menu: the window's */
        menu_window = g;
        item_shade(&window_menu, M_BACK, !browser_window_back_available(g->bw));
        item_shade(&window_menu, M_FORWARD, !browser_window_forward_available(g->bw));
        item_shade(&window_menu, M_STOP, !g->loading);
        show_menu(&window_menu, b[0] - 64, b[1]);
        return;
    }
    if (b[3] == g->pane) {
        switch (b[4]) {
        case I_BACK: browser_window_history_back(g->bw, false); break;
        case I_FORWARD: browser_window_history_forward(g->bw, false); break;
        case I_STOP: browser_window_stop(g->bw); break;
        case I_RELOAD: browser_window_reload(g->bw, true); break;
        case I_HOME: navigate(g, home_page()); break;
        default: break;
        }
        return;
    }
    if (b[2] != 4 && b[2] != 1)
        return;
    int x, y;
    page_xy(g, b[0], b[1], &x, &y);
    struct browser_window_features f = { 0 };
    if (b[2] == 1 && browser_window_get_features(g->bw, x, y, &f) == NSERROR_OK && f.link) {
        browser_window_navigate(g->bw, f.link, browser_window_access_url(g->bw), BW_NAVIGATE_DOWNLOAD, NULL,
                                NULL, NULL);
        return;
    }
    set_caret_invisible(g);                     /* keys come here now */
    browser_window_mouse_click(g->bw, b[2] == 4 ? BROWSER_MOUSE_PRESS_1 : BROWSER_MOUSE_PRESS_2, x, y);
    browser_window_mouse_click(g->bw, b[2] == 4 ? BROWSER_MOUSE_CLICK_1 : BROWSER_MOUSE_CLICK_2, x, y);
}

/* ---- the pointer over a page ------------------------------------------------------ */

/* [0] the window the pointer entered or left */
void ro_window_enter(uint32_t *block)
{
    struct gui_window *g = find((int)block[0]);
    if (g && (int)block[0] == g->w) {
        hover = g;
        g->track_x = g->track_y = -1;
    }
}

void ro_window_leave(uint32_t *block)
{
    struct gui_window *g = find((int)block[0]);
    if (g && g == hover && (int)block[0] == g->w) {
        hover = NULL;
        set_pointer(GUI_POINTER_DEFAULT);
        browser_window_mouse_track(g->bw, BROWSER_MOUSE_LEAVE, 0, 0);
    }
}

bool ro_window_tracking(void)
{
    return hover != NULL;
}

/* A null event while the pointer is over a page: where it is now */
void ro_window_track(void)
{
    int32_t p[5];
    if (!hover || _swix(Wimp_GetPointerInfo, _IN(1), p) || p[3] != hover->w)
        return;
    int x, y;
    page_xy(hover, p[0], p[1], &x, &y);
    if (x == hover->track_x && y == hover->track_y)
        return;
    hover->track_x = x, hover->track_y = y;
    browser_window_mouse_track(hover->bw, BROWSER_MOUSE_HOVER, x, y);
}

/* RISC OS's key codes as NetSurf's, 0 if it has none */
static uint32_t ns_key(int key)
{
    if ((key >= 32 && key < 127) || (key >= 0xA0 && key <= 0xFF))
        return (uint32_t)key;                   /* Latin-1 is Unicode's first 256 */
    switch (key) {
    case 8: case 127: return NS_KEY_DELETE_LEFT;
    case 9: return NS_KEY_TAB;
    case 13: return NS_KEY_CR;
    case 27: return NS_KEY_ESCAPE;
    case 0x1E: return NS_KEY_LINE_START;        /* Home */
    case 0x18B: return NS_KEY_LINE_END;         /* Copy */
    case 0x18C: return NS_KEY_LEFT;
    case 0x18D: return NS_KEY_RIGHT;
    case 0x18E: return NS_KEY_DOWN;
    case 0x18F: return NS_KEY_UP;
    case 0x19E: return NS_KEY_PAGE_DOWN;
    case 0x19F: return NS_KEY_PAGE_UP;
    default: return key >= 1 && key <= 26 ? (uint32_t)key : 0;     /* the Ctrl keys */
    }
}

/* [0] window, [1] icon, [6] the key */
void ro_window_key(uint32_t *block)
{
    const int32_t *b = (const int32_t *)block;
    struct gui_window *g = find(b[0]);
    int key = b[6];
    ro_log("key &%X window %d icon %d", key, b[0], b[1]);
    if (g && b[0] == g->pane && b[1] == I_URL && key == 13) {
        navigate(g, g->url);
        return;
    }
    if (g && b[0] == g->w) {
        uint32_t k = ns_key(key);
        if (k && browser_window_key_press(g->bw, k))
            return;
    }
    _swix(Wimp_ProcessKey, _IN(0), key);
}
