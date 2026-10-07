/* main.c -- !Write, a word processor for the box: a Toolbox application
 * over the Paige engine (ports/paige).
 *
 * The objects are in !Write.Res (tools/mkwrite.py makes it): the icon
 * bar icon and its menu, the document window and its menus, SaveAs for
 * Write's own files and for text, DCS and Quit.  The Toolbox shows them
 * and raises the events below; the documents themselves are doc.c's.
 *
 *   Select on the icon        a new document
 *   a file dropped on Write   opened in a window of its own (Write's own
 *                             files, RTF, HTML and text); a Write or RTF file
 *                             double-clicked likewise (HTML's double-clicks
 *                             are the browser's)
 *   Select in a document      places the caret; drag to select, double-
 *                             click a word, Adjust extends
 *   keys                      typing; Backspace, Delete, Return, Tab, the
 *                             arrows (Shift extends left and right), Home,
 *                             Copy; ^Z undo, ^X ^C ^V, ^A select all,
 *                             ^B bold, ^U underline, ^S save
 *   the clipboard             Cut and Copy claim the desktop's clipboard
 *                             (Message_ClaimEntity); another application's
 *                             Paste is answered (Message_DataRequest, then
 *                             the DataSave protocol) with Write's own format
 *                             or text; Write's Paste asks for the clipboard
 *                             when another application holds it.  A file
 *                             dropped in a document goes in at the caret.
 *   Menu in a document        File (Save, Save as, Export RTF, Export HTML,
 *                             Export text, Print), Edit,
 *                             Style, Size, Font, Spell
 *   Spell                     the word at the caret checked against the
 *                             dictionary (spell.c), and suggestions for it
 *                             to replace it with; Next misspelling (^N)
 *                             selects the next word the dictionary has not;
 *                             Mark as you type (on at the start): a red
 *                             wavy line under every word the dictionary has
 *                             not, but the one being typed; Learn adds
 *                             the word to the user's own (Choices:Write.
 *                             Words), Forget takes it away
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "swis.h"

#include "doc.h"
#include "spell.h"

/* ---- the Toolbox's numbers (its headers' values) --------------------------------- */

#define Toolbox_CreateObject    0x44EC0
#define Toolbox_DeleteObject    0x44EC1
#define Toolbox_ShowObject      0x44EC3
#define Toolbox_HideObject      0x44EC4
#define Toolbox_ObjectMiscOp    0x44EC6
#define Toolbox_SetClientHandle 0x44EC7
#define Toolbox_GetClientHandle 0x44EC8
#define Toolbox_GetSysInfo      0x44ECE
#define Toolbox_Initialise      0x44ECF

#define EV_TOOLBOX_ERROR        0x44EC0
#define EV_AUTO_CREATED         0x44EC1
#define EV_SAVEAS_SHOW          0x82BC0
#define EV_SAVEAS_SAVE          0x82BC2
#define EV_FONTMENU_SELECTION   0x82A42
#define EV_DCS_DISCARD          0x82A81
#define EV_DCS_SAVE             0x82A82
#define EV_QUIT_QUIT            0x82A91
#define EV_PRINT_SHOW           0x82B00
#define EV_PRINT_PRINT          0x82B05
#define PrintDbox_SetPageRange  1
#define EV_MENU_SHOW            0x828C0

#define Window_GetWimpHandle    0
#define Window_SetTitle         11
#define Window_GetToolBars      19
#define Button_SetFlags         961
#define StringSet_SetSelected   898
#define EV_STRINGSET_CHANGED    0x8288E
#define SaveAs_SetFileName      3
#define SaveAs_FileSaveCompleted 12
#define Menu_SetTick            0
#define Menu_SetFade            2
#define Menu_SetEntryText       4
#define Menu_AddEntry           20
#define Menu_RemoveEntry        21

/* the application's own events (tools/mkwrite.py gives them to the objects) */
enum {
    EV_NEW = 0x100, EV_QUIT = 0x101,
    EV_UNDO = 0x110, EV_SELALL, EV_CUT, EV_COPY, EV_PASTE,
    EV_PLAIN = 0x120, EV_BOLD, EV_ITALIC, EV_UNDERLINE,
    EV_SIZE = 0x130,                /* + the size's index */
    EV_SAVE = 0x140,
    EV_SPELL_NEXT = 0x150,
    EV_SPELL_MARK = 0x151,
    EV_SPELL_LEARN = 0x152,
    EV_SUGGEST = 0x160              /* + the suggestion's index */
};
#define SPELL_STATUS 1              /* the Spell menu's status entry */
#define SPELL_MARK 2                /* its Mark as you type */
#define SPELL_LEARN 3               /* its Learn (or Forget) the word */
#define SPELL_FIRST 0x10            /* its suggestions' components, from here */
static const int sizes[] = { 8, 10, 12, 14, 18, 24, 36 };

/* The toolbar's B, I and U, painted in the Font Manager's anti-aliased
 * Trinity over tools/mksprites.py's pixel letters, in the sprites the
 * Toolbox loaded from <Write$Dir>.Sprites: each on its own ground, the
 * button's grey or (its "p" twin, the selected look) blue.  The sprites
 * are 22 pixels at 90 dpi, 44 OS units square.  Without the fonts the
 * pixel letters stay. */
#define TOOL_SIZE 44
#define TOOL_OFF_BG 0xDDDDDD00u     /* &BBGGRR00: mksprites.py's TOOL_OFF */
#define TOOL_ON_BG 0xD0A89000u      /* and TOOL_ON */
#define TOOL_INK 0x10101000u

