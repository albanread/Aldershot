/* clipboard.c -- the Clipboard Manager task.
 *
 * This is a real Wimp task.  It has its own handle, sends
 * Message_TaskInitialise, and takes its turn in the poll as a task with a
 * fast pollword.  It runs on a runtime task of its own, as a C Wimp
 * program would, and calls the Wimp's SWIs.  Its poll block, buffers and
 * pollword are in the Wimp's workspace (the RMA), so nothing of it needs a
 * slot.  It is started as RISC OS 5.30 starts it, from the first
 * Wimp_StartTask's child (task.c).  It is driven in three ways: by the
 * writable-icon code through the pollword, by Wimp_SendMessage's
 * interception, and by the parked DataLoad.
 *
 * Drags of text follow 5.30's CBTask routine by routine.  A drag out of a
 * selection uses a user drag box, and sends a Dragging message every 25 cs
 * to whatever is under the pointer.  Over a writable icon the message goes
 * to the Clipboard Manager itself, without the Wimp.  DragClaims are
 * honoured, and a DataSave is sent at the end.  A drag into a writable
 * icon is claimed with a ghost caret, and the icon is autoscrolled.
 * 5.30's quirks are kept where a task can see them. */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

#define TYPE_TEXT        0xFFFu
#define MSG_DATASAVE     1u
#define MSG_DATASAVEACK  2u
#define MSG_DATALOAD     3u
#define MSG_DATALOADACK  4u
#define MSG_RAMFETCH     6u
#define MSG_RAMTRANSMIT  7u
#define MSG_CLAIMENTITY  15u
#define MSG_DATAREQUEST  16u
#define MSG_DRAGGING     17u
#define MSG_DRAGCLAIM    18u
#define IF_INDIRECT      (1u << 8)

static struct wimp_clip *cb(void)
{
    return &wimp_ws()->clip;
}

static uint32_t rd(const uint8_t *b, unsigned off)
{
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

static void wr(uint8_t *b, unsigned off, uint32_t v)
{
    memcpy(b + off, &v, 4);
}

static int swi(uint32_t number, struct ros_cpu *c)
{
    ros_swi(c, number);
    return !c->v;
}

/* ---- the state ------------------------------------------------------------------------- */

void wimp_clipboard_reset(void)
{
    struct wimp_clip *k = cb();
    memset(&k->d, 0, sizeof k->d);
    k->state = CB_PENDING;
    k->handle = 0;
    k->pollword = 0;
    k->park_size = 0;
    k->parked = 0;
}

int wimp_clipboard_running(uint32_t *handle)
{
    struct wimp_clip *k = cb();
    if (k->state != CB_RUNNING)
        return 0;
    if (handle)
        *handle = k->handle;
    return 1;
}

/* The module title Wimp_StartTask compares by address (5.30's `Title') */
uint32_t wimp_clipboard_title(void)
{
    struct wimp_clip *k = cb();
    if (!k->title[0])
        strcpy((char *)k->title, "WindowManager");
    return ros_addr(k->title);
}

/* ---- writable icons ------------------------------------------------------------------------ */

static int writable(uint32_t window, uint32_t icon)
{
    struct wimp_window *win = window ? wimp_window(window) : NULL;
    const uint8_t *ic = win ? wimp_icon(win, icon) : NULL;
    if (!ic)
        return 0;
    uint32_t type = (rd(ic, 16) >> 12) & 15u;
    return type == 14 || type == 15;
}

/* A DataSave or Dragging of text to a writable icon goes to the Clipboard
 * Manager instead.  Returns 1 with *dest changed. */
int wimp_clipboard_intercept(const uint8_t *b, uint32_t size, uint32_t *dest)
{
    uint32_t h;
    if (size < 28 || !wimp_clipboard_running(&h))
        return 0;
    uint32_t action = rd(b, 16);
    if (action == MSG_DATASAVE) {
        if (size < 44 || rd(b, 40) != TYPE_TEXT)
            return 0;
    } else if (action == MSG_DRAGGING) {
        if (rd(b, 12) != 0)
            return 0;
        int found = 0;
        for (unsigned off = 56; off + 4 <= 256 && off + 4 <= size; off += 4) {
            uint32_t t = rd(b, off);
            if (t == 0xFFFFFFFFu)
                break;
            if (t == TYPE_TEXT)
                found = 1;
        }
        if (!found)
            return 0;
    } else {
        return 0;
    }
    if (!writable(rd(b, 20), rd(b, 24)))
        return 0;
    *dest = h;
    return 1;
}

/* Called when a DataLoad of text into a writable icon is about to be
 * delivered.  If it goes to a task other than the Clipboard Manager and the
 * sender, it is parked. */
void wimp_clipboard_park(uint32_t code, const uint8_t *b, uint32_t size, uint32_t receiver, uint32_t sender)
{
    struct wimp_clip *k = cb();
    uint32_t h;
    if ((code != 17 && code != 18) || size < 44 || size > sizeof k->park || !wimp_clipboard_running(&h))
        return;
    if (rd(b, 16) != MSG_DATALOAD || rd(b, 40) != TYPE_TEXT || !writable(rd(b, 20), rd(b, 24)))
        return;
    if (receiver == h || receiver == sender)
        return;
    memcpy(k->park, b, size);
    k->park_size = size;
    k->parked = 0;
}

/* The owner did something, so the parked DataLoad is dropped unless it
 * has already been passed on */
void wimp_clipboard_unpark(void)
{
    struct wimp_clip *k = cb();
    if (!k->parked)
        k->park_size = 0;
}

/* At every Wimp_Poll entry: a parked DataLoad is passed to the Clipboard
 * Manager through its pollword */
void wimp_clipboard_poll_entry(void)
{
    struct wimp_clip *k = cb();
    if (k->park_size && !k->parked) {
        k->parked = 1;
        k->pollword |= CB_PW_DATALOAD;
    }
}

/* The writable-icon code's requests */
void wimp_clipboard_request(uint32_t what)
{
    struct wimp_clip *k = cb();
    if (k->state == CB_RUNNING)
        k->pollword = (k->pollword & CB_PW_DATALOAD) | what;
}

/* clipboard_abort_drag: Escape during one of our drags ends the drag and
 * uses up the key (s/Wimp05:1741) */
int wimp_clipboard_escape(void)
{
    struct wimp_clip *k = cb();
    if (k->state != CB_RUNNING || !k->d.dragging)
        return 0;
    k->pollword = CB_PW_DRAGABORT;
    return 1;
}

/* clipboard_check_current_drag_op: the dragged selection's window changing
 * its selection (or going) aborts the drag, until the drag has ended */
void wimp_clipboard_sel_changed(uint32_t window)
{
    struct wimp_clip *k = cb();
    if (k->state == CB_RUNNING && k->d.dragging && !k->d.finished && k->d.src_w == window)
        k->pollword = CB_PW_DRAGABORT;
}

/* cbtask_check_abort_drag: the dragged text's window or icon deleted */
void wimp_clipboard_going(uint32_t window, int32_t icon)
{
    struct wimp_clip *k = cb();
    if (k->state == CB_RUNNING && k->d.dragging && k->d.src_w == window && (icon == -1 || k->d.src_i == icon))
        k->pollword = CB_PW_DRAGABORT;
}

/* ---- the task's own calls ------------------------------------------------------------------- */

static uint8_t *blk(void)
{
    return cb()->block;
}

static uint32_t blk_addr(void)
{
    return ros_addr(cb()->block);
}

static void send(uint32_t reason, uint32_t dest)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = reason, c.r[1] = blk_addr(), c.r[2] = dest;
    ros_swi(&c, XWimp_SendMessage);
}

