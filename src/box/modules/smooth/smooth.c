/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* smooth.c: Smooth, the switch that anti-aliases the desktop's Draw calls
 * through GDraw and forces the Font Manager's blending.
 *
 * There is one setting for the whole system. It is off by default. There is
 * also a list of task names that keep the exact path. While the setting is
 * on, it applies to the current Wimp task. Outside the desktop there is no
 * task, and the setting applies. It also needs the output, which is the
 * screen or a sprite, to be 16 or 32 bpp, and no printer job to be current.
 * Then:
 *
 *   - Draw_Fill and thick Draw_Stroke go to GDraw with the styles that
 *     SpecialFX sends. A fill of style 0 goes as &B0. A fill of plot bits
 *     &30 or &38 goes with the style ORed with &80. A thick stroke goes as
 *     &B8 with bit 31. Only winding rules 0 and 2 go, and only when the
 *     graphics foreground plots over (action 0) in a solid colour.
 *     Everything else stays Draw's. That is thin strokes, the other rules,
 *     other actions, patterns, Draw's clipping bits and the FP forms.
 *   - Font_Paint's blend flag (R2 bit 11) is set, through the Font Manager's
 *     hook (fm_paint_blend_hook).
 *
 * Draw's calls are taken on DrawV, which every Draw SWI goes through. This
 * module claims DrawV as it starts, after Draw. It passes every call on
 * untouched unless the switch takes it.
 *
 * The setting is kept in the text file Choices:Smooth. The file is written
 * to <Choices$Write>.Smooth whenever the setting changes. !Boot sets
 * Choices$Path after the ROM's modules have started. So the file is read
 * the first time it is wanted once Choices$Path exists. A *command before
 * then decides the setting itself, and the file is not read over it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "fontmanager.h"
#include "smooth.h"

#define DRAWV 0x20
#define XWIMP_READSYSINFO (0x400F2u | ROS_X_BIT)
#define XTASKMANAGER_TASKNAMEFROMHANDLE (0x42680u | ROS_X_BIT)
#define XPDRIVER_CURRENTJOB (0x80149u | ROS_X_BIT)
#define ERR_SYNTAX 0xDC

#define NAME_MAX_LEN 64

static struct {
    int on;
    int loaded;                         /* set when the choices have been read, or overruled by a command */
    char (*excl)[NAME_MAX_LEN];
    unsigned nexcl;
    int test;                           /* set when the self-test's task name is used in place of the Wimp's */
    char test_name[NAME_MAX_LEN];
    int test_none;
} sm;

static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* Copy a string into the RMA, for a SWI. The caller frees it. */
static char *arena_str(const char *s)
{
    char *p = ros_rma_alloc((uint32_t)strlen(s) + 1);
    if (p)
        strcpy(p, s);
    return p;
}

static int var_exists(const char *name)
{
    char *n = arena_str(name), *buf = ros_rma_alloc(256);
    int there = 0;
    if (n && buf) {
        uint32_t r[10] = { ros_addr(n), ros_addr(buf), 255, 0, 0 };
        there = swi(XOS_ReadVarVal, r) == NULL;
    }
    ros_rma_free(n), ros_rma_free(buf);
    return there;
}

/* ---- the exclusion list ------------------------------------------------------------------ */

static int find(const char *name)
{
    for (unsigned k = 0; k < sm.nexcl; k++)
        if (!strcasecmp(sm.excl[k], name))
            return (int)k;
    return -1;
}

static void exclude(const char *name)
{
    if (!*name || find(name) >= 0)
        return;
    void *n = realloc(sm.excl, (sm.nexcl + 1) * sizeof *sm.excl);
    if (!n)
        return;
    sm.excl = n;
    strncpy(sm.excl[sm.nexcl], name, NAME_MAX_LEN - 1);
    sm.excl[sm.nexcl++][NAME_MAX_LEN - 1] = 0;
}