static void tool_glyph(int area, const char *sprite, const char *font, const char *ch, int rule,
                       unsigned bg)
{
    int save[4], f = 0, w = 0, x, base = rule ? 13 : 9;
    if (_swix(OS_SpriteOp, _INR(0, 3) | _OUTR(0, 3), 256 + 60, area, sprite, 0,
              &save[0], &save[1], &save[2], &save[3]))
        return;
    if (!_swix(Font_FindFont, _INR(1, 5) | _OUT(0), font, 18 * 16, 18 * 16, 0, 0, &f)) {
        _swix(ColourTrans_SetGCOL, _INR(0, 4), bg, 0, 0, 0x80, 0);
        _swix(OS_WriteC, _IN(0), 16);                   /* CLG: the ground */
        _swix(ColourTrans_SetFontColours, _INR(0, 3), f, bg, TOOL_INK, 14);
        _swix(Font_ScanString, _INR(0, 4) | _OUT(3), f, ch, 0x100, 0x7FFFFFFF, 0x7FFFFFFF, &w);
        _swix(Font_ConverttoOS, _INR(1, 2) | _OUT(1), w, 0, &w);
        x = (TOOL_SIZE - w) / 2;
        _swix(Font_Paint, _INR(0, 4), f, ch, 0x110, x, base);
        if (rule) {
            _swix(ColourTrans_SetGCOL, _INR(0, 4), TOOL_INK, 0, 0, 0, 0);
            _swix(OS_Plot, _INR(0, 2), 4, x - 2, base - 8);
            _swix(OS_Plot, _INR(0, 2), 101, x + w + 1, base - 6);
        }
        _swix(Font_LoseFont, _IN(0), f);
    }
    _swix(OS_SpriteOp, _INR(0, 3), save[0], save[1], save[2], save[3]);
}

static void tool_glyphs(void)
{
    static const struct { const char *sprite, *font, *ch; int rule; } g[] = {
        { "tb_bold", "Trinity.Bold", "B", 0 },
        { "tb_italic", "Trinity.Medium.Italic", "I", 0 },
        { "tb_under", "Trinity.Medium", "U", 1 },
    };
    int area = 0;
    unsigned i;
    char name[16];
    /* reason 4: the sprite area (1, the Wimp's pool, if there was no file) */
    if (_swix(Toolbox_GetSysInfo, _IN(0) | _OUT(0), 4, &area) || (unsigned)area < 0x8000)
        return;
    for (i = 0; i < sizeof g / sizeof g[0]; i++) {
        tool_glyph(area, g[i].sprite, g[i].font, g[i].ch, g[i].rule, TOOL_OFF_BG);
        snprintf(name, sizeof name, "%sp", g[i].sprite);
        tool_glyph(area, name, g[i].font, g[i].ch, g[i].rule, TOOL_ON_BG);
    }
}

/* the toolbar (tools/mkwrite.py's TB_ components): it lies over the top
 * TOOLBAR_H of the window's visible area, so the text starts below it */
#define TOOLBAR_H 64
#define DOC_TOP (DOC_MARGIN + TOOLBAR_H)    /* the work area's top to the text's */
enum { TB_BOLD, TB_ITALIC, TB_UNDER, TB_LEFT, TB_CENTRE, TB_RIGHT, TB_FULL, TB_STYLE };
static const char *const tb_styles[] = { "Normal", "Heading 1", "Heading 2", "Heading 3" };

/* ---- the Wimp's ----------------------------------------------------------------------- */

#define WIMP_NULL 0
#define WIMP_REDRAW 1
#define WIMP_OPEN 2
#define WIMP_CLOSE 3
#define WIMP_CLICK 6
#define WIMP_DRAG 7
#define WIMP_KEY 8
#define WIMP_LOSE_CARET 11
#define WIMP_GAIN_CARET 12
#define WIMP_MESSAGE 17
#define WIMP_MESSAGE_RECORDED 18
#define WIMP_TOOLBOX 0x200

#define MSG_QUIT 0
#define MSG_DATALOAD 3
#define MSG_DATALOADACK 4
#define MSG_DATAOPEN 5
#define MSG_PREQUIT 8
#define MSG_DATASAVE 1
#define MSG_DATASAVEACK 2
#define MSG_CLAIMENTITY 15
#define MSG_DATAREQUEST 16

/* ---- state -------------------------------------------------------------------------------- */

static struct doc *docs;
static struct doc *focus;           /* the document with the caret */
static struct doc *dragging;        /* a selection being dragged out */
static int poll_block[64];
static int idb[6];                  /* ancestor, its component, parent, its, self, its */
static int msgs_desc[4];
static int printdbox_id;
static int saveas_id, savertf_id, savehtml_id, savetext_id, dcs_id, quit_id, spellmenu_id;
static int quitting;
static int marking = 1;             /* misspellings marked as one types */
static int task;                    /* Write's task handle */
static int own_clip;                /* the desktop's clipboard is Write's */
static int msg_block[64];           /* messages Write sends */
static struct {                     /* a paste asked of another application */
    struct doc *d;
    int ref;                        /* the DataRequest's my_ref */
} paste_req;
static struct {                     /* data coming through <Wimp$Scrap> */
    struct doc *d;                  /* into it at the caret; 0: a new document */
    int ref;                        /* our DataSaveAck's my_ref */
} incoming;
static struct {                     /* the clipboard given to another application */
    int ref;                        /* our DataSave's my_ref */
    int type;
} giving;
static int prequit_shutdown;        /* a desktop shutdown was stopped for us */
static int next_x = 200, next_y = 1100;

static void report(const char *msg)
{
    static struct { int num; char mess[252]; } e;
    e.num = 0;
    strncpy(e.mess, msg, sizeof e.mess - 1);
    _swix(Wimp_ReportError, _INR(0, 2), &e, 1, "Write");
}

static struct doc *doc_of_object(int obj)
{
    struct doc *d;
    for (d = docs; d; d = d->next)
        if (obj && (d->obj == obj || d->tools == obj))
            return d;
    return NULL;
}

static struct doc *doc_of_window(int w)
{
    struct doc *d;
    for (d = docs; d; d = d->next)
        if (d->w == w)
            return d;
    return NULL;
}

/* the document an event is about: its ancestor's, its parent's, or the
 * document's whose toolbar raised it */
static struct doc *doc_of_event(void)
{
    struct doc *d = doc_of_object(idb[0]);
    if (!d)
        d = doc_of_object(idb[2]);
    return d ? d : doc_of_object(idb[4]);       /* its toolbar's own */
}

/* ---- a document's window ------------------------------------------------------------------- */

static void set_title(struct doc *d)
{
    char t[300];
    const char *name = d->path[0] ? d->path : "<Untitled>";
    strcpy(t, name);
    if (d->modified)
        strcat(t, " *");
    _swix(Toolbox_ObjectMiscOp, _INR(0, 3), 0, d->obj, Window_SetTitle, t);
}

/* the window's visible area and scroll offsets */
static void state(struct doc *d, int *s)
{
    s[0] = d->w;
    _swix(Wimp_GetWindowState, _IN(1), s);
}