/* The same with R3 (an iconbar icon).  Returns the error, or NULL. */
static os_error *send_r3(uint32_t reason, uint32_t dest, uint32_t r3)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = reason, c.r[1] = blk_addr(), c.r[2] = dest, c.r[3] = r3;
    ros_swi(&c, XWimp_SendMessage);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* A nested poll, null events masked: the reason */
static uint32_t poll_nested(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 1, c.r[1] = blk_addr();
    ros_swi(&c, XWimp_Poll);
    return c.v ? 0xFFFFFFFFu : c.r[0];
}

static void report(os_error *e)
{
    struct wimp_ws *w = wimp_ws();
    char *name = (char *)w->scratch + 400;
    strcpy(name, "Clipboard Manager");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(e), c.r[1] = 2, c.r[2] = ros_addr(name);
    ros_swi(&c, XWimp_ReportError);
}

static int grow(uint32_t size)
{
    struct wimp_clip *k = cb();
    if (size <= k->paste_cap)
        return 1;
    void *mem;
    if (xos_module_claim(size, &mem))
        return 0;
    if (k->paste) {
        memcpy(mem, ros_ptr(k->paste), k->paste_len);
        xos_module_free(ros_ptr(k->paste));
    }
    k->paste = ros_addr(mem);
    k->paste_cap = size;
    return 1;
}

static void forget_paste(void)
{
    struct wimp_clip *k = cb();
    if (k->paste)
        xos_module_free(ros_ptr(k->paste));
    k->paste = 0, k->paste_cap = 0, k->paste_len = 0;
}

/* The task handle of an icon's text: its window's owner, or a menu's */
static uint32_t text_owner(struct wimp_window *win)
{
    if (win->owner != NO_WINDOW)
        return win->owner;
    struct wimp_task *t = wimp_menu_owner();
    return t ? t->handle : 0;
}

/* cbtask_get_icon_text: an icon's text, fetched from its owner into
 * k->text.  Returns its buffer's length, or 0 if there is no such icon. */
static uint32_t fetch_text(uint32_t window, int32_t icon)
{
    struct wimp_clip *k = cb();
    struct wimp_window *win = wimp_window(window);
    uint8_t *ic = win && icon >= 0 ? wimp_icon(win, (uint32_t)icon) : NULL;
    if (!ic)
        return 0;
    uint32_t f = rd(ic, 16), cap = (f & IF_INDIRECT) ? rd(ic, 28) : 12;
    if (cap > sizeof k->text)
        cap = sizeof k->text;
    if (f & IF_INDIRECT) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = text_owner(win), c.r[1] = rd(ic, 20), c.r[2] = k->handle, c.r[3] = ros_addr(k->text), c.r[4] = cap;
        if (!swi(XWimp_TransferBlock, &c))
            return 0;
    } else {
        memcpy(k->text, ic + 20, cap);
    }
    return cap;
}

/* cbtask_put_icon_text: k->text back */
static void put_text(uint32_t window, int32_t icon, uint32_t cap)
{
    struct wimp_clip *k = cb();
    struct wimp_window *win = wimp_window(window);
    uint8_t *ic = win && icon >= 0 ? wimp_icon(win, (uint32_t)icon) : NULL;
    if (!ic)
        return;
    if (rd(ic, 16) & IF_INDIRECT) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = k->handle, c.r[1] = ros_addr(k->text), c.r[2] = text_owner(win), c.r[3] = rd(ic, 20), c.r[4] = cap;
        swi(XWimp_TransferBlock, &c);
    } else {
        memcpy(ic + 20, k->text, cap);
    }
}

static void set_caret(uint32_t window, int32_t icon, int32_t x, int32_t y, uint32_t flags, int32_t lo, int32_t hi)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = window, c.r[1] = (uint32_t)icon, c.r[2] = (uint32_t)x, c.r[3] = (uint32_t)y, c.r[4] = flags;
    c.r[5] = (uint32_t)lo, c.r[6] = (uint32_t)hi;
    ros_swi(&c, XWimp_SetCaretPosition);
}

/* The ghost caret taken away */
static void no_ghost(void)
{
    set_caret(0xFFFFFFFFu, 0, (int32_t)TASK_WORD, 0, 1u << 30, 0, 0);
}

/* Wimp_SetIconState with nothing changed: the icon redrawn */
static void redraw_icon(uint32_t window, int32_t icon)
{
    uint8_t *q = cb()->dbox;
    wr(q, 0, window), wr(q, 4, (uint32_t)icon), wr(q, 8, 0), wr(q, 12, 0);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = ros_addr(q);
    ros_swi(&c, XWimp_SetIconState);
}

/* cbtask_insert_text_into_icon: the bytes are put in at the caret, over
 * the icon's selection, as far as the buffer and the U limit allow.  They
 * are then selected, with the caret after them.  Returns 0 if they cannot
 * go in (with a beep), or if there is no room (with the caret put back). */
