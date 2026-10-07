/* doc.c -- !Write's documents over Paige (ports/paige).
 *
 * Paige owns everything about a document: the text, its styles, the
 * selection and the undo.  This file turns the desktop's requests into
 * Paige calls and Paige's answers into what the desktop wants: the
 * height, the caret, what to draw.  Paige draws through its machine
 * layer (ports/paige/rosc/pgrosc.c) into whatever target is set there.
 *
 * Files: Write's own (FILETYPE_WRITE) are Paige's file format, written
 * and read through the machine layer's OS procs on a file handle; RTF
 * (&C32) goes through Paige's RTF codecs (PGTXR) on the same handle; text
 * (&FFF) is imported and exported a character at a time, LF in the file
 * being Paige's CR. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "PAIGE.H"
#include "PGUTILS.H"
#include "PGHLEVEL.H"
#include "DEFPROCS.H"
#include "PGTXR.H"
#include "MACHINE.H"
#include "PGTRAPS.H"

#include "kernel.h"
#include "swis.h"

#include "doc.h"

void rosc_set_target(int w, int xoff, int yoff);    /* pgrosc.c */

static pgm_globals mem_globals;
static pg_globals globals;
static int engine_up;
static pg_ref scrap = MEM_NULL;         /* Cut and Copy's, for Paste */

#define PG(d) ((pg_ref)(d)->pg)
#define TALL 0x3FFFFFF                  /* a page with no bottom */

/* ---- the engine ------------------------------------------------------------ */

int doc_engine_init(void)
{
    static const char name[] = "Trinity";
    size_t i;

    memset(&mem_globals, 0, sizeof mem_globals);
    memset(&globals, 0, sizeof globals);
    pgMemStartup(&mem_globals, 0);
    pgInit(&globals, &mem_globals);
    pgInitStandardHandlers(&globals);
    /* the default font is the Font Manager's Trinity */
    globals.def_font.name[0] = (pg_char)(sizeof name - 1);
    for (i = 0; i < sizeof name - 1; i++)
        globals.def_font.name[i + 1] = (pg_char)name[i];
    globals.def_font.name[sizeof name] = 0;
    engine_up = 1;
    return 0;
}

void doc_engine_final(void)
{
    if (!engine_up)
        return;
    if (scrap != MEM_NULL)
        pgDispose(scrap);
    scrap = MEM_NULL;
    pgShutdown(&globals);
    pgMemShutdown(&mem_globals);
    engine_up = 0;
}

/* ---- life ---------------------------------------------------------------------- */

struct doc *doc_new(void)
{
    rectangle page;
    shape_ref vis, wrap;
    struct doc *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    page.top_left.h = 0;
    page.top_left.v = 0;
    page.bot_right.h = DOC_WIDTH_PT;
    page.bot_right.v = TALL;
    vis = pgRectToShape(&mem_globals, &page);
    wrap = pgRectToShape(&mem_globals, &page);
    d->pg = (void *)pgNew(&globals, 0, vis, wrap, MEM_NULL, 0);
    pgDisposeShape(vis);                /* pgNew keeps copies */
    pgDisposeShape(wrap);
    if (!d->pg) {
        free(d);
        return NULL;
    }
    d->filetype = FILETYPE_WRITE;
    return d;
}

void doc_free(struct doc *d)
{
    if (!d)
        return;
    if (d->undo)
        pgDisposeUndo((undo_ref)d->undo);
    if (d->pg)
        pgDispose(PG(d));
    free(d);
}

/* ---- undo ---------------------------------------------------------------------- */

/* insert_ref is what the verb needs: the paste's pg_ref for undo_paste, a
 * pointer to the insertion's length for undo_insert, else nothing */
static void undo_prepare_ref(struct doc *d, short verb, void *insert_ref);

static void undo_prepare(struct doc *d, short verb)
{
    undo_prepare_ref(d, verb, NULL);
}

static void undo_prepare_ref(struct doc *d, short verb, void *insert_ref)
{
    int kbd = verb == undo_typing || verb == undo_backspace || verb == undo_fwd_delete;
    if (d->undo && (!kbd || !d->undo_kbd)) {
        pgDisposeUndo((undo_ref)d->undo);
        d->undo = 0;
    }
    /* typing hands its last undo back: Paige extends it while the run
     * goes on, and starts it again when it does not */
    d->undo = (void *)pgPrepareUndo(PG(d), verb, kbd ? d->undo : insert_ref);
    d->undo_kbd = kbd;
}

