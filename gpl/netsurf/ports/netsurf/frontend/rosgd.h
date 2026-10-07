/* rosgd.h -- NetSurf's native RISC OS front end: what its parts share
 * (design 22, section 11).  ROSGD's own; MIT licence.
 *
 *   main.c     the task: NetSurf's start, the Wimp_Poll loop, the scheduler
 *   window.c   a browser window, its toolbar pane, and their events
 *   plot.c     the page's sprite, the plotters, fonts and bitmaps
 *   fetch_url.c  http: and https: through URL_Fetcher
 *
 * Coordinates: NetSurf's are pixels, y down.  An OS unit is a pixel
 * shifted by the screen's eigen factors (ro_eigx, ro_eigy).
 */
#ifndef ROSGD_ROSGD_H
#define ROSGD_ROSGD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"

struct gui_window;
struct gui_window_table;
struct gui_download_table;
struct gui_bitmap_table;
struct gui_layout_table;
struct plotter_table;

/* main.c */
void ro_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ro_quit(void);
void ro_loaded(struct gui_window *gw);          /* a page has finished loading */
void ro_windows_gone(void);                     /* the last window has closed */
bool ro_downloads_dir(char *out, size_t max);   /* where downloads go, a Unix path */

/* window.c */
extern struct gui_window_table *ro_window_table;
void ro_window_init(const char *resources);     /* the icon bar icon, the pointers */
nserror ro_window_new(const char *url);         /* NULL: the home page */
void ro_window_redraw(uint32_t *block);
void ro_window_open(uint32_t *block);
void ro_window_close(uint32_t *block);
void ro_window_click(uint32_t *block);
void ro_window_key(uint32_t *block);
void ro_window_menu(uint32_t *block);
void ro_window_enter(uint32_t *block);
void ro_window_leave(uint32_t *block);
bool ro_window_tracking(void);                  /* the pointer is over a page */
void ro_window_track(void);                     /* a null event: follow it */
void ro_window_note(struct gui_window *g, const char *text);    /* the status line, if g is open */
void ro_window_mode_changed(void);              /* laid out again for a new mode */

/* download.c */
extern struct gui_download_table *ro_download_table;

/* plot.c */
extern int ro_eigx, ro_eigy;
void ro_mode_changed(void);
bool ro_canvas_begin(int w, int h);             /* the sprite at least w x h pixels; output to it */
void ro_canvas_end(void);                       /* output back where it was */
void ro_canvas_put(int x, int y, const int32_t *clip);  /* the sprite on the screen, its top left at (x, y),
                                                   only within clip (x0, y0, x1, y1, OS units, x1 y1 exclusive) */
extern const struct plotter_table ro_plotters;
extern struct gui_bitmap_table *ro_bitmap_table;
extern struct gui_layout_table *ro_layout_table;

#endif