static void origin(const int *s, int *xoff, int *yoff)
{
    *xoff = s[1] - s[5] + DOC_MARGIN;
    *yoff = s[4] - s[6] - DOC_TOP;
}

static void set_caret(struct doc *d)
{
    long h, top, bottom;
    if (d != focus)
        return;
    if (doc_caret(d, &h, &top, &bottom))
        _swix(Wimp_SetCaretPosition, _INR(0, 5), d->w, -1, (int)(DOC_MARGIN + h * 5 / 2),
              (int)(-DOC_TOP - bottom * 5 / 2), (int)((bottom - top) * 5 / 2), -1);
    else                                /* a selection: the caret, unseen */
        _swix(Wimp_SetCaretPosition, _INR(0, 5), d->w, -1, 0, -DOC_TOP, 40 | (1 << 25), -1);
}

/* the toolbar as the selection is: its style buttons in when the style
 * is on throughout, the paragraph's alignment in, its style named */
static void toolbar_show(struct doc *d)
{
    int now[3], i;
    if (!d->tools)
        return;
    doc_format(d, &now[0], &now[1], &now[2]);
    if (now[0] != d->shown[0])
        for (i = 0; i < 3; i++)
            _swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, d->tools, Button_SetFlags, TB_BOLD + i, 1 << 21,
                  now[0] & (1 << i) ? 1 << 21 : 0);
    if (now[1] != d->shown[1])
        for (i = 0; i < 4; i++)
            _swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, d->tools, Button_SetFlags, TB_LEFT + i, 1 << 21,
                  now[1] == i ? 1 << 21 : 0);
    if (now[2] != d->shown[2])
        _swix(Toolbox_ObjectMiscOp, _INR(0, 4), 0, d->tools, StringSet_SetSelected, TB_STYLE,
              now[2] >= 0 ? tb_styles[now[2]] : "");
    memcpy(d->shown, now, sizeof now);
}

/* after a change: the extent, the window redrawn, the caret in view */
static void changed(struct doc *d, int scroll_to_caret)
{
    int s[9], ext[4], was = d->modified;
    long height = doc_height(d) * 5 / 2 + DOC_TOP + DOC_MARGIN, h, top, bottom;
    if (height < 1600)
        height = 1600;
    ext[0] = 0;
    ext[1] = -(int)height;
    ext[2] = DOC_WORK_W;
    ext[3] = 0;
    _swix(Wimp_SetExtent, _INR(0, 1), d->w, ext);
    state(d, s);
    if (scroll_to_caret && doc_place(d, doc_selection(d, NULL), &h, &top, &bottom)) {
        int cy0 = (int)(-DOC_TOP - bottom * 5 / 2), cy1 = (int)(-DOC_TOP - top * 5 / 2);
        int vis_h = s[4] - s[2];
        if (cy1 > s[6] - TOOLBAR_H)     /* above what the toolbar leaves */
            s[6] = cy1 + 16 + TOOLBAR_H;
        else if (cy0 < s[6] - vis_h)
            s[6] = cy0 + vis_h - 16;
        _swix(Wimp_OpenWindow, _IN(1), s);
        state(d, s);
    }
    /* the visible part, in work area units */
    _swix(Wimp_ForceRedraw, _INR(0, 4), d->w, s[5], s[6] - (s[4] - s[2]), s[5] + (s[3] - s[1]), s[6]);
    set_caret(d);
    (void)was;
    set_title(d);
    toolbar_show(d);
}

static struct doc *open_doc(const char *path, int filetype)
{
    struct doc *d = doc_new();
    int open[4];
    if (!d) {
        report("There is not enough memory for another document");
        return NULL;
    }
    if (path) {
        const char *err = doc_load(d, path, filetype);
        if (err) {
            report(err);
            doc_free(d);
            return NULL;
        }
        if (filetype == FILETYPE_TEXT)
            d->path[0] = 0;             /* text is imported: Save makes a Write file */
    }
    if (_swix(Toolbox_CreateObject, _INR(0, 1) | _OUT(0), 0, "Doc", &d->obj)) {
        report("The document window could not be made");
        doc_free(d);
        return NULL;
    }
    _swix(Toolbox_SetClientHandle, _INR(0, 2), 0, d->obj, d);
    _swix(Toolbox_ObjectMiscOp, _INR(0, 2) | _OUT(0), 0, d->obj, Window_GetWimpHandle, &d->w);
    if (!_swix(Toolbox_ObjectMiscOp, _INR(0, 2) | _OUT(1), 2, d->obj, Window_GetToolBars, &d->tools) && d->tools)
        _swix(Toolbox_ObjectMiscOp, _INR(0, 2) | _OUT(0), 0, d->tools, Window_GetWimpHandle, &d->tools_w);
    d->shown[0] = d->shown[1] = d->shown[2] = -2;   /* nothing shown yet */
    d->next = docs;
    docs = d;
    open[0] = next_x;
    open[1] = next_y;
    _swix(Toolbox_ShowObject, _INR(0, 5), 0, d->obj, 2, open, 0, -1);
    next_x = next_x >= 520 ? 200 : next_x + 64;
    next_y = next_y <= 780 ? 1100 : next_y - 64;
    focus = d;
    changed(d, 0);
    return d;
}

static void close_doc(struct doc *d)
{
    struct doc **p;
    for (p = &docs; *p; p = &(*p)->next)
        if (*p == d) {
            *p = d->next;
            break;
        }
    if (focus == d)
        focus = NULL;
    if (dragging == d)
        dragging = NULL;
    _swix(Toolbox_DeleteObject, _INR(0, 1), 0, d->obj);
    doc_free(d);
}

static int any_modified(void)
{
    struct doc *d;
    for (d = docs; d; d = d->next)
        if (d->modified)
            return 1;
    return 0;
}

static void save(struct doc *d)
{
    const char *err;
    if (!d->path[0] || d->filetype == FILETYPE_TEXT) {
        _swix(Toolbox_ShowObject, _INR(0, 5), 0, saveas_id, 0, 0, d->obj, -1);
        return;
    }
    err = doc_save(d, d->path, d->filetype);   /* Write's own, or RTF back as RTF */
    if (err)
        report(err);
    else if (d->close_after_save)
        close_doc(d);
    else
        set_title(d);
}

