/* filters.c: the Wimp filters. The native Wimp brings its own
 * FilterManager module, and this file holds it and the filter chains the
 * Wimp calls.
 *
 * A module titled FilterManager answers the Filter_* SWIs and *Filters, as
 * 5.30's does, so that the Toolbox's RMEnsure finds it. The Wimp calls its
 * chains at six points: before a poll, after a poll, on a block copy, on
 * each redraw rectangle, after each rectangle, and after the icons of a
 * rectangle are drawn. The module's state is in the RMA, through its
 * private word. */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "wimp.h"

#define XTaskManager_TaskNameFromHandle 0x62680u
#define FILTER_KINDS 6
#define TASK_QUIT 0x80000000u

/* the kinds, in Wimp_RegisterFilter's order */
enum { K_PRE, K_POST, K_COPY, K_RECT, K_POSTRECT, K_POSTICON };

struct filter {
    uint32_t next;                       /* the next registration (RMA), or 0 */
    uint32_t name, code, r12, task, mask;
};

struct filter_ws {
    uint32_t chain[FILTER_KINDS];        /* the front of each chain */
    uint32_t replaced[FILTER_KINDS], replaced_r12[FILTER_KINDS];  /* from Wimp_RegisterFilter */
    uint32_t msgs[4];                    /* MessageTrans's descriptor */
    int msgs_open;
    char path[64];
};

static uint32_t fws_addr;                /* the module's workspace, an RMA address */

static struct filter_ws *fws(void)
{
    return fws_addr ? ros_ptr(fws_addr) : NULL;
}

static struct filter *rec(uint32_t a)
{
    return a ? ros_ptr(a) : NULL;
}

/* ---- calling the filters ----------------------------------------------------------- */

static void call(uint32_t code, uint32_t r12, struct ros_cpu *c)
{
    c->r[12] = r12;
    c->mode = ROS_MODE_SVC;
    c->irq_off = 0;
    ros_call(c, code);
}

static int applies(const struct filter *f, uint32_t task)
{
    return !(f->task & TASK_QUIT) && (f->task == 0 || f->task == (task & 0xFFFFu));
}

uint32_t wimp_filter_pre(uint32_t mask, uint32_t block, uint32_t task)
{
    struct filter_ws *s = fws();
    if (!s)
        return mask;
    for (struct filter *f = rec(s->chain[K_PRE]); f; f = rec(f->next)) {
        if (!applies(f, task))
            continue;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = mask, c.r[1] = block, c.r[2] = task;
        call(f->code, f->r12, &c);
        mask = c.r[0];
    }
    return mask;
}

/* Returns -1 if a filter claimed the event. */
uint32_t wimp_filter_post(uint32_t reason, uint32_t block, uint32_t task)
{
    struct filter_ws *s = fws();
    if (!s)
        return reason;
    uint32_t original = reason, result = reason;
    int claimed = 0;
    for (struct filter *f = rec(s->chain[K_POST]); f; f = rec(f->next)) {
        if (!applies(f, task))
            continue;
        if (result < 32 && (f->mask & (1u << result)))
            continue;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = claimed ? original : result, c.r[1] = block, c.r[2] = task;
        call(f->code, f->r12, &c);
        if (c.r[0] == 0xFFFFFFFFu)
            claimed = 1;
        else if (!claimed)
            result = c.r[0];
    }
    return claimed ? 0xFFFFFFFFu : result;
}

static void rect_kind(int kind, uint32_t window, uint32_t task, struct wimp_box r)
{
    struct filter_ws *s = fws();
    if (!s)
        return;
    for (struct filter *f = rec(s->chain[kind]); f; f = rec(f->next)) {
        if (!applies(f, task))
            continue;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = window, c.r[2] = task;
        c.r[6] = (uint32_t)r.x0, c.r[7] = (uint32_t)r.y0, c.r[8] = (uint32_t)r.x1, c.r[9] = (uint32_t)r.y1;
        call(f->code, f->r12, &c);
    }
}

void wimp_filter_rect(uint32_t window, uint32_t task, struct wimp_box r)
{
    rect_kind(K_RECT, window, task, r);
}