void doc_undo(struct doc *d)
{
    undo_ref redo;
    if (!d->undo)
        return;
    redo = pgUndo(PG(d), (undo_ref)d->undo, TRUE, draw_none);
    {   /* the selection set again, so its highlight is the new layout's */
        size_t b, e;
        pgGetSelection(PG(d), &b, &e);
        pgSetSelection(PG(d), b, e, 0, FALSE);
    }
    pgDisposeUndo((undo_ref)d->undo);
    d->undo = (void *)redo;             /* Undo again is redo */
    d->undo_kbd = 0;
    d->modified = 1;
}

/* ---- files ---------------------------------------------------------------------- */

static const char *file_error;      /* why file_open failed: the OS's words */

static int file_open(const char *path, int how)
{
    static char msg[252];
    _kernel_swi_regs r;
    _kernel_oserror *e;
    memset(&r, 0, sizeof r);
    r.r[0] = how;
    r.r[1] = (int)(uintptr_t)path;
    file_error = 0;
    if ((e = _kernel_swi(OS_Find, &r, &r)) != NULL) {
        strncpy(msg, e->errmess, sizeof msg - 1);
        file_error = msg;
        return 0;
    }
    return r.r[0];
}

static void file_close(int h)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    r.r[1] = h;
    _kernel_swi(OS_Find, &r, &r);
}

/* Latin-1 put in at the offset, LF as a paragraph's end, other controls
 * but tab left out: how many characters went in */
static long insert_latin(struct doc *d, const unsigned char *s, long n, long at)
{
    pg_char buf[512];
    long i, k = 0, total = 0;
    for (i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c == 10)
            c = CR_CHAR;                /* a line end is a paragraph's */
        else if (c < 32 && c != 9)
            continue;
        buf[k++] = (pg_char)c;
        if (k == (long)(sizeof buf / sizeof buf[0])) {
            pgInsert(PG(d), buf, (size_t)k, (size_t)at, data_insert_mode, 0, draw_none);
            at += k;
            total += k;
            k = 0;
        }
    }
    if (k)
        pgInsert(PG(d), buf, (size_t)k, (size_t)at, data_insert_mode, 0, draw_none);
    return total + k;
}

const char *doc_load(struct doc *d, const char *path, int filetype)
{
    int h = file_open(path, 0x4F);      /* read, no path, errors if absent */
    if (!h)
        return file_error ? file_error : "The file could not be opened";
    if (filetype == FILETYPE_WRITE) {
        size_t pos = 0;
        pg_error e = pgReadDoc(PG(d), &pos, NULL, 0, pgOSReadProc, (file_ref)(uintptr_t)h);
        file_close(h);
        if (e != NO_ERROR)
            return "This is not a Write document Write can read";
    } else if (filetype == FILETYPE_RTF || filetype == FILETYPE_HTML) {
        pg_error e = pgImportFileFromC(PG(d), filetype == FILETYPE_RTF ? pg_rtf_type : pg_html_type,
                                       0, 0, (pg_file_unit)h);
        file_close(h);
        if (e != NO_ERROR)
            return filetype == FILETYPE_RTF ? "This RTF file could not be read"
                                            : "This HTML file could not be read";
    } else {
        static unsigned char chunk[4096];
        _kernel_swi_regs r;
        long at = 0;
        for (;;) {
            memset(&r, 0, sizeof r);
            r.r[0] = 4;
            r.r[1] = h;
            r.r[2] = (int)(uintptr_t)chunk;
            r.r[3] = (int)sizeof chunk;
            if (_kernel_swi(OS_GBPB, &r, &r))
                break;
            {
                long got = (long)sizeof chunk - r.r[3];
                if (got <= 0)
                    break;
                insert_latin(d, chunk, got, at);
                at += got;
            }
        }
        file_close(h);
    }
    pgSetSelection(PG(d), 0, 0, 0, FALSE);
    strncpy(d->path, path, sizeof d->path - 1);
    d->filetype = filetype;
    d->modified = 0;
    return 0;
}

