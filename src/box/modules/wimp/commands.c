/* commands.c: the Wimp's *Commands, its start entry (used by *WimpTask
 * outside a task), and Wimp_SetMode. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

static int call(uint32_t swi, struct ros_cpu *c)
{
    ros_swi(c, swi);
    return !c->v;
}

/* ---- *WimpTask --------------------------------------------------------------------- */

/* The module's start entry. A temporary task starts the command and then
 * exits. */
void wimp_start(struct ros_module *m, uint32_t tail)
{
    (void)m;
    struct wimp_ws *w = wimp_ws();
    char *name = (char *)w->scratch + 448;
    strcpy(name, "<temporary>");
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 200, c.r[1] = TASK_WORD, c.r[2] = ros_addr(name);
    if (call(XWimp_Initialise, &c)) {
        ros_cpu_enter(&c);
        c.r[0] = tail;
        ros_swi(&c, XWimp_StartTask);
    }
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_Exit);
}

os_error *wimp_cmd_wimptask(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)argc;
    struct ros_cpu c;
    if (wimp_current()) {
        ros_cpu_enter(&c);
        c.r[0] = tail;
        ros_swi(&c, XWimp_StartTask);
        return c.v ? ros_ptr(c.r[0]) : NULL;
    }
    ros_cpu_enter(&c);
    c.r[0] = 2, c.r[1] = m->base + ros_ld32(m->base + 0x10), c.r[2] = tail;
    ros_swi(&c, XOS_Module);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* ---- *Pointer ---------------------------------------------------------------------- */

os_error *wimp_cmd_pointer(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct wimp_ws *w = wimp_ws();
    uint32_t ch = argc ? ros_ld8(tail) : '1';
    struct ros_cpu c;
    if (ch == '1' || !argc) {
        char *name = (char *)w->scratch + 448;
        strcpy(name, "ptr_default");
        ros_cpu_enter(&c);
        c.r[0] = 36, c.r[2] = ros_addr(name), c.r[3] = 1, c.r[4] = c.r[5] = c.r[6] = c.r[7] = 0;
        ros_swi(&c, XWimp_SpriteOp);
        uint8_t *b = w->scratch;
        int32_t q[4] = { 0, 0, w->screen_w - 1, w->screen_h - 1 };
        b[0] = 1;
        for (int i = 0; i < 4; i++)
            b[1 + 2 * i] = (uint8_t)q[i], b[2 + 2 * i] = (uint8_t)(q[i] >> 8);
        ros_cpu_enter(&c);
        c.r[0] = 21, c.r[1] = ros_addr(b);
        ros_swi(&c, XOS_Word);
        return NULL;
    }
    if (ch == '0') {
        ros_cpu_enter(&c);
        c.r[0] = 106, c.r[1] = 0;
        ros_swi(&c, XOS_Byte);
        return NULL;
    }
    if (ch == '2')
        return NULL;
    return ros_error(0x28B, "Bad parameter passed to *Pointer");
}

/* ---- *WimpWriteDir, *WimpKillSprite ------------------------------------------------- */

os_error *wimp_cmd_wimpwritedir(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = tail;
    if (!call(XOS_ReadUnsigned, &c))
        return ros_ptr(c.r[0]);
    if (c.r[2] <= 1)
        wimp_ws()->writedir = c.r[2];
    return NULL;
}