/* ---- spelling -------------------------------------------------------------------------------- */

/* the Spell menu as last shown: its word, where it is, what may replace it */
static struct {
    struct doc *d;
    long begin, end;
    int n;
    char word[SPELL_WORD];
    char sugg[SPELL_MAX][SPELL_WORD];
    int added;                      /* suggestion entries in the menu now */
} sp;

/* a dictionary path, canonicalised (OS_FSControl 37), opened if it is there */
static int spell_try(const char *path)
{
    static char full[512];
    int type = 0;
    if (_swix(OS_FSControl, _INR(0, 5), 37, path, full, 0, 0, sizeof full))
        return -1;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0), 17, full, &type) || type != 1)
        return -1;
    return spell_open(full);
}

/* the dictionary: Write$Dictionary (!Boot sets it, a macro over the box
 * disc's BoxDisc$Dir), else the disc's beside the application -- Apps.!Write
 * on the disc, or !Write at its root -- else one in the application */
static void spell_start(void)
{
    char path[256];
    int len = 0;
    if (!_swix(OS_ReadVarVal, _INR(0, 4) | _OUT(2), "Write$Dictionary", path, sizeof path - 1, 0, 3, &len)
        && len > 0) {
        path[len] = 0;
        if (!spell_try(path))
            return;
    }
    if (!spell_try("<Write$Dir>.^.^.Resources.Dictionaries.spelldict")
        || !spell_try("<Write$Dir>.^.Resources.Dictionaries.spelldict"))
        return;
    spell_try("<Write$Dir>.Dictionary");
}

/* a string with its <variables> expanded (OS_GSTrans) */
static const char *expand(const char *in, char *out, int size)
{
    int len = 0;
    if (_swix(OS_GSTrans, _INR(0, 2) | _OUT(2), in, out, size - 1, &len) || len >= size)
        return NULL;
    out[len] = 0;
    return out;
}

/* the user's words: Choices:Write.Words, written to <Choices$Write>.Write
 * (made if need be); with no Choices, Words in the application */
static void spell_user_start(void)
{
    static char rd[256], wr[256], dir[256];
    int type = 0;
    if (expand("<Choices$Write>", dir, sizeof dir) && dir[0]) {
        strcat(dir, ".Write");
        if (_swix(OS_File, _INR(0, 1) | _OUT(0), 17, dir, &type) || type == 0)
            _swix(OS_File, _INR(0, 4), 8, dir, 0, 0, 0);       /* the directory, made */
        strcpy(wr, dir);
        strcat(wr, ".Words");
        spell_user_load(expand("Choices:Write.Words", rd, sizeof rd) ? rd : wr, wr);
        return;
    }
    expand("<Write$Dir>.Words", wr, sizeof wr);
    spell_user_load(wr, wr);
}

static void spell_menu(int menu, struct doc *d)
{
    static char status[80];
    static char texts[SPELL_MAX][SPELL_WORD];
    int i;

    while (sp.added)
        _swix(Toolbox_ObjectMiscOp, _INR(0, 3), 0, menu, Menu_RemoveEntry, SPELL_FIRST + --sp.added);
    sp.d = d;
    sp.n = 0;
    sp.word[0] = 0;
    sp.begin = sp.end = 0;
    if (!spell_ready())
        strcpy(status, "No dictionary");
    else if (!d || !doc_word(d, sp.word, sizeof sp.word, &sp.begin, &sp.end))
        strcpy(status, "No word at the caret");
    else if (spell_check(sp.word)) {
        strcpy(status, "'");
        strcat(status, sp.word);
        strcat(status, "' is spelt right");
    } else {
        sp.n = spell_suggest(sp.word, sp.sugg, SPELL_MAX);
        strcpy(status, "'");
        strcat(status, sp.word);
        strcat(status, sp.n ? "' -- perhaps:" : "' -- no suggestions");
    }
    _swix(Toolbox_ObjectMiscOp, _INR(0, 4), 0, menu, Menu_SetEntryText, SPELL_STATUS, status);
    _swix(Toolbox_ObjectMiscOp, _INR(0, 4), 0, menu, Menu_SetFade, SPELL_STATUS, 1);
    {
        /* Learn the word -- or Forget it, when it is the user's */
        static char learn[80];
        int have = sp.d && spell_ready() && sp.word[0] && sp.begin < sp.end, mine = have && spell_user_has(sp.word);
        if (!have)
            strcpy(learn, "Learn word");
        else {
            strcpy(learn, mine ? "Forget '" : "Learn '");
            strcat(learn, sp.word);
            strcat(learn, "'");
        }
        _swix(Toolbox_ObjectMiscOp, _INR(0, 4), 0, menu, Menu_SetEntryText, SPELL_LEARN, learn);
        _swix(Toolbox_ObjectMiscOp, _INR(0, 4), 0, menu, Menu_SetFade, SPELL_LEARN,
              !have || (!mine && spell_check(sp.word)));
    }
    for (i = 0; i < sp.n; i++) {
        int entry[10];
        strcpy(texts[i], sp.sugg[i]);
        entry[0] = 0;                       /* flags */
        entry[1] = SPELL_FIRST + i;         /* component */
        entry[2] = (int)(uintptr_t)texts[i];
        entry[3] = SPELL_WORD;
        entry[4] = 0;                       /* click show */
        entry[5] = 0;                       /* submenu show */
        entry[6] = 0;                       /* submenu event */
        entry[7] = EV_SUGGEST + i;          /* click event */
        entry[8] = 0;                       /* help */
        entry[9] = 0;
        /* before Learn (flags bit 0): the status, the suggestions, then Learn */
        if (_swix(Toolbox_ObjectMiscOp, _INR(0, 4), 1, menu, Menu_AddEntry, SPELL_LEARN, entry))
            break;
        sp.added++;
    }
}

/* select the next word after the selection the dictionary has not */
static void spell_next(struct doc *d)
{
    char word[SPELL_WORD];
    long from, b, e;
    if (!spell_ready()) {
        report("Write has no dictionary (Write$Dictionary names it)");
        return;
    }
    doc_selection(d, &from);
    _swix(Hourglass_On, 0);
    while (doc_next_word(d, from, word, sizeof word, &b, &e)) {
        if (!spell_check(word)) {
            _swix(Hourglass_Off, 0);
            doc_select(d, b, e);
            changed(d, 1);
            return;
        }
        from = e;
    }
    _swix(Hourglass_Off, 0);
    report("No more words that are not in the dictionary, from the caret to the end");
}