static int insert_text(uint32_t window, uint32_t icon, uint32_t data, uint32_t data_len)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_clip *k = cb();
    struct wimp_window *win = wimp_window(window);
    uint8_t *ic = win ? wimp_icon(win, icon) : NULL;
    if (!ic || !data)
        return 0;
    uint32_t f = rd(ic, 16), text, cap, v;
    if (f & IF_INDIRECT)
        text = rd(ic, 20), v = rd(ic, 24), cap = rd(ic, 28);
    else
        text = ros_addr(ic + 20), v = 0, cap = 12;
    /* clipboard_paste_clamp_length: up to a 0, 10 or 13 */
    uint32_t n = 0;
    uint8_t *p = ros_ptr(data);
    while (n < data_len && p[n] != 0 && p[n] != 10 && p[n] != 13)
        n++;
    /* clipboard_paste_check_validation: every byte >= 32 and allowed */
    for (uint32_t i = 0; i < n; i++)
        if (p[i] < 32)
            n = 0;
    uint8_t *vs = k->valid;
    vs[0] = 0;
    if ((f & IF_INDIRECT) && (int32_t)v > 0) {
        struct ros_cpu c;                       /* through Wimp_Extend 14, as the task would */
        ros_cpu_enter(&c);
        c.r[0] = 14, c.r[1] = window, c.r[2] = icon, c.r[3] = ros_addr(vs), c.r[4] = sizeof k->valid;
        ros_swi(&c, 0x600FBu);                  /* XWimp_Extend */
        if (c.v || (int32_t)c.r[4] < 0)
            vs[0] = 0;
    }
    if (n && vs[0] && !wimp_valid_chars(ros_addr(vs), p, n))
        n = 0;
    if (!n) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 7;
        ros_swi(&c, XOS_WriteC);
        return 0;
    }
    /* the icon's text, fetched from its owner as 5.30 does */
    uint32_t owner = text_owner(win);
    uint8_t *buf = k->text;
    if (cap > sizeof k->text)
        cap = sizeof k->text;
    struct ros_cpu c;
    if (f & IF_INDIRECT) {
        ros_cpu_enter(&c);
        c.r[0] = owner, c.r[1] = text, c.r[2] = k->handle, c.r[3] = ros_addr(buf), c.r[4] = cap;
        if (!swi(XWimp_TransferBlock, &c))
            return 0;
    } else {
        memcpy(buf, ros_ptr(text), cap);
    }
    uint32_t len = 0;
    while (len < cap && buf[len] >= 32)
        len++;
    uint32_t u = vs[0] ? wimp_valid_ulimit(ros_addr(vs)) : 0;
    uint32_t idx = w->caret.w == window && w->caret.i == (int32_t)icon && w->caret.index >= 0
                       ? (uint32_t)w->caret.index : len;
    if (idx > len)
        idx = len;
    uint32_t at;
    n = wimp_paste_text(win, (int32_t)icon, buf, cap, u, idx, p, n, &at);
    if (!n) {                                   /* no room: the caret where it was */
        set_caret(window, (int32_t)icon, 0, 0, 0xFFFFFFFFu, w->caret.index, 0);
        return 0;
    }
    if (f & IF_INDIRECT) {
        ros_cpu_enter(&c);
        c.r[0] = k->handle, c.r[1] = ros_addr(buf), c.r[2] = owner, c.r[3] = text, c.r[4] = cap;
        if (!swi(XWimp_TransferBlock, &c))
            return 0;
    } else {
        memcpy(ros_ptr(text), buf, cap);
    }
    ros_cpu_enter(&c);                          /* the pasted text selected, the caret after it */
    c.r[0] = window, c.r[1] = icon, c.r[2] = 0, c.r[3] = 0, c.r[4] = 1u << 31, c.r[5] = at, c.r[6] = at + n;
    ros_swi(&c, XWimp_SetCaretPosition);
    ros_cpu_enter(&c);
    c.r[0] = window, c.r[1] = icon, c.r[2] = 0, c.r[3] = 0, c.r[4] = 0xFFFFFFFFu, c.r[5] = at + n;
    ros_swi(&c, XWimp_SetCaretPosition);
    return 1;
}

/* cbtask_datasave_transfer_perform: the DataSave in the block is answered
 * with RAMFetch until the text is in.  Returns 0 if it went by file (the
 * DataLoad comes later) or failed. */
static int transfer(void)
{
    struct wimp_clip *k = cb();
    uint8_t *b = blk();
    if (rd(b, 40) != TYPE_TEXT)
        return 0;
    forget_paste();
    uint32_t want = rd(b, 36) + 8;
    if (!grow(want)) {
        report(ros_error(0x280, "Wimp unable to claim work area"));
        return 0;
    }
    uint32_t window = rd(b, 20), icon = rd(b, 24), got = 0;
    for (;;) {
        if (got >= k->paste_cap && !grow(k->paste_cap + 256))
            return 0;
        uint32_t room = k->paste_cap - got;
        uint32_t ref = rd(b, 8), to = rd(b, 4);
        wr(b, 0, 28);
        wr(b, 12, ref);
        wr(b, 16, MSG_RAMFETCH);
        wr(b, 20, k->paste + got);
        wr(b, 24, room);
        send(18, to);
        uint32_t r;
        do
            r = poll_nested();
        while (r != 19 && r != 17 && r != 18 && r != 0xFFFFFFFFu);
        if (r == 19) {                          /* bounced: the file way */
            wr(b, 0, 64);
            wr(b, 12, ref);
            wr(b, 16, MSG_DATASAVEACK);
            wr(b, 20, window), wr(b, 24, icon);
            wr(b, 36, 0xFFFFFFFFu);
            memset(b + 44, 0, 20);
            strcpy((char *)b + 44, "<Wimp$Scrap>");
            send(17, to);
            forget_paste();
            return 0;
        }
        if (r == 0xFFFFFFFFu || rd(b, 16) != MSG_RAMTRANSMIT)
            return 0;
        uint32_t n = rd(b, 24);
        got += n;
        k->paste_len = got;
        if (n < room)
            break;
    }
    wr(b, 20, window), wr(b, 24, icon);
    return 1;
}

/* ---- what it does --------------------------------------------------------------------------- */