os_error *wimp_cmd_wimpkillsprite(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 25, c.r[2] = tail;
    ros_swi(&c, XWimp_SpriteOp);
    wimp_tiles_forget();
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* ---- *WimpVisualFlags ----------------------------------------------------------------- */

os_error *wimp_cmd_wimpvisualflags(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct wimp_ws *w = wimp_ws();
    if (!argc) {                                /* no arguments: all off but NoFontBlending */
        w->dr.threed = 0x40u;
        return NULL;
    }
    /* The arguments are read by OS_ReadArgs with 5.30's template. */
    static const char tmpl[] =
        "3DWindowBorders=3DWB/S,TexturedMenus=TM/S,UseAlternateMenuBg=UAMB/S,RemoveIconBoxes=RIB/S,"
        "NoIconBoxesInTransWindows=NIBITW/S,Fully3DIconBar=F3DIB/S,All=A/S,"
        "WindowBorderFaceColour=WBFC/E,WindowBorderOppColour=WBOC/E,"
        "MenuBorderFaceColour=MBFC/E,MenuBorderOppColour=MBOC/E,"
        "ButtonBorderFaceColour=BBFC/E,ButtonBorderShallowColour=BBSC/E,ButtonBorderOppColour=BBOC/E,"
        "ButtonColour=BC/E,ButtonWellColour=BWC/E,ButtonHighlightColour=BHC/E,"
        "NoFontBlending=NFB/S,FontBlending=FB/S,"
        "WindowOutlineColour=WOC/E,WindowOutlineOver=WOO/S,NoIconBarBorder=NIBB/S";
    void *mem;                                  /* the template and OS_ReadArgs' output, in the RMA */
    os_error *e = xos_module_claim(sizeof tmpl + 256, &mem);
    if (e)
        return e;
    char *t = mem;
    uint8_t *out = (uint8_t *)mem + sizeof tmpl;
    strcpy(t, tmpl);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(t), c.r[1] = tail, c.r[2] = ros_addr(out), c.r[3] = 256;
    ros_swi(&c, XOS_ReadArgs);
    if (c.v) {
        os_error *r = ros_ptr(c.r[0]);         /* copied before the block is freed */
        char m[252];
        strncpy(m, r->errmess, sizeof m - 1);
        m[sizeof m - 1] = 0;
        uint32_t n = r->errnum;
        xos_module_free(mem);
        return ros_error(n, "%s", m);
    }
    uint32_t a[22];
    memcpy(a, out, sizeof a);
    uint32_t f = 0x40u;
    static const uint32_t sw[][2] = { { 0, 1u << 0 }, { 1, 1u << 4 }, { 2, 1u << 1 }, { 3, 1u << 3 },
                                      { 4, 1u << 5 }, { 5, 1u << 2 }, { 20, 1u << 8 }, { 21, 1u << 9 } };
    for (unsigned k = 0; k < sizeof sw / sizeof sw[0]; k++)
        if (a[sw[k][0]])
            f |= sw[k][1];
    if (a[18])
        f &= ~0x40u;                            /* FontBlending. NoFontBlending changes nothing. */
    /* Each colour given as an integer is converted from &RRGGBB to
     * &BBGGRR00. A colour not given takes its default. */
    static const uint8_t col[TH_COUNT] = { 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 19 };
    for (int k = 0; k < TH_COUNT; k++) {
        uint32_t p = a[col[k]];
        uint32_t v = wimp_theme_defaults[k];
        if (p && ros_ld8(p) == 0)
            v = (uint32_t)ros_ld8(p + 3) << 8 | (uint32_t)ros_ld8(p + 2) << 16 | (uint32_t)ros_ld8(p + 1) << 24;
        w->dr.theme[k] = v;
    }
    xos_module_free(mem);
    if (a[6])
        f = 0xFFFFFFBFu;                        /* All */
    w->dr.threed = f;
    return NULL;
}

/* ---- *ToolSprites ------------------------------------------------------------------- */

os_error *wimp_cmd_toolsprites(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    struct wimp_ws *w = wimp_ws();
    if (w->tools)
        xos_module_free(ros_ptr(w->tools));
    w->tools = 0;                               /* WindowManager:Tools is loaded afresh */
    wimp_tools_refresh();
    if (w->ntasks)
        wimp_invalidate_box(wimp_screen_box());
    return NULL;
}

/* ---- Wimp_SetMode and *WimpMode -------------------------------------------------------- */

/* As copy_mode_specifier. A mode selector is copied into the RMA, because
 * the caller's block may go away. It may be *WimpMode's block or be in a
 * task's slot. A mode number is returned as it is. Returns 0 if there is
 * no room. */
static uint32_t copy_selector(uint32_t mode)
{
    if (mode < 256)
        return mode;
    uint32_t n = 20;                            /* past the flags, sizes, depth and rate */
    while (n < 20 + 8 * 32 && ros_ld32(mode + n) != 0xFFFFFFFFu)
        n += 8;
    n += 4;
    void *b;
    if (xos_module_claim(n, &b))
        return 0;
    for (uint32_t i = 0; i < n; i += 4)
        ((uint32_t *)b)[i / 4] = ros_ld32(mode + i);
    return ros_addr(b);
}

/* Make m the Wimp's mode, and free the copy it held before. */
static void keep_mode(uint32_t m)
{
    struct wimp_ws *w = wimp_ws();
    if (w->mode_copy && w->mode_copy != m)
        xos_module_free(ros_ptr(w->mode_copy));
    w->mode_copy = m >= 256 ? m : 0;
    w->mode = m;
}

/* As int_setmode. The mode is changed with the pointer kept where it was,
 * and screen memory is shrunk. If the screen is smaller, windows are forced
 * on screen when the mode-change messages go out. The Wimp's state is then
 * made again for the new mode. */
os_error *wimp_int_setmode(uint32_t mode)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    uint8_t *pb = w->scratch + 496;             /* OS_Word 21 reasons 6 (read) and 3 (set) */
    wimp_flush();
    pb[0] = 6;
    ros_cpu_enter(&c);
    c.r[0] = 21, c.r[1] = ros_addr(pb);
    ros_swi(&c, XOS_Word);
    ros_cpu_enter(&c);
    if (mode >= 256) {
        uint32_t copy = mode == w->mode ? mode : copy_selector(mode);
        if (!copy)
            copy = mode = w->mode;              /* no room to copy: keep the old mode */
        c.r[0] = 0, c.r[1] = copy;
        ros_swi(&c, XOS_ScreenMode);
        if (!c.v)
            keep_mode(copy);
        else if (copy != w->mode)
            xos_module_free(ros_ptr(copy));
    } else {
        uint8_t v[2] = { 22, (uint8_t)mode };
        wimp_vdu_bytes(v, 2);
        keep_mode(mode);
    }
    pb[0] = 3;
    ros_cpu_enter(&c);
    c.r[0] = 21, c.r[1] = ros_addr(pb);
    ros_swi(&c, XOS_Word);
    ros_cpu_enter(&c);
    c.r[0] = 2, c.r[1] = 0x80000000u;           /* screen memory as small as it can be */
    ros_swi(&c, XOS_ChangeDynamicArea);
    wimp_mode_refresh();
    w->forceflags = (w->screen_w < w->lastmode_w || w->screen_h < w->lastmode_h) ? 1 : 0;
    if (w->singletask < 0)
        w->mode_changed = 1;
    struct ros_cpu p;
    ros_cpu_enter(&p);
    ros_swi(&p, XColourTrans_InvalidateCache);  /* the palette is recalculated */
    wimp_tiles_forget();
    /* ROSGD adds a second test: whether the screen is the one the icon
     * bar was laid out for. mode_changed is set only when multitasking,
     * because it drives the round of messages, and those cannot go out
     * while a single task holds the machine. But the tools, the font and
     * the icon bar's layout are the Wimp's own state. If they are left set
     * for the old screen, the right-hand icons fall past the edge. A mode
     * changed from an Obey file (an old-style task, so singletask >= 0)
     * used never to lay out the bar again. The user's rule is that if
     * there is room on the bar for every icon, every icon is shown. */
    if (w->mode_changed || w->iconbar_laid_w != w->screen_w) {
        if (w->tools)
            xos_module_free(ros_ptr(w->tools));
        w->tools = 0;
        wimp_tools_refresh();
        wimp_vdu_init();
        wimp_find_font();
        wimp_iconbar_mode();
    }
    wimp_invalidate_box(wimp_screen_box());
    wimp_input_mode_set();
    return NULL;
}