/* ---- printing ---------------------------------------------------------------------------------- */

#define PRINT_MARGIN 54000          /* millipoints: 3/4 inch, the window's own margin */
#define MAX_PAGES 2000


static long page_tops[MAX_PAGES + 1];

/* the page's height for the document: the paper's less the margins */
static long print_page_height(long *paper_w, long *paper_h)
{
    int w = 595276, h = 841890;             /* A4, if the driver will not say */
    _swix(PDriver_PageSize, _OUTR(1, 2), &w, &h);
    *paper_w = w;
    *paper_h = h;
    return (h - 2 * PRINT_MARGIN) / 1000;
}

static int print_pages(struct doc *d)
{
    long w, h;
    return doc_pages(d, print_page_height(&w, &h), page_tops, MAX_PAGES);
}

/* the job: pages first to last (from 1), copies of each, through PDriver
 * to printer: -- the box's driver makes a PDF and sends it where *PrintTo
 * says.  Each page is one rectangle of its own workspace (OS units, y up:
 * the page from y = 0 to its height) placed at the page's
 * margins, untransformed; the driver asks for it in bands, and doc.c
 * draws each as the screen's redraw does, less the selection. */
static void print_doc(struct doc *d, int first, int last, int copies)
{
    static char title[64];
    _kernel_oserror *e;
    int handle = 0, old = 0, more = 0, rid, page, pages;
    long paper_w, paper_h, page_h;

    page_h = print_page_height(&paper_w, &paper_h);
    pages = doc_pages(d, page_h, page_tops, MAX_PAGES);
    if (first < 1)
        first = 1;
    if (last < 1 || last > pages)
        last = pages;
    if (copies < 1)
        copies = 1;
    if (first > last)
        return;
    strncpy(title, d->path[0] ? d->path : "Write document", sizeof title - 1);

    if ((e = (_kernel_oserror *)_swix(OS_Find, _INR(0, 1) | _OUT(0), 0x80, "printer:", &handle)) != NULL) {
        report(e->errmess);
        return;
    }
    if ((e = (_kernel_oserror *)_swix(PDriver_SelectJob, _INR(0, 1) | _OUT(0), handle, title, &old)) != NULL)
        goto failed;
    /* (the box's Font Manager does not yet hand Font_Paint to the driver,
     * PDriver_FontSWI, so the text is placed by the band and drawn at the
     * screen's resolution: #160) */
    _swix(Hourglass_On, 0);
    for (page = first; page <= last; page++) {
        long v0 = page_tops[page - 1], v1 = page_tops[page];
        int rect[4], matrix[4], pos[2], band[4];
        int top = (int)(v1 * 5 / 2) + 4;    /* the document's (0,0) here: the page from y = 0 up */
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = DOC_WIDTH_PT * 5 / 2 + 4;
        rect[3] = top - (int)(v0 * 5 / 2);
        matrix[0] = 0x10000, matrix[1] = 0, matrix[2] = 0, matrix[3] = 0x10000;
        pos[0] = PRINT_MARGIN;                          /* the rectangle's bottom left */
        pos[1] = (int)(paper_h - PRINT_MARGIN - (rect[3] - rect[1]) * 400);   /* OS unit: 400 mp */
        if ((e = (_kernel_oserror *)_swix(PDriver_GiveRectangle, _INR(0, 4), 0, rect, matrix, pos,
                                          (int)0xFFFFFF00u)) != NULL)
            break;
        if ((e = (_kernel_oserror *)_swix(PDriver_DrawPage, _INR(0, 3) | _OUT(0) | _OUT(2),
                                          copies, band, page, 0, &more, &rid)) != NULL)
            break;
        while (more) {
            doc_draw_print(d, 0, top, band[0], band[1], band[2], band[3]);
            if ((e = (_kernel_oserror *)_swix(PDriver_GetRectangle, _IN(1) | _OUT(0) | _OUT(2),
                                              band, &more, &rid)) != NULL)
                break;
        }
        if (e)
            break;
    }
    _swix(Hourglass_Off, 0);
    if (!e)
        e = (_kernel_oserror *)_swix(PDriver_EndJob, _IN(0), handle);
failed:
    if (e) {
        static char msg[256];
        strncpy(msg, e->errmess, sizeof msg - 1);   /* before the abort's calls reuse it */
        _swix(PDriver_AbortJob, _IN(0), handle);
        report(msg);
    }
    if (old)
        _swix(PDriver_SelectJob, _INR(0, 1), old, 0);
    _swix(OS_Find, _INR(0, 1), 0, handle);          /* closing printer: sends the job */
}

static void cut(struct doc *d);
static void copy(struct doc *d);
static void paste(struct doc *d);

/* ---- Wimp events ----------------------------------------------------------------------------- */

static void redraw(int *b)
{
    struct doc *d = doc_of_window(b[0]);
    int more = 0, xoff, yoff;
    _swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
    while (more) {
        if (d) {
            origin(b, &xoff, &yoff);
            doc_draw(d, d->w, xoff, yoff, b[7], b[8], b[9], b[10]);
            if (marking && spell_ready())
                doc_mark(d, xoff, yoff, b[7], b[8], b[9], b[10], spell_check_cached, d == focus);
        }
        _swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
    }
}

static void doc_point(struct doc *d, int mx, int my, long *h, long *v)
{
    int s[9], xoff, yoff;
    state(d, s);
    origin(s, &xoff, &yoff);
    *h = (mx - xoff) * 2 / 5;
    *v = (yoff - my) * 2 / 5;
    if (*h < 0)
        *h = 0;
    if (*v < 0)
        *v = 0;
}

/* a click on a document's toolbar: its button's component */
static void tool_click(struct doc *d, int component)
{
    switch (component) {
    case TB_BOLD: doc_style(d, DOC_BOLD); break;
    case TB_ITALIC: doc_style(d, DOC_ITALIC); break;
    case TB_UNDER: doc_style(d, DOC_UNDERLINE); break;
    case TB_LEFT: case TB_CENTRE: case TB_RIGHT: case TB_FULL:
        doc_align(d, component - TB_LEFT);
        break;
    default:
        return;
    }
    focus = d;
    _swix(Wimp_SetCaretPosition, _INR(0, 5), d->w, -1, 0, 0, 0, 0);   /* the input back to the text */
    changed(d, 0);
}