static const char *save_text(pg_ref pg, int h)
{
    static unsigned char out[1024];
    long size = pgTextSize(pg), off = 0;
    int n = 0;
    _kernel_swi_regs r;
    while (off < size) {
        text_ref tref;
        size_t len = 0, i;
        pg_char_ptr t = pgExamineText(pg, off, &tref, &len);
        if (!t || !len)
            break;
        for (i = 0; i < len && off + (long)i < size; i++) {
            unsigned c = t[i];
            out[n++] = (unsigned char)(c == CR_CHAR ? 10 : c < 0x100 ? c : '?');
            if (n == (int)sizeof out) {
                memset(&r, 0, sizeof r);
                r.r[0] = 2, r.r[1] = h, r.r[2] = (int)(uintptr_t)out, r.r[3] = n;
                if (_kernel_swi(OS_GBPB, &r, &r)) {
                    UnuseMemory(tref);
                    return "The file could not be written";
                }
                n = 0;
            }
        }
        UnuseMemory(tref);
        off += (long)len;
    }
    if (n) {
        memset(&r, 0, sizeof r);
        r.r[0] = 2, r.r[1] = h, r.r[2] = (int)(uintptr_t)out, r.r[3] = n;
        if (_kernel_swi(OS_GBPB, &r, &r))
            return "The file could not be written";
    }
    return 0;
}

/* a pg_ref written to a file of the type, Write's own or text */
static const char *save_pg(pg_ref pg, const char *path, int filetype)
{
    const char *err = 0;
    _kernel_swi_regs r;
    int h = file_open(path, 0x8F);      /* create, no path */
    if (!h)
        return file_error ? file_error : "The file could not be created";
    if (filetype == FILETYPE_WRITE) {
        size_t pos = 0;
        if (pgSaveDoc(pg, &pos, NULL, 0, pgOSWriteProc, (file_ref)(uintptr_t)h, 0) != NO_ERROR
            || pgTerminateFile(pg, &pos, pgOSWriteProc, (file_ref)(uintptr_t)h) != NO_ERROR)
            err = "The document could not be written";
    } else if (filetype == FILETYPE_RTF) {
        if (pgExportFileFromC(pg, pg_rtf_type, 0, 0, NULL, FALSE, (pg_file_unit)h) != NO_ERROR)
            err = "The RTF file could not be written";
    } else if (filetype == FILETYPE_HTML) {
        if (pgExportFileFromC(pg, pg_html_type, 0, 0, NULL, FALSE, (pg_file_unit)h) != NO_ERROR)
            err = "The HTML file could not be written";
    } else
        err = save_text(pg, h);
    file_close(h);
    memset(&r, 0, sizeof r);
    r.r[0] = 18;                        /* OS_File 18: its type */
    r.r[1] = (int)(uintptr_t)path;
    r.r[2] = filetype;
    _kernel_swi(OS_File, &r, &r);
    return err;
}

const char *doc_save(struct doc *d, const char *path, int filetype)
{
    const char *err = save_pg(PG(d), path, filetype);
    if (!err && filetype != FILETYPE_TEXT) {    /* Write's own, or RTF: the document's file */
        strncpy(d->path, path, sizeof d->path - 1);
        d->filetype = filetype;
        d->modified = 0;
    }
    return err;
}

/* ---- layout and drawing ------------------------------------------------------------ */

long doc_height(struct doc *d)
{
    return pgTotalTextHeight(PG(d), TRUE);
}

static void draw(struct doc *d, int w, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1, int selection);

void doc_draw(struct doc *d, int w, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1)
{
    draw(d, w, xoff, yoff, gx0, gy0, gx1, gy1, 1);
}

/* for the printer: the text alone, no selection */
void doc_draw_print(struct doc *d, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1)
{
    draw(d, 1, xoff, yoff, gx0, gy0, gx1, gy1, 0);
}