static void include(const char *name)
{
    int k = find(name);
    if (k < 0)
        return;
    memmove(sm.excl[k], sm.excl[k + 1], (sm.nexcl - (unsigned)k - 1) * sizeof *sm.excl);
    sm.nexcl--;
}

/* ---- the choices file ------------------------------------------------------------------- */

/* Parse the file. Its lines are "Smooth On" or "Smooth Off", and
 * "Exclude <task name>". */
static void parse(const char *text, size_t size)
{
    sm.on = 0;
    sm.nexcl = 0;
    for (size_t i = 0; i < size;) {
        size_t j = i;
        while (j < size && text[j] != '\n' && text[j] != '\r')
            j++;
        char line[NAME_MAX_LEN + 16];
        size_t n = j - i < sizeof line - 1 ? j - i : sizeof line - 1;
        memcpy(line, text + i, n);
        line[n] = 0;
        while (n && line[n - 1] == ' ')
            line[--n] = 0;
        if (!strncasecmp(line, "Smooth ", 7))
            sm.on = !strcasecmp(line + 7, "On");
        else if (!strncasecmp(line, "Exclude ", 8))
            exclude(line + 8);
        i = j + 1;
    }
}

/* Read Choices:Smooth, once Choices$Path exists. */
static void load(void)
{
    if (sm.loaded || !var_exists("Choices$Path"))
        return;
    sm.loaded = 1;
    char *name = arena_str("Choices:Smooth");
    if (!name)
        return;
    uint32_t r[10] = { 17, ros_addr(name) };            /* OS_File 17: its catalogue entry */
    if (!swi(XOS_File, r) && r[0] == 1 && r[4] < 65536) {
        uint32_t size = r[4];
        char *buf = ros_rma_alloc(size + 1);
        if (buf) {
            uint32_t l[10] = { 16, ros_addr(name), ros_addr(buf), 0 };      /* load it there */
            if (!swi(XOS_File, l))
                parse(buf, size);
            ros_rma_free(buf);
        }
    }
    ros_rma_free(name);
}

/* Write <Choices$Write>.Smooth as a Text file. If there is nowhere to write
 * it, nothing is written. */
static void save(void)
{
    if (!var_exists("Choices$Write"))
        return;
    size_t size = 64 + sm.nexcl * (NAME_MAX_LEN + 10);
    char *text = ros_rma_alloc((uint32_t)size), *name = arena_str("<Choices$Write>.Smooth");
    if (text && name) {
        size_t n = (size_t)snprintf(text, size, "Smooth %s\n", sm.on ? "On" : "Off");
        for (unsigned k = 0; k < sm.nexcl; k++)
            n += (size_t)snprintf(text + n, size - n, "Exclude %s\n", sm.excl[k]);
        uint32_t r[10] = { 10, ros_addr(name), 0xFFF, 0, ros_addr(text), ros_addr(text + n) };
        swi(XOS_File, r);
    }
    ros_rma_free(text), ros_rma_free(name);
}

/* ---- when it applies ---------------------------------------------------------------------- */

/* Get the current Wimp task's name. This returns 0 if there is no task,
 * which is outside the desktop. */
static int task_name(char out[NAME_MAX_LEN])
{
    if (sm.test) {
        strcpy(out, sm.test_name);
        return !sm.test_none;
    }
    uint32_t r[10] = { 5 };                             /* Wimp_ReadSysInfo 5: the current task */
    if (swi(XWIMP_READSYSINFO, r) || !r[0])
        return 0;
    uint32_t t[10] = { r[0] };
    if (swi(XTASKMANAGER_TASKNAMEFROMHANDLE, t) || !t[0])
        return 0;
    unsigned k = 0;
    for (uint32_t p = t[0]; k < NAME_MAX_LEN - 1; k++, p++) {
        uint8_t c = ros_ld8(p);
        if (c < ' ')
            break;
        out[k] = (char)c;
    }
    out[k] = 0;
    return 1;
}