static void click(int *b)
{
    struct doc *d = doc_of_window(b[3]);
    int buttons = b[2];
    long h, v;
    if (!d) {
        for (d = docs; d; d = d->next)
            if (d->tools_w && d->tools_w == b[3]) {
                if (buttons & 0x505)        /* a click or double, not Menu */
                    tool_click(d, idb[5]);
                return;
            }
        return;
    }
    doc_point(d, b[0], b[1], &h, &v);
    focus = d;
    if (buttons == 0x400 || buttons == 0x100) {         /* click: Select, Adjust */
        int m = buttons == 0x100 ? DOC_EXTEND : 0;
        doc_click(d, h, v, DOC_DOWN, m);
        doc_click(d, h, v, DOC_UP, m);
    } else if (buttons == 4) {                          /* double: a word */
        doc_click(d, h, v, DOC_DOWN, DOC_WORDS);
        doc_click(d, h, v, DOC_UP, DOC_WORDS);
    } else if (buttons == 0x40 || buttons == 0x10) {    /* drag */
        int s[9], box[10];
        doc_click(d, h, v, DOC_DOWN, buttons == 0x10 ? DOC_EXTEND : 0);
        state(d, s);
        box[0] = d->w;
        box[1] = 7;                     /* a user drag, nothing drawn */
        box[2] = box[4] = b[0];
        box[3] = box[5] = b[1];
        box[6] = s[1];
        box[7] = s[2];
        box[8] = s[3];
        box[9] = s[4];
        _swix(Wimp_DragBox, _IN(1), box);
        dragging = d;
    }
    changed(d, 0);
}

static void drag_step(int up)
{
    int p[5];
    long h, v;
    struct doc *d = dragging;
    if (!d)
        return;
    _swix(Wimp_GetPointerInfo, _IN(1), p);
    doc_point(d, p[0], p[1], &h, &v);
    doc_click(d, h, v, up ? DOC_UP : DOC_MOVE, 0);
    if (up)
        dragging = NULL;
    changed(d, 0);
}

static void key(int *b)
{
    struct doc *d = doc_of_window(b[0]);
    int k = b[6], done = 1;
    if (!d) {
        _swix(Wimp_ProcessKey, _IN(0), k);
        return;
    }
    switch (k) {
    case 26: doc_undo(d); break;                         /* ^Z */
    case 24: cut(d); break;                              /* ^X */
    case 3: copy(d); break;                              /* ^C */
    case 22: paste(d); return;                           /* ^V */
    case 1: doc_select_all(d); break;                    /* ^A */
    case 2: doc_style(d, DOC_BOLD); break;               /* ^B */
    case 21: doc_style(d, DOC_UNDERLINE); break;         /* ^U */
    case 19: save(d); return;                            /* ^S */
    case 14: spell_next(d); return;                      /* ^N */
    case 16:                                             /* ^P */
        _swix(Toolbox_ShowObject, _INR(0, 5), 0, printdbox_id, 0, 0, d->obj, -1);
        return;
    default:
        done = doc_key(d, k);
        break;
    }
    if (!done) {
        _swix(Wimp_ProcessKey, _IN(0), k);
        return;
    }
    changed(d, 1);
}

/* ---- the desktop's clipboard ----------------------------------------------------------------- */

/* a type Write reads */
static int readable(int type)
{
    return type == FILETYPE_WRITE || type == FILETYPE_RTF || type == FILETYPE_HTML
        || type == FILETYPE_TEXT;
}

static int msg_size(const char *name)
{
    return (44 + (int)strlen(name) + 1 + 3) & ~3;
}

/* Cut or Copy took something: the clipboard is Write's, all others told */
static void clip_claim(void)
{
    int *m = msg_block;
    m[0] = 24;
    m[3] = 0;
    m[4] = MSG_CLAIMENTITY;
    m[5] = 4;                           /* the clipboard */
    _swix(Wimp_SendMessage, _INR(0, 2), 17, m, 0);
    own_clip = 1;
}

static void cut(struct doc *d)
{
    if (doc_cut(d))
        clip_claim();
}

static void copy(struct doc *d)
{
    if (doc_copy(d))
        clip_claim();
}

/* Paste: Write's own, or asked of whoever holds the clipboard */
static void paste(struct doc *d)
{
    int *m = msg_block;
    long h, top, bottom;
    if (own_clip && doc_scrap_exists()) {
        doc_paste(d);
        changed(d, 1);
        return;
    }
    doc_place(d, doc_selection(d, NULL), &h, &top, &bottom);
    m[0] = 60;
    m[3] = 0;
    m[4] = MSG_DATAREQUEST;
    m[5] = d->w;
    m[6] = -1;
    m[7] = (int)(DOC_MARGIN + h * 5 / 2);       /* where, in the window's work area */
    m[8] = (int)(-DOC_TOP - bottom * 5 / 2);
    m[9] = 4;                                   /* the clipboard's */
    m[10] = FILETYPE_WRITE;                     /* the types Write takes, best first */
    m[11] = FILETYPE_RTF;
    m[12] = FILETYPE_HTML;
    m[13] = FILETYPE_TEXT;
    m[14] = -1;
    if (!_swix(Wimp_SendMessage, _INR(0, 2), 17, m, 0)) {
        paste_req.d = d;
        paste_req.ref = m[2];
    }
}

/* another application's Paste: the clipboard's data offered (DataSave) in
 * the first of its types Write can give, else text */
static void clip_request(int *b)
{
    int *m = msg_block, i, type = FILETYPE_TEXT;
    if (!own_clip || !doc_scrap_exists() || !(b[9] & 4))
        return;
    for (i = 10; i < 60 && b[i] != -1; i++)
        if (readable(b[i])) {
            type = b[i];
            break;
        }
    m[0] = msg_size("Clipboard");
    m[3] = b[2];
    m[4] = MSG_DATASAVE;
    m[5] = b[5], m[6] = b[6], m[7] = b[7], m[8] = b[8];
    m[9] = (int)doc_scrap_size();
    m[10] = type;
    strcpy((char *)&m[11], "Clipboard");
    if (!_swix(Wimp_SendMessage, _INR(0, 2), 17, m, b[1])) {
        giving.ref = m[2];
        giving.type = type;
    }
}