static void draw(struct doc *d, int w, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1, int selection)
{
    rectangle clip;
    shape_ref clip_shape;
    size_t b, e;

    /* the graphics window in points, a little over */
    clip.top_left.h = (gx0 - xoff) * 2 / 5 - 2;
    clip.bot_right.h = (gx1 - xoff) * 2 / 5 + 2;
    clip.top_left.v = (yoff - gy1) * 2 / 5 - 2;
    clip.bot_right.v = (yoff - gy0) * 2 / 5 + 2;
    if (clip.top_left.h < 0)
        clip.top_left.h = 0;
    if (clip.top_left.v < 0)
        clip.top_left.v = 0;
    clip_shape = pgRectToShape(&mem_globals, &clip);

    rosc_set_target(w, xoff, yoff);
    pgDisplay(PG(d), NULL, clip_shape, MEM_NULL, NULL, direct_or);

    /* the selection, inverted (EOR with white) */
    pgGetSelection(PG(d), &b, &e);
    if (selection && b != e) {
        shape_ref hilite;
        hilite = pgRectToShape(&mem_globals, NULL);
        if (pgGetHiliteRgn(PG(d), NULL, MEM_NULL, hilite)) {
            long n = GetMemorySize(hilite), i;
            rectangle_ptr rects = (rectangle_ptr)UseMemory(hilite);
            _swix(ColourTrans_SetGCOL, _INR(0, 4), (int)0xFFFFFF00u, 0, 0, 0, 3);
            for (i = n > 1 ? 1 : 0; i < n; i++) {
                rectangle *q = &rects[i];
                if (q->bot_right.h <= q->top_left.h || q->bot_right.v <= q->top_left.v)
                    continue;
                _swix(OS_Plot, _INR(0, 2), 4, (int)(xoff + q->top_left.h * 5 / 2),
                      (int)(yoff - q->top_left.v * 5 / 2));
                _swix(OS_Plot, _INR(0, 2), 101, (int)(xoff + q->bot_right.h * 5 / 2 - 1),
                      (int)(yoff - q->bot_right.v * 5 / 2 + 1));
            }
            UnuseMemory(hilite);
        }
        pgDisposeShape(hilite);
    }
    rosc_set_target(0, 0, 0);
    pgDisposeShape(clip_shape);
}

int doc_caret(struct doc *d, long *h, long *top, long *bottom)
{
    size_t b, e;
    rectangle r;
    pgGetSelection(PG(d), &b, &e);
    if (b != e || !pgCaretPosition(PG(d), (long)b, &r))
        return 0;
    *h = r.top_left.h;
    *top = r.top_left.v;
    *bottom = r.bot_right.v;
    if (*bottom <= *top)
        *bottom = *top + 12;
    return 1;
}

/* ---- editing ------------------------------------------------------------------------- */

void doc_click(struct doc *d, long h, long v, int verb, int mods)
{
    co_ordinate pt;
    short m = 0;
    pt.h = h;
    pt.v = v;
    if (mods & DOC_EXTEND)
        m |= EXTEND_MOD_BIT;
    if (mods & DOC_WORDS)
        m |= WORD_MOD_BIT;
    pgDragSelect(PG(d), &pt, (short)(verb == DOC_DOWN ? mouse_down : verb == DOC_MOVE ? mouse_moved : mouse_up),
                 m, 0, FALSE);
}

static void key_insert(struct doc *d, pg_char c, short mods)
{
    pgInsert(PG(d), &c, 1, CURRENT_POSITION, key_insert_mode, mods, draw_none);
}

int doc_key(struct doc *d, int key)
{
    pg_char c;
    switch (key) {
    case 8:                             /* Backspace */
        undo_prepare(d, undo_backspace);
        key_insert(d, DELETE_CHAR, 0);
        break;
    case 0x7F:                          /* Delete: forwards */
        undo_prepare(d, undo_fwd_delete);
        key_insert(d, FWD_DELETE_CHAR, 0);
        break;
    case 13:
        undo_prepare(d, undo_typing);
        key_insert(d, CR_CHAR, 0);
        break;
    case 9:
        undo_prepare(d, undo_typing);
        key_insert(d, TAB_CHAR, 0);
        break;
    case 0x18C: key_insert(d, LEFT_ARROW, 0); return 1;
    case 0x18D: key_insert(d, RIGHT_ARROW, 0); return 1;
    case 0x18E: key_insert(d, DOWN_ARROW, 0); return 1;
    case 0x18F: key_insert(d, UP_ARROW, 0); return 1;
    case 0x19C: key_insert(d, LEFT_ARROW, EXTEND_MOD_BIT); return 1;     /* Shift */
    case 0x19D: key_insert(d, RIGHT_ARROW, EXTEND_MOD_BIT); return 1;
    case 0x1E:                          /* Home: the start */
        pgSetSelection(PG(d), 0, 0, 0, FALSE);
        return 1;
    case 0x18B:                         /* Copy (End): the end */
        pgSetSelection(PG(d), (size_t)pgTextSize(PG(d)), (size_t)pgTextSize(PG(d)), 0, FALSE);
        return 1;
    default:
        if ((key >= 32 && key < 127) || (key >= 0xA0 && key <= 0xFF)) {
            undo_prepare(d, undo_typing);
            c = (pg_char)key;
            key_insert(d, c, 0);
            break;
        }
        return 0;
    }
    d->modified = 1;
    return 1;
}