void wimp_filter_postrect(uint32_t window, uint32_t task, struct wimp_box r)
{
    rect_kind(K_POSTRECT, window, task, r);
}

void wimp_filter_posticon(uint32_t window, uint32_t task, struct wimp_box r)
{
    rect_kind(K_POSTICON, window, task, r);
}

void wimp_filter_copy(uint32_t window, struct wimp_box dst, struct wimp_box src, int32_t dx, int32_t dy)
{
    struct filter_ws *s = fws();
    if (!s)
        return;
    for (struct filter *f = rec(s->chain[K_COPY]); f; f = rec(f->next)) {
        if (f->task & TASK_QUIT)
            continue;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = window;
        c.r[2] = (uint32_t)dst.x0, c.r[3] = (uint32_t)dst.y0, c.r[4] = (uint32_t)dst.x1, c.r[5] = (uint32_t)dst.y1;
        c.r[6] = (uint32_t)src.x0, c.r[7] = (uint32_t)src.y0;
        c.r[8] = (uint32_t)(src.x1 - dx), c.r[9] = (uint32_t)(src.y1 - dy);
        call(f->code, f->r12, &c);
    }
}

/* ---- the SWIs --------------------------------------------------------------------- */

static os_error *add(int kind, struct ros_cpu *s)
{
    struct filter_ws *w = fws();
    void *mem;
    os_error *e = xos_module_claim(sizeof(struct filter), &mem);
    if (e)
        return e;
    struct filter *f = mem;
    f->name = s->r[0], f->code = s->r[1], f->r12 = s->r[2];
    f->task = kind == K_COPY ? 0 : s->r[3] & 0xFFFFu;
    f->mask = kind == K_POST ? s->r[4] : 0;
    f->next = w->chain[kind];
    w->chain[kind] = ros_addr(mem);
    return NULL;
}

static os_error *unknown(void)
{
    return ros_error(0x605, "Unknown filter");
}

static os_error *remove_one(int kind, struct ros_cpu *s)
{
    struct filter_ws *w = fws();
    uint32_t task = kind == K_COPY ? 0 : s->r[3] & 0xFFFFu;
    for (uint32_t *link = &w->chain[kind]; *link; link = &rec(*link)->next) {
        struct filter *f = rec(*link);
        uint32_t ft = f->task & ~TASK_QUIT;
        if (f->name == s->r[0] && f->code == s->r[1] && f->r12 == s->r[2] && (ft == 0 || ft == task)) {
            uint32_t a = *link;
            *link = f->next;
            xos_module_free(ros_ptr(a));
            return NULL;
        }
    }
    return unknown();
}

static void done(struct ros_cpu *s, os_error *e)
{
    if (e) {
        s->r[0] = ros_addr(e);
        s->v = 1;
    } else {
        s->v = 0;
    }
}

static void swi_RegisterPre(struct ros_cpu *s) { done(s, add(K_PRE, s)); }
static void swi_RegisterPost(struct ros_cpu *s) { done(s, add(K_POST, s)); }
static void swi_DeRegisterPre(struct ros_cpu *s) { done(s, remove_one(K_PRE, s)); }
static void swi_DeRegisterPost(struct ros_cpu *s) { done(s, remove_one(K_POST, s)); }
static void swi_RegisterRect(struct ros_cpu *s) { done(s, add(K_RECT, s)); }
static void swi_DeRegisterRect(struct ros_cpu *s) { done(s, remove_one(K_RECT, s)); }
static void swi_RegisterCopy(struct ros_cpu *s) { done(s, add(K_COPY, s)); }
static void swi_DeRegisterCopy(struct ros_cpu *s) { done(s, remove_one(K_COPY, s)); }
static void swi_RegisterPostRect(struct ros_cpu *s) { done(s, add(K_POSTRECT, s)); }
static void swi_DeRegisterPostRect(struct ros_cpu *s) { done(s, remove_one(K_POSTRECT, s)); }
static void swi_RegisterPostIcon(struct ros_cpu *s) { done(s, add(K_POSTICON, s)); }
static void swi_DeRegisterPostIcon(struct ros_cpu *s) { done(s, remove_one(K_POSTICON, s)); }