/* our DataSave answered: the clipboard written where it says, and DataLoad */
static void clip_give(int *b)
{
    const char *path = (const char *)&b[11];
    const char *err = doc_scrap_save(path, giving.type);
    giving.ref = 0;
    if (err) {
        report(err);
        return;
    }
    b[3] = b[2];
    b[4] = MSG_DATALOAD;
    b[9] = (int)doc_scrap_size();
    b[10] = giving.type;
    _swix(Wimp_SendMessage, _INR(0, 2), 18, b, b[1]);
}

/* data offered to Write (the clipboard's, or a file dragged from another
 * application's Save): asked for through <Wimp$Scrap> */
static void data_offered(int *b)
{
    int *m = msg_block;
    int type = b[10];
    struct doc *into;
    if (!readable(type))
        return;
    into = b[3] && b[3] == paste_req.ref ? paste_req.d : doc_of_window(b[5]);
    paste_req.ref = 0;
    memcpy(m, b, 44);
    m[0] = msg_size("<Wimp$Scrap>");
    m[3] = b[2];
    m[4] = MSG_DATASAVEACK;
    m[9] = -1;                          /* not a safe place: the scrap file */
    strcpy((char *)&m[11], "<Wimp$Scrap>");
    if (!_swix(Wimp_SendMessage, _INR(0, 2), 17, m, b[1])) {
        incoming.d = into;
        incoming.ref = m[2];
    }
}

static int doc_alive(struct doc *d)
{
    struct doc *o;
    for (o = docs; o; o = o->next)
        if (o == d)
            return 1;
    return 0;
}

static int file_type(const char *path)
{
    int type = 0, obj = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(6), 23, path, &obj, &type) || obj != 1)
        return -1;
    return type;
}

static void message(int *b)
{
    int action = b[4];
    switch (action) {
    case MSG_QUIT:
        quitting = 1;
        break;
    case MSG_PREQUIT:
        if (any_modified()) {
            prequit_shutdown = (b[0] < 24) || !(b[5] & 1);
            b[3] = b[2];
            _swix(Wimp_SendMessage, _INR(0, 2), 19, b, b[1]);   /* no, not yet */
            _swix(Toolbox_ShowObject, _INR(0, 5), 0, quit_id, 0, 0, 0, -1);
        }
        break;
    case MSG_CLAIMENTITY:
        if (b[1] != task && (b[5] & 4)) {   /* another's clipboard now */
            own_clip = 0;
            doc_scrap_drop();
        }
        break;
    case MSG_DATAREQUEST:
        clip_request(b);
        break;
    case MSG_DATASAVE:
        data_offered(b);
        break;
    case MSG_DATASAVEACK:
        if (b[3] && b[3] == giving.ref)
            clip_give(b);
        break;
    case MSG_DATALOAD:
    case MSG_DATAOPEN: {
        int type = b[10];
        char path[212];
        int scrap = action == MSG_DATALOAD && b[3] && b[3] == incoming.ref;
        struct doc *into = scrap ? incoming.d : action == MSG_DATALOAD ? doc_of_window(b[5]) : NULL;
        if (action == MSG_DATAOPEN ? type != FILETYPE_WRITE && type != FILETYPE_RTF
                                   : !readable(type))
            break;
        strncpy(path, (const char *)&b[11], sizeof path - 1);
        path[sizeof path - 1] = 0;
        b[3] = b[2];
        b[4] = MSG_DATALOADACK;
        _swix(Wimp_SendMessage, _INR(0, 2), 17, b, b[1]);
        if (scrap)
            incoming.ref = 0;
        if (into && doc_alive(into)) {          /* in at the caret */
            const char *err = doc_insert_file(into, path, type);
            if (err)
                report(err);
            changed(into, 1);
        } else if (scrap) {                     /* a new, untitled document */
            struct doc *d = open_doc(path, type);
            if (d) {
                d->path[0] = 0;
                d->modified = 1;
                set_title(d);
            }
        } else
            open_doc(path, type);
        if (scrap)
            _swix(OS_File, _INR(0, 1), 6, path);  /* the scrap file is the receiver's to delete */
        break;
    }
    default:
        break;
    }
}

/* ---- Toolbox events ---------------------------------------------------------------------------- */