static os_error *set_mode(uint32_t mode)
{
    struct wimp_ws *w = wimp_ws();
    if (!w->ntasks) {
        uint32_t copy = copy_selector(mode);    /* currentmode, for the next Wimp_Initialise */
        if (copy)
            keep_mode(copy);
        return NULL;
    }
    /* The command window is suspended during the change, so that the mode
     * change's own VDU output does not make a pending one active. It is
     * then put back as it was. If it was active, VDU 4 is sent "not to
     * confuse the user" (SetMode in s/Wimp02). */
    uint32_t flag = w->err.commandflag;
    w->err.commandflag = flag | CF_SUSPENDED;
    os_error *e = wimp_int_setmode(mode);
    w->err.commandflag = flag;
    if (flag & CF_ACTIVE) {
        static const uint8_t four = 4;
        wimp_vdu_bytes(&four, 1);
    }
    return e;
}

void wimp_swi_SetMode(struct ros_cpu *s)
{
    os_error *e = set_mode(s->r[0]);
    if (e)
        wimp_fail(s, e);
    else
        s->v = 0;
}

os_error *wimp_cmd_wimpmode(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10, c.r[1] = tail;
    if (call(XOS_ReadUnsigned, &c) && ros_ld8(c.r[1]) < 32) {
        uint32_t mode = c.r[2];
        if (mode > 255)
            return ros_error(0x1EA, "Bad parameters");
        ros_cpu_enter(&c);
        c.r[0] = 1;
        ros_swi(&c, XOS_ScreenMode);            /* the current mode */
        if (!c.v && c.r[1] == mode)
            return NULL;
        return set_mode(mode);
    }
    /* As setmode_from_specifier. The string is converted into a block of
     * ModeSelector_MaxSize, which Wimp_SetMode copies. */
    void *sel;
    os_error *e = xos_module_claim(256, &sel);
    if (e)
        return e;
    ros_cpu_enter(&c);
    c.r[0] = 13, c.r[1] = tail, c.r[2] = ros_addr(sel), c.r[3] = 256;
    e = call(XOS_ScreenMode, &c) ? set_mode(ros_addr(sel)) : ros_error(0x1EA, "Bad parameters");
    xos_module_free(sel);
    return e;
}