static ros_swi_thunk *const thunks[64] = {
    swi_RegisterPre, swi_RegisterPost, swi_DeRegisterPre, swi_DeRegisterPost,
    swi_RegisterRect, swi_DeRegisterRect, swi_RegisterCopy, swi_DeRegisterCopy,
    swi_RegisterPostRect, swi_DeRegisterPostRect, swi_RegisterPostIcon, swi_DeRegisterPostIcon,
};

static const char *const names[] = {
    "RegisterPreFilter", "RegisterPostFilter", "DeRegisterPreFilter", "DeRegisterPostFilter",
    "RegisterRectFilter", "DeRegisterRectFilter", "RegisterCopyFilter", "DeRegisterCopyFilter",
    "RegisterPostRectFilter", "DeRegisterPostRectFilter", "RegisterPostIconFilter",
    "DeRegisterPostIconFilter", NULL,
};

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6, "SWI value out of range for module FilterManager");
}

/* Wimp_RegisterFilter. The routine and R12 are recorded for the kind. */
void wimp_swi_RegisterFilter(struct ros_cpu *s)
{
    if (s->r[0] >= FILTER_KINDS) {
        wimp_fail(s, wimp_error(E_BAD_SYSINFO));
        return;
    }
    struct filter_ws *w = fws();
    if (w) {
        w->replaced[s->r[0]] = s->r[1];
        w->replaced_r12[s->r[0]] = s->r[2];
    }
    s->v = 0;
}

/* ---- *Filters ---------------------------------------------------------------------- */

static void out_c(uint32_t ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ch;
    ros_swi(&c, XOS_WriteC);
}

static void newline(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_NewLine);
}

/* As writefield: write up to width - 3 characters, padded to width. */
static void field(const char *t, int width)
{
    int n = 0;
    while (t[n] >= 32 && n < width - 3)
        out_c((uint8_t)t[n++]);
    for (int k = n; k < width; k++)
        out_c(' ');
}

static void field_guest(uint32_t a, int width)
{
    char t[64];
    unsigned n = 0;
    while (n < sizeof t - 1 && ros_ld8(a + n) >= 32)
        t[n] = (char)ros_ld8(a + n), n++;
    t[n] = 0;
    field(t, width);
}

static void dashed(const char *t, int width)
{
    int n = 0;
    while (t[n] >= 32)
        out_c('-'), n++;
    for (int k = n; k < width; k++)
        out_c(' ');
}

static void lookup(const char *token, char *out, size_t size)
{
    struct filter_ws *w = fws();
    snprintf(out, size, "%s", token);
    if (!w->msgs_open) {
        snprintf(w->path, sizeof w->path, "Resources:$.Resources.FilterMgr.Messages");
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(w->msgs), c.r[1] = ros_addr(w->path), c.r[2] = 0;
        ros_swi(&c, XMessageTrans_OpenFile);
        if (c.v)
            return;
        w->msgs_open = 1;
    }
    struct wimp_ws *ww = wimp_ws();
    char *tok = ww ? (char *)ww->scratch : NULL;
    if (!tok)
        return;
    snprintf(tok, 32, "%s", token);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(w->msgs), c.r[1] = ros_addr(tok), c.r[2] = 0;
    ros_swi(&c, XMessageTrans_Lookup);
    if (c.v)
        return;
    size_t n = 0;
    for (; n < size - 1 && n < c.r[3] && ros_ld8(c.r[2] + n) >= 32; n++)
        out[n] = (char)ros_ld8(c.r[2] + n);
    out[n] = 0;
}

static void write_line(const char *t)
{
    for (; *t; t++)
        out_c((uint8_t)*t);
}

