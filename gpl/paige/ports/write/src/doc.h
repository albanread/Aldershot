/* doc.h -- !Write's document: a Paige pg_ref and what the desktop needs
 * to know of it.  Paige owns the text, its styles, the selection and the
 * undo; doc.c is the only file that sees Paige's headers.
 *
 * Coordinates: a document measures in points, y down from the top of
 * its first line.  The window's work area is the document scaled by
 * 5/2 (180 OS units the inch), inset by DOC_MARGIN, y negated. */
#ifndef DOC_H
#define DOC_H

#define DOC_MARGIN 48               /* OS units round the text */
#define DOC_WIDTH_PT 487            /* A4 less 3/4-inch margins */
#define DOC_WORK_W (DOC_WIDTH_PT * 5 / 2 + 2 * DOC_MARGIN)

#define FILETYPE_TEXT 0xFFF
#define FILETYPE_WRITE 0x0A0        /* Write's own documents (provisional) */
#define FILETYPE_RTF 0xC32          /* Rich Text Format (MimeMap's text/rtf) */
#define FILETYPE_HTML 0xFAF         /* HTML */

enum { DOC_DOWN, DOC_MOVE, DOC_UP };
enum { DOC_EXTEND = 1, DOC_WORDS = 2 };
enum { DOC_BOLD = 1, DOC_ITALIC = 2, DOC_UNDERLINE = 4 };
enum { DOC_LEFT, DOC_CENTRE, DOC_RIGHT, DOC_FULL };

struct doc {
    struct doc *next;
    int obj;                        /* its Toolbox Window object */
    int w;                          /* the Wimp's handle for it */
    int tools, tools_w;             /* its toolbar: the object, the Wimp's handle */
    int shown[3];                   /* what the toolbar shows: style bits, alignment, level */
    void *pg;                       /* the pg_ref */
    void *undo;                     /* the undo_ref, or 0 */
    int undo_kbd;                   /* the undo is typing's (Paige extends it) */
    char path[256];                 /* where it was loaded or saved, or "" */
    int filetype;                   /* FILETYPE_WRITE, _RTF, _HTML or _TEXT */
    int modified;
    int close_after_save;           /* DCS's Save: close once saved */
};

int doc_engine_init(void);
void doc_engine_final(void);

struct doc *doc_new(void);
void doc_free(struct doc *d);
/* 0, or an error message */
const char *doc_load(struct doc *d, const char *path, int filetype);
const char *doc_save(struct doc *d, const char *path, int filetype);

long doc_height(struct doc *d);     /* points */
/* draw what lies in the graphics window (gx0..gy1, screen OS units);
 * xoff, yoff: the screen position of the document's (0,0) */
void doc_draw(struct doc *d, int w, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1);
/* the same for the printer: the text alone, no selection; (xoff, yoff) is
 * where the document's (0,0) is in the job's workspace */
void doc_draw_print(struct doc *d, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1);
/* the document cut into pages of page_height points at lines' tops:
 * tops[0..n-1] each page's top, tops[n] the last page's foot; n */
int doc_pages(struct doc *d, long page_height, long *tops, int max);
/* the caret, points: 1 if there is one (the selection is empty) */
int doc_caret(struct doc *d, long *h, long *top, long *bottom);

void doc_click(struct doc *d, long h, long v, int verb, int mods);
int doc_key(struct doc *d, int key);       /* 1 if it was the document's */
void doc_select_all(struct doc *d);
void doc_undo(struct doc *d);
void doc_style(struct doc *d, int bits);   /* toggles; 0 is plain */
void doc_size(struct doc *d, int points);
void doc_font(struct doc *d, const char *family);
void doc_align(struct doc *d, int how);    /* the selected paragraphs: DOC_LEFT.. */
/* the selected paragraphs made Normal (0) or a heading, level 1 to 3:
 * 12 point, or 24, 18 or 14 point bold */
void doc_heading(struct doc *d, int level);
/* the selection's format, for the toolbar: the style bits on throughout
 * it, its paragraphs' alignment and heading level (-1 if mixed or none) */
void doc_format(struct doc *d, int *bits, int *align, int *level);
int doc_cut(struct doc *d);             /* 1 if there was a selection to take */
int doc_copy(struct doc *d);
void doc_paste(struct doc *d);           /* Write's own clipboard */
/* the clipboard's contents, for other applications: whether there are
 * any, their length, dropping them, and writing them as a file */
int doc_scrap_exists(void);
long doc_scrap_size(void);
void doc_scrap_drop(void);
const char *doc_scrap_save(const char *path, int filetype);
/* a file (Write's own or text) put in at the selection, replacing it */
const char *doc_insert_file(struct doc *d, const char *path, int filetype);
/* the word at (or just before) the caret, Latin-1: its length */
int doc_word(struct doc *d, char *out, int cap, long *begin, long *end);
void doc_replace(struct doc *d, long begin, long end, const char *text);
/* the next word at or after from (letters and apostrophes), Latin-1: its
 * length, 0 at the end; and the selection, and setting it */
int doc_next_word(struct doc *d, long from, char *out, int cap, long *begin, long *end);
long doc_selection(struct doc *d, long *end);
void doc_select(struct doc *d, long begin, long end);
/* where an offset is, points: its line's top and bottom */
int doc_place(struct doc *d, long off, long *h, long *top, long *bottom);
/* under each word in the graphics window right() says is wrong, a red
 * wavy line; spare_caret: not the word the caret is at the end of */
void doc_mark(struct doc *d, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1,
              int (*right)(const char *), int spare_caret);
/* for the tests: the offsets at which its lines start (a line ends where
 * the caret's top moves), up to max; their number */
int doc_lines(struct doc *d, long *starts, int max);

#endif