/* Ctrl-V with no clipboard of the Wimp's */
static void paste(void)
{
    struct wimp_ws *w = wimp_ws();
    uint8_t *b = blk();
    memset(b, 0, 48);
    wr(b, 0, 48);
    wr(b, 16, MSG_DATAREQUEST);
    wr(b, 20, w->caret.w);
    wr(b, 24, (uint32_t)w->caret.i);
    wr(b, 36, 4);
    wr(b, 40, TYPE_TEXT);
    wr(b, 44, 0xFFFFFFFFu);
    uint32_t window = w->caret.w, icon = (uint32_t)w->caret.i;
    send(18, 0);
    for (;;) {
        uint32_t r = poll_nested();
        if (r == 19 || r == 0xFFFFFFFFu)
            return;                             /* bounced: nobody has a clipboard */
        if ((r == 17 || r == 18) && rd(b, 16) == MSG_DATASAVE)
            break;
    }
    if (transfer())
        insert_text(window, icon, cb()->paste, cb()->paste_len);
    forget_paste();
}

static void autoscroll_icon(int on)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = on ? 9u : 8u, c.r[1] = blk_addr() + 20;
    ros_swi(&c, 0x600FDu);                  /* XWimp_AutoScroll */
}

/* cbtask_dragging_x_to_work_x */
static int32_t work_x(uint32_t window, int32_t x)
{
    struct wimp_window *win = wimp_window(window);
    return win ? x + win->s[REQ].scx - win->s[REQ].vis.x0 : x;
}

/* cbtask_datasave_rx: an intercepted DataSave to a writable icon.  The
 * caret is put at the ghost caret if the icon was claimed, or else where
 * the text was dropped.  A selection that the caret is not in is removed.
 * Then the transfer is made. */
static void datasave(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    uint8_t *b = blk();
    uint32_t window = rd(b, 20), icon = rd(b, 24);
    d->claiming = 0;
    if (d->ghost) {
        d->ghost = 0;
        autoscroll_icon(0);
        int32_t gi = w->ghost.index;
        no_ghost();
        set_caret(window, (int32_t)icon, 0, 0, 0xFFFFFFFFu, gi, 0);
    } else {
        set_caret(window, (int32_t)icon, work_x(window, (int32_t)rd(b, 28)), (int32_t)rd(b, 32), 0xFFFFFFFFu, -1,
                  0);
    }
    struct wimp_window *win = wimp_window(window);
    if (!win)
        return;
    int32_t at = w->caret.index;
    if (win->sel.icon < 0 || at > win->sel.high || win->sel.low > at)
        set_caret(window, (int32_t)icon, 0, 0, 1u << 31, at, at);
    if (transfer())
        insert_text(window, icon, k->paste, k->paste_len);
    forget_paste();
}

/* cbtask_bounce_message: a parked DataLoad given back to its sender */
static void bounce_parked(void)
{
    struct wimp_clip *k = cb();
    if (!k->park_size)
        return;
    uint8_t *b = blk();
    memcpy(b, k->park, k->park_size);
    send(19, rd(b, 4));
    k->park_size = 0;
    k->parked = 0;
}

/* A DataLoad (intercepted, a file transfer, or parked) */
static void dataload(void)
{
    struct wimp_clip *k = cb();
    struct wimp_ws *w = wimp_ws();
    uint8_t *b = blk();
    uint32_t name = blk_addr() + 44;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 5, c.r[1] = name;
    if (!swi(XOS_File, &c) || c.r[0] != 1) {
        os_error *e = c.v ? ros_ptr(c.r[0]) : ros_error(0xD6, "File not found");
        bounce_parked();
        report(e);
        return;
    }
    uint32_t length = c.r[4];
    forget_paste();
    if (!grow(length + 1)) {
        bounce_parked();
        report(ros_error(0x280, "Wimp unable to claim work area"));
        return;
    }
    ros_cpu_enter(&c);
    c.r[0] = 16, c.r[1] = name, c.r[2] = k->paste, c.r[3] = 0;
    if (!swi(XOS_File, &c)) {
        os_error *e = ros_ptr(c.r[0]);
        bounce_parked();
        report(e);
        return;
    }
    k->paste_len = length;
    if (rd(b, 12) != 0) {
        ros_cpu_enter(&c);
        c.r[0] = 6, c.r[1] = name;
        ros_swi(&c, XOS_File);                  /* a scrap file: deleted */
    }
    wr(b, 12, rd(b, 8));
    wr(b, 16, MSG_DATALOADACK);
    send(17, rd(b, 4));
    k->park_size = 0;
    k->parked = 0;
    uint32_t window = rd(b, 20), icon = rd(b, 24);
    if (w->caret.w != window || w->caret.i != (int32_t)icon) {
        struct wimp_window *win = wimp_window(window);
        if (win) {
            int32_t x = (int32_t)rd(b, 28) + win->s[REQ].scx - win->s[REQ].vis.x0;
            ros_cpu_enter(&c);
            c.r[0] = window, c.r[1] = icon, c.r[2] = (uint32_t)x, c.r[3] = rd(b, 32);
            c.r[4] = 0, c.r[5] = 0xFFFFFFFFu;
            ros_swi(&c, XWimp_SetCaretPosition);
        }
    }
    insert_text(window, icon, k->paste, k->paste_len);
    forget_paste();
}

/* cbtask_claim_clipboard: after a copy or a cut */
static void claim(void)
{
    uint8_t *b = blk();
    memset(b, 0, 24);
    wr(b, 0, 24);
    wr(b, 16, MSG_CLAIMENTITY);
    wr(b, 20, 4);
    send(17, 0);
}

/* cbtask_export_finished: the end of an export.  For one of our drags, the
 * dragged text is let go.  If the text went and Shift was held, or the
 * drop was in the dragged window or on a trash can, the text is cut from
 * where it was dragged from. */
static void export_finished(int ok, int drag, os_error *e)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    if (e) {
        ok = 0;
        report(e);
    }
    if (!drag)
        return;
    if (k->drag)
        xos_module_free(ros_ptr(k->drag));
    k->drag = 0, k->drag_len = 0;
    if (!ok || (!d->shift && !d->delete_source))
        return;
    uint32_t cap = fetch_text(d->src_w, d->src_i);
    if (!cap)
        return;
    uint8_t *t = k->text;
    for (uint32_t i = (uint32_t)d->src_lo, j = (uint32_t)d->src_hi; i < cap && j < cap;) {
        uint8_t ch = t[j++];
        t[i++] = ch;
        if (ch < 32)
            break;
    }
    put_text(d->src_w, d->src_i, cap);
    struct wimp_window *win = wimp_window(d->src_w);
    if (win && w->caret.w == d->src_w && w->caret.i == d->src_i)
        set_caret(w->caret.w, w->caret.i, 0, 0, 0xFFFFFFFFu, win->sel.low, 0);
    redraw_icon(d->src_w, d->src_i);
}