static void taskname(uint32_t task, const char *all, int width)
{
    if (task == 0) {
        field(all, width);
        return;
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = task;
    ros_swi(&c, XTaskManager_TaskNameFromHandle);
    if (!c.v)
        field_guest(c.r[0], width);
}

static void headings(const char *title, int flags, const char *filter, const char *task, const char *mask,
                     int w5, int w6)
{
    write_line(title);
    newline();
    if (flags & 1) field(filter, w5);
    if (flags & 2) field(task, w6);
    if (flags & 4) field(mask, w6);
    newline();
    if (flags & 1) dashed(filter, w5);
    if (flags & 2) dashed(task, w6);
    if (flags & 4) dashed(mask, w6);
    newline();
}

static os_error *cmd_filters(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    struct filter_ws *w = fws();
    char filter[40], task[40], mask[40], all[40], title[96];
    lookup("Filter", filter, sizeof filter);
    lookup("Task", task, sizeof task);
    lookup("Mask", mask, sizeof mask);
    lookup("All", all, sizeof all);
    int w5 = (int)strlen(filter) + 3, w6 = (int)strlen(task) + 3;
    if (w5 < 20) w5 = 20;
    if (w6 < 24) w6 = 24;
    static const struct { const char *token; int kind, flags, eol, blank; } sections[] = {
        { "PreFT", K_PRE, 3, 1, 1 },     { "PostFT", K_POST, 7, 1, 1 },
        { "RectFT", K_RECT, 3, 0, 1 },   { "PostRectFT", K_POSTRECT, 3, 1, 1 },
        { "PostIconFT", K_POSTICON, 3, 1, 1 }, { "CopyFT", K_COPY, 1, 1, 0 },
    };
    for (unsigned i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        lookup(sections[i].token, title, sizeof title);
        headings(title, sections[i].flags, filter, task, mask, w5, w6);
        for (struct filter *f = rec(w->chain[sections[i].kind]); f; f = rec(f->next)) {
            if (f->task & TASK_QUIT)
                continue;
            field_guest(f->name, w5);
            if (sections[i].flags & 2)
                taskname(f->task, all, w6);
            if (sections[i].flags & 4) {
                char hex[9];
                snprintf(hex, sizeof hex, "%08X", f->mask);
                write_line(hex);
            }
            if (sections[i].eol)
                newline();
        }
        if (sections[i].blank)
            newline();
    }
    return NULL;
}

static const struct ros_command commands[] = {
    { "Filters", 0, "Syntax: *Filters", "*Filters displays all Wimp filters currently active.", cmd_filters },
    { NULL, 0, NULL, NULL, NULL },
};

/* ---- the module ------------------------------------------------------------------ */

/* When a task closes down, its filters are no longer called. */
static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    struct filter_ws *w = fws();
    if (!w || s->r[1] != 0x53 || s->r[0] != 0)
        return;
    uint32_t task = s->r[2] & 0xFFFFu;
    for (int k = 0; k < FILTER_KINDS; k++) {
        if (k == K_COPY)
            continue;
        for (struct filter *f = rec(w->chain[k]); f; f = rec(f->next))
            if (f->task == task)
                f->task |= TASK_QUIT;
    }
}

static void installed(void *arg)
{
    (void)arg;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 30, c.r[1] = 0x87;                 /* Service_FilterManagerInstalled */
    ros_service_call(&c);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    void *block;
    os_error *e = xos_module_claim(sizeof(struct filter_ws), &block);
    if (e)
        return e;
    memset(block, 0, sizeof(struct filter_ws));
    fws_addr = ros_addr(block);
    ros_st32(m->private_word, fws_addr);
    ros_callback_add_native(installed, NULL);   /* the service call is made from a callback */
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct filter_ws *w = fws();
    if (!w)
        return NULL;
    for (int k = 0; k < FILTER_KINDS; k++)
        while (w->chain[k]) {
            uint32_t a = w->chain[k];
            w->chain[k] = rec(a)->next;
            xos_module_free(ros_ptr(a));
        }
    if (w->msgs_open) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ros_addr(w->msgs);
        ros_swi(&c, XMessageTrans_CloseFile);
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = 0x88;                              /* Service_FilterManagerDying */
    ros_service_call(&c);
    xos_module_free(w);
    fws_addr = 0;
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module filtermgr_native_module = {
    .title = "FilterManager",
    .help = "Filter Manager\t0.30 (21 Aug 2023)",
    .init = init,
    .final = final,
    .service = service,
    .bad_swi = bad_swi,
    .swi_chunk = 0x42640,
    .swi_count = 12,
    .swi_thunks = thunks,
    .swi_names = names,
    .swi_prefix = "Filter",
    .commands = commands,
};