void doc_select_all(struct doc *d)
{
    pgSetSelection(PG(d), 0, (size_t)pgTextSize(PG(d)), 0, FALSE);
}

void doc_style(struct doc *d, int bits)
{
    long have = 0, same = 0, want = 0, which;
    undo_prepare(d, undo_format);
    if (!bits) {
        pgSetStyleBits(PG(d), X_PLAIN_TEXT, X_ALL_STYLES, NULL, FALSE);
    } else {
        long pbits = (bits & DOC_BOLD ? X_BOLD_BIT : 0) | (bits & DOC_ITALIC ? X_ITALIC_BIT : 0)
                   | (bits & DOC_UNDERLINE ? X_UNDERLINE_BIT : 0);
        pgGetStyleBits(PG(d), &have, &same);
        which = pbits;
        want = (have & same & pbits) ? 0 : pbits;   /* all on: off; else on */
        pgSetStyleBits(PG(d), want, which, NULL, FALSE);
    }
    d->modified = 1;
}

void doc_size(struct doc *d, int points)
{
    undo_prepare(d, undo_format);
    pgSetPointSize(PG(d), (short)points, NULL, FALSE);
    d->modified = 1;
}

/* the paragraphs the selection touches: from the start of the first to
 * the end of the last */
static void selected_pars(struct doc *d, select_pair *pars)
{
    size_t b, e, first, last;
    pgGetSelection(PG(d), &b, &e);
    pgFindPar(PG(d), b, &first, NULL);
    pgFindPar(PG(d), e > b ? e - 1 : e, NULL, &last);
    pars->begin = (pg_text_offset)first;
    pars->end = (pg_text_offset)last;
}

void doc_align(struct doc *d, int how)
{
    static const short modes[] = { justify_left, justify_center, justify_right, justify_full };
    par_info info, mask;
    undo_prepare(d, undo_format);
    pgFillBlock(&info, sizeof info, 0);
    pgInitParMask(&mask, 0);
    pgGetParInfo(PG(d), NULL, FALSE, &info, &mask);
    pgInitParMask(&mask, 0);
    info.justification = modes[how];
    mask.justification = -1;
    pgSetParInfo(PG(d), NULL, &info, &mask, draw_none);
    d->modified = 1;
}

/* the heading levels' sizes, points: Normal, then 1 to 3 */
static const short heading_size[] = { 12, 24, 18, 14 };

void doc_heading(struct doc *d, int level)
{
    select_pair pars;
    size_t b, e;
    pgGetSelection(PG(d), &b, &e);
    selected_pars(d, &pars);
    /* the undo is the paragraphs', so it is made with them selected */
    pgSetSelection(PG(d), (size_t)pars.begin, (size_t)pars.end, 0, FALSE);
    undo_prepare(d, undo_format);
    pgSetPointSize(PG(d), heading_size[level], &pars, draw_none);
    /* Paige's style bits toggle what is on throughout: off, then on */
    pgSetStyleBits(PG(d), 0, X_BOLD_BIT, &pars, draw_none);
    if (level)
        pgSetStyleBits(PG(d), X_BOLD_BIT, X_BOLD_BIT, &pars, draw_none);
    pgSetSelection(PG(d), b, e, 0, FALSE);
    d->modified = 1;
}