/* ---- Wimp_SetPointerShape -------------------------------------------------------------- */

void wimp_swi_SetPointerShape(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    if (s->r[0] != 0 && s->r[1] != 0xFFFFFFFFu) {
        uint8_t *b = w->scratch + 448;          /* OS_Word 21 reason 0 */
        uint32_t data = s->r[1];
        b[0] = 0, b[1] = (uint8_t)s->r[0], b[2] = (uint8_t)(s->r[2] / 4), b[3] = (uint8_t)s->r[3];
        b[4] = (uint8_t)s->r[4], b[5] = (uint8_t)s->r[5];
        memcpy(b + 6, &data, 4);
        ros_cpu_enter(&c);
        c.r[0] = 21, c.r[1] = ros_addr(b);
        ros_swi(&c, XOS_Word);
        if (c.v) {
            wimp_fail(s, ros_ptr(c.r[0]));
            return;
        }
    }
    ros_cpu_enter(&c);
    c.r[0] = 106, c.r[1] = s->r[0];
    ros_swi(&c, XOS_Byte);
    s->v = 0;
}

/* ---- Wimp_SetWatchdogState ------------------------------------------------------------- */

void wimp_swi_SetWatchdogState(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    if (s->r[0] == 0) {
        if (!w->watchdog_off) {                 /* disable, unless already disabled */
            w->watchdog_off = 1;
            w->watchdog_word = s->r[1];
        }
    } else if (w->watchdog_off && s->r[1] == w->watchdog_word) {
        w->watchdog_off = 0;                    /* enable, given the same word */
    }
    s->v = 0;
}

/* ---- *WimpStats (ROSGD only: how null events are paced) --------------------------- */

static os_error *say(const char *text)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    snprintf((char *)w->scratch, sizeof w->scratch, "%s", text);
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(w->scratch);
    if (!call(XOS_Write0, &c))
        return ros_ptr(c.r[0]);
    ros_cpu_enter(&c);
    return call(XOS_NewLine, &c) ? NULL : ros_ptr(c.r[0]);
}

os_error *wimp_cmd_wimpstats(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    struct wimp_ws *w = wimp_ws();
    if (!w)
        return NULL;
    int reset = argc && !strncasecmp((const char *)ros_ptr(tail), "-reset", 6);
    char line[128];
    os_error *e;
    if (!reset) {
        if (!w->pace_read)
            snprintf(line, sizeof line, "Null events: not paced yet (no task has taken them back to back)");
        else if (w->pace_ns)
            snprintf(line, sizeof line, "Null events: paced to one per %u.%03u ms for a busy-polling task",
                     (unsigned)(w->pace_ns / 1000000u), (unsigned)(w->pace_ns / 1000u % 1000u));
        else
            snprintf(line, sizeof line, "Null events: not paced (Wimp$NullPace or rosgd.nullpace 0)");
        if ((e = say(line)) != NULL || (e = say("Task                            Nulls      Paced")) != NULL)
            return e;
    }
    for (unsigned i = 0; i < WIMP_TASKS; i++) {
        struct wimp_task *t = w->task[i];
        if (!t || !t->live)
            continue;
        if (reset) {
            t->nulls = t->nulls_paced = 0;
            continue;
        }
        snprintf(line, sizeof line, "%-28.28s %10u %10u%s", t->name, t->nulls, t->nulls_paced,
                 t->null_paced ? "  (pacing)" : "");
        if ((e = say(line)) != NULL)
            return e;
    }
    return NULL;
}