static void toolbox_event(int *b)
{
    int code = b[2];
    struct doc *d = doc_of_event();

    switch (code) {
    case EV_TOOLBOX_ERROR:
        report((const char *)&b[5]);
        return;
    case EV_AUTO_CREATED: {
        const char *name = (const char *)&b[4];
        if (!strcmp(name, "SaveAs"))
            saveas_id = idb[4];
        else if (!strcmp(name, "SaveText"))
            savetext_id = idb[4];
        else if (!strcmp(name, "SaveRTF"))
            savertf_id = idb[4];
        else if (!strcmp(name, "SaveHTML"))
            savehtml_id = idb[4];
        else if (!strcmp(name, "PrintDbox"))
            printdbox_id = idb[4];
        else if (!strcmp(name, "DCS"))
            dcs_id = idb[4];
        else if (!strcmp(name, "Quit"))
            quit_id = idb[4];
        else if (!strcmp(name, "SpellMenu"))
            spellmenu_id = idb[4];
        return;
    }
    case EV_NEW:
        open_doc(NULL, 0);
        return;
    case EV_QUIT:
        if (any_modified())
            _swix(Toolbox_ShowObject, _INR(0, 5), 0, quit_id, 0, 0, 0, -1);
        else
            quitting = 1;
        return;
    case EV_QUIT_QUIT:
        if (prequit_shutdown)
            _swix(Wimp_ProcessKey, _IN(0), 0x1FC);      /* the shutdown, again */
        quitting = 1;
        return;
    case EV_SAVEAS_SHOW:
        if (d) {
            const char *name = d->path[0] ? d->path : "Document";
            if (idb[4] == savetext_id)
                name = "Text";
            else if (idb[4] == savertf_id && !(d->path[0] && d->filetype == FILETYPE_RTF))
                name = "Document/rtf";
            else if (idb[4] == savehtml_id && !(d->path[0] && d->filetype == FILETYPE_HTML))
                name = "Page/html";
            _swix(Toolbox_ObjectMiscOp, _INR(0, 3), 0, idb[4], SaveAs_SetFileName, name);
        }
        return;
    case EV_SAVEAS_SAVE:
        if (d) {
            const char *path = (const char *)&b[4];  /* after the header */
            int type = idb[4] == savetext_id ? FILETYPE_TEXT
                     : idb[4] == savertf_id ? FILETYPE_RTF
                     : idb[4] == savehtml_id ? FILETYPE_HTML : FILETYPE_WRITE;
            const char *err = doc_save(d, path, type);
            if (err)
                report(err);
            _swix(Toolbox_ObjectMiscOp, _INR(0, 3), err ? 0 : 1, idb[4], SaveAs_FileSaveCompleted, path);
            if (!err && d->close_after_save && type != FILETYPE_TEXT)
                close_doc(d);
            else
                set_title(d);
        }
        return;
    case EV_DCS_DISCARD:
        if (d)
            close_doc(d);
        return;
    case EV_DCS_SAVE:
        if (d) {
            d->close_after_save = 1;
            save(d);
        }
        return;
    case EV_MENU_SHOW:
        if (idb[4] == spellmenu_id)
            spell_menu(idb[4], d);
        return;
    case EV_STRINGSET_CHANGED: {        /* the toolbar's style: +16 its text */
        int i;
        if (!d || idb[5] != TB_STYLE)
            return;
        for (i = 0; i < 4; i++)
            if (!strcmp((const char *)&b[4], tb_styles[i]))
                doc_heading(d, i);
        focus = d;
        _swix(Wimp_SetCaretPosition, _INR(0, 5), d->w, -1, 0, 0, 0, 0);
        changed(d, 0);
        return;
    }
    case EV_SPELL_NEXT:
        if (d)
            spell_next(d);
        return;
    case EV_PRINT_SHOW:                 /* the dialogue: the document's pages */
        if (d)
            _swix(Toolbox_ObjectMiscOp, _INR(0, 4), 0, idb[4], PrintDbox_SetPageRange, 1, print_pages(d));
        return;
    case EV_PRINT_PRINT:                /* +16 the first page (-1: all), +20 the last, +24 copies */
        if (d)
            print_doc(d, b[4], b[5], b[6]);
        return;
    case EV_SPELL_LEARN:
        if (sp.d && sp.word[0]) {
            struct doc *o;
            int bad = spell_user_has(sp.word) ? spell_user_forget(sp.word) : spell_user_learn(sp.word);
            if (bad)
                report("Write could not write its words (Choices:Write.Words)");
            for (o = docs; o; o = o->next)
                changed(o, 0);
        }
        return;
    case EV_SPELL_MARK: {
        struct doc *o;
        marking = !marking;
        _swix(Toolbox_ObjectMiscOp, _INR(0, 4), 0, spellmenu_id, Menu_SetTick, SPELL_MARK, marking);
        for (o = docs; o; o = o->next)
            changed(o, 0);
        return;
    }
    case EV_FONTMENU_SELECTION:
        if (d) {
            doc_font(d, (const char *)&b[4]);
            changed(d, 0);
        }
        return;
    default:
        break;
    }
    if (!d)
        return;
    switch (code) {
    case EV_SAVE: save(d); return;
    case EV_UNDO: doc_undo(d); break;
    case EV_SELALL: doc_select_all(d); break;
    case EV_CUT: cut(d); break;
    case EV_COPY: copy(d); break;
    case EV_PASTE: paste(d); return;
    case EV_PLAIN: doc_style(d, 0); break;
    case EV_BOLD: doc_style(d, DOC_BOLD); break;
    case EV_ITALIC: doc_style(d, DOC_ITALIC); break;
    case EV_UNDERLINE: doc_style(d, DOC_UNDERLINE); break;
    default:
        if (code >= EV_SIZE && code < EV_SIZE + (int)(sizeof sizes / sizeof sizes[0]))
            doc_size(d, sizes[code - EV_SIZE]);
        else if (code >= EV_SUGGEST && code < EV_SUGGEST + sp.n && sp.d == d)
            doc_replace(d, sp.begin, sp.end, sp.sugg[code - EV_SUGGEST]);
        else
            return;
        break;
    }
    changed(d, 0);
}

/* ---- start --------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    static const int all[] = { 0 };     /* every message, every event */
    int reason;

    if (_swix(Toolbox_Initialise, _INR(0, 6) | _OUT(1), 0, 310, all, all, "<Write$Dir>", msgs_desc, idb,
              &task)) {
        _swix(OS_Write0, _IN(0), "Write: the Toolbox would not start\r\n");
        return 1;
    }
    tool_glyphs();
    doc_engine_init();
    spell_start();
    spell_user_start();
    if (argc > 1) {
        int type = file_type(argv[1]);
        if (readable(type))
            open_doc(argv[1], type);
    }

    while (!quitting) {
        int mask = (1 << 4) | (1 << 5) | (dragging ? 0 : 1);
        _swix(Wimp_Poll, _INR(0, 1) | _OUT(0), mask, poll_block, &reason);
        switch (reason) {
        case WIMP_NULL:
            drag_step(0);
            break;
        case WIMP_REDRAW:
            redraw(poll_block);
            break;
        case WIMP_OPEN:
            _swix(Wimp_OpenWindow, _IN(1), poll_block);
            break;
        case WIMP_CLOSE: {
            struct doc *d = doc_of_window(poll_block[0]);
            if (d && d->modified)
                _swix(Toolbox_ShowObject, _INR(0, 5), 0, dcs_id, 0, 0, d->obj, -1);
            else if (d)
                close_doc(d);
            break;
        }
        case WIMP_CLICK:
            click(poll_block);
            break;
        case WIMP_DRAG:
            drag_step(1);
            break;
        case WIMP_KEY:
            key(poll_block);
            break;
        case WIMP_LOSE_CARET:
            if (focus && focus->w == poll_block[0])
                focus = NULL;
            break;
        case WIMP_GAIN_CARET:
            focus = doc_of_window(poll_block[0]);
            break;
        case WIMP_MESSAGE:
        case WIMP_MESSAGE_RECORDED:
            message(poll_block);
            break;
        case WIMP_TOOLBOX:
            toolbox_event(poll_block);
            break;
        default:
            break;
        }
    }

    while (docs)
        close_doc(docs);
    spell_close();
    doc_engine_final();
    return 0;
}