void doc_format(struct doc *d, int *bits, int *align, int *level)
{
    long have = 0, same = 0;
    short size = 0;
    int i;
    par_info info, mask;
    pgGetStyleBits(PG(d), &have, &same);
    have &= same;
    *bits = (have & X_BOLD_BIT ? DOC_BOLD : 0) | (have & X_ITALIC_BIT ? DOC_ITALIC : 0)
          | (have & X_UNDERLINE_BIT ? DOC_UNDERLINE : 0);
    pgFillBlock(&info, sizeof info, 0);
    pgInitParMask(&mask, 0);
    pgGetParInfo(PG(d), NULL, FALSE, &info, &mask);
    switch (mask.justification ? info.justification : -1) {
    case justify_left: case force_left: *align = DOC_LEFT; break;
    case justify_center: *align = DOC_CENTRE; break;
    case justify_right: case force_right: *align = DOC_RIGHT; break;
    case justify_full: *align = DOC_FULL; break;
    default: *align = -1; break;
    }
    *level = -1;
    if (pgGetPointsize(PG(d), &size))
        for (i = 0; i < 4; i++)
            if (size == heading_size[i] && (i == 0 || (*bits & DOC_BOLD)))
                *level = i;
}

void doc_font(struct doc *d, const char *family)
{
    pg_char name[FONT_SIZE];
    int i;
    undo_prepare(d, undo_format);
    for (i = 0; family[i] && family[i] != '.' && i < FONT_SIZE - 1; i++)
        name[i] = (pg_char)(unsigned char)family[i];
    name[i] = 0;
    pgSetFontByName(PG(d), name, NULL, FALSE);
    d->modified = 1;
}

int doc_copy(struct doc *d)
{
    size_t b, e;
    pgGetSelection(PG(d), &b, &e);
    if (b == e)
        return 0;
    if (scrap != MEM_NULL)
        pgDispose(scrap);
    scrap = pgCopy(PG(d), NULL);
    return scrap != MEM_NULL;
}

int doc_cut(struct doc *d)
{
    size_t b, e;
    pgGetSelection(PG(d), &b, &e);
    if (b == e)
        return 0;
    undo_prepare(d, undo_delete);
    if (scrap != MEM_NULL)
        pgDispose(scrap);
    scrap = pgCut(PG(d), NULL, draw_none);
    d->modified = 1;
    return scrap != MEM_NULL;
}

/* ---- the clipboard's contents, for other applications ------------------------------ */

int doc_scrap_exists(void)
{
    return scrap != MEM_NULL;
}

long doc_scrap_size(void)
{
    return scrap != MEM_NULL ? pgTextSize(scrap) : 0;
}

void doc_scrap_drop(void)
{
    if (scrap != MEM_NULL)
        pgDispose(scrap);
    scrap = MEM_NULL;
}

const char *doc_scrap_save(const char *path, int filetype)
{
    if (scrap == MEM_NULL)
        return "There is nothing on the clipboard";
    return save_pg(scrap, path, filetype);
}

/* a file put in at the selection (which it replaces): text, or Write's own
 * pasted with its styles */
const char *doc_insert_file(struct doc *d, const char *path, int filetype)
{
    size_t b, e;
    pgGetSelection(PG(d), &b, &e);
    if (filetype == FILETYPE_WRITE || filetype == FILETYPE_RTF || filetype == FILETYPE_HTML) {
        struct doc *t = doc_new();
        const char *err;
        if (!t)
            return "There is not enough memory";
        if ((err = doc_load(t, path, filetype)) != NULL) {
            doc_free(t);
            return err;
        }
        undo_prepare_ref(d, undo_paste, t->pg);
        pgPaste(PG(d), PG(t), CURRENT_POSITION, FALSE, draw_none);
        doc_free(t);
    } else {
        static unsigned char chunk[4096];
        _kernel_swi_regs r;
        long at, length;
        int h = file_open(path, 0x4F);
        if (!h)
            return file_error ? file_error : "The file could not be opened";
        memset(&r, 0, sizeof r);
        r.r[0] = 2, r.r[1] = h;                     /* OS_Args 2: its extent */
        length = _kernel_swi(OS_Args, &r, &r) ? 0 : r.r[2];
        if (b != e) {
            select_pair range;
            undo_prepare(d, undo_delete);           /* the selection, replaced */
            range.begin = (long)b;
            range.end = (long)e;
            pgDelete(PG(d), &range, draw_none);
        }
        undo_prepare_ref(d, undo_insert, &length);
        at = (long)b;
        for (;;) {
            long got;
            memset(&r, 0, sizeof r);
            r.r[0] = 4, r.r[1] = h, r.r[2] = (int)(uintptr_t)chunk, r.r[3] = (int)sizeof chunk;
            if (_kernel_swi(OS_GBPB, &r, &r) || (got = (long)sizeof chunk - r.r[3]) <= 0)
                break;
            at += insert_latin(d, chunk, got, at);
        }
        file_close(h);
        pgSetSelection(PG(d), (size_t)at, (size_t)at, 0, FALSE);
    }
    d->modified = 1;
    return 0;
}