/* cbtask_run_datasave: with the block's destination (+4) and your_ref
 * (+12) already set, a DataSave is sent for len bytes of text at data.
 * RAMFetch and RAMTransmit follow, or DataSaveAck and the file.  Success
 * is judged as 5.30 judges it, and passed to export_finished.  A bounce of
 * the DataSave is a failure, and anything else unforeseen is a success. */
static void run_datasave(uint32_t data, uint32_t len, int drag)
{
    struct wimp_clip *k = cb();
    uint8_t *b = blk();
    wr(b, 16, MSG_DATASAVE);
    wr(b, 36, drag ? 0xFFFFFFFFu : len);        /* a drag: 5.30 sends idle's R8, the -1 that ends its type list */
    wr(b, 40, TYPE_TEXT);
    memset(b + 44, 0, 16);
    strcpy((char *)b + 44, "Selection");
    wr(b, 0, (44 + 9 + 1 + 3) & ~3u);
    uint32_t dest = rd(b, 4), r3 = 0;
    if (dest == 0xFFFFFFFEu) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = ros_addr(k->pinfo);
        ros_swi(&c, XWimp_GetPointerInfo);
        r3 = rd(k->pinfo, 16);
    }
    os_error *e = send_r3(18, dest, r3);
    if (e) {
        export_finished(0, drag, e);
        return;
    }
    if (drag) {                                 /* the dragged window gone: backed out */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        wr(k->ws, 0, k->d.src_w);
        c.r[1] = ros_addr(k->ws);
        ros_swi(&c, XWimp_GetWindowState);
        if (c.v) {
            export_finished(0, drag, ros_ptr(c.r[0]));
            return;
        }
    }
    uint32_t r;
    for (;;) {
        r = poll_nested();
        if (r == 0xFFFFFFFFu || r == 19) {
            export_finished(0, drag, NULL);
            return;
        }
        if (r == 17 || r == 18)
            break;
    }
    if (rd(b, 16) == MSG_DATASAVEACK) {         /* the file: saved, then DataLoad */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 10, c.r[1] = blk_addr() + 44, c.r[2] = TYPE_TEXT, c.r[4] = data, c.r[5] = data + len;
        if (!swi(XOS_File, &c)) {
            export_finished(0, drag, ros_ptr(c.r[0]));
            return;
        }
        wr(b, 16, MSG_DATALOAD);
        wr(b, 12, rd(b, 8));
        wr(b, 36, len);
        send(18, rd(b, 4));
        export_finished(1, drag, NULL);
        return;
    }
    if (rd(b, 16) != MSG_RAMFETCH) {
        export_finished(1, drag, NULL);
        return;
    }
    uint32_t from = 0, left = len;
    for (;;) {                                  /* RAM: as much as each buffer takes */
        uint32_t n = rd(b, 24) < left ? rd(b, 24) : left;
        if (n) {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = k->handle, c.r[1] = data + from, c.r[2] = rd(b, 4), c.r[3] = rd(b, 20), c.r[4] = n;
            if (!swi(XWimp_TransferBlock, &c)) {
                export_finished(0, drag, ros_ptr(c.r[0]));
                return;
            }
        }
        wr(b, 0, 28);
        wr(b, 12, rd(b, 8));
        wr(b, 16, MSG_RAMTRANSMIT);
        wr(b, 24, n);
        from += n, left -= n;
        e = send_r3(n ? 18 : 17, rd(b, 4), 0);
        if (e || !n) {
            export_finished(!e, drag, e);
            return;
        }
        do
            r = poll_nested();
        while (r != 17 && r != 18 && r != 19 && r != 0xFFFFFFFFu);
        if (r == 19) {                          /* bounced: the receiver has it all */
            export_finished(1, drag, NULL);
            return;
        }
        if (r == 0xFFFFFFFFu || rd(b, 16) != MSG_RAMFETCH) {
            export_finished(0, drag, NULL);
            return;
        }
    }
}

/* A DataRequest for the clipboard, answered from the Wimp's own whatever
 * types were asked for */
static void serve(void)
{
    struct wimp_ws *w = wimp_ws();
    uint8_t *b = blk();
    if (!w->cliplen || rd(b, 36) != 4)
        return;
    wr(b, 12, rd(b, 8));
    run_datasave(w->clipdata, w->cliplen, 0);
}

/* ---- drags of text ------------------------------------------------------------------------- */

static void pointer_info(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = ros_addr(cb()->pinfo);
    ros_swi(&c, XWimp_GetPointerInfo);
}

static uint32_t pi(unsigned n)
{
    return rd(cb()->pinfo, 4 * n);
}

static void select_pointer(uint32_t n)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 106, c.r[1] = n;
    ros_swi(&c, XOS_Byte);
}

/* cbtask_set_ptr_drop: pointer shape 2, its active point at the bottom left */
static void set_ptr_drop(void)
{
    char *name = (char *)cb()->name + 20;
    strcpy(name, "ptr_drop");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 40, c.r[2] = ros_addr(name);
    ros_swi(&c, XWimp_SpriteOp);
    uint32_t h = c.r[4];
    ros_cpu_enter(&c);
    c.r[0] = 36, c.r[2] = ros_addr(name), c.r[3] = 2, c.r[4] = 0, c.r[5] = h, c.r[6] = c.r[7] = 0;
    ros_swi(&c, XWimp_SpriteOp);
    if (!c.v)
        select_pointer(2);
    strcpy((char *)cb()->name, "Clipboard Manager");
}

/* A writable icon (14 or 15) of a valid window */
static int writable_icon(uint32_t window, int32_t icon)
{
    return icon >= 0 && writable(window, (uint32_t)icon);
}