/* Whether smoothing applies: it is on, the task is not excluded, the output
 * is 16 or 32 bpp, and there is no printer job. */
static int applies(void)
{
    load();
    if (!sm.on)
        return 0;
    char name[NAME_MAX_LEN];
    if (task_name(name) && find(name) >= 0)
        return 0;
    uint32_t *v = ros_rma_alloc(16);
    if (!v)
        return 0;
    v[0] = 9, v[1] = 0xFFFFFFFFu;                       /* Log2BPP */
    uint32_t r[10] = { ros_addr(v), ros_addr(v + 2) };
    os_error *e = swi(XOS_ReadVduVariables, r);
    uint32_t l2 = v[2];
    ros_rma_free(v);
    if (e || (l2 != 4 && l2 != 5))
        return 0;
    uint32_t p[10] = { 0 };
    if (!swi(XPDRIVER_CURRENTJOB, p) && p[0])
        return 0;
    return 1;
}

/* Whether the graphics foreground plots over, in one colour. */
static int overwrite(void)
{
    uint32_t *v = ros_rma_alloc(48);
    if (!v)
        return 0;
    v[0] = 151, v[1] = 9, v[2] = 0xFFFFFFFFu;           /* GPLFMD, Log2BPP */
    uint32_t r[10] = { ros_addr(v), ros_addr(v + 3) };
    int ok = swi(XOS_ReadVduVariables, r) == NULL;
    uint32_t act = v[3], l2 = v[4];
    uint32_t c[10] = { 0x80, ros_addr(v + 4) };          /* OS_SetColour: read the foreground */
    ok = ok && (act & 0x0F) == 0 && ((act & 0xF0) == 0 || (act & 0xF0) == 0x60) &&
         !swi(XOS_SetColour, c) && (c[0] & 0x0F) == 0;
    for (int k = 1; ok && k < 8; k++)
        ok = v[4 + k] == v[4];
    if (ok && l2 == 4)
        ok = (v[4] & 0xFFFF) == v[4] >> 16;
    ros_rma_free(v);
    return ok;
}

/* The style that GDraw is given for a Draw style. This is 0 if Draw keeps
 * the call. */
static uint32_t aa_style(uint32_t style)
{
    if (style == 0)
        return 0xB0;
    if (style & ~0x3Fu)
        return 0;                                   /* Draw's clipping bits and reserved bits */
    if ((style & 3) != 0 && (style & 3) != 2)
        return 0;
    if ((style & 0x3C) != 0x30 && (style & 0x3C) != 0x38)
        return 0;
    return style | 0x80;
}

/* ---- DrawV ------------------------------------------------------------------------------------ */

static int drawv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t n = s->r[8], style, gdraw;
    if (n == 2) {                                   /* Draw_Fill */
        if (!(style = aa_style(s->r[1])))
            return ROS_VECTOR_PASS;
        gdraw = XGDraw_Fill;
    } else if (n == 4) {                            /* Draw_Stroke, thick */
        if (!s->r[4] || !aa_style(s->r[1] & 0x7FFFFFFFu))
            return ROS_VECTOR_PASS;
        style = 0x800000B8u;
        gdraw = XGDraw_Stroke;
    } else {
        return ROS_VECTOR_PASS;
    }
    if (!applies() || !overwrite())
        return ROS_VECTOR_PASS;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, s->r, 8 * sizeof c.r[0]);
    c.r[1] = style;
    ros_swi(&c, gdraw);
    s->r[0] = c.r[0], s->v = c.v;
    return ROS_VECTOR_CLAIM;
}

/* The Font Manager asks this for each Font_Paint. */
static int force_blend(void)
{
    return applies();
}

/* ---- the commands ----------------------------------------------------------------------------- */