void doc_paste(struct doc *d)
{
    if (scrap == MEM_NULL)
        return;
    undo_prepare_ref(d, undo_paste, (void *)scrap);
    pgPaste(PG(d), scrap, CURRENT_POSITION, FALSE, draw_none);
    d->modified = 1;
}

void doc_replace(struct doc *d, long begin, long end, const char *text)
{
    long length = (long)strlen(text);
    pgSetSelection(PG(d), (size_t)begin, (size_t)end, 0, FALSE);
    if (end > begin) {
        select_pair range;
        range.begin = begin;
        range.end = end;
        pgDelete(PG(d), &range, draw_none);
    }
    undo_prepare_ref(d, undo_insert, &length);  /* undoes the insertion */
    insert_latin(d, (const unsigned char *)text, (long)strlen(text), begin);
    pgSetSelection(PG(d), (size_t)(begin + (long)strlen(text)), (size_t)(begin + (long)strlen(text)), 0, FALSE);
    d->modified = 1;
}


int doc_lines(struct doc *d, long *starts, int max)
{
    long size = pgTextSize(PG(d)), i, lastv = -1;
    rectangle r;
    int n = 0;
    for (i = 0; i <= size && n < max; i++) {
        pgCaretPosition(PG(d), i, &r);
        if (r.top_left.v != lastv) {
            starts[n++] = i;
            lastv = r.top_left.v;
        }
    }
    return n;
}

/* ---- words, for the spelling ---------------------------------------------------- */

static int word_char(unsigned c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xFF && c != 0xD7
                                                               && c != 0xF7) || c == '\'' || c == 0x2019;
}

int doc_next_word(struct doc *d, long from, char *out, int cap, long *begin, long *end)
{
    long size = pgTextSize(PG(d)), off = from, start = -1;
    int n = 0;
    while (off < size) {
        text_ref tref;
        size_t len = 0, i;
        pg_char_ptr t = pgExamineText(PG(d), off, &tref, &len);
        if (!t || !len)
            break;
        for (i = 0; i < len && off < size; i++, off++) {
            unsigned c = t[i];
            if (word_char(c) && !(start < 0 && (c == '\'' || c == 0x2019))) {
                if (start < 0)
                    start = off;
                if (n < cap - 1)
                    out[n++] = (char)(c == 0x2019 ? '\'' : c);
            } else if (start >= 0) {
                UnuseMemory(tref);
                goto done;
            }
        }
        UnuseMemory(tref);
    }
done:
    if (start < 0)
        return 0;
    while (n && out[n - 1] == '\'')     /* a closing quote is not the word's */
        n--, off--;
    out[n] = 0;
    *begin = start;
    *end = off;
    return n;
}

void doc_select(struct doc *d, long begin, long end)
{
    pgSetSelection(PG(d), (size_t)begin, (size_t)end, 0, FALSE);
}

long doc_selection(struct doc *d, long *end)
{
    size_t b, e;
    pgGetSelection(PG(d), &b, &e);
    if (end)
        *end = (long)e;
    return (long)b;
}

static unsigned char_at(struct doc *d, long off)
{
    text_ref tref;
    size_t len = 0;
    unsigned c = 0;
    pg_char_ptr t = pgExamineText(PG(d), off, &tref, &len);
    if (t && len) {
        c = t[0];
        UnuseMemory(tref);
    }
    return c;
}