/* cbtask_redo_dragbox: the drag box again, of type 5 or 7, at the pointer */
static void redo_dragbox(uint32_t type)
{
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    uint8_t *q = k->dbox;
    wr(q, 0, 0), wr(q, 4, type);
    int32_t mx = (int32_t)pi(0), my = (int32_t)pi(1);
    wr(q, 8, (uint32_t)(d->box[0] + mx)), wr(q, 12, (uint32_t)(d->box[1] + my));
    wr(q, 16, (uint32_t)(d->box[2] + mx)), wr(q, 20, (uint32_t)(d->box[3] + my));
    for (int i = 0; i < 4; i++)
        wr(q, 24 + 4 * (unsigned)i, (uint32_t)d->parent[i]);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = ros_addr(q);
    ros_swi(&c, XWimp_DragBox);
}

/* cbtask_drag_setupbox: the selection's text as a box relative to the
 * pointer, taken from where the text was last drawn.  5.30 adds the icon's
 * left edge to an origin that already includes it, and so does this.  The
 * box's bounds let the pointer go anywhere on the screen.  Returns 0 if
 * there is nothing to drag. */
static int setup_box(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_cbdrag *d = &cb()->d;
    pointer_info();
    int32_t mx = (int32_t)pi(0), my = (int32_t)pi(1);
    d->src_w = pi(3);
    struct wimp_window *win = wimp_window(d->src_w);
    if (!win || win->sel.icon < 0)
        return 0;
    d->src_i = win->sel.icon;
    d->src_lo = win->sel.low, d->src_hi = win->sel.high;
    const uint8_t *ic = wimp_icon(win, (uint32_t)d->src_i);
    if (!ic)
        return 0;
    int32_t x0 = (int32_t)rd(ic, 0) + w->sel_origin + win->sel.xoff, y0 = (int32_t)rd(ic, 4) + 4;
    int32_t x1 = x0 + win->sel.width, y1 = (int32_t)rd(ic, 12) - 4;
    int32_t ox = wimp_origin_x(win, APP) - mx, oy = wimp_origin_y(win, APP) - my;
    struct wimp_box scr = wimp_screen_box();
    x0 += ox, x1 += ox, y0 += oy, y1 += oy;
    d->parent[0] = x0, d->parent[2] = scr.x1 - scr.x0 + x1;
    d->parent[1] = y0, d->parent[3] = scr.y1 - scr.y0 + y1;
    d->box[0] = x0, d->box[1] = y0, d->box[2] = x1, d->box[3] = y1;
    for (int i = 0; i < 4; i++)
        d->mpt[i] = d->box[i] * 400;
    return 1;
}

/* cbtask_startdrag */
static void startdrag(void)
{
    struct wimp_cbdrag *d = &cb()->d;
    d->dragging = 1;
    if (!setup_box()) {
        d->dragging = 0;
        return;
    }
    pointer_info();
    redo_dragbox(5);
    set_ptr_drop();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 129, c.r[1] = 255, c.r[2] = 255;   /* INKEY(-1): Shift */
    ros_swi(&c, XOS_Byte);
    d->shift = (c.r[1] & 0xFFu) >> 7;
    d->claimant = 0xFFFFFFFFu;
    d->finished = 0, d->aborted = 0;
    d->lastref = 0;
}

static void dragging_rx(void);
static void dragclaim_rx(void);
static void send_datasave(uint32_t dest, uint32_t yref);

/* cbtask_idle: called every 25 cs while dragging, and at the end.  It
 * sends a Dragging message to the claimant, or to the window under the
 * pointer.  Over a writable icon the message comes straight to ourselves. */
static void idle(void)
{
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    uint8_t *b = blk();
    pointer_info();
    wr(b, 0, 64);
    wr(b, 20, pi(3)), wr(b, 24, pi(4)), wr(b, 28, pi(0)), wr(b, 32, pi(1));
    wr(b, 36, 2u | (d->shift ? 8u : 0) | (d->aborted ? 16u : 0));
    for (int i = 0; i < 4; i++)
        wr(b, 40 + 4 * (unsigned)i, (uint32_t)d->mpt[i]);
    wr(b, 56, TYPE_TEXT), wr(b, 60, 0xFFFFFFFFu);
    uint32_t code, dest, yref;
    if (d->claimant != 0xFFFFFFFFu) {
        code = 18, dest = d->claimant, yref = d->lastref;
    } else {
        dest = pi(3);
        if (dest != 0xFFFFFFFEu) {
            if (!wimp_window(dest))
                return;
            if (writable_icon(dest, (int32_t)pi(4)))
                dest = k->handle;
        }
        code = 17 + (uint32_t)d->finished, yref = 0;
    }
    wr(b, 12, yref);
    wr(b, 16, MSG_DRAGGING);
    if (dest == k->handle) {
        wr(b, 4, dest);
        dragging_rx();
        return;
    }
    os_error *e = send_r3(code, dest, (uint32_t)d->mpt[0]);
    if (e)
        report(e);
}

/* cbtask_message_dragging_bounced_int: the claimant let go (or never
 * claimed, or the drag is aborting) */
static void dragging_bounced(void)
{
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    uint8_t *b = blk();
    pointer_info();
    if (d->claimant != 0xFFFFFFFFu) {
        if (!d->finished) {
            if (d->old_flags & 1u)
                set_ptr_drop();
            if (d->old_flags & 2u)
                redo_dragbox(5);
        }
        d->claimant = 0xFFFFFFFFu;
        d->lastref = 0;
        uint32_t dest = pi(3);
        int32_t icon = (int32_t)pi(4);
        wr(b, 12, 0);
        uint32_t code = 17 + (uint32_t)d->finished;
        if (dest != 0xFFFFFFFEu) {
            if (!wimp_window(dest))
                return;
            if (writable_icon(dest, icon)) {
                wr(b, 4, k->handle);
                dragging_rx();
                return;
            }
        }
        send_r3(code, dest, (uint32_t)icon);
        return;
    }
    if (d->aborted)
        return;
    d->delete_source = 0;                       /* a simple drop: DataSave to the window */
    send_datasave(pi(3), 0);
}

/* cbtask_dragging_rx: a Dragging for a writable icon.  It comes from
 * another task through the Wimp's interception, or from ourselves.  It is
 * claimed with a ghost caret at the pointer, and the icon is autoscrolled.
 * The claim is let go when the drag moves to another icon or is aborting.
 * Then there is no reply, so the Dragging bounces. */