static void write0(const char *t)
{
    char *p = arena_str(t);
    if (!p)
        return;
    uint32_t r[10] = { ros_addr(p) };
    swi(XOS_Write0, r);
    ros_rma_free(p);
}

static void newline(void)
{
    uint32_t r[10] = { 0 };
    swi(XOS_NewLine, r);
}

/* The command's tail as a C string, with spaces trimmed. */
static void tail_str(uint32_t tail, char out[NAME_MAX_LEN])
{
    while (ros_ld8(tail) == ' ')
        tail++;
    unsigned k = 0;
    for (; k < NAME_MAX_LEN - 1; k++) {
        uint8_t c = ros_ld8(tail + k);
        if (c < ' ')
            break;
        out[k] = (char)c;
    }
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

static os_error *cmd_smooth(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    load();
    sm.loaded = 1;
    if (argc) {
        char a[NAME_MAX_LEN];
        tail_str(tail, a);
        if (!strcasecmp(a, "On"))
            sm.on = 1;
        else if (!strcasecmp(a, "Off"))
            sm.on = 0;
        else
            return ros_error(ERR_SYNTAX, "Syntax: *Smooth [On|Off]");
        save();
        return NULL;
    }
    write0(sm.on ? "Smoothing is on." : "Smoothing is off.");
    newline();
    if (!sm.nexcl) {
        write0("No tasks are excluded.");
        newline();
        return NULL;
    }
    write0("Excluded tasks:");
    newline();
    for (unsigned k = 0; k < sm.nexcl; k++) {
        write0("  ");
        write0(sm.excl[k]);
        newline();
    }
    return NULL;
}

static os_error *cmd_exclude(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    load();
    sm.loaded = 1;
    char a[NAME_MAX_LEN];
    tail_str(tail, a);
    exclude(a);
    save();
    return NULL;
}

static os_error *cmd_include(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    load();
    sm.loaded = 1;
    char a[NAME_MAX_LEN];
    tail_str(tail, a);
    include(a);
    save();
    return NULL;
}

void smooth_test_task(const char *name)
{
    sm.test = 1;
    sm.test_none = name == NULL;
    strncpy(sm.test_name, name ? name : "", NAME_MAX_LEN - 1);
    sm.test_name[NAME_MAX_LEN - 1] = 0;
}

void smooth_test_task_clear(void)
{
    sm.test = 0;
}

/* ---- the module ------------------------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    sm.on = 0, sm.loaded = 0, sm.nexcl = 0;
    os_error *e = ros_vector_claim_native(DRAWV, drawv, 0);
    if (e)
        return e;
    fm_paint_blend_hook = force_blend;
    load();
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    ros_vector_release_native(DRAWV, drawv, 0);
    if (fm_paint_blend_hook == force_blend)
        fm_paint_blend_hook = NULL;
    free(sm.excl);
    sm.excl = NULL, sm.nexcl = 0, sm.on = 0, sm.loaded = 0;
    return NULL;
}

static const struct ros_command commands[] = {
    { "Smooth", ROS_CMD_INFO(0, 1, 0, 0), "Syntax: *\x1B\x00 [On|Off]",
      "*\x1B\x00 turns anti-aliasing of the desktop's Draw fills, thick strokes and font "
      "painting on or off (at 16 and 32 bpp).  With no parameter it shows the setting and the "
      "tasks excluded.", cmd_smooth },
    { "SmoothExclude", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *\x1B\x00 <task name>",
      "*\x1B\x00 names a task that keeps exact drawing while smoothing is on.", cmd_exclude },
    { "SmoothInclude", ROS_CMD_INFO(1, 255, 0, 0), "Syntax: *\x1B\x00 <task name>",
      "*\x1B\x00 takes a task off the list that *SmoothExclude makes.", cmd_include },
    { 0 },
};

struct ros_module smooth_module = {
    .title = "Smooth",
    .help = "Smooth\t1.00 (27 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .commands = commands,
};