int doc_word(struct doc *d, char *out, int cap, long *begin, long *end)
{
    long b, e, from;
    b = doc_selection(d, &e);
    from = b;
    /* a caret after punctuation or a space, with no word straight after
     * it, is the word's those follow (not over a line's end): "word.|" */
    if (b == e && !(from > 0 && word_char(char_at(d, from - 1))) && !word_char(char_at(d, from))) {
        while (from > 0 && b - from < 4 && !word_char(char_at(d, from - 1)) && char_at(d, from - 1) != CR_CHAR)
            from--;
        if (from > 0 && !word_char(char_at(d, from - 1)))
            from = b;                   /* no word there: the one after the caret */
    }
    while (from > 0 && word_char(char_at(d, from - 1)))
        from--;                         /* back to the start of the word */
    return doc_next_word(d, from, out, cap, begin, end);
}

int doc_place(struct doc *d, long off, long *h, long *top, long *bottom)
{
    rectangle r;
    pgCaretPosition(PG(d), off, &r);
    *h = r.top_left.h;
    *top = r.top_left.v;
    *bottom = r.bot_right.v > r.top_left.v ? r.bot_right.v : r.top_left.v + 12;
    return 1;
}

/* ---- misspellings marked -------------------------------------------------------------- */

/* a wavy line from x0 to x1 at y, screen OS units: up and down by 2 every 4 */
static void wavy(long x0, long x1, long y)
{
    long x;
    int up = 0;
    _swix(OS_Plot, _INR(0, 2), 4, (int)x0, (int)y);
    for (x = x0 + 4; x <= x1; x += 4) {
        up = !up;
        _swix(OS_Plot, _INR(0, 2), 5, (int)x, (int)(up ? y + 2 : y - 2));
    }
}

void doc_mark(struct doc *d, int xoff, int yoff, int gx0, int gy0, int gx1, int gy1,
              int (*right)(const char *), int spare_caret)
{
    co_ordinate pt;
    long from, b, e, sb, se, top_v = (yoff - gy1) * 2 / 5 - 24, bottom_v = (yoff - gy0) * 2 / 5 + 4;
    char word[48];
    int words = 0, coloured = 0;
    (void)gx0; (void)gx1;

    pt.h = 0;
    pt.v = top_v > 0 ? top_v : 0;
    from = (long)pgPtToChar(PG(d), &pt, NULL);
    while (from > 0 && word_char(char_at(d, from - 1)))
        from--;
    sb = doc_selection(d, &se);
    while (words++ < 3000 && doc_next_word(d, from, word, sizeof word, &b, &e)) {
        rectangle rb, re;
        from = e;
        pgCaretPosition(PG(d), b, &rb);
        if (rb.top_left.v > bottom_v)
            break;
        if (spare_caret && sb == se && se == e)
            continue;                   /* the word being typed: not yet */
        if (right(word))
            continue;
        pgCaretPosition(PG(d), e, &re);
        if (re.top_left.v != rb.top_left.v)
            re.top_left.h = rb.top_left.h + 6;      /* not on one line: a little */
        if (!coloured) {
            _swix(ColourTrans_SetGCOL, _INR(0, 4), (int)0x0000E000u, 0, 0, 0, 0);    /* red */
            coloured = 1;
        }
        {
            /* just under the baseline: a line's bottom less the baseline's
             * distance up from it (which a paragraph's space after is in) */
            rectangle rc;
            short up = pgCharacterRect(PG(d), b, FALSE, FALSE, &rc);
            long base = rc.bot_right.v - up;
            wavy(xoff + rb.top_left.h * 5 / 2, xoff + re.top_left.h * 5 / 2, yoff - base * 5 / 2 - 4);
        }
    }
}

/* ---- pages, for printing --------------------------------------------------------------- */

int doc_pages(struct doc *d, long page_height, long *tops, int max)
{
    long starts[4096];
    int lines = doc_lines(d, starts, 4096), i, n = 0;
    long page_top = 0;
    if (max < 2)
        return 0;
    tops[n++] = 0;
    for (i = 0; i < lines; i++) {
        rectangle r;
        pgCaretPosition(PG(d), starts[i], &r);
        /* a line that would cross the page's foot starts the next page,
         * unless it is the page's first (taller than a page: it is cut) */
        if (r.bot_right.v - page_top > page_height && r.top_left.v > page_top) {
            if (n >= max - 1)
                break;
            page_top = r.top_left.v;
            tops[n++] = page_top;
        }
    }
    tops[n] = doc_height(d);                /* the last page's foot */
    if (tops[n] <= tops[n - 1])
        tops[n] = tops[n - 1] + 1;
    return n;
}