static void dragging_rx(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    uint8_t *b = blk();
    uint32_t window = rd(b, 20);
    int32_t icon = (int32_t)rd(b, 24);
    if (!d->claiming) {
        if (rd(b, 36) & 16u)
            return;
        d->claiming = 1;
        autoscroll_icon(1);
        d->claimed_w = window, d->claimed_i = icon;
        int32_t x = work_x(window, (int32_t)rd(b, 28));
        d->last_x = x;
        d->last_scrollx = w->ghost_xoverride;
        set_caret(window, icon, x, (int32_t)rd(b, 32), (1u << 30) | (1u << 28), -2, 0);
        d->ghost = 1;
    } else {
        if ((rd(b, 36) & 16u) || window != d->claimed_w || icon != d->claimed_i) {
            d->claiming = 0;                    /* the claim let go */
            autoscroll_icon(0);
            if (d->ghost)
                no_ghost();
            d->ghost = 0;
            if (rd(b, 4) == k->handle)
                dragging_bounced();
            return;
        }
        d->claimed_w = window, d->claimed_i = icon;
        int32_t x = work_x(window, (int32_t)rd(b, 28));
        if (x != d->last_x || w->ghost_xoverride != d->last_scrollx) {
            d->last_x = x;
            d->last_scrollx = w->ghost_xoverride;
            set_caret(window, icon, x, (int32_t)rd(b, 32), (1u << 30) | (1u << 28), -2, 0);
        }
        d->ghost = 1;
    }
    wr(b, 16, MSG_DRAGCLAIM);                   /* the claim: text, with the drag box removed unless the ghost hides */
    wr(b, 0, 32);
    wr(b, 20, (w->ghost.hf & (1u << 25)) ? 0 : 2u);
    wr(b, 24, TYPE_TEXT), wr(b, 28, 0xFFFFFFFFu);
    wr(b, 12, rd(b, 8));
    uint32_t to = rd(b, 4);
    if (to == k->handle) {
        dragclaim_rx();
        return;
    }
    os_error *e = send_r3(17, to, 0);
    if (e)
        report(e);
}

/* cbtask_get_selected_text: the selected text of the window whose
 * selection is shown, into k->drag.  Returns 0 if there is none. */
static int selected_text(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_clip *k = cb();
    if (w->selwin == 0xFFFFFFFFu)
        return 0;
    struct wimp_window *win = wimp_window(w->selwin);
    if (!win || win->sel.icon < 0)
        return 0;
    uint32_t cap = fetch_text(w->selwin, win->sel.icon);
    uint32_t lo = (uint32_t)win->sel.low, hi = (uint32_t)win->sel.high;
    if (!cap || hi > cap || lo > hi)
        return 0;
    if (k->drag)
        xos_module_free(ros_ptr(k->drag));
    k->drag = 0, k->drag_len = 0;
    void *mem;
    if (xos_module_claim(hi - lo + 1, &mem))
        return 0;
    memcpy(mem, k->text + lo, hi - lo);
    k->drag = ros_addr(mem), k->drag_len = hi - lo;
    return 1;
}

/* cbtask_send_message_datasave: the dragged text offered to dest (a
 * window under the pointer, or a claimant task) */
static void send_datasave(uint32_t dest, uint32_t yref)
{
    uint8_t *b = blk();
    wr(b, 4, dest);
    wr(b, 12, yref);
    pointer_info();
    wr(b, 20, pi(3)), wr(b, 24, pi(4)), wr(b, 28, pi(0)), wr(b, 32, pi(1));
    if (!selected_text())
        return;
    run_datasave(cb()->drag, cb()->drag_len, 1);
}

/* cbtask_dragdrop_internal_xfer, for a drop in the same icon.  The text is
 * moved within it.  With Shift it is copied and the source then cut, as
 * 5.30 does. */
static void same_icon(int32_t gi, uint32_t gw, int32_t gicon)
{
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    d->shift ^= 1;
    struct wimp_window *win = wimp_window(gw);
    if (!win)
        return;
    int32_t lo = win->sel.low, hi = win->sel.high;
    if (gi <= hi && gi >= lo) {
        export_finished(0, 1, NULL);            /* inside the selection: nothing */
        return;
    }
    if (d->shift) {
        set_caret(gw, gicon, 0, 0, 1u << 31, 0, 0);
        set_caret(gw, gicon, 0, 0, 0xFFFFFFFFu, gi, 0);
        int ok = insert_text(gw, (uint32_t)gicon, k->drag, k->drag_len);
        export_finished(ok, 1, NULL);
        return;
    }
    uint32_t cap = fetch_text(gw, gicon);
    if (!cap) {
        export_finished(0, 1, NULL);
        return;
    }
    uint8_t *t = k->text, *data = ros_ptr(k->drag);
    uint32_t n = k->drag_len;
    if (gi < lo) {                              /* before it: what lies between moves up */
        memmove(t + gi + (hi - lo), t + gi, (uint32_t)(lo - gi));
        memcpy(t + gi, data, n);
    } else {                                    /* after it: down */
        memmove(t + lo, t + hi, (uint32_t)(gi - hi));
        memcpy(t + lo + (gi - hi), data, n);
    }
    put_text(gw, gicon, cap);
    set_caret(gw, gicon, 0, 0, 0xFFFFFFFFu, gi, 0);
    redraw_icon(gw, gicon);
    xos_module_free(ros_ptr(k->drag));
    k->drag = 0, k->drag_len = 0;
}

/* cbtask_dragdrop_internal_xfer: one of our drags ended on a writable icon
 * that we had claimed.  The text is put in at the ghost caret. */
static void internal_xfer(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    d->claiming = 0, d->ghost = 0;
    autoscroll_icon(0);
    if (!selected_text()) {
        no_ghost();
        export_finished(0, 1, NULL);
        return;
    }
    int32_t gi = w->ghost.index, gicon = w->ghost.i;
    uint32_t gw = w->ghost.w;
    no_ghost();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    wr(k->ws, 0, d->src_w);
    c.r[1] = ros_addr(k->ws);
    ros_swi(&c, XWimp_GetWindowState);
    if (c.v) {
        export_finished(0, 1, ros_ptr(c.r[0]));
        return;
    }
    if (d->src_w == gw && d->src_i == gicon) {
        same_icon(gi, gw, gicon);
        return;
    }
    struct wimp_window *win = wimp_window(gw);
    if (win && win->sel.icon == gicon && (gi > win->sel.high || win->sel.low > gi))
        set_caret(gw, gicon, 0, 0, 1u << 31, 0, 0);
    set_caret(gw, gicon, 0, 0, 0xFFFFFFFFu, gi, 0);
    int ok = insert_text(gw, (uint32_t)gicon, k->drag, k->drag_len);
    export_finished(ok, 1, NULL);
}

/* cbtask_message_dragclaim_rx: a claim that is starting or going on, or
 * the drop when the drag is over.  The drop sends a DataSave to the
 * claimant.  If the claimant is ourselves, the drop is done here. */
static void dragclaim_rx(void)
{
    struct wimp_clip *k = cb();
    struct wimp_cbdrag *d = &k->d;
    uint8_t *b = blk();
    uint32_t flags = rd(b, 20);
    if (d->finished) {
        if (d->aborted)
            return;
        d->delete_source = (flags & 8u) != 0;
        if (d->claimant == k->handle)
            internal_xfer();
        else
            send_datasave(d->claimant, rd(b, 8));
        return;
    }
    pointer_info();
    if (d->lastref != 0 && (d->old_flags & 1u) && !(flags & 1u))
        set_ptr_drop();
    if (d->lastref != 0 && (d->old_flags & 2u)) {
        if (!(flags & 2u))
            redo_dragbox(5);
    } else if (flags & 2u) {
        redo_dragbox(7);                        /* the box taken away: a point drag */
    }
    d->claimant = rd(b, 4);
    d->lastref = rd(b, 8);
    d->old_flags = flags;
}

/* cbtask_abortdrag: Escape, or the dragged selection changed or gone */
static void abortdrag(void)
{
    struct wimp_cbdrag *d = &cb()->d;
    d->dragging = 0;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = 0xFFFFFFFFu;
    ros_swi(&c, XWimp_DragBox);
    select_pointer(1);
    d->finished = 1, d->aborted = 1;
    d->src_w = 0;
    idle();
}

/* cbtask_drag_end: User_Drag_Box.  Over the dragged window, Shift's sense
 * is reversed (a move rather than a copy) */
static void drag_end(void)
{
    struct wimp_cbdrag *d = &cb()->d;
    d->dragging = 0;
    select_pointer(1);
    d->finished = 1;
    pointer_info();
    if (pi(3) == d->src_w)
        d->shift ^= 1;
    idle();
}
/* The task's own code: on its own runtime task, from task.c */
void wimp_clipboard_main(void)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_clip *k = cb();
    char *name = (char *)k->name;
    strcpy(name, "Clipboard Manager");
    static const uint32_t list[] = { MSG_DATASAVE, MSG_DATASAVEACK, MSG_DATALOAD, MSG_DATALOADACK,
                                     MSG_RAMFETCH, MSG_RAMTRANSMIT, MSG_CLAIMENTITY, MSG_DATAREQUEST,
                                     MSG_DRAGGING, MSG_DRAGCLAIM, 0 };
    memcpy(k->list, list, sizeof list);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 310, c.r[1] = TASK_WORD, c.r[2] = ros_addr(name), c.r[3] = ros_addr(k->list);
    if (!swi(XWimp_Initialise, &c)) {
        k->state = CB_PENDING;
        return;
    }
    k->handle = c.r[1];
    k->state = CB_RUNNING;
    k->pollword = 0;
    (void)w;
    for (;;) {
        ros_cpu_enter(&c);
        c.r[1] = blk_addr(), c.r[3] = ros_addr(&k->pollword);
        if (k->d.dragging) {                    /* nulls every 25 cs while dragging */
            struct ros_cpu t;
            ros_cpu_enter(&t);
            ros_swi(&t, XOS_ReadMonotonicTime);
            c.r[0] = (1u << 22) | (1u << 23);
            c.r[2] = t.r[0] + 25;
            if (!swi(XWimp_PollIdle, &c))
                continue;
        } else {
            c.r[0] = (1u << 22) | (1u << 23) | 1u;  /* the pollword, fast, and no nulls */
            if (!swi(XWimp_Poll, &c))
                continue;
        }
        uint32_t r = c.r[0];
        uint8_t *b = blk();
        if (r == 0) {
            idle();
            continue;
        }
        if (r == 7) {
            drag_end();
            continue;
        }
        if (r == 19) {
            if (rd(b, 16) == MSG_DRAGGING)
                dragging_bounced();
            continue;
        }
        if (r == 13) {
            uint32_t pw = k->pollword;
            if (pw & CB_PW_DATALOAD) {
                k->pollword = pw & ~CB_PW_DATALOAD;
                if (k->park_size) {
                    memcpy(b, k->park, k->park_size);
                    dataload();
                }
                continue;
            }
            k->pollword = 0;
            if (pw == CB_PW_PASTE)
                paste();
            else if (pw == CB_PW_COPY || pw == CB_PW_CUT)
                claim();
            else if (pw == CB_PW_DRAGSTART)
                startdrag();
            else if (pw == CB_PW_DRAGABORT)
                abortdrag();
            continue;
        }
        if (r != 17 && r != 18)
            continue;
        switch (rd(b, 16)) {
        case 0:                                 /* Message_Quit: down for good */
            ros_cpu_enter(&c);
            c.r[0] = k->handle, c.r[1] = TASK_WORD;
            ros_swi(&c, XWimp_CloseDown);
            k->state = CB_DORMANT;
            k->handle = 0;
            ros_cpu_enter(&c);
            ros_swi(&c, XOS_Exit);
            return;
        case MSG_DATASAVE:
            datasave();
            break;
        case MSG_DATALOAD:
            dataload();
            break;
        case MSG_CLAIMENTITY:                   /* another task's clipboard: ours is gone */
            if (rd(b, 4) != k->handle && (rd(b, 20) & 4u))
                wimp_clip_forget();
            break;
        case MSG_DATAREQUEST:                   /* a request for the clipboard */
            serve();
            break;
        case MSG_DRAGGING:                      /* a drag over a writable icon */
            dragging_rx();
            break;
        case MSG_DRAGCLAIM:
            dragclaim_rx();
            break;
        default:
            break;
        }
    }
}
